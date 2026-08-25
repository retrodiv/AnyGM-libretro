/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"

_Static_assert(ANYGM_API_VERSION==3u,"unexpected API version");
_Static_assert(ANYGM_STATE_SCHEMA==16u,"unexpected state schema");
/* The optional graphics seam is framework-neutral by construction: a host describes its context
 * with fixed-width integers, an opaque userdata pointer and two callbacks, and the header names no
 * graphics API type for either of them. */
_Static_assert(sizeof(AnygmGraphicsProc)==sizeof(void(*)(void)),
               "graphics entry points are plain function pointers");

int anygm_public_header_c_probe(void){
  AnygmHostServices services={0};
  AnygmContentSource source={0};
  AnygmGraphicsContext graphics={0};
  size_t (*resume_size)(AnygmEngine*)=anygm_state_resume_size;
  AnygmResult (*resume_save)(AnygmEngine*,void*,size_t,size_t*)=anygm_state_save_for_resume;
  services.struct_size=sizeof services;
  source.struct_size=sizeof source;
  graphics.struct_size=sizeof graphics;
  graphics.api=ANYGM_GRAPHICS_OPENGLES3;
  (void)resume_size;
  (void)resume_save;
  return (int)(services.struct_size+source.struct_size+graphics.struct_size);
}
