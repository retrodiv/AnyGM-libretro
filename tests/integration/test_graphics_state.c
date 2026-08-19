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
#include "graphics_driver_fixture.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"

#include <stdio.h>
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

static int session_open(Session *session){
  AnygmContentSource source={0};
  memset(session,0,sizeof *session);
  if(!anygm_synthetic_framebuffer_content_create(&session->fixture)) return 0;
  session->services.struct_size=sizeof session->services;
  session->services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&session->services);
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=session->fixture.path;
  source.cache_directory=session->fixture.directory;
  source.save_directory=session->fixture.directory;
  if(anygm_create(&session->services,&session->engine)!=ANYGM_OK) return 0;
  return anygm_load(session->engine,&source,NULL)==ANYGM_OK;
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

static uint8_t *serialize(AnygmEngine *engine,size_t *size){
  size_t capacity=anygm_state_size(engine);
  uint8_t *data=(uint8_t*)malloc(capacity?capacity:1);
  size_t written=0;
  if(!data) return NULL;
  if(anygm_state_save(engine,data,capacity,&written)!=ANYGM_OK || written!=capacity){
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

/* An unusable context leaves a fully working software engine, and destroying an engine that never
 * had a current context issues no graphics call at all. */
static int refused_context_case(void){
  Session session;
  AnygmGraphicsContext context;
  int ok;
  anygm_test_graphics_reset();
  REQUIRE(session_open(&session),"the session loads");
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
  {
    uint32_t flags=0;
    ok=advance(session.engine,4,&flags);
    ok=ok && (flags&ANYGM_FRAME_HARDWARE_TARGET)==0;
  }
  if(!ok) fputs("a refused context did not leave a working software engine\n",stderr);
  /* Adopting one and then tearing the engine down without saying the context is current. */
  ok=ok && graphics_adopt(session.engine,ANYGM_GRAPHICS_OPENGLES3)==ANYGM_OK;
  ok=ok && advance(session.engine,1,NULL);
  anygm_test_graphics_forbid_calls();
  session_close(&session);
  ok=ok && anygm_test_graphics_calls_after_forget()==0;
  if(!ok) fputs("destroying an engine outside a current context issued a graphics call\n",stderr);
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

int main(void){
  static const struct { const char *name; int (*run)(void); } cases[]={
    {"state bytes match with and without a target",state_bytes_match_case},
    {"save, load and save again produce the same bytes",save_load_save_case},
    {"a state crosses between a target and none",cross_load_case},
    {"every frame serializes under a target",per_frame_serialization_case},
    {"the context lifecycle around state operations",context_lifecycle_around_state_case},
    {"a refused state leaves the engine as it was",rejected_state_is_transactional_case},
    {"a context lost while a frame is on it",context_lost_with_frame_pending_case},
    {"two interleaved engines stay independent",interleaved_engines_case},
    {"a refused context leaves a working software engine",refused_context_case},
    {"a buffer too short for the frame still takes the state",short_buffer_drops_the_frame_case},
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
