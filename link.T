/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */

/* Export only the libretro API. Keeping internal symbols local prevents
 * host-process symbol collisions and interposition. */
{
  global:
    retro_*;
  local:
    *;
};
