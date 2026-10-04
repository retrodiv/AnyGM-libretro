/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Negotiating the frontend's graphics context and forwarding its lifecycle through the public
 * AnyGM surface.
 *
 * This file translates, and that is all it does: it holds the one ABI-level record the adapter is
 * allowed to own, and it hands the engine a framework-neutral context. It includes no renderer,
 * engine or GPU header, and it names no graphics API type — the frontend resolves entry points and
 * the engine asks for them through the callback. */
#include "libretro_internal.h"

#include <string.h>

static struct retro_hw_render_callback g_hardware;
static bool g_hardware_requested;
static bool g_hardware_adopted;

#ifdef _WIN32
typedef void *(__stdcall *HardwareGetCurrentFn)(void);
#else
typedef void *(*HardwareGetCurrentFn)(void);
#endif
static HardwareGetCurrentFn g_get_current_context;
static void *g_owning_context;

static uint32_t hardware_context_is_current(void *userdata){
  (void)userdata;
  return g_get_current_context && g_owning_context &&
         g_get_current_context()==g_owning_context;
}

static void hardware_resolve_context_access(void){
  static const char *const names[]={
#ifdef _WIN32
    "wglGetCurrentContext","eglGetCurrentContext"
#else
    "glXGetCurrentContext","eglGetCurrentContext"
#endif
  };
  g_get_current_context=NULL;
  g_owning_context=NULL;
  if(!g_hardware.get_proc_address) return;
  /* Resolve through the frontend, never through an ambient graphics loader.
   * These thread-local platform queries need no GL object or current context.
   * A missing getter keeps observation on the established CPU state path. */
  for(size_t index=0;index<sizeof names/sizeof names[0];index++){
    retro_proc_address_t proc=g_hardware.get_proc_address(names[index]);
    HardwareGetCurrentFn getter=NULL;
    if(!proc) continue;
    memcpy(&getter,&proc,sizeof getter);
    void *current=getter();
    if(current){
      g_get_current_context=getter;
      g_owning_context=current;
      return;
    }
  }
}

static AnygmGraphicsProc hardware_get_proc(void *userdata,const char *name){
  (void)userdata;
  if(!g_hardware.get_proc_address || !name) return NULL;
  return (AnygmGraphicsProc)g_hardware.get_proc_address(name);
}

/* Asked once per rendered frame rather than remembered: a frontend is free to hand over a
 * different target each time, and several do. */
static uintptr_t hardware_get_framebuffer(void *userdata){
  (void)userdata;
  if(!g_hardware.get_current_framebuffer) return 0;
  return (uintptr_t)g_hardware.get_current_framebuffer();
}

static void hardware_context_reset(void){
  AnygmGraphicsContext context;
  AnygmResult result;
  if(!g_libretro.engine) return;
  memset(&context,0,sizeof context);
  context.struct_size=sizeof context;
  context.api=g_hardware.context_type==RETRO_HW_CONTEXT_OPENGLES3
    ?ANYGM_GRAPHICS_OPENGLES3:ANYGM_GRAPHICS_OPENGL_CORE;
  context.version_major=g_hardware.version_major;
  context.version_minor=g_hardware.version_minor;
  context.userdata=NULL;
  context.get_proc_address=hardware_get_proc;
  context.get_current_framebuffer=hardware_get_framebuffer;
  hardware_resolve_context_access();
  if(g_get_current_context) context.is_current=hardware_context_is_current;
  result=anygm_graphics_context_reset(g_libretro.engine,&context);
  g_hardware_adopted=result==ANYGM_OK;
  libretro_log(RETRO_LOG_INFO,"[graphics-context-access] state_observation=%s\n",
               g_hardware_adopted && g_get_current_context?"owning-current":"CPU-fallback");
  if(!g_hardware_adopted){
    char error[512];
    anygm_get_last_error(g_libretro.engine,error,sizeof error);
    libretro_log(RETRO_LOG_WARN,
                 "Hardware rendering was accepted but could not be initialised (%d): %s\n",
                 result,error);
  }
  /* Frontends create the requested context only after retro_load_game returns. Keep authored boot
   * code behind that boundary so its first shader query and draw see the final outcome. */
  if(g_libretro.prepared && !g_libretro.loaded){
    libretro_options_finalize_graphics(g_hardware_adopted);
    if(!libretro_content_start())
      libretro_log(RETRO_LOG_ERROR,"Prepared content could not be started after context reset\n");
  }
}

static void hardware_context_destroy(void){
  if(!g_libretro.engine) return;
  /* Controlled teardown normally lends the context. An available exact-context witness is
   * stronger than that notification; never name old objects in an unbound or foreign context. */
  anygm_graphics_context_destroy(g_libretro.engine,
    !g_get_current_context || hardware_context_is_current(NULL)?1u:0u);
  g_hardware_adopted=false;
  g_get_current_context=NULL;
  g_owning_context=NULL;
}

static bool request_context(enum retro_hw_context_type type,unsigned major,unsigned minor){
  memset(&g_hardware,0,sizeof g_hardware);
  g_hardware.context_type=type;
  g_hardware.version_major=major;
  g_hardware.version_minor=minor;
  g_hardware.context_reset=hardware_context_reset;
  g_hardware.context_destroy=hardware_context_destroy;
  g_hardware.depth=false;
  g_hardware.stencil=false;
  /* Controlled destruction observes resident scene pixels first. Unexpected loss instead
   * invalidates volatile device-only surfaces and rebuilds immutable resources on next Draw;
   * neither path requires the frontend to retain the old namespace. */
  g_hardware.cache_context=false;
  g_hardware.debug_context=false;
  /* AnyGM addresses rows from the top and the backend writes them into a bottom-left framebuffer
   * accordingly, so the frontend must not flip again. An asymmetric fixture proves this rather
   * than the picture looking right. */
  g_hardware.bottom_left_origin=true;
  return g_libretro.environment &&
         g_libretro.environment(RETRO_ENVIRONMENT_SET_HW_RENDER,&g_hardware);
}

bool libretro_hw_render_request(bool needed){
  unsigned preferred=RETRO_HW_CONTEXT_NONE;
  bool prefers_embedded=false;
  g_hardware_requested=false;
  g_hardware_adopted=false;
  g_get_current_context=NULL;
  g_owning_context=NULL;
  memset(&g_hardware,0,sizeof g_hardware);
  /* A selector rather than a switch, so a second backend is a new value beside this one. Match the
   * chosen backend exactly: a value this build does not implement -- a newer core's setting read
   * back by an older one -- must fall to the software renderer, never to whichever backend
   * happens to exist here. */
  if(!needed) return false;
  if(!g_libretro.environment){
    libretro_log(RETRO_LOG_WARN,"The selected graphics features need a frontend environment callback\n");
    return false;
  }
  if(g_libretro.environment(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER,&preferred))
    prefers_embedded=preferred==RETRO_HW_CONTEXT_OPENGLES2 ||
                     preferred==RETRO_HW_CONTEXT_OPENGLES3 ||
                     preferred==RETRO_HW_CONTEXT_OPENGLES_VERSION;
  /* Ask for what the frontend said it prefers, then for the other family. A frontend that prefers
   * an API this backend does not implement can still usually provide one of these two. */
  if(prefers_embedded){
    g_hardware_requested=request_context(RETRO_HW_CONTEXT_OPENGLES3,3,0) ||
                         request_context(RETRO_HW_CONTEXT_OPENGL_CORE,3,3);
  } else {
    g_hardware_requested=request_context(RETRO_HW_CONTEXT_OPENGL_CORE,3,3) ||
                         request_context(RETRO_HW_CONTEXT_OPENGLES3,3,0);
  }
  if(!g_hardware_requested){
    memset(&g_hardware,0,sizeof g_hardware);
    /* One message, then ordinary software rendering. This is a supported outcome, not a failure. */
    libretro_log(RETRO_LOG_INFO,
                 "The frontend did not provide an OpenGL or OpenGL ES context; "
                 "continuing with the software renderer\n");
  }
  return g_hardware_requested;
}

void libretro_hw_render_release(void){
  if(g_libretro.engine && g_hardware_adopted){
    /* Content is going away while the context may not be current. Forget the handles rather than
     * naming them in a context that is not there. */
    anygm_graphics_context_destroy(g_libretro.engine,0u);
  }
  g_hardware_adopted=false;
  g_hardware_requested=false;
  g_get_current_context=NULL;
  g_owning_context=NULL;
  memset(&g_hardware,0,sizeof g_hardware);
}

bool libretro_hw_render_requested(void){ return g_hardware_requested; }
