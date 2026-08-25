/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"
#include "engine_internal.h"
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

  AnygmInputFrame input={0};
  AnygmFrameOutput output={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  output.struct_size=sizeof output;
  if(anygm_run_frame(engine,&input,&output)!=ANYGM_OK) return fail("initial frame failed")?0:1;

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
