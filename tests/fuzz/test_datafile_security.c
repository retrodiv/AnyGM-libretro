/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_win.h"
#include "synthetic_content.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t header;
  size_t body;
  size_t size;
} TestChunk;

static uint32_t read_u32(const uint8_t *data,size_t offset){
  return (uint32_t)data[offset] | (uint32_t)data[offset+1]<<8 |
         (uint32_t)data[offset+2]<<16 | (uint32_t)data[offset+3]<<24;
}

static void write_u32(uint8_t *data,size_t offset,uint32_t value){
  data[offset]=(uint8_t)value;
  data[offset+1]=(uint8_t)(value>>8);
  data[offset+2]=(uint8_t)(value>>16);
  data[offset+3]=(uint8_t)(value>>24);
}

static int find_chunk(const uint8_t *data,size_t size,const char name[4],TestChunk *out){
  if(!data || !out || size<8 || memcmp(data,"FORM",4) || read_u32(data,4)!=size-8u)
    return 0;
  size_t offset=8;
  while(offset<size){
    if(offset>size || 8u>size-offset) return 0;
    uint32_t chunk_size=read_u32(data,offset+4);
    size_t body=offset+8u;
    if(chunk_size>size-body) return 0;
    if(!memcmp(data+offset,name,4)){
      out->header=offset;
      out->body=body;
      out->size=chunk_size;
      return 1;
    }
    offset=body+chunk_size;
  }
  return 0;
}

static uint8_t *copy_bytes(const uint8_t *data,size_t size){
  uint8_t *copy=(uint8_t *)malloc(size?size:1u);
  if(copy && size) memcpy(copy,data,size);
  return copy;
}

static int rejected(const char *label,uint8_t *data,size_t size,int owns){
  GmlWin win,zero;
  memset(&win,0xA5,sizeof win);
  memset(&zero,0,sizeof zero);
  int result=gml_win_from_mem(&win,data,size,owns);
  if(result==0){
    fprintf(stderr,"accepted invalid normalized image: %s\n",label);
    gml_win_free(&win);
    return 0;
  }
  if(memcmp(&win,&zero,sizeof win)){
    fprintf(stderr,"failed parse retained partial state: %s\n",label);
    return 0;
  }
  return 1;
}

static int reject_u32(const uint8_t *source,size_t size,size_t offset,uint32_t value,
                      const char *label){
  if(offset>size || 4u>size-offset) return 0;
  uint8_t *copy=copy_bytes(source,size);
  if(!copy) return 0;
  write_u32(copy,offset,value);
  int ok=rejected(label,copy,size,0);
  free(copy);
  return ok;
}

static int reject_byte(const uint8_t *source,size_t size,size_t offset,uint8_t value,
                       const char *label){
  if(offset>=size) return 0;
  uint8_t *copy=copy_bytes(source,size);
  if(!copy) return 0;
  copy[offset]=value;
  int ok=rejected(label,copy,size,0);
  free(copy);
  return ok;
}

static int fixed_container_cases(void){
  uint8_t short_header[12]={0};
  memcpy(short_header,"FORM",4);
  write_u32(short_header,4,4);
  if(!rejected("truncated chunk header",short_header,sizeof short_header,0)) return 0;

  uint8_t overflow[16]={0};
  memcpy(overflow,"FORM",4);
  write_u32(overflow,4,8);
  memcpy(overflow+8,"DATA",4);
  write_u32(overflow,12,UINT32_MAX);
  if(!rejected("overflowing chunk size",overflow,sizeof overflow,0)) return 0;

  uint8_t duplicate[24]={0};
  memcpy(duplicate,"FORM",4);
  write_u32(duplicate,4,16);
  memcpy(duplicate+8,"DATA",4);
  memcpy(duplicate+16,"DATA",4);
  if(!rejected("duplicate chunk identity",duplicate,sizeof duplicate,0)) return 0;

  uint8_t chunks[8u+(GML_WIN_MAX_CHUNKS+1u)*8u];
  memset(chunks,0,sizeof chunks);
  memcpy(chunks,"FORM",4);
  write_u32(chunks,4,(uint32_t)(sizeof chunks-8u));
  for(uint32_t i=0;i<=GML_WIN_MAX_CHUNKS;i++){
    size_t header=8u+(size_t)i*8u;
    chunks[header]=(uint8_t)i;
    chunks[header+1]=(uint8_t)(i>>8);
    chunks[header+2]='C';
    chunks[header+3]='H';
  }
  if(!rejected("chunk count limit",chunks,sizeof chunks,0)) return 0;

  uint8_t short_gen8[20]={0};
  memcpy(short_gen8,"FORM",4);
  write_u32(short_gen8,4,12);
  memcpy(short_gen8+8,"GEN8",4);
  write_u32(short_gen8,12,4);
  if(!rejected("short GEN8 record",short_gen8,sizeof short_gen8,0)) return 0;
  return 1;
}

static int reference_mutations(const uint8_t *data,size_t size,const TestChunk *vari,
                               const TestChunk *func){
  if(func && func->size>=4 &&
     !reject_u32(data,size,func->body,UINT32_MAX,"FUNC record count")) return 0;
  if(!vari || vari->size<32) return 1;
  size_t records=(vari->size-12u)/20u;
  if(!records) return 1;
  size_t first=vari->body+12u;
  if(!reject_u32(data,size,first,0,"reference name pointer")) return 0;
  if(!reject_u32(data,size,first+12u,GML_WIN_MAX_REFERENCES+1u,
                 "reference occurrence limit")) return 0;
  for(size_t i=0;i<records;i++){
    size_t record=first+i*20u;
    uint32_t occurrences=read_u32(data,record+12u);
    uint32_t address=read_u32(data,record+16u);
    if(!occurrences) continue;
    if(!reject_u32(data,size,record+16u,UINT32_MAX,"reference address")) return 0;
    if(address<=size && 8u<=size-address){
      uint8_t *copy=copy_bytes(data,size);
      if(!copy) return 0;
      write_u32(copy,record+12u,2u);
      write_u32(copy,(size_t)address+4u,0u);
      int ok=rejected("truncated reference chain",copy,size,0);
      free(copy);
      if(!ok) return 0;
    }
    break;
  }
  return 1;
}

static int structured_mutations(const uint8_t *data,size_t size){
  TestChunk strg={0},code={0},gen8={0},room={0},vari={0},func={0},classic={0};
  if(!find_chunk(data,size,"STRG",&strg) || !find_chunk(data,size,"CODE",&code) ||
     !find_chunk(data,size,"GEN8",&gen8) || !find_chunk(data,size,"ROOM",&room)) return 0;

  if(!reject_u32(data,size,4,(uint32_t)(size-9u),"FORM total size") ||
     !reject_u32(data,size,strg.header+4u,UINT32_MAX,"chunk body size") ||
     !reject_u32(data,size,strg.body,GML_WIN_MAX_STRINGS+1u,"STRG count")) return 0;
  uint32_t string_record=read_u32(data,strg.body+4u);
  uint32_t string_length=read_u32(data,string_record);
  if(!reject_u32(data,size,strg.body+4u,UINT32_MAX,"STRG record pointer") ||
     !reject_u32(data,size,string_record,GML_WIN_MAX_STRING_BYTES+1u,"string length") ||
     !reject_byte(data,size,(size_t)string_record+4u+string_length,'X',"missing string terminator"))
    return 0;
  if(string_length && !reject_byte(data,size,(size_t)string_record+4u,0,"embedded string NUL"))
    return 0;

  if(!reject_u32(data,size,code.body,GML_WIN_MAX_CODE_ENTRIES+1u,"CODE count")) return 0;
  uint32_t code_record=read_u32(data,code.body+4u);
  int64_t table_relative=(int64_t)(code.body+4u)-
                         (int64_t)((size_t)code_record+12u);
  if(!reject_u32(data,size,code.body+4u,UINT32_MAX,"CODE record pointer") ||
     !reject_u32(data,size,code_record,0,"CODE name pointer") ||
     !reject_u32(data,size,(size_t)code_record+4u,UINT32_MAX,"CODE length") ||
     !reject_u32(data,size,(size_t)code_record+12u,(uint32_t)(int32_t)table_relative,
                 "CODE span overlaps its table") ||
     !reject_u32(data,size,(size_t)code_record+12u,0x7FFFFFFFu,"CODE start") ||
     !reject_u32(data,size,(size_t)code_record+16u,UINT32_MAX,"CODE child offset")) return 0;

  if(!reject_u32(data,size,room.body,GML_WIN_MAX_ROOMS+1u,"ROOM count")) return 0;
  uint32_t room_record=read_u32(data,room.body+4u);
  if(!reject_u32(data,size,room.body+4u,UINT32_MAX,"ROOM record pointer") ||
     !reject_u32(data,size,room_record,0,"ROOM name pointer")) return 0;
  if(!reject_u32(data,size,gen8.body+128u,UINT32_MAX,"room-order count")) return 0;
  if(gen8.size>=136u &&
     !reject_u32(data,size,gen8.body+132u,read_u32(data,room.body),"room-order index")) return 0;

  TestChunk *vari_ptr=find_chunk(data,size,"VARI",&vari)?&vari:NULL;
  TestChunk *func_ptr=find_chunk(data,size,"FUNC",&func)?&func:NULL;
  if(!reference_mutations(data,size,vari_ptr,func_ptr)) return 0;

  if(find_chunk(data,size,"CLSC",&classic) && classic.size>=24u &&
     !reject_u32(data,size,classic.body+20u,GML_WIN_MAX_CLASSIC_INFO_BYTES+1u,
                 "classic information size")) return 0;
  return 1;
}

int main(void){
  if(!fixed_container_cases()) return 1;
  AnygmSyntheticContent fixture;
  uint8_t *data=NULL;
  size_t size=0;
  if(!anygm_synthetic_content_create(&fixture) ||
     !anygm_synthetic_content_read(&fixture,&data,&size)){
    fprintf(stderr,"could not create normalized security fixture\n");
    anygm_synthetic_content_destroy(&fixture);
    return 1;
  }

  GmlWin win;
  if(gml_win_from_mem(&win,data,size,0)!=0 || win.n_code<=0 || gml_room_count(&win)!=1){
    fprintf(stderr,"valid normalized fixture was rejected\n");
    free(data);
    anygm_synthetic_content_destroy(&fixture);
    return 1;
  }
  GmlRoom first_room;
  int valid_room=gml_room_get(&win,0,&first_room)==0 && first_room.name;
  gml_win_free(&win);
  if(!valid_room){
    fprintf(stderr,"valid room table was rejected\n");
    free(data);
    anygm_synthetic_content_destroy(&fixture);
    return 1;
  }

  for(size_t cut=0;cut<size;cut++)
    if(!rejected("truncated normalized image",data,cut,0)){
      free(data);
      anygm_synthetic_content_destroy(&fixture);
      return 1;
    }
  if(!structured_mutations(data,size)){
    free(data);
    anygm_synthetic_content_destroy(&fixture);
    return 1;
  }

  TestChunk code;
  uint8_t *owned=copy_bytes(data,size);
  if(!owned || !find_chunk(owned,size,"CODE",&code)){
    free(owned);
    free(data);
    anygm_synthetic_content_destroy(&fixture);
    return 1;
  }
  write_u32(owned,code.body,GML_WIN_MAX_CODE_ENTRIES+1u);
  if(!rejected("owned partial parse",owned,size,1)){
    free(data);
    anygm_synthetic_content_destroy(&fixture);
    return 1;
  }
  owned[size-1]^=1u;
  free(owned);

  free(data);
  anygm_synthetic_content_destroy(&fixture);
  puts("bounded normalized-data corpus: ok");
  return 0;
}
