/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_classic_import.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC push_options
/* This bundled stb_image_write revision miscompiles its PNG compressor at
 * GCC -O2 (the encoded scanline contains allocator data after its first
 * pixel). Keep just the third-party implementation at its verified level. */
#pragma GCC optimize ("O1")
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_BMP
#define STBI_ONLY_PNG
#define STBI_NO_GIF
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC pop_options
#pragma GCC diagnostic pop
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "gml_default_font_data.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const uint8_t *data;
  size_t size, pos;
  char *err;
  size_t errcap;
} ImportReader;

static uint32_t import_u32_at(const uint8_t *p){
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int import_u32(ImportReader *r, uint32_t *value, const char *what){
  if(r->pos > r->size || r->size - r->pos < 4){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: truncated %s", what);
    return 0;
  }
  *value = import_u32_at(r->data + r->pos);
  r->pos += 4;
  return 1;
}

static int import_skip_words(ImportReader *r,uint64_t count,const char *what){
  if(r->pos>r->size || count>(r->size-r->pos)/4u){
    if(r->err&&r->errcap) snprintf(r->err,r->errcap,"classic import: truncated %s",what);
    return 0;
  }
  r->pos+=(size_t)count*4u;
  return 1;
}

static int import_blob(ImportReader *r, const uint8_t **data, uint32_t *size, const char *what){
  if(!import_u32(r, size, what)) return 0;
  if(r->pos > r->size || *size > r->size - r->pos){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: truncated %s data", what);
    return 0;
  }
  *data = r->data + r->pos;
  r->pos += *size;
  return 1;
}

static int import_skip_string(ImportReader *r, const uint8_t **text, uint32_t *length, const char *what){
  if(!import_u32(r, length, what)) return 0;
  if(r->pos > r->size || *length > r->size - r->pos){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: truncated %s", what);
    return 0;
  }
  if(text) *text = r->data + r->pos;
  r->pos += *length;
  return 1;
}

static int import_copy_string(ImportReader *r, char **text, const char *what){
  const uint8_t *bytes;
  uint32_t length;
  *text = NULL;
  if(!import_skip_string(r, &bytes, &length, what)) return 0;
  char *copy = (char*)malloc((size_t)length + 1);
  if(!copy){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: out of memory reading %s", what);
    return 0;
  }
  memcpy(copy, bytes, length);
  copy[length] = '\0';
  *text = copy;
  return 1;
}

static int import_double(ImportReader *r, double *value, const char *what){
  if(r->pos > r->size || r->size - r->pos < 8){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: truncated %s", what);
    return 0;
  }
  uint64_t bits = (uint64_t)r->data[r->pos] | (uint64_t)r->data[r->pos + 1] << 8 |
                  (uint64_t)r->data[r->pos + 2] << 16 | (uint64_t)r->data[r->pos + 3] << 24 |
                  (uint64_t)r->data[r->pos + 4] << 32 | (uint64_t)r->data[r->pos + 5] << 40 |
                  (uint64_t)r->data[r->pos + 6] << 48 | (uint64_t)r->data[r->pos + 7] << 56;
  memcpy(value, &bits, sizeof(bits));
  r->pos += 8;
  return 1;
}

static char *copy_string(const char *text){
  size_t length = text ? strlen(text) : 0;
  char *copy = (char*)malloc(length + 1);
  if(!copy) return NULL;
  if(length) memcpy(copy, text, length);
  copy[length] = '\0';
  return copy;
}

static char *cache_path(const char *dir, const char *leaf){
  size_t nd = strlen(dir), nl = strlen(leaf);
  int separator = nd && dir[nd - 1] != '/' && dir[nd - 1] != '\\';
  char *path = (char*)malloc(nd + (size_t)separator + nl + 1);
  if(!path) return NULL;
  memcpy(path, dir, nd);
  size_t at = nd;
  if(separator) path[at++] = '/';
  memcpy(path + at, leaf, nl + 1);
  return path;
}

static int write_source(const char *path, const char *source, char *err, size_t errcap){
  FILE *file = fopen(path, "wb");
  if(!file){
    if(err && errcap) snprintf(err, errcap, "classic import: cannot create %s: %s", path, strerror(errno));
    return 0;
  }
  size_t length = source ? strlen(source) : 0;
  int wrote = !length || fwrite(source, 1, length, file) == length;
  int closed = fclose(file) == 0;
  int ok = wrote && closed;
  if(!ok && err && errcap) snprintf(err, errcap, "classic import: cannot write %s", path);
  return ok;
}

static char *import_source_path(GmlcProject *project, const char *cache_dir,
                                const char *leaf, const char *source,
                                char *err, size_t errcap){
  if(project->prefer_memory_files){
    const char *text=source?source:"";
    char *path=gmlc_project_add_memory_file(project,leaf,GMLC_MEMORY_TEXT,
                                            text,strlen(text),0,0);
    if(!path && err && errcap)
      snprintf(err,errcap,"classic import: out of memory retaining %s",leaf);
    return path;
  }
  char *path=cache_path(cache_dir,leaf);
  if(!path || !write_source(path,source,err,errcap)){
    free(path);
    return NULL;
  }
  return path;
}

static void free_imported_scripts(GmlcProject *project){
  for(int i = 0; i < project->n_scripts; ++i){
    free(project->scripts[i].id);
    free(project->scripts[i].name);
    free(project->scripts[i].source_path);
  }
  free(project->scripts);
  project->scripts = NULL;
  project->n_scripts = project->cap_scripts = 0;
  for(int i = 0; i < project->n_script_order; ++i) free(project->script_order_ids[i]);
  free(project->script_order_ids);
  project->script_order_ids = NULL;
  project->n_script_order = 0;
}

int gmlc_classic_import_scripts(const GmlcClassicManifest *classic,
                                GmlcProject *project, const char *cache_dir,
                                char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->scripts || project->n_scripts){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid script-import arguments");
    return 0;
  }
  uint32_t count = classic->inventory.resource_slots[GMLC_CLASSIC_SCRIPT];
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many script slots");
    return 0;
  }
  project->scripts = (GmlcScript*)calloc(count ? count : 1, sizeof(*project->scripts));
  project->script_order_ids = (char**)calloc(count ? count : 1, sizeof(*project->script_order_ids));
  if(!project->scripts || !project->script_order_ids){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating scripts");
    free_imported_scripts(project);
    return 0;
  }
  project->n_scripts = project->cap_scripts = (int)count;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_SCRIPT];
  for(uint32_t i = 0; i < count; ++i){
    if(!slots || !slots[i].exists) continue;
    char leaf[64];
    snprintf(leaf, sizeof(leaf), "classic_script_%06u.gml", i);
    const char *name = slots[i].name ? slots[i].name : "";
    const char *source = slots[i].source ? slots[i].source : "exit;\n";
    GmlcScript *script = &project->scripts[i];
    script->id = copy_string(name);
    script->name = copy_string(name);
    script->source_path = import_source_path(project,cache_dir,leaf,source,err,errcap);
    if(!script->id || !script->name || !script->source_path){
      if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: out of memory importing script %u", i);
      free_imported_scripts(project);
      return 0;
    }
    project->script_order_ids[project->n_script_order] = copy_string(name);
    if(!project->script_order_ids[project->n_script_order]){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory recording script order");
      free_imported_scripts(project);
      return 0;
    }
    ++project->n_script_order;
  }
  return 1;
}

enum {
  CLASSIC_EXTENSION_FILE_LIMIT = 16 * 1024 * 1024,
  CLASSIC_EXTENSION_COMPRESSED_SCRIPT_LIMIT = 4 * 1024 * 1024,
  CLASSIC_EXTENSION_SCRIPT_LIMIT = 8 * 1024 * 1024
};

typedef struct {
  const uint8_t *data;
  size_t size, pos;
  int oom;
} ClassicExtensionReader;

typedef struct {
  char *public_name;
  char *target_name;
  uint32_t file_index;
} ClassicExtensionAlias;



static int classic_extension_skip(ClassicExtensionReader *reader, size_t size){
  if(reader->pos>reader->size || size>reader->size-reader->pos) return 0;
  reader->pos+=size;
  return 1;
}



static int classic_extension_skip_string(ClassicExtensionReader *reader){
  uint32_t length=0;
  return classic_extension_u32(reader,&length) && length<=1024u*1024u &&
         classic_extension_skip(reader,length);
}

static int classic_extension_skip_blob(ClassicExtensionReader *reader){
  uint32_t size=0;
  return classic_extension_u32(reader,&size) && classic_extension_skip(reader,size);
}



static int classic_extension_identifier_equal(const char *left, const char *right){
  while(left && right && *left && *right){
    unsigned char a=(unsigned char)*left++, b=(unsigned char)*right++;
    if(a>='A' && a<='Z') a=(unsigned char)(a-'A'+'a');
    if(b>='A' && b<='Z') b=(unsigned char)(b-'A'+'a');
    if(a!=b) return 0;
  }
  return left && right && *left==*right;
}

static int classic_extension_normalize(const char *source, char *out, size_t capacity){
  size_t length=0;
  if(!out || !capacity) return 0;
  for(const unsigned char *p=(const unsigned char*)source; p && *p; p++){
    unsigned char c=*p;
    if(c>='A' && c<='Z') c=(unsigned char)(c-'A'+'a');
    if((c>='a' && c<='z') || (c>='0' && c<='9')){
      if(length+1>=capacity){ out[0]='\0'; return 0; }
      out[length++]=(char)c;
    }
  }
  out[length]='\0';
  return 1;
}

static int classic_extension_named_by_project(const GmlcClassicManifest *classic,
  const char *package_name){
  char package_key[256];
  if(!classic_extension_normalize(package_name,package_key,sizeof(package_key)) ||
     !package_key[0]) return 0;
  for(uint32_t i=0;i<classic->extension_count;i++){
    char project_key[256];
    if(classic_extension_normalize(classic->extension_names[i],project_key,sizeof(project_key)) &&
       project_key[0] && !strcmp(package_key,project_key)) return 1;
  }
  return 0;
}

static const char *classic_extension_script_target(const GmlcProject *project,
                                                   const char *name){
  for(int i=0;i<project->n_scripts;i++){
    const char *candidate=project->scripts[i].name;
    if(candidate && classic_extension_identifier_equal(candidate,name)) return candidate;
  }
  return NULL;
}

static int classic_extension_span_identifier_equal(const char *span, size_t length,
                                                    const char *name){
  size_t name_length=name?strlen(name):0;
  if(length!=name_length) return 0;
  for(size_t i=0;i<length;i++){
    unsigned char a=(unsigned char)span[i], b=(unsigned char)name[i];
    if(a>='A' && a<='Z') a=(unsigned char)(a-'A'+'a');
    if(b>='A' && b<='Z') b=(unsigned char)(b-'A'+'a');
    if(a!=b) return 0;
  }
  return 1;
}

static int classic_extension_identifier_char(unsigned char c){
  return (c>='a' && c<='z') || (c>='A' && c<='Z') ||
         (c>='0' && c<='9') || c=='_';
}

static int classic_extension_define(const char *source, size_t start, size_t end,
                                    size_t *name_start, size_t *name_length){
  size_t at=start;
  while(at<end && (source[at]==' ' || source[at]=='\t')) at++;
  if(at+7u>end || source[at]!='#' ||
     (source[at+1]!='d' && source[at+1]!='D') ||
     (source[at+2]!='e' && source[at+2]!='E') ||
     (source[at+3]!='f' && source[at+3]!='F') ||
     (source[at+4]!='i' && source[at+4]!='I') ||
     (source[at+5]!='n' && source[at+5]!='N') ||
     (source[at+6]!='e' && source[at+6]!='E')) return 0;
  at+=7;
  if(at>=end || (source[at]!=' ' && source[at]!='\t')) return 0;
  while(at<end && (source[at]==' ' || source[at]=='\t')) at++;
  size_t first=at;
  while(at<end && classic_extension_identifier_char((unsigned char)source[at])) at++;
  if(at==first) return 0;
  *name_start=first;
  *name_length=at-first;
  return 1;
}

static int classic_extension_definition(const char *source, size_t size,
                                        const char *target,
                                        const char **body, size_t *body_size){
  size_t line=0;
  if(size>=3u && (unsigned char)source[0]==0xefu &&
     (unsigned char)source[1]==0xbbu && (unsigned char)source[2]==0xbfu) line=3;
  while(line<size){
    size_t end=line;
    while(end<size && source[end]!='\n') end++;
    size_t text_end=end;
    if(text_end>line && source[text_end-1]=='\r') text_end--;
    size_t name_start=0, name_length=0;
    if(classic_extension_define(source,line,text_end,&name_start,&name_length) &&
       classic_extension_span_identifier_equal(source+name_start,name_length,target)){
      size_t first=end<size ? end+1u : end;
      size_t next=first;
      while(next<size){
        size_t next_end=next;
        while(next_end<size && source[next_end]!='\n') next_end++;
        size_t next_text_end=next_end;
        if(next_text_end>next && source[next_text_end-1]=='\r') next_text_end--;
        if(classic_extension_define(source,next,next_text_end,&name_start,&name_length)) break;
        next=next_end<size ? next_end+1u : next_end;
      }
      *body=source+first;
      *body_size=next-first;
      return 1;
    }
    line=end<size ? end+1u : end;
  }
  return 0;
}

static int classic_extension_add_project_script(GmlcProject *project,
                                                 const char *target,
                                                 const char *source,
                                                 size_t source_size){
  if(project->n_scripts==INT32_MAX || project->n_script_order==INT32_MAX ||
     source_size==SIZE_MAX) return 0;
  char *text=(char*)malloc(source_size+1u);
  if(!text) return 0;
  memcpy(text,source,source_size);
  text[source_size]='\0';
  char leaf[80];
  snprintf(leaf,sizeof(leaf),"classic_extension_script_%06d.gml",project->n_scripts);
  char *id=copy_string(target), *name=copy_string(target);
  char *source_path=gmlc_project_add_memory_file(project,leaf,GMLC_MEMORY_TEXT,
                                                  text,source_size,0,0);
  char *order_id=copy_string(target);
  free(text);
  if(!id || !name || !source_path || !order_id){
    free(id); free(name); free(source_path); free(order_id);
    return 0;
  }
  if(project->n_scripts>=project->cap_scripts){
    if(project->cap_scripts>INT32_MAX/2){
      free(id); free(name); free(source_path); free(order_id);
      return 0;
    }
    int capacity=project->cap_scripts ? project->cap_scripts*2 : 16;
    if(capacity<=project->cap_scripts || capacity<project->n_scripts+1){
      free(id); free(name); free(source_path); free(order_id);
      return 0;
    }
    GmlcScript *scripts=(GmlcScript*)realloc(project->scripts,
                                             (size_t)capacity*sizeof(*scripts));
    if(!scripts){
      free(id); free(name); free(source_path); free(order_id);
      return 0;
    }
    memset(scripts+project->cap_scripts,0,
           (size_t)(capacity-project->cap_scripts)*sizeof(*scripts));
    project->scripts=scripts;
    project->cap_scripts=capacity;
  }
  char **order=(char**)realloc(project->script_order_ids,
                              (size_t)(project->n_script_order+1)*sizeof(*order));
  if(!order){
    free(id); free(name); free(source_path); free(order_id);
    return 0;
  }
  project->script_order_ids=order;
  project->scripts[project->n_scripts]=(GmlcScript){id,name,source_path};
  project->script_order_ids[project->n_script_order]=order_id;
  project->n_scripts++;
  project->n_script_order++;
  return 1;
}

static void classic_extension_rollback_scripts(GmlcProject *project,
                                               int scripts_before,
                                               int order_before,
                                               int memory_before){
  for(int i=scripts_before;i<project->n_scripts;i++){
    free(project->scripts[i].id);
    free(project->scripts[i].name);
    free(project->scripts[i].source_path);
    memset(&project->scripts[i],0,sizeof(project->scripts[i]));
  }
  project->n_scripts=scripts_before;
  for(int i=order_before;i<project->n_script_order;i++){
    free(project->script_order_ids[i]);
    project->script_order_ids[i]=NULL;
  }
  project->n_script_order=order_before;
  for(int i=memory_before;i<project->n_memory_files;i++){
    free(project->memory_files[i].data);
    memset(&project->memory_files[i],0,sizeof(project->memory_files[i]));
  }
  project->n_memory_files=memory_before;
}

static int classic_extension_decode_script(ClassicExtensionReader *reader,
                                           char **source, size_t *source_size){
  uint8_t *compressed=NULL;
  uint32_t compressed_size=0;
  *source=NULL; *source_size=0;
  if(!classic_extension_blob(reader,&compressed,&compressed_size,
                             CLASSIC_EXTENSION_COMPRESSED_SCRIPT_LIMIT) ||
     !compressed_size) return 0;
  size_t capacity=(size_t)compressed_size*4u;
  if(capacity<4096u) capacity=4096u;
  if(capacity>CLASSIC_EXTENSION_SCRIPT_LIMIT) capacity=CLASSIC_EXTENSION_SCRIPT_LIMIT;
  char *decoded=NULL;
  int decoded_size=0;
  for(;;){
    decoded=(char*)malloc(capacity+1u);
    if(!decoded){ reader->oom=1; break; }
    decoded_size=stbi_zlib_decode_buffer(decoded,(int)capacity,
                                         (const char*)compressed,(int)compressed_size);
    if(decoded_size>0) break;
    free(decoded); decoded=NULL;
    if(capacity==CLASSIC_EXTENSION_SCRIPT_LIMIT) break;
    capacity*=2u;
    if(capacity>CLASSIC_EXTENSION_SCRIPT_LIMIT) capacity=CLASSIC_EXTENSION_SCRIPT_LIMIT;
  }
  free(compressed);
  if(!decoded) return 0;
  if(memchr(decoded,'\0',(size_t)decoded_size)){
    free(decoded);
    return 0;
  }
  decoded[decoded_size]='\0';
  *source=decoded;
  *source_size=(size_t)decoded_size;
  return 1;
}

static int classic_extension_add_project_alias(GmlcProject *project,
                                                const char *public_name,
                                                const char *target_name){
  for(int i=0;i<project->n_function_aliases;i++){
    GmlcFunctionAlias *alias=&project->function_aliases[i];
    if(!classic_extension_identifier_equal(alias->public_name,public_name)) continue;
    if(!classic_extension_identifier_equal(alias->target_name,target_name)) alias->ambiguous=1;
    return 1;
  }
  if(project->n_function_aliases>=project->cap_function_aliases){
    int capacity=project->cap_function_aliases ? project->cap_function_aliases*2 : 16;
    GmlcFunctionAlias *aliases=(GmlcFunctionAlias*)realloc(
      project->function_aliases,(size_t)capacity*sizeof(*aliases));
    if(!aliases) return 0;
    project->function_aliases=aliases;
    project->cap_function_aliases=capacity;
  }
  GmlcFunctionAlias *alias=&project->function_aliases[project->n_function_aliases];
  memset(alias,0,sizeof(*alias));
  alias->public_name=copy_string(public_name);
  alias->target_name=copy_string(target_name);
  if(!alias->public_name || !alias->target_name){
    free(alias->public_name); free(alias->target_name);
    memset(alias,0,sizeof(*alias));
    return 0;
  }
  project->n_function_aliases++;
  return 1;
}

static void classic_extension_free_aliases(ClassicExtensionAlias *aliases, int count){
  for(int i=0;i<count;i++){
    free(aliases[i].public_name);
    free(aliases[i].target_name);
  }
  free(aliases);
}

/* Return 1 for a parsed/irrelevant package, 0 for malformed input, and -1 for OOM. */


static int classic_extension_suffix(const char *name){
  size_t length=name?strlen(name):0;
  return length>=4 && name[length-4]=='.' &&
         (name[length-3]=='g' || name[length-3]=='G') &&
         (name[length-2]=='e' || name[length-2]=='E') &&
         (name[length-1]=='x' || name[length-1]=='X');
}

static int classic_extension_name_compare(const void *left, const void *right){
  const char *a=*(const char * const*)left;
  const char *b=*(const char * const*)right;
  int folded=strcasecmp(a,b);
  return folded ? folded : strcmp(a,b);
}

static void classic_extension_names_free(char **names, size_t count){
  for(size_t i=0;i<count;i++) free(names[i]);
  free(names);
}

static int classic_extension_names(const char *project_dir,
                                   char ***names_out, size_t *count_out){
  *names_out=NULL; *count_out=0;
  DIR *directory=opendir(project_dir);
  if(!directory) return 1;
  char **names=NULL;
  size_t count=0,capacity=0;
  struct dirent *entry;
  while((entry=readdir(directory))){
    if(!classic_extension_suffix(entry->d_name)) continue;
    if(count==capacity){
      size_t next=capacity ? capacity*2u : 8u;
      char **grown=(char**)realloc(names,next*sizeof(*grown));
      if(!grown){ classic_extension_names_free(names,count); closedir(directory); return 0; }
      names=grown; capacity=next;
    }
    names[count]=gmlc_strdup(entry->d_name);
    if(!names[count]){ classic_extension_names_free(names,count); closedir(directory); return 0; }
    count++;
  }
  closedir(directory);
  if(count>1) qsort(names,count,sizeof(*names),classic_extension_name_compare);
  *names_out=names; *count_out=count;
  return 1;
}

static int classic_extension_read_prefix(const char *path, uint8_t **data, size_t *size){
  *data=NULL; *size=0;
  FILE *file=fopen(path,"rb");
  if(!file) return 0;
  if(fseek(file,0,SEEK_END)!=0){ fclose(file); return 0; }
  long length=ftell(file);
  if(length<12 || fseek(file,0,SEEK_SET)!=0){ fclose(file); return 0; }
  size_t wanted=(size_t)length;
  if(wanted>CLASSIC_EXTENSION_FILE_LIMIT) wanted=CLASSIC_EXTENSION_FILE_LIMIT;
  uint8_t *bytes=(uint8_t*)malloc(wanted);
  if(!bytes){ fclose(file); return -1; }
  int ok=fread(bytes,1,wanted,file)==wanted;
  fclose(file);
  if(!ok){ free(bytes); return 0; }
  *data=bytes; *size=wanted;
  return 1;
}

static void classic_extension_hash_bytes(uint64_t *hash, const void *data, size_t size){
  const uint8_t *bytes=(const uint8_t*)data;
  for(size_t i=0;i<size;i++){
    *hash^=(uint64_t)bytes[i];
    *hash*=1099511628211ull;
  }
}

static void classic_extension_hash_u64(uint64_t *hash, uint64_t value){
  uint8_t bytes[8];
  for(unsigned i=0;i<8;i++) bytes[i]=(uint8_t)(value>>(i*8u));
  classic_extension_hash_bytes(hash,bytes,sizeof(bytes));
}

int gmlc_classic_extension_dependency_hash(const char *project_dir,
                                           uint64_t seed, uint64_t *hash_out){
  if(!project_dir || !*project_dir || !hash_out) return 0;
  char **names=NULL; size_t count=0;
  if(!classic_extension_names(project_dir,&names,&count)) return 0;
  uint64_t hash=seed;
  const uint8_t domain[4]={'G','E','X',0};
  classic_extension_hash_bytes(&hash,domain,sizeof(domain));
  uint64_t live=0;
  for(size_t i=0;i<count;i++){
    char *path=gmlc_path_join(project_dir,names[i]);
    uint8_t *data=NULL; size_t size=0;
    int read=path ? classic_extension_read_prefix(path,&data,&size) : -1;
    free(path);
    if(read<0){ free(data); classic_extension_names_free(names,count); return 0; }
    if(read==0){ free(data); continue; }
    live++;
    classic_extension_hash_u64(&hash,(uint64_t)size);
    classic_extension_hash_bytes(&hash,data,size);
    free(data);
  }
  classic_extension_hash_u64(&hash,live);
  classic_extension_names_free(names,count);
  *hash_out=hash;
  return 1;
}

int gmlc_classic_import_extension_aliases(const GmlcClassicManifest *classic,
                                          GmlcProject *project,
                                          const char *project_dir,
                                          char *err, size_t errcap){
  if(err && errcap) err[0]='\0';
  if(!classic || !project || !project_dir || !*project_dir){
    if(err && errcap) snprintf(err,errcap,"classic import: invalid extension arguments");
    return 0;
  }
  if(!classic->extension_count) return 1;
  int aliases_before=project->n_function_aliases;
  int scripts_before=project->n_scripts;
  char **names=NULL; size_t count=0;
  int ok=classic_extension_names(project_dir,&names,&count);
  for(size_t i=0;ok && i<count;i++){
    char *path=gmlc_path_join(project_dir,names[i]);
    if(!path){ ok=0; break; }
    uint8_t *data=NULL; size_t size=0;
    int read=classic_extension_read_prefix(path,&data,&size);
    free(path);
    if(read<0){ ok=0; break; }
    if(read>0){
      int parsed=classic_extension_parse(classic,project,data,size);
      free(data);
      if(getenv("GMLC_LOG_CLASSIC_EXTENSIONS"))
        fprintf(stderr,"classic extension package: %s (%s)\n",names[i],
                parsed>0?"parsed":parsed<0?"out of memory":"ignored malformed metadata");
      if(parsed<0){ ok=0; break; }
    }
  }
  classic_extension_names_free(names,count);
  if(!ok && err && errcap)
    snprintf(err,errcap,"classic import: out of memory reading extension metadata");
  if(ok && getenv("GMLC_LOG_CLASSIC_EXTENSIONS")){
    fprintf(stderr,"classic extensions: imported %d script(s), retained %d alias(es) from %s\n",
            project->n_scripts-scripts_before,
            project->n_function_aliases-aliases_before,project_dir);
    for(int i=aliases_before;i<project->n_function_aliases;i++){
      const GmlcFunctionAlias *alias=&project->function_aliases[i];
      fprintf(stderr,"classic extension alias: %s -> %s%s\n",
              alias->public_name,alias->target_name,alias->ambiguous?" (ambiguous)":"");
    }
  }
  return ok;
}

static void free_imported_sprites(GmlcProject *project){
  for(int i = 0; i < project->n_sprites; ++i){
    GmlcSprite *sprite = &project->sprites[i];
    free(sprite->id);
    free(sprite->name);
    for(int frame = 0; frame < sprite->n_frames; ++frame)
      free(sprite->frame_paths ? sprite->frame_paths[frame] : NULL);
    free(sprite->frame_paths);
  }
  free(project->sprites);
  project->sprites = NULL;
  project->n_sprites = project->cap_sprites = 0;
}

static int write_rgba_png(const char *path, const uint8_t *source_rgba, uint32_t bytes,
                          int width, int height, char *err, size_t errcap){
  int out_width = width > 0 ? width : 1;
  int out_height = height > 0 ? height : 1;
  if((size_t)out_width > SIZE_MAX / (size_t)out_height / 4u){
    if(err && errcap) snprintf(err, errcap, "classic import: sprite dimensions overflow");
    return 0;
  }
  size_t pixels = (size_t)out_width * (size_t)out_height;
  if(width > 0 && height > 0 && source_rgba && bytes < pixels * 4u){
    if(err && errcap) snprintf(err, errcap, "classic import: truncated sprite BGRA pixels");
    return 0;
  }
  uint8_t *rgba = (uint8_t*)calloc(pixels, 4);
  if(!rgba){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory converting sprite pixels");
    return 0;
  }
  if(width > 0 && height > 0 && source_rgba){
    memcpy(rgba, source_rgba, pixels * 4u);
  }
  int ok = stbi_write_png(path, out_width, out_height, 4, rgba, out_width * 4);
  free(rgba);
  if(!ok && err && errcap) snprintf(err, errcap, "classic import: cannot write %s", path);
  return ok != 0;
}

static char *import_rgba_path(GmlcProject *project, const char *cache_dir,
                              const char *leaf, const uint8_t *source_rgba,
                              uint32_t bytes, int width, int height,
                              char *err, size_t errcap){
  if(!project->prefer_memory_files){
    char *path=cache_path(cache_dir,leaf);
    if(!path || !write_rgba_png(path,source_rgba,bytes,width,height,err,errcap)){
      free(path);
      return NULL;
    }
    return path;
  }
  int out_width=width>0?width:1, out_height=height>0?height:1;
  if((size_t)out_width>SIZE_MAX/(size_t)out_height/4u){
    if(err && errcap) snprintf(err,errcap,"classic import: image dimensions overflow");
    return NULL;
  }
  size_t size=(size_t)out_width*(size_t)out_height*4u;
  if(bytes && bytes<size){
    if(err && errcap) snprintf(err,errcap,"classic import: truncated image pixels");
    return NULL;
  }
  uint8_t *zero=NULL;
  const void *pixels=source_rgba;
  if(width<=0 || height<=0 || !pixels){
    zero=(uint8_t*)calloc(size?size:1u,1);
    if(!zero) return NULL;
    pixels=zero;
  }
  char *path=gmlc_project_add_memory_file(project,leaf,GMLC_MEMORY_RGBA,pixels,size,
                                          out_width,out_height);
  free(zero);
  return path;
}

/* GM8 stores uncompressed project and executable image blobs in BGRA byte order.  The legacy
 * layouts above contain encoded bitmap files, which stb_image has already converted to RGBA, so
 * only raw blobs pass through this helper. */
static uint8_t *import_bgra_to_rgba(const uint8_t *bgra, uint32_t bytes,
                                    uint32_t width, uint32_t height,
                                    const char *what, char *err, size_t errcap){
  if(!bgra || !width || !height) return NULL;
  if((size_t)height>SIZE_MAX/(size_t)width){
    if(err && errcap) snprintf(err,errcap,"classic import: oversized %s",what);
    return NULL;
  }
  size_t count=(size_t)width*(size_t)height;
  if(count>SIZE_MAX/4u || bytes<count*4u){
    if(err && errcap) snprintf(err,errcap,"classic import: truncated %s BGRA pixels",what);
    return NULL;
  }
  uint8_t *rgba=(uint8_t*)malloc(count*4u);
  if(!rgba){
    if(err && errcap) snprintf(err,errcap,"classic import: out of memory converting %s pixels",what);
    return NULL;
  }
  for(size_t pixel=0;pixel<count;pixel++){
    rgba[pixel*4u]=bgra[pixel*4u+2u];
    rgba[pixel*4u+1u]=bgra[pixel*4u+1u];
    rgba[pixel*4u+2u]=bgra[pixel*4u];
    rgba[pixel*4u+3u]=bgra[pixel*4u+3u];
  }
  return rgba;
}

static int decode_legacy_image(ImportReader *r, int expected_width, int expected_height,
                               int transparent, uint8_t **rgba_out, uint32_t *bytes_out,
                               const char *what){
  uint32_t marker;
  *rgba_out=NULL; *bytes_out=0;
  if(!import_u32(r,&marker,what)) return 0;
  if(marker==UINT32_MAX) return 1;
  const uint8_t *compressed; uint32_t compressed_size;
  if(!import_blob(r,&compressed,&compressed_size,what) || compressed_size>INT32_MAX) return 0;
  int raw_size=0;
  char *raw=stbi_zlib_decode_malloc((const char*)compressed,(int)compressed_size,&raw_size);
  if(!raw || raw_size<=0){
    if(r->err && r->errcap) snprintf(r->err,r->errcap,"classic import: invalid compressed %s",what);
    STBI_FREE(raw); return 0;
  }
  int width=0,height=0,components=0;
  uint8_t *rgba=stbi_load_from_memory((const stbi_uc*)raw,raw_size,&width,&height,&components,4);
  STBI_FREE(raw);
  if(!rgba || width!=expected_width || height!=expected_height ||
     width<0 || height<0 || (size_t)width>UINT32_MAX/(size_t)(height?height:1)/4u){
    if(r->err && r->errcap) snprintf(r->err,r->errcap,"classic import: invalid %s dimensions",what);
    stbi_image_free(rgba); return 0;
  }
  uint32_t bytes=(uint32_t)((size_t)width*(size_t)height*4u);
  if(transparent && width>0 && height>0){
    const uint8_t *key=rgba+((size_t)(height-1)*(size_t)width)*4u;
    uint8_t kr=key[0],kg=key[1],kb=key[2];
    for(size_t p=0;p<(size_t)width*(size_t)height;p++){
      uint8_t *pixel=rgba+p*4u;
      if(pixel[0]==kr && pixel[1]==kg && pixel[2]==kb) pixel[3]=0;
    }
  }
  *rgba_out=rgba; *bytes_out=bytes;
  return 1;
}

int gmlc_classic_import_sprites(const GmlcClassicManifest *classic,
                                GmlcProject *project, const char *cache_dir,
                                char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->sprites || project->n_sprites){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid sprite-import arguments");
    return 0;
  }
  uint32_t slots_count = classic->inventory.resource_slots[GMLC_CLASSIC_SPRITE];
  uint32_t existing = classic->existing[GMLC_CLASSIC_SPRITE];
  if(existing > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many sprites");
    return 0;
  }
  project->sprites = (GmlcSprite*)calloc(existing ? existing : 1, sizeof(*project->sprites));
  if(!project->sprites){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating sprites");
    return 0;
  }
  project->cap_sprites = (int)existing;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_SPRITE];
  for(uint32_t slot_index = 0; slot_index < slots_count; ++slot_index){
    const GmlcClassicResourceSlot *source = &slots[slot_index];
    if(!source->exists) continue;
    GmlcSprite *sprite = &project->sprites[project->n_sprites++];
    sprite->id = copy_string(source->name);
    sprite->name = copy_string(source->name);
    sprite->runtime_id = (int)slot_index;
    ImportReader r = {source->payload, source->payload_size, 0, err, errcap};
    uint32_t xorigin, yorigin, frames;
    if(source->legacy_layout){
      uint32_t fields[13];
      for(int field=0;field<13;field++) if(!import_u32(&r,&fields[field],"legacy sprite field")){
        free_imported_sprites(project); return 0;
      }
      if(!import_u32(&r,&frames,"legacy sprite frame count") || frames>INT32_MAX ||
         fields[0]>INT32_MAX || fields[1]>INT32_MAX){
        free_imported_sprites(project); return 0;
      }
      sprite->width=(int)fields[0]; sprite->height=(int)fields[1];
      sprite->bbox_left=(int32_t)fields[2]; sprite->bbox_right=(int32_t)fields[3];
      sprite->bbox_bottom=(int32_t)fields[4]; sprite->bbox_top=(int32_t)fields[5];
      sprite->bbox_mode=(int32_t)fields[9]; sprite->col_kind=(int32_t)fields[10];
      sprite->xorig=(int32_t)fields[11]; sprite->yorig=(int32_t)fields[12];
      sprite->n_frames=(int)frames;
      sprite->frame_paths=(char**)calloc(frames?frames:1,sizeof(*sprite->frame_paths));
      if(!sprite->id || !sprite->name || !sprite->frame_paths){ free_imported_sprites(project); return 0; }
      for(uint32_t frame=0;frame<frames;frame++){
        uint8_t *rgba=NULL; uint32_t rgba_bytes=0;
        if(!decode_legacy_image(&r,sprite->width,sprite->height,fields[6]!=0,
                                &rgba,&rgba_bytes,"legacy sprite image")){
          free_imported_sprites(project); return 0;
        }
        char leaf[96];
        snprintf(leaf,sizeof(leaf),"classic_sprite_%06u_%06u.png",slot_index,frame);
        sprite->frame_paths[frame]=import_rgba_path(project,cache_dir,leaf,rgba,rgba_bytes,
                                                    sprite->width,sprite->height,err,errcap);
        int ok=sprite->frame_paths[frame]!=NULL;
        stbi_image_free(rgba);
        if(!ok){ free_imported_sprites(project); return 0; }
      }
      if(r.pos!=r.size){ if(err && errcap) snprintf(err,errcap,"classic import: trailing legacy sprite payload"); free_imported_sprites(project); return 0; }
      continue;
    }
    if(!sprite->id || !sprite->name || !import_u32(&r, &xorigin, "sprite x origin") ||
       !import_u32(&r, &yorigin, "sprite y origin") || !import_u32(&r, &frames, "sprite frame count") ||
       frames > INT32_MAX){
      if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: invalid sprite %u", slot_index);
      free_imported_sprites(project);
      return 0;
    }
    sprite->xorig = (int32_t)xorigin;
    sprite->yorig = (int32_t)yorigin;
    sprite->n_frames = (int)frames;
    sprite->frame_paths = (char**)calloc(frames ? frames : 1, sizeof(*sprite->frame_paths));
    if(!sprite->frame_paths){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating sprite frames");
      free_imported_sprites(project);
      return 0;
    }
    for(uint32_t frame = 0; frame < frames; ++frame){
      uint32_t frame_version, width, height, pixel_bytes = 0;
      const uint8_t *pixels = NULL;
      if(!import_u32(&r, &frame_version, "sprite frame version") ||
         !import_u32(&r, &width, "sprite width") || !import_u32(&r, &height, "sprite height") ||
         (width && height && !import_blob(&r, &pixels, &pixel_bytes, "sprite pixels")) ||
         width > INT32_MAX || height > INT32_MAX){
        free_imported_sprites(project);
        return 0;
      }
      (void)frame_version;
      if(frame == 0){ sprite->width = (int)width; sprite->height = (int)height; }
      if((int)width != sprite->width || (int)height != sprite->height){
        if(err && errcap) snprintf(err, errcap, "classic import: inconsistent dimensions in sprite slot %u", slot_index);
        free_imported_sprites(project);
        return 0;
      }
      char leaf[96];
      snprintf(leaf, sizeof(leaf), "classic_sprite_%06u_%06u.png", slot_index, frame);
      uint8_t *converted=NULL;
      const uint8_t *frame_pixels=pixels;
      if(pixels && width && height){
        converted=import_bgra_to_rgba(pixels,pixel_bytes,width,height,"sprite",err,errcap);
        if(!converted){ free_imported_sprites(project); return 0; }
        frame_pixels=converted;
      }
      sprite->frame_paths[frame] = import_rgba_path(project,cache_dir,leaf,frame_pixels,pixel_bytes,
                                                    (int)width,(int)height,err,errcap);
      free(converted);
      if(!sprite->frame_paths[frame]){
        free_imported_sprites(project);
        return 0;
      }
    }
    uint32_t collision[8]={0};
    if(source->executable_layout && frames){
      if(source->version>=810 && !import_skip_words(&r,1,"sprite collision shape")){
        free_imported_sprites(project); return 0;
      }
      uint32_t separate;
      if(!import_u32(&r,&separate,"sprite separate collision maps")){
        free_imported_sprites(project); return 0;
      }
      uint32_t maps=separate?frames:1;
      int32_t left=INT32_MAX,right=INT32_MIN,top=INT32_MAX,bottom=INT32_MIN;
      for(uint32_t map=0;map<maps;map++){
        uint32_t fields[7];
        for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);i++)
          if(!import_u32(&r,&fields[i],"sprite collision map")){
            free_imported_sprites(project); return 0;
          }
        uint64_t pixels=(uint64_t)fields[1]*(uint64_t)fields[2];
        if(!import_skip_words(&r,pixels,"sprite collision pixels")){
          free_imported_sprites(project); return 0;
        }
        if((int32_t)fields[3]<left) left=(int32_t)fields[3];
        if((int32_t)fields[4]>right) right=(int32_t)fields[4];
        if((int32_t)fields[6]<top) top=(int32_t)fields[6];
        if((int32_t)fields[5]>bottom) bottom=(int32_t)fields[5];
      }
      collision[0]=0; collision[1]=0; collision[2]=separate!=0; collision[3]=2;
      collision[4]=(uint32_t)(left==INT32_MAX?0:left);
      collision[5]=(uint32_t)(right==INT32_MIN?sprite->width-1:right);
      collision[6]=(uint32_t)(bottom==INT32_MIN?sprite->height-1:bottom);
      collision[7]=(uint32_t)(top==INT32_MAX?0:top);
    } else if(!source->executable_layout){
      for(int i = 0; i < 8; ++i)
        if(!import_u32(&r, &collision[i], "sprite collision field")){ free_imported_sprites(project); return 0; }
    }
    if(r.pos != r.size){
      if(err && errcap) snprintf(err, errcap, "classic import: trailing sprite payload");
      free_imported_sprites(project);
      return 0;
    }
    sprite->col_kind = (int32_t)collision[0];
    sprite->col_tolerance = (int32_t)collision[1];
    sprite->sep_masks = collision[2] != 0;
    sprite->bbox_mode = (int32_t)collision[3];
    sprite->bbox_left = (int32_t)collision[4];
    sprite->bbox_right = (int32_t)collision[5];
    sprite->bbox_bottom = (int32_t)collision[6];
    sprite->bbox_top = (int32_t)collision[7];
  }
  return 1;
}

static void free_sprite_range(GmlcProject *project, int first){
  for(int i = first; i < project->n_sprites; ++i){
    GmlcSprite *sprite = &project->sprites[i];
    free(sprite->id); free(sprite->name);
    for(int frame = 0; frame < sprite->n_frames; ++frame)
      free(sprite->frame_paths ? sprite->frame_paths[frame] : NULL);
    free(sprite->frame_paths);
  }
  project->n_sprites = first;
}

static void free_imported_backgrounds(GmlcProject *project, int first_sprite){
  free_sprite_range(project, first_sprite);
  for(int i = 0; i < project->n_tilesets; ++i){
    free(project->tilesets[i].id);
    free(project->tilesets[i].name);
  }
  free(project->tilesets);
  project->tilesets = NULL;
  project->n_tilesets = project->cap_tilesets = 0;
}

int gmlc_classic_import_backgrounds(const GmlcClassicManifest *classic,
                                    GmlcProject *project, const char *cache_dir,
                                    char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->tilesets || project->n_tilesets){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid background-import arguments");
    return 0;
  }
  uint32_t slot_count = classic->inventory.resource_slots[GMLC_CLASSIC_BACKGROUND];
  uint32_t existing = classic->existing[GMLC_CLASSIC_BACKGROUND];
  if(slot_count > INT32_MAX || existing > INT32_MAX ||
     project->n_sprites > INT32_MAX - (int)existing){
    if(err && errcap) snprintf(err, errcap, "classic import: too many backgrounds");
    return 0;
  }
  int first_sprite = project->n_sprites;
  int needed = first_sprite + (int)existing;
  GmlcSprite *sprites = (GmlcSprite*)realloc(project->sprites,
                                              (size_t)(needed ? needed : 1) * sizeof(*sprites));
  if(!sprites){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating background images");
    return 0;
  }
  project->sprites = sprites;
  if(needed > first_sprite)
    memset(project->sprites + first_sprite, 0, (size_t)(needed - first_sprite) * sizeof(*sprites));
  project->cap_sprites = needed;
  project->tilesets = (GmlcTileset*)calloc(slot_count ? slot_count : 1, sizeof(*project->tilesets));
  if(!project->tilesets){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating backgrounds");
    return 0;
  }
  project->n_tilesets = project->cap_tilesets = (int)slot_count;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_BACKGROUND];
  for(uint32_t i = 0; i < slot_count; ++i){
    char fallback[64];
    snprintf(fallback, sizeof(fallback), "__classic_missing_background_%u", i);
    const char *name = slots[i].exists && slots[i].name ? slots[i].name : fallback;
    GmlcTileset *background = &project->tilesets[i];
    background->id = copy_string(name);
    background->name = copy_string(name);
    background->sprite_id = -1;
    background->tile_width = background->tile_height = 16;
    if(!background->id || !background->name){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory naming background %u", i);
      free_imported_backgrounds(project, first_sprite);
      return 0;
    }
    if(!slots[i].exists) continue;
    if(slots[i].legacy_layout){
      ImportReader r={slots[i].payload,slots[i].payload_size,0,err,errcap};
      uint32_t fields[12],has_image;
      for(int field=0;field<12;field++) if(!import_u32(&r,&fields[field],"legacy background field")){
        free_imported_backgrounds(project,first_sprite); return 0;
      }
      if(!import_u32(&r,&has_image,"legacy background image flag") ||
         fields[0]>INT32_MAX || fields[1]>INT32_MAX){
        free_imported_backgrounds(project,first_sprite); return 0;
      }
      uint8_t *rgba=NULL; uint32_t rgba_bytes=0;
      if(has_image && !decode_legacy_image(&r,(int)fields[0],(int)fields[1],fields[2]!=0,
                                           &rgba,&rgba_bytes,"legacy background image")){
        free_imported_backgrounds(project,first_sprite); return 0;
      }
      if(r.pos!=r.size){ stbi_image_free(rgba); if(err && errcap) snprintf(err,errcap,"classic import: trailing legacy background payload"); free_imported_backgrounds(project,first_sprite); return 0; }
      GmlcSprite *sprite=&project->sprites[project->n_sprites++];
      sprite->id=copy_string(name); sprite->name=copy_string(name);
      sprite->runtime_id=-1; sprite->tileset_source=1;
      sprite->width=(int)fields[0]; sprite->height=(int)fields[1];
      sprite->bbox_right=sprite->width?sprite->width-1:0;
      sprite->bbox_bottom=sprite->height?sprite->height-1:0;
      sprite->n_frames=1; sprite->frame_paths=(char**)calloc(1,sizeof(*sprite->frame_paths));
      char leaf[80]; snprintf(leaf,sizeof(leaf),"classic_background_%06u.png",i);
      if(sprite->frame_paths)
        sprite->frame_paths[0]=import_rgba_path(project,cache_dir,leaf,rgba,rgba_bytes,
                                                sprite->width,sprite->height,err,errcap);
      int image_ok=sprite->id && sprite->name && sprite->frame_paths && sprite->frame_paths[0];
      stbi_image_free(rgba);
      if(!image_ok){ free_imported_backgrounds(project,first_sprite); return 0; }
      background->sprite_id=project->n_sprites-1;
      background->sprite_no_export=fields[5]?0:1;
      background->tile_width=(int32_t)fields[6]; background->tile_height=(int32_t)fields[7];
      background->border_x=(int32_t)fields[8]; background->border_y=(int32_t)fields[9];
      int step_x=background->tile_width+(int32_t)fields[10];
      int step_y=background->tile_height+(int32_t)fields[11];
      background->columns=step_x>0 && sprite->width>background->border_x
        ? (sprite->width-background->border_x+(int32_t)fields[10])/step_x : 1;
      int rows=step_y>0 && sprite->height>background->border_y
        ? (sprite->height-background->border_y+(int32_t)fields[11])/step_y : 1;
      if(background->columns<1) background->columns=1;
      if(rows<1) rows=1;
      int64_t tile_count=(int64_t)background->columns*(int64_t)rows+1;
      background->tile_count=tile_count>INT32_MAX?INT32_MAX:(int)tile_count;
      continue;
    }
    ImportReader r = {slots[i].payload, slots[i].payload_size, 0, err, errcap};
    uint32_t fields[7] = {0}, image_version, width, height, pixel_bytes = 0;
    const uint8_t *pixels = NULL;
    if(!slots[i].executable_layout)
      for(int field = 0; field < 7; ++field)
        if(!import_u32(&r, &fields[field], "background tile field")){
          free_imported_backgrounds(project, first_sprite); return 0;
        }
    if(!import_u32(&r, &image_version, "background image version") ||
       !import_u32(&r, &width, "background width") || !import_u32(&r, &height, "background height") ||
       (width && height && !import_blob(&r, &pixels, &pixel_bytes, "background pixels")) ||
       width > INT32_MAX || height > INT32_MAX || r.pos != r.size){
      if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: invalid background %u", i);
      free_imported_backgrounds(project, first_sprite);
      return 0;
    }
    (void)image_version;
    GmlcSprite *sprite = &project->sprites[project->n_sprites++];
    sprite->id = copy_string(name); sprite->name = copy_string(name);
    sprite->runtime_id = -1; sprite->tileset_source = 1;
    sprite->width = (int)width; sprite->height = (int)height;
    sprite->bbox_right = width ? (int)width - 1 : 0;
    sprite->bbox_bottom = height ? (int)height - 1 : 0;
    sprite->n_frames = 1;
    sprite->frame_paths = (char**)calloc(1, sizeof(*sprite->frame_paths));
    char leaf[80];
    snprintf(leaf, sizeof(leaf), "classic_background_%06u.png", i);
    uint8_t *converted = NULL;
    const uint8_t *image_pixels = pixels;
    if(pixels && width && height){
      converted=import_bgra_to_rgba(pixels,pixel_bytes,width,height,"background",err,errcap);
      if(!converted){ free_imported_backgrounds(project,first_sprite); return 0; }
      image_pixels=converted;
    }
    if(sprite->frame_paths)
      sprite->frame_paths[0]=import_rgba_path(project,cache_dir,leaf,image_pixels,pixel_bytes,
                                              (int)width,(int)height,err,errcap);
    free(converted);
    if(!sprite->id || !sprite->name || !sprite->frame_paths || !sprite->frame_paths[0]){
      if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: out of memory importing background %u", i);
      free_imported_backgrounds(project, first_sprite);
      return 0;
    }
    background->sprite_id = project->n_sprites - 1;
    background->sprite_no_export = fields[0] ? 0 : 1;
    background->tile_width = (int32_t)fields[1];
    background->tile_height = (int32_t)fields[2];
    background->border_x = (int32_t)fields[3];
    background->border_y = (int32_t)fields[4];
    int step_x = background->tile_width + (int32_t)fields[5];
    int step_y = background->tile_height + (int32_t)fields[6];
    background->columns = step_x > 0 && (int)width > background->border_x
      ? ((int)width - background->border_x + (int32_t)fields[5]) / step_x : 1;
    int rows = step_y > 0 && (int)height > background->border_y
      ? ((int)height - background->border_y + (int32_t)fields[6]) / step_y : 1;
    if(background->columns < 1) background->columns = 1;
    if(rows < 1) rows = 1;
    int64_t tile_count = (int64_t)background->columns * (int64_t)rows + 1;
    background->tile_count = tile_count > INT32_MAX ? INT32_MAX : (int)tile_count;
  }
  return 1;
}

static void free_imported_fonts(GmlcProject *project){
  for(int i = 0; i < project->n_fonts; ++i){
    free(project->fonts[i].id); free(project->fonts[i].name);
    free(project->fonts[i].png_path); free(project->fonts[i].glyphs);
  }
  free(project->fonts);
  project->fonts = NULL;
  project->n_fonts = project->cap_fonts = 0;
}

typedef struct {
  char *face;
  int point_size, bold, italic;
  int first, last, charset, antialias;
} ClassicFontSpec;

typedef struct {
  uint8_t *rgba;
  int width, height, line_height;
  GmlcFontGlyph *glyphs;
  int n_glyphs;
} ClassicFontRaster;

static void classic_font_spec_free(ClassicFontSpec *spec){
  if(!spec) return;
  free(spec->face);
  memset(spec,0,sizeof(*spec));
}

static void classic_font_raster_free(ClassicFontRaster *raster){
  if(!raster) return;
  free(raster->rgba);
  free(raster->glyphs);
  memset(raster,0,sizeof(*raster));
}

static int classic_font_parse(const GmlcClassicManifest *classic,
                              const GmlcClassicResourceSlot *slot,
                              ClassicFontSpec *spec, char *err, size_t errcap){
  memset(spec,0,sizeof(*spec));
  ImportReader r={slot->payload,slot->payload_size,0,err,errcap};
  if(!import_copy_string(&r,&spec->face,"font face")) return 0;
  uint32_t fields[5];
  for(int field=0;field<5;field++) if(!import_u32(&r,&fields[field],"font field")){
    classic_font_spec_free(spec);
    return 0;
  }
  if(r.pos!=r.size && !slot->executable_layout){
    if(err && errcap) snprintf(err,errcap,"classic import: trailing font payload");
    classic_font_spec_free(spec);
    return 0;
  }
  spec->point_size=(int32_t)fields[0];
  if(spec->point_size<1) spec->point_size=1;
  if(spec->point_size>256) spec->point_size=256;
  spec->bold=fields[1]!=0;
  spec->italic=fields[2]!=0;
  uint32_t first=fields[3];
  if(classic->inventory.header.version==GMLC_CLASSIC_GM81){
    spec->antialias=(int)(first>>24);
    spec->charset=(int)((first>>16)&0xffu);
    first&=0xffffu;
  }
  int range_first=(int)first, range_last=(int)fields[4];
  /* Classic strings use the format's single-byte character map. Controls have no printable
   * glyph, and FONT records store a 16-bit character id, so retain the meaningful byte range. */
  if(range_first<GML_DEFAULT_FONT_FIRST) range_first=GML_DEFAULT_FONT_FIRST;
  if(range_last>GML_DEFAULT_FONT_LAST) range_last=GML_DEFAULT_FONT_LAST;
  if(range_last<range_first){
    range_first=GML_DEFAULT_FONT_FIRST;
    range_last=GML_DEFAULT_FONT_LAST;
  }
  spec->first=range_first;
  spec->last=range_last;
  return 1;
}

/* A compiled GM8 resource carries the compiler's complete alpha atlas after the ordinary project
 * metadata. Importing it verbatim preserves the original glyph hints, metrics and host-font
 * choice. Return zero for a metadata-only resource, one for an imported atlas and -1 on error. */
static int classic_font_build_compiled(const GmlcClassicResourceSlot *slot,
                                       const ClassicFontSpec *spec,
                                       ClassicFontRaster *raster,
                                       char *err, size_t errcap){
  memset(raster,0,sizeof(*raster));
  if(!slot || !slot->executable_layout) return 0;
  ImportReader r={slot->payload,slot->payload_size,0,err,errcap};
  uint32_t ignored=0;
  if(!import_skip_string(&r,NULL,&ignored,"compiled font face") ||
     !import_skip_words(&r,5,"compiled font fields")) return -1;
  if(r.pos==r.size) return 0;

  uint32_t map[256u*6u];
  for(size_t i=0;i<sizeof(map)/sizeof(map[0]);i++)
    if(!import_u32(&r,&map[i],"compiled font glyph map")) return -1;
  uint32_t width=0,height=0,alpha_size=0;
  const uint8_t *alpha=NULL;
  if(!import_u32(&r,&width,"compiled font atlas width") ||
     !import_u32(&r,&height,"compiled font atlas height") ||
     !import_blob(&r,&alpha,&alpha_size,"compiled font atlas") || r.pos!=r.size ||
     !width || !height || width>4096 || height>4096 ||
     (uint64_t)width*(uint64_t)height!=alpha_size){
    if(err && errcap && !err[0]) snprintf(err,errcap,"classic import: invalid compiled font atlas");
    return -1;
  }
  uint8_t *rgba=(uint8_t*)malloc((size_t)alpha_size*4u);
  int count=spec->last-spec->first+1;
  GmlcFontGlyph *glyphs=(GmlcFontGlyph*)calloc((size_t)count,sizeof(*glyphs));
  if(!rgba || !glyphs){
    free(rgba); free(glyphs);
    if(err && errcap) snprintf(err,errcap,"classic import: out of memory loading compiled font");
    return -1;
  }
  for(uint32_t pixel=0;pixel<alpha_size;pixel++){
    rgba[pixel*4u]=rgba[pixel*4u+1u]=rgba[pixel*4u+2u]=255;
    rgba[pixel*4u+3u]=alpha[pixel];
  }
  int line_height=0;
  for(int i=0;i<count;i++){
    int ch=spec->first+i;
    const uint32_t *entry=map+(size_t)ch*6u;
    uint32_t x=entry[0],y=entry[1],w=entry[2],h=entry[3];
    if(x>width || y>height || w>width-x || h>height-y ||
       x>INT32_MAX || y>INT32_MAX || w>INT32_MAX || h>INT32_MAX){
      free(rgba); free(glyphs);
      if(err && errcap) snprintf(err,errcap,"classic import: invalid compiled glyph bounds");
      return -1;
    }
    glyphs[i].ch=ch; glyphs[i].x=(int)x; glyphs[i].y=(int)y;
    glyphs[i].w=(int)w; glyphs[i].h=(int)h;
    glyphs[i].shift=(int32_t)entry[4]; glyphs[i].offset=(int32_t)entry[5];
    if((int)h>line_height) line_height=(int)h;
  }
  if(line_height<1) line_height=1;
  raster->rgba=rgba; raster->width=(int)width; raster->height=(int)height;
  raster->line_height=line_height; raster->glyphs=glyphs; raster->n_glyphs=count;
  return 1;
}

typedef struct {
  const char *regular, *bold, *italic, *bold_italic;
  int category, inherent_bold;
} ClassicFontFiles;

typedef struct {
  char *path;
  int bold, italic;
} ClassicFontFile;

enum { CLASSIC_FONT_SANS, CLASSIC_FONT_MONO, CLASSIC_FONT_SERIF };

static void classic_font_normalize_face(const char *face, char *normalized, size_t cap){
  size_t out=0;
  for(const unsigned char *p=(const unsigned char*)face; p && *p && out+1<cap; ++p){
    if(*p<128 && isalnum(*p)) normalized[out++]=(char)tolower(*p);
  }
  normalized[out]='\0';
}

static int classic_font_prefix(const char *name, const char *prefix){
  return !strncmp(name,prefix,strlen(prefix));
}

/* Classic projects name Windows families, while the serialized project contains no outlines.
 * These are family-to-filename conventions used by Windows itself, not resource-specific rules.
 * Script/charset suffixes share the same outlines and are intentionally matched by prefix. */
static ClassicFontFiles classic_font_files_for_face(const char *face){
  char name[128];
  classic_font_normalize_face(face,name,sizeof(name));
  if(classic_font_prefix(name,"arialblack"))
    return (ClassicFontFiles){"ariblk.ttf","ariblk.ttf","ariblk.ttf","ariblk.ttf",CLASSIC_FONT_SANS,1};
  if(classic_font_prefix(name,"arial"))
    return (ClassicFontFiles){"arial.ttf","arialbd.ttf","ariali.ttf","arialbi.ttf",CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"couriernew") || !strcmp(name,"courier"))
    return (ClassicFontFiles){"cour.ttf","courbd.ttf","couri.ttf","courbi.ttf",CLASSIC_FONT_MONO,0};
  if(classic_font_prefix(name,"comicsans"))
    return (ClassicFontFiles){"comic.ttf","comicbd.ttf","comici.ttf","comicz.ttf",CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"calibri"))
    return (ClassicFontFiles){"calibri.ttf","calibrib.ttf","calibrii.ttf","calibriz.ttf",CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"georgia"))
    return (ClassicFontFiles){"georgia.ttf","georgiab.ttf","georgiai.ttf","georgiaz.ttf",CLASSIC_FONT_SERIF,0};
  if(classic_font_prefix(name,"verdana"))
    return (ClassicFontFiles){"verdana.ttf","verdanab.ttf","verdanai.ttf","verdanaz.ttf",CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"timesnewroman"))
    return (ClassicFontFiles){"times.ttf","timesbd.ttf","timesi.ttf","timesbi.ttf",CLASSIC_FONT_SERIF,0};
  if(classic_font_prefix(name,"tahoma"))
    return (ClassicFontFiles){"tahoma.ttf","tahomabd.ttf",NULL,NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"centurygothic"))
    return (ClassicFontFiles){"GOTHIC.TTF","GOTHICB.TTF","GOTHICI.TTF","GOTHICBI.TTF",CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"franklingothicmediumcond"))
    return (ClassicFontFiles){"FRAMDCN.TTF","framd.ttf","framdit.ttf",NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"berlinsans"))
    return (ClassicFontFiles){"BRLNSR.TTF","BRLNSB.TTF",NULL,"BRLNSDB.TTF",CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"microsoftsansserif"))
    return (ClassicFontFiles){"micross.ttf",NULL,NULL,NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"papyrus"))
    return (ClassicFontFiles){"PAPYRUS.TTF",NULL,NULL,NULL,CLASSIC_FONT_SERIF,0};
  if(classic_font_prefix(name,"kristenitc"))
    return (ClassicFontFiles){"ITCKRIST.TTF",NULL,NULL,NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"mvboli"))
    return (ClassicFontFiles){"mvboli.ttf",NULL,NULL,NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"raavi"))
    return (ClassicFontFiles){"raavi.ttf","raavib.ttf",NULL,NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"meiryo"))
    return (ClassicFontFiles){"meiryo.ttc","meiryob.ttc",NULL,NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"dotum") || classic_font_prefix(name,"gulim"))
    return (ClassicFontFiles){"gulim.ttc",NULL,NULL,NULL,CLASSIC_FONT_SANS,0};
  if(classic_font_prefix(name,"batang") || classic_font_prefix(name,"gungsuh"))
    return (ClassicFontFiles){"batang.ttc",NULL,NULL,NULL,CLASSIC_FONT_SERIF,0};
  if(classic_font_prefix(name,"fixedsys") || classic_font_prefix(name,"smallfonts"))
    return (ClassicFontFiles){NULL,NULL,NULL,NULL,CLASSIC_FONT_MONO,0};
  return (ClassicFontFiles){NULL,NULL,NULL,NULL,CLASSIC_FONT_SANS,0};
}

static int classic_font_path_readable(const char *path){
  FILE *file=path?fopen(path,"rb"):NULL;
  if(!file) return 0;
  fclose(file);
  return 1;
}

static char *classic_font_find_leaf(const char *leaf){
  if(!leaf || !*leaf) return NULL;
  const char *env=getenv("GML_CLASSIC_FONT_DIR");
  const char *windir=getenv("WINDIR");
  const char *local=getenv("LOCALAPPDATA");
  const char *home=getenv("HOME");
  char windows_fonts[1024]={0}, local_fonts[1024]={0}, home_fonts[1024]={0};
  if(windir && *windir) snprintf(windows_fonts,sizeof(windows_fonts),"%s/Fonts",windir);
  if(local && *local) snprintf(local_fonts,sizeof(local_fonts),"%s/Microsoft/Windows/Fonts",local);
  if(home && *home) snprintf(home_fonts,sizeof(home_fonts),"%s/.local/share/fonts",home);
  const char *dirs[]={
    env, windows_fonts[0]?windows_fonts:NULL, local_fonts[0]?local_fonts:NULL,
    "/usr/share/fonts/truetype/msttcorefonts",
    "/usr/share/fonts/truetype/liberation",
    "/usr/share/fonts/truetype/liberation2", "/usr/share/fonts/truetype/dejavu",
    "/usr/share/fonts/TTF", "/usr/local/share/fonts", home_fonts[0]?home_fonts:NULL
  };
  for(size_t i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++){
    if(!dirs[i] || !*dirs[i]) continue;
    char *path=cache_path(dirs[i],leaf);
    if(path && classic_font_path_readable(path)) return path;
    free(path);
  }
  return NULL;
}

static const char *classic_font_style_leaf(const ClassicFontFiles *files,
                                           int bold, int italic,
                                           int *file_bold, int *file_italic){
  const char *leaf=NULL;
  *file_bold=*file_italic=0;
  if(bold && italic && files->bold_italic){ leaf=files->bold_italic; *file_bold=*file_italic=1; }
  else if(bold && files->bold){ leaf=files->bold; *file_bold=1; }
  else if(italic && files->italic){ leaf=files->italic; *file_italic=1; }
  else { leaf=files->regular; *file_bold=files->inherent_bold; }
  return leaf;
}

static ClassicFontFile classic_font_resolve_file(const ClassicFontSpec *spec){
  ClassicFontFiles files=classic_font_files_for_face(spec->face);
  ClassicFontFile result={0};
  int file_bold=0,file_italic=0;
  const char *leaf=classic_font_style_leaf(&files,spec->bold,spec->italic,&file_bold,&file_italic);
  result.path=classic_font_find_leaf(leaf);
  if(!result.path && leaf!=files.regular){
    result.path=classic_font_find_leaf(files.regular);
    file_bold=files.inherent_bold; file_italic=0;
  }
  if(!result.path && (files.regular || files.category!=CLASSIC_FONT_SANS)){
    ClassicFontFiles substitute;
    if(files.category==CLASSIC_FONT_MONO)
      substitute=(ClassicFontFiles){"LiberationMono-Regular.ttf","LiberationMono-Bold.ttf",
        "LiberationMono-Italic.ttf","LiberationMono-BoldItalic.ttf",CLASSIC_FONT_MONO,0};
    else if(files.category==CLASSIC_FONT_SERIF)
      substitute=(ClassicFontFiles){"LiberationSerif-Regular.ttf","LiberationSerif-Bold.ttf",
        "LiberationSerif-Italic.ttf","LiberationSerif-BoldItalic.ttf",CLASSIC_FONT_SERIF,0};
    else
      substitute=(ClassicFontFiles){"LiberationSans-Regular.ttf","LiberationSans-Bold.ttf",
        "LiberationSans-Italic.ttf","LiberationSans-BoldItalic.ttf",CLASSIC_FONT_SANS,0};
    leaf=classic_font_style_leaf(&substitute,spec->bold,spec->italic,&file_bold,&file_italic);
    result.path=classic_font_find_leaf(leaf);
  }
  result.bold=file_bold;
  result.italic=file_italic;
  return result;
}

static int classic_font_sfnt_magic(const uint8_t *data, size_t size){
  if(size<12) return 0;
  return (data[0]==0 && data[1]==1 && data[2]==0 && data[3]==0) ||
    !memcmp(data,"true",4) || !memcmp(data,"typ1",4) || !memcmp(data,"OTTO",4) ||
    !memcmp(data,"ttcf",4);
}

static int classic_font_codepoint(const ClassicFontSpec *spec, int ch){
  static const uint16_t windows_1252[32]={
    0x20ac,0,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,
    0x02c6,0x2030,0x0160,0x2039,0x0152,0,0x017d,0,
    0,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
    0x02dc,0x2122,0x0161,0x203a,0x0153,0,0x017e,0x0178
  };
  if(ch>=0x80 && ch<0xa0 && (spec->charset==0 || spec->charset==1)){
    int mapped=windows_1252[ch-0x80];
    if(mapped) return mapped;
  }
  return ch;
}

static int classic_font_build_truetype(const ClassicFontSpec *spec,
                                       const ClassicFontFile *file,
                                       ClassicFontRaster *raster,
                                       char *err, size_t errcap){
  memset(raster,0,sizeof(*raster));
  FILE *fp=file->path?fopen(file->path,"rb"):NULL;
  if(!fp) return 0;
  if(fseek(fp,0,SEEK_END)!=0){ fclose(fp); return 0; }
  long file_size=ftell(fp);
  if(file_size<=0 || file_size>32*1024*1024 || fseek(fp,0,SEEK_SET)!=0){ fclose(fp); return 0; }
  uint8_t *font_data=(uint8_t*)malloc((size_t)file_size);
  if(!font_data || fread(font_data,1,(size_t)file_size,fp)!=(size_t)file_size){
    free(font_data); fclose(fp); return 0;
  }
  fclose(fp);
  if(!classic_font_sfnt_magic(font_data,(size_t)file_size)){ free(font_data); return 0; }
  const char *match=spec->face;
  while(*match=='@') match++;
  int font_offset=stbtt_FindMatchingFont(font_data,match,STBTT_MACSTYLE_DONTCARE);
  if(font_offset<0) font_offset=stbtt_GetFontOffsetForIndex(font_data,0);
  stbtt_fontinfo info;
  if(font_offset<0 || font_offset>file_size-12 || !stbtt_InitFont(&info,font_data,font_offset)){
    free(font_data); return 0;
  }
  float em_pixels=(float)(spec->point_size*96.0/72.0);
  if(em_pixels<4.0f) em_pixels=4.0f;
  if(em_pixels>384.0f) em_pixels=384.0f;
  /* A Windows point size maps the font's em square to points at 96 dpi. ScaleForPixelHeight
   * instead maps ascender-to-descender to that value and noticeably shrinks faces with large
   * Windows line metrics. */
  float scale=stbtt_ScaleForMappingEmToPixels(&info,em_pixels);
  int asc,desc,gap;
  stbtt_GetFontVMetrics(&info,&asc,&desc,&gap);
  int ascent=(int)lround(asc*scale);
  int line_height=(int)lround((asc-desc+gap)*scale);
  if(line_height<4) line_height=4;
  int synth_bold=spec->bold && !file->bold ? (line_height>=24?2:1) : 0;
  int synth_italic=spec->italic && !file->italic;
  int italic_px=synth_italic?(int)ceil(line_height*0.22):0;
  int count=spec->last-spec->first+1;
  int *widths=(int*)calloc((size_t)count,sizeof(*widths));
  int *xmins=(int*)calloc((size_t)count,sizeof(*xmins));
  GmlcFontGlyph *glyphs=(GmlcFontGlyph*)calloc((size_t)count,sizeof(*glyphs));
  if(!widths || !xmins || !glyphs) goto oom;
  int max_width=0;
  for(int i=0;i<count;i++){
    int x0,y0,x1,y1;
    int codepoint=classic_font_codepoint(spec,spec->first+i);
    stbtt_GetCodepointBitmapBox(&info,codepoint,scale,scale,&x0,&y0,&x1,&y1);
    int w=x1-x0;
    if(w<1) w=1;
    widths[i]=w+synth_bold+italic_px;
    xmins[i]=x0;
    if(widths[i]>max_width) max_width=widths[i];
  }
  int atlas_width=512;
  while(atlas_width<max_width+1 && atlas_width<4096) atlas_width*=2;
  if(atlas_width<max_width+1){
    if(err && errcap) snprintf(err,errcap,"classic import: TrueType glyph is too wide");
    free(widths); free(xmins); free(glyphs); free(font_data);
    return 0;
  }
  int x=0,y=0,row_height=line_height,used_height=0;
  for(;;){
    x=0; y=0;
    for(int i=0;i<count;i++){
      if(x+widths[i]+1>atlas_width){ x=0; y+=row_height+1; }
      x+=widths[i]+1;
    }
    used_height=y+row_height;
    if(used_height<=4096 || atlas_width>=4096) break;
    atlas_width*=2;
  }
  int atlas_height=32;
  while(atlas_height<used_height && atlas_height<4096) atlas_height*=2;
  if(atlas_height<used_height){
    if(err && errcap) snprintf(err,errcap,"classic import: TrueType atlas is too tall");
    free(widths); free(xmins); free(glyphs); free(font_data);
    return 0;
  }
  uint8_t *rgba=(uint8_t*)calloc((size_t)atlas_width*atlas_height,4);
  if(!rgba) goto oom;
  x=0; y=0;
  for(int i=0;i<count;i++){
    int ch=spec->first+i, w=widths[i];
    int codepoint=classic_font_codepoint(spec,ch);
    if(x+w+1>atlas_width){ x=0; y+=row_height+1; }
    int advance,lsb,x0,y0,x1,y1;
    stbtt_GetCodepointHMetrics(&info,codepoint,&advance,&lsb);
    stbtt_GetCodepointBitmapBox(&info,codepoint,scale,scale,&x0,&y0,&x1,&y1);
    int bitmap_width=x1-x0, bitmap_height=y1-y0;
    GmlcFontGlyph *glyph=&glyphs[i];
    glyph->ch=ch; glyph->x=x; glyph->y=y; glyph->w=w; glyph->h=line_height;
    glyph->shift=(int)lround(advance*scale)+synth_bold;
    if(glyph->shift<1) glyph->shift=1;
    glyph->offset=xmins[i];
    if(bitmap_width>0 && bitmap_height>0){
      uint8_t *bitmap=(uint8_t*)malloc((size_t)bitmap_width*bitmap_height);
      if(!bitmap){ free(rgba); goto oom; }
      stbtt_MakeCodepointBitmap(&info,bitmap,bitmap_width,bitmap_height,bitmap_width,
                                scale,scale,codepoint);
      int origin_y=ascent+y0;
      for(int py=0;py<bitmap_height;py++){
        int dest_y=origin_y+py;
        if(dest_y<0 || dest_y>=line_height) continue;
        int shear=synth_italic?(int)lround((line_height-1-dest_y)*0.22):0;
        for(int px=0;px<bitmap_width;px++){
          uint8_t alpha=bitmap[py*bitmap_width+px];
          for(int bx=0;bx<=synth_bold;bx++){
            int dest_x=px+shear+bx;
            if(dest_x<0 || dest_x>=w) continue;
            uint8_t *pixel=rgba+((size_t)(y+dest_y)*atlas_width+x+dest_x)*4u;
            if(alpha>pixel[3]){
              pixel[0]=pixel[1]=pixel[2]=255;
              pixel[3]=alpha;
            }
          }
        }
      }
      free(bitmap);
    }
    x+=w+1;
  }
  free(widths); free(xmins); free(font_data);
  raster->rgba=rgba; raster->width=atlas_width; raster->height=atlas_height;
  raster->line_height=line_height; raster->glyphs=glyphs; raster->n_glyphs=count;
  return 1;
oom:
  if(err && errcap) snprintf(err,errcap,"classic import: out of memory rasterizing TrueType font");
  free(widths); free(xmins); free(glyphs); free(font_data);
  return 0;
}

static uint8_t classic_font_sample(const GmlDefaultGlyph *glyph, double x, double y){
  if(x<0.0 || y<0.0 || x>(double)glyph->width-1.0 || y>(double)glyph->height-1.0) return 0;
  int x0=(int)floor(x), y0=(int)floor(y);
  int x1=x0+1<glyph->width?x0+1:x0;
  int y1=y0+1<glyph->height?y0+1:y0;
  double fx=x-x0, fy=y-y0;
  const uint8_t *alpha=gml_default_font_alpha+glyph->off;
  double top=alpha[y0*glyph->width+x0]*(1.0-fx)+alpha[y0*glyph->width+x1]*fx;
  double bottom=alpha[y1*glyph->width+x0]*(1.0-fx)+alpha[y1*glyph->width+x1]*fx;
  int value=(int)lround(top*(1.0-fy)+bottom*fy);
  return (uint8_t)(value<0?0:value>255?255:value);
}

/* The bundled coverage is the portable fallback when the source typeface is unavailable.
 * It represents a 12-point regular face; scale its real per-glyph bounds (not the common line
 * height) and synthesize requested styles so classic font metadata is never silently ignored. */
static int classic_font_build_fallback(const ClassicFontSpec *spec,
                                       ClassicFontRaster *raster,
                                       char *err, size_t errcap){
  memset(raster,0,sizeof(*raster));
  const double scale=(double)spec->point_size/12.0;
  int line_height=(int)lround(GML_DEFAULT_FONT_LINE_HEIGHT*scale);
  if(line_height<4) line_height=4;
  int bold_px=spec->bold?(line_height>=24?2:1):0;
  int italic_px=spec->italic?(int)ceil(line_height*0.22):0;
  int count=spec->last-spec->first+1;
  int *widths=(int*)calloc((size_t)count,sizeof(*widths));
  int *heights=(int*)calloc((size_t)count,sizeof(*heights));
  GmlcFontGlyph *glyphs=(GmlcFontGlyph*)calloc((size_t)count,sizeof(*glyphs));
  if(!widths || !heights || !glyphs) goto oom;
  int max_width=0;
  for(int i=0;i<count;i++){
    const GmlDefaultGlyph *source=&gml_default_glyphs[spec->first+i-GML_DEFAULT_FONT_FIRST];
    int w=(int)ceil(source->width*scale)+bold_px+italic_px;
    int h=(int)ceil(source->height*scale);
    if(w<1) w=1;
    if(h<1) h=1;
    widths[i]=w; heights[i]=h;
    if(w>max_width) max_width=w;
  }
  int atlas_width=512;
  while(atlas_width<max_width+1 && atlas_width<4096) atlas_width*=2;
  if(atlas_width<max_width+1){
    if(err && errcap) snprintf(err,errcap,"classic import: font fallback glyph is too wide");
    free(widths); free(heights); free(glyphs);
    return 0;
  }
  int x=0,y=0,row_height=0,used_height=0;
  for(;;){
    x=0; y=0; row_height=0;
    for(int i=0;i<count;i++){
      int w=widths[i],h=heights[i];
      if(x+w+1>atlas_width){ x=0; y+=row_height+1; row_height=0; }
      if(h>row_height) row_height=h;
      x+=w+1;
    }
    used_height=y+row_height;
    if(used_height<=4096 || atlas_width>=4096) break;
    atlas_width*=2;
  }
  int atlas_height=32;
  while(atlas_height<used_height && atlas_height<4096) atlas_height*=2;
  if(atlas_height<used_height){
    if(err && errcap) snprintf(err,errcap,"classic import: font fallback atlas is too tall");
    free(widths); free(heights); free(glyphs);
    return 0;
  }
  uint8_t *rgba=(uint8_t*)calloc((size_t)atlas_width*atlas_height,4);
  if(!rgba) goto oom;
  x=0; y=0; row_height=0;
  for(int i=0;i<count;i++){
    const GmlDefaultGlyph *source=&gml_default_glyphs[spec->first+i-GML_DEFAULT_FONT_FIRST];
    int w=widths[i], h=heights[i];
    if(x+w+1>atlas_width){ x=0; y+=row_height+1; row_height=0; }
    if(h>row_height) row_height=h;
    GmlcFontGlyph *glyph=&glyphs[i];
    glyph->ch=spec->first+i; glyph->x=x; glyph->y=y; glyph->w=w; glyph->h=h;
    glyph->shift=(int)lround(source->shift*scale)+bold_px;
    if(glyph->shift<1) glyph->shift=1;
    glyph->offset=(int)lround(source->offset*scale);
    for(int py=0;py<h;py++){
      int shear=spec->italic?(int)lround((h-1-py)*0.22):0;
      for(int px=0;px<w-italic_px;px++){
        double sx=((double)px+0.5)/scale-0.5;
        double sy=((double)py+0.5)/scale-0.5;
        uint8_t alpha=classic_font_sample(source,sx,sy);
        for(int bx=0;bx<=bold_px;bx++){
          int dx=x+px+shear+bx;
          if(dx>=x+w) continue;
          uint8_t *pixel=rgba+((size_t)(y+py)*atlas_width+dx)*4u;
          if(alpha>pixel[3]){
            pixel[0]=pixel[1]=pixel[2]=255;
            pixel[3]=alpha;
          }
        }
      }
    }
    x+=w+1;
  }
  free(widths); free(heights);
  raster->rgba=rgba; raster->width=atlas_width; raster->height=atlas_height;
  raster->line_height=line_height; raster->glyphs=glyphs; raster->n_glyphs=count;
  return 1;
oom:
  if(err && errcap) snprintf(err,errcap,"classic import: out of memory building font fallback");
  free(widths); free(heights); free(glyphs);
  return 0;
}

static char *classic_font_store_raster(GmlcProject *project, const char *cache_dir,
                                       uint32_t slot, const ClassicFontRaster *raster,
                                       char *err, size_t errcap){
  char leaf[64];
  snprintf(leaf,sizeof(leaf),"classic_font_%u.png",slot);
  if(project->prefer_memory_files){
    char *path=gmlc_project_add_memory_file(project,leaf,GMLC_MEMORY_RGBA,raster->rgba,
                                            (size_t)raster->width*raster->height*4u,
                                            raster->width,raster->height);
    if(!path && err && errcap) snprintf(err,errcap,"classic import: out of memory retaining %s",leaf);
    return path;
  }
  char *path=cache_path(cache_dir,leaf);
  if(!path) return NULL;
  if(!stbi_write_png(path,raster->width,raster->height,4,raster->rgba,raster->width*4)){
    if(err && errcap) snprintf(err,errcap,"classic import: cannot write %s",path);
    free(path);
    return NULL;
  }
  return path;
}

int gmlc_classic_import_fonts(const GmlcClassicManifest *classic,
                              GmlcProject *project, const char *cache_dir,
                              char *err, size_t errcap){
  if(err && errcap) err[0]='\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->fonts || project->n_fonts){
    if(err && errcap) snprintf(err,errcap,"classic import: invalid font-import arguments");
    return 0;
  }
  uint32_t slot_count=classic->inventory.resource_slots[GMLC_CLASSIC_FONT];
  const GmlcClassicResourceSlot *slots=classic->slots[GMLC_CLASSIC_FONT];
  if(slot_count && !slots){
    if(err && errcap) snprintf(err,errcap,"classic import: missing font slots");
    return 0;
  }
  uint32_t count=0;
  for(uint32_t i=0;i<slot_count;i++) if(slots[i].exists) count++;
  if(count>INT32_MAX){ if(err && errcap) snprintf(err,errcap,"classic import: too many fonts"); return 0; }
  /* Empty and deleted classic slots are not resources. In particular, do not create an atlas or
   * reserve runtime font ids for them: sparse projects can have thousands of slots and no fonts. */
  if(!count) return 1;
  project->fonts=(GmlcFont*)calloc(count,sizeof(*project->fonts));
  if(!project->fonts){ if(err && errcap) snprintf(err,errcap,"classic import: out of memory allocating fonts"); return 0; }
  project->cap_fonts=(int)count;
  for(uint32_t i=0;i<slot_count;i++){
    if(!slots[i].exists) continue;
    char fallback[64];
    snprintf(fallback,sizeof(fallback),"__classic_font_slot_%u",i);
    const char *name=slots[i].name?slots[i].name:fallback;
    /* Dense FONT records are required by data.win. The compiler subsequently binds this resource
     * name to its dense index; the original sparse slot number is deliberately not emitted. */
    GmlcFont *font=&project->fonts[project->n_fonts++];
    font->id=copy_string(name); font->name=copy_string(name);
    ClassicFontSpec spec;
    ClassicFontRaster raster;
    if(!classic_font_parse(classic,&slots[i],&spec,err,errcap)){
      classic_font_spec_free(&spec); free_imported_fonts(project); return 0;
    }
    ClassicFontFile file={0};
    int compiled=classic_font_build_compiled(&slots[i],&spec,&raster,err,errcap);
    if(compiled<0){ classic_font_spec_free(&spec); free_imported_fonts(project); return 0; }
    int rasterized=0;
    if(!compiled){
      file=classic_font_resolve_file(&spec);
      rasterized=file.path && classic_font_build_truetype(&spec,&file,&raster,err,errcap);
    }
    if(!compiled && !rasterized){
      if(err && errcap) err[0]='\0';
      if(!classic_font_build_fallback(&spec,&raster,err,errcap)){
        free(file.path); classic_font_spec_free(&spec); free_imported_fonts(project); return 0;
      }
    }
    if(getenv("GML_LOG_FONT"))
      fprintf(stderr,"[font] classic face=%s pt=%d bold=%d italic=%d source=%s line=%d glyphs=%d\n",
              spec.face?spec.face:"",spec.point_size,spec.bold,spec.italic,
              compiled?"compiled atlas":rasterized?file.path:"embedded fallback",
              raster.line_height,raster.n_glyphs);
    font->png_path=classic_font_store_raster(project,cache_dir,i,&raster,err,errcap);
    font->width=raster.width; font->height=raster.height; font->em_size=raster.line_height;
    font->glyphs=raster.glyphs; font->n_glyphs=font->cap_glyphs=raster.n_glyphs;
    raster.glyphs=NULL;
    classic_font_raster_free(&raster);
    free(file.path);
    classic_font_spec_free(&spec);
    if(!font->id || !font->name || !font->png_path || !font->glyphs){
      free_imported_fonts(project); return 0;
    }
  }
  return 1;
}

static int write_binary(const char *path, const uint8_t *data, size_t size,
                        char *err, size_t errcap){
  FILE *file = fopen(path, "wb");
  if(!file){
    if(err && errcap) snprintf(err, errcap, "classic import: cannot create %s: %s", path, strerror(errno));
    return 0;
  }
  int wrote = !size || fwrite(data, 1, size, file) == size;
  int closed = fclose(file) == 0;
  if((!wrote || !closed) && err && errcap) snprintf(err, errcap, "classic import: cannot write %s", path);
  return wrote && closed;
}

static char *import_binary_path(GmlcProject *project, const char *cache_dir,
                                const char *leaf, const uint8_t *data, size_t size,
                                char *err, size_t errcap){
  if(project->prefer_memory_files){
    char *path=gmlc_project_add_memory_file(project,leaf,GMLC_MEMORY_BLOB,
                                            data,size,0,0);
    if(!path && err && errcap)
      snprintf(err,errcap,"classic import: out of memory retaining %s",leaf);
    return path;
  }
  char *path=cache_path(cache_dir,leaf);
  if(!path || !write_binary(path,data,size,err,errcap)){
    free(path);
    return NULL;
  }
  return path;
}

static void free_imported_sounds(GmlcProject *project){
  for(int i = 0; i < project->n_sounds; ++i){
    free(project->sounds[i].id);
    free(project->sounds[i].name);
    free(project->sounds[i].data_path);
  }
  free(project->sounds);
  project->sounds = NULL;
  project->n_sounds = project->cap_sounds = 0;
}

int gmlc_classic_import_sounds(const GmlcClassicManifest *classic,
                               GmlcProject *project, const char *cache_dir,
                               char *err, size_t errcap){
  static const uint8_t silent_wav[44] = {
    'R','I','F','F',36,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
    1,0,1,0,0x44,0xAC,0,0,0x88,0x58,1,0,2,0,16,0,'d','a','t','a',0,0,0,0
  };
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->sounds || project->n_sounds){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid sound-import arguments");
    return 0;
  }
  uint32_t count = classic->inventory.resource_slots[GMLC_CLASSIC_SOUND];
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many sound slots");
    return 0;
  }
  project->sounds = (GmlcSound*)calloc(count ? count : 1, sizeof(*project->sounds));
  if(!project->sounds){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating sounds");
    return 0;
  }
  project->n_sounds = project->cap_sounds = (int)count;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_SOUND];
  for(uint32_t i = 0; i < count; ++i){
    const GmlcClassicResourceSlot *source = &slots[i];
    if(!source->exists) continue;
    char leaf[96];
    const char *name = source->name ? source->name : "";
    const uint8_t *audio = silent_wav;
    size_t audio_size = sizeof(silent_wav);
    char *owned_audio = NULL;
    char extension_buffer[12];
    const char *extension = ".wav";
    double volume = 1.0;
    {
      ImportReader r = {source->payload, source->payload_size, 0, err, errcap};
      uint32_t kind, type_length, filename_length, has_data, blob_size = 0, ignored;
      const uint8_t *type_text = NULL, *filename_text = NULL, *blob = NULL;
      double pan;
      if(!import_u32(&r, &kind, "sound kind") ||
         !import_skip_string(&r, &type_text, &type_length, "sound file type") ||
         !import_skip_string(&r, &filename_text, &filename_length, "sound filename") ||
         !import_u32(&r, &has_data, "sound data flag") ||
         (has_data && !import_blob(&r, &blob, &blob_size, "sound data")) ||
         !import_u32(&r, &ignored, "sound effects") || !import_double(&r, &volume, "sound volume") ||
         !import_double(&r, &pan, "sound pan") || !import_u32(&r, &ignored, "sound preload") ||
         r.pos != r.size){
        free_imported_sounds(project);
        return 0;
      }
      (void)kind; (void)filename_text; (void)filename_length; (void)pan;
      if(has_data){
        if(source->legacy_layout){
          int decoded_size=0;
          if(blob_size>INT32_MAX ||
             !(owned_audio=stbi_zlib_decode_malloc((const char*)blob,(int)blob_size,&decoded_size)) || decoded_size<0){
            if(err && errcap) snprintf(err,errcap,"classic import: invalid compressed legacy sound");
            STBI_FREE(owned_audio); free_imported_sounds(project); return 0;
          }
          audio=(const uint8_t*)owned_audio; audio_size=(size_t)decoded_size;
        } else { audio = blob; audio_size = blob_size; }
      }
      if(type_length > 1 && type_length < 12 && type_text[0] == '.'){
        int safe = 1;
        for(uint32_t c = 1; c < type_length; ++c)
          if(!((type_text[c] >= 'a' && type_text[c] <= 'z') ||
               (type_text[c] >= 'A' && type_text[c] <= 'Z') ||
               (type_text[c] >= '0' && type_text[c] <= '9'))) safe = 0;
        if(safe){
          memcpy(extension_buffer, type_text, type_length); extension_buffer[type_length] = '\0';
          extension = extension_buffer;
        }
      }
    }
    snprintf(leaf, sizeof(leaf), "classic_sound_%06u%s", i, extension);
    GmlcSound *sound = &project->sounds[i];
    sound->id = copy_string(name);
    sound->name = copy_string(name);
    sound->data_path = import_binary_path(project,cache_dir,leaf,audio,audio_size,err,errcap);
    sound->volume = (float)volume;
    sound->pitch = 1.0f;
    int wrote=sound->id && sound->name && sound->data_path;
    STBI_FREE(owned_audio);
    if(!wrote){
      if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: out of memory importing sound %u", i);
      free_imported_sounds(project);
      return 0;
    }
  }
  return 1;
}

static void free_imported_paths(GmlcProject *project){
  for(int i = 0; i < project->n_paths; ++i){
    free(project->paths[i].id);
    free(project->paths[i].name);
    free(project->paths[i].points);
  }
  free(project->paths);
  project->paths = NULL;
  project->n_paths = project->cap_paths = 0;
}

int gmlc_classic_import_paths(const GmlcClassicManifest *classic,
                              GmlcProject *project, char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || project->paths || project->n_paths){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid path-import arguments");
    return 0;
  }
  uint32_t count = classic->inventory.resource_slots[GMLC_CLASSIC_PATH];
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many path slots");
    return 0;
  }
  project->paths = (GmlcPath*)calloc(count ? count : 1, sizeof(*project->paths));
  if(!project->paths){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating paths");
    return 0;
  }
  project->n_paths = project->cap_paths = (int)count;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_PATH];
  for(uint32_t i = 0; i < count; ++i){
    const GmlcClassicResourceSlot *source = &slots[i];
    char fallback[64];
    snprintf(fallback, sizeof(fallback), "__classic_missing_path_%u", i);
    const char *name = source->exists && source->name ? source->name : fallback;
    GmlcPath *path = &project->paths[i];
    path->id = copy_string(name);
    path->name = copy_string(name);
    if(!path->id || !path->name){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory importing path %u", i);
      free_imported_paths(project);
      return 0;
    }
    if(!source->exists) continue;
    ImportReader r = {source->payload, source->payload_size, 0, err, errcap};
    uint32_t kind, closed, precision, ignored, points;
    if(!import_u32(&r, &kind, "path kind") || !import_u32(&r, &closed, "path closed flag") ||
       !import_u32(&r, &precision, "path precision") ||
       !import_u32(&r, &ignored, "path editor room") || !import_u32(&r, &ignored, "path snap x") ||
       !import_u32(&r, &ignored, "path snap y") || !import_u32(&r, &points, "path point count") ||
       points > INT32_MAX || (size_t)points > (r.size - r.pos) / 24u){
      free_imported_paths(project);
      return 0;
    }
    path->kind = (int32_t)kind;
    path->closed = closed != 0;
    path->precision = (int32_t)precision;
    path->n_points = (int)points;
    path->points = (GmlcPathPoint*)calloc(points ? points : 1, sizeof(*path->points));
    if(!path->points){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating path points");
      free_imported_paths(project);
      return 0;
    }
    for(uint32_t point = 0; point < points; ++point){
      double x, y, speed;
      if(!import_double(&r, &x, "path point x") || !import_double(&r, &y, "path point y") ||
         !import_double(&r, &speed, "path point speed")){
        free_imported_paths(project);
        return 0;
      }
      path->points[point].x = (float)x;
      path->points[point].y = (float)y;
      path->points[point].speed = (float)speed;
    }
    if(r.pos != r.size){
      if(err && errcap) snprintf(err, errcap, "classic import: trailing path payload");
      free_imported_paths(project);
      return 0;
    }
  }
  return 1;
}

typedef struct {
  char *data;
  size_t length, capacity;
} ImportText;

static int text_reserve(ImportText *text, size_t extra){
  if(extra > SIZE_MAX - text->length - 1) return 0;
  size_t need = text->length + extra + 1;
  if(need <= text->capacity) return 1;
  size_t capacity = text->capacity ? text->capacity : 256;
  while(capacity < need){
    if(capacity > SIZE_MAX / 2){ capacity = need; break; }
    capacity *= 2;
  }
  char *data = (char*)realloc(text->data, capacity);
  if(!data) return 0;
  text->data = data;
  text->capacity = capacity;
  return 1;
}

static int text_append_n(ImportText *text, const char *value, size_t length){
  if(!text_reserve(text, length)) return 0;
  memcpy(text->data + text->length, value, length);
  text->length += length;
  text->data[text->length] = '\0';
  return 1;
}

static int text_append(ImportText *text, const char *value){
  return text_append_n(text, value ? value : "", value ? strlen(value) : 0);
}

static int text_append_int(ImportText *text, int32_t value){
  char number[32];
  snprintf(number, sizeof(number), "%d", value);
  return text_append(text, number);
}

static int text_append_quoted(ImportText *text, const char *value){
  if(!text_append(text, "\"")) return 0;
  for(const unsigned char *p = (const unsigned char*)(value ? value : ""); *p; ++p){
    char escaped[2] = {(char)*p, '\0'};
    if(*p == '\\' || *p == '"'){
      if(!text_append(text, "\\")) return 0;
    } else if(*p == '\n'){
      if(!text_append(text, "\\n")) return 0;
      continue;
    } else if(*p == '\r'){
      if(!text_append(text, "\\r")) return 0;
      continue;
    }
    if(!text_append(text, escaped)) return 0;
  }
  return text_append(text, "\"");
}

static int emit_action_call(ImportText *text, const char *function_name,
                            char **arguments, uint32_t *argument_kinds,
                            uint32_t used_arguments, int append_relative, int relative){
  if(!function_name || !*function_name) return text_append(text, "/* empty action */");
  if(!text_append(text, function_name) || !text_append(text, "(")) return 0;
  for(uint32_t i = 0; i < used_arguments; ++i){
    if(i && !text_append(text, ",")) return 0;
    int quote = argument_kinds && argument_kinds[i] == 1;
    if(!quote && i == 0 && argument_kinds && argument_kinds[i] == 2 &&
       (!strcmp(function_name, "action_message") || !strcmp(function_name, "action_draw_text") ||
        !strcmp(function_name, "action_question"))){
      const char *value = arguments[i] ? arguments[i] : "";
      int plain_text = strchr(value, ':') != NULL;
      for(const unsigned char *p=(const unsigned char*)value; !plain_text && *p; ++p){
        if(isspace(*p)) plain_text=1;
        if(p!=(const unsigned char*)value && strchr("+-*/()[]\"'",*p)){ plain_text=0; break; }
      }
      quote=plain_text;
    }
    if(quote){
      if(!text_append_quoted(text, arguments[i])) return 0;
    } else if(!text_append(text, arguments[i] && *arguments[i] ? arguments[i] : "0")) return 0;
  }
  if(append_relative){
    if(used_arguments && !text_append(text,",")) return 0;
    if(!text_append(text,relative?"1":"0")) return 0;
  }
  return text_append(text, ")");
}

static int action_code_has_open_block_comment(const char *code){
  enum { ACTION_NORMAL, ACTION_QUOTE, ACTION_LINE_COMMENT, ACTION_BLOCK_COMMENT } state=ACTION_NORMAL;
  char quote='\0';
  for(size_t i=0; code && code[i]; ++i){
    char ch=code[i], next=code[i+1];
    if(state==ACTION_QUOTE){
      if(ch=='\\' && next){ ++i; continue; }
      if(ch==quote) state=ACTION_NORMAL;
    } else if(state==ACTION_LINE_COMMENT){
      if(ch=='\n' || ch=='\r') state=ACTION_NORMAL;
    } else if(state==ACTION_BLOCK_COMMENT){
      if(ch=='*' && next=='/'){ state=ACTION_NORMAL; ++i; }
    } else if(ch=='"' || ch=='\''){
      state=ACTION_QUOTE; quote=ch;
    } else if(ch=='/' && next=='/'){
      state=ACTION_LINE_COMMENT; ++i;
    } else if(ch=='/' && next=='*'){
      state=ACTION_BLOCK_COMMENT; ++i;
    }
  }
  return state==ACTION_BLOCK_COMMENT;
}

static int emit_action_code_call(ImportText *text, const char *code,
                                 char **arguments, uint32_t *argument_kinds,
                                 uint32_t used_arguments){
  if(!text_append(text,"(function(){\n") || !text_append(text,code)) return 0;
  /* Each classic Execute Code action is compiled as a separate unit. An open
   * block comment therefore ends with that action; close it before appending
   * the synthetic function boundary used by the structural importer. */
  if(action_code_has_open_block_comment(code) && !text_append(text,"\n*/")) return 0;
  if(!text_append(text,"\n})(")) return 0;
  for(uint32_t i=0;i<used_arguments;i++){
    if(i && !text_append(text,",")) return 0;
    if(argument_kinds && argument_kinds[i]==1){
      if(!text_append_quoted(text,arguments[i])) return 0;
    } else if(!text_append(text,arguments[i] && *arguments[i] ? arguments[i] : "0")) return 0;
  }
  return text_append(text,")");
}

static int import_actions(ImportReader *r, ImportText *text){
  uint32_t list_version, count;
  if(!import_u32(r, &list_version, "action-list version") || !import_u32(r, &count, "action count")) return 0;
  (void)list_version;
  for(uint32_t action_index = 0; action_index < count; ++action_index){
    uint32_t action_version = 0, library_id = 0, action_id = 0, kind = 0;
    uint32_t may_relative = 0, question = 0, applies = 0, type = 0;
    uint32_t used_arguments = 0, kind_count = 0, target = 0, relative = 0;
    uint32_t argument_count = 0, negate = 0;
    char *function_name = NULL, *code = NULL;
    uint32_t *argument_kinds = NULL;
    char **arguments = NULL;
    int ok = import_u32(r, &action_version, "action version") &&
      import_u32(r, &library_id, "action library") && import_u32(r, &action_id, "action id") &&
      import_u32(r, &kind, "action kind") && import_u32(r, &may_relative, "action relative capability") &&
      import_u32(r, &question, "action question flag") && import_u32(r, &applies, "action target flag") &&
      import_u32(r, &type, "action type") && import_copy_string(r, &function_name, "action function") &&
      import_copy_string(r, &code, "action code") && import_u32(r, &used_arguments, "used action arguments") &&
      import_u32(r, &kind_count, "action argument-kind count");
    if(!ok) goto action_done;
    if(kind_count > 1024 || used_arguments > kind_count){ ok = 0; goto action_done; }
    argument_kinds = (uint32_t*)calloc(kind_count ? kind_count : 1, sizeof(*argument_kinds));
    if(!argument_kinds){ ok = 0; goto action_done; }
    for(uint32_t i = 0; i < kind_count; ++i)
      if(!import_u32(r, &argument_kinds[i], "action argument kind")){ ok = 0; goto action_done; }
    if(!import_u32(r, &target, "action target") || !import_u32(r, &relative, "action relative flag") ||
       !import_u32(r, &argument_count, "action argument count") || argument_count > 1024){ ok = 0; goto action_done; }
    arguments = (char**)calloc(argument_count ? argument_count : 1, sizeof(*arguments));
    if(!arguments){ ok = 0; goto action_done; }
    for(uint32_t i = 0; i < argument_count; ++i)
      if(!import_copy_string(r, &arguments[i], "action argument")){ ok = 0; goto action_done; }
    if(!import_u32(r, &negate, "action negation flag")){ ok = 0; goto action_done; }
    (void)action_version; (void)library_id; (void)action_id; (void)may_relative;
    if(kind == 1) ok = text_append(text, "{\n");
    else if(kind == 2) ok = text_append(text, "}\n");
    else if(kind == 3) ok = text_append(text, "else\n");
    else if(kind == 4) ok = text_append(text, "exit;\n");
    else if(kind == 5){
      ok = text_append(text, "repeat (") && text_append(text, argument_count && arguments[0][0] ? arguments[0] : "0") &&
           text_append(text, ")\n");
    } else if(kind == 6){
      const char *lhs = argument_count > 0 && arguments[0][0] ? arguments[0] : "__classic_variable";
      const char *rhs = argument_count > 1 && arguments[1][0] ? arguments[1] : "0";
      ok = text_append(text, lhs) && text_append(text, relative ? " += " : " = ") && text_append(text, rhs) && text_append(text, ";\n");
    } else {
      int wrapped_target = applies && !question && (int32_t)target != -1;
      if(wrapped_target){
        ok = text_append(text, "with (") && text_append_int(text, (int32_t)target) && text_append(text, ") {\n");
      }
      if(ok && relative && !question) ok = text_append(text, "action_set_relative(1);\n");
      if(ok && question) ok = text_append(text, "if (") && (!negate || text_append(text, "!"));
      if(ok){
        if(type == 2 || kind == 7){
          if(code && *code)
            ok = emit_action_code_call(text,code,arguments,argument_kinds,
              used_arguments < argument_count ? used_arguments : argument_count);
          else {
            const char *action_code = argument_count && arguments[0] && arguments[0][0] ?
              arguments[0] : "/* empty code action */";
            /* Execute Code is one D&D action, not the whole event. An
             * `exit` inside it ends that action before the dispatcher continues with later actions.
             * A small invoked function preserves that boundary after the action list is
             * normalized into one source file. */
            ok = emit_action_code_call(text,action_code,NULL,NULL,0);
          }
        }
        else ok = emit_action_call(text, function_name, arguments, argument_kinds,
                                   used_arguments < argument_count ? used_arguments : argument_count,
                                   question, relative!=0);
      }
      if(ok && question) ok = text_append(text, ")\n");
      else if(ok) ok = text_append(text, ";\n");
      if(ok && relative && !question) ok = text_append(text, "action_set_relative(0);\n");
      if(ok && wrapped_target) ok = text_append(text, "}\n");
    }
action_done:
    for(uint32_t i = 0; arguments && i < argument_count; ++i) free(arguments[i]);
    free(arguments); free(argument_kinds); free(function_name); free(code);
    if(!ok){
      if(r->err && r->errcap && !r->err[0]) snprintf(r->err, r->errcap, "classic import: invalid action %u", action_index);
      return 0;
    }
  }
  return 1;
}

static void free_imported_objects(GmlcProject *project){
  for(int i = 0; i < project->n_objects; ++i){
    GmlcObject *object = &project->objects[i];
    free(object->id); free(object->name);
    for(int event = 0; event < object->n_events; ++event){
      free(object->events[event].id); free(object->events[event].collision_id);
      free(object->events[event].source_path);
    }
    free(object->events);
  }
  free(project->objects);
  project->objects = NULL;
  project->n_objects = project->cap_objects = 0;
}

static int append_object_event(GmlcObject *object, GmlcObjectEvent event){
  if(object->n_events >= object->cap_events){
    int capacity = object->cap_events ? object->cap_events * 2 : 4;
    GmlcObjectEvent *events = (GmlcObjectEvent*)realloc(object->events, (size_t)capacity * sizeof(*events));
    if(!events) return 0;
    object->events = events;
    object->cap_events = capacity;
  }
  object->events[object->n_events++] = event;
  return 1;
}

static int classic_sprite_project_index(const GmlcProject *project, int32_t runtime_id){
  if(runtime_id < 0) return -1;
  for(int i = 0; i < project->n_sprites; ++i)
    if(project->sprites[i].runtime_id == runtime_id) return i;
  return -1;
}

int gmlc_classic_import_objects(const GmlcClassicManifest *classic,
                                GmlcProject *project, const char *cache_dir,
                                char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->objects || project->n_objects){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid object-import arguments");
    return 0;
  }
  uint32_t count = classic->inventory.resource_slots[GMLC_CLASSIC_OBJECT];
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many object slots");
    return 0;
  }
  project->objects = (GmlcObject*)calloc(count ? count : 1, sizeof(*project->objects));
  if(!project->objects){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating objects");
    return 0;
  }
  project->n_objects = project->cap_objects = (int)count;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_OBJECT];
  for(uint32_t i = 0; i < count; ++i){
    char fallback[64];
    snprintf(fallback, sizeof(fallback), "__classic_missing_object_%u", i);
    const char *name = slots[i].exists && slots[i].name ? slots[i].name : fallback;
    project->objects[i].id = copy_string(name);
    project->objects[i].name = copy_string(name);
    project->objects[i].sprite_id = project->objects[i].mask_id = -1;
    project->objects[i].parent_id = -100;
    project->objects[i].visible = slots[i].exists ? 1 : 0;
    if(!project->objects[i].id || !project->objects[i].name){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory naming object %u", i);
      free_imported_objects(project);
      return 0;
    }
  }
  for(uint32_t i = 0; i < count; ++i){
    const GmlcClassicResourceSlot *source = &slots[i];
    if(!source->exists) continue;
    ImportReader r = {source->payload, source->payload_size, 0, err, errcap};
    uint32_t sprite, solid, visible, depth, persistent, parent, mask, last_event_type;
    if(!import_u32(&r, &sprite, "object sprite") || !import_u32(&r, &solid, "object solid flag") ||
       !import_u32(&r, &visible, "object visible flag") || !import_u32(&r, &depth, "object depth") ||
       !import_u32(&r, &persistent, "object persistent flag") || !import_u32(&r, &parent, "object parent") ||
       !import_u32(&r, &mask, "object mask") || !import_u32(&r, &last_event_type, "object event-type count") ||
       last_event_type > 64){ free_imported_objects(project); return 0; }
    GmlcObject *object = &project->objects[i];
    object->sprite_id = classic_sprite_project_index(project, (int32_t)sprite);
    object->solid = solid != 0; object->visible = visible != 0;
    object->depth = (int32_t)depth; object->persistent = persistent != 0;
    object->parent_id = (int32_t)parent;
    object->mask_id = classic_sprite_project_index(project, (int32_t)mask);
    for(uint32_t event_type = 0; event_type <= last_event_type; ++event_type){
      for(;;){
        uint32_t event_number;
        if(!import_u32(&r, &event_number, "object event number")){ free_imported_objects(project); return 0; }
        if(event_number == UINT32_MAX) break;
        ImportText text = {0};
        if(!import_actions(&r, &text)){
          free(text.data); free_imported_objects(project); return 0;
        }
        if(!text.data && !text_append(&text, "exit;\n")){
          free_imported_objects(project); return 0;
        }
        char leaf[112], event_id[64];
        snprintf(leaf, sizeof(leaf), "classic_object_%06u_event_%02u_%010u.gml", i, event_type, event_number);
        snprintf(event_id, sizeof(event_id), "classic_event_%u_%u_%u", i, event_type, event_number);
        GmlcObjectEvent event;
        memset(&event, 0, sizeof(event));
        event.id = copy_string(event_id);
        event.event_type = (int)event_type;
        event.event_number = (int32_t)event_number;
        event.collision_object_id = event_type == 4 ? (int32_t)event_number : -1;
        event.source_path = import_source_path(project,cache_dir,leaf,text.data,err,errcap);
        if(!event.id || !event.source_path ||
           !append_object_event(object, event)){
          free(event.id); free(event.source_path); free(text.data);
          if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: out of memory importing object event");
          free_imported_objects(project); return 0;
        }
        free(text.data);
      }
    }
    if(r.pos != r.size){
      if(err && errcap) snprintf(err, errcap, "classic import: trailing object payload");
      free_imported_objects(project); return 0;
    }
  }
  return 1;
}

static void free_imported_timelines(GmlcProject *project){
  for(int i=0;i<project->n_timelines;i++){
    free(project->timelines[i].id); free(project->timelines[i].name);
    for(int m=0;m<project->timelines[i].n_moments;m++) free(project->timelines[i].moments[m].source_path);
    free(project->timelines[i].moments);
  }
  free(project->timelines);
  project->timelines=NULL;
  project->n_timelines=project->cap_timelines=0;
}

int gmlc_classic_import_timelines(const GmlcClassicManifest *classic,
                                  GmlcProject *project, const char *cache_dir,
                                  char *err, size_t errcap){
  if(err && errcap) err[0]='\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->timelines || project->n_timelines){
    if(err && errcap) snprintf(err,errcap,"classic import: invalid timeline-import arguments");
    return 0;
  }
  uint32_t count=classic->inventory.resource_slots[GMLC_CLASSIC_TIMELINE];
  if(count>INT32_MAX){ if(err && errcap) snprintf(err,errcap,"classic import: too many timeline slots"); return 0; }
  project->timelines=(GmlcTimeline*)calloc(count?count:1,sizeof(*project->timelines));
  if(!project->timelines){ if(err && errcap) snprintf(err,errcap,"classic import: out of memory allocating timelines"); return 0; }
  project->n_timelines=project->cap_timelines=(int)count;
  const GmlcClassicResourceSlot *slots=classic->slots[GMLC_CLASSIC_TIMELINE];
  for(uint32_t i=0;i<count;i++){
    char fallback[64];
    snprintf(fallback,sizeof(fallback),"__classic_missing_timeline_%u",i);
    const char *name=slots[i].exists && slots[i].name?slots[i].name:fallback;
    GmlcTimeline *timeline=&project->timelines[i];
    timeline->id=copy_string(name); timeline->name=copy_string(name);
    if(!timeline->id || !timeline->name){ free_imported_timelines(project); return 0; }
    if(!slots[i].exists) continue;
    ImportReader r={slots[i].payload,slots[i].payload_size,0,err,errcap};
    uint32_t moments;
    if(!import_u32(&r,&moments,"timeline moment count") || moments>INT32_MAX){
      free_imported_timelines(project); return 0;
    }
    timeline->moments=(GmlcTimelineMoment*)calloc(moments?moments:1,sizeof(*timeline->moments));
    if(!timeline->moments){ free_imported_timelines(project); return 0; }
    timeline->n_moments=timeline->cap_moments=(int)moments;
    for(uint32_t moment=0;moment<moments;moment++){
      uint32_t step;
      ImportText text={0};
      if(!import_u32(&r,&step,"timeline moment step") || !import_actions(&r,&text)){
        free(text.data); free_imported_timelines(project); return 0;
      }
      if(!text.data && !text_append(&text,"exit;\n")){ free_imported_timelines(project); return 0; }
      char leaf[112];
      snprintf(leaf,sizeof(leaf),"classic_timeline_%06u_moment_%06u.gml",i,moment);
      timeline->moments[moment].step=(int32_t)step;
      timeline->moments[moment].source_path=import_source_path(project,cache_dir,leaf,
                                                               text.data,err,errcap);
      if(!timeline->moments[moment].source_path){
        free(text.data); free_imported_timelines(project); return 0;
      }
      free(text.data);
    }
    if(r.pos!=r.size){
      if(err && errcap) snprintf(err,errcap,"classic import: trailing timeline payload");
      free_imported_timelines(project); return 0;
    }
  }
  return 1;
}

static void free_imported_rooms(GmlcProject *project){
  for(int i = 0; i < project->n_rooms; ++i){
    GmlcRoom *room = &project->rooms[i];
    free(room->id); free(room->name); free(room->creation_code_path);
    free(room->backgrounds); free(room->tiles);
    for(int instance = 0; instance < room->n_instances; ++instance){
      free(room->instances[instance].id); free(room->instances[instance].name);
      free(room->instances[instance].creation_code_path);
    }
    free(room->instances);
  }
  free(project->rooms);
  project->rooms = NULL;
  project->n_rooms = project->cap_rooms = 0;
}

static int import_room_code(ImportReader *r, GmlcProject *project,
                            const char *cache_dir, const char *leaf,
                            char **out_path, char *err, size_t errcap){
  char *code = NULL;
  *out_path = NULL;
  if(!import_copy_string(r, &code, "room creation code")) return 0;
  if(code[0]){
    *out_path = import_source_path(project,cache_dir,leaf,code,err,errcap);
    if(!*out_path){
      free(code); free(*out_path); *out_path = NULL;
      return 0;
    }
  }
  free(code);
  return 1;
}

int gmlc_classic_import_rooms(const GmlcClassicManifest *classic,
                              GmlcProject *project, const char *cache_dir,
                              char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->rooms || project->n_rooms){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid room-import arguments");
    return 0;
  }
  uint32_t count = classic->inventory.resource_slots[GMLC_CLASSIC_ROOM];
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many room slots");
    return 0;
  }
  project->rooms = (GmlcRoom*)calloc(count ? count : 1, sizeof(*project->rooms));
  if(!project->rooms){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating rooms");
    return 0;
  }
  project->n_rooms = project->cap_rooms = (int)count;
  project->next_instance_id = classic->inventory.last_instance_id < INT32_MAX
    ? (int)classic->inventory.last_instance_id + 1 : INT32_MAX;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_ROOM];
  for(uint32_t i = 0; i < count; ++i){
    GmlcRoom *room = &project->rooms[i];
    char fallback[64];
    snprintf(fallback, sizeof(fallback), "__classic_missing_room_%u", i);
    const char *name = slots[i].exists && slots[i].name ? slots[i].name : fallback;
    room->id = copy_string(name); room->name = copy_string(name);
    room->width = 640; room->height = 480; room->speed = 30;
    room->background_color = 0xFF000000u; room->draw_background_color = 1;
    for(int view = 0; view < 8; ++view){
      room->views[view].wview = room->views[view].wport = room->width;
      room->views[view].hview = room->views[view].hport = room->height;
      room->views[view].hspeed = room->views[view].vspeed = -1;
      room->views[view].object_id = -1;
    }
    if(!room->id || !room->name){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory naming room %u", i);
      free_imported_rooms(project); return 0;
    }
    if(!slots[i].exists) continue;
    ImportReader r = {slots[i].payload, slots[i].payload_size, 0, err, errcap};
    const uint8_t *caption = NULL; uint32_t caption_length = 0;
    uint32_t fields[9];
    if(!import_skip_string(&r, &caption, &caption_length, "room caption")){
      free_imported_rooms(project); return 0;
    }
    (void)caption; (void)caption_length;
    for(int field = 0; field < 9; ++field)
      if(!import_u32(&r, &fields[field], "room field")){
        free_imported_rooms(project); return 0;
      }
    room->width = (int32_t)fields[0]; room->height = (int32_t)fields[1];
    room->speed = (int32_t)fields[5]; room->persistent = fields[6] != 0;
    room->background_color = fields[7] | 0xFF000000u;
    room->draw_background_color = fields[8] != 0;
    if(getenv("GMLC_LOG_ROOM"))
      fprintf(stderr,"[classic-room] index=%u size=%dx%d speed=%d persistent=%d colour=%08x clear=%d\n",
              i,room->width,room->height,room->speed,room->persistent,
              room->background_color,room->draw_background_color);
    char leaf[112];
    snprintf(leaf, sizeof(leaf), "classic_room_%06u_create.gml", i);
    if(!import_room_code(&r,project,cache_dir,leaf,&room->creation_code_path,err,errcap)){
      free_imported_rooms(project); return 0;
    }
    uint32_t backgrounds;
    if(!import_u32(&r, &backgrounds, "room background count") || backgrounds > INT32_MAX){
      free_imported_rooms(project); return 0;
    }
    room->backgrounds = (GmlcRoomBackground*)calloc(backgrounds ? backgrounds : 1,
                                                     sizeof(*room->backgrounds));
    if(!room->backgrounds){ free_imported_rooms(project); return 0; }
    room->n_backgrounds = room->cap_backgrounds = (int)backgrounds;
    for(uint32_t background = 0; background < backgrounds; ++background){
      uint32_t value[10];
      for(int field = 0; field < 10; ++field)
        if(!import_u32(&r, &value[field], "room background field")){
          free_imported_rooms(project); return 0;
        }
      GmlcRoomBackground *bg = &room->backgrounds[background];
      bg->visible = value[0] != 0; bg->foreground = value[1] != 0;
      bg->background_id = (int32_t)value[2]; bg->x = (int32_t)value[3]; bg->y = (int32_t)value[4];
      bg->htiled = value[5] != 0; bg->vtiled = value[6] != 0;
      bg->hspeed = (int32_t)value[7]; bg->vspeed = (int32_t)value[8]; bg->stretch = value[9] != 0;
    }
    uint32_t view_enabled, views;
    if(!import_u32(&r, &view_enabled, "room view-enabled flag") ||
       !import_u32(&r, &views, "room view count") || views > 1024){
      free_imported_rooms(project); return 0;
    }
    room->view_enabled = view_enabled != 0; room->n_views = views < 8 ? (int)views : 8;
    for(uint32_t view = 0; view < views; ++view){
      uint32_t value[14];
      for(int field = 0; field < 14; ++field)
        if(!import_u32(&r, &value[field], "room view field")){
          free_imported_rooms(project); return 0;
        }
      if(view >= 8) continue;
      GmlcRoomView *target = &room->views[view];
      target->visible = value[0] != 0;
      target->xview = (int32_t)value[1]; target->yview = (int32_t)value[2];
      target->wview = (int32_t)value[3]; target->hview = (int32_t)value[4];
      target->xport = (int32_t)value[5]; target->yport = (int32_t)value[6];
      target->wport = (int32_t)value[7]; target->hport = (int32_t)value[8];
      target->hborder = (int32_t)value[9]; target->vborder = (int32_t)value[10];
      target->hspeed = (int32_t)value[11]; target->vspeed = (int32_t)value[12];
      target->object_id = (int32_t)value[13];
    }
    if(room->n_views){
      room->view_w = room->views[0].wview; room->view_h = room->views[0].hview;
      room->port_w = room->views[0].wport; room->port_h = room->views[0].hport;
    } else {
      room->view_w = room->port_w = room->width;
      room->view_h = room->port_h = room->height;
    }
    uint32_t instances;
    if(!import_u32(&r, &instances, "room instance count") || instances > INT32_MAX){
      free_imported_rooms(project); return 0;
    }
    room->instances = (GmlcRoomInstance*)calloc(instances ? instances : 1, sizeof(*room->instances));
    if(!room->instances){ free_imported_rooms(project); return 0; }
    room->n_instances = room->cap_instances = (int)instances;
    for(uint32_t instance = 0; instance < instances; ++instance){
      uint32_t x, y, object_id, instance_id, locked;
      if(!import_u32(&r, &x, "room instance x") || !import_u32(&r, &y, "room instance y") ||
         !import_u32(&r, &object_id, "room instance object") ||
         !import_u32(&r, &instance_id, "room instance id")){
        free_imported_rooms(project); return 0;
      }
      GmlcRoomInstance *target = &room->instances[instance];
      char instance_name[80], instance_leaf[128];
      snprintf(instance_name, sizeof(instance_name), "classic_instance_%u", instance_id);
      snprintf(instance_leaf, sizeof(instance_leaf), "classic_room_%06u_instance_%010u.gml", i, instance_id);
      target->id = copy_string(instance_name); target->name = copy_string(instance_name);
      target->x = (int32_t)x; target->y = (int32_t)y; target->object_id = (int32_t)object_id;
      target->instance_id = (int32_t)instance_id; target->sx = target->sy = 1.0f;
      target->color = 0xFFFFFFFFu;
      if(!target->id || !target->name ||
         !import_room_code(&r,project,cache_dir,instance_leaf,&target->creation_code_path,err,errcap) ||
         !import_u32(&r, &locked, "room instance locked flag")){
        free_imported_rooms(project); return 0;
      }
      (void)locked;
    }
    uint32_t tiles;
    if(!import_u32(&r, &tiles, "room tile count") || tiles > INT32_MAX){
      free_imported_rooms(project); return 0;
    }
    room->tiles = (GmlcRoomTile*)calloc(tiles ? tiles : 1, sizeof(*room->tiles));
    if(!room->tiles){ free_imported_rooms(project); return 0; }
    room->n_tiles = room->cap_tiles = (int)tiles;
    for(uint32_t tile = 0; tile < tiles; ++tile){
      uint32_t value[10];
      for(int field = 0; field < 10; ++field)
        if(!import_u32(&r, &value[field], "room tile field")){
          free_imported_rooms(project); return 0;
        }
      GmlcRoomTile *target = &room->tiles[tile];
      target->x = (int32_t)value[0]; target->y = (int32_t)value[1];
      target->background_id = (int32_t)value[2]; target->source_x = (int32_t)value[3];
      target->source_y = (int32_t)value[4]; target->width = (int32_t)value[5];
      target->height = (int32_t)value[6]; target->depth = (int32_t)value[7];
      target->tile_id = (int32_t)value[8];
    }
    uint32_t editor_field;
    for(int field = 0; field < 14; ++field)
      if(!import_u32(&r, &editor_field, "room editor field")){
        free_imported_rooms(project); return 0;
      }
    if(r.pos != r.size){
      if(err && errcap) snprintf(err, errcap, "classic import: trailing room payload");
      free_imported_rooms(project); return 0;
    }
  }
  return 1;
}

int gmlc_classic_import_room_order(const GmlcClassicManifest *classic,
                                   GmlcProject *project,
                                   char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid room-order arguments");
    return 0;
  }
  uint32_t slots_count = classic->inventory.resource_slots[GMLC_CLASSIC_ROOM];
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_ROOM];
  if(project->room_order || project->n_room_order){
    /* The synthetic room used by a genuinely roomless project already owns a
     * complete one-entry order. */
    if(!slots_count && project->room_order && project->n_room_order == 1) return 1;
    if(err && errcap) snprintf(err, errcap, "classic import: room order is already populated");
    return 0;
  }
  if(slots_count && !slots){
    if(err && errcap) snprintf(err, errcap, "classic import: room slots are unavailable");
    return 0;
  }

  uint32_t count = classic->room_order_count;
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many ordered rooms");
    return 0;
  }
  if(!count){
    if(classic->existing[GMLC_CLASSIC_ROOM]){
      if(err && errcap) snprintf(err, errcap, "classic import: explicit room order is unavailable");
      return 0;
    }
    return 1;
  }
  if(count != classic->existing[GMLC_CLASSIC_ROOM]){
    if(err && errcap) snprintf(err, errcap, "classic import: room order does not cover every room");
    return 0;
  }

  int *order = (int*)calloc(count, sizeof(*order));
  unsigned char *seen = (unsigned char*)calloc(slots_count ? slots_count : 1, 1);
  if(!order || !seen){
    free(order); free(seen);
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory recording room order");
    return 0;
  }

  uint32_t written = 0;
  for(uint32_t i = 0; i < classic->room_order_count; ++i){
    uint32_t slot = classic->room_order[i];
    if(slot >= slots_count || !slots[slot].exists || seen[slot]){
      free(order); free(seen);
      if(err && errcap) snprintf(err, errcap, "classic import: invalid room order entry %u", slot);
      return 0;
    }
    seen[slot] = 1;
    order[written++] = (int)slot;
  }
  free(seen);
  project->room_order = order;
  project->n_room_order = (int)written;
  return 1;
}
