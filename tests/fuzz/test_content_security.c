/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"
#include "content_router.h"
#include "classic_test_fixture.h"
#include "embedded_cab.h"
#include "embedded_nsis.h"
#include "engine_internal.h"
#include "gml_hash.h"
#include "memory_vfs.h"
#include "synthetic_content.h"
#include "stdio_vfs.h"
#include "vcdiff_fixture.h"

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

static void fixture_log(void *userdata,int level,const char *message){
  (void)userdata;
  if(level>=ANYGM_CONTENT_LOG_ERROR) fprintf(stderr,"content security log: %s\n",message);
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

static uint32_t load_u32(const uint8_t *data){
  return (uint32_t)data[0]|(uint32_t)data[1]<<8|(uint32_t)data[2]<<16|
         (uint32_t)data[3]<<24;
}

static uint16_t load_u16(const uint8_t *data){
  return (uint16_t)((uint16_t)data[0]|(uint16_t)((uint16_t)data[1]<<8));
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

static size_t reject_write(void *userdata,void *file,const void *data,size_t size){
  (void)userdata;
  (void)file;
  (void)data;
  (void)size;
  return 0;
}

static size_t counted_reject_write_calls;

static size_t counted_reject_write(void *userdata,void *file,const void *data,size_t size){
  counted_reject_write_calls++;
  return reject_write(userdata,file,data,size);
}

static AnygmFileWriteFn limited_write_original;
static size_t limited_write_total;
static size_t limited_write_limit;

static size_t limited_write(void *userdata,void *file,const void *data,size_t size){
  if(limited_write_total>=limited_write_limit) return 0;
  size_t available=limited_write_limit-limited_write_total;
  if(size>available) size=available;
  size_t written=limited_write_original(userdata,file,data,size);
  limited_write_total+=written;
  return written;
}

static AnygmResult reject_flush(void *userdata,void *file){
  (void)userdata;
  (void)file;
  return ANYGM_ERROR_IO;
}

static int root_has_directory_prefix(const char *root,const char *prefix){
  DIR *directory=opendir(root);
  if(!directory) return 0;
  int found=0;
  struct dirent *entry;
  while((entry=readdir(directory))!=NULL){
    if(strncmp(entry->d_name,prefix,strlen(prefix))) continue;
    char path[1024]; struct stat info;
    if(snprintf(path,sizeof path,"%s/%s",root,entry->d_name)<(int)sizeof path &&
       lstat(path,&info)==0 && S_ISDIR(info.st_mode)){ found=1; break; }
  }
  closedir(directory);
  return found;
}

static int anygm_content_executable_has_cabinet(const AnygmContentRouter *router,
                                                const char *path){
  AnygmEmbeddedCab cab={0};
  AnygmEmbeddedCabStatus status=anygm_embedded_cab_probe(router,path,&cab);
  return status==ANYGM_EMBEDDED_CAB_SUPPORTED || status==ANYGM_EMBEDDED_CAB_UNSUPPORTED;
}

static int resolve_archive(const AnygmHostServices *services,const char *root,
                           const char *name,const Buffer *archive,int expected){
  char path[512],resolved[1024];
  if(snprintf(path,sizeof path,"%s/%s",root,name)>=(int)sizeof path ||
     !write_file(path,archive->data,archive->size)) return fail("could not write archive case");
  AnygmContentRouter router={0};
  router.host=services;
  router.cache_directory=root;
  router.log=fixture_log;
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

/* The identity fields and strings in this test image are authored here. */
static size_t build_no_code_form_named(uint8_t *form,size_t capacity,const char *filename,
                                       const char *name,uint32_t game_id){
  size_t filename_length=strlen(filename),name_length=strlen(name);
  size_t pool=196u;
  size_t filename_offset=pool+16u;
  size_t name_length_offset=filename_offset+filename_length+1u;
  size_t name_offset=name_length_offset+4u;
  size_t total=name_offset+name_length+1u;
  if(!filename_length || !name_length || total>capacity) return 0;
  memset(form,0,capacity);
  memcpy(form,"FORM",4);
  store_u32(form,4,(uint32_t)(total-8u));
  memcpy(form+8,"GEN8",4);
  store_u32(form,12,136);
  form[17]=15;
  store_u32(form,20,(uint32_t)filename_offset);
  store_u32(form,36,game_id);
  store_u32(form,56,(uint32_t)name_offset);
  store_u32(form,76,320);
  store_u32(form,80,240);
  memcpy(form+152,"ROOM",4);
  store_u32(form,156,4);
  memcpy(form+164,"CODE",4);
  memcpy(form+172,"VARI",4);
  memcpy(form+180,"FUNC",4);
  memcpy(form+188,"STRG",4);
  store_u32(form,192,(uint32_t)(total-pool));
  store_u32(form,pool,2);
  store_u32(form,pool+4u,(uint32_t)(filename_offset-4u));
  store_u32(form,pool+8u,(uint32_t)name_length_offset);
  store_u32(form,filename_offset-4u,(uint32_t)filename_length);
  memcpy(form+filename_offset,filename,filename_length);
  store_u32(form,name_length_offset,(uint32_t)name_length);
  memcpy(form+name_offset,name,name_length);
  return total;
}

/* Read one payload's GEN8 identity straight out of its bytes, so the fixture can claim exactly the
 * identity the neighbour published instead of assuming one. */
static int read_form_identity(const uint8_t *data,size_t size,char *filename,size_t filename_size,
                              char *name,size_t name_size,uint32_t *game_id){
  if(size<12u || memcmp(data,"FORM",4)) return 0;
  size_t end=8u+load_u32(data+4);
  if(end>size) end=size;
  size_t gen8=0;
  for(size_t cursor=8u;cursor+8u<=end;){
    uint32_t length=load_u32(data+cursor+4);
    if((size_t)length>end-(cursor+8u)) return 0;
    if(!memcmp(data+cursor,"GEN8",4)){ gen8=cursor+8u; break; }
    cursor+=8u+length;
  }
  if(!gen8 || gen8+44u>size) return 0;
  size_t filename_offset=load_u32(data+gen8+4),name_offset=load_u32(data+gen8+40);
  *game_id=load_u32(data+gen8+20);
  if(filename_offset<4u || name_offset<4u || filename_offset>size || name_offset>size) return 0;
  uint32_t filename_length=load_u32(data+filename_offset-4),name_length=load_u32(data+name_offset-4);
  if(!filename_length || !name_length || filename_length>=filename_size || name_length>=name_size ||
     filename_offset+filename_length>size || name_offset+name_length>size) return 0;
  memcpy(filename,data+filename_offset,filename_length); filename[filename_length]='\0';
  memcpy(name,data+name_offset,name_length); name[name_length]='\0';
  return 1;
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

static size_t build_pe_launcher_only(uint8_t executable[1024]){
  build_pe_cabinet(executable);
  memset(executable+512,0,512);
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

static int build_lzx_executable(const uint8_t *cabinet,size_t cabinet_size,Buffer *output){
  if(!cabinet || !cabinet_size || !output || cabinet_size>UINT32_MAX-128u) return 0;
  size_t size=640u+cabinet_size;
  uint8_t *bytes=calloc(size,1);
  if(!bytes) return 0;
  bytes[0]='M'; bytes[1]='Z';
  store_u32(bytes,60,128);
  memcpy(bytes+128,"PE\0\0",4);
  store_u16(bytes,132,UINT16_C(0x014c));
  store_u16(bytes,134,1);
  store_u16(bytes,148,UINT16_C(0x00e0));
  memcpy(bytes+376,".rsrc",5);
  store_u32(bytes,376+16,(uint32_t)(size-512u));
  store_u32(bytes,376+20,512);
  memcpy(bytes+520,"WEXTRACT",8);
  memcpy(bytes+640,cabinet,cabinet_size);
  output->data=bytes;
  output->size=output->capacity=size;
  return 1;
}

/* Divide the checked-in neutral compressed stream at a CFDATA boundary that falls inside the
 * LZX bitstream. A decoder must continue its input state across the two records. */
static int split_lzx_data_record(const uint8_t *source,size_t source_size,Buffer *output){
  if(!source || source_size<52u || !output || memcmp(source,"MSCF",4u) ||
     load_u16(source+26u)!=1u || load_u16(source+40u)!=1u) return 0;
  uint32_t data_offset=load_u32(source+36u);
  if(data_offset>source_size-9u) return 0;
  uint16_t compressed=load_u16(source+data_offset+4u);
  uint16_t expanded=load_u16(source+data_offset+6u);
  if(compressed<2u || expanded<2u || data_offset+8u+(size_t)compressed!=source_size ||
     source_size>UINT32_MAX-8u) return 0;
  uint8_t *bytes=malloc(source_size+8u);
  if(!bytes) return 0;
  memcpy(bytes,source,data_offset);
  memset(bytes+data_offset,0,8u);
  store_u16(bytes,data_offset+4u,1u);
  store_u16(bytes,data_offset+6u,1u);
  bytes[data_offset+8u]=source[data_offset+8u];
  memset(bytes+data_offset+9u,0,8u);
  store_u16(bytes,data_offset+13u,(uint16_t)(compressed-1u));
  store_u16(bytes,data_offset+15u,(uint16_t)(expanded-1u));
  memcpy(bytes+data_offset+17u,source+data_offset+9u,(size_t)compressed-1u);
  store_u32(bytes,8u,(uint32_t)(source_size+8u));
  store_u16(bytes,40u,2u);
  output->data=bytes;
  output->size=output->capacity=source_size+8u;
  return 1;
}

static int build_multipart_cabinet(const uint8_t *source,size_t source_size,Buffer *output){
  static const uint8_t names[]={ 'p','a','r','t',0,'d','i','s','k',0 };
  if(!source || source_size<44u || source_size>UINT32_MAX-sizeof names) return 0;
  uint8_t *bytes=malloc(source_size+sizeof names);
  if(!bytes) return 0;
  memcpy(bytes,source,36u);
  memcpy(bytes+36u,names,sizeof names);
  memcpy(bytes+36u+sizeof names,source+36u,source_size-36u);
  store_u32(bytes,8,(uint32_t)(source_size+sizeof names));
  store_u32(bytes,16,(uint32_t)(44u+sizeof names));
  store_u16(bytes,30,1u);
  store_u32(bytes,36u+sizeof names,load_u32(source+36u)+(uint32_t)sizeof names);
  output->data=bytes;
  output->size=output->capacity=source_size+sizeof names;
  return 1;
}

/* Add one empty regular member to the checked-in single-folder fixture without changing its LZX
 * stream. This keeps collision and native-member cases structurally valid through CAB header
 * parsing and makes them exercise extraction policy rather than malformed-table rejection. */
static int cabinet_append_empty_member(const uint8_t *source,size_t source_size,
                                       const char *name,Buffer *output){
  if(!source || source_size<44u || !name || !name[0] || !output ||
     memcmp(source,"MSCF\0\0\0\0",8u) || load_u32(source+8u)!=source_size ||
     load_u16(source+26u)!=1u || !load_u16(source+28u) ||
     load_u16(source+28u)==UINT16_MAX || load_u16(source+30u)!=0u) return 0;
  uint32_t files_offset=load_u32(source+16u);
  uint32_t data_offset=load_u32(source+36u);
  uint16_t file_count=load_u16(source+28u);
  if(files_offset<44u || data_offset<files_offset || data_offset>source_size) return 0;
  size_t cursor=files_offset;
  uint64_t expanded_end=0;
  for(uint16_t index=0;index<file_count;index++){
    if(cursor>data_offset || data_offset-cursor<16u || load_u16(source+cursor+8u)!=0u)
      return 0;
    uint64_t member_offset=load_u32(source+cursor+4u);
    uint64_t member_size=load_u32(source+cursor);
    if(member_offset!=expanded_end || member_size>UINT32_MAX-member_offset) return 0;
    expanded_end=member_offset+member_size;
    cursor+=16u;
    const uint8_t *end=memchr(source+cursor,0,data_offset-cursor);
    if(!end || end==source+cursor) return 0;
    cursor=(size_t)(end-source)+1u;
  }
  if(cursor!=data_offset || expanded_end>UINT32_MAX) return 0;
  size_t name_size=strlen(name);
  if(name_size>ANYGM_CONTENT_MAX_MEMBER_PATH || name_size>SIZE_MAX-17u) return 0;
  size_t inserted=16u+name_size+1u;
  if(inserted>UINT32_MAX-source_size || source_size>SIZE_MAX-inserted) return 0;
  uint8_t *bytes=malloc(source_size+inserted);
  if(!bytes) return 0;
  memcpy(bytes,source,data_offset);
  memset(bytes+data_offset,0,16u);
  store_u32(bytes,data_offset,(uint32_t)0u);
  store_u32(bytes,data_offset+4u,(uint32_t)expanded_end);
  store_u16(bytes,data_offset+8u,0u);
  store_u16(bytes,data_offset+14u,UINT16_C(0x20));
  memcpy(bytes+data_offset+16u,name,name_size+1u);
  memcpy(bytes+data_offset+inserted,source+data_offset,source_size-data_offset);
  store_u32(bytes,8u,(uint32_t)(source_size+inserted));
  store_u16(bytes,28u,(uint16_t)(file_count+1u));
  store_u32(bytes,36u,data_offset+(uint32_t)inserted);
  output->data=bytes;
  output->size=output->capacity=source_size+inserted;
  return 1;
}

static int embedded_cabinet_memory_vfs_case(const Buffer *executable){
  const size_t source_size=256u*1024u;
  uint8_t *source=calloc(source_size,1);
  if(!source || executable->size>source_size){ free(source); return 0; }
  memcpy(source,executable->data,executable->size);
  AnygmMemoryVfs memory;
  AnygmHostServices services;
  anygm_memory_vfs_init(&memory,&services);
  int ok=anygm_memory_vfs_add_file(&memory,"mem/source.exe",source,source_size);
  free(source);
  AnygmContentRouter router={0};
  router.host=&services;
  router.cache_directory="mem/cache";
  AnygmEmbeddedCab cab={0};
  ok=ok && anygm_embedded_cab_probe(&router,"mem/source.exe",&cab)==
             ANYGM_EMBEDDED_CAB_SUPPORTED &&
     memory.max_read_request<=64u*1024u && memory.max_read_request<source_size;
  anygm_memory_vfs_guard_reads(&memory,"mem/source.exe",cab.offset,cab.size);
  char payload[1024],asset_root[1024],external[1200];
  ok=ok && anygm_embedded_cab_extract(&router,"mem/source.exe",&cab,payload,sizeof payload,
                                      asset_root,sizeof asset_root) &&
     !memory.read_violation && memory.max_read_request<=64u*1024u;
  if(ok){
    uint8_t magic[4],asset[23];
    void *file=services.file_open(services.userdata,payload,ANYGM_FILE_READ);
    ok=file && services.file_read(services.userdata,file,magic,sizeof magic)==sizeof magic &&
       !memcmp(magic,"FORM",4);
    if(file) services.file_close(services.userdata,file);
    ok=ok && snprintf(external,sizeof external,"%s/assets/external asset.txt",asset_root)<
               (int)sizeof external;
    file=ok?services.file_open(services.userdata,external,ANYGM_FILE_READ):NULL;
    ok=ok && file && services.file_read(services.userdata,file,asset,sizeof asset)==sizeof asset &&
       !memcmp(asset,"neutral external asset\n",sizeof asset);
    if(file) services.file_close(services.userdata,file);
  }
  anygm_memory_vfs_destroy(&memory);
  return ok?1:fail("the callback-only Cabinet route crossed its memory VFS bounds");
}

static int memory_vfs_has_cabinet_transaction(const AnygmMemoryVfs *memory){
  for(size_t index=0;index<memory->node_count;index++)
    if(strstr(memory->nodes[index].path,"-anygm-cab")) return 1;
  return 0;
}

static int embedded_cabinet_source_stability_case(const Buffer *executable){
  const size_t source_size=256u*1024u;
  uint8_t *source=calloc(source_size,1);
  if(!source || executable->size>source_size){ free(source); return 0; }
  memcpy(source,executable->data,executable->size);

  AnygmMemoryVfs memory;
  AnygmHostServices services;
  AnygmContentRouter router={0};
  AnygmEmbeddedCab cab={0};
  char payload[1024],asset_root[1024];
  anygm_memory_vfs_init(&memory,&services);
  router.host=&services;
  router.cache_directory="mem/cache";
  int ok=anygm_memory_vfs_add_file(&memory,"mem/source.exe",source,source_size) &&
    anygm_embedded_cab_probe(&router,"mem/source.exe",&cab)==ANYGM_EMBEDDED_CAB_SUPPORTED &&
    anygm_embedded_cab_extract(&router,"mem/source.exe",&cab,payload,sizeof payload,
                               asset_root,sizeof asset_root) &&
    anygm_embedded_cab_probe(&router,"mem/source.exe",&cab)==ANYGM_EMBEDDED_CAB_SUPPORTED &&
    anygm_memory_vfs_xor_byte(&memory,"mem/source.exe",source_size-1u,UINT8_C(1));
  payload[0]=0;
  asset_root[0]=0;
  ok=ok && !anygm_embedded_cab_extract(&router,"mem/source.exe",&cab,payload,sizeof payload,
                                        asset_root,sizeof asset_root) &&
     !payload[0] && !asset_root[0];
  anygm_memory_vfs_destroy(&memory);
  if(!ok){ free(source); return fail("a warm Cabinet cache outlived its probed source bytes"); }

  anygm_memory_vfs_init(&memory,&services);
  router.host=&services;
  ok=anygm_memory_vfs_add_file(&memory,"mem/source.exe",source,source_size) &&
    anygm_embedded_cab_probe(&router,"mem/source.exe",&cab)==ANYGM_EMBEDDED_CAB_SUPPORTED &&
    anygm_memory_vfs_xor_byte(&memory,"mem/source.exe",source_size-1u,UINT8_C(1));
  payload[0]=0;
  asset_root[0]=0;
  ok=ok && !anygm_embedded_cab_extract(&router,"mem/source.exe",&cab,payload,sizeof payload,
                                        asset_root,sizeof asset_root) &&
     !payload[0] && !asset_root[0] && !memory_vfs_has_cabinet_transaction(&memory);
  anygm_memory_vfs_destroy(&memory);
  free(source);
  return ok?1:fail("a cold Cabinet cache was published under stale source bytes");
}

static int embedded_cabinet_probe_snapshot_case(const Buffer *executable){
  const size_t source_size=256u*1024u;
  uint8_t *source=calloc(source_size,1);
  uint8_t *replacement=calloc(source_size,1);
  if(!source || !replacement || executable->size>source_size){
    free(source); free(replacement); return 0;
  }
  memcpy(source,executable->data,executable->size);
  memcpy(replacement,source,source_size);
  replacement[0]^=UINT8_C(1);

  AnygmMemoryVfs memory;
  AnygmHostServices services;
  AnygmContentRouter router={0};
  AnygmEmbeddedCab cab;
  memset(&cab,0xa5,sizeof cab);
  anygm_memory_vfs_init(&memory,&services);
  router.host=&services;
  router.cache_directory="mem/cache";
  int ok=anygm_memory_vfs_add_file(&memory,"mem/source.exe",source,source_size);
  anygm_memory_vfs_replace_on_read_open(&memory,"mem/source.exe",2u,replacement,source_size);
  AnygmEmbeddedCabStatus status=anygm_embedded_cab_probe(&router,"mem/source.exe",&cab);
  ok=ok && memory.replacement_complete && status==ANYGM_EMBEDDED_CAB_INVALID &&
     !cab.source_size && !cab.source_hash && !cab.offset && !cab.size && !cab.profile;
  anygm_memory_vfs_destroy(&memory);
  free(source);
  free(replacement);
  return ok?1:fail("a Cabinet probe mixed structure and identity from different source snapshots");
}

static int embedded_cabinet_extraction_snapshot_case(const Buffer *executable){
  const size_t source_size=256u*1024u;
  uint8_t *source=calloc(source_size,1);
  uint8_t *snapshot=calloc(source_size,1);
  if(!source || !snapshot || executable->size>source_size){
    free(source); free(snapshot); return 0;
  }
  memcpy(source,executable->data,executable->size);
  memcpy(snapshot,source,source_size);
  snapshot[source_size-1u]^=UINT8_C(1);

  AnygmMemoryVfs memory;
  AnygmHostServices services;
  AnygmContentRouter router={0};
  AnygmEmbeddedCab cab={0};
  char payload[1024]="sentinel",asset_root[1024]="sentinel";
  anygm_memory_vfs_init(&memory,&services);
  router.host=&services;
  router.cache_directory="mem/cache";
  int ok=hash64_bytes(source,source_size)!=hash64_bytes(snapshot,source_size) &&
    anygm_memory_vfs_add_file(&memory,"mem/source.exe",source,source_size);
  anygm_memory_vfs_snapshot_on_read_open(&memory,"mem/source.exe",3u,snapshot,source_size);
  ok=ok && anygm_embedded_cab_probe(&router,"mem/source.exe",&cab)==
             ANYGM_EMBEDDED_CAB_SUPPORTED;
  payload[0]=0;
  asset_root[0]=0;
  ok=ok && !anygm_embedded_cab_extract(&router,"mem/source.exe",&cab,payload,sizeof payload,
                                        asset_root,sizeof asset_root) &&
     memory.snapshot_complete && memory.snapshot_read_opens>=3u &&
     !payload[0] && !asset_root[0] && !memory_vfs_has_cabinet_transaction(&memory);
  anygm_memory_vfs_destroy(&memory);
  free(source);
  free(snapshot);
  return ok?1:fail("a cold Cabinet extraction published a different per-open source snapshot");
}

static int embedded_cabinet_result_capacity_case(const Buffer *executable){
  const size_t source_size=256u*1024u;
  uint8_t *source=calloc(source_size,1);
  if(!source || executable->size>source_size){ free(source); return 0; }
  memcpy(source,executable->data,executable->size);

  AnygmMemoryVfs memory;
  AnygmHostServices services;
  AnygmContentRouter router={0};
  AnygmEmbeddedCab cab={0};
  char payload[1024]="sentinel",asset_root[1]={'x'};
  anygm_memory_vfs_init(&memory,&services);
  router.host=&services;
  router.cache_directory="mem/cache";
  int cold_ok=anygm_memory_vfs_add_file(&memory,"mem/source.exe",source,source_size) &&
    anygm_embedded_cab_probe(&router,"mem/source.exe",&cab)==ANYGM_EMBEDDED_CAB_SUPPORTED &&
    !anygm_embedded_cab_extract(&router,"mem/source.exe",&cab,payload,sizeof payload,
                                asset_root,sizeof asset_root) &&
    !payload[0] && !asset_root[0] && !memory_vfs_has_cabinet_transaction(&memory);
  anygm_memory_vfs_destroy(&memory);

  char expected_payload[1024],expected_root[1024];
  payload[0]=0;
  asset_root[0]='x';
  anygm_memory_vfs_init(&memory,&services);
  router.host=&services;
  int warm_ok=anygm_memory_vfs_add_file(&memory,"mem/source.exe",source,source_size) &&
    anygm_embedded_cab_probe(&router,"mem/source.exe",&cab)==ANYGM_EMBEDDED_CAB_SUPPORTED &&
    anygm_embedded_cab_extract(&router,"mem/source.exe",&cab,expected_payload,
                               sizeof expected_payload,expected_root,sizeof expected_root);
  size_t warm_nodes=memory.node_count;
  AnygmHostServices no_write=services;
  no_write.file_write=counted_reject_write;
  router.host=&no_write;
  counted_reject_write_calls=0;
  warm_ok=warm_ok &&
    !anygm_embedded_cab_extract(&router,"mem/source.exe",&cab,payload,sizeof payload,
                                asset_root,sizeof asset_root) &&
    !payload[0] && !asset_root[0] && !counted_reject_write_calls &&
    memory.node_count==warm_nodes;
  anygm_memory_vfs_destroy(&memory);
  free(source);

  int ok=1;
  if(!cold_ok) ok=fail("an undersized cold Cabinet result published output or cache state");
  if(!warm_ok) ok=fail("an undersized warm Cabinet result rebuilt or published output");
  return ok;
}

static int embedded_cabinet_entry_metadata_case(void){
  return anygm_embedded_cab_entry_allowed(ANYGM_EMBEDDED_CAB_ENTRY_REGULAR,0,0) &&
    !anygm_embedded_cab_entry_allowed(ANYGM_EMBEDDED_CAB_ENTRY_DIRECTORY,0,0) &&
    !anygm_embedded_cab_entry_allowed(ANYGM_EMBEDDED_CAB_ENTRY_SYMLINK,0,1) &&
    !anygm_embedded_cab_entry_allowed(ANYGM_EMBEDDED_CAB_ENTRY_SPECIAL,0,0) &&
    !anygm_embedded_cab_entry_allowed(ANYGM_EMBEDDED_CAB_ENTRY_REGULAR,1,0) &&
    !anygm_embedded_cab_entry_allowed(ANYGM_EMBEDDED_CAB_ENTRY_REGULAR,0,1);
}

static int embedded_cabinet_limit_policy_case(void){
  return anygm_embedded_cab_limits_allowed(UINT64_C(65536),1,1024,1024) &&
    !anygm_embedded_cab_limits_allowed(UINT64_C(65536),
                                       ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES+1u,1,1) &&
    !anygm_embedded_cab_limits_allowed(UINT64_C(65536),1,
                                       ANYGM_CONTENT_MAX_MEMBER_BYTES+1u,1) &&
    !anygm_embedded_cab_limits_allowed(UINT64_C(65536),1,1,
                                       ANYGM_CONTENT_MAX_EXTRACTED_BYTES+1u) &&
    !anygm_embedded_cab_limits_allowed(1,1,ANYGM_CONTENT_EXPANSION_ALLOWANCE+1u,
                                       ANYGM_CONTENT_EXPANSION_ALLOWANCE+1u);
}

static int embedded_cabinet_marker_budget_case(void){
  const unsigned entries=ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES;
  const size_t payload_path_size=8u;
  const size_t fixed=256u+payload_path_size*2u+(size_t)entries*80u;
  if(fixed>=ANYGM_EMBEDDED_CAB_MARKER_MAX_BYTES ||
     ((ANYGM_EMBEDDED_CAB_MARKER_MAX_BYTES-fixed)&1u)) return 0;
  const size_t accepted_path_bytes=(ANYGM_EMBEDDED_CAB_MARKER_MAX_BYTES-fixed)/2u;
  size_t accepted_budget=0,rejected_budget=1u;
  return accepted_path_bytes>=(size_t)entries &&
    accepted_path_bytes<(size_t)entries*ANYGM_CONTENT_MAX_MEMBER_PATH &&
    anygm_embedded_cab_marker_budget_allowed(payload_path_size,entries,accepted_path_bytes,
                                              &accepted_budget) &&
    accepted_budget==ANYGM_EMBEDDED_CAB_MARKER_MAX_BYTES &&
    !anygm_embedded_cab_marker_budget_allowed(payload_path_size,entries,
                                               accepted_path_bytes+1u,&rejected_budget) &&
    !rejected_budget;
}

static int embedded_cabinet_name_index_case(void){
  const size_t entries=ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES-8u;
  char **paths=calloc(entries,sizeof *paths);
  if(!paths) return fail("could not allocate Cabinet name-index paths");
  int built=1;
  for(size_t index=0;index<entries;index++){
    paths[index]=malloc(128u);
    if(!paths[index] || snprintf(paths[index],128u,
        "assets/common-prefix-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/%05zu.bin",
        index)>=128){
      built=0;
      break;
    }
  }
  static const char miss[]=
    "assets/common-prefix-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/missing.bin";
  const char *lookups[2]={built?paths[entries-1u]:NULL,miss};
  AnygmEmbeddedCabNameMetrics near={0};
  uint64_t operation_bound=(uint64_t)(entries+2u)*ANYGM_EMBEDDED_CAB_NAME_MAX_PROBES;
  int measured=built && anygm_embedded_cab_name_index_measure(
    (const char *const *)paths,entries,lookups,2u,0,&near);

  const char *exact_paths[]={"Folder/Asset.bin"};
  const char *exact_lookups[]={"Folder/Asset.bin","folder/asset.bin"};
  AnygmEmbeddedCabNameMetrics exact={0};
  measured=measured && anygm_embedded_cab_name_index_measure(
    exact_paths,1u,exact_lookups,2u,0,&exact);

  const char *duplicate_paths[]={"Folder/Asset.bin","folder/ASSET.bin"};
  AnygmEmbeddedCabNameMetrics duplicate={0};
  measured=measured && anygm_embedded_cab_name_index_measure(
    duplicate_paths,2u,NULL,0,0,&duplicate);

  const char *unsafe_policy_paths[]={
    "CON","prn.ext","folder/AuX.bin","folder/nul","COM1.cfg","com9",
    "LPT1.log","lpt9.tmp","asset.","asset ",
  };
  int policy_ok=1;
  for(size_t index=0;
      policy_ok && index<sizeof unsafe_policy_paths/sizeof unsafe_policy_paths[0];index++){
    AnygmEmbeddedCabNameMetrics policy={0};
    policy_ok=anygm_embedded_cab_name_index_measure(&unsafe_policy_paths[index],1u,NULL,0,0,
                                                     &policy) &&
              policy.rejected && !policy.inserted;
  }
  const char *portable_policy_paths[]={
    "COM0","COM10.ext","LPT0","LPT10.ext","console","asset.name","asset name",
  };
  AnygmEmbeddedCabNameMetrics portable_policy={0};
  policy_ok=policy_ok && anygm_embedded_cab_name_index_measure(
    portable_policy_paths,sizeof portable_policy_paths/sizeof portable_policy_paths[0],
    NULL,0,0,&portable_policy) && !portable_policy.rejected &&
    portable_policy.inserted==sizeof portable_policy_paths/sizeof portable_policy_paths[0];

  const size_t collision_count=ANYGM_EMBEDDED_CAB_NAME_MAX_PROBES+1u;
  const char **collisions=calloc(collision_count,sizeof *collisions);
  int collisions_built=collisions!=NULL;
  for(size_t index=0;collisions_built && index<collision_count;index++){
    char *path=malloc(32u);
    if(!path || snprintf(path,32u,"collision/%03zu.bin",index)>=32){
      free(path);
      collisions_built=0;
      break;
    }
    collisions[index]=path;
  }
  AnygmEmbeddedCabNameMetrics collision={0};
  measured=measured && collisions_built && anygm_embedded_cab_name_index_measure(
    collisions,collision_count,NULL,0,1,&collision);

  int ok=measured && !near.rejected && !near.work_exhausted && near.inserted==entries &&
         near.hits==1u && near.probes<=operation_bound &&
         near.equality_bytes<=operation_bound*(ANYGM_CONTENT_MAX_MEMBER_PATH+1u) &&
         !exact.rejected && exact.inserted==1u && exact.hits==1u &&
         duplicate.rejected && duplicate.inserted==1u && !duplicate.work_exhausted &&
         policy_ok &&
         collision.rejected && collision.inserted==ANYGM_EMBEDDED_CAB_NAME_MAX_PROBES &&
         !collision.work_exhausted &&
         collision.probes<=((uint64_t)collision_count*ANYGM_EMBEDDED_CAB_NAME_MAX_PROBES);
  fprintf(stderr,
    "content security: Cabinet name index near inserted=%zu hits=%zu probes=%llu bytes=%llu "
    "rejected=%d exhausted=%d; exact hits=%zu; duplicate inserted=%zu rejected=%d; "
    "collision inserted=%zu probes=%llu rejected=%d exhausted=%d\n",
    near.inserted,near.hits,(unsigned long long)near.probes,
    (unsigned long long)near.equality_bytes,near.rejected,near.work_exhausted,
    exact.hits,duplicate.inserted,duplicate.rejected,collision.inserted,
    (unsigned long long)collision.probes,collision.rejected,collision.work_exhausted);
  for(size_t index=0;collisions && index<collision_count;index++) free((void *)collisions[index]);
  free(collisions);
  for(size_t index=0;index<entries;index++) free(paths[index]);
  free(paths);
  return ok?1:fail("Cabinet manifest name work was not linearly bounded");
}

static int embedded_lzx_cabinet_cases(const AnygmHostServices *services,const char *root){
  uint8_t *cabinet=NULL;
  size_t cabinet_size=0;
  Buffer split={0};
  Buffer executable={0};
  if(!read_file("tests/fixtures/embedded_cab_lzx21.cab",&cabinet,&cabinet_size) ||
     !split_lzx_data_record(cabinet,cabinet_size,&split) ||
     !build_lzx_executable(split.data,split.size,&executable)){
    free(split.data);
    free(cabinet);
    return fail("could not read the neutral LZX-21 fixture");
  }
  free(cabinet);
  cabinet=split.data;
  cabinet_size=split.size;
  char path[512],resolved[1024],warm[1024],asset_root[1024];
  if(snprintf(path,sizeof path,"%s/neutral-lzx.exe",root)>=(int)sizeof path ||
     !write_file(path,executable.data,executable.size)){
    free(executable.data); free(cabinet);
    return fail("could not stage the neutral LZX executable");
  }
  AnygmContentRouter router={0};
  router.host=services;
  router.cache_directory=root;
  router.log=fixture_log;
  AnygmEmbeddedCab parsed={0};
  if(anygm_embedded_cab_probe(&router,path,&parsed)!=ANYGM_EMBEDDED_CAB_SUPPORTED ||
     parsed.offset!=640u || parsed.size!=cabinet_size || parsed.profile!=UINT32_C(0x001503)){
    free(executable.data); free(cabinet);
    return fail("the neutral LZX-21 Cabinet was not classified at its PE subrange");
  }
  if(!embedded_cabinet_memory_vfs_case(&executable)){
    free(executable.data); free(cabinet); return 0;
  }
  if(!embedded_cabinet_source_stability_case(&executable)){
    free(executable.data); free(cabinet); return 0;
  }
  if(!embedded_cabinet_marker_budget_case()){
    free(executable.data); free(cabinet);
    return fail("the Cabinet marker writer exceeded the warm reader's shared budget");
  }
  if(!embedded_cabinet_name_index_case()){
    free(executable.data); free(cabinet); return 0;
  }
  if(!embedded_cabinet_probe_snapshot_case(&executable)){
    free(executable.data); free(cabinet); return 0;
  }
  if(!embedded_cabinet_extraction_snapshot_case(&executable)){
    free(executable.data); free(cabinet); return 0;
  }
  if(!embedded_cabinet_result_capacity_case(&executable)){
    free(executable.data); free(cabinet); return 0;
  }
  if(!embedded_cabinet_entry_metadata_case()){
    free(executable.data); free(cabinet);
    return fail("Cabinet link or special-file metadata was accepted");
  }
  if(!embedded_cabinet_limit_policy_case()){
    free(executable.data); free(cabinet);
    return fail("Cabinet entry, member, total, or expansion limits were not enforced");
  }
  if(anygm_content_resolve_path(&router,path,resolved,sizeof resolved,asset_root,
                                sizeof asset_root,NULL,0)!=ANYGM_CONTENT_RESOLVE_OK ||
     !strstr(resolved,"data.win") || !asset_root[0]){
    free(executable.data); free(cabinet);
    return fail("cold LZX-21 extraction did not select the payload and asset root");
  }
  uint8_t magic[4];
  char external[1280];
  uint8_t *asset=NULL; size_t asset_size=0;
  if(!read_prefix(resolved,magic,sizeof magic) || memcmp(magic,"FORM",4) ||
     snprintf(external,sizeof external,"%s/assets/external asset.txt",asset_root)>=(int)sizeof external ||
     !read_file(external,&asset,&asset_size) || asset_size!=23u ||
     memcmp(asset,"neutral external asset\n",23u)){
    free(asset); free(executable.data); free(cabinet);
    return fail("cold LZX-21 extraction changed its payload or external asset");
  }
  free(asset);

  AnygmHostServices no_write=*services;
  no_write.file_write=reject_write;
  router.host=&no_write;
  if(anygm_content_resolve_path(&router,path,warm,sizeof warm,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_OK || strcmp(warm,resolved)){
    free(executable.data); free(cabinet);
    return fail("warm LZX-21 reuse attempted to decompress or rewrite");
  }
  router.host=services;

  static const uint8_t corrupt[]="corrupt";
  if(!write_file(external,corrupt,sizeof corrupt) ||
     anygm_content_resolve_path(&router,path,warm,sizeof warm,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_OK || !read_file(external,&asset,&asset_size) ||
     asset_size!=23u || memcmp(asset,"neutral external asset\n",23u)){
    free(asset); free(executable.data); free(cabinet);
    return fail("a corrupt external asset was reused");
  }
  free(asset);
  if(!write_file(resolved,corrupt,sizeof corrupt) ||
     anygm_content_resolve_path(&router,path,warm,sizeof warm,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_OK || !read_prefix(warm,magic,sizeof magic) || memcmp(magic,"FORM",4)){
    free(executable.data); free(cabinet);
    return fail("a corrupt cached payload was reused");
  }
  char marker[1280];
  if(snprintf(marker,sizeof marker,"%s/.anygm_cab_cache",asset_root)>=(int)sizeof marker ||
     !write_file(marker,corrupt,sizeof corrupt) ||
     anygm_content_resolve_path(&router,path,warm,sizeof warm,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_OK || !read_prefix(warm,magic,sizeof magic) || memcmp(magic,"FORM",4)){
    free(executable.data); free(cabinet);
    return fail("a corrupt Cabinet marker was reused");
  }
  if(unlink(external)!=0 ||
     anygm_content_resolve_path(&router,path,warm,sizeof warm,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_OK || !read_file(external,&asset,&asset_size) ||
     asset_size!=23u || memcmp(asset,"neutral external asset\n",23u)){
    free(asset); free(executable.data); free(cabinet);
    return fail("a Cabinet cache with a missing manifest member was reused");
  }
  free(asset);
  char extra[1280];
  if(snprintf(extra,sizeof extra,"%s/untracked.bin",asset_root)>=(int)sizeof extra ||
     !write_file(extra,"extra",5u) ||
     anygm_content_resolve_path(&router,path,warm,sizeof warm,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_OK || access(extra,F_OK)==0 || errno!=ENOENT ||
     !read_prefix(warm,magic,sizeof magic) || memcmp(magic,"FORM",4)){
    free(executable.data); free(cabinet);
    return fail("a Cabinet cache with an extra unmanifested member was reused");
  }

  AnygmEngine *engine=NULL;
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=path;
  source.cache_directory=root;
  if(anygm_create(services,&engine)!=ANYGM_OK || !engine ||
     anygm_load(engine,&source,NULL)!=ANYGM_OK){
    if(engine) anygm_destroy(engine);
    free(executable.data); free(cabinet);
    return fail("the extracted LZX-21 payload did not pass through the normal loader");
  }
  if(strcmp(engine->win.content_dir,asset_root) ||
     snprintf(external,sizeof external,"%s/assets/external asset.txt",engine->win.content_dir)>=
       (int)sizeof external || !read_file(external,&asset,&asset_size) || asset_size!=23u ||
     memcmp(asset,"neutral external asset\n",23u)){
    free(asset);
    anygm_destroy(engine);
    free(executable.data); free(cabinet);
    return fail("a direct Cabinet engine load discarded its extracted asset root");
  }
  free(asset); asset=NULL;
  anygm_destroy(engine);

  char anchor[512];
  static const char anchor_text[]="neutral-lzx.exe\n";
  if(snprintf(anchor,sizeof anchor,"%s/neutral-lzx.anygm",root)>=(int)sizeof anchor ||
     !write_file(anchor,anchor_text,sizeof anchor_text-1u)){
    free(executable.data); free(cabinet);
    return fail("could not stage the neutral Cabinet anchor");
  }
  source.path=anchor;
  engine=NULL;
  if(anygm_create(services,&engine)!=ANYGM_OK || !engine ||
     anygm_load(engine,&source,NULL)!=ANYGM_OK || strcmp(engine->win.content_dir,asset_root) ||
     snprintf(external,sizeof external,"%s/assets/external asset.txt",engine->win.content_dir)>=
       (int)sizeof external || !read_file(external,&asset,&asset_size) || asset_size!=23u ||
     memcmp(asset,"neutral external asset\n",23u)){
    free(asset);
    if(engine) anygm_destroy(engine);
    free(executable.data); free(cabinet);
    return fail("an anchored Cabinet engine load discarded its extracted asset root");
  }
  free(asset); asset=NULL;
  anygm_destroy(engine);

  uint8_t *unsupported=malloc(executable.size);
  if(!unsupported){ free(executable.data); free(cabinet); return 0; }
  memcpy(unsupported,executable.data,executable.size);
  store_u16(unsupported,640u+42u,UINT16_C(1));
  if(snprintf(path,sizeof path,"%s/unsupported-compression.exe",root)>=(int)sizeof path ||
     !write_file(path,unsupported,executable.size) ||
     anygm_embedded_cab_probe(&router,path,&parsed)!=ANYGM_EMBEDDED_CAB_UNSUPPORTED){
    free(unsupported); free(executable.data); free(cabinet);
    return fail("a well-formed unsupported Cabinet compression was misclassified");
  }
  free(unsupported);

  Buffer multipart={0},multipart_executable={0};
  if(!build_multipart_cabinet(cabinet,cabinet_size,&multipart) ||
     !build_lzx_executable(multipart.data,multipart.size,&multipart_executable) ||
     snprintf(path,sizeof path,"%s/multipart-lzx.exe",root)>=(int)sizeof path ||
     !write_file(path,multipart_executable.data,multipart_executable.size) ||
     anygm_embedded_cab_probe(&router,path,&parsed)!=ANYGM_EMBEDDED_CAB_UNSUPPORTED){
    free(multipart.data); free(multipart_executable.data);
    free(executable.data); free(cabinet);
    return fail("a well-formed multipart Cabinet was misclassified");
  }
  free(multipart.data); free(multipart_executable.data);

  uint8_t *continuation=malloc(executable.size);
  if(!continuation){ free(executable.data); free(cabinet); return 0; }
  memcpy(continuation,executable.data,executable.size);
  store_u16(continuation,640u+52u,UINT16_C(0xfffd));
  if(snprintf(path,sizeof path,"%s/continuation-lzx.exe",root)>=(int)sizeof path ||
     !write_file(path,continuation,executable.size) ||
     anygm_embedded_cab_probe(&router,path,&parsed)!=ANYGM_EMBEDDED_CAB_UNSUPPORTED){
    free(continuation); free(executable.data); free(cabinet);
    return fail("a well-formed continuation Cabinet was misclassified");
  }
  free(continuation);

  const struct { const char *filename; size_t folder_offset; uint16_t folder; } bad_continuations[]={
    {"late-from-previous-lzx.exe",77u,UINT16_C(0xfffd)},
    {"early-to-next-lzx.exe",52u,UINT16_C(0xfffe)},
    {"multi-file-both-lzx.exe",52u,UINT16_C(0xffff)},
  };
  for(size_t index=0;index<sizeof bad_continuations/sizeof bad_continuations[0];index++){
    uint8_t *malformed=malloc(executable.size);
    if(!malformed){ free(executable.data); free(cabinet); return 0; }
    memcpy(malformed,executable.data,executable.size);
    store_u16(malformed,640u+bad_continuations[index].folder_offset,
              bad_continuations[index].folder);
    if(snprintf(path,sizeof path,"%s/%s",root,bad_continuations[index].filename)>=
         (int)sizeof path || !write_file(path,malformed,executable.size) ||
       anygm_embedded_cab_probe(&router,path,&parsed)!=ANYGM_EMBEDDED_CAB_INVALID){
      free(malformed); free(executable.data); free(cabinet);
      return fail("malformed Cabinet continuation placement was accepted as unsupported");
    }
    free(malformed);
  }

  uint8_t *checksum=malloc(executable.size);
  if(!checksum){ free(executable.data); free(cabinet); return 0; }
  memcpy(checksum,executable.data,executable.size);
  uint32_t data_offset=load_u32(cabinet+36u);
  checksum[640u+data_offset]^=UINT8_C(0x80);
  if(snprintf(path,sizeof path,"%s/checksum-lzx.exe",root)>=(int)sizeof path ||
     !write_file(path,checksum,executable.size) ||
     anygm_content_resolve_path(&router,path,warm,sizeof warm,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_INVALID){
    free(checksum); free(executable.data); free(cabinet);
    return fail("a bad CFDATA checksum was accepted");
  }
  free(checksum);

  if(snprintf(path,sizeof path,"%s/truncated-lzx.exe",root)>=(int)sizeof path ||
     !write_file(path,executable.data,executable.size-1u) ||
     anygm_content_resolve_path(&router,path,warm,sizeof warm,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_INVALID){
    free(executable.data); free(cabinet);
    return fail("truncated Cabinet data was accepted");
  }

  const struct { const char *filename; const char replacement[9]; } unsafe_names[]={
    {"absolute-lzx.exe","/bad.win"},
    {"device-lzx.exe","C:badwin"},
  };
  for(size_t index=0;index<sizeof unsafe_names/sizeof unsafe_names[0];index++){
    uint8_t *unsafe=malloc(executable.size);
    if(!unsafe){ free(executable.data); free(cabinet); return 0; }
    memcpy(unsafe,executable.data,executable.size);
    uint8_t *unsafe_name=memmem(unsafe+640,cabinet_size,"data.win",8);
    if(!unsafe_name){ free(unsafe); free(executable.data); free(cabinet); return 0; }
    memcpy(unsafe_name,unsafe_names[index].replacement,8);
    if(snprintf(path,sizeof path,"%s/%s",root,unsafe_names[index].filename)>=(int)sizeof path ||
       !write_file(path,unsafe,executable.size) ||
       anygm_content_resolve_path(&router,path,warm,sizeof warm,NULL,0,NULL,0)!=
         ANYGM_CONTENT_RESOLVE_INVALID){
      free(unsafe); free(executable.data); free(cabinet);
      return fail("an absolute or device Cabinet path was accepted");
    }
    free(unsafe);
  }

  const struct {
    const char *filename;
    const char *member;
    const char *cache_prefix;
    const char *message;
  } unsafe_aliases[]={
    {"reserved-nul-alias-lzx.exe","assets\\NuL.win","reserved-nul-alias-lzx-",
     "a Windows reserved device basename with an extension was accepted"},
    {"reserved-com-alias-lzx.exe","assets\\cOm1.txt","reserved-com-alias-lzx-",
     "a Windows COM device basename with an extension was accepted"},
    {"reserved-lpt-alias-lzx.exe","assets\\LpT9.dat","reserved-lpt-alias-lzx-",
     "a Windows LPT device basename with an extension was accepted"},
    {"trailing-dot-alias-lzx.exe","assets\\external asset.txt.",
     "trailing-dot-alias-lzx-","a trailing-dot Cabinet path alias was accepted"},
    {"trailing-space-alias-lzx.exe","assets\\external asset.txt ",
     "trailing-space-alias-lzx-","a trailing-space Cabinet path alias was accepted"},
  };
  for(size_t index=0;index<sizeof unsafe_aliases/sizeof unsafe_aliases[0];index++){
    Buffer unsafe_cab={0},unsafe_executable={0};
    if(!cabinet_append_empty_member(cabinet,cabinet_size,unsafe_aliases[index].member,
                                    &unsafe_cab) ||
       !build_lzx_executable(unsafe_cab.data,unsafe_cab.size,&unsafe_executable) ||
       snprintf(path,sizeof path,"%s/%s",root,unsafe_aliases[index].filename)>=(int)sizeof path ||
       !write_file(path,unsafe_executable.data,unsafe_executable.size) ||
       anygm_embedded_cab_probe(&router,path,&parsed)!=ANYGM_EMBEDDED_CAB_SUPPORTED ||
       anygm_content_resolve_path(&router,path,warm,sizeof warm,NULL,0,NULL,0)!=
         ANYGM_CONTENT_RESOLVE_INVALID ||
       root_has_directory_prefix(root,unsafe_aliases[index].cache_prefix)){
      free(unsafe_cab.data); free(unsafe_executable.data);
      free(executable.data); free(cabinet);
      return fail(unsafe_aliases[index].message);
    }
    free(unsafe_cab.data);
    free(unsafe_executable.data);
  }

  const struct { const char *filename; const char *member; const char *message; } collisions[]={
    {"exact-duplicate-lzx.exe","assets\\external asset.txt",
     "an exact duplicate Cabinet member was accepted"},
    {"casefold-duplicate-lzx.exe","ASSETS\\EXTERNAL ASSET.TXT",
     "an ASCII case-folded Cabinet collision was accepted"},
    {"reserved-marker-lzx.exe",".ANYGM_CAB_CACHE",
     "a Cabinet member collided with the cache marker"},
  };
  for(size_t index=0;index<sizeof collisions/sizeof collisions[0];index++){
    Buffer duplicate_cab={0},duplicate_executable={0};
    if(!cabinet_append_empty_member(cabinet,cabinet_size,collisions[index].member,
                                    &duplicate_cab) ||
       !build_lzx_executable(duplicate_cab.data,duplicate_cab.size,&duplicate_executable) ||
       snprintf(path,sizeof path,"%s/%s",root,collisions[index].filename)>=(int)sizeof path ||
       !write_file(path,duplicate_executable.data,duplicate_executable.size) ||
       anygm_embedded_cab_probe(&router,path,&parsed)!=ANYGM_EMBEDDED_CAB_SUPPORTED ||
       anygm_content_resolve_path(&router,path,warm,sizeof warm,NULL,0,NULL,0)!=
         ANYGM_CONTENT_RESOLVE_INVALID){
      free(duplicate_cab.data); free(duplicate_executable.data);
      free(executable.data); free(cabinet);
      return fail(collisions[index].message);
    }
    free(duplicate_cab.data);
    free(duplicate_executable.data);
  }

  Buffer native_cab={0},native_executable={0};
  static const char native_member[]="assets\\native-member.exe";
  if(!cabinet_append_empty_member(cabinet,cabinet_size,native_member,&native_cab) ||
     !build_lzx_executable(native_cab.data,native_cab.size,&native_executable) ||
     snprintf(path,sizeof path,"%s/native-member-lzx.exe",root)>=(int)sizeof path ||
     !write_file(path,native_executable.data,native_executable.size)){
    free(native_cab.data); free(native_executable.data);
    free(executable.data); free(cabinet);
    return fail("could not stage the native-member Cabinet case");
  }
  char native_payload[1024],native_root[1024],native_asset[1280],native_path[1280];
  if(anygm_content_resolve_path(&router,path,native_payload,sizeof native_payload,native_root,
                                sizeof native_root,NULL,0)!=ANYGM_CONTENT_RESOLVE_OK ||
     !read_prefix(native_payload,magic,sizeof magic) || memcmp(magic,"FORM",4) ||
     snprintf(native_asset,sizeof native_asset,"%s/assets/external asset.txt",native_root)>=
       (int)sizeof native_asset ||
     !read_file(native_asset,&asset,&asset_size) || asset_size!=23u ||
     memcmp(asset,"neutral external asset\n",23u) ||
     snprintf(native_path,sizeof native_path,"%s/assets/native-member.exe",native_root)>=
       (int)sizeof native_path || access(native_path,F_OK)==0 || errno!=ENOENT){
    free(asset); free(native_cab.data); free(native_executable.data);
    free(executable.data); free(cabinet);
    return fail("a native Cabinet member was published or displaced portable content");
  }
  free(asset);
  source.path=path;
  engine=NULL;
  if(anygm_create(services,&engine)!=ANYGM_OK || !engine ||
     anygm_load(engine,&source,NULL)!=ANYGM_OK){
    if(engine) anygm_destroy(engine);
    free(native_cab.data); free(native_executable.data);
    free(executable.data); free(cabinet);
    return fail("portable Cabinet content stopped loading when a native member was omitted");
  }
  anygm_destroy(engine);
  AnygmHostServices native_no_write=*services;
  native_no_write.file_write=reject_write;
  router.host=&native_no_write;
  if(anygm_content_resolve_path(&router,path,warm,sizeof warm,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_OK || strcmp(warm,native_payload) || access(native_path,F_OK)==0 ||
     errno!=ENOENT){
    router.host=services;
    free(native_cab.data); free(native_executable.data);
    free(executable.data); free(cabinet);
    return fail("the native-member omission was not retained by verified cache reuse");
  }
  router.host=services;
  free(native_cab.data);
  free(native_executable.data);

  const struct { const char *filename; unsigned byte; } failures[]={
    {"write-failure.exe",1u}, {"flush-failure.exe",2u}, {"rename-failure.exe",3u}
  };
  for(size_t index=0;index<sizeof failures/sizeof failures[0];index++){
    uint8_t *failed=malloc(executable.size);
    if(!failed){ free(executable.data); free(cabinet); return 0; }
    memcpy(failed,executable.data,executable.size);
    failed[519]= (uint8_t)failures[index].byte;
    if(snprintf(path,sizeof path,"%s/%s",root,failures[index].filename)>=(int)sizeof path ||
       !write_file(path,failed,executable.size)){
      free(failed); free(executable.data); free(cabinet); return 0;
    }
    free(failed);
    AnygmHostServices fault=*services;
    if(index==0){
      limited_write_original=services->file_write;
      limited_write_total=0; limited_write_limit=1024;
      fault.file_write=limited_write;
    }else if(index==1) fault.file_flush=reject_flush;
    else fault.path_rename=reject_rename;
    router.host=&fault;
    if(anygm_content_resolve_path(&router,path,warm,sizeof warm,NULL,0,NULL,0)!=
         ANYGM_CONTENT_RESOLVE_INVALID ||
       root_has_directory_prefix(root,index==0?"write-failure-":
                                  index==1?"flush-failure-":"rename-failure-")){
      router.host=services; free(executable.data); free(cabinet);
      return fail("a failed Cabinet transaction left a published or staging cache");
    }
    router.host=services;
  }

  uint8_t *dangerous=malloc(executable.size);
  if(!dangerous){ free(executable.data); free(cabinet); return 0; }
  memcpy(dangerous,executable.data,executable.size);
  uint8_t *name=memmem(dangerous+640,cabinet_size,"data.win",8);
  if(!name){ free(dangerous); free(executable.data); free(cabinet); return 0; }
  memcpy(name,"../x.win",8);
  if(snprintf(path,sizeof path,"%s/traversal-lzx.exe",root)>=(int)sizeof path ||
     !write_file(path,dangerous,executable.size) ||
     anygm_content_resolve_path(&router,path,warm,sizeof warm,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_INVALID){
    free(dangerous); free(executable.data); free(cabinet);
    return fail("a traversal path in LZX Cabinet content was accepted");
  }
  free(dangerous);
  free(executable.data);
  free(cabinet);
  return 1;
}

static int build_nsis2_deflate_executable(Buffer *output){
  enum { STUB=512, HEADER_SIZE=97, HEADER_PACKED=100, DATA_PACKED=7 };
  size_t size=STUB+28u+4u+HEADER_PACKED+4u+DATA_PACKED;
  uint8_t *bytes=calloc(size,1);
  if(!bytes) return 0;
  bytes[0]='M'; bytes[1]='Z';
  store_u32(bytes,60u,128u);
  memcpy(bytes+128u,"PE\0\0",4u);
  store_u32(bytes,STUB,4u);
  store_u32(bytes,STUB+4u,UINT32_C(0xdeadbeef));
  store_u32(bytes,STUB+8u,UINT32_C(0x6c6c754e));
  store_u32(bytes,STUB+12u,UINT32_C(0x74666f73));
  store_u32(bytes,STUB+16u,UINT32_C(0x74736e49));
  store_u32(bytes,STUB+20u,HEADER_SIZE);
  store_u32(bytes,STUB+24u,(uint32_t)(size-STUB));
  store_u32(bytes,STUB+28u,UINT32_C(0x80000000)|HEADER_PACKED);
  uint8_t *packed_header=bytes+STUB+32u;
  packed_header[0]=1u;
  store_u16(packed_header,1u,HEADER_SIZE);
  uint8_t *header=packed_header+3u;
  store_u32(header,20u,60u);
  store_u32(header,24u,1u);
  store_u32(header,28u,88u);
  store_u32(header,36u,HEADER_SIZE);
  store_u32(header+60u,0u,20u);
  store_u32(header+60u,8u,0u);
  store_u32(header+60u,12u,0u);
  memcpy(header+88u,"data.win",9u);
  uint8_t *record=packed_header+HEADER_PACKED;
  store_u32(record,0u,UINT32_C(0x80000000)|DATA_PACKED);
  record[4u]=1u;
  store_u16(record,5u,4u);
  memcpy(record+7u,"FORM",4u);
  output->data=bytes;
  output->size=output->capacity=size;
  return 1;
}

static int embedded_nsis_cases(const AnygmHostServices *services,const char *root){
  Buffer executable={0};
  if(!build_nsis2_deflate_executable(&executable)) return fail("could not build NSIS fixture");
  char path[512],resolved[1024],assets[1024];
  if(snprintf(path,sizeof path,"%s/neutral-nsis.exe",root)>=(int)sizeof path ||
     !write_file(path,executable.data,executable.size)){
    free(executable.data); return fail("could not stage NSIS fixture");
  }
  AnygmContentRouter router={0};
  router.host=services;
  router.cache_directory=root;
  router.log=fixture_log;
  AnygmEmbeddedNsis parsed={0};
  uint8_t magic[4];
  int ok=anygm_embedded_nsis_probe(&router,path,&parsed)==ANYGM_EMBEDDED_NSIS_SUPPORTED &&
         parsed.header_offset==512u && parsed.header_size==97u &&
         anygm_content_resolve_path(&router,path,resolved,sizeof resolved,assets,sizeof assets,
                                    NULL,0)==ANYGM_CONTENT_RESOLVE_OK &&
         assets[0] && read_prefix(resolved,magic,sizeof magic) && !memcmp(magic,"FORM",4u);
  if(!ok){ free(executable.data); return fail("NSIS 2 Deflate payload did not resolve"); }

  memcpy(executable.data+512u+32u+3u+88u,"../x.win",9u);
  if(snprintf(path,sizeof path,"%s/traversal-nsis.exe",root)>=(int)sizeof path ||
     !write_file(path,executable.data,executable.size) ||
     anygm_content_resolve_path(&router,path,resolved,sizeof resolved,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_INVALID){
    free(executable.data); return fail("NSIS traversal member was accepted");
  }
  memcpy(executable.data+512u+32u+3u+88u,"data.win",9u);
  store_u32(executable.data,512u+28u,100u);
  if(snprintf(path,sizeof path,"%s/solid-nsis.exe",root)>=(int)sizeof path ||
     !write_file(path,executable.data,executable.size) ||
     anygm_embedded_nsis_probe(&router,path,&parsed)!=ANYGM_EMBEDDED_NSIS_UNSUPPORTED){
    free(executable.data); return fail("unsupported NSIS profile was not classified");
  }
  free(executable.data);
  return 1;
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
  int ok=result==ANYGM_ERROR_UNSUPPORTED && strstr(diagnostic,"Cabinet profile") &&
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
  ok=result==ANYGM_ERROR_INVALID_CONTENT && !strstr(diagnostic,"Cabinet profile") &&
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

static int selected_file_identity(const AnygmContentRouter *router,const char *input,
                                  const char *original,const char *root){
  uint8_t *bytes=NULL,digest[32]; size_t size=0;
  if(!read_file(original,&bytes,&size)) return 0;
  gml_sha256(bytes,size,digest); free(bytes);
  char directory[700],ini[750],hex[65],text[512],overrides[512],resolved[1024];
  snprintf(directory,sizeof directory,"%s/identity-XXXXXX",root);
  if(!mkdtemp(directory)) return 0;
  snprintf(ini,sizeof ini,"%s/AnyGM.ini",directory);
  for(size_t i=0;i<32;i++) snprintf(hex+i*2,3,"%02x",digest[i]);
  snprintf(text,sizeof text,"[overrides]\n$identity=1\n"
    "[sha256:%s.overrides]\n$identity=2\n",hex);
  AnygmContentRouter scoped=*router; scoped.system_directory=directory;
  int ok=write_file(ini,text,strlen(text)) &&
    anygm_content_resolve_path(&scoped,input,resolved,sizeof resolved,NULL,0,
                               overrides,sizeof overrides)==ANYGM_CONTENT_RESOLVE_OK &&
    !strcmp(overrides,"$identity=1\n$identity=2\n");
  anygm_content_directory_remove(router->host,directory);
  return ok;
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
  router.log=fixture_log;
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
  if(!selected_file_identity(&router,path,path,root))
    return fail("embedded executable configuration did not hash the original executable");

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

  /* A structurally supported Cabinet does not supersede an older Classic runner.  Place a real
   * neutral LZX-21 Cabinet in a PE section appended after the complete Classic envelope; the
   * established Classic importer must still produce the selected FORM package. */
  Fixture classic={{0},0};
  char config_path[600];
  const char *declarations="[transforms]\n";
  if(snprintf(config_path,sizeof config_path,"%s/AnyGM.ini",root)>=(int)sizeof config_path ||
     !write_file(config_path,declarations,strlen(declarations)))
    return fail("could not write the empty configuration");
  router.system_directory=root;
  uint8_t *cabinet=NULL;
  size_t cabinet_size=0;
  if(!build_gm6_executable_fixture(&classic) ||
     !read_file("tests/fixtures/embedded_cab_lzx21.cab",&cabinet,&cabinet_size)){
    free(cabinet);
    return fail("could not build the Classic-plus-Cabinet precedence fixture");
  }
  uint8_t combined[sizeof classic.data]={0};
  const size_t pe_offset=64u;
  const size_t cab_offset=128u;
  size_t combined_size=cab_offset+cabinet_size+classic.size-64u;
  if(classic.size<64u || combined_size>sizeof combined){
    free(cabinet);
    return fail("Classic-plus-Cabinet precedence fixture exceeds its bound");
  }
  memcpy(combined,classic.data,64u);
  store_u32(combined,60u,(uint32_t)pe_offset);
  memcpy(combined+pe_offset,"PE\0\0",4u);
  store_u16(combined,pe_offset+6u,1u);
  store_u32(combined,pe_offset+24u+16u,(uint32_t)cabinet_size);
  store_u32(combined,pe_offset+24u+20u,(uint32_t)cab_offset);
  memcpy(combined+cab_offset,cabinet,cabinet_size);
  memcpy(combined+cab_offset+cabinet_size,classic.data+64u,classic.size-64u);
  free(cabinet);
  char classic_asset_root[1024]="";
  if(snprintf(path,sizeof path,"%s/classic-with-cabinet.exe",root)>=(int)sizeof path ||
     !write_file(path,combined,combined_size))
    return fail("could not stage the Classic-plus-Cabinet precedence fixture");
  AnygmEmbeddedCab classic_cab={0};
  if(anygm_embedded_cab_probe(&router,path,&classic_cab)!=ANYGM_EMBEDDED_CAB_SUPPORTED ||
     classic_cab.offset!=cab_offset || classic_cab.size!=cabinet_size)
    return fail("Classic-plus-Cabinet fixture did not retain its supported Cabinet");
  if(anygm_content_resolve_path(&router,path,resolved,sizeof resolved,classic_asset_root,
                                sizeof classic_asset_root,NULL,0)!=ANYGM_CONTENT_RESOLVE_OK ||
     !strstr(resolved,"-anygm-classic/data.win") || classic_asset_root[0])
    return fail("Classic executable did not outrank its embedded Cabinet");
  uint8_t classic_magic[4];
  if(!read_prefix(resolved,classic_magic,sizeof classic_magic) ||
     memcmp(classic_magic,"FORM",sizeof classic_magic))
    return fail("Classic precedence did not proceed through its normal package loader");

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

static int adjacent_executable_payload_cases(const AnygmHostServices *services,const char *root){
  char directory[512],launcher[640],payload[640],resolved[1024],asset_root[1024];
  if(snprintf(directory,sizeof directory,"%s/adjacent-launcher",root)>=(int)sizeof directory ||
     mkdir(directory,0700)!=0 ||
     snprintf(launcher,sizeof launcher,"%s/launcher-only.exe",directory)>=(int)sizeof launcher ||
     snprintf(payload,sizeof payload,"%s/data.win",directory)>=(int)sizeof payload)
    return fail("could not create the adjacent-payload fixture directory");

  AnygmSyntheticContent fixture={0};
  uint8_t *form=NULL;
  size_t form_size=0;
  if(!anygm_synthetic_content_create(&fixture) ||
     !anygm_synthetic_content_read(&fixture,&form,&form_size)){
    anygm_synthetic_content_destroy(&fixture);
    free(form);
    return fail("could not create the adjacent Studio payload fixture");
  }
  anygm_synthetic_content_destroy(&fixture);

  uint8_t executable[1024];
  build_pe_launcher_only(executable);
  if(!write_file(launcher,executable,sizeof executable) || !write_file(payload,form,form_size)){
    free(form);
    return fail("could not stage the adjacent Studio payload fixture");
  }
  AnygmContentRouter router={0};
  router.host=services;
  router.cache_directory=root;
  router.log=fixture_log;
  asset_root[0]='x';
  if(anygm_content_resolve_path(&router,launcher,resolved,sizeof resolved,asset_root,
                                sizeof asset_root,NULL,0)!=ANYGM_CONTENT_RESOLVE_OK ||
     strcmp(resolved,payload) || asset_root[0]){
    free(form);
    return fail("a launcher-only PE did not resolve its exact adjacent data.win");
  }
  if(!selected_file_identity(&router,launcher,payload,root)){
    free(form);
    return fail("launcher-only configuration did not select the adjacent data image hash");
  }

  AnygmEngine *engine=NULL;
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=launcher;
  source.cache_directory=root;
  source.save_directory=directory;
  int ok=anygm_create(services,&engine)==ANYGM_OK && engine &&
         anygm_load(engine,&source,NULL)==ANYGM_OK &&
         !strcmp(engine->content_launch_path,launcher) &&
         !strcmp(engine->current_content_path,payload) &&
         !strcmp(engine->content_program_directory,directory) &&
         anygm_reset(engine)==ANYGM_OK &&
         !strcmp(engine->content_launch_path,launcher) &&
         !strcmp(engine->current_content_path,payload) &&
         !strcmp(engine->content_program_directory,directory);
  anygm_destroy(engine);
  if(!ok){
    free(form);
    return fail("an adjacent payload did not preserve launch identity and Reset paths");
  }


  uint8_t no_code[200];
  build_no_code_form(no_code);
  build_pe_launcher_only(executable);
  memcpy(executable+700,no_code,sizeof no_code);
  char embedded[640];
  if(snprintf(embedded,sizeof embedded,"%s/embedded-first.exe",directory)>=(int)sizeof embedded ||
     !write_file(embedded,executable,sizeof executable) ||
     anygm_content_resolve_path(&router,embedded,resolved,sizeof resolved,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_OK || !strcmp(resolved,payload)){
    free(form);
    return fail("an adjacent payload outranked an embedded Studio payload");
  }

  /* The mirror image of that rule. An embedded payload that carries no code cannot be run at all,
   * so refusing the neighbour protects no load; it only turns one that would work into an error.
   * The neighbour is preferred only when it is demonstrably the same project, which is what the
   * fixture above cannot claim and this one can. */
  char neighbour_filename[128],neighbour_name[128];
  uint32_t neighbour_id=0;
  uint8_t named[512],named_executable[1024];
  size_t named_size=0;
  if(!read_form_identity(form,form_size,neighbour_filename,sizeof neighbour_filename,
                         neighbour_name,sizeof neighbour_name,&neighbour_id) ||
     !(named_size=build_no_code_form_named(named,sizeof named,neighbour_filename,neighbour_name,
                                           neighbour_id))){
    free(form);
    return fail("could not stage the same-project embedded payload fixture");
  }
  memset(named_executable,0,sizeof named_executable);
  build_pe_launcher_only(named_executable);
  memcpy(named_executable+700,named,named_size);
  char same_project[640];
  if(snprintf(same_project,sizeof same_project,"%s/embedded-same-project.exe",directory)>=
       (int)sizeof same_project ||
     !write_file(same_project,named_executable,sizeof named_executable) ||
     anygm_content_resolve_path(&router,same_project,resolved,sizeof resolved,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_OK || strcmp(resolved,payload)){
    free(form);
    return fail("a codeless embedded payload did not defer to the same project beside it");
  }


  char rejected[640];
  build_pe_cabinet(executable);
  if(snprintf(rejected,sizeof rejected,"%s/unsupported-cabinet.exe",directory)>=
       (int)sizeof rejected || !write_file(rejected,executable,sizeof executable)){
    free(form);
    return fail("could not stage the unsupported Cabinet precedence fixture");
  }
  strcpy(resolved,"dirty");
  strcpy(asset_root,"dirty");
  if(anygm_content_resolve_path(&router,rejected,resolved,sizeof resolved,asset_root,
                                sizeof asset_root,NULL,0)!=ANYGM_CONTENT_RESOLVE_UNSUPPORTED ||
     resolved[0] || asset_root[0]){
    free(form);
    return fail("an adjacent payload hid an unsupported Cabinet or published outputs");
  }

  build_pe_cabinet(executable);
  store_u32(executable,512+16,UINT32_MAX);
  if(snprintf(rejected,sizeof rejected,"%s/malformed-cabinet.exe",directory)>=
       (int)sizeof rejected || !write_file(rejected,executable,sizeof executable)){
    free(form);
    return fail("could not stage the malformed Cabinet precedence fixture");
  }
  strcpy(resolved,"dirty");
  if(anygm_content_resolve_path(&router,rejected,resolved,sizeof resolved,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_INVALID || resolved[0]){
    free(form);
    return fail("an adjacent payload hid a malformed Cabinet or published output");
  }

  build_pe_launcher_only(executable);
  executable[0]='N';
  if(snprintf(rejected,sizeof rejected,"%s/not-a-pe.exe",directory)>=(int)sizeof rejected ||
     !write_file(rejected,executable,sizeof executable)){
    free(form);
    return fail("could not stage the invalid executable fixture");
  }
  strcpy(resolved,"dirty");
  if(anygm_content_resolve_path(&router,rejected,resolved,sizeof resolved,NULL,0,NULL,0)!=
       ANYGM_CONTENT_RESOLVE_INVALID || resolved[0]){
    free(form);
    return fail("a non-PE executable gained adjacent-payload authority");
  }
  free(form);
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

/* An archive anchor binds patch files relative to its selected extracted source. */
static int archive_patch_cases(const AnygmHostServices *services,const char *root){
  uint8_t original[200],target[200];
  build_no_code_form(original); memcpy(target,original,sizeof target); store_u32(target,80,242);
  size_t patch_size=0;
  uint8_t *patch=anygm_test_vcdiff_literal(target,sizeof target,&patch_size);
  char source_hash[65],patch_hash[65],result_hash[65],anchor[1024];
  anygm_test_sha256_hex(original,sizeof original,source_hash);
  anygm_test_sha256_hex(patch,patch_size,patch_hash);
  anygm_test_sha256_hex(target,sizeof target,result_hash);
  snprintf(anchor,sizeof anchor,"[anygm]\npayload=alt/payload.win\n[patches]\n"
    "update=xdelta|update.xdelta|%s|%s|%s|200\n[pipelines]\ninput.final=update\n",
    source_hash,patch_hash,result_hash);
  ZipEntry entries[]={
    {(const uint8_t*)"alt/payload.win",15,original,sizeof original,sizeof original,0,0,0,0},
    {(const uint8_t*)"content.anygm",13,(const uint8_t*)anchor,(uint32_t)strlen(anchor),
      (uint32_t)strlen(anchor),0,0,0,0},
    {(const uint8_t*)"alt/update.xdelta",17,patch,(uint32_t)patch_size,(uint32_t)patch_size,0,0,0,0},
  };
  Buffer archive={0}; char path[512],resolved[1536],assets[1536],overrides[4096];
  AnygmContentRouter router={0}; router.host=services; router.cache_directory=root;
  router.log=fixture_log;
  int ok=build_zip(entries,3,&archive) &&
    snprintf(path,sizeof path,"%s/patch-chain.zip",root)<(int)sizeof path &&
    write_file(path,archive.data,archive.size);
  free(archive.data);
  for(unsigned warm=0;ok && warm<2;warm++){
    uint8_t *observed=NULL; size_t observed_size=0;
    ok=anygm_content_resolve_path(&router,path,resolved,sizeof resolved,assets,sizeof assets,
      overrides,sizeof overrides) && read_file(resolved,&observed,&observed_size) &&
      observed_size==sizeof target && !memcmp(observed,target,sizeof target) &&
      strstr(assets,"-anygm-archive/alt") && strstr(resolved,"/input-");
    free(observed);
  }
  if(ok){
    char patch_path[1600];
    snprintf(patch_path,sizeof patch_path,"%s/update.xdelta",assets);
    ok=write_file(patch_path,"broken",6) &&
      !anygm_content_resolve_path(&router,path,resolved,sizeof resolved,assets,sizeof assets,
        overrides,sizeof overrides) && !resolved[0];
  }
  free(patch);
  return ok?1:fail("archive patch resource, source-relative root or warm-cache validation failed");
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
    uint8_t oversized[ANYGM_CONTENT_MAX_ANCHOR_FILE_BYTES+1u];
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


static int input_pipeline_cases(const AnygmHostServices *services,const char *root){
  char directory[600],payload[700],ini[700],anchor[700],resolved[1536],assets[1536],overrides[4096];
  if(snprintf(directory,sizeof directory,"%s/input-pipelines",root)>=(int)sizeof directory ||
     mkdir(directory,0700) ||
     snprintf(payload,sizeof payload,"%s/wrapper.win",directory)>=(int)sizeof payload ||
     snprintf(ini,sizeof ini,"%s/AnyGM.ini",directory)>=(int)sizeof ini ||
     snprintf(anchor,sizeof anchor,"%s/content.anygm",directory)>=(int)sizeof anchor)
    return fail("could not create input-pipeline paths");
  const uint8_t form[]={'F','O','R','M',0,0,0,0};
  const char archive_anchor[]="[anygm]\npayload=data.win\n[overrides]\n$archive_marker=11\n"
    "[transforms]\nretained=buffer retained(){return slice(0,input_size);}\n";
  ZipEntry entries[]={
    {(const uint8_t*)"data.win",8,form,sizeof form,sizeof form,0,0,0,0},
    {(const uint8_t*)"member.anygm",12,(const uint8_t*)archive_anchor,
     sizeof archive_anchor-1,sizeof archive_anchor-1,0,0,0,0}
  };
  Buffer archive={0},wrapped={0};
  uint8_t prefix[64]={'W','R','A','P'},digest[32];
  if(!build_zip(entries,2,&archive) || !buffer_bytes(&wrapped,prefix,sizeof prefix) ||
     !buffer_bytes(&wrapped,archive.data,archive.size)) return fail("could not build wrapped ZIP");
  uint8_t *examples=NULL; size_t examples_size=0;
  if(!read_file("examples/input_transforms.ini",&examples,&examples_size)) return fail("cannot read distributed adapters");
  char hex[65],member_hex[65],selection[1024];
  gml_sha256(wrapped.data,wrapped.size,digest);
  for(size_t i=0;i<32;i++) snprintf(hex+i*2,3,"%02x",digest[i]);
  gml_sha256(form,sizeof form,digest);
  for(size_t i=0;i<32;i++) snprintf(member_hex+i*2,3,"%02x",digest[i]);
  snprintf(selection,sizeof selection,
    "\n[sha256:%s.pipelines]\ninput=embedded_zip\n"
    "[sha256:%s.overrides]\n$source_marker=3\n"
    "[sha256:%s.overrides]\n$wrong_identity=99\n",hex,hex,member_hex);
  Buffer config={0};
  int ok=buffer_bytes(&config,examples,examples_size) && buffer_bytes(&config,selection,strlen(selection)) &&
    write_file(payload,wrapped.data,wrapped.size) && write_file(ini,config.data,config.size);
  free(examples); free(config.data); free(archive.data);
  AnygmContentRouter router={0}; router.host=services; router.cache_directory=directory;
  router.system_directory=directory; router.log=fixture_log;
  if(!ok || !anygm_content_resolve_path(&router,payload,resolved,sizeof resolved,assets,sizeof assets,
       overrides,sizeof overrides) || !strstr(overrides,"$source_marker=3") ||
     !strstr(overrides,"$archive_marker=11") || strstr(overrides,"$wrong_identity"))
    return fail("wrapped ZIP lost original identity or archive directives");
  /* Empty asset_root means the resolved image's own directory, not the original
   * source directory. That is the archive reader's ordinary contract. */
  if(!assets[0]) anygm_content_path_parent(resolved,assets,sizeof assets);
  char extraction_prefix[720];
  snprintf(extraction_prefix,sizeof extraction_prefix,"%s/payload-",directory);
  if(strncmp(assets,extraction_prefix,strlen(extraction_prefix)) ||
     !strstr(assets,"-anygm-archive")) return fail("wrapped ZIP did not retain extracted assets");
  uint8_t observed[8];
  if(!read_prefix(resolved,observed,sizeof observed) || memcmp(observed,form,sizeof form))
    return fail("wrapped ZIP did not reach the ordinary data reader");
  uint8_t *unchanged=NULL; size_t unchanged_size=0;
  if(!read_file(payload,&unchanged,&unchanged_size) || unchanged_size!=wrapped.size ||
     memcmp(unchanged,wrapped.data,wrapped.size)) return fail("input adapter modified original content");
  free(unchanged);
  if(!anygm_content_resolve_path(&router,payload,resolved,sizeof resolved,assets,sizeof assets,
       overrides,sizeof overrides) || !strstr(overrides,"$source_marker=3") ||
     !strstr(overrides,"$archive_marker=11")) return fail("warm adapted ZIP lost configuration");
  /* Normalizing an archive member cannot reset the global nesting budget. The
   * wrapped ZIP consumes one level after the ordinary outer archives. */
  Buffer nested={0};
  for(unsigned level=1;level<=ANYGM_CONTENT_MAX_NESTING_LEVELS;level++){
    const uint8_t *name=(const uint8_t*)(level==1?"wrapper.win":"nested.zip");
    const uint8_t *bytes=level==1?wrapped.data:nested.data;
    size_t length=level==1?wrapped.size:nested.size;
    ZipEntry member={name,strlen((const char*)name),bytes,(uint32_t)length,(uint32_t)length,0,0,0,0};
    Buffer next={0};
    if(!build_zip(&member,1,&next)) return fail("cannot build adapter nesting boundary");
    free(nested.data); nested=next;
    char nested_path[720];
    snprintf(nested_path,sizeof nested_path,"%s/nested-%u.zip",directory,level);
    if(!write_file(nested_path,nested.data,nested.size)) return fail("cannot write adapter nesting boundary");
    int accepted=anygm_content_resolve_path(&router,nested_path,resolved,sizeof resolved,
      assets,sizeof assets,overrides,sizeof overrides)==ANYGM_CONTENT_RESOLVE_OK;
    if(accepted!=(level<ANYGM_CONTENT_MAX_NESTING_LEVELS))
      return fail("input adaptation changed the archive nesting limit");
    if(!accepted && (resolved[0] || overrides[0])) return fail("nesting rejection published partial results");
  }
  free(nested.data);
  const char explicit_adapter[]="[anygm]\npayload=wrapper.win\n[pipelines]\ninput=embedded_zip\n";
  if(!write_file(anchor,explicit_adapter,sizeof explicit_adapter-1u)) return fail("cannot write adapter anchor");
  /* The adapter only locates the envelope. Member checksum validation remains
   * mandatory in the existing archive reader after successful transformation. */
  wrapped.data[64+30+8]^=1;
  if(!write_file(payload,wrapped.data,wrapped.size) ||
     anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,assets,sizeof assets,
       overrides,sizeof overrides) || resolved[0] || overrides[0])
    return fail("a transformed ZIP bypassed member integrity checks");
  free(wrapped.data);
  Fixture project={{0},0};
  Buffer project_wrapper={0};
  const char project_adapter[]="[anygm]\npayload=project.bin\n[pipelines]\ninput=header_payload\n";
  snprintf(payload,sizeof payload,"%s/project.bin",directory);
  ok=build_project_fixture(600,&project) && buffer_u32(&project_wrapper,4) &&
    buffer_bytes(&project_wrapper,project.data,project.size) &&
    write_file(payload,project_wrapper.data,project_wrapper.size) &&
    write_file(anchor,project_adapter,sizeof project_adapter-1u);
  free(project_wrapper.data);
  if(!ok || anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,assets,sizeof assets,
       overrides,sizeof overrides)!=ANYGM_CONTENT_RESOLVE_OK || strcmp(assets,directory) ||
     !read_prefix(resolved,observed,4) || memcmp(observed,"FORM",4))
    return fail("normalized source project lost its structural importer or original assets");
  /* An authored indexed envelope contains a malformed FORM before a valid one.
   * Its directory only supplies ranges; complete reader validation selects one. */
  uint8_t indexed[56]={0};
  memcpy(indexed,"IDX1",4); indexed[4]=2;
  indexed[8]=40; indexed[16]=8; indexed[24]=48; indexed[32]=8;
  memcpy(indexed+40,form,8); indexed[44]=1; memcpy(indexed+48,form,8);
  const char indexed_adapter[]="[anygm]\npayload=project.bin\n[pipelines]\n"
    "input.probe=indexed_candidates\ninput=copy_payload\n";
  if(!write_file(payload,indexed,sizeof indexed) ||
     !write_file(anchor,indexed_adapter,sizeof indexed_adapter-1u) ||
     anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,assets,sizeof assets,
       overrides,sizeof overrides)!=ANYGM_CONTENT_RESOLVE_OK || strcmp(assets,directory) ||
     !read_prefix(resolved,observed,8) || memcmp(observed,form,8))
    return fail("indexed envelope did not select its unique complete data image");
  indexed[44]=0;
  if(!write_file(payload,indexed,sizeof indexed) ||
     anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,assets,sizeof assets,
       overrides,sizeof overrides) || resolved[0] || assets[0] || overrides[0])
    return fail("ambiguous indexed envelope published a partial result");
  indexed[44]=1; indexed[24]=250;
  if(!write_file(payload,indexed,sizeof indexed) ||
     anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,assets,sizeof assets,
       overrides,sizeof overrides) || resolved[0] || assets[0] || overrides[0])
    return fail("out-of-bounds indexed directory was accepted");
  return 1;
}
static int build_wrapped_project(Fixture *project){
  if(!build_project_fixture(701,project) || project->size>sizeof project->data-4u) return 0;
  memmove(project->data+4u,project->data,project->size);
  memcpy(project->data,"WRAP",4u); project->size+=4u;
  return 1;
}

static int transform_configuration_cases(const AnygmHostServices *services,const char *root){
  char directory[600],system[700],ini[800],payload[800],anchor[800],second[800],resolved[1024];
  if(snprintf(directory,sizeof directory,"%s/transform-cases",root)>=(int)sizeof directory ||
     mkdir(directory,0700) ||
     snprintf(system,sizeof system,"%s/system",directory)>=(int)sizeof system || mkdir(system,0700) ||
     snprintf(ini,sizeof ini,"%s/AnyGM.ini",system)>=(int)sizeof ini ||
     snprintf(payload,sizeof payload,"%s/project.gmk",directory)>=(int)sizeof payload ||
     snprintf(anchor,sizeof anchor,"%s/content.anygm",directory)>=(int)sizeof anchor ||
     snprintf(second,sizeof second,"%s/second.anygm",directory)>=(int)sizeof second)
    return fail("could not prepare the transform configuration paths");
  Fixture project={{0},0};
  if(!build_wrapped_project(&project) || !write_file(payload,project.data,project.size))
    return fail("could not write the wrapped project fixture");
  AnygmContentRouter router={0};
  router.host=services;
  router.cache_directory=directory;
  router.system_directory=system;
  router.log=fixture_log;
  const char adapter[]="[transforms]\ninput=buffer unwrap() {\n return slice(4,input_size-4);\n}\n";
  const char reject_program[]="[transforms]\ninput=buffer stop() { reject(); }\n";
  const char selected[]="[anygm]\npayload=project.gmk\n[transforms]\ninput=buffer unwrap() {\n"
    "/*\n[overrides]\n$sentinel=999\n*/\n return slice(4,input_size-4);\n}\n";
  if(anygm_content_resolve_path(&router,payload,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("a wrapped project loaded without its source adapter");
  if(!write_file(ini,adapter,sizeof adapter-1u) ||
     !anygm_content_resolve_path(&router,payload,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("the system configuration did not supply the test program");
  if(!write_file(ini,reject_program,sizeof reject_program-1u) ||
     anygm_content_resolve_path(&router,payload,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("a warm cache bypassed the changed transform program");
  if(!write_file(anchor,selected,sizeof selected-1u) ||
     !anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("the explicit anchor did not override the system program");
  char overrides[1024]={0};
  if(!anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,NULL,0,overrides,sizeof overrides) || overrides[0])
    return fail("transform source comments became anchor overrides");
  /* Runtime overrides remain disabled; transform requirements are independent. */
  if(!anygm_content_resolve_path(&router,payload,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("the lone sibling anchor did not supply its transform");
  if(!write_file(second,selected,sizeof selected-1u) ||
     anygm_content_resolve_path(&router,payload,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("ambiguous sibling anchors selected a transform");
  if(!anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("an explicit anchor was superseded by ambiguous sibling anchors");
  const char pipeline[]="[anygm]\npayload=project.gmk\n[transforms]\n"
    "unwrap=buffer unwrap(){return slice(4,input_size-4);}\n"
    "copy=buffer copy(){ /* [overrides] is only a comment */ return slice(0,input_size); }\n"
    "[pipelines]\ninput=unwrap|copy\n";
  if(!write_file(anchor,pipeline,sizeof pipeline-1u) ||
     !anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,NULL,0,overrides,sizeof overrides) ||
     overrides[0]) return fail("an anchor pipeline failed or injected an override");
  const char broken_pipeline[]="[anygm]\npayload=project.gmk\n[pipelines]\n"
    "input=builtin.zlib:0\n";
  if(!write_file(anchor,broken_pipeline,sizeof broken_pipeline-1u) ||
     anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,NULL,0,overrides,sizeof overrides) ||
     resolved[0] || overrides[0]) return fail("a malformed anchor pipeline published a result");
  if(!write_file(anchor,pipeline,sizeof pipeline-1u)) return fail("could not restore the anchor pipeline");
  const char invalid[]="[transforms]\ninput=not-a-program:\n";
  if(!write_file(ini,invalid,sizeof invalid-1u)) return fail("could not write invalid configuration");
  strcpy(resolved,"stale");
  if(anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,NULL,0,NULL,0) || resolved[0])
    return fail("malformed global configuration did not reject transactionally");
  if(unlink(ini) ||
     !anygm_content_resolve_path(&router,anchor,resolved,sizeof resolved,NULL,0,NULL,0))
    return fail("an explicit anchor required a global configuration file");
  return 1;
}

static int selected_configuration_cases(const AnygmHostServices *services,const char *root){
  char directory[600],ini[700],payload[700],anchor[700],archive_path[700],resolved[1024];
  snprintf(directory,sizeof directory,"%s/selected-config",root);
  if(mkdir(directory,0700)) return fail("could not create selected configuration directory");
  snprintf(ini,sizeof ini,"%s/AnyGM.ini",directory);
  snprintf(payload,sizeof payload,"%s/project.gmk",directory);
  snprintf(anchor,sizeof anchor,"%s/content.anygm",directory);
  snprintf(archive_path,sizeof archive_path,"%s/content.zip",directory);
  Fixture project={{0},0};
  if(!build_wrapped_project(&project) || !write_file(payload,project.data,project.size))
    return fail("could not write selected configuration project");
  uint8_t digest[32]; char hex[65],config[2048],overrides[4096];
  gml_sha256(project.data,project.size,digest);
  for(size_t i=0;i<32;i++) snprintf(hex+i*2,3,"%02x",digest[i]);
  snprintf(config,sizeof config,
    "[overrides]\n$default=1\n[transforms]\ninput=buffer stop(){"
    "if(input_size>=4 && read32(input,0)==0x50415257) reject(); return slice(0,input_size); }\n"
    "[sha256:%s.transforms]\ninput=buffer unwrap(){ return slice(4,input_size-4); }\n"
    "[sha256:%s.overrides]\n$selected=3\n",hex,hex);
  static const uint8_t anchor_text[]="[anygm]\npayload=project.gmk\n[overrides]\n$anchor=2\n"
    "[transforms]\ninput=buffer stop(){"
    "if(input_size>=4 && read32(input,0)==0x50415257) reject(); return slice(0,input_size); }\n";
  if(!write_file(ini,config,strlen(config)) || !write_file(anchor,anchor_text,sizeof anchor_text-1))
    return fail("could not write selected configuration declarations");
  AnygmContentRouter router={0}; router.host=services; router.cache_directory=directory;
  router.system_directory=directory; router.sibling_anchor_overrides=1;
  for(int direct=0;direct<2;direct++){
    if(!anygm_content_resolve_path(&router,direct?payload:anchor,resolved,sizeof resolved,
         NULL,0,overrides,sizeof overrides) || strcmp(overrides,"$default=1\n$anchor=2\n$selected=3\n"))
      return fail("SHA-256 selection did not override an anchor before source preparation");
  }
  ZipEntry entries[2]={
    {(const uint8_t*)"project.gmk",11,project.data,(uint32_t)project.size,(uint32_t)project.size,0,0,0,0},
    {(const uint8_t*)"inner.anygm",11,anchor_text,sizeof anchor_text-1,sizeof anchor_text-1,0,0,0,0}
  };
  Buffer archive={0};
  int ok=build_zip(entries,2,&archive) && write_file(archive_path,archive.data,archive.size);
  free(archive.data);
  if(!ok) return fail("could not write selected configuration archive");
  /* The external sibling is a lower layer than the archive's own anchor. */
  const char outer[]="[anygm]\npayload=content.zip\n[overrides]\n$outer=4\n";
  if(!write_file(anchor,outer,sizeof outer-1)) return fail("could not write outer configuration anchor");
  for(int warm=0;warm<2;warm++){
    if(!anygm_content_resolve_path(&router,archive_path,resolved,sizeof resolved,
         NULL,0,overrides,sizeof overrides) ||
       strcmp(overrides,"$default=1\n$outer=4\n$anchor=2\n$selected=3\n"))
      return fail("archive wrapping or cache warmth changed the selected payload configuration");
  }
  /* A different original fingerprint cannot reuse the prior derived result. */
  config[0]=0;
  hex[0]=hex[0]=='0'?'1':'0';
  snprintf(config,sizeof config,"[transforms]\ninput=buffer stop(){"
    "if(input_size>=4 && read32(input,0)==0x50415257) reject(); return slice(0,input_size);}\n"
    "[sha256:%s.transforms]\ninput=buffer unwrap(){return slice(4,input_size-4);}\n",hex);
  if(!write_file(ini,config,strlen(config)) ||
     anygm_content_resolve_path(&router,archive_path,resolved,sizeof resolved,NULL,0,overrides,sizeof overrides) ||
     overrides[0] || resolved[0]) return fail("unmatched hash selected a cached transform result");
  return 1;
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

  int ok=cache_producer_change_case(&services,root) &&
         embedded_cabinet_cases(&services,root) &&
         embedded_lzx_cabinet_cases(&services,root) &&
         embedded_nsis_cases(&services,root) &&
         embedded_executable_cases(&services,root) &&
         adjacent_executable_payload_cases(&services,root) &&
         archive_anchor_cases(&services,root) &&
         archive_advanced_anchor_cases(&services,root) &&
         archive_patch_cases(&services,root) &&
         direct_anchor_cases(&services,root) &&
         transform_configuration_cases(&services,root) &&
         input_pipeline_cases(&services,root) &&
         selected_configuration_cases(&services,root);
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
