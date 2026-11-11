/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_win.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void word(uint8_t *data,size_t offset,uint32_t value){
  for(unsigned i=0;i<4;i++) data[offset+i]=(uint8_t)(value>>(8*i));
}
static int expect(const char *label,int actual,int wanted){
  if(actual==wanted) return 1;
  fprintf(stderr,"array owner %s: expected %d, got %d\n",label,wanted,actual);
  return 0;
}
int main(void){
  uint8_t data[24]={0};
  GmlCode code[3]={{0}};
  GmlWin content={0}; content.bytecode=17;
  content.data=data; content.size=sizeof data;
  content.code=code; content.n_code=1;
  uint32_t owner=((uint32_t)OP_BREAK<<24)|((uint32_t)DT_INT16<<16)|UINT32_C(0xfffb);
  word(data,0,((uint32_t)OP_PUSH<<24)|((uint32_t)DT_INT32<<16));
  word(data,4,owner); /* Numeric payload, never an instruction. */
  word(data,8,(uint32_t)OP_EXIT<<24);
  code[0].length=12;
  int ok=expect("pattern in integer payload",gml_win_has_array_owner_operations(&content),0);
  word(data,12,owner);
  word(data,16,(uint32_t)OP_EXIT<<24);
  code[0].length=20;
  ok &= expect("actual operation",gml_win_has_array_owner_operations(&content),1);
  code[1].start=8; code[1].length=12;
  code[2].start=12; code[2].length=8;
  content.n_code=3;
  ok &= expect("overlapping child spans",gml_win_has_array_owner_operations(&content),1);
  GmlCode swap=code[0]; code[0]=code[2]; code[2]=swap;
  ok &= expect("unsorted child spans",gml_win_has_array_owner_operations(&content),1);
  content.n_code=1; code[0].start=0; code[0].length=4;
  ok &= expect("truncated operand",gml_win_has_array_owner_operations(&content),-1);
  code[1].start=4; code[1].length=8; content.n_code=2;
  ok &= expect("adjacent entries cannot complete an operand",gml_win_has_array_owner_operations(&content),-1);
  content.n_code=1; code[0].start=24; code[0].length=4;
  ok &= expect("out of bounds",gml_win_has_array_owner_operations(&content),-1);
  code[0].start=0; code[0].length=20;
  word(data,12,((uint32_t)OP_BREAK<<24)|((uint32_t)DT_INT16<<16)|UINT32_C(0xfff8));
  ok &= expect("another extended operation",gml_win_has_array_owner_operations(&content),0);
  content.bytecode=16;
  ok &= expect("earlier encoding needs no owner marker",gml_win_has_array_owner_operations(&content),0);
  content.bytecode=17; content.n_code=0;
  ok &= expect("no code",gml_win_has_array_owner_operations(&content),0);
  if(ok) puts("array owner metadata: ten controls passed");
  return ok?0:1;
}
