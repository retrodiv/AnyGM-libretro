/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"

_Static_assert(ANYGM_API_VERSION==1u,"unexpected API version");
_Static_assert(ANYGM_STATE_SCHEMA==8u,"unexpected state schema");

int anygm_public_header_c_probe(void){
  AnygmHostServices services={0};
  AnygmContentSource source={0};
  services.struct_size=sizeof services;
  source.struct_size=sizeof source;
  return (int)(services.struct_size+source.struct_size);
}
