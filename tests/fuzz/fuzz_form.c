/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_win.h"

int LLVMFuzzerTestOneInput(const uint8_t *data,size_t size){
  GmlWin win;
  if(!gml_win_from_mem(&win,(uint8_t *)data,size,0)) gml_win_free(&win);
  return 0;
}
