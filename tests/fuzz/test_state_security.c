/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"
#include "engine_internal.h"
#include "gml_builtin.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  STATE_HEADER_SIZE=112,
  STATE_CONTENT_LOCATOR_SIZE=1024,
  STATE_LAUNCH_PARAMETERS_SIZE=1024,
  STATE_CORE_FIELDS_OFFSET=STATE_HEADER_SIZE+STATE_CONTENT_LOCATOR_SIZE+
                           STATE_LAUNCH_PARAMETERS_SIZE
};

static int fail(const char *message){
  fprintf(stderr,"state security: %s\n",message);
  return 0;
}

static uint64_t read_u64(const uint8_t *data){
  uint64_t value=0;
  for(unsigned i=0;i<8;i++) value|=(uint64_t)data[i]<<(i*8);
  return value;
}

static void write_u32(uint8_t *data,uint32_t value){
  for(unsigned i=0;i<4;i++) data[i]=(uint8_t)(value>>(i*8));
}

static void write_u64(uint8_t *data,uint64_t value){
  for(unsigned i=0;i<8;i++) data[i]=(uint8_t)(value>>(i*8));
}

static uint64_t state_checksum(const uint8_t *data,size_t size){
  uint64_t hash=UINT64_C(1469598103934665603);
  while(size>=8){
    hash^=read_u64(data);
    hash*=UINT64_C(1099511628211);
    data+=8;
    size-=8;
  }
  while(size--){
    hash^=*data++;
    hash*=UINT64_C(1099511628211);
  }
  return hash;
}

static int save_state(AnygmEngine *engine,uint8_t **bytes,size_t *size){
  size_t capacity=anygm_state_size(engine),written=0;
  uint8_t *data=malloc(capacity?capacity:1);
  if(!data || anygm_state_save(engine,data,capacity,&written)!=ANYGM_OK ||
     written!=capacity){
    free(data);
    return 0;
  }
  *bytes=data;
  *size=written;
  return 1;
}

static int engine_matches(AnygmEngine *engine,const uint8_t *baseline,size_t baseline_size){
  uint8_t *current=NULL;
  size_t current_size=0;
  int saved=save_state(engine,&current,&current_size);
  int same=saved&&current_size==baseline_size&&!memcmp(current,baseline,baseline_size);
  if(!same){
    size_t difference=0;
    while(difference<current_size && difference<baseline_size && current &&
          current[difference]==baseline[difference]) difference++;
    fprintf(stderr,"state security: engine bytes differ at %zu (sizes %zu/%zu, values %u/%u)\n",
            difference,current_size,baseline_size,
            difference<current_size && current?current[difference]:0,
            difference<baseline_size?baseline[difference]:0);
  }
  free(current);
  return same;
}

static int reject_unchanged(AnygmEngine *engine,const uint8_t *candidate,size_t size,
                            const uint8_t *baseline,size_t baseline_size,const char *label){
  if(anygm_state_load(engine,candidate,size)!=ANYGM_ERROR_STATE_MISMATCH){
    fprintf(stderr,"state security: accepted %s\n",label);
    return 0;
  }
  if(!engine_matches(engine,baseline,baseline_size)){
    fprintf(stderr,"state security: %s changed the engine\n",label);
    return 0;
  }
  return 1;
}

static void refresh_checksum(uint8_t *state){
  size_t payload_size=(size_t)read_u64(state+96);
  write_u64(state+56,state_checksum(state+STATE_HEADER_SIZE,payload_size));
}

static int reject_payload_u32(AnygmEngine *engine,uint8_t *candidate,size_t size,
                              const uint8_t *baseline,size_t baseline_size,size_t offset,
                              uint32_t value,const char *label);

typedef struct StateCursor {
  const uint8_t *data;
  size_t size;
  size_t offset;
  int ok;
} StateCursor;

typedef struct RuntimeMaskRecord {
  size_t row_bytes_offset;
  size_t count_offset;
  size_t payload_offset;
  int width;
  int height;
  int frames;
  int row_bytes;
  int count;
} RuntimeMaskRecord;

static uint32_t cursor_u32(StateCursor *cursor){
  if(!cursor || !cursor->ok || cursor->offset>cursor->size ||
     cursor->size-cursor->offset<4){
    if(cursor) cursor->ok=0;
    return 0;
  }
  uint32_t value=(uint32_t)cursor->data[cursor->offset]|
                 ((uint32_t)cursor->data[cursor->offset+1]<<8)|
                 ((uint32_t)cursor->data[cursor->offset+2]<<16)|
                 ((uint32_t)cursor->data[cursor->offset+3]<<24);
  cursor->offset+=4;
  return value;
}

static int cursor_skip(StateCursor *cursor,size_t bytes){
  if(!cursor || !cursor->ok || cursor->offset>cursor->size ||
     bytes>cursor->size-cursor->offset){
    if(cursor) cursor->ok=0;
    return 0;
  }
  cursor->offset+=bytes;
  return 1;
}

/* Locate the first runtime sprite's independently serialized collision plane. The scanner mirrors
 * only enough of the renderer envelope to make each malformed-state mutation field-specific; the
 * product under test remains owned and decoded solely by gml_render_state.c. */
static int locate_runtime_mask_record(const uint8_t *state,size_t state_size,
                                      RuntimeMaskRecord *record){
  if(!state || !record || state_size<STATE_HEADER_SIZE) return 0;
  uint64_t core_size=read_u64(state+64),render_size=read_u64(state+72);
  if(core_size>SIZE_MAX || render_size>SIZE_MAX ||
     (size_t)core_size>state_size-STATE_HEADER_SIZE ||
     (size_t)render_size>state_size-STATE_HEADER_SIZE-(size_t)core_size) return 0;
  size_t render_start=STATE_HEADER_SIZE+(size_t)core_size;
  StateCursor cursor={state+render_start,(size_t)render_size,0,1};
  (void)cursor_u32(&cursor); /* live font count */
  for(int font=0;font<GML_MAX_FONTS && cursor.ok;font++){
    if(!cursor_skip(&cursor,16)) return 0;
    uint32_t map_count=cursor_u32(&cursor);
    if(map_count>4096 || !cursor_skip(&cursor,(size_t)map_count*4)) return 0;
    /* This collision-plane fixture creates no runtime file fonts. */
    if(cursor_u32(&cursor)!=0) return 0;
  }
  if(!cursor_skip(&cursor,40)) return 0;
  for(int surface=0;surface<GML_MAX_SURFACES && cursor.ok;surface++){
    int live=(int32_t)cursor_u32(&cursor);
    (void)cursor_u32(&cursor);
    (void)cursor_u32(&cursor);
    if(live){
      uint32_t run_count=cursor_u32(&cursor);
      size_t run_bytes=(size_t)run_count;
      if((run_bytes && 8u>SIZE_MAX/run_bytes) ||
         !cursor_skip(&cursor,run_bytes*8u)) return 0;
    }
  }
  uint32_t runtime_count=cursor_u32(&cursor);
  if(!cursor.ok || !runtime_count) return 0;
  (void)cursor_u32(&cursor); /* id */
  (void)cursor_u32(&cursor); /* extra */
  record->width=(int32_t)cursor_u32(&cursor);
  record->height=(int32_t)cursor_u32(&cursor);
  record->frames=(int32_t)cursor_u32(&cursor);
  if(!cursor_skip(&cursor,32)) return 0; /* origin, bounds, collision policy */
  int mode=(int32_t)cursor_u32(&cursor);
  if(mode==0){
    if(record->width<=0 || record->height<=0 || record->frames<=0) return 0;
    uint64_t pixels=(uint64_t)(unsigned)record->width*(unsigned)record->height*
                    (unsigned)record->frames;
    if(pixels>SIZE_MAX/4 || !cursor_skip(&cursor,(size_t)pixels*4)) return 0;
  } else if(mode==1){
    (void)cursor_u32(&cursor); /* root */
    uint32_t path_length=cursor_u32(&cursor);
    if(path_length>4095 || !cursor_skip(&cursor,path_length) ||
       !cursor_skip(&cursor,8)) return 0; /* image count and remove-background flag */
  } else return 0;
  record->row_bytes_offset=render_start+cursor.offset;
  record->row_bytes=(int32_t)cursor_u32(&cursor);
  record->count_offset=render_start+cursor.offset;
  record->count=(int32_t)cursor_u32(&cursor);
  record->payload_offset=render_start+cursor.offset;
  return cursor.ok;
}

static int runtime_mask_state_cases(AnygmEngine *engine){
  enum { WIDTH=9, HEIGHT=3, FRAMES=2, ROW_BYTES=(WIDTH+7)/8 };
  uint8_t *rgba=calloc((size_t)WIDTH*HEIGHT*FRAMES,4);
  if(!rgba) return fail("runtime sprite pixel allocation failed");
  for(size_t pixel=0;pixel<(size_t)WIDTH*HEIGHT*FRAMES;pixel++) rgba[pixel*4+3]=255;
  int sprite=gml_sprite_append_from_rgba_frames(
      &engine->render,rgba,WIDTH,HEIGHT,FRAMES,0,0,"<state-security-sprite>");
  if(sprite<0) return fail("runtime sprite creation failed");
  GmlSprite *runtime=&engine->render.spr[sprite];
  /* Shape the synthetic slot as a packaged sprite replaced at runtime. That is the ordinary
   * mode-zero restore path and, unlike a project with no packaged sprite table at all, gives the
   * runtime-extra deletion logic a real base boundary during rollback. */
  runtime->runtime_extra=0;
  engine->render.base_n_spr=engine->render.n_spr;
  runtime->runtime_mask=malloc((size_t)ROW_BYTES*HEIGHT*FRAMES);
  if(!runtime->runtime_mask) return fail("runtime sprite mask allocation failed");
  memset(runtime->runtime_mask,0x5a,(size_t)ROW_BYTES*HEIGHT*FRAMES);
  runtime->mask=runtime->runtime_mask;
  runtime->mask_rowb=ROW_BYTES;
  runtime->mask_count=FRAMES;

  uint8_t *baseline=NULL,*candidate=NULL;
  size_t state_size=0;
  if(!save_state(engine,&baseline,&state_size)) return fail("runtime mask baseline failed");
  candidate=malloc(state_size?state_size:1);
  RuntimeMaskRecord record={0};
  int ok=candidate && locate_runtime_mask_record(baseline,state_size,&record) &&
         record.width==WIDTH && record.height==HEIGHT && record.frames==FRAMES &&
         record.row_bytes==ROW_BYTES && record.count==FRAMES;
  if(!ok) fail("runtime mask record could not be located");

  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               record.row_bytes_offset,(uint32_t)(ROW_BYTES+1),
                               "runtime mask row width");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               record.count_offset,(uint32_t)(FRAMES+1),
                               "runtime mask count beyond frames");
  if(ok){
    uint64_t render_size=read_u64(baseline+72),vm_size=read_u64(baseline+80);
    memcpy(candidate,baseline,state_size);
    if(!render_size || vm_size==UINT64_MAX) ok=fail("runtime mask section cannot be shortened");
    else {
      write_u64(candidate+72,render_size-1);
      write_u64(candidate+80,vm_size+1);
      ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,
                          "truncated runtime mask payload");
    }
  }
  free(candidate);
  free(baseline);
  if(!ok) return 0;

  /* The maximum accepted dimensions make the mask product 8 GiB. It is merely larger than the
   * remaining section on a 64-bit host, but wraps to zero on i686 unless both multiplications are
   * checked. A source-backed record keeps this bounded fixture from allocating those pixels. The
   * live record is deliberately shaped to let the transactional reader reuse it without opening
   * the synthetic path. */
  free(runtime->runtime_mask);
  runtime->runtime_mask=NULL;
  runtime->mask=NULL;
  runtime->mask_rowb=0;
  runtime->mask_count=0;
  free(runtime->runtime_source_path);
  runtime->runtime_source_path=strdup("synthetic-state-sprite.png");
  if(!runtime->runtime_source_path) return fail("runtime sprite source allocation failed");
  runtime->w=4096;
  runtime->h=4096;
  runtime->n_frames=4096;

  baseline=NULL;
  state_size=0;
  if(!save_state(engine,&baseline,&state_size)) return fail("large mask-product baseline failed");
  candidate=malloc(state_size?state_size:1);
  memset(&record,0,sizeof record);
  ok=candidate && locate_runtime_mask_record(baseline,state_size,&record) &&
     record.width==4096 && record.height==4096 && record.frames==4096 &&
     record.row_bytes==0 && record.count==0;
  if(!ok) fail("large runtime mask record could not be located");
  if(ok){
    memcpy(candidate,baseline,state_size);
    write_u32(candidate+record.row_bytes_offset,512);
    write_u32(candidate+record.count_offset,4096);
    refresh_checksum(candidate);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,
                        "overflowing runtime mask product");
  }
  free(candidate);
  free(baseline);
  return ok;
}

static int reject_payload_u32(AnygmEngine *engine,uint8_t *candidate,size_t size,
                              const uint8_t *baseline,size_t baseline_size,size_t offset,
                              uint32_t value,const char *label){
  memcpy(candidate,baseline,baseline_size);
  if(offset>baseline_size-4) return fail("mutation offset is outside the state");
  write_u32(candidate+offset,value);
  refresh_checksum(candidate);
  return reject_unchanged(engine,candidate,size,baseline,baseline_size,label);
}

/* Content-override directives join the state identity while active: a state saved with them
 * must not load without them, and a directive the engine does not recognize must fail the load
 * transactionally instead of being dropped. */
static int content_override_state_cases(const AnygmHostServices *services,
                                        const AnygmSyntheticContent *fixture){
  const char *slash=strrchr(fixture->path,'/');
  const char *payload_name=slash?slash+1:fixture->path;
  char anchor[256];
  if(snprintf(anchor,sizeof anchor,"%s/title.anygm",fixture->directory)>=(int)sizeof anchor)
    return fail("anchor path is too long");
  char anchor_text[512];
  int anchor_length=snprintf(anchor_text,sizeof anchor_text,
                             "[anygm]\npayload=%s\n[overrides]\n# freeze one probe global\n"
                             "$anygm_probe=1\n"
                             "listset|obj_fixture|fixture_lists[0]|1|40\n"
                             "listset|global|fixture_global_list|1|41\n"
                             "introskip|3-5,9\nintroauto|2\n",payload_name);
  if(anchor_length<0 || (size_t)anchor_length>=sizeof anchor_text)
    return fail("anchor text is too long");
  FILE *file=fopen(anchor,"wb");
  int written=file && fwrite(anchor_text,1,(size_t)anchor_length,file)==(size_t)anchor_length;
  if(file && fclose(file)!=0) written=0;
  if(!written) return fail("could not write the override anchor");

  AnygmEngine *engine=NULL;
  if(anygm_create(services,&engine)!=ANYGM_OK) return fail("override engine creation failed");
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=anchor;
  source.cache_directory=fixture->directory;
  int ok=anygm_load(engine,&source,NULL)==ANYGM_OK;
  if(!ok){ anygm_destroy(engine); return fail("anchored content with overrides did not load"); }

  AnygmInputFrame input={0};
  AnygmFrameOutput output={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  output.struct_size=sizeof output;
  uint8_t *baseline=NULL;
  size_t state_size=0;
  ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  if(!ok){ anygm_destroy(engine); return fail("override fixture frames failed"); }
  double list_value=gml_global_num(&engine->vm,"fixture_list_value");
  if(list_value!=40.0){
    fprintf(stderr,"state security: list override produced %.17g, expected 40\n",list_value);
    int object=gml_object_index_by_name(&engine->vm,"obj_fixture");
    GmlInstance *instance=object>=0?gml_find_instance(&engine->vm,object):NULL;
    int found=0;
    GmlVal lists=instance?gml_inst_var_get_val(
      &engine->vm,vreal((double)instance->id),"fixture_lists",&found):vundef();
    GmlVal handle=found?gml_arr_get(lists,0):vundef();
    fprintf(stderr,
            "state security: list diagnostic object=%d instance=%d found=%d type=%d handle_type=%d handle=%.17g kind=%d applied=%d\n",
            object,instance?1:0,found,(int)lists.t,(int)handle.t,handle.d,
            (int)engine->boot_cheats[1].act.kind,engine->boot_cheats[1].applied);
    anygm_destroy(engine);
    return 0;
  }
  if(gml_global_num(&engine->vm,"fixture_global_list_value")!=41.0){
    anygm_destroy(engine);
    return fail("global list override did not survive a later content write");
  }
  ok=save_state(engine,&baseline,&state_size);
  if(!ok){ anygm_destroy(engine); return fail("override state baseline failed"); }

  AnygmConfigDelta delta;
  memset(&delta,0,sizeof delta);
  delta.struct_size=sizeof delta;
  delta.values.struct_size=sizeof delta.values;
  delta.fields=ANYGM_CONFIG_CONTENT_OVERRIDES;
  delta.values.content_overrides=0;
  ok=anygm_set_config(engine,&delta)==ANYGM_OK &&
     anygm_state_load(engine,baseline,state_size)==ANYGM_ERROR_STATE_MISMATCH;
  if(!ok) fail("a state saved with overrides loaded without them");
  if(ok){
    delta.values.content_overrides=1;
    ok=anygm_set_config(engine,&delta)==ANYGM_OK &&
       anygm_state_load(engine,baseline,state_size)==ANYGM_OK;
    if(!ok) fail("re-enabling overrides did not restore the state identity");
  }
  free(baseline);
  anygm_destroy(engine);
  if(!ok) return 0;

  file=fopen(anchor,"wb");
  written=file && fwrite("[anygm]\npayload=",1,16,file)==16 &&
          fwrite(payload_name,1,strlen(payload_name),file)==strlen(payload_name) &&
          fwrite("\n[overrides]\nnot a directive\n",1,29,file)==29;
  if(file && fclose(file)!=0) written=0;
  if(!written) return fail("could not rewrite the broken anchor");
  engine=NULL;
  if(anygm_create(services,&engine)!=ANYGM_OK) return fail("broken-override engine creation failed");
  ok=anygm_load(engine,&source,NULL)!=ANYGM_OK;
  anygm_destroy(engine);
  if(!ok){ remove(anchor); return fail("an unrecognized override directive did not fail the load"); }

  file=fopen(anchor,"wb");
  written=file && fwrite("[anygm]\npayload=",1,16,file)==16 &&
          fwrite(payload_name,1,strlen(payload_name),file)==strlen(payload_name) &&
          fwrite("\n[overrides]\nintroskip|abc\n",1,27,file)==27;
  if(file && fclose(file)!=0) written=0;
  if(!written) return fail("could not rewrite the introskip anchor");
  engine=NULL;
  if(anygm_create(services,&engine)!=ANYGM_OK) return fail("introskip engine creation failed");
  ok=anygm_load(engine,&source,NULL)!=ANYGM_OK;
  anygm_destroy(engine);
  if(!ok){ remove(anchor); return fail("a malformed introskip list did not fail the load"); }

  /* introauto| carries the same room list, so it is held to the same grammar. */
  file=fopen(anchor,"wb");
  written=file && fwrite("[anygm]\npayload=",1,16,file)==16 &&
          fwrite(payload_name,1,strlen(payload_name),file)==strlen(payload_name) &&
          fwrite("\n[overrides]\nintroauto|abc\n",1,27,file)==27;
  if(file && fclose(file)!=0) written=0;
  if(!written) return fail("could not rewrite the introauto anchor");
  engine=NULL;
  if(anygm_create(services,&engine)!=ANYGM_OK) return fail("introauto engine creation failed");
  ok=anygm_load(engine,&source,NULL)!=ANYGM_OK;
  anygm_destroy(engine);
  remove(anchor);
  if(!ok) return fail("a malformed introauto list did not fail the load");
  return 1;
}

static void write_double(uint8_t *data,double value){
  uint64_t bits; memcpy(&bits,&value,sizeof bits); write_u64(data,bits);
}

static int tilemap_state_cases(const AnygmHostServices *services){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_tilemap_content_create(&fixture)) return fail("tilemap fixture creation failed");
  AnygmEngine *engine=NULL; uint8_t *content=NULL,*baseline=NULL,*candidate=NULL;
  size_t content_size=0,state_size=0;
  int ok=anygm_synthetic_content_read(&fixture,&content,&content_size) &&
    anygm_create(services,&engine)==ANYGM_OK;
  AnygmContentSource source={0};
  source.struct_size=sizeof source; source.kind=ANYGM_CONTENT_MEMORY;
  source.path="synthetic-tilemaps.win"; source.data=content; source.size=content_size;
  if(ok){
    AnygmResult result=anygm_load(engine,&source,NULL);
    ok=result==ANYGM_OK && engine->vm.n_tilemaps==2;
    if(!ok){
      char error[256]={0};
      anygm_get_last_error(engine,error,sizeof error);
      fprintf(stderr,"tilemap setup: result=%d maps=%d error=%s\n",
              (int)result,engine->vm.n_tilemaps,error);
    }
  }
  if(!ok){ fail("tilemap fixture did not load two authored maps"); goto done; }

  /* The first capacity answer already includes unedited map metadata. */
  size_t cold_size=anygm_state_size(engine);
  GmlTileMap *first=&engine->vm.tilemaps[0],*second=&engine->vm.tilemaps[1];
  int id=first->id,other_id=second->id,next=engine->vm.next_tilemap_id;
  gml_tilemap_set_position(first,11.25,-7.5); first->tileset=3;
  if(!cold_size || anygm_state_size(engine)!=cold_size){
    ok=fail("position-only mutation grew the cold state"); goto done;
  }
  if(!gml_tilemap_set_cell(first,0,0,5) || !gml_tilemap_set_cell(first,1,1,9) ||
     !save_state(engine,&baseline,&state_size)){
    ok=fail("tilemap state baseline failed"); goto done;
  }
  candidate=malloc(state_size);
  if(!candidate){ ok=0; goto done; }

  /* Locate the exact declared metadata prefix inside the VM section only. No
   * private decoder is called to choose the fields that the decoder must reject. */
  uint8_t prefix[64]={0};
  write_u32(prefix,2); write_u32(prefix+4,(uint32_t)next);
  const int fields[]={id,1,1,first->order,3,2,2};
  for(int i=0;i<7;i++) write_u32(prefix+8+4*i,(uint32_t)fields[i]);
  write_double(prefix+36,11.25); write_double(prefix+44,-7.5);
  write_double(prefix+52,first->depth); write_u32(prefix+60,2);
  size_t vm_start=STATE_HEADER_SIZE+(size_t)read_u64(baseline+64)+(size_t)read_u64(baseline+72);
  size_t vm_end=vm_start+(size_t)read_u64(baseline+80);
  size_t record=0,matches=0;
  for(size_t at=vm_start;at+sizeof prefix<=vm_end;at++){
    if(!memcmp(baseline+at,prefix,sizeof prefix)){ record=at+8; matches++; }
  }
  if(matches!=1 || record+56+16+56>vm_end){
    ok=fail("tilemap state metadata prefix is not unique or complete"); goto done;
  }
  size_t other=record+56+16;
  StateCursor cursor={baseline,state_size,other,1};
  if(cursor_u32(&cursor)!=(uint32_t)other_id){ ok=fail("second map boundary disagrees"); goto done; }
  const struct { size_t offset; uint32_t value; const char *label; } mutations[]={
    {record-8,UINT32_MAX,"negative map count"},
    {record-8,513,"excessive map count"},
    {record-8,512,"map count beyond remaining metadata"},
    {record-4,UINT32_MAX,"negative next map identity"},
    {record-4,(uint32_t)id,"non-advancing next map identity"},
    {record,UINT32_MAX,"negative retained map identity"},
    {other,(uint32_t)id,"duplicate retained map identity"},
    {record+4,2,"invalid map used flag"},
    {record+8,2,"invalid map visibility flag"},
    {record+12,99,"map parent order mismatch"},
    {record+20,0,"zero map width"},
    {record+20,8193,"oversized map width"},
    {record+20,3,"map width disagrees with room"},
    {record+24,0,"zero map height"},
    {record+24,8193,"oversized map height"},
    {record+52,UINT32_MAX,"negative sparse cell count"},
    {record+52,5,"sparse count exceeds map cells"},
    {record+56,UINT32_MAX,"negative sparse cell index"},
    {record+56,4,"sparse cell outside map"},
    {record+64,0,"duplicate sparse cell index"},
    {record+56,3,"unordered sparse cell indices"},
    {vm_start+4,11,"previous map-metadata-free VM schema"}
  };
  for(size_t i=0;ok && i<sizeof mutations/sizeof mutations[0];i++)
    ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                          mutations[i].offset,mutations[i].value,mutations[i].label);
  for(int i=0;ok && i<3;i++){
    memcpy(candidate,baseline,state_size);
    write_double(candidate+record+28+8*i,i==1?INFINITY:NAN);
    refresh_checksum(candidate);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"non-finite map metadata");
  }
  if(ok){
    memcpy(candidate,baseline,state_size);
    write_u32(candidate+record+20,8192); write_u32(candidate+record+24,8192);
    write_u32(candidate+record+52,1000000);
    refresh_checksum(candidate);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"sparse extent exceeds remaining bytes");
  }
  if(ok){
    memcpy(candidate,baseline,state_size);
    write_double(candidate+record+28,91); write_u32(candidate+vm_end,0);
    refresh_checksum(candidate);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"late rejection after map publication");
  }
  if(ok){
    gml_room_enter(&engine->vm,1);
    ok=engine->vm.n_tilemaps==0 && anygm_state_load(engine,baseline,state_size)==ANYGM_OK &&
      engine_matches(engine,baseline,state_size);
    if(!ok) fail("map restoration from an empty room failed");
  }
  if(ok){
    ok=anygm_reset(engine)==ANYGM_OK && anygm_state_load(engine,baseline,state_size)==ANYGM_OK &&
      engine_matches(engine,baseline,state_size);
    first=gml_tilemap_find(&engine->vm,id); second=gml_tilemap_find(&engine->vm,other_id);
    ok=ok && first && second && first->x==11.25 && first->y==-7.5 && first->tileset==3 &&
      first->tiles[0]==5 && first->tiles[12]==9 && second->x==0 && second->y==0 &&
      engine->vm.next_tilemap_id==next;
    if(!ok) fail("tilemap Reset restoration lost metadata or identities");
  }
done:
  free(candidate); free(baseline); anygm_destroy(engine); free(content);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int dormant_visual_state_cases(const AnygmHostServices *services){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_tilemap_content_create(&fixture)) return fail("dormant fixture creation failed");
  AnygmEngine *engine=NULL; uint8_t *content=NULL,*baseline=NULL,*candidate=NULL;
  size_t content_size=0,size=0;
  int ok=anygm_synthetic_content_read(&fixture,&content,&content_size) &&
    anygm_create(services,&engine)==ANYGM_OK;
  AnygmContentSource source={0};
  source.struct_size=sizeof source; source.kind=ANYGM_CONTENT_MEMORY;
  source.path="synthetic-dormant.win"; source.data=content; source.size=content_size;
  if(ok) ok=anygm_load(engine,&source,NULL)==ANYGM_OK &&
    engine->vm.n_rtl==2 && engine->vm.n_tilemaps==2;
  if(!ok){ fail("dormant fixture did not load its declared tables"); goto done; }
  int map_id=engine->vm.tilemaps[0].id,layer_id=engine->vm.rtl[0].id;
  *gml_varmap_put(&engine->vm.globals,"room_persistent")=vreal(1);
  gml_tilemap_set_position(&engine->vm.tilemaps[0],11.25,-7.5);
  if(!gml_tilemap_set_cell(&engine->vm.tilemaps[0],0,0,9)){ ok=0; goto done; }
  GmlRtElem *element=gml_rt_elem_new(&engine->vm);
  if(!element){ ok=0; goto done; }
  element->type=7; element->layer=layer_id; element->sprite=-1; element->alpha=0.25;
  int element_id=element->id;
  engine->vm.frame=37;
  gml_room_enter(&engine->vm,1);
  GmlRtLayer *active=gml_rt_layer_new(&engine->vm);
  if(!active){ ok=0; goto done; }
  int active_id=active->id;
  if(!save_state(engine,&baseline,&size)){ ok=fail("dormant baseline save failed"); goto done; }
  candidate=malloc(size);
  if(!candidate){ ok=0; goto done; }
  size_t vm_start=STATE_HEADER_SIZE+(size_t)read_u64(baseline+64)+(size_t)read_u64(baseline+72);
  size_t vm_end=vm_start+(size_t)read_u64(baseline+80);
  /* Fingerprint the declared room/count/age/layer header, independently of the
   * decoder. Assert every following table boundary before corrupting fields. */
  uint8_t prefix[28]={0};
  write_u32(prefix,1); write_u32(prefix+4,0); write_u64(prefix+8,37);
  write_u32(prefix+16,2); write_u32(prefix+20,1); write_u32(prefix+24,(uint32_t)layer_id);
  size_t record=0,matches=0;
  for(size_t at=vm_start;at+sizeof prefix<=vm_end;at++)
    if(!memcmp(baseline+at,prefix,sizeof prefix)){ record=at; matches++; }
  size_t elements=record+220,shader=record+408,maps=record+424;
  StateCursor cursor={baseline,size,elements,1};
  if(matches!=1 || maps+4+64+56>vm_end || cursor_u32(&cursor)!=1 ||
     cursor_u32(&cursor)!=1 || cursor_u32(&cursor)!=(uint32_t)element_id){
    ok=fail("dormant layer/element record boundaries are not exact"); goto done;
  }
  cursor.offset=maps;
  if(cursor_u32(&cursor)!=2 || cursor_u32(&cursor)!=(uint32_t)map_id){
    ok=fail("dormant map boundary is not exact"); goto done;
  }
  const struct { size_t offset; uint32_t value; const char *label; } mutations[]={
    {record,UINT32_MAX,"negative dormant room count"},
    {record,3,"dormant count exceeds room table"},
    {record+4,UINT32_MAX,"negative dormant room identity"},
    {record+4,1,"active room declared dormant"},
    {record+4,2,"dormant room outside content"},
    {record+16,4097,"excessive dormant layer slots"},
    {record+20,2,"invalid dormant layer used flag"},
    {record+24,UINT32_MAX,"negative dormant layer identity"},
    {record+24,(uint32_t)active_id,"dormant layer duplicates active identity"},
    {elements,1000001,"excessive dormant element slots"},
    {elements,1000000,"dormant slots exceed remaining markers"},
    {elements+4,2,"invalid dormant element used flag"},
    {elements+8,(uint32_t)layer_id,"element duplicates a layer identity"},
    {maps,513,"excessive dormant map count"},
    {maps+4,UINT32_MAX,"negative dormant map identity"},
    {maps+4+64,(uint32_t)map_id,"duplicate dormant map identity"},
    {maps+4+12,99,"dormant map parent mismatch"},
    {maps+4+20,3,"dormant map width disagrees with content"},
    {maps+4+52,1000000,"dormant sparse extent exceeds bytes"},
    {vm_start+4,12,"previous dormant-free VM schema"}
  };
  for(size_t i=0;ok && i<sizeof mutations/sizeof mutations[0];i++)
    ok=reject_payload_u32(engine,candidate,size,baseline,size,mutations[i].offset,
                          mutations[i].value,mutations[i].label);
  for(int i=0;ok && i<3;i++){
    memcpy(candidate,baseline,size);
    write_double(candidate+shader,i==0?NAN:(i==1?-1.0:INFINITY));
    refresh_checksum(candidate);
    ok=reject_unchanged(engine,candidate,size,baseline,size,"invalid dormant shader binding");
  }
  if(ok){
    memcpy(candidate,baseline,size); write_u64(candidate+record+8,UINT64_MAX);
    refresh_checksum(candidate);
    ok=reject_unchanged(engine,candidate,size,baseline,size,"negative dormant room age");
  }
  if(ok){
    memcpy(candidate,baseline,size);
    write_double(candidate+maps+4+28,91); write_u32(candidate+vm_end,0);
    refresh_checksum(candidate);
    ok=reject_unchanged(engine,candidate,size,baseline,size,"late failure after dormant publication");
  }
  if(ok){
    gml_room_enter(&engine->vm,0);
    ok=anygm_state_load(engine,baseline,size)==ANYGM_OK && engine_matches(engine,baseline,size);
    if(!ok) fail("dormant restore after reactivation is not byte-exact");
  }
  if(ok){
    gml_room_enter(&engine->vm,0);
    GmlTileMap *map=gml_tilemap_find(&engine->vm,map_id);
    element=gml_rt_elem_find(&engine->vm,element_id);
    ok=map && map->x==11.25 && map->y==-7.5 && map->tiles[0]==9 &&
      element && element->alpha==0.25 && engine->vm.frame-engine->vm.room_enter_frame==37;
    if(!ok) fail("dormant restore did not retain map, element and room age");
  }
  if(ok) ok=anygm_reset(engine)==ANYGM_OK &&
    anygm_state_load(engine,baseline,size)==ANYGM_OK && engine_matches(engine,baseline,size);
done:
  free(candidate); free(baseline); anygm_destroy(engine); free(content);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

/* Match the complete independently encoded two-call table inside the bounded VM section.
 * This makes mutations field-specific without importing the private pool representation. */
static size_t delayed_table(const uint8_t *bytes,size_t size,uint32_t first,
                            uint32_t second,double callback){
  enum { TABLE_BYTES=12+2*64 };
  uint8_t expected[TABLE_BYTES]={0};
  write_u32(expected,second+1); write_u32(expected+4,1); write_u32(expected+8,2);
  for(int i=0;i<2;i++){
    uint8_t *record=expected+12+i*64;
    write_u32(record,i?second:first); write_u32(record+4,2);
    write_double(record+8,i?7:123.25); write_double(record+16,i?7:123.25);
    write_u32(record+24,i?1:0); write_u32(record+28,1);
    write_u32(record+32,i?UINT32_MAX:1); write_u32(record+36,i?UINT32_MAX:1);
    write_u32(record+44,1); write_u32(record+48,V_REAL);
    write_double(record+52,callback); write_u32(record+60,V_UNDEF);
  }
  if(size<STATE_HEADER_SIZE) return SIZE_MAX;
  uint64_t core=read_u64(bytes+64),render=read_u64(bytes+72),vm=read_u64(bytes+80);
  size_t start=STATE_HEADER_SIZE;
  if(core>size-start) return SIZE_MAX;
  start+=(size_t)core;
  if(render>size-start) return SIZE_MAX;
  start+=(size_t)render;
  if(vm>size-start || vm<TABLE_BYTES) return SIZE_MAX;
  size_t found=SIZE_MAX;
  for(size_t offset=start;offset<=start+(size_t)vm-TABLE_BYTES;offset++){
    if(memcmp(bytes+offset,expected,sizeof expected)) continue;
    if(found!=SIZE_MAX) return SIZE_MAX;
    found=offset;
  }
  return found;
}

static int delayed_call_state_cases(const AnygmHostServices *services,
                                     const AnygmContentSource *source){
  AnygmEngine *engine=NULL; uint8_t *baseline=NULL,*candidate=NULL; size_t size=0;
  int ok=anygm_create(services,&engine)==ANYGM_OK;
  if(!ok) return fail("delayed-call engine creation failed");
  ok=anygm_load(engine,source,NULL)==ANYGM_OK && engine->win.n_code>0;
  if(!ok){fail("delayed-call state content failed"); goto done;}
  GmlVal args[]={vreal(123.25),vreal(0),vreal(GML_FUNCVAL_TAG),vreal(0)};
  GmlVal first=gml_builtin_call(&engine->vm,"call_later",args,4);
  args[0]=vreal(7); args[1]=vreal(1); args[3]=vreal(1);
  GmlVal second=gml_builtin_call(&engine->vm,"call_later",args,4);
  ok=first.t==V_REAL && second.t==V_REAL && first.d>=3 && second.d>first.d &&
     save_state(engine,&baseline,&size);
  if(!ok){fail("delayed-call baseline failed"); goto done;}
  size_t table=delayed_table(baseline,size,(uint32_t)first.d,(uint32_t)second.d,args[2].d);
  if(table==SIZE_MAX){ok=fail("delayed-call table is not uniquely identified"); goto done;}
  size_t record=table+12,loop=record+64;
  candidate=malloc(size);
  if(!candidate){ok=fail("delayed-call mutation allocation failed"); goto done;}
  /* The valid table must load before its corrupt variants have diagnostic value. */
  gml_builtin_call(&engine->vm,"call_cancel",&first,1);
  gml_builtin_call(&engine->vm,"call_cancel",&second,1);
  ok=anygm_state_load(engine,baseline,size)==ANYGM_OK && engine_matches(engine,baseline,size);
  if(!ok){fail("valid delayed-call state does not restore exactly"); goto done;}
  const struct {size_t offset; uint32_t value; const char *label;} mutations[]={
    {table,(uint32_t)second.d,"non-advancing next timer identity"},
    {table,UINT32_MAX,"overflowing next timer identity"},
    {table+8,257,"oversized shared timer count"},
    {record,(uint32_t)second.d,"duplicate timer identity"},
    {record,UINT32_MAX,"overflowing timer identity"},
    {record+4,(uint32_t)second.d,"ordinary timer parented by a hidden timer"},
    {record+4,UINT32_MAX,"missing timer parent"},
    {record+24,2,"invalid delayed-call units"},
    {record+28,0,"initial hidden timer state"},
    {record+28,2,"paused hidden timer state"},
    {record+32,2,"finite multi-repeat hidden timer"},
    {record+36,2,"excess single-shot repetitions"},
    {loop+36,0,"inconsistent looping repetitions"},
    {record+40,UINT32_MAX,"negative completed repetitions"},
    {record+44,0,"nearest-expiry hidden timer"},
    {record+48,UINT32_MAX,"invalid callback value kind"},
    {record+60,UINT32_MAX,"invalid hidden callback argument kind"}
  };
  for(size_t i=0;ok && i<sizeof mutations/sizeof mutations[0];i++)
    ok=reject_payload_u32(engine,candidate,size,baseline,size,mutations[i].offset,
                          mutations[i].value,mutations[i].label);
  const struct {size_t offset; double value; const char *label;} reals[]={
    {record+8,0,"zero delayed-call period"},
    {record+8,NAN,"nonfinite delayed-call period"},
    {record+16,124,"remaining delay exceeds its period"},
    {record+16,-1,"negative remaining delay"},
    {record+16,INFINITY,"infinite remaining delay"},
    {loop+8,7.5,"fractional frame period"},
    {record+52,NAN,"nonfinite callback identity"},
    {record+52,2147483648.0,"overflowing callback identity"},
    {record+52,-1,"negative callback identity"},
    {record+52,GML_FUNCVAL_TAG+0.5,"fractional callback identity"},
    {record+52,0,"untagged callback identity"}
  };
  for(size_t i=0;ok && i<sizeof reals/sizeof reals[0];i++){
    memcpy(candidate,baseline,size); write_double(candidate+reals[i].offset,reals[i].value);
    refresh_checksum(candidate);
    ok=reject_unchanged(engine,candidate,size,baseline,size,reals[i].label);
  }
  if(ok){
    /* A valid source mutation followed by an audio failure must roll back too. */
    size_t audio_start=STATE_HEADER_SIZE+(size_t)read_u64(baseline+64)+
      (size_t)read_u64(baseline+72)+(size_t)read_u64(baseline+80);
    memcpy(candidate,baseline,size); write_double(candidate+record+16,12);
    write_u32(candidate+audio_start,0); refresh_checksum(candidate);
    ok=reject_unchanged(engine,candidate,size,baseline,size,"late rejection after delayed-call decoding");
  }
  if(ok){
    size_t vm_start=STATE_HEADER_SIZE+(size_t)read_u64(baseline+64)+(size_t)read_u64(baseline+72);
    ok=reject_payload_u32(engine,candidate,size,baseline,size,vm_start+4,13,"previous hidden-free VM schema");
  }
  if(ok){
    memcpy(candidate,baseline,size); write_u32(candidate+4,24);
    ok=reject_unchanged(engine,candidate,size,baseline,size,"previous hidden-free root schema");
  }
  if(ok) ok=anygm_reset(engine)==ANYGM_OK && anygm_state_size(engine)<size &&
    anygm_state_load(engine,baseline,size)==ANYGM_OK && engine_matches(engine,baseline,size);
  if(!ok) fail("delayed-call state contract failed");
done:
  free(candidate); free(baseline); anygm_destroy(engine); return ok;
}

int main(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_list_override_content_create(&fixture))
    return fail("fixture creation failed")?0:1;
  uint8_t *content=NULL;
  size_t content_size=0;
  if(!anygm_synthetic_content_read(&fixture,&content,&content_size))
    return fail("fixture read failed")?0:1;

  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmEngine *engine=NULL;
  if(anygm_create(&services,&engine)!=ANYGM_OK) return fail("engine creation failed")?0:1;
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_MEMORY;
  source.path="synthetic-state.win";
  source.data=content;
  source.size=content_size;
  if(anygm_load(engine,&source,NULL)!=ANYGM_OK) return fail("content load failed")?0:1;

  /* Reject a bounded PATH table at the VM/content boundary, including after a renderer
   * existed. A failed cold Reset must also leave an empty, reloadable engine. */
  const GmlChunk *path_chunk=gml_chunk(&engine->win,"PATH");
  if(!path_chunk || path_chunk->off>content_size || content_size-path_chunk->off<4)
    return fail("path rejection fixture has no bounded chunk")?0:1;
  uint8_t *bad_content=malloc(content_size);
  if(!bad_content) return fail("path rejection fixture allocation failed")?0:1;
  memcpy(bad_content,content,content_size);
  write_u32(bad_content+path_chunk->off,GML_PATH_MAX_COUNT+1);
  anygm_unload(engine);
  source.data=bad_content;
  if(anygm_load(engine,&source,NULL)!=ANYGM_ERROR_INVALID_CONTENT || engine->loaded ||
     engine->win.data || engine->lifecycle!=ENGINE_EMPTY)
    return fail("invalid path table did not reject cleanly")?0:1;
  free(bad_content); source.data=content;
  if(anygm_load(engine,&source,NULL)!=ANYGM_OK)
    return fail("reload after path rejection failed")?0:1;
  for(int i=0;i<engine->win.n_chunks;i++)
    if(!strcmp(engine->win.chunks[i].name,"PATH")) engine->win.chunks[i].size=3;
  if(anygm_reset(engine)!=ANYGM_ERROR_INVALID_CONTENT || engine->loaded ||
     engine->win.data || engine->lifecycle!=ENGINE_EMPTY)
    return fail("invalid path Reset did not release runtime owners")?0:1;
  if(anygm_load(engine,&source,NULL)!=ANYGM_OK)
    return fail("reload after rejected Reset failed")?0:1;

  AnygmInputFrame input={0};
  AnygmFrameOutput output={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  output.struct_size=sizeof output;
  if(anygm_run_frame(engine,&input,&output)!=ANYGM_OK) return fail("initial frame failed")?0:1;

  /* Put the first definition in argument zero: later roots must refer back to it, including
   * the self-cycle. This also makes transactional byte equality sensitive to lost aliases. */
  GmlVal graph=gml_arr_new(2,vreal(7));
  gml_arr_set(graph,0,graph);
  engine->vm.script_args[0]=engine->vm.script_args[1]=graph;
  engine->vm.script_argc=2;
  *gml_varmap_put(&engine->vm.globals,"state_graph")=graph;

  const GmlPathControl path_controls[]={{10,20,100},{13,24,0}};
  int path_index=gml_path_add(&engine->vm);
  if(path_index!=0 || !gml_path_replace(&engine->vm,path_index,path_controls,2,0,0,4))
    return fail("runtime path fixture failed")?0:1;

  uint8_t *baseline=NULL;
  size_t state_size=0;
  if(!save_state(engine,&baseline,&state_size)||state_size<=STATE_HEADER_SIZE)
    return fail("baseline state failed")?0:1;
  uint8_t *candidate=malloc(state_size+64);
  if(!candidate) return fail("mutation allocation failed")?0:1;
  int ok=1;

  const size_t header_offsets[]={0,4,8,12,16,24,32,36,40,48,56,64,72,80,88,96,104};
  for(size_t i=0;ok&&i<sizeof header_offsets/sizeof header_offsets[0];i++){
    memcpy(candidate,baseline,state_size);
    candidate[header_offsets[i]]^=UINT8_C(0x5a);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"mutated header field");
  }

  uint64_t core_size=read_u64(baseline+64);
  uint64_t render_size=read_u64(baseline+72);
  uint64_t vm_size=read_u64(baseline+80);
  uint64_t audio_size=read_u64(baseline+88);
  size_t render_start=STATE_HEADER_SIZE+(size_t)core_size;
  size_t vm_start=render_start+(size_t)render_size;
  size_t audio_start=vm_start+(size_t)vm_size;
  if(audio_start+(size_t)audio_size!=state_size) ok=fail("section arithmetic is inconsistent");

  /* Object properties follow the path table. Assert the synthetic table's exact
   * extent before moving the existing path mutations to their current offsets. */
  const size_t object_bytes=4+28;
  size_t object_table=audio_start-object_bytes;
  StateCursor object_cursor={baseline,state_size,object_table,1};
  if(engine->vm.n_objects!=1 || cursor_u32(&object_cursor)!=1)
    ok=fail("object mutation fixture does not identify its table");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               object_table,UINT32_MAX,"negative object count");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               object_table,0,"missing object record");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               object_table,2,"object count exceeds content");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               object_table+12,0,"self object parent");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               object_table+12,1,"out-of-range object parent");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               vm_start+4,10,"previous object-free VM schema");
  if(ok){
    memcpy(candidate,baseline,state_size);
    /* A valid replacement must be published before the later audio rejection.
     * Exact engine equality then proves rollback covers all seven object words. */
    const int words[]={7,8,engine->vm.objects[0].parent==-1?-100:-1,-123,0,1,1};
    for(int i=0;i<7;i++) write_u32(candidate+object_table+4+(size_t)i*4,(uint32_t)words[i]);
    write_u32(candidate+audio_start,0);
    refresh_checksum(candidate);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,
                        "audio rejection after object-property publication");
  }

  /* One runtime path: table header, metadata, two samples, two controls. */
  const size_t path_record_size=4+3*4+1+8+4+2*4*8+4+2*3*8;
  size_t path_table=object_table-12-path_record_size;
  StateCursor path_cursor={baseline,state_size,path_table,1};
  if(cursor_u32(&path_cursor)!=1 || cursor_u32(&path_cursor)!=0 ||
     cursor_u32(&path_cursor)!=1 || cursor_u32(&path_cursor)!=0)
    ok=fail("path mutation fixture does not identify its table");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               path_table,GML_PATH_MAX_COUNT+1u,"oversized path table");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               path_table+8,0,"missing runtime path record");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               path_table+12+25,UINT32_MAX,"oversized sampled path count");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               path_table+12+93,UINT32_MAX,"oversized defining path count");
  if(ok){
    memcpy(candidate,baseline,state_size);
    write_u64(candidate+path_table+12+97,UINT64_C(0x7ff8000000000000));
    refresh_checksum(candidate);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"nonfinite path control");
  }
  if(ok){
    memcpy(candidate,baseline,state_size); candidate[path_table+12+16]=1;
    refresh_checksum(candidate);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"populated deleted path");
  }

  /* VM header and fixed scalar prefix end at 132. The sparse two-element definition has
   * a self reference then a real; argument one is the first cross-root reference. */
  StateCursor graph_cursor={baseline,state_size,vm_start+132,1};
  if(cursor_u32(&graph_cursor)!=V_ARR || cursor_u32(&graph_cursor)!=UINT32_C(0x80000002))
    ok=fail("array mutation fixture does not identify the first definition");
  graph_cursor.offset=vm_start+180;
  if(cursor_u32(&graph_cursor)!=4 || cursor_u32(&graph_cursor)!=1)
    ok=fail("array mutation fixture does not identify the cross-root reference");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               vm_start+160,2,"forward array reference inside a definition");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               vm_start+184,UINT32_MAX,"unknown cross-root array reference");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               vm_start+164,0,"duplicate sparse array index");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               vm_start+136,UINT32_MAX,"oversized array definition");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               vm_start+4,8,"previous VM tree schema");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               vm_start+4,9,"previous sampled-only path schema");
  if(ok){
    memcpy(candidate,baseline,state_size);
    write_u32(candidate+4,ANYGM_STATE_SCHEMA-1u);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"previous path state schema");
  }
  if(ok){
    memcpy(candidate,baseline,state_size);
    write_u32(candidate+4,17);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"previous public schema");
  }

  size_t truncations[]={0,1,3,4,7,8,15,16,STATE_HEADER_SIZE-1,STATE_HEADER_SIZE,
                        render_start-1,render_start,vm_start-1,vm_start,
                        audio_start-1,audio_start,state_size-1};
  for(size_t i=0;ok&&i<sizeof truncations/sizeof truncations[0];i++)
    ok=reject_unchanged(engine,baseline,truncations[i],baseline,state_size,
                        "truncated section boundary");

  const size_t payload_mutations[]={STATE_HEADER_SIZE,STATE_HEADER_SIZE+core_size/2,
                                    render_start,render_start+render_size/2,
                                    vm_start,vm_start+vm_size/2,audio_start,
                                    audio_start+audio_size/2,state_size-1};
  for(size_t i=0;ok&&i<sizeof payload_mutations/sizeof payload_mutations[0];i++){
    memcpy(candidate,baseline,state_size);
    candidate[payload_mutations[i]]^=UINT8_C(0xa5);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,
                        "mutated checksummed payload");
  }

  if(ok){
    memcpy(candidate,baseline,state_size);
    memset(candidate+state_size,0,64);
    ok=anygm_state_load(engine,candidate,state_size+64)==ANYGM_OK&&
       engine_matches(engine,baseline,state_size);
    if(!ok) fail("zero-filled transport capacity was rejected");
  }
  if(ok){
    candidate[state_size+31]=1;
    ok=reject_unchanged(engine,candidate,state_size+64,baseline,state_size,
                        "nonzero transport tail");
  }

  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               STATE_CORE_FIELDS_OFFSET,0,"zero frame width");
  size_t mouse_mask_offset=STATE_CORE_FIELDS_OFFSET+44+
    sizeof engine->pad_current+sizeof engine->pad_previous+
    sizeof engine->key_current+sizeof engine->key_previous;
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               mouse_mask_offset,8,"unsupported mouse suppression bit");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               mouse_mask_offset,UINT32_MAX,"extreme mouse suppression mask");
  if(ok){
    memcpy(candidate,baseline,state_size);
    write_u64(candidate+STATE_CORE_FIELDS_OFFSET+12,UINT64_C(0x7ff8000000000000));
    refresh_checksum(candidate);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"non-finite frame rate");
  }
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               render_start,UINT32_MAX,"extreme font count");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               render_start+20,UINT32_MAX,"extreme font map count");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               vm_start,0,"invalid VM magic");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               vm_start+4,UINT32_MAX,"invalid VM schema");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               vm_start+8,UINT32_MAX,"extreme VM instance count");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               audio_start,0,"invalid audio magic");

  if(ok&&render_size>=4){
    memcpy(candidate,baseline,state_size);
    write_u64(candidate+64,core_size+4);
    write_u64(candidate+72,render_size-4);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"shifted core boundary");
  }
  if(ok&&vm_size>=4){
    memcpy(candidate,baseline,state_size);
    write_u64(candidate+72,render_size+4);
    write_u64(candidate+80,vm_size-4);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"shifted render boundary");
  }
  if(ok&&audio_size>=4){
    memcpy(candidate,baseline,state_size);
    write_u64(candidate+80,vm_size+4);
    write_u64(candidate+88,audio_size-4);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"shifted VM boundary");
  }
  if(ok){
    memcpy(candidate,baseline,state_size);
    write_u64(candidate+64,UINT64_MAX);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"overflowing section size");
  }

  if(ok){
    char long_expression[ANYGM_MAX_RUNTIME_OVERRIDE_EXPRESSION+2];
    memset(long_expression,'x',sizeof long_expression-1);
    long_expression[sizeof long_expression-1]=0;
    ok=anygm_set_runtime_override(engine,0,1,"")==ANYGM_ERROR_INVALID_ARGUMENT&&
       anygm_set_runtime_override(engine,ANYGM_MAX_RUNTIME_OVERRIDES,1,"x")==
         ANYGM_ERROR_INVALID_ARGUMENT&&
       anygm_set_runtime_override(engine,0,1,long_expression)==ANYGM_ERROR_INVALID_ARGUMENT&&
       anygm_set_runtime_override(engine,0,0,NULL)==ANYGM_OK&&
       engine_matches(engine,baseline,state_size);
    if(!ok) fail("runtime override bounds were not transactional");
  }

  if(ok) ok=runtime_mask_state_cases(engine);

  if(ok) ok=tilemap_state_cases(&services);
  if(ok) ok=dormant_visual_state_cases(&services);

  if(ok) ok=delayed_call_state_cases(&services,&source);

  if(ok) ok=content_override_state_cases(&services,&fixture);

  free(candidate);
  free(baseline);
  anygm_destroy(engine);
  free(content);
  anygm_synthetic_content_destroy(&fixture);
  if(!ok) return 1;
  puts("bounded state corpus: ok");
  return 0;
}
