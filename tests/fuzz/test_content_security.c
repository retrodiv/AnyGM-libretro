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
  int result=anygm_content_resolve_path(&router,path,resolved,sizeof resolved);
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

  int ok=1;
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
