/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 *
 * Content path resolution, safe container extraction, and derived-content cache.
 */
#ifndef _WIN32
#define _GNU_SOURCE 1
#endif
#include "content_router.h"
#include "gmlc_package.h"
#include "gmlc_classic_project.h"
#include "gmlc_classic_import.h"
#include "gmlc_project.h"
#include "gml_win.h"
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
#ifdef _WIN32
  const char *bs = path ? strrchr(path, '\\') : NULL;
  if (bs && (!slash || bs > slash)) slash = bs;
#endif
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

static uint64_t cache_hash_bytes(const void *data,size_t size){
  const uint8_t *bytes=data;
  uint64_t hash=UINT64_C(1469598103934665603);
  for(size_t i=0;i<size;i++){
    hash^=(uint64_t)bytes[i];
    hash*=UINT64_C(1099511628211);
  }
  return hash;
}

static int file_hash64(const AnygmContentRouter *router,const char *path,uint64_t *hash_out) {
  if(!router || !anygm_vfs_can_read(router->host)) return 0;
  void *file=router->host->file_open(router->host->userdata,path,ANYGM_FILE_READ);
  if(!file) return 0;
  uint64_t h = UINT64_C(1469598103934665603);
  uint8_t buf[16384];
  size_t n;
  while ((n=router->host->file_read(router->host->userdata,file,buf,sizeof buf))>0) {
    if(n>sizeof buf){ router->host->file_close(router->host->userdata,file); return 0; }
    for (size_t i = 0; i < n; i++) {
      h ^= (uint64_t)buf[i];
      h *= 1099511628211ull;
    }
  }
  router->host->file_close(router->host->userdata,file);
  if (hash_out) *hash_out = h;
  return 1;
}

enum {
  ANYGM_CACHE_SCHEMA=1,
  ANYGM_CACHE_PRODUCER_REVISION=1,
  ANYGM_CACHE_ARCHIVE=1,
  ANYGM_CACHE_SOURCE_PROJECT=2,
  ANYGM_CACHE_CLASSIC_PROJECT=3,
  ANYGM_CACHE_HEADER_SIZE=72
};
/* This fingerprint names the exact deterministic producer recipe, independently of the cache
 * container schema. Change the recipe text whenever generated payload semantics change. */
static uint64_t cache_producer_fingerprint(void){
  static const char recipe[]=
    "AnyGM cache producer: structural package; classic import; bounded archive extraction";
  return cache_hash_bytes(recipe,sizeof recipe-1);
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
                           char *loaded_path,size_t loaded_path_sz) {
  if(!router || !win || !path || gml_win_load_host(win,router->host,path)!=0) return 0;
  if (loaded_path && loaded_path_sz) snprintf(loaded_path, loaded_path_sz, "%s", path);
  if(win->n_code>0) return 1;

  char alt[1024];
  if (!sibling_alternate_path(path, alt, sizeof(alt)) || !file_exists(router,alt)) return 1;
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
 * is stb_image's public-domain raw-DEFLATE implementation already linked for PNG decoding.
 *
 * Content resolution is data driven: data.win, game.droid, an arbitrary *.win fallback, then a
 * nested .port/.apk/.zip. Only the selected payload's subtree is extracted. Android runtime
 * libraries alongside assets/game.droid are therefore neither loaded nor copied. */
extern int stbi_zlib_decode_noheader_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);
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
static int zip_under_root(const char *name,const char *selected){
  const char *slash=strrchr(selected,'/');
  if(!slash) return 1;
  size_t root=(size_t)(slash-selected)+1;
  return !strncasecmp(name,selected,root);
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
  int got=stbi_zlib_decode_noheader_buffer((char*)dst,(int)usz,(const char*)src,(int)csz);
  int ok=got==(int)usz&&zip_crc32(dst,usz)==expected_crc&&
         anygm_vfs_write_all(router->host,path,dst,usz);
  free(dst);
  if(!ok) anygm_vfs_remove(router->host,path);
  return ok;
}

/* Extract a source project (project_rel != NULL) in full, or only the selected game payload
 * subtree. nested_rel receives a fallback archive when this level has no direct payload. */
static int zip_extract_all(const AnygmContentRouter *router,const char *zpath,const char *outdir,
                           char *content_rel,size_t crsz,char *project_rel,size_t prsz,
                           char *nested_rel,size_t nrsz){
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
  if(!zip_seen_names_init(&seen,n_ent?n_ent:1u)){ file_map_close(&map); return 0; }
  int content_best=0,nested_best=0;
  size_t p=cdir;
  if(content_rel && crsz) content_rel[0]=0;
  if(project_rel && prsz) project_rel[0]=0;
  if(nested_rel && nrsz) nested_rel[0]=0;
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
    if(!zip_name_normalize(name,nlen) || !zip_seen_names_add(&seen,name)){ valid=0; break; }
    int cs=content_rel?zip_content_score(name):0;
    if(cs>content_best){ snprintf(content_rel,crsz,"%s",name); content_best=cs; }
    if(project_rel && !project_rel[0] && zip_endswith(name,".yyp")) snprintf(project_rel,prsz,"%s",name);
    int ns=nested_rel?zip_nested_score(name):0;
    if(ns>nested_best){ snprintf(nested_rel,nrsz,"%s",name); nested_best=ns; }
    p=next;
  }
  zip_seen_names_free(&seen);
  if(!valid || scanned!=n_ent || p!=(size_t)cdir+(size_t)cdir_size ||
     (content_rel && !content_rel[0] && (!nested_rel || !nested_rel[0])) ||
     (project_rel && !project_rel[0])){ file_map_close(&map); return 0; }
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
  file_map_close(&map);
  return valid?extracted:0;
}

unsigned anygm_content_path_hash(const char *s){
  uint32_t h=2166136261u; for(;s&&*s;s++){ h^=(uint8_t)*s; h*=16777619u; } return h;
}
static int sibling_payload(const AnygmContentRouter *router,const char *archive,char *out,size_t outsz){
  char parent[1024]; snprintf(parent,sizeof parent,"%s",archive);
  char *slash=strrchr(parent,'/'),*bs=strrchr(parent,'\\');
  if(bs && (!slash || bs>slash)) slash=bs;
  if(slash) *slash=0; else snprintf(parent,sizeof parent,".");
  static const char *rel[]={"data.win","game.droid","assets/data.win","assets/game.droid",
                            "gamedata/data.win","gamedata/game.droid"};
  for(size_t i=0;i<sizeof rel/sizeof rel[0];i++){
    char candidate[1280]; snprintf(candidate,sizeof candidate,"%s/%s",parent,rel[i]);
    if(file_magic_kind(router,candidate)==1){ snprintf(out,outsz,"%s",candidate); return 1; }
  }
  return 0;
}
static int load_archive_content_depth(const AnygmContentRouter *router,const char *zpath,
                                      char *content_path,size_t cpsz,int depth){
  if(depth>=(int)ANYGM_CONTENT_MAX_NESTING_LEVELS){
    content_log(router,ANYGM_CONTENT_LOG_ERROR,"archive: nesting limit reached at %s",zpath);
    return 0;
  }
  const char *base=router?router->cache_directory:NULL;
  char zbuf[1024]; snprintf(zbuf,sizeof zbuf,"%s",zpath);
  const char *slash=strrchr(zbuf,'/'),*bs=strrchr(zbuf,'\\');
  if(bs && (!slash || bs>slash)) slash=bs;
  const char *zname=slash?slash+1:zbuf;
  char stem[192]; snprintf(stem,sizeof stem,"%s",zname);
  { char *dot=strrchr(stem,'.'); if(dot) *dot=0; }
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
    snprintf(rel,sizeof rel,"%s",marker_data+2);
    snprintf(existing,sizeof existing,"%s/%s",outdir,rel);
    uint64_t existing_size=0,existing_hash=0;
    cache_ok=(kind=='C' || kind=='N') && file_size64(router,existing,&existing_size) &&
             existing_size==marker_payload_size && file_hash64(router,existing,&existing_hash) &&
             existing_hash==marker_payload_hash;
  } else cache_ok=0;
  if(!cache_ok){
    char nested[512]=""; rel[0]=0; kind=0;
    if(!mkdirs_for(router,outdir,1)) return 0;
    int n=zip_extract_all(router,zpath,outdir,rel,sizeof rel,NULL,0,nested,sizeof nested);
    if(n<=0 || (!rel[0] && !nested[0])){
      if(sibling_payload(router,zpath,content_path,cpsz)){
        content_log(router,ANYGM_CONTENT_LOG_INFO,"archive: using sibling payload %s",content_path); return 1;
      }
      content_log(router,ANYGM_CONTENT_LOG_ERROR,"archive: no supported payload or nested archive in %s",zpath);
      return 0;
    }
    if(rel[0]) kind='C'; else { kind='N'; snprintf(rel,sizeof rel,"%s",nested); }
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
  char resolved[1536]; snprintf(resolved,sizeof resolved,"%s/%s",outdir,rel);
  int magic=file_magic_kind(router,resolved);
  if(kind=='N' || magic==2) return load_archive_content_depth(router,resolved,content_path,cpsz,depth+1);
  if(kind!='C' || magic!=1) return 0;
  snprintf(content_path,cpsz,"%s",resolved);
  return 1;
}
static int load_archive_content(const AnygmContentRouter *router,const char *zpath,
                                char *content_path,size_t cpsz){
  return load_archive_content_depth(router,zpath,content_path,cpsz,0);
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
  char source_dir[4096];
  anygm_content_path_parent(srcpath,source_dir,sizeof(source_dir));
  if(!gmlc_classic_extension_dependency_hash(router->host,source_dir,src_hash,&src_hash)) return 0;
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
  if(!gmlc_classic_project_load(&project,router->host,srcpath,outdir,err,sizeof err)){
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
    int n=zip_extract_all(router,srcpath,outdir,NULL,0,rel,sizeof rel,NULL,0);
    if(n<=0 || !rel[0]){
      content_log(router,ANYGM_CONTENT_LOG_ERROR,"source: no project manifest found inside %s",srcpath);
      return 0;
    }
    snprintf(project_path,sizeof project_path,"%s/%s",outdir,rel);
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

int anygm_content_resolve_path(const AnygmContentRouter *router,const char *input_path,
                               char *resolved_path,size_t resolved_path_size){
  if(!input_path || !input_path[0] || !resolved_path || resolved_path_size==0) return 0;
  resolved_path[0]=0;
  if(path_ext_is(input_path,".gmk") || path_ext_is(input_path,".gm81") ||
     path_ext_is(input_path,".gm6") || path_ext_is(input_path,".exe")){
    return load_classic_project_content(router,input_path,resolved_path,resolved_path_size);
  }
  if(path_ext_is(input_path,".yyp") || path_ext_is(input_path,".yyz")){
    return load_source_project_content(router,input_path,resolved_path,resolved_path_size);
  }
  if(path_ext_is(input_path,".zip") || path_ext_is(input_path,".port") ||
     path_ext_is(input_path,".apk") || file_magic_kind(router,input_path)==2){
    return load_archive_content(router,input_path,resolved_path,resolved_path_size);
  }
  if(snprintf(resolved_path,resolved_path_size,"%s",input_path)>=(int)resolved_path_size){
    resolved_path[0]=0;
    return 0;
  }
  return 1;
}
