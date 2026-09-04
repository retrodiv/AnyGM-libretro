/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */

/* Keep the repository's notice texts in the compiled core as well as beside
 * the source. A distributor must still present the applicable notices in a
 * form that satisfies their licenses. The externally linked accessor retains
 * the internal table; the version script hides the accessor from the ABI. */

#include <stddef.h>
#include <stdint.h>

#include "anygm_third_party_notices.h"

const uint8_t *anygm_third_party_notices(size_t *size);

const uint8_t *anygm_third_party_notices(size_t *size){
  if(size) *size=sizeof anygm_third_party_notices_data;
  return anygm_third_party_notices_data;
}
