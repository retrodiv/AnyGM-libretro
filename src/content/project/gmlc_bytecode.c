/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_bytecode.h"
#include "gml_win.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint8_t *data;
  size_t len, cap;
} CodeBuf;

typedef enum {
  TOK_EOF, TOK_ID, TOK_NUM, TOK_STR, TOK_SYM
} TokKind;

typedef struct {
  TokKind kind;
  char text[128];
  double num;
  size_t start, end;
} Tok;

typedef struct {
  const char *src;
  size_t pos;
  Tok tok;
  char err[256];
} Lexer;

typedef struct {
  char *name;
  double value;
} Macro;

typedef struct {
  const GmlcProject *project;
  const GmlcFunctionRegistry *funcs;
  const char *source_path;
  int script_index;
  Macro *macros;
  int n_macros, cap_macros;
  CodeBuf code;
  GmlcRefSite *refs;
  int n_refs, cap_refs;
  GmlcStringSite *strings;
  int n_strings, cap_strings;
  char **locals;
  int n_locals, cap_locals;
  size_t *break_sites;
  int n_break_sites, cap_break_sites;
  size_t *continue_sites;
  int n_continue_sites, cap_continue_sites;
  int continue_depth;
  int temp_id;
  Lexer lex;
  int unsupported;
} Compiler;

static char *dup_range(const char *s, size_t n){
  char *out=(char*)malloc(n+1);
  if(out){ memcpy(out,s,n); out[n]=0; }
  return out;
}

static char *read_text(const char *path){
  FILE *f=fopen(path,"rb");
  if(!f) return NULL;
  fseek(f,0,SEEK_END);
  long sz=ftell(f);
  rewind(f);
  if(sz<0){ fclose(f); return NULL; }
  char *buf=(char*)malloc((size_t)sz+1);
  if(!buf){ fclose(f); return NULL; }
  if(fread(buf,1,(size_t)sz,f)!=(size_t)sz){ fclose(f); free(buf); return NULL; }
  fclose(f);
  buf[sz]=0;
  return buf;
}

static int reserve(CodeBuf *b, size_t n){
  if(b->len+n<=b->cap) return 1;
  size_t nc=b->cap?b->cap*2:256;
  while(b->len+n>nc) nc*=2;
  uint8_t *nd=(uint8_t*)realloc(b->data,nc);
  if(!nd) return 0;
  b->data=nd; b->cap=nc;
  return 1;
}

static int emit_u32(CodeBuf *b, uint32_t v){
  if(!reserve(b,4)) return 0;
  b->data[b->len++]=(uint8_t)v;
  b->data[b->len++]=(uint8_t)(v>>8);
  b->data[b->len++]=(uint8_t)(v>>16);
  b->data[b->len++]=(uint8_t)(v>>24);
  return 1;
}

static int emit_i32(CodeBuf *b, int32_t v){ return emit_u32(b,(uint32_t)v); }

static int emit_double(CodeBuf *b, double d){
  if(!reserve(b,8)) return 0;
  memcpy(b->data+b->len,&d,8);
  b->len+=8;
  return 1;
}

static void patch_u32(CodeBuf *b, size_t pos, uint32_t v){
  b->data[pos]=(uint8_t)v;
  b->data[pos+1]=(uint8_t)(v>>8);
  b->data[pos+2]=(uint8_t)(v>>16);
  b->data[pos+3]=(uint8_t)(v>>24);
}

static uint32_t fw(uint8_t op, uint8_t type_byte, int16_t low){
  return ((uint32_t)op<<24) | ((uint32_t)type_byte<<16) | (uint16_t)low;
}

static int add_ref(Compiler *c, const char *name, GmlcRefKind kind, uint32_t instr, uint32_t ref, uint32_t high){
  if(c->n_refs>=c->cap_refs){
    int nc=c->cap_refs?c->cap_refs*2:64;
    GmlcRefSite *nr=(GmlcRefSite*)realloc(c->refs,(size_t)nc*sizeof(*nr));
    if(!nr) return 0;
    c->refs=nr; c->cap_refs=nc;
  }
  GmlcRefSite *r=&c->refs[c->n_refs++];
  memset(r,0,sizeof(*r));
  r->name=gmlc_strdup(name);
  r->kind=kind;
  r->instr_off=instr;
  r->ref_off=ref;
  r->high_bits=high;
  return r->name!=NULL;
}

static int add_string_site(Compiler *c, const char *value, uint32_t payload_off){
  if(c->n_strings>=c->cap_strings){
    int nc=c->cap_strings?c->cap_strings*2:32;
    GmlcStringSite *ns=(GmlcStringSite*)realloc(c->strings,(size_t)nc*sizeof(*ns));
    if(!ns) return 0;
    c->strings=ns; c->cap_strings=nc;
  }
  GmlcStringSite *s=&c->strings[c->n_strings++];
  s->value=gmlc_strdup(value);
  s->payload_off=payload_off;
  return s->value!=NULL;
}

static int emit_push_real(Compiler *c, double d){
  if(floor(d)==d && d>=-32768.0 && d<=32767.0)
    return emit_u32(&c->code,fw(0x84,DT_INT16,(int16_t)d));
  if(floor(d)==d && d>=-2147483648.0 && d<=2147483647.0){
    if(!emit_u32(&c->code,fw(OP_PUSH,DT_INT32,0))) return 0;
    return emit_i32(&c->code,(int32_t)d);
  }
  if(!emit_u32(&c->code,fw(OP_PUSH,DT_DOUBLE,0))) return 0;
  return emit_double(&c->code,d);
}

static int emit_push_string_literal(Compiler *c, const char *s){
  if(!emit_u32(&c->code,fw(OP_PUSH,DT_STRING,0))) return 0;
  uint32_t payload=(uint32_t)c->code.len;
  if(!emit_u32(&c->code,0)) return 0;
  return add_string_site(c,s,payload);
}

static int emit_push_var(Compiler *c, int inst, const char *name, uint8_t reftype){
  uint32_t instr=(uint32_t)c->code.len;
  if(!emit_u32(&c->code,fw(OP_PUSH,DT_VAR,(int16_t)inst))) return 0;
  uint32_t ref=(uint32_t)c->code.len;
  if(!emit_u32(&c->code,(uint32_t)reftype<<24)) return 0;
  return add_ref(c,name,GMLC_REF_VARI,instr,ref,(uint32_t)reftype<<24);
}

static int emit_pop_var(Compiler *c, int inst, const char *name, uint8_t reftype, uint8_t type1){
  uint8_t tb=(uint8_t)((DT_VAR<<4) | (type1&0xF));
  uint32_t instr=(uint32_t)c->code.len;
  if(!emit_u32(&c->code,fw(OP_POP,tb,(int16_t)inst))) return 0;
  uint32_t ref=(uint32_t)c->code.len;
  if(!emit_u32(&c->code,(uint32_t)reftype<<24)) return 0;
  return add_ref(c,name,GMLC_REF_VARI,instr,ref,(uint32_t)reftype<<24);
}

static int emit_call(Compiler *c, const char *name, int argc){
  uint32_t instr=(uint32_t)c->code.len;
  if(!emit_u32(&c->code,fw(OP_CALL,DT_INT32,(int16_t)argc))) return 0;
  uint32_t ref=(uint32_t)c->code.len;
  if(!emit_u32(&c->code,0)) return 0;
  return add_ref(c,name,GMLC_REF_FUNC,instr,ref,0);
}

static int emit_callv(Compiler *c, int argc){
  return emit_u32(&c->code,fw(OP_CALLV,DT_VAR,(int16_t)argc));
}

static size_t emit_branch(Compiler *c, uint8_t op){
  size_t pos=c->code.len;
  emit_u32(&c->code,fw(op,0,0));
  return pos;
}

static void patch_branch(Compiler *c, size_t pos, size_t target){
  int32_t delta=(int32_t)((target - pos) / 4);
  uint32_t old=(uint32_t)c->code.data[pos] | ((uint32_t)c->code.data[pos+1]<<8) | ((uint32_t)c->code.data[pos+2]<<16) | ((uint32_t)c->code.data[pos+3]<<24);
  uint32_t v=(old & 0xFF000000u) | ((uint32_t)delta & 0x7FFFFFu);
  patch_u32(&c->code,pos,v);
}

static int emit_binary(Compiler *c, uint8_t op){
  return emit_u32(&c->code,fw(op,(uint8_t)((DT_VAR<<4)|DT_VAR),0));
}

static int emit_cmp(Compiler *c, uint8_t cmp){
  return emit_u32(&c->code,((uint32_t)OP_CMP<<24) | ((uint32_t)((DT_VAR<<4)|DT_VAR)<<16) | ((uint32_t)cmp<<8));
}

static void lx_skip(Lexer *l){
  for(;;){
    while(isspace((unsigned char)l->src[l->pos])) l->pos++;
    if(l->src[l->pos]=='/' && l->src[l->pos+1]=='/'){ while(l->src[l->pos] && l->src[l->pos]!='\n') l->pos++; continue; }
    if(l->src[l->pos]=='/' && l->src[l->pos+1]=='*'){ l->pos+=2; while(l->src[l->pos] && !(l->src[l->pos]=='*'&&l->src[l->pos+1]=='/')) l->pos++; if(l->src[l->pos]) l->pos+=2; continue; }
    if(l->src[l->pos]=='#'){
      size_t bol=l->pos;
      while(bol>0 && l->src[bol-1]!='\n' && l->src[bol-1]!='\r') bol--;
      int line_directive=1;
      for(size_t p=bol;p<l->pos;p++) if(!isspace((unsigned char)l->src[p])){ line_directive=0; break; }
      if(line_directive){ while(l->src[l->pos] && l->src[l->pos]!='\n') l->pos++; continue; }
    }
    break;
  }
}

static void lx_next(Lexer *l){
  lx_skip(l);
  memset(&l->tok,0,sizeof(l->tok));
  l->tok.start=l->pos;
  const char *s=l->src+l->pos;
  if(!*s){ l->tok.kind=TOK_EOF; l->tok.end=l->pos; return; }
  if(isalpha((unsigned char)*s) || *s=='_'){
    size_t n=0;
    while(isalnum((unsigned char)s[n]) || s[n]=='_') n++;
    if(n>=sizeof(l->tok.text)) n=sizeof(l->tok.text)-1;
    memcpy(l->tok.text,s,n); l->tok.text[n]=0;
    l->tok.kind=TOK_ID; l->pos+=(size_t)n; l->tok.end=l->pos; return;
  }
  if(isdigit((unsigned char)*s) || (*s=='.' && isdigit((unsigned char)s[1]))){
    char *end=NULL;
    l->tok.num=strtod(s,&end);
    size_t n=(size_t)(end-s);
    if(n>=sizeof(l->tok.text)) n=sizeof(l->tok.text)-1;
    memcpy(l->tok.text,s,n); l->tok.text[n]=0;
    l->tok.kind=TOK_NUM; l->pos+=(size_t)(end-s); l->tok.end=l->pos; return;
  }
  if(*s=='"'){
    l->pos++;
    size_t n=0;
    while(l->src[l->pos] && l->src[l->pos]!='"'){
      char ch=l->src[l->pos++];
      if(ch=='\\' && l->src[l->pos]){
        char e=l->src[l->pos++];
        if(e=='n') ch='\n'; else if(e=='t') ch='\t'; else ch=e;
      }
      if(n+1<sizeof(l->tok.text)) l->tok.text[n++]=ch;
    }
    if(l->src[l->pos]=='"') l->pos++;
    l->tok.text[n]=0; l->tok.kind=TOK_STR; l->tok.end=l->pos; return;
  }
  static const char *ops[]={"==","!=","<=",">=","&&","||","+=","-=","*=","/=","++","--",NULL};
  for(int i=0;ops[i];i++){
    size_t n=strlen(ops[i]);
    if(!strncmp(s,ops[i],n)){ snprintf(l->tok.text,sizeof(l->tok.text),"%s",ops[i]); l->tok.kind=TOK_SYM; l->pos+=n; l->tok.end=l->pos; return; }
  }
  l->tok.kind=TOK_SYM; l->tok.text[0]=*s; l->tok.text[1]=0; l->pos++;
  l->tok.end=l->pos;
}

static int tok_is(Compiler *c, const char *s){ return !strcmp(c->lex.tok.text,s); }
static int eat(Compiler *c, const char *s){ if(tok_is(c,s)){ lx_next(&c->lex); return 1; } return 0; }
static int need(Compiler *c, const char *s){ if(eat(c,s)) return 1; snprintf(c->lex.err,sizeof(c->lex.err),"expected '%s'",s); c->unsupported=1; return 0; }
static int is_id(Compiler *c, const char *s){ return c->lex.tok.kind==TOK_ID && !strcmp(c->lex.tok.text,s); }

static int local_index(Compiler *c, const char *name){
  for(int i=0;i<c->n_locals;i++) if(!strcmp(c->locals[i],name)) return i;
  return -1;
}

static int add_local(Compiler *c, const char *name){
  if(local_index(c,name)>=0) return 1;
  if(c->n_locals>=c->cap_locals){
    int nc=c->cap_locals?c->cap_locals*2:16;
    char **nl=(char**)realloc(c->locals,(size_t)nc*sizeof(*nl));
    if(!nl) return 0;
    c->locals=nl; c->cap_locals=nc;
  }
  c->locals[c->n_locals++]=gmlc_strdup(name);
  return c->locals[c->n_locals-1]!=NULL;
}

static int add_break_site(Compiler *c, size_t pos){
  if(c->n_break_sites>=c->cap_break_sites){
    int nc=c->cap_break_sites?c->cap_break_sites*2:32;
    size_t *ns=(size_t*)realloc(c->break_sites,(size_t)nc*sizeof(*ns));
    if(!ns) return 0;
    c->break_sites=ns; c->cap_break_sites=nc;
  }
  c->break_sites[c->n_break_sites++]=pos;
  return 1;
}

static int emit_break_branch(Compiler *c){
  size_t b=emit_branch(c,OP_B);
  return add_break_site(c,b);
}

static void patch_breaks_from(Compiler *c, int mark, size_t target){
  for(int i=mark;i<c->n_break_sites;i++) patch_branch(c,c->break_sites[i],target);
  c->n_break_sites=mark;
}

static int add_continue_site(Compiler *c, size_t pos){
  if(c->n_continue_sites>=c->cap_continue_sites){
    int nc=c->cap_continue_sites?c->cap_continue_sites*2:32;
    size_t *ns=(size_t*)realloc(c->continue_sites,(size_t)nc*sizeof(*ns));
    if(!ns) return 0;
    c->continue_sites=ns; c->cap_continue_sites=nc;
  }
  c->continue_sites[c->n_continue_sites++]=pos;
  return 1;
}

static int emit_continue_branch(Compiler *c){
  size_t b=emit_branch(c,OP_B);
  return add_continue_site(c,b);
}

static void patch_continues_from(Compiler *c, int mark, size_t target){
  for(int i=mark;i<c->n_continue_sites;i++) patch_branch(c,c->continue_sites[i],target);
  c->n_continue_sites=mark;
}

static int macro_value(Compiler *c, const char *name, double *out){
  for(int i=0;i<c->n_macros;i++) if(!strcmp(c->macros[i].name,name)){ *out=c->macros[i].value; return 1; }
  return 0;
}

static int add_macro(Compiler *c, const char *name, double value){
  if(c->n_macros>=c->cap_macros){
    int nc=c->cap_macros?c->cap_macros*2:16;
    Macro *nm=(Macro*)realloc(c->macros,(size_t)nc*sizeof(*nm));
    if(!nm) return 0;
    c->macros=nm; c->cap_macros=nc;
  }
  c->macros[c->n_macros].name=gmlc_strdup(name);
  c->macros[c->n_macros].value=value;
  c->n_macros++;
  return 1;
}

static int resolve_asset(Compiler *c, const char *name, double *out){
  for(int i=0;i<c->project->n_sprites;i++) if(!strcmp(c->project->sprites[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_sounds;i++) if(!strcmp(c->project->sounds[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_objects;i++) if(!strcmp(c->project->objects[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_rooms;i++) if(!strcmp(c->project->rooms[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_shaders;i++) if(!strcmp(c->project->shaders[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_fonts;i++) if(!strcmp(c->project->fonts[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_tilesets;i++) if(!strcmp(c->project->tilesets[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_scripts;i++) if(!strcmp(c->project->scripts[i].name,name)){ *out=i; return 1; }
  return 0;
}

static int source_room_code_count(const GmlcProject *p){
  return p->n_rooms>0 ? p->n_rooms : 1;
}

static int resolve_script_code_index(Compiler *c, const char *name, int *out){
  for(int i=0;i<c->project->n_scripts;i++){
    if(!strcmp(c->project->scripts[i].name,name)){
      *out=source_room_code_count(c->project)+i;
      return 1;
    }
  }
  return 0;
}

static int resolve_function_code_index(Compiler *c, const char *name, int *out){
  if(resolve_script_code_index(c,name,out)) return 1;
  if(!c->funcs) return 0;
  for(int i=0;i<c->funcs->n_defs;i++){
    const GmlcFunctionDef *d=&c->funcs->defs[i];
    if(d->name && !strcmp(d->name,name)){
      *out=d->code_index;
      return 1;
    }
  }
  return 0;
}

static int find_function_literal_index(Compiler *c, const char *name, const char *params, const char *body, int *out){
  if(name && *name && resolve_function_code_index(c,name,out)) return 1;
  if(!c->funcs) return 0;
  for(int i=0;i<c->funcs->n_defs;i++){
    const GmlcFunctionDef *d=&c->funcs->defs[i];
    if(name && *name){
      if(d->name && !strcmp(d->name,name)){ *out=d->code_index; return 1; }
      continue;
    }
    if(!d->name && d->params && d->body &&
       !strcmp(d->params,params?params:"") &&
       !strcmp(d->body,body?body:"")){
      *out=d->code_index;
      return 1;
    }
  }
  return 0;
}

static int emit_script_funcval(Compiler *c, int code_index){
  return emit_push_real(c,(double)(0x40000000u | (uint32_t)(code_index & 0x00FFFFFF)));
}

static int resolve_const(Compiler *c, const char *name, double *out){
  if(!strcmp(name,"true")){ *out=1; return 1; }
  if(!strcmp(name,"false")){ *out=0; return 1; }
  if(!strcmp(name,"noone")){ *out=IT_NOONE; return 1; }
  if(!strcmp(name,"self")){ *out=IT_SELF; return 1; }
  if(!strcmp(name,"other")){ *out=IT_OTHER; return 1; }
  if(!strcmp(name,"all")){ *out=IT_ALL; return 1; }
  if(!strcmp(name,"c_black")){ *out=0; return 1; }
  if(!strcmp(name,"c_maroon")){ *out=128; return 1; }
  if(!strcmp(name,"c_green")){ *out=32768; return 1; }
  if(!strcmp(name,"c_red")){ *out=255; return 1; }
  if(!strcmp(name,"c_white")){ *out=16777215; return 1; }
  if(!strcmp(name,"c_lime")){ *out=65280; return 1; }
  if(!strcmp(name,"mb_left")){ *out=1; return 1; }
  if(!strcmp(name,"bm_normal")){ *out=0; return 1; }
  if(!strcmp(name,"bm_subtract")){ *out=3; return 1; }
  if(!strcmp(name,"vk_space")){ *out=32; return 1; }
  if(!strcmp(name,"vk_enter")){ *out=13; return 1; }
  if(!strcmp(name,"vk_escape")){ *out=27; return 1; }
  if(!strcmp(name,"vk_left")){ *out=37; return 1; }
  if(!strcmp(name,"vk_up")){ *out=38; return 1; }
  if(!strcmp(name,"vk_right")){ *out=39; return 1; }
  if(!strcmp(name,"vk_down")){ *out=40; return 1; }
  if(!strcmp(name,"vk_numpad1")){ *out=97; return 1; }
  if(!strcmp(name,"vk_numpad2")){ *out=98; return 1; }
  if(!strcmp(name,"vk_numpad3")){ *out=99; return 1; }
  if(!strcmp(name,"vk_numpad5")){ *out=101; return 1; }
  if(!strcmp(name,"gp_face1")){ *out=0; return 1; }
  if(!strcmp(name,"gp_face2")){ *out=1; return 1; }
  if(!strcmp(name,"gp_face3")){ *out=2; return 1; }
  if(!strcmp(name,"gp_face4")){ *out=3; return 1; }
  if(!strcmp(name,"gp_start")){ *out=6; return 1; }
  if(!strcmp(name,"gp_shoulderr")){ *out=5; return 1; }
  if(!strcmp(name,"gp_padr")){ *out=15; return 1; }
  if(!strcmp(name,"gp_padl")){ *out=14; return 1; }
  if(!strcmp(name,"gp_padu")){ *out=12; return 1; }
  if(!strcmp(name,"gp_padd")){ *out=13; return 1; }
  if(!strcmp(name,"gp_axislh")){ *out=0; return 1; }
  if(!strcmp(name,"gp_axislv")){ *out=1; return 1; }
  return macro_value(c,name,out) || resolve_asset(c,name,out);
}

static int parse_expr(Compiler *c);
static int parse_statement(Compiler *c);

typedef struct {
  size_t start, end;
} Span;

static int word_match_at(const char *src, size_t pos, const char *w){
  size_t n=strlen(w);
  if(strncmp(src+pos,w,n)) return 0;
  if(pos>0 && (isalnum((unsigned char)src[pos-1]) || src[pos-1]=='_')) return 0;
  if(isalnum((unsigned char)src[pos+n]) || src[pos+n]=='_') return 0;
  return 1;
}

static void trim_span(const char *src, Span *s){
  while(s->start<s->end && isspace((unsigned char)src[s->start])) s->start++;
  while(s->end>s->start && isspace((unsigned char)src[s->end-1])) s->end--;
}

static int span_empty(const char *src, Span s){
  size_t p=s.start;
  while(p<s.end){
    if(isspace((unsigned char)src[p])){ p++; continue; }
    if(p+1<s.end && src[p]=='/' && src[p+1]=='/'){
      p+=2;
      while(p<s.end && src[p]!='\n') p++;
      continue;
    }
    if(p+1<s.end && src[p]=='/' && src[p+1]=='*'){
      p+=2;
      while(p+1<s.end && !(src[p]=='*' && src[p+1]=='/')) p++;
      if(p+1<s.end) p+=2;
      continue;
    }
    return 0;
  }
  return 1;
}

static size_t skip_ws_comments_at(const char *src, size_t pos){
  for(;;){
    while(isspace((unsigned char)src[pos])) pos++;
    if(src[pos]=='/' && src[pos+1]=='/'){
      pos+=2;
      while(src[pos] && src[pos]!='\n') pos++;
      continue;
    }
    if(src[pos]=='/' && src[pos+1]=='*'){
      pos+=2;
      while(src[pos] && !(src[pos]=='*' && src[pos+1]=='/')) pos++;
      if(src[pos]) pos+=2;
      continue;
    }
    break;
  }
  return pos;
}

static int scan_matching_delim(const char *src, size_t open_pos, char open_ch, char close_ch, size_t *out_close){
  int depth=0;
  for(size_t pos=open_pos; src[pos]; pos++){
    char ch=src[pos];
    if(ch=='"'){
      pos++;
      while(src[pos]){
        if(src[pos]=='\\' && src[pos+1]){ pos++; continue; }
        if(src[pos]=='"') break;
        pos++;
      }
      continue;
    }
    if(ch=='/' && src[pos+1]=='/'){
      pos+=2;
      while(src[pos] && src[pos]!='\n') pos++;
      if(!src[pos]) break;
      continue;
    }
    if(ch=='/' && src[pos+1]=='*'){
      pos+=2;
      while(src[pos] && !(src[pos]=='*' && src[pos+1]=='/')) pos++;
      if(!src[pos]) break;
      pos++;
      continue;
    }
    if(ch==open_ch){ depth++; continue; }
    if(ch==close_ch){
      depth--;
      if(depth==0){ *out_close=pos; return 1; }
    }
  }
  return 0;
}

static int compile_expr_slice(Compiler *c, const char *start, size_t len){
  char *tmp=dup_range(start,len);
  if(!tmp) return 0;
  Lexer saved=c->lex;
  Lexer lx;
  memset(&lx,0,sizeof(lx));
  lx.src=tmp;
  c->lex=lx;
  lx_next(&c->lex);
  int ok=parse_expr(c);
  if(ok && c->lex.tok.kind!=TOK_EOF){
    c->unsupported=1;
    snprintf(c->lex.err,sizeof(c->lex.err),"unexpected token '%s' in expression",c->lex.tok.text);
    ok=0;
  }
  char errcopy[sizeof(c->lex.err)];
  snprintf(errcopy,sizeof(errcopy),"%s",c->lex.err);
  c->lex=saved;
  if(!ok && errcopy[0]) snprintf(c->lex.err,sizeof(c->lex.err),"%s",errcopy);
  free(tmp);
  return ok;
}

static int compile_statement_slice(Compiler *c, const char *start, size_t len){
  char *tmp=dup_range(start,len);
  if(!tmp) return 0;
  Lexer saved=c->lex;
  Lexer lx;
  memset(&lx,0,sizeof(lx));
  lx.src=tmp;
  c->lex=lx;
  lx_next(&c->lex);
  int ok=1;
  while(ok && c->lex.tok.kind!=TOK_EOF) ok=parse_statement(c);
  char errcopy[sizeof(c->lex.err)];
  snprintf(errcopy,sizeof(errcopy),"%s",c->lex.err);
  c->lex=saved;
  if(!ok && errcopy[0]) snprintf(c->lex.err,sizeof(c->lex.err),"%s",errcopy);
  free(tmp);
  return ok;
}

static int scan_comma_spans(Compiler *c, size_t start, Span **out_spans, int *out_n, size_t *out_close){
  const char *src=c->lex.src;
  Span *spans=NULL;
  int n=0, cap=0, depth=0;
  size_t pos=start, seg=start;
  while(src[pos]){
    char ch=src[pos];
    if(ch=='"'){
      pos++;
      while(src[pos]){
        if(src[pos]=='\\' && src[pos+1]){ pos+=2; continue; }
        if(src[pos]=='"'){ pos++; break; }
        pos++;
      }
      continue;
    }
    if(ch=='/' && src[pos+1]=='/'){
      pos+=2;
      while(src[pos] && src[pos]!='\n') pos++;
      continue;
    }
    if(ch=='/' && src[pos+1]=='*'){
      pos+=2;
      while(src[pos] && !(src[pos]=='*' && src[pos+1]=='/')) pos++;
      if(src[pos]) pos+=2;
      continue;
    }
    if(ch=='(' || ch=='[' || ch=='{'){ depth++; pos++; continue; }
    if(ch==')'){
      if(depth==0){
        Span s={seg,pos};
        trim_span(src,&s);
        if(s.start<s.end){
          if(n>=cap){
            int nc=cap?cap*2:8;
            Span *ns=(Span*)realloc(spans,(size_t)nc*sizeof(*ns));
            if(!ns){ free(spans); return 0; }
            spans=ns; cap=nc;
          }
          spans[n++]=s;
        }
        *out_spans=spans;
        *out_n=n;
        *out_close=pos;
        return 1;
      }
      depth--; pos++; continue;
    }
    if((ch==']' || ch=='}') && depth>0){ depth--; pos++; continue; }
    if(ch==',' && depth==0){
      Span s={seg,pos};
      trim_span(src,&s);
      if(n>=cap){
        int nc=cap?cap*2:8;
        Span *ns=(Span*)realloc(spans,(size_t)nc*sizeof(*ns));
        if(!ns){ free(spans); return 0; }
        spans=ns; cap=nc;
      }
      spans[n++]=s;
      seg=pos+1;
    }
    pos++;
  }
  free(spans);
  c->unsupported=1;
  snprintf(c->lex.err,sizeof(c->lex.err),"unterminated argument list");
  return 0;
}

static int parse_call_args_reversed(Compiler *c, int *argc){
  if(tok_is(c,")")){
    lx_next(&c->lex);
    *argc=0;
    return 1;
  }
  Span *spans=NULL;
  int n=0;
  size_t close_pos=0;
  if(!scan_comma_spans(c,c->lex.tok.start,&spans,&n,&close_pos)) return 0;
  for(int i=n-1;i>=0;i--){
    if(!compile_expr_slice(c,c->lex.src+spans[i].start,spans[i].end-spans[i].start)){
      free(spans);
      return 0;
    }
  }
  free(spans);
  c->lex.pos=close_pos+1;
  lx_next(&c->lex);
  *argc=n;
  return 1;
}

static int scan_square_span(Compiler *c, size_t start, Span *out, size_t *out_close){
  const char *src=c->lex.src;
  int depth=0;
  size_t pos=start;
  while(src[pos]){
    char ch=src[pos];
    if(ch=='"'){
      pos++;
      while(src[pos]){
        if(src[pos]=='\\' && src[pos+1]){ pos+=2; continue; }
        if(src[pos]=='"'){ pos++; break; }
        pos++;
      }
      continue;
    }
    if(ch=='/' && src[pos+1]=='/'){
      pos+=2;
      while(src[pos] && src[pos]!='\n') pos++;
      continue;
    }
    if(ch=='/' && src[pos+1]=='*'){
      pos+=2;
      while(src[pos] && !(src[pos]=='*' && src[pos+1]=='/')) pos++;
      if(src[pos]) pos+=2;
      continue;
    }
    if(ch=='(' || ch=='[' || ch=='{'){ depth++; pos++; continue; }
    if(ch==']'){
      if(depth==0){
        out->start=start;
        out->end=pos;
        trim_span(src,out);
        *out_close=pos;
        return 1;
      }
      depth--; pos++; continue;
    }
    if((ch==')' || ch=='}') && depth>0){ depth--; pos++; continue; }
    pos++;
  }
  c->unsupported=1;
  snprintf(c->lex.err,sizeof(c->lex.err),"unterminated array index");
  return 0;
}

static int find_top_comma(const char *src, Span s, size_t *comma_pos){
  int depth=0;
  for(size_t pos=s.start; pos<s.end; pos++){
    char ch=src[pos];
    if(ch=='"'){
      pos++;
      while(pos<s.end){
        if(src[pos]=='\\' && pos+1<s.end){ pos++; pos++; continue; }
        if(src[pos]=='"') break;
        pos++;
      }
      continue;
    }
    if(ch=='(' || ch=='[' || ch=='{'){ depth++; continue; }
    if((ch==')' || ch==']' || ch=='}') && depth>0){ depth--; continue; }
    if(ch==',' && depth==0){ *comma_pos=pos; return 1; }
  }
  return 0;
}

static int scan_for_header(Compiler *c, size_t start, Span out[3], size_t *out_close){
  const char *src=c->lex.src;
  int depth=0, part=0;
  size_t pos=start, seg=start;
  memset(out,0,3*sizeof(*out));
  while(src[pos]){
    char ch=src[pos];
    if(ch=='"'){
      pos++;
      while(src[pos]){
        if(src[pos]=='\\' && src[pos+1]){ pos+=2; continue; }
        if(src[pos]=='"'){ pos++; break; }
        pos++;
      }
      continue;
    }
    if(ch=='/' && src[pos+1]=='/'){
      pos+=2;
      while(src[pos] && src[pos]!='\n') pos++;
      continue;
    }
    if(ch=='/' && src[pos+1]=='*'){
      pos+=2;
      while(src[pos] && !(src[pos]=='*' && src[pos+1]=='/')) pos++;
      if(src[pos]) pos+=2;
      continue;
    }
    if(ch=='(' || ch=='[' || ch=='{'){ depth++; pos++; continue; }
    if(ch==')'){
      if(depth==0){
        if(part>2) return 0;
        out[part].start=seg; out[part].end=pos; trim_span(src,&out[part]);
        *out_close=pos;
        return part==2;
      }
      depth--; pos++; continue;
    }
    if((ch==']' || ch=='}') && depth>0){ depth--; pos++; continue; }
    if(ch==';' && depth==0){
      if(part>=2) break;
      out[part].start=seg; out[part].end=pos; trim_span(src,&out[part]);
      part++;
      seg=pos+1;
    }
    pos++;
  }
  c->unsupported=1;
  snprintf(c->lex.err,sizeof(c->lex.err),"unterminated for header");
  return 0;
}

static int scan_brace_body(Compiler *c, Span *body, size_t *out_close){
  if(!tok_is(c,"{")) return need(c,"{");
  const char *src=c->lex.src;
  size_t start=c->lex.tok.end;
  size_t pos=start;
  int depth=0;
  while(src[pos]){
    char ch=src[pos];
    if(ch=='"'){
      pos++;
      while(src[pos]){
        if(src[pos]=='\\' && src[pos+1]){ pos+=2; continue; }
        if(src[pos]=='"'){ pos++; break; }
        pos++;
      }
      continue;
    }
    if(ch=='/' && src[pos+1]=='/'){
      pos+=2;
      while(src[pos] && src[pos]!='\n') pos++;
      continue;
    }
    if(ch=='/' && src[pos+1]=='*'){
      pos+=2;
      while(src[pos] && !(src[pos]=='*' && src[pos+1]=='/')) pos++;
      if(src[pos]) pos+=2;
      continue;
    }
    if(ch=='{'){ depth++; pos++; continue; }
    if(ch=='}'){
      if(depth==0){
        body->start=start;
        body->end=pos;
        *out_close=pos;
        return 1;
      }
      depth--; pos++; continue;
    }
    pos++;
  }
  c->unsupported=1;
  snprintf(c->lex.err,sizeof(c->lex.err),"unterminated block");
  return 0;
}

typedef struct {
  Span label;
  Span body;
  int is_default;
} CaseRec;

static int add_case_rec(CaseRec **items, int *n, int *cap, CaseRec rec){
  if(*n>=*cap){
    int nc=*cap?*cap*2:8;
    CaseRec *ni=(CaseRec*)realloc(*items,(size_t)nc*sizeof(*ni));
    if(!ni) return 0;
    *items=ni; *cap=nc;
  }
  (*items)[(*n)++]=rec;
  return 1;
}

static int scan_colon_top(const char *src, size_t start, size_t end, size_t *colon){
  int depth=0;
  for(size_t pos=start; pos<end; pos++){
    char ch=src[pos];
    if(ch=='"'){
      pos++;
      while(pos<end){
        if(src[pos]=='\\' && pos+1<end){ pos++; pos++; continue; }
        if(src[pos]=='"') break;
        pos++;
      }
      continue;
    }
    if(ch=='(' || ch=='[' || ch=='{'){ depth++; continue; }
    if((ch==')' || ch==']' || ch=='}') && depth>0){ depth--; continue; }
    if(ch==':' && depth==0){ *colon=pos; return 1; }
  }
  return 0;
}

static int scan_switch_cases(Compiler *c, Span body, CaseRec **out_cases, int *out_n){
  const char *src=c->lex.src;
  CaseRec *cases=NULL;
  int n=0, cap=0, depth=0;
  size_t pos=body.start;
  while(pos<body.end){
    char ch=src[pos];
    if(ch=='"'){
      pos++;
      while(pos<body.end){
        if(src[pos]=='\\' && pos+1<body.end){ pos+=2; continue; }
        if(src[pos]=='"'){ pos++; break; }
        pos++;
      }
      continue;
    }
    if(ch=='/' && pos+1<body.end && src[pos+1]=='/'){
      pos+=2; while(pos<body.end && src[pos]!='\n') pos++; continue;
    }
    if(ch=='/' && pos+1<body.end && src[pos+1]=='*'){
      pos+=2; while(pos+1<body.end && !(src[pos]=='*' && src[pos+1]=='/')) pos++; if(pos+1<body.end) pos+=2; continue;
    }
    if(ch=='{'){ depth++; pos++; continue; }
    if(ch=='}' && depth>0){ depth--; pos++; continue; }
    if(depth==0 && (word_match_at(src,pos,"case") || word_match_at(src,pos,"default"))){
      if(n>0) cases[n-1].body.end=pos;
      int is_def=word_match_at(src,pos,"default");
      size_t label_start=pos+(is_def?7:4);
      size_t colon=0;
      if(!scan_colon_top(src,label_start,body.end,&colon)){
        free(cases);
        c->unsupported=1;
        snprintf(c->lex.err,sizeof(c->lex.err),"switch case missing ':'");
        return 0;
      }
      CaseRec rec;
      memset(&rec,0,sizeof(rec));
      rec.is_default=is_def;
      rec.label.start=label_start;
      rec.label.end=colon;
      trim_span(src,&rec.label);
      rec.body.start=colon+1;
      rec.body.end=body.end;
      if(!add_case_rec(&cases,&n,&cap,rec)){ free(cases); return 0; }
      pos=colon+1;
      continue;
    }
    pos++;
  }
  *out_cases=cases;
  *out_n=n;
  return 1;
}

static int emit_receiver_value(Compiler *c, const char *name){
  double cv=0;
  if(resolve_const(c,name,&cv)) return emit_push_real(c,cv);
  if(local_index(c,name)>=0 || !strncmp(name,"argument",8)) return emit_push_var(c,IT_LOCAL,name,0xA0);
  return emit_push_var(c,IT_SELF,name,0xA0);
}

typedef struct {
  char name[128];
  char receiver[128];
  int inst;
  uint8_t reftype;
  int is_stacktop;
  int is_array;
  int is_array_2d;
  int accessor;
  Span index_span;
  Span index2_span;
  char *index_src;
} LValue;

enum {
  ACCESS_NONE=0,
  ACCESS_MAP,
  ACCESS_LIST,
  ACCESS_GRID
};

static int emit_lvalue_address(Compiler *c, LValue *lv){
  if(lv->is_stacktop){
    if(!emit_receiver_value(c,lv->receiver)) return 0;
    if(!emit_push_real(c,-9)) return 0;
  }
  else if(lv->is_array){
    if(!emit_push_real(c,lv->inst)) return 0;
  }
  if(lv->is_array){
    if(!lv->index_src) return 0;
    if(!compile_expr_slice(c,lv->index_src+lv->index_span.start,lv->index_span.end-lv->index_span.start)) return 0;
    if(lv->is_array_2d){
      if(!emit_push_real(c,32000) || !emit_binary(c,OP_MUL)) return 0;
      if(!compile_expr_slice(c,lv->index_src+lv->index2_span.start,lv->index2_span.end-lv->index2_span.start)) return 0;
      if(!emit_binary(c,OP_ADD)) return 0;
    }
  }
  return 1;
}

static int parse_lvalue_from_name(Compiler *c, const char *first, LValue *lv);
static int emit_lvalue_read(Compiler *c, LValue *lv);
static int emit_popz(Compiler *c){ return emit_u32(&c->code,fw(OP_POPZ,0,0)); }

static int function_shape_at(const char *src, size_t pos){
  if(!word_match_at(src,pos,"function")) return 0;
  pos=skip_ws_comments_at(src,pos+8);
  if(src[pos]=='(') return 1;
  if(isalpha((unsigned char)src[pos]) || src[pos]=='_'){
    pos++;
    while(isalnum((unsigned char)src[pos]) || src[pos]=='_') pos++;
    pos=skip_ws_comments_at(src,pos);
    return src[pos]=='(';
  }
  return 0;
}

static int parse_function_value(Compiler *c, int emit_value){
  if(!is_id(c,"function") || !function_shape_at(c->lex.src,c->lex.tok.start)){
    c->unsupported=1;
    snprintf(c->lex.err,sizeof(c->lex.err),"expected function literal");
    return 0;
  }
  lx_next(&c->lex);
  char name[128]={0};
  if(c->lex.tok.kind==TOK_ID){
    size_t p=skip_ws_comments_at(c->lex.src,c->lex.tok.end);
    if(c->lex.src[p]=='('){
      snprintf(name,sizeof(name),"%s",c->lex.tok.text);
      lx_next(&c->lex);
    }
  }
  if(!tok_is(c,"(")) return need(c,"(");
  size_t paren_open=c->lex.tok.start, paren_close=0;
  if(!scan_matching_delim(c->lex.src,paren_open,'(',')',&paren_close)){
    c->unsupported=1;
    snprintf(c->lex.err,sizeof(c->lex.err),"unterminated function parameter list");
    return 0;
  }
  Span params={paren_open+1,paren_close};
  trim_span(c->lex.src,&params);
  c->lex.pos=paren_close+1;
  lx_next(&c->lex);
  Span body={0,0};
  size_t body_close=0;
  if(!scan_brace_body(c,&body,&body_close)) return 0;
  char *params_text=dup_range(c->lex.src+params.start,params.end-params.start);
  char *body_text=dup_range(c->lex.src+body.start,body.end-body.start);
  if(!params_text || !body_text){ free(params_text); free(body_text); return 0; }
  int ci=-1;
  int found=find_function_literal_index(c,name,params_text,body_text,&ci);
  free(params_text);
  free(body_text);
  if(!found){
    c->unsupported=1;
    snprintf(c->lex.err,sizeof(c->lex.err),"function literal not indexed");
    return 0;
  }
  c->lex.pos=body_close+1;
  lx_next(&c->lex);
  return emit_value ? emit_script_funcval(c,ci) : 1;
}

static int parse_primary(Compiler *c){
  if(c->lex.tok.kind==TOK_NUM){ double d=c->lex.tok.num; lx_next(&c->lex); return emit_push_real(c,d); }
  if(c->lex.tok.kind==TOK_STR){
    char value[128];
    snprintf(value,sizeof(value),"%s",c->lex.tok.text);
    lx_next(&c->lex);
    return emit_push_string_literal(c,value);
  }
  int allow_postfix_call=1;
  if(is_id(c,"function") && function_shape_at(c->lex.src,c->lex.tok.start)){
    if(!parse_function_value(c,1)) return 0;
    goto postfix_calls;
  }
  if(eat(c,"(")){ if(!parse_expr(c)) return 0; if(!need(c,")")) return 0; }
  if(c->lex.tok.kind==TOK_ID){
    char name[128]; snprintf(name,sizeof(name),"%s",c->lex.tok.text); lx_next(&c->lex);
    if(eat(c,"(")){
      int argc=0;
      if(local_index(c,name)>=0 || !strncmp(name,"argument",8)){
        if(!emit_push_var(c,IT_LOCAL,name,0xA0)) return 0;
        if(!parse_call_args_reversed(c,&argc)) return 0;
        if(!emit_callv(c,argc)) return 0;
      } else {
        if(!parse_call_args_reversed(c,&argc)) return 0;
        if(!emit_call(c,name,argc)) return 0;
      }
      allow_postfix_call=1;
      goto postfix_calls;
    }
    if(tok_is(c,".") || tok_is(c,"[")){
      LValue lv;
      if(!parse_lvalue_from_name(c,name,&lv)) return 0;
      int ok=emit_lvalue_read(c,&lv);
      free(lv.index_src);
      if(!ok) return 0;
      goto postfix_calls;
    }
    double cv=0;
    int sci=-1;
    if(resolve_function_code_index(c,name,&sci)){
      if(!emit_script_funcval(c,sci)) return 0;
      goto postfix_calls;
    }
    if(resolve_const(c,name,&cv)){
      if(!emit_push_real(c,cv)) return 0;
      goto postfix_calls;
    }
    int inst=(local_index(c,name)>=0 || !strncmp(name,"argument",8)) ? IT_LOCAL : IT_SELF;
    if(!emit_push_var(c,inst,name,0xA0)) return 0;
    goto postfix_calls;
  }
  if(allow_postfix_call) goto postfix_calls;
  c->unsupported=1;
  snprintf(c->lex.err,sizeof(c->lex.err),"unexpected expression token '%s'",c->lex.tok.text);
  return 0;

postfix_calls:
  while(tok_is(c,"(")){
    lx_next(&c->lex);
    int argc=0;
    if(!parse_call_args_reversed(c,&argc)) return 0;
    if(!emit_callv(c,argc)) return 0;
  }
  return 1;
}

static int parse_unary(Compiler *c){
  if(eat(c,"!")){ if(!parse_unary(c)) return 0; return emit_u32(&c->code,fw(OP_NOT,(uint8_t)((DT_VAR<<4)|DT_VAR),0)); }
  if(eat(c,"-")){ if(!parse_unary(c)) return 0; return emit_u32(&c->code,fw(OP_NEG,(uint8_t)((DT_VAR<<4)|DT_VAR),0)); }
  return parse_primary(c);
}

static int parse_mul(Compiler *c){
  if(!parse_unary(c)) return 0;
  while(tok_is(c,"*")||tok_is(c,"/")||is_id(c,"div")||is_id(c,"mod")){
    int op=tok_is(c,"*")?OP_MUL:tok_is(c,"/")?OP_DIV:is_id(c,"div")?OP_REM:OP_MOD;
    lx_next(&c->lex);
    if(!parse_unary(c)) return 0;
    emit_binary(c,(uint8_t)op);
  }
  return 1;
}

static int parse_add(Compiler *c){
  if(!parse_mul(c)) return 0;
  while(tok_is(c,"+")||tok_is(c,"-")){
    int sub=tok_is(c,"-"); lx_next(&c->lex);
    if(!parse_mul(c)) return 0;
    emit_binary(c,sub?OP_SUB:OP_ADD);
  }
  return 1;
}

static int parse_cmp_expr(Compiler *c){
  if(!parse_add(c)) return 0;
  while(tok_is(c,"<")||tok_is(c,"<=")||tok_is(c,">")||tok_is(c,">=")){
    uint8_t cmp=tok_is(c,"<")?CMP_LT:tok_is(c,"<=")?CMP_LTE:tok_is(c,">")?CMP_GT:CMP_GTE;
    lx_next(&c->lex);
    if(!parse_add(c)) return 0;
    emit_cmp(c,cmp);
  }
  return 1;
}

static int parse_eq(Compiler *c){
  if(!parse_cmp_expr(c)) return 0;
  while(tok_is(c,"==")||tok_is(c,"!=")){
    int ne=tok_is(c,"!="); lx_next(&c->lex);
    if(!parse_cmp_expr(c)) return 0;
    emit_cmp(c,ne?CMP_NEQ:CMP_EQ);
  }
  return 1;
}

static int parse_and(Compiler *c){
  if(!parse_eq(c)) return 0;
  while(tok_is(c,"&&")){ lx_next(&c->lex); if(!parse_eq(c)) return 0; emit_binary(c,OP_AND); }
  return 1;
}

static int parse_expr(Compiler *c){
  if(!parse_and(c)) return 0;
  while(tok_is(c,"||")){ lx_next(&c->lex); if(!parse_and(c)) return 0; emit_binary(c,OP_OR); }
  if(tok_is(c,"?")){
    lx_next(&c->lex);
    size_t bf=emit_branch(c,OP_BF);
    if(!parse_expr(c)) return 0;
    if(!need(c,":")) return 0;
    size_t b=emit_branch(c,OP_B);
    patch_branch(c,bf,c->code.len);
    if(!parse_expr(c)) return 0;
    patch_branch(c,b,c->code.len);
  }
  return 1;
}

static int parse_lvalue_from_name(Compiler *c, const char *first, LValue *lv){
  memset(lv,0,sizeof(*lv));
  snprintf(lv->name,sizeof(lv->name),"%s",first);
  snprintf(lv->receiver,sizeof(lv->receiver),"%s",first);
  lv->inst=(local_index(c,first)>=0 || !strncmp(first,"argument",8)) ? IT_LOCAL : IT_SELF;
  lv->reftype=0xA0;
  if(eat(c,".")){
    if(c->lex.tok.kind!=TOK_ID){ c->unsupported=1; snprintf(c->lex.err,sizeof(c->lex.err),"expected field name"); return 0; }
    char field[128]; snprintf(field,sizeof(field),"%s",c->lex.tok.text); lx_next(&c->lex);
    double cv=0;
    if(!strcmp(first,"global")) lv->inst=IT_GLOBAL;
    else if(resolve_asset(c,first,&cv)) lv->inst=(int)cv;
    else { lv->is_stacktop=1; lv->inst=IT_STACK; }
    snprintf(lv->name,sizeof(lv->name),"%s",field);
  }
  if(eat(c,"[")){
    size_t close_pos=0;
    if(!scan_square_span(c,c->lex.tok.start,&lv->index_span,&close_pos)) return 0;
    lv->index_src=gmlc_strdup(c->lex.src);
    if(!lv->index_src) return 0;
    if(lv->index_span.start<lv->index_span.end){
      char ch=c->lex.src[lv->index_span.start];
      if(ch=='?' || ch=='|' || ch=='#'){
        lv->accessor=(ch=='?')?ACCESS_MAP:(ch=='|')?ACCESS_LIST:ACCESS_GRID;
        lv->index_span.start++;
        trim_span(c->lex.src,&lv->index_span);
      }
    }
    size_t comma=0;
    if(lv->accessor==ACCESS_GRID){
      if(!find_top_comma(c->lex.src,lv->index_span,&comma)){
        c->unsupported=1;
        snprintf(c->lex.err,sizeof(c->lex.err),"grid accessor requires two indices");
        free(lv->index_src);
        lv->index_src=NULL;
        return 0;
      }
      lv->index2_span.start=comma+1;
      lv->index2_span.end=lv->index_span.end;
      lv->index_span.end=comma;
      trim_span(c->lex.src,&lv->index_span);
      trim_span(c->lex.src,&lv->index2_span);
    } else if(lv->accessor==ACCESS_MAP || lv->accessor==ACCESS_LIST){
      if(find_top_comma(c->lex.src,lv->index_span,&comma)){
        c->unsupported=1;
        snprintf(c->lex.err,sizeof(c->lex.err),"accessor requires one index");
        free(lv->index_src);
        lv->index_src=NULL;
        return 0;
      }
    } else {
      lv->is_array=1;
      if(find_top_comma(c->lex.src,lv->index_span,&comma)){
        lv->is_array_2d=1;
        lv->index2_span.start=comma+1;
        lv->index2_span.end=lv->index_span.end;
        lv->index_span.end=comma;
        trim_span(c->lex.src,&lv->index_span);
        trim_span(c->lex.src,&lv->index2_span);
      }
      lv->reftype=0x00;
    }
    c->lex.pos=close_pos+1;
    lx_next(&c->lex);
  }
  return 1;
}

static int emit_lvalue_base_read(Compiler *c, LValue *lv){
  if(lv->is_stacktop){
    if(!emit_lvalue_address(c,lv)) return 0;
    return emit_push_var(c,IT_STACK,lv->name,0x80);
  }
  return emit_push_var(c,lv->inst,lv->name,0xA0);
}

static int emit_accessor_key_args(Compiler *c, LValue *lv){
  if(!lv->index_src) return 0;
  if(lv->accessor==ACCESS_GRID){
    if(!compile_expr_slice(c,lv->index_src+lv->index2_span.start,lv->index2_span.end-lv->index2_span.start)) return 0;
    if(!compile_expr_slice(c,lv->index_src+lv->index_span.start,lv->index_span.end-lv->index_span.start)) return 0;
    return 1;
  }
  return compile_expr_slice(c,lv->index_src+lv->index_span.start,lv->index_span.end-lv->index_span.start);
}

static int emit_accessor_read(Compiler *c, LValue *lv){
  const char *fn = lv->accessor==ACCESS_MAP ? "ds_map_find_value" :
                   lv->accessor==ACCESS_LIST ? "ds_list_find_value" : "ds_grid_get";
  int argc = lv->accessor==ACCESS_GRID ? 3 : 2;
  if(!emit_accessor_key_args(c,lv)) return 0;
  if(!emit_lvalue_base_read(c,lv)) return 0;
  return emit_call(c,fn,argc);
}

static int emit_accessor_write(Compiler *c, LValue *lv){
  const char *fn = lv->accessor==ACCESS_MAP ? "ds_map_set" :
                   lv->accessor==ACCESS_LIST ? "ds_list_replace" : "ds_grid_set";
  int argc = lv->accessor==ACCESS_GRID ? 4 : 3;
  if(!emit_accessor_key_args(c,lv)) return 0;
  if(!emit_lvalue_base_read(c,lv)) return 0;
  if(!emit_call(c,fn,argc)) return 0;
  return emit_popz(c);
}

static int emit_lvalue_read(Compiler *c, LValue *lv){
  if(lv->accessor) return emit_accessor_read(c,lv);
  if(lv->is_array){
    if(!emit_lvalue_address(c,lv)) return 0;
    return emit_push_var(c,IT_SELF,lv->name,0x00);
  }
  if(lv->is_stacktop){
    if(!emit_lvalue_address(c,lv)) return 0;
    return emit_push_var(c,IT_STACK,lv->name,0x80);
  }
  return emit_push_var(c,lv->inst,lv->name,0xA0);
}

static int emit_lvalue_write(Compiler *c, LValue *lv, uint8_t type1){
  if(lv->accessor) return emit_accessor_write(c,lv);
  if(lv->is_array) return emit_pop_var(c,IT_SELF,lv->name,0x00,type1);
  if(lv->is_stacktop) return emit_pop_var(c,IT_STACK,lv->name,0x80,type1);
  return emit_pop_var(c,lv->inst,lv->name,0xA0,type1);
}

static int parse_statement(Compiler *c);

static int parse_block_or_stmt(Compiler *c){
  if(eat(c,"{")){
    while(c->lex.tok.kind!=TOK_EOF && !eat(c,"}")) if(!parse_statement(c)) return 0;
    return 1;
  }
  return parse_statement(c);
}

static int parse_var_decl(Compiler *c){
  lx_next(&c->lex);
  do {
    if(c->lex.tok.kind!=TOK_ID){ c->unsupported=1; snprintf(c->lex.err,sizeof(c->lex.err),"expected local name"); return 0; }
    char name[128]; snprintf(name,sizeof(name),"%s",c->lex.tok.text); lx_next(&c->lex);
    add_local(c,name);
    if(eat(c,"=")){ if(!parse_expr(c)) return 0; if(!emit_pop_var(c,IT_LOCAL,name,0xA0,DT_VAR)) return 0; }
  } while(eat(c,","));
  eat(c,";");
  return 1;
}

static int parse_if(Compiler *c){
  lx_next(&c->lex);
  if(!need(c,"(") || !parse_expr(c) || !need(c,")")) return 0;
  size_t bf=emit_branch(c,OP_BF);
  if(!parse_block_or_stmt(c)) return 0;
  if(is_id(c,"else")){
    size_t b=emit_branch(c,OP_B);
    patch_branch(c,bf,c->code.len);
    lx_next(&c->lex);
    if(!parse_block_or_stmt(c)) return 0;
    patch_branch(c,b,c->code.len);
  } else {
    patch_branch(c,bf,c->code.len);
  }
  return 1;
}

static int parse_while(Compiler *c){
  lx_next(&c->lex);
  size_t start=c->code.len;
  if(!need(c,"(") || !parse_expr(c) || !need(c,")")) return 0;
  int break_mark=c->n_break_sites;
  int continue_mark=c->n_continue_sites;
  c->continue_depth++;
  size_t bf=emit_branch(c,OP_BF);
  if(!parse_block_or_stmt(c)) return 0;
  c->continue_depth--;
  patch_continues_from(c,continue_mark,start);
  size_t b=emit_branch(c,OP_B);
  patch_branch(c,b,start);
  patch_branch(c,bf,c->code.len);
  patch_breaks_from(c,break_mark,c->code.len);
  return 1;
}

static int parse_for(Compiler *c){
  lx_next(&c->lex);
  if(!need(c,"(")) return 0;
  Span parts[3];
  size_t close_pos=0;
  if(!scan_for_header(c,c->lex.tok.start,parts,&close_pos)) return 0;
  const char *src=c->lex.src;
  if(parts[0].start<parts[0].end && !compile_statement_slice(c,src+parts[0].start,parts[0].end-parts[0].start)) return 0;
  size_t loop_start=c->code.len;
  size_t bf=(size_t)-1;
  if(parts[1].start<parts[1].end){
    if(!compile_expr_slice(c,src+parts[1].start,parts[1].end-parts[1].start)) return 0;
    bf=emit_branch(c,OP_BF);
  }
  c->lex.pos=close_pos+1;
  lx_next(&c->lex);
  int break_mark=c->n_break_sites;
  int continue_mark=c->n_continue_sites;
  c->continue_depth++;
  if(!parse_block_or_stmt(c)) return 0;
  c->continue_depth--;
  size_t continue_target=c->code.len;
  patch_continues_from(c,continue_mark,continue_target);
  if(parts[2].start<parts[2].end && !compile_statement_slice(c,src+parts[2].start,parts[2].end-parts[2].start)) return 0;
  size_t b=emit_branch(c,OP_B);
  patch_branch(c,b,loop_start);
  if(bf!=(size_t)-1) patch_branch(c,bf,c->code.len);
  patch_breaks_from(c,break_mark,c->code.len);
  return 1;
}

static int parse_repeat(Compiler *c){
  lx_next(&c->lex);
  char tmpname[64];
  snprintf(tmpname,sizeof(tmpname),"__gmlc_repeat_%d",c->temp_id++);
  if(!add_local(c,tmpname)) return 0;
  if(!need(c,"(") || !parse_expr(c) || !need(c,")")) return 0;
  if(!emit_pop_var(c,IT_LOCAL,tmpname,0xA0,DT_VAR)) return 0;
  size_t loop_start=c->code.len;
  if(!emit_push_var(c,IT_LOCAL,tmpname,0xA0) ||
     !emit_push_real(c,0) ||
     !emit_cmp(c,CMP_LTE)) return 0;
  size_t done=emit_branch(c,OP_BT);
  if(!emit_push_var(c,IT_LOCAL,tmpname,0xA0) ||
     !emit_push_real(c,1) ||
     !emit_binary(c,OP_SUB) ||
     !emit_pop_var(c,IT_LOCAL,tmpname,0xA0,DT_VAR)) return 0;
  int break_mark=c->n_break_sites;
  int continue_mark=c->n_continue_sites;
  c->continue_depth++;
  if(!parse_block_or_stmt(c)) return 0;
  c->continue_depth--;
  patch_continues_from(c,continue_mark,loop_start);
  size_t b=emit_branch(c,OP_B);
  patch_branch(c,b,loop_start);
  patch_branch(c,done,c->code.len);
  patch_breaks_from(c,break_mark,c->code.len);
  return 1;
}

static int parse_with(Compiler *c){
  lx_next(&c->lex);
  if(!need(c,"(") || !parse_expr(c) || !need(c,")")) return 0;
  int break_mark=c->n_break_sites;
  int continue_mark=c->n_continue_sites;
  c->continue_depth++;
  size_t push=emit_branch(c,OP_PUSHENV);
  size_t body_start=c->code.len;
  if(!parse_block_or_stmt(c)) return 0;
  c->continue_depth--;
  size_t pop=emit_branch(c,OP_POPENV);
  patch_continues_from(c,continue_mark,pop);
  patch_branch(c,pop,body_start);
  patch_branch(c,push,c->code.len);
  patch_breaks_from(c,break_mark,c->code.len);
  return 1;
}

static int parse_switch(Compiler *c){
  lx_next(&c->lex);
  if(!need(c,"(") || !parse_expr(c) || !need(c,")")) return 0;
  char tmpname[64];
  snprintf(tmpname,sizeof(tmpname),"__gmlc_switch_%d",c->temp_id++);
  if(!add_local(c,tmpname)) return 0;
  if(!emit_pop_var(c,IT_LOCAL,tmpname,0xA0,DT_VAR)) return 0;
  Span body={0,0};
  size_t close_pos=0;
  if(!scan_brace_body(c,&body,&close_pos)) return 0;
  CaseRec *cases=NULL;
  int n_cases=0;
  if(!scan_switch_cases(c,body,&cases,&n_cases)) return 0;
  int *targets=(int*)calloc((size_t)(n_cases?n_cases:1),sizeof(int));
  size_t *case_br=(size_t*)calloc((size_t)(n_cases?n_cases:1),sizeof(size_t));
  size_t *body_pos=(size_t*)calloc((size_t)(n_cases?n_cases:1),sizeof(size_t));
  if(!targets || !case_br || !body_pos){ free(cases); free(targets); free(case_br); free(body_pos); return 0; }
  int default_idx=-1;
  for(int i=0;i<n_cases;i++){
    int t=i;
    while(t+1<n_cases && span_empty(c->lex.src,cases[t].body)) t++;
    targets[i]=t;
    if(cases[i].is_default && default_idx<0) default_idx=t;
  }
  for(int i=0;i<n_cases;i++){
    if(cases[i].is_default) continue;
    if(!emit_push_var(c,IT_LOCAL,tmpname,0xA0) ||
       !compile_expr_slice(c,c->lex.src+cases[i].label.start,cases[i].label.end-cases[i].label.start) ||
       !emit_cmp(c,CMP_EQ)){
      free(cases); free(targets); free(case_br); free(body_pos);
      return 0;
    }
    case_br[i]=emit_branch(c,OP_BT);
  }
  size_t dispatch_end=emit_branch(c,OP_B);
  int break_mark=c->n_break_sites;
  for(int i=0;i<n_cases;i++){
    body_pos[i]=c->code.len;
    if(!span_empty(c->lex.src,cases[i].body)){
      if(!compile_statement_slice(c,c->lex.src+cases[i].body.start,cases[i].body.end-cases[i].body.start)){
        free(cases); free(targets); free(case_br); free(body_pos);
        return 0;
      }
    }
  }
  size_t end=c->code.len;
  for(int i=0;i<n_cases;i++) if(case_br[i]) patch_branch(c,case_br[i],body_pos[targets[i]]);
  patch_branch(c,dispatch_end,default_idx>=0?body_pos[default_idx]:end);
  patch_breaks_from(c,break_mark,end);
  c->lex.pos=close_pos+1;
  lx_next(&c->lex);
  free(cases); free(targets); free(case_br); free(body_pos);
  return 1;
}

static int parse_return_stmt(Compiler *c){
  lx_next(&c->lex);
  if(!tok_is(c,";")){ if(!parse_expr(c)) return 0; if(!emit_u32(&c->code,fw(OP_RET,DT_VAR,0))) return 0; }
  else if(!emit_u32(&c->code,fw(OP_EXIT,0,0))) return 0;
  eat(c,";");
  return 1;
}

static int parse_assignment_tail(Compiler *c, LValue *lv){
  int is_assign=tok_is(c,"=");
  int is_inc=tok_is(c,"++");
  int is_dec=tok_is(c,"--");
  uint8_t binop=tok_is(c,"+=")?OP_ADD:tok_is(c,"-=")?OP_SUB:tok_is(c,"*=")?OP_MUL:OP_DIV;
  lx_next(&c->lex);
  if(is_assign){
    if(!parse_expr(c)) return 0;
    if((lv->is_array || lv->is_stacktop) && !emit_lvalue_address(c,lv)) return 0;
  } else {
    if(is_inc || is_dec){
      if(!emit_lvalue_read(c,lv) || !emit_push_real(c,1) || !emit_binary(c,is_inc?OP_ADD:OP_SUB)) return 0;
    } else {
      if(!emit_lvalue_read(c,lv) || !parse_expr(c)) return 0;
      emit_binary(c,binop);
    }
    if((lv->is_array || lv->is_stacktop) && !emit_lvalue_address(c,lv)) return 0;
  }
  if(!emit_lvalue_write(c,lv,DT_VAR)) return 0;
  eat(c,";");
  return 1;
}

static int parse_simple_or_assign(Compiler *c){
  if(c->lex.tok.kind!=TOK_ID){ if(!parse_expr(c)) return 0; eat(c,";"); emit_u32(&c->code,fw(OP_POPZ,0,0)); return 1; }
  char first[128]; snprintf(first,sizeof(first),"%s",c->lex.tok.text); lx_next(&c->lex);
  if(tok_is(c,"=")||tok_is(c,"+=")||tok_is(c,"-=")||tok_is(c,"*=")||tok_is(c,"/=")||tok_is(c,"++")||tok_is(c,"--")){
    LValue lv;
    if(!parse_lvalue_from_name(c,first,&lv)) return 0;
    int ok=parse_assignment_tail(c,&lv);
    free(lv.index_src);
    return ok;
  }
  if(tok_is(c,".") || tok_is(c,"[")){
    LValue lv;
    if(!parse_lvalue_from_name(c,first,&lv)) return 0;
    if(tok_is(c,"=")||tok_is(c,"+=")||tok_is(c,"-=")||tok_is(c,"*=")||tok_is(c,"/=")||tok_is(c,"++")||tok_is(c,"--")){
      int ok=parse_assignment_tail(c,&lv);
      free(lv.index_src);
      return ok;
    }
    if(!emit_lvalue_read(c,&lv)){ free(lv.index_src); return 0; }
    free(lv.index_src);
    while(tok_is(c,"(")){
      lx_next(&c->lex);
      int argc=0;
      if(!parse_call_args_reversed(c,&argc)) return 0;
      if(!emit_callv(c,argc)) return 0;
    }
    eat(c,";");
    emit_u32(&c->code,fw(OP_POPZ,0,0));
    return 1;
  }
  if(tok_is(c,"(")){
    lx_next(&c->lex);
    int argc=0;
    if(local_index(c,first)>=0 || !strncmp(first,"argument",8)){
      if(!emit_push_var(c,IT_LOCAL,first,0xA0)) return 0;
      if(!parse_call_args_reversed(c,&argc)) return 0;
      if(!emit_callv(c,argc)) return 0;
      emit_u32(&c->code,fw(OP_POPZ,0,0));
      eat(c,";");
      return 1;
    }
    if(!parse_call_args_reversed(c,&argc)) return 0;
    if(!emit_call(c,first,argc)) return 0;
    emit_u32(&c->code,fw(OP_POPZ,0,0));
    eat(c,";");
    return 1;
  }
  double cv=0;
  int sci=-1;
  if(resolve_function_code_index(c,first,&sci)) emit_script_funcval(c,sci);
  else if(resolve_const(c,first,&cv)) emit_push_real(c,cv);
  else emit_push_var(c,(local_index(c,first)>=0 || !strncmp(first,"argument",8))?IT_LOCAL:IT_SELF,first,0xA0);
  eat(c,";");
  emit_u32(&c->code,fw(OP_POPZ,0,0));
  return 1;
}

static int parse_statement(Compiler *c){
  if(c->lex.tok.kind==TOK_EOF) return 1;
  if(eat(c,";")) return 1;
  if(is_id(c,"function") && function_shape_at(c->lex.src,c->lex.tok.start)) return parse_function_value(c,0);
  if(is_id(c,"var")) return parse_var_decl(c);
  if(is_id(c,"if")) return parse_if(c);
  if(is_id(c,"while")) return parse_while(c);
  if(is_id(c,"for")) return parse_for(c);
  if(is_id(c,"repeat")) return parse_repeat(c);
  if(is_id(c,"switch")) return parse_switch(c);
  if(is_id(c,"with")) return parse_with(c);
  if(is_id(c,"return")) return parse_return_stmt(c);
  if(is_id(c,"exit")){ lx_next(&c->lex); emit_u32(&c->code,fw(OP_EXIT,0,0)); eat(c,";"); return 1; }
  if(is_id(c,"break")){ lx_next(&c->lex); if(!emit_break_branch(c)) return 0; eat(c,";"); return 1; }
  if(is_id(c,"continue")){
    if(c->continue_depth<=0){
      c->unsupported=1;
      snprintf(c->lex.err,sizeof(c->lex.err),"continue outside loop");
      return 0;
    }
    lx_next(&c->lex);
    if(!emit_continue_branch(c)) return 0;
    eat(c,";");
    return 1;
  }
  if(tok_is(c,"{")) return parse_block_or_stmt(c);
  return parse_simple_or_assign(c);
}

static int collect_macros(Compiler *c){
  for(int i=0;i<c->project->n_scripts;i++){
    char *txt=read_text(c->project->scripts[i].source_path);
    if(!txt) continue;
    const char *p=txt;
    while((p=strstr(p,"#macro"))){
      p+=6;
      while(*p==' '||*p=='\t') p++;
      const char *ns=p;
      while(isalnum((unsigned char)*p)||*p=='_') p++;
      if(p==ns) continue;
      char *name=dup_range(ns,(size_t)(p-ns));
      while(*p==' '||*p=='\t') p++;
      char *end=NULL;
      double v=strtod(p,&end);
      if(end!=p && name) add_macro(c,name,v);
      free(name);
    }
    free(txt);
  }
  return 1;
}

static int registry_extra_so_far(const GmlcFunctionRegistry *r){
  int n=0;
  for(int i=0;i<r->n_defs;i++) if(!r->defs[i].is_script_wrapper) n++;
  return n;
}

int gmlc_function_registry_extra_count(const GmlcFunctionRegistry *r){
  return r ? registry_extra_so_far(r) : 0;
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
  if(src[pos]=='"'){
    pos++;
    while(pos<end && src[pos]){
      if(src[pos]=='\\' && pos+1<end){ pos+=2; continue; }
      if(src[pos]=='"'){ pos++; break; }
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

static int detect_unsupported_function_capture(const char *src, size_t len, Span params, Span body, char *capture, size_t cap){
  NameList outer={0}, own={0};
  Span full={0,len};
  int rc=-1;
  if(!collect_var_decls(src,full,1,&outer)) goto done;
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
  size_t len=strlen(src);
  size_t pos=0;
  while(src[pos]){
    if(src[pos]=='"'){
      pos++;
      while(src[pos]){
        if(src[pos]=='\\' && src[pos+1]){ pos+=2; continue; }
        if(src[pos]=='"'){ pos++; break; }
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
        int cap=detect_unsupported_function_capture(src,len,params,body,capture,sizeof(capture));
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
        pos=end;
        continue;
      }
    }
    pos++;
  }
  return 1;
}

int gmlc_bytecode_collect_functions(const GmlcProject *project, int appended_base, GmlcFunctionRegistry *out, char *err, size_t errcap){
  memset(out,0,sizeof(*out));
  for(int i=0;i<project->n_rooms;i++){
    const char *path=project->rooms[i].creation_code_path;
    if(path && *path){
      char *txt=read_text(path);
      if(txt){ int ok=collect_functions_from_text(out,path,NULL,-1,appended_base,txt,err,errcap); free(txt); if(!ok) goto fail; }
    }
  }
  for(int i=0;i<project->n_scripts;i++){
    const char *path=project->scripts[i].source_path;
    if(path && *path){
      char *txt=read_text(path);
      if(txt){
        int ci=source_room_code_count(project)+i;
        int ok=collect_functions_from_text(out,path,project->scripts[i].name,ci,appended_base,txt,err,errcap);
        free(txt);
        if(!ok) goto fail;
      }
    }
  }
  for(int oi=0;oi<project->n_objects;oi++){
    const GmlcObject *obj=&project->objects[oi];
    for(int ei=0;ei<obj->n_events;ei++){
      const char *path=obj->events[ei].source_path;
      if(path && *path){
        char *txt=read_text(path);
        if(txt){ int ok=collect_functions_from_text(out,path,NULL,-1,appended_base,txt,err,errcap); free(txt); if(!ok) goto fail; }
      }
    }
  }
  for(int ri=0;ri<project->n_rooms;ri++){
    const GmlcRoom *room=&project->rooms[ri];
    for(int ii=0;ii<room->n_instances;ii++){
      const char *path=room->instances[ii].creation_code_path;
      if(path && *path){
        char *txt=read_text(path);
        if(txt){ int ok=collect_functions_from_text(out,path,NULL,-1,appended_base,txt,err,errcap); free(txt); if(!ok) goto fail; }
      }
    }
  }
  return 1;
fail:
  if(!err[0]) snprintf(err,errcap,"function registry allocation failed");
  gmlc_function_registry_free(out);
  return 0;
}

static const GmlcFunctionDef *find_script_wrapper(const GmlcFunctionRegistry *funcs, const char *path, int script_index){
  if(!funcs || !path || script_index<0) return NULL;
  for(int i=0;i<funcs->n_defs;i++){
    const GmlcFunctionDef *d=&funcs->defs[i];
    if(d->is_script_wrapper && d->source_path && !strcmp(d->source_path,path))
      return d;
  }
  return NULL;
}

static int emit_param_prologue(Compiler *c, const char *params){
  if(!params || !*params) return 1;
  size_t len=strlen(params), pos=0;
  int arg=0;
  while(pos<len){
    while(pos<len && (isspace((unsigned char)params[pos]) || params[pos]==',')) pos++;
    if(pos>=len) break;
    if(!(isalpha((unsigned char)params[pos]) || params[pos]=='_')){
      c->unsupported=1;
      snprintf(c->lex.err,sizeof(c->lex.err),"unsupported function parameter");
      return 0;
    }
    size_t ns=pos;
    pos++;
    while(pos<len && (isalnum((unsigned char)params[pos]) || params[pos]=='_')) pos++;
    char *name=dup_range(params+ns,pos-ns);
    if(!name) return 0;
    if(!add_local(c,name)){ free(name); return 0; }
    char argname[32];
    snprintf(argname,sizeof(argname),"argument%d",arg++);
    int ok=emit_push_var(c,IT_LOCAL,argname,0xA0) && emit_pop_var(c,IT_LOCAL,name,0xA0,DT_VAR);
    free(name);
    if(!ok) return 0;
    while(pos<len && isspace((unsigned char)params[pos])) pos++;
    if(pos<len && params[pos]==',') pos++;
    else if(pos<len){
      c->unsupported=1;
      snprintf(c->lex.err,sizeof(c->lex.err),"unsupported function parameter list");
      return 0;
    }
  }
  return 1;
}

static int compile_text_internal(const GmlcProject *project, const GmlcFunctionRegistry *funcs, int script_index, const char *source_path, const char *text, const char *params, GmlcCodeBlob *out, char *err, size_t errcap){
  Compiler c;
  memset(&c,0,sizeof(c));
  c.project=project;
  c.funcs=funcs;
  c.source_path=source_path;
  c.script_index=script_index;
  collect_macros(&c);
  c.lex.src=text;
  lx_next(&c.lex);
  if(!emit_param_prologue(&c,params)){
    c.unsupported=1;
  }
  while(c.lex.tok.kind!=TOK_EOF && !c.unsupported){
    if(!parse_statement(&c)) break;
  }
  if(!c.unsupported && c.lex.tok.kind==TOK_EOF){
    emit_u32(&c.code,fw(OP_EXIT,0,0));
    out->data=c.code.data;
    out->size=c.code.len;
    out->refs=c.refs;
    out->n_refs=c.n_refs;
    out->cap_refs=c.cap_refs;
    out->strings=c.strings;
    out->n_strings=c.n_strings;
    out->cap_strings=c.cap_strings;
    c.code.data=NULL; c.refs=NULL; c.n_refs=c.cap_refs=0;
    c.strings=NULL; c.n_strings=c.cap_strings=0;
  } else {
    gmlc_bytecode_emit_empty(out);
    out->diagnostic=gmlc_strdup(c.lex.err[0]?c.lex.err:"unsupported source construct");
  }
  for(int i=0;i<c.n_macros;i++) free(c.macros[i].name);
  free(c.macros);
  for(int i=0;i<c.n_locals;i++) free(c.locals[i]);
  free(c.locals);
  free(c.break_sites);
  free(c.continue_sites);
  for(int i=0;i<c.n_refs;i++) free(c.refs[i].name);
  free(c.refs);
  for(int i=0;i<c.n_strings;i++) free(c.strings[i].value);
  free(c.strings);
  free(c.code.data);
  if(out->data) return 1;
  snprintf(err,errcap,"%s: compile failed",source_path?source_path:"<source>");
  return 0;
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
  char *txt=read_text(path);
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

void gmlc_function_registry_free(GmlcFunctionRegistry *r){
  if(!r) return;
  for(int i=0;i<r->n_defs;i++){
    free(r->defs[i].name);
    free(r->defs[i].params);
    free(r->defs[i].body);
    free(r->defs[i].source_path);
  }
  free(r->defs);
  memset(r,0,sizeof(*r));
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
