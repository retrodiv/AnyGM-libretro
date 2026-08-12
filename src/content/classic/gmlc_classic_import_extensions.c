/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_classic_import_internal.h"

#include "anygm_host.h"
#include "anygm_vfs.h"
#include "gml_image_codec.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CLASSIC_EXTENSION_FILE_LIMIT = 16 * 1024 * 1024,
  CLASSIC_EXTENSION_COMPRESSED_SCRIPT_LIMIT = 4 * 1024 * 1024,
  CLASSIC_EXTENSION_SCRIPT_LIMIT = 8 * 1024 * 1024,
  CLASSIC_EXTENSION_EXTERNAL_NAME_LIMIT = 4096
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
  int needs_script;
} ClassicExtensionAlias;

/* Binary-extension calls cannot retain a native function pointer in a portable
 * package. Encode the declared library and symbol in a valid GML identifier.
 * The runtime compatibility owner decodes this self-describing name and may
 * provide a portable implementation; unknown APIs remain unknown. */
static char *classic_extension_external_target(const char *library,
                                               const char *symbol){
  static const char prefix[]="__anygm_external_";
  static const char hex[]="0123456789abcdef";
  size_t library_size=library?strlen(library):0;
  size_t symbol_size=symbol?strlen(symbol):0;
  if(!library_size || !symbol_size ||
     library_size>CLASSIC_EXTENSION_EXTERNAL_NAME_LIMIT ||
     symbol_size>CLASSIC_EXTENSION_EXTERNAL_NAME_LIMIT ||
     library_size>(SIZE_MAX-sizeof(prefix)-2u)/2u) return NULL;
  size_t size=sizeof(prefix)-1u+library_size*2u+1u;
  if(symbol_size>(SIZE_MAX-size-1u)/2u) return NULL;
  size+=symbol_size*2u;
  char *target=(char*)malloc(size+1u);
  if(!target) return NULL;
  size_t at=0;
  memcpy(target,prefix,sizeof(prefix)-1u); at+=sizeof(prefix)-1u;
  for(size_t i=0;i<library_size;i++){
    unsigned char value=(unsigned char)library[i];
    target[at++]=hex[value>>4]; target[at++]=hex[value&15u];
  }
  target[at++]='_';
  for(size_t i=0;i<symbol_size;i++){
    unsigned char value=(unsigned char)symbol[i];
    target[at++]=hex[value>>4]; target[at++]=hex[value&15u];
  }
  target[at]='\0';
  return target;
}



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
    size_t inflated_size=0;
    if(gml_deflate_decode_to_buffer(compressed,compressed_size,GML_DEFLATE_ZLIB,
                                    (uint8_t*)decoded,capacity,&inflated_size) &&
       inflated_size<=(size_t)INT_MAX)
      decoded_size=(int)inflated_size;
    else
      decoded_size=-1;
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

static int classic_extension_import_manifest_aliases(
  const GmlcClassicManifest *classic,GmlcProject *project
){
  if(!classic->extensions || !classic->extension_detail_count) return 1;
  for(uint32_t extension=0;extension<classic->extension_detail_count;extension++){
    const GmlcClassicExtension *detail=&classic->extensions[extension];
    for(uint32_t file_index=0;file_index<detail->file_count;file_index++){
      const GmlcClassicExtensionFile *file=&detail->files[file_index];
      if(file->kind!=1 || !file->name || !file->name[0]) continue;
      for(uint32_t function=0;function<file->function_count;function++){
        const GmlcClassicExtensionFunction *item=&file->functions[function];
        if(!item->name || !item->name[0] ||
           !item->external_name || !item->external_name[0]) continue;
        char *target=classic_extension_external_target(file->name,item->external_name);
        if(!target || !classic_extension_add_project_alias(project,item->name,target)){
          free(target);
          return 0;
        }
        free(target);
      }
    }
  }
  return 1;
}

static int classic_extension_add_project_constant(GmlcProject *project,
                                                   const char *name,
                                                   const char *expression){
  if(!name || !name[0] || !expression) return 1;
  /* Project constants are loaded before sibling extension packages and remain
   * authoritative on a duplicate name.  Likewise, the first declared package
   * wins when two installed copies expose the same constant. */
  for(int i=0;i<project->n_constants;i++)
    if(classic_extension_identifier_equal(project->constants[i].name,name)) return 1;
  if(project->n_constants>=project->cap_constants){
    if(project->cap_constants>INT32_MAX/2) return 0;
    int capacity=project->cap_constants ? project->cap_constants*2 : 16;
    if(capacity<=project->cap_constants || capacity<project->n_constants+1) return 0;
    GmlcProjectConstant *constants=(GmlcProjectConstant*)realloc(
      project->constants,(size_t)capacity*sizeof(*constants));
    if(!constants) return 0;
    memset(constants+project->cap_constants,0,
           (size_t)(capacity-project->cap_constants)*sizeof(*constants));
    project->constants=constants;
    project->cap_constants=capacity;
  }
  GmlcProjectConstant *constant=&project->constants[project->n_constants];
  constant->name=copy_string(name);
  constant->expression=copy_string(expression);
  if(!constant->name || !constant->expression){
    free(constant->name); free(constant->expression);
    memset(constant,0,sizeof(*constant));
    return 0;
  }
  project->n_constants++;
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

static int classic_extension_names(const AnygmHostServices *host,const char *project_dir,
                                   char ***names_out, size_t *count_out){
  *names_out=NULL; *count_out=0;
  if(!host || !host->directory_open || !host->directory_read || !host->directory_close) return 1;
  void *directory=host->directory_open(host->userdata,project_dir);
  if(!directory) return 1;
  char **names=NULL;
  size_t count=0,capacity=0;
  for(;;){
    AnygmDirectoryEntry entry;
    memset(&entry,0,sizeof entry);
    entry.struct_size=sizeof entry;
    AnygmResult result=host->directory_read(host->userdata,directory,&entry);
    if(result==ANYGM_RESULT_END) break;
    if(result!=ANYGM_OK){ classic_extension_names_free(names,count);
      host->directory_close(host->userdata,directory); return 0; }
    if(!classic_extension_suffix(entry.name)) continue;
    if(count==capacity){
      size_t next=capacity ? capacity*2u : 8u;
      char **grown=(char**)realloc(names,next*sizeof(*grown));
      if(!grown){ classic_extension_names_free(names,count);
        host->directory_close(host->userdata,directory); return 0; }
      names=grown; capacity=next;
    }
    names[count]=gmlc_strdup(entry.name);
    if(!names[count]){ classic_extension_names_free(names,count);
      host->directory_close(host->userdata,directory); return 0; }
    count++;
  }
  host->directory_close(host->userdata,directory);
  if(count>1) qsort(names,count,sizeof(*names),classic_extension_name_compare);
  *names_out=names; *count_out=count;
  return 1;
}

static int classic_extension_read_prefix(const AnygmHostServices *host,const char *path,
                                         uint8_t **data,size_t *size){
  *data=NULL; *size=0;
  size_t wanted=CLASSIC_EXTENSION_FILE_LIMIT;
  AnygmFileInfo info;
  if(anygm_vfs_stat(host,path,&info) && info.size<SIZE_MAX && info.size<wanted)
    wanted=(size_t)info.size;
  if(wanted<12) return 0;
  uint8_t *bytes=(uint8_t*)malloc(wanted);
  if(!bytes) return -1;
  size_t read=0;
  if(!anygm_vfs_read_prefix(host,path,bytes,wanted,&read) || read<12){ free(bytes); return 0; }
  *data=bytes; *size=read;
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

int gmlc_classic_extension_dependency_hash(const AnygmHostServices *host,
                                           const char *project_dir,
                                           uint64_t seed, uint64_t *hash_out){
  if(!project_dir || !*project_dir || !hash_out) return 0;
  char **names=NULL; size_t count=0;
  if(!classic_extension_names(host,project_dir,&names,&count)) return 0;
  uint64_t hash=seed;
  const uint8_t domain[4]={'G','E','X',0};
  classic_extension_hash_bytes(&hash,domain,sizeof(domain));
  uint64_t live=0;
  for(size_t i=0;i<count;i++){
    char *path=gmlc_path_join(project_dir,names[i]);
    uint8_t *data=NULL; size_t size=0;
    int read=path ? classic_extension_read_prefix(host,path,&data,&size) : -1;
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
  if(!classic_extension_import_manifest_aliases(classic,project)){
    if(err && errcap)
      snprintf(err,errcap,"classic import: invalid, oversized, or unavailable binary extension metadata");
    return 0;
  }
  char **names=NULL; size_t count=0;
  int ok=classic_extension_names(project->host,project_dir,&names,&count);
  for(size_t i=0;ok && i<count;i++){
    char *path=gmlc_path_join(project_dir,names[i]);
    if(!path){ ok=0; break; }
    uint8_t *data=NULL; size_t size=0;
    int read=classic_extension_read_prefix(project->host,path,&data,&size);
    free(path);
    if(read<0){ ok=0; break; }
    if(read>0){
      int parsed=classic_extension_parse(classic,project,data,size);
      free(data);
      if(anygm_host_development_setting(project->host,"GMLC_LOG_CLASSIC_EXTENSIONS"))
        anygm_host_logf(project ? project->host : NULL,ANYGM_LOG_DEBUG,"classic extension package: %s (%s)\n",names[i],
                parsed>0?"parsed":parsed<0?"out of memory":"ignored malformed metadata");
      if(parsed<0){ ok=0; break; }
    }
  }
  classic_extension_names_free(names,count);
  if(!ok && err && errcap)
    snprintf(err,errcap,"classic import: out of memory reading extension metadata");
  if(ok && anygm_host_development_setting(project->host,"GMLC_LOG_CLASSIC_EXTENSIONS")){
    anygm_host_logf(project ? project->host : NULL,ANYGM_LOG_DEBUG,"classic extensions: imported %d script(s), retained %d alias(es) from %s\n",
            project->n_scripts-scripts_before,
            project->n_function_aliases-aliases_before,project_dir);
    for(int i=aliases_before;i<project->n_function_aliases;i++){
      const GmlcFunctionAlias *alias=&project->function_aliases[i];
      anygm_host_logf(project ? project->host : NULL,ANYGM_LOG_DEBUG,"classic extension alias: %s -> %s%s\n",
              alias->public_name,alias->target_name,alias->ambiguous?" (ambiguous)":"");
    }
  }
  return ok;
}
