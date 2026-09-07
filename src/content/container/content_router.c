/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 *
 * Content path resolution, safe container extraction, and derived-content cache.
 */
#ifndef _WIN32
#define _GNU_SOURCE 1
#endif
#include "content_router.h"
#include "content_transform_source.h"
#include "content_config.h"
#include "gml_hash.h"
#include "embedded_cab.h"
#include "embedded_nsis.h"
#include "gmlc_package.h"
#include "gmlc_classic_project.h"
#include "gmlc_classic_import.h"
#include "gmlc_classic_fidelity.h"
#include "gmlc_project.h"
#include "gml_win.h"
#include "gml_image_codec.h"
#include "anygm_producer_fingerprint.h"
#include "anygm_vfs.h"
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

_Static_assert(GMLC_CLASSIC_FILE_LIMIT==ANYGM_CONTENT_MAX_MEMBER_BYTES,
               "classic project and content-router limits must remain aligned");

enum { CONFIG_ANCHOR_LAYERS=ANYGM_CONTENT_MAX_NESTING_LEVELS+3u };
typedef struct ContentConfigResolution {
  AnygmContentConfig *ini;
  AnygmContentTransforms *base_transforms;
  int input_adapted;
  uint8_t original_digest[32];
  char defaults[ANYGM_CONFIG_OVERRIDE_BYTES];
  char specific[ANYGM_CONFIG_OVERRIDE_BYTES];
  char anchors[CONFIG_ANCHOR_LAYERS][ANYGM_CONTENT_MAX_ANCHOR_BYTES];
} ContentConfigResolution;

static int select_payload_configuration(const AnygmContentRouter *router,const char *path);
static int original_file_digest(const AnygmContentRouter *router,const char *path,uint8_t digest[32]);
static int select_payload_digest(const AnygmContentRouter *router,const uint8_t digest[32],const char *label);
static int load_input_pipeline(const AnygmContentRouter *router,const char *path,
                                char *content_path,size_t content_size,
                                char *asset_root,size_t asset_root_size,
                                char *overrides,size_t overrides_size,int depth);

static void content_log(const AnygmContentRouter *router,int level,const char *format,...){
  if(!router || !router->log) return;
  char message[2048];
  va_list args;
  va_start(args,format);
  vsnprintf(message,sizeof message,format,args);
  va_end(args);
  router->log(router->log_userdata,level,message);
}

static int sibling_alternate_path(const char *path, char *out, size_t outsz) {
  if (!path || !out || outsz == 0) return 0;
  const char *slash = strrchr(path, '/');
  const char *bslash = strrchr(path, '\\');
  if (bslash && (!slash || bslash > slash)) slash = bslash;
  const char *name = "data.alternate.win";
  size_t dirn = slash ? (size_t)(slash - path + 1) : 0;
  size_t namen = strlen(name);
  if (dirn + namen + 1 > outsz) return 0;
  if (dirn) memcpy(out, path, dirn);
  memcpy(out + dirn, name, namen + 1);
  return 1;
}

static int path_join_bounded(char *out,size_t outsz,const char *parent,const char *relative){
  if(!out || !outsz || !parent || !relative) return 0;
  size_t parent_size=strlen(parent),relative_size=strlen(relative);
  if(parent_size>=outsz || relative_size>=outsz-parent_size-1u){
    out[0]='\0';
    return 0;
  }
  memcpy(out,parent,parent_size);
  out[parent_size]='/';
  memcpy(out+parent_size+1u,relative,relative_size+1u);
  return 1;
}

static int file_exists(const AnygmContentRouter *router,const char *path) {
  AnygmFileInfo info;
  return router&&anygm_vfs_stat(router->host,path,&info) &&
         (info.flags&ANYGM_FILE_INFO_REGULAR);
}

static int path_ext_is(const char *path, const char *ext) {
  size_t np = path ? strlen(path) : 0, ne = ext ? strlen(ext) : 0;
  return np >= ne && ne > 0 && !strcasecmp(path + np - ne, ext);
}

static const char *path_basename(const char *path) {
  const char *slash = path ? strrchr(path, '/') : NULL;
  const char *bs = path ? strrchr(path, '\\') : NULL;
  if (bs && (!slash || bs > slash)) slash = bs;
  return slash ? slash + 1 : (path ? path : "");
}

void anygm_content_path_stem(const char *path, char *out, size_t outsz) {
  if (!out || !outsz) return;
  snprintf(out, outsz, "%s", path_basename(path));
  char *dot = strrchr(out, '.');
  if (dot) *dot = 0;
}

void anygm_content_path_parent(const char *path, char *out, size_t outsz) {
  if (!out || !outsz) return;
  char parent[1024];
  snprintf(parent, sizeof(parent), "%s", path ? path : ".");
  char *slash = strrchr(parent, '/');
  char *bslash = strrchr(parent, '\\');
  if (bslash && (!slash || bslash > slash)) slash = bslash;
  if (slash) {
    if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
  } else snprintf(parent, sizeof(parent), ".");
  snprintf(out,outsz,"%s",parent);
}

static int path_name_is_generic_payload(const char *name){
  /* Exporters use platform-specific payload names. They contain the same data and share
   * one loader, so recognize the Linux name alongside the Windows and Android names. */
  return name && (!strcasecmp(name,"data.win") ||
                  !strcasecmp(name,"data.alternate.win") ||
                  !strcasecmp(name,"game.unx") ||
                  !strcasecmp(name,"game.droid"));
}

void anygm_content_save_label(const char *path,char *out,size_t outsz){
  if(!out || !outsz) return;
  char candidate[1024]={0};
  const char *name=path_basename(path);
  if(path_name_is_generic_payload(name)){
    char parent[1024];
    anygm_content_path_parent(path,parent,sizeof parent);
    const char *parent_name=path_basename(parent);
    if(parent_name[0] && strcmp(parent_name,".") && strcmp(parent_name,".."))
      snprintf(candidate,sizeof candidate,"%s",parent_name);
  }
  if(!candidate[0]) anygm_content_path_stem(path,candidate,sizeof candidate);

  size_t written=0;
  for(const unsigned char *source=(const unsigned char *)candidate;
      *source && written+1<outsz;source++){
    unsigned char c=*source;
    int safe=(c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') ||
             c=='-' || c=='_' || c=='.';
    out[written++]=safe?(char)c:'_';
  }
  out[written]='\0';
  if(!out[0]) snprintf(out,outsz,"content");
}

static uint64_t cache_hash_bytes(const void *data,size_t size){
  const uint8_t *bytes=data;
  uint64_t hash=UINT64_C(1469598103934665603);
  for(size_t i=0;i<size;i++){
    hash^=(uint64_t)bytes[i];
    hash*=UINT64_C(1099511628211);
  }
  return hash;
}

/* Every cached payload is identified by hashing the bytes it came from, so this reads whole
 * content files: a large archive is hundreds of megabytes and is read on every load.
 *
 * The arithmetic is not what costs; the reads are. Asking a host for sixteen kilobytes at a time
 * spends most of the wall clock crossing into it, and hosts that reach a Windows filesystem or a
 * network share pay that crossing dearly. Asking for a quarter of a megabyte instead moves the
 * same bytes in a fraction of the time and yields the same hash, since the result cannot depend
 * on how the stream was divided. */
#define FILE_STREAM_BUFFER_BYTES (256u*1024u)

static int file_hash64(const AnygmContentRouter *router,const char *path,uint64_t *hash_out) {
  if(!router || !anygm_vfs_can_read(router->host)) return 0;
  void *file=router->host->file_open(router->host->userdata,path,ANYGM_FILE_READ);
  if(!file) return 0;
  uint64_t h = UINT64_C(1469598103934665603);
  uint8_t fallback[16384];
  size_t capacity=FILE_STREAM_BUFFER_BYTES;
  uint8_t *buf=malloc(capacity);
  if(!buf){ buf=fallback; capacity=sizeof fallback; }
  size_t n;
  while ((n=router->host->file_read(router->host->userdata,file,buf,capacity))>0) {
    if(n>capacity){
      if(buf!=fallback) free(buf);
      router->host->file_close(router->host->userdata,file);
      return 0;
    }
    for (size_t i = 0; i < n; i++) {
      h ^= (uint64_t)buf[i];
      h *= 1099511628211ull;
    }
  }
  if(buf!=fallback) free(buf);
  router->host->file_close(router->host->userdata,file);
  if (hash_out) *hash_out = h;
  return 1;
}

enum {
  ANYGM_CACHE_SCHEMA=1,
  ANYGM_CACHE_PRODUCER_REVISION=2,
  ANYGM_CACHE_ARCHIVE=1,
  ANYGM_CACHE_SOURCE_PROJECT=2,
  ANYGM_CACHE_CLASSIC_PROJECT=3,
  ANYGM_CACHE_STUDIO_EXECUTABLE=4,
  ANYGM_CACHE_HEADER_SIZE=72
};
/* The cache producer combines the structural recipe with a fingerprint of the
 * first-party sources. A changed producer invalidates prior cached payloads. */
static uint64_t cache_producer_fingerprint(void){
  static const char recipe[]=
    "AnyGM cache producer: structural package; classic import; bounded archive and executable extraction";
  return cache_hash_bytes(recipe,sizeof recipe-1)^ANYGM_PRODUCER_FINGERPRINT;
}
static void cache_put_u32(uint8_t *p,uint32_t v){
  p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static void cache_put_u64(uint8_t *p,uint64_t v){
  cache_put_u32(p,(uint32_t)v); cache_put_u32(p+4,(uint32_t)(v>>32));
}
static uint32_t cache_get_u32(const uint8_t *p){
  return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static uint64_t cache_get_u64(const uint8_t *p){
  return (uint64_t)cache_get_u32(p)|((uint64_t)cache_get_u32(p+4)<<32);
}
static int cache_marker_read(const AnygmContentRouter *router,const char *path,uint32_t kind,
                             uint64_t source_fingerprint,uint64_t source_size,
                             uint64_t *payload_fingerprint,uint64_t *payload_size,
                             char *data,size_t data_cap){
  uint8_t *contents=NULL;
  size_t contents_size=0;
  if(!router || !anygm_vfs_read_all(router->host,path,&contents,&contents_size,
                                    ANYGM_CACHE_HEADER_SIZE+data_cap)) return 0;
  uint8_t *header=contents;
  int ok=contents_size>=ANYGM_CACHE_HEADER_SIZE;
  uint32_t data_len=ok?cache_get_u32(header+20):0;
  if(!ok || memcmp(header,"AGCH",4) || cache_get_u32(header+4)!=ANYGM_CACHE_SCHEMA ||
     cache_get_u32(header+8)!=ANYGM_CACHE_HEADER_SIZE || cache_get_u32(header+12)!=kind ||
     cache_get_u32(header+16)!=ANYGM_CACHE_PRODUCER_REVISION ||
     cache_get_u64(header+24)!=source_fingerprint || cache_get_u64(header+32)!=source_size ||
     cache_get_u64(header+64)!=cache_producer_fingerprint() || data_len>=data_cap){
    free(contents); return 0;
  }
  if(contents_size!=ANYGM_CACHE_HEADER_SIZE+data_len) ok=0;
  uint64_t stored_data_hash=ok?cache_get_u64(header+56):0;
  uint64_t stored_payload_fingerprint=ok?cache_get_u64(header+40):0;
  uint64_t stored_payload_size=ok?cache_get_u64(header+48):0;
  if(ok&&data_len) memcpy(data,contents+ANYGM_CACHE_HEADER_SIZE,data_len);
  free(contents);
  if(ok && cache_hash_bytes(data,data_len)!=stored_data_hash) ok=0;
  if(!ok) return 0;
  data[data_len]=0;
  if(payload_fingerprint) *payload_fingerprint=stored_payload_fingerprint;
  if(payload_size) *payload_size=stored_payload_size;
  return 1;
}
static int cache_marker_write(const AnygmContentRouter *router,const char *path,uint32_t kind,
                              uint64_t source_fingerprint,uint64_t source_size,
                              uint64_t payload_fingerprint,uint64_t payload_size,
                              const char *data){
  size_t data_len=data?strlen(data):0;
  if(data_len>UINT32_MAX) return 0;
  uint8_t header[ANYGM_CACHE_HEADER_SIZE]={0};
  memcpy(header,"AGCH",4);
  cache_put_u32(header+4,ANYGM_CACHE_SCHEMA);
  cache_put_u32(header+8,ANYGM_CACHE_HEADER_SIZE);
  cache_put_u32(header+12,kind);
  cache_put_u32(header+16,ANYGM_CACHE_PRODUCER_REVISION);
  cache_put_u32(header+20,(uint32_t)data_len);
  cache_put_u64(header+24,source_fingerprint);
  cache_put_u64(header+32,source_size);
  cache_put_u64(header+40,payload_fingerprint);
  cache_put_u64(header+48,payload_size);
  cache_put_u64(header+56,cache_hash_bytes(data,data_len));
  cache_put_u64(header+64,cache_producer_fingerprint());
  char temporary[1024];
  if(snprintf(temporary,sizeof temporary,"%s.tmp",path)>=(int)sizeof temporary) return 0;
  if(!router || SIZE_MAX-sizeof header<data_len) return 0;
  size_t total=sizeof header+data_len;
  uint8_t *contents=malloc(total?total:1);
  if(!contents) return 0;
  memcpy(contents,header,sizeof header);
  if(data_len) memcpy(contents+sizeof header,data,data_len);
  int ok=anygm_vfs_write_all(router->host,temporary,contents,total);
  free(contents);
  if(!ok){ anygm_vfs_remove(router->host,temporary); return 0; }
  if(!anygm_vfs_publish(router->host,temporary,path)){
    /* Never remove a previously valid marker to emulate replace semantics. The host rename
     * contract is atomic; a provider that cannot replace leaves the old marker authoritative. */
    anygm_vfs_remove(router->host,temporary);
    return 0;
  }
  return 1;
}
static int file_size64(const AnygmContentRouter *router,const char *path,uint64_t *size_out){
  AnygmFileInfo info;
  if(!router || !anygm_vfs_stat(router->host,path,&info) ||
     !(info.flags&ANYGM_FILE_INFO_REGULAR)) return 0;
  if(size_out) *size_out=info.size;
  return 1;
}

int anygm_content_load_win(const AnygmContentRouter *router,GmlWin *win,const char *path,
                           const char *asset_root,char *loaded_path,size_t loaded_path_sz) {
  if(!router || !win || !path) return 0;
  if(gml_win_load_host(win,router->host,path)!=0){
    /* Preserve the loader's specific validation reason instead of flattening every failure
     * to the same content-load diagnostic. */
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"payload rejected: %s (%s)",
                gml_win_last_load_error(),path);
    return 0;
  }
  if (loaded_path && loaded_path_sz) snprintf(loaded_path, loaded_path_sz, "%s", path);
  if(win->n_code>0) return 1;

  char alt[1024];
  if (!sibling_alternate_path(path, alt, sizeof(alt)) || !file_exists(router,alt)){
    /* A normalized image can live in the disposable cache while its authored
     * code companion remains alongside the original assets. */
    if(!asset_root || !asset_root[0] ||
       !path_join_bounded(alt,sizeof alt,asset_root,"data.alternate.win") ||
       !file_exists(router,alt)) return 1;
  }
  GmlWin altw;
  memset(&altw, 0, sizeof(altw));
  if (gml_win_load_host(&altw,router->host,alt) != 0) return 1;
  if (altw.n_code <= 0) {
    gml_win_free(&altw);
    return 1;
  }
  gml_win_free(win);
  *win=altw;
  if (loaded_path && loaded_path_sz) snprintf(loaded_path, loaded_path_sz, "%s", alt);
  return 2;
}

/* ---- ZIP-compatible content containers (.zip/.port/.apk and ZIP-shaped game.droid) ----
 * The archive image and each selected deflated member are held in bounded memory. The inflater
 * uses the same bounded raw-DEFLATE leaf that backs PNG decoding.
 *
 * Content resolution is data driven: data.win, game.droid, an arbitrary *.win fallback, then a
 * nested .port/.apk/.zip. Only the selected payload's subtree is extracted. Android runtime
 * libraries alongside assets/game.droid are therefore neither loaded nor copied. */
static uint32_t zu32(const uint8_t *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t zu16(const uint8_t *p){ return (uint16_t)(p[0]|(p[1]<<8)); }
static int mkdirs_for(const AnygmContentRouter *router,char *path,int full){
  if(!router || !path) return 0;
  if(full) return anygm_vfs_mkdirs(router->host,path);
  char *slash=strrchr(path,'/'),*backslash=strrchr(path,'\\');
  if(backslash&&(!slash||backslash>slash)) slash=backslash;
  if(!slash) return 1;
  char saved=*slash;
  *slash=0;
  int ok=path[0]?anygm_vfs_mkdirs(router->host,path):1;
  *slash=saved;
  return ok;
}
int anygm_content_directory_create(const struct AnygmHostServices *host,const char *path){
  return anygm_vfs_mkdirs(host,path);
}

/* Remove one generated namespace and everything under it.  Bounded by ANYGM_CONTENT_CLEAR_MAX_DEPTH
 * so a host that reports a cyclic tree cannot drive unbounded recursion, and confined to the caller's
 * path: this only ever runs on a directory the runtime itself generated. */
#define ANYGM_CONTENT_CLEAR_MAX_DEPTH 8
static int content_directory_remove_depth(const struct AnygmHostServices *host,const char *path,
                                          int depth){
  if(!host || !path || !path[0]) return 0;
  if(depth>ANYGM_CONTENT_CLEAR_MAX_DEPTH) return 0;
  if(host->directory_open && host->directory_read && host->directory_close){
    void *directory=host->directory_open(host->userdata,path);
    if(directory){
      for(;;){
        AnygmDirectoryEntry entry;
        memset(&entry,0,sizeof entry);
        entry.struct_size=sizeof entry;
        if(host->directory_read(host->userdata,directory,&entry)!=ANYGM_OK) break;
        if(!entry.name[0] || !strcmp(entry.name,".") || !strcmp(entry.name,"..")) continue;
        char child[4096];
        if((size_t)snprintf(child,sizeof child,"%s/%s",path,entry.name)>=sizeof child) continue;
        if(entry.flags&ANYGM_FILE_INFO_DIRECTORY)
          content_directory_remove_depth(host,child,depth+1);
        else
          anygm_vfs_remove(host,child);
      }
      host->directory_close(host->userdata,directory);
    }
  }
  anygm_vfs_remove(host,path);
  AnygmFileInfo info;
  return anygm_vfs_stat(host,path,&info) && (info.flags&ANYGM_FILE_INFO_EXISTS) ? 0 : 1;
}
int anygm_content_directory_remove(const struct AnygmHostServices *host,const char *path){
  return content_directory_remove_depth(host,path,0);
}

typedef struct { uint8_t *data; size_t size; } GmlFileMap;
static int file_map_readonly(const AnygmContentRouter *router,const char *path,GmlFileMap *m){
  memset(m,0,sizeof(*m));
  return router&&anygm_vfs_read_all(router->host,path,&m->data,&m->size,
                                    (size_t)ANYGM_CONTENT_MAX_ARCHIVE_BYTES);
}
static void file_map_close(GmlFileMap *m){
  if(!m || !m->data) return;
  free(m->data);
  memset(m,0,sizeof(*m));
}

static int load_classic_project_content(const AnygmContentRouter *router,const char *srcpath,
                                        char *content_path,size_t cpsz);
static int file_magic_kind(const AnygmContentRouter *router,const char *path);

static int router_read_at(const AnygmContentRouter *router,void *file,uint64_t source_size,
                          uint64_t offset,void *data,size_t size){
  if(!router || !file || !router->host->file_seek || offset>source_size ||
     (uint64_t)size>source_size-offset || offset>INT64_MAX ||
     router->host->file_seek(router->host->userdata,file,(int64_t)offset,ANYGM_SEEK_START)!=
       (int64_t)offset) return 0;
  size_t used=0;
  while(used<size){
    size_t count=router->host->file_read(router->host->userdata,file,
                                         (uint8_t *)data+used,size-used);
    if(!count || count>size-used) return 0;
    used+=count;
  }
  return 1;
}

/* Select a single bounded FORM data image carried by an executable. Validation uses the ordinary
 * content reader; ambiguous or malformed candidates are never selected. */
static int embedded_studio_form_path(const AnygmContentRouter *router,const char *path,
                                     uint64_t source_size,uint8_t **payload,size_t *payload_size){
  *payload=NULL; *payload_size=0;
  if(!router || !path || source_size<10u || !router->host->file_seek) return 0;
  void *file=router->host->file_open(router->host->userdata,path,ANYGM_FILE_READ);
  if(!file) return 0;
  uint8_t magic[8];
  int answer=0;
  if(!router_read_at(router,file,source_size,0,magic,2u) || magic[0]!='M' || magic[1]!='Z')
    goto done;
  uint8_t *scan=malloc(FILE_STREAM_BUFFER_BYTES);
  if(!scan){ answer=-1; goto done; }
  uint64_t cursor=2u,last_candidate=UINT64_MAX;
  while(cursor+8u<=source_size){
    size_t count=(uint64_t)FILE_STREAM_BUFFER_BYTES<source_size-cursor
                   ?FILE_STREAM_BUFFER_BYTES:(size_t)(source_size-cursor);
    if(!router_read_at(router,file,source_size,cursor,scan,count)){ answer=-1; break; }
    for(size_t index=0;index+8u<=count;index++){
      if(scan[index]!='F' || memcmp(scan+index,"FORM",4)) continue;
      uint64_t offset=cursor+(uint64_t)index;
      if(offset==last_candidate) continue;
      last_candidate=offset;
      uint32_t body=zu32(scan+index+4u);
      uint64_t extent=(uint64_t)body+8u;
      if(body>GML_WIN_MAX_FILE_BYTES-8u || extent>GML_WIN_MAX_FILE_BYTES ||
         extent>source_size-offset || extent>SIZE_MAX) continue;
      uint8_t *candidate=malloc((size_t)extent);
      if(!candidate){ answer=-1; break; }
      if(!router_read_at(router,file,source_size,offset,candidate,(size_t)extent)){
        free(candidate); answer=-1; break;
      }
      GmlWin probe;
      if(gml_win_from_mem(&probe,candidate,(size_t)extent,0)!=0){ free(candidate); continue; }
      gml_win_free(&probe);
      if(*payload){ free(candidate); answer=-1; break; }
      *payload=candidate; *payload_size=(size_t)extent; answer=1;
    }
    if(answer<0 || count<8u) break;
    cursor+=(uint64_t)count-7u;
  }
  free(scan);
done:
  router->host->file_close(router->host->userdata,file);
  if(answer<=0){ free(*payload); *payload=NULL; *payload_size=0; }
  return answer;
}

static int load_studio_executable_content(const AnygmContentRouter *router,const char *srcpath,
                                          char *content_path,size_t content_path_size){
  uint64_t source_size=0,source_hash=0;
  if(!router || !file_size64(router,srcpath,&source_size) ||
     source_size>ANYGM_CONTENT_MAX_EXECUTABLE_BYTES){
    if(source_size>ANYGM_CONTENT_MAX_EXECUTABLE_BYTES)
      content_log(router,ANYGM_CONTENT_LOG_ERROR,
                  "executable: input exceeds the bounded executable size");
    return source_size>ANYGM_CONTENT_MAX_EXECUTABLE_BYTES?-1:0;
  }
  if(!file_hash64(router,srcpath,&source_hash)) return 0;

  const char *base=router->cache_directory?router->cache_directory:"tmp";
  char stem[256],outdir[768],outwin[900],marker[900];
  anygm_content_path_stem(srcpath,stem,sizeof stem);
  snprintf(outdir,sizeof outdir,"%s/%s-%016llx-anygm-studio-executable",base,stem,
           (unsigned long long)source_hash);
  snprintf(outwin,sizeof outwin,"%s/data.win",outdir);
  snprintf(marker,sizeof marker,"%s/.anygm_cache",outdir);

  char marker_data[1];
  uint64_t stored_hash=0,stored_size=0,actual_hash=0,actual_size=0;
  if(cache_marker_read(router,marker,ANYGM_CACHE_STUDIO_EXECUTABLE,source_hash,source_size,
                       &stored_hash,&stored_size,marker_data,sizeof marker_data) &&
     file_size64(router,outwin,&actual_size) && actual_size==stored_size &&
     file_hash64(router,outwin,&actual_hash) && actual_hash==stored_hash){
    snprintf(content_path,content_path_size,"%s",outwin);
    content_log(router,ANYGM_CONTENT_LOG_INFO,
                "executable: reusing extracted Studio payload at %s",outwin);
    return 1;
  }

  uint8_t *form=NULL;
  size_t form_size=0;
  int found=embedded_studio_form_path(router,srcpath,source_size,&form,&form_size);
  if(found<0){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,
                "executable: multiple normalized Studio payloads are ambiguous");
    return -1;
  }
  if(!found) return 0;
  if(!mkdirs_for(router,outdir,1) ||
     !anygm_vfs_write_all(router->host,outwin,form,form_size)){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,
                "executable: could not materialize the normalized Studio payload");
    free(form);
    return -1;
  }
  free(form);
  uint64_t verified_source_hash=0;
  if(!file_hash64(router,srcpath,&verified_source_hash) || verified_source_hash!=source_hash){
    anygm_vfs_remove(router->host,outwin);
    content_log(router,ANYGM_CONTENT_LOG_ERROR,
                "executable: input changed while it was being inspected");
    return -1;
  }
  if(!file_size64(router,outwin,&actual_size) || actual_size!=form_size ||
     !file_hash64(router,outwin,&actual_hash)){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,
                "executable: extracted Studio payload failed verification");
    return -1;
  }
  if(!cache_marker_write(router,marker,ANYGM_CACHE_STUDIO_EXECUTABLE,source_hash,source_size,
                         actual_hash,actual_size,NULL))
    content_log(router,ANYGM_CONTENT_LOG_WARN,
                "executable: could not publish cache marker");
  snprintf(content_path,content_path_size,"%s",outwin);
  content_log(router,ANYGM_CONTENT_LOG_INFO,
              "executable: extracted Studio payload to %s",outwin);
  return 1;
}

/* Every supported Classic executable family carries one of these structural candidate markers.
 * Looking for them outside an already validated CAB range keeps the higher-priority Classic route
 * intact without making a CAB-only load allocate the complete executable merely to prove absence. */
static int classic_executable_maybe(const AnygmContentRouter *router,const char *path,
                                    uint64_t source_size,const AnygmEmbeddedCab *cab){
  if(!router || !path || !router->host->file_seek || source_size<8u) return 0;
  void *file=router->host->file_open(router->host->userdata,path,ANYGM_FILE_READ);
  if(!file) return 0;
  uint8_t *buffer=malloc(FILE_STREAM_BUFFER_BYTES);
  if(!buffer){ router->host->file_close(router->host->userdata,file); return 0; }
  int found=0;
  uint64_t cursor=0;
  while(cursor+8u<=source_size && !found){
    size_t count=(uint64_t)FILE_STREAM_BUFFER_BYTES<source_size-cursor
                   ?FILE_STREAM_BUFFER_BYTES:(size_t)(source_size-cursor);
    if(!router_read_at(router,file,source_size,cursor,buffer,count)) break;
    for(size_t index=0;index+8u<=count;index++){
      uint64_t absolute=cursor+(uint64_t)index;
      if(cab && absolute>=cab->offset && absolute<cab->offset+cab->size) continue;
      uint32_t first=zu32(buffer+index),second=zu32(buffer+index+4u);
      if(first==UINT32_C(1234321)){
        found=1;
        break;
      }
    }
    if(count<8u) break;
    cursor+=(uint64_t)count-7u;
  }
  free(buffer);
  router->host->file_close(router->host->userdata,file);
  return found;
}

/* The adjacent data.win convention requires a bounded PE envelope. Checking the
 * PE envelope before following it keeps an arbitrary file named .exe from gaining sibling-file
 * authority. The payload and its assets still pass through the ordinary host VFS and Studio
 * loader; no native code is executed. */
static int executable_pe_valid(const AnygmContentRouter *router,const char *path,
                               uint64_t source_size){
  if(!router || !path || !router->host || !router->host->file_open ||
     !router->host->file_read || !router->host->file_seek || !router->host->file_close ||
     source_size<64u) return 0;
  void *file=router->host->file_open(router->host->userdata,path,ANYGM_FILE_READ);
  if(!file) return 0;
  uint8_t header[64];
  int valid=router_read_at(router,file,source_size,0,header,sizeof header) &&
            header[0]=='M' && header[1]=='Z';
  uint64_t pe_offset=valid?zu32(header+60u):0;
  if(!valid || pe_offset>source_size || source_size-pe_offset<24u ||
     !router_read_at(router,file,source_size,pe_offset,header,24u) ||
     memcmp(header,"PE\0\0",4)) valid=0;
  uint16_t section_count=valid?(uint16_t)(header[6]|(header[7]<<8)):0;
  uint16_t optional_size=valid?(uint16_t)(header[20]|(header[21]<<8)):0;
  if(!section_count || section_count>96u ||
     pe_offset>UINT64_MAX-24u-(uint64_t)optional_size) valid=0;
  uint64_t section_table=valid?pe_offset+24u+(uint64_t)optional_size:0;
  if(valid && (section_table>source_size ||
               (uint64_t)section_count>(source_size-section_table)/40u)) valid=0;
  for(uint16_t index=0;valid && index<section_count;index++){
    if(!router_read_at(router,file,source_size,section_table+(uint64_t)index*40u,header,40u)){
      valid=0;
      break;
    }
    uint64_t size=zu32(header+16u),offset=zu32(header+20u);
    if(size && (offset>source_size || size>source_size-offset)) valid=0;
  }
  router->host->file_close(router->host->userdata,file);
  return valid;
}

static AnygmContentResolveResult resolve_adjacent_studio_payload(
    const AnygmContentRouter *router,const char *executable,uint64_t source_size,
    char *content_path,size_t content_size){
  if(!executable_pe_valid(router,executable,source_size))
    return ANYGM_CONTENT_RESOLVE_INVALID;
  char parent[1024],payload[1536];
  anygm_content_path_parent(executable,parent,sizeof parent);
  if(!path_join_bounded(payload,sizeof payload,parent,"data.win") ||
     !file_exists(router,payload) || file_magic_kind(router,payload)!=1 ||
     snprintf(content_path,content_size,"%s",payload)>=(int)content_size){
    content_path[0]='\0';
    return ANYGM_CONTENT_RESOLVE_INVALID;
  }
  content_log(router,ANYGM_CONTENT_LOG_INFO,
              "executable: using adjacent Studio payload at %s",payload);
  return ANYGM_CONTENT_RESOLVE_OK;
}

/* A Studio payload identifies its project in GEN8: two string offsets into the payload's own
 * string pool (the project file name and the display name) and one numeric game id. Reading those
 * three fields answers "is this the same project?" without loading either payload, and reports
 * whether the payload carries executable code at all.
 *
 * Walking the chunk table must not stop at the first zero-length chunk: several chunks are
 * routinely empty in a normal payload, and stopping there never reaches CODE. */
typedef struct {
  char filename[128];
  char name[128];
  uint32_t game_id;
  int has_code;
} StudioPayloadIdentity;

static int studio_payload_string(const AnygmContentRouter *router,void *file,uint64_t size,
                                 uint64_t offset,char *out,size_t out_size){
  uint8_t length_field[4];
  out[0]='\0';
  if(offset<4u || !router_read_at(router,file,size,offset-4u,length_field,4u)) return 0;
  uint64_t length=zu32(length_field);
  if(!length || length>=out_size) return 0;
  if(!router_read_at(router,file,size,offset,out,(size_t)length)) return 0;
  out[length]='\0';
  for(uint64_t index=0;index<length;index++)
    if((unsigned char)out[index]<0x20u) return 0;
  return 1;
}

static int studio_payload_identity(const AnygmContentRouter *router,const char *path,
                                   StudioPayloadIdentity *identity){
  memset(identity,0,sizeof *identity);
  uint64_t size=0;
  if(!router || !anygm_vfs_can_read(router->host) || !file_size64(router,path,&size)) return 0;
  void *file=router->host->file_open(router->host->userdata,path,ANYGM_FILE_READ);
  if(!file) return 0;
  int ok=0;
  uint8_t header[8];
  uint64_t gen8=0,gen8_length=0;
  if(router_read_at(router,file,size,0,header,8u) && !memcmp(header,"FORM",4)){
    uint64_t end=8u+(uint64_t)zu32(header+4u);
    if(end>size) end=size;
    for(uint64_t cursor=8u;cursor+8u<=end;){
      if(!router_read_at(router,file,size,cursor,header,8u)) break;
      uint64_t length=zu32(header+4u);
      if(length>end-(cursor+8u)) break;
      if(!memcmp(header,"GEN8",4)){ gen8=cursor+8u; gen8_length=length; }
      else if(!memcmp(header,"CODE",4) && length) identity->has_code=1;
      cursor+=8u+length;
    }
  }
  /* GEN8 places the project file name at +4, the game id at +20 and the display name at +40. */
  if(gen8 && gen8_length>=44u){
    uint8_t fields[4];
    uint64_t filename_offset=0,name_offset=0;
    if(router_read_at(router,file,size,gen8+4u,fields,4u)) filename_offset=zu32(fields);
    if(router_read_at(router,file,size,gen8+20u,fields,4u)) identity->game_id=zu32(fields);
    if(router_read_at(router,file,size,gen8+40u,fields,4u)) name_offset=zu32(fields);
    ok=studio_payload_string(router,file,size,filename_offset,
                             identity->filename,sizeof identity->filename) &&
       studio_payload_string(router,file,size,name_offset,identity->name,sizeof identity->name);
  }
  router->host->file_close(router->host->userdata,file);
  return ok;
}

/* An executable whose embedded payload carries no code cannot be run at all: the choice is not
 * between a safe load and an unsafe one, it is between the neighbour and nothing. Deferring to the
 * neighbour is still a substitution, so it is allowed only when the neighbour is demonstrably the
 * same project - same project file name, same display name, same game id - which a payload that
 * merely sits in the same directory cannot claim. */
static int embedded_payload_defers_to_adjacent(const AnygmContentRouter *router,
                                               const char *executable,const char *extracted){
  StudioPayloadIdentity embedded,adjacent;
  if(!studio_payload_identity(router,extracted,&embedded) || embedded.has_code) return 0;
  char parent[1024],payload[1536];
  anygm_content_path_parent(executable,parent,sizeof parent);
  if(!path_join_bounded(payload,sizeof payload,parent,"data.win") || !file_exists(router,payload))
    return 0;
  if(!studio_payload_identity(router,payload,&adjacent) || !adjacent.has_code) return 0;
  if(strcmp(embedded.filename,adjacent.filename) || strcmp(embedded.name,adjacent.name) ||
     embedded.game_id!=adjacent.game_id) return 0;
  content_log(router,ANYGM_CONTENT_LOG_INFO,
              "executable: the embedded payload carries no code and the adjacent payload is the "
              "same project (%s, game id %u)",adjacent.name,(unsigned)adjacent.game_id);
  return 1;
}

static AnygmContentResolveResult resolve_executable_content(
    const AnygmContentRouter *router,const char *path,char *content_path,size_t content_size,
    char *asset_root,size_t asset_root_size){
  int embedded=load_studio_executable_content(router,path,content_path,content_size);
  if(embedded>0){
    char extracted[1024];
    if(snprintf(extracted,sizeof extracted,"%s",content_path)<(int)sizeof extracted &&
       embedded_payload_defers_to_adjacent(router,path,extracted)){
      uint64_t executable_size=0;
      file_size64(router,path,&executable_size);
      if(resolve_adjacent_studio_payload(router,path,executable_size,content_path,content_size)==
         ANYGM_CONTENT_RESOLVE_OK) return select_payload_configuration(router,content_path)
           ?ANYGM_CONTENT_RESOLVE_OK:ANYGM_CONTENT_RESOLVE_INVALID;
      snprintf(content_path,content_size,"%s",extracted);
    }
    return select_payload_configuration(router,path)?ANYGM_CONTENT_RESOLVE_OK:ANYGM_CONTENT_RESOLVE_INVALID;
  }
  if(embedded) return ANYGM_CONTENT_RESOLVE_INVALID;
  AnygmEmbeddedCab cab={0};
  AnygmEmbeddedCabStatus status=anygm_embedded_cab_probe(router,path,&cab);
  AnygmEmbeddedNsis nsis={0};
  AnygmEmbeddedNsisStatus nsis_status=status==ANYGM_EMBEDDED_CAB_NOT_FOUND
    ? anygm_embedded_nsis_probe(router,path,&nsis):ANYGM_EMBEDDED_NSIS_NOT_FOUND;
  uint64_t source_size=0;
  file_size64(router,path,&source_size);
  int classic_candidate=status!=ANYGM_EMBEDDED_CAB_NOT_FOUND
    ? classic_executable_maybe(router,path,source_size,&cab)
    : (nsis_status==ANYGM_EMBEDDED_NSIS_NOT_FOUND
       ? classic_executable_maybe(router,path,source_size,NULL):0);
  if((status==ANYGM_EMBEDDED_CAB_NOT_FOUND &&
      nsis_status==ANYGM_EMBEDDED_NSIS_NOT_FOUND) || classic_candidate){
    if(!select_payload_configuration(router,path)) return ANYGM_CONTENT_RESOLVE_INVALID;
    if(load_classic_project_content(router,path,content_path,content_size))
      return ANYGM_CONTENT_RESOLVE_OK;
    if(classic_candidate) return ANYGM_CONTENT_RESOLVE_INVALID;
  }
  if(status==ANYGM_EMBEDDED_CAB_UNSUPPORTED){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,
                "cabinet: structurally valid executable uses an unsupported Cabinet profile");
    return ANYGM_CONTENT_RESOLVE_UNSUPPORTED;
  }
  if(status==ANYGM_EMBEDDED_CAB_NOT_FOUND)
  {
    if(nsis_status==ANYGM_EMBEDDED_NSIS_UNSUPPORTED){
      content_log(router,ANYGM_CONTENT_LOG_ERROR,
                  "nsis: structurally valid executable uses an unsupported NSIS profile");
      return ANYGM_CONTENT_RESOLVE_UNSUPPORTED;
    }
    if(nsis_status==ANYGM_EMBEDDED_NSIS_SUPPORTED){
      if(!select_payload_configuration(router,path)) return ANYGM_CONTENT_RESOLVE_INVALID;
      if(!anygm_embedded_nsis_extract(router,path,&nsis,content_path,content_size,
                                      asset_root,asset_root_size))
        return ANYGM_CONTENT_RESOLVE_INVALID;
      return ANYGM_CONTENT_RESOLVE_OK;
    }
    if(nsis_status==ANYGM_EMBEDDED_NSIS_INVALID) return ANYGM_CONTENT_RESOLVE_INVALID;
    AnygmContentResolveResult adjacent=resolve_adjacent_studio_payload(
      router,path,source_size,content_path,content_size);
    if(adjacent==ANYGM_CONTENT_RESOLVE_OK && !select_payload_configuration(router,content_path))
      return ANYGM_CONTENT_RESOLVE_INVALID;
    return adjacent;
  }
  if(status!=ANYGM_EMBEDDED_CAB_SUPPORTED) return ANYGM_CONTENT_RESOLVE_INVALID;
  if(!select_payload_configuration(router,path)) return ANYGM_CONTENT_RESOLVE_INVALID;
  if(!anygm_embedded_cab_extract(router,path,&cab,content_path,content_size,
                                 asset_root,asset_root_size))
    return ANYGM_CONTENT_RESOLVE_INVALID;
  return ANYGM_CONTENT_RESOLVE_OK;
}

static int file_magic_kind(const AnygmContentRouter *router,const char *path){
  uint8_t b[4];
  if(!router || !anygm_vfs_can_read(router->host)) return 0;
  void *file=router->host->file_open(router->host->userdata,path,ANYGM_FILE_READ);
  if(!file) return 0;
  size_t n=router->host->file_read(router->host->userdata,file,b,sizeof b);
  router->host->file_close(router->host->userdata,file);
  if(n==4 && !memcmp(b,"FORM",4)) return 1;
  if(n==4 && b[0]=='P' && b[1]=='K' &&
     ((b[2]==3 && b[3]==4)||(b[2]==5 && b[3]==6)||(b[2]==7 && b[3]==8))) return 2;
  return 0;
}
static int zip_name_normalize(char *name,size_t raw_size){
  if(!name || !raw_size || raw_size>ANYGM_CONTENT_MAX_MEMBER_PATH ||
     memchr(name,0,raw_size)) return 0;
  name[raw_size]=0;
  for(size_t i=0;i<raw_size;i++){
    unsigned char c=(unsigned char)name[i];
    if(c=='\\') name[i]='/';
    else if(c<0x20u || c==0x7fu || c==':') return 0;
  }
  if(name[0]=='/') return 0;
  size_t segment=0;
  for(size_t i=0;i<=raw_size;i++){
    if(i<raw_size && name[i]!='/') continue;
    size_t length=i-segment;
    int final_directory=(i==raw_size && i>0 && name[i-1]=='/');
    if((!length && !final_directory) ||
       (length==1 && name[segment]=='.') ||
       (length==2 && name[segment]=='.' && name[segment+1]=='.')) return 0;
    segment=i+1;
  }
  return 1;
}

typedef struct ZipSeenNames {
  char **slots;
  size_t capacity;
} ZipSeenNames;

static uint64_t zip_name_hash_folded(const char *name){
  uint64_t hash=UINT64_C(1469598103934665603);
  size_t size=strlen(name);
  if(size && name[size-1]=='/') size--;
  for(size_t i=0;i<size;i++){
    unsigned char c=(unsigned char)name[i];
    if(c>='A' && c<='Z') c=(unsigned char)(c-'A'+'a');
    hash^=c;
    hash*=UINT64_C(1099511628211);
  }
  return hash;
}

static int zip_name_equal_folded(const char *left,const char *right){
  size_t left_size=strlen(left),right_size=strlen(right);
  if(left_size&&left[left_size-1]=='/') left_size--;
  if(right_size&&right[right_size-1]=='/') right_size--;
  if(left_size!=right_size) return 0;
  for(size_t i=0;i<left_size;i++){
    unsigned char a=(unsigned char)left[i],b=(unsigned char)right[i];
    if(a>='A'&&a<='Z') a=(unsigned char)(a-'A'+'a');
    if(b>='A'&&b<='Z') b=(unsigned char)(b-'A'+'a');
    if(a!=b) return 0;
  }
  return 1;
}

static int zip_seen_names_init(ZipSeenNames *seen,size_t entries){
  memset(seen,0,sizeof *seen);
  size_t capacity=1;
  while(capacity<entries*2u){
    if(capacity>SIZE_MAX/2u) return 0;
    capacity*=2u;
  }
  seen->slots=calloc(capacity,sizeof *seen->slots);
  if(!seen->slots) return 0;
  seen->capacity=capacity;
  return 1;
}

static void zip_seen_names_free(ZipSeenNames *seen){
  if(!seen) return;
  for(size_t i=0;i<seen->capacity;i++) free(seen->slots[i]);
  free(seen->slots);
  memset(seen,0,sizeof *seen);
}

static int zip_seen_names_add(ZipSeenNames *seen,const char *name){
  if(!seen || !seen->capacity || !name) return 0;
  size_t slot=(size_t)zip_name_hash_folded(name)&(seen->capacity-1u);
  for(size_t probes=0;probes<seen->capacity;probes++){
    if(!seen->slots[slot]){
      seen->slots[slot]=strdup(name);
      return seen->slots[slot]!=NULL;
    }
    if(zip_name_equal_folded(seen->slots[slot],name)) return 0;
    slot=(slot+1u)&(seen->capacity-1u);
  }
  return 0;
}
static const char *zip_basename(const char *name){ const char *p=strrchr(name,'/'); return p?p+1:name; }
static int zip_endswith(const char *name,const char *suffix){
  size_t n=strlen(name),s=strlen(suffix); return n>=s && !strcasecmp(name+n-s,suffix);
}
static int zip_content_score(const char *name){
  const char *base=zip_basename(name);
  int score=0;
  if(!strcasecmp(base,"data.win")) score=400;
  else if(!strcasecmp(base,"game.droid")) score=300;
  /* The Linux export name, ranked with its Android sibling: both are the platform's spelling of
   * data.win, and neither is more authoritative than the other when an archive carries only one. */
  else if(!strcasecmp(base,"game.unx")) score=300;
  else if(!strcasecmp(base,"data.alternate.win")) score=200;
  else if(zip_endswith(base,".win")) score=100;
  if(score){ for(const char *p=name;*p;p++) if(*p=='/') score--; }
  return score;
}
static int zip_nested_score(const char *name){
  int score=zip_endswith(name,".port")?300:zip_endswith(name,".apk")?200:zip_endswith(name,".zip")?100:0;
  if(score){ for(const char *p=name;*p;p++) if(*p=='/') score--; }
  return score;
}
/* A classic source is the last payload an archive can offer. It is only consulted when the archive
 * carries neither a compiled payload nor a nested archive, because an executable sitting beside a
 * real payload is a launcher or a tool rather than the game. Declared project revisions outrank an
 * executable, which is the least explicit of the classic containers. */
static int zip_classic_score(const char *name){
  const char *base=zip_basename(name);
  int score=zip_endswith(base,".gmk")?500:zip_endswith(base,".gm81")?400:
            zip_endswith(base,".gm6")?300:zip_endswith(base,".gmd")?200:
            zip_endswith(base,".exe")?100:0;
  if(score){ for(const char *p=name;*p;p++) if(*p=='/') score--; }
  return score;
}
static int zip_under_root(const char *name,const char *selected){
  const char *slash=strrchr(selected,'/');
  if(!slash) return 1;
  size_t root=(size_t)(slash-selected)+1;
  return !strncasecmp(name,selected,root);
}
/* ---- Anchor files (.anygm) ----
 * A text file names a payload relative to the anchor's own directory and provides
 * an alias without renaming that payload. Normalize the reference as an archive
 * member, confine it to the anchor's subtree, and reject another anchor target
 * so resolution cannot recurse.
 *
 * The basic form is one significant line holding the reference. The advanced form opens with the
 * exact header line "[anygm]", selects the payload with one "payload=<reference>" key, and may
 * carry an "[overrides]" section whose lines are handed verbatim to the engine's override
 * channel. Blank lines and lines whose first significant character is '#' are comments in both
 * forms. Structure is validated here; directive grammar belongs to the engine and is validated
 * at load. When several anchors take part in one resolution (an anchor selecting an archive that
 * carries its own), the outermost declarations have higher priority for matching destinations. */
#define ANYGM_CONTENT_ANCHOR_MEMBER_COPY ".anygm_anchor"
static int anchor_next_line(const uint8_t *data,size_t size,size_t *cursor,
                            const uint8_t **line,size_t *length){
  while(*cursor<size){
    size_t begin=*cursor,end=begin;
    while(end<size && data[end]!='\n' && data[end]!='\r') end++;
    size_t next=end;
    if(next<size && data[next]=='\r') next++;
    if(next<size && data[next]=='\n') next++;
    *cursor=next;
    while(begin<end && (data[begin]==' '||data[begin]=='\t')) begin++;
    while(end>begin && (data[end-1]==' '||data[end-1]=='\t')) end--;
    if(begin==end || data[begin]=='#' || data[begin]==';') continue;
    *line=data+begin;
    *length=end-begin;
    return 1;
  }
  return 0;
}
static int anchor_reference_normalize(const uint8_t *line,size_t length,char *out,size_t outsz){
  if(!length || length>ANYGM_CONTENT_MAX_MEMBER_PATH || length+1>outsz) return 0;
  memcpy(out,line,length);
  if(!zip_name_normalize(out,length)) return 0;
  if(out[strlen(out)-1]=='/') return 0;
  if(zip_endswith(out,".anygm")) return 0;
  return 1;
}
/* overrides may be NULL when the caller has no channel to hand directives to; the payload
 * selection still resolves and the directives are dropped. The engine always supplies one. */
static int anchor_parse(const uint8_t *data,size_t size,char *reference,size_t refsz,
                        char *overrides,size_t overrides_size){
  if(overrides && overrides_size) overrides[0]=0;
  if(!data || !size || size>ANYGM_CONTENT_MAX_ANCHOR_FILE_BYTES) return 0;
  size_t cursor=0;
  if(size>=3 && data[0]==0xefu && data[1]==0xbbu && data[2]==0xbfu) cursor=3;
  const uint8_t *line=NULL;
  size_t length=0;
  if(!anchor_next_line(data,size,&cursor,&line,&length)) return 0;
  if(!(length==7 && !memcmp(line,"[anygm]",7))){
    if(!anchor_reference_normalize(line,length,reference,refsz)) return 0;
    return !anchor_next_line(data,size,&cursor,&line,&length);
  }
  int section=0,have_payload=0,have_overrides=0,have_transforms=0,have_pipelines=0;
  size_t used=0;
  while(anchor_next_line(data,size,&cursor,&line,&length)){
    if(line[0]=='['){
      if(length==11 && !memcmp(line,"[overrides]",11) && !have_overrides){
        section=1; have_overrides=1; continue;
      }
      if(length==12 && !memcmp(line,"[transforms]",12) && !have_transforms){
        section=2; have_transforms=1; continue;
      }
      if(length==11 && !memcmp(line,"[pipelines]",11) && !have_pipelines){
        section=3; have_pipelines=1; continue;
      }
      return 0;
    }
    if(section==0){
      if(have_payload || length<7 || memcmp(line,"payload",7)) return 0;
      const uint8_t *value=line+7;
      size_t value_length=length-7;
      while(value_length && (value[0]==' '||value[0]=='\t')){ value++; value_length--; }
      if(!value_length || value[0]!='=') return 0;
      value++; value_length--;
      while(value_length && (value[0]==' '||value[0]=='\t')){ value++; value_length--; }
      if(!anchor_reference_normalize(value,value_length,reference,refsz)) return 0;
      have_payload=1;
    } else if(section==2){
      /* Skip the complete compiled function. INI-looking text inside a C comment
       * is source text, never an anchor section or an override directive. */
      const uint8_t *equal=(const uint8_t*)memchr(line,'=',length);
      if(!equal) return 0;
      size_t source_at=(size_t)(equal+1-data),consumed=0,program_size=0,parameter_size=0;
      uint8_t *program=NULL,*parameters=NULL;
      int valid=anygm_content_transform_compile((const char*)data+source_at,size-source_at,&consumed,
        &program,&program_size,&parameters,&parameter_size,NULL,0);
      free(program); free(parameters);
      if(!valid) return 0;
      cursor=source_at+consumed;
    } else if(section==1 && overrides){
      if(overrides_size-used<length+2u) return 0;
      memcpy(overrides+used,line,length);
      used+=length;
      overrides[used++]='\n';
      overrides[used]=0;
    }
  }
  if(!have_payload) return 0;
  if(have_transforms || have_pipelines){
    AnygmContentTransforms *validation=anygm_content_transforms_create();
    int valid=validation && anygm_content_transforms_parse(validation,data,size,NULL,0);
    anygm_content_transforms_destroy(validation);
    if(!valid) return 0;
  }
  return 1;
}

/* Declarations are interpreted only during an owned resolution transaction. */
static int anchor_apply_transforms(const AnygmContentRouter *router,
                                   const uint8_t *data,size_t size,unsigned priority){
  char error[256]={0};
  if(!router || !router->transforms ||
     !anygm_content_transforms_parse_layer(router->transforms,data,size,priority,error,sizeof error)){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"%s",
                error[0]?error:"anchor: transform context is unavailable");
    return 0;
  }
  if(router->configuration && router->configuration->base_transforms &&
     !anygm_content_transforms_parse_layer(router->configuration->base_transforms,
       data,size,priority,error,sizeof error)){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"%s",error);
    return 0;
  }
  return 1;
}

static int anchor_apply_configuration(const AnygmContentRouter *router,const uint8_t *data,
                                       size_t size,unsigned priority){
  if(!router->configuration || priority>=CONFIG_ANCHOR_LAYERS) return 0;
  char reference[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
  int ok=anchor_parse(data,size,reference,sizeof reference,
    router->configuration->anchors[priority],ANYGM_CONTENT_MAX_ANCHOR_BYTES) &&
    anchor_apply_transforms(router,data,size,priority);
  if(ok) content_log(router,ANYGM_CONTENT_LOG_INFO,"configuration: applied anchor layer %u",priority);
  return ok;
}

static uint32_t zip_crc32(const uint8_t *data,size_t size){
  uint32_t crc=UINT32_MAX;
  for(size_t i=0;i<size;i++){
    crc^=data[i];
    for(unsigned bit=0;bit<8;bit++)
      crc=(crc>>1)^((crc&1u)?UINT32_C(0xedb88320):0u);
  }
  return ~crc;
}
static int zip_write_stored(const AnygmContentRouter *router,const char *path,
                            const uint8_t *src,size_t size,uint32_t expected_crc){
  if(zip_crc32(src,size)!=expected_crc) return 0;
  int ok=router&&anygm_vfs_write_all(router->host,path,src,size);
  if(!ok&&router) anygm_vfs_remove(router->host,path);
  return ok;
}
static int zip_write_deflated(const AnygmContentRouter *router,const char *path,
                              const uint8_t *src,uint32_t csz,uint32_t usz,
                              uint32_t expected_crc){
  if(csz>(uint32_t)INT_MAX || usz>(uint32_t)INT_MAX) return 0;
  if(!usz) return zip_write_stored(router,path,src,0,expected_crc);
  uint8_t *dst=malloc(usz);
  if(!dst) return 0;
  size_t got=0;
  int ok=gml_deflate_decode_to_buffer(src,csz,GML_DEFLATE_RAW,dst,usz,&got)&&
         got==(size_t)usz&&zip_crc32(dst,usz)==expected_crc&&
         anygm_vfs_write_all(router->host,path,dst,usz);
  free(dst);
  if(!ok) anygm_vfs_remove(router->host,path);
  return ok;
}

static int load_classic_project_content(const AnygmContentRouter *router,const char *srcpath,
                                        char *content_path,size_t cpsz);

/* Extract a source project (project_rel != NULL) in full, or only the selected game payload
 * subtree. nested_rel receives a fallback archive when this level has no direct payload.
 * content_is_classic reports that the selected payload is a source container to import rather
 * than a compiled payload to load. */
static int zip_extract_all(const AnygmContentRouter *router,const char *zpath,const char *outdir,
                           char *content_rel,size_t crsz,char *project_rel,size_t prsz,
                           char *nested_rel,size_t nrsz,int *content_is_classic){
  GmlFileMap map;
  if(!file_map_readonly(router,zpath,&map) || map.size<22){ file_map_close(&map); return 0; }
  const uint8_t *zd=map.data; size_t fsz=map.size;
  size_t lo=fsz>22+65536?fsz-22-65536:0, eocd=SIZE_MAX;
  for(size_t i=fsz-22;;i--){
    if(zu32(zd+i)==0x06054b50u){ eocd=i; break; }
    if(i==lo) break;
  }
  if(eocd==SIZE_MAX || eocd+22>fsz){ file_map_close(&map); return 0; }
  unsigned n_ent=zu16(zd+eocd+10);
  unsigned n_ent_disk=zu16(zd+eocd+8);
  uint32_t cdir_size=zu32(zd+eocd+12);
  uint32_t cdir=zu32(zd+eocd+16);
  if(zu16(zd+eocd+4)!=0 || zu16(zd+eocd+6)!=0 || n_ent!=n_ent_disk ||
     n_ent==0xffffu || n_ent>ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES ||
     cdir==0xffffffffu || cdir_size==0xffffffffu || (size_t)cdir>eocd ||
     (size_t)cdir_size>eocd-(size_t)cdir){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"archive: ZIP64 is not supported: %s",zpath);
    file_map_close(&map); return 0;
  }
  ZipSeenNames seen;
  memset(&seen,0,sizeof seen);
  int content_best=0,nested_best=0,classic_best=0,anchor_best=0;
  char classic_rel[ANYGM_CONTENT_MAX_MEMBER_PATH+1u]="";
  char anchor_rel[ANYGM_CONTENT_MAX_MEMBER_PATH+1u]="";
  uint32_t anchor_crc=0,anchor_csz=0,anchor_usz=0,anchor_lho=0;
  unsigned anchor_flags=0,anchor_method=0;
  size_t p=cdir;
  if(content_rel && crsz) content_rel[0]=0;
  if(project_rel && prsz) project_rel[0]=0;
  if(nested_rel && nrsz) nested_rel[0]=0;
  if(content_is_classic) *content_is_classic=0;
  /* Pass 1: choose deterministic payloads without allocating an entry table. */
  unsigned scanned=0;
  int valid=1;
  for(;scanned<n_ent;scanned++){
    if(p>fsz || fsz-p<46 || zu32(zd+p)!=0x02014b50u){ valid=0; break; }
    unsigned nlen=zu16(zd+p+28),elen=zu16(zd+p+30),clen=zu16(zd+p+32);
    size_t next=p+46u+nlen+elen+clen;
    if(next<p || next>fsz || !nlen || nlen>ANYGM_CONTENT_MAX_MEMBER_PATH){ valid=0; break; }
    char name[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
    memcpy(name,zd+p+46,nlen);
    if(!zip_name_normalize(name,nlen)){ valid=0; break; }
    int cs=content_rel?zip_content_score(name):0;
    if(cs>content_best){ snprintf(content_rel,crsz,"%s",name); content_best=cs; }
    if(project_rel && !project_rel[0] && zip_endswith(name,".yyp")) snprintf(project_rel,prsz,"%s",name);
    int ns=nested_rel?zip_nested_score(name):0;
    if(ns>nested_best){ snprintf(nested_rel,nrsz,"%s",name); nested_best=ns; }
    int ks=(content_rel && content_is_classic)?zip_classic_score(name):0;
    if(ks>classic_best){ snprintf(classic_rel,sizeof classic_rel,"%s",name); classic_best=ks; }
    if(content_rel && name[strlen(name)-1]!='/' && zip_endswith(name,".anygm")){
      int as=1000;
      for(const char *q=name;*q;q++) if(*q=='/') as--;
      if(as>anchor_best){
        anchor_best=as;
        snprintf(anchor_rel,sizeof anchor_rel,"%s",name);
        anchor_flags=zu16(zd+p+8);
        anchor_method=zu16(zd+p+10);
        anchor_crc=zu32(zd+p+16);
        anchor_csz=zu32(zd+p+20);
        anchor_usz=zu32(zd+p+24);
        anchor_lho=zu32(zd+p+42);
      }
    }
    p=next;
  }
  /* An anchor member overrides scored selection: it is the archive stating which payload it
   * carries. A present anchor that cannot be read, parsed, or matched to a member rejects the
   * archive instead of quietly losing to the scores, because silently loading something other
   * than what the archive declared is worse than not loading it. */
  if(valid && content_rel && anchor_best){
    uint8_t anchor_data[ANYGM_CONTENT_MAX_ANCHOR_FILE_BYTES];
    char reference[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
    char target[ANYGM_CONTENT_MAX_MEMBER_PATH+1u]="";
    int anchor_ok=0;
    if(!(anchor_flags&1u) && anchor_usz && anchor_usz<=sizeof anchor_data &&
       (uint64_t)anchor_lho+30u<=fsz && zu32(zd+anchor_lho)==0x04034b50u){
      unsigned lnl=zu16(zd+anchor_lho+26),lel=zu16(zd+anchor_lho+28);
      uint64_t doff=(uint64_t)anchor_lho+30u+lnl+lel;
      if(doff<=fsz && (uint64_t)anchor_csz<=(uint64_t)fsz-doff){
        size_t got=0;
        if(anchor_method==0 && anchor_csz==anchor_usz){
          memcpy(anchor_data,zd+(size_t)doff,anchor_usz);
          anchor_ok=1;
        } else if(anchor_method==8){
          anchor_ok=gml_deflate_decode_to_buffer(zd+(size_t)doff,anchor_csz,GML_DEFLATE_RAW,
                                                 anchor_data,anchor_usz,&got) &&
                    got==(size_t)anchor_usz;
        }
        if(anchor_ok) anchor_ok=zip_crc32(anchor_data,anchor_usz)==anchor_crc;
      }
    }
    if(anchor_ok)
      anchor_ok=anchor_parse(anchor_data,anchor_usz,reference,sizeof reference,NULL,0);
    if(anchor_ok){
      const char *slash=strrchr(anchor_rel,'/');
      size_t rootlen=slash?(size_t)(slash-anchor_rel)+1u:0u;
      if(rootlen+strlen(reference)>=sizeof target) anchor_ok=0;
      else {
        memcpy(target,anchor_rel,rootlen);
        snprintf(target+rootlen,sizeof target-rootlen,"%s",reference);
      }
    }
    int target_found=0;
    if(anchor_ok){
      size_t q=cdir;
      for(unsigned e2=0;e2<n_ent && !target_found;e2++){
        if(q>fsz || fsz-q<46) break;
        unsigned nl2=zu16(zd+q+28),el2=zu16(zd+q+30),cl2=zu16(zd+q+32);
        char name2[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
        if(!nl2 || nl2>ANYGM_CONTENT_MAX_MEMBER_PATH || 46u+(size_t)nl2>fsz-q) break;
        memcpy(name2,zd+q+46,nl2);
        q+=46u+nl2+el2+cl2;
        if(!zip_name_normalize(name2,nl2)) break;
        if(name2[strlen(name2)-1]=='/') continue;
        if(zip_name_equal_folded(name2,target)){
          snprintf(target,sizeof target,"%s",name2);
          target_found=1;
        }
      }
    }
    if(!anchor_ok || !target_found){
      content_log(router,ANYGM_CONTENT_LOG_ERROR,
                  "archive: anchor %s is invalid or names no member of %s",anchor_rel,zpath);
      file_map_close(&map);
      return 0;
    }
    /* Keep a copy of the anchor beside the extracted payload so a warm cache load can hand the
     * archive's override directives to the engine without reopening the archive. The copy lives
     * in the disposable hash-keyed cache and is parsed as untrusted input on every read. */
    {
      char anchor_copy[1536];
      if(!path_join_bounded(anchor_copy,sizeof anchor_copy,outdir,
                            ANYGM_CONTENT_ANCHOR_MEMBER_COPY) ||
         !anygm_vfs_write_all(router->host,anchor_copy,anchor_data,anchor_usz)){
        content_log(router,ANYGM_CONTENT_LOG_ERROR,
                    "archive: could not stage the anchor copy for %s",zpath);
        file_map_close(&map);
        return 0;
      }
    }
    if(zip_nested_score(target)>0){
      content_rel[0]=0;
      if(nested_rel) snprintf(nested_rel,nrsz,"%s",target);
    } else {
      snprintf(content_rel,crsz,"%s",target);
      if(content_is_classic) *content_is_classic=zip_classic_score(target)>0;
    }
    content_log(router,ANYGM_CONTENT_LOG_INFO,"archive: anchor %s selected %s",
                anchor_rel,target);
  }
  if(valid && content_rel && !content_rel[0] && (!nested_rel || !nested_rel[0]) &&
     classic_rel[0]){
    snprintf(content_rel,crsz,"%s",classic_rel);
    if(content_is_classic) *content_is_classic=1;
  }
  if(!valid || scanned!=n_ent || p!=(size_t)cdir+(size_t)cdir_size ||
     (content_rel && !content_rel[0] && (!nested_rel || !nested_rel[0])) ||
     (project_rel && !project_rel[0])){ file_map_close(&map); return 0; }
  /* Validate case-folded collisions only among members that can be extracted. Android packages
   * commonly contain case-distinct resource names outside the selected payload subtree. Rejecting
   * those unrelated names made an otherwise safe payload unloadable, while checking the selected
   * set here retains the cross-platform overwrite protection. */
  if(!zip_seen_names_init(&seen,n_ent?n_ent:1u)){ file_map_close(&map); return 0; }
  int extracted=0;
  uint64_t total_out=0;
  p=cdir;
  for(unsigned e=0;e<n_ent;e++){
    if(p>fsz || fsz-p<46 || zu32(zd+p)!=0x02014b50u){ valid=0; break; }
    unsigned flags=zu16(zd+p+8),method=zu16(zd+p+10);
    uint32_t expected_crc=zu32(zd+p+16);
    uint32_t csz=zu32(zd+p+20),usz=zu32(zd+p+24);
    unsigned nlen=zu16(zd+p+28),elen=zu16(zd+p+30),clen=zu16(zd+p+32);
    uint32_t attrs=zu32(zd+p+38),lho=zu32(zd+p+42);
    size_t next=p+46u+nlen+elen+clen;
    if(next<p || next>fsz || !nlen || nlen>ANYGM_CONTENT_MAX_MEMBER_PATH){ valid=0; break; }
    char name[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
    memcpy(name,zd+p+46,nlen);
    p=next;
    if(!zip_name_normalize(name,nlen)){ valid=0; break; }
    int wanted=project_rel!=NULL;
    if(!wanted && content_rel && content_rel[0]) wanted=zip_under_root(name,content_rel);
    else if(!wanted && nested_rel && nested_rel[0]) wanted=!strcasecmp(name,nested_rel);
    if(!wanted) continue;
    if(!zip_seen_names_add(&seen,name)){ valid=0; break; }
    if((flags&1u) || ((attrs>>16)&0170000u)==0120000u){ valid=0; break; }
    if((uint64_t)usz>ANYGM_CONTENT_MAX_MEMBER_BYTES ||
       ((uint64_t)usz>ANYGM_CONTENT_EXPANSION_ALLOWANCE &&
        (!csz || (uint64_t)usz>(uint64_t)csz*ANYGM_CONTENT_MAX_EXPANSION_RATIO))){
      valid=0; break;
    }
    char out[1536];
    int out_length=snprintf(out,sizeof out,"%s/%s",outdir,name);
    if(out_length<0 || (size_t)out_length>=sizeof out){ valid=0; break; }
    size_t ol=strlen(out);
    if(ol && out[ol-1]=='/'){ if(!mkdirs_for(router,out,1)){ valid=0; break; } continue; }
    if((uint64_t)usz>ANYGM_CONTENT_MAX_EXTRACTED_BYTES-total_out){ valid=0; break; }
    total_out+=usz;
    if(!mkdirs_for(router,out,0)){ valid=0; break; }
    if((uint64_t)lho+30u>fsz || zu32(zd+lho)!=0x04034b50u){ valid=0; break; }
    unsigned lnl=zu16(zd+lho+26),lel=zu16(zd+lho+28);
    uint64_t doff=(uint64_t)lho+30u+lnl+lel;
    if(doff>fsz || (uint64_t)csz>(uint64_t)fsz-doff){ valid=0; break; }
    int ok=0;
    if(method==0 && csz==usz)
      ok=zip_write_stored(router,out,zd+(size_t)doff,usz,expected_crc);
    else if(method==8)
      ok=zip_write_deflated(router,out,zd+(size_t)doff,csz,usz,expected_crc);
    if(!ok){ valid=0; break; }
    extracted++;
  }
  zip_seen_names_free(&seen);
  file_map_close(&map);
  return valid?extracted:0;
}

unsigned anygm_content_path_hash(const char *s){
  /* One resolved content path has one writable namespace. Anchor resolution and native host
   * paths may spell the same separator differently; canonicalize it before hashing. */
  uint32_t h=2166136261u;
  for(;s&&*s;s++){ uint8_t c=(uint8_t)*s; if(c=='\\') c='/'; h^=c; h*=16777619u; }
  return h;
}
static int sibling_payload(const AnygmContentRouter *router,const char *archive,char *out,size_t outsz){
  char parent[1024]; snprintf(parent,sizeof parent,"%s",archive);
  char *slash=strrchr(parent,'/'),*bs=strrchr(parent,'\\');
  if(bs && (!slash || bs>slash)) slash=bs;
  if(slash) *slash=0; else snprintf(parent,sizeof parent,".");
  static const char *rel[]={"data.win","game.droid","game.unx",
                            "assets/data.win","assets/game.droid","assets/game.unx",
                            "gamedata/data.win","gamedata/game.droid","gamedata/game.unx"};
  for(size_t i=0;i<sizeof rel/sizeof rel[0];i++){
    char candidate[1280]; snprintf(candidate,sizeof candidate,"%s/%s",parent,rel[i]);
    if(file_magic_kind(router,candidate)==1){ snprintf(out,outsz,"%s",candidate); return 1; }
  }
  return 0;
}
static int load_archive_content_depth(const AnygmContentRouter *router,const char *zpath,
                                      char *content_path,size_t cpsz,
                                      char *asset_root,size_t arsz,
                                      char *content_overrides,size_t content_overrides_size,
                                      int depth){
  if(depth>=(int)ANYGM_CONTENT_MAX_NESTING_LEVELS){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"archive: nesting limit reached at %s",zpath);
    return 0;
  }
  const char *base=router?router->cache_directory:NULL;
  char zbuf[1024]; snprintf(zbuf,sizeof zbuf,"%s",zpath);
  const char *slash=strrchr(zbuf,'/'),*bs=strrchr(zbuf,'\\');
  if(bs && (!slash || bs>slash)) slash=bs;
  char stem[192];
  anygm_content_path_stem(zpath,stem,sizeof stem);
  char zdir[1024];
  if(slash){ size_t dl=(size_t)(slash-zbuf); if(dl>=sizeof zdir) dl=sizeof zdir-1;
    memcpy(zdir,zbuf,dl); zdir[dl]=0; } else snprintf(zdir,sizeof zdir,".");
  if(!base) base=zdir;
  uint64_t zsz=0,zhash=0;
  if(!file_size64(router,zpath,&zsz) || !file_hash64(router,zpath,&zhash)) return 0;
  char outdir[1400];
  snprintf(outdir,sizeof outdir,"%s/%s-%016llx-anygm-archive",base,stem,
           (unsigned long long)zhash);
  char marker[1536]; snprintf(marker,sizeof marker,"%s/.anygm_cache",outdir);
  char kind=0,rel[512]="",marker_data[516]="",existing[2048];
  uint64_t marker_payload_hash=0,marker_payload_size=0;
  int cache_ok=cache_marker_read(router,marker,ANYGM_CACHE_ARCHIVE,zhash,zsz,
                                 &marker_payload_hash,&marker_payload_size,
                                 marker_data,sizeof marker_data);
  if(cache_ok && marker_data[1]=='\n' && marker_data[2]){
    kind=marker_data[0];
    size_t relative_size=strlen(marker_data+2);
    if(relative_size>=sizeof rel) cache_ok=0;
    else memcpy(rel,marker_data+2,relative_size+1);
  } else cache_ok=0;
  if(cache_ok){
    snprintf(existing,sizeof existing,"%s/%s",outdir,rel);
    uint64_t existing_size=0,existing_hash=0;
    cache_ok=(kind=='C' || kind=='N' || kind=='S') && file_size64(router,existing,&existing_size) &&
             existing_size==marker_payload_size && file_hash64(router,existing,&existing_hash) &&
             existing_hash==marker_payload_hash;
  }
  char anchor_copy[1536];
  if(!path_join_bounded(anchor_copy,sizeof anchor_copy,outdir,
                        ANYGM_CONTENT_ANCHOR_MEMBER_COPY)) return 0;
  if(!cache_ok){
    char nested[512]=""; rel[0]=0; kind=0;
    int classic_payload=0;
    if(!mkdirs_for(router,outdir,1)) return 0;
    anygm_vfs_remove(router->host,anchor_copy);
    int n=zip_extract_all(router,zpath,outdir,rel,sizeof rel,NULL,0,nested,sizeof nested,
                          &classic_payload);
    if(n<=0 || (!rel[0] && !nested[0])){
      if(sibling_payload(router,zpath,content_path,cpsz)){
        content_log(router,ANYGM_CONTENT_LOG_INFO,"archive: using sibling payload %s",content_path); return 1;
      }
      content_log(router,ANYGM_CONTENT_LOG_ERROR,"archive: no supported payload or nested archive in %s",zpath);
      return 0;
    }
    if(rel[0]) kind=classic_payload?'S':'C'; else { kind='N'; snprintf(rel,sizeof rel,"%s",nested); }
    snprintf(marker_data,sizeof marker_data,"%c\n%s",kind,rel);
    char selected[2048];
    uint64_t selected_size=0,selected_hash=0;
    snprintf(selected,sizeof selected,"%s/%s",outdir,rel);
    if(!file_size64(router,selected,&selected_size) || !file_hash64(router,selected,&selected_hash) ||
       !cache_marker_write(router,marker,ANYGM_CACHE_ARCHIVE,zhash,zsz,
                           selected_hash,selected_size,marker_data))
      content_log(router,ANYGM_CONTENT_LOG_WARN,"archive: could not publish cache marker");
    content_log(router,ANYGM_CONTENT_LOG_INFO,"archive: extracted %d files to %s (%c: %s)",n,outdir,kind,rel);
  } else content_log(router,ANYGM_CONTENT_LOG_INFO,"archive: reusing extracted copy at %s",outdir);
  char resolved[1536];
  if(!path_join_bounded(resolved,sizeof resolved,outdir,rel)){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"archive: resolved payload path is too long");
    return 0;
  }
  /* Collect this archive's declared layer before nested resolution, retaining its
   * priority. The staged copy is untrusted input reparsed on every load; a copy
   * that no longer parses is a corrupted cache entry and rejects the load rather than dropping
   * directives the archive declared. */
  {
    AnygmFileInfo anchor_info;
    if(anygm_vfs_stat(router->host,anchor_copy,&anchor_info) &&
       (anchor_info.flags&ANYGM_FILE_INFO_EXISTS) &&
       !(anchor_info.flags&ANYGM_FILE_INFO_DIRECTORY)){
      uint8_t *anchor_bytes=NULL;
      size_t anchor_size=0;
      int parsed=anygm_vfs_read_all(router->host,anchor_copy,&anchor_bytes,&anchor_size,
                                    (size_t)ANYGM_CONTENT_MAX_ANCHOR_FILE_BYTES);
      if(parsed) parsed=anchor_apply_configuration(router,anchor_bytes,anchor_size,
        ANYGM_CONTENT_MAX_NESTING_LEVELS+1u-(unsigned)depth);
      free(anchor_bytes);
      if(!parsed){
        content_log(router,ANYGM_CONTENT_LOG_ERROR,
                    "archive: staged anchor copy is corrupt: %s",anchor_copy);
        return 0;
      }
    }
  }
  int magic=file_magic_kind(router,resolved);
  if(kind=='N' || (kind!='S' && magic==2))
    return load_archive_content_depth(router,resolved,content_path,cpsz,asset_root,arsz,
                                      content_overrides,content_overrides_size,depth+1);
  if(kind=='S'){
    /* The generated payload lands in the cache, so the extracted tree stays the asset root the
     * content opens by path. Route executable members through the same Classic/Cabinet boundary
     * as a directly selected path. */
    int resolved_ok=0;
    int adapted=load_input_pipeline(router,resolved,content_path,cpsz,asset_root,arsz,
                                     content_overrides,content_overrides_size,depth+1);
    if(adapted<0) return 0;
    if(adapted>0) resolved_ok=1;
    else if(path_ext_is(resolved,".exe")){
      AnygmContentResolveResult executable=resolve_executable_content(
        router,resolved,content_path,cpsz,asset_root,arsz);
      if(executable==ANYGM_CONTENT_RESOLVE_UNSUPPORTED) return executable;
      resolved_ok=executable==ANYGM_CONTENT_RESOLVE_OK;
    } else resolved_ok=select_payload_configuration(router,resolved) &&
                         load_classic_project_content(router,resolved,content_path,cpsz);
    if(resolved_ok && asset_root && arsz && !asset_root[0])
      snprintf(asset_root,arsz,"%s",outdir);
    return resolved_ok;
  }
  if(kind!='C') return 0;
  int adapted=load_input_pipeline(router,resolved,content_path,cpsz,asset_root,arsz,
                                   content_overrides,content_overrides_size,depth+1);
  if(adapted) return adapted>0;
  if(magic!=1) return 0;
  if(!select_payload_configuration(router,resolved)) return 0;
  snprintf(content_path,cpsz,"%s",resolved);
  return 1;
}
static int load_archive_content(const AnygmContentRouter *router,const char *zpath,
                                char *content_path,size_t cpsz,char *asset_root,size_t arsz,
                                char *content_overrides,size_t content_overrides_size){
  return load_archive_content_depth(router,zpath,content_path,cpsz,asset_root,arsz,
                                    content_overrides,content_overrides_size,0);
}

/* A selected source adapter returns a supported byte representation, never an
 * executable callback. It runs once before routing its result. A byte-identical
 * result declines adaptation and leaves ordinary member/adjacent-file selection
 * unchanged. Original paths remain authoritative for assets and fidelity data. */
static int load_input_pipeline(const AnygmContentRouter *router,const char *path,
                                char *content_path,size_t content_size,
                                char *asset_root,size_t asset_root_size,
                                char *overrides,size_t overrides_size,int depth){
  ContentConfigResolution *config=router->configuration;
  if(!config || config->input_adapted ||
     (!anygm_content_transforms_has(router->transforms,"input") &&
      !anygm_content_config_has_input(config->ini))) return 0;
  AnygmContentTransforms *selected=anygm_content_transforms_create();
  uint8_t *original=NULL,*image=NULL,digest[32];
  size_t original_size=0,image_size=0;
  char specific[ANYGM_CONFIG_OVERRIDE_BYTES]={0},error[256]={0};
  int result=-1;
  if(!selected || !anygm_content_transforms_copy(selected,config->base_transforms?
       config->base_transforms:router->transforms) ||
     !original_file_digest(router,path,digest)) goto done;
  if(!anygm_content_config_apply(config->ini,digest,selected,UINT_MAX,
       specific,sizeof specific,error,sizeof error)) goto done;
  if(!anygm_content_transforms_has(selected,"input")){ result=0; goto done; }
  if(!anygm_vfs_read_all(router->host,path,&original,&original_size,
                         (size_t)ANYGM_TRANSFORM_MAX_INPUT_BYTES)) goto done;
  uint8_t observed_digest[32];
  gml_sha256(original,original_size,observed_digest);
  if(memcmp(digest,observed_digest,32)){
    snprintf(error,sizeof error,"original bytes changed during selection"); goto done;
  }
  if(!anygm_content_transform_run(selected,"input",original,original_size,
       &image,&image_size,error,sizeof error)) goto done;
  if(image_size==original_size && (!image_size || !memcmp(image,original,image_size))){
    result=0; goto done;
  }
  int kind=image_size>=8u && !memcmp(image,"FORM",4)?1:
    image_size>=4u && !memcmp(image,"PK\003\004",4)?2:
    image_size>=8u && zu32(image)==GMLC_CLASSIC_MAGIC?3:
    image_size>=2u && image[0]=='M' && image[1]=='Z'?3:0;
  if(!kind){ snprintf(error,sizeof error,"result is not a supported input representation"); goto done; }
  if(!config->base_transforms){
    config->base_transforms=anygm_content_transforms_create();
    if(!config->base_transforms ||
       !anygm_content_transforms_copy(config->base_transforms,router->transforms)) goto done;
  }
  if(!anygm_content_transforms_copy(router->transforms,selected)) goto done;
  memcpy(config->specific,specific,sizeof specific);
  memcpy(config->original_digest,digest,32); config->input_adapted=1;
  uint8_t identity[96],key[32]; char hex[65];
  memcpy(identity,digest,32); anygm_content_transforms_hash(selected,identity+32);
  gml_sha256(image,image_size,identity+64); gml_sha256(identity,sizeof identity,key);
  for(size_t i=0;i<32;i++) snprintf(hex+i*2,3,"%02x",key[i]);
  char directory[1400],output[1536];
  const char *cache=router->cache_directory?router->cache_directory:"tmp";
  if(snprintf(directory,sizeof directory,"%s/input-%s",cache,hex)>=(int)sizeof directory ||
     !path_join_bounded(output,sizeof output,directory,kind==2?"payload.zip":"data.win") ||
     !anygm_vfs_mkdirs(router->host,directory)) goto done;
  if(kind==3){
    /* Always re-import an adapted project: companion files may have changed.
     * No cache marker claims that the primary image covers those dependencies. */
    GmlcProject project;
    if(!gmlc_classic_project_load_image(selected,&project,router->host,path,image,image_size,
         directory,error,sizeof error)) goto done;
    int ok=gmlc_package_write_structural(&project,output,error,sizeof error);
    gmlc_project_free(&project);
    if(!ok) goto done;
  } else {
    char temporary[1600];
    if(snprintf(temporary,sizeof temporary,"%s.tmp",output)>=(int)sizeof temporary) goto done;
    if(!anygm_vfs_write_all(router->host,temporary,image,image_size) ||
       !anygm_vfs_publish(router->host,temporary,output)){
      anygm_vfs_remove(router->host,temporary);
      goto done;
    }
  }
  if(kind==2){
    if(!load_archive_content_depth(router,output,content_path,content_size,asset_root,asset_root_size,
                                   overrides,overrides_size,depth)) goto done;
  } else {
    if(snprintf(content_path,content_size,"%s",output)>=(int)content_size) goto done;
    if(asset_root && asset_root_size)
      anygm_content_path_parent(path,asset_root,asset_root_size);
  }
  for(size_t i=0;i<32;i++) snprintf(hex+i*2,3,"%02x",digest[i]);
  content_log(router,ANYGM_CONTENT_LOG_INFO,"input pipeline: normalized %s SHA-256 %s",path,hex);
  result=1;
done:
  anygm_content_transforms_destroy(selected); free(original); free(image);
  if(result<0){
    if(content_path && content_size) content_path[0]=0;
    if(asset_root && asset_root_size) asset_root[0]=0;
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"input pipeline: %s",error[0]?error:"source preparation failed");
  }
  return result;
}

static int load_classic_project_content(const AnygmContentRouter *router,const char *srcpath,
                                        char *content_path,size_t cpsz){
  const char *base=router?router->cache_directory:NULL;
  if(!base) base="tmp";
  char stem[256];
  anygm_content_path_stem(srcpath,stem,sizeof stem);
  uint64_t src_hash=0;
  if(!file_hash64(router,srcpath,&src_hash)) return 0;
  uint64_t src_size=0;
  if(!file_size64(router,srcpath,&src_size)) return 0;
  uint8_t transform_hash[32];
  anygm_content_transforms_hash(router->transforms,transform_hash);
  for(size_t i=0;i<sizeof transform_hash;i++){
    src_hash^=transform_hash[i]; src_hash*=UINT64_C(1099511628211);
  }
  char source_dir[4096];
  anygm_content_path_parent(srcpath,source_dir,sizeof(source_dir));
  if(!gmlc_classic_extension_dependency_hash(router->host,source_dir,src_hash,&src_hash)) return 0;
  if(!gmlc_classic_fidelity_dependency_hash(router->host,srcpath,src_hash,&src_hash)) return 0;
  if(!gmlc_classic_included_dependency_hash(router->transforms,router->host,srcpath,src_hash,&src_hash)) return 0;
  char outdir[768];
  snprintf(outdir,sizeof outdir,"%s/%s-%016llx-anygm-classic",base,stem,
           (unsigned long long)src_hash);
  char outwin[900], marker[900];
  snprintf(outwin,sizeof outwin,"%s/data.win",outdir);
  snprintf(marker,sizeof marker,"%s/.anygm_cache",outdir);
  char marker_data[1];
  uint64_t stored_hash=0,stored_size=0,actual_hash=0,actual_size=0;
  if(cache_marker_read(router,marker,ANYGM_CACHE_CLASSIC_PROJECT,src_hash,src_size,
                       &stored_hash,&stored_size,marker_data,sizeof marker_data) &&
     file_size64(router,outwin,&actual_size) && actual_size==stored_size &&
     file_hash64(router,outwin,&actual_hash) && actual_hash==stored_hash){
    snprintf(content_path,cpsz,"%s",outwin);
    content_log(router,ANYGM_CONTENT_LOG_INFO,"classic: reusing cached package at %s",outwin);
    return 1;
  }
  if(!mkdirs_for(router,outdir,1)) return 0;
  GmlcProject project;
  char err[1024]={0};
  if(!gmlc_classic_project_load(router->transforms,&project,router->host,srcpath,outdir,err,sizeof err)){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"classic: %s",err[0]?err:"project load failed");
    return 0;
  }
  int ok=gmlc_package_write_structural(&project,outwin,err,sizeof err);
  gmlc_project_free(&project);
  if(!ok){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"classic: %s",err[0]?err:"package write failed");
    return 0;
  }
  if(file_size64(router,outwin,&actual_size) && file_hash64(router,outwin,&actual_hash) &&
     !cache_marker_write(router,marker,ANYGM_CACHE_CLASSIC_PROJECT,src_hash,src_size,
                         actual_hash,actual_size,NULL))
    content_log(router,ANYGM_CONTENT_LOG_WARN,"classic: could not publish cache marker");
  snprintf(content_path,cpsz,"%s",outwin);
  content_log(router,ANYGM_CONTENT_LOG_INFO,"classic: compiled %s -> %s",srcpath,outwin);
  return 1;
}

static int load_source_project_content(const AnygmContentRouter *router,const char *srcpath,
                                       char *content_path,size_t cpsz){
  const char *base=router?router->cache_directory:NULL;
  if(!base) base="tmp";
  char stem[256];
  anygm_content_path_stem(srcpath,stem,sizeof stem);
  int archive_input=path_ext_is(srcpath,".yyz");
  uint64_t src_hash=0;
  if(!file_hash64(router,srcpath,&src_hash)) return 0;
  uint64_t src_size=0;
  if(!file_size64(router,srcpath,&src_size)) return 0;
  char outdir[768];
  snprintf(outdir,sizeof outdir,"%s/%s-%016llx-anygm-source",base,stem,
           (unsigned long long)src_hash);
  char outwin[900];
  snprintf(outwin,sizeof outwin,"%s/data.win",outdir);
  char marker[900];
  snprintf(marker,sizeof marker,"%s/.anygm_cache",outdir);
  char marker_data[1];
  uint64_t stored_hash=0,stored_size=0,actual_hash=0,actual_size=0;
  if(archive_input &&
     cache_marker_read(router,marker,ANYGM_CACHE_SOURCE_PROJECT,src_hash,src_size,
                       &stored_hash,&stored_size,marker_data,sizeof marker_data) &&
     file_size64(router,outwin,&actual_size) && actual_size==stored_size &&
     file_hash64(router,outwin,&actual_hash) && actual_hash==stored_hash){
    snprintf(content_path,cpsz,"%s",outwin);
    content_log(router,ANYGM_CONTENT_LOG_INFO,"source: reusing cached package at %s",outwin);
    return 1;
  }

  if(!mkdirs_for(router,outdir,1)) return 0;
  char project_path[1024];
  snprintf(project_path,sizeof project_path,"%s",srcpath);
  if(archive_input){
    char rel[512]="";
    int n=zip_extract_all(router,srcpath,outdir,NULL,0,rel,sizeof rel,NULL,0,NULL);
    if(n<=0 || !rel[0]){
      content_log(router,ANYGM_CONTENT_LOG_ERROR,"source: no project manifest found inside %s",srcpath);
      return 0;
    }
    if(!path_join_bounded(project_path,sizeof project_path,outdir,rel)){
      content_log(router,ANYGM_CONTENT_LOG_ERROR,"source: extracted project path is too long");
      return 0;
    }
    content_log(router,ANYGM_CONTENT_LOG_INFO,"source: extracted %d files to %s (project: %s)",n,outdir,rel);
  }

  GmlcProject p;
  char err[1024]={0};
  if(!gmlc_project_load_yyp(&p,router->host,project_path,err,sizeof err)){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"source: %s",err[0]?err:"project load failed");
    return 0;
  }
  int ok=gmlc_package_write_structural(&p,outwin,err,sizeof err);
  gmlc_project_free(&p);
  if(!ok){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"source: %s",err[0]?err:"package write failed");
    return 0;
  }
  if(archive_input){
    if(file_size64(router,outwin,&actual_size) && file_hash64(router,outwin,&actual_hash) &&
       !cache_marker_write(router,marker,ANYGM_CACHE_SOURCE_PROJECT,src_hash,src_size,
                           actual_hash,actual_size,NULL))
      content_log(router,ANYGM_CONTENT_LOG_WARN,"source: could not publish cache marker");
  }
  snprintf(content_path,cpsz,"%s",outwin);
  content_log(router,ANYGM_CONTENT_LOG_INFO,"source: compiled %s -> %s",srcpath,outwin);
  return 1;
}

static AnygmContentResolveResult content_resolve_path_direct(
    const AnygmContentRouter *router,const char *input_path,
    char *resolved_path,size_t resolved_path_size,
    char *asset_root,size_t asset_root_size,
    char *content_overrides,size_t content_overrides_size);

/* A directly loaded anchor resolves its reference against its own directory and routes the
 * result as if that file had been loaded. The parser already refuses a reference to another
 * anchor, so this cannot recurse through itself; every other container keeps its own depth
 * accounting. */
static int load_anchor_content(const AnygmContentRouter *router,const char *anchor_path,
                               char *resolved_path,size_t resolved_path_size,
                               char *asset_root,size_t asset_root_size,
                               char *content_overrides,size_t content_overrides_size){
  uint8_t *bytes=NULL;
  size_t size=0;
  if(!router || !anygm_vfs_read_all(router->host,anchor_path,&bytes,&size,
                                    (size_t)ANYGM_CONTENT_MAX_ANCHOR_FILE_BYTES)){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"anchor: cannot read %s",anchor_path);
    free(bytes);
    return 0;
  }
  char reference[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
  int ok=anchor_parse(bytes,size,reference,sizeof reference,
                      NULL,0);
  if(ok) ok=anchor_apply_configuration(router,bytes,size,ANYGM_CONTENT_MAX_NESTING_LEVELS+2u);
  free(bytes);
  if(!ok){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,
                "anchor: %s is not a valid anchor file",anchor_path);
    return 0;
  }
  char parent[1024],target[1536];
  anygm_content_path_parent(anchor_path,parent,sizeof parent);
  if(!path_join_bounded(target,sizeof target,parent,reference)){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"anchor: referenced path is too long: %s",
                anchor_path);
    return 0;
  }
  AnygmFileInfo info;
  if(!anygm_vfs_stat(router->host,target,&info) || !(info.flags&ANYGM_FILE_INFO_EXISTS) ||
     (info.flags&ANYGM_FILE_INFO_DIRECTORY)){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"anchor: referenced payload is missing: %s",
                target);
    return 0;
  }
  content_log(router,ANYGM_CONTENT_LOG_INFO,"anchor: %s -> %s",anchor_path,target);
  /* A directly selected anchor takes precedence over nearby anchors. */
  return content_resolve_path_direct(router,target,resolved_path,resolved_path_size,
                                     asset_root,asset_root_size,
                                     content_overrides,content_overrides_size);
}

int anygm_content_identity_path(const AnygmContentRouter *router,const char *input_path,
                                char *output,size_t output_size){
  if(!input_path || !output || !output_size) return 0;
  if(!path_ext_is(input_path,".anygm")){
    return snprintf(output,output_size,"%s",input_path)<(int)output_size;
  }
  uint8_t *bytes=NULL;
  size_t size=0;
  if(!router || !anygm_vfs_read_all(router->host,input_path,&bytes,&size,
                                    (size_t)ANYGM_CONTENT_MAX_ANCHOR_FILE_BYTES)){
    free(bytes);
    return 0;
  }
  char reference[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
  int ok=anchor_parse(bytes,size,reference,sizeof reference,NULL,0);
  free(bytes);
  if(!ok) return 0;
  char parent[1024];
  anygm_content_path_parent(input_path,parent,sizeof parent);
  char target[1536];
  if(!path_join_bounded(target,sizeof target,parent,reference)) return 0;
  /* The identity is compared as a string against the path the frontend passes when the payload
   * is loaded directly, so it must be spelled in the same separator flavor. The join and the
   * parsed reference use forward slashes; a frontend that spelled the anchor's own path with
   * backslashes spells the payload's path that way too. */
  if(strchr(input_path,'\\') && !strchr(input_path,'/'))
    for(char *cursor=target;*cursor;cursor++)
      if(*cursor=='/') *cursor='\\';
  return snprintf(output,output_size,"%s",target)<(int)output_size;
}

/* Resolution collects configuration layers in the private transaction. The
 * public entry point emits them in priority order after selecting the payload. */
static AnygmContentResolveResult content_resolve_path_direct(
    const AnygmContentRouter *router,const char *input_path,
    char *resolved_path,size_t resolved_path_size,
    char *asset_root,size_t asset_root_size,
    char *content_overrides,size_t content_overrides_size){
  if(!input_path || !input_path[0] || !resolved_path || resolved_path_size==0) return 0;
  resolved_path[0]=0;
  if(asset_root && asset_root_size) asset_root[0]=0;
  if(!path_ext_is(input_path,".anygm")){
    int adapted=load_input_pipeline(router,input_path,resolved_path,resolved_path_size,
                                     asset_root,asset_root_size,content_overrides,content_overrides_size,0);
    if(adapted) return adapted>0?ANYGM_CONTENT_RESOLVE_OK:ANYGM_CONTENT_RESOLVE_INVALID;
  }
  if(path_ext_is(input_path,".exe")){
    return resolve_executable_content(router,input_path,resolved_path,resolved_path_size,
                                      asset_root,asset_root_size);
  }
  if(path_ext_is(input_path,".gmd") || path_ext_is(input_path,".gmk") ||
     path_ext_is(input_path,".gm81") || path_ext_is(input_path,".gm6") ||
     path_ext_is(input_path,".exe")){
    if(!select_payload_configuration(router,input_path)) return ANYGM_CONTENT_RESOLVE_INVALID;
    return load_classic_project_content(router,input_path,resolved_path,resolved_path_size);
  }
  if(path_ext_is(input_path,".yyp") || path_ext_is(input_path,".yyz")){
    if(!select_payload_configuration(router,input_path)) return ANYGM_CONTENT_RESOLVE_INVALID;
    return load_source_project_content(router,input_path,resolved_path,resolved_path_size);
  }
  if(path_ext_is(input_path,".anygm")){
    return load_anchor_content(router,input_path,resolved_path,resolved_path_size,
                               asset_root,asset_root_size,
                               content_overrides,content_overrides_size);
  }
  if(path_ext_is(input_path,".zip") || path_ext_is(input_path,".port") ||
     path_ext_is(input_path,".apk") || file_magic_kind(router,input_path)==2){
    return load_archive_content(router,input_path,resolved_path,resolved_path_size,
                                asset_root,asset_root_size,
                                content_overrides,content_overrides_size);
  }
  if(!select_payload_configuration(router,input_path) ||
     snprintf(resolved_path,resolved_path_size,"%s",input_path)>=(int)resolved_path_size){
    resolved_path[0]=0;
    return 0;
  }
  return 1;
}

/* Adopt directives only when one unambiguous adjacent anchor is present.
 * A direct or nested anchor retains precedence over this fallback. */
#define ANYGM_CONTENT_SIBLING_ANCHOR_SCAN_MAX 4096u

/* The lone anchor beside the input, or nothing when the directory holds none or several. */
static int sibling_anchor_path(const AnygmContentRouter *router,const char *parent,
                               char *selected,size_t selected_size){
  const struct AnygmHostServices *host=router->host;
  void *directory=host->directory_open(host->userdata,parent);
  if(!directory) return 0;
  unsigned found=0,scanned=0,truncated=0;
  selected[0]=0;
  for(;;){
    AnygmDirectoryEntry entry;
    memset(&entry,0,sizeof entry);
    entry.struct_size=sizeof entry;
    if(host->directory_read(host->userdata,directory,&entry)!=ANYGM_OK) break;
    if(scanned++>=ANYGM_CONTENT_SIBLING_ANCHOR_SCAN_MAX){ truncated=1; break; }
    if(!(entry.flags&ANYGM_FILE_INFO_REGULAR) || !path_ext_is(entry.name,".anygm")) continue;
    if(++found>1u) break;
    if(!path_join_bounded(selected,selected_size,parent,entry.name)) found=0;
  }
  host->directory_close(host->userdata,directory);
  if(found>1u){
    content_log(router,ANYGM_CONTENT_LOG_WARN,
                "anchor: %s holds more than one anchor; adopting no directives",parent);
    return 0;
  }
  /* Report when the scan limit prevents a complete search. */
  if(!found && truncated)
    content_log(router,ANYGM_CONTENT_LOG_WARN,
                "anchor: stopped looking in %s after %u entries",parent,
                ANYGM_CONTENT_SIBLING_ANCHOR_SCAN_MAX);
  return found==1u && selected[0]!=0;
}

static void adopt_sibling_anchor_overrides(const AnygmContentRouter *router,const char *input_path,
                                           char *overrides,size_t overrides_size){
  const struct AnygmHostServices *host=router?router->host:NULL;
  if(!host || !host->directory_open || !host->directory_read || !host->directory_close) return;
  char parent[1024],selected[1536];
  anygm_content_path_parent(input_path,parent,sizeof parent);
  if(!parent[0] || !sibling_anchor_path(router,parent,selected,sizeof selected)) return;
  uint8_t *bytes=NULL;
  size_t size=0;
  if(!anygm_vfs_read_all(router->host,selected,&bytes,&size,
                         (size_t)ANYGM_CONTENT_MAX_ANCHOR_FILE_BYTES)){
    free(bytes);
    return;
  }
  char reference[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
  int parsed=anchor_parse(bytes,size,reference,sizeof reference,overrides,overrides_size);
  free(bytes);
  /* Invalid adjacent anchors do not supply directives. */
  if(!parsed){
    overrides[0]=0;
    content_log(router,ANYGM_CONTENT_LOG_WARN,"anchor: %s is not a valid anchor file",selected);
    return;
  }
  if(overrides[0])
    content_log(router,ANYGM_CONTENT_LOG_INFO,
                "anchor: adopting the directives %s carries for %s",selected,input_path);
}

/* User declarations are selected for one resolution and never inherited by another load. */
static int adopt_sibling_anchor_transforms(const AnygmContentRouter *router,const char *input_path){
  const struct AnygmHostServices *host=router->host;
  if(!input_path || path_ext_is(input_path,".anygm") || !host ||
     !host->directory_open || !host->directory_read || !host->directory_close) return 1;
  char parent[1024],selected[1536];
  anygm_content_path_parent(input_path,parent,sizeof parent);
  if(!parent[0] || !sibling_anchor_path(router,parent,selected,sizeof selected)) return 1;
  uint8_t *bytes=NULL; size_t size=0;
  if(!anygm_vfs_read_all(host,selected,&bytes,&size,ANYGM_CONTENT_MAX_ANCHOR_FILE_BYTES)){
    free(bytes);
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"anchor: cannot read %s",selected);
    return 0;
  }
  size_t cursor=0,length=0;
  const uint8_t *line=NULL;
  int declared=0;
  while(anchor_next_line(bytes,size,&cursor,&line,&length))
    if((length==12 && !memcmp(line,"[transforms]",12)) ||
       (length==11 && !memcmp(line,"[pipelines]",11))){ declared=1; break; }
  char reference[ANYGM_CONTENT_MAX_MEMBER_PATH+1u];
  int ok=!declared || (anchor_parse(bytes,size,reference,sizeof reference,NULL,0) &&
                       anchor_apply_transforms(router,bytes,size,1u));
  free(bytes);
  if(!ok) content_log(router,ANYGM_CONTENT_LOG_ERROR,"anchor: invalid transform declarations in %s",selected);
  return ok;
}

static int load_transform_defaults(const AnygmContentRouter *router){
  if(!router->system_directory || !router->system_directory[0]) return 1;
  char path[1536];
  if(!path_join_bounded(path,sizeof path,router->system_directory,"anygm.ini")){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"transform configuration path is too long");
    return 0;
  }
  AnygmFileInfo info;
  /* Hosts that cannot distinguish absence from inaccessibility supply no defaults. */
  if(!anygm_vfs_stat(router->host,path,&info)) return 1;
  if(!(info.flags&ANYGM_FILE_INFO_EXISTS)) return 1;
  uint8_t *text=NULL; size_t size=0; char error[256]={0};
  int ok=(info.flags&ANYGM_FILE_INFO_REGULAR) &&
    anygm_vfs_read_all(router->host,path,&text,&size,ANYGM_TRANSFORM_MAX_CONFIG_BYTES);
  if(ok){
    router->configuration->ini=anygm_content_config_parse(text,size,error,sizeof error);
    ok=router->configuration->ini && anygm_content_config_apply(router->configuration->ini,
      NULL,router->transforms,0,router->configuration->defaults,
      sizeof router->configuration->defaults,error,sizeof error);
  }
  free(text);
  if(!ok) content_log(router,ANYGM_CONTENT_LOG_ERROR,"configuration %s: %s",path,
                      error[0]?error:"cannot read bounded regular file");
  else content_log(router,ANYGM_CONTENT_LOG_INFO,"configuration: loaded defaults from %s",path);
  return ok;
}

static int original_file_digest(const AnygmContentRouter *router,const char *path,uint8_t digest[32]){
  uint64_t size=0;
  if(!file_size64(router,path,&size) || size>UINT64_C(2147483648)) return 0;
  void *file=router->host->file_open(router->host->userdata,path,ANYGM_FILE_READ);
  if(!file) return 0;
  GmlSha256 hash;
  gml_sha256_init(&hash);
  uint8_t buffer[65536];
  uint64_t remaining=size;
  int ok=1;
  while(remaining){
    size_t take=remaining<sizeof buffer?(size_t)remaining:sizeof buffer;
    size_t got=router->host->file_read(router->host->userdata,file,buffer,take);
    if(!got || got>take){ ok=0; break; }
    gml_sha256_update(&hash,buffer,got); remaining-=got;
  }
  if(ok && router->host->file_read(router->host->userdata,file,buffer,1)!=0) ok=0;
  router->host->file_close(router->host->userdata,file);
  if(!ok){ content_log(router,ANYGM_CONTENT_LOG_ERROR,"configuration: incomplete hash read of %s",path); return 0; }
  gml_sha256_final(&hash,digest);
  return 1;
}

static int select_payload_configuration(const AnygmContentRouter *router,const char *path){
  ContentConfigResolution *config=router->configuration;
  if(!config || !config->ini) return 1;
  if(config->input_adapted)
    return select_payload_digest(router,config->original_digest,"original pipeline input");
  uint8_t digest[32];
  return original_file_digest(router,path,digest) && select_payload_digest(router,digest,path);
}

static int select_payload_digest(const AnygmContentRouter *router,const uint8_t digest[32],const char *label){
  ContentConfigResolution *config=router->configuration;
  if(!config || !config->ini) return 1;
  if(config->input_adapted) digest=config->original_digest;
  /* Executable routing may establish that its real content is an adjacent data
   * image. Reselect from the same defaults/anchors, never from the first hash. */
  if(!config->base_transforms){
    config->base_transforms=anygm_content_transforms_create();
    if(!config->base_transforms || !anygm_content_transforms_copy(config->base_transforms,
       router->transforms)) return 0;
  } else if(!anygm_content_transforms_copy(router->transforms,config->base_transforms)) return 0;
  char error[256]={0},hex[65];
  for(size_t i=0;i<32;i++) snprintf(hex+i*2,3,"%02x",digest[i]);
  int ok=anygm_content_config_apply(config->ini,digest,router->transforms,UINT_MAX,
    config->specific,sizeof config->specific,error,sizeof error);
  content_log(router,ok?ANYGM_CONTENT_LOG_INFO:ANYGM_CONTENT_LOG_ERROR,
    "configuration: payload %s SHA-256 %s%s%s",label,hex,ok?"":" rejected: ",ok?"":error);
  return ok;
}

static int append_override_text(char *out,size_t capacity,const char *text){
  if(!out || !capacity) return 1;
  size_t used=strlen(out),length=text?strlen(text):0;
  if(!length) return 1;
  if(length+2u>capacity-used) return 0;
  memcpy(out+used,text,length); used+=length;
  if(out[used-1]!='\n') out[used++]='\n';
  out[used]=0;
  return 1;
}

static int collect_override_layers(const AnygmContentRouter *router,char *out,size_t capacity){
  ContentConfigResolution *config=router->configuration;
  if(out && capacity) out[0]=0;
  if(router->anchor_overrides && router->anchor_overrides_size) router->anchor_overrides[0]=0;
  if(!append_override_text(out,capacity,config->defaults)) return 0;
  for(unsigned i=1;i<CONFIG_ANCHOR_LAYERS;i++){
    if(!append_override_text(out,capacity,config->anchors[i]) ||
       ((!router->inherited_overrides || !router->inherited_overrides[0]) &&
        !append_override_text(router->anchor_overrides,router->anchor_overrides_size,
                              config->anchors[i]))) return 0;
  }
  return append_override_text(out,capacity,router->inherited_overrides) &&
    append_override_text(router->anchor_overrides,router->anchor_overrides_size,router->inherited_overrides) &&
    append_override_text(out,capacity,config->specific);
}

int anygm_content_prepare_memory(const AnygmContentRouter *router,const void *data,size_t size,
                                  uint8_t **normalized,size_t *normalized_size,
                                  char *overrides,size_t overrides_size){
  if(overrides && overrides_size) overrides[0]=0;
  if(normalized) *normalized=NULL;
  if(normalized_size) *normalized_size=0;
  if(!router || !normalized || !normalized_size || (size && !data)) return 0;
  AnygmContentRouter scoped=*router;
  scoped.configuration=calloc(1,sizeof *scoped.configuration);
  scoped.transforms=anygm_content_transforms_create();
  int ok=scoped.configuration && scoped.transforms && load_transform_defaults(&scoped);
  if(ok && scoped.configuration->ini){
    uint8_t digest[32];
    gml_sha256(data,size,digest);
    ok=select_payload_digest(&scoped,digest,"memory image");
  }
  if(ok) ok=collect_override_layers(&scoped,overrides,overrides_size);
  if(ok && anygm_content_transforms_has(scoped.transforms,"input")){
    char error[256]={0};
    ok=anygm_content_transform_run(scoped.transforms,"input",data,size,
      normalized,normalized_size,error,sizeof error);
    if(!ok) content_log(&scoped,ANYGM_CONTENT_LOG_ERROR,"input pipeline: %s",error);
  }
  if(scoped.configuration){
    anygm_content_config_destroy(scoped.configuration->ini);
    anygm_content_transforms_destroy(scoped.configuration->base_transforms);
    free(scoped.configuration);
  }
  anygm_content_transforms_destroy(scoped.transforms);
  if(!ok && overrides && overrides_size) overrides[0]=0;
  return ok;
}

AnygmContentResolveResult anygm_content_resolve_path(
    const AnygmContentRouter *router,const char *input_path,
    char *resolved_path,size_t resolved_path_size,
    char *asset_root,size_t asset_root_size,
    char *content_overrides,size_t content_overrides_size){
  if(resolved_path && resolved_path_size) resolved_path[0]=0;
  if(asset_root && asset_root_size) asset_root[0]=0;
  if(content_overrides && content_overrides_size) content_overrides[0]=0;
  if(!router || !input_path || !resolved_path || !resolved_path_size)
    return ANYGM_CONTENT_RESOLVE_INVALID;
  AnygmContentRouter scoped=*router;
  scoped.configuration=calloc(1,sizeof *scoped.configuration);
  scoped.transforms=anygm_content_transforms_create();
  if(!scoped.transforms || !scoped.configuration){
    anygm_content_transforms_destroy(scoped.transforms); free(scoped.configuration);
    return ANYGM_CONTENT_RESOLVE_INVALID;
  }
  if(!load_transform_defaults(&scoped) || !adopt_sibling_anchor_transforms(&scoped,input_path)){
    anygm_content_config_destroy(scoped.configuration->ini);
    free(scoped.configuration);
    anygm_content_transforms_destroy(scoped.transforms);
    return ANYGM_CONTENT_RESOLVE_INVALID;
  }
  router=&scoped;
  if(router->sibling_anchor_overrides && !path_ext_is(input_path,".anygm"))
    adopt_sibling_anchor_overrides(router,input_path,scoped.configuration->anchors[1],
                                   sizeof scoped.configuration->anchors[1]);

  /* Publish only after the complete configuration and payload have resolved. */
  if(content_overrides && content_overrides_size) content_overrides[0]=0;
  AnygmContentResolveResult result=content_resolve_path_direct(
    router,input_path,resolved_path,resolved_path_size,asset_root,asset_root_size,
    content_overrides,content_overrides_size);
  if(result==ANYGM_CONTENT_RESOLVE_OK &&
     !collect_override_layers(router,content_overrides,content_overrides_size)){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"configuration: combined overrides exceed their buffer");
    result=ANYGM_CONTENT_RESOLVE_INVALID;
  }
  if(result!=ANYGM_CONTENT_RESOLVE_OK){
    resolved_path[0]=0;
    if(asset_root && asset_root_size) asset_root[0]=0;
    if(content_overrides && content_overrides_size) content_overrides[0]=0;
    if(router->anchor_overrides && router->anchor_overrides_size) router->anchor_overrides[0]=0;
  }
  anygm_content_config_destroy(scoped.configuration->ini);
  anygm_content_transforms_destroy(scoped.configuration->base_transforms);
  free(scoped.configuration);
  anygm_content_transforms_destroy(scoped.transforms);
  return result;
}
