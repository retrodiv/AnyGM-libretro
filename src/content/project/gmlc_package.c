/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_package.h"
#include "gmlc_package_internal.h"
#include "anygm_host.h"
#include "anygm_vfs.h"
#include "gmlc_bytecode.h"
#include "gml_image_codec.h"
#include "gml_win.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

static int development_flag_enabled(const AnygmHostServices *host,const char *name){
  const char *v=anygm_host_development_setting(host,name);
  if(!v || !*v) return 0;
  if(!strcmp(v,"0") || !strcmp(v,"false") || !strcmp(v,"FALSE") ||
     !strcmp(v,"no") || !strcmp(v,"NO")){
    return 0;
  }
  return 1;
}

static int write_file(const GmlcProject *project,const char *path,const uint8_t *data,
                      size_t len,char *err,size_t errcap){
  if(project&&anygm_vfs_write_all(project->host,path,data,len)) return 1;
  snprintf(err,errcap,"%s: write failed",path);
  return 0;
}

static int safe_relative_folder(const char *folder){
  if(!folder || !*folder) return 1;
  if(folder[0]=='/' || folder[0]=='\\' || strchr(folder,':')) return 0;
  const char *part=folder;
  for(const char *cursor=folder;;cursor++) if(!*cursor || *cursor=='/' || *cursor=='\\'){
    size_t length=(size_t)(cursor-part);
    if(length==2 && part[0]=='.' && part[1]=='.') return 0;
    if(!*cursor) break;
    part=cursor+1;
  }
  return 1;
}

static const char *package_leaf_name(const char *name){
  const char *leaf=name?name:"";
  for(const char *cursor=leaf;*cursor;cursor++)
    if(*cursor=='/' || *cursor=='\\') leaf=cursor+1;
  return leaf;
}

static int materialize_included_files(const GmlcProject *project, const char *out_path,
                                      char *err, size_t errcap){
  if(!project || project->n_included_files<=0) return 1;
  char *base=gmlc_path_dirname(out_path);
  if(!base) return 0;
  for(int i=0;i<project->n_included_files;i++){
    const GmlcProjectIncludedFile *included=&project->included_files[i];
    if(included->export_mode==0 || !included->data) continue;
    const char *leaf=package_leaf_name(included->file_name);
    if(!*leaf || !strcmp(leaf,".") || !strcmp(leaf,"..") ||
       !safe_relative_folder(included->custom_folder)){
      if(err && errcap) snprintf(err,errcap,"unsafe included-file export path");
      free(base); return 0;
    }
    char *folder=included->export_mode>2 && included->custom_folder && *included->custom_folder
      ? gmlc_path_join(base,included->custom_folder) : gmlc_strdup(base);
    if(!folder || !anygm_vfs_mkdirs(project->host,folder)){
      if(err && errcap) snprintf(err,errcap,"cannot create included-file export directory");
      free(folder); free(base); return 0;
    }
    char *path=gmlc_path_join(folder,leaf);
    free(folder);
    if(!path){ free(base); return 0; }
    if(!included->overwrite_file){
      AnygmFileInfo existing;
      if(anygm_vfs_stat(project->host,path,&existing)){ free(path); continue; }
    }
    if(!write_file(project,path,included->data,included->data_size,err,errcap)){
      free(path); free(base); return 0;
    }
    free(path);
  }
  free(base);
  return 1;
}

static void free_pkg(Pkg *pkg){
  free(pkg->b.data);
  free(pkg->code_data.data);
  for(int i=0;i<pkg->strs.n;i++) free(pkg->strs.items[i]);
  free(pkg->strs.items);
  free(pkg->strs.char_off);
  free(pkg->strs.hash_slots);
  free(pkg->patches);
  free(pkg->frame_patches);
  free(pkg->font_patches);
  free(pkg->frame_tpag_ptr);
  free(pkg->font_tpag_ptr);
  free(pkg->texture_place);
  free(pkg->code_blob_patches);
  free(pkg->code_name_refs);
  for(int i=0;i<pkg->n_code_refs;i++) free(pkg->code_refs[i].name);
  free(pkg->code_refs);
  free_ref_texture_layout(&pkg->ref_tex);
}

int gmlc_package_write_structural(const GmlcProject *p, const char *out_path, char *err, size_t errcap){
  Pkg pkg;
  memset(&pkg,0,sizeof(pkg));
  pkg.code_placeholders=development_flag_enabled(p?p->host:NULL,"GMLC_CODE_PLACEHOLDERS");
  pkg.fail_on_placeholder=development_flag_enabled(p?p->host:NULL,"GMLC_FAIL_ON_PLACEHOLDER");
  pkg.log_code_compile=development_flag_enabled(p?p->host:NULL,"GMLC_LOG_CODE_COMPILE");
  const char *ref_path=anygm_host_development_setting(p?p->host:NULL,"GMLC_REFERENCE_WIN");
  if(ref_path && *ref_path && !load_reference_texture_layout(&pkg,p,ref_path,err,errcap)){
    free_pkg(&pkg);
    return 0;
  }
  wbytes(&pkg.b,"FORM",4);
  size_t form_size_pos=pkg.b.len;
  wu32(&pkg.b,0);
  const char *stage="initialization";
#define PACKAGE_STEP(label, call) (stage=(label),(call))
  if(!PACKAGE_STEP("code-string seed",seed_code_string_order(&pkg,p,err,errcap)) ||
     !PACKAGE_STEP("general info",write_gen8(&pkg,p)) ||
     !PACKAGE_STEP("classic marker",write_classic_marker(&pkg,p)) ||
     !PACKAGE_STEP("options",write_optn(&pkg)) ||
     !PACKAGE_STEP("language",fixed_zero_chunk(&pkg,"LANG",12)) ||
     !PACKAGE_STEP("extensions",empty_list_chunk(&pkg,"EXTN")) ||
     !PACKAGE_STEP("sounds",write_sond(&pkg,p)) ||
     !PACKAGE_STEP("audio groups",write_agrp(&pkg)) ||
     !PACKAGE_STEP("sprites",write_sprt(&pkg,p,err,errcap)) ||
     !PACKAGE_STEP("backgrounds",write_bgnd(&pkg,p)) ||
     !PACKAGE_STEP("paths",write_path(&pkg,p)) ||
     !PACKAGE_STEP("scripts",write_scpt(&pkg,p)) ||
     !PACKAGE_STEP("globals",empty_list_chunk(&pkg,"GLOB")) ||
     !PACKAGE_STEP("shaders",write_shdr(&pkg,p)) ||
     !PACKAGE_STEP("fonts",write_font(&pkg,p)) ||
     !PACKAGE_STEP("timelines",write_tmln(&pkg,p)) ||
     !PACKAGE_STEP("objects",write_objt(&pkg,p)) ||
     !PACKAGE_STEP("rooms",write_room_chunk(&pkg,p)) ||
     !PACKAGE_STEP("classic triggers",write_trig(&pkg,p)) ||
     !PACKAGE_STEP("data files",fixed_zero_chunk(&pkg,"DAFL",0)) ||
     !PACKAGE_STEP("embedded images",write_embi(&pkg)) ||
     !PACKAGE_STEP("texture pages",write_tpag(&pkg,p)) ||
     !PACKAGE_STEP("code",write_code(&pkg,p,err,errcap)) ||
     !PACKAGE_STEP("variables",write_vari(&pkg)) ||
     !PACKAGE_STEP("functions",write_func(&pkg)) ||
     !PACKAGE_STEP("strings",write_strg(&pkg)) ||
     !PACKAGE_STEP("textures",write_txtr(&pkg,p,err,errcap)) ||
     !PACKAGE_STEP("audio",write_audo(&pkg,p,err,errcap))){
    if(!err[0]) snprintf(err,errcap,"package %s stage failed",stage);
#undef PACKAGE_STEP
    free_pkg(&pkg);
    return 0;
  }
#undef PACKAGE_STEP
  if(pkg.fail_on_placeholder && pkg.placeholder_code>0){
    snprintf(err,errcap,"refusing package with %d code placeholder%s",
      pkg.placeholder_code,pkg.placeholder_code==1?"":"s");
    free_pkg(&pkg);
    return 0;
  }
  patch32(&pkg.b,form_size_pos,(uint32_t)(pkg.b.len-8));
  int ok=materialize_included_files(p,out_path,err,errcap) &&
         write_file(p,out_path,pkg.b.data,pkg.b.len,err,errcap);
  free_pkg(&pkg);
  return ok;
}
