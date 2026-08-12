/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_classic_fidelity.h"

#include "anygm_vfs.h"
#include "gml_hash.h"
#include "bzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const uint8_t *data;
  size_t size,pos;
} FidelityReader;

static uint32_t fidelity_u32_at(const uint8_t *data){
  return (uint32_t)data[0]|(uint32_t)data[1]<<8|(uint32_t)data[2]<<16|
         (uint32_t)data[3]<<24;
}

static uint64_t fidelity_u64_at(const uint8_t *data){
  return (uint64_t)fidelity_u32_at(data)|(uint64_t)fidelity_u32_at(data+4)<<32;
}

static int fidelity_read(FidelityReader *reader,size_t size,const uint8_t **data){
  if(reader->pos>reader->size || size>reader->size-reader->pos) return 0;
  if(data) *data=reader->data+reader->pos;
  reader->pos+=size; return 1;
}

static int fidelity_u32(FidelityReader *reader,uint32_t *value){
  const uint8_t *data=NULL;
  if(!fidelity_read(reader,4,&data)) return 0;
  *value=fidelity_u32_at(data); return 1;
}

static int fidelity_u64(FidelityReader *reader,uint64_t *value){
  const uint8_t *data=NULL;
  if(!fidelity_read(reader,8,&data)) return 0;
  *value=fidelity_u64_at(data); return 1;
}

static char *fidelity_path(const char *project_path){
  size_t path_size=project_path?strlen(project_path):0;
  size_t suffix_size=strlen(GMLC_CLASSIC_FIDELITY_SUFFIX);
  if(!path_size || path_size>SIZE_MAX-suffix_size-1u) return NULL;
  char *path=(char*)malloc(path_size+suffix_size+1u);
  if(path){ memcpy(path,project_path,path_size);
    memcpy(path+path_size,GMLC_CLASSIC_FIDELITY_SUFFIX,suffix_size+1u); }
  return path;
}

static int fidelity_fail(char *err,size_t errcap,const char *detail){
  if(err&&errcap) snprintf(err,errcap,"classic fidelity: %s",detail);
  return 0;
}

int gmlc_classic_fidelity_apply_data(GmlcClassicManifest *manifest,
                                     const void *companion_data,size_t companion_size,
                                     const void *project_data,size_t project_size,
                                     char *err,size_t errcap){
  const uint8_t *contents=(const uint8_t*)companion_data;
  size_t size=companion_size;
  if(!manifest || !contents || !project_data)
    return fidelity_fail(err,errcap,"invalid arguments");
  if(size<GMLC_CLASSIC_FIDELITY_HEADER_SIZE || memcmp(contents,"AGCF",4))
    return fidelity_fail(err,errcap,"invalid companion header");
  uint32_t format=fidelity_u32_at(contents+4),header_size=fidelity_u32_at(contents+8);
  uint32_t header_flags=fidelity_u32_at(contents+12);
  uint32_t project_version=fidelity_u32_at(contents+16);
  uint32_t record_count=fidelity_u32_at(contents+20);
  uint64_t expected_size=fidelity_u64_at(contents+24);
  uint8_t digest[32]; gml_sha256(project_data,project_size,digest);
  if(format!=GMLC_CLASSIC_FIDELITY_VERSION ||
     header_size!=GMLC_CLASSIC_FIDELITY_HEADER_SIZE ||
     (header_flags&~(GMLC_CLASSIC_FIDELITY_EXECUTABLE_LAYOUT|
                     GMLC_CLASSIC_FIDELITY_SETTINGS|
                     GMLC_CLASSIC_FIDELITY_GAME_INFORMATION)) ||
     project_version!=(uint32_t)manifest->inventory.header.version ||
     expected_size!=(uint64_t)project_size || memcmp(digest,contents+32,32)){
    return fidelity_fail(err,errcap,"companion does not match this project");
  }
  uint64_t expected_records=0;
  for(int type=0;type<GMLC_CLASSIC_RESOURCE_TYPES;type++)
    for(uint32_t slot=0;slot<manifest->inventory.resource_slots[type];slot++)
      if(manifest->slots[type][slot].exists) expected_records++;
  if(record_count!=expected_records){
    return fidelity_fail(err,errcap,"resource record count mismatch");
  }
  FidelityReader reader={contents,size,GMLC_CLASSIC_FIDELITY_HEADER_SIZE};
  if(header_flags&GMLC_CLASSIC_FIDELITY_SETTINGS){
    uint32_t swap_creation_events=0;
    if(!fidelity_u32(&reader,&swap_creation_events) || swap_creation_events>1u)
      return fidelity_fail(err,errcap,"invalid settings record");
    manifest->inventory.settings.swap_creation_events=(int)swap_creation_events;
  }
  if(header_flags&GMLC_CLASSIC_FIDELITY_GAME_INFORMATION){
    uint64_t information_size=0; const uint8_t *information=NULL;
    if(!fidelity_u64(&reader,&information_size) ||
       information_size>GMLC_CLASSIC_FIDELITY_FILE_LIMIT ||
       !fidelity_read(&reader,(size_t)information_size,&information))
      return fidelity_fail(err,errcap,"invalid game-information record");
    uint8_t *copy=information_size?(uint8_t*)malloc((size_t)information_size):NULL;
    if(information_size && !copy) return fidelity_fail(err,errcap,"out of memory");
    if(information_size) memcpy(copy,information,(size_t)information_size);
    free(manifest->game_information.data);
    manifest->game_information.data=copy;
    manifest->game_information.size=(size_t)information_size;
  }
  uint64_t decoded_total=0;
  for(int expected_type=0;expected_type<GMLC_CLASSIC_RESOURCE_TYPES;expected_type++){
    for(uint32_t expected_slot=0;
        expected_slot<manifest->inventory.resource_slots[expected_type];expected_slot++){
      GmlcClassicResourceSlot *slot=&manifest->slots[expected_type][expected_slot];
      if(!slot->exists) continue;
      uint32_t type=0,index=0,version=0,flags=0;
      uint64_t payload_size=0,decoded_payload_size=0,source_size=0;
      if(!fidelity_u32(&reader,&type) || !fidelity_u32(&reader,&index) ||
         !fidelity_u32(&reader,&version) || !fidelity_u32(&reader,&flags) ||
         !fidelity_u64(&reader,&payload_size) ||
         !fidelity_u64(&reader,&decoded_payload_size) ||
         !fidelity_u64(&reader,&source_size) ||
         type!=(uint32_t)expected_type || index!=expected_slot || version!=slot->version ||
         (flags&~31u) ||
         (!(flags&GMLC_CLASSIC_FIDELITY_REPLACE_PAYLOAD) &&
          (payload_size || decoded_payload_size ||
           (flags&GMLC_CLASSIC_FIDELITY_PAYLOAD_BZIP2))) ||
         (!(flags&GMLC_CLASSIC_FIDELITY_REPLACE_SOURCE) && source_size) ||
         ((flags&GMLC_CLASSIC_FIDELITY_PAYLOAD_BZIP2) &&
          (!payload_size || !decoded_payload_size)) ||
         payload_size>SIZE_MAX || decoded_payload_size>GMLC_CLASSIC_FIDELITY_FILE_LIMIT ||
         source_size>=SIZE_MAX || decoded_payload_size>UINT64_MAX-decoded_total ||
         decoded_total+decoded_payload_size>GMLC_CLASSIC_FIDELITY_FILE_LIMIT){
        return fidelity_fail(err,errcap,"invalid resource record");
      }
      decoded_total+=decoded_payload_size;
      const uint8_t *payload=NULL,*source=NULL;
      if(!fidelity_read(&reader,(size_t)payload_size,&payload) ||
         !fidelity_read(&reader,(size_t)source_size,&source)){
        return fidelity_fail(err,errcap,"truncated resource record");
      }
      if(source_size && memchr(source,'\0',(size_t)source_size))
        return fidelity_fail(err,errcap,"invalid resource source text");
      if(flags&GMLC_CLASSIC_FIDELITY_REPLACE_PAYLOAD){
        uint8_t *copy=(uint8_t*)malloc(decoded_payload_size?(size_t)decoded_payload_size:1u);
        if(!copy) return fidelity_fail(err,errcap,"out of memory");
        int decoded=1;
        if(flags&GMLC_CLASSIC_FIDELITY_PAYLOAD_BZIP2){
          unsigned int destination_size=(unsigned int)decoded_payload_size;
          decoded=payload_size<=UINT32_MAX && decoded_payload_size<=UINT32_MAX &&
            BZ2_bzBuffToBuffDecompress((char*)copy,&destination_size,(char*)payload,
              (unsigned int)payload_size,0,0)==BZ_OK &&
            destination_size==(unsigned int)decoded_payload_size;
        } else if(payload_size==decoded_payload_size){
          if(payload_size) memcpy(copy,payload,(size_t)payload_size);
        } else decoded=0;
        if(!decoded){ free(copy);
          return fidelity_fail(err,errcap,"invalid compressed resource payload"); }
        free(slot->payload); slot->payload=copy;
        slot->payload_size=(size_t)decoded_payload_size;
      }
      if(flags&GMLC_CLASSIC_FIDELITY_REPLACE_SOURCE){
        char *copy=(char*)malloc((size_t)source_size+1u);
        if(!copy) return fidelity_fail(err,errcap,"out of memory");
        if(source_size) memcpy(copy,source,(size_t)source_size);
        copy[source_size]='\0'; free(slot->source); slot->source=copy;
      }
      slot->executable_layout=(flags&GMLC_CLASSIC_FIDELITY_SLOT_EXECUTABLE_LAYOUT)!=0;
      slot->legacy_layout=(flags&GMLC_CLASSIC_FIDELITY_SLOT_LEGACY_LAYOUT)!=0;
    }
  }
  if(reader.pos!=reader.size){
    return fidelity_fail(err,errcap,"trailing companion data");
  }
  manifest->executable_layout=(header_flags&GMLC_CLASSIC_FIDELITY_EXECUTABLE_LAYOUT)!=0;
  return 1;
}

int gmlc_classic_fidelity_apply(GmlcClassicManifest *manifest,
                                const AnygmHostServices *host,
                                const char *project_path,
                                const void *project_data,size_t project_size,
                                char *err,size_t errcap){
  char *path=fidelity_path(project_path);
  if(!manifest || !project_data || !path){
    free(path); return fidelity_fail(err,errcap,"invalid arguments");
  }
  AnygmFileInfo info;
  if(!anygm_vfs_stat(host,path,&info) || !(info.flags&ANYGM_FILE_INFO_REGULAR)){
    free(path); return 1;
  }
  uint8_t *contents=NULL; size_t size=0;
  int read=anygm_vfs_read_all(host,path,&contents,&size,GMLC_CLASSIC_FIDELITY_FILE_LIMIT);
  free(path);
  if(!read) return fidelity_fail(err,errcap,"cannot read companion");
  int ok=gmlc_classic_fidelity_apply_data(manifest,contents,size,project_data,project_size,
                                          err,errcap);
  free(contents); return ok;
}

static void fidelity_hash_bytes(uint64_t *hash,const void *data,size_t size){
  const uint8_t *bytes=(const uint8_t*)data;
  for(size_t i=0;i<size;i++){ *hash^=bytes[i]; *hash*=UINT64_C(1099511628211); }
}

int gmlc_classic_fidelity_dependency_hash(const AnygmHostServices *host,
                                          const char *project_path,
                                          uint64_t seed,uint64_t *hash_out){
  if(!project_path || !hash_out) return 0;
  char *path=fidelity_path(project_path);
  if(!path) return 0;
  uint8_t *contents=NULL; size_t size=0;
  AnygmFileInfo info;
  int present=anygm_vfs_stat(host,path,&info)&&(info.flags&ANYGM_FILE_INFO_REGULAR);
  int ok=!present || anygm_vfs_read_all(host,path,&contents,&size,
                                        GMLC_CLASSIC_FIDELITY_FILE_LIMIT);
  free(path);
  if(!ok){ free(contents); return 0; }
  uint64_t hash=seed;
  static const uint8_t domain[4]={'A','G','C','F'};
  fidelity_hash_bytes(&hash,domain,sizeof(domain));
  uint8_t marker=(uint8_t)present; fidelity_hash_bytes(&hash,&marker,1);
  if(present){
    uint8_t encoded_size[8];
    for(unsigned i=0;i<8;i++) encoded_size[i]=(uint8_t)((uint64_t)size>>(i*8u));
    fidelity_hash_bytes(&hash,encoded_size,sizeof(encoded_size));
    fidelity_hash_bytes(&hash,contents,size);
  }
  free(contents); *hash_out=hash; return 1;
}
