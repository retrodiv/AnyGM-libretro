/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"

static_assert(ANYGM_API_VERSION==2u,"unexpected API version");
static_assert(ANYGM_STATE_SCHEMA==11u,"unexpected state schema");

int anygm_public_header_cpp_probe(){
  AnygmHostServices services{};
  AnygmGraphicsContext graphics{};
  AnygmEngine *engine=nullptr;
  services.struct_size=sizeof services;
  graphics.struct_size=sizeof graphics;
  graphics.api=ANYGM_GRAPHICS_OPENGL_CORE;
  if(anygm_create(&services,&engine)!=ANYGM_OK) return 0;
  /* A context with no callbacks is refused rather than adopted, and the engine stays usable. */
  return anygm_graphics_context_reset(engine,&graphics)==ANYGM_OK?0:1;
}
