/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* What a host graphics target may and may not do to serialized state.
 *
 * The whole risk of presenting a frame somewhere other than the processor is that the processor's
 * copy of it stops being the truth while something still reads it. Save states read it, rewind
 * reads it every frame, and a loaded state presents it once before simulation resumes. So these
 * cases drive a real engine through a real load, a real graphics context and real serialization,
 * and ask whether the same logical
 * state produces the same bytes with and without a target, whether a state crosses between them,
 * whether a context arriving, disappearing or being taken away changes any of it, and whether a
 * rejected state leaves the engine exactly as it was.
 *
 * The graphics driver is the shared fake. Nothing here measures real-device pixels;
 * that question needs a separate framebuffer comparison against the software renderer. */
#include "anygm.h"
#include "engine_internal.h"
#include "gml_render_resources.h"
#include "gml_render_surfaces_internal.h"
#include "graphics_driver_fixture.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"
#include "../unit/media/font_test_fixture.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition,label) do{ \
  if(!(condition)){ \
    fprintf(stderr,"graphics state failed: %s\n",label); \
    return 0; \
  } \
}while(0)

typedef struct Session {
  AnygmSyntheticContent fixture;
  AnygmHostServices services;
  AnygmEngine *engine;
} Session;

static AnygmGraphicsContext graphics_context(uint32_t api){
  AnygmGraphicsContext context;
  memset(&context,0,sizeof context);
  context.struct_size=sizeof context;
  context.api=api;
  context.version_major=3;
  context.version_minor=api==ANYGM_GRAPHICS_OPENGLES3?0u:3u;
  context.get_proc_address=anygm_test_graphics_proc;
  context.get_current_framebuffer=anygm_test_graphics_framebuffer;
  return context;
}

/* A context is a value the caller owns; binding it before handing over the address is what the
 * public boundary asks for. */
static AnygmResult graphics_adopt(AnygmEngine *engine,uint32_t api){
  AnygmGraphicsContext context=graphics_context(api);
  return anygm_graphics_context_reset(engine,&context);
}

typedef struct GraphicsWitness {
  unsigned slot;
  unsigned proc_queries;
  unsigned framebuffer_queries;
} GraphicsWitness;

static unsigned graphics_witness_error_calls[3];
static unsigned graphics_witness_error(unsigned slot){
  void (*generic)(void)=anygm_test_graphics_proc(NULL,"glGetError");
  unsigned (*typed)(void)=NULL;
  graphics_witness_error_calls[slot]++;
  if(!generic) return 0u;
  memcpy(&typed,&generic,sizeof typed);
  return typed(); /* Preserve the fake driver's forbidden-call observation. */
}
static unsigned graphics_witness_error_0(void){ return graphics_witness_error(0u); }
static unsigned graphics_witness_error_1(void){ return graphics_witness_error(1u); }
static unsigned graphics_witness_error_2(void){ return graphics_witness_error(2u); }

/* The driver remains the existing fake; only glGetError has a distinct function pointer per
 * context. That makes a stale or shared engine dispatch table observable after one engine resets. */
static void (*graphics_witness_proc(void *userdata,const char *name))(void){
  GraphicsWitness *witness=(GraphicsWitness*)userdata;
  if(!witness || witness->slot>=3u) return NULL;
  witness->proc_queries++;
  if(!strcmp(name,"glGetError")){
    unsigned (*typed)(void)=witness->slot==0u?graphics_witness_error_0:
                           witness->slot==1u?graphics_witness_error_1:
                                             graphics_witness_error_2;
    void (*generic)(void)=NULL;
    memcpy(&generic,&typed,sizeof generic);
    return generic;
  }
  return anygm_test_graphics_proc(NULL,name);
}

static uintptr_t graphics_witness_framebuffer(void *userdata){
  GraphicsWitness *witness=(GraphicsWitness*)userdata;
  if(!witness || witness->slot>=3u) return 0u;
  witness->framebuffer_queries++;
  return 7u+10u*witness->slot;
}

static AnygmResult graphics_adopt_witness(AnygmEngine *engine,uint32_t api,
                                          GraphicsWitness *witness){
  AnygmGraphicsContext context=graphics_context(api);
  context.userdata=witness;
  context.get_proc_address=graphics_witness_proc;
  context.get_current_framebuffer=graphics_witness_framebuffer;
  return anygm_graphics_context_reset(engine,&context);
}

static int session_open_fixture_with_scene(Session *session,uint32_t scene_renderer,
                                           int (*create_fixture)(AnygmSyntheticContent *)){
  AnygmContentSource source={0};
  memset(session,0,sizeof *session);
  if(!create_fixture(&session->fixture)) return 0;
  session->services.struct_size=sizeof session->services;
  session->services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&session->services);
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=session->fixture.path;
  source.cache_directory=session->fixture.directory;
  source.save_directory=session->fixture.directory;
  if(anygm_create(&session->services,&session->engine)!=ANYGM_OK) return 0;
  /* Adopting a context is transport; final-pass acceleration is a separate explicit policy. */
  AnygmConfigDelta graphics={0};
  graphics.struct_size=sizeof graphics;
  graphics.values.struct_size=sizeof graphics.values;
  graphics.fields=ANYGM_CONFIG_HYBRID_GPU_PRESENTATION|ANYGM_CONFIG_SCENE_RENDERER;
  graphics.values.hybrid_gpu_presentation=1u;
  graphics.values.scene_renderer=scene_renderer;
  if(anygm_set_config(session->engine,&graphics)!=ANYGM_OK) return 0;
  return anygm_load(session->engine,&source,NULL)==ANYGM_OK;
}

static int session_open_with_scene(Session *session,uint32_t scene_renderer){
  return session_open_fixture_with_scene(
    session,scene_renderer,anygm_synthetic_framebuffer_content_create);
}

static int session_open(Session *session){
  return session_open_with_scene(session,ANYGM_SCENE_RENDERER_CPU);
}

static void session_close(Session *session){
  if(session->engine) anygm_destroy(session->engine);
  session->engine=NULL;
  anygm_synthetic_content_destroy(&session->fixture);
}

static int advance(AnygmEngine *engine,int frames,uint32_t *last_flags){
  AnygmInputFrame input={0};
  AnygmFrameOutput output={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  for(int frame=0;frame<frames;frame++){
    memset(&output,0,sizeof output);
    output.struct_size=sizeof output;
    if(anygm_run_frame(engine,&input,&output)!=ANYGM_OK) return 0;
    if(last_flags) *last_flags=output.flags;
  }
  return 1;
}

static uint8_t *serialize(AnygmEngine *engine,size_t *size);

typedef struct CurrentContextWitness {
  uint32_t current,queries;
} CurrentContextWitness;

static uint32_t graphics_context_is_current(void *userdata){
  CurrentContextWitness *witness=(CurrentContextWitness*)userdata;
  witness->queries++;
  return witness->current;
}

static int scene_frame_end_defers_observation_case(void){
  for(unsigned witnessed=0;witnessed<2u;witnessed++){
    Session selected={0},restored={0};
    CurrentContextWitness witness={1u,0u};
    AnygmGraphicsContext context=graphics_context(ANYGM_GRAPHICS_OPENGLES3);
    GmlRenderDrawOp operation={0};
    GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
    GmlRenderJournalMetrics metrics={0};
    GmlRenderResource *resource;
    uint8_t *saved=NULL;
    size_t capacity=0,written=0;
    int id;
    anygm_test_graphics_reset();
    REQUIRE(session_open_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL) &&
            session_open(&restored),"on-demand observation opens fresh scene and restore sessions");
    context.userdata=&witness;
    context.is_current=witnessed?graphics_context_is_current:NULL;
    REQUIRE(anygm_graphics_context_reset(selected.engine,&context)==ANYGM_OK &&
            advance(selected.engine,1,NULL),"on-demand observation begins with a completed frame");
    id=gml_surface_create(&selected.engine->render,4,4);
    resource=gml_render_surface_resource_ensure(&selected.engine->render,id);
    REQUIRE(id>0 && resource,"on-demand observation creates a persistent device target");
    operation.opcode=GML_RENDER_OP_CLEAR;
    operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    operation.target.storage_version=1u;
    operation.target.width=operation.target.height=4u;
    operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    operation.target.region=(GmlRenderRect){0,0,4u,4u};
    operation.payload.clear_colour=0x40556677u;
    for(unsigned frame=0;frame<3u;frame++){
      operation.target.image=gml_render_resource_image_ref(resource);
      operation.target.image.content_version++;
      REQUIRE(gml_render_resource_submit_deferred(resource,
                gml_render_device(&selected.engine->render),&batch,NULL,0u)==
                GML_RENDER_RESOURCE_EXEC_DEVICE && advance(selected.engine,1,NULL),
              "a persistent pending device image survives the next ordinary frame");
      gml_render_resource_journal_metrics(resource,&metrics);
      if(metrics.authority!=(witnessed?GML_RENDER_RESOURCE_GPU_RESIDENT:
                                       GML_RENDER_RESOURCE_BOTH_CURRENT) ||
         metrics.device_checkpoints!=(witnessed?0u:frame+1u))
        fprintf(stderr,"frame observation: witnessed=%u frame=%u authority=%u checkpoints=%llu\n",
                witnessed,frame,metrics.authority,(unsigned long long)metrics.device_checkpoints);
      REQUIRE(metrics.authority==(witnessed?GML_RENDER_RESOURCE_GPU_RESIDENT:
                                            GML_RENDER_RESOURCE_BOTH_CURRENT) &&
              metrics.device_checkpoints==(witnessed?0u:frame+1u) && !metrics.cpu_replays &&
              !metrics.journal_operations,
              "witnessed frames leave pixels resident; unwitnessed frames retain their seal");
    }
    witness.queries=0u;
    anygm_test_graphics_forbid_calls();
    capacity=anygm_state_size(selected.engine);
    REQUIRE(capacity && !witness.queries && !anygm_test_graphics_calls_after_forget(),
            "pending persistent pixels do not change metadata-only state sizing");
    anygm_test_graphics_allow_calls();
    saved=(uint8_t*)malloc(capacity);
    REQUIRE(saved,"on-demand observation allocates advertised state capacity");
    anygm_test_graphics_readback_alpha(0x4Du);
    REQUIRE(anygm_state_save(selected.engine,saved,capacity,&written)==ANYGM_OK && written &&
            written<=capacity && anygm_state_load(restored.engine,saved,written)==ANYGM_OK,
            "explicit state observation preserves a pending image in the same execution");
    const uint32_t *pixels=gml_surface_pixels_read(&restored.engine->render,id,NULL,NULL);
    gml_render_resource_journal_metrics(resource,&metrics);
    REQUIRE(pixels && metrics.device_checkpoints==(witnessed?1u:3u) && !metrics.cpu_replays &&
            (witnessed?(pixels[0]==0x4D030033u && pixels[15]==0x4D000333u):
                         (pixels[0]==0xFF030033u && pixels[15]==0xFF000333u)),
            "saving reads actual persistent RGBA only when its device version is unobserved");
    free(saved);
    session_close(&selected);session_close(&restored);
  }
  return 1;
}

static int scene_state_unbound_observation_is_retryable_case(void){
  for(unsigned completed=0;completed<2u;completed++){
    Session selected={0},restored={0};
    CurrentContextWitness witness={1u,0u};
    AnygmGraphicsContext context=graphics_context(ANYGM_GRAPHICS_OPENGLES3);
    GmlRenderDrawOp operation={0};
    GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
    GmlRenderJournalMetrics before={0},after={0};
    GmlRenderResource *resource;
    uint8_t *saved=NULL,*prior=NULL;
    uint32_t shadow[16],frame_authority=0;
    size_t capacity=0,written=0,prior_size=0;
    unsigned reads=0;
    int id;
    anygm_test_graphics_reset();
    REQUIRE(session_open_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL) &&
            session_open(&restored),"unbound observation starts from fresh content");
    prior=serialize(restored.engine,&prior_size);
    REQUIRE(prior && prior_size,"unbound restore input is created in this execution");
    context.userdata=&witness;
    context.is_current=graphics_context_is_current;
    REQUIRE(anygm_graphics_context_reset(selected.engine,&context)==ANYGM_OK &&
            (!completed || advance(selected.engine,1,NULL)),
            "unbound observation starts with an accessible owning context");
    id=gml_surface_create(&selected.engine->render,4,4);
    resource=gml_render_surface_resource_ensure(&selected.engine->render,id);
    REQUIRE(id>0 && resource,"unbound observation creates a persistent target");
    operation.opcode=GML_RENDER_OP_CLEAR;
    operation.reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    operation.target.image=gml_render_resource_image_ref(resource);
    operation.target.image.content_version++;
    operation.target.storage_version=1u;
    operation.target.width=operation.target.height=4u;
    operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    operation.target.region=(GmlRenderRect){0,0,4u,4u};
    operation.payload.clear_colour=0x40556677u;
    REQUIRE(gml_render_surface_scene_submit(&selected.engine->render,id,&batch,NULL,0u)==
            GML_RENDER_RESOURCE_EXEC_DEVICE,"unbound observation actually dirties device pixels");
    memcpy(shadow,selected.engine->render.surface[id-1].px,sizeof shadow);
    gml_render_resource_journal_metrics(resource,&before);
    REQUIRE(before.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            before.cpu_version!=before.logical_version && !before.journal_operations &&
            !before.retained_versions && !before.journal_bytes,
            "unbound observation retains actual device authority without a CPU recipe");
    frame_authority=selected.engine->frame_authority;
    reads=anygm_test_graphics_read_pixels();
    witness.current=0u;
    witness.queries=0u;
    anygm_test_graphics_forbid_calls();
    capacity=anygm_state_size(selected.engine);
    REQUIRE(capacity>0u && anygm_state_resume_size(selected.engine)>0u && !witness.queries,
            "unbound metadata queries need neither access proof nor pixels");
    saved=(uint8_t*)malloc(capacity);
    REQUIRE(saved,"unbound observation allocates advertised capacity");
    memset(saved,0xA5,capacity);
    REQUIRE(anygm_state_save(selected.engine,saved,capacity,&written)!=ANYGM_OK && !written,
            "unbound save refuses before substituting CPU surface recipes");
    REQUIRE(anygm_state_save_for_resume(selected.engine,saved,capacity,&written)!=ANYGM_OK &&
            !written && anygm_state_load(selected.engine,prior,prior_size)!=ANYGM_OK,
            "resume and rollback also refuse an unavailable actual device image");
    gml_render_resource_journal_metrics(resource,&after);
    REQUIRE(after.authority==before.authority && after.cpu_version==before.cpu_version &&
            after.gpu_version==before.gpu_version && after.logical_version==before.logical_version &&
            after.journal_operations==before.journal_operations &&
            after.cpu_replays==before.cpu_replays &&
            !memcmp(shadow,selected.engine->render.surface[id-1].px,sizeof shadow) &&
            selected.engine->frame_authority==frame_authority && saved[0]==0xA5 &&
            anygm_test_graphics_read_pixels()==reads &&
            !anygm_test_graphics_calls_after_forget(),
            "unavailable observation preserves authority, CPU planes, frame and caller bytes");
    anygm_test_graphics_allow_calls();
    witness.current=1u;
    anygm_test_graphics_readback_alpha(0x49u);
    REQUIRE(anygm_state_save(selected.engine,saved,capacity,&written)==ANYGM_OK &&
            written>0u && written<=capacity &&
            anygm_state_load(restored.engine,saved,written)==ANYGM_OK,
            "rebinding permits retry and context-free restoration of actual device pixels");
    const uint32_t *pixels=gml_surface_pixels_read(&restored.engine->render,id,NULL,NULL);
    REQUIRE(pixels && pixels[0]==0x49030033u && pixels[15]==0x49000333u,
            "retry observes the actual RGBA instead of the retained clear recipe");
    free(saved); free(prior);
    session_close(&selected); session_close(&restored);
  }
  return 1;
}

static int scene_destroy_observation_fault_case(void){
  for(unsigned fault=0;fault<3u;fault++){
    Session selected={0};
    CurrentContextWitness witness={1u,0u};
    AnygmGraphicsContext context=graphics_context(ANYGM_GRAPHICS_OPENGLES3);
    GmlRenderDrawOp operation={0};
    GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
    GmlRenderResource *resource;
    unsigned reads=0,deletes=0;
    int id,backed;
    anygm_test_graphics_reset();
    REQUIRE(session_open_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL),
            "destruction fault starts from fresh content");
    context.userdata=&witness;
    context.is_current=graphics_context_is_current;
    REQUIRE(anygm_graphics_context_reset(selected.engine,&context)==ANYGM_OK &&
            advance(selected.engine,1,NULL),"destruction fault completes a device frame");
    id=gml_surface_create(&selected.engine->render,4,4);
    backed=gml_surface_create(&selected.engine->render,2,2);
    resource=gml_render_surface_resource_ensure(&selected.engine->render,id);
    REQUIRE(id>0 && backed>0 && resource,"destruction fault owns device and CPU surfaces");
    selected.engine->render.surface[backed-1].px[0]=0x72554433u;
    operation.opcode=GML_RENDER_OP_CLEAR;
    operation.reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    operation.target.image=gml_render_resource_image_ref(resource);
    operation.target.image.content_version++;
    operation.target.storage_version=1u;
    operation.target.width=operation.target.height=4u;
    operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    operation.target.region=(GmlRenderRect){0,0,4u,4u};
    operation.payload.clear_colour=0x40556677u;
    REQUIRE(gml_render_surface_scene_submit(&selected.engine->render,id,&batch,NULL,0u)==
            GML_RENDER_RESOURCE_EXEC_DEVICE,"destruction fault leaves unobserved device pixels");
    reads=anygm_test_graphics_read_pixels();
    deletes=(unsigned)anygm_test_graphics_deletes();
    anygm_test_graphics_readback_alpha(0x4Bu);
    if(fault==0u){
      witness.current=0u;
      anygm_test_graphics_forbid_calls();
    } else if(fault==1u) anygm_test_graphics_fail_readback_at(reads+1u);
    else anygm_test_graphics_fail_all_readbacks(1);
    /* A notification is not stronger evidence than the exact-context witness. */
    anygm_graphics_context_destroy(selected.engine,1u);
    REQUIRE(!engine_graphics_active(selected.engine) &&
            gml_surface_exists(&selected.engine->render,backed) &&
            selected.engine->render.surface[backed-1].px[0]==0x72554433u,
            "destruction always preserves already-current CPU surfaces");
    if(fault==1u){
      const uint32_t *pixels=gml_surface_pixels_read(&selected.engine->render,id,NULL,NULL);
      REQUIRE(pixels && pixels[0]==0x4B030033u && pixels[15]==0x4B000333u &&
              selected.engine->frame_authority==ENGINE_FRAME_CPU_MATERIALIZED &&
              anygm_test_graphics_read_pixels()>reads+1u,
              "one transient destruction read failure retries and preserves actual RGBA");
    } else {
      REQUIRE(!gml_surface_exists(&selected.engine->render,id) &&
              selected.engine->frame_authority==ENGINE_FRAME_NONE &&
              !selected.engine->have_presented_frame,
              "unrecoverable destruction cannot publish CPU recipes as actual device pixels");
      if(fault==0u)
        REQUIRE(anygm_test_graphics_read_pixels()==reads &&
                (unsigned)anygm_test_graphics_deletes()==deletes &&
                !anygm_test_graphics_calls_after_forget(),
                "negative exact-context proof forbids all graphics calls despite destroy notice");
      else REQUIRE(anygm_test_graphics_read_pixels()==reads+2u,
                   "persistent failure uses exactly two bounded observation attempts");
    }
    anygm_test_graphics_allow_calls();
    anygm_test_graphics_fail_all_readbacks(0);
    session_close(&selected);
  }
  return 1;
}

static int scene_state_observes_lent_device_case(void){
  for(unsigned lent=0;lent<2u;lent++){
    Session selected={0},restored={0};
    CurrentContextWitness witness={lent,0u};
    AnygmGraphicsContext context=graphics_context(ANYGM_GRAPHICS_OPENGLES3);
    GmlRenderJournalMetrics before={0},after={0};
    GmlRenderDrawOp operation={0};
    GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
    GmlRenderResource *resource;
    uint8_t *saved=NULL,*again=NULL;
    size_t capacity=0,written=0,again_written=0;
    int id;
    anygm_test_graphics_reset();
    REQUIRE(session_open_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL) &&
            session_open(&restored),"observed state sessions load fresh content");
    context.userdata=&witness;
    context.is_current=lent?graphics_context_is_current:NULL;
    REQUIRE(anygm_graphics_context_reset(selected.engine,&context)==ANYGM_OK,
            "observed state lends an explicitly witnessed context");
    id=gml_surface_create(&selected.engine->render,4,4);
    resource=gml_render_surface_resource_ensure(&selected.engine->render,id);
    REQUIRE(id>0 && resource,"observed state owns a persistent numbered surface");
    operation.opcode=GML_RENDER_OP_CLEAR;
    operation.reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    operation.target.image=gml_render_resource_image_ref(resource);
    operation.target.image.content_version++;
    operation.target.storage_version=1u;
    operation.target.width=operation.target.height=4u;
    operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    operation.target.region=(GmlRenderRect){0,0,4u,4u};
    operation.payload.clear_colour=0x40556677u;
    REQUIRE(gml_render_surface_scene_submit(&selected.engine->render,id,&batch,NULL,0u)==
            GML_RENDER_RESOURCE_EXEC_DEVICE,"observed state actually executes device work");
    gml_render_resource_journal_metrics(resource,&before);
    REQUIRE(before.authority==(lent?GML_RENDER_RESOURCE_GPU_RESIDENT:
                                   GML_RENDER_RESOURCE_GPU_REPLAYABLE) &&
            before.cpu_version!=before.logical_version &&
            !anygm_test_graphics_read_pixels(),"observed state starts with stale CPU pixels");
    witness.queries=0u;
    anygm_test_graphics_forbid_calls();
    capacity=anygm_state_size(selected.engine);
    REQUIRE(capacity>0u && anygm_state_resume_size(selected.engine)>0u &&
            !witness.queries && !anygm_test_graphics_calls_after_forget(),
            "state sizing never queries context access or observes pixels");
    anygm_test_graphics_allow_calls();
    saved=(uint8_t*)malloc(capacity);
    again=(uint8_t*)malloc(capacity);
    REQUIRE(saved && again,"observed state allocates its advertised capacity");
    /* The fake's patterned response is intentionally not its clear recipe. */
    anygm_test_graphics_readback_alpha(0x41u);
    if(lent){
      anygm_test_graphics_fail_readback_at(1u);
      REQUIRE(anygm_state_save(selected.engine,saved,capacity,&written)!=ANYGM_OK &&
              selected.engine->render.surface[id-1].px[0]==0u &&
              selected.engine->render.surface[id-1].px[15]==0u,
              "failed device state observation does not publish a partial surface");
      gml_render_resource_journal_metrics(resource,&after);
      REQUIRE(after.authority==before.authority &&
              after.cpu_version==before.cpu_version &&
              after.logical_version==before.logical_version &&
              after.journal_operations==before.journal_operations,
              "failed state observation retains the resource and its recovery recipe");
      anygm_test_graphics_fail_readback_at(0u);
    } else anygm_test_graphics_forbid_calls();
    REQUIRE(anygm_state_save(selected.engine,saved,capacity,&written)==ANYGM_OK &&
            written>0u && written<=capacity && (lent?witness.queries>0u:!witness.queries),
            "state writes through the witnessed observation policy");
    anygm_test_graphics_allow_calls();
    REQUIRE(anygm_state_load(restored.engine,saved,written)==ANYGM_OK,
            "observed state loads without a graphics context");
    const uint32_t *pixels=gml_surface_pixels_read(&restored.engine->render,id,NULL,NULL);
    gml_render_resource_journal_metrics(resource,&after);
    REQUIRE(pixels,"observed persistent surface is restored");
    if(lent){
      REQUIRE((pixels[0]&0xFF0000FFu)==0x41000033u &&
              (pixels[15]&0xFF0000FFu)==0x41000033u &&
              after.cpu_replays==0u && after.device_checkpoints==1u &&
              anygm_test_graphics_read_pixels()==2u,
              "state preserves actual device RGBA, not CPU replay RGBA");
    } else {
      REQUIRE(pixels[0]==operation.payload.clear_colour &&
              pixels[15]==operation.payload.clear_colour &&
              after.cpu_replays==1u && !after.device_checkpoints &&
              !anygm_test_graphics_read_pixels() &&
              !anygm_test_graphics_calls_after_forget(),
              "host without an access witness retains the existing GL-free state path");
    }
    witness.current=0u;
    anygm_test_graphics_forbid_calls();
    REQUIRE(anygm_state_save(selected.engine,again,capacity,&again_written)==ANYGM_OK &&
            again_written==written && !memcmp(saved,again,written) &&
            !anygm_test_graphics_calls_after_forget(),
            "observed state bytes remain exact after the host unbinds its context");
    anygm_test_graphics_allow_calls();
    free(saved); free(again);
    session_close(&selected); session_close(&restored);
  }
  return 1;
}

static struct {
  unsigned render_reports,release_reports,reads_at_render,reads_at_release;
  unsigned render_materializations;
  char release[2048];
} release_report;

static const char *release_report_setting(void *userdata,const char *name){
  (void)userdata;
  return !strcmp(name,"GML_GRAPHICS_DEVICE_STATS")?"1":NULL;
}

static void release_report_log(void *userdata,AnygmLogLevel level,const char *message){
  (void)userdata;(void)level;
  if(strstr(message,"[graphics-device] ")){
    const char *field=strstr(message," materializations=");
    release_report.render_reports++;
    release_report.reads_at_render=anygm_test_graphics_read_pixels();
    if(field) sscanf(field," materializations=%u",&release_report.render_materializations);
  }
  if(strstr(message,"[graphics-release-observation] ")){
    release_report.release_reports++;
    release_report.reads_at_release=anygm_test_graphics_read_pixels();
    snprintf(release_report.release,sizeof release_report.release,"%s",message);
  }
}

static int scene_state_preserves_completed_device_case(void){
  Session selected={0},restored={0};
  CurrentContextWitness witness={1u,0u};
  AnygmGraphicsContext context=graphics_context(ANYGM_GRAPHICS_OPENGLES3);
  uint8_t *saved=NULL,*again=NULL;
  uint32_t *shadow=NULL,flags=0u;
  size_t capacity=0,written=0,again_written=0,pixels=0;
  unsigned width=0,height=0,reads=0;
  anygm_test_graphics_reset();
  REQUIRE(session_open_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL) &&
          session_open(&restored),"completed-device state starts from fresh content");
  context.userdata=&witness;
  context.is_current=graphics_context_is_current;
  REQUIRE(anygm_graphics_context_reset(selected.engine,&context)==ANYGM_OK &&
          advance(selected.engine,1,&flags) && (flags&ANYGM_FRAME_HARDWARE_TARGET),
          "completed-device state presents an actual hardware frame");
  width=selected.engine->output_width;
  height=selected.engine->output_height;
  pixels=(size_t)width*height;
  shadow=(uint32_t*)malloc(pixels*sizeof *shadow);
  REQUIRE(shadow && width>1u && height>1u,"completed-device shadow exists");
  memcpy(shadow,selected.engine->screen,pixels*sizeof *shadow);
  reads=anygm_test_graphics_read_pixels();
  witness.queries=0u;
  anygm_test_graphics_forbid_calls();
  capacity=anygm_state_size(selected.engine);
  REQUIRE(capacity>0u && !witness.queries &&
          !anygm_test_graphics_calls_after_forget(),
          "completed-device size does not observe the saved framebuffer");
  anygm_test_graphics_allow_calls();
  anygm_test_graphics_readback_alpha(0x43u);
  anygm_test_graphics_fail_readback_at(reads+1u);
  REQUIRE(!engine_materialize_completed_frame(selected.engine) &&
          !memcmp(shadow,selected.engine->screen,pixels*sizeof *shadow),
          "failed completed-device read publishes no partial CPU image");
  anygm_test_graphics_fail_readback_at(0u);
  saved=(uint8_t*)malloc(capacity);
  again=(uint8_t*)malloc(capacity);
  REQUIRE(saved && again,"completed-device state allocates advertised capacity");
  REQUIRE(anygm_state_save(selected.engine,saved,capacity,&written)==ANYGM_OK &&
          written>0u && written<=capacity &&
          anygm_state_load(restored.engine,saved,written)==ANYGM_OK,
          "completed-device pixels save and restore without a target");
  REQUIRE(restored.engine->state_frame_available &&
          restored.engine->state_frame_width==width &&
          restored.engine->state_frame_height==height,
          "completed-device state retains the authored completed extent");
  for(unsigned row=0;row<height;row++){
    for(unsigned column=0;column<width;column++){
      uint32_t expected=UINT32_C(0x43000033)|
        (((height-1u-row)&255u)<<16)|((column&255u)<<8);
      REQUIRE(restored.engine->screen[(size_t)row*width+column]==expected,
              "completed-device state preserves actual RGBA rather than CPU reconstruction");
    }
  }
  witness.current=0u;
  reads=anygm_test_graphics_read_pixels();
  anygm_test_graphics_forbid_calls();
  REQUIRE(anygm_state_save(selected.engine,again,capacity,&again_written)==ANYGM_OK &&
          again_written==written && !memcmp(saved,again,written) &&
          anygm_test_graphics_read_pixels()==reads &&
          !anygm_test_graphics_calls_after_forget(),
          "observed completed state remains byte-identical without a context");
  anygm_test_graphics_allow_calls();
  witness.current=1u;
  REQUIRE(advance(selected.engine,1,&flags) && (flags&ANYGM_FRAME_HARDWARE_TARGET),
          "controlled destruction starts with another resident completed frame");
  anygm_test_graphics_readback_alpha(0x45u);
  reads=anygm_test_graphics_read_pixels();
  unsigned materializations=selected.engine->frame_materializations;
  memset(&release_report,0,sizeof release_report);
  selected.engine->host.development_setting=release_report_setting;
  selected.engine->host.log=release_report_log;
  anygm_graphics_context_destroy(selected.engine,1u);
  REQUIRE(!engine_graphics_active(selected.engine) &&
          selected.engine->frame_authority==ENGINE_FRAME_CPU_MATERIALIZED &&
          anygm_test_graphics_read_pixels()>reads,
          "controlled destruction observes completed GPU pixels before deleting resources");
  REQUIRE(release_report.render_reports==1u && release_report.release_reports==1u &&
          release_report.reads_at_render==reads &&
          release_report.reads_at_release==anygm_test_graphics_read_pixels() &&
          release_report.render_materializations==materializations && materializations>0u &&
          selected.engine->frame_materializations==materializations+1u &&
          strstr(release_report.release,"context_current=1 access_witness=1 attempts=1 observed=1") &&
          strstr(release_report.release," materializations=1 ") &&
          strstr(release_report.release," completed_reads=1 "),
          "render report precedes required release reads without clearing prior observer counters");
  engine_graphics_report(selected.engine);
  REQUIRE(release_report.render_reports==1u && release_report.release_reports==1u,
          "later engine unload does not duplicate a released context report");
  witness.current=0u;
  anygm_test_graphics_forbid_calls();
  REQUIRE(anygm_state_save(selected.engine,again,capacity,&again_written)==ANYGM_OK &&
          !anygm_test_graphics_calls_after_forget() &&
          anygm_state_load(restored.engine,again,again_written)==ANYGM_OK,
          "state after controlled destruction needs no graphics context");
  for(unsigned row=0;row<height;row++) for(unsigned column=0;column<width;column++){
    uint32_t expected=UINT32_C(0x45000033)|
      (((height-1u-row)&255u)<<16)|((column&255u)<<8);
    REQUIRE(restored.engine->screen[(size_t)row*width+column]==expected,
            "controlled destruction retains actual completed RGBA, not a CPU replay");
  }
  anygm_test_graphics_allow_calls();
  free(saved); free(again); free(shadow);
  session_close(&selected); session_close(&restored);
  return 1;
}

static int scene_volatile_context_loss_case(void){
  for(unsigned scene=0u;scene<2u;scene++){
    Session selected={0};
    CurrentContextWitness witness={1u,0u};
    AnygmGraphicsContext context=graphics_context(ANYGM_GRAPHICS_OPENGLES3);
    GmlRenderDrawOp operation={0};
    GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
    GmlRenderResource *resource;
    GmlRenderImageRef previous;
    GmlGpuCounters before={0},after={0};
    uint8_t *saved=NULL,*lost=NULL;
    size_t capacity=0,written=0,lost_written=0;
    uint32_t backed_pixel=0u,flags=0u;
    unsigned reads=0u,deletions=0u;
    int volatile_id,backed_id,reused;
    anygm_test_graphics_reset();
    REQUIRE(session_open_with_scene(&selected,scene?ANYGM_SCENE_RENDERER_OPENGL:
                                                   ANYGM_SCENE_RENDERER_CPU),
            "volatile-loss control starts from fresh content");
    volatile_id=gml_surface_create(&selected.engine->render,4,4);
    REQUIRE(volatile_id>0,"volatile-loss control creates a pre-context surface");
    context.userdata=&witness;
    context.is_current=graphics_context_is_current;
    REQUIRE(anygm_graphics_context_reset(selected.engine,&context)==ANYGM_OK &&
            gml_surface_exists(&selected.engine->render,volatile_id) &&
            advance(selected.engine,1,&flags),
            "initial adoption does not masquerade as volatile context loss");
    backed_id=gml_surface_create(&selected.engine->render,4,4);
    resource=gml_render_surface_resource_ensure(&selected.engine->render,volatile_id);
    REQUIRE(resource && backed_id>0,"volatile-loss control owns both surface classes");
    operation.opcode=GML_RENDER_OP_CLEAR;
    operation.reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    operation.target.image=gml_render_resource_image_ref(resource);
    operation.target.image.content_version++;
    operation.target.storage_version=1u;
    operation.target.width=operation.target.height=4u;
    operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    operation.target.region=(GmlRenderRect){0,0,4u,4u};
    operation.payload.clear_colour=0x40556677u;
    REQUIRE(gml_render_surface_scene_submit(&selected.engine->render,volatile_id,
              &batch,NULL,0u)==GML_RENDER_RESOURCE_EXEC_DEVICE,
            "volatile-loss control actually writes device pixels");
    capacity=anygm_state_size(selected.engine);
    saved=(uint8_t*)malloc(capacity); lost=(uint8_t*)malloc(capacity);
    REQUIRE(saved && lost &&
            anygm_state_save(selected.engine,saved,capacity,&written)==ANYGM_OK,
            "volatile-loss control retains an earlier valid same-run save");
    selected.engine->render.surface[backed_id-1].px[0]=0x71423324u;
    backed_pixel=selected.engine->render.surface[backed_id-1].px[0];
    REQUIRE(advance(selected.engine,1,&flags),"volatile-loss control advances after its save");
    resource=selected.engine->render.surface[volatile_id-1].resource;
    REQUIRE(resource,"volatile-loss source remains live before the real loss");
    previous=gml_render_resource_image_ref(resource);
    operation.target.image=previous;
    operation.target.image.content_version++;
    operation.payload.clear_colour=0x50778899u;
    REQUIRE(gml_render_surface_scene_submit(&selected.engine->render,volatile_id,
              &batch,NULL,0u)==GML_RENDER_RESOURCE_EXEC_DEVICE,
            "volatile-loss control dirties GPU pixels beyond the saved CPU version");
    reads=anygm_test_graphics_read_pixels();
    deletions=(unsigned)anygm_test_graphics_deletes();
    gml_gpu_counters(selected.engine->gpu,&before);
    REQUIRE(anygm_graphics_context_reset(selected.engine,&context)==ANYGM_OK &&
            anygm_test_graphics_read_pixels()==reads &&
            (unsigned)anygm_test_graphics_deletes()==deletions,
            "replacement neither reads nor deletes objects in the dead namespace");
    REQUIRE(gml_surface_exists(&selected.engine->render,backed_id) &&
            selected.engine->render.surface[backed_id-1].px[0]==backed_pixel,
            "current CPU pixels survive a graphics-cache loss");
    if(scene){
      REQUIRE(!gml_surface_exists(&selected.engine->render,volatile_id) &&
              !gml_surface_width(&selected.engine->render,volatile_id) &&
              !gml_surface_height(&selected.engine->render,volatile_id),
              "device-only user surfaces become nonexistent after genuine context loss");
      REQUIRE(!selected.engine->have_presented_frame &&
              selected.engine->frame_authority==ENGINE_FRAME_NONE &&
              !selected.engine->host_plan_valid &&
              anygm_state_save(selected.engine,lost,capacity,&lost_written)==ANYGM_OK,
              "lost device-only frame is omitted rather than reconstructed from CPU recipes");
      selected.engine->render.next_surface_id=volatile_id;
      reused=gml_surface_create(&selected.engine->render,4,4);
      resource=gml_render_surface_resource_ensure(&selected.engine->render,reused);
      REQUIRE(reused==volatile_id && resource &&
              gml_render_resource_image_ref(resource).incarnation!=previous.incarnation,
              "reused volatile surface IDs cannot revive the lost image incarnation");
    } else {
      const uint32_t *pixels=gml_surface_pixels_read(&selected.engine->render,volatile_id,NULL,NULL);
      REQUIRE(pixels && pixels[0]==operation.payload.clear_colour,
              "default CPU scene retains its existing exact recovery behaviour");
    }
    REQUIRE(anygm_state_load(selected.engine,saved,written)==ANYGM_OK &&
            gml_surface_exists(&selected.engine->render,volatile_id) &&
            advance(selected.engine,1,&flags) && (flags&ANYGM_FRAME_HARDWARE_TARGET),
            "a valid saved image still restores and redraws on the replacement context");
    gml_gpu_counters(selected.engine->gpu,&after);
    REQUIRE(after.context_resets==before.context_resets+1u && after.passes_accepted>before.passes_accepted,
            "immutable resources rebuild for accepted work after loss");
    free(saved); free(lost);
    session_close(&selected);
  }
  return 1;
}

static int scene_size_query_is_metadata_only_case(void){
  Session plain={0},selected={0};
  GmlRenderResourceMetrics before={0},after={0};
  uint32_t *shadow=NULL;
  uint8_t *saved=NULL,*reference=NULL;
  size_t capacity=0,resume=0,written=0,reference_size=0,pixels=0;
  uint32_t materializations=0,authority=0;
  int pure=0,ok=0;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&plain),"metadata-size CPU reference loads");
  REQUIRE(session_open_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL),
          "metadata-size GPU session loads");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK &&
          advance(plain.engine,1,NULL) && advance(selected.engine,1,NULL),
          "metadata-size sessions complete matching frames");
  REQUIRE(selected.engine->frame_materializations==0u &&
          selected.engine->frame_authority!=ENGINE_FRAME_CPU_MATERIALIZED,
          "metadata-size positive control leaves the completed frame GPU-resident");
  pixels=(size_t)selected.engine->output_width*selected.engine->output_height;
  shadow=(uint32_t*)malloc(pixels*sizeof *shadow);
  REQUIRE(shadow && selected.engine->screen,"metadata-size shadow exists");
  memcpy(shadow,selected.engine->screen,pixels*sizeof *shadow);
  materializations=selected.engine->frame_materializations;
  authority=selected.engine->frame_authority;
  gml_render_resource_metrics(&selected.engine->render,&before);
  anygm_test_graphics_forbid_calls();
  capacity=anygm_state_size(selected.engine);
  resume=anygm_state_resume_size(selected.engine);
  pure=capacity>resume && resume>0u &&
       anygm_state_size(selected.engine)==capacity &&
       anygm_state_resume_size(selected.engine)==resume &&
       selected.engine->frame_materializations==materializations &&
       selected.engine->frame_authority==authority &&
       !memcmp(shadow,selected.engine->screen,pixels*sizeof *shadow) &&
       anygm_test_graphics_calls_after_forget()==0;
  gml_render_resource_metrics(&selected.engine->render,&after);
  pure=pure && before.completed_screen_cpu_materializations==
                 after.completed_screen_cpu_materializations;
  anygm_test_graphics_allow_calls();
  if(!pure){
    fprintf(stderr,"GPU size query observed pixels: capacity=%zu resume=%zu "
            "materializations=%u/%u authority=%u/%u\n",capacity,resume,
            materializations,selected.engine->frame_materializations,
            authority,selected.engine->frame_authority);
    goto done;
  }
  saved=(uint8_t*)malloc(capacity);
  reference=serialize(plain.engine,&reference_size);
  if(!saved || !reference) goto done;
  ok=anygm_state_save(selected.engine,saved,capacity,&written)==ANYGM_OK &&
     written>0u && written<=capacity && written==reference_size &&
     !memcmp(saved,reference,written) && selected.engine->frame_materializations==1u &&
     anygm_state_size(selected.engine)==capacity &&
     anygm_state_resume_size(selected.engine)==resume;
  if(!ok) fprintf(stderr,"metadata-sized save changed canonical bytes or exceeded its bound\n");
done:
  free(saved); free(reference); free(shadow);
  session_close(&selected); session_close(&plain);
  return ok;
}

/* Pre-Draw has its own screen raster before world admission. Its implicit clear
 * must not force an otherwise admitted frame through the processor. The fixture
 * also changes the visible view here, so moving the event into Draw would fail. */
static int resident_pre_draw_screen_case(void){
  Session plain={0},selected={0};
  GmlRenderDispatchMetrics dispatch={0};
  GmlGpuCounters counters={0};
  uint8_t *cpu_state=NULL,*gpu_state=NULL;
  size_t cpu_size=0,gpu_size=0;
  int matching;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(&plain,ANYGM_SCENE_RENDERER_CPU,
          anygm_synthetic_draw_content_create) &&
          session_open_fixture_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL,
          anygm_synthetic_draw_content_create),"Pre-Draw paired fixtures load");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "Pre-Draw selected fixture adopts a context");
  gml_render_dispatch_diagnostics_enable(&selected.engine->render,1);
  selected.engine->diagnostics.dispatch_frames.enabled=1;
  gml_render_dispatch_frame_bind(&selected.engine->render,
                                &selected.engine->diagnostics.dispatch_frames);
  REQUIRE(advance(plain.engine,2,NULL) && advance(selected.engine,2,NULL),
          "Pre-Draw paired fixtures complete two frames");
  REQUIRE(gml_render_dispatch_metrics(&selected.engine->render,&dispatch) &&
          dispatch.enabled && !dispatch.overflow && !dispatch.failed && !dispatch.unclassified,
          "Pre-Draw interval has enabled and classified dispatch accounting");
  for(unsigned family=0;family<GML_RENDER_SCENE_FAMILY_COUNT;family++)
    for(unsigned reason=0;reason<GML_RENDER_DISPATCH_REASON_COUNT;reason++)
      if(dispatch.cpu[family][reason]){
        fprintf(stderr,"Pre-Draw ordinary CPU work: family=%u reason=%u count=%llu\n",
                family,reason,(unsigned long long)dispatch.cpu[family][reason]);
        session_close(&selected); session_close(&plain);
        return 0;
      }
  REQUIRE(selected.engine->diagnostics.dispatch_frames.frames==2u &&
          !selected.engine->diagnostics.dispatch_frames.cpu_frames &&
          !selected.engine->diagnostics.dispatch_frames.no_scene_frames,
          "Pre-Draw clear does not create a no-scene frame");
  REQUIRE(gml_global_num(&selected.engine->vm,"fixture_pre_draw")==2 &&
          gml_global_num(&selected.engine->vm,"fixture_draw")==2 &&
          gml_global_num(&selected.engine->vm,"fixture_post_draw")==2 &&
          gml_global_num(&selected.engine->vm,"fixture_view_sum")==4,
          "resident Pre-Draw preserves event ordering and its view mutation");
  gml_gpu_counters(selected.engine->gpu,&counters);
  /* Two Pre-Draw and two world clears; the presented content needs no extra GUI clear. */
  REQUIRE(counters.scene_operations[GML_RENDER_OP_CLEAR]>=4u &&
          !counters.scene_batches_refused && !counters.cpu_upload_frames,
          "Pre-Draw, world and screen clears execute on the selected device");
  cpu_state=serialize(plain.engine,&cpu_size);
  gpu_state=serialize(selected.engine,&gpu_size);
  matching=cpu_state && gpu_state && cpu_size==gpu_size &&
           !memcmp(cpu_state,gpu_state,cpu_size);
  free(cpu_state); free(gpu_state);
  session_close(&selected); session_close(&plain);
  REQUIRE(matching,"resident Pre-Draw preserves exact CPU pixels and canonical state");
  return 1;
}

static int resident_world_selection_case(void){
  Session plain,selected;
  GmlGpuCounters counters={0};
  size_t plain_size=0,selected_size=0;
  uint8_t *plain_state=NULL,*selected_state=NULL;
  int matching;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&plain),"plain world fixture loads");
  REQUIRE(session_open_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL),
          "resident world fixture loads with the public scene selector");
  AnygmConfigDelta hybrid_off={0};
  hybrid_off.struct_size=sizeof hybrid_off;
  hybrid_off.values.struct_size=sizeof hybrid_off.values;
  hybrid_off.fields=ANYGM_CONFIG_HYBRID_GPU_PRESENTATION;
  REQUIRE(anygm_set_config(selected.engine,&hybrid_off)==ANYGM_OK,
          "scene-only fixture keeps hybrid presentation disabled");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "resident world fixture adopts a context");
  unsigned prepared_compiles=anygm_test_graphics_compile_calls();
  unsigned prepared_texture=anygm_test_graphics_first_texture_with_format(0x8058); /* RGBA8 storage. */
  REQUIRE(prepared_texture &&
          anygm_test_graphics_texture_width(prepared_texture)==(int)selected.engine->width &&
          anygm_test_graphics_texture_height(prepared_texture)==(int)selected.engine->height &&
          anygm_test_graphics_scene_draw_calls()==1 && anygm_test_graphics_read_pixels()==0,
          "GPU binding prepares extent capacity before the first frame without publishing pixels");
  REQUIRE(advance(plain.engine,1,NULL) && advance(selected.engine,1,NULL),
          "both world policies complete one frame");
  gml_gpu_counters(selected.engine->gpu,&counters);
  REQUIRE(counters.scene_operations[GML_RENDER_OP_CLEAR]>0u,
          "selected world submits a resident clear");
  REQUIRE(selected.engine->screen_pass_frames==1u && counters.cpu_upload_frames==0u,
          "scene-only presentation uses the resident image without a complete-frame upload");
  REQUIRE(anygm_test_graphics_compile_calls()==prepared_compiles,
          "the first resident frame reuses prepared common programs");
  AnygmConfigDelta changed={0};
  changed.struct_size=sizeof changed;
  changed.values.struct_size=sizeof changed.values;
  changed.fields=ANYGM_CONFIG_SCENE_RENDERER;
  changed.values.scene_renderer=ANYGM_SCENE_RENDERER_CPU;
  REQUIRE(anygm_set_config(selected.engine,&changed)==ANYGM_OK &&
          selected.engine->scene_renderer_session==ANYGM_SCENE_RENDERER_OPENGL,
          "a live menu change cannot switch the scene executor mid-session");
  plain_state=serialize(plain.engine,&plain_size);
  selected_state=serialize(selected.engine,&selected_size);
  matching=plain_state && selected_state && plain_size==selected_size &&
           !memcmp(plain_state,selected_state,plain_size);
  free(selected_state); free(plain_state);
  session_close(&selected); session_close(&plain);
  REQUIRE(matching,"resident world selection preserves canonical state bytes");
  return 1;
}

static int classic_viewless_application_scene_case(void){
  Session plain={0},selected={0};
  AnygmInputFrame input={0};
  AnygmFrameOutput cpu={0},gpu={0};
  GmlGpuCounters counters={0};
  GmlRenderPresentationMetrics metrics={0};
  int matching=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(
            &plain,ANYGM_SCENE_RENDERER_CPU,
            anygm_synthetic_classic_viewless_draw_content_create),
          "viewless classic CPU fixture loads");
  REQUIRE(session_open_fixture_with_scene(
            &selected,ANYGM_SCENE_RENDERER_OPENGL,
            anygm_synthetic_classic_viewless_draw_content_create),
          "viewless classic GPU fixture loads");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "viewless classic GPU context adopts");
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  for(int frame=0;frame<2 && matching;frame++){
    cpu.struct_size=gpu.struct_size=sizeof cpu;
    matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
             anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
             cpu.pixels && gpu.width==cpu.width && gpu.height==cpu.height &&
             selected.engine->screen;
    if(matching)
      matching=!memcmp(cpu.pixels,selected.engine->screen,
                       (size_t)cpu.width*cpu.height*sizeof(uint32_t));
  }
  gml_render_presentation_metrics(&selected.engine->render,&metrics);
  gml_gpu_counters(selected.engine->gpu,&counters);
  if(!matching || counters.scene_families[GML_RENDER_SCENE_SPRITE]==0u ||
     counters.scene_families[GML_RENDER_SCENE_TEXT]==0u)
    fprintf(stderr,"viewless classic: owned=%d draw=%d scene=%llu/%llu sprite=%llu text=%llu "
                   "transport=%u matching=%d classic=%d tpag=%d sprites=%d runtime_rgba=%d "
                   "sample=%08x\n",
            metrics.application_owned,metrics.application_draw_enabled,
            (unsigned long long)counters.scene_batches_offered,
            (unsigned long long)counters.scene_batches_accepted,
            (unsigned long long)counters.scene_families[GML_RENDER_SCENE_SPRITE],
            (unsigned long long)counters.scene_families[GML_RENDER_SCENE_TEXT],
            counters.cpu_upload_frames,matching,selected.engine->render.classic,
            selected.engine->render.n_tpag,selected.engine->render.n_spr,
            selected.engine->render.n_spr?selected.engine->render.spr[0].runtime_rgba!=NULL:0,
            cpu.pixels?((const uint32_t*)cpu.pixels)[17u*cpu.width+49u]:0u);
  matching=matching && metrics.application_owned && metrics.application_draw_enabled &&
           counters.scene_families[GML_RENDER_SCENE_SPRITE]>0u &&
           counters.scene_families[GML_RENDER_SCENE_TEXT]>0u &&
           counters.scene_families[GML_RENDER_SCENE_PRIMITIVE]==0u &&
           counters.scene_batches_accepted>0u && counters.scene_batches_refused==0u;
  session_close(&selected); session_close(&plain);
  REQUIRE(matching,"viewless classic application target accepts Draw scene work with CPU pixels");
  return 1;
}

/* A sole enlarged classic port keeps the logical Draw raster, not a larger per-sprite target. */
static int classic_single_view_application_scene_case(void){
  Session plain={0},selected={0};
  AnygmInputFrame input={0};
  AnygmFrameOutput cpu={0},gpu={0};
  GmlGpuCounters counters={0};
  GmlRenderPresentationMetrics metrics={0};
  int matching=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(
            &plain,ANYGM_SCENE_RENDERER_CPU,
            anygm_synthetic_classic_single_view_draw_content_create),
          "single-view classic CPU fixture loads");
  REQUIRE(session_open_fixture_with_scene(
            &selected,ANYGM_SCENE_RENDERER_OPENGL,
            anygm_synthetic_classic_single_view_draw_content_create),
          "single-view classic GPU fixture loads");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "single-view classic GPU context adopts");
  gml_render_presentation_metrics(&selected.engine->render,&metrics);
  REQUIRE(!metrics.application_owned && metrics.application_width==128 &&
          metrics.application_height==96,
          "single-view classic initially borrows the display-size application");
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  for(int frame=0;frame<3 && matching;frame++){
    cpu.struct_size=gpu.struct_size=sizeof cpu;
    matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
             anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
             engine_materialize_completed_frame(selected.engine) &&
             cpu.pixels && selected.engine->screen &&
             cpu.width==128u && cpu.height==96u &&
             gpu.width==cpu.width && gpu.height==cpu.height &&
             selected.engine->width==64u && selected.engine->height==48u &&
             selected.engine->present_mouse_width==plain.engine->present_mouse_width &&
             selected.engine->present_mouse_height==plain.engine->present_mouse_height &&
             selected.engine->vm.room_index==plain.engine->vm.room_index &&
             selected.engine->vm.frame==plain.engine->vm.frame &&
             !memcmp(cpu.pixels,selected.engine->screen,
                     (size_t)cpu.width*cpu.height*sizeof(uint32_t));
    if(!matching)
      fprintf(stderr,"single-view classic frame=%d cpu=%ux%u gpu=%ux%u "
                     "logical=%ux%u/%ux%u room=%d/%d frame=%ld/%ld pixels=%d/%d\n",
              frame,cpu.width,cpu.height,gpu.width,gpu.height,
              plain.engine->width,plain.engine->height,
              selected.engine->width,selected.engine->height,
              plain.engine->vm.room_index,selected.engine->vm.room_index,
              plain.engine->vm.frame,selected.engine->vm.frame,
             cpu.pixels!=NULL,selected.engine->screen!=NULL);
    gml_gpu_counters(selected.engine->gpu,&counters);
    gml_render_presentation_metrics(&selected.engine->render,&metrics);
    /* The first Draw still exposes the display-size borrowed surface. Its metadata
     * does not change the world resource's stride or prevent resident drawing. */
    GmlVal *cpu_width=gml_varmap_get(&plain.engine->vm.globals,"fixture_application_width");
    GmlVal *gpu_width=gml_varmap_get(&selected.engine->vm.globals,"fixture_application_width");
    GmlVal *cpu_height=gml_varmap_get(&plain.engine->vm.globals,"fixture_application_height");
    GmlVal *gpu_height=gml_varmap_get(&selected.engine->vm.globals,"fixture_application_height");
    matching=matching && cpu_width && gpu_width && cpu_height && gpu_height &&
      cpu_width->d==gpu_width->d && cpu_height->d==gpu_height->d &&
      cpu_width->d==(frame==0?128.0:64.0) && cpu_height->d==(frame==0?96.0:48.0) &&
      !metrics.application_owned &&
      counters.scene_families[GML_RENDER_SCENE_SPRITE]==(uint64_t)frame+1u &&
      counters.scene_families[GML_RENDER_SCENE_TEXT]==((uint64_t)frame+1u)*3u;
  }
  gml_render_presentation_metrics(&selected.engine->render,&metrics);
  gml_gpu_counters(selected.engine->gpu,&counters);
  if(!matching || metrics.application_owned ||
     metrics.application_width!=64 || metrics.application_height!=48 ||
     counters.scene_families[GML_RENDER_SCENE_SPRITE]!=3u ||
     counters.scene_families[GML_RENDER_SCENE_TEXT]==0u ||
     counters.scene_batches_refused!=0u)
    fprintf(stderr,"single-view classic: matching=%d owned=%d app=%dx%d "
                   "output=%ux%u sprite=%llu text=%llu refused=%llu\n",
            matching,metrics.application_owned,metrics.application_width,
            metrics.application_height,cpu.width,cpu.height,
            (unsigned long long)counters.scene_families[GML_RENDER_SCENE_SPRITE],
            (unsigned long long)counters.scene_families[GML_RENDER_SCENE_TEXT],
            (unsigned long long)counters.scene_batches_refused);
  matching=matching && !metrics.application_owned &&
           metrics.application_width==64 && metrics.application_height==48 &&
           counters.scene_families[GML_RENDER_SCENE_SPRITE]==3u &&
           counters.scene_families[GML_RENDER_SCENE_TEXT]>0u &&
           counters.scene_batches_refused==0u;
  session_close(&selected); session_close(&plain);
  REQUIRE(matching,"single classic view admits logical Draw without changing its presentation");
  return 1;
}

static int classic_single_view_geometry_transition_case(void){
  Session plain={0},selected={0};
  AnygmInputFrame input={.struct_size=sizeof input,.pointer_x=-1,.pointer_y=-1};
  AnygmFrameOutput cpu={0},gpu={0};
  int matching=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(&plain,ANYGM_SCENE_RENDERER_CPU,
            anygm_synthetic_classic_single_view_geometry_content_create) &&
          session_open_fixture_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL,
            anygm_synthetic_classic_single_view_geometry_content_create),
          "classic geometry transition fixtures load");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "classic geometry transition context adopts");
  for(unsigned frame=0;frame<6u && matching;frame++){
    cpu.struct_size=gpu.struct_size=sizeof cpu;
    matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
             anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
             engine_materialize_completed_frame(selected.engine) &&
             cpu.pixels && selected.engine->screen && cpu.width==gpu.width &&
             cpu.height==gpu.height && plain.engine->vm.room_index==selected.engine->vm.room_index &&
             !memcmp(cpu.pixels,selected.engine->screen,(size_t)cpu.width*cpu.height*4u);
    GmlVal *cpu_width=gml_varmap_get(&plain.engine->vm.globals,"fixture_application_width");
    GmlVal *gpu_width=gml_varmap_get(&selected.engine->vm.globals,"fixture_application_width");
    GmlVal *cpu_height=gml_varmap_get(&plain.engine->vm.globals,"fixture_application_height");
    GmlVal *gpu_height=gml_varmap_get(&selected.engine->vm.globals,"fixture_application_height");
    matching=matching && cpu_width && gpu_width && cpu_height && gpu_height &&
             cpu_width->d==gpu_width->d && cpu_height->d==gpu_height->d;
    if(!matching) fprintf(stderr,"classic geometry transition frame=%u room=%d/%d logical=%ux%u/%ux%u "
      "output=%ux%u/%ux%u application=%.0fx%.0f/%.0fx%.0f\n",frame,
      plain.engine->vm.room_index,selected.engine->vm.room_index,
      plain.engine->width,plain.engine->height,selected.engine->width,selected.engine->height,
      cpu.width,cpu.height,gpu.width,gpu.height,
      cpu_width?cpu_width->d:-1.0,cpu_height?cpu_height->d:-1.0,
      gpu_width?gpu_width->d:-1.0,gpu_height?gpu_height->d:-1.0);
  }
  session_close(&selected); session_close(&plain);
  REQUIRE(matching,"classic GPU admission preserves a later logical geometry transition");
  return 1;
}

static int classic_borrowed_resize_scene_case(void){
  Session plain={0},selected={0};
  AnygmInputFrame input={.struct_size=sizeof input,.pointer_x=-1,.pointer_y=-1};
  AnygmFrameOutput cpu={0},gpu={0};
  int matching=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(&plain,ANYGM_SCENE_RENDERER_CPU,
            anygm_synthetic_classic_borrowed_resize_content_create) &&
          session_open_fixture_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL,
            anygm_synthetic_classic_borrowed_resize_content_create),
          "borrowed resize fixtures load");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "borrowed resize context adopts");
  for(unsigned frame=0;frame<9u && matching;frame++){
    unsigned width=frame<3u?128u:frame<6u?80u:64u;
    unsigned height=frame<3u?96u:frame<6u?60u:48u;
    unsigned old_width=frame==0u?64u:frame==3u?128u:frame==6u?80u:width;
    unsigned old_height=frame==0u?48u:frame==3u?96u:frame==6u?60u:height;
    cpu.struct_size=gpu.struct_size=sizeof cpu;
    matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
             anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
             engine_materialize_completed_frame(selected.engine) &&
             cpu.pixels && selected.engine->screen &&
             cpu.width==width && cpu.height==height &&
             gpu.width==width && gpu.height==height &&
             selected.engine->width==width && selected.engine->height==height &&
             !memcmp(cpu.pixels,selected.engine->screen,(size_t)width*height*4u);
    GmlRenderPresentationMetrics metrics={0};
    GmlGpuCounters counters={0};
    gml_render_presentation_metrics(&selected.engine->render,&metrics);
    gml_gpu_counters(selected.engine->gpu,&counters);
    GmlVal *cw=gml_varmap_get(&plain.engine->vm.globals,"fixture_application_width");
    GmlVal *gw=gml_varmap_get(&selected.engine->vm.globals,"fixture_application_width");
    GmlVal *ch=gml_varmap_get(&plain.engine->vm.globals,"fixture_application_height");
    GmlVal *gh=gml_varmap_get(&selected.engine->vm.globals,"fixture_application_height");
    matching=matching && cw && gw && ch && gh &&
      cw->d==old_width && gw->d==cw->d && ch->d==old_height && gh->d==ch->d &&
      !metrics.application_owned && metrics.application_width==(int)width &&
      metrics.application_height==(int)height &&
      counters.scene_families[GML_RENDER_SCENE_SPRITE]==frame+1u &&
      counters.scene_families[GML_RENDER_SCENE_TEXT]==(frame+1u)*3u &&
      !counters.scene_batches_refused;
    if(!matching) fprintf(stderr,"borrowed resize frame=%u output=%ux%u/%ux%u "
      "application=%.0fx%.0f/%.0fx%.0f sprite=%llu text=%llu\n",frame,
      cpu.width,cpu.height,gpu.width,gpu.height,cw?cw->d:-1.0,ch?ch->d:-1.0,
      gw?gw->d:-1.0,gh?gh->d:-1.0,
      (unsigned long long)counters.scene_families[GML_RENDER_SCENE_SPRITE],
      (unsigned long long)counters.scene_families[GML_RENDER_SCENE_TEXT]);
  }
  session_close(&selected); session_close(&plain);
  REQUIRE(matching,"borrowed resize preserves CPU pixels and old Draw metadata with resident work");
  return 1;
}

static int classic_single_view_exclusions_case(void){
  int (*factories[])(AnygmSyntheticContent *)={
    anygm_synthetic_classic_single_view_filtered_content_create,
    anygm_synthetic_classic_single_view_fractional_content_create,
    anygm_synthetic_classic_single_view_modern_content_create,
    anygm_synthetic_classic_single_view_offset_content_create
  };
  for(unsigned variant=0;variant<sizeof factories/sizeof factories[0];variant++){
    Session plain={0},selected={0};
    AnygmInputFrame input={.struct_size=sizeof input,.pointer_x=-1,.pointer_y=-1};
    AnygmFrameOutput cpu={0},gpu={0};
    GmlRenderPresentationMetrics metrics={0};
    GmlGpuCounters counters={0};
    int matching=1;
    anygm_test_graphics_reset();
    REQUIRE(session_open_fixture_with_scene(&plain,ANYGM_SCENE_RENDERER_CPU,factories[variant]) &&
            session_open_fixture_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL,factories[variant]),
            "excluded single-view classic CPU/GPU fixtures load");
    REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
            "excluded single-view classic context adopts");
    for(unsigned frame=0;frame<3u && matching;frame++){
      cpu.struct_size=gpu.struct_size=sizeof cpu;
      matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
               anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
               engine_materialize_completed_frame(selected.engine) && cpu.pixels &&
               selected.engine->screen && cpu.width==gpu.width && cpu.height==gpu.height &&
               !memcmp(cpu.pixels,selected.engine->screen,(size_t)cpu.width*cpu.height*4u);
      gml_render_presentation_metrics(&selected.engine->render,&metrics);
      matching=matching && !metrics.application_owned;
    }
    gml_gpu_counters(selected.engine->gpu,&counters);
    matching=matching && counters.scene_families[GML_RENDER_SCENE_SPRITE]==0u &&
             counters.scene_families[GML_RENDER_SCENE_TEXT]==0u;
    if(!matching) fprintf(stderr,"excluded classic single-view variant=%u owned=%d sprite=%llu text=%llu\n",
      variant,metrics.application_owned,
      (unsigned long long)counters.scene_families[GML_RENDER_SCENE_SPRITE],
      (unsigned long long)counters.scene_families[GML_RENDER_SCENE_TEXT]);
    session_close(&selected); session_close(&plain);
    REQUIRE(matching,"filtered, fractional, modern and translated classic views retain CPU policy");
  }
  return 1;
}

static int classic_single_view_draw_contracts_case(void){
  int (*factories[])(AnygmSyntheticContent *)={
    anygm_synthetic_classic_single_view_rectangle_content_create,
    anygm_synthetic_classic_single_view_binary_content_create,
    anygm_synthetic_classic_single_view_partial_alpha_content_create,
    anygm_synthetic_classic_single_view_source_alpha_content_create
  };
  for(unsigned variant=0;variant<sizeof factories/sizeof factories[0];variant++){
    Session plain={0},selected={0};
    AnygmInputFrame input={.struct_size=sizeof input,.pointer_x=-1,.pointer_y=-1};
    AnygmFrameOutput cpu={0},gpu={0};
    int matching=1;
    unsigned maximum[4]={0};
    anygm_test_graphics_reset();
    REQUIRE(session_open_fixture_with_scene(&plain,ANYGM_SCENE_RENDERER_CPU,factories[variant]) &&
            session_open_fixture_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL,factories[variant]),
            "classic draw contract fixtures load");
    REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
            "classic draw contract GPU context adopts");
    for(unsigned frame=0;frame<6u && matching;frame++){
      cpu.struct_size=gpu.struct_size=sizeof cpu;
      matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
               anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
               engine_materialize_completed_frame(selected.engine) && cpu.pixels &&
               selected.engine->screen && cpu.width==gpu.width && cpu.height==gpu.height;
      if(matching){
        const uint8_t *a=(const uint8_t *)cpu.pixels;
        const uint8_t *b=(const uint8_t *)selected.engine->screen;
        for(size_t byte=0;byte<(size_t)cpu.width*cpu.height*4u;byte++){
          unsigned difference=(unsigned)abs((int)a[byte]-(int)b[byte]);
          if(difference>maximum[byte%4u]) maximum[byte%4u]=difference;
          if(difference>0u) matching=0;
        }
      }
      GmlRenderPresentationMetrics metrics={0};
      gml_render_presentation_metrics(&selected.engine->render,&metrics);
      matching=matching && !metrics.application_owned;
    }
    GmlGpuCounters counters={0};
    gml_gpu_counters(selected.engine->gpu,&counters);
    /* This fake context deliberately has no owning-context witness. Exact replay draws
     * qualify here; tolerant pixels require the separate real-driver controls. */
    matching=matching && counters.scene_families[GML_RENDER_SCENE_SPRITE]==(variant<2u?6u:0u) &&
             counters.scene_batches_refused==0u;
    if(variant==0u) matching=matching &&
      counters.scene_families[GML_RENDER_SCENE_PRIMITIVE]==24u;
    fprintf(stderr,"classic draw variant=%u max_bytes=%u,%u,%u,%u sprite=%llu primitive=%llu matching=%d\n",
      variant,maximum[0],maximum[1],maximum[2],maximum[3],
      (unsigned long long)counters.scene_families[GML_RENDER_SCENE_SPRITE],
      (unsigned long long)counters.scene_families[GML_RENDER_SCENE_PRIMITIVE],matching);
    session_close(&selected); session_close(&plain);
    REQUIRE(matching,"classic exact draws qualify while unwitnessed tolerant draws retain CPU fallback");
  }
  return 1;
}

static int classic_retained_target_scene_case(void){
  int (*factories[])(AnygmSyntheticContent *)={
    anygm_synthetic_classic_retained_target_content_create,
    anygm_synthetic_classic_late_retained_target_content_create
  };
  for(unsigned variant=0;variant<sizeof factories/sizeof factories[0];variant++){
    Session plain={0},selected={0};
    AnygmInputFrame input={.struct_size=sizeof input,.pointer_x=-1,.pointer_y=-1};
    AnygmFrameOutput cpu={0},gpu={0};
    GmlGpuCounters counters={0};
    int matching=1;
    anygm_test_graphics_reset();
    REQUIRE(session_open_fixture_with_scene(&plain,ANYGM_SCENE_RENDERER_CPU,factories[variant]) &&
            session_open_fixture_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL,factories[variant]),
            "classic retained-target CPU/GPU fixtures load");
    REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
            "classic retained-target context adopts");
    for(unsigned frame=0;frame<4u && matching;frame++){
      cpu.struct_size=gpu.struct_size=sizeof cpu;
      matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
               anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
               engine_materialize_completed_frame(selected.engine) && cpu.pixels &&
               selected.engine->screen && cpu.width==gpu.width && cpu.height==gpu.height &&
               !memcmp(cpu.pixels,selected.engine->screen,(size_t)cpu.width*cpu.height*4u);
      GmlVal *cpu_id=gml_varmap_get(&plain.engine->vm.globals,"fixture_surface");
      GmlVal *gpu_id=gml_varmap_get(&selected.engine->vm.globals,"fixture_surface");
      const uint32_t *cpu_surface=cpu_id?
        gml_surface_pixels_read(&plain.engine->render,(int)cpu_id->d,NULL,NULL):NULL;
      const uint32_t *gpu_surface=gpu_id?
        gml_surface_pixels_read(&selected.engine->render,(int)gpu_id->d,NULL,NULL):NULL;
      matching=matching && cpu_surface && gpu_surface &&
        !memcmp(cpu_surface,gpu_surface,8u*8u*4u) &&
        cpu_surface[0]==0xFF0000FFu && cpu_surface[18]!=0xFF0000FFu &&
        gml_surface_get_target(&selected.engine->render)==-1;
      if(!matching){
        size_t frame_difference=SIZE_MAX,surface_difference=SIZE_MAX;
        if(cpu.pixels && selected.engine->screen && cpu.width==gpu.width && cpu.height==gpu.height)
          for(size_t pixel=0;pixel<(size_t)cpu.width*cpu.height;pixel++)
            if(((const uint32_t *)cpu.pixels)[pixel]!=selected.engine->screen[pixel]){
              frame_difference=pixel;break;
            }
        if(cpu_surface && gpu_surface)
          for(size_t pixel=0;pixel<64u;pixel++)
            if(cpu_surface[pixel]!=gpu_surface[pixel]){surface_difference=pixel;break;}
        fprintf(stderr,"classic retained-target variant=%u frame=%u dimensions=%ux%u/%ux%u "
          "frame_difference=%zu surface_difference=%zu cpu_surface=%08x/%08x "
          "gpu_surface=%08x/%08x target=%d\n",variant,frame,cpu.width,cpu.height,
          gpu.width,gpu.height,frame_difference,surface_difference,
          cpu_surface?cpu_surface[0]:0u,cpu_surface?cpu_surface[18]:0u,
          gpu_surface?gpu_surface[0]:0u,gpu_surface?gpu_surface[18]:0u,
          gml_surface_get_target(&selected.engine->render));
      }
    }
    gml_gpu_counters(selected.engine->gpu,&counters);
    matching=matching && counters.scene_families[GML_RENDER_SCENE_SPRITE]==4u &&
      counters.scene_batches_refused==0u;
    if(!matching) fprintf(stderr,"classic retained-target variant=%u sprite=%llu refused=%llu\n",
      variant,(unsigned long long)counters.scene_families[GML_RENDER_SCENE_SPRITE],
      (unsigned long long)counters.scene_batches_refused);
    session_close(&selected);session_close(&plain);
    REQUIRE(matching,"classic Step target survives GPU Draw and GUI before exact composition");
  }
  return 1;
}

static int classic_single_view_cpu_observers_case(void){
  int (*factories[])(AnygmSyntheticContent *)={
    anygm_synthetic_classic_single_view_read_content_create,
    anygm_synthetic_classic_single_view_cpu_gui_content_create,
    anygm_synthetic_classic_single_view_filter_transition_content_create,
    anygm_synthetic_classic_single_view_zero_alpha_content_create,
    anygm_synthetic_classic_single_view_zero_alpha_part_content_create,
    anygm_synthetic_classic_single_view_software3d_content_create
  };
  for(unsigned variant=0;variant<sizeof factories/sizeof factories[0];variant++){
    Session plain={0},selected={0};
    AnygmInputFrame input={.struct_size=sizeof input,.pointer_x=-1,.pointer_y=-1};
    AnygmFrameOutput cpu={0},gpu={0};
    int matching=1;
    anygm_test_graphics_reset();
    REQUIRE(session_open_fixture_with_scene(&plain,ANYGM_SCENE_RENDERER_CPU,factories[variant]) &&
            session_open_fixture_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL,factories[variant]),
            "classic application observation fixtures load");
    REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
            "classic observation GPU context adopts");
    for(unsigned frame=0;frame<6u && matching;frame++){
      cpu.struct_size=gpu.struct_size=sizeof cpu;
      matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
               anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
               engine_materialize_completed_frame(selected.engine) && cpu.pixels &&
               selected.engine->screen && cpu.width==gpu.width && cpu.height==gpu.height &&
               !memcmp(cpu.pixels,selected.engine->screen,(size_t)cpu.width*cpu.height*4u);
      GmlRenderPresentationMetrics metrics={0};
      gml_render_presentation_metrics(&selected.engine->render,&metrics);
      matching=matching && !metrics.application_owned;
      if(variant==0u){
        GmlVal *cpu_pixel=gml_varmap_get(&plain.engine->vm.globals,"fixture_pixel");
        GmlVal *gpu_pixel=gml_varmap_get(&selected.engine->vm.globals,"fixture_pixel");
        matching=matching && cpu_pixel && gpu_pixel && cpu_pixel->d==gpu_pixel->d;
      }
      if(!matching) fprintf(stderr,"classic observer variant=%u frame=%u\n",variant,frame);
    }
    GmlGpuCounters counters={0};
    gml_gpu_counters(selected.engine->gpu,&counters);
    if(variant==3u)
      matching=matching && counters.scene_families[GML_RENDER_SCENE_SPRITE]==6u &&
               counters.scene_families[GML_RENDER_SCENE_TEXT]==18u;
    if(variant==4u)
      matching=matching && counters.scene_families[GML_RENDER_SCENE_SPRITE]==12u &&
               counters.scene_families[GML_RENDER_SCENE_TEXT]==18u;
    if(variant==5u)
      matching=matching && counters.scene_families[GML_RENDER_SCENE_SPRITE]==6u &&
               counters.scene_families[GML_RENDER_SCENE_TEXT]==0u;
    if(!matching || !counters.scene_families[GML_RENDER_SCENE_SPRITE])
      fprintf(stderr,"classic observer variant=%u sprite=%llu text=%llu refused=%llu matching=%d\n",
        variant,(unsigned long long)counters.scene_families[GML_RENDER_SCENE_SPRITE],
        (unsigned long long)counters.scene_families[GML_RENDER_SCENE_TEXT],
        (unsigned long long)counters.scene_batches_refused,matching);
    matching=matching && counters.scene_families[GML_RENDER_SCENE_SPRITE]>0u &&
             counters.scene_batches_refused==0u;
    session_close(&selected); session_close(&plain);
    REQUIRE(matching,"borrowed GPU application preserves CPU observers, 3D and zero-alpha prefixes");
  }
  return 1;
}

/* The target can be discarded between two frontend calls even after the runtime has ended.
 * Replaying the final frame then needs the completed CPU picture, not a stale screen buffer. */
static int ended_frame_retention_variant(int lose_context){
  Session plain={0},selected={0};
  AnygmInputFrame input={0};
  AnygmFrameOutput cpu={0},gpu={0};
  uint8_t *cpu_state=NULL,*gpu_state=NULL;
  size_t cpu_size=0,gpu_size=0;
  int ok=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(
            &plain,ANYGM_SCENE_RENDERER_CPU,
            anygm_synthetic_ended_frame_content_create),"ended CPU fixture loads");
  REQUIRE(session_open_fixture_with_scene(
            &selected,ANYGM_SCENE_RENDERER_OPENGL,
            anygm_synthetic_ended_frame_content_create),"ended GPU fixture loads");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "ended GPU fixture adopts a context");
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  for(int frame=0;frame<5 && ok;frame++){
    if(frame==2 && lose_context)
      anygm_graphics_context_destroy(selected.engine,1u);
    cpu.struct_size=gpu.struct_size=sizeof cpu;
    ok=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
       anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
       cpu.pixels && cpu.width==64u && cpu.height==48u &&
       gpu.width==cpu.width && gpu.height==cpu.height;
    if(!ok) break;
    if(frame==1)
      ok=selected.engine->runtime_ended &&
         selected.engine->screen_pass_frames==2u &&
         gml_render_deferred_presentation(&selected.engine->render,NULL);
    if(frame>=2){
      ok=ok && plain.engine->runtime_ended && selected.engine->runtime_ended &&
         (cpu.flags&ANYGM_FRAME_SHUTDOWN_REQUESTED) &&
         (gpu.flags&ANYGM_FRAME_SHUTDOWN_REQUESTED) &&
         selected.engine->frame_materializations==1u &&
         plain.engine->vm.frame==selected.engine->vm.frame;
      for(unsigned row=0;row<cpu.height && ok;row++)
        ok=!memcmp((const uint8_t*)cpu.pixels+(size_t)row*cpu.pitch,
                   selected.engine->screen+(size_t)row*cpu.width,
                   (size_t)cpu.width*4u);
    }
  }
  if(ok){
    cpu_state=serialize(plain.engine,&cpu_size);
    gpu_state=serialize(selected.engine,&gpu_size);
    ok=cpu_state && gpu_state && cpu_size==gpu_size &&
       !memcmp(cpu_state,gpu_state,cpu_size);
  }
  free(gpu_state); free(cpu_state);
  session_close(&selected); session_close(&plain);
  REQUIRE(ok,lose_context?"ended frame survives context loss and preserves state":
                          "ended frame survives repeated host calls and preserves state");
  return 1;
}

static int ended_frame_retention_case(void){
  return ended_frame_retention_variant(0) && ended_frame_retention_variant(1);
}

static int resident_cursor_frame_case(void){
  Session plain={0},selected={0};
  AnygmInputFrame input={0};
  AnygmFrameOutput cpu={0},gpu={0};
  GmlGpuCounters counters={0};
  int matching=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(
            &plain,ANYGM_SCENE_RENDERER_CPU,
            anygm_synthetic_cursor_frame_content_create),"cursor CPU fixture loads");
  REQUIRE(session_open_fixture_with_scene(
            &selected,ANYGM_SCENE_RENDERER_OPENGL,
            anygm_synthetic_cursor_frame_content_create),"cursor GPU fixture loads");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "cursor GPU fixture adopts a context");
  input.struct_size=sizeof input;
  input.pointer_x=32;
  input.pointer_y=24;
  cpu.struct_size=gpu.struct_size=sizeof cpu;
  matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
           anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
           cpu.pixels && cpu.width==64u && cpu.height==48u &&
           selected.engine->screen && gpu.width==cpu.width && gpu.height==cpu.height;
  if(matching){
    for(unsigned row=0;row<cpu.height && matching;row++)
      matching=!memcmp((const uint8_t*)cpu.pixels+(size_t)row*cpu.pitch,
                       selected.engine->screen+(size_t)row*cpu.width,
                       (size_t)cpu.width*4u);
  }
  gml_gpu_counters(selected.engine->gpu,&counters);
  matching=matching && counters.scene_batches_accepted>0u &&
           selected.engine->frame_materializations==1u &&
           !gml_render_deferred_presentation(&selected.engine->render,NULL) &&
           ((const uint32_t*)cpu.pixels)[0]!=0u &&
           ((const uint32_t*)cpu.pixels)[24u*64u+32u]!=
             ((const uint32_t*)cpu.pixels)[0];
  session_close(&selected); session_close(&plain);
  REQUIRE(matching,"authored cursor preserves the completed resident scene underneath");
  return 1;
}

static int resident_transition_snapshot_case(void){
  Session plain={0},selected={0};
  uint32_t flags=0;
  int matching=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&plain),"transition snapshot CPU fixture loads");
  REQUIRE(session_open_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL),
          "transition snapshot GPU fixture loads");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "transition snapshot GPU fixture adopts a context");
  matching=advance(plain.engine,1,&flags) && advance(selected.engine,1,&flags) &&
           gml_render_deferred_presentation(&selected.engine->render,NULL) &&
           classic_transition_start(plain.engine,64u,48u,20) &&
           classic_transition_start(selected.engine,64u,48u,20);
  if(matching)
    matching=!memcmp(plain.engine->classic_transition.old_frame,
                     selected.engine->classic_transition.old_frame,64u*48u*4u);
  matching=matching && selected.engine->frame_materializations==1u &&
           !gml_render_deferred_presentation(&selected.engine->render,NULL);
  session_close(&selected); session_close(&plain);
  REQUIRE(matching,"transition snapshot reads the completed resident frame");
  return 1;
}

static int classic_temporal_fallback_variant(
    int (*create_fixture)(AnygmSyntheticContent *),int transition){
  Session plain={0},selected={0};
  AnygmInputFrame input={0};
  AnygmFrameOutput cpu={0},gpu={0};
  GmlGpuCounters counters={0};
  long previous_step=-1;
  double previous_fixture_step=-1.0;
  uint32_t first_colour=0,first_fade=0,black_midpoint=0,new_colour=0;
  int ok=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(
            &plain,ANYGM_SCENE_RENDERER_CPU,create_fixture),
          "classic temporal CPU fixture loads");
  REQUIRE(session_open_fixture_with_scene(
            &selected,ANYGM_SCENE_RENDERER_OPENGL,create_fixture),
          "classic temporal selected fixture loads");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "classic temporal fixture adopts a context");
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  for(int frame=0;frame<(transition?9:3) && ok;frame++){
    cpu.struct_size=gpu.struct_size=sizeof cpu;
    ok=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
       anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
       cpu.pixels && cpu.width==64u && cpu.height==48u &&
       selected.engine->screen && gpu.width==cpu.width && gpu.height==cpu.height &&
       plain.engine->vm.frame==selected.engine->vm.frame;
    if(!ok) break;
    for(unsigned row=0;row<cpu.height && ok;row++)
      ok=!memcmp((const uint8_t*)cpu.pixels+(size_t)row*cpu.pitch,
                 selected.engine->screen+(size_t)row*cpu.width,
                 (size_t)cpu.width*4u);
    if(previous_step>=0 && !transition)
      ok=ok && plain.engine->vm.frame==previous_step+1;
    previous_step=plain.engine->vm.frame;
    if(transition){
      static const long expected_steps[9]={1,2,2,2,2,2,3,4,5};
      double fixture_step=gml_global_num(&plain.engine->vm,"fixture_frame");
      ok=ok && plain.engine->vm.frame==expected_steps[frame] &&
         selected.engine->vm.frame==expected_steps[frame];
      if(frame>=2) ok=ok && fixture_step==previous_fixture_step;
      previous_fixture_step=fixture_step;
      if(frame==0) first_colour=((const uint32_t*)cpu.pixels)[32u+24u*64u]&0xFFFFFFu;
      if(frame==1) first_fade=((const uint32_t*)cpu.pixels)[32u+24u*64u]&0xFFFFFFu;
      if(frame==3) black_midpoint=((const uint32_t*)cpu.pixels)[32u+24u*64u]&0xFFFFFFu;
      if(frame==6) new_colour=((const uint32_t*)cpu.pixels)[32u+24u*64u]&0xFFFFFFu;
    } else {
      ok=ok && gml_global_num(&plain.engine->vm,"fixture_frame")==frame+1 &&
         gml_global_num(&selected.engine->vm,"fixture_frame")==frame+1 &&
         gml_global_num(&plain.engine->vm,"fixture_seen")==frame+2 &&
         gml_global_num(&selected.engine->vm,"fixture_seen")==frame+2;
    }
  }
  gml_gpu_counters(selected.engine->gpu,&counters);
  ok=ok && counters.scene_batches_offered==0u && counters.scene_batches_accepted==0u &&
     counters.cpu_upload_frames==(unsigned)(transition?9:3);
  if(transition)
    ok=ok && plain.engine->vm.room_index==1 && selected.engine->vm.room_index==1 &&
       first_colour!=0u && first_fade!=0u && first_fade!=first_colour &&
       black_midpoint==0u && new_colour!=0u && new_colour!=first_fade;
  if(!ok)
    fprintf(stderr,"classic temporal %s: room=%d/%d step=%ld fixture=%.0f "
                   "first=%06x fade=%06x mid=%06x new=%06x scene=%llu/%llu transport=%u\n",
            transition?"transition":"redraw",plain.engine->vm.room_index,
            selected.engine->vm.room_index,plain.engine->vm.frame,
            gml_global_num(&plain.engine->vm,"fixture_frame"),first_colour,first_fade,
            black_midpoint,new_colour,
            (unsigned long long)counters.scene_batches_offered,
            (unsigned long long)counters.scene_batches_accepted,
            counters.cpu_upload_frames);
  session_close(&selected); session_close(&plain);
  REQUIRE(ok,transition?"classic fade has old, black and new frames without extra Step":
                        "screen_redraw repeats Draw once and refresh does not");
  return 1;
}

static int classic_temporal_fallback_case(void){
  return classic_temporal_fallback_variant(
           anygm_synthetic_classic_redraw_content_create,0) &&
         classic_temporal_fallback_variant(
           anygm_synthetic_classic_transition_content_create,1);
}

static int resident_indirect_gui_variant(
    int (*create_fixture)(AnygmSyntheticContent *),int scale_direction){
  Session plain={0},selected={0};
  AnygmInputFrame input={0};
  AnygmFrameOutput cpu={0},gpu={0};
  GmlGpuCounters counters={0};
  int matching;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(
            &plain,ANYGM_SCENE_RENDERER_CPU,create_fixture),
          "indirect GUI CPU fixture loads");
  REQUIRE(session_open_fixture_with_scene(
            &selected,ANYGM_SCENE_RENDERER_OPENGL,create_fixture),
          "indirect GUI resident fixture loads");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "indirect GUI context adopted");
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  cpu.struct_size=gpu.struct_size=sizeof cpu;
  REQUIRE(anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
          anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK,
          "indirect GUI CPU and resident frames complete");
  if((scale_direction>=0 && !selected.engine->gui_buffer) ||
     (scale_direction>0?selected.engine->gui_space_width>=(int)selected.engine->output_width:
      scale_direction==0?selected.engine->gui_space_width<=(int)selected.engine->output_width:
                         selected.engine->gui_space_width!=(int)selected.engine->output_width))
    fprintf(stderr,"GUI %s geometry gui=%dx%d output=%ux%u canvas=%d\n",
            scale_direction>0?"upscale":scale_direction==0?"reduce":"direct",
            selected.engine->gui_space_width,
            selected.engine->gui_space_height,selected.engine->output_width,
            selected.engine->output_height,selected.engine->canvas_mode);
  REQUIRE((scale_direction>=0?selected.engine->gui_buffer!=NULL:
                              selected.engine->gui_buffer==NULL) &&
          (scale_direction>0?selected.engine->gui_space_width<(int)selected.engine->output_width:
           scale_direction==0?selected.engine->gui_space_width>(int)selected.engine->output_width:
                              selected.engine->gui_space_width==(int)selected.engine->output_width) &&
          (scale_direction>0?selected.engine->gui_space_height<(int)selected.engine->output_height:
           scale_direction==0?selected.engine->gui_space_height>(int)selected.engine->output_height:
                              selected.engine->gui_space_height==(int)selected.engine->output_height),
          "the GUI target has the selected physical geometry");
  gml_gpu_counters(selected.engine->gpu,&counters);
  if(!selected.engine->render.base_scene_resource ||
     counters.scene_operations[GML_RENDER_OP_TEXTURED_QUAD]==0u)
    fprintf(stderr,"GUI %s clear=%llu textured=%llu offered=%llu accepted=%llu "
            "sprite=%llu target=%p gui=%p automatic=%d covered=%d interp=%d resident=%d "
            "underlay=%d source=%dx%d mask=%d\n",
            scale_direction>0?"upscale":scale_direction==0?"reduce":"direct",
            (unsigned long long)counters.scene_operations[GML_RENDER_OP_CLEAR],
            (unsigned long long)counters.scene_operations[GML_RENDER_OP_TEXTURED_QUAD],
            (unsigned long long)counters.scene_batches_offered,
            (unsigned long long)counters.scene_batches_accepted,
            (unsigned long long)counters.scene_families[GML_RENDER_SCENE_SPRITE],
            (void *)selected.engine->render.base_scene_resource,
            (void *)selected.engine->gui_buffer,
            create_fixture==anygm_synthetic_automatic_direct_gui_content_create,
            create_fixture==anygm_synthetic_automatic_alpha_gui_content_create,
            selected.engine->render.interp,selected.engine->render.resident_application_pass,
            selected.engine->render.pending_underlay,selected.engine->render.app_w,
            selected.engine->render.app_h,selected.engine->render.app_presentation_coverage_active);
  REQUIRE(selected.engine->render.base_scene_resource &&
          selected.engine->render.base_scene_resource_pixels==
            (scale_direction>=0?selected.engine->gui_buffer:selected.engine->screen) &&
          counters.scene_operations[GML_RENDER_OP_CLEAR]>0u &&
          counters.scene_operations[GML_RENDER_OP_TEXTURED_QUAD]>0u,
          "GUI uses a counted resident SCREEN clear and sprite");
  if(create_fixture==anygm_synthetic_automatic_direct_gui_content_create ||
    create_fixture==anygm_synthetic_automatic_alpha_gui_content_create ||
    create_fixture==anygm_synthetic_automatic_viewless_gui_content_create ||
    create_fixture==anygm_synthetic_automatic_viewless_alpha_gui_content_create){
    GmlRenderPresentationMetrics presentation={0};
    gml_render_presentation_metrics(&selected.engine->render,&presentation);
    REQUIRE(presentation.application_draw_enabled &&
            counters.scene_families[GML_RENDER_SCENE_SURFACE]>0u &&
            counters.scene_families[GML_RENDER_SCENE_SPRITE]>0u &&
            cpu.pixels &&
            (((const uint32_t *)cpu.pixels)[0]&0xFFFFFFu)==0x00FF00u,
            "visible automatic application underlay precedes the first GPU GUI stretch");
    if(create_fixture==anygm_synthetic_automatic_viewless_gui_content_create){
      GmlVal *views=gml_varmap_get(&selected.engine->vm.globals,"view_enabled");
      REQUIRE(views && views->t==V_REAL && views->d==0.0 &&
              present_view_count(selected.engine,NULL,NULL,NULL)==0,
              "automatic viewless GUI keeps the authored view-disabled state");
    }
  }
  if(scale_direction>1){
    if(selected.engine->screen_pass_frames!=1u || counters.cpu_upload_frames!=0u){
      fprintf(stderr,"upscaled SCREEN passes=%u transport=%u pending=%d materializations=%u "
              "canvas=%d target=%d/%d resident=%d deferred=%d source=%u %dx%d "
              "fallback=%u error=%s\n",selected.engine->screen_pass_frames,
              counters.cpu_upload_frames,
              gml_render_deferred_presentation(&selected.engine->render,NULL),
              selected.engine->frame_materializations,selected.engine->host_canvas_active,
              selected.engine->render.target_id,selected.engine->render.target_sp,
              selected.engine->render.resident_application_pass,
              selected.engine->render.presentation_deferral_enabled,
              selected.engine->render.presentation.resident_image.resource_class,
              selected.engine->render.presentation.source_width,
              selected.engine->render.presentation.source_height,
              selected.engine->host_plan.fallback_reason,
              gml_gpu_last_error(selected.engine->gpu)?gml_gpu_last_error(selected.engine->gpu):"");
    }
    REQUIRE(selected.engine->screen_pass_frames==1u &&
            counters.cpu_upload_frames==0u &&
            gpu.pixels==NULL && (gpu.flags&ANYGM_FRAME_HARDWARE_TARGET),
            "upscaled GUI presents its current SCREEN without complete CPU transport");
    REQUIRE(selected.engine->frame_materializations==0u &&
            gml_render_deferred_presentation(&selected.engine->render,NULL),
            "upscaled GUI retains its completed-picture recipe without CPU raster shadow");
    uint64_t source_version=selected.engine->render.presentation.resident_image.content_version;
    uint32_t before_pixel=selected.engine->screen[0];
    selected.engine->render.presentation.resident_image.content_version++;
    REQUIRE(!engine_materialize_completed_frame(selected.engine) &&
            gml_render_deferred_presentation(&selected.engine->render,NULL) &&
            selected.engine->screen[0]==before_pixel &&
            selected.engine->frame_materializations==0u,
            "stale GUI observation rejects transactionally without consuming the recipe");
    selected.engine->render.presentation.resident_image.content_version=source_version;
    size_t cpu_size=0,gpu_size=0;
    uint8_t *cpu_state=serialize(plain.engine,&cpu_size);
    uint8_t *gpu_state=serialize(selected.engine,&gpu_size);
    int same_state=cpu_state && gpu_state && cpu_size==gpu_size &&
                   !memcmp(cpu_state,gpu_state,cpu_size) &&
                   selected.engine->frame_materializations==1u;
    free(cpu_state); free(gpu_state);
    REQUIRE(same_state,"serialization observes exact upscaled GUI bytes and simulation state");
  }
  if(scale_direction<-1){
    if(selected.engine->screen_pass_frames!=1u || counters.cpu_upload_frames!=0u){
      fprintf(stderr,"direct SCREEN passes=%u transport=%u fallback=%u error=%s\n",
              selected.engine->screen_pass_frames,counters.cpu_upload_frames,
              selected.engine->host_plan.fallback_reason,
              gml_gpu_last_error(selected.engine->gpu)?gml_gpu_last_error(selected.engine->gpu):"");
    }
    REQUIRE(selected.engine->screen_pass_frames==1u &&
            counters.cpu_upload_frames==0u &&
            gpu.pixels==NULL && (gpu.flags&ANYGM_FRAME_HARDWARE_TARGET),
            "direct GUI presents its resident SCREEN without CPU frame transport");
    REQUIRE(selected.engine->frame_materializations==0u,
            "resident SCREEN delivery does not replay a completed CPU plane");
    size_t cpu_size=0,gpu_size=0;
    uint8_t *cpu_state=serialize(plain.engine,&cpu_size);
    uint8_t *gpu_state=serialize(selected.engine,&gpu_size);
    int same_state=cpu_state && gpu_state && cpu_size==gpu_size &&
                   !memcmp(cpu_state,gpu_state,cpu_size) &&
                   selected.engine->frame_materializations==1u;
    free(cpu_state); free(gpu_state);
    REQUIRE(same_state,"serialization observes the exact resident SCREEN and simulation state");
  }
  REQUIRE(engine_materialize_completed_frame(selected.engine),
          "the completed GUI frame remains observable by CPU without another Draw");
  matching=cpu.pixels && selected.engine->screen &&
           cpu.width==selected.engine->output_width &&
           cpu.height==selected.engine->output_height &&
           cpu.pitch==(size_t)cpu.width*sizeof(uint32_t) &&
           !memcmp(cpu.pixels,selected.engine->screen,(size_t)cpu.pitch*cpu.height);
  if(matching){
    unsigned sample=scale_direction>0?24u:scale_direction==0?6u:12u;
    uint32_t colour=((const uint32_t *)cpu.pixels)[(size_t)sample*cpu.width+sample]&0xFFFFFFu;
    matching=colour!=0x0000FFu && colour!=0xFFFFFFu;
  }
  if(!matching && cpu.pixels && selected.engine->screen && cpu.width==gpu.width &&
     cpu.height==gpu.height){
    const uint32_t *a=cpu.pixels,*b=selected.engine->screen;
    for(size_t index=0;index<(size_t)cpu.width*cpu.height;index++) if(a[index]!=b[index]){
      fprintf(stderr,"GUI %s first mismatch %zu,%zu: %08x != %08x; "
              "gui=%dx%d output=%ux%u clear=%llu\n",
              scale_direction>0?"upscale":scale_direction==0?"reduce":"direct",
              index%cpu.width,index/cpu.width,a[index],b[index],
              selected.engine->gui_space_width,selected.engine->gui_space_height,
              gpu.width,gpu.height,
              (unsigned long long)counters.scene_operations[GML_RENDER_OP_CLEAR]);
      break;
    }
  }
  for(int replay=0;matching && replay<2;replay++){
    if(replay) anygm_graphics_context_destroy(selected.engine,1u);
    memset(&cpu,0,sizeof cpu); cpu.struct_size=sizeof cpu;
    memset(&gpu,0,sizeof gpu); gpu.struct_size=sizeof gpu;
    matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
             anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
             engine_materialize_completed_frame(selected.engine) &&
             cpu.pixels && selected.engine->screen &&
             cpu.width==selected.engine->output_width &&
             cpu.height==selected.engine->output_height &&
             cpu.pitch==(size_t)cpu.width*sizeof(uint32_t) &&
             !memcmp(cpu.pixels,selected.engine->screen,(size_t)cpu.pitch*cpu.height);
  }
  session_close(&selected);
  session_close(&plain);
  REQUIRE(matching,"GUI CPU and resident output pixels agree");
  return 1;
}

static int resident_indirect_gui_case(void){
  return resident_indirect_gui_variant(
           anygm_synthetic_resident_indirect_gui_upscale_content_create,2) &&
         resident_indirect_gui_variant(
           anygm_synthetic_indirect_gui_upscale_content_create,1) &&
         resident_indirect_gui_variant(
           anygm_synthetic_indirect_gui_reduce_content_create,0) &&
         resident_indirect_gui_variant(
           anygm_synthetic_direct_gui_sprite_content_create,-1) &&
         resident_indirect_gui_variant(
           anygm_synthetic_resident_direct_gui_content_create,-2) &&
         resident_indirect_gui_variant(
           anygm_synthetic_automatic_direct_gui_content_create,-2) &&
         resident_indirect_gui_variant(
           anygm_synthetic_automatic_viewless_gui_content_create,-2);
}

static int resident_indirect_gui_alpha_fallback_case(void){
  Session plain={0},selected={0};
  AnygmInputFrame input={0};
  AnygmFrameOutput cpu={0},gpu={0};
  GmlGpuCounters counters={0};
  int matching;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(
            &plain,ANYGM_SCENE_RENDERER_CPU,
            anygm_synthetic_indirect_gui_alpha_content_create),
          "partial-alpha GUI CPU fixture loads");
  REQUIRE(session_open_fixture_with_scene(
            &selected,ANYGM_SCENE_RENDERER_OPENGL,
            anygm_synthetic_indirect_gui_alpha_content_create),
          "partial-alpha GUI scene fixture loads");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "partial-alpha GUI context adopted");
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  cpu.struct_size=gpu.struct_size=sizeof cpu;
  REQUIRE(anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
          anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK,
          "partial-alpha GUI CPU and scene frames complete");
  gml_gpu_counters(selected.engine->gpu,&counters);
  if(selected.engine->gui_space_width!=128 ||
     selected.engine->output_width!=64 ||
     counters.scene_operations[GML_RENDER_OP_TEXTURED_QUAD]!=0u)
    fprintf(stderr,"partial-alpha GUI geometry=%dx%d output=%ux%u scale=%.2f,%.2f "
            "clear=%llu texture=%llu\n",
            selected.engine->gui_space_width,selected.engine->gui_space_height,
            selected.engine->output_width,selected.engine->output_height,
            selected.engine->render.gui_scale_x,selected.engine->render.gui_scale_y,
            (unsigned long long)counters.scene_operations[GML_RENDER_OP_CLEAR],
            (unsigned long long)counters.scene_operations[GML_RENDER_OP_TEXTURED_QUAD]);
  REQUIRE(selected.engine->gui_space_width==128 &&
          selected.engine->output_width==64 &&
          selected.engine->render.base_scene_resource_pixels==selected.engine->gui_buffer &&
          counters.scene_operations[GML_RENDER_OP_CLEAR]>0u &&
          counters.scene_operations[GML_RENDER_OP_TEXTURED_QUAD]==0u,
          "partial-alpha GUI sprite retains the CPU fallback after a GPU clear");
  matching=cpu.pixels && selected.engine->screen &&
           cpu.width==selected.engine->output_width &&
           cpu.height==selected.engine->output_height &&
           cpu.pitch==(size_t)cpu.width*sizeof(uint32_t) &&
           !memcmp(cpu.pixels,selected.engine->screen,(size_t)cpu.pitch*cpu.height);
  if(matching){
    uint32_t colour=((const uint32_t *)cpu.pixels)[6u*cpu.width+6u]&0xFFFFFFu;
    matching=colour!=0x0000FFu && colour!=0xFFFFFFu;
  }
  session_close(&selected);
  session_close(&plain);
  REQUIRE(matching,"partial-alpha GUI CPU suffix preserves complete output pixels");
  return 1;
}

static int resident_multiview_cpu_seed_variant(
    int (*create_fixture)(AnygmSyntheticContent *),int offset_port,int expect_gpu_sprite){
  Session plain={0},selected={0};
  AnygmInputFrame input={0};
  AnygmFrameOutput cpu={0},gpu={0};
  GmlGpuCounters first={0},second={0};
  int matching=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(
            &plain,ANYGM_SCENE_RENDERER_CPU,
            create_fixture),
          "two-view CPU fixture loads");
  REQUIRE(session_open_fixture_with_scene(
            &selected,ANYGM_SCENE_RENDERER_OPENGL,
            create_fixture),
          "two-view resident fixture loads");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "two-view context adopted");
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  for(int frame=0;matching && frame<3;frame++){
    if(frame==2) anygm_graphics_context_destroy(selected.engine,1u);
    memset(&cpu,0,sizeof cpu); cpu.struct_size=sizeof cpu;
    memset(&gpu,0,sizeof gpu); gpu.struct_size=sizeof gpu;
    matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
             anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
             cpu.pixels && selected.engine->screen && cpu.width==64u &&
             cpu.height==48u && cpu.pitch==64u*sizeof(uint32_t) &&
             !memcmp(cpu.pixels,selected.engine->screen,(size_t)cpu.pitch*cpu.height) &&
             plain.engine->vm.room_index==selected.engine->vm.room_index;
    if(matching && frame==0){
      const uint32_t *pixels=(const uint32_t *)cpu.pixels;
      matching=(pixels[14u*64u+(offset_port?15u:12u)]&0xFFFFFFu)!=0x996633u &&
               (pixels[18u*64u+50u]&0xFFFFFFu)!=0x996633u;
      gml_gpu_counters(selected.engine->gpu,&first);
      matching=matching && first.scene_operations[GML_RENDER_OP_CLEAR]>=2u &&
               (expect_gpu_sprite?
                  first.scene_families[GML_RENDER_SCENE_SPRITE]>=2u:
                  first.scene_families[GML_RENDER_SCENE_SPRITE]==0u);
    }
    if(matching && frame==1){
      gml_gpu_counters(selected.engine->gpu,&second);
      matching=selected.engine->vm.room_index==1 &&
               (expect_gpu_sprite?
                  second.scene_families[GML_RENDER_SCENE_SPRITE]>=
                    first.scene_families[GML_RENDER_SCENE_SPRITE]+2u:
                  second.scene_families[GML_RENDER_SCENE_SPRITE]==0u) &&
               second.scene_operations[GML_RENDER_OP_CLEAR]==
                 first.scene_operations[GML_RENDER_OP_CLEAR];
    }
  }
  if(!matching){
    GmlGpuCounters observed={0};
    gml_gpu_counters(selected.engine->gpu,&observed);
    fprintf(stderr,"two-view CPU seed mismatch: room=%d first clear=%llu sprite=%llu "
            "second clear=%llu sprite=%llu observed clear=%llu sprite=%llu "
            "cpu=%ux%u gpu=%ux%u\n",
            selected.engine?selected.engine->vm.room_index:-1,
            (unsigned long long)first.scene_operations[GML_RENDER_OP_CLEAR],
            (unsigned long long)first.scene_families[GML_RENDER_SCENE_SPRITE],
            (unsigned long long)second.scene_operations[GML_RENDER_OP_CLEAR],
            (unsigned long long)second.scene_families[GML_RENDER_SCENE_SPRITE],
            (unsigned long long)observed.scene_operations[GML_RENDER_OP_CLEAR],
            (unsigned long long)observed.scene_families[GML_RENDER_SCENE_SPRITE],
            cpu.width,cpu.height,gpu.width,gpu.height);
    if(cpu.pixels && selected.engine->screen && cpu.width==64u && cpu.height==48u){
      const uint32_t *a=(const uint32_t *)cpu.pixels,*b=selected.engine->screen;
      for(size_t at=0;at<64u*48u;at++) if(a[at]!=b[at]){
        fprintf(stderr,"two-view first pixel mismatch %zu,%zu: %08x != %08x\n",
                at%64u,at/64u,a[at],b[at]);
        break;
      }
    }
  }
  session_close(&selected);
  session_close(&plain);
  REQUIRE(matching,"two-view retained CPU seed, GPU sprite and context loss agree");
  return 1;
}

static int resident_multiview_cpu_seed_case(void){
  return resident_multiview_cpu_seed_variant(
           anygm_synthetic_multiview_gpu_content_create,0,1) &&
         resident_multiview_cpu_seed_variant(
           anygm_synthetic_multiview_offset_gpu_content_create,1,1) &&
         resident_multiview_cpu_seed_variant(
           anygm_synthetic_multiview_fractional_scale_content_create,1,0);
}

static int resident_multiview_step_target_variant(
    int (*create_fixture)(AnygmSyntheticContent *),int nested){
  Session plain={0},selected={0};
  AnygmInputFrame input={0};
  AnygmFrameOutput cpu={0},gpu={0};
  int matching=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(
            &plain,ANYGM_SCENE_RENDERER_CPU,
            create_fixture),
          "Step-target CPU fixture loads");
  REQUIRE(session_open_fixture_with_scene(
            &selected,ANYGM_SCENE_RENDERER_OPENGL,
            create_fixture),
          "Step-target scene fixture loads");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "Step-target context adopted");
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  for(int frame=0;matching && frame<3;frame++){
    if(frame==2) anygm_graphics_context_destroy(selected.engine,1u);
    memset(&cpu,0,sizeof cpu); cpu.struct_size=sizeof cpu;
    memset(&gpu,0,sizeof gpu); gpu.struct_size=sizeof gpu;
    matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
             anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK;
    if(!matching) break;
    int cpu_id=(int)gml_global_num(&plain.engine->vm,"fixture_surface");
    int gpu_id=(int)gml_global_num(&selected.engine->vm,"fixture_surface");
    int cw=0,ch=0,gw=0,gh=0;
    const uint32_t *cpu_surface=gml_surface_pixels_read(
      &plain.engine->render,cpu_id,&cw,&ch);
    const uint32_t *gpu_surface=gml_surface_pixels_read(
      &selected.engine->render,gpu_id,&gw,&gh);
    int cpu_inner_id=nested?(int)gml_global_num(&plain.engine->vm,"fixture_inner"):-1;
    int gpu_inner_id=nested?(int)gml_global_num(&selected.engine->vm,"fixture_inner"):-1;
    int ciw=0,cih=0,giw=0,gih=0;
    const uint32_t *cpu_inner=nested?gml_surface_pixels_read(
      &plain.engine->render,cpu_inner_id,&ciw,&cih):NULL;
    const uint32_t *gpu_inner=nested?gml_surface_pixels_read(
      &selected.engine->render,gpu_inner_id,&giw,&gih):NULL;
    uint32_t expected_outer=nested?0xFFFF0000u:0xFF0000FFu;
    matching=cpu.pixels && selected.engine->screen && cpu.width==64u &&
             cpu.height==48u && cpu.pitch==64u*sizeof(uint32_t) &&
             !memcmp(cpu.pixels,selected.engine->screen,(size_t)cpu.pitch*cpu.height) &&
             cpu_surface && gpu_surface && cw==8 && ch==8 && gw==8 && gh==8 &&
             cpu_surface[0]==expected_outer &&
             !memcmp(cpu_surface,gpu_surface,64u*sizeof(uint32_t)) &&
             (((const uint32_t *)cpu.pixels)[0]&0xFFFFFFu)==
               (expected_outer&0xFFFFFFu) &&
             (!nested || (cpu_surface[63]!=0u && cpu_surface[63]!=expected_outer &&
               cpu_inner && gpu_inner && ciw==8 && cih==8 && giw==8 && gih==8 &&
               cpu_inner[0]==0xFF0000FFu &&
               !memcmp(cpu_inner,gpu_inner,64u*sizeof(uint32_t)) &&
               (((const uint32_t *)cpu.pixels)[16]&0xFFFFFFu)==0x000000FFu));
    if(!matching){
      GmlGpuCounters counts={0};
      gml_gpu_counters(selected.engine->gpu,&counts);
      fprintf(stderr,"Step-target nested=%d frame=%d ids=%d/%d extent=%dx%d/%dx%d "
              "surface=%08x/%08x tail=%08x inner=%08x/%08x output=%08x/%08x "
              "output16=%08x sprite=%llu clear=%llu\n",
              nested,frame,cpu_id,gpu_id,cw,ch,gw,gh,
              cpu_surface?cpu_surface[0]:0u,gpu_surface?gpu_surface[0]:0u,
              cpu_surface?cpu_surface[63]:0u,
              cpu_inner?cpu_inner[0]:0u,gpu_inner?gpu_inner[0]:0u,
              cpu.pixels?((const uint32_t *)cpu.pixels)[0]:0u,
              selected.engine->screen?selected.engine->screen[0]:0u,
              cpu.pixels?((const uint32_t *)cpu.pixels)[16]:0u,
              (unsigned long long)counts.scene_families[GML_RENDER_SCENE_SPRITE],
              (unsigned long long)counts.scene_operations[GML_RENDER_OP_CLEAR]);
    }
    if(matching && frame==0){
      GmlGpuCounters counts={0};
      gml_gpu_counters(selected.engine->gpu,&counts);
      matching=counts.scene_families[GML_RENDER_SCENE_SPRITE]>=1u &&
               counts.scene_operations[GML_RENDER_OP_CLEAR]>=1u;
    }
  }
  session_close(&selected);
  session_close(&plain);
  REQUIRE(matching,"Step-retained surface survives first view and same-frame GUI read");
  return 1;
}

static int resident_multiview_step_target_case(void){
  return resident_multiview_step_target_variant(
           anygm_synthetic_multiview_step_target_content_create,0) &&
         resident_multiview_step_target_variant(
           anygm_synthetic_multiview_nested_target_content_create,1);
}

static int resident_multiview_assigned_surface_restore_case(void){
  Session plain={0},selected={0};
  AnygmInputFrame input={0};
  AnygmFrameOutput cpu={0},gpu={0};
  uint8_t *saved=NULL,*selected_saved=NULL;
  size_t saved_size=0,selected_size=0;
  int matching=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(
            &plain,ANYGM_SCENE_RENDERER_CPU,
            anygm_synthetic_multiview_assigned_surface_content_create),
          "assigned-view CPU fixture loads");
  REQUIRE(session_open_fixture_with_scene(
            &selected,ANYGM_SCENE_RENDERER_OPENGL,
            anygm_synthetic_multiview_assigned_surface_content_create),
          "assigned-view scene fixture loads");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "assigned-view context adopted");
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  for(int frame=0;matching && frame<3;frame++){
    if(frame==2) anygm_graphics_context_destroy(selected.engine,1u);
    memset(&cpu,0,sizeof cpu); cpu.struct_size=sizeof cpu;
    memset(&gpu,0,sizeof gpu); gpu.struct_size=sizeof gpu;
    matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
             anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK;
    if(!matching) break;
    int outer_id=(int)gml_global_num(&plain.engine->vm,"fixture_surface");
    int inner_id=(int)gml_global_num(&plain.engine->vm,"fixture_inner");
    int selected_outer=(int)gml_global_num(&selected.engine->vm,"fixture_surface");
    int selected_inner=(int)gml_global_num(&selected.engine->vm,"fixture_inner");
    int ow=0,oh=0,iw=0,ih=0,sw=0,sh=0,tw=0,th=0;
    const uint32_t *outer=gml_surface_pixels_read(
      &plain.engine->render,outer_id,&ow,&oh);
    const uint32_t *inner=gml_surface_pixels_read(
      &plain.engine->render,inner_id,&iw,&ih);
    const uint32_t *scene_outer=gml_surface_pixels_read(
      &selected.engine->render,selected_outer,&sw,&sh);
    const uint32_t *scene_inner=gml_surface_pixels_read(
      &selected.engine->render,selected_inner,&tw,&th);
    matching=cpu.pixels && selected.engine->screen && cpu.width==64u &&
             cpu.height==48u && cpu.pitch==64u*sizeof(uint32_t) &&
             !memcmp(cpu.pixels,selected.engine->screen,(size_t)cpu.pitch*cpu.height) &&
             outer && inner && scene_outer && scene_inner &&
             ow==8 && oh==8 && iw==8 && ih==8 &&
             sw==8 && sh==8 && tw==8 && th==8 &&
             outer[0]==0xFF0000FFu && inner[0]==0xFF008000u &&
             gml_global_num(&plain.engine->vm,"fixture_seen")==2.0*(frame+1) &&
             !memcmp(outer,scene_outer,64u*sizeof(uint32_t)) &&
             !memcmp(inner,scene_inner,64u*sizeof(uint32_t)) &&
             (((const uint32_t *)cpu.pixels)[0]&0xFFFFFFu)==0x000000FFu &&
             (((const uint32_t *)cpu.pixels)[16]&0xFFFFFFu)==0x00008000u;
    if(!matching)
      fprintf(stderr,"assigned-view frame=%d ids=%d/%d bindings=%.0f/%.0f "
              "surfaces=%08x/%08x scene=%08x/%08x "
              "world=%08x scratch=%08x room=%d vmframe=%ld seen=%.0f "
              "output=%08x/%08x\n",
              frame,outer_id,inner_id,
              gml_global_arr(&plain.engine->vm,"view_surface_id",0),
              gml_global_arr(&plain.engine->vm,"view_surface_id",1),
              outer?outer[0]:0u,inner?inner[0]:0u,
              scene_outer?scene_outer[0]:0u,scene_inner?scene_inner[0]:0u,
              plain.engine->fb?plain.engine->fb[0]:0u,
              plain.engine->app_crop?plain.engine->app_crop[0]:0u,
              plain.engine->vm.room_index,plain.engine->vm.frame,
              gml_global_num(&plain.engine->vm,"fixture_seen"),
              cpu.pixels?((const uint32_t *)cpu.pixels)[0]:0u,
              cpu.pixels?((const uint32_t *)cpu.pixels)[16]:0u);
    if(matching && frame==0){
      GmlGpuCounters counts={0};
      gml_gpu_counters(selected.engine->gpu,&counts);
      matching=counts.scene_families[GML_RENDER_SCENE_SPRITE]>=2u;
      saved=matching?serialize(plain.engine,&saved_size):NULL;
      selected_saved=matching?serialize(selected.engine,&selected_size):NULL;
      matching=matching && saved && selected_saved && saved_size>0 &&
               saved_size==selected_size &&
               !memcmp(saved,selected_saved,saved_size) &&
               anygm_state_load(plain.engine,saved,saved_size)==ANYGM_OK &&
               anygm_state_load(selected.engine,saved,saved_size)==ANYGM_OK;
      if(matching){
        const uint32_t *derived=gml_surface_pixels_read(
          &plain.engine->render,outer_id,NULL,NULL);
        const uint32_t *preserved=gml_surface_pixels_read(
          &plain.engine->render,inner_id,NULL,NULL);
        const uint32_t *selected_derived=gml_surface_pixels_read(
          &selected.engine->render,selected_outer,NULL,NULL);
        const uint32_t *selected_preserved=gml_surface_pixels_read(
          &selected.engine->render,selected_inner,NULL,NULL);
        matching=derived && preserved && derived[0]==0u &&
                 preserved[0]==0xFF008000u &&
                 selected_derived && selected_preserved &&
                 selected_derived[0]==0u &&
                 selected_preserved[0]==0xFF008000u;
      }
      if(matching){
        AnygmFrameOutput restored_cpu={0},restored_gpu={0};
        restored_cpu.struct_size=restored_gpu.struct_size=sizeof restored_cpu;
        matching=anygm_run_frame(plain.engine,&input,&restored_cpu)==ANYGM_OK &&
                 anygm_run_frame(selected.engine,&input,&restored_gpu)==ANYGM_OK &&
                 restored_cpu.pixels && selected.engine->screen &&
                 restored_cpu.width==64u && restored_cpu.height==48u &&
                 !memcmp(restored_cpu.pixels,selected.engine->screen,
                         64u*48u*sizeof(uint32_t)) &&
                 plain.engine->vm.frame==1 &&
                 gml_global_num(&plain.engine->vm,"fixture_seen")==2.0 &&
                 (((const uint32_t *)restored_cpu.pixels)[0]&0xFFFFFFu)==0x000000FFu &&
                 (((const uint32_t *)restored_cpu.pixels)[16]&0xFFFFFFu)==0x00008000u;
      }
    }
  }
  free(selected_saved);
  free(saved);
  session_close(&selected);
  session_close(&plain);
  REQUIRE(matching,"assigned views and first-restored-frame GUI reader stay current");
  return 1;
}

static int resident_native_assigned_view_variant(int draw_enabled,int native_surface){
  Session plain={0},selected={0};
  AnygmInputFrame input={0};
  uint8_t *saved=NULL,*selected_saved=NULL;
  size_t saved_size=0,selected_size=0;
  int matching=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(
            &plain,ANYGM_SCENE_RENDERER_CPU,
            anygm_synthetic_multiview_assigned_surface_content_create),
          "native assigned-view CPU fixture loads");
  REQUIRE(session_open_fixture_with_scene(
            &selected,ANYGM_SCENE_RENDERER_OPENGL,
            anygm_synthetic_multiview_assigned_surface_content_create),
          "native assigned-view scene fixture loads");
  Session *sessions[2]={&plain,&selected};
  for(int index=0;index<2;index++){
    AnygmEngine *engine=sessions[index]->engine;
    int surface=(int)gml_global_num(&engine->vm,"fixture_surface");
    gml_surface_resize(&engine->render,surface,native_surface?64:32,native_surface?48:24);
    gml_render_application_surface_set_draw_enabled(&engine->render,draw_enabled);
    gml_set_global_arr(&engine->vm,"view_visible",1,0);
    gml_set_global_arr(&engine->vm,"view_wview",0,64);
    gml_set_global_arr(&engine->vm,"view_hview",0,48);
    gml_set_global_arr(&engine->vm,"view_wport",0,64);
    gml_set_global_arr(&engine->vm,"view_hport",0,48);
    gml_set_global_arr(&engine->vm,"view_xview",0,2);
    gml_set_global_arr(&engine->vm,"view_yview",0,3);
    int camera=(int)gml_global_arr(&engine->vm,"view_camera",0);
    if(camera>=0 && camera<GML_CAMERA_LIMIT &&
       gml_global_arr(&engine->vm,"__gml_camera_live",camera)>=0.5){
      gml_set_global_arr(&engine->vm,"__gml_camera_x",camera,2);
      gml_set_global_arr(&engine->vm,"__gml_camera_y",camera,3);
      gml_set_global_arr(&engine->vm,"__gml_camera_w",camera,64);
      gml_set_global_arr(&engine->vm,"__gml_camera_h",camera,48);
    }
  }
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "native assigned-view context adopts");
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  for(int frame=0;matching && frame<4;frame++){
    if(frame==2) anygm_graphics_context_destroy(selected.engine,1u);
    if(frame==3)
      matching=graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK;
    AnygmFrameOutput cpu={0},gpu={0};
    cpu.struct_size=gpu.struct_size=sizeof cpu;
    matching=matching && anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
             anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
             cpu.pixels && selected.engine->screen && cpu.width==64u && cpu.height==48u &&
             gpu.width==cpu.width && gpu.height==cpu.height &&
             !memcmp(cpu.pixels,selected.engine->screen,64u*48u*sizeof(uint32_t));
    int surface=(int)gml_global_num(&plain.engine->vm,"fixture_surface");
    int selected_surface=(int)gml_global_num(&selected.engine->vm,"fixture_surface");
    int width=0,height=0,selected_width=0,selected_height=0;
    const uint32_t *pixels=gml_surface_pixels_read(
      &plain.engine->render,surface,&width,&height);
    const uint32_t *scene_pixels=gml_surface_pixels_read(
      &selected.engine->render,selected_surface,&selected_width,&selected_height);
    matching=matching && pixels && scene_pixels && width==(native_surface?64:32) &&
             height==(native_surface?48:24) && selected_width==width && selected_height==height &&
             !memcmp(pixels,scene_pixels,(size_t)width*height*sizeof(uint32_t)) &&
             gml_global_num(&plain.engine->vm,"fixture_seen")==frame+1;
    if(matching && native_surface)
      matching=pixels[0]==0xFF0000FFu && pixels[9u*64u+6u]==0xFF2030C0u;
    if(matching && frame==0){
      GmlGpuCounters counts={0};
      gml_gpu_counters(selected.engine->gpu,&counts);
      unsigned long long sprites=(unsigned long long)counts.scene_families[GML_RENDER_SCENE_SPRITE];
      matching=native_surface?sprites>=1u:sprites==0u;
      if(!matching)
        fprintf(stderr,"native assigned view: draw=%d native=%d sprites=%llu expected=%s\n",
                draw_enabled,native_surface,sprites,native_surface?"resident":"CPU fallback");
      saved=matching?serialize(plain.engine,&saved_size):NULL;
      selected_saved=matching?serialize(selected.engine,&selected_size):NULL;
      matching=matching && saved && selected_saved && saved_size==selected_size &&
               !memcmp(saved,selected_saved,saved_size) &&
               anygm_state_load(plain.engine,selected_saved,selected_size)==ANYGM_OK &&
               anygm_state_load(selected.engine,saved,saved_size)==ANYGM_OK;
      if(matching){
        AnygmFrameOutput restored_cpu={0},restored_gpu={0};
        restored_cpu.struct_size=restored_gpu.struct_size=sizeof restored_cpu;
        matching=anygm_run_frame(plain.engine,&input,&restored_cpu)==ANYGM_OK &&
                 anygm_run_frame(selected.engine,&input,&restored_gpu)==ANYGM_OK &&
                 restored_cpu.pixels && selected.engine->screen &&
                 !memcmp(restored_cpu.pixels,selected.engine->screen,64u*48u*sizeof(uint32_t)) &&
                 plain.engine->vm.frame==1 && selected.engine->vm.frame==1 &&
                 gml_global_num(&plain.engine->vm,"fixture_seen")==1;
      }
    }
  }
  free(selected_saved); free(saved);
  session_close(&selected); session_close(&plain);
  REQUIRE(matching,"native assigned view keeps camera, output and cross-restored state");
  return 1;
}

static int resident_native_assigned_view_case(void){
  return resident_native_assigned_view_variant(1,1) &&
         resident_native_assigned_view_variant(0,1) &&
         resident_native_assigned_view_variant(1,0);
}

static int presentation_geometry_variant(
    const char *name,int (*create_fixture)(AnygmSyntheticContent *),
    unsigned logical_raster,unsigned aspect_mode,unsigned monitor_width,
    unsigned monitor_height,int variant){
  Session plain={0},selected={0};
  AnygmInputFrame input={0};
  AnygmConfigDelta presentation={0};
  int matching=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(&plain,ANYGM_SCENE_RENDERER_CPU,create_fixture),
          "geometry CPU fixture loads");
  REQUIRE(session_open_fixture_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL,create_fixture),
          "geometry scene fixture loads");
  presentation.struct_size=sizeof presentation;
  presentation.values.struct_size=sizeof presentation.values;
  presentation.fields=ANYGM_CONFIG_PRESENT_LOGICAL_RASTER|ANYGM_CONFIG_ASPECT_MODE;
  presentation.values.present_logical_raster=logical_raster;
  presentation.values.aspect_mode=aspect_mode;
  if(monitor_width && monitor_height){
    presentation.fields|=ANYGM_CONFIG_MONITOR_WIDTH|ANYGM_CONFIG_MONITOR_HEIGHT;
    presentation.values.monitor_width=monitor_width;
    presentation.values.monitor_height=monitor_height;
  }
  REQUIRE(anygm_set_config(plain.engine,&presentation)==ANYGM_OK &&
          anygm_set_config(selected.engine,&presentation)==ANYGM_OK,
          "geometry policy selected");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "geometry context adopted");
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  for(int frame=0;matching && frame<3;frame++){
    AnygmFrameOutput cpu={0},gpu={0};
    const uint32_t *selected_host=NULL;
    unsigned selected_host_width=0,selected_host_height=0;
    cpu.struct_size=gpu.struct_size=sizeof cpu;
    if(frame==2) anygm_graphics_context_destroy(selected.engine,1u);
    matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
             anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
             cpu.pixels && plain.engine->screen && selected.engine->screen &&
             plain.engine->output_width==selected.engine->output_width &&
             plain.engine->output_height==selected.engine->output_height &&
             !memcmp(plain.engine->screen,selected.engine->screen,
                     (size_t)plain.engine->output_width*plain.engine->output_height*
                     sizeof(uint32_t)) &&
             resolve_host_frame(selected.engine,&selected_host,
                                &selected_host_width,&selected_host_height) &&
             selected_host && cpu.width==gpu.width && cpu.height==gpu.height &&
             cpu.width==selected_host_width && cpu.height==selected_host_height &&
             cpu.pitch==(size_t)cpu.width*sizeof(uint32_t) &&
             !memcmp(cpu.pixels,selected_host,(size_t)cpu.pitch*cpu.height);
    if(!matching){
      size_t first_diff=(size_t)-1;
      if(cpu.pixels && selected_host && cpu.width==gpu.width &&
         cpu.height==gpu.height)
        for(size_t pixel=0;pixel<(size_t)cpu.width*cpu.height;pixel++)
          if(((const uint32_t *)cpu.pixels)[pixel]!=selected_host[pixel]){
            first_diff=pixel;
            break;
          }
      fprintf(stderr,"geometry %s frame=%d mismatch cpu=%ux%u/%zu gpu=%ux%u "
              "pixel=%zu cpu=%08x gpu=%08x\n",name,frame,cpu.width,cpu.height,
              cpu.pitch,gpu.width,gpu.height,first_diff,
              first_diff!=(size_t)-1?((const uint32_t *)cpu.pixels)[first_diff]:0u,
              first_diff!=(size_t)-1?selected_host[first_diff]:0u);
    }
    if(matching){
      const uint32_t *pixels=cpu.pixels;
      size_t count=(size_t)cpu.width*cpu.height;
      unsigned lit=0,distinct=0;
      uint32_t first=pixels[0]&0xFFFFFFu;
      for(size_t i=0;i<count;i++){
        uint32_t colour=pixels[i]&0xFFFFFFu;
        lit+=colour!=0;
        distinct|=colour!=first;
      }
      matching=lit>0 && distinct;
    }
    if(frame==0){
      GmlGpuCounters counters={0};
      gml_gpu_counters(selected.engine->gpu,&counters);
      int geometry_ok=variant==0?
        selected.engine->width==64 && selected.engine->height==48 &&
        selected.engine->output_width==64 && selected.engine->output_height==64 &&
        cpu.width==128 && cpu.height==128 &&
        selected.engine->gui_space_width==64 && selected.engine->gui_space_height==32 &&
        selected.engine->gui_offset_x==0 && selected.engine->gui_offset_y==16 &&
        selected.engine->canvas_mode && !selected.engine->aspect_force_active &&
        !selected.engine->screen_stage_window_raster &&
        counters.scene_batches_accepted==counters.scene_batches_offered &&
        counters.scene_batches_accepted>=5u &&
        counters.scene_families[GML_RENDER_SCENE_SPRITE]>=2u &&
        counters.scene_readbacks==0u && counters.cpu_upload_frames==0u:
        variant==1?
        selected.engine->width==64 && selected.engine->height==48 &&
        selected.engine->output_width==128 && selected.engine->output_height==128 &&
        cpu.width==128 && cpu.height==128 &&
        selected.engine->gui_space_width==128 && selected.engine->gui_space_height==128 &&
        !selected.engine->canvas_mode && !selected.engine->aspect_force_active &&
        selected.engine->screen_stage_window_raster &&
        counters.scene_batches_accepted==counters.scene_batches_offered &&
        counters.scene_batches_accepted>=3u &&
        counters.scene_families[GML_RENDER_SCENE_SPRITE]>=1u &&
        counters.scene_readbacks==0u && counters.cpu_upload_frames==1u:
        selected.engine->width==112 && selected.engine->height==48 &&
        selected.engine->output_width==112 && selected.engine->output_height==48 &&
        cpu.width==112 && cpu.height==48 &&
        selected.engine->gui_space_width==112 && selected.engine->gui_space_height==48 &&
        selected.engine->gui_offset_x==24 && selected.engine->gui_offset_y==0 &&
        !selected.engine->canvas_mode && selected.engine->aspect_force_active &&
        !selected.engine->screen_stage_window_raster &&
        counters.scene_batches_offered==0u &&
        counters.scene_batches_accepted==0u &&
        counters.scene_families[GML_RENDER_SCENE_SPRITE]==0u &&
        counters.cpu_upload_frames==1u;
      if(!geometry_ok)
        fprintf(stderr,"geometry %s world=%ux%u output=%ux%u delivered=%ux%u "
                "gui=%dx%d offset=%d,%d canvas=%d aspect=%d window=%d "
                "host=%ux%u scene=%llu/%llu sprite=%llu uploads=%llu "
                "readbacks=%llu transport=%llu\n",name,
                selected.engine->width,selected.engine->height,
                selected.engine->output_width,selected.engine->output_height,
                cpu.width,cpu.height,
                selected.engine->gui_space_width,selected.engine->gui_space_height,
                selected.engine->gui_offset_x,selected.engine->gui_offset_y,
                selected.engine->canvas_mode,selected.engine->aspect_force_active,
                selected.engine->screen_stage_window_raster,
                selected.engine->host_output_width,selected.engine->host_output_height,
                (unsigned long long)counters.scene_batches_accepted,
                (unsigned long long)counters.scene_batches_offered,
                (unsigned long long)counters.scene_families[GML_RENDER_SCENE_SPRITE],
                (unsigned long long)counters.scene_uploads,
                (unsigned long long)counters.scene_readbacks,
                (unsigned long long)counters.cpu_upload_frames);
      matching=matching && geometry_ok;
    }
  }
  session_close(&selected);
  session_close(&plain);
  REQUIRE(matching,"geometry CPU and selected images remain visibly identical");
  return 1;
}

static int presentation_geometry_case(void){
  return presentation_geometry_variant(
           "canvas",anygm_synthetic_multiview_canvas_content_create,
           0u,GMC_ASPECT_FORCE_NONE,128u,128u,0) &&
         presentation_geometry_variant(
           "window",anygm_synthetic_window_raster_content_create,
           0u,GMC_ASPECT_FORCE_NONE,128u,128u,1) &&
         presentation_geometry_variant(
           "centered",anygm_synthetic_direct_gui_sprite_content_create,
           1u,GMC_ASPECT_FORCE_21_9,0u,0u,2);
}

static int forced_owned_scene_variant(int narrow_room){
  Session plain={0},selected={0};
  AnygmInputFrame input={0};
  AnygmConfigDelta presentation={0};
  int matching=1;
  int (*create_fixture)(AnygmSyntheticContent *)=narrow_room?
    anygm_synthetic_forced_narrow_content_create:anygm_synthetic_forced_owned_content_create;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(&plain,ANYGM_SCENE_RENDERER_CPU,
            create_fixture) &&
          session_open_fixture_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL,
            create_fixture),
          "forced owned sessions load");
  presentation.struct_size=sizeof presentation;
  presentation.values.struct_size=sizeof presentation.values;
  presentation.fields=ANYGM_CONFIG_PRESENT_LOGICAL_RASTER|ANYGM_CONFIG_ASPECT_MODE;
  presentation.values.present_logical_raster=1u;
  presentation.values.aspect_mode=GMC_ASPECT_FORCE_21_9;
  REQUIRE(anygm_set_config(plain.engine,&presentation)==ANYGM_OK &&
          anygm_set_config(selected.engine,&presentation)==ANYGM_OK,
          "forced owned geometry selected");
  const char *overrides[]={"?aspect @compositor_fullwidth=1",
                          "?aspect @wide_gameplay_view=1"};
  for(unsigned slot=0;slot<2u;slot++)
    REQUIRE(anygm_set_runtime_override(plain.engine,slot,1u,overrides[slot])==ANYGM_OK &&
            anygm_set_runtime_override(selected.engine,slot,1u,overrides[slot])==ANYGM_OK,
            "forced owned declaration selected");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "forced owned context selected");
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  for(int frame=0;matching && frame<3;frame++){
    AnygmFrameOutput cpu={0},gpu={0};
    GmlGpuCounters counters={0};
    const uint32_t *pixels=NULL;
    unsigned width=0,height=0;
    cpu.struct_size=gpu.struct_size=sizeof cpu;
    matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
             anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
             plain.engine->aspect_force_active && selected.engine->aspect_force_active &&
             selected.engine->width>selected.engine->base_width &&
             cpu.width==gpu.width && cpu.height==gpu.height;
    gml_gpu_counters(selected.engine->gpu,&counters);
    matching=matching && (narrow_room?counters.scene_batches_offered==0u:
                                    counters.scene_batches_accepted>0u);
    matching=matching && resolve_host_frame(selected.engine,&pixels,&width,&height) &&
             pixels && cpu.pixels && width==cpu.width && height==cpu.height &&
             !memcmp(cpu.pixels,pixels,(size_t)cpu.pitch*cpu.height);
    if(!matching)
      fprintf(stderr,"forced owned narrow=%d frame=%d scene=%llu/%llu cpu=%ux%u gpu=%ux%u\n",
              narrow_room,frame,(unsigned long long)counters.scene_batches_offered,
              (unsigned long long)counters.scene_batches_accepted,
              cpu.width,cpu.height,gpu.width,gpu.height);
  }
  session_close(&selected); session_close(&plain);
  REQUIRE(matching,"forced owned scene matches CPU and preserves narrow-room fallback");
  return 1;
}

static int forced_owned_scene_case(void){
  return forced_owned_scene_variant(0) && forced_owned_scene_variant(1);
}

static int presentation_filter_variant(
    const char *name,int (*create_fixture)(AnygmSyntheticContent *),int variant){
  Session plain={0},selected={0};
  AnygmInputFrame input={0};
  AnygmConfigDelta presentation={0};
  int matching=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(&plain,ANYGM_SCENE_RENDERER_CPU,create_fixture),
          "filter CPU fixture loads");
  REQUIRE(session_open_fixture_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL,create_fixture),
          "filter scene fixture loads");
  presentation.struct_size=sizeof presentation;
  presentation.values.struct_size=sizeof presentation.values;
  presentation.fields=ANYGM_CONFIG_PRESENT_LOGICAL_RASTER|ANYGM_CONFIG_ADJUST_CRT_TV;
  presentation.values.present_logical_raster=variant!=2;
  presentation.values.adjust_crt_tv=variant!=2;
  if(variant==2){
    presentation.fields|=ANYGM_CONFIG_MONITOR_WIDTH|ANYGM_CONFIG_MONITOR_HEIGHT;
    presentation.values.monitor_width=64u;
    presentation.values.monitor_height=64u;
  }
  REQUIRE(anygm_set_config(plain.engine,&presentation)==ANYGM_OK &&
          anygm_set_config(selected.engine,&presentation)==ANYGM_OK,
          "filter presentation policy selected");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "filter context adopted");
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  for(int frame=0;matching && frame<3;frame++){
    AnygmFrameOutput cpu={0},gpu={0};
    const uint32_t *selected_host=NULL;
    unsigned selected_host_width=0,selected_host_height=0;
    cpu.struct_size=gpu.struct_size=sizeof cpu;
    if(frame==2) anygm_graphics_context_destroy(selected.engine,1u);
    matching=anygm_run_frame(plain.engine,&input,&cpu)==ANYGM_OK &&
             anygm_run_frame(selected.engine,&input,&gpu)==ANYGM_OK &&
             cpu.pixels && plain.engine->screen && selected.engine->screen &&
             plain.engine->output_width==selected.engine->output_width &&
             plain.engine->output_height==selected.engine->output_height &&
             !memcmp(plain.engine->screen,selected.engine->screen,
                     (size_t)plain.engine->output_width*plain.engine->output_height*
                     sizeof(uint32_t)) &&
             resolve_host_frame(selected.engine,&selected_host,
                                &selected_host_width,&selected_host_height) &&
             selected_host && cpu.width==gpu.width && cpu.height==gpu.height &&
             cpu.width==selected_host_width && cpu.height==selected_host_height &&
             cpu.pitch==(size_t)cpu.width*sizeof(uint32_t) &&
             !memcmp(cpu.pixels,selected_host,(size_t)cpu.pitch*cpu.height);
    if(frame==0){
      GmlGpuCounters counters={0};
      gml_gpu_counters(selected.engine->gpu,&counters);
      unsigned source_width=variant==0?64u:variant==1?400u:128u;
      unsigned source_height=variant==0?32u:variant==1?300u:96u;
      unsigned host_width=variant==0?64u:variant==1?640u:64u;
      unsigned host_height=variant==0?48u:variant==1?480u:64u;
      unsigned fallback=variant==2?GML_PLAN_FALLBACK_BOX_REDUCTION:
                                   GML_PLAN_FALLBACK_UNSUPPORTED_OPERATION;
      int filter_ok=selected.engine->output_width==source_width &&
        selected.engine->output_height==source_height &&
        cpu.width==host_width && cpu.height==host_height &&
        selected.engine->host_canvas_active &&
        selected.engine->host_crt_active==(variant!=2) &&
        selected.engine->host_canvas_y==(variant==1?0:8) &&
        counters.scene_batches_accepted==counters.scene_batches_offered &&
        counters.scene_batches_accepted>=2u &&
        counters.scene_families[GML_RENDER_SCENE_SPRITE]>=1u &&
        counters.scene_readbacks==0u &&
        counters.fallbacks[fallback]==1u &&
        counters.cpu_upload_frames==1u &&
        selected.engine->frame_materializations==1u;
      if(!filter_ok)
        fprintf(stderr,"filter %s source=%ux%u host=%ux%u canvas=%d crt=%d y=%d "
                "scene=%llu/%llu sprite=%llu fallback=%u transport=%u "
                "materialized=%u\n",name,selected.engine->output_width,
                selected.engine->output_height,cpu.width,cpu.height,
                selected.engine->host_canvas_active,selected.engine->host_crt_active,
                selected.engine->host_canvas_y,
                (unsigned long long)counters.scene_batches_accepted,
                (unsigned long long)counters.scene_batches_offered,
                (unsigned long long)counters.scene_families[GML_RENDER_SCENE_SPRITE],
                counters.fallbacks[fallback],counters.cpu_upload_frames,
                selected.engine->frame_materializations);
      matching=matching && filter_ok;
    }
    if(matching){
      const uint32_t *pixels=cpu.pixels;
      const uint32_t *logical=selected.engine->screen;
      uint32_t first=pixels[0]&0xFFFFFFu;
      unsigned different=0;
      for(size_t i=0;i<(size_t)cpu.width*cpu.height;i++)
        different|=(pixels[i]&0xFFFFFFu)!=first;
      matching=different &&
        logical[(size_t)12u*selected.engine->output_width+8u]!=
        logical[(size_t)12u*selected.engine->output_width+24u];
    }
  }
  session_close(&selected);
  session_close(&plain);
  REQUIRE(matching,"filter CPU and selected images remain visibly identical");
  return 1;
}

static int presentation_filter_case(void){
  return presentation_filter_variant(
           "CRT native",anygm_synthetic_crt_native_content_create,0) &&
         presentation_filter_variant(
           "CRT scaled",anygm_synthetic_crt_scaled_content_create,1) &&
         presentation_filter_variant(
           "host box",anygm_synthetic_host_box_content_create,2);
}

static int scene_canvas_cpu_source_case(void){
  Session selected={0};
  AnygmConfigDelta presentation={0};
  GmlGpuCounters before={0},after={0};
  GmlRenderDispatchMetrics dispatch={0};
  GmlRenderImageRef image={0};
  unsigned width=7u,height=9u;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL,
          anygm_synthetic_host_box_content_create),"cold canvas fixture loads");
  presentation.struct_size=sizeof presentation;
  presentation.values.struct_size=sizeof presentation.values;
  presentation.fields=ANYGM_CONFIG_HYBRID_GPU_PRESENTATION|
    ANYGM_CONFIG_PRESENT_LOGICAL_RASTER|ANYGM_CONFIG_MONITOR_WIDTH|ANYGM_CONFIG_MONITOR_HEIGHT;
  presentation.values.monitor_width=presentation.values.monitor_height=64u;
  REQUIRE(anygm_set_config(selected.engine,&presentation)==ANYGM_OK &&
          advance(selected.engine,1,NULL),"cold canvas completes a CPU source before adoption");
  REQUIRE(selected.engine->host_canvas_active && selected.engine->output_width==128u &&
          selected.engine->output_height==96u,"cold canvas selects the box presentation path");
  REQUIRE(graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "cold canvas adopts a scene-only context");
  REQUIRE(!gml_render_completed_screen_image(&selected.engine->render,selected.engine->screen,
          128,96,&image),"cold canvas has no current resident source");
  gml_render_dispatch_diagnostics_enable(&selected.engine->render,1);
  gml_gpu_counters(selected.engine->gpu,&before);
  REQUIRE(!engine_present_hardware_canvas(selected.engine,&width,&height),
          "cold scene canvas retains software presentation");
  gml_gpu_counters(selected.engine->gpu,&after);
  REQUIRE(gml_render_dispatch_metrics(&selected.engine->render,&dispatch) &&
          dispatch.failed==0u && dispatch.unclassified==0u &&
          after.frames_offered==before.frames_offered && after.draw_calls==before.draw_calls &&
          after.full_uploads==before.full_uploads && width==7u && height==9u,
          "cold scene canvas does not submit an unqualified borrowed-source plan");
  session_close(&selected);
  return 1;
}

/* Restored pixels are already authoritative on the CPU. Upload that small logical image once;
 * the device, not a CPU host buffer, should clear the margins and perform a nearest enlargement. */
static int scene_canvas_restored_nearest_case(void){
  Session selected={0};
  AnygmConfigDelta presentation={0};
  GmlGpuCounters before={0},after={0};
  GmlRenderDispatchMetrics dispatch={0};
  GmlRenderImageRef image={0};
  unsigned width=7u,height=9u;
  size_t saved_size=0;
  uint8_t *saved=NULL;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL,
          anygm_synthetic_host_box_content_create),"restored nearest canvas fixture loads");
  presentation.struct_size=sizeof presentation;
  presentation.values.struct_size=sizeof presentation.values;
  presentation.fields=ANYGM_CONFIG_HYBRID_GPU_PRESENTATION|
    ANYGM_CONFIG_PRESENT_LOGICAL_RASTER|ANYGM_CONFIG_MONITOR_WIDTH|ANYGM_CONFIG_MONITOR_HEIGHT;
  presentation.values.monitor_width=192u;
  presentation.values.monitor_height=144u;
  REQUIRE(anygm_set_config(selected.engine,&presentation)==ANYGM_OK &&
          advance(selected.engine,1,NULL),"nearest canvas completes its logical CPU source");
  saved=serialize(selected.engine,&saved_size);
  REQUIRE(saved && graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "nearest canvas saves before adopting a scene-only context");
  int loaded=anygm_state_load(selected.engine,saved,saved_size)==ANYGM_OK;
  free(saved);
  REQUIRE(loaded && selected.engine->host_canvas_active &&
          selected.engine->frame_authority==ENGINE_FRAME_CPU_MATERIALIZED &&
          !gml_render_completed_screen_image(&selected.engine->render,selected.engine->screen,
          128,96,&image),"restored canvas has authoritative CPU pixels, not a stale device source");
  REQUIRE(gml_render_begin_resident_base(&selected.engine->render,selected.engine->screen,
          128,96,0.0,0.0),"restored CPU picture can retain a logical SCREEN resource");
  gml_render_resident_application_cpu_barrier(&selected.engine->render);
  REQUIRE(selected.engine->render.base_scene_resource &&
          !gml_render_completed_screen_image(&selected.engine->render,selected.engine->screen,
          128,96,&image),"CPU-current SCREEN resource has no device image");
  uint32_t logical[128u*96u];
  memcpy(logical,selected.engine->screen,sizeof logical);
  gml_render_dispatch_diagnostics_enable(&selected.engine->render,1);
  gml_gpu_counters(selected.engine->gpu,&before);
  REQUIRE(engine_present_hardware_canvas(selected.engine,&width,&height),
          "restored scene canvas performs nearest host fitting on the device");
  gml_gpu_counters(selected.engine->gpu,&after);
  REQUIRE(width==192u && height==144u && selected.engine->host_plan_valid &&
          selected.engine->frame_authority==ENGINE_FRAME_CPU_MATERIALIZED &&
          !memcmp(logical,selected.engine->screen,sizeof logical) &&
          selected.engine->host_plan.images[0].cpu_pixels==selected.engine->screen &&
          !selected.engine->host_plan.images[0].resident_image.resource_class &&
          after.passes_accepted==before.passes_accepted+1u &&
          after.full_uploads==before.full_uploads+1u &&
          after.cpu_upload_frames==before.cpu_upload_frames &&
          after.scene_readbacks==before.scene_readbacks &&
          gml_render_dispatch_metrics(&selected.engine->render,&dispatch) &&
          !dispatch.failed && !dispatch.unclassified,
          "restored fitting uploads only the logical image and preserves its state authority");
  for(unsigned family=0;family<GML_RENDER_SCENE_FAMILY_COUNT;family++)
    for(unsigned reason=0;reason<GML_RENDER_DISPATCH_REASON_COUNT;reason++)
      REQUIRE(!dispatch.cpu[family][reason],"restored host fit executes no CPU raster operation");
  REQUIRE(dispatch.gpu[GML_RENDER_SCENE_PRIMITIVE]==1u &&
          dispatch.gpu[GML_RENDER_SCENE_SURFACE]==1u,
          "restored host clear and nearest blit are counted as GPU work");
  /* The existing cold box and CRT/shift/crop exclusions are not widened by this variant. */
  for(unsigned exclusion=0;exclusion<5u;exclusion++){
    GmlRenderResourceMetrics resources_before={0},resources_after={0};
    selected.engine->host_crt_active=exclusion==0u;
    selected.engine->present_shift_x=exclusion==1u;
    selected.engine->present_crop_left=exclusion==2u;
    selected.engine->host_canvas_active=exclusion!=4u;
    selected.engine->frame_authority=exclusion==3u?ENGINE_FRAME_NONE:ENGINE_FRAME_CPU_MATERIALIZED;
    gml_gpu_counters(selected.engine->gpu,&before);
    gml_render_resource_metrics(&selected.engine->render,&resources_before);
    REQUIRE(!engine_present_hardware_canvas(selected.engine,NULL,NULL),
            "unqualified CPU canvas variants retain their established fallback");
    gml_gpu_counters(selected.engine->gpu,&after);
    gml_render_resource_metrics(&selected.engine->render,&resources_after);
    REQUIRE(after.frames_offered==before.frames_offered &&
            after.full_uploads==before.full_uploads &&
            resources_after.completed_screen_cpu_reads==resources_before.completed_screen_cpu_reads,
            "unqualified CPU canvas is refused before any pixel observation or device submission");
  }
  selected.engine->frame_authority=ENGINE_FRAME_CPU_MATERIALIZED;
  session_close(&selected);
  return 1;
}

static uint8_t *serialize(AnygmEngine *engine,size_t *size){
  size_t capacity=anygm_state_size(engine);
  uint8_t *data=(uint8_t*)malloc(capacity?capacity:1);
  size_t written=0;
  if(!data) return NULL;
  int gpu_bound=engine->scene_renderer_session==ANYGM_SCENE_RENDERER_OPENGL &&
                engine_graphics_active(engine);
  if(anygm_state_save(engine,data,capacity,&written)!=ANYGM_OK || written>capacity ||
     (!gpu_bound && written!=capacity)){
    free(data);
    return NULL;
  }
  *size=written;
  return data;
}

/* The same logical state, once with a graphics target adopted and once without. A pass that leaves
 * the processor's copy stale rather than rebuilding it on demand shows up here as different bytes,
 * which is the failure worth catching: the frame is inside the state. */
static int state_bytes_match_case(void){
  Session plain,adopted;
  uint8_t *without=NULL,*with=NULL;
  size_t without_size=0,with_size=0;
  int equal;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&plain),"a session without a target loads");
  REQUIRE(session_open(&adopted),"a session with a target loads");
  REQUIRE(graphics_adopt(adopted.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "the context is adopted");
  REQUIRE(advance(plain.engine,8,NULL),"the plain session runs");
  {
    uint32_t flags=0;
    REQUIRE(advance(adopted.engine,8,&flags),"the adopted session runs");
    REQUIRE((flags&ANYGM_FRAME_HARDWARE_TARGET)!=0,
            "a frame reached the graphics target, so this comparison is about something");
  }
  without=serialize(plain.engine,&without_size);
  with=serialize(adopted.engine,&with_size);
  equal=without && with && without_size==with_size &&
        memcmp(without,with,without_size)==0;
  if(!equal)
    fprintf(stderr,"state bytes differ: %zu without a target against %zu with one\n",
            without_size,with_size);
  free(without);
  free(with);
  session_close(&plain);
  session_close(&adopted);
  return equal;
}

/* Saving twice without an intervening mutation must produce the same bytes, whether or not a
 * target is adopted. With one, the second save is the one that has to rebuild the frame. */
static int save_load_save_case(void){
  Session session;
  uint8_t *first=NULL,*second=NULL;
  size_t first_size=0,second_size=0;
  int equal;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"the session loads");
  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGL_CORE)==ANYGM_OK,
          "the context is adopted");
  REQUIRE(advance(session.engine,6,NULL),"the session runs");
  first=serialize(session.engine,&first_size);
  REQUIRE(first!=NULL,"the first save succeeds");
  REQUIRE(anygm_state_load(session.engine,first,first_size)==ANYGM_OK,"the state loads back");
  second=serialize(session.engine,&second_size);
  equal=second && first_size==second_size && memcmp(first,second,first_size)==0;
  if(!equal)
    fprintf(stderr,"save/load/save differs: %zu then %zu bytes\n",first_size,second_size);
  free(first);
  free(second);
  session_close(&session);
  return equal;
}

/* A state written by one of them loads into the other, both ways. The target is a transport choice
 * and not emulated content, so it must be absent from the state's own identity. */
static int cross_load_case(void){
  Session plain,adopted;
  uint8_t *from_plain=NULL,*from_adopted=NULL;
  size_t plain_size=0,adopted_size=0;
  int ok;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&plain),"the plain session loads");
  REQUIRE(session_open(&adopted),"the adopted session loads");
  REQUIRE(graphics_adopt(adopted.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "the context is adopted");
  REQUIRE(advance(plain.engine,5,NULL),"the plain session runs");
  REQUIRE(advance(adopted.engine,5,NULL),"the adopted session runs");
  from_plain=serialize(plain.engine,&plain_size);
  from_adopted=serialize(adopted.engine,&adopted_size);
  REQUIRE(from_plain && from_adopted,"both saves succeed");
  ok=anygm_state_load(adopted.engine,from_plain,plain_size)==ANYGM_OK &&
     anygm_state_load(plain.engine,from_adopted,adopted_size)==ANYGM_OK;
  if(!ok) fputs("a state did not cross between a session with a target and one without\n",stderr);
  /* Both keep running afterwards, which is the part a discarded plan or a stale cache would break. */
  ok=ok && advance(adopted.engine,3,NULL) && advance(plain.engine,3,NULL);
  free(from_plain);
  free(from_adopted);
  session_close(&plain);
  session_close(&adopted);
  return ok;
}

/* Scene selection belongs to the receiving host, not the saved game. Restore a genuinely older
 * snapshot into each destination after it has advanced further; the GPU destination also loses
 * and recreates its context before loading. Compare the complete canonical state and the next
 * three frames, not merely the return code from the state reader. */
static int scene_state_transition_variant(int source_gpu,int destination_gpu){
  Session source={0},destination={0};
  AnygmInputFrame input={0};
  AnygmFrameOutput source_frame={0},destination_frame={0};
  GmlGpuCounters source_counters={0},destination_counters={0};
  uint8_t *saved=NULL,*source_state=NULL,*destination_state=NULL;
  size_t saved_size=0,source_size=0,destination_size=0;
  uint64_t sprites_before_continuation=0;
  const char *stage="save";
  int ok=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(
            &source,source_gpu?ANYGM_SCENE_RENDERER_OPENGL:ANYGM_SCENE_RENDERER_CPU,
            anygm_synthetic_multiview_gpu_content_create),
          "transition source loads");
  REQUIRE(session_open_fixture_with_scene(
            &destination,destination_gpu?ANYGM_SCENE_RENDERER_OPENGL:ANYGM_SCENE_RENDERER_CPU,
            anygm_synthetic_multiview_gpu_content_create),
          "transition destination loads");
  if(source_gpu)
    REQUIRE(graphics_adopt(source.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
            "transition source adopts a context");
  if(destination_gpu)
    REQUIRE(graphics_adopt(destination.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
            "transition destination adopts a context");
  REQUIRE(advance(source.engine,5,NULL) && advance(destination.engine,7,NULL),
          "transition sessions reach different frame positions");
  if(source_gpu){
    gml_gpu_counters(source.engine->gpu,&source_counters);
    REQUIRE(source_counters.scene_families[GML_RENDER_SCENE_SPRITE]>0u,
            "transition source actually submits scene work");
  }
  if(destination_gpu){
    gml_gpu_counters(destination.engine->gpu,&destination_counters);
    REQUIRE(destination_counters.scene_families[GML_RENDER_SCENE_SPRITE]>0u,
            "transition destination actually submits scene work before recreation");
  }

  anygm_test_graphics_forbid_calls();
  saved=serialize(source.engine,&saved_size);
  ok=saved!=NULL && saved_size>0u && anygm_test_graphics_calls_after_forget()==0;
  anygm_test_graphics_allow_calls();
  stage="recreate";
  if(destination_gpu && ok)
    ok=graphics_adopt(destination.engine,ANYGM_GRAPHICS_OPENGL_CORE)==ANYGM_OK;
  if(ok){
    stage="load and immediate state";
    anygm_test_graphics_forbid_calls();
    ok=anygm_state_load(destination.engine,saved,saved_size)==ANYGM_OK;
    destination_state=ok?serialize(destination.engine,&destination_size):NULL;
    ok=ok && destination_state && destination_size==saved_size &&
       !memcmp(destination_state,saved,saved_size) &&
       anygm_test_graphics_calls_after_forget()==0;
    anygm_test_graphics_allow_calls();
  }
  free(destination_state);
  destination_state=NULL;
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  stage="restored completed frame";
  destination_frame.struct_size=sizeof destination_frame;
  if(ok){
    ok=anygm_run_frame(destination.engine,&input,&destination_frame)==ANYGM_OK &&
       destination_frame.width==64u && destination_frame.height==48u &&
       destination_frame.audio_frames==0u &&
       destination.engine->vm.frame==source.engine->vm.frame &&
       source.engine->screen && destination.engine->screen &&
       !memcmp(source.engine->screen,destination.engine->screen,64u*48u*sizeof(uint32_t));
    if(ok){
      anygm_test_graphics_forbid_calls();
      destination_state=serialize(destination.engine,&destination_size);
      ok=destination_state && destination_size==saved_size &&
         !memcmp(destination_state,saved,saved_size) &&
         anygm_test_graphics_calls_after_forget()==0;
      anygm_test_graphics_allow_calls();
      free(destination_state);
      destination_state=NULL;
    }
  }
  if(ok && destination_gpu){
    gml_gpu_counters(destination.engine->gpu,&destination_counters);
    sprites_before_continuation=
      destination_counters.scene_families[GML_RENDER_SCENE_SPRITE];
  }
  for(int frame=0;frame<3 && ok;frame++){
    AnygmResult source_result,destination_result;
    stage="continued frame";
    memset(&source_frame,0,sizeof source_frame);
    memset(&destination_frame,0,sizeof destination_frame);
    source_frame.struct_size=destination_frame.struct_size=sizeof source_frame;
    source_result=anygm_run_frame(source.engine,&input,&source_frame);
    destination_result=anygm_run_frame(destination.engine,&input,&destination_frame);
    ok=source_result==ANYGM_OK && destination_result==ANYGM_OK &&
       source_frame.width==destination_frame.width &&
       source_frame.height==destination_frame.height &&
       source_frame.audio_rate==destination_frame.audio_rate &&
       source_frame.audio_frames==destination_frame.audio_frames &&
       (!source_frame.audio_frames ||
        (source_frame.audio && destination_frame.audio &&
         !memcmp(source_frame.audio,destination_frame.audio,
                 source_frame.audio_frames*2u*sizeof(int16_t))));
    if(!ok){
      fprintf(stderr,"continued frame %d: results=%d/%d size=%ux%u/%ux%u "
              "audio=%zu@%u/%zu@%u\n",frame,(int)source_result,(int)destination_result,
              source_frame.width,source_frame.height,destination_frame.width,
              destination_frame.height,source_frame.audio_frames,source_frame.audio_rate,
              destination_frame.audio_frames,destination_frame.audio_rate);
      break;
    }
    stage="continued state";
    anygm_test_graphics_forbid_calls();
    source_state=serialize(source.engine,&source_size);
    destination_state=serialize(destination.engine,&destination_size);
    ok=source_state && destination_state && source_size==destination_size &&
       !memcmp(source_state,destination_state,source_size) &&
       anygm_test_graphics_calls_after_forget()==0;
    anygm_test_graphics_allow_calls();
    free(source_state);
    free(destination_state);
    source_state=destination_state=NULL;
  }
  if(ok && destination_gpu){
    stage="continued GPU work";
    gml_gpu_counters(destination.engine->gpu,&destination_counters);
    ok=destination_counters.scene_families[GML_RENDER_SCENE_SPRITE]>
       sprites_before_continuation;
    if(!ok)
      fprintf(stderr,"continued GPU sprite count=%llu before=%llu frame_flags=%u "
              "session=%u gpu=%p\n",
              (unsigned long long)destination_counters.scene_families[GML_RENDER_SCENE_SPRITE],
              (unsigned long long)sprites_before_continuation,destination_frame.flags,
              destination.engine->scene_renderer_session,(void*)destination.engine->gpu);
  }
  if(!ok)
    fprintf(stderr,"scene state transition failed: %s to %s at %s "
            "(saved=%zu source=%zu destination=%zu forbidden=%d)\n",
            source_gpu?"GPU":"CPU",destination_gpu?"GPU":"CPU",stage,
            saved_size,source_size,destination_size,anygm_test_graphics_calls_after_forget());
  anygm_test_graphics_allow_calls();
  free(source_state);
  free(destination_state);
  free(saved);
  session_close(&destination);
  session_close(&source);
  return ok;
}

static int scene_state_transition_matrix_case(void){
  return scene_state_transition_variant(0,0) &&
         scene_state_transition_variant(0,1) &&
         scene_state_transition_variant(1,0) &&
         scene_state_transition_variant(1,1);
}

/* Serializing every frame is what rewind and run-ahead do. The sizes may move as content allocates;
 * what may not happen is a frame that cannot be produced, or a save that reports a size it then
 * fails to fill. */
static int per_frame_serialization_case(void){
  Session session;
  int ok=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"the session loads");
  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "the context is adopted");
  for(int frame=0;ok && frame<12;frame++){
    size_t size=0;
    uint8_t *data;
    ok=advance(session.engine,1,NULL);
    data=ok?serialize(session.engine,&size):NULL;
    ok=ok && data!=NULL && size>0;
    /* The reported size is the exact size of what was written, every frame, whatever the frame's
     * run-length encoding happened to compress to. */
    ok=ok && size==anygm_state_size(session.engine);
    free(data);
  }
  if(!ok) fputs("per-frame serialization under a graphics target failed\n",stderr);
  session_close(&session);
  return ok;
}

/* A context that arrives, is taken away properly, disappears without notice, and arrives again,
 * with a state operation on either side of each. */
static int context_lifecycle_around_state_case(void){
  Session session;
  uint8_t *before=NULL,*after=NULL;
  size_t before_size=0,after_size=0;
  int ok;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"the session loads");
  REQUIRE(advance(session.engine,4,NULL),"the session runs without a target");
  before=serialize(session.engine,&before_size);
  REQUIRE(before!=NULL,"a save before any context succeeds");

  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "a context adopted after content is loaded");
  REQUIRE(anygm_state_load(session.engine,before,before_size)==ANYGM_OK,
          "the state loads immediately after the context arrives");
  REQUIRE(advance(session.engine,2,NULL),"the session runs on the target");

  /* A reset with no destroy before it: the previous context is already gone. */
  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGL_CORE)==ANYGM_OK,
          "a repeated reset is accepted");
  REQUIRE(advance(session.engine,2,NULL),"the session runs on the new target");
  after=serialize(session.engine,&after_size);
  REQUIRE(after!=NULL,"a save while a target is adopted succeeds");

  /* Taken away properly, then a state operation with no target at all. */
  anygm_graphics_context_destroy(session.engine,1u);
  REQUIRE(anygm_state_load(session.engine,after,after_size)==ANYGM_OK,
          "the state loads immediately after the context is destroyed");
  ok=advance(session.engine,2,NULL);
  if(!ok) fputs("the session did not continue after the context was destroyed\n",stderr);
  free(before);
  free(after);
  session_close(&session);
  return ok;
}

/* A corrupt state is refused, and refusing it leaves the engine exactly as it was — including while
 * a frame has been presented through a graphics target and the processor's copy of it has not been
 * rebuilt yet. Restoring the bytes it would have written is the part that has to survive. */
/* The read-back budget decides once, from the first passes, and never from fewer than it needs. */
static int readback_budget_case(void){
  anygm_test_graphics_reset();
  REQUIRE(!engine_readback_over_budget(0,0),"no frames, no verdict");
  REQUIRE(!engine_readback_over_budget(7u*20000000u,7),"seven expensive frames are not yet a verdict");
  REQUIRE(engine_readback_over_budget(8u*20000000u,8),"eight frames at twenty milliseconds are over budget");
  REQUIRE(!engine_readback_over_budget(8u*3000000u,8),"one three-millisecond pass a frame is within it");
  REQUIRE(!engine_readback_over_budget(8u*8000000u,8),"the budget itself is within it");
  REQUIRE(engine_readback_over_budget(8u*8000001u,8),"a nanosecond over is over");
  /* What the rule is for: the cost of a frame, not of a pass. Three passes of three milliseconds
   * fit in a frame; six of the same size do not. */
  REQUIRE(!engine_readback_over_budget(8u*(3u*2500000u),8),"three passes at 2.5 ms a frame fit");
  REQUIRE(engine_readback_over_budget(8u*(6u*2500000u),8),"six of the same passes do not");
  return 1;
}

static int rejected_state_is_transactional_case(void){
  Session session;
  uint8_t *good=NULL,*damaged=NULL,*after=NULL;
  size_t size=0,after_size=0;
  int ok;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"the session loads");
  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "the context is adopted");
  {
    uint32_t flags=0;
    REQUIRE(advance(session.engine,7,&flags),"the session runs");
    REQUIRE((flags&ANYGM_FRAME_HARDWARE_TARGET)!=0,
            "the current frame is on the graphics target");
  }
  anygm_test_graphics_forbid_calls();
  good=serialize(session.engine,&size);
  REQUIRE(good!=NULL && size>64,"a save succeeds and is long enough to damage meaningfully");
  damaged=(uint8_t*)malloc(size);
  REQUIRE(damaged!=NULL,"the damaged copy is allocated");
  memcpy(damaged,good,size);
  /* Past the header, so framing survives and a section reader is the one that refuses. */
  for(size_t index=size/2;index<size/2+16 && index<size;index++) damaged[index]^=0xA5u;
  REQUIRE(anygm_state_load(session.engine,damaged,size)!=ANYGM_OK,
          "a damaged state is refused");
  /* The engine is unchanged, so serializing again produces the bytes it would have produced. */
  after=serialize(session.engine,&after_size);
  ok=after && after_size==size && memcmp(after,good,size)==0;
  if(!ok) fputs("a refused state did not leave the engine as it was\n",stderr);
  ok=ok && anygm_test_graphics_calls_after_forget()==0;
  if(anygm_test_graphics_calls_after_forget())
    fputs("size/save/rejected load called graphics without a current context\n",stderr);
  anygm_test_graphics_allow_calls();
  ok=ok && advance(session.engine,2,NULL);
  free(good);
  free(damaged);
  free(after);
  session_close(&session);
  return ok;
}

/* The context disappears while the current frame is on it and the processor's copy has not been
 * rebuilt. Losing a context is announced as a reset with no destroy before it, and the frame still
 * has to be produced afterwards: the operation that made it is retained, not the pixels it wrote. */
static int context_lost_with_frame_pending_case(void){
  Session session;
  uint8_t *before=NULL,*after=NULL;
  size_t before_size=0,after_size=0;
  int ok;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"the session loads");
  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "the context is adopted");
  {
    uint32_t flags=0;
    REQUIRE(advance(session.engine,6,&flags),"the session runs");
    REQUIRE((flags&ANYGM_FRAME_HARDWARE_TARGET)!=0,"the frame is on the graphics target");
  }
  /* What the state would have been, taken before the loss. Producing it here is itself the first
   * half of the question: the frame was not written on the processor. */
  before=serialize(session.engine,&before_size);
  REQUIRE(before!=NULL,"the frame is produced while the context is still there");

  /* Run one more so a frame is on the target again and nothing has rebuilt it. */
  REQUIRE(advance(session.engine,1,NULL),"one more frame reaches the target");
  /* A reset with no destroy: the previous context is gone. */
  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGL_CORE)==ANYGM_OK,
          "a replacement context is adopted");
  after=serialize(session.engine,&after_size);
  ok=after!=NULL && after_size>0;
  if(!ok) fputs("the frame could not be produced after the context disappeared\n",stderr);
  ok=ok && advance(session.engine,2,NULL);
  free(before);
  free(after);
  session_close(&session);
  return ok;
}

/* Restoring a frame may retire scene images while GL is not current. The frontend can then lose
 * that context without a destroy callback and hand us a replacement whose texture names start
 * from the same values. Neither the deferred retirement nor the old dispatch table may delete a
 * newly allocated texture with the recycled name. */
static int context_replacement_reuses_names_case(void){
  Session plain={0},selected={0};
  uint8_t *saved=NULL,*plain_state=NULL,*selected_state=NULL;
  size_t saved_size=0,plain_size=0,selected_size=0;
  GmlGpuCounters before={0},after={0};
  unsigned recycled_texture=0;
  const char *stage="open";
  int ok=1;
  anygm_test_graphics_reset();
  ok=session_open_fixture_with_scene(&plain,ANYGM_SCENE_RENDERER_CPU,
                                     anygm_synthetic_multiview_gpu_content_create) &&
     session_open_fixture_with_scene(&selected,ANYGM_SCENE_RENDERER_OPENGL,
                                     anygm_synthetic_multiview_gpu_content_create);
  if(ok){
    stage="initial GPU draw";
    ok=graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK &&
       advance(plain.engine,5,NULL) && advance(selected.engine,5,NULL);
  }
  if(ok){
    gml_gpu_counters(selected.engine->gpu,&before);
    recycled_texture=anygm_test_graphics_scene_texture();
    ok=before.scene_families[GML_RENDER_SCENE_SPRITE]>0u && recycled_texture>0u &&
       anygm_test_graphics_texture_width(recycled_texture)>0;
  }
  if(ok){
    stage="state and deferred retirement";
    saved=serialize(selected.engine,&saved_size);
    plain_state=serialize(plain.engine,&plain_size);
    ok=saved && plain_state && saved_size==plain_size &&
       !memcmp(saved,plain_state,saved_size);
  }
  free(plain_state);
  plain_state=NULL;
  if(ok){
    anygm_test_graphics_forbid_calls();
    ok=anygm_state_load(selected.engine,saved,saved_size)==ANYGM_OK &&
       anygm_state_load(plain.engine,saved,saved_size)==ANYGM_OK &&
       anygm_test_graphics_calls_after_forget()==0;
    anygm_test_graphics_allow_calls();
  }
  if(ok){
    stage="replacement namespace";
    anygm_test_graphics_reset(); /* The old context disappeared; generated names now restart. */
    ok=graphics_adopt(selected.engine,ANYGM_GRAPHICS_OPENGL_CORE)==ANYGM_OK &&
       anygm_test_graphics_deletes()==0 &&
       anygm_test_graphics_reuse_name(recycled_texture);
  }
  for(int frame=0;frame<4 && ok;frame++){
    stage=frame==0?"restored completed frame":"continued frame";
    ok=advance(plain.engine,1,NULL) && advance(selected.engine,1,NULL) &&
       plain.engine->screen && selected.engine->screen &&
       !memcmp(plain.engine->screen,selected.engine->screen,64u*48u*sizeof(uint32_t));
    if(ok && frame==1){
      stage="recycled live texture";
      ok=anygm_test_graphics_texture_width(recycled_texture)>0 &&
         !anygm_test_graphics_texture_deleted(recycled_texture) &&
         anygm_test_graphics_deletes()==0;
    }
    if(ok){
      stage="continued canonical state";
      anygm_test_graphics_forbid_calls();
      plain_state=serialize(plain.engine,&plain_size);
      selected_state=serialize(selected.engine,&selected_size);
      ok=plain_state && selected_state && plain_size==selected_size &&
         !memcmp(plain_state,selected_state,plain_size) &&
         anygm_test_graphics_calls_after_forget()==0;
      anygm_test_graphics_allow_calls();
      free(plain_state);
      free(selected_state);
      plain_state=selected_state=NULL;
    }
  }
  if(ok){
    stage="new GPU sprite work";
    gml_gpu_counters(selected.engine->gpu,&after);
    ok=after.scene_families[GML_RENDER_SCENE_SPRITE]>
       before.scene_families[GML_RENDER_SCENE_SPRITE];
  }
  if(!ok)
    fprintf(stderr,"context replacement failed at %s (old texture=%u, new scene texture=%u, "
            "old width=%d, old deleted=%d, deletes=%d, forbidden calls=%d)\n",stage,
            recycled_texture,anygm_test_graphics_scene_texture(),
            anygm_test_graphics_texture_width(recycled_texture),
            anygm_test_graphics_texture_deleted(recycled_texture),
            anygm_test_graphics_deletes(),anygm_test_graphics_calls_after_forget());
  anygm_test_graphics_allow_calls();
  free(plain_state);
  free(selected_state);
  free(saved);
  session_close(&selected);
  session_close(&plain);
  return ok;
}

/* Two engines at once, one with a graphics target and one without, interleaved. Every handle and
 * every dispatch pointer is owned by its own engine, so neither may reach into the other. */
static int interleaved_engines_case(void){
  Session adopted,plain;
  uint8_t *from_adopted=NULL;
  size_t adopted_size=0;
  int ok;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&adopted),"the adopted session loads");
  REQUIRE(session_open(&plain),"the plain session loads");
  REQUIRE(graphics_adopt(adopted.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "one engine adopts a context");
  for(int frame=0;frame<6;frame++){
    uint32_t adopted_flags=0,plain_flags=0;
    REQUIRE(advance(adopted.engine,1,&adopted_flags),"the adopted engine runs a frame");
    REQUIRE(advance(plain.engine,1,&plain_flags),"the plain engine runs a frame");
    REQUIRE((adopted_flags&ANYGM_FRAME_HARDWARE_TARGET)!=0,
            "the adopted engine presents through its target");
    REQUIRE((plain_flags&ANYGM_FRAME_HARDWARE_TARGET)==0,
            "the engine without a target never claims one");
  }
  from_adopted=serialize(adopted.engine,&adopted_size);
  REQUIRE(from_adopted!=NULL,"the adopted engine serializes");
  ok=anygm_state_load(plain.engine,from_adopted,adopted_size)==ANYGM_OK;
  if(!ok) fputs("a state did not cross between two live engines\n",stderr);
  ok=ok && advance(plain.engine,2,NULL) && advance(adopted.engine,2,NULL);
  free(from_adopted);
  session_close(&adopted);
  session_close(&plain);
  return ok;
}

static int live_engine_pair_matches(Session *plain,Session *selected){
  uint8_t *plain_state=NULL,*selected_state=NULL;
  size_t plain_size=0,selected_size=0;
  int same=plain->engine && selected->engine && plain->engine->screen &&
           selected->engine->screen &&
           plain->engine->vm.frame==selected->engine->vm.frame &&
           !memcmp(plain->engine->screen,selected->engine->screen,
                   64u*48u*sizeof(uint32_t));
  if(same){
    plain_state=serialize(plain->engine,&plain_size);
    selected_state=serialize(selected->engine,&selected_size);
    same=plain_state && selected_state && plain_size==selected_size &&
         !memcmp(plain_state,selected_state,plain_size);
  }
  free(plain_state);
  free(selected_state);
  return same;
}

/* Unlike the older GPU+CPU interleaving control, both selected engines own live GL backends.
 * Different resolver functions and framebuffer callbacks reveal accidental sharing of the
 * dispatch table; distinct texture names, frame positions and CPU controls reveal shared state. */
static int two_live_gpu_engines_case(void){
  Session plain[2]={0},selected[2]={0};
  GraphicsWitness witness[3]={{0}};
  GmlGpuCounters before[2]={{0}},after[2]={{0}};
  unsigned texture[2]={0},old_errors=0,old_framebuffers=0,replacement_errors=0;
  const char *stage="open";
  int ok=1;
  anygm_test_graphics_reset();
  memset(graphics_witness_error_calls,0,sizeof graphics_witness_error_calls);
  for(unsigned index=0;index<2u && ok;index++){
    witness[index].slot=index;
    ok=session_open_fixture_with_scene(&plain[index],ANYGM_SCENE_RENDERER_CPU,
                                       anygm_synthetic_multiview_gpu_content_create) &&
       session_open_fixture_with_scene(&selected[index],ANYGM_SCENE_RENDERER_OPENGL,
                                       anygm_synthetic_multiview_gpu_content_create);
  }
  if(ok){
    stage="adopt two contexts";
    ok=graphics_adopt_witness(selected[0].engine,ANYGM_GRAPHICS_OPENGLES3,
                              &witness[0])==ANYGM_OK &&
       graphics_adopt_witness(selected[1].engine,ANYGM_GRAPHICS_OPENGL_CORE,
                              &witness[1])==ANYGM_OK;
  }
  for(int frame=0;frame<6 && ok;frame++){
    for(unsigned index=0;index<2u && ok;index++){
      if(index==0u && frame>=4) continue;
      stage="initial interleaving";
      ok=advance(plain[index].engine,1,NULL) &&
         advance(selected[index].engine,1,NULL) &&
         live_engine_pair_matches(&plain[index],&selected[index]);
      if(ok && frame==0) texture[index]=anygm_test_graphics_scene_texture();
    }
  }
  if(ok){
    stage="independent initial ownership";
    gml_gpu_counters(selected[0].engine->gpu,&before[0]);
    gml_gpu_counters(selected[1].engine->gpu,&before[1]);
    ok=plain[0].engine->vm.frame==4 && plain[1].engine->vm.frame==6 &&
       before[0].scene_families[GML_RENDER_SCENE_SPRITE]>0u &&
       before[1].scene_families[GML_RENDER_SCENE_SPRITE]>0u &&
       texture[0]>0u && texture[1]>0u && texture[0]!=texture[1] &&
       !anygm_test_graphics_texture_deleted(texture[1]) &&
       graphics_witness_error_calls[0]>0u && graphics_witness_error_calls[1]>0u &&
       witness[0].proc_queries>0u && witness[1].proc_queries>0u &&
       witness[0].framebuffer_queries>0u && witness[1].framebuffer_queries>0u;
  }
  if(ok){
    stage="reset only first engine";
    witness[2].slot=2u;
    ok=graphics_adopt_witness(selected[0].engine,ANYGM_GRAPHICS_OPENGL_CORE,
                              &witness[2])==ANYGM_OK &&
       !anygm_test_graphics_texture_deleted(texture[1]) &&
       live_engine_pair_matches(&plain[1],&selected[1]);
  }
  if(ok){
    stage="second engine survives first reset";
    old_errors=graphics_witness_error_calls[0];
    old_framebuffers=witness[0].framebuffer_queries;
    replacement_errors=graphics_witness_error_calls[2];
    unsigned second_errors=graphics_witness_error_calls[1];
    unsigned second_framebuffers=witness[1].framebuffer_queries;
    ok=advance(plain[1].engine,1,NULL) && advance(selected[1].engine,1,NULL) &&
       live_engine_pair_matches(&plain[1],&selected[1]) &&
       graphics_witness_error_calls[1]>second_errors &&
       witness[1].framebuffer_queries>second_framebuffers &&
       graphics_witness_error_calls[0]==old_errors &&
       graphics_witness_error_calls[2]==replacement_errors &&
       witness[0].framebuffer_queries==old_framebuffers &&
       !anygm_test_graphics_texture_deleted(texture[1]);
  }
  if(ok){
    stage="first engine resumes on replacement";
    ok=advance(plain[0].engine,1,NULL) && advance(selected[0].engine,1,NULL) &&
       live_engine_pair_matches(&plain[0],&selected[0]) &&
       graphics_witness_error_calls[2]>replacement_errors &&
       graphics_witness_error_calls[0]==old_errors &&
       witness[2].framebuffer_queries>0u &&
       anygm_test_graphics_scene_texture()!=texture[1];
    if(ok){
      gml_gpu_counters(selected[0].engine->gpu,&after[0]);
      ok=after[0].scene_families[GML_RENDER_SCENE_SPRITE]>
         before[0].scene_families[GML_RENDER_SCENE_SPRITE];
    }
  }
  if(ok){
    stage="non-current shutdown leaves second engine alive";
    unsigned first_errors=graphics_witness_error_calls[2];
    unsigned first_framebuffers=witness[2].framebuffer_queries;
    anygm_test_graphics_forbid_calls();
    anygm_graphics_context_destroy(selected[0].engine,0u);
    ok=anygm_test_graphics_calls_after_forget()==0 &&
       graphics_witness_error_calls[2]==first_errors &&
       witness[2].framebuffer_queries==first_framebuffers;
    anygm_test_graphics_allow_calls();
    ok=ok && advance(plain[0].engine,1,NULL) && advance(selected[0].engine,1,NULL) &&
       live_engine_pair_matches(&plain[0],&selected[0]) &&
       advance(plain[1].engine,1,NULL) && advance(selected[1].engine,1,NULL) &&
       live_engine_pair_matches(&plain[1],&selected[1]) &&
       !anygm_test_graphics_texture_deleted(texture[1]);
  }
  if(ok){
    stage="continued GPU work in second engine";
    gml_gpu_counters(selected[1].engine->gpu,&after[1]);
    ok=after[1].scene_families[GML_RENDER_SCENE_SPRITE]>
       before[1].scene_families[GML_RENDER_SCENE_SPRITE];
  }
  if(!ok)
    fprintf(stderr,"two live GPU engines failed at %s (textures=%u/%u, error calls=%u/%u/%u, "
            "framebuffer calls=%u/%u/%u, forbidden=%d)\n",stage,texture[0],texture[1],
            graphics_witness_error_calls[0],graphics_witness_error_calls[1],
            graphics_witness_error_calls[2],witness[0].framebuffer_queries,
            witness[1].framebuffer_queries,witness[2].framebuffer_queries,
            anygm_test_graphics_calls_after_forget());
  anygm_test_graphics_allow_calls();
  for(unsigned index=0;index<2u;index++){
    session_close(&selected[index]);
    session_close(&plain[index]);
  }
  return ok;
}

/* An unusable context leaves a fully working software engine, and destroying an engine that never
 * had a current context issues no graphics call at all. */
static int refused_context_case(void){
  Session session;
  AnygmGraphicsContext context;
  GmlGpuCounters counters={0};
  int ok;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(&session,ANYGM_SCENE_RENDERER_OPENGL,
            anygm_synthetic_multiview_gpu_content_create),"the session loads");
  context=graphics_context(ANYGM_GRAPHICS_OPENGLES3);
  context.struct_size=(uint32_t)(sizeof context-1u);
  REQUIRE(anygm_graphics_context_reset(session.engine,&context)==ANYGM_ERROR_INCOMPATIBLE_ABI,
          "a context that declares a smaller size than it has is refused");
  context=graphics_context(ANYGM_GRAPHICS_OPENGLES3);
  context.get_proc_address=NULL;
  REQUIRE(anygm_graphics_context_reset(session.engine,&context)!=ANYGM_OK,
          "a context with no resolver is refused");
  context=graphics_context(0xBADu);
  REQUIRE(anygm_graphics_context_reset(session.engine,&context)==ANYGM_ERROR_UNSUPPORTED,
          "an unknown graphics family is refused");
  anygm_test_graphics_withhold("glTexSubImage2D");
  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_ERROR_UNSUPPORTED,
          "a missing entry point prevents initial GPU adoption");
  {
    const char *reason=gml_gpu_last_error(session.engine->gpu);
    REQUIRE(reason && strstr(reason,"glTexSubImage2D")!=NULL,
            "the initial failure names the unavailable function");
  }
  anygm_test_graphics_withhold(NULL);
  {
    uint32_t flags=0;
    ok=advance(session.engine,4,&flags);
    ok=ok && (flags&ANYGM_FRAME_HARDWARE_TARGET)==0;
  }
  if(!ok) fputs("a refused context did not leave a working software engine\n",stderr);
  /* Recovery adopts the same engine after its CPU frames; it must perform actual scene work. */
  ok=ok && graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK;
  ok=ok && advance(session.engine,1,NULL);
  if(ok){
    gml_gpu_counters(session.engine->gpu,&counters);
    ok=counters.scene_families[GML_RENDER_SCENE_SPRITE]>0u;
  }
  if(!ok) fputs("a valid later context did not resume GPU sprite work\n",stderr);
  /* Tear down without promising that the context is current. */
  anygm_test_graphics_forbid_calls();
  session_close(&session);
  ok=ok && anygm_test_graphics_calls_after_forget()==0;
  if(!ok) fputs("destroying an engine outside a current context issued a graphics call\n",stderr);
  return ok;
}

static int neutral_device_binding_case(void){
  Session session;
  const GmlRenderDeviceServices *device;
  GmlRenderDeviceCaps caps;
  GmlRenderResourceSource old_atlas={0},new_atlas={0};
  GmlRenderImageUpload transfer={0};
  GmlGpuCounters before={0},after={0};
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"session loads for device binding");
  REQUIRE(gml_render_device(&session.engine->render)==NULL,
          "CPU scene begins without a bound device");
  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "a context is adopted for device binding");
  device=gml_render_device(&session.engine->render);
  REQUIRE(device && device->context && device->capabilities && device->submit,
          "the engine binds the neutral service on successful adoption");
  memset(&caps,0,sizeof caps);
  REQUIRE(device->capabilities(device->context,&caps) &&
          (caps.supported_operations&(UINT64_C(1)<<GML_RENDER_OP_TEXTURED_QUAD)),
          "binding exposes the qualified prototype without selecting it for the CPU scene");
  REQUIRE(gml_render_atlas_scene_source_view(&session.engine->render,0,&old_atlas),
          "the loaded renderer identifies its first immutable atlas page");
  transfer.image=old_atlas.image;
  transfer.storage_width=old_atlas.width;
  transfer.storage_height=old_atlas.height;
  transfer.region=(GmlRenderRect){0,0,old_atlas.width,old_atlas.height};
  transfer.pixel_format=old_atlas.pixel_format;
  transfer.stride_bytes=old_atlas.stride_bytes;
  transfer.byte_capacity=old_atlas.byte_capacity;
  transfer.bytes=old_atlas.bytes;
  gml_gpu_counters(session.engine->gpu,&before);
  REQUIRE(device->upload(device->context,&transfer) &&
          device->upload(device->context,&transfer),
          "the first immutable page uploads and same-version reuse succeeds");
  gml_gpu_counters(session.engine->gpu,&after);
  REQUIRE(after.scene_uploads==before.scene_uploads+1u,
          "same-version immutable atlas reuse does not upload twice");
  REQUIRE(anygm_reset(session.engine)==ANYGM_OK,
          "reset retains the adopted graphics context");
  device=gml_render_device(&session.engine->render);
  REQUIRE(device && device->context && device->capabilities && device->submit,
          "the newly initialized renderer rebinds the existing device after reset");
  REQUIRE(gml_render_atlas_scene_source_view(&session.engine->render,0,&new_atlas) &&
          new_atlas.image.identity==old_atlas.image.identity &&
          new_atlas.image.incarnation!=old_atlas.image.incarnation &&
          new_atlas.image.content_version==1u,
          "Reset gives the same atlas index a new source incarnation");
  transfer.image=new_atlas.image;
  transfer.storage_width=new_atlas.width;
  transfer.storage_height=new_atlas.height;
  transfer.region=(GmlRenderRect){0,0,new_atlas.width,new_atlas.height};
  transfer.pixel_format=new_atlas.pixel_format;
  transfer.stride_bytes=new_atlas.stride_bytes;
  transfer.byte_capacity=new_atlas.byte_capacity;
  transfer.bytes=new_atlas.bytes;
  REQUIRE(device->upload(device->context,&transfer),
          "the retained device accepts the fresh Reset atlas generation");
  gml_gpu_counters(session.engine->gpu,&before);
  REQUIRE(before.scene_uploads==after.scene_uploads+1u,
          "Reset atlas generation uploads anew on the retained device");
  anygm_graphics_context_destroy(session.engine,1u);
  REQUIRE(gml_render_device(&session.engine->render)==NULL,
          "device service is cleared before context destruction");
  REQUIRE(advance(session.engine,1,NULL),"CPU scene continues after context destruction");
  session_close(&session);
  return 1;
}

static int runtime_font_reset_scene_case(void){
  Session session={0};
  GmlRenderResourceSource first={0},second={0};
  GmlRenderImageUpload transfer={0};
  GmlGpuCounters before={0},after={0};
  const GmlRenderDeviceServices *device;
  char font_path[256]={0};
  uint8_t font_bytes[552];
  const char *failure=NULL;
  FILE *file=NULL;
  int font,previous_atlas;
  anygm_test_graphics_reset();
  if(!session_open(&session)) return 0;
  if(snprintf(font_path,sizeof font_path,"%s/fixture.ttf",session.fixture.directory)>=
     (int)sizeof font_path){ failure="font fixture path is bounded"; goto done; }
  file=fopen(font_path,"wb");
  if(!file){ failure="font fixture opens"; goto done; }
  size_t size=font_fixture_build(font_bytes);
  int written=fwrite(font_bytes,1,size,file)==size;
  int closed=fclose(file)==0;
  file=NULL;
  if(!written || !closed){ failure="font fixture writes completely"; goto done; }
  if(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)!=ANYGM_OK){
    failure="runtime-font reset context adopted"; goto done;
  }
  font=gml_font_add_file(&session.engine->render,font_path,32,0,0,65,65);
  if(font<0){ failure="runtime font created before Reset"; goto done; }
  previous_atlas=session.engine->render.fonts[font].atlas;
  if(!gml_render_atlas_scene_source_view(&session.engine->render,previous_atlas,&first) ||
     first.borrowed_immutable || !first.bytes){
    failure="runtime page exposed before Reset"; goto done;
  }
  device=gml_render_device(&session.engine->render);
  transfer.image=first.image;
  transfer.storage_width=first.width; transfer.storage_height=first.height;
  transfer.region=(GmlRenderRect){0,0,first.width,first.height};
  transfer.pixel_format=first.pixel_format;
  transfer.stride_bytes=first.stride_bytes;
  transfer.byte_capacity=first.byte_capacity;
  transfer.bytes=first.bytes;
  gml_gpu_counters(session.engine->gpu,&before);
  if(!device || !device->upload(device->context,&transfer) ||
     anygm_reset(session.engine)!=ANYGM_OK){
    failure="runtime page uploaded and Reset completed"; goto done;
  }
  font=gml_font_add_file(&session.engine->render,font_path,32,0,0,65,65);
  if(font<0 || session.engine->render.fonts[font].atlas!=previous_atlas ||
     !gml_render_atlas_scene_source_view(&session.engine->render,previous_atlas,&second) ||
     second.image.incarnation==first.image.incarnation ||
     second.image.content_version!=1u || second.borrowed_immutable){
    failure="Reset rebuilds the same-index runtime page with a fresh generation"; goto done;
  }
  device=gml_render_device(&session.engine->render);
  transfer.image=second.image;
  transfer.storage_width=second.width; transfer.storage_height=second.height;
  transfer.region=(GmlRenderRect){0,0,second.width,second.height};
  transfer.pixel_format=second.pixel_format;
  transfer.stride_bytes=second.stride_bytes;
  transfer.byte_capacity=second.byte_capacity;
  transfer.bytes=second.bytes;
  if(!device || !device->upload(device->context,&transfer)){
    failure="new runtime generation uploads on the retained device"; goto done;
  }
  gml_gpu_counters(session.engine->gpu,&after);
  if(after.scene_uploads!=before.scene_uploads+2u){
    failure="both runtime generations upload separately"; goto done;
  }
done:
  if(file) fclose(file);
  if(font_path[0]) remove(font_path);
  session_close(&session);
  if(failure){ fprintf(stderr,"graphics state failed: %s\n",failure); return 0; }
  return 1;
}

/* A context is shared transport, not permission to turn every graphics policy on. Disabling the
 * hybrid policy keeps eligible final passes on the processor, and leaving content GLSL disabled
 * keeps the renderer from reporting a shader device even while that context carries the frame. */
static int independent_graphics_policy_case(void){
  Session session;
  AnygmConfigDelta graphics={0};
  uint32_t flags=0;
  int ok;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"the independent-policy session loads");
  graphics.struct_size=sizeof graphics;
  graphics.values.struct_size=sizeof graphics.values;
  graphics.fields=ANYGM_CONFIG_HYBRID_GPU_PRESENTATION;
  graphics.values.hybrid_gpu_presentation=0u;
  REQUIRE(anygm_set_config(session.engine,&graphics)==ANYGM_OK,
          "hybrid presentation can be disabled independently");
  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "the shared context is adopted");
  ok=advance(session.engine,1,&flags) &&
     (flags&ANYGM_FRAME_HARDWARE_TARGET)!=0 &&
     session.engine->screen_pass_frames==0 && session.engine->canvas_pass_frames==0 &&
     !engine_hybrid_presentation_active(session.engine) &&
     !engine_content_shaders_active(session.engine) &&
     !gml_render_shader_device_present(&session.engine->render);
  if(!ok) fputs("an adopted context enabled a graphics policy implicitly\n",stderr);
  graphics.fields=ANYGM_CONFIG_CONTENT_SHADER_DEVICE_EXPECTED|
                  ANYGM_CONFIG_CONTENT_SHADER_READBACK;
  graphics.values.content_shader_device_expected=1u;
  graphics.values.content_shader_readback=ANYGM_SHADER_READBACK_BUDGETED;
  ok=ok && anygm_set_config(session.engine,&graphics)==ANYGM_OK &&
     engine_content_shaders_active(session.engine) &&
     session.engine->render.shader_device_expected==1;
  if(!ok) fputs("content GLSL did not activate without hybrid presentation\n",stderr);
  {
    AnygmConfigDelta monitor={0};
    monitor.struct_size=sizeof monitor;
    monitor.values.struct_size=sizeof monitor.values;
    monitor.fields=ANYGM_CONFIG_MONITOR_WIDTH;
    monitor.values.monitor_width=640u;
    ok=ok && anygm_set_config(session.engine,&monitor)==ANYGM_OK &&
       session.engine->render.shader_device_expected==1;
  }
  if(!ok) fputs("an unrelated configuration update cleared the GLSL session policy\n",stderr);
  graphics.values.content_shader_device_expected=0u;
  graphics.values.content_shader_readback=ANYGM_SHADER_READBACK_NEVER;
  ok=ok && anygm_set_config(session.engine,&graphics)==ANYGM_OK &&
     !engine_content_shaders_active(session.engine);
  graphics.values.hybrid_gpu_presentation=1u;
  graphics.fields=ANYGM_CONFIG_HYBRID_GPU_PRESENTATION;
  ok=ok && anygm_set_config(session.engine,&graphics)==ANYGM_OK &&
     engine_hybrid_presentation_active(session.engine) &&
     !engine_content_shaders_active(session.engine) && advance(session.engine,1,NULL);
  if(!ok) fputs("the explicit hybrid policy did not activate independently\n",stderr);
  session_close(&session);
  return ok;
}

/* A frontend that sized its rewind buffer once, before a presentation option grew the state, keeps
 * offering the buffer it fixed. The completed frame is the section that grew and the only section a
 * restore can do without, so a save that cannot fit it writes the state without it rather than
 * failing: a refusal costs every later snapshot, and with them the whole rewind history, while a
 * state without a frame simply resumes by drawing one. A buffer one byte short of the whole state
 * is the narrowest form of that situation, and the roomy buffer beside it shows the frame is only
 * dropped when it has to be. */
static int short_buffer_drops_the_frame_case(void){
  Session session;
  size_t whole=0,written=0,short_written=0;
  uint8_t *data=NULL;
  int ok;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"the session loads");
  REQUIRE(advance(session.engine,6,NULL),"the session runs");
  whole=anygm_state_size(session.engine);
  REQUIRE(whole>1,"the state has a size");
  data=(uint8_t*)malloc(whole);
  REQUIRE(data!=NULL,"the buffer is allocated");
  ok=anygm_state_save(session.engine,data,whole,&written)==ANYGM_OK && written==whole;
  if(!ok) fprintf(stderr,"the whole state did not fit its own size: %zu of %zu\n",written,whole);
  ok=ok && anygm_state_save(session.engine,data,whole-1u,&short_written)==ANYGM_OK;
  if(!ok) fprintf(stderr,"a save into %zu bytes failed instead of dropping the frame\n",whole-1u);
  ok=ok && short_written<whole;
  ok=ok && anygm_state_load(session.engine,data,short_written)==ANYGM_OK;
  if(!ok)
    fprintf(stderr,"the state written without its frame does not load back (%zu of %zu bytes)\n",
            short_written,whole);
  free(data);
  session_close(&session);
  return ok;
}

/* A host that explicitly asks for a frequent in-memory resume point gets the same canonical
 * simulation state without the optional completed picture. The complete API remains unchanged,
 * while the compact form roundtrips exactly and the restored engine keeps running. */
static int explicit_resume_state_case(void){
  Session session;
  size_t whole=0,resume=0,whole_written=0,resume_written=0,again_written=0;
  uint8_t *whole_data=NULL,*resume_data=NULL,*again=NULL;
  int ok;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"the session loads");
  REQUIRE(advance(session.engine,6,NULL),"the session runs");
  whole=anygm_state_size(session.engine);
  resume=anygm_state_resume_size(session.engine);
  REQUIRE(resume>0 && resume<whole,"the explicit resume state omits the completed frame");
  whole_data=(uint8_t*)malloc(whole);
  resume_data=(uint8_t*)malloc(resume);
  again=(uint8_t*)malloc(resume);
  REQUIRE(whole_data && resume_data && again,"the state buffers are allocated");
  ok=anygm_state_save(session.engine,whole_data,whole,&whole_written)==ANYGM_OK &&
     whole_written==whole;
  ok=ok && anygm_state_save_for_resume(session.engine,resume_data,resume,&resume_written)==ANYGM_OK &&
     resume_written==resume;
  ok=ok && anygm_state_load(session.engine,resume_data,resume_written)==ANYGM_OK;
  ok=ok && anygm_state_save_for_resume(session.engine,again,resume,&again_written)==ANYGM_OK &&
     again_written==resume && memcmp(resume_data,again,resume)==0;
  ok=ok && advance(session.engine,2,NULL);
  if(!ok)
    fprintf(stderr,"the explicit resume state failed (%zu compact, %zu complete)\n",resume,whole);
  free(whole_data);
  free(resume_data);
  free(again);
  session_close(&session);
  return ok;
}

/* Detailed native rasters should take the bounded raw path instead of expanding to one eight-byte
 * run per pixel, while presentation repetition should retain the much smaller row-RLE path. Both
 * encodings are state bytes, so prove their exact pixels survive a real transactional load. */
static int completed_frame_encodings_case(void){
  Session session;
  uint32_t *expected=NULL;
  uint8_t *state=NULL;
  size_t pixels=0,raw_size=0,rle_size=0,resume_size=0;
  int ok=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"the frame-encoding session loads");
  REQUIRE(advance(session.engine,1,NULL),"the frame-encoding session presents");
  pixels=(size_t)session.engine->output_width*session.engine->output_height;
  REQUIRE(pixels>0 && session.engine->screen!=NULL,"the completed frame has pixels");
  expected=(uint32_t*)malloc(pixels*sizeof(uint32_t));
  REQUIRE(expected!=NULL,"the expected frame is allocated");

  for(size_t index=0;index<pixels;index++)
    expected[index]=UINT32_C(0xFF000000)|
                    (uint32_t)((index*UINT32_C(2654435761))&UINT32_C(0x00FFFFFF));
  memcpy(session.engine->screen,expected,pixels*sizeof(uint32_t));
  resume_size=anygm_state_resume_size(session.engine);
  state=serialize(session.engine,&raw_size);
  ok=state && raw_size>resume_size && raw_size-resume_size<=pixels*4u+64u;
  if(ok){
    memset(session.engine->screen,0,pixels*sizeof(uint32_t));
    ok=anygm_state_load(session.engine,state,raw_size)==ANYGM_OK &&
       memcmp(session.engine->screen,expected,pixels*sizeof(uint32_t))==0;
  }
  free(state); state=NULL;

  for(size_t index=0;index<pixels;index++) expected[index]=UINT32_C(0xFF24486C);
  memcpy(session.engine->screen,expected,pixels*sizeof(uint32_t));
  state=serialize(session.engine,&rle_size);
  ok=ok && state && rle_size>resume_size && rle_size-resume_size<pixels;
  if(ok){
    memset(session.engine->screen,0,pixels*sizeof(uint32_t));
    ok=anygm_state_load(session.engine,state,rle_size)==ANYGM_OK &&
       memcmp(session.engine->screen,expected,pixels*sizeof(uint32_t))==0;
  }
  if(!ok)
    fprintf(stderr,"completed-frame encodings failed: raw=%zu rle=%zu resume=%zu pixels=%zu\n",
            raw_size,rle_size,resume_size,pixels);
  free(state);
  free(expected);
  session_close(&session);
  return ok;
}

/* Capacity is not an encoding policy. A detailed picture can exceed a small slot while a later
 * repeated picture of exactly the same extent fits; each successful write must equal the ordinary
 * complete or explicit resume writer. Failed attempts must not change the live canonical state. */
static int completed_frame_capacity_case(void){
  Session session;
  AnygmConfigDelta presentation={0};
  size_t pixels,complete_size,resume_size,written;
  uint8_t *complete=NULL,*resume=NULL,*buffer=NULL,*after=NULL;
  int ok=1;
  anygm_test_graphics_reset();
  REQUIRE(session_open_fixture_with_scene(&session,ANYGM_SCENE_RENDERER_CPU,
          anygm_synthetic_large_moving_sound_content_create),
          "capacity control loads a large authored neutral fixture");
  presentation.struct_size=sizeof presentation;
  presentation.values.struct_size=sizeof presentation.values;
  presentation.fields=ANYGM_CONFIG_PRESENT_LOGICAL_RASTER|ANYGM_CONFIG_ASPECT_MODE;
  presentation.values.present_logical_raster=1u;
  presentation.values.aspect_mode=GMC_ASPECT_FORCE_NONE;
  REQUIRE(anygm_set_config(session.engine,&presentation)==ANYGM_OK &&
          advance(session.engine,1,NULL) && session.engine->output_width==1920u &&
          session.engine->output_height==1080u,"capacity control presents its large raster");
  pixels=(size_t)session.engine->output_width*session.engine->output_height;
  for(unsigned variant=0;variant<4u && ok;variant++){
    for(size_t index=0;index<pixels;index++){
      size_t row=index/1920u,column=index%1920u;
      size_t value=variant==0u?index:
                   variant==1u?row*160u+column/12u:
                   variant==2u?(row/16u)*160u+column/12u:0u;
      session.engine->screen[index]=UINT32_C(0xFF000000)|
        (uint32_t)((value*UINT32_C(2654435761))&UINT32_C(0x00FFFFFF));
    }
    complete=serialize(session.engine,&complete_size);
    resume_size=anygm_state_resume_size(session.engine);
    resume=malloc(resume_size);
    ok=complete && resume && resume_size>1u &&
       anygm_state_save_for_resume(session.engine,resume,resume_size,&written)==ANYGM_OK &&
       written==resume_size;
    ok=ok && (variant<2u?complete_size>1048576u:complete_size<1048576u);
    const size_t capacities[]={1u,8192u,resume_size-1u,resume_size,resume_size+32u,
                               1048576u,complete_size-1u,complete_size,complete_size+16u};
    for(size_t choice=0;choice<sizeof capacities/sizeof capacities[0] && ok;choice++){
      size_t capacity=capacities[choice];
      buffer=malloc(capacity+16u);
      if(!buffer){ ok=0; break; }
      memset(buffer,0xA5,capacity+16u);
      written=0;
      anygm_test_graphics_forbid_calls();
      AnygmResult result=anygm_state_save(session.engine,buffer,capacity,&written);
      anygm_test_graphics_allow_calls();
      ok=session.engine->state_omit_frame==0 && !anygm_test_graphics_calls_after_forget();
      for(size_t guard=capacity;guard<capacity+16u;guard++)
        ok=ok && buffer[guard]==0xA5u;
      if(capacity<resume_size){
        ok=ok && result!=ANYGM_OK && written>capacity;
      } else {
        const uint8_t *expected=capacity>=complete_size?complete:resume;
        size_t expected_size=capacity>=complete_size?complete_size:resume_size;
        ok=ok && result==ANYGM_OK && written==expected_size &&
           !memcmp(buffer,expected,expected_size);
      }
      size_t after_size=0;
      after=serialize(session.engine,&after_size);
      ok=ok && after && after_size==complete_size &&
         !memcmp(after,complete,complete_size);
      if(!ok)
        fprintf(stderr,"frame capacity failed: variant=%u capacity=%zu complete=%zu "
                       "resume=%zu result=%d written=%zu\n",
                variant,capacity,complete_size,resume_size,(int)result,written);
      free(after); after=NULL;
      free(buffer); buffer=NULL;
    }
    free(resume); resume=NULL;
    free(complete); complete=NULL;
  }
  free(after); free(buffer); free(resume); free(complete);
  session_close(&session);
  return ok;
}

/* A terminal content program must finish at its authored raster before CRT filtering. The fake
 * proves synchronous readback, ownership and transport; the software plan cases own filter pixels. */
static int crt_terminal_program_extent(unsigned source_w,unsigned source_h,
                                       unsigned host_w,unsigned host_h,int native_padding){
  Session session;
  uint32_t source[16];
  struct GmlShaderPal pal[1]={0};
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"CRT graphics session loads");
  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,"CRT context adopted");
  AnygmEngine *engine=session.engine;
  GmlRender *render=&engine->render;
  REQUIRE(render->n_shader_pal==0,"fixture has no content programs");
  engine->config.content_shader_device_expected=1;
  engine->config.content_shader_readback=ANYGM_SHADER_READBACK_BUDGETED;
  engine->output_width=source_w; engine->output_height=source_h;
  engine->host_output_width=host_w; engine->host_output_height=host_h;
  engine->host_crt_active=engine->host_canvas_active=1;
  engine->host_canvas_width=native_padding?(int)source_w:(int)host_w;
  engine->host_canvas_height=native_padding?(int)source_h:(int)host_h;
  engine->host_canvas_x=((int)host_w-engine->host_canvas_width)/2;
  engine->host_canvas_y=((int)host_h-engine->host_canvas_height)/2;
  engine->readback_is_pipelined=1;
  for(unsigned i=0;i<16;i++) source[i]=0xFFAA5500u;
  pal[0].source_vertex_es="attribute vec3 in_Position; void main(){ gl_Position=vec4(in_Position,1.0); }";
  pal[0].source_fragment_es="varying vec2 v_vTexcoord; void main(){ gl_FragColor=texture2D(gm_BaseTexture,v_vTexcoord); }";
  struct GmlShaderPal *saved_pal=render->shader_pal;
  render->shader_pal=pal; render->n_shader_pal=1;
  gml_render_application_surface_bind(render,source,4,4,1);
  gml_render_application_surface_set_draw_enabled(render,0);
  gml_render_set_deferred_presentation(render,1);
  gml_render_begin(render,engine->screen,source_w,source_h,0,0);
  gml_render_gui_begin(render,source_w,source_h);
  gml_render_gui_set_size(render,source_w,source_h);
  gml_render_shader_set_current(render,0);
  gml_draw_surface_stretched(render,0,0,0,source_w,source_h,0xFFFFFFu,1.0);
  gml_render_shader_set_current(render,-1);
  int ok=gml_render_deferred_presentation(render,NULL) &&
         !engine_present_hardware_screen(engine,NULL,NULL) &&
         anygm_test_graphics_quad_draw_calls()==1 &&
         anygm_test_graphics_read_pixels()==1 && !anygm_test_graphics_async_read_pixels() &&
         !gml_render_deferred_presentation(render,NULL) &&
         engine->frame_authority==ENGINE_FRAME_CPU_MATERIALIZED;
  const uint32_t *pixels=NULL; unsigned width=0,height=0;
  ok=ok && resolve_host_frame(engine,&pixels,&width,&height) && width==host_w && height==host_h &&
           engine_present_hardware_frame(engine,pixels,width,height);
  int x=0,y=0,w=0,h=0;
  anygm_test_graphics_viewport(&x,&y,&w,&h);
  ok=ok && w==(int)host_w && h==(int)host_h && engine->readback_is_pipelined==1;
  gml_render_gui_end(render);
  render->shader_pal=saved_pal; render->n_shader_pal=0;
  gml_render_application_surface_bind(render,engine->fb,engine->width,engine->height,1);
  session_close(&session);
  REQUIRE(ok,"terminal shader readback precedes CRT fit and hardware transport");
  return 1;
}
static int crt_terminal_program_case(void){
  return crt_terminal_program_extent(400,300,640,480,0) &&
         crt_terminal_program_extent(320,180,320,240,1) &&
         crt_terminal_program_extent(240,240,320,240,1);
}

/* A surface's vertex coordinates belong to the draw's authored space, not a local shader tile. */
static int content_surface_authored_position_case(void){
  Session session;
  struct GmlShaderPal program[1]={0};
  uint32_t target[16]={0};
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"authored surface position session loads");
  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "authored surface position context adopted");
  GmlRender *render=&session.engine->render;
  struct GmlShaderPal *saved_programs=render->shader_pal;
  int saved_program_count=render->n_shader_pal;
  render->shader_pal=program; render->n_shader_pal=1;
  session.engine->config.content_shader_device_expected=1;
  session.engine->config.content_shader_readback=ANYGM_SHADER_READBACK_ALWAYS;
  program[0].source_vertex_es=
    "attribute vec3 in_Position; uniform mat4 gm_Matrices[5]; "
    "varying vec2 fixture_position; void main(){fixture_position=in_Position.xy; "
    "gl_Position=gm_Matrices[4]*vec4(in_Position,1.0);}";
  program[0].source_fragment_es=
    "precision mediump float; varying vec2 fixture_position; "
    "void main(){gl_FragColor=vec4(fixture_position.x/32.0,0.,0.,1.);}";
  gml_render_set_shader_executor(render,engine_execute_content_shader,session.engine);
  int surface=gml_surface_create(render,2,2);
  int ok=surface>0;
  if(ok){
    uint32_t *pixels=render->surface[surface-1].px;
    ok=pixels!=NULL;
    if(pixels) for(unsigned pixel=0;pixel<4;pixel++) pixels[pixel]=0xFFFFFFFFu;
  }
  /* The viewport remains at x=4 in both cases. The vertex attribute must retain the authored
   * x=4 or x=14, not local zero or camera-relative x=4. Observe submission, not fake pixels. */
  for(unsigned variant=0;variant<7 && surface>0;variant++){
    double camera=variant==1 || variant==2?10.0:0.0;
    double authored_x=4.0+camera;
    double authored_width=2.0;
    gml_render_begin(render,target,8,2,camera,0);
    if(variant==2){
      gml_render_world_set_logical_extent(render,4,2);
      authored_x=12.0; authored_width=1.0;
    } else if(variant>=3 && variant<=4){
      gml_render_gui_begin(render,8,2);
      gml_render_gui_set_size(render,4,2);
      authored_x=2.0; authored_width=1.0;
      if(variant==4){
        gml_render_gui_set_maximise(render,1,2.0,1.0,1.0,0.0,8,2);
        authored_x=1.5;
      }
    }
    gml_render_shader_set_current(render,0);
    render->alphablend=0;
    unsigned before=anygm_test_graphics_quad_draw_calls();
    if(variant==5)
      gml_draw_surface_part_ext(render,surface,1,0,1,2,authored_x,0,2,1,0xFFFFFFu,1.0);
    else if(variant==6){
      authored_x=8; authored_width=-2;
      gml_draw_surface_stretched(render,surface,authored_x,0,authored_width,2,0xFFFFFFu,1.0);
    } else
      gml_draw_surface_stretched(render,surface,authored_x,0,authored_width,2,0xFFFFFFu,1.0);
    unsigned after=anygm_test_graphics_quad_draw_calls();
    int vx=0,vy=0,vw=0,vh=0;
    anygm_test_graphics_viewport(&vx,&vy,&vw,&vh);
    /* Signed draws retain the existing temporary readback target, then compose the flip. */
    int matches=after==before+1u && vx==(variant==6?0:4) && vy==0 && vw==2 && vh==2;
    float projection[16]={0},combined[16]={0};
    matches=matches && anygm_test_graphics_content_matrix(1,projection) &&
                       anygm_test_graphics_content_matrix(4,combined);
    for(int corner=0;corner<4;corner++){
      float xyz[3]={0};
      float expected_x=(float)authored_x+(corner&1?(float)authored_width:0.0f);
      float expected_y=corner&2?2.0f:0.0f;
      if(!anygm_test_graphics_quad_position(corner,xyz) ||
         xyz[0]!=expected_x || xyz[1]!=expected_y || xyz[2]!=0.0f){
        fprintf(stderr,"authored surface position camera=%.0f corner=%d: "
                "(%g,%g,%g) != (%g,%g,0)\n",camera,corner,
                xyz[0],xyz[1],xyz[2],expected_x,expected_y);
        matches=0;
      }
      /* Observe the paired transform as well: fixing the attribute must not move the quad. */
      for(unsigned matrix=0;matrix<2;matrix++){
        const float *values=matrix?combined:projection;
        float clip_x=values[0]*xyz[0]+values[4]*xyz[1]+values[12];
        float clip_y=values[1]*xyz[0]+values[5]*xyz[1]+values[13];
        if(fabsf(clip_x-(corner&1?1.0f:-1.0f))>0.00001f ||
           fabsf(clip_y-(corner&2?-1.0f:1.0f))>0.00001f){
          fprintf(stderr,"surface projection variant=%u matrix=%u corner=%d: (%g,%g)\n",
                  variant,matrix,corner,clip_x,clip_y);
          matches=0;
        }
      }
    }
    if(!matches) fprintf(stderr,"authored surface position draws=%u->%u viewport=%d,%d %dx%d\n",
                         before,after,vx,vy,vw,vh);
    ok=ok && matches;
    if(variant>=3 && variant<=4) gml_render_gui_end(render);
  }
  gml_render_shader_set_current(render,-1);
  render->shader_pal=saved_programs; render->n_shader_pal=saved_program_count;
  if(surface>0) gml_surface_free(render,surface);
  session_close(&session);
  REQUIRE(ok,"surface shaders preserve authored positions and camera-relative target placement");
  return 1;
}

/* A recorded shader draw keeps its authored space after GUI teardown and target clipping. */
static int content_terminal_authored_position_case(void){
  Session session;
  struct GmlShaderPal program[1]={0};
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"terminal authored-position session loads");
  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "terminal authored-position context adopted");
  AnygmEngine *engine=session.engine;
  GmlRender *render=&engine->render;
  struct GmlShaderPal *saved_programs=render->shader_pal;
  int saved_program_count=render->n_shader_pal;
  render->shader_pal=program; render->n_shader_pal=1;
  engine->config.content_shader_device_expected=1;
  engine->config.content_shader_readback=ANYGM_SHADER_READBACK_ALWAYS;
  engine->output_width=8; engine->output_height=4;
  program[0].source_vertex_es=
    "attribute vec3 in_Position; uniform mat4 gm_Matrices[5]; "
    "varying vec2 fixture_position; void main(){fixture_position=in_Position.xy; "
    "gl_Position=gm_Matrices[4]*vec4(in_Position,1.0);}";
  program[0].source_fragment_es=
    "precision mediump float; varying vec2 fixture_position; "
    "void main(){gl_FragColor=vec4(fixture_position.x/32.0,0.,0.,1.);}";
  int surface=gml_surface_create(render,2,2),ok=surface>0;
  if(surface>0) for(unsigned pixel=0;pixel<4;pixel++)
    render->surface[surface-1].px[pixel]=0xFFFFFFFFu;
  gml_render_set_deferred_presentation(render,1);
  for(unsigned variant=0;variant<3 && surface>0;variant++){
    double x=variant? -1.0:10.0,y=variant? -1.0:3.0;
    double width=variant?6.0:8.0,height=4.0;
    double expected_x=variant==2? -0.5:variant?0.0:10.0;
    double expected_y=variant?0.0:3.0;
    double expected_width=variant?4.0:8.0,expected_height=variant?2.0:4.0;
    gml_render_begin(render,engine->screen,8,4,variant?0:10,variant?0:3);
    if(variant){
      gml_render_gui_begin(render,8,4);
      gml_render_gui_set_size(render,4,2);
      if(variant==2)
        gml_render_gui_set_maximise(render,1,2,2,1,0,8,4);
    }
    gml_render_shader_set_current(render,0);
    gml_draw_surface_stretched(render,surface,x,y,width,height,0xFFFFFFu,1.0);
    GmlRenderDeferredPresentation recorded={0};
    int matches=gml_render_deferred_presentation(render,&recorded) &&
      recorded.shader_geometry.valid && recorded.shader_geometry.origin_x==x &&
      recorded.shader_geometry.origin_y==y && recorded.shader_geometry.extent_x==width &&
      recorded.shader_geometry.extent_y==height;
    gml_render_shader_set_current(render,-1);
    if(variant) gml_render_gui_end(render);
    unsigned before=anygm_test_graphics_quad_draw_calls();
    matches=matches && engine_present_hardware_screen(engine,NULL,NULL) &&
      (unsigned)anygm_test_graphics_quad_draw_calls()==before+1u;
    int vx=0,vy=0,vw=0,vh=0;
    anygm_test_graphics_viewport(&vx,&vy,&vw,&vh);
    matches=matches && vx==0 && vy==0 && vw==8 && vh==4;
    float projection[16]={0};
    matches=matches && anygm_test_graphics_content_matrix(4,projection);
    for(int corner=0;corner<4;corner++){
      float xyz[3]={0};
      float expected_corner_x=(float)(expected_x+(corner&1?expected_width:0));
      float expected_corner_y=(float)(expected_y+(corner&2?expected_height:0));
      if(!anygm_test_graphics_quad_position(corner,xyz) ||
         xyz[0]!=expected_corner_x || xyz[1]!=expected_corner_y || xyz[2]!=0 ||
         fabsf(projection[0]*xyz[0]+projection[12]-(corner&1?1.0f:-1.0f))>0.00001f ||
         fabsf(projection[5]*xyz[1]+projection[13]-(corner&2?-1.0f:1.0f))>0.00001f){
        fprintf(stderr,"terminal authored position variant=%u corner=%d (%g,%g) expected (%g,%g)\n",
                variant,corner,xyz[0],xyz[1],expected_corner_x,expected_corner_y);
        matches=0;
      }
    }
    if(!matches) fprintf(stderr,"terminal authored position variant=%u recorded=%u viewport=%d,%d %dx%d\n",
                         variant,recorded.shader_geometry.valid,vx,vy,vw,vh);
    ok=ok && matches;
  }
  gml_render_discard_deferred_presentation(render);
  render->shader_pal=saved_programs; render->n_shader_pal=saved_program_count;
  if(surface>0) gml_surface_free(render,surface);
  session_close(&session);
  REQUIRE(ok,"terminal shader retains draw-time coordinates through clipping and GUI teardown");
  return 1;
}

/* Observe uploaded bytes, not fake shader output. Each input must survive the next scratch
 * accessor, including a larger auxiliary that grows the shared allocation. */
static int content_sampler_upload_lifetime_case(void){
  Session session;
  GmlSprite sprites[3]={0};
  struct GmlShaderPal program[1]={0};
  uint8_t rgba[3][36]={{0}};
  uint32_t primary[4]={0xFF204060u,0xFF408020u,0xFF608040u,0xFF802060u};
  const uint32_t auxiliary[2]={0xFF306090u,0xFF90C030u};
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"sampler lifetime session loads");
  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "sampler lifetime context adopted");
  GmlRender *render=&session.engine->render;
  GmlSprite *saved_sprites=render->spr;
  int saved_sprite_count=render->n_spr;
  struct GmlShaderPal *saved_programs=render->shader_pal;
  int saved_program_count=render->n_shader_pal;
  render->spr=sprites; render->n_spr=3;
  render->shader_pal=program; render->n_shader_pal=1;
  session.engine->config.content_shader_device_expected=1;
  session.engine->config.content_shader_readback=ANYGM_SHADER_READBACK_ALWAYS;
  program[0].source_vertex_es=
    "attribute vec3 in_Position; attribute vec2 in_TextureCoord; "
    "uniform mat4 gm_Matrices[5]; varying vec2 fixture_uv; "
    "void main(){fixture_uv=in_TextureCoord; "
    "gl_Position=gm_Matrices[4]*vec4(in_Position,1.0);}";
  program[0].source_fragment_es=
    "precision mediump float; varying vec2 fixture_uv; uniform sampler2D gm_BaseTexture; "
    "uniform sampler2D fixture_a; uniform sampler2D fixture_b; "
    "void main(){gl_FragColor=texture2D(gm_BaseTexture,fixture_uv) "
    "+texture2D(fixture_a,fixture_uv)+texture2D(fixture_b,fixture_uv);}";
  int ok=1;
  unsigned serial=0;
  for(unsigned enlarged=0;enlarged<2 && ok;enlarged++){
    for(unsigned sprite=0;sprite<3;sprite++){
      sprites[sprite].w=sprites[sprite].h=sprite && enlarged?3:2;
      sprites[sprite].n_frames=1;
      sprites[sprite].runtime_rgba=rgba[sprite];
      for(int pixel=0;pixel<sprites[sprite].w*sprites[sprite].h;pixel++){
        uint32_t value=sprite?auxiliary[sprite-1]:primary[pixel];
        for(unsigned channel=0;channel<3;channel++)
          rgba[sprite][pixel*4+channel]=(uint8_t)(value>>(16-channel*8));
        rgba[sprite][pixel*4+3]=255;
      }
    }
    for(unsigned sampler_count=1;sampler_count<=2 && ok;sampler_count++){
      program[0].generic_sampler_count=(int)sampler_count;
      for(unsigned index=0;index<sampler_count;index++){
        snprintf(program[0].generic_sampler[index].name,32,"fixture_%c",'a'+index);
        program[0].generic_sampler[index].texture=
          gml_render_sprite_texture_handle((int)index+1,0);
      }
      for(unsigned scratch=0;scratch<2 && ok;scratch++){
        GmlRenderShaderRequest request={0};
        uint32_t output[4]={0};
        const uint32_t *source=primary;
        if(scratch && !gml_render_sprite_frame_plane(render,0,0,NULL,NULL,&source)){ ok=0; break; }
        const uint32_t *scratch_before=render->content_sampler_plane;
        size_t scratch_capacity=render->content_sampler_plane_capacity;
        request.shader=0; request.source=source; request.output=output;
        request.source_width=request.source_height=request.source_pitch=2;
        request.width=request.height=request.region_width=request.region_height=2;
        request.target_width=request.target_height=2;
        request.source_identity=17; request.serial=++serial;
        for(unsigned i=0;i<4;i++) request.vertex_colour[i]=1.0f;
        if(!engine_execute_content_shader(session.engine,&request)){ ok=0; break; }
        if(memcmp(source,primary,sizeof primary) ||
           render->content_sampler_plane!=scratch_before ||
           render->content_sampler_plane_capacity!=scratch_capacity){ ok=0; break; }
        for(unsigned unit=0;unit<=sampler_count;unit++){
          size_t bytes=0;
          unsigned texture=anygm_test_graphics_quad_texture(unit);
          const unsigned char *uploaded=anygm_test_graphics_texture_bytes(texture,&bytes);
          unsigned pixels=unit?(unsigned)(sprites[unit].w*sprites[unit].h):4u;
          if(!texture || !uploaded || bytes!=pixels*sizeof(uint32_t)){ ok=0; break; }
          for(unsigned pixel=0;pixel<pixels;pixel++){
            uint32_t actual=0,expected=unit?auxiliary[unit-1]:primary[pixel];
            memcpy(&actual,uploaded+pixel*sizeof actual,sizeof actual);
            if(actual!=expected){
              fprintf(stderr,"sampler upload enlarged=%u samplers=%u scratch=%u "
                      "unit=%u pixel=%u: %08x != %08x\n",enlarged,sampler_count,scratch,
                      unit,pixel,actual,expected);
              ok=0; break;
            }
          }
          if(!ok) break;
        }
      }
    }
  }
  if(ok){
    /* The retained terminal presentation uses the same sampler preparation as a mid-frame
     * readback, but its plan lives in the engine after these temporary uploads are consumed. */
    /* Start a new graphics namespace: the bounded fake records texture names below 32, while
     * the preceding eight submissions also allocate program, target and buffer names. */
    anygm_graphics_context_destroy(session.engine,1);
    anygm_test_graphics_reset();
    ok=graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK;
  }
  if(ok){
    session.engine->output_width=session.engine->output_height=2;
    session.engine->host_canvas_active=0;
    gml_render_application_surface_bind(render,primary,2,2,1);
    gml_render_application_surface_set_draw_enabled(render,0);
    gml_render_set_deferred_presentation(render,1);
    gml_render_begin(render,session.engine->screen,2,2,0,0);
    gml_render_gui_begin(render,2,2);
    gml_render_gui_set_size(render,2,2);
    gml_render_shader_set_current(render,0);
    gml_draw_surface_stretched(render,0,0,0,2,2,0xFFFFFFu,1.0);
    gml_render_shader_set_current(render,-1);
    GmlRenderDeferredPresentation terminal={0};
    int deferred=gml_render_deferred_presentation(render,&terminal);
    unsigned draws_before=anygm_test_graphics_quad_draw_calls();
    int presented=deferred?engine_present_hardware_screen(session.engine,NULL,NULL):0;
    ok=deferred && presented;
    if(!ok) fprintf(stderr,"sampler terminal deferred=%d presented=%d shader=%d "
                    "images=%u fallback=%u\n",deferred,presented,render->active_shader,
                    session.engine->host_plan.image_count,session.engine->host_plan.fallback_reason);
    for(unsigned unit=0;unit<3 && ok;unit++){
      size_t bytes=0;
      const unsigned char *uploaded=anygm_test_graphics_texture_bytes(
        anygm_test_graphics_quad_texture(unit),&bytes);
      unsigned pixels=unit?9u:4u;
      ok=uploaded && bytes==pixels*sizeof(uint32_t);
      if(!ok) fprintf(stderr,"sampler terminal unit=%u texture=%u bytes=%zu expected=%zu "
                      "shader=%d draws=%u->%u images=%u fallback=%u error=%s\n",
                      unit,anygm_test_graphics_quad_texture(unit),bytes,pixels*sizeof(uint32_t),
                      terminal.shader,draws_before,anygm_test_graphics_quad_draw_calls(),
                      session.engine->host_plan.image_count,session.engine->host_plan.fallback_reason,
                      gml_gpu_last_error(session.engine->gpu));
      for(unsigned pixel=0;pixel<pixels && ok;pixel++){
        uint32_t actual=0,expected=unit?auxiliary[unit-1]:primary[pixel];
        memcpy(&actual,uploaded+pixel*sizeof actual,sizeof actual);
        ok=actual==expected;
        if(!ok) fprintf(stderr,"sampler terminal unit=%u pixel=%u %08x != %08x\n",
                        unit,pixel,actual,expected);
      }
    }
    ok=ok && session.engine->host_plan.images[1].cpu_pixels==NULL &&
       session.engine->host_plan.images[2].cpu_pixels==NULL;
    gml_render_gui_end(render);
    gml_render_application_surface_bind(render,session.engine->fb,
                                        session.engine->width,session.engine->height,1);
  }
  if(ok){
    /* Refuse after one owned auxiliary has been prepared. The software fallback must still
     * have its original borrowed source and no partly written shader output. */
    GmlRenderShaderRequest request={0};
    uint32_t output[4]={0xFFAABBCCu,0xFFAABBCCu,0xFFAABBCCu,0xFFAABBCCu};
    uint32_t before[4];
    const uint32_t *source=NULL;
    program[0].generic_sampler_count=2;
    program[0].generic_sampler[1].texture=gml_render_sprite_texture_handle(99,0);
    ok=gml_render_sprite_frame_plane(render,0,0,NULL,NULL,&source);
    memcpy(before,output,sizeof before);
    request.shader=0; request.source=source; request.output=output;
    request.source_width=request.source_height=request.source_pitch=2;
    request.width=request.height=request.region_width=request.region_height=2;
    request.target_width=request.target_height=2;
    request.source_identity=17; request.serial=++serial;
    ok=ok && !engine_execute_content_shader(session.engine,&request) &&
       !memcmp(source,primary,sizeof primary) && !memcmp(output,before,sizeof before);
  }
  render->spr=saved_sprites; render->n_spr=saved_sprite_count;
  render->shader_pal=saved_programs; render->n_shader_pal=saved_program_count;
  session_close(&session);
  REQUIRE(ok,"primary and auxiliary uploads retain independent draw-time pixels");
  return 1;
}

static int texture_filter_state_case(void){
  Session source,destination;
  REQUIRE(session_open(&source),"filter source loads");
  REQUIRE(session_open(&destination),"filter destination loads");
  GmlRender *r=&source.engine->render;
  gml_render_texture_filter_all(r,1);
  gml_render_texture_filter_set(r,0,0);
  gml_render_texture_filter_set(r,3,0);
  REQUIRE(gml_render_gpu_state_push(r),"mixed filter state pushes");
  gml_render_texture_filter_all(r,0);
  gml_render_texture_filter_set(r,7,1);
  size_t size=0,again_size=0;
  uint8_t *state=serialize(source.engine,&size);
  REQUIRE(state,"mixed filter state serializes before the first frame");
  REQUIRE(anygm_state_load(destination.engine,state,size)==ANYGM_OK,
          "mixed filter state loads into another engine");
  GmlRender *loaded=&destination.engine->render;
  REQUIRE(!gml_render_texture_filter_get(loaded,0) &&
          !gml_render_texture_filter_get(loaded,1) &&
          gml_render_texture_filter_get(loaded,7),"restored filters match each stage");
  uint8_t *again=serialize(destination.engine,&again_size);
  REQUIRE(again && again_size==size && !memcmp(state,again,size),
          "restored sampler state is byte stable");
  REQUIRE(gml_render_gpu_state_pop(loaded) &&
          !gml_render_texture_filter_get(loaded,0) &&
          gml_render_texture_filter_get(loaded,1) &&
          !gml_render_texture_filter_get(loaded,3) &&
          gml_render_texture_filter_get(loaded,7),"restored GPU stack retains per-stage filters");
  free(again); free(state);
  session_close(&destination); session_close(&source);
  return 1;
}

static uint64_t profile_test_clock(void *userdata){
  uint64_t *ticks=userdata;
  return ++*ticks*1000000u;
}

static int final_scene_seal_profile_case(void){
  Session session;
  uint64_t ticks=0;
  anygm_test_graphics_reset();
  REQUIRE(session_open_with_scene(&session,ANYGM_SCENE_RENDERER_OPENGL),
          "profile control opens a neutral scene");
  REQUIRE(graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK,
          "profile control adopts its fake device");
  session.engine->host.monotonic_time_ns=profile_test_clock;
  session.engine->host.userdata=&ticks;
  session.engine->profile_enabled=1;
  REQUIRE(advance(session.engine,1,NULL),"first profiled scene completes");
  REQUIRE(session.engine->profile.seal_ms==1.0,
          "profile accounts for the final scene seal separately");
  REQUIRE(session.engine->profile.total_ms==session.engine->profile.frame_ms+
          session.engine->profile.present_ms &&
          session.engine->profile.max_ms==session.engine->profile.total_ms,
          "seal is included once in total and maximum frame cost");
  REQUIRE(advance(session.engine,1,NULL) && session.engine->profile.seal_ms==2.0 &&
          session.engine->profile.frames==2,
          "sealing accumulates across completed logical frames");
  session.engine->host.userdata=NULL;
  session.engine->host.monotonic_time_ns=NULL;
  session_close(&session);
  return 1;
}

int main(void){
  static const struct { const char *name; int (*run)(void); } cases[]={
    {"witnessed frame end submits without automatic pixel observation",
     scene_frame_end_defers_observation_case},
    {"unbound state observation preserves actual device authority for retry",
     scene_state_unbound_observation_is_retryable_case},
    {"destruction respects access proof and bounded observation failure",
     scene_destroy_observation_fault_case},
    {"scene state observes explicitly lent device pixels",scene_state_observes_lent_device_case},
    {"scene device-only surfaces are volatile on genuine context loss",scene_volatile_context_loss_case},
    {"scene state preserves the actual completed device framebuffer",
     scene_state_preserves_completed_device_case},
    {"scene state size queries do not observe pixels",scene_size_query_is_metadata_only_case},
    {"final scene seal is included in frame profiling",final_scene_seal_profile_case},
    {"resident Pre-Draw screen retains CPU state",resident_pre_draw_screen_case},
    {"resident world selection retains CPU state",resident_world_selection_case},
    {"viewless classic application accepts Draw scene work",
     classic_viewless_application_scene_case},
    {"single classic view admits logical Draw without changing presentation",
     classic_single_view_application_scene_case},
    {"excluded single classic views retain CPU presentation",classic_single_view_exclusions_case},
    {"classic single view preserves later geometry transitions",classic_single_view_geometry_transition_case},
    {"borrowed resize keeps Draw metadata and resident pixels",classic_borrowed_resize_scene_case},
    {"classic single view preserves CPU observers",classic_single_view_cpu_observers_case},
    {"classic single view executes nearest draw contracts",classic_single_view_draw_contracts_case},
    {"classic Step target survives GPU Draw and GUI",classic_retained_target_scene_case},
    {"ended resident frame remains visible and canonical",ended_frame_retention_case},
    {"authored cursor preserves a resident completed frame",resident_cursor_frame_case},
    {"transition snapshot reads a resident completed frame",resident_transition_snapshot_case},
    {"classic redraw and transition retain exact selected fallback",
     classic_temporal_fallback_case},
    {"resident GUI preserves direct and scaled CPU output",resident_indirect_gui_case},
    {"partial-alpha indirect GUI keeps ordered CPU fallback",
     resident_indirect_gui_alpha_fallback_case},
    {"two-view retained CPU seed stays current",resident_multiview_cpu_seed_case},
    {"two-view Step target survives Draw Begin and GUI",resident_multiview_step_target_case},
    {"assigned views repopulate a derived surface before restored GUI",
     resident_multiview_assigned_surface_restore_case},
    {"native assigned view accepts scene work and preserves restored output",
     resident_native_assigned_view_case},
    {"presentation geometry remains visible across selected scenes",
     presentation_geometry_case},
    {"forced owned scene retains geometry and narrow-room fallback",
     forced_owned_scene_case},
    {"CRT and box presentation preserve a resident scene through CPU filtering",
     presentation_filter_case},
    {"scene-only cold canvas retains CPU presentation without failed device work",
     scene_canvas_cpu_source_case},
    {"restored scene canvas fits a CPU-current image with GPU nearest",
     scene_canvas_restored_nearest_case},
    {"primary and auxiliary scratch uploads stay independent",content_sampler_upload_lifetime_case},
    {"surface shader vertices retain authored positions",content_surface_authored_position_case},
    {"terminal shader retains clipped authored positions",content_terminal_authored_position_case},
    {"texture filters and GPU stack restore",texture_filter_state_case},
    {"state bytes match with and without a target",state_bytes_match_case},
    {"save, load and save again produce the same bytes",save_load_save_case},
    {"a state crosses between a target and none",cross_load_case},
    {"CPU and scene GPU states cross in all four directions after context recreation",
     scene_state_transition_matrix_case},
    {"every frame serializes under a target",per_frame_serialization_case},
    {"the context lifecycle around state operations",context_lifecycle_around_state_case},
    {"a refused state leaves the engine as it was",rejected_state_is_transactional_case},
    {"readback_budget",readback_budget_case},
    {"a context lost while a frame is on it",context_lost_with_frame_pending_case},
    {"deferred retirement survives numeric name reuse after a lost context",
     context_replacement_reuses_names_case},
    {"two interleaved engines stay independent",interleaved_engines_case},
    {"two live GPU engines keep independent functions and resources",
     two_live_gpu_engines_case},
    {"a refused context leaves a working software engine",refused_context_case},
    {"neutral device binds and clears with context",neutral_device_binding_case},
    {"runtime font Reset gets a fresh device generation",runtime_font_reset_scene_case},
    {"content GLSL and hybrid presentation remain independent",independent_graphics_policy_case},
    {"a buffer too short for the frame still takes the state",short_buffer_drops_the_frame_case},
    {"an explicit resume state omits only the completed frame",explicit_resume_state_case},
    {"raw and repeated-row completed frames roundtrip",completed_frame_encodings_case},
    {"completed frame capacity retains exact fitting bytes",completed_frame_capacity_case},
    {"CRT preserves terminal content programs",crt_terminal_program_case},
  };
  int failed=0;
  for(size_t index=0;index<sizeof cases/sizeof cases[0];index++)
    if(!cases[index].run()){
      fprintf(stderr,"graphics state case failed: %s\n",cases[index].name);
      failed=1;
    }
  if(failed) return EXIT_FAILURE;
  printf("graphics state: ok\n");
  return EXIT_SUCCESS;
}
