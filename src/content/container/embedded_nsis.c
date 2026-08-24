/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 *
 * Bounded recognition and transactional extraction for an NSIS 2 profile:
 * ANSI instruction tables, non-solid storage, and raw Deflate.
 */
#include "embedded_nsis.h"

#include "content_router.h"
#include "anygm_vfs.h"
#include "gml_image_codec.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#define NSIS_SCAN_LIMIT (20u*1024u*1024u)
#define NSIS_HEADER_LIMIT (16u*1024u*1024u)
#define NSIS_FIRST_HEADER_SIZE 28u
#define NSIS_ENTRY_SIZE 28u
#define NSIS_OPCODE_CREATEDIR 11u
#define NSIS_OPCODE_EXTRACTFILE 20u
#define NSIS_FLAG_NO_CRC 4u
#define NSIS_FLAGS_MASK 15u

typedef struct NsisMember {
  char path[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
  uint32_t data_offset;
} NsisMember;

typedef struct NsisPlan {
  NsisMember *members;
  size_t count;
  size_t capacity;
  char payload[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
} NsisPlan;

static uint32_t nsis_u32(const uint8_t *p){
  return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}

static uint64_t nsis_hash(const void *data,size_t size){
  const uint8_t *bytes=(const uint8_t *)data;
  uint64_t hash=UINT64_C(1469598103934665603);
  for(size_t index=0;index<size;index++){
    hash^=bytes[index];
    hash*=UINT64_C(1099511628211);
  }
  return hash;
}

static void nsis_log(const AnygmContentRouter *router,int level,const char *format,...){
  if(!router || !router->log) return;
  char message[1024];
  va_list arguments;
  va_start(arguments,format);
  vsnprintf(message,sizeof message,format,arguments);
  va_end(arguments);
  router->log(router->log_userdata,level,message);
}

static uint32_t nsis_crc32(const uint8_t *data,size_t size){
  uint32_t table[256];
  for(uint32_t value=0;value<256u;value++){
    uint32_t item=value;
    for(unsigned bit=0;bit<8u;bit++) item=(item>>1)^(UINT32_C(0xedb88320)&(0u-(item&1u)));
    table[value]=item;
  }
  uint32_t crc=UINT32_MAX;
  for(size_t index=0;index<size;index++) crc=table[(crc^data[index])&255u]^(crc>>8);
  return crc^UINT32_MAX;
}

static int nsis_header_shape(const uint8_t *header,size_t size){
  if(!header || size<60u) return 0;
  uint32_t entries_offset=nsis_u32(header+20u);
  uint32_t entries=nsis_u32(header+24u);
  uint32_t strings_offset=nsis_u32(header+28u);
  uint32_t languages_offset=nsis_u32(header+36u);
  if(entries>ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES || entries_offset>size ||
     entries>(size-entries_offset)/NSIS_ENTRY_SIZE ||
     strings_offset!=entries_offset+(uint64_t)entries*NSIS_ENTRY_SIZE ||
     languages_offset<strings_offset || languages_offset>size) return 0;
  return 1;
}

static int nsis_inflate_exact(const uint8_t *packed,size_t packed_size,
                              uint8_t *output,size_t output_size){
  size_t actual=0;
  return gml_deflate_decode_nsis_to_buffer(packed,packed_size,output,output_size,&actual) &&
         actual==output_size;
}

static int nsis_candidate(const uint8_t *source,size_t source_size,size_t offset,
                          AnygmEmbeddedNsis *result){
  static const uint8_t signature[16]={
    0xef,0xbe,0xad,0xde,0x4e,0x75,0x6c,0x6c,
    0x73,0x6f,0x66,0x74,0x49,0x6e,0x73,0x74
  };
  if(offset>source_size-NSIS_FIRST_HEADER_SIZE ||
     memcmp(source+offset+4u,signature,sizeof signature)) return 0;
  uint32_t flags=nsis_u32(source+offset);
  uint32_t header_size=nsis_u32(source+offset+20u);
  uint32_t archive_size=nsis_u32(source+offset+24u);
  if((flags&~NSIS_FLAGS_MASK) || header_size<60u || header_size>NSIS_HEADER_LIMIT ||
     archive_size<NSIS_FIRST_HEADER_SIZE+4u || offset>source_size-archive_size ||
     offset+(uint64_t)archive_size!=source_size) return -1;
  uint32_t packed_word=nsis_u32(source+offset+NSIS_FIRST_HEADER_SIZE);
  if(!(packed_word&UINT32_C(0x80000000))) return -2;
  uint32_t packed_size=packed_word&UINT32_C(0x7fffffff);
  uint64_t data_offset=offset+NSIS_FIRST_HEADER_SIZE+4u+(uint64_t)packed_size;
  uint64_t data_end=offset+(uint64_t)archive_size-((flags&NSIS_FLAG_NO_CRC)?0u:4u);
  if(!packed_size || data_offset>data_end) return -1;
  uint8_t *header=(uint8_t *)malloc(header_size);
  if(!header) return -1;
  int ok=nsis_inflate_exact(source+offset+NSIS_FIRST_HEADER_SIZE+4u,packed_size,
                            header,header_size) && nsis_header_shape(header,header_size);
  free(header);
  if(!ok) return -1;
  if(!(flags&NSIS_FLAG_NO_CRC)){
    if(source_size<516u || nsis_u32(source+source_size-4u)!=
       nsis_crc32(source+512u,source_size-516u)) return -1;
  }
  memset(result,0,sizeof *result);
  result->source_size=source_size;
  result->source_hash=nsis_hash(source,source_size);
  result->header_offset=offset;
  result->archive_size=archive_size;
  result->data_offset=data_offset;
  result->data_size=data_end-data_offset;
  result->header_size=header_size;
  result->header_packed_size=packed_size;
  result->flags=flags;
  return 1;
}

AnygmEmbeddedNsisStatus anygm_embedded_nsis_probe(const AnygmContentRouter *router,
                                                   const char *path,
                                                   AnygmEmbeddedNsis *nsis){
  if(nsis) memset(nsis,0,sizeof *nsis);
  if(!router || !path || !nsis || !anygm_vfs_can_read(router->host))
    return ANYGM_EMBEDDED_NSIS_INVALID;
  uint8_t *source=NULL;
  size_t size=0;
  if(!anygm_vfs_read_all(router->host,path,&source,&size,
                         (size_t)ANYGM_CONTENT_MAX_EXECUTABLE_BYTES)) return ANYGM_EMBEDDED_NSIS_INVALID;
  if(size<NSIS_FIRST_HEADER_SIZE || source[0]!='M' || source[1]!='Z'){
    free(source);
    return ANYGM_EMBEDDED_NSIS_NOT_FOUND;
  }
  static const uint8_t prefix[4]={0xef,0xbe,0xad,0xde};
  size_t limit=size<NSIS_SCAN_LIMIT?size:NSIS_SCAN_LIMIT;
  unsigned valid=0,unsupported=0,invalid=0;
  AnygmEmbeddedNsis found={0};
  for(size_t at=4u;at+16u<=limit;at++){
    if(memcmp(source+at,prefix,sizeof prefix)) continue;
    AnygmEmbeddedNsis candidate={0};
    int status=nsis_candidate(source,size,at-4u,&candidate);
    if(status>0){ found=candidate; valid++; }
    else if(status==-2) unsupported++;
    else if(status<0) invalid++;
  }
  free(source);
  if(valid==1u){ *nsis=found; return ANYGM_EMBEDDED_NSIS_SUPPORTED; }
  if(valid>1u || invalid) return ANYGM_EMBEDDED_NSIS_INVALID;
  if(unsupported) return ANYGM_EMBEDDED_NSIS_UNSUPPORTED;
  return ANYGM_EMBEDDED_NSIS_NOT_FOUND;
}

static int nsis_literal_string(const uint8_t *header,size_t header_size,uint32_t strings_offset,
                               uint32_t strings_end,int32_t pointer,char *output,size_t capacity,
                               int directory){
  (void)header_size;
  if(pointer<0 || (uint32_t)pointer>=strings_end-strings_offset || !capacity) return 0;
  const uint8_t *start=header+strings_offset+(uint32_t)pointer;
  const uint8_t *end=memchr(start,0,(size_t)(header+strings_end-start));
  if(!end) return 0;
  const uint8_t *literal=start;
  if(start<end && (*start<0x20u || *start>=0x7fu)){
    const uint8_t *slash=NULL;
    for(const uint8_t *cursor=start;cursor<end;cursor++)
      if(*cursor=='/' || *cursor=='\\'){ slash=cursor; break; }
    if(!slash){
      if(directory){ output[0]=0; return 1; }
      return 0;
    }
    literal=slash+1u;
  }
  size_t length=(size_t)(end-literal);
  if(length>=capacity) return 0;
  memcpy(output,literal,length);
  output[length]=0;
  return 1;
}

static int nsis_path_normalize(char *path,int allow_empty){
  if(!path || (!path[0]&&!allow_empty)) return 0;
  size_t length=strlen(path);
  if(length>ANYGM_CONTENT_MAX_MEMBER_PATH) return 0;
  for(size_t index=0;index<length;index++){
    unsigned char value=(unsigned char)path[index];
    if(value=='\\') path[index]='/';
    else if(value<0x20u || value>=0x7fu || value==':') return 0;
  }
  while(path[0]=='/') memmove(path,path+1u,strlen(path));
  if(!path[0]) return allow_empty;
  const char *segment=path;
  for(const char *cursor=path;;cursor++){
    if(*cursor!='/' && *cursor) continue;
    size_t size=(size_t)(cursor-segment);
    if(!size || (size==1u&&segment[0]=='.') ||
       (size==2u&&segment[0]=='.'&&segment[1]=='.')) return 0;
    if(!*cursor) break;
    segment=cursor+1u;
  }
  return 1;
}

static int nsis_path_join(char *output,size_t capacity,const char *left,const char *right){
  int written=left[0]?snprintf(output,capacity,"%s/%s",left,right):
                      snprintf(output,capacity,"%s",right);
  return written>=0 && (size_t)written<capacity;
}

static int nsis_case_equal(const char *left,const char *right){
  while(*left&&*right){
    unsigned char a=(unsigned char)*left++,b=(unsigned char)*right++;
    if(a>='A'&&a<='Z') a=(unsigned char)(a-'A'+'a');
    if(b>='A'&&b<='Z') b=(unsigned char)(b-'A'+'a');
    if(a!=b) return 0;
  }
  return *left==*right;
}

static int nsis_native_name(const char *path){
  const char *name=strrchr(path,'/');
  name=name?name+1u:path;
  size_t length=strlen(name);
  return (length>=4u && (!strcasecmp(name+length-4u,".exe") ||
                         !strcasecmp(name+length-4u,".dll")));
}

static int nsis_payload_name(const char *path){
  const char *name=strrchr(path,'/');
  name=name?name+1u:path;
  return !strcasecmp(name,"data.win") || !strcasecmp(name,"game.win") ||
         !strcasecmp(name,"game.unx") || !strcasecmp(name,"game.droid");
}

static void nsis_plan_free(NsisPlan *plan){
  if(!plan) return;
  free(plan->members);
  memset(plan,0,sizeof *plan);
}

static int nsis_plan_add(NsisPlan *plan,const char *path,uint32_t data_offset){
  for(size_t index=0;index<plan->count;index++)
    if(nsis_case_equal(plan->members[index].path,path)) return 0;
  if(plan->count==plan->capacity){
    size_t capacity=plan->capacity?plan->capacity*2u:32u;
    if(capacity>ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES) capacity=ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES;
    if(capacity<=plan->count) return 0;
    NsisMember *members=(NsisMember *)realloc(plan->members,capacity*sizeof *members);
    if(!members) return 0;
    plan->members=members;
    plan->capacity=capacity;
  }
  snprintf(plan->members[plan->count].path,sizeof plan->members[plan->count].path,"%s",path);
  plan->members[plan->count++].data_offset=data_offset;
  if(nsis_payload_name(path)){
    if(plan->payload[0]) return 0;
    snprintf(plan->payload,sizeof plan->payload,"%s",path);
  }
  return 1;
}

static int nsis_build_plan(const uint8_t *header,size_t header_size,NsisPlan *plan){
  uint32_t entries_offset=nsis_u32(header+20u),entries=nsis_u32(header+24u);
  uint32_t strings_offset=nsis_u32(header+28u),strings_end=nsis_u32(header+36u);
  char directory[ANYGM_CONTENT_MAX_MEMBER_PATH+1u]={0};
  for(uint32_t index=0;index<entries;index++){
    const uint8_t *entry=header+entries_offset+(size_t)index*NSIS_ENTRY_SIZE;
    uint32_t opcode=nsis_u32(entry);
    if(opcode==NSIS_OPCODE_CREATEDIR && nsis_u32(entry+8u)){
      char value[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
      if(!nsis_literal_string(header,header_size,strings_offset,strings_end,
                              (int32_t)nsis_u32(entry+4u),value,sizeof value,1) ||
         !nsis_path_normalize(value,1)) return 0;
      snprintf(directory,sizeof directory,"%s",value);
    }else if(opcode==NSIS_OPCODE_EXTRACTFILE){
      char name[ANYGM_CONTENT_MAX_MEMBER_PATH+1u],path[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
      if(!nsis_literal_string(header,header_size,strings_offset,strings_end,
                              (int32_t)nsis_u32(entry+8u),name,sizeof name,0)) continue;
      if(!nsis_path_normalize(name,0) ||
         !nsis_path_join(path,sizeof path,directory,name)) return 0;
      if(nsis_native_name(path)) continue;
      if(!nsis_plan_add(plan,path,nsis_u32(entry+12u))) return 0;
    }
  }
  return plan->count && plan->payload[0];
}

static int nsis_mkdir_parent(const AnygmContentRouter *router,char *path){
  char *slash=strrchr(path,'/');
  if(!slash) return 1;
  *slash=0;
  int ok=path[0]?anygm_vfs_mkdirs(router->host,path):1;
  *slash='/';
  return ok;
}

static int nsis_decode_member(const uint8_t *source,size_t source_size,
                              const AnygmEmbeddedNsis *nsis,uint32_t relative,
                              uint8_t **decoded,size_t *decoded_size){
  *decoded=NULL; *decoded_size=0;
  if((uint64_t)relative>nsis->data_size-4u) return 0;
  size_t offset=(size_t)nsis->data_offset+relative;
  if(offset>source_size-4u) return 0;
  uint32_t word=nsis_u32(source+offset),packed=word&UINT32_C(0x7fffffff);
  if((uint64_t)packed>nsis->data_size-relative-4u || packed>source_size-offset-4u) return 0;
  if(!(word&UINT32_C(0x80000000))){
    if(packed>ANYGM_CONTENT_MAX_MEMBER_BYTES) return 0;
    uint8_t *bytes=(uint8_t *)malloc(packed?packed:1u);
    if(!bytes) return 0;
    if(packed) memcpy(bytes,source+offset+4u,packed);
    *decoded=bytes; *decoded_size=packed;
    return 1;
  }
  size_t limit=(size_t)ANYGM_CONTENT_MAX_MEMBER_BYTES;
  uint64_t ratio=(uint64_t)packed*ANYGM_CONTENT_MAX_EXPANSION_RATIO+
                 ANYGM_CONTENT_EXPANSION_ALLOWANCE;
  if(ratio<limit) limit=(size_t)ratio;
  size_t capacity=(size_t)packed*2u+65536u;
  if(capacity<packed || capacity>limit) capacity=limit;
  while(capacity){
    uint8_t *bytes=(uint8_t *)malloc(capacity);
    if(!bytes) return 0;
    size_t actual=0;
    if(gml_deflate_decode_nsis_to_buffer(source+offset+4u,packed,bytes,capacity,&actual)){
      *decoded=bytes; *decoded_size=actual; return 1;
    }
    free(bytes);
    if(capacity==limit) break;
    size_t next=capacity>limit/2u?limit:capacity*2u;
    if(next<=capacity) break;
    capacity=next;
  }
  return 0;
}

int anygm_embedded_nsis_extract(const AnygmContentRouter *router,const char *source_path,
                                const AnygmEmbeddedNsis *nsis,char *payload_path,
                                size_t payload_path_size,char *asset_root,
                                size_t asset_root_size){
  char ignored_root[1024];
  if(!asset_root&&!asset_root_size){ asset_root=ignored_root; asset_root_size=sizeof ignored_root; }
  if(!router || !source_path || !nsis || !payload_path || !payload_path_size ||
     !asset_root || !asset_root_size || !anygm_vfs_can_read(router->host) ||
     !anygm_vfs_can_write(router->host) || !router->host->path_rename ||
     !router->host->path_remove) return 0;
  payload_path[0]=0; asset_root[0]=0;
  uint8_t *source=NULL;
  size_t source_size=0;
  if(!anygm_vfs_read_all(router->host,source_path,&source,&source_size,
                         (size_t)ANYGM_CONTENT_MAX_EXECUTABLE_BYTES) ||
     source_size!=nsis->source_size || nsis_hash(source,source_size)!=nsis->source_hash){
    free(source); return 0;
  }
  uint8_t *header=(uint8_t *)malloc(nsis->header_size);
  NsisPlan plan={0};
  int ok=header && nsis_inflate_exact(source+nsis->header_offset+NSIS_FIRST_HEADER_SIZE+4u,
                                      nsis->header_packed_size,header,nsis->header_size) &&
         nsis_header_shape(header,nsis->header_size) &&
         nsis_build_plan(header,nsis->header_size,&plan);
  free(header);
  const char *base=router->cache_directory?router->cache_directory:"tmp";
  char stem[256],cache[1024]={0},staging[1152]={0};
  anygm_content_path_stem(source_path,stem,sizeof stem);
  if(ok && (snprintf(cache,sizeof cache,"%s/%s-%016llx-anygm-nsis",base,stem,
                     (unsigned long long)nsis->source_hash)>=(int)sizeof cache ||
            snprintf(staging,sizeof staging,"%s.staging-%llx",cache,
                     (unsigned long long)(uintptr_t)router)>=(int)sizeof staging)) ok=0;
  if(ok){
    anygm_content_directory_remove(router->host,staging);
    ok=anygm_vfs_mkdirs(router->host,staging);
  }
  uint64_t total=0;
  for(size_t index=0;ok && index<plan.count;index++){
    uint8_t *decoded=NULL; size_t decoded_size=0;
    ok=nsis_decode_member(source,source_size,nsis,plan.members[index].data_offset,
                          &decoded,&decoded_size) &&
       decoded_size<=ANYGM_CONTENT_MAX_EXTRACTED_BYTES-total;
    if(ok){
      total+=decoded_size;
      char output[1536];
      ok=nsis_path_join(output,sizeof output,staging,plan.members[index].path) &&
         nsis_mkdir_parent(router,output) &&
         anygm_vfs_write_all(router->host,output,decoded,decoded_size);
    }
    free(decoded);
  }
  free(source);
  char selected[1536];
  if(ok) ok=nsis_path_join(selected,sizeof selected,cache,plan.payload) &&
            strlen(selected)+1u<=payload_path_size && strlen(cache)+1u<=asset_root_size;
  if(ok){
    anygm_content_directory_remove(router->host,cache);
    ok=anygm_vfs_publish(router->host,staging,cache);
  }
  if(!ok){
    if(staging[0]) anygm_content_directory_remove(router->host,staging);
    nsis_log(router,ANYGM_CONTENT_LOG_ERROR,"nsis: extraction failed");
    nsis_plan_free(&plan);
    return 0;
  }
  snprintf(payload_path,payload_path_size,"%s",selected);
  snprintf(asset_root,asset_root_size,"%s",cache);
  nsis_log(router,ANYGM_CONTENT_LOG_INFO,"nsis: extracted verified content to %s",cache);
  nsis_plan_free(&plan);
  return 1;
}
