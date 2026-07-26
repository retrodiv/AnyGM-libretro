/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_bytecode.h"
#include "gmlc_bytecode_internal.h"
#include "gml_win.h"
#include "anygm_host.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <inttypes.h>

static const GmlcFunctionDef *find_script_wrapper(const GmlcFunctionRegistry *funcs, const char *path, int script_index){
  if(!funcs || !path || script_index<0) return NULL;
  for(int i=0;i<funcs->n_defs;i++){
    const GmlcFunctionDef *d=&funcs->defs[i];
    if(d->is_script_wrapper && d->source_path && !strcmp(d->source_path,path))
      return d;
  }
  return NULL;
}

int gmlc_bytecode_emit_empty(GmlcCodeBlob *out){
  memset(out,0,sizeof(*out));
  out->data=(uint8_t*)malloc(4);
  if(!out->data) return 0;
  out->data[0]=0; out->data[1]=0; out->data[2]=0; out->data[3]=0x9D;
  out->size=4;
  out->is_placeholder=1;
  return 1;
}

int gmlc_bytecode_compile_source_ex(const GmlcProject *project, const GmlcFunctionRegistry *funcs, int script_index, const char *path, GmlcCodeBlob *out, char *err, size_t errcap){
  memset(out,0,sizeof(*out));
  char *txt=compiler_read_source(project,path);
  if(!txt){
    snprintf(err,errcap,"%s: read failed",path);
    return 0;
  }
  const GmlcFunctionDef *wrapper=find_script_wrapper(funcs,path,script_index);
  int ok=wrapper ?
    compile_text_internal(project,funcs,script_index,path,wrapper->body,wrapper->params,out,err,errcap) :
    compile_text_internal(project,funcs,script_index,path,txt,NULL,out,err,errcap);
  free(txt);
  return ok;
}

int gmlc_bytecode_compile_source(const GmlcProject *project, const char *path, GmlcCodeBlob *out, char *err, size_t errcap){
  return gmlc_bytecode_compile_source_ex(project,NULL,-1,path,out,err,errcap);
}

int gmlc_bytecode_compile_function_body(const GmlcProject *project, const GmlcFunctionRegistry *funcs, const GmlcFunctionDef *def, GmlcCodeBlob *out, char *err, size_t errcap){
  memset(out,0,sizeof(*out));
  if(!def || !def->body){
    snprintf(err,errcap,"missing function body");
    return 0;
  }
  return compile_text_internal(project,funcs,-1,def->source_path,def->body,def->params,out,err,errcap);
}

void gmlc_bytecode_free(GmlcCodeBlob *b){
  if(!b) return;
  free(b->data);
  for(int i=0;i<b->n_refs;i++) free(b->refs[i].name);
  free(b->refs);
  for(int i=0;i<b->n_strings;i++) free(b->strings[i].value);
  free(b->strings);
  free(b->diagnostic);
  memset(b,0,sizeof(*b));
}
