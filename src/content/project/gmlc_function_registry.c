/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_bytecode_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int source_room_code_count(const GmlcProject *p){
  int n=0;
  for(int i=0;i<p->n_rooms;i++)
    if(p->rooms[i].creation_code_path && *p->rooms[i].creation_code_path) n++;
  return n;
}

static int registry_extra_so_far(const GmlcFunctionRegistry *r){
  int n=0;
  for(int i=0;i<r->n_defs;i++) if(!r->defs[i].is_script_wrapper) n++;
  return n;
}

int gmlc_function_registry_extra_count(const GmlcFunctionRegistry *r){
  return r ? registry_extra_so_far(r) : 0;
}

static int registry_add_global(GmlcFunctionRegistry *r, const char *name){
  for(int i=0;i<r->n_globals;i++) if(!strcmp(r->globals[i],name)) return 1;
  if(r->n_globals>=r->cap_globals){
    int nc=r->cap_globals?r->cap_globals*2:16;
    char **ng=(char**)realloc(r->globals,(size_t)nc*sizeof(*ng));
    if(!ng) return 0;
    r->globals=ng; r->cap_globals=nc;
  }
  r->globals[r->n_globals++]=gmlc_strdup(name);
  return r->globals[r->n_globals-1]!=NULL;
}

static int registry_add_macro(GmlcFunctionRegistry *r, const char *name, double value){
  for(int i=0;i<r->n_macros;i++) if(!strcmp(r->macro_names[i],name)){
    r->macro_values[i]=value;
    return 1;
  }
  if(r->n_macros>=r->cap_macros){
    int nc=r->cap_macros?r->cap_macros*2:16;
    char **names=(char**)realloc(r->macro_names,(size_t)nc*sizeof(*names));
    if(!names) return 0;
    r->macro_names=names;
    double *values=(double*)realloc(r->macro_values,(size_t)nc*sizeof(*values));
    if(!values) return 0;
    r->macro_values=values; r->cap_macros=nc;
  }
  r->macro_names[r->n_macros]=gmlc_strdup(name);
  if(!r->macro_names[r->n_macros]) return 0;
  r->macro_values[r->n_macros]=value;
  r->n_macros++;
  return 1;
}

static int registry_add_constant(GmlcFunctionRegistry *r, const char *name, const char *expression){
  if(!name || !*name || !expression) return 1;
  if(r->n_constants>=r->cap_constants){
    int nc=r->cap_constants?r->cap_constants*2:16;
    char **names=(char**)realloc(r->constant_names,(size_t)nc*sizeof(*names));
    if(!names) return 0;
    r->constant_names=names;
    char **exprs=(char**)realloc(r->constant_exprs,(size_t)nc*sizeof(*exprs));
    if(!exprs) return 0;
    r->constant_exprs=exprs; r->cap_constants=nc;
  }
  r->constant_names[r->n_constants]=gmlc_strdup(name);
  r->constant_exprs[r->n_constants]=gmlc_strdup(expression);
  if(!r->constant_names[r->n_constants] || !r->constant_exprs[r->n_constants]){
    free(r->constant_names[r->n_constants]); free(r->constant_exprs[r->n_constants]);
    return 0;
  }
  r->n_constants++;
  return 1;
}

static int registry_collect_macros_from_text(GmlcFunctionRegistry *r, const char *text){
  const char *p=text;
  while((p=strstr(p,"#macro"))){
    p+=6;
    while(*p==' ' || *p=='\t') p++;
    const char *start=p;
    while(isalnum((unsigned char)*p) || *p=='_') p++;
    if(p==start) continue;
    char *name=dup_range(start,(size_t)(p-start));
    while(*p==' ' || *p=='\t') p++;
    char *end=NULL;
    double value=strtod(p,&end);
    int ok=end==p || !name ? 1 : registry_add_macro(r,name,value);
    free(name);
    if(!ok) return 0;
  }
  return 1;
}

static int registry_add_asset(GmlcFunctionRegistry *r, const char *name, double value,
                              int script_code_index){
  if(!name || !*name) return 1;
  uint64_t hash=1469598103934665603ull;
  for(const char *p=name;*p;p++){
    hash^=(uint8_t)*p;
    hash*=1099511628211ull;
  }
  if(!r->asset_hash_cap || (int64_t)(r->n_assets+1)*10>=(int64_t)r->asset_hash_cap*7){
    if(r->asset_hash_cap>INT_MAX/2) return 0;
    int capacity=r->asset_hash_cap?r->asset_hash_cap*2:256;
    int *slots=(int*)malloc((size_t)capacity*sizeof(*slots));
    if(!slots) return 0;
    for(int i=0;i<capacity;i++) slots[i]=-1;
    for(int i=0;i<r->n_assets;i++){
      uint64_t existing_hash=1469598103934665603ull;
      for(const char *p=r->assets[i].name;*p;p++){
        existing_hash^=(uint8_t)*p;
        existing_hash*=1099511628211ull;
      }
      size_t at=(size_t)existing_hash&((size_t)capacity-1u);
      while(slots[at]>=0) at=(at+1u)&((size_t)capacity-1u);
      slots[at]=i;
    }
    free(r->asset_hash_slots);
    r->asset_hash_slots=slots;
    r->asset_hash_cap=capacity;
  }
  size_t slot=(size_t)hash&((size_t)r->asset_hash_cap-1u);
  while(r->asset_hash_slots[slot]>=0){
    int index=r->asset_hash_slots[slot];
    if(!strcmp(r->assets[index].name,name)){
      if(script_code_index>=0) r->assets[index].script_code_index=script_code_index;
      return 1;
    }
    slot=(slot+1u)&((size_t)r->asset_hash_cap-1u);
  }
  if(r->n_assets>=r->cap_assets){
    int nc=r->cap_assets?r->cap_assets*2:128;
    GmlcAssetBinding *assets=(GmlcAssetBinding*)realloc(r->assets,(size_t)nc*sizeof(*assets));
    if(!assets) return 0;
    r->assets=assets; r->cap_assets=nc;
  }
  int index=r->n_assets++;
  GmlcAssetBinding *asset=&r->assets[index];
  asset->name=gmlc_strdup(name);
  asset->value=value;
  asset->script_code_index=script_code_index;
  if(!asset->name){ r->n_assets--; return 0; }
  r->asset_hash_slots[slot]=index;
  return 1;
}

static int asset_binding_cmp(const void *a, const void *b){
  const GmlcAssetBinding *aa=(const GmlcAssetBinding*)a;
  const GmlcAssetBinding *bb=(const GmlcAssetBinding*)b;
  return strcmp(aa->name,bb->name);
}

static int registry_collect_assets(GmlcFunctionRegistry *r, const GmlcProject *p){
  int room_codes=source_room_code_count(p);
  for(int i=0;i<p->n_sprites;i++){
    int runtime_id=gmlc_project_sprite_runtime_id(p,i);
    if(runtime_id>=0 && !registry_add_asset(r,p->sprites[i].name,runtime_id,-1)) return 0;
  }
  for(int i=0;i<p->n_sounds;i++)
    if(!registry_add_asset(r,p->sounds[i].name,i,-1)) return 0;
  for(int i=0;i<p->n_objects;i++)
    if(!registry_add_asset(r,p->objects[i].name,i,-1)) return 0;
  for(int i=0;i<p->n_rooms;i++)
    if(!registry_add_asset(r,p->rooms[i].name,i,-1)) return 0;
  for(int i=0;i<p->n_shaders;i++)
    if(!registry_add_asset(r,p->shaders[i].name,i,-1)) return 0;
  for(int i=0;i<p->n_fonts;i++)
    if(!registry_add_asset(r,p->fonts[i].name,i,-1)) return 0;
  for(int i=0;i<p->n_tilesets;i++)
    if(!registry_add_asset(r,p->tilesets[i].name,i,-1)) return 0;
  for(int i=0;i<p->n_scripts;i++)
    if(!registry_add_asset(r,p->scripts[i].name,i,room_codes+i)) return 0;
  for(int i=0;i<p->n_paths;i++)
    if(!registry_add_asset(r,p->paths[i].name,i,-1)) return 0;
  for(int i=0;i<p->n_timelines;i++)
    if(!registry_add_asset(r,p->timelines[i].name,i,-1)) return 0;
  if(r->n_assets>1)
    qsort(r->assets,(size_t)r->n_assets,sizeof(*r->assets),asset_binding_cmp);
  return 1;
}

static int collect_globalvars_from_text(GmlcFunctionRegistry *r, const char *src){
  Lexer lex; memset(&lex,0,sizeof(lex)); lex.src=src; lx_next(&lex);
  while(lex.tok.kind!=TOK_EOF){
    size_t before=lex.tok.start;
    while(before>0 && isspace((unsigned char)src[before-1])) before--;
    if(lex.tok.kind==TOK_ID && !strcmp(lex.tok.text,"globalvar") &&
       !(before>0 && src[before-1]=='.')){
      lx_next(&lex);
      while(lex.tok.kind==TOK_ID){
        if(!registry_add_global(r,lex.tok.text)) return 0;
        lx_next(&lex);
        if(strcmp(lex.tok.text,",")) break;
        lx_next(&lex);
      }
      continue;
    }
    lx_next(&lex);
  }
  return 1;
}

static int registry_add_function(GmlcFunctionRegistry *r, const char *path, const char *name, Span params, Span body, const char *src, int code_index, int is_wrapper){
  for(int i=0;i<r->n_defs;i++){
    if(r->defs[i].source_path && path && !strcmp(r->defs[i].source_path,path) && r->defs[i].start==body.start)
      return 1;
  }
  if(r->n_defs>=r->cap_defs){
    int nc=r->cap_defs?r->cap_defs*2:16;
    GmlcFunctionDef *nd=(GmlcFunctionDef*)realloc(r->defs,(size_t)nc*sizeof(*nd));
    if(!nd) return 0;
    r->defs=nd; r->cap_defs=nc;
  }
  GmlcFunctionDef *d=&r->defs[r->n_defs++];
  memset(d,0,sizeof(*d));
  d->name=(name && *name)?gmlc_strdup(name):NULL;
  d->params=dup_range(src+params.start,params.end-params.start);
  d->body=dup_range(src+body.start,body.end-body.start);
  d->source_path=path?gmlc_strdup(path):NULL;
  d->start=body.start;
  d->end=body.end;
  d->code_index=code_index;
  d->is_script_wrapper=is_wrapper;
  if((name && *name && !d->name) || !d->params || !d->body || (path && !d->source_path)) return 0;
  return 1;
}

static int scan_function_at(const char *src, size_t pos, char *name, size_t name_cap, Span *params, Span *body, size_t *out_end){
  if(!function_shape_at(src,pos)) return 0;
  pos=skip_ws_comments_at(src,pos+8);
  name[0]=0;
  if(isalpha((unsigned char)src[pos]) || src[pos]=='_'){
    size_t ns=pos;
    pos++;
    while(isalnum((unsigned char)src[pos]) || src[pos]=='_') pos++;
    size_t n=pos-ns;
    if(n>=name_cap) n=name_cap-1;
    memcpy(name,src+ns,n);
    name[n]=0;
    pos=skip_ws_comments_at(src,pos);
  }
  if(src[pos]!='(') return 0;
  size_t paren_close=0;
  if(!scan_matching_delim(src,pos,'(',')',&paren_close)) return 0;
  params->start=pos+1;
  params->end=paren_close;
  trim_span(src,params);
  pos=skip_ws_comments_at(src,paren_close+1);
  if(src[pos]!='{') return 0;
  size_t brace_close=0;
  if(!scan_matching_delim(src,pos,'{','}',&brace_close)) return 0;
  body->start=pos+1;
  body->end=brace_close;
  *out_end=brace_close+1;
  return 1;
}

typedef struct {
  char **items;
  int n, cap;
} NameList;

static void name_list_free(NameList *l){
  if(!l) return;
  for(int i=0;i<l->n;i++) free(l->items[i]);
  free(l->items);
  memset(l,0,sizeof(*l));
}

static int name_list_has(const NameList *l, const char *name){
  if(!l || !name) return 0;
  for(int i=0;i<l->n;i++) if(!strcmp(l->items[i],name)) return 1;
  return 0;
}

static int name_list_add(NameList *l, const char *name){
  if(!name || !*name || name_list_has(l,name)) return 1;
  if(l->n>=l->cap){
    int nc=l->cap?l->cap*2:16;
    char **ni=(char**)realloc(l->items,(size_t)nc*sizeof(*ni));
    if(!ni) return 0;
    l->items=ni;
    l->cap=nc;
  }
  l->items[l->n]=gmlc_strdup(name);
  if(!l->items[l->n]) return 0;
  l->n++;
  return 1;
}

static int prev_nonspace_is_dot(const char *src, size_t pos){
  while(pos>0){
    pos--;
    if(isspace((unsigned char)src[pos])) continue;
    return src[pos]=='.';
  }
  return 0;
}

static size_t skip_string_or_comment(const char *src, size_t pos, size_t end){
  if(pos>=end) return pos;
  if(src[pos]=='"' || src[pos]=='\''){
    char quote=src[pos];
    pos++;
    while(pos<end && src[pos]){
      if(src[pos]=='\\' && pos+1<end){ pos+=2; continue; }
      if(src[pos]==quote){ pos++; break; }
      pos++;
    }
    return pos;
  }
  if(pos+1<end && src[pos]=='/' && src[pos+1]=='/'){
    pos+=2;
    while(pos<end && src[pos] && src[pos]!='\n') pos++;
    return pos;
  }
  if(pos+1<end && src[pos]=='/' && src[pos+1]=='*'){
    pos+=2;
    while(pos+1<end && src[pos] && !(src[pos]=='*' && src[pos+1]=='/')) pos++;
    if(pos+1<end) pos+=2;
    return pos;
  }
  return pos;
}

static int collect_param_names(const char *src, Span params, NameList *out){
  size_t pos=params.start;
  while(pos<params.end){
    pos=skip_ws_comments_at(src,pos);
    if(pos>=params.end) break;
    if(isalpha((unsigned char)src[pos]) || src[pos]=='_'){
      size_t ns=pos++;
      while(pos<params.end && (isalnum((unsigned char)src[pos]) || src[pos]=='_')) pos++;
      char name[128];
      size_t n=pos-ns;
      if(n>=sizeof(name)) n=sizeof(name)-1;
      memcpy(name,src+ns,n);
      name[n]=0;
      if(!name_list_add(out,name)) return 0;
    }
    while(pos<params.end && src[pos]!=',') pos++;
    if(pos<params.end && src[pos]==',') pos++;
  }
  return 1;
}

static int collect_var_decls(const char *src, Span range, int skip_functions, NameList *out){
  size_t pos=range.start;
  while(pos<range.end && src[pos]){
    size_t sp=skip_string_or_comment(src,pos,range.end);
    if(sp!=pos){ pos=sp; continue; }
    if(skip_functions && word_match_at(src,pos,"function") && function_shape_at(src,pos)){
      char name[128];
      Span params={0,0}, body={0,0};
      size_t end=0;
      if(scan_function_at(src,pos,name,sizeof(name),&params,&body,&end)){
        pos=end;
        continue;
      }
    }
    if(word_match_at(src,pos,"var") && !prev_nonspace_is_dot(src,pos)){
      pos=skip_ws_comments_at(src,pos+3);
      for(;;){
        pos=skip_ws_comments_at(src,pos);
        if(pos>=range.end || !src[pos] || src[pos]==';') break;
        if(isalpha((unsigned char)src[pos]) || src[pos]=='_'){
          size_t ns=pos++;
          while(pos<range.end && (isalnum((unsigned char)src[pos]) || src[pos]=='_')) pos++;
          char name[128];
          size_t n=pos-ns;
          if(n>=sizeof(name)) n=sizeof(name)-1;
          memcpy(name,src+ns,n);
          name[n]=0;
          if(!name_list_add(out,name)) return 0;
        }
        int depth=0;
        while(pos<range.end && src[pos]){
          size_t np=skip_string_or_comment(src,pos,range.end);
          if(np!=pos){ pos=np; continue; }
          char ch=src[pos];
          if(ch=='(' || ch=='[' || ch=='{'){ depth++; pos++; continue; }
          if((ch==')' || ch==']' || ch=='}') && depth>0){ depth--; pos++; continue; }
          if(depth==0 && (ch==',' || ch==';')) break;
          pos++;
        }
        if(pos<range.end && src[pos]==','){ pos++; continue; }
        if(pos<range.end && src[pos]==';') pos++;
        break;
      }
      continue;
    }
    pos++;
  }
  return 1;
}

static int function_body_uses_outer_local(const char *src, Span body, const NameList *outer, const NameList *own, char *capture, size_t cap){
  size_t pos=body.start;
  while(pos<body.end && src[pos]){
    size_t sp=skip_string_or_comment(src,pos,body.end);
    if(sp!=pos){ pos=sp; continue; }
    if(word_match_at(src,pos,"function") && function_shape_at(src,pos)){
      char name[128];
      Span params={0,0}, nested_body={0,0};
      size_t end=0;
      if(scan_function_at(src,pos,name,sizeof(name),&params,&nested_body,&end)){
        pos=end;
        continue;
      }
    }
    if(isalpha((unsigned char)src[pos]) || src[pos]=='_'){
      size_t ns=pos++;
      while(pos<body.end && (isalnum((unsigned char)src[pos]) || src[pos]=='_')) pos++;
      char name[128];
      size_t n=pos-ns;
      if(n>=sizeof(name)) n=sizeof(name)-1;
      memcpy(name,src+ns,n);
      name[n]=0;
      if(!prev_nonspace_is_dot(src,ns) && name_list_has(outer,name) && !name_list_has(own,name)){
        snprintf(capture,cap,"%s",name);
        return 1;
      }
      continue;
    }
    pos++;
  }
  return 0;
}

static int detect_unsupported_function_capture(const GmlcFunctionRegistry *registry,
    const char *path, const char *src, size_t len, Span params, Span body, char *capture, size_t cap){
  NameList outer={0}, own={0};
  Span full={0,len};
  int rc=-1;
  if(!collect_var_decls(src,full,1,&outer)) goto done;
  /* Definitions are indexed outer-first. A nested function must also reject
   * captures from its enclosing functions, not just from the source's root. */
  for(int i=0;i<registry->n_defs;i++){
    const GmlcFunctionDef *parent=&registry->defs[i];
    if(!path || !parent->source_path || strcmp(path,parent->source_path) ||
       parent->start>params.start || parent->end<body.end) continue;
    Span parent_params={0,strlen(parent->params)}, parent_body={0,strlen(parent->body)};
    if(!collect_param_names(parent->params,parent_params,&outer) ||
       !collect_var_decls(parent->body,parent_body,1,&outer)) goto done;
  }
  if(!outer.n){ rc=0; goto done; }
  if(!collect_param_names(src,params,&own)) goto done;
  if(!collect_var_decls(src,body,1,&own)) goto done;
  rc=function_body_uses_outer_local(src,body,&outer,&own,capture,cap);
done:
  name_list_free(&outer);
  name_list_free(&own);
  return rc;
}

static int collect_functions_from_text(GmlcFunctionRegistry *r, const char *path, const char *script_name, int script_code_index, int appended_base, const char *src, char *err, size_t errcap){
  if(!collect_globalvars_from_text(r,src)){
    if(err && errcap) snprintf(err,errcap,"out of memory collecting global variables");
    return 0;
  }
  size_t len=strlen(src);
  size_t pos=0;
  while(src[pos]){
    if(src[pos]=='"' || src[pos]=='\''){
      char quote=src[pos];
      pos++;
      while(src[pos]){
        if(src[pos]=='\\' && src[pos+1]){ pos+=2; continue; }
        if(src[pos]==quote){ pos++; break; }
        pos++;
      }
      continue;
    }
    if(src[pos]=='/' && src[pos+1]=='/'){ pos+=2; while(src[pos] && src[pos]!='\n') pos++; continue; }
    if(src[pos]=='/' && src[pos+1]=='*'){ pos+=2; while(src[pos] && !(src[pos]=='*' && src[pos+1]=='/')) pos++; if(src[pos]) pos+=2; continue; }
    if(word_match_at(src,pos,"function") && function_shape_at(src,pos)){
      char name[128];
      Span params={0,0}, body={0,0};
      size_t end=0;
      if(scan_function_at(src,pos,name,sizeof(name),&params,&body,&end)){
        Span before={0,pos}, after={end,len};
        int is_wrapper=script_name && *script_name && name[0] && !strcmp(name,script_name) && span_empty(src,before) && span_empty(src,after);
        char capture[128]={0};
        int cap=detect_unsupported_function_capture(r,path,src,len,params,body,capture,sizeof(capture));
        if(cap<0){
          snprintf(err,errcap,"function capture analysis allocation failed");
          return 0;
        }
        if(cap>0){
          snprintf(err,errcap,"%s: unsupported function closure capture of local '%s'",path?path:"<source>",capture);
          return 0;
        }
        int code_index=is_wrapper ? script_code_index : appended_base + registry_extra_so_far(r);
        if(!registry_add_function(r,path,name,params,body,src,code_index,is_wrapper)) return 0;
        /* Scan the body too: imported action predicates can contain another
         * function literal, which needs its own code entry before compilation. */
        pos=body.start;
        continue;
      }
    }
    pos++;
  }
  return 1;
}

int gmlc_bytecode_collect_functions(const GmlcProject *project, int appended_base, GmlcFunctionRegistry *out, char *err, size_t errcap){
  memset(out,0,sizeof(*out));
  for(int i=0;i<project->n_constants;i++)
    if(!registry_add_constant(out,project->constants[i].name,project->constants[i].expression)) goto fail;
  if(!registry_collect_assets(out,project)) goto fail;
  for(int i=0;i<project->n_rooms;i++){
    const char *path=project->rooms[i].creation_code_path;
    if(path && *path){
      char *txt=compiler_read_source(project,path);
      if(txt){ int ok=collect_functions_from_text(out,path,NULL,-1,appended_base,txt,err,errcap); free(txt); if(!ok) goto fail; }
    }
  }
  for(int i=0;i<project->n_scripts;i++){
    const char *path=project->scripts[i].source_path;
    if(path && *path){
      char *txt=compiler_read_source(project,path);
      if(txt){
        int ci=source_room_code_count(project)+i;
        int ok=registry_collect_macros_from_text(out,txt) &&
          collect_functions_from_text(out,path,project->scripts[i].name,ci,appended_base,txt,err,errcap);
        free(txt);
        if(!ok) goto fail;
      }
    }
  }
  for(int i=0;i<project->n_timelines;i++){
    const GmlcTimeline *timeline=&project->timelines[i];
    for(int m=0;m<timeline->n_moments;m++){
      const char *path=timeline->moments[m].source_path;
      if(path && *path){
        char *txt=compiler_read_source(project,path);
        if(txt){ int ok=collect_functions_from_text(out,path,NULL,-1,appended_base,txt,err,errcap); free(txt); if(!ok) goto fail; }
      }
    }
  }
  for(int oi=0;oi<project->n_objects;oi++){
    const GmlcObject *obj=&project->objects[oi];
    for(int ei=0;ei<obj->n_events;ei++){
      const char *path=obj->events[ei].source_path;
      if(path && *path){
        char *txt=compiler_read_source(project,path);
        if(txt){ int ok=collect_functions_from_text(out,path,NULL,-1,appended_base,txt,err,errcap); free(txt); if(!ok) goto fail; }
      }
    }
  }
  for(int ri=0;ri<project->n_rooms;ri++){
    const GmlcRoom *room=&project->rooms[ri];
    for(int ii=0;ii<room->n_instances;ii++){
      const char *path=room->instances[ii].creation_code_path;
      if(path && *path){
        char *txt=compiler_read_source(project,path);
        if(txt){ int ok=collect_functions_from_text(out,path,NULL,-1,appended_base,txt,err,errcap); free(txt); if(!ok) goto fail; }
      }
    }
  }
  return 1;
fail:
  if(err && errcap && !err[0]) snprintf(err,errcap,"function registry allocation failed");
  gmlc_function_registry_free(out);
  return 0;
}

void gmlc_function_registry_free(GmlcFunctionRegistry *r){
  if(!r) return;
  for(int i=0;i<r->n_defs;i++){
    free(r->defs[i].name);
    free(r->defs[i].params);
    free(r->defs[i].body);
    free(r->defs[i].source_path);
  }
  free(r->defs);
  for(int i=0;i<r->n_globals;i++) free(r->globals[i]);
  free(r->globals);
  for(int i=0;i<r->n_assets;i++) free(r->assets[i].name);
  free(r->assets);
  free(r->asset_hash_slots);
  for(int i=0;i<r->n_macros;i++) free(r->macro_names[i]);
  free(r->macro_names);
  free(r->macro_values);
  for(int i=0;i<r->n_constants;i++){
    free(r->constant_names[i]); free(r->constant_exprs[i]);
  }
  free(r->constant_names); free(r->constant_exprs);
  memset(r,0,sizeof(*r));
}
