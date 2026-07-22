/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_bytecode.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void write_u32(uint8_t *data,size_t offset,uint32_t value){
  data[offset]=(uint8_t)value;
  data[offset+1]=(uint8_t)(value>>8);
  data[offset+2]=(uint8_t)(value>>16);
  data[offset+3]=(uint8_t)(value>>24);
}

static int is_zero(const GmlInsn *instruction){
  GmlInsn zero;
  memset(&zero,0,sizeof zero);
  return !memcmp(instruction,&zero,sizeof zero);
}

static int exhaustive_truncation_corpus(void){
  uint8_t bytes[12]={0};
  for(uint8_t bytecode=14;bytecode<=15;bytecode++){
    for(unsigned opcode=0;opcode<=255;opcode++){
      memset(bytes,0,sizeof bytes);
      write_u32(bytes,0,(uint32_t)opcode<<24);
      for(size_t available=0;available<=sizeof bytes;available++){
        GmlInsn instruction;
        memset(&instruction,0xA5,sizeof instruction);
        int consumed=gml_decode_bc_bounded(bytes,available,0,bytecode,&instruction);
        if(consumed<0 || (consumed && (size_t)consumed>available) ||
           (!consumed && !is_zero(&instruction))){
          fprintf(stderr,"bounded decode invariant failed: bc=%u opcode=%u available=%zu\n",
                  bytecode,opcode,available);
          return 0;
        }
      }
    }
  }
  return 1;
}

static int equivalence_corpus(void){
  uint8_t data[16];
  uint32_t state=0xC001D00Du;
  for(uint8_t bytecode=14;bytecode<=15;bytecode++){
    for(unsigned sample=0;sample<8192;sample++){
      for(size_t i=0;i<sizeof data;i++){
        state=state*1664525u+1013904223u;
        data[i]=(uint8_t)(state>>24);
      }
      GmlInsn legacy,bounded;
      int legacy_size=gml_decode_bc(data,4,bytecode,&legacy);
      int bounded_size=gml_decode_bc_bounded(data,sizeof data,4,bytecode,&bounded);
      if(legacy_size!=bounded_size ||
         (legacy_size && memcmp(&legacy,&bounded,sizeof legacy)) ||
         (!bounded_size && !is_zero(&bounded))){
        fprintf(stderr,"bounded decoder changed a complete instruction: bc=%u sample=%u\n",
                bytecode,sample);
        return 0;
      }
    }
  }
  return 1;
}

static int exact_operand_boundaries(void){
  uint8_t data[20]={0};
  GmlInsn instruction;
  write_u32(data,4,(uint32_t)OP_PUSH<<24 | (uint32_t)DT_DOUBLE<<16);
  for(size_t size=0;size<16;size++){
    memset(&instruction,0xA5,sizeof instruction);
    if(gml_decode_bc_bounded(data,size,4,15,&instruction)!=0 || !is_zero(&instruction))
      return 0;
  }
  if(gml_decode_bc_bounded(data,16,4,15,&instruction)!=12) return 0;

  memset(data,0,sizeof data);
  write_u32(data,4,(uint32_t)OP_CALL<<24);
  if(gml_decode_bc_bounded(data,11,4,15,&instruction)!=0 ||
     gml_decode_bc_bounded(data,12,4,15,&instruction)!=8 || instruction.refaddr!=8u)
    return 0;

  memset(&instruction,0xA5,sizeof instruction);
  if(gml_decode_bc_bounded(NULL,0,0,15,&instruction)!=0 || !is_zero(&instruction) ||
     gml_decode_bc_bounded(data,sizeof data,UINT32_MAX,15,&instruction)!=0 ||
     !is_zero(&instruction) ||
     gml_decode_bc_bounded(data,sizeof data,0,15,NULL)!=0) return 0;
  return 1;
}

int main(void){
  if(!exhaustive_truncation_corpus() || !equivalence_corpus() ||
     !exact_operand_boundaries()) return 1;
  puts("bounded bytecode corpus: ok");
  return 0;
}
