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
static uint64_t cr_u64(CoreR *s){
  uint8_t bytes[8]={0}; cr_raw(s,bytes,sizeof bytes); uint64_t value=0;
  for(unsigned i=0;i<8;i++) value|=(uint64_t)bytes[i]<<(i*8);
  return value;
}
static int64_t cr_i64(CoreR *s){ return (int64_t)cr_u64(s); }
static int cr_i32(CoreR *s){ return (int)(int32_t)cr_u32(s); }
static double cr_d(CoreR *s){ uint64_t bits=cr_u64(s); double value=0; memcpy(&value,&bits,sizeof value); return value; }

static void state_write_completed_frame(AnygmEngine *engine,CoreW *state){
  unsigned width=0,height=0;
  /* The state carries the exact completed frame. A frame that was produced somewhere other than
   * the processor has to be produced here first: serializing the buffer as it stands would write
   * whatever the previous frame left in it. */
  engine_materialize_completed_frame(engine);
  if(engine->have_presented_frame && engine->screen && engine->output_width && engine->output_height){
    width=engine->output_width;
    height=engine->output_height;
  } else if(engine->state_frame_available && engine->screen){
    width=engine->state_frame_width;
    height=engine->state_frame_height;
  }
  if(width>FB_MAX_W || height>FB_MAX_H ||
     (width && (height==0 || (size_t)width>SIZE_MAX/(size_t)height))){
    state->ok=0;
    width=height=0;
  }
  cw_u32(state,width);
  cw_u32(state,height);
  size_t pixels=(size_t)width*height;
  uint32_t runs=0;
  for(size_t at=0;at<pixels;){
    uint32_t value=engine->screen[at];
    size_t end=at+1;
    while(end<pixels && engine->screen[end]==value && end-at<UINT32_MAX) end++;
    if(runs==UINT32_MAX){ state->ok=0; break; }
    runs++;
    at=end;
  }
  cw_u32(state,runs);
  for(size_t at=0;at<pixels;){
    uint32_t value=engine->screen[at];
    size_t end=at+1;
    while(end<pixels && engine->screen[end]==value && end-at<UINT32_MAX) end++;
    cw_u32(state,(uint32_t)(end-at));
    cw_u32(state,value);
    at=end;
  }
}

size_t engine_state_frame_capacity(const AnygmEngine *engine){
  /* Ceiling of state_write_completed_frame under the geometry this session is already known to
   * reach: width, height and run count cost 12 bytes, every run costs 8, and a run can cover a
   * single pixel, so the frame slot can cost up to 8 bytes per output pixel. The configured
   * virtual monitor counts even before the first frame presents, because a frontend that sizes
   * a rewind ring does it once, at load, when none of the presentation has happened yet. */
  size_t w=engine->output_width,h=engine->output_height;
  if(engine->config.monitor_width>w) w=engine->config.monitor_width;
  if(engine->config.monitor_height>h) h=engine->config.monitor_height;
  if((size_t)engine->state_frame_width>w) w=engine->state_frame_width;
  if((size_t)engine->state_frame_height>h) h=engine->state_frame_height;
  if(!w || !h){ w=engine->width; h=engine->height; }
  if(w>FB_MAX_W) w=FB_MAX_W;
  if(h>FB_MAX_H) h=FB_MAX_H;
  return 12u+8u*w*h;
}

static int state_read_completed_frame(AnygmEngine *engine,CoreR *state){
  uint32_t width=cr_u32(state),height=cr_u32(state),runs=cr_u32(state);
  if(width>FB_MAX_W || height>FB_MAX_H || (!!width != !!height)) return 0;
  size_t pixels=(size_t)width*height;
  if((!pixels && runs) || (pixels && (!runs || runs>pixels))) return 0;
  size_t at=0;
  for(uint32_t index=0;index<runs;index++){
    uint32_t count=cr_u32(state),value=cr_u32(state);
    if(!count || count>pixels-at) return 0;
    for(uint32_t pixel=0;pixel<count;pixel++) engine->screen[at++]=value;
  }
  if(!state->ok || at!=pixels) return 0;
  engine->state_frame_available=pixels?1:0;
  engine->state_frame_width=width;
  engine->state_frame_height=height;
  return 1;
}

enum {
  ANYGM_STATE_HEADER_SIZE=112,
  ANYGM_STATE_ENCODING_LITTLE_ENDIAN_IEEE754=1
};
#define ANYGM_STATE_MAGIC UINT32_C(0x54534741)

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

static int state_header_read(AnygmEngine *engine,const void *data,size_t size,
                             AnygmStateHeader *header){
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
  if(header->content_fingerprint!=engine->content_fingerprint ||
     header->compatibility_fingerprint!=engine->compatibility_fingerprint ||
     header->config_fingerprint!=state_current_config_fingerprint(engine)) return 0;
  const uint8_t *payload=(const uint8_t*)data+ANYGM_STATE_HEADER_SIZE;
  return state_hash_bytes(payload,(size_t)header->payload_size)==header->payload_checksum;
}

static void state_write(AnygmEngine *engine,CoreW *s){
  uint8_t empty_header[ANYGM_STATE_HEADER_SIZE]={0};
  cw_raw(s,empty_header,sizeof empty_header);
  size_t core_start=s->pos;
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
bool engine_state_save(AnygmEngine *engine,void *d,size_t n,size_t *written){
  if(!engine->loaded || !d) return false;
  CoreW s={(uint8_t*)d,n,0,1}; state_write(engine,&s);
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
bool state_unserialize_impl(AnygmEngine *engine,const void *d, size_t n, int schedule_reapply){
  if(!engine->loaded || !d) return false;
  AnygmStateHeader header={0};
  if(!state_header_read(engine,d,n,&header)) return false;
  uint64_t tp0=0,tp1=0,tp2=0,tp3=0;
  int st_time=anygm_host_development_setting(&engine->host,"GML_DBG_STATE_TIME")!=NULL;
  if(st_time) tp0=anygm_host_monotonic_time_ns(&engine->host);
  const uint8_t *payload=(const uint8_t*)d+ANYGM_STATE_HEADER_SIZE;
  size_t core_size=(size_t)header.core_size;
  size_t render_size=(size_t)header.render_size;
  size_t vm_size=(size_t)header.vm_size;
  size_t audio_size=(size_t)header.audio_size;
  CoreR core={payload,core_size,0,1};
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
  return offset==(size_t)header.payload_size;
}
bool engine_state_load(AnygmEngine *engine,const void *d,size_t n){
  if(!engine->loaded || !d) return false;
  AnygmStateHeader target_header={0};
  if(!state_header_read(engine,d,n,&target_header)) return false;
  size_t target_size=(size_t)target_header.total_size;

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

  int prior_just_loaded=engine->state_just_loaded;
  size_t prior_reapply_size=engine->state_reapply_size;
  if(!state_unserialize_impl(engine,d,target_size,1)){
    int restored=state_unserialize_impl(engine,snapshot,snapshot_size,0);
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

/* Peak serialized size, remembered across sessions as a disposable cache entry in the content's
 * writable namespace. A frontend that sizes a rewind ring once, at load, otherwise sizes it from
 * the boot-time state, which gameplay allocation routinely dwarfs; the remembered peak lets the
 * next session announce a capacity the whole run fits in. The entry is untrusted input: only a
 * short ASCII decimal with an optional trailing newline is believed, and the value is bounded. */
#define ENGINE_STATE_PEAK_FILE ".anygm-state-peak"
#define ENGINE_STATE_PEAK_LIMIT ((size_t)1u<<30)

static int state_peak_path(const AnygmEngine *engine,char *out,size_t out_size){
  if(!engine->state_peak_enabled || !engine->win.save_dir[0]) return 0;
  int n=snprintf(out,out_size,"%s/%s",engine->win.save_dir,ENGINE_STATE_PEAK_FILE);
  return n>0 && (size_t)n<out_size;
}

void engine_state_peak_load(AnygmEngine *engine){
  engine->state_peak_hint=0;
  engine->state_peak_persisted=0;
  char path[1088];
  if(!state_peak_path(engine,path,sizeof path)) return;
  uint8_t *data=NULL;
  size_t size=0;
  if(!anygm_vfs_read_all(&engine->host,path,&data,&size,32)) return;
  uint64_t value=0;
  size_t digits=0;
  while(digits<size && digits<12 && data[digits]>='0' && data[digits]<='9'){
    value=value*10u+(uint64_t)(data[digits]-'0');
    digits++;
  }
  int valid=digits>0 && value<=ENGINE_STATE_PEAK_LIMIT &&
            (digits==size || (data[digits]=='\n' && digits+1==size));
  free(data);
  if(!valid) return;
  engine->state_peak_hint=(size_t)value;
  engine->state_peak_persisted=(size_t)value;
}

void engine_state_peak_flush(AnygmEngine *engine){
  if(engine->state_peak_hint<=engine->state_peak_persisted) return;
  char path[1088];
  if(!state_peak_path(engine,path,sizeof path)) return;
  char text[32];
  int n=snprintf(text,sizeof text,"%llu\n",(unsigned long long)engine->state_peak_hint);
  if(n<=0) return;
  if(anygm_vfs_write_all(&engine->host,path,text,(size_t)n))
    engine->state_peak_persisted=engine->state_peak_hint;
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
