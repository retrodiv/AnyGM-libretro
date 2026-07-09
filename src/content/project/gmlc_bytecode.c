/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_bytecode.h"
#include <stdlib.h>
#include <string.h>

int gmlc_bytecode_emit_empty(GmlcCodeBlob *out){
  memset(out,0,sizeof(*out));
  out->data=(uint8_t*)malloc(4);
  if(!out->data) return 0;
  out->data[0]=0x9D; out->data[1]=0; out->data[2]=0; out->data[3]=0;
  out->size=4;
  return 1;
}

void gmlc_bytecode_free(GmlcCodeBlob *b){
  if(!b) return;
  free(b->data);
  memset(b,0,sizeof(*b));
}
