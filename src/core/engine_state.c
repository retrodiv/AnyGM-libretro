/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Root state framing, identity checks, canonical section order, and
 * transactional restore for the single shared engine. */
#include "engine_internal.h"
#include "anygm_host.h"
#include "anygm_vfs.h"
#include "gml_render_state.h"


typedef struct { uint8_t *data; size_t cap, pos; int ok; } CoreW;
typedef struct { const uint8_t *data; size_t cap, pos; int ok; } CoreR;
static void cw_raw(CoreW *s, const void *p, size_t n){
  if(n>SIZE_MAX-s->pos){ s->ok=0; s->pos=SIZE_MAX; return; }
  if(s->data){ if(s->pos<=s->cap && n<=s->cap-s->pos) memcpy(s->data+s->pos,p,n); else s->ok=0; }
  s->pos+=n;
}
static void cr_raw(CoreR *s, void *p, size_t n){
  if(n>SIZE_MAX-s->pos){ memset(p,0,n); s->ok=0; s->pos=SIZE_MAX; return; }
  if(s->pos<=s->cap && n<=s->cap-s->pos) memcpy(p,s->data+s->pos,n);
  else { memset(p,0,n); s->ok=0; }
  s->pos+=n;
}
static void cw_u32(CoreW *s, uint32_t v){
  uint8_t bytes[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
  cw_raw(s,bytes,sizeof bytes);
}
static void cw_u32_array(CoreW *s,const uint32_t *values,size_t count){
  if(count>SIZE_MAX/4u){ s->ok=0; s->pos=SIZE_MAX; return; }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
  cw_raw(s,values,count*4u);
#else
  for(size_t index=0;index<count;index++) cw_u32(s,values[index]);
#endif
}
static void cw_u64(CoreW *s, uint64_t v){
  uint8_t bytes[8];
  for(unsigned i=0;i<8;i++) bytes[i]=(uint8_t)(v>>(i*8));
  cw_raw(s,bytes,sizeof bytes);
}
static void cw_i64(CoreW *s, int64_t v){ cw_u64(s,(uint64_t)v); }
static void cw_i32(CoreW *s, int v){ cw_u32(s,(uint32_t)(int32_t)v); }
static void cw_d(CoreW *s, double v){ uint64_t bits=0; memcpy(&bits,&v,sizeof bits); cw_u64(s,bits); }
static uint32_t cr_u32(CoreR *s){
  uint8_t bytes[4]={0}; cr_raw(s,bytes,sizeof bytes);
  return (uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|((uint32_t)bytes[2]<<16)|((uint32_t)bytes[3]<<24);
}
static void cr_u32_array(CoreR *s,uint32_t *values,size_t count){
  if(count>SIZE_MAX/4u){ s->ok=0; s->pos=SIZE_MAX; return; }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
  cr_raw(s,values,count*4u);
#else
  for(size_t index=0;index<count;index++) values[index]=cr_u32(s);
#endif
}
static uint64_t cr_u64(CoreR *s){
  uint8_t bytes[8]={0}; cr_raw(s,bytes,sizeof bytes); uint64_t value=0;
  for(unsigned i=0;i<8;i++) value|=(uint64_t)bytes[i]<<(i*8);
  return value;
}
static int64_t cr_i64(CoreR *s){ return (int64_t)cr_u64(s); }
static int cr_i32(CoreR *s){ return (int)(int32_t)cr_u32(s); }
static double cr_d(CoreR *s){ uint64_t bits=cr_u64(s); double value=0; memcpy(&value,&bits,sizeof value); return value; }

/* The completed frame selects raw pixels or a row table plus run-length encoded literal rows.
 * Presentation canvases are dominated by repetition an upscale manufactures - integer scales
 * repeat adjacent rows and letterboxes repeat black ones - and the row table removes it before the
 * runs are counted. Detailed authored rasters use one bounded raw copy when their run stream would
 * be larger. A row entry names the root of an adjacent identical row, or the literal marker when
 * the row's pixels follow in the run stream, in row order. */
#define ANYGM_FRAME_ROW_LITERAL UINT32_C(0xFFFFFFFF)
enum { ANYGM_FRAME_ENCODING_RAW=1, ANYGM_FRAME_ENCODING_ROW_RLE=2 };

size_t engine_state_frame_capacity(const AnygmEngine *engine){
  /* Ceiling of state_write_completed_frame under the geometry this session is already known to
   * reach: width, height, encoding, literal count and run count cost 20 bytes, the row table four per row,
   * and a fully literal frame of single-pixel runs costs eight bytes per pixel. The configured
   * virtual monitor counts even before the first frame presents, because a frontend that sizes a
   * rewind ring does it once, at load, when none of the presentation has happened yet. */
  size_t w=engine->output_width,h=engine->output_height;
  if(engine->config.monitor_width>w) w=engine->config.monitor_width;
  if(engine->config.monitor_height>h) h=engine->config.monitor_height;
  if((size_t)engine->state_frame_width>w) w=engine->state_frame_width;
  if((size_t)engine->state_frame_height>h) h=engine->state_frame_height;
  if(!w || !h){ w=engine->width; h=engine->height; }
  if(w>FB_MAX_W) w=FB_MAX_W;
  if(h>FB_MAX_H) h=FB_MAX_H;
  return 20u+4u*h+8u*w*h;
}

static void state_write_completed_frame(AnygmEngine *engine,CoreW *state){
  unsigned width=0,height=0;
  /* The state carries the exact completed frame. A frame that was produced somewhere other than
   * the processor has to be produced here first: serializing the buffer as it stands would write
   * whatever the previous frame left in it. */
  if(!engine->state_omit_frame){
    engine_materialize_completed_frame(engine);
    if(engine->have_presented_frame && engine->screen && engine->output_width && engine->output_height){
      width=engine->output_width;
      height=engine->output_height;
    } else if(engine->state_frame_available && engine->screen){
      width=engine->state_frame_width;
      height=engine->state_frame_height;
    }
  }
  if(width>FB_MAX_W || height>FB_MAX_H ||
     (width && (height==0 || (size_t)width>SIZE_MAX/(size_t)height))){
    state->ok=0;
    width=height=0;
  }
  cw_u32(state,width);
  cw_u32(state,height);
  if(!width || !height){ cw_u32(state,0); return; }
  uint32_t *rows=malloc((size_t)height*sizeof(uint32_t));
  if(!rows){ state->ok=0; return; }
  uint32_t literal_rows=0;
  for(uint32_t y=0;y<height;y++){
    /* Every repetition produced by an integer vertical scale or letterbox is adjacent. Retaining
     * only that useful match avoids hashing every pixel and scanning all earlier row hashes for a
     * detailed native frame which will be stored raw anyway. */
    uint32_t match=y;
    const uint32_t *row=engine->screen+(size_t)y*width;
    if(y && !memcmp(row-width,row,(size_t)width*sizeof(uint32_t))) match=rows[y-1];
    rows[y]=match;
    if(rows[y]==y) literal_rows++;
  }
  /* Runs over the literal rows in row order; a run never crosses a row boundary, so the reader
   * decodes straight into each destination row. */
  uint32_t runs=0;
  size_t pixels=(size_t)width*height;
  size_t raw_bytes=pixels*4u;
  size_t rle_base=(size_t)height*4u+8u;
  int use_raw=raw_bytes<=rle_base;
  for(uint32_t y=0;y<height && state->ok;y++){
    if(use_raw) break;
    if(rows[y]!=y) continue;
    const uint32_t *row=engine->screen+(size_t)y*width;
    for(unsigned at=0;at<width;){
      uint32_t value=row[at]; unsigned rend=at+1;
      while(rend<width && row[rend]==value) rend++;
      if(runs==UINT32_MAX){ state->ok=0; break; }
      runs++; at=rend;
      /* Every remaining literal can only make the RLE larger. Stop as soon as its lower bound
       * reaches raw size instead of scanning the rest of a detailed frame. */
      if((size_t)runs>(raw_bytes-rle_base)/8u){ use_raw=1; break; }
    }
  }
  if(use_raw || raw_bytes<=rle_base+(size_t)runs*8u){
    cw_u32(state,ANYGM_FRAME_ENCODING_RAW);
    cw_u32_array(state,engine->screen,pixels);
    free(rows);
    return;
  }
  cw_u32(state,ANYGM_FRAME_ENCODING_ROW_RLE);
  for(uint32_t y=0;y<height;y++)
    cw_u32(state,rows[y]==y?ANYGM_FRAME_ROW_LITERAL:rows[y]);
  cw_u32(state,literal_rows);
  cw_u32(state,runs);
  for(uint32_t y=0;y<height && state->ok;y++){
    if(rows[y]!=y) continue;
    const uint32_t *row=engine->screen+(size_t)y*width;
    for(unsigned at=0;at<width;){
      uint32_t value=row[at]; unsigned rend=at+1;
      while(rend<width && row[rend]==value) rend++;
      cw_u32(state,(uint32_t)(rend-at));
      cw_u32(state,value);
      at=rend;
    }
  }
  free(rows);
}

static int state_read_completed_frame(AnygmEngine *engine,CoreR *state){
  uint32_t width=cr_u32(state),height=cr_u32(state);
  if(width>FB_MAX_W || height>FB_MAX_H || (!!width != !!height)) return 0;
  size_t pixels=(size_t)width*height;
  if(!pixels){
    uint32_t runs=cr_u32(state);
    if(runs || !state->ok) return 0;
    engine->state_frame_available=0;
    engine->state_frame_width=0;
    engine->state_frame_height=0;
    return 1;
  }
  uint32_t encoding=cr_u32(state);
  if(encoding==ANYGM_FRAME_ENCODING_RAW){
    cr_u32_array(state,engine->screen,pixels);
    if(!state->ok) return 0;
    engine->state_frame_available=1;
    engine->state_frame_width=width;
    engine->state_frame_height=height;
    return 1;
  }
  if(encoding!=ANYGM_FRAME_ENCODING_ROW_RLE) return 0;
  uint32_t *rows=malloc((size_t)height*sizeof(uint32_t));
  if(!rows) return 0;
  uint32_t literal_declared=0;
  for(uint32_t y=0;y<height;y++){
    uint32_t reference=cr_u32(state);
    if(reference==ANYGM_FRAME_ROW_LITERAL){ rows[y]=y; literal_declared++; }
    else if(reference<y && rows[reference]==reference) rows[y]=reference;
    else { free(rows); return 0; }
  }
  uint32_t literal_count=cr_u32(state);
  uint32_t runs=cr_u32(state);
  if(!state->ok || literal_count!=literal_declared || (literal_count && !runs) ||
     (uint64_t)runs>(uint64_t)literal_count*width){ free(rows); return 0; }
  uint32_t consumed=0;
  for(uint32_t y=0;y<height;y++){
    if(rows[y]!=y) continue;
    uint32_t *row=engine->screen+(size_t)y*width;
    unsigned at=0;
    while(at<width){
      if(consumed>=runs){ free(rows); return 0; }
      uint32_t count=cr_u32(state),value=cr_u32(state);
      consumed++;
      if(!count || count>width-at){ free(rows); return 0; }
      for(uint32_t pixel=0;pixel<count;pixel++) row[at++]=value;
    }
  }
  if(!state->ok || consumed!=runs){ free(rows); return 0; }
  for(uint32_t y=0;y<height;y++){
    if(rows[y]==y) continue;
    memcpy(engine->screen+(size_t)y*width,engine->screen+(size_t)rows[y]*width,
           (size_t)width*sizeof(uint32_t));
  }
  free(rows);
  engine->state_frame_available=1;
  engine->state_frame_width=width;
  engine->state_frame_height=height;
  return 1;
}

enum {
  ANYGM_STATE_HEADER_SIZE=112,
  ANYGM_STATE_CONTENT_LOCATOR_SIZE=1024,
  ANYGM_STATE_LAUNCH_PARAMETERS_SIZE=1024,
  ANYGM_STATE_ENCODING_LITTLE_ENDIAN_IEEE754=1
};
#define ANYGM_STATE_MAGIC UINT32_C(0x54534741)

_Static_assert(sizeof(((AnygmEngine *)0)->state_content_locator)==
               ANYGM_STATE_CONTENT_LOCATOR_SIZE,"state content locator size");
_Static_assert(sizeof(((AnygmEngine *)0)->launch_parameters)==
               ANYGM_STATE_LAUNCH_PARAMETERS_SIZE,"state launch parameter size");

typedef struct {
  uint64_t total_size;
  uint64_t content_fingerprint;
  uint64_t compatibility_fingerprint;
  uint64_t config_fingerprint;
  uint64_t payload_checksum;
  uint64_t core_size;
  uint64_t render_size;
  uint64_t vm_size;
  uint64_t audio_size;
  uint64_t payload_size;
} AnygmStateHeader;

typedef struct {
  char content_locator[ANYGM_STATE_CONTENT_LOCATOR_SIZE];
  char launch_parameters[ANYGM_STATE_LAUNCH_PARAMETERS_SIZE];
} AnygmStateLaunch;

uint64_t state_hash_bytes(const void *data,size_t size){
  const uint8_t *bytes=data;
  uint64_t hash=UINT64_C(1469598103934665603);
  while(size>=8){
    uint64_t word=(uint64_t)bytes[0]|((uint64_t)bytes[1]<<8)|((uint64_t)bytes[2]<<16)|
                  ((uint64_t)bytes[3]<<24)|((uint64_t)bytes[4]<<32)|((uint64_t)bytes[5]<<40)|
                  ((uint64_t)bytes[6]<<48)|((uint64_t)bytes[7]<<56);
    hash^=word; hash*=UINT64_C(1099511628211);
    bytes+=8; size-=8;
  }
  while(size--){ hash^=*bytes++; hash*=UINT64_C(1099511628211); }
  return hash;
}

static uint64_t state_current_config_fingerprint(AnygmEngine *engine){
  uint8_t encoded[16]={0};
  CoreW writer={encoded,sizeof encoded,0,1};
  /* Presentation configuration remains host-owned across a state load. Only configuration which
   * changes simulation semantics belongs in the state identity. Active content overrides change
   * what every frame simulates, so their text joins the identity — but only while they are
   * active, which keeps the encoding of override-free content byte-identical to what it always
   * was and lets its states keep loading. */
  cw_u32(&writer,engine->config.god_mode);
  if(engine_boot_cheats_active(engine)>0 && engine->content_overrides_text[0])
    cw_u64(&writer,state_hash_bytes(engine->content_overrides_text,
                                    strlen(engine->content_overrides_text)));
  return writer.ok?state_hash_bytes(encoded,writer.pos):0;
}

void state_identity_refresh(AnygmEngine *engine){
  engine->content_fingerprint=state_hash_bytes(engine->win.data,engine->win.size);
  engine->compatibility_fingerprint=engine->compatibility.fingerprint;
}
static void state_profile_maybe_log(AnygmEngine *engine,size_t total, size_t rendern, size_t vmn, size_t audn){
  if(!anygm_host_development_setting(&engine->host,"GML_STATE_PROFILE")) return;
  if(total <= engine->diagnostics.state_profile_last_total) return;
  engine->diagnostics.state_profile_last_total = total;
  GmlRenderStateProfileMetrics metrics={0};
  if(!gml_render_state_profile_metrics(&engine->render,&metrics)) return;
  engine_logf(engine,ANYGM_LOG_INFO,
           "[state-profile] total=%llu render=%llu vm=%llu audio=%llu surfaces=%d/%llu runtime_sprites=%d raw/%llu file=%d/%llu inst=%d globals=%d\n",
           (unsigned long long)total, (unsigned long long)rendern,
           (unsigned long long)vmn, (unsigned long long)audn,
           metrics.surface_count, (unsigned long long)metrics.surface_bytes,
           metrics.runtime_sprite_count,
           (unsigned long long)metrics.inline_runtime_sprite_bytes,
           metrics.file_runtime_sprite_count,
           (unsigned long long)metrics.file_runtime_sprite_bytes,
           engine->vm.inst_count,engine->vm.globals.len);
}
static void state_header_write(uint8_t *destination,const AnygmStateHeader *header){
  CoreW writer={destination,ANYGM_STATE_HEADER_SIZE,0,1};
  cw_u32(&writer,ANYGM_STATE_MAGIC);
  cw_u32(&writer,ANYGM_STATE_SCHEMA);
  cw_u32(&writer,ANYGM_STATE_HEADER_SIZE);
  cw_u32(&writer,ANYGM_STATE_ENCODING_LITTLE_ENDIAN_IEEE754);
  cw_u64(&writer,header->total_size);
  cw_u64(&writer,header->content_fingerprint);
  cw_u32(&writer,ANYGM_COMPATIBILITY_SCHEMA);
  cw_u32(&writer,0);
  cw_u64(&writer,header->compatibility_fingerprint);
  cw_u64(&writer,header->config_fingerprint);
  cw_u64(&writer,header->payload_checksum);
  cw_u64(&writer,header->core_size);
  cw_u64(&writer,header->render_size);
  cw_u64(&writer,header->vm_size);
  cw_u64(&writer,header->audio_size);
  cw_u64(&writer,header->payload_size);
  cw_u64(&writer,0);
}

static int state_header_read(const void *data,size_t size,AnygmStateHeader *header){
  if(!data || !header || size<ANYGM_STATE_HEADER_SIZE) return 0;
  CoreR reader={(const uint8_t*)data,ANYGM_STATE_HEADER_SIZE,0,1};
  uint32_t magic=cr_u32(&reader);
  uint32_t schema=cr_u32(&reader);
  uint32_t header_size=cr_u32(&reader);
  uint32_t encoding=cr_u32(&reader);
  header->total_size=cr_u64(&reader);
  header->content_fingerprint=cr_u64(&reader);
  uint32_t compatibility_schema=cr_u32(&reader);
  uint32_t flags=cr_u32(&reader);
  header->compatibility_fingerprint=cr_u64(&reader);
  header->config_fingerprint=cr_u64(&reader);
  header->payload_checksum=cr_u64(&reader);
  header->core_size=cr_u64(&reader);
  header->render_size=cr_u64(&reader);
  header->vm_size=cr_u64(&reader);
  header->audio_size=cr_u64(&reader);
  header->payload_size=cr_u64(&reader);
  uint64_t reserved=cr_u64(&reader);
  if(!reader.ok || reader.pos!=ANYGM_STATE_HEADER_SIZE || magic!=ANYGM_STATE_MAGIC ||
     schema!=ANYGM_STATE_SCHEMA || header_size!=ANYGM_STATE_HEADER_SIZE ||
     encoding!=ANYGM_STATE_ENCODING_LITTLE_ENDIAN_IEEE754 ||
     compatibility_schema!=ANYGM_COMPATIBILITY_SCHEMA || flags || reserved) return 0;
  if(header->total_size<ANYGM_STATE_HEADER_SIZE || header->total_size>size ||
     header->payload_size!=header->total_size-ANYGM_STATE_HEADER_SIZE) return 0;
  const uint8_t *all_bytes=data;
  for(size_t i=(size_t)header->total_size;i<size;i++) if(all_bytes[i]) return 0;
  uint64_t sections=header->core_size;
  if(UINT64_MAX-sections<header->render_size) return 0;
  sections+=header->render_size;
  if(UINT64_MAX-sections<header->vm_size) return 0;
  sections+=header->vm_size;
  if(UINT64_MAX-sections<header->audio_size) return 0;
  sections+=header->audio_size;
  if(sections!=header->payload_size || header->payload_size>SIZE_MAX) return 0;
  const uint8_t *payload=(const uint8_t*)data+ANYGM_STATE_HEADER_SIZE;
  return state_hash_bytes(payload,(size_t)header->payload_size)==header->payload_checksum;
}

static int state_header_matches_engine(AnygmEngine *engine,const AnygmStateHeader *header){
  return engine && header &&
    header->content_fingerprint==engine->content_fingerprint &&
    header->compatibility_fingerprint==engine->compatibility_fingerprint &&
    header->config_fingerprint==state_current_config_fingerprint(engine);
}

static int state_read_fixed_string(CoreR *reader,char *destination,size_t capacity){
  if(!reader || !destination || !capacity) return 0;
  cr_raw(reader,destination,capacity);
  if(!reader->ok) return 0;
  size_t length=0;
  while(length<capacity && destination[length]) length++;
  if(length==capacity) return 0;
  for(size_t index=length+1;index<capacity;index++)
    if(destination[index]) return 0;
  return 1;
}

static void state_write_fixed_string(CoreW *writer,const char *value,size_t capacity){
  uint8_t encoded[ANYGM_STATE_CONTENT_LOCATOR_SIZE]={0};
  if(capacity>sizeof encoded){ writer->ok=0; return; }
  size_t length=value?strlen(value):0;
  if(length>=capacity){ writer->ok=0; return; }
  if(length) memcpy(encoded,value,length);
  cw_raw(writer,encoded,capacity);
}

static int state_read_launch(const void *data,const AnygmStateHeader *header,
                             AnygmStateLaunch *launch){
  if(!data || !header || !launch ||
     header->core_size<ANYGM_STATE_CONTENT_LOCATOR_SIZE+
                       ANYGM_STATE_LAUNCH_PARAMETERS_SIZE) return 0;
  const uint8_t *payload=(const uint8_t *)data+ANYGM_STATE_HEADER_SIZE;
  CoreR reader={payload,(size_t)header->core_size,0,1};
  memset(launch,0,sizeof *launch);
  if(!state_read_fixed_string(&reader,launch->content_locator,
                              sizeof launch->content_locator) ||
     !state_read_fixed_string(&reader,launch->launch_parameters,
                              sizeof launch->launch_parameters) ||
     !engine_state_content_locator_valid(launch->content_locator)) return 0;
  return 1;
}

static int state_launch_matches_engine(const AnygmEngine *engine,
                                       const AnygmStateLaunch *launch){
  return engine && launch &&
    !strcmp(engine->state_content_locator,launch->content_locator) &&
    !strcmp(engine->launch_parameters,launch->launch_parameters);
}

static void state_write(AnygmEngine *engine,CoreW *s){
  uint8_t empty_header[ANYGM_STATE_HEADER_SIZE]={0};
  cw_raw(s,empty_header,sizeof empty_header);
  size_t core_start=s->pos;
  state_write_fixed_string(s,engine->state_content_locator,
                           ANYGM_STATE_CONTENT_LOCATOR_SIZE);
  state_write_fixed_string(s,engine->launch_parameters,
                           ANYGM_STATE_LAUNCH_PARAMETERS_SIZE);
  cw_u32(s,engine->width); cw_u32(s,engine->height); cw_u32(s,engine->background); cw_d(s,engine->fps);
  cw_i32(s,engine->follow_player); cw_i32(s,engine->player_object); cw_d(s,engine->audio_accumulator);
  { cw_i64(s,(int64_t)engine->vm.frame); }
  cw_raw(s,engine->pad_current,sizeof(engine->pad_current)); cw_raw(s,engine->pad_previous,sizeof(engine->pad_previous));
  cw_raw(s,engine->key_current,sizeof(engine->key_current)); cw_raw(s,engine->key_previous,sizeof(engine->key_previous));
  state_write_completed_frame(engine,s);
  size_t coren=s->pos-core_start;
  size_t render_start=s->pos;
  int derived_view_surface=(int)gml_global_arr(&engine->vm,"view_surface_id",0);
  if(s->data){
    size_t written=0;
    size_t available=s->pos<=s->cap?s->cap-s->pos:0;
    uint8_t *destination=s->data+(s->pos<=s->cap?s->pos:s->cap);
    if(!gml_render_state_save(&engine->render,derived_view_surface,destination,available,&written))
      s->ok=0;
    s->pos+=written;
  } else {
    size_t measured=gml_render_state_size(&engine->render,derived_view_surface);
    if(!measured) s->ok=0;
    s->pos+=measured;
  }
  size_t rendern=s->pos-render_start;
  size_t vmn=0, audn=0;
  if(s->data){
    size_t wr=0, avail=s->pos<=s->cap? s->cap-s->pos : 0;
    uint8_t *dst=s->data+(s->pos<=s->cap? s->pos : s->cap);
    if(!gml_vm_state_save(&engine->vm,dst,avail,&wr)) s->ok=0;
    vmn=wr;
    s->pos+=wr;
  } else {
    vmn = gml_vm_state_size(&engine->vm);
    s->pos+=vmn;
  }
  if(s->data){
    size_t wr=0, avail=s->pos<=s->cap? s->cap-s->pos : 0;
    uint8_t *dst=s->data+(s->pos<=s->cap? s->pos : s->cap);
    if(!gml_audio_state_save(engine->audio,dst,avail,&wr)) s->ok=0;
    audn=wr;
    s->pos+=wr;
  } else {
    audn = gml_audio_state_size(engine->audio);
    s->pos+=audn;
  }
  if(s->data && s->ok && s->pos<=s->cap){
    AnygmStateHeader header={0};
    header.total_size=s->pos;
    header.content_fingerprint=engine->content_fingerprint;
    header.compatibility_fingerprint=engine->compatibility_fingerprint;
    header.config_fingerprint=state_current_config_fingerprint(engine);
    header.core_size=coren;
    header.render_size=rendern;
    header.vm_size=vmn;
    header.audio_size=audn;
    header.payload_size=s->pos-ANYGM_STATE_HEADER_SIZE;
    header.payload_checksum=state_hash_bytes(s->data+ANYGM_STATE_HEADER_SIZE,
                                             (size_t)header.payload_size);
    state_header_write(s->data,&header);
  }
  state_profile_maybe_log(engine,s->pos, rendern, vmn, audn);
}
size_t engine_state_size(AnygmEngine *engine){
  if(!engine->loaded) return 0;
  CoreW measure={0};
  measure.ok=1;
  state_write(engine,&measure);
  return measure.ok?measure.pos:0;
}
size_t engine_state_resume_size(AnygmEngine *engine){
  if(!engine->loaded) return 0;
  int previous=engine->state_omit_frame;
  engine->state_omit_frame=1;
  CoreW measure={0};
  measure.ok=1;
  state_write(engine,&measure);
  engine->state_omit_frame=previous;
  return measure.ok?measure.pos:0;
}
bool engine_state_save(AnygmEngine *engine,void *d,size_t n,size_t *written){
  if(!engine->loaded || !d) return false;
  CoreW s={(uint8_t*)d,n,0,1}; state_write(engine,&s);
  /* The completed frame is the one section a state can be restored without, and it is the section
   * that grows by megabytes the moment a virtual monitor is selected. A frontend that fixed its
   * rewind buffer from an earlier, smaller answer offers a buffer that can no longer hold one, and
   * refusing the save there costs the whole rewind history rather than one picture: every later
   * snapshot fails, the buffer keeps only the states from before the growth, and rewinding walks
   * back to them. So write the state again without the frame and keep the history. The restored
   * picture is then the one the next frame draws, which is what a state without a frame has always
   * presented. */
  if(s.pos > n && !engine->state_omit_frame){
    engine->state_omit_frame=1;
    CoreW retry={(uint8_t*)d,n,0,1}; state_write(engine,&retry);
    engine->state_omit_frame=0;
    if(retry.ok && retry.pos<=n){
      if(written) *written=retry.pos;
      if(!engine->diagnostics.state_frame_dropped_reported){
        engine->diagnostics.state_frame_dropped_reported=1;
        engine_logf(engine,ANYGM_LOG_WARN,
                    "[anygm] the %llu-byte buffer offered for this state cannot hold its completed "
                    "frame; saving without it so rewind history survives\n",
                    (unsigned long long)n);
      }
      return true;
    }
  }
  if(written) *written=s.pos;
  if(s.pos > n){
    /* Log the first few overflows and then a heartbeat without flooding a host that retries. */
    engine->diagnostics.state_overflow_count++;
    if(engine->diagnostics.state_overflow_count <= 3 || engine->diagnostics.state_overflow_count % 600 == 0)
      engine_logf(engine,ANYGM_LOG_WARN,
                  "[anygm] state needs %llu bytes but the supplied buffer holds %llu (x%ld)\n",
                  (unsigned long long)s.pos,(unsigned long long)n,
                  engine->diagnostics.state_overflow_count);
  }
  return s.ok && s.pos<=n;
}
bool engine_state_save_for_resume(AnygmEngine *engine,void *d,size_t n,size_t *written){
  if(!engine->loaded || !d) return false;
  int previous=engine->state_omit_frame;
  engine->state_omit_frame=1;
  CoreW s={(uint8_t*)d,n,0,1};
  state_write(engine,&s);
  engine->state_omit_frame=previous;
  if(written) *written=s.pos;
  if(s.pos>n){
    engine->diagnostics.state_overflow_count++;
    if(engine->diagnostics.state_overflow_count<=3 ||
       engine->diagnostics.state_overflow_count%600==0)
      engine_logf(engine,ANYGM_LOG_WARN,
                  "[anygm] resumed state needs %llu bytes but the supplied buffer holds %llu "
                  "(x%ld)\n",
                  (unsigned long long)s.pos,(unsigned long long)n,
                  engine->diagnostics.state_overflow_count);
  }
  return s.ok && s.pos<=n;
}
bool state_unserialize_impl(AnygmEngine *engine,const void *d, size_t n, int schedule_reapply){
  if(!engine->loaded || !d) return false;
  AnygmStateHeader header={0};
  AnygmStateLaunch launch={0};
  if(!state_header_read(d,n,&header) || !state_header_matches_engine(engine,&header) ||
     !state_read_launch(d,&header,&launch) || !state_launch_matches_engine(engine,&launch))
    return false;
  uint64_t tp0=0,tp1=0,tp2=0,tp3=0;
  int st_time=anygm_host_development_setting(&engine->host,"GML_DBG_STATE_TIME")!=NULL;
  if(st_time) tp0=anygm_host_monotonic_time_ns(&engine->host);
  const uint8_t *payload=(const uint8_t*)d+ANYGM_STATE_HEADER_SIZE;
  size_t core_size=(size_t)header.core_size;
  size_t render_size=(size_t)header.render_size;
  size_t vm_size=(size_t)header.vm_size;
  size_t audio_size=(size_t)header.audio_size;
  CoreR core={payload,core_size,0,1};
  char content_locator[ANYGM_STATE_CONTENT_LOCATOR_SIZE];
  char launch_parameters[ANYGM_STATE_LAUNCH_PARAMETERS_SIZE];
  if(!state_read_fixed_string(&core,content_locator,sizeof content_locator) ||
     !state_read_fixed_string(&core,launch_parameters,sizeof launch_parameters)) return false;
  engine->width=cr_u32(&core); engine->height=cr_u32(&core); engine->background=cr_u32(&core); engine->fps=cr_d(&core);
  if(engine->width>FB_MAX_W || engine->height>FB_MAX_H || engine->width==0 || engine->height==0 || !isfinite(engine->fps) || engine->fps<=0) return false;
  engine->follow_player=cr_i32(&core); engine->player_object=cr_i32(&core); engine->audio_accumulator=cr_d(&core);
  if(!isfinite(engine->audio_accumulator)) return false;
  int64_t frame=cr_i64(&core);
  if(frame<0) frame=0;
  if(frame>(int64_t)LONG_MAX) frame=(int64_t)LONG_MAX;
  { engine->vm.frame=(long)frame; }
  cr_raw(&core,engine->pad_current,sizeof(engine->pad_current)); cr_raw(&core,engine->pad_previous,sizeof(engine->pad_previous));
  cr_raw(&core,engine->key_current,sizeof(engine->key_current)); cr_raw(&core,engine->key_previous,sizeof(engine->key_previous));
  if(!state_read_completed_frame(engine,&core)) return false;
  if(!core.ok || core.pos!=core.cap) return false;
  /* The serialized current state is the edge-detection baseline for the first advancing frame.
   * The presentation-only load frame clears input temporarily and reapplies this state afterward,
   * so retaining it cannot deliver stale input to Draw events. */
  memset(engine->hardware_key_current,0,sizeof(engine->hardware_key_current));
  memset(engine->hardware_key_previous,0,sizeof(engine->hardware_key_previous));
  memset(engine->event_vk_current,0,sizeof(engine->event_vk_current));
  memset(engine->event_vk_previous,0,sizeof(engine->event_vk_previous));
  memset(engine->event_key_current,0,sizeof(engine->event_key_current));
  memset(engine->event_key_previous,0,sizeof(engine->event_key_previous));
  memset(engine->axis_current,0,sizeof(engine->axis_current));
  memset(engine->axis_previous,0,sizeof(engine->axis_previous));
  memset(engine->mouse_button_current,0,sizeof(engine->mouse_button_current));
  memset(engine->mouse_button_previous,0,sizeof(engine->mouse_button_previous));
  engine->mouse_wheel = 0;
  /* After restoration, seed edge detection from the first host poll rather than the
   * cleared buffers. A key already held across the boundary is not a new press. */
  engine->input_continuity_pending = 1;
  size_t offset=core_size;
  size_t render_used=0;
  if(!gml_render_state_load(&engine->render,payload+offset,render_size,&render_used) ||
     render_used!=render_size){
    if(anygm_host_development_setting(&engine->host,"GML_LOG_STATE"))
      engine_logf(engine,ANYGM_LOG_DEBUG,"[state] render section rejected (used=%" PRIu64 " size=%" PRIu64 " ok=%d)\n",
              (uint64_t)render_used,(uint64_t)render_size,0);
    return false;
  }
  offset+=render_size;
  if(st_time) tp1=anygm_host_monotonic_time_ns(&engine->host);
  size_t used=0;
  if(!gml_vm_state_load(&engine->vm,payload+offset,vm_size,&used) || used!=vm_size){
    if(anygm_host_development_setting(&engine->host,"GML_LOG_STATE"))
      engine_logf(engine,ANYGM_LOG_DEBUG,"[state] VM section rejected (used=%" PRIu64 " size=%" PRIu64 ")\n",(uint64_t)used,(uint64_t)vm_size);
    return false;
  }
  offset+=vm_size;
  if(st_time) tp2=anygm_host_monotonic_time_ns(&engine->host);
  used=0;
  if(!gml_audio_state_load(engine->audio,payload+offset,audio_size,&used) || used!=audio_size){
    if(anygm_host_development_setting(&engine->host,"GML_LOG_STATE"))
      engine_logf(engine,ANYGM_LOG_DEBUG,"[state] audio section rejected (used=%" PRIu64 " size=%" PRIu64 ")\n",(uint64_t)used,(uint64_t)audio_size);
    return false;
  }
  offset+=audio_size;
  if(anygm_host_development_setting(&engine->host,"GML_REENTER_ROOM") &&
     engine->vm.room_index>=0)
    gml_room_enter(&engine->vm,engine->vm.room_index);
  if(st_time){ tp3=anygm_host_monotonic_time_ns(&engine->host);
    #define MS(a,b) ((double)((b)-(a))/1000000.0)
    if(engine->diagnostics.state_time_prints++<8)
      engine_logf(engine,ANYGM_LOG_DEBUG,"[state-time] render=%.2fms vm=%.2fms audio=%.2fms\n",
        MS(tp0,tp1), MS(tp1,tp2), MS(tp2,tp3));
    #undef MS
  }
  engine->vm.render=&engine->render; engine->vm.audio=engine->audio;
  engine->vm.draw_event_hook = aspect_draw_event_hook;
  engine->vm.draw_event_hook_user = engine;
  engine->vm.room_layer_hook = screen_redraw_room_layer_hook;
  engine->vm.room_layer_hook_user = engine;
  engine->vm.present_latch_hook = screen_refresh_present_latch_hook;
  engine->vm.present_latch_hook_user = engine;
  engine_overrides_note_state_load(engine);
  /* The restored VM decides whether this is an ended state. Leave one presentation pass available
   * so its framebuffer can be reconstructed, then the normal Game End handling freezes it. */
  engine->runtime_ended = 0;
  engine->shutdown_sent = 0;
  engine->fps_room = -1;
  sync_room_fps(engine,1);
  classic_transition_reset(engine);
  /* The restored frame replaces whatever the completed frame was, so a deferred presentation that
   * described the previous one has nowhere to go. Derived graphics caches follow it: they are
   * rebuilt from the restored state rather than restored. */
  gml_render_discard_deferred_presentation(&engine->render);
  engine->frame_authority = ENGINE_FRAME_CPU_MATERIALIZED;
  engine->have_presented_frame = 0;
  engine->state_just_loaded = schedule_reapply && engine->state_frame_available ? 1 : 0;
  /* A completed frame made under another presentation policy cannot be handed to the host at its
   * stored extent. Draw once without advancing instead; the existing reapply transaction then puts
   * the canonical post-frame simulation back while leaving the newly composed pixels visible. */
  if(schedule_reapply && engine->state_frame_available &&
     (engine->state_frame_width!=engine->output_width ||
      engine->state_frame_height!=engine->output_height)){
    engine->state_frame_available=0;
    engine->state_just_loaded=1;
  }
  return offset==(size_t)header.payload_size;
}
bool engine_state_load(AnygmEngine *engine,const void *d,size_t n){
  if(!engine->loaded || !d) return false;
  AnygmStateHeader target_header={0};
  AnygmStateLaunch target_launch={0};
  if(!state_header_read(d,n,&target_header) ||
     !state_read_launch(d,&target_header,&target_launch)) return false;
  size_t target_size=(size_t)target_header.total_size;

  if(!state_header_matches_engine(engine,&target_header) ||
     !state_launch_matches_engine(engine,&target_launch)){
    AnygmEngine *staged=NULL;
    if(engine_state_stage_content(engine,target_launch.content_locator,
                                  target_launch.launch_parameters,&staged)!=ANYGM_OK || !staged)
      return false;
    if(!state_header_matches_engine(staged,&target_header) ||
       !state_launch_matches_engine(staged,&target_launch) ||
       !engine_state_load(staged,d,target_size)){
      anygm_destroy(staged);
      return false;
    }
    staged->state_peak_enabled=engine->state_peak_enabled;
    staged->state_peak_hint=engine->state_peak_hint;
    staged->state_peak_persisted=engine->state_peak_persisted;
    staged->state_resume_peak_hint=engine->state_resume_peak_hint;
    staged->state_resume_peak_persisted=engine->state_resume_peak_persisted;
    engine_state_commit_staged_content(engine,staged);
    return true;
  }

  CoreW measure={0}; measure.ok=1;
  state_write(engine,&measure);
  if(!measure.ok || !measure.pos) return false;
  size_t snapshot_size=measure.pos;
  uint8_t *snapshot=malloc(snapshot_size);
  if(!snapshot) return false;
  CoreW snapshot_writer={snapshot,snapshot_size,0,1};
  state_write(engine,&snapshot_writer);
  if(!snapshot_writer.ok || snapshot_writer.pos!=snapshot_size){
    free(snapshot);
    return false;
  }

  CheatSlot prior_cheats[GML_MAX_CHEATS];
  CheatSlot prior_boot_cheats[GML_MAX_CHEATS];
  memcpy(prior_cheats,engine->cheats,sizeof prior_cheats);
  memcpy(prior_boot_cheats,engine->boot_cheats,sizeof prior_boot_cheats);
  int prior_override_room=engine->override_room;
  engine_overrides_prepare_state_load(engine);

  int prior_just_loaded=engine->state_just_loaded;
  size_t prior_reapply_size=engine->state_reapply_size;
  if(!state_unserialize_impl(engine,d,target_size,1)){
    int restored=state_unserialize_impl(engine,snapshot,snapshot_size,0);
    memcpy(engine->cheats,prior_cheats,sizeof prior_cheats);
    memcpy(engine->boot_cheats,prior_boot_cheats,sizeof prior_boot_cheats);
    engine->override_room=prior_override_room;
    engine->state_just_loaded=prior_just_loaded;
    engine->state_reapply_size=prior_reapply_size;
    free(snapshot);
    if(!restored)
      engine_logf(engine,ANYGM_LOG_ERROR,"[anygm] state rollback failed after a rejected load\n");
    return false;
  }
  if(target_size>engine->state_reapply_capacity){
    uint8_t *next=realloc(engine->state_reapply,target_size?target_size:1);
    if(!next){
      int restored=state_unserialize_impl(engine,snapshot,snapshot_size,0);
      memcpy(engine->cheats,prior_cheats,sizeof prior_cheats);
      memcpy(engine->boot_cheats,prior_boot_cheats,sizeof prior_boot_cheats);
      engine->override_room=prior_override_room;
      engine->state_just_loaded=prior_just_loaded;
      engine->state_reapply_size=prior_reapply_size;
      free(snapshot);
      if(!restored)
        engine_logf(engine,ANYGM_LOG_ERROR,"[anygm] state rollback failed after an allocation error\n");
      return false;
    }
    engine->state_reapply=next;
    engine->state_reapply_capacity=target_size;
  }
  memcpy(engine->state_reapply,d,target_size);
  engine->state_reapply_size=target_size;
  free(snapshot);
  return true;
}

/* Peak complete and frame-free sizes, remembered separately across sessions as disposable cache
 * entries in the content's writable namespace. Gameplay allocation routinely dwarfs the boot-time
 * required sections, so the frame-free peak lets a fixed resume ring cover the known run without
 * inheriting the much larger completed-frame ceiling. Each entry is untrusted input: only a short
 * ASCII decimal with an optional trailing newline is believed, and the value is bounded. */
#define ENGINE_STATE_PEAK_FILE ".anygm-state-peak"
#define ENGINE_STATE_RESUME_PEAK_FILE ".anygm-state-resume-peak"
#define ENGINE_STATE_PEAK_LIMIT ((size_t)1u<<30)

static int state_peak_path(const AnygmEngine *engine,const char *name,char *out,size_t out_size){
  if(!engine->state_peak_enabled || !engine->win.save_dir[0]) return 0;
  int n=snprintf(out,out_size,"%s/%s",engine->win.save_dir,name);
  return n>0 && (size_t)n<out_size;
}

static size_t state_peak_read(AnygmEngine *engine,const char *name){
  char path[1088];
  if(!state_peak_path(engine,name,path,sizeof path)) return 0;
  uint8_t *data=NULL;
  size_t size=0;
  if(!anygm_vfs_read_all(&engine->host,path,&data,&size,32)) return 0;
  uint64_t value=0;
  size_t digits=0;
  while(digits<size && digits<12 && data[digits]>='0' && data[digits]<='9'){
    value=value*10u+(uint64_t)(data[digits]-'0');
    digits++;
  }
  int valid=digits>0 && value<=ENGINE_STATE_PEAK_LIMIT &&
            (digits==size || (data[digits]=='\n' && digits+1==size));
  free(data);
  return valid?(size_t)value:0;
}

void engine_state_peak_load(AnygmEngine *engine){
  engine->state_peak_hint=state_peak_read(engine,ENGINE_STATE_PEAK_FILE);
  engine->state_peak_persisted=engine->state_peak_hint;
  engine->state_resume_peak_hint=state_peak_read(engine,ENGINE_STATE_RESUME_PEAK_FILE);
  engine->state_resume_peak_persisted=engine->state_resume_peak_hint;
}

static void state_peak_write(AnygmEngine *engine,const char *name,size_t hint,size_t *persisted){
  if(hint<=*persisted) return;
  char path[1088];
  if(!state_peak_path(engine,name,path,sizeof path)) return;
  char text[32];
  int n=snprintf(text,sizeof text,"%llu\n",(unsigned long long)hint);
  if(n<=0) return;
  if(anygm_vfs_write_all(&engine->host,path,text,(size_t)n))
    *persisted=hint;
}

void engine_state_peak_flush(AnygmEngine *engine){
  state_peak_write(engine,ENGINE_STATE_PEAK_FILE,engine->state_peak_hint,
                   &engine->state_peak_persisted);
  state_peak_write(engine,ENGINE_STATE_RESUME_PEAK_FILE,engine->state_resume_peak_hint,
                   &engine->state_resume_peak_persisted);
}

void engine_state_peak_note(AnygmEngine *engine,size_t written){
  if(written>ENGINE_STATE_PEAK_LIMIT || written<=engine->state_peak_hint) return;
  engine->state_peak_hint=written;
  /* Rewind serializes every frame and a growing state grows by a few bytes at a time, so the
   * cache is rewritten on meaningful growth only; unload flushes the exact final value. */
  size_t persisted=engine->state_peak_persisted;
  if(engine->state_peak_hint>persisted+(persisted>>2) ||
     engine->state_peak_hint-persisted>=((size_t)1u<<20))
    engine_state_peak_flush(engine);
}

void engine_state_resume_peak_note(AnygmEngine *engine,size_t written){
  if(written>ENGINE_STATE_PEAK_LIMIT || written<=engine->state_resume_peak_hint) return;
  engine->state_resume_peak_hint=written;
  size_t persisted=engine->state_resume_peak_persisted;
  if(engine->state_resume_peak_hint>persisted+(persisted>>2) ||
     engine->state_resume_peak_hint-persisted>=((size_t)1u<<20))
    engine_state_peak_flush(engine);
}
