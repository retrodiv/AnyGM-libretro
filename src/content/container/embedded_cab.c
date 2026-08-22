/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 *
 * Bounded PE-section Cabinet discovery and transactional VFS extraction.
 * Raw libarchive types remain confined to this owner.
 */
#include "embedded_cab.h"

#include "content_router.h"
#include "anygm_vfs.h"
#include "archive.h"
#include "archive_entry.h"

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

#define CAB_SCAN_BYTES (64u*1024u)
#define CAB_PROFILE_LZX21 UINT32_C(0x001503)
#define CAB_CACHE_SCHEMA 1u
#define CAB_CACHE_MARKER ".anygm_cab_cache"
#define CAB_MARKER_MAX_BYTES (20u*1024u*1024u)

typedef struct CabFolder {
  uint32_t data_offset;
  uint16_t block_count;
  uint16_t compression;
  uint64_t compressed_size;
  uint64_t expanded_size;
} CabFolder;

typedef struct CabMember {
  char *path;
  uint64_t size;
  uint64_t hash;
} CabMember;

typedef struct CabManifest {
  CabMember *members;
  size_t count;
  size_t capacity;
  char payload[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
} CabManifest;

typedef struct CabRangeClient {
  const AnygmHostServices *host;
  void *file;
  uint64_t begin;
  uint64_t size;
  uint64_t position;
  uint8_t buffer[CAB_SCAN_BYTES];
  int failed;
} CabRangeClient;

static uint16_t cab_u16(const uint8_t *p){
  return (uint16_t)((uint16_t)p[0]|(uint16_t)((uint16_t)p[1]<<8));
}

static uint32_t cab_u32(const uint8_t *p){
  return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}

static uint64_t cab_hash_begin(void){ return UINT64_C(1469598103934665603); }

static uint64_t cab_hash_update(uint64_t hash,const void *data,size_t size){
  const uint8_t *bytes=(const uint8_t *)data;
  for(size_t index=0;index<size;index++){
    hash^=(uint64_t)bytes[index];
    hash*=UINT64_C(1099511628211);
  }
  return hash;
}

static void cab_log(const AnygmContentRouter *router,int level,const char *format,...){
  if(!router || !router->log) return;
  char message[1024];
  va_list arguments;
  va_start(arguments,format);
  vsnprintf(message,sizeof message,format,arguments);
  va_end(arguments);
  router->log(router->log_userdata,level,message);
}

static int cab_file_size(const AnygmContentRouter *router,const char *path,uint64_t *size){
  AnygmFileInfo info;
  if(!router || !path || !anygm_vfs_stat(router->host,path,&info) ||
     !(info.flags&ANYGM_FILE_INFO_REGULAR)) return 0;
  if(size) *size=info.size;
  return 1;
}

static int cab_seek_absolute(const AnygmHostServices *host,void *file,uint64_t offset){
  return host && host->file_seek && offset<=INT64_MAX &&
         host->file_seek(host->userdata,file,(int64_t)offset,ANYGM_SEEK_START)==(int64_t)offset;
}

static int cab_read_at(const AnygmHostServices *host,void *file,uint64_t source_size,
                       uint64_t offset,void *data,size_t size){
  if(!host || !file || (!data && size) || offset>source_size ||
     (uint64_t)size>source_size-offset || !cab_seek_absolute(host,file,offset)) return 0;
  size_t read=0;
  while(read<size){
    size_t step=host->file_read(host->userdata,file,(uint8_t *)data+read,size-read);
    if(!step || step>size-read) return 0;
    read+=step;
  }
  return 1;
}

static int cab_hash_file(const AnygmContentRouter *router,const char *path,uint64_t expected_size,
                         uint64_t *hash_out){
  if(!router || !path || !hash_out || !anygm_vfs_can_read(router->host)) return 0;
  void *file=router->host->file_open(router->host->userdata,path,ANYGM_FILE_READ);
  if(!file) return 0;
  uint8_t *buffer=malloc(CAB_SCAN_BYTES);
  uint8_t fallback[16384];
  size_t capacity=CAB_SCAN_BYTES;
  if(!buffer){ buffer=fallback; capacity=sizeof fallback; }
  uint64_t hash=cab_hash_begin(),total=0;
  int ok=1;
  for(;;){
    size_t count=router->host->file_read(router->host->userdata,file,buffer,capacity);
    if(!count) break;
    if(count>capacity || total>UINT64_MAX-(uint64_t)count){ ok=0; break; }
    total+=(uint64_t)count;
    if(total>expected_size){ ok=0; break; }
    hash=cab_hash_update(hash,buffer,count);
  }
  if(buffer!=fallback) free(buffer);
  router->host->file_close(router->host->userdata,file);
  if(!ok || total!=expected_size) return 0;
  *hash_out=hash;
  return 1;
}

static int cab_source_matches(const AnygmContentRouter *router,const char *path,
                              const AnygmEmbeddedCab *cab){
  uint64_t size=0,hash=0;
  return cab && cab_file_size(router,path,&size) && size==cab->source_size &&
         cab_hash_file(router,path,size,&hash) && hash==cab->source_hash;
}

static int cab_read_cstring(const AnygmHostServices *host,void *file,uint64_t source_size,
                            uint64_t start,uint64_t end,uint64_t *next){
  uint8_t buffer[256];
  uint64_t cursor=start;
  while(cursor<end){
    size_t count=(uint64_t)sizeof buffer<end-cursor?sizeof buffer:(size_t)(end-cursor);
    if(!cab_read_at(host,file,source_size,cursor,buffer,count)) return 0;
    const uint8_t *zero=memchr(buffer,0,count);
    if(zero){
      *next=cursor+(uint64_t)(zero-buffer)+1u;
      return 1;
    }
    cursor+=(uint64_t)count;
  }
  return 0;
}

/* Validate structural boundaries and classify the profile without asking the decompressor to
 * allocate. Checksum and bitstream validity remain extraction-time validation. */
static AnygmEmbeddedCabStatus cab_validate_candidate(const AnygmHostServices *host,void *file,
                                                       uint64_t source_size,uint64_t offset,
                                                       uint64_t section_end,
                                                       AnygmEmbeddedCab *result){
  uint8_t header[40];
  if(section_end<offset || section_end-offset<36u ||
     !cab_read_at(host,file,source_size,offset,header,36u)) return ANYGM_EMBEDDED_CAB_INVALID;
  if(memcmp(header,"MSCF\0\0\0\0",8)) return ANYGM_EMBEDDED_CAB_NOT_FOUND;
  uint64_t cabinet_size=cab_u32(header+8u);
  uint64_t files_offset=cab_u32(header+16u);
  uint16_t folder_count=cab_u16(header+26u),file_count=cab_u16(header+28u);
  uint16_t flags=cab_u16(header+30u);
  if(cab_u32(header+12u) || cab_u32(header+20u) || header[24]!=3 || header[25]!=1 ||
     !folder_count || !file_count || folder_count>ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES ||
     file_count>ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES || (flags&~7u) || cabinet_size<36u ||
     cabinet_size>section_end-offset || files_offset>=cabinet_size)
    return ANYGM_EMBEDDED_CAB_INVALID;

  uint64_t cursor=36u;
  uint8_t folder_reserve=0,data_reserve=0;
  if(flags&4u){
    if(cabinet_size-cursor<4u ||
       !cab_read_at(host,file,source_size,offset+cursor,header,4u))
      return ANYGM_EMBEDDED_CAB_INVALID;
    uint16_t header_reserve=cab_u16(header);
    folder_reserve=header[2]; data_reserve=header[3];
    cursor+=4u;
    if((uint64_t)header_reserve>cabinet_size-cursor) return ANYGM_EMBEDDED_CAB_INVALID;
    cursor+=(uint64_t)header_reserve;
  }
  unsigned names=(flags&1u?2u:0u)+(flags&2u?2u:0u);
  for(unsigned index=0;index<names;index++)
    if(!cab_read_cstring(host,file,source_size,offset+cursor,offset+cabinet_size,&cursor))
      return ANYGM_EMBEDDED_CAB_INVALID;
    else cursor-=offset;

  uint64_t folder_record=8u+(uint64_t)folder_reserve;
  if(folder_record<8u || (uint64_t)folder_count>(cabinet_size-cursor)/folder_record ||
     cursor+(uint64_t)folder_count*folder_record>files_offset)
    return ANYGM_EMBEDDED_CAB_INVALID;
  CabFolder *folders=calloc(folder_count,sizeof *folders);
  if(!folders) return ANYGM_EMBEDDED_CAB_INVALID;
  AnygmEmbeddedCabStatus status=ANYGM_EMBEDDED_CAB_SUPPORTED;
  int unsupported=(flags&3u)!=0;
  for(uint16_t index=0;index<folder_count;index++){
    if(!cab_read_at(host,file,source_size,offset+cursor+(uint64_t)index*folder_record,
                    header,8u)){ status=ANYGM_EMBEDDED_CAB_INVALID; goto done; }
    folders[index].data_offset=cab_u32(header);
    folders[index].block_count=cab_u16(header+4u);
    folders[index].compression=cab_u16(header+6u);
    unsigned method=folders[index].compression&0xfu;
    unsigned window=(folders[index].compression>>8)&0x1fu;
    if(folders[index].data_offset>=cabinet_size){
      status=ANYGM_EMBEDDED_CAB_INVALID; goto done;
    }
    if(method!=3u || window!=21u || (folders[index].compression&UINT16_C(0xe0f0)))
      unsupported=1;
  }

  cursor=files_offset;
  uint16_t prior_folder=0;
  uint64_t *file_end=calloc(folder_count,sizeof *file_end);
  if(!file_end){ status=ANYGM_EMBEDDED_CAB_INVALID; goto done; }
  int have_prior=0;
  for(uint16_t index=0;index<file_count;index++){
    if(cabinet_size-cursor<16u ||
       !cab_read_at(host,file,source_size,offset+cursor,header,16u)){
      status=ANYGM_EMBEDDED_CAB_INVALID; goto files_done;
    }
    uint32_t size=cab_u32(header),file_offset=cab_u32(header+4u);
    uint16_t folder=cab_u16(header+8u),mapped=folder;
    if(size>ANYGM_CONTENT_MAX_MEMBER_BYTES || (uint64_t)file_offset+(uint64_t)size>UINT32_MAX){
      status=ANYGM_EMBEDDED_CAB_INVALID; goto files_done;
    }
    if(folder==UINT16_C(0xfffd)){
      if(index!=0u){ status=ANYGM_EMBEDDED_CAB_INVALID; goto files_done; }
      unsupported=1; mapped=0;
    }else if(folder==UINT16_C(0xfffe)){
      if(index+1u!=file_count){ status=ANYGM_EMBEDDED_CAB_INVALID; goto files_done; }
      unsupported=1; mapped=(uint16_t)(folder_count-1u);
    }else if(folder==UINT16_C(0xffff)){
      if(file_count!=1u){ status=ANYGM_EMBEDDED_CAB_INVALID; goto files_done; }
      unsupported=1; mapped=0;
    }
    if(mapped>=folder_count || (have_prior && mapped<prior_folder) ||
       (have_prior && mapped==prior_folder && (uint64_t)file_offset!=file_end[mapped])){
      status=ANYGM_EMBEDDED_CAB_INVALID; goto files_done;
    }
    file_end[mapped]=(uint64_t)file_offset+(uint64_t)size;
    prior_folder=mapped; have_prior=1;
    cursor+=16u;
    uint64_t absolute_next=0;
    if(!cab_read_cstring(host,file,source_size,offset+cursor,offset+cabinet_size,&absolute_next) ||
       absolute_next==offset+cursor){ status=ANYGM_EMBEDDED_CAB_INVALID; goto files_done; }
    cursor=absolute_next-offset;
  }

  {
    uint64_t previous_end=cursor,total_expanded=0;
    for(uint16_t index=0;index<folder_count;index++){
      uint64_t block=folders[index].data_offset;
      if(block<previous_end){ status=ANYGM_EMBEDDED_CAB_INVALID; goto files_done; }
      for(uint16_t number=0;number<folders[index].block_count;number++){
        uint64_t block_header=8u+(uint64_t)data_reserve;
        if(block_header>cabinet_size-block ||
           !cab_read_at(host,file,source_size,offset+block,header,8u)){
          status=ANYGM_EMBEDDED_CAB_INVALID; goto files_done;
        }
        uint16_t compressed=cab_u16(header+4u),expanded=cab_u16(header+6u);
        block+=block_header;
        if(!compressed || !expanded || compressed>cabinet_size-block ||
           folders[index].compressed_size>UINT64_MAX-compressed ||
           folders[index].expanded_size>UINT64_MAX-expanded){
          status=ANYGM_EMBEDDED_CAB_INVALID; goto files_done;
        }
        folders[index].compressed_size+=compressed;
        folders[index].expanded_size+=expanded;
        block+=(uint64_t)compressed;
      }
      previous_end=block;
      if(file_end[index]>folders[index].expanded_size ||
         total_expanded>ANYGM_CONTENT_MAX_EXTRACTED_BYTES-folders[index].expanded_size){
        status=ANYGM_EMBEDDED_CAB_INVALID; goto files_done;
      }
      total_expanded+=folders[index].expanded_size;
      if(folders[index].expanded_size>ANYGM_CONTENT_EXPANSION_ALLOWANCE &&
         (!folders[index].compressed_size ||
          folders[index].compressed_size>UINT64_MAX/ANYGM_CONTENT_MAX_EXPANSION_RATIO ||
          folders[index].expanded_size>
            folders[index].compressed_size*ANYGM_CONTENT_MAX_EXPANSION_RATIO)){
        status=ANYGM_EMBEDDED_CAB_INVALID; goto files_done;
      }
    }
  }
  status=unsupported?ANYGM_EMBEDDED_CAB_UNSUPPORTED:ANYGM_EMBEDDED_CAB_SUPPORTED;
  if(result){
    result->offset=offset;
    result->size=cabinet_size;
    result->profile=CAB_PROFILE_LZX21;
  }
files_done:
  free(file_end);
done:
  free(folders);
  return status;
}

AnygmEmbeddedCabStatus anygm_embedded_cab_probe(const AnygmContentRouter *router,
                                                 const char *path,AnygmEmbeddedCab *cab){
  if(cab) memset(cab,0,sizeof *cab);
  uint64_t source_size=0;
  if(!router || !path || !router->host || !router->host->file_open ||
     !router->host->file_read || !router->host->file_seek || !router->host->file_close ||
     !cab_file_size(router,path,&source_size) || source_size>ANYGM_CONTENT_MAX_EXECUTABLE_BYTES)
    return ANYGM_EMBEDDED_CAB_NOT_FOUND;
  void *file=router->host->file_open(router->host->userdata,path,ANYGM_FILE_READ);
  if(!file) return ANYGM_EMBEDDED_CAB_NOT_FOUND;
  uint8_t header[64];
  AnygmEmbeddedCabStatus answer=ANYGM_EMBEDDED_CAB_NOT_FOUND;
  if(source_size<64u || !cab_read_at(router->host,file,source_size,0,header,sizeof header) ||
     header[0]!='M' || header[1]!='Z') goto finish;
  uint64_t pe_offset=cab_u32(header+60u);
  if(pe_offset>source_size || source_size-pe_offset<24u ||
     !cab_read_at(router->host,file,source_size,pe_offset,header,24u) ||
     memcmp(header,"PE\0\0",4)) goto finish;
  uint16_t section_count=cab_u16(header+6u),optional_size=cab_u16(header+20u);
  if(!section_count || section_count>96u || pe_offset>UINT64_MAX-24u-optional_size) goto finish;
  uint64_t section_table=pe_offset+24u+(uint64_t)optional_size;
  if(section_table>source_size || (uint64_t)section_count>(source_size-section_table)/40u)
    goto finish;
  struct CabPeRange { uint64_t begin,end; } ranges[96];
  size_t range_count=0;
  for(uint16_t index=0;index<section_count;index++){
    if(!cab_read_at(router->host,file,source_size,section_table+(uint64_t)index*40u,header,40u))
      goto finish;
    uint64_t size=cab_u32(header+16u),begin=cab_u32(header+20u);
    if(!size || begin>source_size || size>source_size-begin) continue;
    ranges[range_count].begin=begin; ranges[range_count++].end=begin+size;
  }
  for(size_t index=1;index<range_count;index++){
    struct CabPeRange value=ranges[index]; size_t at=index;
    while(at && ranges[at-1u].begin>value.begin){ ranges[at]=ranges[at-1u]; at--; }
    ranges[at]=value;
  }
  size_t merged=0;
  for(size_t index=0;index<range_count;index++){
    if(merged && ranges[index].begin<=ranges[merged-1u].end){
      if(ranges[index].end>ranges[merged-1u].end) ranges[merged-1u].end=ranges[index].end;
    }else ranges[merged++]=ranges[index];
  }
  uint8_t *scan=malloc(CAB_SCAN_BYTES);
  if(!scan){ answer=ANYGM_EMBEDDED_CAB_INVALID; goto finish; }
  unsigned valid=0;
  int saw_magic=0;
  AnygmEmbeddedCab found={0};
  for(size_t range=0;range<merged;range++){
    uint64_t cursor=ranges[range].begin;
    while(cursor<ranges[range].end){
      size_t count=(uint64_t)CAB_SCAN_BYTES<ranges[range].end-cursor
                     ?CAB_SCAN_BYTES:(size_t)(ranges[range].end-cursor);
      if(!cab_read_at(router->host,file,source_size,cursor,scan,count)){
        free(scan); answer=ANYGM_EMBEDDED_CAB_INVALID; goto finish;
      }
      for(size_t index=0;index+4u<=count;index++){
        if(scan[index]!='M' || memcmp(scan+index,"MSCF",4)) continue;
        saw_magic=1;
        uint64_t candidate=cursor+(uint64_t)index;
        AnygmEmbeddedCab parsed={0};
        AnygmEmbeddedCabStatus status=cab_validate_candidate(router->host,file,source_size,
                                                              candidate,ranges[range].end,&parsed);
        if(status==ANYGM_EMBEDDED_CAB_SUPPORTED || status==ANYGM_EMBEDDED_CAB_UNSUPPORTED){
          if(valid++){
            free(scan); answer=ANYGM_EMBEDDED_CAB_INVALID; goto finish;
          }
          found=parsed; answer=status;
        }
      }
      if(count<4u) break;
      cursor+=(uint64_t)count-3u;
    }
  }
  free(scan);
  if(!valid && saw_magic) answer=ANYGM_EMBEDDED_CAB_INVALID;
  if(valid){
    uint64_t source_hash=0;
    if(!cab_hash_file(router,path,source_size,&source_hash)) answer=ANYGM_EMBEDDED_CAB_INVALID;
    else {
      found.source_size=source_size; found.source_hash=source_hash;
      if(cab) *cab=found;
    }
  }
finish:
  router->host->file_close(router->host->userdata,file);
  return answer;
}

static int cab_member_path(char *path){
  if(!path || !path[0]) return 0;
  size_t size=strlen(path);
  if(size>ANYGM_CONTENT_MAX_MEMBER_PATH || path[0]=='/' || path[0]=='\\' ||
     path[size-1]=='/' || path[size-1]=='\\') return 0;
  char *segment=path;
  unsigned components=0;
  for(char *cursor=path;;cursor++){
    unsigned char byte=(unsigned char)*cursor;
    if(byte=='\\'){ *cursor='/'; byte='/'; }
    if(byte && (byte<0x20u || byte==0x7fu || byte==':')) return 0;
    if(byte=='/' || !byte){
      size_t length=(size_t)(cursor-segment);
      if(!length || (length==1u && segment[0]=='.') ||
         (length==2u && segment[0]=='.' && segment[1]=='.')) return 0;
      /* Generated namespaces are removed by a recursion-bounded VFS walker.  Keep members within
       * that same bound so every failed transaction remains removable on every host. */
      if(++components>9u) return 0;
      if(!byte) break;
      segment=cursor+1;
    }
  }
  return 1;
}

static int cab_path_fold_equal(const char *left,const char *right){
  while(*left && *right){
    unsigned char a=(unsigned char)*left++,b=(unsigned char)*right++;
    if(a>='A'&&a<='Z') a=(unsigned char)(a-'A'+'a');
    if(b>='A'&&b<='Z') b=(unsigned char)(b-'A'+'a');
    if(a!=b) return 0;
  }
  return *left==*right;
}

static void cab_manifest_free(CabManifest *manifest){
  if(!manifest) return;
  for(size_t index=0;index<manifest->count;index++) free(manifest->members[index].path);
  free(manifest->members);
  memset(manifest,0,sizeof *manifest);
}

static int cab_manifest_add(CabManifest *manifest,const char *path,uint64_t size,uint64_t hash){
  if(!manifest || !path || manifest->count>=ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES) return 0;
  for(size_t index=0;index<manifest->count;index++)
    if(cab_path_fold_equal(manifest->members[index].path,path)) return 0;
  if(manifest->count==manifest->capacity){
    size_t capacity=manifest->capacity?manifest->capacity*2u:16u;
    if(capacity>ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES) capacity=ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES;
    CabMember *grown=realloc(manifest->members,capacity*sizeof *grown);
    if(!grown) return 0;
    manifest->members=grown; manifest->capacity=capacity;
  }
  char *copy=strdup(path);
  if(!copy) return 0;
  manifest->members[manifest->count++]=(CabMember){copy,size,hash};
  return 1;
}

static int cab_name_is_payload(const char *path){
  const char *name=strrchr(path,'/');
  name=name?name+1:path;
  return !strcasecmp(name,"data.win") || !strcasecmp(name,"data.alternate.win") ||
         !strcasecmp(name,"game.unx") || !strcasecmp(name,"game.droid");
}

static int cab_name_is_native(const char *path){
  const char *name=strrchr(path,'/');
  name=name?name+1:path;
  const char *extension=strrchr(name,'.');
  if(!extension) return 0;
  return !strcasecmp(extension,".exe") || !strcasecmp(extension,".dll") ||
         !strcasecmp(extension,".com") || !strcasecmp(extension,".scr") ||
         !strcasecmp(extension,".msi") || !strcasecmp(extension,".bat") ||
         !strcasecmp(extension,".cmd");
}

int anygm_embedded_cab_entry_allowed(AnygmEmbeddedCabEntryKind kind,
                                     int has_hardlink,int has_symlink){
  return kind==ANYGM_EMBEDDED_CAB_ENTRY_REGULAR && !has_hardlink && !has_symlink;
}

int anygm_embedded_cab_limits_allowed(uint64_t cabinet_size,unsigned entries,
                                      uint64_t member_size,uint64_t total_size){
  if(entries>ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES ||
     member_size>ANYGM_CONTENT_MAX_MEMBER_BYTES ||
     total_size>ANYGM_CONTENT_MAX_EXTRACTED_BYTES) return 0;
  if(total_size>ANYGM_CONTENT_EXPANSION_ALLOWANCE &&
     (!cabinet_size || cabinet_size>UINT64_MAX/ANYGM_CONTENT_MAX_EXPANSION_RATIO ||
      total_size>cabinet_size*ANYGM_CONTENT_MAX_EXPANSION_RATIO)) return 0;
  return 1;
}

static AnygmEmbeddedCabEntryKind cab_entry_kind(mode_t filetype){
  if(filetype==AE_IFREG) return ANYGM_EMBEDDED_CAB_ENTRY_REGULAR;
  if(filetype==AE_IFDIR) return ANYGM_EMBEDDED_CAB_ENTRY_DIRECTORY;
  if(filetype==AE_IFLNK) return ANYGM_EMBEDDED_CAB_ENTRY_SYMLINK;
  return ANYGM_EMBEDDED_CAB_ENTRY_SPECIAL;
}

static int cab_mkdir_parent(const AnygmContentRouter *router,char *path){
  char *slash=strrchr(path,'/');
  if(!slash) return 1;
  *slash=0;
  int ok=path[0] && anygm_vfs_mkdirs(router->host,path);
  *slash='/';
  return ok;
}

static int cab_join(char *out,size_t capacity,const char *root,const char *relative){
  int length=snprintf(out,capacity,"%s/%s",root,relative);
  return length>=0 && (size_t)length<capacity;
}

static int cab_write_member(const AnygmContentRouter *router,struct archive *archive,
                            const char *path,uint64_t declared,uint64_t *hash_out){
  char output[1536];
  if(!cab_join(output,sizeof output,path,"")) return 0;
  size_t length=strlen(output);
  if(length && output[length-1]=='/') output[length-1]=0;
  if(!cab_mkdir_parent(router,output)) return 0;
  void *file=router->host->file_open(router->host->userdata,output,
                                    ANYGM_FILE_WRITE|ANYGM_FILE_CREATE|ANYGM_FILE_TRUNCATE);
  if(!file) return 0;
  uint64_t written=0,hash=cab_hash_begin();
  int ok=1;
  for(;;){
    const void *block=NULL;
    size_t size=0;
    la_int64_t offset=0;
    int result=archive_read_data_block(archive,&block,&size,&offset);
    if(result==ARCHIVE_EOF) break;
    if(result!=ARCHIVE_OK || offset<0 || (uint64_t)offset!=written ||
       (uint64_t)size>declared-written){ ok=0; break; }
    size_t consumed=0;
    while(consumed<size){
      size_t step=router->host->file_write(router->host->userdata,file,
                                           (const uint8_t *)block+consumed,size-consumed);
      if(!step || step>size-consumed){ ok=0; break; }
      consumed+=step;
    }
    if(!ok) break;
    hash=cab_hash_update(hash,block,size);
    written+=(uint64_t)size;
  }
  if(written!=declared) ok=0;
  if(ok && router->host->file_flush)
    ok=router->host->file_flush(router->host->userdata,file)==ANYGM_OK;
  router->host->file_close(router->host->userdata,file);
  if(!ok){ anygm_vfs_remove(router->host,output); return 0; }
  *hash_out=hash;
  return 1;
}

static int cab_client_open(struct archive *archive,void *opaque){
  (void)archive;
  CabRangeClient *client=opaque;
  client->position=0;
  return ARCHIVE_OK;
}

static la_ssize_t cab_client_read(struct archive *archive,void *opaque,const void **buffer){
  (void)archive;
  CabRangeClient *client=opaque;
  if(client->position>=client->size){ *buffer=NULL; return 0; }
  size_t count=client->size-client->position>sizeof client->buffer
                 ?sizeof client->buffer:(size_t)(client->size-client->position);
  if(!cab_seek_absolute(client->host,client->file,client->begin+client->position)){
    client->failed=1; return -1;
  }
  size_t got=client->host->file_read(client->host->userdata,client->file,client->buffer,count);
  if(!got || got>count){ client->failed=1; return -1; }
  client->position+=(uint64_t)got;
  *buffer=client->buffer;
  return (la_ssize_t)got;
}

static la_int64_t cab_client_skip(struct archive *archive,void *opaque,la_int64_t request){
  (void)archive;
  CabRangeClient *client=opaque;
  if(request<=0) return 0;
  uint64_t amount=(uint64_t)request;
  if(amount>client->size-client->position) amount=client->size-client->position;
  if(!cab_seek_absolute(client->host,client->file,client->begin+client->position+amount)){
    client->failed=1; return -1;
  }
  client->position+=amount;
  return (la_int64_t)amount;
}

static la_int64_t cab_client_seek(struct archive *archive,void *opaque,la_int64_t request,
                                  int origin){
  (void)archive;
  CabRangeClient *client=opaque;
  uint64_t base=origin==SEEK_SET?0u:origin==SEEK_CUR?client->position:
                origin==SEEK_END?client->size:UINT64_MAX;
  uint64_t target=0;
  if(base==UINT64_MAX || (request<0 && (uint64_t)(-(request+1))+1u>base) ||
     (request>=0 && (uint64_t)request>client->size-base)) return ARCHIVE_FAILED;
  target=request<0?base-((uint64_t)(-(request+1))+1u):base+(uint64_t)request;
  if(!cab_seek_absolute(client->host,client->file,client->begin+target)){
    client->failed=1; return ARCHIVE_FAILED;
  }
  client->position=target;
  return (la_int64_t)target;
}

static int cab_client_close(struct archive *archive,void *opaque){
  (void)archive;
  CabRangeClient *client=opaque;
  if(client->file){
    client->host->file_close(client->host->userdata,client->file);
    client->file=NULL;
  }
  return ARCHIVE_OK;
}

static uint64_t cab_producer(void){
  static const char recipe[]="AnyGM embedded Cabinet cache; libarchive 3.8.9 CAB LZX-21";
  return cab_hash_update(cab_hash_begin(),recipe,sizeof recipe-1u);
}

static char cab_hex_digit(unsigned value){
  return (char)(value<10u?'0'+value:'a'+value-10u);
}

static int cab_path_encode(const char *path,char *encoded,size_t capacity){
  size_t length=path?strlen(path):0;
  if(!length || length>(capacity-1u)/2u) return 0;
  for(size_t index=0;index<length;index++){
    unsigned byte=(unsigned char)path[index];
    encoded[index*2u]=cab_hex_digit(byte>>4);
    encoded[index*2u+1u]=cab_hex_digit(byte&15u);
  }
  encoded[length*2u]=0;
  return 1;
}

static int cab_hex_value(char value){
  if(value>='0' && value<='9') return value-'0';
  if(value>='a' && value<='f') return value-'a'+10;
  if(value>='A' && value<='F') return value-'A'+10;
  return -1;
}

static int cab_path_decode(const char *encoded,char *path,size_t capacity){
  size_t length=encoded?strlen(encoded):0;
  if(!length || (length&1u) || length/2u>=capacity) return 0;
  for(size_t index=0;index<length/2u;index++){
    int high=cab_hex_value(encoded[index*2u]);
    int low=cab_hex_value(encoded[index*2u+1u]);
    if(high<0 || low<0) return 0;
    path[index]=(char)((unsigned)high<<4|(unsigned)low);
    if(!path[index]) return 0;
  }
  path[length/2u]=0;
  return cab_member_path(path);
}

static int cab_marker_build(const AnygmEmbeddedCab *cab,const CabManifest *manifest,
                            uint8_t **bytes,size_t *size){
  *bytes=NULL; *size=0;
  size_t capacity=256u+strlen(manifest->payload)*2u;
  for(size_t index=0;index<manifest->count;index++){
    size_t path_size=strlen(manifest->members[index].path);
    if(path_size>(SIZE_MAX-capacity-80u)/2u) return 0;
    capacity+=path_size*2u+80u;
  }
  uint8_t *buffer=malloc(capacity);
  if(!buffer) return 0;
  char encoded[ANYGM_CONTENT_MAX_MEMBER_PATH*2u+1u];
  if(!cab_path_encode(manifest->payload,encoded,sizeof encoded)){
    free(buffer); return 0;
  }
  int length=snprintf((char *)buffer,capacity,
    "AGCB %u %016llx %016llx %llu %llu %llu %u %llu %s\n",
    CAB_CACHE_SCHEMA,(unsigned long long)cab_producer(),
    (unsigned long long)cab->source_hash,(unsigned long long)cab->source_size,
    (unsigned long long)cab->offset,(unsigned long long)cab->size,cab->profile,
    (unsigned long long)manifest->count,encoded);
  if(length<0 || (size_t)length>=capacity){ free(buffer); return 0; }
  size_t used=(size_t)length;
  for(size_t index=0;index<manifest->count;index++){
    const CabMember *member=&manifest->members[index];
    if(!cab_path_encode(member->path,encoded,sizeof encoded)){
      free(buffer); return 0;
    }
    length=snprintf((char *)buffer+used,capacity-used,"%016llx %llu %s\n",
      (unsigned long long)member->hash,(unsigned long long)member->size,encoded);
    if(length<0 || (size_t)length>=capacity-used){ free(buffer); return 0; }
    used+=(size_t)length;
  }
  *bytes=buffer; *size=used;
  return 1;
}

static int cab_marker_parse(const uint8_t *bytes,size_t size,const AnygmEmbeddedCab *cab,
                            CabManifest *manifest){
  if(!bytes || !size || bytes[size-1u]!='\n') return 0;
  char *text=malloc(size+1u);
  if(!text) return 0;
  memcpy(text,bytes,size); text[size]=0;
  char *newline=strchr(text,'\n');
  if(!newline){ free(text); return 0; }
  *newline=0;
  unsigned schema=0,profile=0;
  unsigned long long producer=0,source_hash=0,source_size=0,offset=0,cab_size=0,count=0;
  char payload_hex[ANYGM_CONTENT_MAX_MEMBER_PATH*2u+1u];
  char payload[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
  int consumed=0;
  int fields=sscanf(text,"AGCB %u %llx %llx %llu %llu %llu %u %llu %1022s%n",
    &schema,&producer,&source_hash,&source_size,&offset,&cab_size,&profile,&count,
    payload_hex,&consumed);
  if(fields!=9 || text[consumed] || schema!=CAB_CACHE_SCHEMA || producer!=cab_producer() ||
     source_hash!=cab->source_hash || source_size!=cab->source_size || offset!=cab->offset ||
     cab_size!=cab->size || profile!=cab->profile || count>ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES ||
     !cab_path_decode(payload_hex,payload,sizeof payload)){
    free(text); return 0;
  }
  snprintf(manifest->payload,sizeof manifest->payload,"%s",payload);
  char *line=newline+1;
  for(unsigned long long index=0;index<count;index++){
    newline=strchr(line,'\n');
    if(!newline){ cab_manifest_free(manifest); free(text); return 0; }
    *newline=0;
    unsigned long long hash=0,member_size=0;
    char path_hex[ANYGM_CONTENT_MAX_MEMBER_PATH*2u+1u];
    char path[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
    consumed=0;
    if(sscanf(line,"%llx %llu %1022s%n",&hash,&member_size,path_hex,&consumed)!=3 ||
       line[consumed] || member_size>ANYGM_CONTENT_MAX_MEMBER_BYTES ||
       !cab_path_decode(path_hex,path,sizeof path) || cab_name_is_native(path) ||
       cab_path_fold_equal(path,CAB_CACHE_MARKER) ||
       !cab_manifest_add(manifest,path,member_size,hash)){
      cab_manifest_free(manifest); free(text); return 0;
    }
    line=newline+1;
  }
  int payload_found=0;
  for(size_t index=0;index<manifest->count;index++)
    if(!strcmp(manifest->members[index].path,manifest->payload)) payload_found=1;
  int ok=*line==0 && manifest->count==(size_t)count && payload_found &&
         cab_name_is_payload(manifest->payload);
  free(text);
  if(!ok) cab_manifest_free(manifest);
  return ok;
}

static const CabMember *cab_manifest_find(const CabManifest *manifest,const char *path){
  for(size_t index=0;index<manifest->count;index++)
    if(!strcmp(manifest->members[index].path,path)) return &manifest->members[index];
  return NULL;
}

static int cab_cache_walk(const AnygmContentRouter *router,const char *root,const char *relative,
                          const CabManifest *manifest,size_t *files,unsigned depth){
  if(depth>256u || !router->host->directory_open || !router->host->directory_read ||
     !router->host->directory_close) return 0;
  char directory[1536];
  if(relative[0]){ if(!cab_join(directory,sizeof directory,root,relative)) return 0; }
  else if(snprintf(directory,sizeof directory,"%s",root)>=(int)sizeof directory) return 0;
  void *handle=router->host->directory_open(router->host->userdata,directory);
  if(!handle) return 0;
  int ok=1;
  for(;;){
    AnygmDirectoryEntry entry={0}; entry.struct_size=sizeof entry;
    if(router->host->directory_read(router->host->userdata,handle,&entry)!=ANYGM_OK) break;
    if(!entry.name[0] || !strcmp(entry.name,".") || !strcmp(entry.name,"..")) continue;
    char child[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
    int length=relative[0]?snprintf(child,sizeof child,"%s/%s",relative,entry.name):
                           snprintf(child,sizeof child,"%s",entry.name);
    if(length<0 || (size_t)length>=sizeof child || !cab_member_path(child)){ ok=0; break; }
    if(entry.flags&ANYGM_FILE_INFO_DIRECTORY){
      if(!cab_cache_walk(router,root,child,manifest,files,depth+1u)){ ok=0; break; }
    }else if(entry.flags&ANYGM_FILE_INFO_REGULAR){
      if(!strcmp(child,CAB_CACHE_MARKER)) continue;
      if(!cab_manifest_find(manifest,child)){ ok=0; break; }
      (*files)++;
    }else { ok=0; break; }
  }
  router->host->directory_close(router->host->userdata,handle);
  return ok;
}

static int cab_cache_reuse(const AnygmContentRouter *router,const char *root,
                           const AnygmEmbeddedCab *cab,char *payload,size_t payload_size,
                           char *asset_root,size_t asset_root_size){
  char marker_path[1536];
  if(!cab_join(marker_path,sizeof marker_path,root,CAB_CACHE_MARKER)) return 0;
  uint8_t *bytes=NULL; size_t size=0;
  if(!anygm_vfs_read_all(router->host,marker_path,&bytes,&size,CAB_MARKER_MAX_BYTES)) return 0;
  CabManifest manifest={0};
  int ok=cab_marker_parse(bytes,size,cab,&manifest);
  free(bytes);
  uint64_t total=0;
  for(size_t index=0;ok && index<manifest.count;index++){
    char path[1536]; uint64_t hash=0;
    const CabMember *member=&manifest.members[index];
    AnygmFileInfo info;
    ok=cab_join(path,sizeof path,root,member->path) && anygm_vfs_stat(router->host,path,&info) &&
       (info.flags&ANYGM_FILE_INFO_REGULAR) && info.size==member->size &&
       cab_hash_file(router,path,member->size,&hash) && hash==member->hash &&
       total<=ANYGM_CONTENT_MAX_EXTRACTED_BYTES-member->size;
    if(ok) total+=member->size;
  }
  size_t files=0;
  if(ok) ok=cab_cache_walk(router,root,"",&manifest,&files,0) && files==manifest.count;
  if(ok){
    char selected[1536];
    ok=cab_join(selected,sizeof selected,root,manifest.payload) &&
       snprintf(payload,payload_size,"%s",selected)<(int)payload_size &&
       snprintf(asset_root,asset_root_size,"%s",root)<(int)asset_root_size;
  }
  cab_manifest_free(&manifest);
  return ok;
}

int anygm_embedded_cab_extract(const AnygmContentRouter *router,const char *source_path,
                               const AnygmEmbeddedCab *cab,char *payload_path,
                               size_t payload_path_size,char *asset_root,size_t asset_root_size){
  char ignored_asset_root[1024];
  if(!asset_root && !asset_root_size){
    asset_root=ignored_asset_root;
    asset_root_size=sizeof ignored_asset_root;
  }
  if(!router || !source_path || !cab || !payload_path || !payload_path_size ||
     !asset_root || !asset_root_size || !anygm_vfs_can_read(router->host) ||
     !anygm_vfs_can_write(router->host) || !router->host->file_seek ||
     !router->host->directory_create || !router->host->path_rename ||
     !router->host->path_remove) return 0;
  payload_path[0]=0; asset_root[0]=0;
  const char *base=router->cache_directory?router->cache_directory:"tmp";
  char stem[256],cache[1024],staging[1152];
  anygm_content_path_stem(source_path,stem,sizeof stem);
  if(snprintf(cache,sizeof cache,"%s/%s-%016llx-anygm-cab",base,stem,
              (unsigned long long)cab->source_hash)>=(int)sizeof cache ||
     snprintf(staging,sizeof staging,"%s.staging-%llx",cache,
              (unsigned long long)(uintptr_t)router)>=(int)sizeof staging) return 0;
  if(cab_cache_reuse(router,cache,cab,payload_path,payload_path_size,
                     asset_root,asset_root_size)){
    if(!cab_source_matches(router,source_path,cab)){
      payload_path[0]=0;
      asset_root[0]=0;
      cab_log(router,ANYGM_CONTENT_LOG_ERROR,
              "cabinet: input changed before verified cache reuse");
      return 0;
    }
    cab_log(router,ANYGM_CONTENT_LOG_INFO,"cabinet: reusing verified cache at %s",cache);
    return 1;
  }
  anygm_content_directory_remove(router->host,staging);
  if(!anygm_vfs_mkdirs(router->host,staging)) return 0;

  CabRangeClient client={0};
  client.host=router->host; client.begin=cab->offset; client.size=cab->size;
  client.file=router->host->file_open(router->host->userdata,source_path,ANYGM_FILE_READ);
  struct archive *archive=NULL;
  CabManifest manifest={0};
  CabManifest seen={0};
  uint64_t total=0;
  const char *failure="reader initialization";
  int ok=client.file!=NULL;
  if(ok) ok=(archive=archive_read_new())!=NULL;
  if(ok) ok=archive_read_support_filter_none(archive)==ARCHIVE_OK &&
            archive_read_support_format_cab(archive)==ARCHIVE_OK &&
            archive_read_set_callback_data(archive,&client)==ARCHIVE_OK &&
            archive_read_set_open_callback(archive,cab_client_open)==ARCHIVE_OK &&
            archive_read_set_read_callback(archive,cab_client_read)==ARCHIVE_OK &&
            archive_read_set_skip_callback(archive,cab_client_skip)==ARCHIVE_OK &&
            archive_read_set_seek_callback(archive,cab_client_seek)==ARCHIVE_OK &&
            archive_read_set_close_callback(archive,cab_client_close)==ARCHIVE_OK &&
            archive_read_open1(archive)==ARCHIVE_OK;
  unsigned entries=0,payloads=0;
  while(ok){
    struct archive_entry *entry=NULL;
    int result=archive_read_next_header(archive,&entry);
    if(result==ARCHIVE_EOF) break;
    if(result!=ARCHIVE_OK || !entry || ++entries>ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES){
      failure="member header"; ok=0; break;
    }
    const char *raw=archive_entry_pathname(entry);
    la_int64_t declared=archive_entry_size(entry);
    if(!raw || declared<0 ||
       !anygm_embedded_cab_limits_allowed(cab->size,entries,(uint64_t)declared,
                                           total+(uint64_t)declared) ||
       !anygm_embedded_cab_entry_allowed(cab_entry_kind(archive_entry_filetype(entry)),
                                         archive_entry_hardlink(entry)!=NULL,
                                         archive_entry_symlink(entry)!=NULL)){
      failure="member type or size"; ok=0; break;
    }
    char relative[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
    if(strlen(raw)>=sizeof relative){ failure="member path length"; ok=0; break; }
    snprintf(relative,sizeof relative,"%s",raw);
    if(!cab_member_path(relative) || cab_path_fold_equal(relative,CAB_CACHE_MARKER)){
      failure="member path"; ok=0; break;
    }
    if(total>ANYGM_CONTENT_MAX_EXTRACTED_BYTES-(uint64_t)declared){
      failure="total size limit"; ok=0; break;
    }
    if(!cab_manifest_add(&seen,relative,0,0)){ failure="member collision"; ok=0; break; }
    total+=(uint64_t)declared;
    if(cab_name_is_native(relative)){
      if(archive_read_data_skip(archive)!=ARCHIVE_OK){ failure="native member skip"; ok=0; break; }
      continue;
    }
    char output[1536]; uint64_t hash=0;
    if(!cab_join(output,sizeof output,staging,relative) ||
       !cab_write_member(router,archive,output,(uint64_t)declared,&hash) ||
       !cab_manifest_add(&manifest,relative,(uint64_t)declared,hash)){
      failure="member data write"; ok=0; break;
    }
    if(cab_name_is_payload(relative)){
      if(++payloads>1u){ failure="payload ambiguity"; ok=0; break; }
      snprintf(manifest.payload,sizeof manifest.payload,"%s",relative);
    }
  }
  if(archive){
    int close_result=archive_read_free(archive);
    if(close_result!=ARCHIVE_OK) ok=0;
  }else if(client.file) cab_client_close(NULL,&client);
  if(client.failed){ failure="bounded source callback"; ok=0; }
  if(ok && (payloads!=1u || !manifest.count)){
    cab_log(router,ANYGM_CONTENT_LOG_ERROR,
            "cabinet: payload selection saw %u entries, %u payloads, %llu extracted members",
            entries,payloads,(unsigned long long)manifest.count);
    failure="payload selection"; ok=0;
  }
  if(ok && !cab_source_matches(router,source_path,cab)){
    failure="source stability";
    ok=0;
  }
  uint8_t *marker=NULL; size_t marker_size=0;
  char marker_path[1536];
  if(ok){
    failure="cache marker";
    ok=cab_marker_build(cab,&manifest,&marker,&marker_size) &&
       cab_join(marker_path,sizeof marker_path,staging,CAB_CACHE_MARKER) &&
       anygm_vfs_write_all(router->host,marker_path,marker,marker_size);
  }
  free(marker);
  if(ok){
    anygm_content_directory_remove(router->host,cache);
    failure="atomic publication";
    ok=anygm_vfs_publish(router->host,staging,cache);
  }
  if(!ok){
    cab_log(router,ANYGM_CONTENT_LOG_ERROR,"cabinet: %s failed",failure);
    anygm_content_directory_remove(router->host,staging);
    cab_manifest_free(&manifest);
    cab_manifest_free(&seen);
    return 0;
  }
  char selected[1536];
  ok=cab_join(selected,sizeof selected,cache,manifest.payload) &&
     snprintf(payload_path,payload_path_size,"%s",selected)<(int)payload_path_size &&
     snprintf(asset_root,asset_root_size,"%s",cache)<(int)asset_root_size;
  cab_manifest_free(&manifest);
  cab_manifest_free(&seen);
  if(!ok) return 0;
  cab_log(router,ANYGM_CONTENT_LOG_INFO,"cabinet: extracted verified content to %s",cache);
  return 1;
}
