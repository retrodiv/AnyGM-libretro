/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"

static_assert(ANYGM_API_VERSION==3u,"unexpected API version");
static_assert(ANYGM_STATE_SCHEMA==12u,"unexpected state schema");

int anygm_public_header_cpp_probe(){
  AnygmHostServices services{};
  AnygmGraphicsContext graphics{};
  AnygmEngine *engine=nullptr;
  auto resume_size=&anygm_state_resume_size;
  auto resume_save=&anygm_state_save_for_resume;
  services.struct_size=sizeof services;
  graphics.struct_size=sizeof graphics;
  graphics.api=ANYGM_GRAPHICS_OPENGL_CORE;
  (void)resume_size;
  (void)resume_save;
  if(anygm_create(&services,&engine)!=ANYGM_OK) return 0;
  /* A context with no callbacks is refused rather than adopted, and the engine stays usable. */
  return anygm_graphics_context_reset(engine,&graphics)==ANYGM_OK?0:1;
}
