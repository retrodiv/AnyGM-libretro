/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"
#include "content_router.h"
#include "stdio_vfs.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct Buffer {
  uint8_t *data;
  size_t size;
  size_t capacity;
} Buffer;

typedef struct ZipEntry {
  const uint8_t *name;
  size_t name_size;
  const uint8_t *data;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
  uint32_t method;
  uint32_t flags;
  uint32_t external_attributes;
  int corrupt_crc;
} ZipEntry;

static int fail(const char *message){
  fprintf(stderr,"content security: %s\n",message);
  return 0;
}

static int buffer_reserve(Buffer *buffer,size_t add){
  if(add>SIZE_MAX-buffer->size) return 0;
  size_t needed=buffer->size+add;
  if(needed<=buffer->capacity) return 1;
  size_t capacity=buffer->capacity?buffer->capacity:256;
  while(capacity<needed){
    if(capacity>SIZE_MAX/2) return 0;
    capacity*=2;
  }
  uint8_t *next=realloc(buffer->data,capacity);
  if(!next) return 0;
  buffer->data=next;
  buffer->capacity=capacity;
  return 1;
}

static int buffer_bytes(Buffer *buffer,const void *data,size_t size){
  if(!buffer_reserve(buffer,size)) return 0;
  if(size) memcpy(buffer->data+buffer->size,data,size);
  buffer->size+=size;
  return 1;
}

static int buffer_u16(Buffer *buffer,uint16_t value){
  uint8_t encoded[2]={(uint8_t)value,(uint8_t)(value>>8)};
  return buffer_bytes(buffer,encoded,sizeof encoded);
}

static int buffer_u32(Buffer *buffer,uint32_t value){
  uint8_t encoded[4]={(uint8_t)value,(uint8_t)(value>>8),(uint8_t)(value>>16),
                      (uint8_t)(value>>24)};
  return buffer_bytes(buffer,encoded,sizeof encoded);
}

static void store_u16(uint8_t *data,size_t offset,uint16_t value){
  data[offset]=(uint8_t)value;
  data[offset+1]=(uint8_t)(value>>8);
}

static void store_u32(uint8_t *data,size_t offset,uint32_t value){
  data[offset]=(uint8_t)value;
  data[offset+1]=(uint8_t)(value>>8);
  data[offset+2]=(uint8_t)(value>>16);
  data[offset+3]=(uint8_t)(value>>24);
}

static uint32_t crc32_bytes(const uint8_t *data,size_t size){
  uint32_t crc=UINT32_MAX;
  for(size_t i=0;i<size;i++){
    crc^=data[i];
    for(unsigned bit=0;bit<8;bit++)
      crc=(crc>>1)^((crc&1u)?UINT32_C(0xedb88320):0u);
  }
  return ~crc;
}

static uint64_t hash64_bytes(const uint8_t *data,size_t size){
  uint64_t hash=UINT64_C(1469598103934665603);
  for(size_t i=0;i<size;i++){
    hash^=data[i];
    hash*=UINT64_C(1099511628211);
  }
  return hash;
}

static int build_zip(const ZipEntry *entries,size_t count,Buffer *output){
  Buffer local={0},central={0};
  uint32_t *offsets=calloc(count?count:1,sizeof *offsets);
  if(!offsets) return 0;
  int ok=1;
  for(size_t i=0;i<count&&ok;i++){
    const ZipEntry *entry=&entries[i];
    if(entry->name_size>UINT16_MAX || local.size>UINT32_MAX){ ok=0; break; }
    offsets[i]=(uint32_t)local.size;
    uint32_t crc=crc32_bytes(entry->data,entry->compressed_size);
    if(entry->corrupt_crc) crc^=UINT32_C(0x01010101);
    ok=buffer_u32(&local,UINT32_C(0x04034b50)) && buffer_u16(&local,20) &&
       buffer_u16(&local,(uint16_t)entry->flags) &&
       buffer_u16(&local,(uint16_t)entry->method) && buffer_u16(&local,0) &&
       buffer_u16(&local,0) && buffer_u32(&local,crc) &&
       buffer_u32(&local,entry->compressed_size) &&
       buffer_u32(&local,entry->uncompressed_size) &&
       buffer_u16(&local,(uint16_t)entry->name_size) && buffer_u16(&local,0) &&
       buffer_bytes(&local,entry->name,entry->name_size) &&
       buffer_bytes(&local,entry->data,entry->compressed_size);
    if(!ok) break;
    ok=buffer_u32(&central,UINT32_C(0x02014b50)) && buffer_u16(&central,0x0314) &&
       buffer_u16(&central,20) && buffer_u16(&central,(uint16_t)entry->flags) &&
       buffer_u16(&central,(uint16_t)entry->method) && buffer_u16(&central,0) &&
       buffer_u16(&central,0) && buffer_u32(&central,crc) &&
       buffer_u32(&central,entry->compressed_size) &&
       buffer_u32(&central,entry->uncompressed_size) &&
       buffer_u16(&central,(uint16_t)entry->name_size) && buffer_u16(&central,0) &&
       buffer_u16(&central,0) && buffer_u16(&central,0) && buffer_u16(&central,0) &&
       buffer_u32(&central,entry->external_attributes) &&
       buffer_u32(&central,offsets[i]) &&
       buffer_bytes(&central,entry->name,entry->name_size);
  }
  if(ok && (count>UINT16_MAX || local.size>UINT32_MAX || central.size>UINT32_MAX)) ok=0;
  if(ok){
    output->data=local.data;
    output->size=local.size;
    output->capacity=local.capacity;
    local.data=NULL;
    ok=buffer_bytes(output,central.data,central.size) &&
       buffer_u32(output,UINT32_C(0x06054b50)) && buffer_u16(output,0) &&
       buffer_u16(output,0) && buffer_u16(output,(uint16_t)count) &&
       buffer_u16(output,(uint16_t)count) && buffer_u32(output,(uint32_t)central.size) &&
       buffer_u32(output,(uint32_t)local.size) && buffer_u16(output,0);
  }
  free(offsets);
  free(local.data);
  free(central.data);
  if(!ok){ free(output->data); memset(output,0,sizeof *output); }
  return ok;
}

static int write_file(const char *path,const void *data,size_t size){
  FILE *file=fopen(path,"wb");
  int ok=file&&fwrite(data,1,size,file)==size;
  if(file&&fclose(file)!=0) ok=0;
  return ok;
}

static int read_prefix(const char *path,void *data,size_t size){
  FILE *file=fopen(path,"rb");
  int ok=file&&fread(data,1,size,file)==size;
  if(file) fclose(file);
  return ok;
}

static int read_file(const char *path,uint8_t **data,size_t *size){
  if(data) *data=NULL;
  if(size) *size=0;
  FILE *file=fopen(path,"rb");
  if(!file) return 0;
  int ok=fseek(file,0,SEEK_END)==0;
  long length=ok?ftell(file):-1;
  if(length<0 || fseek(file,0,SEEK_SET)!=0) ok=0;
  uint8_t *bytes=ok?malloc((size_t)length?((size_t)length):1u):NULL;
  if(ok && !bytes) ok=0;
  if(ok && fread(bytes,1,(size_t)length,file)!=(size_t)length) ok=0;
  if(fclose(file)!=0) ok=0;
  if(!ok){ free(bytes); return 0; }
  if(data) *data=bytes; else free(bytes);
  if(size) *size=(size_t)length;
  return 1;
}

static int remove_tree(const char *path){
  DIR *directory=opendir(path);
  if(!directory) return errno==ENOENT;
  struct dirent *entry;
  int ok=1;
  while((entry=readdir(directory))!=NULL){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    char child[1024];
    int length=snprintf(child,sizeof child,"%s/%s",path,entry->d_name);
    if(length<0||(size_t)length>=sizeof child){ ok=0; break; }
    struct stat info;
    if(lstat(child,&info)!=0){ ok=0; break; }
    if(S_ISDIR(info.st_mode)){ if(!remove_tree(child)){ ok=0; break; } }
    else if(unlink(child)!=0){ ok=0; break; }
  }
  if(closedir(directory)!=0) ok=0;
  if(ok&&rmdir(path)!=0) ok=0;
  return ok;
}

static AnygmResult reject_rename(void *userdata,const char *from,const char *to){
  (void)userdata;
  (void)from;
  (void)to;
  return ANYGM_ERROR_IO;
}

static int resolve_archive(const AnygmHostServices *services,const char *root,
                           const char *name,const Buffer *archive,int expected){
  char path[512],resolved[1024];
  if(snprintf(path,sizeof path,"%s/%s",root,name)>=(int)sizeof path ||
     !write_file(path,archive->data,archive->size)) return fail("could not write archive case");
  AnygmContentRouter router={0};
  router.host=services;
  router.cache_directory=root;
  int result=anygm_content_resolve_path(&router,path,resolved,sizeof resolved,NULL,0,NULL,0);
  if((result!=0)!=expected) return fail(name);
  if(expected){
    uint8_t magic[4];
    if(!read_prefix(resolved,magic,sizeof magic)||memcmp(magic,"FORM",4))
      return fail("safe archive payload changed");
  }
  return 1;
}

static int invalid_case(const AnygmHostServices *services,const char *root,const char *filename,
                        const ZipEntry *entries,size_t count){
  Buffer archive={0};
  int ok=build_zip(entries,count,&archive)&&
         resolve_archive(services,root,filename,&archive,0);
  free(archive.data);
  return ok;
}

static void build_no_code_form(uint8_t form[200]){
  memset(form,0,200);
  memcpy(form,"FORM",4);
  store_u32(form,4,192);
  memcpy(form+8,"GEN8",4);
  store_u32(form,12,136);
  form[17]=15;
  store_u32(form,76,320);
  store_u32(form,80,240);
  memcpy(form+152,"ROOM",4);
  store_u32(form,156,4);
  memcpy(form+164,"CODE",4);
  memcpy(form+172,"VARI",4);
  memcpy(form+180,"FUNC",4);
  memcpy(form+188,"STRG",4);
  store_u32(form,192,4);
}

static size_t build_pe_cabinet(uint8_t executable[1024]){
  memset(executable,0,1024);
  executable[0]='M';
  executable[1]='Z';
  store_u32(executable,60,128);
  memcpy(executable+128,"PE\0\0",4);
  store_u16(executable,132,UINT16_C(0x014c));
  store_u16(executable,134,1);
  store_u16(executable,148,UINT16_C(0x00e0));
  memcpy(executable+376,".rsrc",5);
  store_u32(executable,376+16,512);
  store_u32(executable,376+20,512);

  uint8_t *cabinet=executable+512;
  static const uint8_t member[]="DATA";
  static const char name[]="payload.win";
  const uint32_t files_offset=44;
  const uint32_t data_offset=files_offset+16u+(uint32_t)sizeof name;
  const uint32_t cabinet_size=data_offset+8u+(uint32_t)sizeof member-1u;
  memcpy(cabinet,"MSCF",4);
  store_u32(cabinet,8,cabinet_size);
  store_u32(cabinet,16,files_offset);
  cabinet[24]=3;
  cabinet[25]=1;
  store_u16(cabinet,26,1);
  store_u16(cabinet,28,1);
  store_u32(cabinet,36,data_offset);
  store_u16(cabinet,40,1);
  store_u16(cabinet,42,0);
  store_u32(cabinet,files_offset,(uint32_t)sizeof member-1u);
  store_u16(cabinet,files_offset+8u,0);
  memcpy(cabinet+files_offset+16u,name,sizeof name);
  store_u16(cabinet,data_offset+4u,(uint16_t)(sizeof member-1u));
  store_u16(cabinet,data_offset+6u,(uint16_t)(sizeof member-1u));
  memcpy(cabinet+data_offset+8u,member,sizeof member-1u);
  return 1024;
}

static size_t build_pe_multifolder_cabinet(uint8_t executable[1024]){
  memset(executable,0,1024);
  executable[0]='M';
  executable[1]='Z';
  store_u32(executable,60,128);
  memcpy(executable+128,"PE\0\0",4);
  store_u16(executable,132,UINT16_C(0x014c));
  store_u16(executable,134,1);
  store_u16(executable,148,UINT16_C(0x00e0));
  memcpy(executable+376,".rsrc",5);
  store_u32(executable,376+16,512);
  store_u32(executable,376+20,512);

  uint8_t *cabinet=executable+512;
  const uint32_t files_offset=52,data0_offset=118,data1_offset=134,cabinet_size=146;
  memcpy(cabinet,"MSCF",4);
  store_u32(cabinet,8,cabinet_size);
  store_u32(cabinet,16,files_offset);
  cabinet[24]=3;
  cabinet[25]=1;
  store_u16(cabinet,26,2);
  store_u16(cabinet,28,3);
  store_u32(cabinet,36,data0_offset);
  store_u16(cabinet,40,1);
  store_u32(cabinet,44,data1_offset);
  store_u16(cabinet,48,1);
  const char *names[]={"a.win","b.win","c.win"};
  const uint32_t offsets[]={0,4,0};
  const uint16_t folders[]={0,0,1};
  size_t cursor=files_offset;
  for(size_t i=0;i<3;i++){
    store_u32(cabinet,cursor,4);
    store_u32(cabinet,cursor+4u,offsets[i]);
    store_u16(cabinet,cursor+8u,folders[i]);
    memcpy(cabinet+cursor+16u,names[i],6);
    cursor+=22u;
  }
  store_u16(cabinet,data0_offset+4u,8);
  store_u16(cabinet,data0_offset+6u,8);
  memcpy(cabinet+data0_offset+8u,"ABCDEFGH",8);
  store_u16(cabinet,data1_offset+4u,4);
  store_u16(cabinet,data1_offset+6u,4);
  memcpy(cabinet+data1_offset+8u,"IJKL",4);
  return 1024;
}

static size_t build_overlapping_section_cabinet(uint8_t executable[10240]){
  uint8_t source[1024];
  build_pe_cabinet(source);
  memset(executable,0,10240);
  executable[0]='M';
  executable[1]='Z';
  store_u32(executable,60,128);
  memcpy(executable+128,"PE\0\0",4);
  store_u16(executable,132,UINT16_C(0x014c));
  store_u16(executable,134,96);
  for(size_t i=0;i<96;i++){
    size_t header=152u+i*40u;
    store_u32(executable,header+16u,1024);
    store_u32(executable,header+20u,8192);
  }
  memset(executable+8192,'M',1024);
  memcpy(executable+9092,source+512,84);
  return 10240;
}

static int embedded_cabinet_cases(const AnygmHostServices *services,const char *root){
  uint8_t executable[1024];
  size_t executable_size=build_pe_cabinet(executable);
  char path[512];
  if(snprintf(path,sizeof path,"%s/cabinet.exe",root)>=(int)sizeof path ||
     !write_file(path,executable,executable_size))
    return fail("could not stage executable Cabinet fixture");
  AnygmContentRouter router={0};
  router.host=services;
  router.cache_directory=root;
  if(!anygm_content_executable_has_cabinet(&router,path))
    return fail("structural executable Cabinet was not recognized");

  AnygmEngine *engine=NULL;
  if(anygm_create(services,&engine)!=ANYGM_OK || !engine)
    return fail("could not create engine for executable Cabinet diagnostic");
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=path;
  source.cache_directory=root;
  AnygmResult result=anygm_load(engine,&source,NULL);
  char diagnostic[512];
  anygm_get_last_error(engine,diagnostic,sizeof diagnostic);
  int ok=result==ANYGM_ERROR_UNSUPPORTED && strstr(diagnostic,"Microsoft Cabinet (CAB)") &&
         anygm_state_size(engine)==0;
  anygm_destroy(engine);
  if(!ok){
    fprintf(stderr,"content security: executable Cabinet result=%d diagnostic=%s\n",
            (int)result,diagnostic);
    return fail("executable Cabinet did not return the stable unsupported diagnostic");
  }
  char direct_diagnostic[512];
  memcpy(direct_diagnostic,diagnostic,sizeof direct_diagnostic);

  char anchor[512];
  static const char anchor_text[]="cabinet.exe\n";
  if(snprintf(anchor,sizeof anchor,"%s/cabinet.anygm",root)>=(int)sizeof anchor ||
     !write_file(anchor,anchor_text,sizeof anchor_text-1u))
    return fail("could not stage executable Cabinet anchor");
  source.path=anchor;
  if(anygm_create(services,&engine)!=ANYGM_OK || !engine)
    return fail("could not create engine for anchored Cabinet diagnostic");
  result=anygm_load(engine,&source,NULL);
  anygm_get_last_error(engine,diagnostic,sizeof diagnostic);
  ok=result==ANYGM_ERROR_UNSUPPORTED && !strcmp(diagnostic,direct_diagnostic) &&
     anygm_state_size(engine)==0;
  anygm_destroy(engine);
  if(!ok)
    return fail("anchored executable Cabinet did not preserve the direct diagnostic");

  uint8_t malformed[1024];
  memcpy(malformed,executable,sizeof malformed);
  store_u32(malformed,512+16,UINT32_MAX);
  if(snprintf(path,sizeof path,"%s/malformed-cabinet.exe",root)>=(int)sizeof path ||
     !write_file(path,malformed,sizeof malformed) ||
     anygm_content_executable_has_cabinet(&router,path))
    return fail("malformed executable Cabinet was accepted");

  memcpy(malformed,executable,sizeof malformed);
  memset(malformed+512,0,128);
  memcpy(malformed+512,"MSCF",4);
  if(snprintf(path,sizeof path,"%s/incidental-cabinet-marker.exe",root)>=(int)sizeof path ||
     !write_file(path,malformed,sizeof malformed) ||
     anygm_content_executable_has_cabinet(&router,path))
    return fail("incidental Cabinet marker was accepted");

  if(snprintf(path,sizeof path,"%s/truncated-cabinet.exe",root)>=(int)sizeof path ||
     !write_file(path,executable,580) || anygm_content_executable_has_cabinet(&router,path))
    return fail("truncated executable Cabinet was accepted");

  memcpy(malformed,executable,sizeof malformed);
  store_u32(malformed,512+44,5);
  if(snprintf(path,sizeof path,"%s/uncovered-cabinet-file.exe",root)>=(int)sizeof path ||
     !write_file(path,malformed,sizeof malformed) ||
     anygm_content_executable_has_cabinet(&router,path))
    return fail("Cabinet file beyond its folder stream was accepted");
  source.path=path;
  if(anygm_create(services,&engine)!=ANYGM_OK || !engine)
    return fail("could not create engine for malformed Cabinet result");
  result=anygm_load(engine,&source,NULL);
  anygm_get_last_error(engine,diagnostic,sizeof diagnostic);
  ok=result==ANYGM_ERROR_INVALID_CONTENT && !strstr(diagnostic,"Microsoft Cabinet (CAB)") &&
     anygm_state_size(engine)==0;
  anygm_destroy(engine);
  if(!ok) return fail("uncovered Cabinet file did not remain invalid content");

  uint8_t multifolder[1024];
  build_pe_multifolder_cabinet(multifolder);
  if(snprintf(path,sizeof path,"%s/multifolder-cabinet.exe",root)>=(int)sizeof path ||
     !write_file(path,multifolder,sizeof multifolder) ||
     !anygm_content_executable_has_cabinet(&router,path))
    return fail("valid multi-file multi-folder Cabinet was rejected");
  store_u32(multifolder,512+52+22+4,3);
  if(snprintf(path,sizeof path,"%s/overlapping-cabinet-files.exe",root)>=(int)sizeof path ||
     !write_file(path,multifolder,sizeof multifolder) ||
     anygm_content_executable_has_cabinet(&router,path))
    return fail("overlapping Cabinet files were accepted");
  build_pe_multifolder_cabinet(multifolder);
  store_u32(multifolder,512+52+44,5);
  if(snprintf(path,sizeof path,"%s/multifolder-overrun.exe",root)>=(int)sizeof path ||
     !write_file(path,multifolder,sizeof multifolder) ||
     anygm_content_executable_has_cabinet(&router,path))
    return fail("Cabinet file crossing its folder boundary was accepted");
  build_pe_multifolder_cabinet(multifolder);
  store_u32(multifolder,512+52+22+4,UINT32_MAX);
  if(snprintf(path,sizeof path,"%s/overflowing-cabinet-file.exe",root)>=(int)sizeof path ||
     !write_file(path,multifolder,sizeof multifolder) ||
     anygm_content_executable_has_cabinet(&router,path))
    return fail("overflowing Cabinet file range was accepted");

  uint8_t *overlapping_sections=malloc(10240);
  if(!overlapping_sections) return fail("could not allocate overlapping PE section fixture");
  build_overlapping_section_cabinet(overlapping_sections);
  ok=snprintf(path,sizeof path,"%s/overlapping-sections.exe",root)<(int)sizeof path &&
     write_file(path,overlapping_sections,10240) &&
     anygm_content_executable_has_cabinet(&router,path);
  free(overlapping_sections);
  if(!ok) return fail("merged overlapping PE sections lost their Cabinet");
  return 1;
}

static int embedded_executable_cases(const AnygmHostServices *services,const char *root){
  uint8_t form[200],executable[224]={0};
  build_no_code_form(form);
  executable[0]='M';
  executable[1]='Z';
  memcpy(executable+4,"FORM",4);
  store_u32(executable,8,UINT32_MAX);
  memcpy(executable+24,form,sizeof form);

  char path[512],resolved[1024],resolved_again[1024];
  if(snprintf(path,sizeof path,"%s/embedded.exe",root)>=(int)sizeof path ||
     !write_file(path,executable,sizeof executable)) return 0;
  AnygmContentRouter router={0};
  router.host=services;
  router.cache_directory=root;
  if(!anygm_content_resolve_path(&router,path,resolved,sizeof resolved,NULL,0,NULL,0) ||
     !anygm_content_resolve_path(&router,path,resolved_again,sizeof resolved_again,NULL,0,NULL,0) ||
     strcmp(resolved,resolved_again) || !strcmp(resolved,path))
    return fail("embedded executable was not resolved through its stable cache");
  uint8_t *extracted=NULL;
  size_t extracted_size=0;
  int ok=read_file(resolved,&extracted,&extracted_size) &&
         extracted_size==sizeof form && !memcmp(extracted,form,sizeof form);
  free(extracted);
  if(!ok) return fail("embedded executable payload changed");

  uint8_t supported_with_cabinet[1024];
  build_pe_cabinet(supported_with_cabinet);
  memcpy(supported_with_cabinet+700,form,sizeof form);
  if(snprintf(path,sizeof path,"%s/supported-with-cabinet.exe",root)>=(int)sizeof path ||
     !write_file(path,supported_with_cabinet,sizeof supported_with_cabinet) ||
     !anygm_content_executable_has_cabinet(&router,path) ||
     !anygm_content_resolve_path(&router,path,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("supported embedded executable did not outrank its incidental Cabinet");
  extracted=NULL;
  ok=read_file(resolved,&extracted,&extracted_size) &&
     extracted_size==sizeof form && !memcmp(extracted,form,sizeof form);
  free(extracted);
  if(!ok) return fail("supported executable precedence changed its embedded payload");

  uint8_t ambiguous[424]={0};
  ambiguous[0]='M';
  ambiguous[1]='Z';
  memcpy(ambiguous+16,form,sizeof form);
  memcpy(ambiguous+224,form,sizeof form);
  if(snprintf(path,sizeof path,"%s/ambiguous.exe",root)>=(int)sizeof path ||
     !write_file(path,ambiguous,sizeof ambiguous) ||
     anygm_content_resolve_path(&router,path,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("ambiguous embedded executable was accepted");
  return 1;
}

/* An archive carrying a source container rather than a compiled payload must import it and still
 * report where the sidecar files it opens by path were extracted. A compiled payload beside a
 * source container keeps priority, so an executable shipped next to one is never selected. */
static int archive_source_container_cases(const AnygmHostServices *services,const char *root){
  uint8_t form[200],executable[224]={0};
  build_no_code_form(form);
  executable[0]='M';
  executable[1]='Z';
  memcpy(executable+4,"FORM",4);
  store_u32(executable,8,UINT32_MAX);
  memcpy(executable+24,form,sizeof form);

  static const uint8_t executable_name[]="bundle.exe";
  static const uint8_t asset_name[]="assets/level.txt";
  static const uint8_t payload_name[]="data.win";
  static const uint8_t asset[]="fixture asset";
  ZipEntry entries[3]={
    {executable_name,sizeof executable_name-1,executable,sizeof executable,sizeof executable,0,0,0,0},
    {asset_name,sizeof asset_name-1,asset,sizeof asset-1,sizeof asset-1,0,0,0,0},
    {payload_name,sizeof payload_name-1,form,sizeof form,sizeof form,0,0,0,0}
  };

  Buffer archive={0};
  char path[512],resolved[1024],asset_root[1024],staged[1600];
  int ok=build_zip(entries,2,&archive) &&
         snprintf(path,sizeof path,"%s/source-container.zip",root)<(int)sizeof path &&
         write_file(path,archive.data,archive.size);
  free(archive.data);
  if(!ok) return fail("could not write the source-container archive");
  AnygmContentRouter router={0};
  router.host=services;
  router.cache_directory=root;
  asset_root[0]=0;
  if(!anygm_content_resolve_path(&router,path,resolved,sizeof resolved,
                                 asset_root,sizeof asset_root,NULL,0) || !asset_root[0])
    return fail("archive source container was not imported");
  uint8_t magic[4];
  if(!read_prefix(resolved,magic,sizeof magic) || memcmp(magic,"FORM",4))
    return fail("archive source container did not produce a payload");
  if(snprintf(staged,sizeof staged,"%s/assets/level.txt",asset_root)>=(int)sizeof staged)
    return fail("reported asset root is too long");
  uint8_t staged_prefix[7];
  if(!read_prefix(staged,staged_prefix,sizeof staged_prefix) ||
     memcmp(staged_prefix,"fixture",sizeof staged_prefix))
    return fail("reported asset root does not hold the extracted sidecar");

  memset(&archive,0,sizeof archive);
  ok=build_zip(entries,3,&archive) &&
     snprintf(path,sizeof path,"%s/payload-and-source.zip",root)<(int)sizeof path &&
     write_file(path,archive.data,archive.size);
  free(archive.data);
  if(!ok) return fail("could not write the payload-and-source archive");
  asset_root[0]=0;
  if(!anygm_content_resolve_path(&router,path,resolved,sizeof resolved,
                                 asset_root,sizeof asset_root,NULL,0) || asset_root[0])
    return fail("a compiled payload lost priority to a source container");
  return 1;
}

/* A cached payload is only reusable while the code that produced it is unchanged. The marker
 * therefore carries a producer fingerprint, and a marker written by a different producer must be
 * regenerated rather than trusted; the alternative is content silently running bytes that the
 * current revision would not produce. */
static int cache_producer_change_case(const AnygmHostServices *services,const char *root){
  uint8_t form[200],executable[224]={0};
  build_no_code_form(form);
  executable[0]='M';
  executable[1]='Z';
  memcpy(executable+4,"FORM",4);
  store_u32(executable,8,UINT32_MAX);
  memcpy(executable+24,form,sizeof form);

  char path[512],resolved[1024],marker[1200];
  if(snprintf(path,sizeof path,"%s/producer.exe",root)>=(int)sizeof path ||
     !write_file(path,executable,sizeof executable)) return fail("producer fixture");
  AnygmContentRouter router={0};
  router.host=services;
  router.cache_directory=root;
  if(!anygm_content_resolve_path(&router,path,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("producer fixture was not resolved");
  char *slash=strrchr(resolved,'/');
  if(!slash) return fail("resolved payload has no directory");
  *slash=0;
  int marker_size=snprintf(marker,sizeof marker,"%s/.anygm_cache",resolved);
  *slash='/';
  if(marker_size<0 || marker_size>=(int)sizeof marker) return fail("marker path is too long");

  uint8_t *stored=NULL;
  size_t stored_size=0;
  if(!read_file(marker,&stored,&stored_size) || stored_size<72)
    return fail("cache marker was not published");
  uint8_t original[8];
  memcpy(original,stored+64,sizeof original);
  for(size_t i=0;i<sizeof original;i++) stored[64+i]^=0xA5u;
  int written=write_file(marker,stored,stored_size);
  free(stored);
  if(!written) return fail("could not rewrite the cache marker");

  if(!anygm_content_resolve_path(&router,path,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("a marker from another producer was not regenerated");
  stored=NULL;
  stored_size=0;
  int ok=read_file(marker,&stored,&stored_size) && stored_size>=72 &&
         !memcmp(stored+64,original,sizeof original);
  free(stored);
  if(!ok) return fail("the regenerated marker did not restore this producer");
  return 1;
}

/* An anchor member is the archive stating which payload it carries, so it overrides scored
 * selection, resolves relative to its own directory, may name a nested archive, and a present
 * anchor that is broken rejects the archive rather than silently losing to the scores. */
static int archive_anchor_cases(const AnygmHostServices *services,const char *root){
  uint8_t decoy_form[200],picked_form[200];
  build_no_code_form(decoy_form);
  build_no_code_form(picked_form);
  store_u32(picked_form,80,241);

  static const uint8_t decoy_name[]="data.win";
  static const uint8_t picked_name[]="alt/payload.win";
  static const uint8_t anchor_name[]="pick.anygm";
  static const uint8_t anchor_text[]="alt/payload.win\n";
  ZipEntry entries[3]={
    {decoy_name,sizeof decoy_name-1,decoy_form,sizeof decoy_form,sizeof decoy_form,0,0,0,0},
    {picked_name,sizeof picked_name-1,picked_form,sizeof picked_form,sizeof picked_form,0,0,0,0},
    {anchor_name,sizeof anchor_name-1,anchor_text,sizeof anchor_text-1,sizeof anchor_text-1,
     0,0,0,0},
  };
  Buffer archive={0};
  char path[512],resolved[1024];
  AnygmContentRouter router={0};
  router.host=services;
  router.cache_directory=root;
  int ok=build_zip(entries,3,&archive) &&
         snprintf(path,sizeof path,"%s/anchored.zip",root)<(int)sizeof path &&
         write_file(path,archive.data,archive.size);
  free(archive.data);
  if(!ok) return fail("could not write the anchored archive");
  if(!anygm_content_resolve_path(&router,path,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("anchored archive was not resolved");
  uint8_t *extracted=NULL;
  size_t extracted_size=0;
  ok=read_file(resolved,&extracted,&extracted_size) &&
     extracted_size==sizeof picked_form && !memcmp(extracted,picked_form,sizeof picked_form);
  free(extracted);
  if(!ok) return fail("the anchor did not override payload scoring");

  static const uint8_t sub_anchor_name[]="sub/pick.anygm";
  static const uint8_t sub_anchor_text[]="payload.win";
  static const uint8_t sub_picked_name[]="sub/payload.win";
  ZipEntry sub_entries[3]={
    {decoy_name,sizeof decoy_name-1,decoy_form,sizeof decoy_form,sizeof decoy_form,0,0,0,0},
    {sub_picked_name,sizeof sub_picked_name-1,picked_form,sizeof picked_form,
     sizeof picked_form,0,0,0,0},
    {sub_anchor_name,sizeof sub_anchor_name-1,sub_anchor_text,sizeof sub_anchor_text-1,
     sizeof sub_anchor_text-1,0,0,0,0},
  };
  memset(&archive,0,sizeof archive);
  ok=build_zip(sub_entries,3,&archive) &&
     snprintf(path,sizeof path,"%s/sub-anchored.zip",root)<(int)sizeof path &&
     write_file(path,archive.data,archive.size);
  free(archive.data);
  if(!ok) return fail("could not write the subdirectory-anchored archive");
  if(!anygm_content_resolve_path(&router,path,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("subdirectory-anchored archive was not resolved");
  extracted=NULL;
  ok=read_file(resolved,&extracted,&extracted_size) &&
     extracted_size==sizeof picked_form && !memcmp(extracted,picked_form,sizeof picked_form);
  free(extracted);
  if(!ok) return fail("the anchor reference was not resolved against its own directory");

  Buffer inner={0};
  static const uint8_t inner_payload_name[]="data.win";
  ZipEntry inner_entry={inner_payload_name,sizeof inner_payload_name-1,picked_form,
                        sizeof picked_form,sizeof picked_form,0,0,0,0};
  if(!build_zip(&inner_entry,1,&inner)) return fail("could not build the inner archive");
  static const uint8_t decoy_win_name[]="decoy.win";
  static const uint8_t inner_name[]="inner.zip";
  static const uint8_t nested_anchor_text[]="inner.zip";
  ZipEntry nested_entries[3]={
    {decoy_win_name,sizeof decoy_win_name-1,decoy_form,sizeof decoy_form,sizeof decoy_form,
     0,0,0,0},
    {inner_name,sizeof inner_name-1,inner.data,(uint32_t)inner.size,(uint32_t)inner.size,
     0,0,0,0},
    {anchor_name,sizeof anchor_name-1,nested_anchor_text,sizeof nested_anchor_text-1,
     sizeof nested_anchor_text-1,0,0,0,0},
  };
  memset(&archive,0,sizeof archive);
  ok=build_zip(nested_entries,3,&archive) &&
     snprintf(path,sizeof path,"%s/nested-anchored.zip",root)<(int)sizeof path &&
     write_file(path,archive.data,archive.size);
  free(inner.data);
  free(archive.data);
  if(!ok) return fail("could not write the nested-anchored archive");
  if(!anygm_content_resolve_path(&router,path,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("nested-anchored archive was not resolved");
  extracted=NULL;
  ok=read_file(resolved,&extracted,&extracted_size) &&
     extracted_size==sizeof picked_form && !memcmp(extracted,picked_form,sizeof picked_form);
  free(extracted);
  if(!ok) return fail("the anchor did not select the nested archive");

  static const uint8_t traversal_text[]="../escape.win";
  static const uint8_t dangling_text[]="missing.win";
  static const uint8_t chained_text[]="other.anygm";
  static const uint8_t empty_text[]="\n";
  const struct { const char *filename; const uint8_t *text; size_t size; } broken[]={
    {"anchor-traversal.zip",traversal_text,sizeof traversal_text-1},
    {"anchor-dangling.zip",dangling_text,sizeof dangling_text-1},
    {"anchor-chained.zip",chained_text,sizeof chained_text-1},
    {"anchor-empty.zip",empty_text,sizeof empty_text-1},
  };
  for(size_t i=0;i<sizeof broken/sizeof broken[0];i++){
    ZipEntry broken_entries[2]={
      {decoy_name,sizeof decoy_name-1,decoy_form,sizeof decoy_form,sizeof decoy_form,0,0,0,0},
      {anchor_name,sizeof anchor_name-1,broken[i].text,(uint32_t)broken[i].size,
       (uint32_t)broken[i].size,0,0,0,0},
    };
    if(!invalid_case(services,root,broken[i].filename,broken_entries,2))
      return fail("a broken anchor did not reject its archive");
  }
  return 1;
}

/* The advanced anchor form selects the payload through a [anygm] section, tolerates comments,
 * and hands its [overrides] lines to the caller verbatim — on the cold extraction and again on
 * the warm cache path, which reads the staged anchor copy instead of the archive. */
static int archive_advanced_anchor_cases(const AnygmHostServices *services,const char *root){
  uint8_t decoy_form[200],picked_form[200];
  build_no_code_form(decoy_form);
  build_no_code_form(picked_form);
  store_u32(picked_form,80,242);

  static const uint8_t decoy_name[]="data.win";
  static const uint8_t picked_name[]="alt/payload.win";
  static const uint8_t anchor_name[]="select.anygm";
  static const uint8_t anchor_text[]=
    "\xef\xbb\xbf[anygm]\n"
    "# which payload this archive carries\n"
    "payload = alt/payload.win\n"
    "\n"
    "[overrides]\n"
    "# apply a neutral override\n"
    "  $lives=99  \n"
    "?aspect view_wport[0]=$forced_w\n";
  static const char expected_overrides[]="$lives=99\n?aspect view_wport[0]=$forced_w\n";
  ZipEntry entries[3]={
    {decoy_name,sizeof decoy_name-1,decoy_form,sizeof decoy_form,sizeof decoy_form,0,0,0,0},
    {picked_name,sizeof picked_name-1,picked_form,sizeof picked_form,sizeof picked_form,0,0,0,0},
    {anchor_name,sizeof anchor_name-1,anchor_text,sizeof anchor_text-1,sizeof anchor_text-1,
     0,0,0,0},
  };
  Buffer archive={0};
  char path[512],resolved[1024],overrides[4096];
  AnygmContentRouter router={0};
  router.host=services;
  router.cache_directory=root;
  int ok=build_zip(entries,3,&archive) &&
         snprintf(path,sizeof path,"%s/advanced-anchored.zip",root)<(int)sizeof path &&
         write_file(path,archive.data,archive.size);
  free(archive.data);
  if(!ok) return fail("could not write the advanced-anchored archive");
  overrides[0]=0;
  if(!anygm_content_resolve_path(&router,path,resolved,sizeof resolved,NULL,0,
                                 overrides,sizeof overrides))
    return fail("advanced-anchored archive was not resolved");
  uint8_t *extracted=NULL;
  size_t extracted_size=0;
  ok=read_file(resolved,&extracted,&extracted_size) &&
     extracted_size==sizeof picked_form && !memcmp(extracted,picked_form,sizeof picked_form);
  free(extracted);
  if(!ok) return fail("the advanced anchor did not select its payload");
  if(strcmp(overrides,expected_overrides))
    return fail("the advanced anchor's overrides were not delivered on extraction");
  overrides[0]=0;
  if(!anygm_content_resolve_path(&router,path,resolved,sizeof resolved,NULL,0,
                                 overrides,sizeof overrides) ||
     strcmp(overrides,expected_overrides))
    return fail("the warm cache path did not redeliver the anchor's overrides");

  static const uint8_t unknown_section[]="[anygm]\npayload=data.win\n[extras]\nx=1\n";
  static const uint8_t two_payloads[]="[anygm]\npayload=data.win\npayload=data.win\n";
  static const uint8_t no_payload[]="[anygm]\n[overrides]\n$lives=99\n";
  const struct { const char *filename; const uint8_t *text; size_t size; } broken[]={
    {"advanced-unknown-section.zip",unknown_section,sizeof unknown_section-1},
    {"advanced-two-payloads.zip",two_payloads,sizeof two_payloads-1},
    {"advanced-no-payload.zip",no_payload,sizeof no_payload-1},
  };
  for(size_t i=0;i<sizeof broken/sizeof broken[0];i++){
    ZipEntry broken_entries[2]={
      {decoy_name,sizeof decoy_name-1,decoy_form,sizeof decoy_form,sizeof decoy_form,0,0,0,0},
      {anchor_name,sizeof anchor_name-1,broken[i].text,(uint32_t)broken[i].size,
       (uint32_t)broken[i].size,0,0,0,0},
    };
    if(!invalid_case(services,root,broken[i].filename,broken_entries,2))
      return fail("a malformed advanced anchor did not reject its archive");
  }
  return 1;
}

/* A directly loaded anchor routes its referenced payload, confined to its own subtree. */
static int direct_anchor_cases(const AnygmHostServices *services,const char *root){
  uint8_t form[200];
  build_no_code_form(form);
  char dir[600],payload[700],anchor[700],resolved[1024];
  if(snprintf(dir,sizeof dir,"%s/anchor-dir",root)>=(int)sizeof dir ||
     (mkdir(dir,0777)!=0 && errno!=EEXIST))
    return fail("could not create the anchor directory");
  if(snprintf(payload,sizeof payload,"%s/payload.win",dir)>=(int)sizeof payload ||
     !write_file(payload,form,sizeof form))
    return fail("could not write the anchored payload");
  AnygmContentRouter router={0};
  router.host=services;
  router.cache_directory=root;
  if(snprintf(anchor,sizeof anchor,"%s/title.anygm",dir)>=(int)sizeof anchor ||
     !write_file(anchor,"# identity anchor\n payload.win \r\n# trailing note\n",49))
    return fail("could not write the direct anchor");
  if(!anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("direct anchor was not resolved");
  uint8_t magic[4];
  if(!read_prefix(resolved,magic,sizeof magic) || memcmp(magic,"FORM",4) ||
     !strstr(resolved,"payload.win"))
    return fail("direct anchor did not resolve its referenced payload");

  const struct { const char *name; const char *text; } broken[]={
    {"traversal.anygm","../payload.win"},
    {"missing.anygm","absent.win"},
    {"chained.anygm","title.anygm"},
    {"absolute.anygm","/payload.win"},
  };
  for(size_t i=0;i<sizeof broken/sizeof broken[0];i++){
    if(snprintf(anchor,sizeof anchor,"%s/%s",dir,broken[i].name)>=(int)sizeof anchor ||
       !write_file(anchor,broken[i].text,strlen(broken[i].text)))
      return fail("could not write a broken direct anchor");
    if(anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,NULL,0,NULL,0))
      return fail("a broken direct anchor was accepted");
  }
  {
    uint8_t oversized[4097];
    memset(oversized,'a',sizeof oversized);
    if(snprintf(anchor,sizeof anchor,"%s/oversized.anygm",dir)>=(int)sizeof anchor ||
       !write_file(anchor,oversized,sizeof oversized))
      return fail("could not write the oversized direct anchor");
    if(anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,NULL,0,NULL,0))
      return fail("an oversized direct anchor was accepted");
  }
  /* An anchor's identity is its referenced payload; other inputs keep their own identity. */
  {
    char identity[1024],expected[1024];
    if(snprintf(anchor,sizeof anchor,"%s/title.anygm",dir)>=(int)sizeof anchor ||
       snprintf(expected,sizeof expected,"%s/payload.win",dir)>=(int)sizeof expected)
      return fail("identity paths are too long");
    if(!anygm_content_identity_path(&router,anchor,identity,sizeof identity) ||
       strcmp(identity,expected))
      return fail("the anchor's identity is not its referenced payload");
    if(!anygm_content_identity_path(&router,expected,identity,sizeof identity) ||
       strcmp(identity,expected))
      return fail("a direct payload's identity is not itself");
  }
  return 1;
}

/* A marker from another producer must be regenerated for an ordinary archive. */
static int cache_producer_change_case(const AnygmHostServices *services,const char *root){
  static const uint8_t name[]="data.win";
  static const uint8_t form[]="FORM";
  ZipEntry entry={name,sizeof name-1,form,sizeof form-1,sizeof form-1,0,0,0,0};
  Buffer archive={0};
  char path[512],resolved[1024],marker[1200];
  int ok=build_zip(&entry,1,&archive) &&
         snprintf(path,sizeof path,"%s/producer.zip",root)<(int)sizeof path &&
         write_file(path,archive.data,archive.size);
  free(archive.data);
  if(!ok) return fail("producer archive fixture");

  AnygmContentRouter router={0};
  router.host=services;
  router.cache_directory=root;
  if(!anygm_content_resolve_path(&router,path,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("producer archive was not resolved");
  char *slash=strrchr(resolved,'/');
  if(!slash) return fail("resolved producer payload has no directory");
  *slash=0;
  int written=snprintf(marker,sizeof marker,"%s/.anygm_cache",resolved);
  if(written<0 || written>=(int)sizeof marker) return fail("producer marker path");

  FILE *stream=fopen(marker,"r+b");
  uint8_t original[8],changed[8];
  if(!stream) return fail("producer marker was not published");
  ok=fseek(stream,0,SEEK_END)==0 && ftell(stream)>=72 &&
     fseek(stream,64,SEEK_SET)==0 && fread(original,1,sizeof original,stream)==sizeof original;
  if(ok){
    for(size_t i=0;i<sizeof changed;i++) changed[i]=original[i]^0xA5u;
    ok=fseek(stream,64,SEEK_SET)==0 &&
       fwrite(changed,1,sizeof changed,stream)==sizeof changed;
  }
  if(fclose(stream)!=0) ok=0;
  if(!ok) return fail("could not change producer marker");

  if(!anygm_content_resolve_path(&router,path,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("another producer marker was not regenerated");
  stream=fopen(marker,"rb");
  if(!stream) return fail("regenerated producer marker is missing");
  ok=fseek(stream,64,SEEK_SET)==0 &&
     fread(changed,1,sizeof changed,stream)==sizeof changed &&
     memcmp(changed,original,sizeof original)==0;
  if(fclose(stream)!=0) ok=0;
  return ok?1:fail("regenerated marker did not restore this producer");
}

int main(void){
  char root[]="build/content-security-XXXXXX";
  if(!mkdtemp(root)) return fail("could not create temporary root")?0:1;
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);

  static const uint8_t form[]="FORM";
  static const uint8_t one[]={0};
  static const uint8_t safe_name[]="data.win";
  static const uint8_t traversal_name[]="folder/../escape.bin";
  static const uint8_t absolute_name[]="/escape.bin";
  static const uint8_t case_name[]="DATA.WIN";
  static const uint8_t embedded_nul_name[]={ 'd','a','t','a','.','w','i','n',0,'x' };

  ZipEntry safe={safe_name,sizeof safe_name-1,form,sizeof form-1,sizeof form-1,0,0,0,0};
  ZipEntry traversal={traversal_name,sizeof traversal_name-1,one,sizeof one,sizeof one,0,0,0,0};
  ZipEntry absolute={absolute_name,sizeof absolute_name-1,one,sizeof one,sizeof one,0,0,0,0};
  ZipEntry duplicate={case_name,sizeof case_name-1,form,sizeof form-1,sizeof form-1,0,0,0,0};
  ZipEntry embedded_nul={embedded_nul_name,sizeof embedded_nul_name,form,sizeof form-1,
                         sizeof form-1,0,0,0,0};
  ZipEntry symlink=safe;
  symlink.external_attributes=(uint32_t)0120000u<<16;
  ZipEntry ratio=safe;
  ratio.data=one;
  ratio.compressed_size=1;
  ratio.uncompressed_size=(uint32_t)ANYGM_CONTENT_EXPANSION_ALLOWANCE+1u;
  ratio.method=8;
  ZipEntry oversized=ratio;
  oversized.uncompressed_size=(uint32_t)ANYGM_CONTENT_MAX_MEMBER_BYTES+1u;
  ZipEntry bad_crc=safe;
  bad_crc.corrupt_crc=1;

  int ok=embedded_executable_cases(&services,root) &&
         cache_producer_change_case(&services,root) &&
         embedded_cabinet_cases(&services,root) &&
         archive_anchor_cases(&services,root) &&
         archive_advanced_anchor_cases(&services,root) &&
         direct_anchor_cases(&services,root);
  ZipEntry pair[2]={traversal,safe};
  ok=ok&&invalid_case(&services,root,"traversal.zip",pair,2);
  pair[0]=absolute;
  ok=ok&&invalid_case(&services,root,"absolute.zip",pair,2);
  pair[0]=safe; pair[1]=duplicate;
  ok=ok&&invalid_case(&services,root,"duplicate.zip",pair,2);
  ok=ok&&invalid_case(&services,root,"embedded-nul.zip",&embedded_nul,1);
  ok=ok&&invalid_case(&services,root,"symlink.zip",&symlink,1);
  ok=ok&&invalid_case(&services,root,"ratio.zip",&ratio,1);
  ok=ok&&invalid_case(&services,root,"oversized.zip",&oversized,1);
  ok=ok&&invalid_case(&services,root,"crc.zip",&bad_crc,1);

  Buffer safe_archive={0};
  ok=ok&&build_zip(&safe,1,&safe_archive)&&
     resolve_archive(&services,root,"safe.zip",&safe_archive,1);

  if(ok){
    static const uint8_t payload_name[]="assets/game.droid";
    static const uint8_t unused_upper[]="unused/Example.bin";
    static const uint8_t unused_lower[]="unused/example.bin";
    ZipEntry scoped_entries[3]={
      {payload_name,sizeof payload_name-1,form,sizeof form-1,sizeof form-1,0,0,0,0},
      {unused_upper,sizeof unused_upper-1,one,sizeof one,sizeof one,0,0,0,0},
      {unused_lower,sizeof unused_lower-1,one,sizeof one,sizeof one,0,0,0,0},
    };
    Buffer scoped_archive={0};
    ok=build_zip(scoped_entries,3,&scoped_archive)&&
       resolve_archive(&services,root,"scoped-duplicates.apk",&scoped_archive,1);
    free(scoped_archive.data);
  }

  if(ok){
    char outdir[1024],marker[1100],payload[1100],archive_path[512];
    snprintf(archive_path,sizeof archive_path,"%s/safe.zip",root);
    snprintf(outdir,sizeof outdir,"%s/safe-%016llx-anygm-archive",root,
             (unsigned long long)hash64_bytes(safe_archive.data,safe_archive.size));
    snprintf(marker,sizeof marker,"%s/.anygm_cache",outdir);
    snprintf(payload,sizeof payload,"%s/data.win",outdir);
    ok=write_file(marker,"broken",6)&&write_file(payload,"FAIL",4)&&
       resolve_archive(&services,root,"safe.zip",&safe_archive,1);
    uint8_t restored[4];
    if(!ok||!read_prefix(payload,restored,sizeof restored)||memcmp(restored,"FORM",4))
      ok=fail("corrupt cache was not regenerated");

    AnygmHostServices no_publish=services;
    no_publish.path_rename=reject_rename;
    if(ok){
      ok=write_file(marker,"preserved",9)&&write_file(payload,"FAIL",4)&&
         resolve_archive(&no_publish,root,"safe.zip",&safe_archive,1);
      uint8_t preserved[9];
      if(!ok||!read_prefix(marker,preserved,sizeof preserved)||memcmp(preserved,"preserved",9))
        ok=fail("failed cache publication removed the previous marker");
    }
  }

  Buffer nested={0};
  if(ok) ok=build_zip(&safe,1,&nested);
  static const uint8_t nested_name[]="nested.zip";
  for(unsigned depth=0;ok&&depth<ANYGM_CONTENT_MAX_NESTING_LEVELS;depth++){
    ZipEntry entry={nested_name,sizeof nested_name-1,nested.data,(uint32_t)nested.size,
                    (uint32_t)nested.size,0,0,0,0};
    Buffer wrapper={0};
    ok=build_zip(&entry,1,&wrapper);
    free(nested.data);
    nested=wrapper;
  }
  if(ok) ok=resolve_archive(&services,root,"nested-limit.zip",&nested,0);
  free(nested.data);

  if(ok){
    uint8_t eocd[22]={ 'P','K',5,6 };
    uint16_t count=(uint16_t)(ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES+1u);
    eocd[8]=(uint8_t)count; eocd[9]=(uint8_t)(count>>8);
    eocd[10]=(uint8_t)count; eocd[11]=(uint8_t)(count>>8);
    Buffer excessive={eocd,sizeof eocd,sizeof eocd};
    ok=resolve_archive(&services,root,"entry-limit.zip",&excessive,0);
  }

  free(safe_archive.data);
  if(!remove_tree(root)) ok=fail("could not remove temporary root");
  if(!ok) return 1;
  puts("bounded content corpus: ok");
  return 0;
}
