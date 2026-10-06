/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include <iostream>

/* Intentionally retains global console-stream destruction for the probe's
 * negative control. This fixture is never linked into the product. */
extern "C" __declspec(dllexport) unsigned lifetime_negative_control(void) {
  return 1;
}
