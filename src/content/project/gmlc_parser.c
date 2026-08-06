/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_bytecode_internal.h"

#include "anygm_host.h"
#include "gml_win.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tok_is(Compiler *c, const char *s){
  return c->lex.tok.kind==TOK_SYM && !strcmp(c->lex.tok.text,s);
}
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

static int global_index(Compiler *c, const char *name){
  for(int i=0;i<c->n_globals;i++) if(!strcmp(c->globals[i],name)) return i;
  if(c->funcs) for(int i=0;i<c->funcs->n_globals;i++) if(!strcmp(c->funcs->globals[i],name)) return i;
  return -1;
}

static int add_global(Compiler *c, const char *name){
  if(global_index(c,name)>=0) return 1;
  if(c->n_globals>=c->cap_globals){
    int nc=c->cap_globals?c->cap_globals*2:16;
    char **ng=(char**)realloc(c->globals,(size_t)nc*sizeof(*ng));
    if(!ng) return 0;
    c->globals=ng; c->cap_globals=nc;
  }
  c->globals[c->n_globals++]=gmlc_strdup(name);
  return c->globals[c->n_globals-1]!=NULL;
}

static int plain_scope(Compiler *c, const char *name){
  if(local_index(c,name)>=0 || !strncmp(name,"argument",8)) return IT_LOCAL;
  if(global_index(c,name)>=0) return IT_GLOBAL;
  return IT_SELF;
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
  if(c->funcs && c->funcs->assets){
    int lo=0,hi=c->funcs->n_assets;
    while(lo<hi){
      int mid=lo+(hi-lo)/2;
      int cmp=strcmp(c->funcs->assets[mid].name,name);
      if(cmp<0) lo=mid+1; else hi=mid;
    }
    if(lo<c->funcs->n_assets && !strcmp(c->funcs->assets[lo].name,name)){
      *out=c->funcs->assets[lo].value;
      return 1;
    }
    return 0;
  }
  for(int i=0;i<c->project->n_sprites;i++) if(c->project->sprites[i].runtime_id>=0 &&
      c->project->sprites[i].name && !strcmp(c->project->sprites[i].name,name)){
    *out=gmlc_project_sprite_runtime_id(c->project,i);
    return 1;
  }
  for(int i=0;i<c->project->n_sounds;i++) if(c->project->sounds[i].name && !strcmp(c->project->sounds[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_objects;i++) if(c->project->objects[i].name && !strcmp(c->project->objects[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_rooms;i++) if(c->project->rooms[i].name && !strcmp(c->project->rooms[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_shaders;i++) if(c->project->shaders[i].name && !strcmp(c->project->shaders[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_fonts;i++) if(c->project->fonts[i].name && !strcmp(c->project->fonts[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_tilesets;i++) if(c->project->tilesets[i].name && !strcmp(c->project->tilesets[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_scripts;i++) if(c->project->scripts[i].name && !strcmp(c->project->scripts[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_paths;i++) if(c->project->paths[i].name && !strcmp(c->project->paths[i].name,name)){ *out=i; return 1; }
  for(int i=0;i<c->project->n_timelines;i++) if(c->project->timelines[i].name && !strcmp(c->project->timelines[i].name,name)){ *out=i; return 1; }
  return 0;
}

static int resolve_script_code_index(Compiler *c, const char *name, int *out){
  if(c->funcs && c->funcs->assets){
    int lo=0,hi=c->funcs->n_assets;
    while(lo<hi){
      int mid=lo+(hi-lo)/2;
      int cmp=strcmp(c->funcs->assets[mid].name,name);
      if(cmp<0) lo=mid+1; else hi=mid;
    }
    if(lo<c->funcs->n_assets && !strcmp(c->funcs->assets[lo].name,name) &&
       c->funcs->assets[lo].script_code_index>=0){
      *out=c->funcs->assets[lo].script_code_index;
      return 1;
    }
    return 0;
  }
  for(int i=0;i<c->project->n_scripts;i++){
    if(c->project->scripts[i].name && !strcmp(c->project->scripts[i].name,name)){
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
  if(!strcmp(name,"c_navy")){ *out=0x800000; return 1; }
  if(!strcmp(name,"c_blue")){ *out=0xFF0000; return 1; }
  if(!strcmp(name,"c_teal")){ *out=0x808000; return 1; }
  if(!strcmp(name,"c_aqua")){ *out=0xFFFF00; return 1; }
  if(!strcmp(name,"c_olive")){ *out=0x008080; return 1; }
  if(!strcmp(name,"c_yellow")){ *out=0x00FFFF; return 1; }
  if(!strcmp(name,"c_purple")){ *out=0x800080; return 1; }
  if(!strcmp(name,"c_fuchsia")){ *out=0xFF00FF; return 1; }
  if(!strcmp(name,"c_gray") || !strcmp(name,"c_grey")){ *out=0x808080; return 1; }
  if(!strcmp(name,"c_ltgray") || !strcmp(name,"c_ltgrey") || !strcmp(name,"c_silver")){ *out=0xC0C0C0; return 1; }
  if(!strcmp(name,"c_dkgray") || !strcmp(name,"c_dkgrey")){ *out=0x404040; return 1; }
  if(!strcmp(name,"c_orange")){ *out=0x40A0FF; return 1; }
  if(!strcmp(name,"c_white")){ *out=16777215; return 1; }
  if(!strcmp(name,"c_lime")){ *out=65280; return 1; }
  static const struct { const char *name; int value; } mouse_buttons[]={
    {"mb_any",-1}, {"mb_none",0}, {"mb_left",1}, {"mb_right",2}, {"mb_middle",3}
  };
  for(int i=0;i<(int)(sizeof(mouse_buttons)/sizeof(*mouse_buttons));i++)
    if(!strcmp(name,mouse_buttons[i].name)){ *out=mouse_buttons[i].value; return 1; }
  if(!strcmp(name,"bm_normal")){ *out=0; return 1; }
  if(!strcmp(name,"bm_subtract")){ *out=3; return 1; }
  static const char *effect_kinds[]={
    "ef_explosion","ef_ring","ef_ellipse","ef_firework","ef_smoke","ef_smokeup",
    "ef_star","ef_spark","ef_flare","ef_cloud","ef_rain","ef_snow"
  };
  for(int i=0;i<(int)(sizeof(effect_kinds)/sizeof(*effect_kinds));i++)
    if(!strcmp(name,effect_kinds[i])){ *out=i; return 1; }
  if(!strcmp(name,"pi")){ *out=M_PI; return 1; }
  if(!strcmp(name,"fa_left") || !strcmp(name,"fa_top")){ *out=0; return 1; }
  if(!strcmp(name,"fa_center") || !strcmp(name,"fa_middle")){ *out=1; return 1; }
  if(!strcmp(name,"fa_right") || !strcmp(name,"fa_bottom")){ *out=2; return 1; }
  if(!strcmp(name,"ps_shape_rectangle") || !strcmp(name,"ps_distr_linear")){ *out=0; return 1; }
  if(!strcmp(name,"ps_shape_ellipse") || !strcmp(name,"ps_distr_gaussian")){ *out=1; return 1; }
  if(!strcmp(name,"ps_shape_diamond") || !strcmp(name,"ps_distr_invgaussian")){ *out=2; return 1; }
  if(!strcmp(name,"ps_shape_line")){ *out=3; return 1; }
  static const char *particle_shapes[]={
    "pt_shape_pixel","pt_shape_disk","pt_shape_square","pt_shape_line","pt_shape_star",
    "pt_shape_circle","pt_shape_ring","pt_shape_sphere","pt_shape_flare","pt_shape_spark",
    "pt_shape_explosion","pt_shape_cloud","pt_shape_smoke","pt_shape_snow"
  };
  for(int i=0;i<(int)(sizeof(particle_shapes)/sizeof(*particle_shapes));i++)
    if(!strcmp(name,particle_shapes[i])){ *out=i; return 1; }
  /* Classic projects encode Win32 virtual-key values. Keep the
   * complete named-key family here: an unresolved constant otherwise becomes an instance
   * variable and silently reads as zero, which is especially hard to spot in modifier checks. */
  static const struct { const char *name; int value; } virtual_keys[]={
    {"vk_nokey",0}, {"vk_anykey",1},
    {"vk_backspace",8}, {"vk_tab",9}, {"vk_enter",13}, {"vk_return",13},
    {"vk_shift",16}, {"vk_control",17}, {"vk_alt",18}, {"vk_pause",19},
    {"vk_escape",27}, {"vk_space",32}, {"vk_pageup",33}, {"vk_pagedown",34},
    {"vk_end",35}, {"vk_home",36}, {"vk_left",37}, {"vk_up",38},
    {"vk_right",39}, {"vk_down",40}, {"vk_printscreen",44},
    {"vk_insert",45}, {"vk_delete",46},
    {"vk_numpad0",96}, {"vk_numpad1",97}, {"vk_numpad2",98},
    {"vk_numpad3",99}, {"vk_numpad4",100}, {"vk_numpad5",101},
    {"vk_numpad6",102}, {"vk_numpad7",103}, {"vk_numpad8",104},
    {"vk_numpad9",105}, {"vk_multiply",106}, {"vk_add",107},
    {"vk_subtract",109}, {"vk_decimal",110}, {"vk_divide",111},
    {"vk_f1",112}, {"vk_f2",113}, {"vk_f3",114}, {"vk_f4",115},
    {"vk_f5",116}, {"vk_f6",117}, {"vk_f7",118}, {"vk_f8",119},
    {"vk_f9",120}, {"vk_f10",121}, {"vk_f11",122}, {"vk_f12",123},
    {"vk_lshift",160}, {"vk_rshift",161},
    {"vk_lcontrol",162}, {"vk_rcontrol",163},
    {"vk_lalt",164}, {"vk_ralt",165}
  };
  for(int i=0;i<(int)(sizeof(virtual_keys)/sizeof(*virtual_keys));i++)
    if(!strcmp(name,virtual_keys[i].name)){ *out=virtual_keys[i].value; return 1; }
  static const struct { const char *name; int value; } event_constants[]={
    {"ev_create",0}, {"ev_destroy",1}, {"ev_alarm",2}, {"ev_step",3},
    {"ev_collision",4}, {"ev_keyboard",5}, {"ev_mouse",6}, {"ev_other",7},
    {"ev_draw",8}, {"ev_keypress",9}, {"ev_keyrelease",10}, {"ev_trigger",11},
    {"ev_cleanup",12},
    {"ev_step_normal",0}, {"ev_step_begin",1}, {"ev_step_end",2},
    {"ev_draw_normal",0},
    {"ev_user0",10}, {"ev_user1",11}, {"ev_user2",12}, {"ev_user3",13},
    {"ev_user4",14}, {"ev_user5",15}, {"ev_user6",16}, {"ev_user7",17},
    {"ev_user8",18}, {"ev_user9",19}, {"ev_user10",20}, {"ev_user11",21},
    {"ev_user12",22}, {"ev_user13",23}, {"ev_user14",24}, {"ev_user15",25}
  };
  for(int i=0;i<(int)(sizeof(event_constants)/sizeof(*event_constants));i++)
    if(!strcmp(name,event_constants[i].name)){ *out=event_constants[i].value; return 1; }
  if(!strcmp(name,"gp_face1")){ *out=32769; return 1; }
  if(!strcmp(name,"gp_face2")){ *out=32770; return 1; }
  if(!strcmp(name,"gp_face3")){ *out=32771; return 1; }
  if(!strcmp(name,"gp_face4")){ *out=32772; return 1; }
  if(!strcmp(name,"gp_start")){ *out=32778; return 1; }
  if(!strcmp(name,"gp_shoulderr")){ *out=32774; return 1; }
  if(!strcmp(name,"gp_padr")){ *out=32784; return 1; }
  if(!strcmp(name,"gp_padl")){ *out=32783; return 1; }
  if(!strcmp(name,"gp_padu")){ *out=32781; return 1; }
  if(!strcmp(name,"gp_padd")){ *out=32782; return 1; }
  if(!strcmp(name,"gp_axislh")){ *out=32785; return 1; }
  if(!strcmp(name,"gp_axislv")){ *out=32786; return 1; }
  if(macro_value(c,name,out) || resolve_asset(c,name,out)) return 1;
  if(c->funcs){
    for(int i=c->funcs->n_constants-1;i>=0;i--) if(!strcmp(c->funcs->constant_names[i],name)){
      const char *expr=c->funcs->constant_exprs[i];
      char *end=NULL;
      double value=strtod(expr,&end);
      while(end && isspace((unsigned char)*end)) end++;
      if(end && end!=expr && !*end){ *out=value; return 1; }
      break;
    }
  }
  return 0;
}

static int parse_expr(Compiler *c);
static int parse_statement(Compiler *c);

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

static int emit_popz(Compiler *c);

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

static int scan_comma_spans_until(Compiler *c, size_t start, char close_ch, Span **out_spans, int *out_n, size_t *out_close){
  const char *src=c->lex.src;
  Span *spans=NULL;
  int n=0, cap=0, depth=0;
  size_t pos=start, seg=start;
  while(src[pos]){
    char ch=src[pos];
    if(ch=='"' || ch=='\''){
      char quote=ch;
      pos++;
      while(src[pos]){
        if(src[pos]=='\\' && src[pos+1]){ pos+=2; continue; }
        if(src[pos]==quote){ pos++; break; }
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
    if(ch==close_ch){
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
  snprintf(c->lex.err,sizeof(c->lex.err),"unterminated delimited list");
  return 0;
}

static int scan_comma_spans(Compiler *c, size_t start, Span **out_spans, int *out_n, size_t *out_close){
  return scan_comma_spans_until(c,start,')',out_spans,out_n,out_close);
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

static int parse_array_literal(Compiler *c){
  size_t content_start=c->lex.tok.end;
  Span *spans=NULL;
  int n=0;
  size_t close_pos=0;
  if(!scan_comma_spans_until(c,content_start,']',&spans,&n,&close_pos)) return 0;
  char temp[64];
  snprintf(temp,sizeof(temp),"@@array@@%d",c->array_depth++);
  int ok=emit_push_i32_full(c,n) &&
         emit_call(c,"array_create",1) &&
         emit_pop_var(c,IT_LOCAL,temp,0xA0,DT_VAR);
  for(int i=0;ok && i<n;i++){
    ok=compile_expr_slice(c,c->lex.src+spans[i].start,spans[i].end-spans[i].start) &&
       emit_push_i32_full(c,i) &&
       emit_push_var(c,IT_LOCAL,temp,0xA0) &&
       emit_call(c,"array_set",3) &&
       emit_popz(c);
  }
  if(ok) ok=emit_push_var(c,IT_LOCAL,temp,0xA0);
  c->array_depth--;
  free(spans);
  if(!ok) return 0;
  c->lex.pos=close_pos+1;
  lx_next(&c->lex);
  expr_not_const(c);
  c->expr_boolish=0;
  return 1;
}

static int scan_square_span(Compiler *c, size_t start, Span *out, size_t *out_close){
  const char *src=c->lex.src;
  int depth=0;
  size_t pos=start;
  while(src[pos]){
    char ch=src[pos];
    if(ch=='"' || ch=='\''){
      char quote=ch;
      pos++;
      while(src[pos]){
        if(src[pos]=='\\' && src[pos+1]){ pos+=2; continue; }
        if(src[pos]==quote){ pos++; break; }
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
    if(ch=='"' || ch=='\''){
      char quote=ch;
      pos++;
      while(pos<s.end){
        if(src[pos]=='\\' && pos+1<s.end){ pos++; pos++; continue; }
        if(src[pos]==quote) break;
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

static size_t skip_bounded_space(const char *src, size_t pos, size_t end){
  while(pos<end && isspace((unsigned char)src[pos])) pos++;
  return pos;
}

static int classic_for_assignment_at(const char *src, size_t pos, size_t end){
  pos=skip_bounded_space(src,pos,end);
  if(pos>=end || !(isalpha((unsigned char)src[pos]) || src[pos]=='_')) return 0;
  pos++;
  while(pos<end && (isalnum((unsigned char)src[pos]) || src[pos]=='_')) pos++;
  for(;;){
    pos=skip_bounded_space(src,pos,end);
    if(pos<end && src[pos]=='.'){
      pos=skip_bounded_space(src,pos+1,end);
      if(pos>=end || !(isalpha((unsigned char)src[pos]) || src[pos]=='_')) return 0;
      pos++;
      while(pos<end && (isalnum((unsigned char)src[pos]) || src[pos]=='_')) pos++;
      continue;
    }
    if(pos<end && src[pos]=='['){
      int depth=1;
      pos++;
      while(pos<end && depth){
        if(src[pos]=='"' || src[pos]=='\''){
          char quote=src[pos++];
          while(pos<end){
            if(src[pos]=='\\' && pos+1<end){ pos+=2; continue; }
            if(src[pos++]==quote) break;
          }
          continue;
        }
        if(src[pos]=='[') depth++;
        else if(src[pos]==']') depth--;
        pos++;
      }
      if(depth) return 0;
      continue;
    }
    break;
  }
  pos=skip_bounded_space(src,pos,end);
  if(pos>=end) return 0;
  if((src[pos]=='+' || src[pos]=='-') && pos+1<end && src[pos+1]==src[pos]) return 1;
  if(strchr("+-*/%",src[pos]) && pos+1<end && src[pos+1]=='=') return 1;
  return src[pos]=='=' && (pos+1>=end || src[pos+1]!='=');
}

/* Classic source may omit the second semicolon in a for header. Recover only when
 * the trailing top-level term has the shape of an assignment or update, keeping modern source
 * strict and avoiding guesses inside calls, array indices, strings or comments. */
/* GML lets a for header separate its clauses with nothing but whitespace, and classic content is
 * commonly written that way. Two clauses meet where a complete term is followed by the start of
 * another with no operator between them. A word operator spelled as a name — and, or, div — joins
 * terms rather than separating clauses, so neither side of the gap may be one. */
static int classic_for_word_operator(const char *src, size_t start, size_t end){
  static const char *const words[]={"and","or","xor","not","div","mod"};
  for(size_t i=0;i<sizeof words/sizeof words[0];i++){
    size_t length=strlen(words[i]);
    if(start+length<=end && !strncmp(src+start,words[i],length) &&
       (start+length==end ||
        !(isalnum((unsigned char)src[start+length]) || src[start+length]=='_')))
      return 1;
  }
  return 0;
}
static int classic_for_clause_boundary(const char *src, size_t gap, Span combined){
  size_t before=gap;
  while(before>combined.start && isspace((unsigned char)src[before-1])) before--;
  if(before==combined.start) return 0;
  char last=src[before-1];
  if(!(isalnum((unsigned char)last) || last=='_' || last==')' || last==']' ||
       last=='"' || last=='\'')) return 0;
  size_t word=before;
  while(word>combined.start && (isalnum((unsigned char)src[word-1]) || src[word-1]=='_')) word--;
  if(word<before && classic_for_word_operator(src,word,before)) return 0;
  size_t after=skip_bounded_space(src,gap,combined.end);
  if(after>=combined.end) return 0;
  char next=src[after];
  if(!(isalpha((unsigned char)next) || next=='_' || isdigit((unsigned char)next) || next=='('))
    return 0;
  return !classic_for_word_operator(src,after,combined.end);
}
static int split_classic_for_clauses(const char *src, Span combined, Span *out){
  size_t split[2]; int found=0;
  int depth=0;
  for(size_t pos=combined.start;pos<combined.end && found<2;pos++){
    char ch=src[pos];
    if(ch=='"' || ch=='\''){
      char quote=ch;
      pos++;
      while(pos<combined.end){
        if(src[pos]=='\\' && pos+1<combined.end){ pos+=2; continue; }
        if(src[pos]==quote) break;
        pos++;
      }
      continue;
    }
    if(ch=='(' || ch=='[' || ch=='{'){ depth++; continue; }
    if((ch==')' || ch==']' || ch=='}') && depth>0){ depth--; continue; }
    if(depth || !isspace((unsigned char)ch)) continue;
    if(classic_for_clause_boundary(src,pos,combined)) split[found++]=pos;
  }
  if(found!=2) return 0;
  out[0].start=combined.start; out[0].end=split[0];
  out[1].start=split[0];       out[1].end=split[1];
  out[2].start=split[1];       out[2].end=combined.end;
  for(int i=0;i<3;i++) trim_span(src,&out[i]);
  return out[0].start<out[0].end && out[1].start<out[1].end && out[2].start<out[2].end;
}
static int split_classic_for_condition_step(const char *src, Span combined,
                                            Span *condition, Span *step){
  int depth=0;
  size_t split=(size_t)-1;
  for(size_t pos=combined.start;pos<combined.end;pos++){
    char ch=src[pos];
    if(ch=='"' || ch=='\''){
      char quote=ch;
      pos++;
      while(pos<combined.end){
        if(src[pos]=='\\' && pos+1<combined.end){ pos+=2; continue; }
        if(src[pos]==quote) break;
        pos++;
      }
      continue;
    }
    if(ch=='/' && pos+1<combined.end && src[pos+1]=='/'){
      pos+=2;
      while(pos<combined.end && src[pos]!='\n') pos++;
      continue;
    }
    if(ch=='/' && pos+1<combined.end && src[pos+1]=='*'){
      pos+=2;
      while(pos+1<combined.end && !(src[pos]=='*' && src[pos+1]=='/')) pos++;
      if(pos+1<combined.end) pos++;
      continue;
    }
    if(ch=='(' || ch=='[' || ch=='{'){ depth++; continue; }
    if((ch==')' || ch==']' || ch=='}') && depth>0){ depth--; continue; }
    if(depth==0 && isspace((unsigned char)ch)){
      size_t candidate=skip_bounded_space(src,pos,combined.end);
      if(candidate<combined.end && classic_for_assignment_at(src,candidate,combined.end))
        split=candidate;
    }
  }
  if(split==(size_t)-1) return 0;
  condition->start=combined.start;
  condition->end=split;
  step->start=split;
  step->end=combined.end;
  trim_span(src,condition);
  trim_span(src,step);
  return condition->start<condition->end && step->start<step->end;
}

static int scan_for_header(Compiler *c, size_t start, Span out[3], size_t *out_close){
  const char *src=c->lex.src;
  int depth=0, part=0;
  size_t pos=start, seg=start;
  memset(out,0,3*sizeof(*out));
  while(src[pos]){
    char ch=src[pos];
    if(ch=='"' || ch=='\''){
      char quote=ch;
      pos++;
      while(src[pos]){
        if(src[pos]=='\\' && src[pos+1]){ pos+=2; continue; }
        if(src[pos]==quote){ pos++; break; }
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
        if(part==2) return 1;
        if(part==1 && c->project && c->project->classic_version>0){
          Span combined=out[1];
          if(split_classic_for_condition_step(src,combined,&out[1],&out[2])) return 1;
        }
        if(part==0 && c->project && c->project->classic_version>0){
          Span combined=out[0];
          if(split_classic_for_clauses(src,combined,out)) return 1;
        }
        c->unsupported=1;
        snprintf(c->lex.err,sizeof(c->lex.err),"for header requires three clauses");
        return 0;
      }
      depth--; pos++; continue;
    }
    if((ch==']' || ch=='}') && depth>0){ depth--; pos++; continue; }
    if(ch==';' && depth==0){
      if(part>=2){
        out[part].start=seg; out[part].end=pos; trim_span(src,&out[part]);
        pos=skip_ws_comments_at(src,pos+1);
        if(src[pos]==')'){
          *out_close=pos;
          return 1;
        }
        break;
      }
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
    if(ch=='"' || ch=='\''){
      char quote=ch;
      pos++;
      while(src[pos]){
        if(src[pos]=='\\' && src[pos+1]){ pos+=2; continue; }
        if(src[pos]==quote){ pos++; break; }
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
    if(ch=='"' || ch=='\''){
      char quote=ch;
      pos++;
      while(pos<end){
        if(src[pos]=='\\' && pos+1<end){ pos++; pos++; continue; }
        if(src[pos]==quote) break;
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
    if(ch=='"' || ch=='\''){
      char quote=ch;
      pos++;
      while(pos<body.end){
        if(src[pos]=='\\' && pos+1<body.end){ pos+=2; continue; }
        if(src[pos]==quote){ pos++; break; }
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
  return emit_push_var(c,plain_scope(c,name),name,0xA0);
}

typedef struct {
  char name[128];
  char receiver[128];
  int inst;
  uint8_t reftype;
  int is_stacktop;
  int receiver_on_stack;
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

static int emit_array_2d_index_cast(Compiler *c, Span s);

static int emit_lvalue_address(Compiler *c, LValue *lv){
  if(lv->is_stacktop){
    if(!lv->receiver_on_stack){
      if(!emit_receiver_value(c,lv->receiver)) return 0;
      if(!emit_conv(c,DT_VAR,DT_INT32)) return 0;
    }
  }
  else if(lv->is_array){
    if(!emit_push_real(c,lv->inst)) return 0;
  }
  if(lv->is_array){
    if(!lv->index_src) return 0;
    if(!compile_expr_slice(c,lv->index_src+lv->index_span.start,lv->index_span.end-lv->index_span.start)) return 0;
    if(lv->is_array_2d){
      if(!emit_array_2d_index_cast(c,lv->index_span)) return 0;
      if(!emit_push_i32_full(c,32000) || !emit_binary_typed(c,OP_MUL,DT_INT32,DT_INT32)) return 0;
      if(!compile_expr_slice(c,lv->index_src+lv->index2_span.start,lv->index2_span.end-lv->index2_span.start)) return 0;
      if(!emit_array_2d_index_cast(c,lv->index2_span)) return 0;
      if(!emit_binary_typed(c,OP_ADD,DT_INT32,DT_INT32)) return 0;
    }
  }
  return 1;
}

static int span_is_integer_literal(const char *src, Span s){
  trim_span(src,&s);
  if(s.start>=s.end) return 0;
  size_t p=s.start;
  if(src[p]=='+' || src[p]=='-') p++;
  if(p>=s.end) return 0;
  for(;p<s.end;p++) if(!isdigit((unsigned char)src[p])) return 0;
  return 1;
}

static int emit_array_2d_index_cast(Compiler *c, Span s){
  if(!span_is_integer_literal(c->lex.src,s) && !emit_conv(c,DT_VAR,DT_INT32)) return 0;
  return emit_u32(&c->code,fw(OP_BREAK,DT_INT16,-1));
}

static int parse_lvalue_from_name(Compiler *c, const char *first, LValue *lv);
static int emit_lvalue_read(Compiler *c, LValue *lv);
static int emit_lvalue_write(Compiler *c, LValue *lv, uint8_t type1);
static int emit_popz(Compiler *c){ return emit_u32(&c->code,fw(OP_POPZ,DT_VAR,0)); }
static int emit_popz_typed(Compiler *c, uint8_t type1){ return emit_u32(&c->code,fw(OP_POPZ,type1,0)); }
static int emit_dup(Compiler *c, uint8_t type1){ return emit_u32(&c->code,fw(OP_DUP,type1,0)); }
/* Swap the two one-value blocks at the top of the stack.  The low count is
 * one here: zero is the no-argument receiver shuffle used by member calls and
 * leaves [receiver,value] unchanged, so a following StackTop store consumes
 * the value as though it were the receiver. */
static int emit_swap_top(Compiler *c){ return emit_u32(&c->code,fw(OP_DUP,DT_VAR,(int16_t)0x8801)); }

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

/* Compile postfix operations whose receiver is already on the value stack. Classic
 * GML commonly dereferences a value returned by a call, for example
 * `(instance_place(x,y,obj)).object_index`.  Named receivers use LValue below;
 * this path deliberately handles expression receivers without reconstructing or
 * re-evaluating the expression. */
static int parse_value_postfix(Compiler *c){
  for(;;){
    if(eat(c,".")){
      if(c->lex.tok.kind!=TOK_ID){
        c->unsupported=1;
        snprintf(c->lex.err,sizeof(c->lex.err),"expected field name");
        return 0;
      }
      char field[128];
      snprintf(field,sizeof(field),"%s",c->lex.tok.text);
      lx_next(&c->lex);
      if(!emit_conv(c,DT_VAR,DT_INT32) || !emit_push_var(c,0,field,0x80)) return 0;
      expr_not_const(c);
      c->expr_boolish=0;
      continue;
    }
    if(tok_is(c,"(")){
      size_t gap=c->lex.tok.start;
      int line_break=0;
      while(gap>0 && isspace((unsigned char)c->lex.src[gap-1])){
        if(c->lex.src[gap-1]=='\n' || c->lex.src[gap-1]=='\r') line_break=1;
        gap--;
      }
      if(line_break) return 1;
      lx_next(&c->lex);
      int argc=0;
      if(!parse_call_args_reversed(c,&argc) || !emit_callv(c,argc)) return 0;
      expr_not_const(c);
      c->expr_boolish=0;
      continue;
    }
    return 1;
  }
}

/* Classic project constants are textual GML expressions, not merely numeric
 * defines. Compile the expression at each use so strings, asset references and
 * references to other project constants retain their normal language meaning. */
static int emit_project_constant(Compiler *c, const char *name){
  if(!c->funcs) return 0;
  const char *expr=NULL;
  for(int i=c->funcs->n_constants-1;i>=0;i--)
    if(!strcmp(c->funcs->constant_names[i],name)){ expr=c->funcs->constant_exprs[i]; break; }
  if(!expr) return 0;
  if(c->constant_depth>=64){
    c->unsupported=1; snprintf(c->lex.err,sizeof(c->lex.err),"project constant expansion is too deep");
    return -1;
  }
  for(int i=0;i<c->constant_depth;i++) if(!strcmp(c->constant_stack[i],name)){
    c->unsupported=1; snprintf(c->lex.err,sizeof(c->lex.err),"cyclic project constant '%s'",name);
    return -1;
  }
  Lexer outer=c->lex, nested;
  memset(&nested,0,sizeof(nested)); nested.src=expr;
  c->constant_stack[c->constant_depth++]=name;
  c->lex=nested; lx_next(&c->lex);
  int ok=parse_expr(c);
  if(ok && c->lex.tok.kind!=TOK_EOF){
    c->unsupported=1;
    snprintf(c->lex.err,sizeof(c->lex.err),"trailing token in project constant '%s'",name);
    ok=0;
  }
  char nested_error[sizeof(c->lex.err)];
  snprintf(nested_error,sizeof(nested_error),"%s",c->lex.err);
  c->constant_depth--;
  c->lex=outer;
  if(!ok && nested_error[0]) snprintf(c->lex.err,sizeof(c->lex.err),"%s",nested_error);
  return ok?1:-1;
}

static int parse_primary(Compiler *c){
  if(c->lex.tok.kind==TOK_NUM){ double d=c->lex.tok.num; lx_next(&c->lex); return emit_const_number(c,d); }
  if(c->lex.tok.kind==TOK_STR){
    char *value=lx_string_value(&c->lex);
    if(!value) return 0;
    lx_next(&c->lex);
    expr_not_const(c);
    c->expr_boolish=0;
    int emitted=emit_push_string_literal(c,value);
    free(value);
    return emitted;
  }
  if(is_id(c,"function") && function_shape_at(c->lex.src,c->lex.tok.start)){
    if(!parse_function_value(c,1)) return 0;
    expr_not_const(c);
    c->expr_boolish=0;
    goto postfix_calls;
  }
  if(tok_is(c,"[")){
    if(!parse_array_literal(c)) return 0;
    goto postfix_calls;
  }
  if(eat(c,"(")){
    if(!parse_expr(c)) return 0;
    if(!need(c,")")) return 0;
    goto postfix_calls;
  }
  if(c->lex.tok.kind==TOK_ID){
    char name[128]; snprintf(name,sizeof(name),"%s",c->lex.tok.text); lx_next(&c->lex);
    if(eat(c,"(")){
      if(!strcmp(name,"ord") && c->lex.tok.kind==TOK_STR){
        unsigned char ch=(unsigned char)c->lex.tok.text[0];
        lx_next(&c->lex);
        if(!need(c,")")) return 0;
        if(!emit_const_number(c,(double)ch)) return 0;
        goto postfix_calls;
      }
      int argc=0;
      int scope=plain_scope(c,name);
      if(scope!=IT_SELF){
        if(!emit_push_var(c,scope,name,0xA0)) return 0;
        if(!parse_call_args_reversed(c,&argc)) return 0;
        if(!emit_callv(c,argc)) return 0;
      } else {
        if(!parse_call_args_reversed(c,&argc)) return 0;
        if(!emit_call(c,name,argc)) return 0;
      }
      expr_not_const(c);
      c->expr_boolish=0;
      goto postfix_calls;
    }
    if(tok_is(c,".") || tok_is(c,"[")){
      LValue lv;
      if(!parse_lvalue_from_name(c,name,&lv)) return 0;
      int ok=emit_lvalue_read(c,&lv);
      free(lv.index_src);
      if(!ok) return 0;
      expr_not_const(c);
      c->expr_boolish=0;
      goto postfix_calls;
    }
    int project_constant=emit_project_constant(c,name);
    if(project_constant<0) return 0;
    if(project_constant>0) goto postfix_calls;
    double cv=0;
    if(resolve_const(c,name,&cv)){
      if(!strcmp(name,"pi")){
        if(!emit_const_number(c,cv)) return 0;
        goto postfix_calls;
      }
      if(!emit_push_real(c,cv)) return 0;
      expr_not_const(c);
      c->expr_boolish=0;
      goto postfix_calls;
    }
    int sci=-1;
    if(resolve_function_code_index(c,name,&sci)){
      if(!emit_script_funcval(c,sci)) return 0;
      expr_not_const(c);
      c->expr_boolish=0;
      goto postfix_calls;
    }
    int inst=plain_scope(c,name);
    if(!emit_push_var(c,inst,name,0xA0)) return 0;
    expr_not_const(c);
    c->expr_boolish=0;
    goto postfix_calls;
  }
  c->unsupported=1;
  snprintf(c->lex.err,sizeof(c->lex.err),"unexpected expression token '%s'",c->lex.tok.text);
  return 0;

postfix_calls:
  return parse_value_postfix(c);
}

static int parse_unary(Compiler *c){
  if(eat(c,"+")) return parse_unary(c);
  if(tok_is(c,"!") || is_id(c,"not")){
    lx_next(&c->lex);
    if(!parse_unary(c) || !emit_conv(c,DT_VAR,DT_BOOL)) return 0;
    if(!emit_u32(&c->code,fw(OP_NOT,DT_BOOL,0))) return 0;
    expr_not_const(c);
    c->expr_boolish=1;
    return 1;
  }
  if(eat(c,"-")){
    if(!parse_unary(c)) return 0;
    if(c->expr_const){
      size_t start=c->expr_const_start;
      double v=-c->expr_const_value;
      c->code.len=start;
      return emit_const_number(c,v);
    }
    c->expr_boolish=0;
    expr_not_const(c);
    return emit_u32(&c->code,fw(OP_NEG,(uint8_t)((DT_VAR<<4)|DT_VAR),0));
  }
  return parse_primary(c);
}

static int parse_mul(Compiler *c){
  if(!parse_unary(c)) return 0;
  int left_const=c->expr_const;
  double left_val=c->expr_const_value;
  size_t left_start=c->expr_const_start;
  int did=0;
  while(tok_is(c,"*")||tok_is(c,"/")||tok_is(c,"%")||is_id(c,"div")||is_id(c,"mod")){
    int op=tok_is(c,"*")?OP_MUL:tok_is(c,"/")?OP_DIV:is_id(c,"div")?OP_REM:OP_MOD;
    lx_next(&c->lex);
    if(!parse_unary(c)) return 0;
    int right_const=c->expr_const;
    double right_val=c->expr_const_value;
    if(left_const && right_const && (op==OP_MUL || (op==OP_DIV && right_val!=0.0))){
      double v=(op==OP_MUL) ? left_val*right_val : left_val/right_val;
      c->code.len=left_start;
      if(!emit_const_number(c,v)) return 0;
      left_const=1;
      left_val=v;
      left_start=c->expr_const_start;
    } else {
      emit_binary(c,(uint8_t)op);
      expr_not_const(c);
      left_const=0;
    }
    did=1;
  }
  if(did) c->expr_boolish=0;
  return 1;
}

static int parse_add(Compiler *c){
  if(!parse_mul(c)) return 0;
  int left_const=c->expr_const;
  double left_val=c->expr_const_value;
  size_t left_start=c->expr_const_start;
  int did=0;
  while(tok_is(c,"+")||tok_is(c,"-")||tok_is(c,"++")){
    int sub=tok_is(c,"-"); lx_next(&c->lex);
    if(!parse_mul(c)) return 0;
    int right_const=c->expr_const;
    double right_val=c->expr_const_value;
    if(left_const && right_const){
      double v=sub ? left_val-right_val : left_val+right_val;
      c->code.len=left_start;
      if(!emit_const_number(c,v)) return 0;
      left_const=1;
      left_val=v;
      left_start=c->expr_const_start;
    } else {
      emit_binary(c,sub?OP_SUB:OP_ADD);
      expr_not_const(c);
      left_const=0;
    }
    did=1;
  }
  if(did) c->expr_boolish=0;
  return 1;
}

static int parse_shift(Compiler *c){
  if(!parse_add(c)) return 0;
  int did=0;
  while(tok_is(c,"<<")||tok_is(c,">>")){
    uint8_t op=tok_is(c,"<<") ? OP_SHL : OP_SHR;
    lx_next(&c->lex);
    if(!parse_add(c)) return 0;
    if(!emit_binary(c,op)) return 0;
    expr_not_const(c);
    did=1;
  }
  if(did) c->expr_boolish=0;
  return 1;
}

static int parse_cmp_expr(Compiler *c){
  if(!parse_shift(c)) return 0;
  int did=0;
  while(tok_is(c,"<")||tok_is(c,"<=")||tok_is(c,">")||tok_is(c,">=")){
    uint8_t cmp=tok_is(c,"<")?CMP_LT:tok_is(c,"<=")?CMP_LTE:tok_is(c,">")?CMP_GT:CMP_GTE;
    lx_next(&c->lex);
    if(!parse_shift(c)) return 0;
    emit_cmp(c,cmp);
    did=1;
  }
  if(did){ expr_not_const(c); c->expr_boolish=1; }
  return 1;
}

static int parse_eq(Compiler *c){
  if(!parse_cmp_expr(c)) return 0;
  int did=0;
  while(tok_is(c,"=")||tok_is(c,"==")||tok_is(c,"!=")){
    int ne=tok_is(c,"!="); lx_next(&c->lex);
    if(!parse_cmp_expr(c)) return 0;
    emit_cmp(c,ne?CMP_NEQ:CMP_EQ);
    did=1;
  }
  if(did){ expr_not_const(c); c->expr_boolish=1; }
  return 1;
}

static int parse_bit_and(Compiler *c){
  if(!parse_eq(c)) return 0;
  int did=0;
  while(tok_is(c,"&")){
    lx_next(&c->lex);
    if(!parse_eq(c)) return 0;
    if(!emit_binary(c,OP_AND)) return 0;
    expr_not_const(c);
    did=1;
  }
  if(did) c->expr_boolish=0;
  return 1;
}

static int parse_bit_xor(Compiler *c){
  if(!parse_bit_and(c)) return 0;
  int did=0;
  while(tok_is(c,"^") || is_id(c,"xor")){
    lx_next(&c->lex);
    if(!parse_bit_and(c)) return 0;
    if(!emit_binary(c,OP_XOR)) return 0;
    expr_not_const(c);
    did=1;
  }
  if(did) c->expr_boolish=0;
  return 1;
}

static int parse_bit_or(Compiler *c){
  if(!parse_bit_xor(c)) return 0;
  int did=0;
  while(tok_is(c,"|")){
    lx_next(&c->lex);
    if(!parse_bit_xor(c)) return 0;
    if(!emit_binary(c,OP_OR)) return 0;
    expr_not_const(c);
    did=1;
  }
  if(did) c->expr_boolish=0;
  return 1;
}

static int parse_and(Compiler *c){
  if(!parse_bit_or(c)) return 0;
  size_t false_sites[128];
  int n_false=0;
  while(tok_is(c,"&&") || is_id(c,"and")){
    lx_next(&c->lex);
    if(n_false>=(int)(sizeof(false_sites)/sizeof(false_sites[0]))){
      c->unsupported=1;
      snprintf(c->lex.err,sizeof(c->lex.err),"too many logical-and terms");
      return 0;
    }
    if(!emit_condition_bool(c)) return 0;
    false_sites[n_false++]=emit_branch(c,OP_BF);
    if(!parse_bit_or(c)) return 0;
  }
  if(n_false>0){
    if(!emit_condition_bool(c)) return 0;
    size_t b=emit_branch(c,OP_B);
    size_t false_pos=c->code.len;
    if(!emit_push_i16_full(c,0)) return 0;
    for(int i=0;i<n_false;i++) patch_branch(c,false_sites[i],false_pos);
    patch_branch(c,b,c->code.len);
    expr_not_const(c);
    c->expr_boolish=1;
  }
  return 1;
}

static int parse_classic_logical(Compiler *c){
  if(!parse_bit_or(c)) return 0;
  while(tok_is(c,"&&") || is_id(c,"and") ||
        tok_is(c,"||") || is_id(c,"or")){
    int is_and=tok_is(c,"&&") || is_id(c,"and");
    lx_next(&c->lex);
    if(!emit_condition_bool(c)) return 0;
    size_t shortcut=emit_branch(c,is_and?OP_BF:OP_BT);
    if(!parse_bit_or(c) || !emit_condition_bool(c)) return 0;
    size_t done=emit_branch(c,OP_B);
    size_t shortcut_pos=c->code.len;
    if(!emit_push_i16_full(c,is_and?0:1)) return 0;
    patch_branch(c,shortcut,shortcut_pos);
    patch_branch(c,done,c->code.len);
    expr_not_const(c);
    c->expr_boolish=1;
  }
  return 1;
}

static int parse_expr(Compiler *c){
  int classic=c->project && c->project->classic_version>0;
  if(classic){
    if(!parse_classic_logical(c)) return 0;
  } else if(!parse_and(c)) return 0;
  size_t true_sites[128];
  int n_true=0;
  while(!classic && (tok_is(c,"||") || is_id(c,"or"))){
    lx_next(&c->lex);
    if(n_true>=(int)(sizeof(true_sites)/sizeof(true_sites[0]))){
      c->unsupported=1;
      snprintf(c->lex.err,sizeof(c->lex.err),"too many logical-or terms");
      return 0;
    }
    if(!emit_condition_bool(c)) return 0;
    true_sites[n_true++]=emit_branch(c,OP_BT);
    if(!parse_and(c)) return 0;
  }
  if(n_true>0){
    if(!emit_condition_bool(c)) return 0;
    size_t b=emit_branch(c,OP_B);
    size_t true_pos=c->code.len;
    if(!emit_push_i16_full(c,1)) return 0;
    for(int i=0;i<n_true;i++) patch_branch(c,true_sites[i],true_pos);
    patch_branch(c,b,c->code.len);
    expr_not_const(c);
    c->expr_boolish=1;
  }
  if(tok_is(c,"?")){
    lx_next(&c->lex);
    if(!emit_condition_bool(c)) return 0;
    size_t bf=emit_branch(c,OP_BF);
    if(!parse_expr(c)) return 0;
    if(!need(c,":")) return 0;
    size_t b=emit_branch(c,OP_B);
    patch_branch(c,bf,c->code.len);
    if(!parse_expr(c)) return 0;
    patch_branch(c,b,c->code.len);
    expr_not_const(c);
    c->expr_boolish=0;
  }
  return 1;
}

static int parse_lvalue_from_name(Compiler *c, const char *first, LValue *lv){
  memset(lv,0,sizeof(*lv));
  snprintf(lv->name,sizeof(lv->name),"%s",first);
  snprintf(lv->receiver,sizeof(lv->receiver),"%s",first);
  lv->inst=plain_scope(c,first);
  lv->reftype=0xA0;
  if(eat(c,".")){
    if(c->lex.tok.kind!=TOK_ID){ c->unsupported=1; snprintf(c->lex.err,sizeof(c->lex.err),"expected field name"); return 0; }
    char field[128]; snprintf(field,sizeof(field),"%s",c->lex.tok.text); lx_next(&c->lex);
    double cv=0;
    if(!strcmp(first,"self")) lv->inst=IT_SELF;
    else if(!strcmp(first,"other")) lv->inst=IT_OTHER;
    else if(!strcmp(first,"global")) lv->inst=IT_GLOBAL;
    else if(resolve_asset(c,first,&cv)) lv->inst=(int)cv;
    else { lv->is_stacktop=1; lv->inst=IT_STACK; }
    snprintf(lv->name,sizeof(lv->name),"%s",field);
    while(eat(c,".")){
      if(c->lex.tok.kind!=TOK_ID){
        c->unsupported=1;
        snprintf(c->lex.err,sizeof(c->lex.err),"expected field name");
        return 0;
      }
      if(!emit_lvalue_read(c,lv) || !emit_conv(c,DT_VAR,DT_INT32)) return 0;
      snprintf(lv->name,sizeof(lv->name),"%s",c->lex.tok.text);
      lv->inst=IT_STACK;
      lv->is_stacktop=1;
      lv->receiver_on_stack=1;
      lx_next(&c->lex);
    }
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
      if(!strcmp(lv->name,"argument")) lv->inst=IT_SELF;
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
  /* An array/accessor element can itself be an instance or struct receiver:
   * entries[i].field and table[? key].field are both ordinary GML lvalues.
   * Resolve the element once, then continue the existing stack-receiver path
   * so reads, direct assignments, and compound assignments share semantics. */
  if(tok_is(c,".") && (lv->is_array || lv->accessor!=ACCESS_NONE)){
    if(!emit_lvalue_read(c,lv) || !emit_conv(c,DT_VAR,DT_INT32)) return 0;
    free(lv->index_src);
    lv->index_src=NULL;
    lv->is_array=0;
    lv->is_array_2d=0;
    lv->accessor=ACCESS_NONE;
    lv->reftype=0x80;
    lv->inst=IT_STACK;
    lv->is_stacktop=1;
    lv->receiver_on_stack=1;
    while(eat(c,".")){
      if(c->lex.tok.kind!=TOK_ID){
        c->unsupported=1;
        snprintf(c->lex.err,sizeof(c->lex.err),"expected field name");
        return 0;
      }
      snprintf(lv->name,sizeof(lv->name),"%s",c->lex.tok.text);
      lx_next(&c->lex);
      if(tok_is(c,".")){
        if(!emit_lvalue_read(c,lv) || !emit_conv(c,DT_VAR,DT_INT32)) return 0;
      }
    }
  }
  return 1;
}

static int emit_lvalue_base_read(Compiler *c, LValue *lv){
  if(lv->is_stacktop){
    if(!emit_lvalue_address(c,lv)) return 0;
    return emit_push_var(c,0,lv->name,0x80);
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
    return emit_push_var(c,0,lv->name,0x00);
  }
  if(lv->is_stacktop){
    if(!emit_lvalue_address(c,lv)) return 0;
    return emit_push_var(c,0,lv->name,0x80);
  }
  return emit_push_var(c,lv->inst,lv->name,0xA0);
}

static int emit_lvalue_write(Compiler *c, LValue *lv, uint8_t type1){
  if(lv->accessor) return emit_accessor_write(c,lv);
  if(lv->is_array) return emit_pop_var(c,0,lv->name,0x00,type1);
  if(lv->is_stacktop) return emit_pop_var(c,0,lv->name,0x80,type1);
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
  /* An empty `var;` action fragment is a no-op. Handle it before parsing
   * named locals. */
  if(eat(c,";")) return 1;
  do {
    if(c->lex.tok.kind!=TOK_ID){ c->unsupported=1; snprintf(c->lex.err,sizeof(c->lex.err),"expected local name"); return 0; }
    char name[128]; snprintf(name,sizeof(name),"%s",c->lex.tok.text); lx_next(&c->lex);
    add_local(c,name);
    if(eat(c,"=")){ if(!parse_expr(c)) return 0; if(!emit_pop_var(c,IT_LOCAL,name,0xA0,DT_VAR)) return 0; }
  } while(eat(c,","));
  eat(c,";");
  return 1;
}

static int parse_globalvar_decl(Compiler *c){
  lx_next(&c->lex);
  do {
    if(c->lex.tok.kind!=TOK_ID){ c->unsupported=1; snprintf(c->lex.err,sizeof(c->lex.err),"expected global name"); return 0; }
    char name[128]; snprintf(name,sizeof(name),"%s",c->lex.tok.text); lx_next(&c->lex);
    if(!add_global(c,name)) return 0;
  } while(eat(c,","));
  eat(c,";");
  return 1;
}

static int parse_if(Compiler *c){
  lx_next(&c->lex);
  if(!parse_expr(c)) return 0;
  if(!emit_condition_bool(c)) return 0;
  size_t bf=emit_branch(c,OP_BF);
  /* GM6-GM8 accept an optional Pascal-style `then` between the condition and
   * its statement.  It is syntax, not an identifier expression: leaving it
   * for parse_block_or_stmt() makes the branch guard only a throwaway read of
   * a variable named `then`, while the following braced body runs
   * unconditionally. */
  if(is_id(c,"then")) lx_next(&c->lex);
  if(!parse_block_or_stmt(c)) return 0;
  /* Classic drag-and-drop actions terminate a braced true arm before emitting
   * `else` ("if (...) { ... }; else { ... }").  GM keeps that else attached
   * to the conditional.  Treat the intervening empty statements the same way;
   * otherwise `else` is compiled as a variable read and its block runs
   * unconditionally. */
  while(eat(c,";")) {}
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
  if(!parse_expr(c)) return 0;
  if(!emit_condition_bool(c)) return 0;
  int break_mark=c->n_break_sites;
  int continue_mark=c->n_continue_sites;
  c->break_depth++;
  c->continue_depth++;
  size_t bf=emit_branch(c,OP_BF);
  if(!parse_block_or_stmt(c)) return 0;
  c->continue_depth--;
  c->break_depth--;
  patch_continues_from(c,continue_mark,start);
  size_t b=emit_branch(c,OP_B);
  patch_branch(c,b,start);
  patch_branch(c,bf,c->code.len);
  patch_breaks_from(c,break_mark,c->code.len);
  return 1;
}

static int parse_do_until(Compiler *c){
  lx_next(&c->lex);
  size_t start=c->code.len;
  int break_mark=c->n_break_sites;
  int continue_mark=c->n_continue_sites;
  c->break_depth++;
  c->continue_depth++;
  if(!parse_block_or_stmt(c)) return 0;
  c->continue_depth--;
  c->break_depth--;
  size_t condition=c->code.len;
  patch_continues_from(c,continue_mark,condition);
  if(!is_id(c,"until")){
    c->unsupported=1;
    snprintf(c->lex.err,sizeof(c->lex.err),"expected until after do body");
    return 0;
  }
  lx_next(&c->lex);
  if(!parse_expr(c) || !emit_condition_bool(c)) return 0;
  size_t bf=emit_branch(c,OP_BF);
  patch_branch(c,bf,start);
  eat(c,";");
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
    if(!emit_condition_bool(c)) return 0;
    bf=emit_branch(c,OP_BF);
  }
  c->lex.pos=close_pos+1;
  lx_next(&c->lex);
  int break_mark=c->n_break_sites;
  int continue_mark=c->n_continue_sites;
  c->break_depth++;
  c->continue_depth++;
  if(!parse_block_or_stmt(c)) return 0;
  c->continue_depth--;
  c->break_depth--;
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
  if(!parse_expr(c) ||
     !emit_conv(c,DT_VAR,DT_INT32) ||
     !emit_dup(c,DT_INT32) ||
     !emit_push_i32_full(c,0) ||
     !emit_cmp_typed(c,CMP_LTE,DT_INT32,DT_INT32)) return 0;
  size_t done=emit_branch(c,OP_BT);
  size_t loop_start=c->code.len;
  int break_mark=c->n_break_sites;
  int continue_mark=c->n_continue_sites;
  c->break_depth++;
  c->continue_depth++;
  if(!parse_block_or_stmt(c)) return 0;
  c->continue_depth--;
  c->break_depth--;
  size_t step=c->code.len;
  patch_continues_from(c,continue_mark,step);
  if(!emit_push_i32_full(c,1) ||
     !emit_binary_typed(c,OP_SUB,DT_INT32,DT_INT32) ||
     !emit_dup(c,DT_INT32) ||
     !emit_conv(c,DT_INT32,DT_BOOL)) return 0;
  size_t again=emit_branch(c,OP_BT);
  patch_branch(c,again,loop_start);
  size_t cleanup=c->code.len;
  if(!emit_popz_typed(c,DT_INT32)) return 0;
  patch_branch(c,done,cleanup);
  patch_breaks_from(c,break_mark,cleanup);
  return 1;
}

static int parse_with(Compiler *c){
  lx_next(&c->lex);
  int parenthesized=eat(c,"(");
  if(c->lex.tok.kind==TOK_ID &&
     (!strcmp(c->lex.tok.text,"self") || !strcmp(c->lex.tok.text,"other"))){
    size_t after=skip_ws_comments_at(c->lex.src,c->lex.tok.end);
    if(!parenthesized || c->lex.src[after]==')'){
      int inst=!strcmp(c->lex.tok.text,"self") ? IT_SELF : IT_OTHER;
      lx_next(&c->lex);
      if(!emit_push_var(c,inst,"id",0xA0) || !emit_conv(c,DT_VAR,DT_INT32) ||
         (parenthesized && !need(c,")"))) return 0;
    } else {
      if(!parse_expr(c) || (parenthesized && !need(c,")"))) return 0;
    }
  } else if(!parse_expr(c) || (parenthesized && !need(c,")"))) return 0;
  int break_mark=c->n_break_sites;
  int continue_mark=c->n_continue_sites;
  c->break_depth++;
  c->continue_depth++;
  size_t push=emit_branch(c,OP_PUSHENV);
  size_t body_start=c->code.len;
  if(!parse_block_or_stmt(c)) return 0;
  c->continue_depth--;
  c->break_depth--;
  size_t pop=emit_branch(c,OP_POPENV);
  patch_continues_from(c,continue_mark,pop);
  patch_branch(c,pop,body_start);
  patch_branch(c,push,pop);
  patch_breaks_from(c,break_mark,c->code.len);
  return 1;
}

static int parse_switch(Compiler *c){
  lx_next(&c->lex);
  if(!parse_expr(c)) return 0;
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
    if(!emit_dup(c,DT_VAR) ||
       !compile_expr_slice(c,c->lex.src+cases[i].label.start,cases[i].label.end-cases[i].label.start) ||
       !emit_cmp(c,CMP_EQ)){
      free(cases); free(targets); free(case_br); free(body_pos);
      return 0;
    }
    case_br[i]=emit_branch(c,OP_BT);
  }
  size_t dispatch_end=emit_branch(c,OP_B);
  int break_mark=c->n_break_sites;
  c->break_depth++;
  for(int i=0;i<n_cases;i++){
    body_pos[i]=c->code.len;
    if(!span_empty(c->lex.src,cases[i].body)){
      if(!compile_statement_slice(c,c->lex.src+cases[i].body.start,cases[i].body.end-cases[i].body.start)){
        free(cases); free(targets); free(case_br); free(body_pos);
        return 0;
      }
    }
  }
  size_t cleanup=c->code.len;
  c->break_depth--;
  if(!emit_popz(c)){ free(cases); free(targets); free(case_br); free(body_pos); return 0; }
  for(int i=0;i<n_cases;i++) if(case_br[i]) patch_branch(c,case_br[i],body_pos[targets[i]]);
  patch_branch(c,dispatch_end,default_idx>=0?body_pos[default_idx]:cleanup);
  patch_breaks_from(c,break_mark,cleanup);
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
  uint8_t binop=tok_is(c,"+=")?OP_ADD:tok_is(c,"-=")?OP_SUB:tok_is(c,"*=")?OP_MUL:tok_is(c,"/=")?OP_DIV:OP_MOD;
  lx_next(&c->lex);
  if(is_assign){
    /* The chained receiver was resolved as an int32 instance id.  DUP's block
     * shuffle is measured in encoded stack widths, so normalize both operands
     * to variable-width slots before swapping them; the expression remains
     * dynamically typed. */
    if(lv->receiver_on_stack && !emit_conv(c,DT_INT32,DT_VAR)) return 0;
    if(!parse_expr(c)) return 0;
    /* A chained receiver such as global.actor.position.x is resolved while the
     * lvalue is parsed, leaving the final instance below the assignment value.
     * StackTop stores require the opposite order. Preserve ordinary `actor.x`
     * evaluation (whose receiver is emitted after the value) and swap only a
     * receiver that is already resident on the stack. */
    if(lv->receiver_on_stack &&
       (!emit_conv(c,DT_VAR,DT_VAR) || !emit_swap_top(c))) return 0;
    if((lv->is_array || lv->is_stacktop) && !emit_lvalue_address(c,lv)) return 0;
  } else {
    if(lv->is_stacktop && !lv->is_array && !lv->accessor){
      if(!emit_lvalue_address(c,lv) ||
         !emit_dup(c,DT_INT32) ||
         !emit_push_var(c,0,lv->name,0x80)) return 0;
      if(is_inc || is_dec){
        if(!emit_push_i16_full(c,1) ||
           !emit_binary_typed(c,is_inc?OP_ADD:OP_SUB,DT_INT32,DT_VAR)) return 0;
      } else {
        if(!parse_expr(c) ||
           !emit_binary_typed(c,binop,DT_INT32,DT_VAR)) return 0;
      }
      if(!emit_pop_var(c,0,lv->name,0x80,DT_INT32)) return 0;
      eat(c,";");
      return 1;
    }
    if(is_inc || is_dec){
      if(!emit_lvalue_read(c,lv) ||
         !emit_push_i16_full(c,1) ||
         !emit_binary_typed(c,is_inc?OP_ADD:OP_SUB,DT_INT32,DT_VAR)) return 0;
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

/* Some classic action libraries emit an expression followed directly by case
 * labels instead of spelling out `switch (expression)`.  The classic grammar
 * treats the expression value as the selector.  At entry that selector is the
 * only value on the stack; leave the closing brace to the enclosing block. */
static int parse_implicit_cases(Compiler *c){
  size_t done_sites[128];
  int n_done=0;
  while(is_id(c,"case") || is_id(c,"default")){
    int is_default=is_id(c,"default");
    lx_next(&c->lex);
    size_t skip=(size_t)-1;
    if(!is_default){
      if(!emit_dup(c,DT_VAR) || !parse_expr(c) || !emit_cmp(c,CMP_EQ)) return 0;
      skip=emit_branch(c,OP_BF);
    }
    if(!need(c,":")) return 0;
    while(c->lex.tok.kind!=TOK_EOF && !tok_is(c,"}") &&
          !is_id(c,"case") && !is_id(c,"default")){
      if(!parse_statement(c)) return 0;
    }
    if(n_done>=(int)(sizeof(done_sites)/sizeof(done_sites[0]))){
      c->unsupported=1;
      snprintf(c->lex.err,sizeof(c->lex.err),"too many implicit case labels");
      return 0;
    }
    done_sites[n_done++]=emit_branch(c,OP_B);
    if(skip!=(size_t)-1) patch_branch(c,skip,c->code.len);
  }
  size_t cleanup=c->code.len;
  if(!emit_popz(c)) return 0;
  for(int i=0;i<n_done;i++) patch_branch(c,done_sites[i],cleanup);
  return 1;
}

static int parse_simple_or_assign(Compiler *c){
  if(tok_is(c,"(")){
    lx_next(&c->lex);
    if(!parse_expr(c) || !need(c,")")) return 0;
    if(eat(c,".")){
      if(c->lex.tok.kind!=TOK_ID){
        c->unsupported=1;
        snprintf(c->lex.err,sizeof(c->lex.err),"expected field name");
        return 0;
      }
      char receiver[128];
      snprintf(receiver,sizeof(receiver),"__postfix_receiver_%" PRIu64,(uint64_t)c->code.len);
      if(!add_local(c,receiver) || !emit_pop_var(c,IT_LOCAL,receiver,0xA0,DT_VAR)) return 0;
      LValue lv;
      memset(&lv,0,sizeof(lv));
      snprintf(lv.receiver,sizeof(lv.receiver),"%s",receiver);
      snprintf(lv.name,sizeof(lv.name),"%s",c->lex.tok.text);
      lv.inst=IT_STACK;
      lv.is_stacktop=1;
      lx_next(&c->lex);
      while(eat(c,".")){
        if(c->lex.tok.kind!=TOK_ID){
          c->unsupported=1;
          snprintf(c->lex.err,sizeof(c->lex.err),"expected field name");
          return 0;
        }
        if(!emit_lvalue_read(c,&lv) ||
           !emit_pop_var(c,IT_LOCAL,receiver,0xA0,DT_VAR)) return 0;
        snprintf(lv.name,sizeof(lv.name),"%s",c->lex.tok.text);
        lx_next(&c->lex);
      }
      if(tok_is(c,"=")||tok_is(c,"+=")||tok_is(c,"-=")||tok_is(c,"*=")||
         tok_is(c,"/=")||tok_is(c,"%=")||tok_is(c,"++")||tok_is(c,"--"))
        return parse_assignment_tail(c,&lv);
      if(!emit_lvalue_read(c,&lv) || !parse_value_postfix(c)) return 0;
      eat(c,";");
      return emit_popz(c);
    }
    if(!parse_value_postfix(c)) return 0;
    eat(c,";");
    return emit_popz(c);
  }
  if(c->lex.tok.kind!=TOK_ID){ if(!parse_expr(c)) return 0; eat(c,";"); emit_popz(c); return 1; }
  char first[128]; snprintf(first,sizeof(first),"%s",c->lex.tok.text); lx_next(&c->lex);
  if(tok_is(c,"=")||tok_is(c,"+=")||tok_is(c,"-=")||tok_is(c,"*=")||tok_is(c,"/=")||tok_is(c,"%=")||tok_is(c,"++")||tok_is(c,"--")){
    LValue lv;
    if(!parse_lvalue_from_name(c,first,&lv)) return 0;
    int ok=parse_assignment_tail(c,&lv);
    free(lv.index_src);
    return ok;
  }
  if(tok_is(c,".") || tok_is(c,"[")){
    LValue lv;
    if(!parse_lvalue_from_name(c,first,&lv)) return 0;
    if(tok_is(c,"=")||tok_is(c,"+=")||tok_is(c,"-=")||tok_is(c,"*=")||tok_is(c,"/=")||tok_is(c,"%=")||tok_is(c,"++")||tok_is(c,"--")){
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
    emit_popz(c);
    return 1;
  }
  if(tok_is(c,"(")){
    lx_next(&c->lex);
    int argc=0;
    int scope=plain_scope(c,first);
    if(scope!=IT_SELF){
      if(!emit_push_var(c,scope,first,0xA0)) return 0;
      if(!parse_call_args_reversed(c,&argc)) return 0;
      if(!emit_callv(c,argc) || !parse_value_postfix(c)) return 0;
      if(is_id(c,"case") || is_id(c,"default")) return parse_implicit_cases(c);
      emit_popz(c);
      eat(c,";");
      return 1;
    }
    if(!parse_call_args_reversed(c,&argc)) return 0;
    if(!emit_call(c,first,argc) || !parse_value_postfix(c)) return 0;
    if(is_id(c,"case") || is_id(c,"default")) return parse_implicit_cases(c);
    emit_popz(c);
    eat(c,";");
    return 1;
  }
  double cv=0;
  int sci=-1;
  if(resolve_const(c,first,&cv)) emit_push_real(c,cv);
  else if(resolve_function_code_index(c,first,&sci)) emit_script_funcval(c,sci);
  else emit_push_var(c,plain_scope(c,first),first,0xA0);
  eat(c,";");
  emit_popz(c);
  return 1;
}

static int parse_statement(Compiler *c){
  if(c->lex.tok.kind==TOK_EOF) return 1;
  if(c->log_statements)
    anygm_host_logf(c->project?c->project->host:NULL,ANYGM_LOG_DEBUG,
                    "gmlc: statement: %s: %s @ %" PRIu64 "\n",
                    c->source_path?c->source_path:"<source>",
                    c->lex.tok.text,
                    (uint64_t)c->lex.tok.start);
  if(eat(c,";")) return 1;
  if(is_id(c,"function") && function_shape_at(c->lex.src,c->lex.tok.start)) return parse_function_value(c,0);
  if(is_id(c,"var")) return parse_var_decl(c);
  if(is_id(c,"globalvar")) return parse_globalvar_decl(c);
  if(is_id(c,"if")) return parse_if(c);
  if(is_id(c,"while")) return parse_while(c);
  if(is_id(c,"do")) return parse_do_until(c);
  if(is_id(c,"for")) return parse_for(c);
  if(is_id(c,"repeat")) return parse_repeat(c);
  if(is_id(c,"switch")) return parse_switch(c);
  if(is_id(c,"with")) return parse_with(c);
  if(is_id(c,"return")) return parse_return_stmt(c);
  if(is_id(c,"exit")){ lx_next(&c->lex); emit_u32(&c->code,fw(OP_EXIT,0,0)); eat(c,";"); return 1; }
  if(is_id(c,"break")){
    lx_next(&c->lex);
    /* Classic drag-and-drop actions can emit a top-level break to stop the
     * current event. A zero-offset branch would loop forever, so encode that
     * form as an event exit while retaining normal loop/switch patching. */
    if(c->break_depth>0){
      if(!emit_break_branch(c)) return 0;
    } else if(!emit_u32(&c->code,fw(OP_EXIT,0,0))) return 0;
    eat(c,";");
    return 1;
  }
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
    char *txt=compiler_read_source(c->project,c->project->scripts[i].source_path);
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

int compile_text_internal(const GmlcProject *project, const GmlcFunctionRegistry *funcs, int script_index, const char *source_path, const char *text, const char *params, GmlcCodeBlob *out, char *err, size_t errcap){
  Compiler c;
  memset(&c,0,sizeof(c));
  int success=0;
  c.project=project;
  c.funcs=funcs;
  c.source_path=source_path;
  c.script_index=script_index;
  c.log_statements=anygm_host_development_setting(project?project->host:NULL,
                                                   "GMLC_LOG_STATEMENTS")!=NULL;
  if(funcs){
    for(int i=0;i<funcs->n_macros;i++) if(!add_macro(&c,funcs->macro_names[i],funcs->macro_values[i])){
      c.unsupported=1;
      snprintf(c.lex.err,sizeof(c.lex.err),"macro registry allocation failed");
      break;
    }
  } else collect_macros(&c);
  c.lex.src=text;
  lx_next(&c.lex);
  if(!emit_param_prologue(&c,params)){
    c.unsupported=1;
  }
  while(c.lex.tok.kind!=TOK_EOF && !c.unsupported){
    if(!parse_statement(&c)) break;
  }
  if(!c.unsupported && c.lex.tok.kind==TOK_EOF){
    out->data=c.code.data;
    out->size=c.code.len;
    out->refs=c.refs;
    out->n_refs=c.n_refs;
    out->cap_refs=c.cap_refs;
    out->strings=c.strings;
    out->n_strings=c.n_strings;
    out->cap_strings=c.cap_strings;
    success=1;
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
  for(int i=0;i<c.n_globals;i++) free(c.globals[i]);
  free(c.globals);
  free(c.break_sites);
  free(c.continue_sites);
  for(int i=0;i<c.n_refs;i++) free(c.refs[i].name);
  free(c.refs);
  for(int i=0;i<c.n_strings;i++) free(c.strings[i].value);
  free(c.strings);
  free(c.code.data);
  if(success) return 1;
  snprintf(err,errcap,"%s: %s",
           source_path?source_path:"<source>",
           out->diagnostic?out->diagnostic:"compile failed");
  return 0;
}
