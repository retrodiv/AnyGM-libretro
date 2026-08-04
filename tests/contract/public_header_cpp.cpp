/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"

static_assert(ANYGM_API_VERSION==1u,"unexpected API version");
static_assert(ANYGM_STATE_SCHEMA==4u,"unexpected state schema");

int anygm_public_header_cpp_probe(){
  AnygmHostServices services{};
  AnygmEngine *engine=nullptr;
  services.struct_size=sizeof services;
  return anygm_create(&services,&engine)==ANYGM_OK?1:0;
}
