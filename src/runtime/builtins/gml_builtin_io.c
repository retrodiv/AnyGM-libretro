/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* VFS-backed files, buffers, INI data, encoding, hashing, and ordered I/O dispatch. */
#include "gml_builtin_internal.h"
#include "anygm_host.h"
#include "anygm_vfs.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int wild_match(const char *pat, const char *s){
  if(*pat==0) return *s==0;
  if(*pat=='*'){ if(wild_match(pat+1,s)) return 1; return *s ? wild_match(pat,s+1) : 0; }
  if(*s==0) return 0;
  if(*pat=='?' || tolower((unsigned char)*pat)==tolower((unsigned char)*s)) return wild_match(pat+1,s+1);
  return 0;
}

static int path_readable(GmlVM *vm,const char *p){
  AnygmFileInfo info;
  return vm && p && anygm_vfs_stat(vm->host,p,&info) &&
         (info.flags&ANYGM_FILE_INFO_REGULAR)!=0;
}
static int path_absolute(const char *p){
  return p && (p[0]=='/' || (p[0] && p[1]==':'));
}
static int path_present(GmlVM *vm,const char *p);
static char *path_under(const char *dir,const char *p){
  if(!dir || !dir[0]) return strdup(p?p:"");
  size_t n=strlen(dir)+1+strlen(p?p:"")+1;
  char *out=malloc(n);
  if(!out) return strdup(p?p:"");
  snprintf(out,n,"%s/%s",dir,p?p:"");
  /* GameMaker paths are authored with Windows separators even on a
   * non-Windows host.  A backslash cannot name a distinct path component in the original
   * environment, so preserve that contract at the sandbox boundary instead of creating files
   * whose literal names contain backslashes on Unix.  Forward slashes are accepted by the
   * Windows CRT as well, which keeps this representation portable across host targets. */
  for(char *cursor=out+strlen(dir)+1;*cursor;cursor++)
    if(*cursor=='\\') *cursor='/';
  return out;
}
static int path_component_equal_folded(const char *left,size_t left_size,
                                       const char *right){
  if(!left || !right || strlen(right)!=left_size) return 0;
  for(size_t index=0;index<left_size;index++)
    if(tolower((unsigned char)left[index])!=
       tolower((unsigned char)right[index])) return 0;
  return 1;
}
static int path_component_casefold(GmlVM *vm,const char *directory,
                                   const char *component,size_t component_size,
                                   char *resolved,size_t resolved_size){
  if(!vm || !vm->host || !vm->host->directory_open ||
     !vm->host->directory_read || !vm->host->directory_close ||
     !directory || !component || !component_size || !resolved ||
     resolved_size<=component_size) return 0;
  void *handle=vm->host->directory_open(vm->host->userdata,directory);
  if(!handle) return 0;
  int found=0;
  AnygmDirectoryEntry entry={0};
  entry.struct_size=sizeof entry;
  while(vm->host->directory_read(vm->host->userdata,handle,&entry)==ANYGM_OK){
    if(path_component_equal_folded(component,component_size,entry.name) &&
       (!found || strcmp(entry.name,resolved)<0)){
      snprintf(resolved,resolved_size,"%s",entry.name);
      found=1;
    }
    memset(&entry,0,sizeof entry);
    entry.struct_size=sizeof entry;
  }
  vm->host->directory_close(vm->host->userdata,handle);
  return found;
}
/* Installed Windows paths use case-insensitive component lookup. Keep exact-path reads fast, then
 * recover each component through the host VFS when a portable host stores the same bundle on a
 * case-sensitive filesystem. */
static char *path_under_read(GmlVM *vm,const char *directory,const char *relative){
  char *exact=path_under(directory,relative);
  if(path_present(vm,exact) || !vm || !vm->host ||
     !vm->host->directory_open || !vm->host->directory_read ||
     !vm->host->directory_close) return exact;
  size_t capacity=strlen(exact)+1;
  char *resolved=malloc(capacity);
  if(!resolved) return exact;
  size_t used=0;
  if(directory && directory[0]){
    used=strlen(directory);
    while(used>0 && (directory[used-1]=='/' || directory[used-1]=='\\')) used--;
    memcpy(resolved,directory,used);
  }
  resolved[used]=0;
  const char *cursor=relative?relative:"";
  while(*cursor){
    while(*cursor=='/' || *cursor=='\\') cursor++;
    if(!*cursor) break;
    const char *end=cursor;
    while(*end && *end!='/' && *end!='\\') end++;
    size_t component_size=(size_t)(end-cursor);
    size_t directory_size=used;
    if(used && used+1<capacity){
      resolved[used++]='/';
      resolved[used]=0;
    }
    char candidate[sizeof(((AnygmDirectoryEntry *)0)->name)]={0};
    if(component_size>=sizeof candidate ||
       !path_component_casefold(vm,directory_size?resolved:"",cursor,component_size,
                                candidate,sizeof candidate)){
      if(used+component_size>=capacity){
        free(resolved);
        return exact;
      }
      memcpy(resolved+used,cursor,component_size);
    } else {
      memcpy(resolved+used,candidate,component_size);
    }
    used+=component_size;
    resolved[used]=0;
    cursor=end;
  }
  free(exact);
  return resolved;
}
static int path_present(GmlVM *vm,const char *p){
  AnygmFileInfo info;
  return vm && p && anygm_vfs_stat(vm->host,p,&info);
}
/* working_directory exposes the installed bundle while the file API overlays the writable save
 * area. An absolute path formed from working_directory therefore retains the overlay on reads and
 * redirects writes. Recover that logical relative path at the file-API boundary instead of
 * exposing the physical save directory as working_directory, which would hide bundled defaults. */
static const char *path_relative_to_root(const char *path,const char *root){
  if(!path || !root || !*root) return NULL;
  size_t n=strlen(root);
  while(n>0 && (root[n-1]=='/' || root[n-1]=='\\')) n--;
  if(!n || strncmp(path,root,n)) return NULL;
  if(path[n] && path[n]!='/' && path[n]!='\\') return NULL;
  const char *relative=path+n;
  while(*relative=='/' || *relative=='\\') relative++;
  return relative;
}
char *resolve_read_path(GmlVM *vm, const char *p){
  if(!p || !*p) return strdup("");
  if(path_absolute(p)){
    if(vm && vm->win){
      const char *relative=path_relative_to_root(p,vm->win->content_dir);
      if(relative){
        if(vm->win->save_dir[0]){
          char *save=path_under_read(vm,vm->win->save_dir,relative);
          if(path_present(vm,save)) return save;
          free(save);
        }
        return path_under_read(vm,vm->win->content_dir,relative);
      }
      relative=path_relative_to_root(p,vm->win->save_dir);
      if(relative){
        char *save=path_under_read(vm,vm->win->save_dir,relative);
        if(path_present(vm,save)) return save;
        free(save);
        if(vm->win->content_dir[0]){
          char *content=path_under_read(vm,vm->win->content_dir,relative);
          if(path_present(vm,content)) return content;
          free(content);
        }
        return path_under(vm->win->save_dir,relative);
      }
    }
    return strdup(p);
  }
  /* Relative reads use an overlay: the writable copy wins, followed by installed assets.
   * Never consult the host working directory while either sandbox is known. */
  if(vm && vm->win){
    if(vm->win->save_dir[0]){
      char *save=path_under_read(vm,vm->win->save_dir,p);
      if(path_present(vm,save)) return save;
      free(save);
    }
    if(vm->win->content_dir[0]) return path_under_read(vm,vm->win->content_dir,p);
    if(vm->win->save_dir[0]) return path_under(vm->win->save_dir,p);
  }
  if(path_readable(vm,p)) return strdup(p);
  return strdup(p);
}
char *resolve_write_path(GmlVM *vm, const char *p){
  if(!p || !*p) return strdup("");
  if(path_absolute(p)){
    if(vm && vm->win && vm->win->save_dir[0]){
      const char *relative=path_relative_to_root(p,vm->win->content_dir);
      if(relative) return path_under(vm->win->save_dir,relative);
    }
    return strdup(p);
  }
  if(vm && vm->win){
    if(vm->win->save_dir[0]) return path_under(vm->win->save_dir,p);
    if(vm->win->content_dir[0]) return path_under(vm->win->content_dir,p);
  }
  return strdup(p);
}
static int copy_file_path(GmlVM *vm,const char *src,const char *dst){
  return vm && anygm_vfs_copy(vm->host,src,dst);
}

int vm_file_slot(GmlVM *vm, int id){
  int i=id-1;
  return (i>=0 && i<16 && vm->builtins->bin_file[i]) ? i : -1;
}
typedef struct {
  void *handle;
  int pushed;
  unsigned char pushed_byte;
} GmlHostFile;
static GmlHostFile *vm_host_file(GmlVM *vm,int slot){
  return vm && slot>=0 && slot<16 ? (GmlHostFile *)vm->builtins->bin_file[slot] : NULL;
}
int vm_file_open(GmlVM *vm, const char *path, const char *mode){
  if(!vm || !vm->host || !vm->host->file_open || !vm->host->file_close || !path || !mode) return -1;
  AnygmFileMode flags=0;
  if(strchr(mode,'r') || strchr(mode,'+')) flags|=ANYGM_FILE_READ;
  if(strchr(mode,'w') || strchr(mode,'a') || strchr(mode,'+')) flags|=ANYGM_FILE_WRITE;
  if(strchr(mode,'w') || strchr(mode,'a')) flags|=ANYGM_FILE_CREATE;
  if(strchr(mode,'w')) flags|=ANYGM_FILE_TRUNCATE;
  void *handle=vm->host->file_open(vm->host->userdata,path,flags);
  if(!handle) return -1;
  GmlHostFile *file=calloc(1,sizeof *file);
  if(!file){ vm->host->file_close(vm->host->userdata,handle); return -1; }
  file->handle=handle;
  if(strchr(mode,'a') && vm->host->file_seek)
    vm->host->file_seek(vm->host->userdata,handle,0,ANYGM_SEEK_END);
  for(int i=0;i<16;i++) if(!vm->builtins->bin_file[i]){ vm->builtins->bin_file[i]=file; return i+1; }
  vm->host->file_close(vm->host->userdata,handle);
  free(file);
  return -1;
}
void vm_file_close(GmlVM *vm,int slot){
  GmlHostFile *file=vm_host_file(vm,slot);
  if(!file) return;
  if(vm->host && vm->host->file_flush) vm->host->file_flush(vm->host->userdata,file->handle);
  if(vm->host && vm->host->file_close) vm->host->file_close(vm->host->userdata,file->handle);
  free(file);
  vm->builtins->bin_file[slot]=NULL;
}
void builtin_io_files_close(GmlBuiltinState *state){
  if(!state || !state->vm || state->vm->builtins!=state) return;
  GmlVM *vm=state->vm;
  for(int i=0;i<16;i++) vm_file_close(vm,i);
}
static size_t vm_file_read(GmlVM *vm,int slot,void *data,size_t size){
  GmlHostFile *file=vm_host_file(vm,slot);
  if(!file || !vm->host || !vm->host->file_read || (!data&&size)) return 0;
  uint8_t *output=data;
  size_t used=0;
  if(size && file->pushed){ output[used++]=file->pushed_byte; file->pushed=0; }
  if(used<size) used+=vm->host->file_read(vm->host->userdata,file->handle,output+used,size-used);
  return used;
}
static size_t vm_file_write(GmlVM *vm,int slot,const void *data,size_t size){
  GmlHostFile *file=vm_host_file(vm,slot);
  if(!file || !vm->host || !vm->host->file_write || (!data&&size)) return 0;
  size_t written=0;
  while(written<size){
    size_t step=vm->host->file_write(vm->host->userdata,file->handle,
                                    (const uint8_t *)data+written,size-written);
    if(!step || step>size-written) break;
    written+=step;
  }
  return written;
}
int vm_file_getc(GmlVM *vm,int slot){
  unsigned char byte=0;
  return vm_file_read(vm,slot,&byte,1)==1 ? byte : EOF;
}
void vm_file_ungetc(GmlVM *vm,int slot,int value){
  GmlHostFile *file=vm_host_file(vm,slot);
  if(file && value!=EOF && !file->pushed){ file->pushed=1; file->pushed_byte=(unsigned char)value; }
}
static int64_t vm_file_seek(GmlVM *vm,int slot,int64_t offset,AnygmSeekOrigin origin){
  GmlHostFile *file=vm_host_file(vm,slot);
  if(!file || !vm->host || !vm->host->file_seek) return -1;
  file->pushed=0;
  return vm->host->file_seek(vm->host->userdata,file->handle,offset,origin);
}
static int64_t vm_file_tell(GmlVM *vm,int slot){
  GmlHostFile *file=vm_host_file(vm,slot);
  if(!file || !vm->host || !vm->host->file_seek) return -1;
  int64_t position=vm->host->file_seek(vm->host->userdata,file->handle,0,ANYGM_SEEK_CURRENT);
  if(position>=0 && file->pushed) position--;
  return position;
}
static int64_t vm_file_size(GmlVM *vm,int slot){
  int64_t position=vm_file_tell(vm,slot);
  if(position<0) return -1;
  int64_t size=vm_file_seek(vm,slot,0,ANYGM_SEEK_END);
  if(size>=0) vm_file_seek(vm,slot,position,ANYGM_SEEK_START);
  return size;
}
int vm_file_writef(GmlVM *vm,int slot,const char *format,...){
  va_list ap;
  va_start(ap,format);
  va_list copy;
  va_copy(copy,ap);
  int needed=vsnprintf(NULL,0,format,copy);
  va_end(copy);
  if(needed<0){ va_end(ap); return 0; }
  char local[256];
  char *text=needed<(int)sizeof local?local:malloc((size_t)needed+1);
  if(!text){ va_end(ap); return 0; }
  vsnprintf(text,(size_t)needed+1,format,ap);
  va_end(ap);
  int ok=vm_file_write(vm,slot,text,(size_t)needed)==(size_t)needed;
  if(text!=local) free(text);
  return ok;
}
int vm_file_read_real(GmlVM *vm,int slot,double *value){
  if(!value) return 0;
  char token[128];
  size_t length=0;
  int c;
  do c=vm_file_getc(vm,slot); while(c!=EOF && isspace((unsigned char)c));
  while(c!=EOF && !isspace((unsigned char)c)){
    if(length+1<sizeof token) token[length++]=(char)c;
    c=vm_file_getc(vm,slot);
  }
  if(c!=EOF) vm_file_ungetc(vm,slot,c);
  token[length]=0;
  char *end=NULL;
  double parsed=strtod(token,&end);
  if(!length || end==token) return 0;
  *value=parsed;
  return 1;
}
int vm_buffer_slot(GmlVM *vm, int id){
  int i=id-1;
  return (i>=0 && i<16 && vm->builtins->buffer[i].live) ? i : -1;
}

static GmlVal ini_default_string(GmlVM *vm,GmlVal *a,int n){
  const char *s = n>2 ? S(vm,a,n,2) : "";
  char *c = strdup(s?s:"");
  return c ? vstr_owned(c) : vstr("");
}

static void ini_reset(GmlVM *vm){
  builtin_state_ini_reset(vm?vm->builtins:NULL);
}
static char *trim_ws(char *s){
  while(*s && isspace((unsigned char)*s)) s++;
  char *e=s+strlen(s);
  while(e>s && isspace((unsigned char)e[-1])) *--e=0;
  return s;
}
static char *ini_unquote_value(char *s){
  s=trim_ws(s);
  size_t n=strlen(s);
  if(n>=2 && s[0]=='"' && s[n-1]=='"'){
    s[n-1]=0;
    s++;
  }
  return s;
}
static double ini_parse_real(const char *text){
  const char *source=text?text:"";
  char *end=NULL;
  double value=strtod(source,&end);
  while(end && isspace((unsigned char)*end)) end++;
  if(end && end!=source && !*end) return value;

  /* Settings files may contain localized decimal separators. Keep parsing locale-independent while
   * accepting a single decimal comma when no decimal point is present. Other punctuation remains
   * invalid instead of being silently truncated. */
  const char *comma=strchr(source,',');
  if(!comma || strchr(source,'.') || strchr(comma+1,',')) return value;
  size_t length=strlen(source);
  if(length>=128) return value;
  char normalized[128];
  memcpy(normalized,source,length+1);
  normalized[comma-source]='.';
  end=NULL;
  double localized=strtod(normalized,&end);
  while(end && isspace((unsigned char)*end)) end++;
  return end && end!=normalized && !*end ? localized : value;
}
static void ini_add_kv(GmlVM *vm, const char *sec, const char *key, const char *val){
  if(!sec || !key || !*key || vm->builtins->ini_n>=GML_INI_MAX) return;
  vm->builtins->ini_kv[vm->builtins->ini_n]=(typeof(vm->builtins->ini_kv[0])){
    .section=strdup(sec),
    .key=strdup(key),
    .sval=strdup(val?val:""),
    .val=ini_parse_real(val),
    .is_str=1
  };
  vm->builtins->ini_n++;
}
static void ini_parse_text(GmlVM *vm, const char *text){
  char *copy=strdup(text?text:"");
  char *sec=NULL, *p=copy;
  while(p && *p){
    char *line=p;
    char *nl=strpbrk(p,"\r\n");
    if(nl){
      char c=*nl;
      *nl=0;
      p=nl+1;
      if((c=='\r' && *p=='\n') || (c=='\n' && *p=='\r')) p++;
    } else {
      p=NULL;
    }
    line=trim_ws(line);
    if(!*line || *line==';' || *line=='#') continue;
    if(*line=='['){
      char *e=strchr(line,']');
      if(e){ *e=0; free(sec); sec=strdup(trim_ws(line+1)); }
      continue;
    }
    char *eq=strchr(line,'=');
    if(!eq || !sec) continue;
    *eq=0;
    ini_add_kv(vm,sec,trim_ws(line),ini_unquote_value(eq+1));
  }
  free(sec);
  free(copy);
}
GmlVal builtin_ini_open_file(GmlVM *vm, GmlVal *a, int n){
  ini_reset(vm);
  vm->builtins->ini_open=1;
  char *fn=resolve_read_path(vm,S(vm,a,n,0));
  char *write_fn=resolve_write_path(vm,S(vm,a,n,0));
  snprintf(vm->builtins->ini_path,sizeof vm->builtins->ini_path,"%s",write_fn?write_fn:"");
  free(write_fn);
  uint8_t *data=NULL;
  size_t size=0;
  if(anygm_vfs_read_all(vm->host,fn,&data,&size,16u*1024u*1024u)){
    char *text=realloc(data,size+1);
    if(text){ text[size]=0; ini_parse_text(vm,text); free(text); }
    else free(data);
  }
  free(fn);
  return vreal(1);
}
GmlVal builtin_file_text_open_read(GmlVM *vm, GmlVal *a, int n){
  char *path=resolve_read_path(vm,S(vm,a,n,0));
  int id=vm_file_open(vm,path,"r");
  if(builtin_setting(vm,"GML_LOG_IO"))
    anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_DEBUG,
                    "[io] file_text_open_read path=%s handle=%d\n",
                    path?path:"",id);
  free(path);
  return vreal(id);
}
GmlVal builtin_file_text_read_string(GmlVM *vm, GmlVal *a, int n){
  int i=vm_file_slot(vm,(int)N(a,n,0));
  if(i<0) return vstr_owned(strdup(""));
  size_t cap=4096, k=0;
  int c;
  char *b=malloc(cap);
  if(!b) return vstr_owned(strdup(""));
  while((c=vm_file_getc(vm,i))!=EOF){
    if(c=='\n' || c=='\r'){ vm_file_ungetc(vm,i,c); break; }
    if(k+1>=cap){
      if(cap>=16*1024*1024) break;
      cap*=2;
      char *nb=realloc(b,cap);
      if(!nb) break;
      b=nb;
    }
    b[k++]=(char)c;
  }
  b[k]=0;
  if(builtin_setting(vm,"GML_LOG_IO"))
    anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_DEBUG,
                    "[io] file_text_read_string handle=%d value=%s\n",
                    i+1,b);
  return vstr_owned(b);
}
GmlVal builtin_file_text_readln(GmlVM *vm, GmlVal *a, int n){
  int i=vm_file_slot(vm,(int)N(a,n,0));
  if(i<0) return vstr_owned(strdup(""));
  size_t cap=256, k=0;
  int c;
  char *b=malloc(cap);
  if(!b) return vstr_owned(strdup(""));
  while((c=vm_file_getc(vm,i))!=EOF && c!='\n'){
    if(c=='\r') continue;
    if(k+1>=cap){
      cap*=2;
      char *nb=realloc(b,cap);
      if(!nb) break;
      b=nb;
    }
    b[k++]=(char)c;
  }
  b[k]=0;
  return vstr_owned(b);
}
static int buffer_alloc(GmlVM *vm, int cap){
  for(int k=0;k<16;k++){
    int i=(vm->builtins->next_buffer_id+k-1)%16;
    if(!vm->builtins->buffer[i].live){
      if(cap<1) cap=1;
      vm->builtins->buffer[i].data=calloc((size_t)cap,1);
      vm->builtins->buffer[i].cap=cap; vm->builtins->buffer[i].size=cap; vm->builtins->buffer[i].pos=0; vm->builtins->buffer[i].live=1;
      vm->builtins->next_buffer_id=i+2; if(vm->builtins->next_buffer_id>16) vm->builtins->next_buffer_id=1;
      return i+1;
    }
  }
  return -1;
}
static void buffer_ensure(GmlVM *vm, int bi, int need){
  if(bi<0||bi>=16||need<=vm->builtins->buffer[bi].cap) return;
  int nc=vm->builtins->buffer[bi].cap?vm->builtins->buffer[bi].cap:1;
  while(nc<need) nc*=2;
  vm->builtins->buffer[bi].data=realloc(vm->builtins->buffer[bi].data,(size_t)nc);
  memset(vm->builtins->buffer[bi].data+vm->builtins->buffer[bi].cap,0,(size_t)(nc-vm->builtins->buffer[bi].cap));
  vm->builtins->buffer[bi].cap=nc;
}
static void buffer_resize_slot(GmlVM *vm, int bi, int size){
  if(bi<0||bi>=16||!vm->builtins->buffer[bi].live) return;
  if(size<0) size=0;
  int alloc=size>0?size:1;
  unsigned char *nd=realloc(vm->builtins->buffer[bi].data,(size_t)alloc);
  if(!nd) return;
  if(size>vm->builtins->buffer[bi].cap) memset(nd+vm->builtins->buffer[bi].cap,0,(size_t)(size-vm->builtins->buffer[bi].cap));
  vm->builtins->buffer[bi].data=nd;
  vm->builtins->buffer[bi].cap=alloc;
  vm->builtins->buffer[bi].size=size;
  if(vm->builtins->buffer[bi].pos>size) vm->builtins->buffer[bi].pos=size;
}
static void buffer_write_at(GmlVM *vm, int bi, int pos, const void *p, int n){
  if(bi<0||bi>=16||pos<0||n<=0) return;
  buffer_ensure(vm,bi,pos+n);
  memcpy(vm->builtins->buffer[bi].data+pos,p,(size_t)n);
  if(vm->builtins->buffer[bi].size<pos+n) vm->builtins->buffer[bi].size=pos+n;
}
static int buffer_write_typed_at(GmlVM *vm, int bi, int pos, int type, const char *sv, double dv){
  if(type==11 || type==13){
    int len=(int)strlen(sv?sv:"") + (type==11);
    buffer_write_at(vm,bi,pos,sv?sv:"",len);
    return len;
  }
  if(type==1||type==2||type==10){ uint8_t v=(uint8_t)((int)dv); buffer_write_at(vm,bi,pos,&v,1); return 1; }
  if(type==3||type==4){ uint16_t v=(uint16_t)((int)dv); buffer_write_at(vm,bi,pos,&v,2); return 2; }
  if(type==5||type==6){ uint32_t v=(uint32_t)((int32_t)dv); buffer_write_at(vm,bi,pos,&v,4); return 4; }
  if(type==8){ float v=(float)dv; buffer_write_at(vm,bi,pos,&v,4); return 4; }
  if(type==7){ uint16_t v=0; float f=(float)dv; uint32_t u; memcpy(&u,&f,4);
    v=(uint16_t)(((u>>16)&0x8000)|((((u>>23)&0xFF)-112)<<10&0x7C00)|((u>>13)&0x3FF));
    buffer_write_at(vm,bi,pos,&v,2); return 2; }
  if(type==12){ uint64_t v=(uint64_t)(int64_t)dv; buffer_write_at(vm,bi,pos,&v,8); return 8; }
  double v=dv; buffer_write_at(vm,bi,pos,&v,8); return 8;
}
void buffer_write_raw(GmlVM *vm, int bi, const void *p, int n){
  if(n<=0) return;
  int pos=vm->builtins->buffer[bi].pos;
  buffer_ensure(vm,bi,pos+n);
  memcpy(vm->builtins->buffer[bi].data+pos,p,(size_t)n);
  vm->builtins->buffer[bi].pos=pos+n;
  if(vm->builtins->buffer[bi].size<vm->builtins->buffer[bi].pos) vm->builtins->buffer[bi].size=vm->builtins->buffer[bi].pos;
}
static uint64_t buffer_read_u(GmlVM *vm, int bi, int n){
  uint64_t v=0;
  if(n<=0) return 0;
  int pos=vm->builtins->buffer[bi].pos, avail=vm->builtins->buffer[bi].size-pos;
  if(avail<n) n=avail;
  if(n>0){ memcpy(&v,vm->builtins->buffer[bi].data+pos,(size_t)n); vm->builtins->buffer[bi].pos=pos+n; }
  return v;
}
static GmlVal buffer_read_typed_at_pos(GmlVM *vm, int bi, int pos, int type){
  if(bi<0||bi>=16||!vm->builtins->buffer[bi].live) return vreal(0);
  int old=vm->builtins->buffer[bi].pos;
  if(pos<0) pos=0;
  if(pos>vm->builtins->buffer[bi].size) pos=vm->builtins->buffer[bi].size;
  vm->builtins->buffer[bi].pos=pos;
  GmlVal out=vreal(0);
  if(type==11){
    int p=vm->builtins->buffer[bi].pos, e=p;
    while(e<vm->builtins->buffer[bi].size && vm->builtins->buffer[bi].data[e]) e++;
    out=vstr_owned(dup_n((char*)vm->builtins->buffer[bi].data+p,e-p));
  } else if(type==13){
    int p=vm->builtins->buffer[bi].pos, e=vm->builtins->buffer[bi].size;
    out=vstr_owned(dup_n((char*)vm->builtins->buffer[bi].data+p,e-p));
  } else if(type==1||type==10) out=vreal((uint8_t)buffer_read_u(vm,bi,1));
  else if(type==2) out=vreal((int8_t)buffer_read_u(vm,bi,1));
  else if(type==3) out=vreal((uint16_t)buffer_read_u(vm,bi,2));
  else if(type==4) out=vreal((int16_t)buffer_read_u(vm,bi,2));
  else if(type==5) out=vreal((uint32_t)buffer_read_u(vm,bi,4));
  else if(type==6) out=vreal((int32_t)buffer_read_u(vm,bi,4));
  else if(type==7){ uint16_t h=(uint16_t)buffer_read_u(vm,bi,2);
    uint32_t sgn=(h&0x8000)<<16, ex=(h>>10)&0x1F, mn=h&0x3FF;
    uint32_t u = ex==0 ? sgn : (sgn|((ex+112)<<23)|(mn<<13));
    float f; memcpy(&f,&u,4); out=vreal(f);
  } else if(type==8){ uint32_t u=(uint32_t)buffer_read_u(vm,bi,4); float f; memcpy(&f,&u,4); out=vreal(f);
  } else if(type==12){ uint64_t u=buffer_read_u(vm,bi,8); out=vreal((double)(int64_t)u);
  } else { uint64_t u=buffer_read_u(vm,bi,8); double d; memcpy(&d,&u,8); out=vreal(d); }
  vm->builtins->buffer[bi].pos=old;
  return out;
}
typedef struct { uint32_t h[4]; uint64_t bits; uint8_t buf[64]; int used; } GmlMd5;
static uint32_t md5_rot(uint32_t x, uint32_t n){ return (x<<n)|(x>>(32-n)); }
static void md5_transform(GmlMd5 *m, const uint8_t b[64]){
  static const uint32_t K[64]={
    0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
    0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
    0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
    0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
    0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
    0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
    0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
    0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u};
  static const uint8_t SFT[64]={
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
  uint32_t x[16];
  for(int i=0;i<16;i++)
    x[i]=(uint32_t)b[i*4]|((uint32_t)b[i*4+1]<<8)|((uint32_t)b[i*4+2]<<16)|((uint32_t)b[i*4+3]<<24);
  uint32_t A=m->h[0], B=m->h[1], C=m->h[2], D=m->h[3];
  for(int i=0;i<64;i++){
    uint32_t F,g;
    if(i<16){ F=(B&C)|((~B)&D); g=(uint32_t)i; }
    else if(i<32){ F=(D&B)|((~D)&C); g=(uint32_t)((5*i+1)&15); }
    else if(i<48){ F=B^C^D; g=(uint32_t)((3*i+5)&15); }
    else { F=C^(B|(~D)); g=(uint32_t)((7*i)&15); }
    uint32_t T=D;
    D=C; C=B;
    B += md5_rot(A+F+K[i]+x[g],SFT[i]);
    A=T;
  }
  m->h[0]+=A; m->h[1]+=B; m->h[2]+=C; m->h[3]+=D;
}
static void md5_init(GmlMd5 *m){
  m->h[0]=0x67452301u; m->h[1]=0xefcdab89u; m->h[2]=0x98badcfeu; m->h[3]=0x10325476u;
  m->bits=0; m->used=0;
}
static void md5_update(GmlMd5 *m, const uint8_t *p, size_t n){
  m->bits += (uint64_t)n*8u;
  while(n>0){
    size_t take=64u-(size_t)m->used;
    if(take>n) take=n;
    memcpy(m->buf+m->used,p,take);
    m->used += (int)take; p += take; n -= take;
    if(m->used==64){ md5_transform(m,m->buf); m->used=0; }
  }
}
static void md5_final(GmlMd5 *m, uint8_t out[16]){
  uint64_t bits=m->bits;
  m->buf[m->used++]=0x80;
  if(m->used>56){
    while(m->used<64) m->buf[m->used++]=0;
    md5_transform(m,m->buf);
    m->used=0;
  }
  while(m->used<56) m->buf[m->used++]=0;
  for(int i=0;i<8;i++) m->buf[56+i]=(uint8_t)(bits>>(8*i));
  md5_transform(m,m->buf);
  for(int i=0;i<4;i++){
    out[i*4]=(uint8_t)m->h[i];
    out[i*4+1]=(uint8_t)(m->h[i]>>8);
    out[i*4+2]=(uint8_t)(m->h[i]>>16);
    out[i*4+3]=(uint8_t)(m->h[i]>>24);
  }
}
GmlVal md5_hex_val(const uint8_t *p, size_t n){
  static const char H[]="0123456789abcdef";
  uint8_t d[16];
  GmlMd5 m;
  md5_init(&m);
  if(p && n) md5_update(&m,p,n);
  md5_final(&m,d);
  char *s=malloc(33);
  if(!s) return vstr("");
  for(int i=0;i<16;i++){ s[i*2]=H[d[i]>>4]; s[i*2+1]=H[d[i]&15]; }
  s[32]=0;
  return vstr_owned(s);
}
typedef struct { uint32_t h[5]; uint64_t bits; uint8_t buf[64]; int used; } GmlSha1;
static uint32_t sha1_rot(uint32_t x, unsigned n){ return (x<<n)|(x>>(32u-n)); }
static void sha1_transform(GmlSha1 *s, const uint8_t b[64]){
  uint32_t w[80];
  for(int i=0;i<16;i++)
    w[i]=((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|
         ((uint32_t)b[i*4+2]<<8)|(uint32_t)b[i*4+3];
  for(int i=16;i<80;i++) w[i]=sha1_rot(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
  uint32_t a=s->h[0],bb=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4];
  for(int i=0;i<80;i++){
    uint32_t f,k;
    if(i<20){ f=(bb&c)|((~bb)&d); k=0x5a827999u; }
    else if(i<40){ f=bb^c^d; k=0x6ed9eba1u; }
    else if(i<60){ f=(bb&c)|(bb&d)|(c&d); k=0x8f1bbcdcu; }
    else { f=bb^c^d; k=0xca62c1d6u; }
    uint32_t t=sha1_rot(a,5)+f+e+k+w[i];
    e=d; d=c; c=sha1_rot(bb,30); bb=a; a=t;
  }
  s->h[0]+=a; s->h[1]+=bb; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}
static void sha1_init(GmlSha1 *s){
  s->h[0]=0x67452301u; s->h[1]=0xefcdab89u; s->h[2]=0x98badcfeu;
  s->h[3]=0x10325476u; s->h[4]=0xc3d2e1f0u; s->bits=0; s->used=0;
}
static void sha1_update(GmlSha1 *s, const uint8_t *p, size_t n){
  s->bits+=(uint64_t)n*8u;
  while(n>0){
    size_t take=64u-(size_t)s->used;
    if(take>n) take=n;
    memcpy(s->buf+s->used,p,take);
    s->used+=(int)take; p+=take; n-=take;
    if(s->used==64){ sha1_transform(s,s->buf); s->used=0; }
  }
}
static void sha1_final(GmlSha1 *s, uint8_t out[20]){
  uint64_t bits=s->bits;
  s->buf[s->used++]=0x80;
  if(s->used>56){
    while(s->used<64) s->buf[s->used++]=0;
    sha1_transform(s,s->buf); s->used=0;
  }
  while(s->used<56) s->buf[s->used++]=0;
  for(int i=0;i<8;i++) s->buf[56+i]=(uint8_t)(bits>>(56-8*i));
  sha1_transform(s,s->buf);
  for(int i=0;i<5;i++){
    out[i*4]=(uint8_t)(s->h[i]>>24); out[i*4+1]=(uint8_t)(s->h[i]>>16);
    out[i*4+2]=(uint8_t)(s->h[i]>>8); out[i*4+3]=(uint8_t)s->h[i];
  }
}
GmlVal sha1_hex_val(const uint8_t *p, size_t n){
  static const char H[]="0123456789abcdef";
  uint8_t d[20]; GmlSha1 s;
  sha1_init(&s); if(p && n) sha1_update(&s,p,n); sha1_final(&s,d);
  char *out=malloc(41);
  if(!out) return vstr("");
  for(int i=0;i<20;i++){ out[i*2]=H[d[i]>>4]; out[i*2+1]=H[d[i]&15]; }
  out[40]=0;
  return vstr_owned(out);
}
char *base64_encode_alloc(const unsigned char *bytes, int length){
  static const char B64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if(length<0) length=0;
  char *out=malloc(((size_t)length+2)/3*4+1);
  if(!out) return NULL;
  char *p=out;
  int i=0;
  for(; i+3<=length; i+=3){
    uint32_t v=((uint32_t)bytes[i]<<16)|((uint32_t)bytes[i+1]<<8)|bytes[i+2];
    *p++=B64[(v>>18)&63]; *p++=B64[(v>>12)&63]; *p++=B64[(v>>6)&63]; *p++=B64[v&63];
  }
  if(length-i==1){ uint32_t v=(uint32_t)bytes[i]<<16;
    *p++=B64[(v>>18)&63]; *p++=B64[(v>>12)&63]; *p++='='; *p++='='; }
  else if(length-i==2){ uint32_t v=((uint32_t)bytes[i]<<16)|((uint32_t)bytes[i+1]<<8);
    *p++=B64[(v>>18)&63]; *p++=B64[(v>>12)&63]; *p++=B64[(v>>6)&63]; *p++='='; }
  *p=0;
  return out;
}
unsigned char *base64_decode_alloc(const char *s, int *out_len){
  size_t L=s?strlen(s):0;
  unsigned char *o=malloc(L/4*3+4);
  if(!o){ if(out_len) *out_len=0; return NULL; }
  unsigned char *p=o;
  int bits=0; uint32_t acc=0;
  for(size_t i=0;i<L;i++){
    unsigned char ch=(unsigned char)s[i];
    if(ch=='=') break;
    int c=ch>='A'&&ch<='Z'?ch-'A':
          ch>='a'&&ch<='z'?ch-'a'+26:
          ch>='0'&&ch<='9'?ch-'0'+52:
          ch=='+'?62:ch=='/'?63:-1;
    if(c<0) continue;
    acc=(acc<<6)|(uint32_t)c; bits+=6;
    if(bits>=8){ bits-=8; *p++=(unsigned char)((acc>>bits)&0xff); }
  }
  if(out_len) *out_len=(int)(p-o);
  return o;
}

static void async_saveload_queue(GmlVM *vm, int req, int ok){
  if(!vm || req<=0) return;
  if(vm->builtins->n_async_sl<16){
    vm->builtins->async_sl_q[vm->builtins->n_async_sl]=req;
    vm->builtins->async_sl_status[vm->builtins->n_async_sl]=ok ? 1 : 0;
    vm->builtins->n_async_sl++;
    if(builtin_setting(vm,"GML_LOG_ASYNC")){ 
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[async] f%ld queue id=%d status=%d n=%d\n",vm->frame,req,ok?1:0,vm->builtins->n_async_sl); }
  }
}

static int async_saveload_request(GmlVM *vm, int ok){
  if(!vm) return 0;
  if(vm->builtins->async_group_active){
    if(vm->builtins->async_group_id<=0) vm->builtins->async_group_id=++vm->builtins->async_seq;
    if(!ok) vm->builtins->async_group_status=0;
    vm->builtins->async_group_count++;
    if(builtin_setting(vm,"GML_LOG_ASYNC")){ 
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[async] f%ld grouped id=%d status=%d count=%d\n",
        vm->frame,vm->builtins->async_group_id,ok?1:0,vm->builtins->async_group_count); }
    return vm->builtins->async_group_id;
  }
  int req=++vm->builtins->async_seq;
  async_saveload_queue(vm, req, ok);
  return req;
}


GmlVal gml_builtin_try_io_ini(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- ini persistence ---- */
  if(!strcmp(nm,"ini_open")||!strcmp(nm,"FS_ini_open")) return builtin_ini_open_file(vm,a,n);
  if(!strcmp(nm,"ini_open_from_string")||!strcmp(nm,"FS_ini_open_from_string")){
    ini_reset(vm);
    vm->builtins->ini_open=1;
    ini_parse_text(vm,S(vm,a,n,0));
    return vreal(1);
  }
  if(!strcmp(nm,"ini_close")||!strcmp(nm,"FS_ini_close")){
    if(!vm->builtins->ini_open) return vreal(0);
    vm->builtins->ini_open=0;
    /* write the current key-value table back to the .ini file */
    if(vm->builtins->ini_path[0]){
      int id=vm_file_open(vm,vm->builtins->ini_path,"w");
      if(id>=0){
        int slot=id-1;
        const char *last_sec=NULL;
        for(int i=0;i<vm->builtins->ini_n;i++){
          if(!last_sec||strcmp(vm->builtins->ini_kv[i].section,last_sec)){
            vm_file_writef(vm,slot,"[%s]\n",vm->builtins->ini_kv[i].section); last_sec=vm->builtins->ini_kv[i].section;
          }
          if(vm->builtins->ini_kv[i].is_str) vm_file_writef(vm,slot,"%s=%s\n",vm->builtins->ini_kv[i].key,vm->builtins->ini_kv[i].sval?vm->builtins->ini_kv[i].sval:"");
          else vm_file_writef(vm,slot,"%s=%g\n",vm->builtins->ini_kv[i].key,vm->builtins->ini_kv[i].val);
        }
        vm_file_close(vm,slot);
      }
    }
    ini_reset(vm);
    return vreal(0);
  }
  if(!strcmp(nm,"ini_write_real")||!strcmp(nm,"FS_ini_write_real")){
    if(!vm->builtins->ini_open||vm->builtins->ini_n>=GML_INI_MAX) return vreal(0);
    const char *sec=S(vm,a,n,0), *key=S(vm,a,n,1); double val=N(a,n,2);
    /* replace existing key under the same section, or append */
    for(int i=0;i<vm->builtins->ini_n;i++)
      if(!strcmp(vm->builtins->ini_kv[i].section,sec) && !strcmp(vm->builtins->ini_kv[i].key,key)){
        free(vm->builtins->ini_kv[i].sval); vm->builtins->ini_kv[i].sval=NULL; vm->builtins->ini_kv[i].val=val; vm->builtins->ini_kv[i].is_str=0; return vreal(0); }
    vm->builtins->ini_kv[vm->builtins->ini_n++]=(typeof(vm->builtins->ini_kv[0])){.section=strdup(sec),.key=strdup(key),.val=val,.is_str=0};
    return vreal(0);
  }
  if(!strcmp(nm,"ini_write_string")||!strcmp(nm,"FS_ini_write_string")){
    if(!vm->builtins->ini_open||vm->builtins->ini_n>=GML_INI_MAX) return vreal(0);
    const char *sec=S(vm,a,n,0), *key=S(vm,a,n,1), *val=S(vm,a,n,2);
    for(int i=0;i<vm->builtins->ini_n;i++)
      if(!strcmp(vm->builtins->ini_kv[i].section,sec) && !strcmp(vm->builtins->ini_kv[i].key,key)){
        free(vm->builtins->ini_kv[i].sval); vm->builtins->ini_kv[i].sval=strdup(val); vm->builtins->ini_kv[i].val=ini_parse_real(val); vm->builtins->ini_kv[i].is_str=1; return vreal(0); }
    vm->builtins->ini_kv[vm->builtins->ini_n++]=(typeof(vm->builtins->ini_kv[0])){.section=strdup(sec),.key=strdup(key),.sval=strdup(val),.val=ini_parse_real(val),.is_str=1};
    return vreal(0);
  }
  if(!strcmp(nm,"ini_read_real")||!strcmp(nm,"FS_ini_read_real")){
    double def=N(a,n,2);
    if(n>2 && a[2].t==V_STR){
      def=ini_parse_real(S(vm,a,n,2));
    }
    if(!vm->builtins->ini_open) return vreal(def);  /* default */
    const char *sec=S(vm,a,n,0), *key=S(vm,a,n,1);
    /* search the key-value table backwards so later writes override */
    for(int i=vm->builtins->ini_n-1;i>=0;i--)
      if(!strcmp(vm->builtins->ini_kv[i].section,sec) && !strcmp(vm->builtins->ini_kv[i].key,key))
        return vreal(vm->builtins->ini_kv[i].val);
    return vreal(def);
  }
  if(!strcmp(nm,"ini_read_string")||!strcmp(nm,"FS_ini_read_string")){
    if(!vm->builtins->ini_open) return ini_default_string(vm,a,n);
    const char *sec=S(vm,a,n,0), *key=S(vm,a,n,1);
    for(int i=vm->builtins->ini_n-1;i>=0;i--) if(!strcmp(vm->builtins->ini_kv[i].section,sec) && !strcmp(vm->builtins->ini_kv[i].key,key)){
      /* Return an OWNED copy, not a pointer into ini_kv[].sval: a later ini_close()/ini_open() frees
       * that storage, and GML code routinely keeps the read value across such calls (use-after-free). */
      if(vm->builtins->ini_kv[i].is_str){ const char *s=vm->builtins->ini_kv[i].sval?vm->builtins->ini_kv[i].sval:""; char *c=strdup(s); return c?vstr_owned(c):vstr(""); }
      char b[64]; snprintf(b,sizeof b,"%g",vm->builtins->ini_kv[i].val); return vstr_owned(strdup(b)); }
    return ini_default_string(vm,a,n);
  }
  if(!strcmp(nm,"ini_section_exists")||!strcmp(nm,"FS_ini_section_exists")){
    if(!vm->builtins->ini_open) return vreal(0);
    const char *sec=S(vm,a,n,0);
    for(int i=0;i<vm->builtins->ini_n;i++) if(!strcmp(vm->builtins->ini_kv[i].section,sec)) return vreal(1);
    return vreal(0);
  }
  if(!strcmp(nm,"ini_key_exists")||!strcmp(nm,"FS_ini_key_exists")){
    if(!vm->builtins->ini_open) return vreal(0);
    const char *sec=S(vm,a,n,0), *key=S(vm,a,n,1);
    for(int i=vm->builtins->ini_n-1;i>=0;i--)
      if(!strcmp(vm->builtins->ini_kv[i].section,sec) && !strcmp(vm->builtins->ini_kv[i].key,key))
        return vreal(1);
    return vreal(0);
  }
  if(!strcmp(nm,"ini_key_delete")||!strcmp(nm,"FS_ini_key_delete")){
    if(!vm->builtins->ini_open) return vreal(0);
    const char *sec=S(vm,a,n,0), *key=S(vm,a,n,1);
    for(int i=0;i<vm->builtins->ini_n;i++){
      if(strcmp(vm->builtins->ini_kv[i].section,sec) || strcmp(vm->builtins->ini_kv[i].key,key)) continue;
      free(vm->builtins->ini_kv[i].section);
      free(vm->builtins->ini_kv[i].key);
      free(vm->builtins->ini_kv[i].sval);
      if(i+1<vm->builtins->ini_n) memmove(&vm->builtins->ini_kv[i],&vm->builtins->ini_kv[i+1],(size_t)(vm->builtins->ini_n-i-1)*sizeof(vm->builtins->ini_kv[0]));
      vm->builtins->ini_n--;
      memset(&vm->builtins->ini_kv[vm->builtins->ini_n],0,sizeof(vm->builtins->ini_kv[0]));
      return vreal(0);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"ini_section_delete")||!strcmp(nm,"FS_ini_section_delete")){
    if(!vm->builtins->ini_open) return vreal(0);
    const char *sec=S(vm,a,n,0);
    for(int i=0;i<vm->builtins->ini_n;){
      if(strcmp(vm->builtins->ini_kv[i].section,sec)){ i++; continue; }
      free(vm->builtins->ini_kv[i].section);
      free(vm->builtins->ini_kv[i].key);
      free(vm->builtins->ini_kv[i].sval);
      if(i+1<vm->builtins->ini_n) memmove(&vm->builtins->ini_kv[i],&vm->builtins->ini_kv[i+1],(size_t)(vm->builtins->ini_n-i-1)*sizeof(vm->builtins->ini_kv[0]));
      vm->builtins->ini_n--;
      memset(&vm->builtins->ini_kv[vm->builtins->ini_n],0,sizeof(vm->builtins->ini_kv[0]));
    }
    return vreal(0);
  }

  return gml_builtin_try_instances_rooms(vm,nm,a,n);
}


GmlVal gml_builtin_try_io(GmlVM *vm, const char *nm, GmlVal *a, int n){
  /* The high-score table retains ten places in score order. Reads beyond
   * populated places return zero or an empty string. */
  if(!strncmp(nm,"highscore_",10)){
    GmlBuiltinState *hs=builtin_state_ensure(vm);
    if(!hs) return vreal(0);
    if(!strcmp(nm,"highscore_clear")){
      for(int i=0;i<GML_HIGHSCORE_PLACES;i++){ hs->highscore[i].used=0; hs->highscore[i].score=0;
        hs->highscore[i].name[0]=0; }
      return vreal(0);
    }
    if(!strcmp(nm,"highscore_add")){
      const char *who=S(vm,a,n,0); double score=N(a,n,1);
      int at=GML_HIGHSCORE_PLACES;
      for(int i=0;i<GML_HIGHSCORE_PLACES;i++)
        if(!hs->highscore[i].used || score>hs->highscore[i].score){ at=i; break; }
      if(at>=GML_HIGHSCORE_PLACES) return vreal(0);
      for(int i=GML_HIGHSCORE_PLACES-1;i>at;i--) hs->highscore[i]=hs->highscore[i-1];
      hs->highscore[at].used=1;
      hs->highscore[at].score=score;
      snprintf(hs->highscore[at].name,sizeof hs->highscore[at].name,"%s",who?who:"");
      return vreal(0);
    }
    int place=(int)N(a,n,0)-1;   /* Places are one-based. */
    int have=place>=0 && place<GML_HIGHSCORE_PLACES && hs->highscore[place].used;
    if(!strcmp(nm,"highscore_value")) return vreal(have?hs->highscore[place].score:0.0);
    if(!strcmp(nm,"highscore_name")){
      /* Return owned storage: vstr retains a borrowed pointer to builtin state. */
      const char *who=have?hs->highscore[place].name:"";
      char *copy=(char*)malloc(strlen(who)+1);
      if(!copy) return vstr("");
      memcpy(copy,who,strlen(who)+1);
      return vstr_owned(copy);
    }
  }
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- file / buffer runtime I/O ---- */
  if(!strcmp(nm,"FS_set_gm_save_area")||!strcmp(nm,"FS_set_working_directory")) return vreal(1);
  if(!strcmp(nm,"FS_export_image")||!strcmp(nm,"FS_export_image_adv")||
     !strcmp(nm,"FS_export_raw")||!strcmp(nm,"FS_import_image")) return vreal(0);
  if(!strcmp(nm,"FS_unique_fname")){
    const char *raw=S(vm,a,n,0);
    if(!raw[0]) return vstr("");
    char *path=resolve_read_path(vm,raw);
    if(!path || !path_present(vm,path)){ free(path); return vstr_owned(strdup(raw)); }
    free(path);
    const char *slash=strrchr(raw,'/');
    const char *dot=strrchr(raw,'.');
    if(dot && slash && dot<slash) dot=NULL;
    size_t base_len=dot?(size_t)(dot-raw):strlen(raw);
    const char *ext=dot?dot:"";
    for(int k=1;k<10000;k++){
      char mid[32]; snprintf(mid,sizeof mid,"_%d",k);
      size_t need=base_len+strlen(mid)+strlen(ext)+1;
      char *cand=malloc(need);
      if(!cand) return vstr_owned(strdup(raw));
      memcpy(cand,raw,base_len); strcpy(cand+base_len,mid); strcat(cand,ext);
      char *full=resolve_read_path(vm,cand);
      int exists=full && path_present(vm,full);
      free(full);
      if(!exists) return vstr_owned(cand);
      free(cand);
    }
    return vstr_owned(strdup(raw));
  }
  if(!strcmp(nm,"file_exists")||!strcmp(nm,"FS_file_exists")){ char *path=resolve_read_path(vm,S(vm,a,n,0));
    AnygmFileInfo info; int ok=path && anygm_vfs_stat(vm->host,path,&info) &&
      (info.flags&ANYGM_FILE_INFO_REGULAR)!=0;
    if(builtin_setting(vm,"GML_LOG_IO"))
      anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_DEBUG,
                      "[io] file_exists raw=%s resolved=%s result=%d\n",
                      S(vm,a,n,0),path?path:"",ok);
    free(path); return vreal(ok); }
  if(!strcmp(nm,"directory_exists")){ char *path=resolve_read_path(vm,S(vm,a,n,0));
    AnygmFileInfo info; int ok=path && anygm_vfs_stat(vm->host,path,&info) &&
      (info.flags&ANYGM_FILE_INFO_DIRECTORY)!=0;
    free(path); return vreal(ok); }
  if(!strcmp(nm,"directory_create")){ char *path=resolve_write_path(vm,S(vm,a,n,0)); int ok=0;
    if(path && path[0]) ok=anygm_vfs_mkdirs(vm->host,path);
    free(path); return vreal(ok); }
  if(!strcmp(nm,"directory_destroy")){ char *path=resolve_write_path(vm,S(vm,a,n,0));
    int ok=path && vm->host && vm->host->path_remove &&
      vm->host->path_remove(vm->host->userdata,path)==ANYGM_OK; free(path); return vreal(ok); }
  if(!strcmp(nm,"file_delete")){ char *path=resolve_write_path(vm,S(vm,a,n,0));
    int ok=path && vm->host && vm->host->path_remove &&
      vm->host->path_remove(vm->host->userdata,path)==ANYGM_OK; free(path); return vreal(ok); }
  if(!strcmp(nm,"file_copy")||!strcmp(nm,"FS_file_copy")||!strcmp(nm,"FS_copy_fast")){
    char *src=resolve_read_path(vm,S(vm,a,n,0)); char *dst=resolve_write_path(vm,S(vm,a,n,1));
    int ok=copy_file_path(vm,src,dst);
    free(src); free(dst); return vreal(ok); }
  /* file_find_first/next/close scans a directory with a glob mask. Names are returned without the
   * directory, matching GM behavior, and an empty string marks the end. */
  if(!strcmp(nm,"file_find_first")){
    GmlBuiltinState *state=builtin_state_ensure(vm);
    if(!state) return vstr("");
    file_find_reset(state);
    char *mask=resolve_read_path(vm,S(vm,a,n,0));
    char *slash=strrchr(mask,'/');
    const char *pat = slash ? slash+1 : mask;
    char dirbuf[1024];
    if(slash){ size_t dl=(size_t)(slash-mask); if(dl>=sizeof dirbuf) dl=sizeof dirbuf-1;
      memcpy(dirbuf,mask,dl); dirbuf[dl]=0; } else snprintf(dirbuf,sizeof dirbuf,".");
    void *directory=vm->host && vm->host->directory_open?
      vm->host->directory_open(vm->host->userdata,dirbuf):NULL;
    if(directory){
      AnygmDirectoryEntry entry={.struct_size=sizeof entry};
      while(state->file_find_count<GML_FF_MAX &&
            vm->host->directory_read(vm->host->userdata,directory,&entry)==ANYGM_OK){
        if(entry.name[0]=='.') continue;
        if(wild_match(pat,entry.name))
          state->file_find_name[state->file_find_count++]=strdup(entry.name);
        entry.struct_size=sizeof entry;
      }
      if(vm->host->directory_close) vm->host->directory_close(vm->host->userdata,directory);
      /* Directory iteration order is host-dependent, so normalize it. */
      for(int i=0;i<state->file_find_count;i++) for(int j=i+1;j<state->file_find_count;j++)
        if(strcmp(state->file_find_name[j],state->file_find_name[i])<0){
          char *swap=state->file_find_name[i];
          state->file_find_name[i]=state->file_find_name[j];
          state->file_find_name[j]=swap;
        }
    }
    free(mask); state->file_find_index=0;
    return state->file_find_count>0
      ? vstr_owned(strdup(state->file_find_name[0])) : vstr("");
  }
  if(!strcmp(nm,"file_find_next")){
    GmlBuiltinState *state=builtin_state_ensure(vm);
    if(!state) return vstr("");
    state->file_find_index++;
    return state->file_find_index<state->file_find_count
      ? vstr_owned(strdup(state->file_find_name[state->file_find_index])) : vstr("");
  }
  if(!strcmp(nm,"file_find_close")){
    file_find_reset(vm?vm->builtins:NULL);
    return vreal(0);
  }
  if(!strcmp(nm,"file_rename")){ char *oldp=resolve_write_path(vm,S(vm,a,n,0)); char *newp=resolve_write_path(vm,S(vm,a,n,1));
    int ok=vm->host && vm->host->path_rename &&
      vm->host->path_rename(vm->host->userdata,oldp,newp)==ANYGM_OK;
    free(oldp); free(newp); return vreal(ok); }
  if(!strcmp(nm,"game_save")){
    char *path=resolve_write_path(vm,S(vm,a,n,0));
    size_t size=gml_vm_state_size(vm), written=0; unsigned char *data=size?malloc(size):NULL;
    int ok=path && data && gml_vm_state_save(vm,data,size,&written) && written==size;
    if(ok) ok=anygm_vfs_write_all(vm->host,path,data,written);
    free(data); free(path); (void)ok;
    return vreal(0);
  }
  if(!strcmp(nm,"game_load")){
    char *path=resolve_read_path(vm,S(vm,a,n,0));
    uint8_t *data=NULL; size_t length=0,used=0; int ok=0;
    if(path && anygm_vfs_read_all(vm->host,path,&data,&length,256u*1024u*1024u) && length)
      ok=gml_vm_state_load(vm,data,length,&used) && used==length;
    free(data); free(path); (void)ok;
    return vreal(0);
  }
  if(!strcmp(nm,"game_save_buffer")){
    int bi=vm_buffer_slot(vm,(int)N(a,n,0));
    if(bi<0) return vreal(0);
    size_t size=gml_vm_state_size(vm), written=0;
    if(size>(size_t)INT_MAX) return vreal(0);
    buffer_resize_slot(vm,bi,(int)size);
    if(vm->builtins->buffer[bi].data && gml_vm_state_save(vm,vm->builtins->buffer[bi].data,size,&written)){
      vm->builtins->buffer[bi].size=(int)written;
      vm->builtins->buffer[bi].pos=0;
      return vreal(1);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"game_load_buffer")){
    int bi=vm_buffer_slot(vm,(int)N(a,n,0));
    if(bi<0 || !vm->builtins->buffer[bi].data || vm->builtins->buffer[bi].size<=0) return vreal(0);
    size_t size=(size_t)vm->builtins->buffer[bi].size, used=0;
    /* State loading clears transient runtime buffers, including the source handle. */
    unsigned char *copy=malloc(size);
    if(!copy) return vreal(0);
    memcpy(copy,vm->builtins->buffer[bi].data,size);
    int ok=gml_vm_state_load(vm,copy,size,&used) && used==size;
    free(copy);
    return vreal(ok);
  }
  if(!strcmp(nm,"file_bin_open")||!strcmp(nm,"FS_file_bin_open")){ int mode=(int)N(a,n,1);
    char *path=mode==0?resolve_read_path(vm,S(vm,a,n,0)):resolve_write_path(vm,S(vm,a,n,0));
    const char *fm = mode==1 ? "wb+" : (mode==2 ? "ab+" : "rb");
    int id=vm_file_open(vm,path,fm); if(id<0 && mode==1) id=vm_file_open(vm,path,"wb+"); free(path); return vreal(id); }
  if(!strcmp(nm,"file_bin_close")||!strcmp(nm,"FS_file_bin_close")){ int i=vm_file_slot(vm,(int)N(a,n,0));
    if(i>=0) vm_file_close(vm,i);
    return vreal(0); }
  if(!strcmp(nm,"file_bin_size")||!strcmp(nm,"FS_file_bin_size")){ int i=vm_file_slot(vm,(int)N(a,n,0)); if(i<0) return vreal(0);
    int64_t sz=vm_file_size(vm,i);
    return vreal(sz<0?0:sz); }
  if(!strcmp(nm,"file_bin_seek")||!strcmp(nm,"FS_file_bin_seek")){ int i=vm_file_slot(vm,(int)N(a,n,0));
    if(i>=0){ int64_t p=(int64_t)N(a,n,1); if(p<0) p=0; vm_file_seek(vm,i,p,ANYGM_SEEK_START); } return vreal(0); }
  if(!strcmp(nm,"file_bin_read_byte")||!strcmp(nm,"FS_file_bin_read_byte")){ int i=vm_file_slot(vm,(int)N(a,n,0)); if(i<0) return vreal(0);
    int c=vm_file_getc(vm,i); return vreal(c==EOF?0:(c&0xff)); }
  if(!strcmp(nm,"file_bin_write_byte")||!strcmp(nm,"FS_file_bin_write_byte")){ int i=vm_file_slot(vm,(int)N(a,n,0));
    if(i>=0){ unsigned char byte=(unsigned char)((int)N(a,n,1)); vm_file_write(vm,i,&byte,1); } return vreal(0); }

  if(!strcmp(nm,"file_text_open_read")) return builtin_file_text_open_read(vm,a,n);
  if(!strcmp(nm,"file_text_open_write")){ char *path=resolve_write_path(vm,S(vm,a,n,0)); int id=vm_file_open(vm,path,"w"); free(path); return vreal(id); }
  if(!strcmp(nm,"file_text_open_append")){ char *path=resolve_write_path(vm,S(vm,a,n,0)); int id=vm_file_open(vm,path,"a+"); free(path); return vreal(id); }
  if(!strcmp(nm,"file_text_close")){ int i=vm_file_slot(vm,(int)N(a,n,0));
    if(i>=0) vm_file_close(vm,i);
    return vreal(0); }
  if(!strcmp(nm,"file_text_eof")){ int i=vm_file_slot(vm,(int)N(a,n,0)); if(i<0) return vreal(1);
    int c=vm_file_getc(vm,i); if(c==EOF) return vreal(1); vm_file_ungetc(vm,i,c); return vreal(0); }
  if(!strcmp(nm,"file_text_read_real")){ int i=vm_file_slot(vm,(int)N(a,n,0)); double v=0; if(i>=0) vm_file_read_real(vm,i,&v); return vreal(v); }
  if(!strcmp(nm,"file_text_read_string")){
    /* GM reads the rest of the line verbatim, including leading spaces, and leaves the newline for
     * file_text_readln. Use a dynamic buffer because a logical line can be much larger than a small
     * stack buffer. */
    return builtin_file_text_read_string(vm,a,n); }
  if(!strcmp(nm,"file_text_readln")){
    /* GM returns the rest of the current line without its newline, then advances past the newline.
     * An advance-only implementation loses data for callers that consume readln directly. */
    return builtin_file_text_readln(vm,a,n); }
  if(!strcmp(nm,"file_text_write_real")){ int i=vm_file_slot(vm,(int)N(a,n,0)); if(i>=0) vm_file_writef(vm,i,"%g",N(a,n,1)); return vreal(0); }
  if(!strcmp(nm,"file_text_write_string")){ int i=vm_file_slot(vm,(int)N(a,n,0)); const char *text=S(vm,a,n,1);
    if(i>=0) vm_file_write(vm,i,text,strlen(text));
    return vreal(0); }
  if(!strcmp(nm,"file_text_writeln")){ int i=vm_file_slot(vm,(int)N(a,n,0)); if(i>=0) vm_file_write(vm,i,"\n",1); return vreal(0); }

  if(!strcmp(nm,"buffer_create")){ int id=buffer_alloc(vm,(int)N(a,n,0)); return vreal(id); }
  if(!strcmp(nm,"buffer_exists")) return vreal(vm_buffer_slot(vm,(int)N(a,n,0))>=0);
  if(!strcmp(nm,"buffer_base64_decode")){
    int len=0; unsigned char *bytes=base64_decode_alloc(S(vm,a,n,0),&len);
    if(!bytes) return vreal(-1);
    int id=buffer_alloc(vm,len>0?len:1);
    int bi=vm_buffer_slot(vm,id);
    if(bi>=0){
      buffer_resize_slot(vm,bi,len);
      if(len>0) memcpy(vm->builtins->buffer[bi].data,bytes,(size_t)len);
      vm->builtins->buffer[bi].pos=0;
    }
    free(bytes);
    return vreal(id);
  }
  if(!strcmp(nm,"buffer_base64_encode")){
    /* Encode the selected range of an existing buffer. A negative length
     * consumes the remaining bytes from the clamped offset. */
    int i=vm_buffer_slot(vm,(int)N(a,n,0));
    if(i<0) return vstr("");
    int size=vm->builtins->buffer[i].size;
    int off=(int)N(a,n,1), len=(int)N(a,n,2);
    if(off<0) off=0;
    if(off>size) off=size;
    if(len<0 || off+len>size) len=size-off;
    char *text=base64_encode_alloc(vm->builtins->buffer[i].data+off,len);
    return text ? vstr_owned(text) : vstr("");
  }
  if(!strcmp(nm,"buffer_delete")){ int i=vm_buffer_slot(vm,(int)N(a,n,0));
    if(i>=0){ free(vm->builtins->buffer[i].data); memset(&vm->builtins->buffer[i],0,sizeof(vm->builtins->buffer[i])); } return vreal(0); }
  if(!strcmp(nm,"buffer_get_size")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); return vreal(i>=0?vm->builtins->buffer[i].size:0); }
  if(!strcmp(nm,"buffer_md5")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); if(i<0) return md5_hex_val(NULL,0);
    int off=(int)N(a,n,1), sz=(int)N(a,n,2);
    if(off<0) off=0;
    if(off>vm->builtins->buffer[i].size) off=vm->builtins->buffer[i].size;
    if(sz<0 || off+sz>vm->builtins->buffer[i].size) sz=vm->builtins->buffer[i].size-off;
    return md5_hex_val(vm->builtins->buffer[i].data+off,(size_t)sz); }
  if(!strcmp(nm,"buffer_get_address")){ int id=(int)N(a,n,0), i=vm_buffer_slot(vm,id);
    return vreal(i>=0 ? (double)(0xB0000000u | (uint32_t)(id&0xFFFF)) : 0); }
  if(!strcmp(nm,"buffer_tell")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); return vreal(i>=0?vm->builtins->buffer[i].pos:0); }
  if(!strcmp(nm,"buffer_resize")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); if(i>=0) buffer_resize_slot(vm,i,(int)N(a,n,1)); return vreal(0); }
  if(!strcmp(nm,"buffer_seek")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); if(i>=0){
      int base=(int)N(a,n,1), off=(int)N(a,n,2), pos=off;
      if(base==1) pos=vm->builtins->buffer[i].pos+off; else if(base==2) pos=vm->builtins->buffer[i].size+off;
      if(pos<0) pos=0;
      if(pos>vm->builtins->buffer[i].size) pos=vm->builtins->buffer[i].size;
      vm->builtins->buffer[i].pos=pos; }
    return vreal(0); }
  if(!strcmp(nm,"buffer_copy")){
    int si=vm_buffer_slot(vm,(int)N(a,n,0)), so=(int)N(a,n,1), bytes=(int)N(a,n,2);
    int di=vm_buffer_slot(vm,(int)N(a,n,3)), doff=(int)N(a,n,4);
    if(si>=0 && di>=0 && so>=0 && doff>=0 && bytes>0){
      if(so+bytes>vm->builtins->buffer[si].size) bytes=vm->builtins->buffer[si].size-so;
      if(bytes>0){
        unsigned char *tmp=malloc((size_t)bytes);
        if(tmp){ memcpy(tmp,vm->builtins->buffer[si].data+so,(size_t)bytes);
          buffer_write_at(vm,di,doff,tmp,bytes); free(tmp); }
      }
    }
    return vreal(0);
  }
  if(!strcmp(nm,"buffer_poke")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); if(i>=0)
      buffer_write_typed_at(vm,i,(int)N(a,n,1),(int)N(a,n,2),S(vm,a,n,3),N(a,n,3));
    return vreal(0); }
  if(!strcmp(nm,"buffer_fill")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); int off=(int)N(a,n,1), type=(int)N(a,n,2), bytes=(int)N(a,n,4);
    if(i>=0 && off>=0 && bytes>0){
      int p=off, end=off+bytes;
      while(p<end){ int w=buffer_write_typed_at(vm,i,p,type,S(vm,a,n,3),N(a,n,3)); if(w<=0) break; p+=w; }
    }
    return vreal(0); }
  /* GM buffer types: 1 u8, 2 s8, 3 u16, 4 s16, 5 u32, 6 s32, 7 f16, 8 f32, 9 f64,
   * 10 bool, 11 NUL-terminated string, 12 u64, and 13 unterminated text. */
  if(!strcmp(nm,"buffer_write")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); if(i<0) return vreal(0);
    int type=(int)N(a,n,1);
    if(type==11 || type==13){ const char *s=S(vm,a,n,2); buffer_write_raw(vm,i,s,(int)strlen(s)+(type==11)); return vreal(0); }
    double dv=N(a,n,2);
    if(type==1||type==2||type==10){ uint8_t v=(uint8_t)((int)dv); buffer_write_raw(vm,i,&v,1); }
    else if(type==3||type==4){ uint16_t v=(uint16_t)((int)dv); buffer_write_raw(vm,i,&v,2); }
    else if(type==5||type==6){ uint32_t v=(uint32_t)((int32_t)dv); buffer_write_raw(vm,i,&v,4); }
    else if(type==8){ float v=(float)dv; buffer_write_raw(vm,i,&v,4); }
    else if(type==7){ uint16_t v=0; float f=(float)dv;   /* f16: crude truncation via f32 bits */
      uint32_t u; memcpy(&u,&f,4); v=(uint16_t)(((u>>16)&0x8000)|((((u>>23)&0xFF)-112)<<10&0x7C00)|((u>>13)&0x3FF));
      buffer_write_raw(vm,i,&v,2); }
    else if(type==12){ uint64_t v=(uint64_t)(int64_t)dv; buffer_write_raw(vm,i,&v,8); }
    else { double v=dv; buffer_write_raw(vm,i,&v,8); }
    return vreal(0); }
  if(!strcmp(nm,"buffer_peek")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); if(i<0) return vreal(0);
    return buffer_read_typed_at_pos(vm,i,(int)N(a,n,1),(int)N(a,n,2)); }
  if(!strcmp(nm,"buffer_read")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); if(i<0) return vreal(0);
    int type=(int)N(a,n,1);
    if(type==11){ int p=vm->builtins->buffer[i].pos, e=p;   /* buffer_string: NUL-terminated */
      while(e<vm->builtins->buffer[i].size && vm->builtins->buffer[i].data[e]) e++;
      char *s=dup_n((char*)vm->builtins->buffer[i].data+p,e-p);
      vm->builtins->buffer[i].pos=(e<vm->builtins->buffer[i].size)?e+1:e; return vstr_owned(s); }
    if(type==13){ int p=vm->builtins->buffer[i].pos, e=vm->builtins->buffer[i].size;   /* buffer_text: to end */
      char *s=dup_n((char*)vm->builtins->buffer[i].data+p,e-p);
      vm->builtins->buffer[i].pos=e; return vstr_owned(s); }
    if(type==1||type==10) return vreal((uint8_t)buffer_read_u(vm,i,1));
    if(type==2) return vreal((int8_t)buffer_read_u(vm,i,1));
    if(type==3) return vreal((uint16_t)buffer_read_u(vm,i,2));
    if(type==4) return vreal((int16_t)buffer_read_u(vm,i,2));
    if(type==5) return vreal((uint32_t)buffer_read_u(vm,i,4));
    if(type==6) return vreal((int32_t)buffer_read_u(vm,i,4));
    if(type==7){ uint16_t h=(uint16_t)buffer_read_u(vm,i,2);   /* f16 -> f32 */
      uint32_t sgn=(h&0x8000)<<16, ex=(h>>10)&0x1F, mn=h&0x3FF;
      uint32_t u = ex==0 ? sgn : (sgn|((ex+112)<<23)|(mn<<13));
      float f; memcpy(&f,&u,4); return vreal(f); }
    if(type==8){ uint32_t u=(uint32_t)buffer_read_u(vm,i,4); float f; memcpy(&f,&u,4); return vreal(f); }
    if(type==12){ uint64_t u=buffer_read_u(vm,i,8); return vreal((double)(int64_t)u); }
    uint64_t u=buffer_read_u(vm,i,8); double d; memcpy(&d,&u,8); return vreal(d); }
  /* Async buffer file I/O reads into the caller's buffer and returns a request id. I/O is completed
   * synchronously, but the Async Save/Load event is deferred until the end of the step so the caller
   * can store the returned id before its handler observes async_load. */
  if(!strcmp(nm,"buffer_save_async")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); const char *raw=S(vm,a,n,1);
    int ok=0;
    if(i>=0 && raw[0]){ char *path=resolve_write_path(vm,raw); int off=(int)N(a,n,2), sz=(int)N(a,n,3);
      if(off<0) off=0;
      if(sz<=0||off+sz>vm->builtins->buffer[i].size) sz=vm->builtins->buffer[i].size-off;
      if(sz>=0) ok=anygm_vfs_write_all(vm->host,path,vm->builtins->buffer[i].data+off,(size_t)sz);
      free(path); }
    int req=async_saveload_request(vm, ok);
    return vreal(req); }
  if(!strcmp(nm,"buffer_load_async")){ int i=vm_buffer_slot(vm,(int)N(a,n,0));
    char *path=resolve_read_path(vm,S(vm,a,n,1)); int off=(int)N(a,n,2);
    if(off<0) off=0;
    int ok=0; uint8_t *data=NULL; size_t size=0;
    if(path && i>=0 && anygm_vfs_read_all(vm->host,path,&data,&size,256u*1024u*1024u)){
      if(size==0){
        vm->builtins->buffer[i].pos=0;
        ok=1;
      } else if(size<=(size_t)(INT_MAX-off)){
        int count=(int)size;
        if(off+count > vm->builtins->buffer[i].cap){
          int nc=off+count; unsigned char *nd=realloc(vm->builtins->buffer[i].data,(size_t)nc);
          if(nd){ vm->builtins->buffer[i].data=nd;
            if(nc>vm->builtins->buffer[i].cap) memset(nd+vm->builtins->buffer[i].cap,0,(size_t)(nc-vm->builtins->buffer[i].cap));
            vm->builtins->buffer[i].cap=nc; }
        }
        if(off+count <= vm->builtins->buffer[i].cap){
          memcpy(vm->builtins->buffer[i].data+off,data,size);
          if(off+count > vm->builtins->buffer[i].size) vm->builtins->buffer[i].size=off+count;
          vm->builtins->buffer[i].pos=0; ok=1;
        }
      }
    }
    free(data);
    free(path);
    int req=async_saveload_request(vm, ok);
    return vreal(req); }
  /* Synchronous buffer file I/O. */
  if(!strcmp(nm,"buffer_save")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); const char *raw=S(vm,a,n,1);
    if(i>=0 && raw[0]){ char *path=resolve_write_path(vm,raw);
      anygm_vfs_write_all(vm->host,path,vm->builtins->buffer[i].data,(size_t)vm->builtins->buffer[i].size);
      free(path); }
    return vreal(0); }
  if(!strcmp(nm,"buffer_save_ext")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); const char *raw=S(vm,a,n,1);
    if(i>=0 && raw[0]){ char *path=resolve_write_path(vm,raw); int off=(int)N(a,n,2), sz=(int)N(a,n,3);
      if(off<0) off=0;
      if(sz<=0||off+sz>vm->builtins->buffer[i].size) sz=vm->builtins->buffer[i].size-off;
      if(sz>=0) anygm_vfs_write_all(vm->host,path,vm->builtins->buffer[i].data+off,(size_t)sz);
      free(path); }
    return vreal(0); }
  if(!strcmp(nm,"buffer_load")){ char *path=resolve_read_path(vm,S(vm,a,n,0)); uint8_t *data=NULL; size_t size=0;
    if(!path || !anygm_vfs_read_all(vm->host,path,&data,&size,256u*1024u*1024u) || size>INT_MAX){ free(data); free(path); return vreal(-1); }
    int id=buffer_alloc(vm,size>0?(int)size:1); int i=vm_buffer_slot(vm,id);
    if(i>=0){ buffer_resize_slot(vm,i,(int)size); if(size) memcpy(vm->builtins->buffer[i].data,data,size); vm->builtins->buffer[i].pos=0; }
    free(data); free(path); return vreal(id); }
  if(!strcmp(nm,"buffer_load_ext")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); char *path=resolve_read_path(vm,S(vm,a,n,1)); int off=(int)N(a,n,2);
    uint8_t *data=NULL; size_t size=0; if(off<0) off=0;
    if(i>=0 && path && anygm_vfs_read_all(vm->host,path,&data,&size,256u*1024u*1024u) &&
       size<=(size_t)(INT_MAX-off) && off+(int)size<=vm->builtins->buffer[i].size && size)
      memcpy(vm->builtins->buffer[i].data+off,data,size);
    free(data); free(path); return vreal(0); }
  if(!strcmp(nm,"buffer_async_group_begin")){
    vm->builtins->async_group_active=1;
    vm->builtins->async_group_id=++vm->builtins->async_seq;
    vm->builtins->async_group_status=1;
    vm->builtins->async_group_count=0;
    return vreal(0);
  }
  if(!strcmp(nm,"buffer_async_group_end")){
    if(!vm->builtins->async_group_active) return vreal(0);
    int req=vm->builtins->async_group_id;
    int ok=vm->builtins->async_group_status;
    int count=vm->builtins->async_group_count;
    vm->builtins->async_group_active=0;
    vm->builtins->async_group_id=0;
    vm->builtins->async_group_status=1;
    vm->builtins->async_group_count=0;
    if(count>0) async_saveload_queue(vm, req, ok);
    return vreal(req);
  }
  if(!strcmp(nm,"buffer_async_group_option")) return vreal(0);

  return gml_builtin_try_instances_scripts(vm,nm,a,n);
}
