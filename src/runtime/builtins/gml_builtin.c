/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_builtin.c — GameMaker built-in function dispatch. */
#include "gml_vm.h"
#include "gml_render.h"
#include "gml_particle.h"
#include "gml_audio.h"
#include "gml_fmod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>
#include <dirent.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#if defined(_WIN32)
#include <direct.h>
#define GML_MKDIR(p) _mkdir(p)
#define GML_RMDIR(p) _rmdir(p)
#else
#define GML_MKDIR(p) mkdir((p),0777)
#define GML_RMDIR(p) rmdir(p)
#endif
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

/* file_find_first/next state (GM exposes one active find). Wildcard match is hand-rolled:
 * mingw has no fnmatch.h and GM masks only use * and ?. Case-insensitive like Windows. */
#define GML_FF_MAX 512
static char *g_ff_name[GML_FF_MAX]; static int g_ff_n=0, g_ff_idx=0;
static void ff_reset(void){ for(int i=0;i<g_ff_n;i++) free(g_ff_name[i]); g_ff_n=0; g_ff_idx=0; }
static int wild_match(const char *pat, const char *s){
  if(*pat==0) return *s==0;
  if(*pat=='*'){ if(wild_match(pat+1,s)) return 1; return *s ? wild_match(pat,s+1) : 0; }
  if(*s==0) return 0;
  if(*pat=='?' || tolower((unsigned char)*pat)==tolower((unsigned char)*s)) return wild_match(pat+1,s+1);
  return 0;
}

static uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o]|(uint32_t)d[o+1]<<8|(uint32_t)d[o+2]<<16|(uint32_t)d[o+3]<<24;
}
static double N(GmlVal *a, int n, int i){ return (i<n)? (a[i].t==V_REAL?a[i].d:(a[i].s?atof(a[i].s):0)) : 0; }
static void builtin_set_blendmode_ext(GmlRender *R, int src, int dst){
  if(!R) return;
  /* The software renderer only models the simple normal/add/subtract states.
   * A factor-based blend call still replaces that state in GameMaker, so clear
   * any previous simple mode instead of letting bm_subtract/bm_add leak. */
  if(getenv("GML_DBG_BM")) fprintf(stderr,"[bm] gpu_set_blendmode_ext(%d,%d) -> normal\n",src,dst);
  R->blendmode=0;
}
static double builtin_game_speed(GmlVM *vm){
  GmlVal *p=vm?gml_varmap_get(&vm->globals,"__game_speed_fps"):NULL;
  double v=p?N(p,1,0):60.0;
  return v>0 ? v : 60.0;
}
static void builtin_set_game_speed(GmlVM *vm, double fps){
  if(!vm) return;
  if(fps<=0) fps=60.0;
  if(fabs(fps-60.0)<0.000001 && !gml_varmap_get(&vm->globals,"__game_speed_fps")) return;
  *gml_varmap_put(&vm->globals,"__game_speed_fps")=vreal(fps);
}
static int presentation_base_size(const GmlVM *vm, const GmlRender *r, int height){
  if(r && r->aspect_fullwidth){
    int wide=height?r->aspect_wide_h:r->aspect_wide_w;
    if(wide>0) return wide;
  }
  if(vm && vm->win){
    int native=height?(int)vm->win->disp_h:(int)vm->win->disp_w;
    if(native>0) return native;
  }
  return height?216:288;
}
static int presentation_size(const GmlVM *vm, const GmlRender *r, int height){
  int effective=r?(height?r->presentation_h:r->presentation_w):0;
  if(effective>0) return effective;
  int configured=r?(height?r->resolution_h:r->resolution_w):0;
  if(configured>0) return configured;
  return presentation_base_size(vm,r,height);
}
static const char *gm_string_tmp(GmlVal v){
  static char ring[8][64];
  static int ri;
  if(v.t==V_STR) return v.s?v.s:"";
  if(v.t==V_UNDEF || v.t==V_ARR) return "";
  char *b=ring[ri++ & 7];
  double d=v.t==V_REAL?v.d:0;
  if(d==floor(d)&&fabs(d)<1e15) snprintf(b,64,"%.0f",d);
  else snprintf(b,64,"%.2f",d);
  return b;
}
static const char *S(GmlVal *a, int n, int i){ return (i<n)? gm_string_tmp(a[i]) : ""; }

/* A bounded classic execute_string compatibility path. Old projects commonly construct a
 * resource assignment such as "sprite_index=spr_name" at runtime. Resolve that controlled
 * assignment without introducing a second runtime compiler into the core. */
static int classic_execute_assignment(GmlVM *vm, const char *source){
  if(!vm || !source) return 0;
  while(isspace((unsigned char)*source)) source++;
  const char *eq=strchr(source,'='); if(!eq) return 0;
  const char *lhs_end=eq; while(lhs_end>source && isspace((unsigned char)lhs_end[-1])) lhs_end--;
  const char *rhs=eq+1; while(isspace((unsigned char)*rhs)) rhs++;
  const char *rhs_end=rhs+strlen(rhs); while(rhs_end>rhs && (isspace((unsigned char)rhs_end[-1])||rhs_end[-1]==';')) rhs_end--;
  char lhs[192],value_name[192]; size_t ln=(size_t)(lhs_end-source),rn=(size_t)(rhs_end-rhs);
  if(!ln || ln>=sizeof(lhs) || !rn || rn>=sizeof(value_name)) return 0;
  memcpy(lhs,source,ln); lhs[ln]=0; memcpy(value_name,rhs,rn); value_name[rn]=0;
  char *dot=strrchr(lhs,'.'); const char *field=dot?dot+1:lhs;
  GmlInstance *target=vm->cur_self;
  if(dot){
    *dot=0; target=NULL;
    if(!strcmp(lhs,"self")) target=vm->cur_self;
    else if(!strcmp(lhs,"other")) target=vm->cur_other;
    else {
      GmlVal *ref=vm->cur_self?gml_varmap_get(&vm->cur_self->vars,lhs):NULL;
      if(ref && ref->t==V_REAL){
        int id=(int)ref->d;
        for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked && (int)vm->inst[i].id==id){ target=&vm->inst[i]; break; }
        if(!target && id>=0 && id<vm->n_objects) target=gml_find_instance(vm,id);
      }
      if(!target){ int object=gml_object_index_by_name(vm,lhs); if(object>=0) target=gml_find_instance(vm,object); }
    }
  }
  if(!target || !*field) return 0;
  char *end=NULL; double value=strtod(value_name,&end);
  if(!end || *end){
    value=0; int found=0; GmlRender *render=(GmlRender*)vm->render;
    if(render) for(int i=0;i<render->n_spr;i++) if(render->spr[i].name && !strcmp(render->spr[i].name,value_name)){
      value=i; found=1; break;
    }
    if(!found){ int object=gml_object_index_by_name(vm,value_name); if(object>=0){ value=object; found=1; } }
    if(!found) return 0;
  }
  return gml_inst_var_set_val(vm,vreal(target->id),field,vreal(value));
}
/* Shared shader uniform handling (LUT row + CRT-geom parameters). Handle = sh*16 + slot:
 *   slot 1 = LUT row, 3 = CRT sizes vec4, 4 = CRT distortion, 5 = CRT distort bool,
 *   6 = CRT border bool, 15 = accepted-and-ignored. See parse_shader_palettes / draw_surface_crt. */
static double gml_shader_get_uniform(GmlRender *R, int sh, const char *un){
  if(R && sh>=0 && sh<R->n_shader_pal && R->shader_pal){
    struct GmlShaderPal *p=&R->shader_pal[sh];
    if(p->lut && !strcmp(un,p->lut_row_uniform)) return sh*16+1;
    if(p->crt){
      if(p->crt_sizes_uniform[0]      && !strcmp(un,p->crt_sizes_uniform))      return sh*16+3;
      if(p->crt_distortion_uniform[0] && !strcmp(un,p->crt_distortion_uniform)) return sh*16+4;
      if(p->crt_distort_uniform[0]    && !strcmp(un,p->crt_distort_uniform))    return sh*16+5;
      if(p->crt_border_uniform[0]     && !strcmp(un,p->crt_border_uniform))     return sh*16+6;
    }
  }
  return sh>=0? sh*16+15 : -1;
}
static void gml_shader_set_uniform_f(GmlRender *R, int h, GmlVal *a, int n){
  if(!R || h<0) return;
  int sh=h/16, slot=h%16;
  if(sh<0 || sh>=R->n_shader_pal || !R->shader_pal) return;
  struct GmlShaderPal *p=&R->shader_pal[sh];
  if(slot==1){ if(p->lut) p->lut_row=(float)N(a,n,1); return; }
  if(!p->crt) return;
  switch(slot){
    case 3: p->crt_sizes[0]=(float)N(a,n,1); p->crt_sizes[1]=(float)N(a,n,2);
            p->crt_sizes[2]=(float)N(a,n,3); p->crt_sizes[3]=(float)N(a,n,4); break;
    case 4: p->crt_distortion=(float)N(a,n,1); break;
    case 5: p->crt_distort=(N(a,n,1)!=0.0); break;
    case 6: p->crt_border=(N(a,n,1)!=0.0); break;
    default: break;
  }
}
/* the event path argument of an fmod_* call: first "event:/..." string, else first non-empty string */
static const char *fmod_path_arg(GmlVal *a, int n){
  const char *first=NULL;
  for(int i=0;i<n;i++) if(a[i].t==V_STR && a[i].s && a[i].s[0]){
    if(!strncmp(a[i].s,"event:",6)) return a[i].s;
    if(!first) first=a[i].s;
  }
  return first;
}
/* Negative subimage values select the drawing instance image_index. */
static int gml_draw_subimg(GmlVM *vm, double raw){
  if(raw < 0){ GmlInstance *s=vm->cur_self; return s? (int)s->image_index : 0; }
  return (int)raw;
}
static GmlVal array4(double a, double b, double c, double d){
  GmlArr *A=calloc(1,sizeof(*A));
  if(!A) return vreal(0);
  A->data=calloc(4,sizeof(GmlVal));
  if(!A->data){ free(A); return vreal(0); }
  A->len=A->cap=4;
  A->data[0]=vreal(a); A->data[1]=vreal(b); A->data[2]=vreal(c); A->data[3]=vreal(d);
  GmlVal v=vreal(0); v.t=V_ARR; v.arr=A; return v;
}
static uint32_t phys_next_id(GmlVM *vm){
  if(!vm->phys_next_id) vm->phys_next_id=4000001u;
  return vm->phys_next_id++;
}
static GmlPhysicsFixture *phys_fixture_find(GmlVM *vm, int id){
  if(!vm || id<=0) return NULL;
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++)
    if(vm->phys_fixture[i].live && vm->phys_fixture[i].id==(uint32_t)id) return &vm->phys_fixture[i];
  return NULL;
}
static GmlPhysicsFixture *phys_fixture_new(GmlVM *vm){
  if(!vm) return NULL;
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++) if(!vm->phys_fixture[i].live){
    GmlPhysicsFixture *f=&vm->phys_fixture[i];
    memset(f,0,sizeof(*f));
    f->live=1; f->id=phys_next_id(vm); f->bound_inst=-1; f->awake=1;
    return f;
  }
  return NULL;
}
static GmlPhysicsJoint *phys_joint_find(GmlVM *vm, int id){
  if(!vm || id<=0) return NULL;
  for(int i=0;i<GML_PHYS_JOINT_MAX;i++)
    if(vm->phys_joint[i].live && vm->phys_joint[i].id==(uint32_t)id) return &vm->phys_joint[i];
  return NULL;
}
static GmlPhysicsJoint *phys_joint_new(GmlVM *vm, int type){
  if(!vm) return NULL;
  for(int i=0;i<GML_PHYS_JOINT_MAX;i++) if(!vm->phys_joint[i].live){
    GmlPhysicsJoint *j=&vm->phys_joint[i];
    memset(j,0,sizeof(*j));
    j->live=1; j->id=phys_next_id(vm); j->type=type;
    return j;
  }
  return NULL;
}
static GmlVal arr_newv(int n){
  GmlArr *A=calloc(1,sizeof(*A));
  if(!A) return vreal(0);
  A->data=calloc((size_t)(n>0?n:1),sizeof(GmlVal));
  if(!A->data){ free(A); return vreal(0); }
  A->len=A->cap=n;
  GmlVal v=vreal(0); v.t=V_ARR; v.arr=A; return v;
}
#define GML_TEX_SPR_TAG  0x54000000u
#define GML_TEX_SURF_TAG 0x55000000u
#define GML_TEX_BG_TAG   0x56000000u
#define GML_TEX_KIND_MASK 0xFF000000u
static GmlVal arr8(double a0,double a1,double a2,double a3,double a4,double a5,double a6,double a7){
  GmlVal v=arr_newv(8);
  if(v.t!=V_ARR) return v;
  GmlArr *A=(GmlArr*)v.arr;
  A->data[0]=vreal(a0); A->data[1]=vreal(a1); A->data[2]=vreal(a2); A->data[3]=vreal(a3);
  A->data[4]=vreal(a4); A->data[5]=vreal(a5); A->data[6]=vreal(a6); A->data[7]=vreal(a7);
  return v;
}
static int sprite_tpag_info(GmlRender *R, int spr, int img, GmlSprite **os, GmlTpag **ot, GmlAtlas **oa){
  if(os) *os=NULL;
  if(ot) *ot=NULL;
  if(oa) *oa=NULL;
  if(!R || spr<0 || spr>=R->n_spr) return 0;
  GmlSprite *s=&R->spr[spr];
  if(os) *os=s;
  if(s->runtime_rgba) return 2;
  if(s->n_frames<=0 || !s->frame) return 0;
  int sub=((img%s->n_frames)+s->n_frames)%s->n_frames;
  int ti=s->frame[sub];
  if(ti<0 || ti>=R->n_tpag) return 0;
  GmlTpag *t=&R->tpag[ti];
  if(t->atlas<0 || t->atlas>=R->n_atlas) return 0;
  GmlAtlas *at=&R->atlas[t->atlas];
  if(!at->px || at->w<=0 || at->h<=0) return 0;
  if(ot) *ot=t;
  if(oa) *oa=at;
  return 1;
}
static GmlVal sprite_uvs(GmlRender *R, int spr, int img){
  GmlSprite *s=NULL; GmlTpag *t=NULL; GmlAtlas *at=NULL;
  int kind=sprite_tpag_info(R,spr,img,&s,&t,&at);
  if(kind==2 && s && s->w>0 && s->h>0)
    return arr8(0,0,1,1,0,0,1,1);
  if(kind!=1 || !s || !t || !at) return arr8(0,0,0,0,0,0,0,0);
  double aw=at->w, ah=at->h;
  double saved_w = s->w>0 ? (double)t->sw / (double)s->w : 0.0;
  double saved_h = s->h>0 ? (double)t->sh / (double)s->h : 0.0;
  return arr8(t->sx/aw, t->sy/ah, (t->sx+t->sw)/aw, (t->sy+t->sh)/ah,
              t->tx, t->ty, saved_w, saved_h);
}
static int texture_info(GmlRender *R, int tex, double *uw, double *uh, double *tw, double *th){
  if(uw) *uw=0;
  if(uh) *uh=0;
  if(tw) *tw=0;
  if(th) *th=0;
  uint32_t u=(uint32_t)tex, kind=u&GML_TEX_KIND_MASK;
  if(kind==GML_TEX_SPR_TAG){
    int spr=(int)((u>>10)&0xFFFF), img=(int)(u&0x3FF);
    GmlSprite *s=NULL; GmlTpag *t=NULL; GmlAtlas *at=NULL;
    int k=sprite_tpag_info(R,spr,img,&s,&t,&at);
    if(k==2 && s && s->w>0 && s->h>0){
      if(uw) *uw=1;
      if(uh) *uh=1;
      if(tw) *tw=1.0/s->w;
      if(th) *th=1.0/s->h;
      return 1;
    }
    if(k==1 && t && at){
      if(uw) *uw=(double)t->sw/(double)at->w;
      if(uh) *uh=(double)t->sh/(double)at->h;
      if(tw) *tw=1.0/(double)at->w;
      if(th) *th=1.0/(double)at->h;
      return 1;
    }
    return 0;
  }
  if(kind==GML_TEX_SURF_TAG){
    int sid=(int)(u&0xFFFF), w=R?gml_surface_width(R,sid):0, h=R?gml_surface_height(R,sid):0;
    if(w<=0 || h<=0) return 0;
    if(uw) *uw=1;
    if(uh) *uh=1;
    if(tw) *tw=1.0/w;
    if(th) *th=1.0/h;
    return 1;
  }
  return 0;
}
static GmlVal texture_uvs(GmlRender *R, int tex){
  uint32_t u=(uint32_t)tex, kind=u&GML_TEX_KIND_MASK;
  if(kind==GML_TEX_SPR_TAG)
    return sprite_uvs(R,(int)((u>>10)&0xFFFF),(int)(u&0x3FF));
  if(kind==GML_TEX_SURF_TAG)
    return array4(0,0,1,1);
  return array4(0,0,0,0);
}
static GmlVal string_filter_ascii(const char *s, int digits){
  size_t n=s?strlen(s):0;
  char *o=malloc(n+1);
  if(!o) return vstr("");
  size_t k=0;
  for(size_t i=0;i<n;i++){
    unsigned char c=(unsigned char)s[i];
    if((c>='A'&&c<='Z') || (c>='a'&&c<='z') || (digits && c>='0'&&c<='9'))
      o[k++]=(char)c;
  }
  o[k]=0;
  return vstr_owned(o);
}
static int gm_string_count(const char *needle, const char *hay){
  if(!needle || !hay || !needle[0]) return 0;
  int n=0;
  size_t nl=strlen(needle);
  const char *p=hay;
  while((p=strstr(p,needle))){
    n++;
    p += nl;
  }
  return n;
}
static void gm_datetime_tm(double d, struct tm *out){
  memset(out,0,sizeof(*out));
  double sec=(d-25569.0)*86400.0;
  time_t tt=(time_t)(sec>=0 ? sec+0.5 : sec-0.5);
#ifdef _WIN32
  struct tm *p=localtime(&tt);
  if(p) *out=*p;
#else
  localtime_r(&tt,out);
#endif
}
/* runtime layer/element pools: linear scan is fine (dozens of layers, thousands of tiles
 * touched only through the compat scripts, never per-frame). Freed slots are reused. */
GmlRtLayer *gml_rt_layer_find(GmlVM *vm, int id){
  for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used && vm->rtl[i].id==id) return &vm->rtl[i];
  return NULL;
}
GmlRtLayer *gml_rt_layer_find_by_name(GmlVM *vm, const char *nm){
  if(!nm) return NULL;
  for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used && !strcmp(vm->rtl[i].name,nm)) return &vm->rtl[i];
  return NULL;
}
/* Resolve layer arguments from numeric IDs or names. */
static GmlRtLayer *rt_layer_resolve(GmlVM *vm, GmlVal *a, int n){
  if(n>0 && a[0].t==V_STR && a[0].s) return gml_rt_layer_find_by_name(vm,a[0].s);
  return gml_rt_layer_find(vm,(int)N(a,n,0));
}
/* First time GML sets a layer's x/y/hspeed/vspeed at runtime, freeze its accumulated scroll position
 * (until now the draw derived it as x0 + hspeed*elapsed from the room definition) and switch it to
 * per-step accumulation. Untouched layers keep the exact room-def model, so non-scripted games stay
 * byte-identical. */
static void rt_layer_touch(GmlVM *vm, GmlRtLayer *l){
  if(!l || l->touched) return;
  extern long g_vm_frame; long fin=g_vm_frame - vm->room_enter_frame; if(fin<0) fin=0;
  l->x += l->hs*fin; l->y += l->vs*fin; l->touched=1;
}
GmlRtElem *gml_rt_elem_find(GmlVM *vm, int id){
  for(int i=0;i<vm->n_rte;i++) if(vm->rte[i].used && vm->rte[i].id==id) return &vm->rte[i];
  return NULL;
}
GmlRtLayer *gml_rt_layer_new(GmlVM *vm){
  int slot=-1;
  for(int i=0;i<vm->n_rtl;i++) if(!vm->rtl[i].used){ slot=i; break; }
  if(slot<0){
    if(vm->n_rtl>=vm->cap_rtl){ int nc=vm->cap_rtl?vm->cap_rtl*2:16;
      GmlRtLayer *nl=realloc(vm->rtl,(size_t)nc*sizeof(*nl)); if(!nl) return NULL;
      vm->rtl=nl; vm->cap_rtl=nc; }
    slot=vm->n_rtl++;
  }
  GmlRtLayer *l=&vm->rtl[slot]; memset(l,0,sizeof *l);
  if(!vm->rt_next_id) vm->rt_next_id=1000001;
  l->id=vm->rt_next_id++; l->used=1; l->visible=1; l->order=slot;
  l->script_begin=-1; l->script_end=-1;
  return l;
}
GmlRtElem *gml_rt_elem_new(GmlVM *vm){
  int slot=-1;
  for(int i=0;i<vm->n_rte;i++) if(!vm->rte[i].used){ slot=i; break; }
  if(slot<0){
    if(vm->n_rte>=vm->cap_rte){ int nc=vm->cap_rte?vm->cap_rte*2:64;
      GmlRtElem *ne=realloc(vm->rte,(size_t)nc*sizeof(*ne)); if(!ne) return NULL;
      vm->rte=ne; vm->cap_rte=nc; }
    slot=vm->n_rte++;
  }
  GmlRtElem *e=&vm->rte[slot]; memset(e,0,sizeof *e);
  if(!vm->rt_next_id) vm->rt_next_id=1000001;
  e->id=vm->rt_next_id++; e->used=1; e->visible=1;
  e->xs=1; e->ys=1; e->alpha=1; e->blend=0xFFFFFF;
  return e;
}
static GmlRtElem *rt_sprite_for_layer_name(GmlVM *vm, int layer_id, const char *name){
  if(!vm || !name) return NULL;
  for(int i=0;i<vm->n_rte;i++){
    GmlRtElem *e=&vm->rte[i];
    if(e->used && e->type==3 && e->layer==layer_id && !strcmp(e->name,name)) return e;
  }
  return NULL;
}
static GmlRtElem *rt_background_for_layer(GmlVM *vm, GmlRtLayer *l, int create_from_room){
  if(!vm || !l) return NULL;
  for(int i=0;i<vm->n_rte;i++){
    GmlRtElem *e=&vm->rte[i];
    if(e->used && e->type==1 && e->layer==l->id) return e;
  }
  if(!create_from_room || !vm->win || vm->win->bytecode<17 || vm->room_index<0) return NULL;
  const GmlChunk *rc=gml_chunk(vm->win,"ROOM");
  const uint8_t *d=vm->win->data;
  uint32_t rp=rc ? u32(d,rc->off+4+(uint32_t)vm->room_index*4) : 0;
  uint32_t lay=(rp && rp+92<vm->win->size) ? u32(d,rp+88) : 0;
  uint32_t lcnt=(lay && lay+4<vm->win->size) ? u32(d,lay) : 0;
  if(!lcnt || lcnt>=512) return NULL;
  for(uint32_t i=0;i<lcnt;i++){
    uint32_t lp=u32(d,lay+4+i*4);
    if(!lp || u32(d,lp+8)!=1) continue;
    uint32_t np=u32(d,lp+0);
    if(!np || np>=vm->win->size || strcmp((const char*)(d+np),l->name)) continue;
    uint32_t b=gml_room_layer_type_off(vm,lp);
    if(b+28>vm->win->size) continue;
    int spr=(int32_t)u32(d,b+8);
    GmlRtElem *e=gml_rt_elem_new(vm);
    if(!e) return NULL;
    e->type=1;
    e->layer=l->id;
    e->visible=u32(d,b)?1:0;
    e->sprite=spr;
    e->htiled=(int)u32(d,b+12);
    e->vtiled=(int)u32(d,b+16);
    e->stretch=(int)u32(d,b+20);
    uint32_t col=u32(d,b+24);
    e->blend=col&0xFFFFFFu;
    e->alpha=((col>>24)&0xFF)/255.0;
    return e;
  }
  return NULL;
}
static int builtin_layer_exact(GmlVM *vm, const char *nm, GmlVal *a, int n, GmlVal *out){
  if(!vm || !nm || !out) return 0;
  if(!strcmp(nm,"layer_create")){
    GmlRtLayer *l=gml_rt_layer_new(vm);
    if(!l){ *out=vreal(-1); return 1; }
    l->depth=N(a,n,0);
    if(getenv("GML_LOG_RTL")){ extern long g_vm_frame;
      fprintf(stderr,"[rtl] f%ld layer_create depth=%f id=%d\n",g_vm_frame,l->depth,l->id); }
    snprintf(l->name,sizeof l->name,"%s",(n>=2 && a[1].t==V_STR && a[1].s)?a[1].s:"_rt_layer");
    *out=vreal(l->id); return 1;
  }
  if(!strcmp(nm,"layer_destroy")){
    GmlRtLayer *l=gml_rt_layer_find(vm,(int)N(a,n,0));
    if(l){ for(int i=0;i<vm->n_rte;i++) if(vm->rte[i].used && vm->rte[i].layer==l->id) vm->rte[i].used=0;
      l->used=0; }
    *out=vreal(0); return 1;
  }
  if(!strcmp(nm,"layer_get_all")){
    int cnt=0; for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used) cnt++;
    GmlVal v=arr_newv(cnt); if(v.t!=V_ARR){ *out=v; return 1; }
    GmlArr *A=(GmlArr*)v.arr; int k=0;
    for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used) A->data[k++]=vreal(vm->rtl[i].id);
    *out=v; return 1;
  }
  if(!strcmp(nm,"layer_get_all_elements")){
    int lid=(int)N(a,n,0), cnt=0;
    for(int i=0;i<vm->n_rte;i++) if(vm->rte[i].used && vm->rte[i].layer==lid) cnt++;
    GmlVal v=arr_newv(cnt); if(v.t!=V_ARR){ *out=v; return 1; }
    GmlArr *A=(GmlArr*)v.arr; int k=0;
    for(int i=0;i<vm->n_rte;i++) if(vm->rte[i].used && vm->rte[i].layer==lid) A->data[k++]=vreal(vm->rte[i].id);
    *out=v; return 1;
  }
  if(!strcmp(nm,"layer_get_element_type")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0));
    *out=vreal(e?e->type:-1); return 1; }
  if(!strcmp(nm,"layer_get_element_layer")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0));
    *out=vreal(e?e->layer:-1); return 1; }
  if(!strcmp(nm,"layer_get_depth")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); *out=vreal(l?l->depth:0); return 1; }
  if(!strcmp(nm,"layer_depth")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n);
    if(l){
      double old=l->depth;
      l->depth=N(a,n,1);
      if(getenv("GML_LOG_RTL")){ extern long g_vm_frame;
        fprintf(stderr,"[rtl] f%ld layer_depth %s id=%d %.0f -> %.0f\n",
                g_vm_frame,l->name,l->id,old,l->depth); }
    }else if(getenv("GML_LOG_RTL")){
      extern long g_vm_frame;
      fprintf(stderr,"[rtl] f%ld layer_depth unresolved arg0=%s value=%.0f\n",
              g_vm_frame,S(a,n,0),N(a,n,1));
    }
    *out=vreal(0); return 1;
  }
  if(!strcmp(nm,"layer_get_name")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n);
    *out=l?vstr_owned(strdup(l->name)):vstr(""); return 1; }
  if(!strcmp(nm,"layer_get_id")){
    const char *name=S(a,n,0);
    for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used && !strcmp(vm->rtl[i].name,name)){ *out=vreal(vm->rtl[i].id); return 1; }
    *out=vreal(-1); return 1;
  }
  if(!strcmp(nm,"layer_exists")){ *out=vreal(rt_layer_resolve(vm,a,n)!=NULL); return 1; }
  if(!strcmp(nm,"layer_set_visible")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(l) l->visible=(int)N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_get_visible")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); *out=vreal(l?l->visible:0); return 1; }
  if(!strcmp(nm,"layer_x")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(l){ rt_layer_touch(vm,l); l->x=N(a,n,1); } *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_y")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(l){ rt_layer_touch(vm,l); l->y=N(a,n,1); } *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_hspeed")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(l){ rt_layer_touch(vm,l); l->hs=N(a,n,1); } *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_vspeed")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(l){ rt_layer_touch(vm,l); l->vs=N(a,n,1); } *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_get_x")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(!l){ *out=vreal(0); return 1; }
    if(l->touched){ *out=vreal(l->x); return 1; }
    extern long g_vm_frame; long fin=g_vm_frame-vm->room_enter_frame; if(fin<0)fin=0; *out=vreal(l->x+l->hs*fin); return 1; }
  if(!strcmp(nm,"layer_get_y")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(!l){ *out=vreal(0); return 1; }
    if(l->touched){ *out=vreal(l->y); return 1; }
    extern long g_vm_frame; long fin=g_vm_frame-vm->room_enter_frame; if(fin<0)fin=0; *out=vreal(l->y+l->vs*fin); return 1; }
  if(!strcmp(nm,"layer_get_hspeed")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); *out=vreal(l?l->hs:0); return 1; }
  if(!strcmp(nm,"layer_get_vspeed")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); *out=vreal(l?l->vs:0); return 1; }
  if(!strcmp(nm,"layer_force_draw_depth")||!strcmp(nm,"layer_reset_target_all")){
    *out=vreal(0); return 1;
  }

  if(!strcmp(nm,"layer_sprite_get_id")){
    GmlRtLayer *l=rt_layer_resolve(vm,a,n);
    GmlRtElem *e=rt_sprite_for_layer_name(vm,l?l->id:-1,S(a,n,1));
    *out=vreal(e?e->id:-1); return 1;
  }
  if(!strcmp(nm,"layer_sprite_create")){
    GmlRtLayer *l=rt_layer_resolve(vm,a,n);
    if(!l){ *out=vreal(-1); return 1; }
    GmlRtElem *e=gml_rt_elem_new(vm);
    if(!e){ *out=vreal(-1); return 1; }
    e->type=3; e->layer=l->id; e->x=N(a,n,1); e->y=N(a,n,2); e->sprite=(int)N(a,n,3);
    *out=vreal(e->id); return 1;
  }
  if(!strcmp(nm,"layer_sprite_destroy")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->used=0; *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_sprite_exists")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e && e->type==3); return 1; }
  if(!strcmp(nm,"layer_sprite_x")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->x=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_sprite_y")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->y=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_sprite_alpha")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->alpha=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_sprite_speed")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->image_speed=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_sprite_index")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->image_index=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_sprite_change")||!strcmp(nm,"layer_sprite_sprite")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->sprite=(int)N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_sprite_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->xs=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_sprite_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->ys=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_sprite_angle")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->image_angle=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_sprite_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->blend=(uint32_t)N(a,n,1)&0xFFFFFFu; *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_sprite_visible")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->visible=(int)N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_sprite_get_x")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal((e && e->type==3)?e->x:0); return 1; }
  if(!strcmp(nm,"layer_sprite_get_y")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal((e && e->type==3)?e->y:0); return 1; }
  if(!strcmp(nm,"layer_sprite_get_sprite")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal((e && e->type==3)?e->sprite:-1); return 1; }
  if(!strcmp(nm,"layer_sprite_get_alpha")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal((e && e->type==3)?e->alpha:0); return 1; }
  if(!strcmp(nm,"layer_sprite_get_speed")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal((e && e->type==3)?e->image_speed:0); return 1; }
  if(!strcmp(nm,"layer_sprite_get_index")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal((e && e->type==3)?e->image_index:0); return 1; }
  if(!strcmp(nm,"layer_sprite_get_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal((e && e->type==3)?e->xs:1); return 1; }
  if(!strcmp(nm,"layer_sprite_get_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal((e && e->type==3)?e->ys:1); return 1; }
  if(!strcmp(nm,"layer_sprite_get_angle")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal((e && e->type==3)?e->image_angle:0); return 1; }
  if(!strcmp(nm,"layer_sprite_get_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal((e && e->type==3)?(double)e->blend:0xFFFFFF); return 1; }
  if(!strcmp(nm,"layer_sprite_get_visible")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e && e->type==3 && e->visible); return 1; }
  if(!strcmp(nm,"layer_sprite_get_name")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=(e && e->type==3)?vstr_owned(strdup(e->name)):vstr(""); return 1; }

  if(!strcmp(nm,"layer_tile_create")){
    GmlRtElem *e=gml_rt_elem_new(vm);
    if(!e){ *out=vreal(-1); return 1; }
    e->type=7; e->layer=(int)N(a,n,0);
    e->x=N(a,n,1); e->y=N(a,n,2); e->sprite=(int)N(a,n,3);
    e->sx=(int)N(a,n,4); e->sy=(int)N(a,n,5); e->w=(int)N(a,n,6); e->h=(int)N(a,n,7);
    *out=vreal(e->id); return 1;
  }
  if(!strcmp(nm,"layer_tile_destroy")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->used=0; *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_tile_exists")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e && e->type==7); return 1; }
  if(!strcmp(nm,"layer_tile_change")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->sprite=(int)N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_tile_x")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->x=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_tile_y")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->y=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_tile_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->xs=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_tile_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->ys=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_tile_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->blend=(uint32_t)N(a,n,1)&0xFFFFFF; *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_tile_alpha")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->alpha=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_tile_visible")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->visible=(int)N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_tile_region")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0));
    if(e){ e->sx=(int)N(a,n,1); e->sy=(int)N(a,n,2); e->w=(int)N(a,n,3); e->h=(int)N(a,n,4); } *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_tile_get_sprite")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->sprite:-1); return 1; }
  if(!strcmp(nm,"layer_tile_get_x")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->x:0); return 1; }
  if(!strcmp(nm,"layer_tile_get_y")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->y:0); return 1; }
  if(!strcmp(nm,"layer_tile_get_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->xs:1); return 1; }
  if(!strcmp(nm,"layer_tile_get_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->ys:1); return 1; }
  if(!strcmp(nm,"layer_tile_get_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?(double)e->blend:0xFFFFFF); return 1; }
  if(!strcmp(nm,"layer_tile_get_alpha")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->alpha:1); return 1; }
  if(!strcmp(nm,"layer_tile_get_visible")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->visible:0); return 1; }
  if(!strcmp(nm,"layer_tile_get_region")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0));
    *out=e?array4(e->sx,e->sy,e->w,e->h):array4(0,0,0,0); return 1; }

  if(!strcmp(nm,"layer_background_create")){
    GmlRtElem *e=gml_rt_elem_new(vm);
    if(!e){ *out=vreal(-1); return 1; }
    e->type=1; e->layer=(int)N(a,n,0); e->sprite=(int)N(a,n,1);
    *out=vreal(e->id); return 1;
  }
  if(!strcmp(nm,"layer_background_exists")){
    GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,1));
    *out=vreal(e && e->type==1 && e->layer==(int)N(a,n,0)); return 1;
  }
  if(!strcmp(nm,"layer_background_destroy")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->used=0; *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_get_id")){
    GmlRtLayer *l=rt_layer_resolve(vm,a,n);
    GmlRtElem *e=rt_background_for_layer(vm,l,1);
    *out=vreal(e?e->id:-1); return 1;
  }
  if(!strcmp(nm,"layer_background_change")||!strcmp(nm,"layer_background_sprite")){
    GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->sprite=(int)N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_visible")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->visible=(int)N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_alpha")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->alpha=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->blend=(uint32_t)N(a,n,1)&0xFFFFFF; *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_htiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->htiled=(int)N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_vtiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->vtiled=(int)N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_stretch")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->stretch=(int)N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->xs=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->ys=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_index")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->sx=(int)N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_speed")){ *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_get_sprite")||!strcmp(nm,"layer_background_get_index")){
    GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->sprite:-1); return 1; }
  if(!strcmp(nm,"layer_background_get_visible")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->visible:0); return 1; }
  if(!strcmp(nm,"layer_background_get_alpha")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->alpha:1); return 1; }
  if(!strcmp(nm,"layer_background_get_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?(double)e->blend:0xFFFFFF); return 1; }
  if(!strcmp(nm,"layer_background_get_htiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->htiled:0); return 1; }
  if(!strcmp(nm,"layer_background_get_vtiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->vtiled:0); return 1; }
  if(!strcmp(nm,"layer_background_get_stretch")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->stretch:0); return 1; }
  if(!strcmp(nm,"layer_background_get_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->xs:1); return 1; }
  if(!strcmp(nm,"layer_background_get_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->ys:1); return 1; }
  if(!strcmp(nm,"layer_background_get_speed")){ *out=vreal(0); return 1; }
  return 0;
}
static int path_readable(const char *p){
  FILE *f=fopen(p,"rb");
  if(!f) return 0;
  fclose(f);
  return 1;
}
static char *resolve_content_path(GmlVM *vm, const char *p){
  if(!p || !*p) return strdup("");
  /* Resolve relative paths beneath content_dir when available; otherwise use the current directory. */
  if(p[0]=='/' || (p[0] && p[1]==':')) return strdup(p);
  if(!(vm && vm->win && vm->win->content_dir[0]) && path_readable(p)) return strdup(p);
  if(vm && vm->win && vm->win->content_dir[0]){
    size_t n=strlen(vm->win->content_dir)+1+strlen(p)+1;
    char *out=malloc(n);
    if(!out) return strdup(p);
    snprintf(out,n,"%s/%s",vm->win->content_dir,p);
    return out;
  }
  return strdup(p);
}
static int copy_file_path(const char *src, const char *dst){
  int ok=0;
  FILE *fi=src?fopen(src,"rb"):NULL;
  FILE *fo=(fi&&dst)?fopen(dst,"wb"):NULL;
  if(fi&&fo){
    char buf[8192];
    size_t nr;
    ok=1;
    while((nr=fread(buf,1,sizeof(buf),fi))>0)
      if(fwrite(buf,1,nr,fo)!=nr){ ok=0; break; }
  }
  if(fi) fclose(fi);
  if(fo) fclose(fo);
  return ok;
}
static int log_ds_on(void){ static int on=-1; if(on<0) on=getenv("GML_LOG_DS")!=NULL; return on; }
static int log_col_on(void){
  static int on=-1;
  if(on<0) on=(getenv("GML_LOG_COL") || getenv("GML_LOG_COL_OBJ")) ? 1 : 0;
  return on;
}
static const char *log_col_filter(void){
  static const char *f=(const char*)-1;
  if(f==(const char*)-1){ f=getenv("GML_LOG_COL_OBJ"); if(!f) f=""; }
  return f;
}
static int log_col_match(const char *obj_name){
  if(!log_col_on()) return 0;
  const char *f=log_col_filter();
  return !f || !*f || (obj_name && strstr(obj_name,f));
}
static int log_tilecol_on(void){ static int on=-1; if(on<0) on=getenv("GML_LOG_TILECOL")!=NULL; return on; }
static char *dup_n(const char *s, int n){
  if(n<0) n=0;
  char *o=malloc((size_t)n+1);
  if(!o) return strdup("");
  if(n) memcpy(o,s,(size_t)n);
  o[n]=0;
  return o;
}
static int vm_file_slot(GmlVM *vm, int id){
  int i=id-1;
  return (i>=0 && i<16 && vm->bin_file[i]) ? i : -1;
}
static int vm_file_open(GmlVM *vm, const char *path, const char *mode){
  FILE *f=fopen(path,mode); if(!f) return -1;
  for(int i=0;i<16;i++) if(!vm->bin_file[i]){ vm->bin_file[i]=f; return i+1; }
  fclose(f); return -1;
}
static int vm_buffer_slot(GmlVM *vm, int id){
  int i=id-1;
  return (i>=0 && i<16 && vm->buffer[i].live) ? i : -1;
}
static int vm_buffer_slot_from_addr(GmlVM *vm, double addr){
  uint32_t a=(uint32_t)(uint64_t)addr;
  if((a & 0xFFFF0000u) != 0xB0000000u) return -1;
  return vm_buffer_slot(vm,(int)(a & 0xFFFFu));
}
static char *ds_key_make(GmlVal v){
  char buf[96];
  if(v.t==V_STR){
    const char *s=v.s?v.s:"";
    size_t n=strlen(s);
    char *o=malloc(n+3);
    if(!o) return strdup("s:");
    o[0]='s'; o[1]=':'; memcpy(o+2,s,n+1);
    return o;
  }
  if(v.t==V_UNDEF) return strdup("u:");
  snprintf(buf,sizeof(buf),"r:%.17g",v.t==V_REAL?v.d:0.0);
  return strdup(buf);
}
typedef struct { char buf[160]; char *heap; } DsKeyTemp;
static const char *ds_key_temp(GmlVal v, DsKeyTemp *t){
  if(!t) return NULL;
  t->heap=NULL;
  if(v.t==V_STR){
    const char *s=v.s?v.s:"";
    size_t n=strlen(s);
    if(n+3<=sizeof(t->buf)){
      t->buf[0]='s'; t->buf[1]=':'; memcpy(t->buf+2,s,n+1);
      return t->buf;
    }
    t->heap=malloc(n+3);
    if(!t->heap){
      t->buf[0]='s'; t->buf[1]=':'; t->buf[2]=0;
      return t->buf;
    }
    t->heap[0]='s'; t->heap[1]=':'; memcpy(t->heap+2,s,n+1);
    return t->heap;
  }
  if(v.t==V_UNDEF) return "u:";
  snprintf(t->buf,sizeof(t->buf),"r:%.17g",v.t==V_REAL?v.d:0.0);
  return t->buf;
}
static void ds_key_temp_free(DsKeyTemp *t){
  if(!t) return;
  free(t->heap);
  t->heap=NULL;
}
static char *ds_key_temp_steal(DsKeyTemp *t){
  if(!t) return NULL;
  char *heap=t->heap;
  t->heap=NULL;
  return heap;
}
static GmlVal ds_key_val_clone(GmlVal v){
  /* Copy stored strings so they survive the caller temporary-string cleanup. */
  if(v.t==V_STR){ char *c=v.s?strdup(v.s):NULL; return c?vstr_owned(c):vstr(""); }
  if(v.t==V_UNDEF) return vundef();
  return vreal(v.t==V_REAL?v.d:0.0);
}
/* Free the lookup key only. Retain key/value strings because callers may hold non-owned references. */
static void ds_entry_free(GmlDSMapEntry *e){
  free(e->key);
}
/* Return stored strings as non-owned references, outside the temporary result tracker. */
static GmlVal ds_ret(GmlVal v){ if(v.t==V_STR) v.d=0; return v; }
static GmlVal ds_val_clone(GmlVal v){
  /* Copy stored strings independently of the source scope. */
  if(v.t==V_STR){ char *c=v.s?strdup(v.s):NULL; return c?vstr_owned(c):vstr(""); }
  if(v.t==V_ARR){ gml_arr_mark_escaped(v); return v; }   /* ds structures outlive the scope */
  if(v.t==V_UNDEF) return vundef();
  return vreal(v.t==V_REAL?v.d:0.0);
}
static GmlDSMap *ds_map_slot(GmlVM *vm, int id){
  if(!vm) return NULL;
  int li=vm->ds_map_last_slot;
  if(li>=0 && li<GML_DS_MAP_MAX && vm->ds_map[li].live && (int)vm->ds_map[li].id==id)
    return &vm->ds_map[li];
  for(int i=0;i<GML_DS_MAP_MAX;i++) if(vm->ds_map[i].live && (int)vm->ds_map[i].id==id){
    vm->ds_map_last_slot=i;
    return &vm->ds_map[i];
  }
  return NULL;
}
static int ds_map_create_id(GmlVM *vm){
  for(int i=0;i<GML_DS_MAP_MAX;i++) if(!vm->ds_map[i].live){
    vm->ds_map[i].live=1;
    vm->ds_map[i].id=(uint32_t)vm->next_ds_id++;
    vm->ds_map[i].last_lookup=-1;
    vm->ds_map_last_slot=i;
    if(vm->next_ds_id<=0) vm->next_ds_id=1;
    return (int)vm->ds_map[i].id;
  }
  return -1;
}
/* Ordered value lists share the next_ds_id counter with maps. */
static GmlDSList *ds_list_slot(GmlVM *vm, int id){
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(vm->ds_list[i].live && (int)vm->ds_list[i].id==id) return &vm->ds_list[i];
  return NULL;
}
static GmlDSList *ds_list_slot_repair(GmlVM *vm, int id){
  GmlDSList *l=ds_list_slot(vm,id);
  if(l || !vm->ds_list_compat_repair || id<=0 || id>=vm->next_ds_id) return l;
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(!vm->ds_list[i].live){
    vm->ds_list[i].live=1;
    vm->ds_list[i].id=(uint32_t)id;
    vm->ds_list[i].len=0;
    vm->ds_list[i].cap=0;
    vm->ds_list[i].item=NULL;
    return &vm->ds_list[i];
  }
  return NULL;
}
static int ds_list_create_id(GmlVM *vm){
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(!vm->ds_list[i].live){
    vm->ds_list[i].live=1; vm->ds_list[i].len=0;
    vm->ds_list[i].id=(uint32_t)vm->next_ds_id++;
    if(vm->next_ds_id<=0) vm->next_ds_id=1;
    return (int)vm->ds_list[i].id;
  }
  return -1;
}
static void ds_list_push(GmlDSList *l, GmlVal v){
  if(!l) return;
  if(l->len>=l->cap){ int nc=l->cap?l->cap*2:8; GmlVal *ni=realloc(l->item,(size_t)nc*sizeof(GmlVal)); if(!ni) return; l->item=ni; l->cap=nc; }
  l->item[l->len++]=v;
}
static int ds_val_equal(GmlVal a, GmlVal b){
  if(a.t==V_UNDEF || b.t==V_UNDEF) return a.t==b.t;
  if(a.t==V_STR || b.t==V_STR) return !strcmp(gm_string_tmp(a),gm_string_tmp(b));
  return fabs((a.t==V_REAL?a.d:0.0)-(b.t==V_REAL?b.d:0.0))<1e-9;
}
static GmlVal ds_priority_entry(GmlVal val, double pri){
  GmlVal e=arr_newv(2);
  if(e.t!=V_ARR || !e.arr) return ds_val_clone(val);
  GmlArr *A=(GmlArr*)e.arr;
  A->data[0]=ds_val_clone(val);
  A->data[1]=vreal(pri);
  gml_arr_mark_escaped(e);
  return e;
}
static GmlVal ds_priority_value(GmlVal e){
  if(e.t==V_ARR && e.arr){
    GmlArr *A=(GmlArr*)e.arr;
    if(A->len>0) return ds_ret(A->data[0]);
  }
  return ds_ret(e);
}
static double ds_priority_priority(GmlVal e){
  if(e.t==V_ARR && e.arr){
    GmlArr *A=(GmlArr*)e.arr;
    if(A->len>1) return N(&A->data[1],1,0);
  }
  return 0;
}
static int ds_priority_best_index(GmlDSList *l, int want_max){
  if(!l || l->len<=0) return -1;
  int bi=0;
  double bp=ds_priority_priority(l->item[0]);
  for(int i=1;i<l->len;i++){
    double p=ds_priority_priority(l->item[i]);
    if((want_max && p>bp) || (!want_max && p<bp)){ bp=p; bi=i; }
  }
  return bi;
}
static void ds_grid_store(GmlDSGrid *g, int x, int y, GmlVal v){
  if(g && g->cell && x>=0 && y>=0 && x<g->w && y<g->h)
    g->cell[(size_t)y*g->w+x]=ds_val_clone(v);
}
/* Two-dimensional value grids. */
static GmlDSGrid *ds_grid_slot(GmlVM *vm, int id){
  for(int i=0;i<32;i++) if(vm->ds_grid[i].live && (int)vm->ds_grid[i].id==id) return &vm->ds_grid[i];
  return NULL;
}
static int ds_grid_make(GmlVM *vm, int w, int h){
  if(w<0)w=0; if(h<0)h=0; if((long)w*h>8000000) return -1;
  for(int i=0;i<32;i++) if(!vm->ds_grid[i].live){
    vm->ds_grid[i].live=1; vm->ds_grid[i].w=w; vm->ds_grid[i].h=h;
    vm->ds_grid[i].cell=(w&&h)?calloc((size_t)w*h,sizeof(GmlVal)):NULL;
    vm->ds_grid[i].id=(uint32_t)vm->next_ds_id++;
    if(vm->next_ds_id<=0) vm->next_ds_id=1;
    return (int)vm->ds_grid[i].id;
  }
  return -1;
}
/* Use linear lookup for small maps and a lazy open-addressed index for larger maps. */
static uint32_t ds_key_hash(const char *k){
  uint32_t h=2166136261u; while(*k){ h^=(unsigned char)*k++; h*=16777619u; } return h;
}
static void ds_map_index_free(GmlDSMap *m){ free(m->hidx); m->hidx=NULL; m->hcap=0; m->hdirty=0; }
static void ds_map_index_rebuild(GmlDSMap *m){
  ds_map_index_free(m);
  int hc=64; while(hc < m->len*2) hc*=2;
  m->hidx=malloc((size_t)hc*sizeof(int));
  if(!m->hidx) return;
  for(int i=0;i<hc;i++) m->hidx[i]=-1;
  m->hcap=hc;
  for(int i=0;i<m->len;i++){
    if(!m->entry[i].key) continue;
    uint32_t h=ds_key_hash(m->entry[i].key)&(uint32_t)(hc-1);
    while(m->hidx[h]>=0) h=(h+1)&(uint32_t)(hc-1);
    m->hidx[h]=i;
  }
  m->hdirty=0;
}
static void ds_map_index_add(GmlDSMap *m, int i){
  if(!m->hidx || m->hdirty) return;
  if(m->len*2 > m->hcap){ ds_map_index_rebuild(m); return; }
  uint32_t h=ds_key_hash(m->entry[i].key)&(uint32_t)(m->hcap-1);
  while(m->hidx[h]>=0) h=(h+1)&(uint32_t)(m->hcap-1);
  m->hidx[h]=i;
}
static int ds_map_find_entry(GmlDSMap *m, const char *key){
  if(!m||!key) return -1;
  int li=m->last_lookup;
  if(li>=0 && li<m->len && m->entry[li].key && !strcmp(m->entry[li].key,key)) return li;
  int ni=li+1;
  if(ni>=0 && ni<m->len && m->entry[ni].key && !strcmp(m->entry[ni].key,key)){
    m->last_lookup=ni;
    return ni;
  }
  int pi=li-1;
  if(pi>=0 && pi<m->len && m->entry[pi].key && !strcmp(m->entry[pi].key,key)){
    m->last_lookup=pi;
    return pi;
  }
  if(m->len>=48){
    if(!m->hidx || m->hdirty) ds_map_index_rebuild(m);
    if(m->hidx){
      uint32_t h=ds_key_hash(key)&(uint32_t)(m->hcap-1);
      while(m->hidx[h]>=0){
        int i=m->hidx[h];
        if(i<m->len && m->entry[i].key && !strcmp(m->entry[i].key,key)){
          m->last_lookup=i;
          return i;
        }
        h=(h+1)&(uint32_t)(m->hcap-1);
      }
      return -1;
    }
  }
  for(int i=0;i<m->len;i++) if(m->entry[i].key && !strcmp(m->entry[i].key,key)){
    m->last_lookup=i;
    return i;
  }
  return -1;
}
static int ds_map_reserve(GmlDSMap *m, int n){
  if(n<=m->cap) return 1;
  int nc=m->cap?m->cap:8; while(nc<n) nc*=2;
  GmlDSMapEntry *e=realloc(m->entry,(size_t)nc*sizeof(*e));
  if(!e) return 0;
  memset(e+m->cap,0,(size_t)(nc-m->cap)*sizeof(*e));
  m->entry=e; m->cap=nc; return 1;
}
static int ds_map_put(GmlVM *vm, int id, GmlVal keyv, GmlVal val, int overwrite){
  GmlDSMap *m=ds_map_slot(vm,id);
  if(!m) return 0;
  DsKeyTemp kt;
  const char *lookup=ds_key_temp(keyv,&kt);
  int i=ds_map_find_entry(m,lookup);
  if(i>=0){
    if(overwrite) m->entry[i].val=ds_val_clone(val);   /* old val leaks: refs may have escaped via ds_ret */
    ds_key_temp_free(&kt);
    return 1;
  }
  char *key=ds_key_temp_steal(&kt);
  if(!key) key=ds_key_make(keyv);
  ds_key_temp_free(&kt);
  if(!key) return 0;
  if(!ds_map_reserve(m,m->len+1)){ free(key); return 0; }
  m->entry[m->len].key=key;
  m->entry[m->len].key_val=ds_key_val_clone(keyv);
  m->entry[m->len].val=ds_val_clone(val);
  m->len++;
  ds_map_index_add(m,m->len-1);
  return 1;
}
static GmlVal ds_map_lookup_s(GmlVM *vm, int id, const char *key, int *ok){
  if(ok) *ok=0;
  GmlDSMap *m=ds_map_slot(vm,id);
  DsKeyTemp kt;
  const char *dk=ds_key_temp(vstr(key?key:""),&kt);
  int i=ds_map_find_entry(m,dk);
  ds_key_temp_free(&kt);
  if(i<0) return vundef();
  if(ok) *ok=1;
  return ds_ret(m->entry[i].val);
}
static void inst_store_val(GmlInstance *in, const char *key, GmlVal v){
  if(!in || !key) return;
  GmlVal *p=gml_varmap_get(&in->vars,key);
  if(p){ *p=v; return; }
  char *owned=strdup(key);
  if(!owned) return;
  *gml_varmap_put(&in->vars,owned)=v;
}
static void inst_store_real(GmlInstance *in, const char *key, double v){
  inst_store_val(in,key,vreal(v));
}
static void inst_store_string(GmlInstance *in, const char *key, const char *s){
  char *copy=strdup(s?s:"");
  if(!copy) return;
  inst_store_val(in,key,vstr(copy));
}
static GmlVal inst_lookup(GmlInstance *in, const char *key){
  GmlVal *p=(in&&key)?gml_varmap_get(&in->vars,key):NULL;
  return p?*p:vundef();
}
static int skel_key(char *out, size_t cap, const char *kind, const char *name, const char *field){
  int n=snprintf(out,cap,"__skel_%s:%s:%s",kind?kind:"",name?name:"",field?field:"");
  return n>=0 && (size_t)n<cap;
}
static double skel_bone_num(GmlInstance *in, const char *kind, const char *bone, const char *field, double def){
  char key[384];
  if(!skel_key(key,sizeof(key),kind,bone,field)) return def;
  GmlVal v=inst_lookup(in,key);
  if(v.t==V_UNDEF) return def;
  return N(&v,1,0);
}
static const char *skel_bone_str(GmlInstance *in, const char *kind, const char *bone, const char *field, const char *def){
  char key[384];
  if(!skel_key(key,sizeof(key),kind,bone,field)) return def;
  GmlVal v=inst_lookup(in,key);
  return v.t==V_STR ? (v.s?v.s:"") : def;
}
static int skel_bone_has(GmlInstance *in, const char *kind, const char *bone, const char *field){
  char key[384];
  return skel_key(key,sizeof(key),kind,bone,field) && inst_lookup(in,key).t!=V_UNDEF;
}
static void skel_store_map_num(GmlVM *vm, GmlInstance *in, int mapid,
                               const char *kind, const char *bone, const char *field, double def){
  int ok=0;
  GmlVal mv=ds_map_lookup_s(vm,mapid,field,&ok);
  double v=ok?N(&mv,1,0):skel_bone_num(in,kind,bone,field,def);
  char key[384];
  if(skel_key(key,sizeof(key),kind,bone,field)) inst_store_real(in,key,v);
}
static void skel_store_map_str(GmlVM *vm, GmlInstance *in, int mapid,
                               const char *kind, const char *bone, const char *field, const char *def){
  int ok=0;
  GmlVal mv=ds_map_lookup_s(vm,mapid,field,&ok);
  const char *v=(ok && mv.t==V_STR) ? (mv.s?mv.s:"") : skel_bone_str(in,kind,bone,field,def);
  char key[384];
  if(skel_key(key,sizeof(key),kind,bone,field)) inst_store_string(in,key,v);
}
static void skel_bone_map_put_base(GmlVM *vm, GmlInstance *in, int mapid, const char *kind, const char *bone){
  ds_map_put(vm,mapid,vstr("x"),vreal(skel_bone_num(in,kind,bone,"x",0)),1);
  ds_map_put(vm,mapid,vstr("y"),vreal(skel_bone_num(in,kind,bone,"y",0)),1);
  ds_map_put(vm,mapid,vstr("angle"),vreal(skel_bone_num(in,kind,bone,"angle",0)),1);
  ds_map_put(vm,mapid,vstr("xscale"),vreal(skel_bone_num(in,kind,bone,"xscale",1)),1);
  ds_map_put(vm,mapid,vstr("yscale"),vreal(skel_bone_num(in,kind,bone,"yscale",1)),1);
  ds_map_put(vm,mapid,vstr("parent"),vstr(skel_bone_str(in,kind,bone,"parent","")),1);
}
static GmlVal builtin_skeleton(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlInstance *in=vm?vm->cur_self:NULL;
  if(getenv("GML_LOG_SKEL")){
    static long c=0;
    if(c<256){
      extern long g_vm_frame;
      fprintf(stderr,"[skel] f%ld self=%u %s",g_vm_frame,in?in->id:0,nm);
      for(int i=0;i<n && i<3;i++){
        if(a[i].t==V_STR) fprintf(stderr,"%s\"%s\"",i?" ":" ",a[i].s?a[i].s:"");
        else fprintf(stderr,"%s%g",i?" ":" ",N(a,n,i));
      }
      fprintf(stderr,"\n");
    }
    c++;
  }
  if(!strcmp(nm,"skeleton_animation_set")){
    if(in) inst_store_string(in,"__skel_animation",S(a,n,0));
    return vreal(0);
  }
  if(!strcmp(nm,"skeleton_animation_get")){
    GmlVal v=inst_lookup(in,"__skel_animation");
    return v.t==V_STR ? vstr(v.s?v.s:"") : vstr("");
  }
  if(!strcmp(nm,"skeleton_animation_mix")){
    if(in){
      char key[384];
      int k=snprintf(key,sizeof(key),"__skel_mix:%s:%s",S(a,n,0),S(a,n,1));
      if(k>=0 && (size_t)k<sizeof(key)) inst_store_real(in,key,N(a,n,2));
    }
    return vreal(0);
  }
  if(!strcmp(nm,"skeleton_skin_set")){
    if(in) inst_store_string(in,"__skel_skin",S(a,n,0));
    return vreal(0);
  }
  if(!strcmp(nm,"skeleton_skin_get")){
    GmlVal v=inst_lookup(in,"__skel_skin");
    return v.t==V_STR ? vstr(v.s?v.s:"") : vstr("");
  }
  if(!strcmp(nm,"skeleton_attachment_set")){
    if(in){
      char key[384];
      int k=snprintf(key,sizeof(key),"__skel_attachment:%s",S(a,n,0));
      if(k>=0 && (size_t)k<sizeof(key)) inst_store_string(in,key,S(a,n,1));
    }
    return vreal(0);
  }
  if(!strcmp(nm,"skeleton_attachment_get")){
    char key[384];
    int k=snprintf(key,sizeof(key),"__skel_attachment:%s",S(a,n,0));
    GmlVal v=(k>=0 && (size_t)k<sizeof(key))?inst_lookup(in,key):vundef();
    return v.t==V_STR ? vstr(v.s?v.s:"") : vstr("");
  }
  if(!strcmp(nm,"skeleton_bone_data_get")){
    if(in && n>=2) skel_bone_map_put_base(vm,in,(int)N(a,n,1),"bone_data",S(a,n,0));
    return vreal(0);
  }
  if(!strcmp(nm,"skeleton_bone_data_set")){
    if(in && n>=2){
      int mapid=(int)N(a,n,1);
      const char *bone=S(a,n,0);
      skel_store_map_num(vm,in,mapid,"bone_data",bone,"x",0);
      skel_store_map_num(vm,in,mapid,"bone_data",bone,"y",0);
      skel_store_map_num(vm,in,mapid,"bone_data",bone,"angle",0);
      skel_store_map_num(vm,in,mapid,"bone_data",bone,"xscale",1);
      skel_store_map_num(vm,in,mapid,"bone_data",bone,"yscale",1);
      skel_store_map_str(vm,in,mapid,"bone_data",bone,"parent","");
    }
    return vreal(0);
  }
  if(!strcmp(nm,"skeleton_bone_state_get")){
    if(in && n>=2){
      int mapid=(int)N(a,n,1);
      const char *bone=S(a,n,0);
      if(!skel_bone_has(in,"bone_state",bone,"angle") &&
         !skel_bone_has(in,"bone_state",bone,"x") &&
         !skel_bone_has(in,"bone_state",bone,"y"))
        return vreal(0);
      double x=skel_bone_num(in,"bone_state",bone,"x",skel_bone_num(in,"bone_data",bone,"x",0));
      double y=skel_bone_num(in,"bone_state",bone,"y",skel_bone_num(in,"bone_data",bone,"y",0));
      double angle=skel_bone_num(in,"bone_state",bone,"angle",skel_bone_num(in,"bone_data",bone,"angle",0));
      double xs=skel_bone_num(in,"bone_state",bone,"xscale",skel_bone_num(in,"bone_data",bone,"xscale",1));
      double ys=skel_bone_num(in,"bone_state",bone,"yscale",skel_bone_num(in,"bone_data",bone,"yscale",1));
      ds_map_put(vm,mapid,vstr("x"),vreal(x),1);
      ds_map_put(vm,mapid,vstr("y"),vreal(y),1);
      ds_map_put(vm,mapid,vstr("angle"),vreal(angle),1);
      ds_map_put(vm,mapid,vstr("xscale"),vreal(xs),1);
      ds_map_put(vm,mapid,vstr("yscale"),vreal(ys),1);
      ds_map_put(vm,mapid,vstr("worldX"),vreal(x),1);
      ds_map_put(vm,mapid,vstr("worldY"),vreal(y),1);
      ds_map_put(vm,mapid,vstr("worldAngleX"),vreal(angle),1);
      ds_map_put(vm,mapid,vstr("worldAngleY"),vreal(angle),1);
      ds_map_put(vm,mapid,vstr("worldScaleX"),vreal(fabs(xs)),1);
      ds_map_put(vm,mapid,vstr("worldScaleY"),vreal(fabs(ys)),1);
      ds_map_put(vm,mapid,vstr("appliedAngle"),vreal(angle),1);
      ds_map_put(vm,mapid,vstr("parent"),vstr(skel_bone_str(in,"bone_state",bone,"parent",
        skel_bone_str(in,"bone_data",bone,"parent",""))),1);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"skeleton_bone_state_set")){
    if(in && n>=2){
      int mapid=(int)N(a,n,1);
      const char *bone=S(a,n,0);
      skel_store_map_num(vm,in,mapid,"bone_state",bone,"x",skel_bone_num(in,"bone_data",bone,"x",0));
      skel_store_map_num(vm,in,mapid,"bone_state",bone,"y",skel_bone_num(in,"bone_data",bone,"y",0));
      skel_store_map_num(vm,in,mapid,"bone_state",bone,"angle",skel_bone_num(in,"bone_data",bone,"angle",0));
      skel_store_map_num(vm,in,mapid,"bone_state",bone,"xscale",skel_bone_num(in,"bone_data",bone,"xscale",1));
      skel_store_map_num(vm,in,mapid,"bone_state",bone,"yscale",skel_bone_num(in,"bone_data",bone,"yscale",1));
      skel_store_map_str(vm,in,mapid,"bone_state",bone,"parent",skel_bone_str(in,"bone_data",bone,"parent",""));
    }
    return vreal(0);
  }
  return vreal(0);
}
static void ds_map_clear_entries(GmlDSMap *m){
  if(!m) return;
  for(int i=0;i<m->len;i++) ds_entry_free(&m->entry[i]);
  m->len=0;
  m->hdirty=1;
  m->last_lookup=-1;
}
static void ds_map_destroy_live(GmlDSMap *m){
  if(!m) return;
  ds_map_clear_entries(m);
  free(m->entry);
  ds_map_index_free(m);
  memset(m,0,sizeof(*m));
}
static int ds_map_replace_from_map(GmlVM *vm, int dst_id, int src_id){
  GmlDSMap *dst=ds_map_slot(vm,dst_id);
  GmlDSMap *src=ds_map_slot(vm,src_id);
  if(!dst||!src) return 0;
  if(dst==src) return 1;
  ds_map_clear_entries(dst);
  for(int i=0;i<src->len;i++) ds_map_put(vm,dst_id,src->entry[i].key_val,src->entry[i].val,1);
  return 1;
}
static void ds_log_val_simple(GmlVal v){
  if(v.t==V_STR) fprintf(stderr,"\"%s\"",v.s?v.s:"");
  else if(v.t==V_ARR) fprintf(stderr,"<array>");
  else if(v.t==V_UNDEF) fprintf(stderr,"undefined");
  else fprintf(stderr,"%g",v.t==V_REAL?v.d:0.0);
}
static void ds_log_val(GmlVM *vm, GmlVal v, int depth){
  if(!getenv("GML_LOG_DS_VERBOSE") || depth>2){
    ds_log_val_simple(v);
    return;
  }
  if(v.t==V_REAL && GML_IS_STRUCT_ID(v.d)){
    GmlInstance *st=gml_struct_find(vm,(unsigned)v.d);
    fprintf(stderr,"{struct id=%u", (unsigned)v.d);
    if(st){
      int shown=0;
      for(int i=0;i<st->vars.cap && shown<12;i++){
        GmlVarSlot *slot=&st->vars.slots[i];
        if(!slot->key) continue;
        fprintf(stderr," %s=",slot->key);
        ds_log_val(vm,slot->val,depth+1);
        shown++;
      }
    }
    fprintf(stderr,"}");
    return;
  }
  if(v.t==V_ARR && v.arr){
    GmlArr *A=(GmlArr*)v.arr;
    fprintf(stderr,"[array len=%d",A->len);
    int max=A->len<6?A->len:6;
    for(int i=0;i<max;i++){
      fprintf(stderr," %d=",i);
      ds_log_val(vm,A->data[i],depth+1);
    }
    if(A->len>max) fprintf(stderr," ...");
    fprintf(stderr,"]");
    return;
  }
  ds_log_val_simple(v);
}
GmlVal gml_ds_map_find_value_direct(GmlVM *vm, int id, GmlVal keyv, int has_key){
  GmlDSMap *m=ds_map_slot(vm,id);
  DsKeyTemp kt={0};
  const char *key=has_key?ds_key_temp(keyv,&kt):NULL;
  int i=ds_map_find_entry(m,key);
  GmlVal out=(i>=0)?m->entry[i].val:vundef();
  if(log_ds_on()){
    fprintf(stderr,"[ds_map_find_value] id=%d live=%d key=%s raw=",id,m?m->len:-1,key?key:"<null>");
    if(has_key) ds_log_val(vm,keyv,0); else fprintf(stderr,"<missing>");
    fprintf(stderr," hit=%d out=",i);
    ds_log_val(vm,out,0);
    fprintf(stderr,"\n");
  }
  ds_key_temp_free(&kt);
  return ds_ret(out);
}
GmlVal gml_ds_map_find_first_direct(GmlVM *vm, int id){
  GmlDSMap *m=ds_map_slot(vm,id);
  return (m && m->len>0) ? ds_ret(m->entry[0].key_val) : vstr("");
}
GmlVal gml_ds_map_find_next_direct(GmlVM *vm, int id, GmlVal keyv, int has_key){
  GmlDSMap *m=ds_map_slot(vm,id);
  DsKeyTemp kt={0};
  const char *key=has_key?ds_key_temp(keyv,&kt):NULL;
  int i=ds_map_find_entry(m,key);
  ds_key_temp_free(&kt);
  int j=(i<0)?-1:i+1;
  return (m && j>=0 && j<m->len) ? ds_ret(m->entry[j].key_val) : vundef();
}
int gml_ds_map_size_direct(GmlVM *vm, int id){
  GmlDSMap *m=ds_map_slot(vm,id);
  return m?m->len:0;
}
static GmlVal ini_default_string(GmlVal *a, int n){
  const char *s = n>2 ? S(a,n,2) : "";
  char *c = strdup(s?s:"");
  return c ? vstr_owned(c) : vstr("");
}

typedef struct { char *s; size_t n, cap; } JsonBuf;
static int jb_reserve(JsonBuf *b, size_t add){
  if(b->n+add+1<=b->cap) return 1;
  size_t nc=b->cap?b->cap*2:128;
  while(nc<b->n+add+1) nc*=2;
  char *ns=realloc(b->s,nc);
  if(!ns) return 0;
  b->s=ns; b->cap=nc; return 1;
}
static int jb_putc(JsonBuf *b, char c){ if(!jb_reserve(b,1)) return 0; b->s[b->n++]=c; b->s[b->n]=0; return 1; }
static int jb_puts(JsonBuf *b, const char *s){
  size_t n=s?strlen(s):0; if(!jb_reserve(b,n)) return 0;
  if(n) memcpy(b->s+b->n,s,n);
  b->n+=n; b->s[b->n]=0; return 1;
}
static int jb_put_json_string(JsonBuf *b, const char *s){
  if(!jb_putc(b,'"')) return 0;
  for(const unsigned char *p=(const unsigned char*)(s?s:""); *p; p++){
    char tmp[8];
    switch(*p){
      case '"': if(!jb_puts(b,"\\\"")) return 0; break;
      case '\\': if(!jb_puts(b,"\\\\")) return 0; break;
      case '\b': if(!jb_puts(b,"\\b")) return 0; break;
      case '\f': if(!jb_puts(b,"\\f")) return 0; break;
      case '\n': if(!jb_puts(b,"\\n")) return 0; break;
      case '\r': if(!jb_puts(b,"\\r")) return 0; break;
      case '\t': if(!jb_puts(b,"\\t")) return 0; break;
      default:
        if(*p<0x20){ snprintf(tmp,sizeof(tmp),"\\u%04x",*p); if(!jb_puts(b,tmp)) return 0; }
        else if(!jb_putc(b,(char)*p)) return 0;
    }
  }
  return jb_putc(b,'"');
}
static int json_encode_val(GmlVM *vm, JsonBuf *b, GmlVal v, int depth, int allow_ds);
static GmlVal var_store_clone(GmlVal v);
static int json_struct_skip_key(const char *key){
  return key && (!strcmp(key,"__fn") || !strcmp(key,"__self") ||
                 !strcmp(key,"__name") || !strcmp(key,"__ctor"));
}
static int json_encode_struct(GmlVM *vm, JsonBuf *b, GmlInstance *st, int depth, int allow_ds){
  if(depth>16) return jb_puts(b,"null");
  if(!st) return jb_puts(b,"null");
  if(!jb_putc(b,'{')) return 0;
  int first=1;
  for(int i=0;i<st->vars.cap;i++){
    GmlVarSlot *slot=&st->vars.slots[i];
    if(!slot->key || json_struct_skip_key(slot->key)) continue;
    if(!first && !jb_putc(b,',')) return 0;
    first=0;
    if(!jb_put_json_string(b,slot->key) || !jb_putc(b,':')) return 0;
    if(!json_encode_val(vm,b,slot->val,depth+1,allow_ds)) return 0;
  }
  return jb_putc(b,'}');
}
static int json_encode_map(GmlVM *vm, JsonBuf *b, int id, int depth){
  if(depth>16) return jb_puts(b,"null");
  GmlDSMap *m=ds_map_slot(vm,id);
  if(!m) return jb_puts(b,"null");
  if(!jb_putc(b,'{')) return 0;
  for(int i=0;i<m->len;i++){
    if(i && !jb_putc(b,',')) return 0;
    char keybuf[64]; const char *key=NULL;
    if(m->entry[i].key_val.t==V_STR) key=m->entry[i].key_val.s?m->entry[i].key_val.s:"";
    else { snprintf(keybuf,sizeof(keybuf),"%.17g",m->entry[i].key_val.t==V_REAL?m->entry[i].key_val.d:0.0); key=keybuf; }
    if(!jb_put_json_string(b,key) || !jb_putc(b,':')) return 0;
    if(!json_encode_val(vm,b,m->entry[i].val,depth+1,0)) return 0;
  }
  return jb_putc(b,'}');
}
static int json_encode_arr(GmlVM *vm, JsonBuf *b, GmlArr *A, int depth){
  if(depth>16) return jb_puts(b,"null");
  if(!A) return jb_puts(b,"[]");
  if(!jb_putc(b,'[')) return 0;
  for(int i=0;i<A->len;i++){
    if(i && !jb_putc(b,',')) return 0;
    if(!json_encode_val(vm,b,A->data[i],depth+1,0)) return 0;
  }
  return jb_putc(b,']');
}
static int json_encode_val(GmlVM *vm, JsonBuf *b, GmlVal v, int depth, int allow_ds){
  if(v.t==V_STR) return jb_put_json_string(b,v.s?v.s:"");
  if(v.t==V_UNDEF) return jb_puts(b,"null");
  if(v.t==V_ARR) return json_encode_arr(vm,b,(GmlArr*)v.arr,depth);
  if(v.t==V_REAL){
    int id=(int)v.d;
    if(GML_IS_STRUCT_ID(v.d)) return json_encode_struct(vm,b,gml_struct_find(vm,(unsigned)v.d),depth,allow_ds);
    if(allow_ds && fabs(v.d-(double)id)<1e-9 && ds_map_slot(vm,id)) return json_encode_map(vm,b,id,depth);
    if(!isfinite(v.d)) return jb_puts(b,"null");
    char num[64]; snprintf(num,sizeof(num),"%.17g",v.d); return jb_puts(b,num);
  }
  return jb_puts(b,"null");
}

typedef struct { const char *s; size_t p, n; GmlVM *vm; int ok, obj_as_struct; } JsonIn;
static void js_ws(JsonIn *j){ while(j->p<j->n && (j->s[j->p]==' '||j->s[j->p]=='\n'||j->s[j->p]=='\r'||j->s[j->p]=='\t')) j->p++; }
static int js_consume(JsonIn *j, char c){ js_ws(j); if(j->p<j->n && j->s[j->p]==c){ j->p++; return 1; } return 0; }
static char *js_string(JsonIn *j){
  js_ws(j); if(j->p>=j->n || j->s[j->p++]!='"'){ j->ok=0; return strdup(""); }
  JsonBuf b={0};
  while(j->p<j->n){
    unsigned char c=(unsigned char)j->s[j->p++];
    if(c=='"') return b.s?b.s:strdup("");
    if(c=='\\'){
      if(j->p>=j->n){ j->ok=0; break; }
      c=(unsigned char)j->s[j->p++];
      if(c=='"'||c=='\\'||c=='/') jb_putc(&b,(char)c);
      else if(c=='b') jb_putc(&b,'\b');
      else if(c=='f') jb_putc(&b,'\f');
      else if(c=='n') jb_putc(&b,'\n');
      else if(c=='r') jb_putc(&b,'\r');
      else if(c=='t') jb_putc(&b,'\t');
      else if(c=='u'){
        int v=0;
        for(int k=0;k<4 && j->p<j->n;k++){
          char h=j->s[j->p++]; v*=16;
          if(h>='0'&&h<='9') v+=h-'0';
          else if(h>='a'&&h<='f') v+=h-'a'+10;
          else if(h>='A'&&h<='F') v+=h-'A'+10;
          else { j->ok=0; break; }
        }
        jb_putc(&b,(v>=0x20 && v<0x80)?(char)v:'?');
      } else { j->ok=0; break; }
    } else jb_putc(&b,(char)c);
  }
  free(b.s); j->ok=0; return strdup("");
}
static int js_arr_push(GmlArr *A, GmlVal v){
  if(A->len>=A->cap){
    int nc=A->cap?A->cap*2:8;
    GmlVal *nd=realloc(A->data,(size_t)nc*sizeof(GmlVal));
    if(!nd) return 0;
    A->data=nd; A->cap=nc;
  }
  A->data[A->len++]=v; return 1;
}
static int sort_dir;
static int gml_val_sort_rank(GmlVal v){
  if(v.t==V_REAL) return 0;
  if(v.t==V_STR) return 1;
  if(v.t==V_ARR) return 2;
  return 3;
}
static int gml_val_sort_cmp(const void *pa, const void *pb){
  const GmlVal *a=(const GmlVal*)pa, *b=(const GmlVal*)pb;
  int r=0;
  if(a->t==V_REAL && b->t==V_REAL){
    double ad=a->d, bd=b->d;
    if(isnan(ad) && isnan(bd)) r=0;
    else if(isnan(ad)) r=1;
    else if(isnan(bd)) r=-1;
    else r=(ad>bd)-(ad<bd);
  } else if(a->t==V_STR && b->t==V_STR){
    r=strcmp(a->s?a->s:"",b->s?b->s:"");
  } else {
    int ar=gml_val_sort_rank(*a), br=gml_val_sort_rank(*b);
    r=(ar>br)-(ar<br);
  }
  return sort_dir>=0 ? r : -r;
}
static void gml_array_sort(GmlVal arr, int ascending){
  if(arr.t!=V_ARR || !arr.arr) return;
  GmlArr *A=(GmlArr*)arr.arr;
  if(A->len<=1 || !A->data) return;
  sort_dir=ascending?1:-1;
  qsort(A->data,(size_t)A->len,sizeof(GmlVal),gml_val_sort_cmp);
}
static GmlVal json_parse_value(JsonIn *j, int depth);
static GmlVal json_parse_array(JsonIn *j, int depth){
  if(!js_consume(j,'[')){ j->ok=0; return vreal(0); }
  GmlArr *A=calloc(1,sizeof(*A)); if(!A){ j->ok=0; return vreal(0); }
  A->escaped=1;
  js_ws(j);
  if(js_consume(j,']')){ GmlVal v=vreal(0); v.t=V_ARR; v.arr=A; return v; }
  for(;;){
    GmlVal v=json_parse_value(j,depth+1);
    if(!j->ok || !js_arr_push(A,v)){ j->ok=0; return vreal(0); }
    if(js_consume(j,']')) break;
    if(!js_consume(j,',')){ j->ok=0; return vreal(0); }
  }
  GmlVal out=vreal(0); out.t=V_ARR; out.arr=A; return out;
}
static GmlVal json_parse_object(JsonIn *j, int depth){
  if(!js_consume(j,'{')){ j->ok=0; return vreal(0); }
  if(j->obj_as_struct){
    GmlInstance *st=gml_struct_new(j->vm);
    if(!st){ j->ok=0; return vreal(0); }
    js_ws(j);
    if(js_consume(j,'}')) return vreal((double)st->id);
    for(;;){
      char *key=js_string(j);
      if(!j->ok){ free(key); return vreal(0); }
      if(!js_consume(j,':')){ free(key); j->ok=0; return vreal(0); }
      GmlVal val=json_parse_value(j,depth+1);
      if(!j->ok){ free(key); return vreal(0); }
      *gml_varmap_put(&st->vars,key)=var_store_clone(val);
      if(js_consume(j,'}')) break;
      if(!js_consume(j,',')){ j->ok=0; return vreal(0); }
    }
    return vreal((double)st->id);
  }
  int id=ds_map_create_id(j->vm); if(id<0){ j->ok=0; return vreal(0); }
  js_ws(j);
  if(js_consume(j,'}')) return vreal(id);
  for(;;){
    char *key=js_string(j);
    if(!j->ok){ free(key); return vreal(0); }
    if(!js_consume(j,':')){ free(key); j->ok=0; return vreal(0); }
    GmlVal val=json_parse_value(j,depth+1);
    if(!j->ok){ free(key); return vreal(0); }
    ds_map_put(j->vm,id,vstr(key),val,1);
    free(key);
    if(js_consume(j,'}')) break;
    if(!js_consume(j,',')){ j->ok=0; return vreal(0); }
  }
  return vreal(id);
}
static int js_match(JsonIn *j, const char *lit){
  size_t n=strlen(lit);
  if(j->p+n<=j->n && !strncmp(j->s+j->p,lit,n)){ j->p+=n; return 1; }
  return 0;
}
static GmlVal json_parse_value(JsonIn *j, int depth){
  if(depth>32){ j->ok=0; return vreal(0); }
  js_ws(j); if(j->p>=j->n){ j->ok=0; return vreal(0); }
  char c=j->s[j->p];
  if(c=='{') return json_parse_object(j,depth);
  if(c=='[') return json_parse_array(j,depth);
  if(c=='"'){ char *s=js_string(j); return vstr_owned(s); }
  if(c=='t'){ if(js_match(j,"true")) return vreal(1); j->ok=0; return vreal(0); }
  if(c=='f'){ if(js_match(j,"false")) return vreal(0); j->ok=0; return vreal(0); }
  if(c=='n'){ if(js_match(j,"null")) return vundef(); j->ok=0; return vreal(0); }
  char *end=NULL; double d=strtod(j->s+j->p,&end);
  if(end==j->s+j->p){ j->ok=0; return vreal(0); }
  j->p=(size_t)(end-j->s); return vreal(d);
}
static GmlVal json_decode_text_mode(GmlVM *vm, const char *s, int obj_as_struct){
  JsonIn j={s?s:"",0,s?strlen(s):0,vm,1,obj_as_struct};
  GmlVal v=json_parse_value(&j,0);
  js_ws(&j);
  if(getenv("GML_DBG_JSON")){ static int c=0; if(c++<12)
    fprintf(stderr,"[json] len=%zu ok=%d consumed=%zu/%zu head='%.60s'\n",
      s?strlen(s):0, j.ok, j.p, j.n, s?s:""); }
  if(!j.ok || j.p!=j.n) return vreal(0);
  return v;
}
static GmlVal json_decode_text(GmlVM *vm, const char *s){
  return json_decode_text_mode(vm,s,0);
}
static GmlVal json_encode_root(GmlVM *vm, GmlVal v){
  JsonBuf b={0};
  if(!json_encode_val(vm,&b,v,0,1)){ free(b.s); return vstr_owned(strdup("{}")); }
  return vstr_owned(b.s?b.s:strdup("null"));
}
static int varmap_delete_key(GmlVarMap *m, const char *key){
  if(!m||!m->slots||!key) return 0;
  int found=-1;
  for(int i=0;i<m->cap;i++) if(m->slots[i].key && !strcmp(m->slots[i].key,key)){ found=i; break; }
  if(found<0) return 0;
  GmlVarMap nm={0};
  for(int i=0;i<m->cap;i++){
    if(i==found || !m->slots[i].key) continue;
    *gml_varmap_put(&nm,m->slots[i].key)=m->slots[i].val;
  }
  free(m->slots);
  *m=nm;
  return 1;
}
static GmlVal var_store_clone(GmlVal v){
  if(v.t==V_STR){
    char *c=v.s?strdup(v.s):NULL;
    return c?vstr(c):vstr("");
  }
  if(v.t==V_ARR){ gml_arr_mark_escaped(v); return v; }
  if(v.t==V_UNDEF) return vundef();
  return vreal(v.t==V_REAL?v.d:0.0);
}
static GmlVal ds_map_write_text(GmlVM *vm, int id){
  GmlDSMap *m=ds_map_slot(vm,id);
  JsonBuf b={0};
  if(!m || !jb_puts(&b,"{\"__gml_ds_map__\":[")){ free(b.s); return vstr_owned(strdup("{}")); }
  for(int i=0;i<m->len;i++){
    if(i && !jb_putc(&b,',')){ free(b.s); return vstr_owned(strdup("{}")); }
    if(!jb_putc(&b,'[')){ free(b.s); return vstr_owned(strdup("{}")); }
    GmlVal key=m->entry[i].key_val;
    if(key.t==V_STR){
      if(!jb_puts(&b,"\"s\",") || !json_encode_val(vm,&b,key,0,0)){ free(b.s); return vstr_owned(strdup("{}")); }
    } else if(key.t==V_UNDEF){
      if(!jb_puts(&b,"\"u\",null")){ free(b.s); return vstr_owned(strdup("{}")); }
    } else {
      if(!jb_puts(&b,"\"r\",") || !json_encode_val(vm,&b,vreal(key.t==V_REAL?key.d:0.0),0,0)){ free(b.s); return vstr_owned(strdup("{}")); }
    }
    if(!jb_putc(&b,',') || !json_encode_val(vm,&b,m->entry[i].val,0,0) || !jb_putc(&b,']')){
      free(b.s);
      return vstr_owned(strdup("{}"));
    }
  }
  if(!jb_puts(&b,"]}")){ free(b.s); return vstr_owned(strdup("{}")); }
  return vstr_owned(b.s?b.s:strdup("{}"));
}
static int ds_map_read_text(GmlVM *vm, int dst_id, const char *text){
  GmlVal parsed=json_decode_text(vm,text);
  if(parsed.t!=V_REAL) return 0;
  int src_id=(int)parsed.d;
  if(fabs(parsed.d-(double)src_id)>=1e-9) return 0;
  GmlDSMap *src=ds_map_slot(vm,src_id);
  if(!src) return 0;
  int wi=ds_map_find_entry(src,"s:__gml_ds_map__");
  if(wi>=0 && src->entry[wi].val.t==V_ARR && src->entry[wi].val.arr){
    GmlDSMap *dst=ds_map_slot(vm,dst_id);
    if(!dst){ ds_map_destroy_live(src); return 0; }
    ds_map_clear_entries(dst);
    GmlArr *rows=(GmlArr*)src->entry[wi].val.arr;
    for(int i=0;i<rows->len;i++){
      if(rows->data[i].t!=V_ARR || !rows->data[i].arr) continue;
      GmlArr *row=(GmlArr*)rows->data[i].arr;
      if(row->len<3) continue;
      const char *kind=(row->data[0].t==V_STR && row->data[0].s)?row->data[0].s:"";
      GmlVal key=vundef();
      if(kind[0]=='s'){
        key = row->data[1].t==V_STR ? row->data[1] : vstr(gm_string_tmp(row->data[1]));
      } else if(kind[0]=='r'){
        key = row->data[1].t==V_REAL ? row->data[1] : vreal(atof(gm_string_tmp(row->data[1])));
      } else if(kind[0]!='u') {
        continue;
      }
      ds_map_put(vm,dst_id,key,row->data[2],1);
    }
    ds_map_destroy_live(src);
    return 1;
  }
  int ok=ds_map_replace_from_map(vm,dst_id,src_id);
  ds_map_destroy_live(src);
  return ok;
}
static void ini_reset(GmlVM *vm){
  for(int i=0;i<vm->ini_n;i++){
    free(vm->ini_kv[i].section);
    free(vm->ini_kv[i].key);
    free(vm->ini_kv[i].sval);
  }
  memset(vm->ini_kv,0,sizeof(vm->ini_kv));
  vm->ini_n=0;
  vm->ini_open=0;
  vm->ini_path[0]=0;
}
static char *trim_ws(char *s){
  while(*s && isspace((unsigned char)*s)) s++;
  char *e=s+strlen(s);
  while(e>s && isspace((unsigned char)e[-1])) *--e=0;
  return s;
}
static void ini_add_kv(GmlVM *vm, const char *sec, const char *key, const char *val){
  if(!sec || !key || !*key || vm->ini_n>=256) return;
  vm->ini_kv[vm->ini_n]=(typeof(vm->ini_kv[0])){
    .section=strdup(sec),
    .key=strdup(key),
    .sval=strdup(val?val:""),
    .val=atof(val?val:""),
    .is_str=1
  };
  vm->ini_n++;
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
    ini_add_kv(vm,sec,trim_ws(line),trim_ws(eq+1));
  }
  free(sec);
  free(copy);
}
static GmlVal builtin_ini_open_file(GmlVM *vm, GmlVal *a, int n){
  ini_reset(vm);
  vm->ini_open=1;
  char *fn=resolve_content_path(vm,S(a,n,0));
  snprintf(vm->ini_path,sizeof vm->ini_path,"%s",fn);
  FILE *f=fopen(fn,"r");
  if(!f){ free(fn); return vreal(1); }
  char line[512], *sec=NULL;
  while(fgets(line,sizeof line,f)){
    int len=strlen(line);
    while(len>0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
    char *t=trim_ws(line);
    if(!*t || *t==';' || *t=='#') continue;
    if(t[0]=='['){ char *e=strchr(t,']'); if(e){ *e=0; free(sec); sec=strdup(trim_ws(t+1)); } continue; }
    if(!sec) continue;
    char *eq=strchr(t,'=');
    if(!eq) continue;
    *eq=0;
    ini_add_kv(vm,sec,trim_ws(t),trim_ws(eq+1));
  }
  free(sec);
  fclose(f);
  free(fn);
  return vreal(1);
}
static GmlVal builtin_file_text_open_read(GmlVM *vm, GmlVal *a, int n){
  char *path=resolve_content_path(vm,S(a,n,0));
  int id=vm_file_open(vm,path,"r");
  free(path);
  return vreal(id);
}
static GmlVal builtin_file_text_read_string(GmlVM *vm, GmlVal *a, int n){
  int i=vm_file_slot(vm,(int)N(a,n,0));
  if(i<0) return vstr_owned(strdup(""));
  FILE *f=(FILE*)vm->bin_file[i];
  size_t cap=4096, k=0;
  int c;
  char *b=malloc(cap);
  if(!b) return vstr_owned(strdup(""));
  while((c=fgetc(f))!=EOF){
    if(c=='\n' || c=='\r'){ ungetc(c,f); break; }
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
  return vstr_owned(b);
}
static GmlVal builtin_file_text_readln(GmlVM *vm, GmlVal *a, int n){
  int i=vm_file_slot(vm,(int)N(a,n,0));
  if(i<0) return vstr_owned(strdup(""));
  FILE *f=(FILE*)vm->bin_file[i];
  size_t cap=256, k=0;
  int c;
  char *b=malloc(cap);
  if(!b) return vstr_owned(strdup(""));
  while((c=fgetc(f))!=EOF && c!='\n'){
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
    int i=(vm->next_buffer_id+k-1)%16;
    if(!vm->buffer[i].live){
      if(cap<1) cap=1;
      vm->buffer[i].data=calloc((size_t)cap,1);
      vm->buffer[i].cap=cap; vm->buffer[i].size=cap; vm->buffer[i].pos=0; vm->buffer[i].live=1;
      vm->next_buffer_id=i+2; if(vm->next_buffer_id>16) vm->next_buffer_id=1;
      return i+1;
    }
  }
  return -1;
}
static void buffer_ensure(GmlVM *vm, int bi, int need){
  if(bi<0||bi>=16||need<=vm->buffer[bi].cap) return;
  int nc=vm->buffer[bi].cap?vm->buffer[bi].cap:1;
  while(nc<need) nc*=2;
  vm->buffer[bi].data=realloc(vm->buffer[bi].data,(size_t)nc);
  memset(vm->buffer[bi].data+vm->buffer[bi].cap,0,(size_t)(nc-vm->buffer[bi].cap));
  vm->buffer[bi].cap=nc;
}
static void buffer_resize_slot(GmlVM *vm, int bi, int size){
  if(bi<0||bi>=16||!vm->buffer[bi].live) return;
  if(size<0) size=0;
  int alloc=size>0?size:1;
  unsigned char *nd=realloc(vm->buffer[bi].data,(size_t)alloc);
  if(!nd) return;
  if(size>vm->buffer[bi].cap) memset(nd+vm->buffer[bi].cap,0,(size_t)(size-vm->buffer[bi].cap));
  vm->buffer[bi].data=nd;
  vm->buffer[bi].cap=alloc;
  vm->buffer[bi].size=size;
  if(vm->buffer[bi].pos>size) vm->buffer[bi].pos=size;
}
static void buffer_write_at(GmlVM *vm, int bi, int pos, const void *p, int n){
  if(bi<0||bi>=16||pos<0||n<=0) return;
  buffer_ensure(vm,bi,pos+n);
  memcpy(vm->buffer[bi].data+pos,p,(size_t)n);
  if(vm->buffer[bi].size<pos+n) vm->buffer[bi].size=pos+n;
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
static void buffer_write_raw(GmlVM *vm, int bi, const void *p, int n){
  if(n<=0) return;
  int pos=vm->buffer[bi].pos;
  buffer_ensure(vm,bi,pos+n);
  memcpy(vm->buffer[bi].data+pos,p,(size_t)n);
  vm->buffer[bi].pos=pos+n;
  if(vm->buffer[bi].size<vm->buffer[bi].pos) vm->buffer[bi].size=vm->buffer[bi].pos;
}
static uint64_t buffer_read_u(GmlVM *vm, int bi, int n){
  uint64_t v=0;
  if(n<=0) return 0;
  int pos=vm->buffer[bi].pos, avail=vm->buffer[bi].size-pos;
  if(avail<n) n=avail;
  if(n>0){ memcpy(&v,vm->buffer[bi].data+pos,(size_t)n); vm->buffer[bi].pos=pos+n; }
  return v;
}
static GmlVal buffer_read_typed_at_pos(GmlVM *vm, int bi, int pos, int type){
  if(bi<0||bi>=16||!vm->buffer[bi].live) return vreal(0);
  int old=vm->buffer[bi].pos;
  if(pos<0) pos=0;
  if(pos>vm->buffer[bi].size) pos=vm->buffer[bi].size;
  vm->buffer[bi].pos=pos;
  GmlVal out=vreal(0);
  if(type==11){
    int p=vm->buffer[bi].pos, e=p;
    while(e<vm->buffer[bi].size && vm->buffer[bi].data[e]) e++;
    out=vstr_owned(dup_n((char*)vm->buffer[bi].data+p,e-p));
  } else if(type==13){
    int p=vm->buffer[bi].pos, e=vm->buffer[bi].size;
    out=vstr_owned(dup_n((char*)vm->buffer[bi].data+p,e-p));
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
  vm->buffer[bi].pos=old;
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
static GmlVal md5_hex_val(const uint8_t *p, size_t n){
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
static unsigned char *base64_decode_alloc(const char *s, int *out_len){
  static signed char D[256]; static int dinit=0;
  if(!dinit){
    dinit=1;
    for(int k=0;k<256;k++) D[k]=-1;
    const char *B="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(int k=0;k<64;k++) D[(unsigned char)B[k]]=(signed char)k;
  }
  size_t L=s?strlen(s):0;
  unsigned char *o=malloc(L/4*3+4);
  if(!o){ if(out_len) *out_len=0; return NULL; }
  unsigned char *p=o;
  int bits=0; uint32_t acc=0;
  for(size_t i=0;i<L;i++){
    unsigned char ch=(unsigned char)s[i];
    if(ch=='=') break;
    int c=D[ch];
    if(c<0) continue;
    acc=(acc<<6)|(uint32_t)c; bits+=6;
    if(bits>=8){ bits-=8; *p++=(unsigned char)((acc>>bits)&0xff); }
  }
  if(out_len) *out_len=(int)(p-o);
  return o;
}

/* A manual draw_background_tiled[_ext] doesn't carry the layer's tiling axes, so recover them from
 * the room's background layer that uses this background def (background_htiled[]/vtiled[]). Default
 * to tile-both when no layer matches (GM's draw_background_tiled fills the room). */
static void bg_layer_tiling(GmlVM *vm, int bgdef, int *htiled, int *vtiled){
  for(int i=0;i<8;i++){
    if((int)gml_global_arr(vm,"background_index",i)==bgdef){
      *htiled=gml_global_arr(vm,"background_htiled",i)>=0.5;
      *vtiled=gml_global_arr(vm,"background_vtiled",i)>=0.5;
      return;
    }
  }
}
static uint32_t gm_color_to_xrgb(uint32_t c){
  /* alpha byte 0xFF = drawn/opaque: surfaces track per-pixel coverage in the high byte so
   * draw_clear_alpha(c,0) ("clear to transparent") is representable — see gml_render.h. */
  return 0xFF000000u | ((c&0xff)<<16) | (c&0xff00) | ((c>>16)&0xff);
}
static uint32_t xrgb_lerp(uint32_t a, uint32_t b, int num, int den){
  if(den<=0 || num<=0) return a;
  if(num>=den) return b;
  int ar=(a>>16)&0xff, ag=(a>>8)&0xff, ab=a&0xff;
  int br=(b>>16)&0xff, bg=(b>>8)&0xff, bb=b&0xff;
  int r=ar + (br-ar)*num/den;
  int g=ag + (bg-ag)*num/den;
  int bl=ab + (bb-ab)*num/den;
  return 0xFF000000u|((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)bl;
}
#if defined(__GNUC__) || defined(__clang__)
typedef uint64_t GmlBuiltinU64Alias __attribute__((__may_alias__));
#endif
static void fill_xrgb_run(uint32_t *dp, int n, uint32_t src){
  if(n<=0) return;
#if defined(__SSE2__)
  if(n>=4){
    __m128i v=_mm_set1_epi32((int)src);
    while(n>=4){
      _mm_storeu_si128((__m128i*)dp,v);
      dp+=4;
      n-=4;
    }
  }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  if(n>=4){
    uint32x4_t v=vdupq_n_u32(src);
    while(n>=4){
      vst1q_u32(dp,v);
      dp+=4;
      n-=4;
    }
  }
#endif
#if defined(__GNUC__) || defined(__clang__)
  if(n>=4){
    if(((uintptr_t)dp & 7u) != 0){ *dp++=src; n--; }
    GmlBuiltinU64Alias *p=(GmlBuiltinU64Alias*)dp;
    int pairs=n/2;
    uint64_t v=(uint64_t)src | ((uint64_t)src<<32);
    for(int i=0;i<pairs;i++) p[i]=v;
    dp += pairs*2;
    n -= pairs*2;
  }
#endif
  for(int i=0;i<n;i++) dp[i]=src;
}
static void draw_xrgb_run_alpha(GmlRender *R, uint32_t *dp, int n, uint32_t src, double alpha){
  if(n<=0) return;
  if(!R->alphablend || alpha>=1.0){
    uint32_t out=src;
    if(R->target_sp>0 && !R->alphablend){
      uint32_t sa=(uint32_t)lround(alpha*255.0);
      out=(src&0x00FFFFFFu)|(sa<<24);
    }
    fill_xrgb_run(dp,n,out);
    return;
  }
  uint32_t af=(uint32_t)(alpha*256.0);
  if(af>=256u){ fill_xrgb_run(dp,n,src); return; }
  if(!af) return;
  uint32_t ia=256u-af;
  uint32_t srb=(src&0x00FF00FFu)*af;
  uint32_t sg=(src&0x0000FF00u)*af;
  uint32_t sa=(uint32_t)lround(alpha*255.0);
  for(int i=0;i<n;i++){
    uint32_t dv=dp[i];
    uint32_t rb=((srb+(dv&0x00FF00FFu)*ia)>>8)&0x00FF00FFu;
    uint32_t g=((sg+(dv&0x0000FF00u)*ia)>>8)&0x0000FF00u;
    uint32_t oa=0xFF;
    if(R->target_sp>0){
      uint32_t da=dv>>24;
      oa=(sa*sa+da*(255u-sa)+127u)/255u;
    }
    dp[i]=(oa<<24)|rb|g;
  }
}
static void draw_px_alpha(GmlRender *R, int x, int y, uint32_t gmcol, double alpha){
  if(!R||x<0||y<0||x>=R->fbw||y>=R->fbh) return;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(alpha<=0) return;
  uint32_t src=gm_color_to_xrgb(gmcol), *dp=&R->fb[(size_t)y*R->fbw+x];
  if(R->alphablend && R->blendmode!=0){
    int sr=(src>>16)&0xff, sg=(src>>8)&0xff, sb=src&0xff;
    int dr=(*dp>>16)&0xff, dg=(*dp>>8)&0xff, db=*dp&0xff;
    int or_,og,ob;
    uint32_t oc=R->target_sp>0 ? *dp>>24 : 0xff;
    if(R->blendmode==1){
      or_=dr+(int)(sr*alpha); og=dg+(int)(sg*alpha); ob=db+(int)(sb*alpha);
      if(or_>255) or_=255;
      if(og>255) og=255;
      if(ob>255) ob=255;
      if(R->target_sp>0){ uint32_t a=oc+(uint32_t)(255*alpha); oc=a>255?255:a; }
    } else {
      or_=dr-(int)(sr*alpha); og=dg-(int)(sg*alpha); ob=db-(int)(sb*alpha);
      if(or_<0) or_=0;
      if(og<0) og=0;
      if(ob<0) ob=0;
      if(R->target_sp>0){ int a=(int)oc-(int)(255*alpha); oc=a<0?0:(uint32_t)a; }
    }
    *dp=(oc<<24)|((uint32_t)or_<<16)|((uint32_t)og<<8)|(uint32_t)ob;
    return;
  }
  if(!R->alphablend || alpha>=1){
    if(R->target_sp>0 && !R->alphablend){
      uint32_t sa=(uint32_t)lround(alpha*255.0);
      src=(src&0x00FFFFFFu)|(sa<<24);
    }
    *dp=src; return;
  }
  int sr=(src>>16)&0xff, sg=(src>>8)&0xff, sb=src&0xff;
  int dr=(*dp>>16)&0xff, dg=(*dp>>8)&0xff, db=*dp&0xff;
  int or_=(int)(sr*alpha+dr*(1-alpha)); if(or_>255) or_=255; else if(or_<0) or_=0;
  int og=(int)(sg*alpha+dg*(1-alpha)); if(og>255) og=255; else if(og<0) og=0;
  int ob=(int)(sb*alpha+db*(1-alpha)); if(ob>255) ob=255; else if(ob<0) ob=0;
  uint32_t oa=0xFF;
  if(R->target_sp>0){
    uint32_t sa=(uint32_t)lround(alpha*255.0), da=*dp>>24;
    oa=(sa*sa+da*(255u-sa)+127u)/255u;
  }
  *dp=(oa<<24)|(or_<<16)|(og<<8)|ob;
}
static void draw_px(GmlRender *R, int x, int y, uint32_t gmcol){
  draw_px_alpha(R,x,y,gmcol,R?R->alpha:1);
}

typedef struct {
  double x,y,z,u,v;
  double r,g,b,alpha;
  double nx,ny,nz;
  int has_normal;
} GmlD3Vertex;
typedef struct {
  int active, hidden, culling, ortho;
  double eye[3], right[3], up[3], forward[3];
  double ortho_x, ortho_y, ortho_w, ortho_h, ortho_angle;
  int zwrite, smooth, fog, perspective;
  double fov, aspect, near_clip, far_clip, draw_depth;
  double fog_start, fog_end;
  uint32_t fog_color, ambient_color;
  double transform[16], transform_stack[32][16];
  int transform_stack_n;
  int lighting;
  struct { int defined, enabled; double x,y,z,range; uint32_t color; } light[8];
  double shade_r, shade_g, shade_b;
  float *depth; size_t depth_cap;
  int depth_w, depth_h;
  long depth_frame;
} GmlD3State;
static GmlD3State g_d3={.shade_r=1,.shade_g=1,.shade_b=1,.depth_frame=-1};
#define GML_D3_PRIM_MAX 4096
static GmlD3Vertex g_d3_prim[GML_D3_PRIM_MAX];
static int g_d3_prim_kind, g_d3_prim_texture=-1, g_d3_prim_n;

#define GML_D3_MODEL_MAX 256
#define GML_D3_MODEL_VERTEX_MAX 1048576
typedef struct { int kind,first,count; } GmlD3Batch;
typedef struct {
  int used,building;
  GmlD3Vertex *vertex;
  int vertex_n,vertex_cap;
  GmlD3Batch *batch;
  int batch_n,batch_cap;
} GmlD3Model;
static GmlD3Model g_d3_model[GML_D3_MODEL_MAX];

static void d3_model_clear_data(GmlD3Model *model){
  if(!model) return;
  free(model->vertex); free(model->batch);
  model->vertex=NULL; model->batch=NULL;
  model->vertex_n=model->vertex_cap=model->batch_n=model->batch_cap=0;
  model->building=-1;
}
static void d3_models_clear_all(void){
  for(int i=0;i<GML_D3_MODEL_MAX;i++){
    d3_model_clear_data(&g_d3_model[i]);
    g_d3_model[i].used=0;
  }
}

static void d3_matrix_identity(double matrix[16]){
  memset(matrix,0,16*sizeof(*matrix));
  matrix[0]=matrix[5]=matrix[10]=matrix[15]=1;
}

void gml_d3_reset(void){
  float *depth=g_d3.depth; size_t cap=g_d3.depth_cap;
  memset(&g_d3,0,sizeof(g_d3));
  g_d3.depth=depth; g_d3.depth_cap=cap; g_d3.depth_frame=-1;
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  g_d3.zwrite=1; g_d3.smooth=1; g_d3.perspective=1;
  g_d3.fov=41.2; g_d3.near_clip=.05; g_d3.far_clip=32000;
  g_d3.ambient_color=0x262626u;
  d3_matrix_identity(g_d3.transform);
  g_d3_prim_n=0; g_d3_prim_kind=0; g_d3_prim_texture=-1;
  d3_models_clear_all();
}
void gml_d3_state_get(int flags[GML_D3_STATE_FLAG_COUNT],
                      double values[GML_D3_STATE_VALUE_COUNT],
                      uint32_t colors[GML_D3_STATE_COLOR_COUNT]){
  flags[0]=g_d3.active; flags[1]=g_d3.hidden; flags[2]=g_d3.lighting;
  int v=0; for(int i=0;i<3;i++) values[v++]=g_d3.eye[i];
  for(int i=0;i<3;i++) values[v++]=g_d3.right[i];
  for(int i=0;i<3;i++) values[v++]=g_d3.up[i];
  for(int i=0;i<3;i++) values[v++]=g_d3.forward[i];
  for(int i=0;i<8;i++){
    flags[3+i*2]=g_d3.light[i].defined; flags[4+i*2]=g_d3.light[i].enabled;
    values[v++]=g_d3.light[i].x; values[v++]=g_d3.light[i].y;
    values[v++]=g_d3.light[i].z; values[v++]=g_d3.light[i].range;
    colors[i]=g_d3.light[i].color;
  }
  flags[19]=g_d3.culling; flags[20]=g_d3.ortho;
  values[44]=g_d3.ortho_x; values[45]=g_d3.ortho_y;
  values[46]=g_d3.ortho_w; values[47]=g_d3.ortho_h; values[48]=g_d3.ortho_angle;
  flags[21]=g_d3.zwrite; flags[22]=g_d3.smooth; flags[23]=g_d3.fog;
  flags[24]=g_d3.perspective; flags[25]=g_d3.transform_stack_n;
  values[49]=g_d3.fov; values[50]=g_d3.aspect; values[51]=g_d3.near_clip;
  values[52]=g_d3.far_clip; values[53]=g_d3.draw_depth;
  values[54]=g_d3.fog_start; values[55]=g_d3.fog_end;
  memcpy(values+56,g_d3.transform,16*sizeof(*values));
  memcpy(values+72,g_d3.transform_stack,512*sizeof(*values));
  colors[8]=g_d3.fog_color;
  colors[9]=g_d3.ambient_color;
}
void gml_d3_state_set(const int flags[GML_D3_STATE_FLAG_COUNT],
                      const double values[GML_D3_STATE_VALUE_COUNT],
                      const uint32_t colors[GML_D3_STATE_COLOR_COUNT]){
  gml_d3_reset();
  g_d3.active=flags[0]; g_d3.hidden=flags[1]; g_d3.lighting=flags[2];
  int v=0; for(int i=0;i<3;i++) g_d3.eye[i]=values[v++];
  for(int i=0;i<3;i++) g_d3.right[i]=values[v++];
  for(int i=0;i<3;i++) g_d3.up[i]=values[v++];
  for(int i=0;i<3;i++) g_d3.forward[i]=values[v++];
  for(int i=0;i<8;i++){
    g_d3.light[i].defined=flags[3+i*2]; g_d3.light[i].enabled=flags[4+i*2];
    g_d3.light[i].x=values[v++]; g_d3.light[i].y=values[v++];
    g_d3.light[i].z=values[v++]; g_d3.light[i].range=values[v++];
    g_d3.light[i].color=colors[i];
  }
  g_d3.culling=flags[19]; g_d3.ortho=flags[20];
  g_d3.ortho_x=values[44]; g_d3.ortho_y=values[45];
  g_d3.ortho_w=values[46]; g_d3.ortho_h=values[47]; g_d3.ortho_angle=values[48];
  g_d3.zwrite=flags[21]; g_d3.smooth=flags[22]; g_d3.fog=flags[23];
  g_d3.perspective=flags[24];
  g_d3.transform_stack_n=flags[25];
  if(g_d3.transform_stack_n<0) g_d3.transform_stack_n=0;
  if(g_d3.transform_stack_n>32) g_d3.transform_stack_n=32;
  g_d3.fov=values[49]; g_d3.aspect=values[50]; g_d3.near_clip=values[51];
  g_d3.far_clip=values[52]; g_d3.draw_depth=values[53];
  g_d3.fog_start=values[54]; g_d3.fog_end=values[55];
  memcpy(g_d3.transform,values+56,16*sizeof(*values));
  memcpy(g_d3.transform_stack,values+72,512*sizeof(*values));
  g_d3.fog_color=colors[8];
  g_d3.ambient_color=colors[9];
}

static double d3_dot(const double a[3], const double b[3]){
  return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
}
static int d3_normalize(double v[3]){
  double length=sqrt(d3_dot(v,v));
  if(length<1e-12) return 0;
  v[0]/=length; v[1]/=length; v[2]/=length;
  return 1;
}
static void d3_cross(const double a[3], const double b[3], double out[3]){
  out[0]=a[1]*b[2]-a[2]*b[1];
  out[1]=a[2]*b[0]-a[0]*b[2];
  out[2]=a[0]*b[1]-a[1]*b[0];
}
static void d3_matrix_multiply(const double left[16],const double right[16],double out[16]){
  double result[16];
  for(int row=0;row<4;row++) for(int col=0;col<4;col++){
    double sum=0;
    for(int k=0;k<4;k++) sum+=left[row+k*4]*right[k+col*4];
    result[row+col*4]=sum;
  }
  memcpy(out,result,sizeof(result));
}
static void d3_matrix_prepend(const double added[16]){
  d3_matrix_multiply(added,g_d3.transform,g_d3.transform);
}
static void d3_matrix_translation(double out[16],double x,double y,double z){
  d3_matrix_identity(out); out[12]=x; out[13]=y; out[14]=z;
}
static void d3_matrix_scaling(double out[16],double x,double y,double z){
  d3_matrix_identity(out); out[0]=x; out[5]=y; out[10]=z;
}
static void d3_matrix_rotation_axis(double out[16],double x,double y,double z,double degrees){
  double axis[3]={x,y,z};
  if(!d3_normalize(axis)){ d3_matrix_identity(out); return; }
  x=axis[0]; y=axis[1]; z=axis[2];
  double angle=degrees*M_PI/180.0,c=cos(angle),s=sin(angle),t=1-c;
  d3_matrix_identity(out);
  out[0]=t*x*x+c;   out[4]=t*x*y-s*z; out[8]=t*x*z+s*y;
  out[1]=t*x*y+s*z; out[5]=t*y*y+c;   out[9]=t*y*z-s*x;
  out[2]=t*x*z-s*y; out[6]=t*y*z+s*x; out[10]=t*z*z+c;
}
static void d3_transform_point(double *x,double *y,double *z){
  double inx=*x,iny=*y,inz=*z;
  *x=g_d3.transform[0]*inx+g_d3.transform[4]*iny+g_d3.transform[8]*inz+g_d3.transform[12];
  *y=g_d3.transform[1]*inx+g_d3.transform[5]*iny+g_d3.transform[9]*inz+g_d3.transform[13];
  *z=g_d3.transform[2]*inx+g_d3.transform[6]*iny+g_d3.transform[10]*inz+g_d3.transform[14];
}
static int d3_depth_prepare(GmlRender *R){
  extern long g_vm_frame;
  if(!R || R->fbw<=0 || R->fbh<=0) return 0;
  size_t count=(size_t)R->fbw*(size_t)R->fbh;
  if(count>g_d3.depth_cap){
    float *depth=(float*)realloc(g_d3.depth,count*sizeof(*depth));
    if(!depth) return 0;
    g_d3.depth=depth; g_d3.depth_cap=count;
  }
  if(g_d3.depth_frame!=g_vm_frame || g_d3.depth_w!=R->fbw || g_d3.depth_h!=R->fbh){
    memset(g_d3.depth,0,count*sizeof(*g_d3.depth));
    g_d3.depth_frame=g_vm_frame; g_d3.depth_w=R->fbw; g_d3.depth_h=R->fbh;
  }
  return 1;
}
typedef struct {
  int kind,w,h,stride,offset_x,offset_y;
  const uint8_t *rgba;
  const uint32_t *xrgb;
} GmlD3Texture;
static int d3_texture(GmlRender *R, int handle, GmlD3Texture *out){
  if(out) memset(out,0,sizeof(*out));
  if(!R) return 0;
  uint32_t encoded=(uint32_t)handle,kind=encoded&GML_TEX_KIND_MASK;
  if(kind==GML_TEX_SPR_TAG){
    int spr=(int)((encoded>>10)&0xFFFF),img=(int)(encoded&0x3FF);
    GmlSprite *sprite=NULL; GmlTpag *t=NULL; GmlAtlas *atlas=NULL;
    gml_render_warm_sprite(R,spr);
    int sprite_kind=sprite_tpag_info(R,spr,img,&sprite,&t,&atlas);
    if(sprite_kind==1 && t && atlas){
      if(out){ out->kind=1; out->w=t->sw; out->h=t->sh; out->stride=atlas->w;
        out->offset_x=t->sx; out->offset_y=t->sy; out->rgba=atlas->px; }
      return 1;
    }
    if(sprite_kind==2 && sprite && sprite->runtime_rgba && sprite->w>0 && sprite->h>0){
      int frames=sprite->n_frames>0?sprite->n_frames:1;
      int sub=((img%frames)+frames)%frames;
      if(out){ out->kind=1; out->w=sprite->w; out->h=sprite->h; out->stride=sprite->w;
        out->rgba=sprite->runtime_rgba+(size_t)sub*sprite->w*sprite->h*4; }
      return 1;
    }
    return 0;
  }
  if(kind==GML_TEX_SURF_TAG){
    int sid=(int)(encoded&0xFFFF),w=0,h=0;
    const uint32_t *pixels=gml_surface_pixels_read(R,sid,&w,&h);
    if(!pixels || w<=0 || h<=0) return 0;
    if(out){ out->kind=2; out->w=w; out->h=h; out->stride=w; out->xrgb=pixels; }
    return 1;
  }
  if(kind!=GML_TEX_BG_TAG){
    if(getenv("GML_LOG_D3D")) fprintf(stderr,"[d3d] invalid texture handle %d\n",handle);
    return 0;
  }
  int bg=handle&0x00FFFFFF;
  if(bg<0 || bg>=R->n_bg) return 0;
  gml_render_warm_bg(R,bg);
  int ti=R->bg[bg].tpag;
  if(ti<0 || ti>=R->n_tpag) return 0;
  GmlTpag *t=&R->tpag[ti];
  if(t->atlas<0 || t->atlas>=R->n_atlas) return 0;
  GmlAtlas *a=&R->atlas[t->atlas];
  if(!a->px || t->sw<=0 || t->sh<=0) return 0;
  if(getenv("GML_LOG_D3D")){ static int logged=0; if(logged++<8)
    fprintf(stderr,"[d3d] texture bg=%d tpag=%d atlas=%d rect=%dx%d\n",bg,ti,t->atlas,t->sw,t->sh); }
  if(out){ out->kind=1; out->w=t->sw; out->h=t->sh; out->stride=a->w;
    out->offset_x=t->sx; out->offset_y=t->sy; out->rgba=a->px; }
  return 1;
}
static void d3_texture_texel(const GmlD3Texture *texture,int x,int y,int channel[4]){
  if(texture->kind==1){
    const uint8_t *pixel=texture->rgba+((size_t)(texture->offset_y+y)*texture->stride+texture->offset_x+x)*4;
    for(int c=0;c<4;c++) channel[c]=pixel[c];
  } else {
    uint32_t pixel=texture->xrgb[(size_t)y*texture->stride+x];
    channel[0]=(pixel>>16)&255; channel[1]=(pixel>>8)&255;
    channel[2]=pixel&255; channel[3]=(pixel>>24)&255;
  }
}
static uint32_t d3_sample(GmlRender *R, const GmlD3Texture *texture, double u, double v, double *alpha){
  if(!isfinite(u) || !isfinite(v)){
    if(alpha) *alpha=0.0;
    return 0;
  }
  u-=floor(u); v-=floor(v);
  double fx=u*texture->w-0.5, fy=v*texture->h-0.5;
  int x0=(int)floor(fx), y0=(int)floor(fy);
  double ax=fx-floor(fx), ay=fy-floor(fy);
  x0%=texture->w; y0%=texture->h;
  if(x0<0) x0+=texture->w; if(y0<0) y0+=texture->h;
  int x1=(x0+1)%texture->w, y1=(y0+1)%texture->h;
  int p[4][4];
  d3_texture_texel(texture,x0,y0,p[0]); d3_texture_texel(texture,x1,y0,p[1]);
  d3_texture_texel(texture,x0,y1,p[2]); d3_texture_texel(texture,x1,y1,p[3]);
  double w[4]={(1-ax)*(1-ay),ax*(1-ay),(1-ax)*ay,ax*ay};
  int channel[4]={0};
  if(R->interp){
    for(int c=0;c<4;c++) channel[c]=(int)lround(p[0][c]*w[0]+p[1][c]*w[1]+p[2][c]*w[2]+p[3][c]*w[3]);
  } else {
    const int *q=p[(ay>=0.5)*2+(ax>=0.5)];
    for(int c=0;c<4;c++) channel[c]=q[c];
  }
  if(alpha) *alpha=channel[3]/255.0;
  return (uint32_t)channel[0]|((uint32_t)channel[1]<<8)|((uint32_t)channel[2]<<16);
}
static double d3_edge(double ax,double ay,double bx,double by,double px,double py){
  return (px-ax)*(by-ay)-(py-ay)*(bx-ax);
}
static void d3_raster_triangle(GmlRender *R, const GmlD3Vertex in[3], const GmlD3Texture *texture){
  if(!d3_depth_prepare(R)) return;
  const char *fov_text=getenv("GML_D3D_FOV");
  double fov=fov_text?atof(fov_text):g_d3.fov;
  if(fov<1.0 || fov>170.0) fov=41.2;
  double tangent=tan(fov*M_PI/360.0);
  double focal_y=(R->fbh*0.5)/tangent;
  double focal_x=g_d3.aspect>1e-9?(R->fbw*0.5)/(tangent*g_d3.aspect):focal_y;
  double sx[3],sy[3],iz[3],uz[3],vz[3],riz[3],giz[3],biz[3],aiz[3];
  double depth_value[3],view_distance[3];
  for(int i=0;i<3;i++){
    if(g_d3.ortho){
      double ow=fabs(g_d3.ortho_w)>1e-9?g_d3.ortho_w:1;
      double oh=fabs(g_d3.ortho_h)>1e-9?g_d3.ortho_h:1;
      iz[i]=1; uz[i]=in[i].u; vz[i]=in[i].v;
      riz[i]=in[i].r; giz[i]=in[i].g; biz[i]=in[i].b; aiz[i]=in[i].alpha;
      sx[i]=in[i].x*R->fbw/ow; sy[i]=in[i].y*R->fbh/oh;
      depth_value[i]=1000000.0-in[i].z;
      view_distance[i]=fabs(in[i].z);
    } else {
      if(in[i].z<=1e-6) return;
      iz[i]=1.0/in[i].z; uz[i]=in[i].u*iz[i]; vz[i]=in[i].v*iz[i];
      riz[i]=in[i].r*iz[i]; giz[i]=in[i].g*iz[i]; biz[i]=in[i].b*iz[i]; aiz[i]=in[i].alpha*iz[i];
      sx[i]=R->fbw*0.5+in[i].x*focal_x*iz[i];
      sy[i]=R->fbh*0.5-in[i].y*focal_y*iz[i];
      depth_value[i]=iz[i];
      view_distance[i]=in[i].z;
    }
  }
  double area=d3_edge(sx[0],sy[0],sx[1],sy[1],sx[2],sy[2]);
  if(fabs(area)<1e-9) return;
  if(g_d3.culling && area>=0) return;
  int minx=(int)floor(fmin(sx[0],fmin(sx[1],sx[2]))), maxx=(int)ceil(fmax(sx[0],fmax(sx[1],sx[2])));
  int miny=(int)floor(fmin(sy[0],fmin(sy[1],sy[2]))), maxy=(int)ceil(fmax(sy[0],fmax(sy[1],sy[2])));
  if(minx<0) minx=0; if(miny<0) miny=0;
  if(maxx>=R->fbw) maxx=R->fbw-1; if(maxy>=R->fbh) maxy=R->fbh-1;
  for(int y=miny;y<=maxy;y++) for(int x=minx;x<=maxx;x++){
    double px=x+0.5,py=y+0.5;
    double b0=d3_edge(sx[1],sy[1],sx[2],sy[2],px,py)/area;
    double b1=d3_edge(sx[2],sy[2],sx[0],sy[0],px,py)/area;
    double b2=1.0-b0-b1;
    if(b0<-1e-9 || b1<-1e-9 || b2<-1e-9) continue;
    double invz=b0*iz[0]+b1*iz[1]+b2*iz[2];
    double ztest=b0*depth_value[0]+b1*depth_value[1]+b2*depth_value[2];
    size_t di=(size_t)y*R->fbw+x;
    if(g_d3.hidden && ztest<=g_d3.depth[di]) continue;
    double texture_alpha=1.0;
    uint32_t color=texture&&texture->kind ? d3_sample(R,texture,(b0*uz[0]+b1*uz[1]+b2*uz[2])/invz,
                                    (b0*vz[0]+b1*vz[1]+b2*vz[2])/invz,&texture_alpha) : 0xFFFFFFu;
    double vr=(b0*riz[0]+b1*riz[1]+b2*riz[2])/invz;
    double vg=(b0*giz[0]+b1*giz[1]+b2*giz[2])/invz;
    double vb=(b0*biz[0]+b1*biz[1]+b2*biz[2])/invz;
    double vertex_alpha=(b0*aiz[0]+b1*aiz[1]+b2*aiz[2])/invz;
    if(vr<0)vr=0; else if(vr>255)vr=255;
    if(vg<0)vg=0; else if(vg>255)vg=255;
    if(vb<0)vb=0; else if(vb>255)vb=255;
    int mod_r=(int)lround((color&255)*vr/255.0);
    int mod_g=(int)lround(((color>>8)&255)*vg/255.0);
    int mod_b=(int)lround(((color>>16)&255)*vb/255.0);
    color=(uint32_t)mod_r|((uint32_t)mod_g<<8)|((uint32_t)mod_b<<16);
    if(g_d3.lighting){
      int cr=(int)((color&255)*g_d3.shade_r), cg=(int)(((color>>8)&255)*g_d3.shade_g);
      int cb=(int)(((color>>16)&255)*g_d3.shade_b);
      if(cr>255)cr=255; if(cg>255)cg=255; if(cb>255)cb=255;
      color=(uint32_t)cr|((uint32_t)cg<<8)|((uint32_t)cb<<16);
    }
    if(g_d3.fog){
      double distance=g_d3.ortho?
        b0*view_distance[0]+b1*view_distance[1]+b2*view_distance[2]:1.0/invz;
      double span=g_d3.fog_end-g_d3.fog_start;
      double amount=span>1e-9?(distance-g_d3.fog_start)/span:(distance>=g_d3.fog_end?1:0);
      if(amount<0) amount=0; else if(amount>1) amount=1;
      uint32_t fog=g_d3.fog_color;
      int cr=(int)lround((color&255)*(1-amount)+(fog&255)*amount);
      int cg=(int)lround(((color>>8)&255)*(1-amount)+((fog>>8)&255)*amount);
      int cb=(int)lround(((color>>16)&255)*(1-amount)+((fog>>16)&255)*amount);
      color=(uint32_t)cr|((uint32_t)cg<<8)|((uint32_t)cb<<16);
    }
    double alpha=texture_alpha*vertex_alpha;
    draw_px_alpha(R,x,y,color,alpha);
    if(g_d3.hidden && g_d3.zwrite && alpha>0.0) g_d3.depth[di]=(float)ztest;
  }
}
static GmlD3Vertex d3_camera_vertex(double x,double y,double z,double u,double v){
  d3_transform_point(&x,&y,&z);
  if(g_d3.ortho){
    double dx=x-g_d3.ortho_x,dy=y-g_d3.ortho_y;
    double angle=g_d3.ortho_angle*M_PI/180.0,cs=cos(angle),sn=sin(angle);
    GmlD3Vertex out={0};
    out.x=cs*dx+sn*dy; out.y=-sn*dx+cs*dy; out.z=z; out.u=u; out.v=v;
    return out;
  }
  double d[3]={x-g_d3.eye[0],y-g_d3.eye[1],z-g_d3.eye[2]};
  GmlD3Vertex out={0};
  out.x=d3_dot(d,g_d3.right); out.y=d3_dot(d,g_d3.up);
  out.z=d3_dot(d,g_d3.forward); out.u=u; out.v=v;
  return out;
}
static GmlD3Vertex d3_vertex_lerp(GmlD3Vertex a,GmlD3Vertex b,double amount){
  GmlD3Vertex out;
#define D3_LERP(field) out.field=a.field+(b.field-a.field)*amount
  D3_LERP(x); D3_LERP(y); D3_LERP(z); D3_LERP(u); D3_LERP(v);
  D3_LERP(r); D3_LERP(g); D3_LERP(b); D3_LERP(alpha);
  D3_LERP(nx); D3_LERP(ny); D3_LERP(nz);
#undef D3_LERP
  out.has_normal=a.has_normal||b.has_normal;
  return out;
}
static int d3_clip_z(const GmlD3Vertex *input,int count,GmlD3Vertex *output,double plane,int keep_greater){
  int n=0;
  for(int i=0;i<count;i++){
    GmlD3Vertex a=input[i], b=input[(i+1)%count];
    int ain=keep_greater?a.z>=plane:a.z<=plane;
    int bin=keep_greater?b.z>=plane:b.z<=plane;
    if(ain) output[n++]=a;
    if(ain!=bin){
      double k=(plane-a.z)/(b.z-a.z);
      output[n]=d3_vertex_lerp(a,b,k); output[n++].z=plane;
    }
  }
  return n;
}
static void d3_draw_quad(GmlRender *R, const double p[4][3], int texture, double hrep, double vrep){
  GmlD3Texture resolved={0};
  if(texture!=-1) d3_texture(R,texture,&resolved);
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  if(g_d3.lighting){
    double e1[3]={p[1][0]-p[0][0],p[1][1]-p[0][1],p[1][2]-p[0][2]};
    double e2[3]={p[2][0]-p[0][0],p[2][1]-p[0][1],p[2][2]-p[0][2]}, normal[3];
    d3_cross(e1,e2,normal); d3_normalize(normal);
    double center[3]={0,0,0}; for(int i=0;i<4;i++) for(int k=0;k<3;k++) center[k]+=p[i][k]*.25;
    double lr=(g_d3.ambient_color&255)/255.0;
    double lg=((g_d3.ambient_color>>8)&255)/255.0;
    double lb=((g_d3.ambient_color>>16)&255)/255.0;
    for(int i=0;i<8;i++) if(g_d3.light[i].defined&&g_d3.light[i].enabled){
      uint32_t col=g_d3.light[i].color;
      if(g_d3.light[i].range<0){
        double ray[3]={-g_d3.light[i].x,-g_d3.light[i].y,-g_d3.light[i].z};
        if(!d3_normalize(ray)) continue;
        double diffuse=fabs(d3_dot(normal,ray));
        lr+=diffuse*(col&255)/255.0; lg+=diffuse*((col>>8)&255)/255.0; lb+=diffuse*((col>>16)&255)/255.0;
        continue;
      }
      double ray[3]={g_d3.light[i].x-center[0],g_d3.light[i].y-center[1],g_d3.light[i].z-center[2]};
      double dist=sqrt(d3_dot(ray,ray)); if(dist<=1e-9 || dist>=g_d3.light[i].range) continue;
      ray[0]/=dist; ray[1]/=dist; ray[2]/=dist;
      double diffuse=fabs(d3_dot(normal,ray))*(1.0-dist/g_d3.light[i].range);
      lr+=diffuse*(col&255)/255.0; lg+=diffuse*((col>>8)&255)/255.0; lb+=diffuse*((col>>16)&255)/255.0;
    }
    g_d3.shade_r=lr; g_d3.shade_g=lg; g_d3.shade_b=lb;
  }
  static const double uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
  GmlD3Vertex q[4], clipped[12], far_clipped[12];
  for(int i=0;i<4;i++){
    q[i]=d3_camera_vertex(p[i][0],p[i][1],p[i][2],uv[i][0]*hrep,uv[i][1]*vrep);
    q[i].r=R->color&255; q[i].g=(R->color>>8)&255; q[i].b=(R->color>>16)&255; q[i].alpha=R->alpha;
  }
  int count;
  if(g_d3.ortho){ memcpy(clipped,q,sizeof(q)); count=4; }
  else {
    double nearz=g_d3.near_clip>1e-6?g_d3.near_clip:.05;
    double farz=g_d3.far_clip>nearz?g_d3.far_clip:32000;
    count=d3_clip_z(q,4,clipped,nearz,1);
    if(count>0){ count=d3_clip_z(clipped,count,far_clipped,farz,0); memcpy(clipped,far_clipped,(size_t)count*sizeof(*clipped)); }
  }
  for(int i=1;i+1<count;i++){
    GmlD3Vertex tri[3]={clipped[0],clipped[i],clipped[i+1]};
    d3_raster_triangle(R,tri,&resolved);
  }
}
static void d3_set_camera(double xfrom,double yfrom,double zfrom,
                          double xto,double yto,double zto,
                          double xup,double yup,double zup){
  g_d3.ortho=0; g_d3.perspective=1;
  g_d3.eye[0]=xfrom; g_d3.eye[1]=yfrom; g_d3.eye[2]=zfrom;
  g_d3.forward[0]=xto-xfrom; g_d3.forward[1]=yto-yfrom; g_d3.forward[2]=zto-zfrom;
  double supplied_up[3]={xup,yup,zup};
  if(!d3_normalize(g_d3.forward)) g_d3.forward[0]=1;
  d3_cross(supplied_up,g_d3.forward,g_d3.right);
  if(!d3_normalize(g_d3.right)){ g_d3.right[0]=0; g_d3.right[1]=1; g_d3.right[2]=0; }
  d3_cross(g_d3.forward,g_d3.right,g_d3.up);
  d3_normalize(g_d3.up);
}
static GmlD3Vertex d3_vertex_camera(GmlD3Vertex input){
  GmlD3Vertex output=d3_camera_vertex(input.x,input.y,input.z,input.u,input.v);
  output.r=input.r; output.g=input.g; output.b=input.b; output.alpha=input.alpha;
  output.nx=input.nx; output.ny=input.ny; output.nz=input.nz; output.has_normal=input.has_normal;
  return output;
}
static int d3_transform_normal(const GmlD3Vertex *vertex,double output[3]){
  double a00=g_d3.transform[0],a01=g_d3.transform[4],a02=g_d3.transform[8];
  double a10=g_d3.transform[1],a11=g_d3.transform[5],a12=g_d3.transform[9];
  double a20=g_d3.transform[2],a21=g_d3.transform[6],a22=g_d3.transform[10];
  double c00=a11*a22-a12*a21,c01=a12*a20-a10*a22,c02=a10*a21-a11*a20;
  double c10=a02*a21-a01*a22,c11=a00*a22-a02*a20,c12=a01*a20-a00*a21;
  double c20=a01*a12-a02*a11,c21=a02*a10-a00*a12,c22=a00*a11-a01*a10;
  double determinant=a00*c00+a01*c01+a02*c02;
  if(fabs(determinant)<1e-12) return 0;
  output[0]=(c00*vertex->nx+c01*vertex->ny+c02*vertex->nz)/determinant;
  output[1]=(c10*vertex->nx+c11*vertex->ny+c12*vertex->nz)/determinant;
  output[2]=(c20*vertex->nx+c21*vertex->ny+c22*vertex->nz)/determinant;
  return d3_normalize(output);
}
static int d3_vertex_light_factor(const GmlD3Vertex *vertex,double factor[3]){
  if(!vertex->has_normal) return 0;
  double normal[3];
  if(!d3_transform_normal(vertex,normal)) return 0;
  double x=vertex->x,y=vertex->y,z=vertex->z;
  d3_transform_point(&x,&y,&z);
  factor[0]=(g_d3.ambient_color&255)/255.0;
  factor[1]=((g_d3.ambient_color>>8)&255)/255.0;
  factor[2]=((g_d3.ambient_color>>16)&255)/255.0;
  for(int i=0;i<8;i++) if(g_d3.light[i].defined&&g_d3.light[i].enabled){
    uint32_t color=g_d3.light[i].color;
    double diffuse=0;
    if(g_d3.light[i].range<0){
      double ray[3]={-g_d3.light[i].x,-g_d3.light[i].y,-g_d3.light[i].z};
      if(!d3_normalize(ray)) continue;
      diffuse=fabs(d3_dot(normal,ray));
    } else {
      double ray[3]={g_d3.light[i].x-x,g_d3.light[i].y-y,g_d3.light[i].z-z};
      double distance=sqrt(d3_dot(ray,ray));
      if(distance<=1e-9 || distance>=g_d3.light[i].range) continue;
      ray[0]/=distance; ray[1]/=distance; ray[2]/=distance;
      diffuse=fabs(d3_dot(normal,ray))*(1-distance/g_d3.light[i].range);
    }
    factor[0]+=diffuse*(color&255)/255.0;
    factor[1]+=diffuse*((color>>8)&255)/255.0;
    factor[2]+=diffuse*((color>>16)&255)/255.0;
  }
  return 1;
}
static void d3_apply_light_factor(GmlD3Vertex *vertex,const double factor[3]){
  vertex->r*=factor[0]; vertex->g*=factor[1]; vertex->b*=factor[2];
  if(vertex->r>255) vertex->r=255;
  if(vertex->g>255) vertex->g=255;
  if(vertex->b>255) vertex->b=255;
}
static void d3_emit_triangle(GmlRender *R,const GmlD3Vertex world[3],const GmlD3Texture *texture){
  GmlD3Vertex camera[3],near_clipped[12],far_clipped[12];
  GmlD3Vertex lit[3]={world[0],world[1],world[2]};
  if(g_d3.lighting){
    double flat[3]; int have_flat=0;
    if(!g_d3.smooth) for(int i=0;i<3&&!have_flat;i++) have_flat=d3_vertex_light_factor(&lit[i],flat);
    for(int i=0;i<3;i++){
      double factor[3]; int have=have_flat;
      if(have_flat) memcpy(factor,flat,sizeof(factor));
      else have=d3_vertex_light_factor(&lit[i],factor);
      if(have) d3_apply_light_factor(&lit[i],factor);
    }
  }
  for(int i=0;i<3;i++) camera[i]=d3_vertex_camera(lit[i]);
  int count;
  if(g_d3.ortho){ memcpy(near_clipped,camera,sizeof(camera)); count=3; }
  else {
    double nearz=g_d3.near_clip>1e-6?g_d3.near_clip:.05;
    double farz=g_d3.far_clip>nearz?g_d3.far_clip:32000;
    count=d3_clip_z(camera,3,near_clipped,nearz,1);
    if(count>0){
      count=d3_clip_z(near_clipped,count,far_clipped,farz,0);
      memcpy(near_clipped,far_clipped,(size_t)count*sizeof(*near_clipped));
    }
  }
  for(int i=1;i+1<count;i++){
    GmlD3Vertex triangle[3]={near_clipped[0],near_clipped[i],near_clipped[i+1]};
    d3_raster_triangle(R,triangle,texture);
  }
}
void gml_d3_set_draw_depth(double depth){ g_d3.draw_depth=depth; }
int gml_d3_draw_sprite_2d(GmlRender *R,int sprite_id,int subimg,double x,double y,
                          double xs,double ys,double rotation,uint32_t blend,double alpha){
  if(!g_d3.active) return 0;
  if(!R || sprite_id<0 || sprite_id>=R->n_spr || alpha<=0) return 1;
  GmlSprite *sprite=NULL; GmlTpag *page=NULL; GmlAtlas *atlas=NULL;
  gml_render_warm_sprite(R,sprite_id);
  int kind=sprite_tpag_info(R,sprite_id,subimg,&sprite,&page,&atlas);
  if(!sprite || (kind!=1&&kind!=2)) return 1;
  double left=-sprite->originx,top=-sprite->originy,right=sprite->w-sprite->originx,bottom=sprite->h-sprite->originy;
  if(kind==1&&page){
    left=page->tx-sprite->originx; top=page->ty-sprite->originy;
    right=left+page->sw; bottom=top+page->sh;
  }
  GmlD3Texture texture={0};
  int handle=(int)(GML_TEX_SPR_TAG|((sprite_id&0xFFFF)<<10)|(subimg&0x3FF));
  if(!d3_texture(R,handle,&texture)) return 1;
  double radians=rotation*M_PI/180.0,cs=cos(radians),sn=sin(radians);
  const double local[4][2]={{left,top},{right,top},{right,bottom},{left,bottom}};
  const double uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
  GmlD3Vertex vertex[4];
  for(int i=0;i<4;i++){
    double px=local[i][0]*xs,py=local[i][1]*ys;
    memset(&vertex[i],0,sizeof(vertex[i]));
    vertex[i].x=x+px*cs+py*sn; vertex[i].y=y-px*sn+py*cs; vertex[i].z=g_d3.draw_depth;
    vertex[i].u=uv[i][0]; vertex[i].v=uv[i][1];
    vertex[i].r=blend&255; vertex[i].g=(blend>>8)&255; vertex[i].b=(blend>>16)&255; vertex[i].alpha=alpha;
  }
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  gml_render_maybe_prepare_draw(R);
  GmlD3Vertex first[3]={vertex[0],vertex[1],vertex[2]};
  GmlD3Vertex second[3]={vertex[0],vertex[2],vertex[3]};
  d3_emit_triangle(R,first,&texture); d3_emit_triangle(R,second,&texture);
  return 1;
}
static int d3_project_vertex(GmlRender *R,const GmlD3Vertex *vertex,
                             double *screen_x,double *screen_y,double *inverse_z,
                             double *depth,double *distance){
  if(!R || !vertex) return 0;
  if(g_d3.ortho){
    double width=fabs(g_d3.ortho_w)>1e-9?g_d3.ortho_w:1;
    double height=fabs(g_d3.ortho_h)>1e-9?g_d3.ortho_h:1;
    *screen_x=vertex->x*R->fbw/width; *screen_y=vertex->y*R->fbh/height;
    *inverse_z=1; *depth=1000000.0-vertex->z; *distance=fabs(vertex->z);
    return 1;
  }
  double nearz=g_d3.near_clip>1e-6?g_d3.near_clip:.05;
  double farz=g_d3.far_clip>nearz?g_d3.far_clip:32000;
  if(vertex->z<nearz || vertex->z>farz) return 0;
  double fov=g_d3.fov;
  if(fov<1.0 || fov>170.0) fov=41.2;
  double tangent=tan(fov*M_PI/360.0);
  double focal_y=(R->fbh*.5)/tangent;
  double focal_x=g_d3.aspect>1e-9?(R->fbw*.5)/(tangent*g_d3.aspect):focal_y;
  *inverse_z=1.0/vertex->z;
  *screen_x=R->fbw*.5+vertex->x*focal_x*(*inverse_z);
  *screen_y=R->fbh*.5-vertex->y*focal_y*(*inverse_z);
  *depth=*inverse_z; *distance=vertex->z;
  return 1;
}
static void d3_raster_sample(GmlRender *R,int x,int y,double depth,double distance,
                             double u,double v,double red,double green,double blue,double alpha,
                             const GmlD3Texture *texture){
  if(!R || x<0 || y<0 || x>=R->fbw || y>=R->fbh || alpha<=0) return;
  size_t index=(size_t)y*R->fbw+x;
  if(g_d3.hidden && depth<=g_d3.depth[index]) return;
  double texture_alpha=1;
  uint32_t color=texture&&texture->kind?d3_sample(R,texture,u,v,&texture_alpha):0xFFFFFFu;
  if(red<0) red=0; else if(red>255) red=255;
  if(green<0) green=0; else if(green>255) green=255;
  if(blue<0) blue=0; else if(blue>255) blue=255;
  int cr=(int)lround((color&255)*red/255.0);
  int cg=(int)lround(((color>>8)&255)*green/255.0);
  int cb=(int)lround(((color>>16)&255)*blue/255.0);
  if(g_d3.lighting){
    cr=(int)(cr*g_d3.shade_r); cg=(int)(cg*g_d3.shade_g); cb=(int)(cb*g_d3.shade_b);
    if(cr>255) cr=255; if(cg>255) cg=255; if(cb>255) cb=255;
  }
  if(g_d3.fog){
    double span=g_d3.fog_end-g_d3.fog_start;
    double amount=span>1e-9?(distance-g_d3.fog_start)/span:(distance>=g_d3.fog_end?1:0);
    if(amount<0) amount=0; else if(amount>1) amount=1;
    uint32_t fog=g_d3.fog_color;
    cr=(int)lround(cr*(1-amount)+(fog&255)*amount);
    cg=(int)lround(cg*(1-amount)+((fog>>8)&255)*amount);
    cb=(int)lround(cb*(1-amount)+((fog>>16)&255)*amount);
  }
  double final_alpha=texture_alpha*alpha;
  draw_px_alpha(R,x,y,(uint32_t)cr|((uint32_t)cg<<8)|((uint32_t)cb<<16),final_alpha);
  if(g_d3.hidden && g_d3.zwrite && final_alpha>0) g_d3.depth[index]=(float)depth;
}
static int d3_clip_segment_plane(GmlD3Vertex *a,GmlD3Vertex *b,double plane,int keep_greater){
  int a_inside=keep_greater?a->z>=plane:a->z<=plane;
  int b_inside=keep_greater?b->z>=plane:b->z<=plane;
  if(!a_inside&&!b_inside) return 0;
  if(a_inside!=b_inside){
    double amount=(plane-a->z)/(b->z-a->z);
    GmlD3Vertex intersection=d3_vertex_lerp(*a,*b,amount); intersection.z=plane;
    if(!a_inside) *a=intersection; else *b=intersection;
  }
  return 1;
}
static void d3_emit_point(GmlRender *R,GmlD3Vertex world,const GmlD3Texture *texture){
  if(g_d3.lighting){ double factor[3]; if(d3_vertex_light_factor(&world,factor)) d3_apply_light_factor(&world,factor); }
  GmlD3Vertex camera=d3_vertex_camera(world);
  double x,y,inverse_z,depth,distance;
  if(!d3_project_vertex(R,&camera,&x,&y,&inverse_z,&depth,&distance)) return;
  (void)inverse_z;
  d3_raster_sample(R,(int)lround(x),(int)lround(y),depth,distance,camera.u,camera.v,
                   camera.r,camera.g,camera.b,camera.alpha,texture);
}
static void d3_emit_line(GmlRender *R,GmlD3Vertex a,GmlD3Vertex b,const GmlD3Texture *texture){
  if(g_d3.lighting){
    double factor[3]; int have=d3_vertex_light_factor(&a,factor);
    if(have) d3_apply_light_factor(&a,factor);
    if(g_d3.smooth){ if(d3_vertex_light_factor(&b,factor)) d3_apply_light_factor(&b,factor); }
    else if(have) d3_apply_light_factor(&b,factor);
  }
  a=d3_vertex_camera(a); b=d3_vertex_camera(b);
  if(!g_d3.ortho){
    double nearz=g_d3.near_clip>1e-6?g_d3.near_clip:.05;
    double farz=g_d3.far_clip>nearz?g_d3.far_clip:32000;
    if(!d3_clip_segment_plane(&a,&b,nearz,1) || !d3_clip_segment_plane(&a,&b,farz,0)) return;
  }
  double ax,ay,a_inverse_z,a_depth,a_distance,bx,by,b_inverse_z,b_depth,b_distance;
  if(!d3_project_vertex(R,&a,&ax,&ay,&a_inverse_z,&a_depth,&a_distance) ||
     !d3_project_vertex(R,&b,&bx,&by,&b_inverse_z,&b_depth,&b_distance)) return;
  int steps=(int)ceil(fmax(fabs(bx-ax),fabs(by-ay)));
  if(steps<1) steps=1;
  for(int i=0;i<=steps;i++){
    double amount=(double)i/steps;
    double inverse_z=a_inverse_z+(b_inverse_z-a_inverse_z)*amount;
    double denominator=g_d3.ortho?1:inverse_z;
#define D3_LINE_ATTR(field) ((a.field*a_inverse_z+(b.field*b_inverse_z-a.field*a_inverse_z)*amount)/denominator)
    double u=D3_LINE_ATTR(u),v=D3_LINE_ATTR(v);
    double red=D3_LINE_ATTR(r),green=D3_LINE_ATTR(g),blue=D3_LINE_ATTR(b);
    double alpha=D3_LINE_ATTR(alpha);
#undef D3_LINE_ATTR
    double distance=g_d3.ortho?a_distance+(b_distance-a_distance)*amount:1.0/inverse_z;
    double depth=a_depth+(b_depth-a_depth)*amount;
    int x=(int)lround(ax+(bx-ax)*amount),y=(int)lround(ay+(by-ay)*amount);
    d3_raster_sample(R,x,y,depth,distance,u,v,red,green,blue,alpha,texture);
  }
}
static void d3_primitive_flush(GmlRender *R){
  if(!R || g_d3_prim_n<=0) return;
  GmlD3Texture texture={0};
  if(g_d3_prim_texture!=-1) d3_texture(R,g_d3_prim_texture,&texture);
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  gml_render_maybe_prepare_draw(R);
  if(!d3_depth_prepare(R)) return;
  if(g_d3_prim_kind==1){
    for(int i=0;i<g_d3_prim_n;i++) d3_emit_point(R,g_d3_prim[i],&texture);
  } else if(g_d3_prim_kind==2){
    for(int i=0;i+1<g_d3_prim_n;i+=2) d3_emit_line(R,g_d3_prim[i],g_d3_prim[i+1],&texture);
  } else if(g_d3_prim_kind==3){
    for(int i=0;i+1<g_d3_prim_n;i++) d3_emit_line(R,g_d3_prim[i],g_d3_prim[i+1],&texture);
  } else if(g_d3_prim_kind==4){
    for(int i=0;i+2<g_d3_prim_n;i+=3) d3_emit_triangle(R,&g_d3_prim[i],&texture);
  } else if(g_d3_prim_kind==5){
    for(int i=2;i<g_d3_prim_n;i++){
      GmlD3Vertex triangle[3];
      if(i&1){ triangle[0]=g_d3_prim[i-1]; triangle[1]=g_d3_prim[i-2]; }
      else { triangle[0]=g_d3_prim[i-2]; triangle[1]=g_d3_prim[i-1]; }
      triangle[2]=g_d3_prim[i]; d3_emit_triangle(R,triangle,&texture);
    }
  } else if(g_d3_prim_kind==6){
    for(int i=1;i+1<g_d3_prim_n;i++){
      GmlD3Vertex triangle[3]={g_d3_prim[0],g_d3_prim[i],g_d3_prim[i+1]};
      d3_emit_triangle(R,triangle,&texture);
    }
  }
}
static int d3_model_grow_vertices(GmlD3Model *model,int needed){
  if(!model || needed<0 || needed>GML_D3_MODEL_VERTEX_MAX) return 0;
  if(needed<=model->vertex_cap) return 1;
  int capacity=model->vertex_cap?model->vertex_cap:64;
  while(capacity<needed){
    if(capacity>GML_D3_MODEL_VERTEX_MAX/2){ capacity=GML_D3_MODEL_VERTEX_MAX; break; }
    capacity*=2;
  }
  GmlD3Vertex *grown=realloc(model->vertex,(size_t)capacity*sizeof(*grown));
  if(!grown) return 0;
  model->vertex=grown; model->vertex_cap=capacity;
  return 1;
}
static int d3_model_grow_batches(GmlD3Model *model,int needed){
  if(!model || needed<0 || needed>GML_D3_MODEL_VERTEX_MAX) return 0;
  if(needed<=model->batch_cap) return 1;
  int capacity=model->batch_cap?model->batch_cap:16;
  while(capacity<needed){
    if(capacity>GML_D3_MODEL_VERTEX_MAX/2){ capacity=GML_D3_MODEL_VERTEX_MAX; break; }
    capacity*=2;
  }
  GmlD3Batch *grown=realloc(model->batch,(size_t)capacity*sizeof(*grown));
  if(!grown) return 0;
  model->batch=grown; model->batch_cap=capacity;
  return 1;
}
static int d3_model_begin_batch(GmlD3Model *model,int kind){
  if(!model || !model->used || kind<1 || kind>6 ||
     !d3_model_grow_batches(model,model->batch_n+1)) return 0;
  model->building=model->batch_n;
  model->batch[model->batch_n++]=(GmlD3Batch){kind,model->vertex_n,0};
  return 1;
}
static int d3_model_append_vertex(GmlD3Model *model,GmlD3Vertex vertex){
  if(!model || !model->used || model->building<0 || model->building>=model->batch_n ||
     !d3_model_grow_vertices(model,model->vertex_n+1)) return 0;
  model->vertex[model->vertex_n++]=vertex;
  model->batch[model->building].count++;
  return 1;
}
static int d3_model_append_quad(GmlD3Model *model,const double point[4][3],double hrepeat,double vrepeat){
  double edge_a[3]={point[1][0]-point[0][0],point[1][1]-point[0][1],point[1][2]-point[0][2]};
  double edge_b[3]={point[2][0]-point[0][0],point[2][1]-point[0][1],point[2][2]-point[0][2]};
  double normal[3]; d3_cross(edge_a,edge_b,normal); int has_normal=d3_normalize(normal);
  static const int corner[6]={0,1,2,0,2,3};
  static const double uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
  for(int i=0;i<6;i++){
    int c=corner[i]; GmlD3Vertex vertex={0};
    vertex.x=point[c][0]; vertex.y=point[c][1]; vertex.z=point[c][2];
    vertex.u=uv[c][0]*hrepeat; vertex.v=uv[c][1]*vrepeat;
    vertex.r=vertex.g=vertex.b=255; vertex.alpha=1;
    vertex.nx=normal[0]; vertex.ny=normal[1]; vertex.nz=normal[2]; vertex.has_normal=has_normal;
    if(!d3_model_append_vertex(model,vertex)) return 0;
  }
  return 1;
}
static int d3_model_add_shape(GmlD3Model *model,int shape,double x1,double y1,double z1,
                              double x2,double y2,double z2,double hrepeat,double vrepeat,
                              int closed,int requested_steps){
  if(!d3_model_begin_batch(model,4)) return 0;
  int ok=1;
  if(shape==14 || shape==15){
    double point[4][3];
    if(shape==15){
      double vertices[4][3]={{x1,y1,z1},{x2,y1,z1},{x2,y2,z2},{x1,y2,z2}};
      memcpy(point,vertices,sizeof(point));
    } else {
      double vertices[4][3]={{x1,y1,z1},{x2,y2,z1},{x2,y2,z2},{x1,y1,z2}};
      memcpy(point,vertices,sizeof(point));
    }
    ok=d3_model_append_quad(model,point,hrepeat,vrepeat);
  } else if(shape==10){
    double face[6][4][3]={
      {{x1,y1,z1},{x2,y1,z1},{x2,y2,z1},{x1,y2,z1}},
      {{x1,y2,z2},{x2,y2,z2},{x2,y1,z2},{x1,y1,z2}},
      {{x1,y1,z2},{x2,y1,z2},{x2,y1,z1},{x1,y1,z1}},
      {{x1,y2,z1},{x2,y2,z1},{x2,y2,z2},{x1,y2,z2}},
      {{x1,y1,z1},{x1,y2,z1},{x1,y2,z2},{x1,y1,z2}},
      {{x2,y1,z2},{x2,y2,z2},{x2,y2,z1},{x2,y1,z1}}};
    for(int i=0;i<6&&ok;i++) ok=d3_model_append_quad(model,face[i],hrepeat,vrepeat);
  } else if(shape==11 || shape==12){
    double cx=(x1+x2)*.5,cy=(y1+y2)*.5,rx=fabs(x2-x1)*.5,ry=fabs(y2-y1)*.5;
    int steps=requested_steps; if(steps<3) steps=3; if(steps>128) steps=128;
    int cone=shape==12;
    for(int i=0;i<steps&&ok;i++){
      double q0=2*M_PI*i/steps,q1=2*M_PI*(i+1)/steps;
      double top0x=cone?cx:cx+cos(q0)*rx,top0y=cone?cy:cy+sin(q0)*ry;
      double top1x=cone?cx:cx+cos(q1)*rx,top1y=cone?cy:cy+sin(q1)*ry;
      double side[4][3]={{cx+cos(q0)*rx,cy+sin(q0)*ry,z1},{cx+cos(q1)*rx,cy+sin(q1)*ry,z1},
                         {top1x,top1y,z2},{top0x,top0y,z2}};
      ok=d3_model_append_quad(model,side,hrepeat/steps,vrepeat);
      if(closed&&ok){
        double cap0[4][3]={{cx,cy,z1},{cx+cos(q1)*rx,cy+sin(q1)*ry,z1},
                            {cx+cos(q0)*rx,cy+sin(q0)*ry,z1},{cx,cy,z1}};
        ok=d3_model_append_quad(model,cap0,hrepeat,vrepeat);
        if(!cone&&ok){
          double cap1[4][3]={{cx,cy,z2},{cx+cos(q0)*rx,cy+sin(q0)*ry,z2},
                              {cx+cos(q1)*rx,cy+sin(q1)*ry,z2},{cx,cy,z2}};
          ok=d3_model_append_quad(model,cap1,hrepeat,vrepeat);
        }
      }
    }
  } else if(shape==13){
    double cx=(x1+x2)*.5,cy=(y1+y2)*.5,cz=(z1+z2)*.5;
    double rx=fabs(x2-x1)*.5,ry=fabs(y2-y1)*.5,rz=fabs(z2-z1)*.5;
    int steps=requested_steps; if(steps<4) steps=4; if(steps>64) steps=64;
    for(int lat=0;lat<steps&&ok;lat++) for(int lon=0;lon<steps*2&&ok;lon++){
      double latitude0=-M_PI*.5+M_PI*lat/steps,latitude1=-M_PI*.5+M_PI*(lat+1)/steps;
      double longitude0=M_PI*lon/steps,longitude1=M_PI*(lon+1)/steps;
      double point[4][3]={{cx+rx*cos(latitude0)*cos(longitude0),cy+ry*cos(latitude0)*sin(longitude0),cz+rz*sin(latitude0)},
                          {cx+rx*cos(latitude0)*cos(longitude1),cy+ry*cos(latitude0)*sin(longitude1),cz+rz*sin(latitude0)},
                          {cx+rx*cos(latitude1)*cos(longitude1),cy+ry*cos(latitude1)*sin(longitude1),cz+rz*sin(latitude1)},
                          {cx+rx*cos(latitude1)*cos(longitude0),cy+ry*cos(latitude1)*sin(longitude0),cz+rz*sin(latitude1)}};
      ok=d3_model_append_quad(model,point,hrepeat/steps,vrepeat/steps);
    }
  } else ok=0;
  model->building=-1;
  if(!ok){
    GmlD3Batch *batch=&model->batch[model->batch_n-1];
    model->vertex_n=batch->first; model->batch_n--;
  }
  return ok;
}
static void d3_model_draw(GmlRender *R,const GmlD3Model *model,double x,double y,double z,int texture_handle){
  if(!R || !model || !model->used) return;
  GmlD3Texture texture={0};
  if(texture_handle!=-1) d3_texture(R,texture_handle,&texture);
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  gml_render_maybe_prepare_draw(R);
  if(!d3_depth_prepare(R)) return;
  for(int b=0;b<model->batch_n;b++){
    const GmlD3Batch *batch=&model->batch[b];
    if(batch->first<0 || batch->count<0 || batch->first+batch->count>model->vertex_n) continue;
#define D3_MODEL_VERTEX(index) ({ GmlD3Vertex _v=model->vertex[batch->first+(index)]; _v.x+=x; _v.y+=y; _v.z+=z; _v; })
    if(batch->kind==1){
      for(int i=0;i<batch->count;i++) d3_emit_point(R,D3_MODEL_VERTEX(i),&texture);
    } else if(batch->kind==2){
      for(int i=0;i+1<batch->count;i+=2) d3_emit_line(R,D3_MODEL_VERTEX(i),D3_MODEL_VERTEX(i+1),&texture);
    } else if(batch->kind==3){
      for(int i=0;i+1<batch->count;i++) d3_emit_line(R,D3_MODEL_VERTEX(i),D3_MODEL_VERTEX(i+1),&texture);
    } else if(batch->kind==4){
      for(int i=0;i+2<batch->count;i+=3){
        GmlD3Vertex triangle[3]={D3_MODEL_VERTEX(i),D3_MODEL_VERTEX(i+1),D3_MODEL_VERTEX(i+2)};
        d3_emit_triangle(R,triangle,&texture);
      }
    } else if(batch->kind==5){
      for(int i=2;i<batch->count;i++){
        GmlD3Vertex triangle[3];
        if(i&1){ triangle[0]=D3_MODEL_VERTEX(i-1); triangle[1]=D3_MODEL_VERTEX(i-2); }
        else { triangle[0]=D3_MODEL_VERTEX(i-2); triangle[1]=D3_MODEL_VERTEX(i-1); }
        triangle[2]=D3_MODEL_VERTEX(i); d3_emit_triangle(R,triangle,&texture);
      }
    } else if(batch->kind==6){
      for(int i=1;i+1<batch->count;i++){
        GmlD3Vertex triangle[3]={D3_MODEL_VERTEX(0),D3_MODEL_VERTEX(i),D3_MODEL_VERTEX(i+1)};
        d3_emit_triangle(R,triangle,&texture);
      }
    }
#undef D3_MODEL_VERTEX
  }
}

typedef struct { unsigned char *data; size_t cap,pos; int ok; } D3BlobW;
typedef struct { const unsigned char *data; size_t cap,pos; int ok; } D3BlobR;
static void d3_blob_write(D3BlobW *writer,const void *data,size_t size){
  if(writer->data){
    if(writer->pos<=writer->cap && size<=writer->cap-writer->pos) memcpy(writer->data+writer->pos,data,size);
    else writer->ok=0;
  }
  if(size>SIZE_MAX-writer->pos){ writer->ok=0; return; }
  writer->pos+=size;
}
static void d3_blob_read(D3BlobR *reader,void *data,size_t size){
  if(reader->pos<=reader->cap && size<=reader->cap-reader->pos) memcpy(data,reader->data+reader->pos,size);
  else { memset(data,0,size); reader->ok=0; }
  if(size>SIZE_MAX-reader->pos){ reader->ok=0; return; }
  reader->pos+=size;
}
static void d3_blob_u32(D3BlobW *writer,uint32_t value){ d3_blob_write(writer,&value,sizeof(value)); }
static uint32_t d3_blob_read_u32(D3BlobR *reader){ uint32_t value=0; d3_blob_read(reader,&value,sizeof(value)); return value; }
static void d3_blob_write_model(D3BlobW *writer,const GmlD3Model *model){
  d3_blob_u32(writer,(uint32_t)model->vertex_n); d3_blob_u32(writer,(uint32_t)model->batch_n);
  for(int i=0;i<model->vertex_n;i++){
    const GmlD3Vertex *vertex=&model->vertex[i];
    const double values[12]={vertex->x,vertex->y,vertex->z,vertex->u,vertex->v,
      vertex->r,vertex->g,vertex->b,vertex->alpha,vertex->nx,vertex->ny,vertex->nz};
    d3_blob_write(writer,values,sizeof(values)); d3_blob_u32(writer,(uint32_t)vertex->has_normal);
  }
  for(int i=0;i<model->batch_n;i++){
    d3_blob_u32(writer,(uint32_t)model->batch[i].kind);
    d3_blob_u32(writer,(uint32_t)model->batch[i].first);
    d3_blob_u32(writer,(uint32_t)model->batch[i].count);
  }
}
static int d3_blob_read_model(D3BlobR *reader,GmlD3Model *model){
  uint32_t vertex_n=d3_blob_read_u32(reader),batch_n=d3_blob_read_u32(reader);
  if(!reader->ok || vertex_n>GML_D3_MODEL_VERTEX_MAX || batch_n>GML_D3_MODEL_VERTEX_MAX ||
     !d3_model_grow_vertices(model,(int)vertex_n) || !d3_model_grow_batches(model,(int)batch_n)) return 0;
  model->vertex_n=(int)vertex_n; model->batch_n=(int)batch_n; model->building=-1;
  for(int i=0;i<model->vertex_n;i++){
    double values[12]; d3_blob_read(reader,values,sizeof(values));
    GmlD3Vertex *vertex=&model->vertex[i];
    vertex->x=values[0]; vertex->y=values[1]; vertex->z=values[2];
    vertex->u=values[3]; vertex->v=values[4]; vertex->r=values[5]; vertex->g=values[6];
    vertex->b=values[7]; vertex->alpha=values[8]; vertex->nx=values[9]; vertex->ny=values[10]; vertex->nz=values[11];
    vertex->has_normal=(int)d3_blob_read_u32(reader);
  }
  for(int i=0;i<model->batch_n;i++){
    GmlD3Batch *batch=&model->batch[i];
    batch->kind=(int)d3_blob_read_u32(reader); batch->first=(int)d3_blob_read_u32(reader);
    batch->count=(int)d3_blob_read_u32(reader);
    if(batch->kind<1 || batch->kind>6 || batch->first<0 || batch->count<0 ||
       batch->first>model->vertex_n-batch->count) reader->ok=0;
  }
  return reader->ok;
}
size_t gml_d3_models_state_size(void){
  D3BlobW writer={.ok=1};
  d3_blob_u32(&writer,0x31534D44u);
  int count=0; for(int i=0;i<GML_D3_MODEL_MAX;i++) if(g_d3_model[i].used) count++;
  d3_blob_u32(&writer,(uint32_t)count);
  for(int i=0;i<GML_D3_MODEL_MAX;i++) if(g_d3_model[i].used){
    d3_blob_u32(&writer,(uint32_t)i); d3_blob_write_model(&writer,&g_d3_model[i]);
  }
  return writer.ok?writer.pos:0;
}
int gml_d3_models_state_save(void *data,size_t capacity){
  D3BlobW writer={.data=data,.cap=capacity,.ok=1};
  d3_blob_u32(&writer,0x31534D44u);
  int count=0; for(int i=0;i<GML_D3_MODEL_MAX;i++) if(g_d3_model[i].used) count++;
  d3_blob_u32(&writer,(uint32_t)count);
  for(int i=0;i<GML_D3_MODEL_MAX;i++) if(g_d3_model[i].used){
    d3_blob_u32(&writer,(uint32_t)i); d3_blob_write_model(&writer,&g_d3_model[i]);
  }
  return writer.ok&&writer.pos==capacity;
}
int gml_d3_models_state_load(const void *data,size_t size){
  d3_models_clear_all();
  if(!data || size<8) return size==0;
  D3BlobR reader={.data=data,.cap=size,.ok=1};
  uint32_t magic=d3_blob_read_u32(&reader),count=d3_blob_read_u32(&reader);
  if(magic!=0x31534D44u || count>GML_D3_MODEL_MAX) reader.ok=0;
  for(uint32_t i=0;i<count&&reader.ok;i++){
    uint32_t id=d3_blob_read_u32(&reader);
    if(id>=GML_D3_MODEL_MAX || g_d3_model[id].used){ reader.ok=0; break; }
    g_d3_model[id].used=1; g_d3_model[id].building=-1;
    if(!d3_blob_read_model(&reader,&g_d3_model[id])) break;
  }
  if(!reader.ok || reader.pos!=size){ d3_models_clear_all(); return 0; }
  return 1;
}
static int d3_model_file_save(const char *path,const GmlD3Model *model){
  if(!path || !model || !model->used) return 0;
  FILE *file=fopen(path,"w"); if(!file) return 0;
  size_t records=(size_t)model->vertex_n+(size_t)model->batch_n*2;
  int ok=fprintf(file,"100\n%zu\n",records)>0;
  for(int b=0;b<model->batch_n&&ok;b++){
    const GmlD3Batch *batch=&model->batch[b];
    ok=fprintf(file,"0 %d\n",batch->kind)>0;
    for(int i=0;i<batch->count&&ok;i++){
      const GmlD3Vertex *vertex=&model->vertex[batch->first+i];
      int red=(int)lround(vertex->r),green=(int)lround(vertex->g),blue=(int)lround(vertex->b);
      if(red<0) red=0; else if(red>255) red=255;
      if(green<0) green=0; else if(green>255) green=255;
      if(blue<0) blue=0; else if(blue>255) blue=255;
      uint32_t color=(uint32_t)red|((uint32_t)green<<8)|((uint32_t)blue<<16);
      ok=fprintf(file,"9 %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %u %.17g\n",
        vertex->x,vertex->y,vertex->z,vertex->nx,vertex->ny,vertex->nz,
        vertex->u,vertex->v,color,vertex->alpha)>0;
    }
    if(ok) ok=fprintf(file,"1\n")>0;
  }
  if(fclose(file)!=0) ok=0;
  return ok;
}
static int d3_model_file_load(const char *path,GmlD3Model *model){
  if(!path || !model || !model->used) return 0;
  d3_model_clear_data(model);
  FILE *file=fopen(path,"r"); if(!file) return 0;
  unsigned header=0,record_count=0;
  int ok=fscanf(file,"%u",&header)==1 && header==100 && fscanf(file,"%u",&record_count)==1 &&
         record_count<=GML_D3_MODEL_VERTEX_MAX*2u;
  char line[2048]; if(ok) (void)fgets(line,sizeof(line),file);
  for(unsigned record=0;record<record_count&&ok;record++){
    if(!fgets(line,sizeof(line),file)){ ok=0; break; }
    char *cursor=line,*end=NULL; double token[16]; int token_n=0;
    while(token_n<16){
      while(*cursor==' '||*cursor=='\t'||*cursor=='\r'||*cursor=='\n') cursor++;
      if(!*cursor) break;
      token[token_n]=strtod(cursor,&end); if(end==cursor){ ok=0; break; }
      token_n++; cursor=end;
    }
    if(!ok || token_n<1) { ok=0; break; }
    int opcode=(int)token[0];
    if(opcode==0){ ok=token_n>=2&&d3_model_begin_batch(model,(int)token[1]); continue; }
    if(opcode==1){ model->building=-1; continue; }
    if(opcode>=2&&opcode<=9){
      GmlD3Vertex vertex={0}; vertex.r=vertex.g=vertex.b=255; vertex.alpha=1;
      if(token_n<4){ ok=0; break; }
      vertex.x=token[1]; vertex.y=token[2]; vertex.z=token[3]; int index=4;
      int normal=opcode>=6,texture=opcode==4||opcode==5||opcode==8||opcode==9;
      int colored=opcode==3||opcode==5||opcode==7||opcode==9;
      if(normal){ if(token_n<index+3){ ok=0; break; }
        vertex.nx=token[index++]; vertex.ny=token[index++]; vertex.nz=token[index++]; vertex.has_normal=1; }
      if(texture){ if(token_n<index+2){ ok=0; break; } vertex.u=token[index++]; vertex.v=token[index++]; }
      if(colored){ if(token_n<index+2){ ok=0; break; } uint32_t color=(uint32_t)token[index++];
        vertex.r=color&255; vertex.g=(color>>8)&255; vertex.b=(color>>16)&255; vertex.alpha=token[index++]; }
      ok=d3_model_append_vertex(model,vertex); continue;
    }
    if(opcode>=10&&opcode<=15){
      if(token_n<9){ ok=0; break; }
      double vrepeat=token[8];
      int closed=1,steps=16;
      if(opcode==11||opcode==12){
        int base=9;
        if(token_n>=12){ vrepeat=token[9]; base=10; }
        if(token_n<=base+1){ ok=0; break; }
        closed=(int)token[base]; steps=(int)token[base+1];
      } else if(opcode==13){
        int at=9; if(token_n>=11){ vrepeat=token[9]; at=10; }
        if(token_n<=at){ ok=0; break; } steps=(int)token[at];
      } else if(token_n>=10) vrepeat=token[9];
      ok=d3_model_add_shape(model,opcode,token[1],token[2],token[3],token[4],token[5],token[6],token[7],vrepeat,closed,steps);
      continue;
    }
    ok=0;
  }
  if(fclose(file)!=0) ok=0;
  if(!ok) d3_model_clear_data(model);
  return ok;
}
static void draw_rect_prim_alpha(GmlRender *R, int x1, int y1, int x2, int y2, uint32_t gmcol, int outline, double alpha){
  if(!R) return;
  if(x1>x2){ int t=x1; x1=x2; x2=t; } if(y1>y2){ int t=y1; y1=y2; y2=t; }
  if(x2<0||y2<0||x1>=R->fbw||y1>=R->fbh) return;
  if(x1<0)x1=0; if(y1<0)y1=0; if(x2>=R->fbw)x2=R->fbw-1; if(y2>=R->fbh)y2=R->fbh-1;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(!outline && alpha<=0) return;
  if(!outline && R->blendmode==0 && (alpha>=1 || !R->alphablend)){   /* opaque filled rect: fast per-row fill */
    gml_render_maybe_prepare_opaque_rect(R,x1,y1,x2+1,y2+1);
    uint32_t src=gm_color_to_xrgb(gmcol);
    if(x1==0 && y1==0 && x2==R->fbw-1 && y2==R->fbh-1){
      gml_render_set_pending_fill(R,src);
      return;
    }
    for(int y=y1;y<=y2;y++) fill_xrgb_run(R->fb+(size_t)y*R->fbw+x1,x2-x1+1,src);
    return;
  }
  if(!outline){
    gml_render_maybe_prepare_draw(R);
    if(R->blendmode!=0){
      for(int y=y1;y<=y2;y++) for(int x=x1;x<=x2;x++) draw_px_alpha(R,x,y,gmcol,alpha);
      return;
    }
    uint32_t src=gm_color_to_xrgb(gmcol);
    for(int y=y1;y<=y2;y++)
      draw_xrgb_run_alpha(R,&R->fb[(size_t)y*R->fbw+x1],x2-x1+1,src,alpha);
    return;
  }
  for(int y=y1;y<=y2;y++) for(int x=x1;x<=x2;x++){
    if(outline && y>y1 && y<y2 && x>x1 && x<x2) continue;
    draw_px_alpha(R,x,y,gmcol,alpha);
  }
}
static void draw_rect_prim(GmlRender *R, int x1, int y1, int x2, int y2, uint32_t gmcol, int outline){
  draw_rect_prim_alpha(R,x1,y1,x2,y2,gmcol,outline,R?R->alpha:1);
}
/* Full-screen color fill for spriteless GMS2 background layers (screen space, at layer depth). */
void gml_draw_layer_color_fill(GmlRender *R, uint32_t gmcol, double alpha){
  if(!R) return;
  draw_rect_prim_alpha(R,0,0,R->fbw-1,R->fbh-1,gmcol,0,alpha);
}
static void draw_rect_colour_prim(GmlRender *R, int x1, int y1, int x2, int y2,
                                  uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4,
                                  int outline){
  if(!R) return;
  if(c1==c2 && c1==c3 && c1==c4){
    draw_rect_prim(R,x1,y1,x2,y2,c1,outline);
    return;
  }
  if(outline){
    draw_rect_prim(R,x1,y1,x2,y2,c1,outline);
    return;
  }
  if(x1>x2){ int t=x1; x1=x2; x2=t; } if(y1>y2){ int t=y1; y1=y2; y2=t; }
  if(x2<0||y2<0||x1>=R->fbw||y1>=R->fbh) return;
  if(x1<0)x1=0; if(y1<0)y1=0; if(x2>=R->fbw)x2=R->fbw-1; if(y2>=R->fbh)y2=R->fbh-1;
  double alpha=R->alpha; if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(alpha<=0) return;
  if(alpha>=1.0 || !R->alphablend) gml_render_maybe_prepare_opaque_rect(R,x1,y1,x2+1,y2+1);
  else gml_render_maybe_prepare_draw(R);
  uint32_t tl=gm_color_to_xrgb(c1), tr=gm_color_to_xrgb(c2), br=gm_color_to_xrgb(c3), bl=gm_color_to_xrgb(c4);
  int wden=x2-x1, hden=y2-y1, n=x2-x1+1;
  if(tl==tr && bl==br){
    for(int y=y1;y<=y2;y++){
      uint32_t rowc=xrgb_lerp(tl,bl,y-y1,hden);
      draw_xrgb_run_alpha(R,R->fb+(size_t)y*R->fbw+x1,n,rowc,alpha);
    }
    return;
  }
  for(int y=y1;y<=y2;y++){
    uint32_t lc=xrgb_lerp(tl,bl,y-y1,hden);
    uint32_t rc=xrgb_lerp(tr,br,y-y1,hden);
    uint32_t *row=R->fb+(size_t)y*R->fbw+x1;
    if(lc==rc){ draw_xrgb_run_alpha(R,row,n,lc,alpha); continue; }
    for(int x=0;x<n;x++){
      uint32_t c=xrgb_lerp(lc,rc,x,wden);
      draw_xrgb_run_alpha(R,row+x,1,c,alpha);
    }
  }
}
static void draw_line_prim(GmlRender *R, int x0, int y0, int x1, int y1, uint32_t gmcol, int width){
  if(width<1) width=1;
  int dx=abs(x1-x0), sx=x0<x1?1:-1;
  int dy=-abs(y1-y0), sy=y0<y1?1:-1;
  int err=dx+dy;
  for(;;){
    int r=width/2;
    draw_rect_prim(R,x0-r,y0-r,x0-r+width-1,y0-r+width-1,gmcol,0);
    if(x0==x1 && y0==y1) break;
    int e2=2*err;
    if(e2>=dy){ err+=dy; x0+=sx; }
    if(e2<=dx){ err+=dx; y0+=sy; }
  }
}
static int point_in_poly(double px, double py, const double *vx, const double *vy, int n){
  int inside=0;
  for(int i=0,j=n-1;i<n;j=i++){
    int crosses=((vy[i]>py)!=(vy[j]>py));
    if(crosses){
      double xint=(vx[j]-vx[i])*(py-vy[i])/(vy[j]-vy[i]) + vx[i];
      if(px<xint) inside=!inside;
    }
  }
  return inside;
}
static void draw_circle_prim(GmlRender *R, int cx, int cy, int rx, int ry, uint32_t gmcol, int outline){
  if(!R||rx<0||ry<0) return;
  if(rx==0||ry==0){ draw_px(R,cx,cy,gmcol); return; }
  int n=R->circle_precision;
  if(n<4) n=4;
  if(n>64) n=64;
  n=(n/4)*4; if(n<4) n=4;
  double vx[64], vy[64];
  for(int i=0;i<n;i++){
    double a=(i*2.0*M_PI)/n;
    vx[i]=cx+cos(a)*rx;
    vy[i]=cy-sin(a)*ry;
  }
  if(outline){
    for(int i=0;i<n;i++){
      int j=(i+1)%n;
      draw_line_prim(R,(int)lround(vx[i]),(int)lround(vy[i]),(int)lround(vx[j]),(int)lround(vy[j]),gmcol,1);
    }
  } else {
    /* The regular polygon is convex, so each scanline has at most one filled span. This is
     * pixel-identical to point_in_poly(x+0.5,y+0.5) but avoids testing every edge per pixel. */
    for(int y=cy-ry;y<=cy+ry;y++){
      double py=y+0.5, xl=1e30, xr=-1e30;
      for(int i=0,j=n-1;i<n;j=i++){
        if((vy[i]>py)==(vy[j]>py)) continue;
        double xi=(vx[j]-vx[i])*(py-vy[i])/(vy[j]-vy[i])+vx[i];
        if(xi<xl) xl=xi;
        if(xi>xr) xr=xi;
      }
      if(xr<xl) continue;
      int x0=(int)ceil(xl-0.5), x1=(int)ceil(xr-0.5)-1;
      if(x0<cx-rx) x0=cx-rx;
      if(x1>cx+rx) x1=cx+rx;
      for(int x=x0;x<=x1;x++) draw_px(R,x,y,gmcol);
    }
  }
}

/* immediate-mode primitive buffer: draw_primitive_begin + draw_vertex... + end.
 * Transient within a single draw call, so plain statics are safe. */
#define GML_PRIM_MAX 256
static int g_prim_kind=0, g_prim_n=0;
static double g_prim_x[GML_PRIM_MAX], g_prim_y[GML_PRIM_MAX], g_prim_a[GML_PRIM_MAX];
static uint32_t g_prim_c[GML_PRIM_MAX];
static void prim_tri_fill_ex(GmlRender *R, double X1,double Y1,double X2,double Y2,double X3,double Y3,
                             uint32_t col, double alpha, int outline){
  if(!R||!R->fb||R->fbw<=0||R->fbh<=0) return;
  if(!isfinite(X1)||!isfinite(Y1)||!isfinite(X2)||!isfinite(Y2)||!isfinite(X3)||!isfinite(Y3)) return;
  if(!isfinite(alpha)) return;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(alpha<=0) return;
  double den=(Y2-Y3)*(X1-X3)+(X3-X2)*(Y1-Y3);
  if(!isfinite(den)||fabs(den)<1e-9) return;
  double minxd=floor(fmin(X1,fmin(X2,X3))), maxxd=floor(fmax(X1,fmax(X2,X3)));
  double minyd=floor(fmin(Y1,fmin(Y2,Y3))), maxyd=floor(fmax(Y1,fmax(Y2,Y3)));
  if(maxxd<0.0||maxyd<0.0||minxd>(double)(R->fbw-1)||minyd>(double)(R->fbh-1)) return;
  int minx=minxd<0.0?0:(int)minxd, maxx=maxxd>=(double)R->fbw?R->fbw-1:(int)maxxd;
  int miny=minyd<0.0?0:(int)minyd, maxy=maxyd>=(double)R->fbh?R->fbh-1:(int)maxyd;
  for(int yy=miny;yy<=maxy;yy++) for(int xx=minx;xx<=maxx;xx++){
    double a0=((Y2-Y3)*(xx-X3)+(X3-X2)*(yy-Y3))/den;
    double b0=((Y3-Y1)*(xx-X3)+(X1-X3)*(yy-Y3))/den;
    double c0=1-a0-b0;
    if(outline && a0>0.04&&b0>0.04&&c0>0.04) continue;
    if(a0>=0&&b0>=0&&c0>=0) draw_px_alpha(R,xx,yy,col,alpha);
  }
}
static void prim_tri_fill(GmlRender *R, double X1,double Y1,double X2,double Y2,double X3,double Y3,
                          uint32_t col, double alpha){
  prim_tri_fill_ex(R,X1,Y1,X2,Y2,X3,Y3,col,alpha,0);
}
static void prim_flush(GmlRender *R){
  int n=g_prim_n; if(!R||n<1) return;
  gml_render_maybe_prepare_draw(R);
  double cx=R->cam_x, cy=R->cam_y;
  switch(g_prim_kind){
    case 1: for(int i=0;i<n;i++) draw_px_alpha(R,(int)floor(g_prim_x[i]-cx),(int)floor(g_prim_y[i]-cy),g_prim_c[i],g_prim_a[i]); break;
    case 2: for(int i=0;i+1<n;i+=2) draw_line_prim(R,(int)floor(g_prim_x[i]-cx),(int)floor(g_prim_y[i]-cy),(int)floor(g_prim_x[i+1]-cx),(int)floor(g_prim_y[i+1]-cy),g_prim_c[i],1); break;
    case 3: for(int i=0;i+1<n;i++)  draw_line_prim(R,(int)floor(g_prim_x[i]-cx),(int)floor(g_prim_y[i]-cy),(int)floor(g_prim_x[i+1]-cx),(int)floor(g_prim_y[i+1]-cy),g_prim_c[i],1); break;
    case 4: for(int i=0;i+2<n;i+=3) prim_tri_fill(R,g_prim_x[i]-cx,g_prim_y[i]-cy,g_prim_x[i+1]-cx,g_prim_y[i+1]-cy,g_prim_x[i+2]-cx,g_prim_y[i+2]-cy,g_prim_c[i],g_prim_a[i]); break;
    case 5: for(int i=0;i+2<n;i++)  prim_tri_fill(R,g_prim_x[i]-cx,g_prim_y[i]-cy,g_prim_x[i+1]-cx,g_prim_y[i+1]-cy,g_prim_x[i+2]-cx,g_prim_y[i+2]-cy,g_prim_c[i],g_prim_a[i]); break;
    case 6: for(int i=1;i+1<n;i++)  prim_tri_fill(R,g_prim_x[0]-cx,g_prim_y[0]-cy,g_prim_x[i]-cx,g_prim_y[i]-cy,g_prim_x[i+1]-cx,g_prim_y[i+1]-cy,g_prim_c[i],g_prim_a[i]); break;
    default: break;
  }
}

/* GM round(): round half to even (banker's). */
static double gm_round(double x){
  double f=floor(x), diff=x-f;
  if(diff<0.5) return f; if(diff>0.5) return f+1;
  return (fmod(f,2.0)==0.0)? f : f+1;
}
static double gm_sign(double x){ return x>0?1:(x<0?-1:0); }
static void motion_from_components(GmlInstance *in){
  in->speed=hypot(in->hspeed,in->vspeed);
  in->direction=atan2(-in->vspeed,in->hspeed)*180.0/M_PI;
  if(in->direction<0) in->direction+=360;
}

static double physics_room_scale(GmlVM *vm){
  if(!vm || !vm->win) return 0.0;
  GmlVal *dynamic=gml_varmap_get(&vm->globals,"__physics_world_scale");
  GmlVal *dynamic_room=gml_varmap_get(&vm->globals,"__physics_world_scale_room");
  if(dynamic && dynamic->t==V_REAL && dynamic->d>0.0 && dynamic_room &&
     dynamic_room->t==V_REAL && (int)dynamic_room->d==vm->room_index) return dynamic->d;
  const GmlChunk *rc=gml_chunk(vm->win,"ROOM");
  if(!rc || vm->room_index<0) return 0.0;
  uint64_t rend=(uint64_t)rc->off+rc->size;
  uint64_t slot=(uint64_t)rc->off+4u+(uint64_t)(uint32_t)vm->room_index*4u;
  if(slot+4u>rend) return 0.0;
  const uint8_t *d=vm->win->data;
  uint32_t rp=u32(d,(uint32_t)slot);
  if(rp<rc->off || (uint64_t)rp+88u>rend || u32(d,rp+56)!=1) return 0.0;
  uint32_t bits=u32(d,rp+84); float scale;
  memcpy(&scale,&bits,sizeof(scale));
  return isfinite(scale) && scale>0.000001f && scale<=1000.0f ? scale : 0.0;
}

static double physics_instance_mass(GmlVM *vm, GmlInstance *in, double scale){
  if(!vm || !in || scale<=0.0) return 0.0;
  GmlVal *explicit_mass=gml_varmap_get(&in->vars,"phy_mass");
  if(explicit_mass && explicit_mass->t==V_REAL && explicit_mass->d>0.0) return explicit_mass->d;
  if(in->obj<0 || in->obj>=vm->n_objects) return 0.0;
  GmlObject *o=&vm->objects[in->obj];
  if(!o->physics_enabled || o->physics_kinematic || o->physics_density<=0.0 || o->physics_area_px<=0.0)
    return 0.0;
  return o->physics_density*o->physics_area_px*scale*scale;
}

/* hooks provided elsewhere */
extern int gml_input_key(int vk, int edge);       /* edge: 0=held,1=pressed,2=released; ret 0/1 */
extern int gml_input_gamepad(int button, int edge);
void __attribute__((weak)) gml_input_key_clear(int vk){ (void)vk; }
void __attribute__((weak)) gml_input_key_press(int vk){ (void)vk; }
void __attribute__((weak)) gml_input_key_release(int vk){ (void)vk; }
int __attribute__((weak)) gml_input_gamepad_connected(int device){ return device == 0; }
int __attribute__((weak)) gml_input_gamepad_device_count(void){ return 1; }
double __attribute__((weak)) gml_input_gamepad_axis(int device, int axis){
  (void)device; (void)axis; return 0.0;
}
void __attribute__((weak)) gml_input_gamepad_set_vibration(int device, double low, double high){
  (void)device; (void)low; (void)high;
}

#define GML_GP_AXIS_LH 32785
#define GML_GP_AXIS_LV 32786
#define GML_GP_AXIS_RH 32787
#define GML_GP_AXIS_RV 32788
#define GML_GP_MAX_DEV 16
static double g_gp_deadzone[GML_GP_MAX_DEV];
static unsigned char g_gp_deadzone_set[GML_GP_MAX_DEV];
static int gp_axis_const(int ax){
  switch(ax){
    case 0: case GML_GP_AXIS_LH: return GML_GP_AXIS_LH;
    case 1: case GML_GP_AXIS_LV: return GML_GP_AXIS_LV;
    case 2: case GML_GP_AXIS_RH: return GML_GP_AXIS_RH;
    case 3: case GML_GP_AXIS_RV: return GML_GP_AXIS_RV;
    default: return 0;
  }
}
static double gp_deadzone_get(int dev){
  if(dev >= 0 && dev < GML_GP_MAX_DEV && g_gp_deadzone_set[dev]) return g_gp_deadzone[dev];
  return 0.2;  /* GameMaker's standard default deadzone. */
}
static void gp_deadzone_set(int dev, double dz){
  if(dev < 0 || dev >= GML_GP_MAX_DEV) return;
  if(!isfinite(dz)) dz = 0.0;
  if(dz < 0.0) dz = 0.0;
  if(dz > 1.0) dz = 1.0;
  g_gp_deadzone[dev] = dz;
  g_gp_deadzone_set[dev] = 1;
}
void gml_gamepad_set_axis_deadzone_direct(int device, double dz){
  gp_deadzone_set(device,dz);
}
static double gp_axis_digital_fallback(int ax){
  if(ax == GML_GP_AXIS_LH) return gml_input_gamepad(32784,0) - gml_input_gamepad(32783,0);  /* R - L */
  if(ax == GML_GP_AXIS_LV) return gml_input_gamepad(32782,0) - gml_input_gamepad(32781,0);  /* D - U */
  return 0.0;
}
static double gp_axis_value_filtered(int dev, int ax){
  ax = gp_axis_const(ax);
  if(!ax) return 0.0;
  double v = gml_input_gamepad_axis(dev, ax);
  if(v == 0.0)
    v = gp_axis_digital_fallback(ax);   /* Preserve the existing RetroPad/digital-keyboard transport. */
  if(!isfinite(v)) v = 0.0;
  if(v < -1.0) v = -1.0;
  if(v > 1.0) v = 1.0;
  double dz = gp_deadzone_get(dev);
  return fabs(v) < dz ? 0.0 : v;
}
/* mouse state in GM coordinate spaces; the libretro frontend provides the strong definition */
void __attribute__((weak)) gml_input_mouse(double *rx, double *ry, double *gx, double *gy,
                                           double *wx, double *wy,
                                           int *held, int *pressed, int *released, int *wheel){
  if(rx)*rx=0; if(ry)*ry=0; if(gx)*gx=0; if(gy)*gy=0; if(wx)*wx=0; if(wy)*wy=0;
  if(held)*held=0; if(pressed)*pressed=0; if(released)*released=0; if(wheel)*wheel=0;
}
void __attribute__((weak)) gml_input_mouse_set(double x, double y){ (void)x; (void)y; }
/* GM mouse button constant -> state bitmask: mb_left=1,mb_right=2,mb_middle=3,mb_any=-1,mb_none=0 */
static int mb_mask(int mb){
  switch(mb){ case 1: return 1; case 2: return 2; case 3: return 4; case -1: return 7; default: return 0; }
}
static int mouse_btn_check(int mb, int edge){
  int held, pressed, released;
  gml_input_mouse(NULL,NULL,NULL,NULL,NULL,NULL,&held,&pressed,&released,NULL);
  int m = mb_mask(mb);
  int v = edge==1 ? pressed : edge==2 ? released : held;
  int r = (mb==0) ? (edge==0 ? !(held&7) : 0) : ((v & m) != 0);
  if(getenv("GML_DBG_MOUSE") && r){ extern long g_vm_frame;
    double rx,ry; gml_input_mouse(&rx,&ry,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
    fprintf(stderr,"[mouse] f%ld btn mb=%d edge=%d -> 1 at room(%.0f,%.0f)\n",g_vm_frame,mb,edge,rx,ry); }
  return r;
}

/* current room's ROOM index -> play-order position */
static int order_pos(GmlVM *vm, int room_idx){
  for(int i=0;i<vm->win->n_room_order;i++) if((int)vm->win->room_order[i]==room_idx) return i;
  return -1;
}
static int script_code_of(GmlVM *vm, int sid){
  const GmlChunk *c=gml_chunk(vm->win,"SCPT"); if(!c) return -1;
  const uint8_t *d=vm->win->data; uint32_t n=u32(d,c->off);
  if(sid<0||(uint32_t)sid>=n) return -1;
  uint32_t p=u32(d,c->off+4+sid*4);
  return (int)u32(d,p+4);  /* codeId */
}
static int script_ref_code_of(GmlVM *vm, GmlVal v){
  if(v.t!=V_REAL) return -1;
  int iv=(int)v.d;
  if(GML_IS_FUNCVAL(iv)) return iv & 0x00FFFFFF;
  if(GML_IS_STRUCT_ID(v.d)){
    GmlInstance *bm=gml_struct_find(vm,(unsigned)v.d);
    GmlVal *pf=bm?gml_varmap_get(&bm->vars,"__fn"):NULL;
    if(pf && pf->t==V_REAL){
      int f=(int)pf->d;
      if(GML_IS_FUNCVAL(f)) return f & 0x00FFFFFF;
    }
  }
  return script_code_of(vm,iv);
}

/* ACRV curve records contain inline channels and points. Evaluate linearly between knots and clamp at the ends. */
static float acrv_f32(const uint8_t *d, uint32_t o){ float f; memcpy(&f,d+o,4); return f; }
static uint32_t acrv_curve_ptr(GmlVM *vm, int idx){
  const GmlChunk *c=gml_chunk(vm->win,"ACRV"); if(!c) return 0;
  const uint8_t *d=vm->win->data; uint32_t n=u32(d,c->off+4);
  if(idx<0||(uint32_t)idx>=n) return 0;
  return u32(d,c->off+8+idx*4);
}
static uint32_t acrv_channel_ptr(GmlVM *vm, int curve, int ch){
  uint32_t cp=acrv_curve_ptr(vm,curve); if(!cp) return 0;
  const uint8_t *d=vm->win->data; uint32_t nch=u32(d,cp+8);
  if(ch<0||(uint32_t)ch>=nch) return 0;
  uint32_t p=cp+12;
  for(int i=0;i<ch;i++){ uint32_t npts=u32(d,p+12); p+=16+npts*24; }
  return p;
}
#define GML_ACRV_TAG 0x52000000
static double acrv_evaluate(GmlVM *vm, int curve, int ch, double x){
  uint32_t p=acrv_channel_ptr(vm,curve,ch); if(!p) return 0;
  const uint8_t *d=vm->win->data; uint32_t npts=u32(d,p+12);
  if(!npts) return 0;
  uint32_t pt=p+16;
  double x0=acrv_f32(d,pt), y0=acrv_f32(d,pt+4);
  if(npts==1 || x<=x0) return y0;
  for(uint32_t i=1;i<npts;i++){
    double x1=acrv_f32(d,pt+i*24), y1=acrv_f32(d,pt+i*24+4);
    if(x<=x1){ double t=(x1>x0)?(x-x0)/(x1-x0):0; return y0+(y1-y0)*t; }
    x0=x1; y0=y1;
  }
  return acrv_f32(d,pt+(npts-1)*24+4);   /* past the last knot */
}

/* mask bbox (world coords) of an instance placed at (atx,aty), from its sprite margins. */
static int inst_mask_sprite_index(GmlInstance *in){
  return in->mask_index>=0 ? (int)in->mask_index : (int)in->sprite_index;
}
static int target_matches_instance(GmlVM *vm, GmlInstance *self, GmlInstance *o, int target){
  if(!o || !o->active || o->marked) return 0;
  if(target==IT_NOONE) return 0;
  if(target==IT_ALL) return 1;
  if(target==IT_SELF) return o==self;
  if(target==IT_OTHER) return o==vm->cur_other;
  if(target>=100000) return (int)o->id==target;
  return target>=0 && gml_object_is(vm,o->obj,target);
}
static int inst_bbox(GmlVM *vm, GmlInstance *in, double atx, double aty,
                     double *l, double *t, double *r, double *b){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return 0;
  int si=inst_mask_sprite_index(in); if(si<0||si>=R->n_spr) return 0;
  GmlSprite *s=&R->spr[si]; if(s->mr<s->ml || s->mb<s->mt) return 0;
  double xs=in->image_xscale, ys=in->image_yscale;
  if(fabs(xs)<1e-9 || fabs(ys)<1e-9) return 0;
  if(in->image_angle==0){
    /* unrotated fast path — the overwhelming majority of collision candidates (terrain) sit at
     * angle 0; the generic path costs a cos+sin per candidate per query (many trig calls per frame in a
     * large level: every place_meeting ground probe scans the instance list). */
    double px0=(s->ml-s->originx)*xs, px1=(s->mr+1.0-s->originx)*xs;
    double py0=(s->mt-s->originy)*ys, py1=(s->mb+1.0-s->originy)*ys;
    double minx = px0<px1? px0:px1, maxx = px0<px1? px1:px0;
    double miny = py0<py1? py0:py1, maxy = py0<py1? py1:py0;
    *l=floor(atx+minx); *t=floor(aty+miny); *r=ceil(atx+maxx)-1.0; *b=ceil(aty+maxy)-1.0; return 1;
  }
  double ang=in->image_angle*M_PI/180.0, c=cos(ang), sn=sin(ang);
  double minx=1e30,miny=1e30,maxx=-1e30,maxy=-1e30;
  double x0=s->ml, x1=s->mr+1.0, y0=s->mt, y1=s->mb+1.0;
  double corners[4][2]={{x0,y0},{x1,y0},{x0,y1},{x1,y1}};
  for(int i=0;i<4;i++){
    double px=(corners[i][0]-s->originx)*xs, py=(corners[i][1]-s->originy)*ys;
    double wx=atx + px*c + py*sn;
    double wy=aty - px*sn + py*c;
    if(wx<minx) minx=wx;
    if(wx>maxx) maxx=wx;
    if(wy<miny) miny=wy;
    if(wy>maxy) maxy=wy;
  }
  *l=floor(minx); *t=floor(miny); *r=ceil(maxx)-1.0; *b=ceil(maxy)-1.0; return 1;
}
static int bbox_overlap(double l1,double t1,double r1,double b1, double l2,double t2,double r2,double b2){
  return l1<=r2 && l2<=r1 && t1<=b2 && t2<=b1;
}
static int mask_hit_world(GmlRender *R, GmlInstance *in, GmlSprite *s, int sprite, double atx, double aty, int wx, int wy){
  double xs=in->image_xscale, ys=in->image_yscale;
  if(fabs(xs)<1e-9 || fabs(ys)<1e-9) return 0;
  if(in->image_angle==0){   /* unrotated fast path: the generic one pays cos+sin PER PIXEL */
    int lx=(int)floor(((double)wx-atx)/xs + s->originx);
    int ly=(int)floor(((double)wy-aty)/ys + s->originy);
    return gml_sprite_collision(R,sprite,(int)in->image_index,lx,ly);
  }
  double ang=in->image_angle*M_PI/180.0, c=cos(ang), sn=sin(ang);
  double rx=(double)wx-atx, ry=(double)wy-aty;
  double sxr=rx*c - ry*sn, syr=rx*sn + ry*c;
  int lx=(int)floor(sxr/xs + s->originx);
  int ly=(int)floor(syr/ys + s->originy);
  return gml_sprite_collision(R,sprite,(int)in->image_index,lx,ly);
}
/* Per-pixel (precise) mask overlap of self placed at (sx,sy) vs another
 * instance at its own position, scanned over the bbox intersection. This
 * matters for shaped collision masks such as diagonal slopes. Falls back to
 * "overlap" if a sprite is missing. */
static int masks_overlap(GmlVM *vm, GmlInstance *self, double sx, double sy, GmlInstance *o){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return 1;
  int ss=inst_mask_sprite_index(self), os=inst_mask_sprite_index(o); if(ss<0||os<0) return 1;
  GmlSprite *sp=&R->spr[ss], *op=&R->spr[os];
  double sl,st,sr,sb,ol,ot,orr,ob;
  if(!inst_bbox(vm,self,sx,sy,&sl,&st,&sr,&sb)) return 0;
  if(!inst_bbox(vm,o,o->x,o->y,&ol,&ot,&orr,&ob)) return 0;
  int x0=(int)floor(fmax(sl,ol)), x1=(int)ceil(fmin(sr,orr)+1.0);
  int y0=(int)floor(fmax(st,ot)), y1=(int)ceil(fmin(sb,ob)+1.0);
  /* GM bbox coordinates are inclusive. A common grounding probe is place_meeting(x,y+1,...),
   * where the moved mask's bottom row exactly equals the wall's top row. The bbox precheck
   * treats that as overlap, so the mask scan must include that single edge row/column too. */
  if(x1==x0) x1++;
  if(y1==y0) y1++;
  for(int wy=y0; wy<y1; wy++) for(int wx=x0; wx<x1; wx++){
    if(!mask_hit_world(R,self,sp,ss,sx,sy,wx,wy)) continue;
    if( mask_hit_world(R,o,op,os,o->x,o->y,wx,wy)) return 1;
  }
  return 0;
}
/* Bucket instance bounding boxes lazily per frame. Queries combine grid candidates
 * with the touched-instance overlay, then perform exact tests using live data.
 * GML_NO_COLGRID selects linear scanning; GML_DBG_GRIDCHECK compares candidate paths. */
static GmlVM *g_cg_vm=NULL;   /* owner of the current build (single-VM in practice; guarded) */
void gml_colgrid_invalidate(GmlVM *vm){ if(vm) vm->cg_built_frame=-1; if(g_cg_vm==vm) g_cg_vm=NULL; }
void gml_colgrid_touch(GmlInstance *in){
  GmlVM *vm=g_cg_vm; if(!vm || !in) return;
  extern long g_vm_frame;
  if(vm->cg_built_frame!=g_vm_frame) return;                    /* no valid build to patch */
  if(in<vm->inst || in>=vm->inst+vm->inst_cap){ vm->cg_built_frame=-1; return; }  /* foreign VM */
  if(in->cg_touch==vm->cg_gen) return;                          /* already in the overlay */
  in->cg_touch=vm->cg_gen;
  if(vm->cg_overlay_n>=vm->cg_overlay_cap){
    int nc=vm->cg_overlay_cap? vm->cg_overlay_cap*2 : 256;
    int *p=realloc(vm->cg_overlay,nc*sizeof(int)); if(!p){ vm->cg_built_frame=-1; return; }
    vm->cg_overlay=p; vm->cg_overlay_cap=nc;
  }
  vm->cg_overlay[vm->cg_overlay_n++]=(int)(in-vm->inst);
}
static void cg_cell_range(GmlVM *vm, double l, double t, double r, double b,
                          int *cx0,int *cy0,int *cx1,int *cy1){
  /* Clamp both ends of each axis to grid bounds. Out-of-grid boxes and probes use border cells. */
  int x0=(int)floor((l-vm->cg_ox)/vm->cg_cell), y0=(int)floor((t-vm->cg_oy)/vm->cg_cell);
  int x1=(int)floor((r-vm->cg_ox)/vm->cg_cell), y1=(int)floor((b-vm->cg_oy)/vm->cg_cell);
  if(x0<0)x0=0; if(x0>=vm->cg_cw)x0=vm->cg_cw-1;
  if(y0<0)y0=0; if(y0>=vm->cg_ch)y0=vm->cg_ch-1;
  if(x1<0)x1=0; if(x1>=vm->cg_cw)x1=vm->cg_cw-1;
  if(y1<0)y1=0; if(y1>=vm->cg_ch)y1=vm->cg_ch-1;
  if(x1<x0)x1=x0; if(y1<y0)y1=y0;
  *cx0=x0;*cy0=y0;*cx1=x1;*cy1=y1;
}
static int cg_build(GmlVM *vm){
  extern long g_vm_frame;
  GmlRoom rm; double rw=2048,rh=2048;
  if(gml_room_get(vm->win,vm->room_index,&rm)==0){ rw=rm.width; rh=rm.height; }
  vm->cg_ox=-512; vm->cg_oy=-512;
  double span=fmax(rw,rh)+1024;
  vm->cg_cell=64; while(span/vm->cg_cell>128) vm->cg_cell*=2;
  vm->cg_cw=(int)(span/vm->cg_cell)+1; vm->cg_ch=(int)(span/vm->cg_cell)+1;
  int ncell=vm->cg_cw*vm->cg_ch;
  static int off_cap=0;
  if(!vm->cg_off || off_cap<ncell+1){ free(vm->cg_off);
    vm->cg_off=malloc((ncell+1)*sizeof(int)); off_cap=ncell+1; if(!vm->cg_off) return 0; }
  memset(vm->cg_off,0,(ncell+1)*sizeof(int));
  int n=vm->inst_count;
  /* pass 1: count cell spans (skip dead slots; deactivated stay in — they may re-activate
   * mid-frame and the query filters on live flags anyway) */
  for(int i=0;i<n;i++){ GmlInstance *o=&vm->inst[i];
    if(!o->active && !o->deactivated) continue;
    double l,t,r,b; if(!inst_bbox(vm,o,o->x,o->y,&l,&t,&r,&b)) continue;
    int x0,y0,x1,y1; cg_cell_range(vm,l,t,r,b,&x0,&y0,&x1,&y1);
    for(int cy=y0;cy<=y1;cy++) for(int cx=x0;cx<=x1;cx++) vm->cg_off[cy*vm->cg_cw+cx+1]++;
  }
  for(int c=0;c<ncell;c++) vm->cg_off[c+1]+=vm->cg_off[c];
  int total=vm->cg_off[ncell];
  if(total>vm->cg_items_cap){ free(vm->cg_items);
    vm->cg_items=malloc((total>256?total:256)*sizeof(int));
    vm->cg_items_cap=total>256?total:256; if(!vm->cg_items){ free(vm->cg_off); vm->cg_off=NULL; off_cap=0; return 0; } }
  /* pass 2: fill (cursor = shifted offsets) */
  int *cur=malloc((ncell?ncell:1)*sizeof(int)); if(!cur) return 0;
  memcpy(cur,vm->cg_off,ncell*sizeof(int));
  for(int i=0;i<n;i++){ GmlInstance *o=&vm->inst[i];
    o->cg_touch=0; o->cg_visit=0;
    if(!o->active && !o->deactivated) continue;
    double l,t,r,b; if(!inst_bbox(vm,o,o->x,o->y,&l,&t,&r,&b)) continue;
    int x0,y0,x1,y1; cg_cell_range(vm,l,t,r,b,&x0,&y0,&x1,&y1);
    for(int cy=y0;cy<=y1;cy++) for(int cx=x0;cx<=x1;cx++) vm->cg_items[cur[cy*vm->cg_cw+cx]++]=i;
  }
  free(cur);
  vm->cg_overlay_n=0;
  vm->cg_gen++; if(vm->cg_gen<=0) vm->cg_gen=1;
  vm->cg_built_frame=g_vm_frame;
  g_cg_vm=vm;
  return 1;
}
/* family-alive shortcut: a query targeting an object class with NO living family member can
 * only return "nothing" — skip the scan. Only for plain object targets (not all/other/id).
 * Under GML_DBG_GRIDCHECK the skipped scan still runs and screams if it finds anything. */
static int obj_family_absent(GmlVM *vm, int obj){
  return obj>=0 && obj<vm->n_objects && vm->obj_alive && vm->obj_alive[obj]==0;
}
int gml_colgrid_mode(void){ static int m=-1;
  if(m<0) m=getenv("GML_NO_COLGRID")?0:(getenv("GML_DBG_GRIDCHECK")?2:1); return m; }
static int cg_ensure(GmlVM *vm){
  extern long g_vm_frame;
  if(vm->cg_built_frame==g_vm_frame && g_cg_vm==vm){
    /* Self-tuning: the overlay (instances touched since the build) is appended to EVERY query's
     * candidates regardless of location. A bomb blast turns ~150 debris into per-frame movers,
     * so each of their many pixel-step probes paid the whole overlay again. Once it outgrows a
     * small bound, re-snapshot the grid mid-frame (~tens of µs, correct by construction: it
     * captures live positions) and the overlay drains back to zero. */
    if(vm->cg_overlay_n > 96) return cg_build(vm);
    return 1;
  }
  return cg_build(vm);
}
static int cg_cmp_int(const void*a,const void*b){ return *(const int*)a-*(const int*)b; }
/* Collect the candidate slot indices (grid cells overlapping the probe + the touched overlay),
 * deduped and sorted ascending so callers preserve the linear scan's first-match-by-slot-order
 * semantics. Returns count, or -1 when the grid is unusable (caller falls back to linear). */
int gml_colgrid_collect(GmlVM *vm, double l, double t, double r, double b, int **out){
  /* Candidates are gathered into a per-slot BITSET and emitted by scanning set bits upward:
   * ascending slot order (the linear scan's event/return order) and dedupe come for free.
   * The first version qsort()ed the list per query — with an exploding bomb (~150 collision
   * sources x ~320 candidates each) the comparator-callback sort alone burned more than the
   * pruning saved. */
  static int *cand=NULL; static int cap=0;
  static uint64_t *bits=NULL; static int bits_words=0;
  if(gml_colgrid_mode()==0) return -1;
  if(!cg_ensure(vm)) return -1;
  int words=(vm->inst_count+63)/64;
  if(words>bits_words){ uint64_t *p=realloc(bits,(size_t)words*8); if(!p) return -1; bits=p; bits_words=words; }
  memset(bits,0,(size_t)words*8);
  int x0,y0,x1,y1; cg_cell_range(vm,l,t,r,b,&x0,&y0,&x1,&y1);
  for(int cy=y0;cy<=y1;cy++) for(int cx=x0;cx<=x1;cx++){
    int c=cy*vm->cg_cw+cx;
    for(int k=vm->cg_off[c];k<vm->cg_off[c+1];k++){
      int i=vm->cg_items[k];
      if(i<vm->inst_count) bits[i>>6] |= 1ull<<(i&63);
    }
  }
  for(int k2=0;k2<vm->cg_overlay_n;k2++){
    int i=vm->cg_overlay[k2];
    if(i<vm->inst_count) bits[i>>6] |= 1ull<<(i&63);
  }
  int n=0;
  for(int w=0;w<words;w++){
    uint64_t m=bits[w];
    while(m){
      int bit=__builtin_ctzll(m); m&=m-1;
      int i=(w<<6)|bit;
      if(n>=cap){ int nc=cap?cap*2:256; int *p=realloc(cand,nc*sizeof(int)); if(!p) return -1; cand=p; cap=nc; }
      cand[n++]=i;
    }
  }
  *out=cand;
  return n;
}
/* solid=1 -> test only solid instances (place_free); else test instances matching obj. */
static GmlInstance *collision_instance_at_linear(GmlVM *vm, double x, double y, int obj, int solid_only){
  GmlInstance *self=vm->cur_self; if(!self) return 0;
  double sl,st,sr,sb; if(!inst_bbox(vm,self,x,y,&sl,&st,&sr,&sb)) return 0;
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
    if(o==self || !o->active || o->marked) continue;
    if(solid_only){ if(o->solid<0.5) continue; }
    else if(!target_matches_instance(vm,self,o,obj)) continue;
    double ol,ot,orr,ob; if(!inst_bbox(vm,o,o->x,o->y,&ol,&ot,&orr,&ob)) continue;
    if(bbox_overlap(sl,st,sr,sb, ol,ot,orr,ob) && masks_overlap(vm,self,x,y,o)) return o;
  }
  return 0;
}
static int cg_candidate_hit(GmlVM *vm, GmlInstance *self, GmlInstance *o,
                            double x, double y, int obj, int solid_only,
                            double sl,double st,double sr,double sb){
  if(o==self || !o->active || o->marked) return 0;
  if(solid_only){ if(o->solid<0.5) return 0; }
  else if(!target_matches_instance(vm,self,o,obj)) return 0;
  double ol,ot,orr,ob; if(!inst_bbox(vm,o,o->x,o->y,&ol,&ot,&orr,&ob)) return 0;
  return bbox_overlap(sl,st,sr,sb, ol,ot,orr,ob) && masks_overlap(vm,self,x,y,o);
}
typedef struct { int idx; double d2; } GmlColListHit;
static int col_list_cmp(const void *aa, const void *bb){
  const GmlColListHit *a=(const GmlColListHit*)aa, *b=(const GmlColListHit*)bb;
  if(a->d2 < b->d2) return -1;
  if(a->d2 > b->d2) return 1;
  return a->idx - b->idx;
}
static int col_list_add(GmlVM *vm, GmlDSList *list, int ordered,
                        GmlColListHit **hits, int *nhit, int *cap,
                        int idx, double x, double y){
  if(!ordered){
    if(list) ds_list_push(list,vreal((double)vm->inst[idx].id));
    (*nhit)++;
    return 1;
  }
  if(*nhit>=*cap){
    int nc=*cap?*cap*2:16;
    GmlColListHit *p=realloc(*hits,(size_t)nc*sizeof(*p));
    if(!p) return 0;
    *hits=p; *cap=nc;
  }
  double dx=vm->inst[idx].x-x, dy=vm->inst[idx].y-y;
  (*hits)[*nhit]=(GmlColListHit){ idx, dx*dx+dy*dy };
  (*nhit)++;
  return 1;
}
static int collision_instance_list_at(GmlVM *vm, double x, double y, int obj, GmlDSList *list, int ordered){
  if(obj_family_absent(vm,obj)) return 0;
  GmlInstance *self=vm->cur_self; if(!self) return 0;
  double sl,st,sr,sb; if(!inst_bbox(vm,self,x,y,&sl,&st,&sr,&sb)) return 0;
  GmlColListHit *hits=NULL; int nhit=0, cap=0;
  int *cand=NULL, cn=-1;
  if(gml_colgrid_mode()!=0)
    cn=gml_colgrid_collect(vm,sl,st,sr,sb,&cand);
  if(cn>=0){
    for(int c=0;c<cn;c++){
      int i=cand[c]; if(i<0 || i>=vm->inst_count) continue;
      if(cg_candidate_hit(vm,self,&vm->inst[i],x,y,obj,0,sl,st,sr,sb) &&
         !col_list_add(vm,list,ordered,&hits,&nhit,&cap,i,x,y)) break;
    }
  } else {
    for(int i=0;i<vm->inst_count;i++){
      if(cg_candidate_hit(vm,self,&vm->inst[i],x,y,obj,0,sl,st,sr,sb) &&
         !col_list_add(vm,list,ordered,&hits,&nhit,&cap,i,x,y)) break;
    }
  }
  if(ordered && hits){
    qsort(hits,(size_t)nhit,sizeof(*hits),col_list_cmp);
    if(list) for(int i=0;i<nhit;i++) ds_list_push(list,vreal((double)vm->inst[hits[i].idx].id));
  }
  free(hits);
  return nhit;
}
static GmlInstance *collision_instance_at(GmlVM *vm, double x, double y, int obj, int solid_only){
  static int cg_mode=-1;   /* 0 = linear (GML_NO_COLGRID), 1 = grid, 2 = grid+verify */
  if(cg_mode<0) cg_mode = getenv("GML_NO_COLGRID")?0 : (getenv("GML_DBG_GRIDCHECK")?2:1);
  if(!solid_only && obj_family_absent(vm,obj)){
    if(cg_mode==2){ GmlInstance *lin=collision_instance_at_linear(vm,x,y,obj,solid_only);
      if(lin){ extern long g_vm_frame; fprintf(stderr,"[gridcheck] MISMATCH alive-count f%ld obj=%d found id=%u\n",g_vm_frame,obj,lin->id); return lin; } }
    return NULL;
  }
  if(cg_mode==0) return collision_instance_at_linear(vm,x,y,obj,solid_only);
  GmlInstance *self=vm->cur_self; if(!self) return 0;
  extern long g_vm_frame;
  if(vm->cg_built_frame!=g_vm_frame || g_cg_vm!=vm){
    if(!cg_build(vm)) return collision_instance_at_linear(vm,x,y,obj,solid_only);
  }
  double sl,st,sr,sb; if(!inst_bbox(vm,self,x,y,&sl,&st,&sr,&sb)) return 0;
  /* result keeps the LINEAR scan's semantics: lowest slot index wins (instance_place returns
   * "an instance" — keep it deterministic and identical to the old order). */
  int best=-1;
  int x0,y0,x1,y1; cg_cell_range(vm,sl,st,sr,sb,&x0,&y0,&x1,&y1);
  int vgen=++vm->cg_vgen; if(vm->cg_vgen<=0) vgen=vm->cg_vgen=1;
  for(int cy=y0;cy<=y1;cy++) for(int cx=x0;cx<=x1;cx++){
    int c=cy*vm->cg_cw+cx;
    for(int k=vm->cg_off[c];k<vm->cg_off[c+1];k++){
      int i=vm->cg_items[k]; GmlInstance *o=&vm->inst[i];
      if(o->cg_visit==vgen) continue; o->cg_visit=vgen;
      if(best>=0 && i>=best) continue;
      if(cg_candidate_hit(vm,self,o,x,y,obj,solid_only,sl,st,sr,sb)) best=i;
    }
  }
  for(int k2=0;k2<vm->cg_overlay_n;k2++){
    int i=vm->cg_overlay[k2]; GmlInstance *o=&vm->inst[i];
    if(o->cg_visit==vgen) continue; o->cg_visit=vgen;
    if(best>=0 && i>=best) continue;
    if(cg_candidate_hit(vm,self,o,x,y,obj,solid_only,sl,st,sr,sb)) best=i;
  }
  GmlInstance *res = best>=0 ? &vm->inst[best] : NULL;
  if(cg_mode==2){
    GmlInstance *lin=collision_instance_at_linear(vm,x,y,obj,solid_only);
    if(lin!=res){ extern long g_vm_frame;
      fprintf(stderr,"[gridcheck] MISMATCH f%ld probe=(%.2f,%.2f) obj=%d solid=%d grid=%d linear=%d\n",
        g_vm_frame,x,y,obj,solid_only,best,lin?(int)(lin-vm->inst):-1);
      res=lin; }   /* trust the reference path when verifying */
  }
  return res;
}
static int collision_at(GmlVM *vm, double x, double y, int obj, int solid_only){
  return collision_instance_at(vm,x,y,obj,solid_only)!=NULL;
}
static int instance_region_hit(GmlVM *vm, GmlInstance *o, double rx, double ry, double rw, double rh){
  double rl=rx, rt=ry, rr=rx+rw, rb=ry+rh;
  if(rr<rl){ double t=rl; rl=rr; rr=t; }
  if(rb<rt){ double t=rt; rt=rb; rb=t; }
  double l,t,r,b;
  if(inst_bbox(vm,o,o->x,o->y,&l,&t,&r,&b))
    return bbox_overlap(l,t,r,b,rl,rt,rr,rb);
  return o->x>=rl && o->x<=rr && o->y>=rt && o->y<=rb;
}
static void snap_contact_axis(GmlVM *vm, GmlInstance *s, double dx, double dy){
  double nx=s->x, ny=s->y;
  if(fabs(dx)<1e-9 && fabs(dy)>0.999999)
    ny = dy>0 ? floor(s->y+1e-9) : ceil(s->y-1e-9);
  else if(fabs(dy)<1e-9 && fabs(dx)>0.999999)
    nx = dx>0 ? floor(s->x+1e-9) : ceil(s->x-1e-9);
  else
    return;
  if(!collision_at(vm,nx,ny,0,1)){ s->x=nx; s->y=ny; gml_colgrid_touch(s); }
}
static int resolve_landing_overlap(GmlVM *vm, GmlInstance *s, int md){
  if(s->vspeed<=0) return 0;
  double ox=s->x, oy=s->y;
  int limit=md + (int)ceil(fabs(s->vspeed)) + 2;
  if(limit<1) limit=1;
  for(int k=0;k<=limit;k++){
    if(!collision_at(vm,s->x,s->y,0,1)){
      double free_y=s->y, hit_y=s->y+1.0;
      for(int i=0;i<10;i++){
        double mid=(free_y+hit_y)*0.5;
        if(collision_at(vm,s->x,mid,0,1)) hit_y=mid;
        else free_y=mid;
      }
      s->y=free_y; gml_colgrid_touch(s);
      return 1;
    }
    s->y-=1.0; gml_colgrid_touch(s);
  }
  s->x=ox; s->y=oy; gml_colgrid_touch(s);
  return 0;
}

/* first active instance of `obj` whose mask covers world point (px,py), or NULL. */
static int point_hits_instance_prec(GmlVM *vm, GmlInstance *o, double px, double py, int obj, GmlInstance *skip, int precise){
  GmlRender *R=(GmlRender*)vm->render;
  if(o==skip) return 0;
  if(!target_matches_instance(vm,vm->cur_self,o,obj)) return 0;
  double ol,ot,orr,ob; if(!inst_bbox(vm,o,o->x,o->y,&ol,&ot,&orr,&ob)) return 0;
  if(px<ol||px>orr||py<ot||py>ob) return 0;
  if(!precise) return 1;
  /* bbox hit; refine with the per-pixel mask when available */
  if(R){ int si=inst_mask_sprite_index(o);
    if(si>=0 && si<R->n_spr){ GmlSprite *s=&R->spr[si];
      if(!mask_hit_world(R,o,s,si,o->x,o->y,(int)floor(px),(int)floor(py))) return 0; } }
  return 1;
}
static int point_hits_instance(GmlVM *vm, GmlInstance *o, double px, double py, int obj, GmlInstance *skip){
  return point_hits_instance_prec(vm,o,px,py,obj,skip,1);
}
static GmlInstance *instance_at_point_ex_linear(GmlVM *vm, double px, double py, int obj, GmlInstance *skip){
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
    if(point_hits_instance(vm,o,px,py,obj,skip)) return o; }
  return NULL;
}
static GmlInstance *instance_at_point_ex(GmlVM *vm, double px, double py, int obj, GmlInstance *skip){
  int mode=gml_colgrid_mode(); int *cand=NULL; int n;
  if(obj_family_absent(vm,obj)){
    if(mode==2){ GmlInstance *lin=instance_at_point_ex_linear(vm,px,py,obj,skip);
      if(lin){ extern long g_vm_frame; fprintf(stderr,"[gridcheck] MISMATCH alive-count(point) f%ld obj=%d id=%u\n",g_vm_frame,obj,lin->id); return lin; } }
    return NULL;
  }
  if(mode==0 || (n=gml_colgrid_collect(vm,px,py,px,py,&cand))<0)
    return instance_at_point_ex_linear(vm,px,py,obj,skip);
  GmlInstance *res=NULL;
  for(int k=0;k<n;k++){ GmlInstance *o=&vm->inst[cand[k]];
    if(point_hits_instance(vm,o,px,py,obj,skip)){ res=o; break; } }
  if(mode==2){ GmlInstance *lin=instance_at_point_ex_linear(vm,px,py,obj,skip);
    if(lin!=res){ extern long g_vm_frame;
      fprintf(stderr,"[gridcheck] MISMATCH point f%ld (%.2f,%.2f) obj=%d grid=%d linear=%d\n",
        g_vm_frame,px,py,obj,res?(int)(res-vm->inst):-1,lin?(int)(lin-vm->inst):-1);
      res=lin; } }
  return res;
}
static GmlInstance *instance_at_point(GmlVM *vm, double px, double py, int obj){
  return instance_at_point_ex(vm,px,py,obj,NULL);
}
static int line_hits_instance(GmlVM *vm, GmlInstance *o,
                              double x1, double y1, double x2, double y2,
                              int obj, GmlInstance *skip, int precise, int steps,
                              double *first_d2){
  if(!o || !o->active || o->marked) return 0;
  for(int k=0;k<=steps;k++){
    double t=(double)k/(double)steps;
    double px=x1+(x2-x1)*t, py=y1+(y2-y1)*t;
    if(point_hits_instance_prec(vm,o,px,py,obj,skip,precise)){
      if(first_d2){
        double dx=px-x1, dy=py-y1;
        *first_d2=dx*dx+dy*dy;
      }
      return 1;
    }
  }
  return 0;
}
static GmlInstance *collision_line_query(GmlVM *vm, double x1, double y1, double x2, double y2,
                                         int obj, int precise, int notme){
  int steps=(int)ceil(fmax(fabs(x2-x1),fabs(y2-y1))); if(steps<1) steps=1;
  GmlInstance *skip=notme?vm->cur_self:NULL;
  if(obj_family_absent(vm,obj) && gml_colgrid_mode()!=2) return NULL;
  int *cand=NULL; int cn=-1;
  if(gml_colgrid_mode()!=0)
    cn=gml_colgrid_collect(vm, fmin(x1,x2), fmin(y1,y2), fmax(x1,x2), fmax(y1,y2), &cand);
  GmlInstance *hit=NULL;
  for(int k=0;k<=steps && !hit;k++){
    double t=(double)k/(double)steps, px=x1+(x2-x1)*t, py=y1+(y2-y1)*t;
    if(cn>=0){
      for(int c=0;c<cn;c++){ GmlInstance *o=&vm->inst[cand[c]];
        if(point_hits_instance_prec(vm,o,px,py,obj,skip,precise)){ hit=o; break; } }
    } else {
      for(int i=0;i<vm->inst_count;i++){
        GmlInstance *o=&vm->inst[i];
        if(point_hits_instance_prec(vm,o,px,py,obj,skip,precise)){ hit=o; break; }
      }
    }
  }
  if(gml_colgrid_mode()==2){
    GmlInstance *lin=NULL;
    for(int k=0;k<=steps && !lin;k++){
      double t=(double)k/(double)steps, px=x1+(x2-x1)*t, py=y1+(y2-y1)*t;
      for(int i=0;i<vm->inst_count;i++){
        GmlInstance *o=&vm->inst[i];
        if(point_hits_instance_prec(vm,o,px,py,obj,skip,precise)){ lin=o; break; }
      }
    }
    if(lin!=hit){ extern long g_vm_frame;
      fprintf(stderr,"[gridcheck] MISMATCH line f%ld (%.1f,%.1f)-(%.1f,%.1f) obj=%d grid=%d linear=%d\n",
        g_vm_frame,x1,y1,x2,y2,obj,hit?(int)(hit-vm->inst):-1,lin?(int)(lin-vm->inst):-1);
      hit=lin;
    }
  }
  return hit;
}
static int collision_line_list_query(GmlVM *vm, double x1, double y1, double x2, double y2,
                                     int obj, int precise, int notme, GmlDSList *list, int ordered){
  int steps=(int)ceil(fmax(fabs(x2-x1),fabs(y2-y1))); if(steps<1) steps=1;
  GmlInstance *skip=notme?vm->cur_self:NULL;
  if(obj_family_absent(vm,obj) && gml_colgrid_mode()!=2) return 0;
  int *cand=NULL; int cn=-1;
  if(gml_colgrid_mode()!=0)
    cn=gml_colgrid_collect(vm, fmin(x1,x2), fmin(y1,y2), fmax(x1,x2), fmax(y1,y2), &cand);
  GmlColListHit *hits=NULL; int nhit=0, cap=0;
  if(cn>=0){
    for(int c=0;c<cn;c++){
      int i=cand[c]; if(i<0 || i>=vm->inst_count) continue;
      double d2=0;
      if(line_hits_instance(vm,&vm->inst[i],x1,y1,x2,y2,obj,skip,precise,steps,&d2)){
        if(!col_list_add(vm,ordered?NULL:list,ordered,&hits,&nhit,&cap,i,x1,y1)) break;
        if(ordered && nhit>0) hits[nhit-1].d2=d2;
      }
    }
  } else {
    for(int i=0;i<vm->inst_count;i++){
      double d2=0;
      if(line_hits_instance(vm,&vm->inst[i],x1,y1,x2,y2,obj,skip,precise,steps,&d2)){
        if(!col_list_add(vm,ordered?NULL:list,ordered,&hits,&nhit,&cap,i,x1,y1)) break;
        if(ordered && nhit>0) hits[nhit-1].d2=d2;
      }
    }
  }
  if(ordered && hits){
    qsort(hits,(size_t)nhit,sizeof(*hits),col_list_cmp);
    if(list) for(int i=0;i<nhit;i++) ds_list_push(list,vreal((double)vm->inst[hits[i].idx].id));
  }
  free(hits);
  return nhit;
}
static int shape_hits_instance(GmlVM *vm, GmlInstance *o, int kind, double *p, int obj, GmlInstance *skip,
                               double sl,double st,double sr,double sb){
  GmlRender *R=(GmlRender*)vm->render;
  if(o==skip || !target_matches_instance(vm,vm->cur_self,o,obj)) return 0;
  double ol,ot,orr,ob; if(!inst_bbox(vm,o,o->x,o->y,&ol,&ot,&orr,&ob)) return 0;
  if(!bbox_overlap(sl,st,sr,sb,ol,ot,orr,ob)) return 0;
  int x0=(int)floor(fmax(sl,ol)), x1=(int)ceil(fmin(sr,orr)+1.0);
  int y0=(int)floor(fmax(st,ot)), y1=(int)ceil(fmin(sb,ob)+1.0);
  if(x1==x0) x1++;
  if(y1==y0) y1++;
  for(int wy=y0; wy<y1; wy++) for(int wx=x0; wx<x1; wx++){
    if(kind==2){ double dx=wx-p[0], dy=wy-p[1]; if(dx*dx+dy*dy>p[2]*p[2]) continue; }
    if(R){ int si=inst_mask_sprite_index(o);
      if(si>=0 && si<R->n_spr){ GmlSprite *s=&R->spr[si];
        if(!mask_hit_world(R,o,s,si,o->x,o->y,wx,wy)) continue; } }
    return 1;
  }
  return 0;
}
static void shape_bounds(int kind, double *p, double *sl,double *st,double *sr,double *sb){
  if(kind==0){ *sl=p[0]; *st=p[1]; *sr=p[0]; *sb=p[1]; }
  else if(kind==1){ *sl=fmin(p[0],p[2]); *st=fmin(p[1],p[3]); *sr=fmax(p[0],p[2]); *sb=fmax(p[1],p[3]); }
  else { *sl=p[0]-p[2]; *st=p[1]-p[2]; *sr=p[0]+p[2]; *sb=p[1]+p[2]; }
}
static GmlInstance *collision_shape_linear(GmlVM *vm, int kind, double *p, int obj, int notme){
  GmlInstance *skip=notme?vm->cur_self:NULL;
  double sl,st,sr,sb; shape_bounds(kind,p,&sl,&st,&sr,&sb);
  for(int i=0;i<vm->inst_count;i++)
    if(shape_hits_instance(vm,&vm->inst[i],kind,p,obj,skip,sl,st,sr,sb)) return &vm->inst[i];
  return NULL;
}
static GmlInstance *collision_shape(GmlVM *vm, int kind, double *p, int obj, int notme){
  GmlInstance *skip=notme?vm->cur_self:NULL;
  double sl,st,sr,sb; shape_bounds(kind,p,&sl,&st,&sr,&sb);
  int mode=gml_colgrid_mode(); int *cand=NULL; int n;
  if(obj_family_absent(vm,obj)){
    if(mode==2){ GmlInstance *lin=collision_shape_linear(vm,kind,p,obj,notme);
      if(lin){ extern long g_vm_frame; fprintf(stderr,"[gridcheck] MISMATCH alive-count(shape) f%ld obj=%d id=%u\n",g_vm_frame,obj,lin->id); return lin; } }
    return NULL;
  }
  if(mode==0 || (n=gml_colgrid_collect(vm,sl,st,sr,sb,&cand))<0)
    return collision_shape_linear(vm,kind,p,obj,notme);
  GmlInstance *res=NULL;
  for(int k=0;k<n;k++){ GmlInstance *o=&vm->inst[cand[k]];
    if(shape_hits_instance(vm,o,kind,p,obj,skip,sl,st,sr,sb)){ res=o; break; } }
  if(mode==2){ GmlInstance *lin=collision_shape_linear(vm,kind,p,obj,notme);
    if(lin!=res){ extern long g_vm_frame;
      fprintf(stderr,"[gridcheck] MISMATCH shape f%ld kind=%d obj=%d grid=%d linear=%d\n",
        g_vm_frame,kind,obj,res?(int)(res-vm->inst):-1,lin?(int)(lin-vm->inst):-1);
      res=lin; } }
  return res;
}

/* Fire GM's "gamepad discovered" async event (ev_async_system / Other_75) so games that only enable
 * gamepad input once a pad is detected recognize our single virtual pad. GM delivers it via the
 * async_load ds_map; we build {event_type:"gamepad discovered", pad_index:0}, point the async_load
 * global at it, dispatch to every live instance, then tear it down. Called on room enter (the handler
 * object may only exist in some rooms). Skipped entirely if no code defines a _Other_75 handler. */
void gml_fire_gamepad_connected(GmlVM *vm){
  if(!vm || !vm->win) return;
  if(!gml_input_gamepad_connected(0)) return;
  int has=0; for(int i=0;i<vm->win->n_code;i++){ const char *cn=vm->win->code[i].name;
    if(cn && strstr(cn,"_Other_75")){ has=1; break; } }
  if(!has) return;
  int id=ds_map_create_id(vm); if(id<0) return;
  ds_map_put(vm,id,vstr("event_type"),vstr("gamepad discovered"),1);
  ds_map_put(vm,id,vstr("pad_index"),vreal(0),1);
  *gml_varmap_put(&vm->globals,"async_load")=vreal(id);
  int n0=vm->inst_count;
  for(int i=0;i<n0;i++) if(vm->inst[i].active && !vm->inst[i].marked)
    gml_run_event(vm,&vm->inst[i],"Other_75");
  *gml_varmap_put(&vm->globals,"async_load")=vreal(-1);
  GmlDSMap *m=ds_map_slot(vm,id);
  if(m){ for(int j=0;j<m->len;j++) ds_entry_free(&m->entry[j]); free(m->entry); memset(m,0,sizeof(*m)); }
  if(getenv("GML_LOG_INST")) fprintf(stderr,"[gamepad] fired Other_75 to %d instances\n", n0);
}

/* Fire GM's Async Save/Load event (Other_72) for every buffer_*_async request queued this step.
 * async_load = {id, status, error:0}, dispatched to all live instances, then torn down. */
void gml_fire_async_saveload(GmlVM *vm){
  if(!vm || vm->n_async_sl<=0) return;
  int n=vm->n_async_sl; vm->n_async_sl=0;
  if(getenv("GML_LOG_ASYNC")){ extern long g_vm_frame;
    fprintf(stderr,"[async] f%ld drain n=%d\n",g_vm_frame,n); }
  for(int k=0;k<n;k++){
    int id=ds_map_create_id(vm); if(id<0) return;
    ds_map_put(vm,id,vstr("id"),vreal(vm->async_sl_q[k]),1);
    ds_map_put(vm,id,vstr("status"),vreal(vm->async_sl_status[k]),1);
    ds_map_put(vm,id,vstr("error"),vreal(0),1);
    *gml_varmap_put(&vm->globals,"async_load")=vreal(id);
    int n0=vm->inst_count;
    for(int i=0;i<n0;i++) if(vm->inst[i].active && !vm->inst[i].marked)
      gml_run_event(vm,&vm->inst[i],"Other_72");
    *gml_varmap_put(&vm->globals,"async_load")=vreal(-1);
    GmlDSMap *m=ds_map_slot(vm,id);
    if(m){ for(int j=0;j<m->len;j++) ds_entry_free(&m->entry[j]); free(m->entry); memset(m,0,sizeof(*m)); }
  }
}

/* Fire GM's Async HTTP event (Other_62) for every http_* request made this step. No network in
 * the core: every response is a failure ({id, status:-1, http_status:0, result:""}), so games
 * with online scoreboards fall into their error path instead of stalling on "downloading". */
void gml_fire_async_http(GmlVM *vm){
  if(!vm || vm->n_async_http<=0) return;
  int n=vm->n_async_http; vm->n_async_http=0;
  if(getenv("GML_LOG_ASYNC")){ extern long g_vm_frame;
    fprintf(stderr,"[async] f%ld http drain n=%d\n",g_vm_frame,n); }
  for(int k=0;k<n;k++){
    int id=ds_map_create_id(vm); if(id<0) return;
    ds_map_put(vm,id,vstr("id"),vreal(vm->async_http_q[k]),1);
    ds_map_put(vm,id,vstr("status"),vreal(-1),1);
    ds_map_put(vm,id,vstr("http_status"),vreal(0),1);
    ds_map_put(vm,id,vstr("result"),vstr(""),1);
    *gml_varmap_put(&vm->globals,"async_load")=vreal(id);
    int n0=vm->inst_count;
    for(int i=0;i<n0;i++) if(vm->inst[i].active && !vm->inst[i].marked)
      gml_run_event(vm,&vm->inst[i],"Other_62");
    *gml_varmap_put(&vm->globals,"async_load")=vreal(-1);
    GmlDSMap *m=ds_map_slot(vm,id);
    if(m){ for(int j=0;j<m->len;j++) ds_entry_free(&m->entry[j]); free(m->entry); memset(m,0,sizeof(*m)); }
  }
}
static GmlVal builtin_http_request_stub(GmlVM *vm){
  int req=++vm->async_seq;
  if(vm->n_async_http<16) vm->async_http_q[vm->n_async_http++]=req;
  if(getenv("GML_LOG_ASYNC")){ extern long g_vm_frame;
    fprintf(stderr,"[async] f%ld http queue id=%d (offline: will fail)\n",g_vm_frame,req); }
  return vreal(req);
}
static void async_saveload_queue(GmlVM *vm, int req, int ok){
  if(!vm || req<=0) return;
  if(vm->n_async_sl<16){
    vm->async_sl_q[vm->n_async_sl]=req;
    vm->async_sl_status[vm->n_async_sl]=ok ? 1 : 0;
    vm->n_async_sl++;
    if(getenv("GML_LOG_ASYNC")){ extern long g_vm_frame;
      fprintf(stderr,"[async] f%ld queue id=%d status=%d n=%d\n",g_vm_frame,req,ok?1:0,vm->n_async_sl); }
  }
}

static int async_saveload_request(GmlVM *vm, int ok){
  if(!vm) return 0;
  if(vm->async_group_active){
    if(vm->async_group_id<=0) vm->async_group_id=++vm->async_seq;
    if(!ok) vm->async_group_status=0;
    vm->async_group_count++;
    if(getenv("GML_LOG_ASYNC")){ extern long g_vm_frame;
      fprintf(stderr,"[async] f%ld grouped id=%d status=%d count=%d\n",
        g_vm_frame,vm->async_group_id,ok?1:0,vm->async_group_count); }
    return vm->async_group_id;
  }
  int req=++vm->async_seq;
  async_saveload_queue(vm, req, ok);
  return req;
}

typedef struct { const char *name; long calls; double ms; } HotBuiltinProf;
#define HOTPROF_MAX 128
static HotBuiltinProf g_hotprof[HOTPROF_MAX];
static int g_hotprof_n;
static long g_hotprof_last_frame=-1;

static int hotprof_enabled(void){
  static int on=-1;
  if(on<0) on=getenv("GML_PROFILE_HOTBUILTIN")!=NULL;
  return on;
}
static double hotprof_now(void){
#ifdef _WIN32
  return 0.0;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC,&ts);
  return ts.tv_sec + ts.tv_nsec/1000000000.0;
#endif
}
static void hotprof_add(const char *name, double ms){
  if(!hotprof_enabled() || !name) return;
  int slot=-1;
  for(int i=0;i<g_hotprof_n;i++) if(g_hotprof[i].name==name || !strcmp(g_hotprof[i].name,name)){ slot=i; break; }
  if(slot<0){
    if(g_hotprof_n<HOTPROF_MAX) slot=g_hotprof_n++;
    else slot=HOTPROF_MAX-1;
    g_hotprof[slot]=(HotBuiltinProf){name,0,0};
  }
  g_hotprof[slot].calls++;
  g_hotprof[slot].ms+=ms;
  extern long g_vm_frame;
  if(g_vm_frame<=0 || g_vm_frame==g_hotprof_last_frame || g_vm_frame%300) return;
  g_hotprof_last_frame=g_vm_frame;
  fprintf(stderr,"[hotprof] f=%ld top:\n",g_vm_frame);
  int used[16]; for(int i=0;i<16;i++) used[i]=-1;
  for(int rank=0;rank<16;rank++){
    int best=-1;
    for(int i=0;i<g_hotprof_n;i++){
      int seen=0; for(int j=0;j<rank;j++) if(used[j]==i){ seen=1; break; }
      if(!seen && (best<0 || g_hotprof[i].ms>g_hotprof[best].ms)) best=i;
    }
    if(best<0 || g_hotprof[best].ms<=0) break;
    used[rank]=best;
    fprintf(stderr,"[hotprof]   %7.2fms %6ld %s\n",g_hotprof[best].ms,g_hotprof[best].calls,g_hotprof[best].name);
  }
  memset(g_hotprof,0,sizeof g_hotprof);
  g_hotprof_n=0;
}

/* Short-circuit very hot draw/UI builtins before the broad legacy strcmp chain below. Keep these
 * branches behavior-equivalent to their canonical handlers; this only avoids dispatch overhead. */
static int fast_hot_builtin(GmlVM *vm, const char *nm, GmlVal *a, int n, GmlVal *out){
  GmlRender *R=(GmlRender*)vm->render;
  if(!nm || !out) return 0;
  if(nm[0]=='a'){
    if(!strncmp(nm,"array_",6)){
      if(!strcmp(nm,"array_length")||!strcmp(nm,"array_length_1d")){ *out=vreal(n>0?gml_val_array_length(a[0]):0); return 1; }
      if(!strcmp(nm,"array_get")){ *out=n>1?gml_arr_get(a[0],(int)N(a,n,1)):vreal(0); return 1; }
      if(!strcmp(nm,"array_set")){ if(n>2) gml_arr_set(a[0],(int)N(a,n,1),a[2]); *out=vreal(0); return 1; }
      if(!strcmp(nm,"array_create")){ int sz=n>0?(int)N(a,n,0):0; GmlVal fill=n>1?a[1]:vreal(0); *out=gml_arr_new(sz,fill); return 1; }
      if(!strcmp(nm,"array_push")){ for(int i=1;i<n;i++) gml_arr_push(a[0],a[i]); *out=vreal(0); return 1; }
      if(!strcmp(nm,"array_pop")){ *out=n>0?gml_arr_pop(a[0]):vreal(0); return 1; }
      if(!strcmp(nm,"array_resize")){ if(n>1) gml_arr_resize(a[0],(int)N(a,n,1)); *out=vreal(0); return 1; }
      if(!strcmp(nm,"array_copy")){ if(n>4) gml_arr_copy(a[0],(int)N(a,n,1),a[2],(int)N(a,n,3),(int)N(a,n,4)); *out=vreal(0); return 1; }
      if(!strcmp(nm,"array_sort")){ if(n>0) gml_array_sort(a[0], n<2 || N(a,n,1)!=0); *out=vreal(0); return 1; }
      if(!strcmp(nm,"array_height_2d")){ *out=vreal(n>0?gml_val_array_height_2d(a[0]):0); return 1; }
      if(!strcmp(nm,"array_length_2d")){ *out=vreal(n>0?gml_val_array_length_2d(a[0],(int)N(a,n,1)):0); return 1; }
      return 0;
    }
    if(!strcmp(nm,"abs")){ *out=vreal(fabs(N(a,n,0))); return 1; }
    if(!strcmp(nm,"arctan")){ *out=vreal(atan(N(a,n,0))); return 1; }
    if(!strcmp(nm,"arcsin")){ *out=vreal(asin(N(a,n,0))); return 1; }
    if(!strcmp(nm,"arccos")){ *out=vreal(acos(N(a,n,0))); return 1; }
    if(!strcmp(nm,"arctan2")){ *out=vreal(atan2(N(a,n,0),N(a,n,1))); return 1; }
  }
  if(nm[0]=='b' && !strcmp(nm,"bool")){
    if(n>=1 && a[0].t==V_STR && a[0].s) *out=vreal(!strcmp(a[0].s,"true")||!strcmp(a[0].s,"1"));
    else *out=vreal(N(a,n,0)!=0);
    return 1;
  }
  if(nm[0]=='c'){
    if(!strcmp(nm,"ceil")){ *out=vreal(ceil(N(a,n,0))); return 1; }
    if(!strcmp(nm,"cos")){ *out=vreal(cos(N(a,n,0))); return 1; }
    if(!strcmp(nm,"clamp")){ double x=N(a,n,0), lo=N(a,n,1), hi=N(a,n,2); if(x<lo)x=lo; if(x>hi)x=hi; *out=vreal(x); return 1; }
    if(!strcmp(nm,"collision_point")){ double p[4]={N(a,n,0),N(a,n,1),0,0}; GmlInstance *o=collision_shape(vm,0,p,(int)N(a,n,2),(int)N(a,n,4)); *out=vreal(o?(double)o->id:-4); return 1; }
    if(!strcmp(nm,"collision_rectangle")){ double p[4]={N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3)}; GmlInstance *o=collision_shape(vm,1,p,(int)N(a,n,4),(int)N(a,n,6)); *out=vreal(o?(double)o->id:-4); return 1; }
    if(!strcmp(nm,"collision_circle")){ double p[3]={N(a,n,0),N(a,n,1),N(a,n,2)}; GmlInstance *o=collision_shape(vm,2,p,(int)N(a,n,3),(int)N(a,n,5)); *out=vreal(o?(double)o->id:-4); return 1; }
  }
  if(nm[0]=='d' && strncmp(nm,"draw_",5)){
    if(!strcmp(nm,"dsin")){ *out=vreal(sin(N(a,n,0)*M_PI/180.0)); return 1; }
    if(!strcmp(nm,"dcos")){ *out=vreal(cos(N(a,n,0)*M_PI/180.0)); return 1; }
    if(!strcmp(nm,"dtan")){ *out=vreal(tan(N(a,n,0)*M_PI/180.0)); return 1; }
    if(!strcmp(nm,"darcsin")){ *out=vreal(asin(N(a,n,0))*180.0/M_PI); return 1; }
    if(!strcmp(nm,"darccos")){ *out=vreal(acos(N(a,n,0))*180.0/M_PI); return 1; }
    if(!strcmp(nm,"darctan")){ *out=vreal(atan(N(a,n,0))*180.0/M_PI); return 1; }
    if(!strcmp(nm,"darctan2")){ *out=vreal(atan2(N(a,n,0),N(a,n,1))*180.0/M_PI); return 1; }
    if(!strcmp(nm,"degtorad")){ *out=vreal(N(a,n,0)*M_PI/180.0); return 1; }
    if(!strcmp(nm,"distance_to_point")){ GmlInstance*s=vm->cur_self; *out=s?vreal(hypot(N(a,n,0)-s->x,N(a,n,1)-s->y)):vreal(0); return 1; }
    if(!strcmp(nm,"distance_to_object")){ GmlInstance*s=vm->cur_self; if(!s){ *out=vreal(0); return 1; }
      int obj=(int)N(a,n,0); double best=1e18;
      for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
        if(o==s || !target_matches_instance(vm,s,o,obj)) continue;
        double d=hypot(o->x-s->x,o->y-s->y); if(d<best) best=d; }
      *out=vreal(best>1e17?-1:best); return 1; }
    if(!strcmp(nm,"display_get_width")){ *out=vreal(presentation_size(vm,R,0)); return 1; }
    if(!strcmp(nm,"display_get_height")){ *out=vreal(presentation_size(vm,R,1)); return 1; }
    if(!strcmp(nm,"display_get_gui_width")){ *out=vreal(vm->gui_w>0? vm->gui_w : ((R&&R->fbw>0)? R->fbw : (vm->win&&vm->win->disp_w? (int)vm->win->disp_w : 288))); return 1; }
    if(!strcmp(nm,"display_get_gui_height")){ *out=vreal(vm->gui_h>0? vm->gui_h : ((R&&R->fbh>0)? R->fbh : (vm->win&&vm->win->disp_h? (int)vm->win->disp_h : 216))); return 1; }
  }
  if(nm[0]=='f'){
    if(!strcmp(nm,"floor")){ *out=vreal(floor(N(a,n,0))); return 1; }
    if(!strcmp(nm,"frac")){ double v=N(a,n,0); *out=vreal(v-floor(v)); return 1; }
  }
  if(nm[0]=='i'){
    if(!strcmp(nm,"instance_number")){ *out=vreal(gml_instance_number(vm,(int)N(a,n,0))); return 1; }
    if(!strcmp(nm,"instance_exists")){ *out=vreal(gml_instance_number(vm,(int)N(a,n,0))>0); return 1; }
    if(!strcmp(nm,"instance_find")){
      int obj=(int)N(a,n,0), nth=(int)N(a,n,1), seen=0;
      if(nth<0){ *out=vreal(-4); return 1; }
      for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
        if(!target_matches_instance(vm,vm->cur_self,o,obj)) continue;
        if(seen++==nth){ *out=vreal(o->id); return 1; }
      }
      *out=vreal(-4); return 1;
    }
    if(!strcmp(nm,"instance_position")){ GmlInstance *o=instance_at_point(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2)); *out=vreal(o?(double)o->id:-4); return 1; }
    if(!strcmp(nm,"instance_place")){ GmlInstance *o=collision_instance_at(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2),0); *out=vreal(o?(double)o->id:-4); return 1; }
    if(!strcmp(nm,"instance_place_list")){
      GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,3));
      int r=collision_instance_list_at(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2),l,N(a,n,4)>=0.5);
      if(log_col_on()){ GmlInstance*cs=vm->cur_self; const char*cnm=(cs&&cs->obj>=0&&cs->obj<vm->n_objects)?vm->objects[cs->obj].name:"?";
        const char*tn=((int)N(a,n,2)>=0&&(int)N(a,n,2)<vm->n_objects)?vm->objects[(int)N(a,n,2)].name:"?";
        if(log_col_match(cnm)) fprintf(stderr,"[col] %s instance_place_list(%.0f,%.0f,%s)=%d\n",cnm,N(a,n,0),N(a,n,1),tn,r); }
      *out=vreal(r); return 1;
    }
  }
  if(nm[0]=='l'){
    if(!strcmp(nm,"lerp")){ double a0=N(a,n,0), a1=N(a,n,1), t=N(a,n,2); *out=vreal(a0+(a1-a0)*t); return 1; }
    if(!strcmp(nm,"lengthdir_x")){ *out=vreal(N(a,n,0)*cos(N(a,n,1)*M_PI/180.0)); return 1; }
    if(!strcmp(nm,"lengthdir_y")){ *out=vreal(-N(a,n,0)*sin(N(a,n,1)*M_PI/180.0)); return 1; }
  }
  if(nm[0]=='m'){
    if(!strcmp(nm,"min")){ if(n<=0){ *out=vreal(0); return 1; } double v=N(a,n,0); for(int i=1;i<n;i++){ double x=N(a,n,i); if(x<v) v=x; } *out=vreal(v); return 1; }
    if(!strcmp(nm,"max")){ if(n<=0){ *out=vreal(0); return 1; } double v=N(a,n,0); for(int i=1;i<n;i++){ double x=N(a,n,i); if(x>v) v=x; } *out=vreal(v); return 1; }
    if(!strcmp(nm,"mean")){ double s=0; for(int i=0;i<n;i++) s+=N(a,n,i); *out=vreal(n? s/n : 0); return 1; }
  }
  if(nm[0]=='p'){
    if(!strcmp(nm,"place_meeting")){ int r=collision_at(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2),0);
      if(log_col_on()){ GmlInstance*cs=vm->cur_self; const char*cn=(cs&&cs->obj>=0&&cs->obj<vm->n_objects)?vm->objects[cs->obj].name:"?";
        const char*tn=((int)N(a,n,2)>=0&&(int)N(a,n,2)<vm->n_objects)?vm->objects[(int)N(a,n,2)].name:"?";
        if(log_col_match(cn)) fprintf(stderr,"[col] %s place_meeting(%.0f,%.0f,%s)=%d\n",cn,N(a,n,0),N(a,n,1),tn,r); }
      *out=vreal(r); return 1; }
    if(!strcmp(nm,"place_free")){ int r=!collision_at(vm,N(a,n,0),N(a,n,1),0,1);
      if(log_col_on()){ GmlInstance*cs=vm->cur_self; const char*cn=(cs&&cs->obj>=0&&cs->obj<vm->n_objects)?vm->objects[cs->obj].name:"?";
        if(log_col_match(cn)) fprintf(stderr,"[col] %s place_free(%.0f,%.0f)=%d\n",cn,N(a,n,0),N(a,n,1),r); }
      *out=vreal(r); return 1; }
    if(!strcmp(nm,"place_empty")){ *out=vreal(!collision_at(vm,N(a,n,0),N(a,n,1),IT_ALL,0)); return 1; }
    if(!strcmp(nm,"position_meeting")){ double p[4]={N(a,n,0),N(a,n,1),0,0}; *out=vreal(collision_shape(vm,0,p,(int)N(a,n,2),0)!=NULL); return 1; }
    if(!strcmp(nm,"point_distance")){ *out=vreal(hypot(N(a,n,2)-N(a,n,0),N(a,n,3)-N(a,n,1))); return 1; }
    if(!strcmp(nm,"point_direction")){ double dx=N(a,n,2)-N(a,n,0), dy=N(a,n,3)-N(a,n,1);
      double r=atan2(-dy,dx)*180.0/M_PI; if(r<0)r+=360; *out=vreal(r); return 1; }
    if(!strcmp(nm,"power")){ *out=vreal(pow(N(a,n,0),N(a,n,1))); return 1; }
  }
  if(nm[0]=='r'){
    if(!strcmp(nm,"real")){ *out=vreal(N(a,n,0)); return 1; }
    if(!strcmp(nm,"radtodeg")){ *out=vreal(N(a,n,0)*180.0/M_PI); return 1; }
  }
  if(nm[0]=='r' && !strcmp(nm,"round")){ *out=vreal(gm_round(N(a,n,0))); return 1; }
  if(nm[0]=='m' && (!strcmp(nm,"make_color_hsv")||!strcmp(nm,"make_colour_hsv"))){
    double h=fmod(N(a,n,0),255.0); if(h<0) h+=255.0; double s=N(a,n,1)/255.0, v=N(a,n,2)/255.0;
    double c=v*s, hp=h/42.5, x=c*(1-fabs(fmod(hp,2)-1)), m=v-c, r=0,g=0,b=0;
    if(hp<1){ r=c; g=x; } else if(hp<2){ r=x; g=c; } else if(hp<3){ g=c; b=x; }
    else if(hp<4){ g=x; b=c; } else if(hp<5){ r=x; b=c; } else { r=c; b=x; }
    *out=vreal((int)((r+m)*255) + ((int)((g+m)*255)<<8) + ((int)((b+m)*255)<<16)); return 1;
  }
  if(nm[0]=='s'){
    if(!strcmp(nm,"surface_exists")){ *out=vreal(R?gml_surface_exists(R,(int)N(a,n,0)):0); return 1; }
    if(!strcmp(nm,"surface_create")){ *out=vreal(R?gml_surface_create(R,(int)N(a,n,0),(int)N(a,n,1)):-1); return 1; }
    if(!strcmp(nm,"surface_free")){ if(R) gml_surface_free(R,(int)N(a,n,0)); *out=vreal(0); return 1; }
    if(!strcmp(nm,"surface_get_texture")){ int sid=(int)N(a,n,0); *out=vreal((R&&gml_surface_exists(R,sid))?(double)(GML_TEX_SURF_TAG | (sid&0xFFFF)):-1); return 1; }
    if(!strcmp(nm,"surface_get_width")){ *out=vreal(R?gml_surface_width(R,(int)N(a,n,0)):0); return 1; }
    if(!strcmp(nm,"surface_get_height")){ *out=vreal(R?gml_surface_height(R,(int)N(a,n,0)):0); return 1; }
    if(!strcmp(nm,"surface_set_target")){ if(getenv("GML_LOG_SURF"))fprintf(stderr,"[surf] set_target %d\n",(int)N(a,n,0)); *out=vreal(R?gml_surface_set_target(R,(int)N(a,n,0)):0); return 1; }
    if(!strcmp(nm,"surface_reset_target")){ if(getenv("GML_LOG_SURF"))fprintf(stderr,"[surf] reset_target\n"); if(R) gml_surface_reset_target(R); *out=vreal(0); return 1; }
    if(!strcmp(nm,"shader_set")){ if(R) R->active_shader=(int)N(a,n,0); *out=vreal(0); return 1; }
    if(!strcmp(nm,"shader_reset")){ if(R) R->active_shader=-1; *out=vreal(0); return 1; }
    if(!strcmp(nm,"shader_get_uniform")){
      *out=vreal(gml_shader_get_uniform(R,(int)N(a,n,0),S(a,n,1)));
      return 1;
    }
    if(!strcmp(nm,"shader_set_uniform_f")||!strcmp(nm,"shader_set_uniform_f_array")){
      gml_shader_set_uniform_f(R,(int)N(a,n,0),a,n);
      *out=vreal(0); return 1;
    }
    if(!strcmp(nm,"string_width")){ *out=vreal(R?gml_text_width(R,S(a,n,0)):(int)strlen(S(a,n,0))*8); return 1; }
    if(!strcmp(nm,"string_height")){ *out=vreal(R?gml_text_height(R,S(a,n,0)):8); return 1; }
    if(!strcmp(nm,"sign")){ *out=vreal(gm_sign(N(a,n,0))); return 1; }
    if(!strcmp(nm,"sqrt")){ *out=vreal(sqrt(N(a,n,0))); return 1; }
    if(!strcmp(nm,"sqr")){ double x=N(a,n,0); *out=vreal(x*x); return 1; }
    if(!strcmp(nm,"sin")){ *out=vreal(sin(N(a,n,0))); return 1; }
  }
  if(nm[0]=='t'){
    if(!strcmp(nm,"texture_get_texel_width")){ double tw; *out=vreal(texture_info(R,(int)N(a,n,0),NULL,NULL,&tw,NULL)?tw:0); return 1; }
    if(!strcmp(nm,"texture_get_texel_height")){ double th; *out=vreal(texture_info(R,(int)N(a,n,0),NULL,NULL,NULL,&th)?th:0); return 1; }
  }
  if(nm[0]=='w'){
    if(!strcmp(nm,"window_get_width")){ *out=vreal(presentation_size(vm,R,0)); return 1; }
    if(!strcmp(nm,"window_get_height")){ *out=vreal(presentation_size(vm,R,1)); return 1; }
  }
  if(nm[0]=='g'){
    if(!strcmp(nm,"gpu_set_blendenable")){ if(R) R->alphablend=N(a,n,0)>=0.5; *out=vreal(0); return 1; }
    if(!strcmp(nm,"gpu_set_blendmode")){ int bm=(int)N(a,n,0); if(R) R->blendmode=(bm==1)?1:(bm==3)?2:0; *out=vreal(0); return 1; }
    if(!strcmp(nm,"gpu_set_blendmode_ext")){ builtin_set_blendmode_ext(R,(int)N(a,n,0),(int)N(a,n,1)); *out=vreal(0); return 1; }
    if(!strcmp(nm,"gpu_set_blendmode_ext_sepalpha")){ builtin_set_blendmode_ext(R,(int)N(a,n,0),(int)N(a,n,1)); *out=vreal(0); return 1; }
  }
  if(nm[0]=='p' && (!strcmp(nm,"part_system_drawit")||!strcmp(nm,"part_system_drawit_ext"))){
    if(R) gml_part_system_drawit(R,(int)N(a,n,0)); *out=vreal(0); return 1;
  }
  if(nm[0]!='d' || strncmp(nm,"draw_",5)) return 0;
  if(!strcmp(nm,"draw_sprite")){ if(R) gml_draw_sprite(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_sprite_ext")){ if(R) gml_draw_sprite_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),
      N(a,n,4),N(a,n,5),N(a,n,6),(uint32_t)N(a,n,7),N(a,n,8)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_self")){ GmlInstance*s=vm->cur_self; if(R&&s) gml_draw_sprite_ext(R,(int)s->sprite_index,
      (int)s->image_index,s->x,s->y,s->image_xscale,s->image_yscale,s->image_angle,(uint32_t)s->image_blend,s->image_alpha); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_sprite_stretched")){ if(R) gml_draw_sprite_stretched(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),0xFFFFFF,R->alpha); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_sprite_stretched_ext")){ if(R) gml_draw_sprite_stretched(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),(uint32_t)N(a,n,6),N(a,n,7)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_surface")){ if(R){ int s=(int)N(a,n,0);
      if(s>0 && s==(int)gml_global_arr(vm,"view_surface_id",0) && N(a,n,1)==0 && N(a,n,2)==0)
        gml_draw_surface_stretched(R,s,R->cam_x,R->cam_y,R->fbw,R->fbh,0xFFFFFF,R->alpha);
      else
        gml_draw_surface_stretched(R,s,N(a,n,1),N(a,n,2),gml_surface_width(R,s),gml_surface_height(R,s),0xFFFFFF,R->alpha); }
    *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_surface_stretched")){ if(R) gml_draw_surface_stretched(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),0xFFFFFF,R->alpha); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_surface_stretched_ext")){ if(R) gml_draw_surface_stretched(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),(uint32_t)N(a,n,5),N(a,n,6)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_surface_ext")){ if(R){ int s=(int)N(a,n,0); gml_draw_surface_stretched(R,s,N(a,n,1),N(a,n,2),gml_surface_width(R,s)*N(a,n,3),gml_surface_height(R,s)*N(a,n,4),(uint32_t)N(a,n,6),N(a,n,7)); } *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_surface_part_ext")){ if(R) gml_draw_surface_part_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),N(a,n,7),N(a,n,8),(uint32_t)N(a,n,9),N(a,n,10)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_rectangle")||!strcmp(nm,"draw_rectangle_colour")||!strcmp(nm,"draw_rectangle_color")){
    if(R){ int plain=!strcmp(nm,"draw_rectangle"); int outline=(int)N(a,n,plain?4:8);
      int x1=(int)floor(N(a,n,0)-R->cam_x), y1=(int)floor(N(a,n,1)-R->cam_y);
      int x2=(int)ceil(N(a,n,2)-R->cam_x), y2=(int)ceil(N(a,n,3)-R->cam_y);
      if(plain) draw_rect_prim(R,x1,y1,x2,y2,R->color,outline);
      else draw_rect_colour_prim(R,x1,y1,x2,y2,(uint32_t)N(a,n,4),(uint32_t)N(a,n,5),(uint32_t)N(a,n,6),(uint32_t)N(a,n,7),outline); }
    *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_set_color")||!strcmp(nm,"draw_set_colour")){ if(R) R->color=(uint32_t)N(a,n,0); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_set_alpha")){ if(R){ R->alpha=N(a,n,0); if(R->alpha<0) R->alpha=0; if(R->alpha>1) R->alpha=1; } *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_set_blend_mode_ext")){ builtin_set_blendmode_ext(R,(int)N(a,n,0),(int)N(a,n,1)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_set_font")){ if(R){ R->font=(int)N(a,n,0); if((int)N(a,n,0)<0 && getenv("GML_LOG_FONT")) fprintf(stderr,"[font] draw_set_font(%d) — default-font request\n",(int)N(a,n,0)); } *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_set_halign")){ if(R) R->halign=(int)N(a,n,0); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_set_valign")){ if(R) R->valign=(int)N(a,n,0); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_text")){ if(R) gml_draw_text(R,N(a,n,0),N(a,n,1),S(a,n,2)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_text_color")||!strcmp(nm,"draw_text_colour")){
    if(R){ uint32_t c=R->color; double al=R->alpha; R->color=(uint32_t)N(a,n,3); R->alpha=N(a,n,7); gml_draw_text(R,N(a,n,0),N(a,n,1),S(a,n,2)); R->color=c; R->alpha=al; }
    *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_text_ext")){ if(R) gml_draw_text_ext(R,N(a,n,0),N(a,n,1),S(a,n,2),N(a,n,3),N(a,n,4)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_text_ext_colour")||!strcmp(nm,"draw_text_ext_color")){
    if(R){ uint32_t c=R->color; double al=R->alpha; R->color=(uint32_t)N(a,n,5); R->alpha=N(a,n,9); gml_draw_text_ext(R,N(a,n,0),N(a,n,1),S(a,n,2),N(a,n,3),N(a,n,4)); R->color=c; R->alpha=al; }
    *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_text_transformed")){ if(R) gml_draw_text_transformed(R,N(a,n,0),N(a,n,1),S(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),R->color,R->alpha); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_text_transformed_color")||!strcmp(nm,"draw_text_transformed_colour")){ if(R) gml_draw_text_transformed(R,N(a,n,0),N(a,n,1),S(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),(uint32_t)N(a,n,6),N(a,n,10)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_text_ext_transformed")){ if(R) gml_draw_text_transformed(R,N(a,n,0),N(a,n,1),S(a,n,2),N(a,n,5),N(a,n,6),N(a,n,7),R->color,R->alpha); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_text_ext_transformed_color")||!strcmp(nm,"draw_text_ext_transformed_colour")){ if(R) gml_draw_text_transformed(R,N(a,n,0),N(a,n,1),S(a,n,2),N(a,n,5),N(a,n,6),N(a,n,7),(uint32_t)N(a,n,8),N(a,n,12)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"draw_set_blend_mode")){ int bm=(int)N(a,n,0); if(R) R->blendmode=(bm==1)?1:(bm==3)?2:0; *out=vreal(0); return 1; }
  return 0;
}

enum {
  BID_ARRAY_LENGTH=1,
  BID_ARRAY_LENGTH_1D,
  BID_ARRAY_GET,
  BID_ARRAY_SET,
  BID_ARRAY_CREATE,
  BID_ARRAY_POP,
  BID_ARRAY_RESIZE,
  BID_ARRAY_COPY,
  BID_ARRAY_HEIGHT_2D,
  BID_ARRAY_LENGTH_2D,
  BID_DRAW_SPRITE,
  BID_DRAW_SPRITE_EXT,
  BID_DRAW_SELF,
  BID_DRAW_SURFACE,
  BID_DRAW_SURFACE_EXT,
  BID_DRAW_SURFACE_STRETCHED,
  BID_DRAW_SURFACE_STRETCHED_EXT,
  BID_DRAW_SURFACE_PART_EXT,
  BID_DRAW_RECTANGLE_COLOR,
  BID_DRAW_RECTANGLE_COLOUR,
  BID_DRAW_TEXT,
  BID_DRAW_TEXT_EXT,
  BID_DRAW_TEXT_EXT_TRANSFORMED_COLOUR,
  BID_DRAW_TEXT_EXT_TRANSFORMED_COLOR,
  BID_DRAW_SET_ALPHA,
  BID_DRAW_SET_FONT,
  BID_DRAW_SET_HALIGN,
  BID_DRAW_SET_VALIGN,
  BID_GPU_SET_BLENDENABLE,
  BID_GPU_SET_BLENDMODE,
  BID_PART_SYSTEM_DRAWIT,
  BID_PART_SYSTEM_DRAWIT_EXT,
  BID_PLACE_MEETING,
  BID_INSTANCE_EXISTS,
  BID_INSTANCE_NUMBER,
  BID_INSTANCE_PLACE_LIST,
  BID_SIN,
  BID_ARRAY_PUSH,
  BID_DRAW_RECTANGLE,
  BID_DRAW_SET_COLOR,
  BID_SHADER_SET,
  BID_SHADER_RESET,
  BID_SHADER_GET_UNIFORM,
  BID_SHADER_SET_UNIFORM_F,
  BID_SHADER_SET_UNIFORM_F_ARRAY,
  BID_DISTANCE_TO_OBJECT,
  BID_FLOOR,
  BID_FRAC,
  BID_ABS,
  BID_MIN,
  BID_MAX,
  BID_CLAMP,
  BID_LENGTHDIR_X,
  BID_LENGTHDIR_Y,
  BID_POINT_DISTANCE,
  BID_POINT_DIRECTION,
  BID_KEYBOARD_CHECK,
  BID_KEYBOARD_CHECK_PRESSED,
  BID_KEYBOARD_CHECK_RELEASED,
  BID_KEYBOARD_CHECK_DIRECT,
  BID_KEYBOARD_CLEAR,
  BID_KEYBOARD_KEY_PRESS,
  BID_KEYBOARD_KEY_RELEASE,
  BID_GAMEPAD_BUTTON_CHECK,
  BID_GAMEPAD_BUTTON_VALUE,
  BID_GAMEPAD_BUTTON_CHECK_PRESSED,
  BID_GAMEPAD_BUTTON_CHECK_RELEASED,
  BID_GAMEPAD_IS_CONNECTED,
  BID_GAMEPAD_IS_SUPPORTED,
  BID_GAMEPAD_GET_DEVICE_COUNT,
  BID_GAMEPAD_BUTTON_COUNT,
  BID_GAMEPAD_AXIS_COUNT,
  BID_GAMEPAD_SET_AXIS_DEADZONE,
  BID_GAMEPAD_SET_VIBRATION,
  BID_GAMEPAD_AXIS_VALUE,
  BID_MOUSE_CHECK_BUTTON,
  BID_MOUSE_CHECK_BUTTON_PRESSED,
  BID_MOUSE_CHECK_BUTTON_RELEASED,
  BID_DEVICE_MOUSE_CHECK_BUTTON,
  BID_DEVICE_MOUSE_CHECK_BUTTON_PRESSED,
  BID_DEVICE_MOUSE_CHECK_BUTTON_RELEASED,
  BID_DEVICE_MOUSE_X,
  BID_DEVICE_MOUSE_Y,
  BID_DEVICE_MOUSE_X_TO_GUI,
  BID_DEVICE_MOUSE_Y_TO_GUI,
  BID_DEVICE_MOUSE_RAW_X,
  BID_DEVICE_MOUSE_RAW_Y,
  BID_WINDOW_MOUSE_SET,
  BID_DISPLAY_GET_WIDTH,
  BID_DISPLAY_GET_HEIGHT,
  BID_DISPLAY_GET_GUI_WIDTH,
  BID_DISPLAY_GET_GUI_HEIGHT,
  BID_WINDOW_GET_WIDTH,
  BID_WINDOW_GET_HEIGHT,
  BID_SURFACE_EXISTS,
  BID_SURFACE_CREATE,
  BID_SURFACE_FREE,
  BID_SURFACE_GET_TEXTURE,
  BID_SURFACE_GET_WIDTH,
  BID_SURFACE_GET_HEIGHT,
  BID_SURFACE_SET_TARGET,
  BID_SURFACE_RESET_TARGET,
  BID_TEXTURE_GET_TEXEL_WIDTH,
  BID_TEXTURE_GET_TEXEL_HEIGHT,
  BID_STRING_WIDTH,
  BID_STRING_HEIGHT,
  BID_GPU_SET_BLENDMODE_EXT,
  BID_DS_MAP_FIND_VALUE,
  BID_DS_MAP_FIND_NEXT,
  BID_DS_MAP_FIND_PREVIOUS,
  BID_DS_MAP_EXISTS,
  BID_DS_MAP_SIZE,
  BID_DS_MAP_EMPTY,
  BID_DS_MAP_FIND_FIRST,
  BID_DS_MAP_FIND_LAST,
  BID_DS_LIST_FIND_VALUE,
  BID_DS_LIST_SIZE,
  BID_IS_ARRAY,
  BID_IS_UNDEFINED,
  BID_IS_STRING,
  BID_IS_REAL,
  BID_STRING_ORD_AT,
  BID_STRING_CHAR_AT,
  BID_STRING_LENGTH,
  BID_ORD,
  BID_FILE_TEXT_EOF,
  BID_FILE_TEXT_OPEN_READ,
  BID_FILE_TEXT_READ_STRING,
  BID_FILE_TEXT_READLN,
  BID_INI_OPEN,
  BID_FMOD_PREFIX,
  BID_EVENT_INHERITED,
  BID_DS_LIST_CLEAR,
  BID_GPU_SET_TEXFILTER,
  BID_WINDOW_HAS_FOCUS,
  BID_SPRITE_EXISTS,
  BID_SPRITE_GET_WIDTH,
  BID_SPRITE_GET_HEIGHT,
  BID_INPUT_KBGP
};

int gml_builtin_fast_id(const char *nm){
  if(!nm || !*nm) return -1;
  switch(nm[0]){
    case 'a':
      if(!strcmp(nm,"array_length")) return BID_ARRAY_LENGTH;
      if(!strcmp(nm,"array_length_1d")) return BID_ARRAY_LENGTH_1D;
      if(!strcmp(nm,"array_get")) return BID_ARRAY_GET;
      if(!strcmp(nm,"array_set")) return BID_ARRAY_SET;
      if(!strcmp(nm,"array_create")) return BID_ARRAY_CREATE;
      if(!strcmp(nm,"array_push")) return BID_ARRAY_PUSH;
      if(!strcmp(nm,"array_pop")) return BID_ARRAY_POP;
      if(!strcmp(nm,"array_resize")) return BID_ARRAY_RESIZE;
      if(!strcmp(nm,"array_copy")) return BID_ARRAY_COPY;
      if(!strcmp(nm,"array_height_2d")) return BID_ARRAY_HEIGHT_2D;
      if(!strcmp(nm,"array_length_2d")) return BID_ARRAY_LENGTH_2D;
      if(!strcmp(nm,"abs")) return BID_ABS;
      return -1;
    case 'c':
      if(!strcmp(nm,"clamp")) return BID_CLAMP;
      return -1;
    case 'd':
      if(!strcmp(nm,"display_get_width")) return BID_DISPLAY_GET_WIDTH;
      if(!strcmp(nm,"display_get_height")) return BID_DISPLAY_GET_HEIGHT;
      if(!strcmp(nm,"display_get_gui_width")) return BID_DISPLAY_GET_GUI_WIDTH;
      if(!strcmp(nm,"display_get_gui_height")) return BID_DISPLAY_GET_GUI_HEIGHT;
      if(!strcmp(nm,"ds_map_find_value")) return log_ds_on()? -1 : BID_DS_MAP_FIND_VALUE;
      if(!strcmp(nm,"ds_map_find_next")) return BID_DS_MAP_FIND_NEXT;
      if(!strcmp(nm,"ds_map_find_previous")) return BID_DS_MAP_FIND_PREVIOUS;
      if(!strcmp(nm,"ds_map_exists")) return BID_DS_MAP_EXISTS;
      if(!strcmp(nm,"ds_map_size")) return BID_DS_MAP_SIZE;
      if(!strcmp(nm,"ds_map_empty")) return BID_DS_MAP_EMPTY;
      if(!strcmp(nm,"ds_map_find_first")) return BID_DS_MAP_FIND_FIRST;
      if(!strcmp(nm,"ds_map_find_last")) return BID_DS_MAP_FIND_LAST;
      if(!strcmp(nm,"ds_list_find_value")) return BID_DS_LIST_FIND_VALUE;
      if(!strcmp(nm,"ds_list_size")) return BID_DS_LIST_SIZE;
      if(!strcmp(nm,"draw_sprite")) return BID_DRAW_SPRITE;
      if(!strcmp(nm,"draw_sprite_ext")) return BID_DRAW_SPRITE_EXT;
      if(!strcmp(nm,"draw_self")) return BID_DRAW_SELF;
      if(!strcmp(nm,"draw_surface")) return BID_DRAW_SURFACE;
      if(!strcmp(nm,"draw_surface_ext")) return BID_DRAW_SURFACE_EXT;
      if(!strcmp(nm,"draw_surface_stretched")) return BID_DRAW_SURFACE_STRETCHED;
      if(!strcmp(nm,"draw_surface_stretched_ext")) return BID_DRAW_SURFACE_STRETCHED_EXT;
      if(!strcmp(nm,"draw_surface_part_ext")) return BID_DRAW_SURFACE_PART_EXT;
      if(!strcmp(nm,"draw_rectangle")) return BID_DRAW_RECTANGLE;
      if(!strcmp(nm,"draw_rectangle_color")) return BID_DRAW_RECTANGLE_COLOR;
      if(!strcmp(nm,"draw_rectangle_colour")) return BID_DRAW_RECTANGLE_COLOUR;
      if(!strcmp(nm,"draw_text")) return BID_DRAW_TEXT;
      if(!strcmp(nm,"draw_text_ext")) return BID_DRAW_TEXT_EXT;
      if(!strcmp(nm,"draw_text_ext_transformed_colour")) return BID_DRAW_TEXT_EXT_TRANSFORMED_COLOUR;
      if(!strcmp(nm,"draw_text_ext_transformed_color")) return BID_DRAW_TEXT_EXT_TRANSFORMED_COLOR;
      if(!strcmp(nm,"draw_set_color")) return BID_DRAW_SET_COLOR;
      if(!strcmp(nm,"draw_set_colour")) return BID_DRAW_SET_COLOR;
      if(!strcmp(nm,"draw_set_alpha")) return BID_DRAW_SET_ALPHA;
      if(!strcmp(nm,"draw_set_font")) return BID_DRAW_SET_FONT;
      if(!strcmp(nm,"draw_set_halign")) return BID_DRAW_SET_HALIGN;
      if(!strcmp(nm,"draw_set_valign")) return BID_DRAW_SET_VALIGN;
      if(!strcmp(nm,"distance_to_object")) return BID_DISTANCE_TO_OBJECT;
      if(!strcmp(nm,"device_mouse_check_button")) return BID_DEVICE_MOUSE_CHECK_BUTTON;
      if(!strcmp(nm,"device_mouse_check_button_pressed")) return BID_DEVICE_MOUSE_CHECK_BUTTON_PRESSED;
      if(!strcmp(nm,"device_mouse_check_button_released")) return BID_DEVICE_MOUSE_CHECK_BUTTON_RELEASED;
      if(!strcmp(nm,"device_mouse_x")) return BID_DEVICE_MOUSE_X;
      if(!strcmp(nm,"device_mouse_y")) return BID_DEVICE_MOUSE_Y;
      if(!strcmp(nm,"device_mouse_x_to_gui")) return BID_DEVICE_MOUSE_X_TO_GUI;
      if(!strcmp(nm,"device_mouse_y_to_gui")) return BID_DEVICE_MOUSE_Y_TO_GUI;
      if(!strcmp(nm,"device_mouse_raw_x")) return BID_DEVICE_MOUSE_RAW_X;
      if(!strcmp(nm,"device_mouse_raw_y")) return BID_DEVICE_MOUSE_RAW_Y;
      return -1;
    case 'f':
      if(!strcmp(nm,"floor")) return BID_FLOOR;
      if(!strcmp(nm,"frac")) return BID_FRAC;
      if(!strcmp(nm,"file_text_eof")) return BID_FILE_TEXT_EOF;
      if(!strcmp(nm,"file_text_open_read")) return BID_FILE_TEXT_OPEN_READ;
      if(!strcmp(nm,"file_text_read_string")) return BID_FILE_TEXT_READ_STRING;
      if(!strcmp(nm,"file_text_readln")) return BID_FILE_TEXT_READLN;
      if(!strncmp(nm,"fmod_",5)) return BID_FMOD_PREFIX;
      return -1;
    case 'g':
      if(!strcmp(nm,"gpu_set_blendenable")) return BID_GPU_SET_BLENDENABLE;
      if(!strcmp(nm,"gpu_set_blendmode")) return BID_GPU_SET_BLENDMODE;
      if(!strcmp(nm,"gpu_set_blendmode_ext")) return BID_GPU_SET_BLENDMODE_EXT;
      if(!strcmp(nm,"gpu_set_texfilter")) return BID_GPU_SET_TEXFILTER;
      if(!strcmp(nm,"gpu_set_texfilter_ext")) return BID_GPU_SET_TEXFILTER;
      if(!strcmp(nm,"gpu_set_tex_filter")) return BID_GPU_SET_TEXFILTER;
      if(!strcmp(nm,"gamepad_button_check")) return BID_GAMEPAD_BUTTON_CHECK;
      if(!strcmp(nm,"gamepad_button_value")) return BID_GAMEPAD_BUTTON_VALUE;
      if(!strcmp(nm,"gamepad_button_check_pressed")) return BID_GAMEPAD_BUTTON_CHECK_PRESSED;
      if(!strcmp(nm,"gamepad_button_check_released")) return BID_GAMEPAD_BUTTON_CHECK_RELEASED;
      if(!strcmp(nm,"gamepad_is_connected")) return BID_GAMEPAD_IS_CONNECTED;
      if(!strcmp(nm,"gamepad_is_supported")) return BID_GAMEPAD_IS_SUPPORTED;
      if(!strcmp(nm,"gamepad_get_device_count")) return BID_GAMEPAD_GET_DEVICE_COUNT;
      if(!strcmp(nm,"gamepad_button_count")) return BID_GAMEPAD_BUTTON_COUNT;
      if(!strcmp(nm,"gamepad_axis_count")) return BID_GAMEPAD_AXIS_COUNT;
      if(!strcmp(nm,"gamepad_set_axis_deadzone")) return BID_GAMEPAD_SET_AXIS_DEADZONE;
      if(!strcmp(nm,"gamepad_set_vibration")) return BID_GAMEPAD_SET_VIBRATION;
      if(!strcmp(nm,"gamepad_axis_value")) return BID_GAMEPAD_AXIS_VALUE;
      if(!strncmp(nm,"gamepad_",8)) return BID_INPUT_KBGP;
      return -1;
    case 'e':
      if(!strcmp(nm,"event_inherited")) return BID_EVENT_INHERITED;
      return -1;
    case 'F':
      if(!strcmp(nm,"FS_ini_open")) return BID_INI_OPEN;
      return -1;
    case 'i':
      if(!strcmp(nm,"ini_open")) return BID_INI_OPEN;
      if(!strcmp(nm,"instance_exists")) return BID_INSTANCE_EXISTS;
      if(!strcmp(nm,"instance_number")) return BID_INSTANCE_NUMBER;
      if(!strcmp(nm,"instance_place_list")) return BID_INSTANCE_PLACE_LIST;
      if(!strcmp(nm,"is_array")) return BID_IS_ARRAY;
      if(!strcmp(nm,"is_undefined")) return BID_IS_UNDEFINED;
      if(!strcmp(nm,"is_string")) return BID_IS_STRING;
      if(!strcmp(nm,"is_real")||!strcmp(nm,"is_numeric")) return BID_IS_REAL;
      return -1;
    case 'k':
      if(!strcmp(nm,"keyboard_check")) return BID_KEYBOARD_CHECK;
      if(!strcmp(nm,"keyboard_check_pressed")) return BID_KEYBOARD_CHECK_PRESSED;
      if(!strcmp(nm,"keyboard_check_released")) return BID_KEYBOARD_CHECK_RELEASED;
      if(!strcmp(nm,"keyboard_check_direct")) return BID_KEYBOARD_CHECK_DIRECT;
      if(!strcmp(nm,"keyboard_clear")) return BID_KEYBOARD_CLEAR;
      if(!strcmp(nm,"keyboard_key_press")) return BID_KEYBOARD_KEY_PRESS;
      if(!strcmp(nm,"keyboard_key_release")) return BID_KEYBOARD_KEY_RELEASE;
      return -1;
    case 'l':
      if(!strcmp(nm,"lengthdir_x")) return BID_LENGTHDIR_X;
      if(!strcmp(nm,"lengthdir_y")) return BID_LENGTHDIR_Y;
      return -1;
    case 'm':
      if(!strcmp(nm,"mouse_check_button")) return BID_MOUSE_CHECK_BUTTON;
      if(!strcmp(nm,"mouse_check_button_pressed")) return BID_MOUSE_CHECK_BUTTON_PRESSED;
      if(!strcmp(nm,"mouse_check_button_released")) return BID_MOUSE_CHECK_BUTTON_RELEASED;
      if(!strcmp(nm,"min")) return BID_MIN;
      if(!strcmp(nm,"max")) return BID_MAX;
      return -1;
    case 'p':
      if(!strcmp(nm,"part_system_drawit")) return BID_PART_SYSTEM_DRAWIT;
      if(!strcmp(nm,"part_system_drawit_ext")) return BID_PART_SYSTEM_DRAWIT_EXT;
      if(!strcmp(nm,"place_meeting")) return BID_PLACE_MEETING;
      if(!strcmp(nm,"point_distance")) return BID_POINT_DISTANCE;
      if(!strcmp(nm,"point_direction")) return BID_POINT_DIRECTION;
      return -1;
    case 'o':
      if(!strcmp(nm,"ord")) return BID_ORD;
      return -1;
    case 's':
      if(!strcmp(nm,"string_ord_at")) return BID_STRING_ORD_AT;
      if(!strcmp(nm,"string_char_at")) return BID_STRING_CHAR_AT;
      if(!strcmp(nm,"string_length")) return BID_STRING_LENGTH;
      if(!strcmp(nm,"sin")) return BID_SIN;
      if(!strcmp(nm,"shader_set")) return BID_SHADER_SET;
      if(!strcmp(nm,"shader_reset")) return BID_SHADER_RESET;
      if(!strcmp(nm,"shader_get_uniform")) return BID_SHADER_GET_UNIFORM;
      if(!strcmp(nm,"shader_set_uniform_f")) return BID_SHADER_SET_UNIFORM_F;
      if(!strcmp(nm,"shader_set_uniform_f_array")) return BID_SHADER_SET_UNIFORM_F_ARRAY;
      if(!strcmp(nm,"surface_exists")) return BID_SURFACE_EXISTS;
      if(!strcmp(nm,"surface_create")) return BID_SURFACE_CREATE;
      if(!strcmp(nm,"surface_free")) return BID_SURFACE_FREE;
      if(!strcmp(nm,"surface_get_texture")) return BID_SURFACE_GET_TEXTURE;
      if(!strcmp(nm,"surface_get_width")) return BID_SURFACE_GET_WIDTH;
      if(!strcmp(nm,"surface_get_height")) return BID_SURFACE_GET_HEIGHT;
      if(!strcmp(nm,"surface_set_target")) return BID_SURFACE_SET_TARGET;
      if(!strcmp(nm,"surface_reset_target")) return BID_SURFACE_RESET_TARGET;
      if(!strcmp(nm,"sprite_exists")) return BID_SPRITE_EXISTS;
      if(!strcmp(nm,"sprite_get_width")) return BID_SPRITE_GET_WIDTH;
      if(!strcmp(nm,"sprite_get_height")) return BID_SPRITE_GET_HEIGHT;
      if(!strcmp(nm,"string_width")) return BID_STRING_WIDTH;
      if(!strcmp(nm,"string_height")) return BID_STRING_HEIGHT;
      return -1;
    case 't':
      if(!strcmp(nm,"texture_get_texel_width")) return BID_TEXTURE_GET_TEXEL_WIDTH;
      if(!strcmp(nm,"texture_get_texel_height")) return BID_TEXTURE_GET_TEXEL_HEIGHT;
      return -1;
    case 'w':
      if(!strcmp(nm,"window_mouse_get_x")) return BID_DEVICE_MOUSE_RAW_X;
      if(!strcmp(nm,"window_mouse_get_y")) return BID_DEVICE_MOUSE_RAW_Y;
      if(!strcmp(nm,"window_mouse_set")) return BID_WINDOW_MOUSE_SET;
      if(!strcmp(nm,"window_has_focus")) return BID_WINDOW_HAS_FOCUS;
      if(!strcmp(nm,"window_get_width")) return BID_WINDOW_GET_WIDTH;
      if(!strcmp(nm,"window_get_height")) return BID_WINDOW_GET_HEIGHT;
      return -1;
    default:
      return -1;
  }
}

static int gp_debug_on(void){
  static int dbg=-1;
  if(dbg<0) dbg=getenv("GML_DBG_GP")!=NULL;
  return dbg;
}

/* keyboard/gamepad dispatch shared by the generic chain and the cached fast path — the
 * bodies are the chain handlers verbatim (input polls run hundreds of times per frame in
 * GMS2 input wrappers, and each one otherwise walks most of the name chain). */
static int builtin_input_kbgp(const char *nm, GmlVal *a, int n, GmlVal *out){
  if(!strcmp(nm,"keyboard_check")){          *out=vreal(gml_input_key((int)N(a,n,0),0)); return 1; }
  if(!strcmp(nm,"keyboard_check_pressed")){  *out=vreal(gml_input_key((int)N(a,n,0),1)); return 1; }
  if(!strcmp(nm,"keyboard_check_released")){ *out=vreal(gml_input_key((int)N(a,n,0),2)); return 1; }
  if(!strcmp(nm,"keyboard_check_direct")){   *out=vreal(gml_input_key((int)N(a,n,0),0)); return 1; }
  if(!strcmp(nm,"keyboard_clear")){ gml_input_key_clear((int)N(a,n,0)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"keyboard_key_press")){ gml_input_key_press((int)N(a,n,0)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"keyboard_key_release")){ gml_input_key_release((int)N(a,n,0)); *out=vreal(0); return 1; }
  /* gamepad button/axis reads expose the single RetroPad slot; connection/discovery is separate. */
  {
    if(gp_debug_on() && !strncmp(nm,"gamepad_button",14)){
      extern long g_vm_frame;
      fprintf(stderr,"[gp] f%ld %s n=%d a0=%.0f a1=%.0f -> %d\n",g_vm_frame,nm,n,N(a,n,0),N(a,n,1),
        gml_input_gamepad((int)N(a,n,1),0)); } }
  if(!strcmp(nm,"gamepad_button_check")){          *out=vreal(gml_input_gamepad((int)N(a,n,1),0)); return 1; }
  if(!strcmp(nm,"gamepad_button_value")){          *out=vreal(gml_input_gamepad((int)N(a,n,1),0) ? 1.0 : 0.0); return 1; }
  if(!strcmp(nm,"gamepad_button_check_pressed")){  *out=vreal(gml_input_gamepad((int)N(a,n,1),1)); return 1; }
  if(!strcmp(nm,"gamepad_button_check_released")){ *out=vreal(gml_input_gamepad((int)N(a,n,1),2)); return 1; }
  if(!strcmp(nm,"gamepad_is_connected")){          *out=vreal(gml_input_gamepad_connected((int)N(a,n,0))); return 1; }
  if(!strcmp(nm,"gamepad_is_supported")){          *out=vreal(1); return 1; }
  if(!strcmp(nm,"gamepad_get_device_count")){      *out=vreal(gml_input_gamepad_device_count()); return 1; }
  if(!strcmp(nm,"gamepad_button_count")){          *out=vreal(16); return 1; }
  if(!strcmp(nm,"gamepad_axis_count")){            *out=vreal(4); return 1; }
  if(!strcmp(nm,"gamepad_get_description")){       *out=vstr(gml_input_gamepad_connected((int)N(a,n,0)) ? "libretro" : ""); return 1; }
  if(!strcmp(nm,"gamepad_set_axis_deadzone")){ gp_deadzone_set((int)N(a,n,0), N(a,n,1)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"gamepad_set_button_threshold")){ *out=vreal(0); return 1; }
  if(!strcmp(nm,"gamepad_set_vibration")){
    gml_input_gamepad_set_vibration((int)N(a,n,0), N(a,n,1), N(a,n,2));
    *out=vreal(0); return 1;
  }
  if(!strcmp(nm,"gamepad_axis_value")){
    *out=vreal(gp_axis_value_filtered((int)N(a,n,0), (int)N(a,n,1))); return 1; }
  if(!strncmp(nm,"gamepad_",8)){ *out=vreal(0); return 1; }
  return 0;
}

static GmlVal builtin_fmod(GmlVM *vm, const char *nm, GmlVal *a, int n);
static GmlVal builtin_call_impl(GmlVM *vm, const char *nm, GmlVal *a, int n);
GmlVal gml_builtin_call_fast_id(GmlVM *vm, int id, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  switch(id){
    /* the bodies below mirror their generic-chain handlers exactly; keep both in sync */
    case BID_FMOD_PREFIX:
      return builtin_fmod(vm,nm,a,n);
    case BID_INPUT_KBGP:{
      GmlVal v; if(builtin_input_kbgp(nm,a,n,&v)) return v;
      return builtin_call_impl(vm,nm,a,n); }   /* unreachable for the ids we hand out; safety net */
    case BID_EVENT_INHERITED:
      gml_event_inherited(vm); return vreal(0);
    case BID_DS_LIST_CLEAR:{
      GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); if(l) l->len=0; return vreal(0); }
    case BID_GPU_SET_TEXFILTER:
      return vreal(0);
    case BID_WINDOW_HAS_FOCUS:
      return vreal(1);
    case BID_SPRITE_EXISTS:{
      int spr=(int)N(a,n,0); return vreal(R&&gml_sprite_exists(R,spr)); }
    case BID_SPRITE_GET_WIDTH:{
      int spr=(int)N(a,n,0); return vreal((R&&gml_sprite_exists(R,spr))?R->spr[spr].w:0); }
    case BID_SPRITE_GET_HEIGHT:{
      int spr=(int)N(a,n,0); return vreal((R&&gml_sprite_exists(R,spr))?R->spr[spr].h:0); }
    case BID_DS_MAP_FIND_VALUE:{
      return gml_ds_map_find_value_direct(vm,(int)N(a,n,0),n>=2?a[1]:vundef(),n>=2); }
    case BID_DS_MAP_FIND_NEXT:
    case BID_DS_MAP_FIND_PREVIOUS:{
      GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
      DsKeyTemp kt={0};
      const char *key=(n>=2)?ds_key_temp(a[1],&kt):NULL;
      int i=ds_map_find_entry(m,key); ds_key_temp_free(&kt);
      int j = (i<0) ? -1 : (id==BID_DS_MAP_FIND_NEXT ? i+1 : i-1);
      if(m && j>=0 && j<m->len) return ds_ret(m->entry[j].key_val);
      return vundef(); }
    case BID_DS_MAP_EXISTS:{
      GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
      DsKeyTemp kt={0};
      const char *key=(n>=2)?ds_key_temp(a[1],&kt):NULL;
      int i=ds_map_find_entry(m,key); ds_key_temp_free(&kt); return vreal(i>=0); }
    case BID_DS_MAP_SIZE:{
      GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0)); return vreal(m?m->len:0); }
    case BID_DS_MAP_EMPTY:{
      GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0)); return vreal(!m||m->len==0); }
    case BID_DS_MAP_FIND_FIRST:{
      GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
      return (m && m->len>0) ? ds_ret(m->entry[0].key_val) : vstr(""); }
    case BID_DS_MAP_FIND_LAST:{
      GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
      return (m && m->len>0) ? ds_ret(m->entry[m->len-1].key_val) : vstr(""); }
    case BID_DS_LIST_FIND_VALUE:{
      GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); int p=(int)N(a,n,1);
      return (l && p>=0 && p<l->len)? ds_ret(l->item[p]) : vreal(0); }
    case BID_DS_LIST_SIZE:{
      GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return vreal(l?l->len:0); }
    case BID_IS_ARRAY:
      return vreal(n>0 && a[0].t==V_ARR);
    case BID_IS_UNDEFINED:
      return vreal(n>0 && a[0].t==V_UNDEF);
    case BID_IS_STRING:
      return vreal(n>0 && a[0].t==V_STR);
    case BID_IS_REAL:
      return vreal(n>0 && a[0].t==V_REAL);
    case BID_STRING_ORD_AT:{
      const unsigned char*s=(const unsigned char*)S(a,n,0); int idx=(int)N(a,n,1), len=(int)strlen((const char*)s);
      return vreal((idx>=1&&idx<=len)?s[idx-1]:0); }
    case BID_STRING_CHAR_AT:{
      const char*s=S(a,n,0); int idx=(int)N(a,n,1), len=(int)strlen(s);
      if(idx<1||idx>len) return vstr("");
      return vstr_owned(dup_n(s+idx-1,1)); }
    case BID_STRING_LENGTH:
      return vreal((double)strlen(S(a,n,0)));
    case BID_ORD:{
      const unsigned char*s=(const unsigned char*)S(a,n,0); return vreal(s[0]); }
    case BID_FILE_TEXT_EOF:{
      int i=vm_file_slot(vm,(int)N(a,n,0)); if(i<0) return vreal(1);
      FILE *f=(FILE*)vm->bin_file[i]; int c=fgetc(f); if(c==EOF) return vreal(1); ungetc(c,f); return vreal(0); }
    case BID_FILE_TEXT_OPEN_READ:
      return builtin_file_text_open_read(vm,a,n);
    case BID_FILE_TEXT_READ_STRING:
      return builtin_file_text_read_string(vm,a,n);
    case BID_FILE_TEXT_READLN:
      return builtin_file_text_readln(vm,a,n);
    case BID_INI_OPEN:
      return builtin_ini_open_file(vm,a,n);
    case BID_ARRAY_LENGTH:
    case BID_ARRAY_LENGTH_1D:
      return vreal(n>0?gml_val_array_length(a[0]):0);
    case BID_ARRAY_GET:
      return n>1?gml_arr_get(a[0],(int)N(a,n,1)):vreal(0);
    case BID_ARRAY_SET:
      if(n>2) gml_arr_set(a[0],(int)N(a,n,1),a[2]);
      return vreal(0);
    case BID_ARRAY_CREATE:{
      int sz=n>0?(int)N(a,n,0):0;
      GmlVal fill=n>1?a[1]:vreal(0);
      return gml_arr_new(sz,fill); }
    case BID_ARRAY_PUSH:
      for(int i=1;i<n;i++) gml_arr_push(a[0],a[i]);
      return vreal(0);
    case BID_ARRAY_POP:
      return n>0?gml_arr_pop(a[0]):vreal(0);
    case BID_ARRAY_RESIZE:
      if(n>1) gml_arr_resize(a[0],(int)N(a,n,1));
      return vreal(0);
    case BID_ARRAY_COPY:
      if(n>4) gml_arr_copy(a[0],(int)N(a,n,1),a[2],(int)N(a,n,3),(int)N(a,n,4));
      return vreal(0);
    case BID_ARRAY_HEIGHT_2D:
      return vreal(n>0?gml_val_array_height_2d(a[0]):0);
    case BID_ARRAY_LENGTH_2D:
      return vreal(n>0?gml_val_array_length_2d(a[0],(int)N(a,n,1)):0);
    case BID_DRAW_SPRITE:
      if(R) gml_draw_sprite(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3));
      return vreal(0);
    case BID_DRAW_SPRITE_EXT:
      if(R) gml_draw_sprite_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),
        N(a,n,4),N(a,n,5),N(a,n,6),(uint32_t)N(a,n,7),N(a,n,8));
      return vreal(0);
    case BID_DRAW_SELF:{
      GmlInstance*s=vm->cur_self;
      if(R&&s) gml_draw_sprite_ext(R,(int)s->sprite_index,
        (int)s->image_index,s->x,s->y,s->image_xscale,s->image_yscale,s->image_angle,(uint32_t)s->image_blend,s->image_alpha);
      return vreal(0); }
    case BID_DRAW_SURFACE:
      if(R){ int s=(int)N(a,n,0);
        if(s>0 && s==(int)gml_global_arr(vm,"view_surface_id",0) && N(a,n,1)==0 && N(a,n,2)==0)
          gml_draw_surface_stretched(R,s,R->cam_x,R->cam_y,R->fbw,R->fbh,0xFFFFFF,R->alpha);
        else
          gml_draw_surface_stretched(R,s,N(a,n,1),N(a,n,2),gml_surface_width(R,s),gml_surface_height(R,s),0xFFFFFF,R->alpha);
      }
      return vreal(0);
    case BID_DRAW_SURFACE_EXT:
      if(R){ int s=(int)N(a,n,0); gml_draw_surface_stretched(R,s,N(a,n,1),N(a,n,2),
        gml_surface_width(R,s)*N(a,n,3),gml_surface_height(R,s)*N(a,n,4),(uint32_t)N(a,n,6),N(a,n,7)); }
      return vreal(0);
    case BID_DRAW_SURFACE_STRETCHED:
      if(R) gml_draw_surface_stretched(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),0xFFFFFF,R->alpha);
      return vreal(0);
    case BID_DRAW_SURFACE_STRETCHED_EXT:
      if(R) gml_draw_surface_stretched(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),(uint32_t)N(a,n,5),N(a,n,6));
      return vreal(0);
    case BID_DRAW_SURFACE_PART_EXT:
      if(R) gml_draw_surface_part_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),N(a,n,7),N(a,n,8),(uint32_t)N(a,n,9),N(a,n,10));
      return vreal(0);
    case BID_DRAW_RECTANGLE_COLOR:
    case BID_DRAW_RECTANGLE_COLOUR:
      if(R){ int outline=(int)N(a,n,8);
        int x1=(int)floor(N(a,n,0)-R->cam_x), y1=(int)floor(N(a,n,1)-R->cam_y);
        int x2=(int)ceil(N(a,n,2)-R->cam_x), y2=(int)ceil(N(a,n,3)-R->cam_y);
        draw_rect_colour_prim(R,x1,y1,x2,y2,(uint32_t)N(a,n,4),(uint32_t)N(a,n,5),(uint32_t)N(a,n,6),(uint32_t)N(a,n,7),outline);
      }
      return vreal(0);
    case BID_DRAW_RECTANGLE:
      if(R){
        int x1=(int)floor(N(a,n,0)-R->cam_x), y1=(int)floor(N(a,n,1)-R->cam_y);
        int x2=(int)ceil(N(a,n,2)-R->cam_x), y2=(int)ceil(N(a,n,3)-R->cam_y);
        draw_rect_prim(R,x1,y1,x2,y2,R->color,(int)N(a,n,4));
      }
      return vreal(0);
    case BID_DRAW_SET_COLOR:
      if(R) R->color=(uint32_t)N(a,n,0);
      return vreal(0);
    case BID_DRAW_TEXT:
      if(R) gml_draw_text(R,N(a,n,0),N(a,n,1),S(a,n,2));
      return vreal(0);
    case BID_DRAW_TEXT_EXT:
      if(R) gml_draw_text_ext(R,N(a,n,0),N(a,n,1),S(a,n,2),N(a,n,3),N(a,n,4));
      return vreal(0);
    case BID_DRAW_TEXT_EXT_TRANSFORMED_COLOUR:
    case BID_DRAW_TEXT_EXT_TRANSFORMED_COLOR:
      if(R) gml_draw_text_transformed(R,N(a,n,0),N(a,n,1),S(a,n,2),N(a,n,5),N(a,n,6),N(a,n,7),(uint32_t)N(a,n,8),N(a,n,12));
      return vreal(0);
    case BID_DRAW_SET_ALPHA:
      if(R){ R->alpha=N(a,n,0); if(R->alpha<0) R->alpha=0; if(R->alpha>1) R->alpha=1; }
      return vreal(0);
    case BID_DRAW_SET_FONT:
      if(R){ R->font=(int)N(a,n,0); if((int)N(a,n,0)<0 && getenv("GML_LOG_FONT")) fprintf(stderr,"[font] draw_set_font(%d) — default-font request\n",(int)N(a,n,0)); }
      return vreal(0);
    case BID_DRAW_SET_HALIGN:
      if(R) R->halign=(int)N(a,n,0);
      return vreal(0);
    case BID_DRAW_SET_VALIGN:
      if(R) R->valign=(int)N(a,n,0);
      return vreal(0);
    case BID_GPU_SET_BLENDENABLE:
      if(R) R->alphablend=N(a,n,0)>=0.5;
      return vreal(0);
    case BID_GPU_SET_BLENDMODE:
      if(R){ int bm=(int)N(a,n,0); R->blendmode=(bm==1)?1:(bm==3)?2:0; }
      return vreal(0);
    case BID_GPU_SET_BLENDMODE_EXT:
      builtin_set_blendmode_ext(R,(int)N(a,n,0),(int)N(a,n,1));
      return vreal(0);
    case BID_SHADER_SET:
      if(R) R->active_shader=(int)N(a,n,0);
      return vreal(0);
    case BID_SHADER_RESET:
      if(R) R->active_shader=-1;
      return vreal(0);
    case BID_SHADER_GET_UNIFORM:{
      int sh=(int)N(a,n,0); const char *un=S(a,n,1);
      return vreal(gml_shader_get_uniform(R,sh,un)); }
    case BID_SHADER_SET_UNIFORM_F:
    case BID_SHADER_SET_UNIFORM_F_ARRAY:
      if(R) gml_shader_set_uniform_f(R,(int)N(a,n,0),a,n);
      return vreal(0);
    case BID_PART_SYSTEM_DRAWIT:
    case BID_PART_SYSTEM_DRAWIT_EXT:
      if(R) gml_part_system_drawit(R,(int)N(a,n,0));
      return vreal(0);
    case BID_PLACE_MEETING:{
      int r=collision_at(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2),0);
      if(log_col_on()){ GmlInstance*cs=vm->cur_self; const char*cn=(cs&&cs->obj>=0&&cs->obj<vm->n_objects)?vm->objects[cs->obj].name:"?";
        const char*tn=((int)N(a,n,2)>=0&&(int)N(a,n,2)<vm->n_objects)?vm->objects[(int)N(a,n,2)].name:"?";
        if(log_col_match(cn)) fprintf(stderr,"[col] %s place_meeting(%.0f,%.0f,%s)=%d\n",cn,N(a,n,0),N(a,n,1),tn,r); }
      return vreal(r); }
    case BID_INSTANCE_EXISTS:
      return vreal(gml_instance_number(vm,(int)N(a,n,0))>0);
    case BID_INSTANCE_NUMBER:
      return vreal(gml_instance_number(vm,(int)N(a,n,0)));
    case BID_INSTANCE_PLACE_LIST:{
      GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,3));
      int r=collision_instance_list_at(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2),l,N(a,n,4)>=0.5);
      if(log_col_on()){ GmlInstance*cs=vm->cur_self; const char*cnm=(cs&&cs->obj>=0&&cs->obj<vm->n_objects)?vm->objects[cs->obj].name:"?";
        const char*tn=((int)N(a,n,2)>=0&&(int)N(a,n,2)<vm->n_objects)?vm->objects[(int)N(a,n,2)].name:"?";
        if(log_col_match(cnm)) fprintf(stderr,"[col] %s instance_place_list(%.0f,%.0f,%s)=%d\n",cnm,N(a,n,0),N(a,n,1),tn,r); }
      return vreal(r); }
    case BID_SIN:
      return vreal(sin(N(a,n,0)));
    case BID_DISTANCE_TO_OBJECT:{
      GmlInstance*s=vm->cur_self;
      if(!s) return vreal(0);
      int obj=(int)N(a,n,0);
      double best=1e18;
      for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
        if(o==s || !target_matches_instance(vm,s,o,obj)) continue;
        double d=hypot(o->x-s->x,o->y-s->y);
        if(d<best) best=d;
      }
      return vreal(best>1e17?-1:best); }
    case BID_FLOOR:
      return vreal(floor(N(a,n,0)));
    case BID_FRAC:{
      double v=N(a,n,0); return vreal(v-floor(v)); }
    case BID_ABS:
      return vreal(fabs(N(a,n,0)));
    case BID_MIN:{
      if(n<=0) return vreal(0);
      double v=N(a,n,0);
      for(int i=1;i<n;i++){ double x=N(a,n,i); if(x<v) v=x; }
      return vreal(v); }
    case BID_MAX:{
      if(n<=0) return vreal(0);
      double v=N(a,n,0);
      for(int i=1;i<n;i++){ double x=N(a,n,i); if(x>v) v=x; }
      return vreal(v); }
    case BID_CLAMP:{
      double x=N(a,n,0), lo=N(a,n,1), hi=N(a,n,2);
      if(x<lo)x=lo;
      if(x>hi)x=hi;
      return vreal(x); }
    case BID_LENGTHDIR_X:
      return vreal(N(a,n,0)*cos(N(a,n,1)*M_PI/180.0));
    case BID_LENGTHDIR_Y:
      return vreal(-N(a,n,0)*sin(N(a,n,1)*M_PI/180.0));
    case BID_POINT_DISTANCE:
      return vreal(hypot(N(a,n,2)-N(a,n,0),N(a,n,3)-N(a,n,1)));
    case BID_POINT_DIRECTION:{
      double dx=N(a,n,2)-N(a,n,0), dy=N(a,n,3)-N(a,n,1);
      double r=atan2(-dy,dx)*180.0/M_PI; if(r<0)r+=360; return vreal(r); }
    case BID_KEYBOARD_CHECK:
    case BID_KEYBOARD_CHECK_DIRECT:
      return vreal(gml_input_key((int)N(a,n,0),0));
    case BID_KEYBOARD_CHECK_PRESSED:
      return vreal(gml_input_key((int)N(a,n,0),1));
    case BID_KEYBOARD_CHECK_RELEASED:
      return vreal(gml_input_key((int)N(a,n,0),2));
    case BID_KEYBOARD_CLEAR:
      gml_input_key_clear((int)N(a,n,0));
      return vreal(0);
    case BID_KEYBOARD_KEY_PRESS:
      gml_input_key_press((int)N(a,n,0));
      return vreal(0);
    case BID_KEYBOARD_KEY_RELEASE:
      gml_input_key_release((int)N(a,n,0));
      return vreal(0);
    case BID_GAMEPAD_BUTTON_CHECK:
      if(gp_debug_on()){ extern long g_vm_frame;
        fprintf(stderr,"[gp] f%ld gamepad_button_check n=%d a0=%.0f a1=%.0f -> %d\n",g_vm_frame,n,N(a,n,0),N(a,n,1),gml_input_gamepad((int)N(a,n,1),0)); }
      return vreal(gml_input_gamepad((int)N(a,n,1),0));
    case BID_GAMEPAD_BUTTON_VALUE:
      if(gp_debug_on()){ extern long g_vm_frame;
        fprintf(stderr,"[gp] f%ld gamepad_button_value n=%d a0=%.0f a1=%.0f -> %d\n",g_vm_frame,n,N(a,n,0),N(a,n,1),gml_input_gamepad((int)N(a,n,1),0)); }
      return vreal(gml_input_gamepad((int)N(a,n,1),0) ? 1.0 : 0.0);
    case BID_GAMEPAD_BUTTON_CHECK_PRESSED:
      if(gp_debug_on()){ extern long g_vm_frame;
        fprintf(stderr,"[gp] f%ld gamepad_button_check_pressed n=%d a0=%.0f a1=%.0f -> %d\n",g_vm_frame,n,N(a,n,0),N(a,n,1),gml_input_gamepad((int)N(a,n,1),1)); }
      return vreal(gml_input_gamepad((int)N(a,n,1),1));
    case BID_GAMEPAD_BUTTON_CHECK_RELEASED:
      if(gp_debug_on()){ extern long g_vm_frame;
        fprintf(stderr,"[gp] f%ld gamepad_button_check_released n=%d a0=%.0f a1=%.0f -> %d\n",g_vm_frame,n,N(a,n,0),N(a,n,1),gml_input_gamepad((int)N(a,n,1),2)); }
      return vreal(gml_input_gamepad((int)N(a,n,1),2));
    case BID_GAMEPAD_IS_CONNECTED:
      return vreal(gml_input_gamepad_connected((int)N(a,n,0)));
    case BID_GAMEPAD_IS_SUPPORTED:
      return vreal(1);
    case BID_GAMEPAD_GET_DEVICE_COUNT:
      return vreal(gml_input_gamepad_device_count());
    case BID_GAMEPAD_BUTTON_COUNT:
      return vreal(16);
    case BID_GAMEPAD_AXIS_COUNT:
      return vreal(4);
    case BID_GAMEPAD_SET_AXIS_DEADZONE:
      gp_deadzone_set((int)N(a,n,0),N(a,n,1));
      return vreal(0);
    case BID_GAMEPAD_SET_VIBRATION:
      gml_input_gamepad_set_vibration((int)N(a,n,0),N(a,n,1),N(a,n,2));
      return vreal(0);
    case BID_GAMEPAD_AXIS_VALUE:
      return vreal(gp_axis_value_filtered((int)N(a,n,0),(int)N(a,n,1)));
    case BID_MOUSE_CHECK_BUTTON:
      return vreal(mouse_btn_check((int)N(a,n,0),0));
    case BID_MOUSE_CHECK_BUTTON_PRESSED:
      return vreal(mouse_btn_check((int)N(a,n,0),1));
    case BID_MOUSE_CHECK_BUTTON_RELEASED:
      return vreal(mouse_btn_check((int)N(a,n,0),2));
    case BID_DEVICE_MOUSE_CHECK_BUTTON:
      return vreal(mouse_btn_check((int)N(a,n,1),0));
    case BID_DEVICE_MOUSE_CHECK_BUTTON_PRESSED:
      return vreal(mouse_btn_check((int)N(a,n,1),1));
    case BID_DEVICE_MOUSE_CHECK_BUTTON_RELEASED:
      return vreal(mouse_btn_check((int)N(a,n,1),2));
    case BID_DEVICE_MOUSE_X:{
      double v; gml_input_mouse(&v,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
    case BID_DEVICE_MOUSE_Y:{
      double v; gml_input_mouse(NULL,&v,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
    case BID_DEVICE_MOUSE_X_TO_GUI:{
      double v; gml_input_mouse(NULL,NULL,&v,NULL,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
    case BID_DEVICE_MOUSE_Y_TO_GUI:{
      double v; gml_input_mouse(NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
    case BID_DEVICE_MOUSE_RAW_X:{
      double v; gml_input_mouse(NULL,NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
    case BID_DEVICE_MOUSE_RAW_Y:{
      double v; gml_input_mouse(NULL,NULL,NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL); return vreal(v); }
    case BID_WINDOW_MOUSE_SET:
      return vreal(0);
    case BID_DISPLAY_GET_WIDTH:
    case BID_WINDOW_GET_WIDTH:
      return vreal(presentation_size(vm,R,0));
    case BID_DISPLAY_GET_HEIGHT:
    case BID_WINDOW_GET_HEIGHT:
      return vreal(presentation_size(vm,R,1));
    case BID_DISPLAY_GET_GUI_WIDTH:
      return vreal(vm->gui_w>0? vm->gui_w : ((R&&R->fbw>0)? R->fbw : (vm->win&&vm->win->disp_w? (int)vm->win->disp_w : 288)));
    case BID_DISPLAY_GET_GUI_HEIGHT:
      return vreal(vm->gui_h>0? vm->gui_h : ((R&&R->fbh>0)? R->fbh : (vm->win&&vm->win->disp_h? (int)vm->win->disp_h : 216)));
    case BID_SURFACE_EXISTS:
      return vreal(R?gml_surface_exists(R,(int)N(a,n,0)):0);
    case BID_SURFACE_CREATE:
      return vreal(R?gml_surface_create(R,(int)N(a,n,0),(int)N(a,n,1)):-1);
    case BID_SURFACE_FREE:
      if(R) gml_surface_free(R,(int)N(a,n,0));
      return vreal(0);
    case BID_SURFACE_GET_TEXTURE:{
      int sid=(int)N(a,n,0);
      return vreal((R&&gml_surface_exists(R,sid))?(double)(GML_TEX_SURF_TAG | (sid&0xFFFF)):-1); }
    case BID_SURFACE_GET_WIDTH:
      return vreal(R?gml_surface_width(R,(int)N(a,n,0)):0);
    case BID_SURFACE_GET_HEIGHT:
      return vreal(R?gml_surface_height(R,(int)N(a,n,0)):0);
    case BID_SURFACE_SET_TARGET:
      if(getenv("GML_LOG_SURF")) fprintf(stderr,"[surf] set_target %d\n",(int)N(a,n,0));
      return vreal(R?gml_surface_set_target(R,(int)N(a,n,0)):0);
    case BID_SURFACE_RESET_TARGET:
      if(getenv("GML_LOG_SURF")) fprintf(stderr,"[surf] reset_target\n");
      if(R) gml_surface_reset_target(R);
      return vreal(0);
    case BID_TEXTURE_GET_TEXEL_WIDTH:{
      double tw; return vreal(texture_info(R,(int)N(a,n,0),NULL,NULL,&tw,NULL)?tw:0); }
    case BID_TEXTURE_GET_TEXEL_HEIGHT:{
      double th; return vreal(texture_info(R,(int)N(a,n,0),NULL,NULL,NULL,&th)?th:0); }
    case BID_STRING_WIDTH:
      return vreal(R?gml_text_width(R,S(a,n,0)):(int)strlen(S(a,n,0))*8);
    case BID_STRING_HEIGHT:
      return vreal(R?gml_text_height(R,S(a,n,0)):8);
    default:
      return vreal(0);
  }
}

/* full FMOD extension dispatch, shared by the generic chain and the cached fast path
 * so extension calls use the same ordered dispatch in either path. */
static GmlVal builtin_fmod(GmlVM *vm, const char *nm, GmlVal *a, int n){
      const char *f=nm+5;
      GmlFmodBanks *fb=gml_audio_get_fmod((GmlAudio*)vm->audio);
      if(fb){
        /* real playback path */
        if(!strcmp(f,"event_one_shot_3d")){
          const char *p=fmod_path_arg(a,n);
          if(p){
            int h=gml_fmod_start(fb,p,1,1);
            if(h>0) gml_fmod_set_3d(fb,h,N(a,n,1),N(a,n,2));
          }
          return vreal(0);
        }
        if(!strncmp(f,"event_one_shot",14)){ const char *p=fmod_path_arg(a,n); if(p) gml_fmod_start(fb,p,1,1); return vreal(0); }
        if(!strcmp(f,"event_create_instance")){ const char *p=fmod_path_arg(a,n); return vreal(p?gml_fmod_start(fb,p,0,0):0); }
        if(!strcmp(f,"event_load")) return vreal(1);
        if(!strcmp(f,"event_get_length")){ const char *p=fmod_path_arg(a,n); return vreal(p?gml_fmod_get_length(fb,p)*1000.0:0); }  /* ms */
        if(!strcmp(f,"event_instance_play")){ gml_fmod_play(fb,(int)N(a,n,0)); return vreal(0); }
        if(!strcmp(f,"event_instance_is_playing")) return vreal(gml_fmod_is_playing(fb,(int)N(a,n,0)));
        if(!strcmp(f,"event_instance_get_paused")) return vreal(gml_fmod_get_paused(fb,(int)N(a,n,0)));
        if(!strcmp(f,"event_instance_set_paused")){ gml_fmod_set_paused(fb,(int)N(a,n,0),N(a,n,1)>=0.5); return vreal(0); }
        if(!strcmp(f,"event_instance_set_paused_all")){ gml_fmod_set_paused_all(fb,N(a,n,0)>=0.5); return vreal(0); }
        if(!strcmp(f,"event_instance_stop")){ gml_fmod_stop(fb,(int)N(a,n,0)); return vreal(0); }
        if(!strcmp(f,"event_instance_release")){ gml_fmod_release(fb,(int)N(a,n,0)); return vreal(0); }
        if(!strcmp(f,"event_instance_get_timeline_pos")) return vreal(gml_fmod_get_timeline_pos(fb,(int)N(a,n,0)));
        if(!strcmp(f,"event_instance_set_timeline_pos")){ gml_fmod_set_timeline_pos(fb,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
        if(!strcmp(f,"event_instance_set_parameter")){ gml_fmod_set_param(fb,(int)N(a,n,0),S(a,n,1),N(a,n,2)); return vreal(0); }
        if(!strcmp(f,"event_instance_get_parameter")) return vreal(gml_fmod_get_param(fb,(int)N(a,n,0),S(a,n,1)));
        if(!strcmp(f,"set_parameter")){ gml_fmod_set_global_param(fb,S(a,n,0),N(a,n,1)); return vreal(0); }
        if(!strcmp(f,"get_parameter")) return vreal(gml_fmod_get_global_param(fb,S(a,n,0)));
        if(!strcmp(f,"bank_load")||!strcmp(f,"bank_load_sample_data")||
           !strcmp(f,"init")||!strcmp(f,"studio_init")) return vreal(1);
        if(!strcmp(f,"destroy")){ gml_fmod_stop_all(fb); return vreal(0); }
        if(!strcmp(f,"set_num_listeners")||!strcmp(f,"update")) return vreal(0);
        if(!strcmp(f,"set_listener_attributes")){ gml_fmod_set_listener(fb,N(a,n,1),N(a,n,2)); return vreal(0); }
        if(!strcmp(f,"event_instance_set_3d_attributes")){ gml_fmod_set_3d(fb,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
        if(getenv("GML_DBG_FMOD_CALLS")){ fprintf(stderr,"[fmodcall] %s(",f);
          for(int i=0;i<n;i++){ if(a[i].t==V_STR) fprintf(stderr,"%s\"%s\"",i?",":"",a[i].s?a[i].s:""); else fprintf(stderr,"%s%g",i?",":"",N(a,n,i)); }
          fprintf(stderr,")\n"); }
        return vreal(0);   /* remaining FMOD calls: no-op but harmless */
      }
      /* fallback lifecycle model (no bank set): keep audio-gated logic advancing */
      static struct { int used, playing, paused; struct { char name[40]; double value; } param[16]; int nparam; } fmod_ev[512];
      static struct { char name[40]; double value; } fmod_gp[16]; static int fmod_ngp=0;
      if(!strcmp(f,"event_create_instance")||!strncmp(f,"event_one_shot",14)){
        for(int i=1;i<512;i++) if(!fmod_ev[i].used){ fmod_ev[i].used=1; fmod_ev[i].playing=1; fmod_ev[i].paused=0; return vreal(i); }
        return vreal(0); }
      if(!strcmp(f,"event_instance_play")){ int id=(int)N(a,n,0); if(id>0&&id<512){ fmod_ev[id].used=1; fmod_ev[id].playing=1; fmod_ev[id].paused=0; } return vreal(0); }
      if(!strcmp(f,"event_instance_is_playing")){ int id=(int)N(a,n,0); return vreal((id>0&&id<512&&fmod_ev[id].used&&fmod_ev[id].playing)?1:0); }
      if(!strcmp(f,"event_instance_get_paused")){ int id=(int)N(a,n,0); return vreal((id>0&&id<512&&fmod_ev[id].paused)?1:0); }
      if(!strcmp(f,"event_instance_set_paused")){ int id=(int)N(a,n,0); if(id>0&&id<512) fmod_ev[id].paused=(N(a,n,1)>=0.5); return vreal(0); }
      if(!strcmp(f,"event_instance_stop")||!strcmp(f,"event_instance_release")){ int id=(int)N(a,n,0); if(id>0&&id<512){ fmod_ev[id].playing=0; if(!strcmp(f,"event_instance_release")) fmod_ev[id].used=0; } return vreal(0); }
      if(!strcmp(f,"event_instance_set_parameter")){ int id=(int)N(a,n,0); const char *pn=S(a,n,1);
        if(id>0&&id<512&&pn&&*pn){ int pi=-1; for(int i=0;i<fmod_ev[id].nparam;i++) if(!strcmp(fmod_ev[id].param[i].name,pn)){ pi=i; break; }
          if(pi<0 && fmod_ev[id].nparam<16){ pi=fmod_ev[id].nparam++; snprintf(fmod_ev[id].param[pi].name,sizeof(fmod_ev[id].param[pi].name),"%s",pn); }
          if(pi>=0) fmod_ev[id].param[pi].value=N(a,n,2); } return vreal(0); }
      if(!strcmp(f,"event_instance_get_parameter")){ int id=(int)N(a,n,0); const char *pn=S(a,n,1);
        if(id>0&&id<512&&pn&&*pn) for(int i=0;i<fmod_ev[id].nparam;i++) if(!strcmp(fmod_ev[id].param[i].name,pn)) return vreal(fmod_ev[id].param[i].value);
        return vreal(0); }
      if(!strcmp(f,"set_parameter")){ const char *pn=S(a,n,0);
        if(pn&&*pn){ int pi=-1; for(int i=0;i<fmod_ngp;i++) if(!strcmp(fmod_gp[i].name,pn)){ pi=i; break; }
          if(pi<0 && fmod_ngp<16){ pi=fmod_ngp++; snprintf(fmod_gp[pi].name,sizeof(fmod_gp[pi].name),"%s",pn); }
          if(pi>=0) fmod_gp[pi].value=N(a,n,1); } return vreal(0); }
      if(!strcmp(f,"get_parameter")){ const char *pn=S(a,n,0);
        if(pn&&*pn) for(int i=0;i<fmod_ngp;i++) if(!strcmp(fmod_gp[i].name,pn)) return vreal(fmod_gp[i].value);
        return vreal(0); }
      if(!strcmp(f,"bank_load")||!strcmp(f,"bank_load_sample_data")||!strcmp(f,"init")||!strcmp(f,"studio_init")) return vreal(1);
      if(!strcmp(f,"destroy")){ memset(fmod_ev,0,sizeof(fmod_ev)); fmod_ngp=0; return vreal(0); }
      if(!strcmp(f,"set_num_listeners")||!strcmp(f,"set_listener_attributes")||
         !strcmp(f,"event_instance_set_3d_attributes")||!strcmp(f,"update")) return vreal(0);
      return vreal(0);
}
static int builtin_fmod_exact_name(const char *nm){
  static const char *const names[]={
    "fmod_bank_load",
    "fmod_bank_load_sample_data",
    "fmod_destroy",
    "fmod_event_create_instance",
    "fmod_event_get_length",
    "fmod_event_instance_get_parameter",
    "fmod_event_instance_get_paused",
    "fmod_event_instance_get_timeline_pos",
    "fmod_event_instance_is_playing",
    "fmod_event_instance_play",
    "fmod_event_instance_release",
    "fmod_event_instance_set_3d_attributes",
    "fmod_event_instance_set_parameter",
    "fmod_event_instance_set_paused",
    "fmod_event_instance_set_paused_all",
    "fmod_event_instance_set_timeline_pos",
    "fmod_event_instance_stop",
    "fmod_event_load",
    "fmod_event_one_shot",
    "fmod_event_one_shot_3d",
    "fmod_get_parameter",
    "fmod_init",
    "fmod_set_listener_attributes",
    "fmod_set_num_listeners",
    "fmod_set_parameter",
    "fmod_studio_init",
    "fmod_update",
  };
  for(size_t i=0;i<sizeof(names)/sizeof(names[0]);i++) if(!strcmp(nm,names[i])) return 1;
  return 0;
}

static GmlVal builtin_call_impl(GmlVM *vm, const char *nm, GmlVal *a, int n);
GmlVal gml_builtin_call(GmlVM *vm, const char *nm, GmlVal *a, int n){
  if(!hotprof_enabled()) return builtin_call_impl(vm,nm,a,n);
  double t0=hotprof_now();
  GmlVal v=builtin_call_impl(vm,nm,a,n);
  hotprof_add(nm,(hotprof_now()-t0)*1000.0);
  return v;
}
static GmlVal builtin_call_impl(GmlVM *vm, const char *nm, GmlVal *a, int n){
  { GmlVal v;
    if(fast_hot_builtin(vm,nm,a,n,&v)) return v; }
  /* Return zero for prefixed script names whose suffix is sleep; leave the unprefixed builtin untouched. */
  { const char *sb=NULL;
    if(!strncmp(nm,"gml_GlobalScript_",17)) sb=nm+17;
    else if(!strncmp(nm,"gml_Script_",11)) sb=nm+11;
    if(sb && !strcmp(sb,"sleep")) return vreal(0); }
  /* Handle tile_layer_find and tile_delete through runtime layer/element storage.
   * Search in slot order with half-open bounds and account for negative scale. */
  { const char *bare = strncmp(nm,"gml_Script_",11)? nm : nm+11;
    if(bare[0]=='t' && vm->n_rte>0){
      if(!strcmp(bare,"tile_layer_find")){
        double dep=N(a,n,0), px=N(a,n,1), py=N(a,n,2);
        for(int i=0;i<vm->n_rtl;i++){ GmlRtLayer *l=&vm->rtl[i];
          if(!l->used || l->depth!=dep) continue;
          for(int j=0;j<vm->n_rte;j++){ GmlRtElem *e=&vm->rte[j];
            if(!e->used || e->layer!=l->id || e->type!=7) continue;
            double xs=e->xs, ys=e->ys;
            if(xs>=0 && ys>=0){
              if(px<e->x || py<e->y) continue;
              if(px>=e->x+xs*e->w || py>=e->y+ys*e->h) continue;
              return vreal(e->id);
            } else {
              double minx=e->x, maxx=e->x+xs*e->w;
              if(minx>maxx){ double t=minx; minx=maxx; maxx=t; }
              if(px<minx || px>=maxx) continue;
              double miny=e->y, maxy=e->y+ys*e->h;
              if(miny>maxy){ double t=miny; miny=maxy; maxy=t; }
              if(py<miny || py>=maxy) continue;
              return vreal(e->id);
            }
          }
        }
        return vreal(-1);
      }
      if(!strcmp(bare,"tile_delete")){
        GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->used=0; return vreal(0);
      }
    } }
  /* ---- collision ---- */
  if(!strcmp(nm,"place_meeting")){ int r=collision_at(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2),0);
    if(log_col_on()){ GmlInstance*cs=vm->cur_self; const char*cn=(cs&&cs->obj>=0&&cs->obj<vm->n_objects)?vm->objects[cs->obj].name:"?";
      const char*tn=((int)N(a,n,2)>=0&&(int)N(a,n,2)<vm->n_objects)?vm->objects[(int)N(a,n,2)].name:"?";
      if(log_col_match(cn)) fprintf(stderr,"[col] %s place_meeting(%.0f,%.0f,%s)=%d\n",cn,N(a,n,0),N(a,n,1),tn,r); }
    return vreal(r); }
  if(!strcmp(nm,"place_free")){ int r=!collision_at(vm,N(a,n,0),N(a,n,1),0,1);
    if(log_col_on()){ GmlInstance*cs=vm->cur_self; const char*cn=(cs&&cs->obj>=0&&cs->obj<vm->n_objects)?vm->objects[cs->obj].name:"?";
      if(log_col_match(cn)) fprintf(stderr,"[col] %s place_free(%.0f,%.0f)=%d\n",cn,N(a,n,0),N(a,n,1),r); }
    return vreal(r); }
  if(!strcmp(nm,"place_empty"))   return vreal(!collision_at(vm,N(a,n,0),N(a,n,1),IT_ALL,0));
  if(!strcmp(nm,"collision_point")){ double p[4]={N(a,n,0),N(a,n,1),0,0}; GmlInstance *o=collision_shape(vm,0,p,(int)N(a,n,2),(int)N(a,n,4)); return vreal(o?(double)o->id:-4); }
  /* position_meeting(x,y,obj): is the point (x,y) inside any instance of obj? (bool; checks all). */
  if(!strcmp(nm,"position_meeting")){ double p[4]={N(a,n,0),N(a,n,1),0,0}; return vreal(collision_shape(vm,0,p,(int)N(a,n,2),0)!=NULL); }
  if(!strcmp(nm,"collision_rectangle")){ double p[4]={N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3)}; GmlInstance *o=collision_shape(vm,1,p,(int)N(a,n,4),(int)N(a,n,6)); return vreal(o?(double)o->id:-4); }
  if(!strcmp(nm,"rectangle_in_rectangle")){
    double ax1=N(a,n,0), ay1=N(a,n,1), ax2=N(a,n,2), ay2=N(a,n,3);
    double bx1=N(a,n,4), by1=N(a,n,5), bx2=N(a,n,6), by2=N(a,n,7);
    if(ax1>ax2){ double t=ax1; ax1=ax2; ax2=t; }
    if(ay1>ay2){ double t=ay1; ay1=ay2; ay2=t; }
    if(bx1>bx2){ double t=bx1; bx1=bx2; bx2=t; }
    if(by1>by2){ double t=by1; by1=by2; by2=t; }
    return vreal(!(ax2<bx1 || bx2<ax1 || ay2<by1 || by2<ay1));
  }
  if(!strcmp(nm,"collision_circle")){ double p[3]={N(a,n,0),N(a,n,1),N(a,n,2)}; GmlInstance *o=collision_shape(vm,2,p,(int)N(a,n,3),(int)N(a,n,5)); return vreal(o?(double)o->id:-4); }
  if(!strcmp(nm,"move_contact_solid")||!strcmp(nm,"move_contact")){ GmlInstance *s=vm->cur_self; if(!s) return vreal(0);
    double dir=N(a,n,0), md=N(a,n,1); if(md<0) md=1000;
    double dx=cos(dir*M_PI/180.0), dy=-sin(dir*M_PI/180.0);
    if(collision_at(vm,s->x,s->y,0,1)){
      if(resolve_landing_overlap(vm,s,(int)md)) return vreal(0);
      for(int k=0;k<(int)md;k++){ if(!collision_at(vm,s->x,s->y,0,1)) break; s->x-=dx; s->y-=dy; }
      gml_colgrid_touch(s);
      snap_contact_axis(vm,s,dx,dy);
      return vreal(0);
    }
    for(int k=0;k<(int)md;k++){ if(collision_at(vm,s->x+dx,s->y+dy,0,1)) break; s->x+=dx; s->y+=dy; }
    gml_colgrid_touch(s);
    return vreal(0); }
  /* ---- math ---- */
  if(!strcmp(nm,"floor")) return vreal(floor(N(a,n,0)));
  if(!strcmp(nm,"ceil"))  return vreal(ceil(N(a,n,0)));
  if(!strcmp(nm,"round")) return vreal(gm_round(N(a,n,0)));
  if(!strcmp(nm,"sign"))  return vreal(gm_sign(N(a,n,0)));
  if(!strcmp(nm,"abs"))   return vreal(fabs(N(a,n,0)));
  if(!strcmp(nm,"sqrt"))  return vreal(sqrt(N(a,n,0)));
  if(!strcmp(nm,"sqr"))   { double x=N(a,n,0); return vreal(x*x); }
  if(!strcmp(nm,"sin"))   return vreal(sin(N(a,n,0)));
  if(!strcmp(nm,"cos"))   return vreal(cos(N(a,n,0)));
  if(!strcmp(nm,"tan"))   return vreal(tan(N(a,n,0)));
  if(!strcmp(nm,"log10")) return vreal(log10(N(a,n,0)));
  if(!strcmp(nm,"logn"))  return vreal(log(N(a,n,1))/log(N(a,n,0)));
  if(!strcmp(nm,"dsin"))  return vreal(sin(N(a,n,0)*M_PI/180.0));   /* degree trig (GM classics) */
  if(!strcmp(nm,"dcos"))  return vreal(cos(N(a,n,0)*M_PI/180.0));
  if(!strcmp(nm,"dtan"))  return vreal(tan(N(a,n,0)*M_PI/180.0));
  if(!strcmp(nm,"darcsin")) return vreal(asin(N(a,n,0))*180.0/M_PI);
  if(!strcmp(nm,"darccos")) return vreal(acos(N(a,n,0))*180.0/M_PI);
  if(!strcmp(nm,"darctan")) return vreal(atan(N(a,n,0))*180.0/M_PI);
  if(!strcmp(nm,"darctan2")) return vreal(atan2(N(a,n,0),N(a,n,1))*180.0/M_PI);
  if(!strcmp(nm,"random")) return vreal(gml_rng_value(vm) * N(a,n,0));  /* GM WELL512: (next/2^32)*x */
  if(!strcmp(nm,"randomize")){
    /* GML_RANDOMIZE_SEED optionally fixes the seed; otherwise derive it from the clock and VM address. */
    static int fixed_init=0; static long fixed=-1;
    if(!fixed_init){ const char *e=getenv("GML_RANDOMIZE_SEED"); fixed=e?atol(e):-1; fixed_init=1; }
    uint32_t s = fixed>=0 ? (uint32_t)fixed : ((uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)vm);
    vm->rng_state=s; gml_rng_seed(vm,s); return vreal(0); }
  if(!strcmp(nm,"random_set_seed")){ uint32_t s=(uint32_t)(int64_t)N(a,n,0); vm->rng_state=s; gml_rng_seed(vm,s); return vreal(0); }
  if(!strcmp(nm,"random_get_seed"))
    return vreal((double)((vm->win&&vm->win->classic_version)?vm->rng_classic_state:vm->rng_state));
  if(!strncmp(nm,"http_",5)&&(!strcmp(nm,"http_get")||!strcmp(nm,"http_get_file")||!strcmp(nm,"http_post_string")||!strcmp(nm,"http_request")))
    return builtin_http_request_stub(vm);
  if(!strcmp(nm,"random_range")){ double a0=N(a,n,0), a1=N(a,n,1); return vreal(a0 + gml_rng_value(vm)*(a1-a0)); }
  if(!strcmp(nm,"irandom")){ int mx=(int)floor(N(a,n,0)); return vreal(mx<=0?0:(int)floor(gml_rng_value(vm)*(mx+1))); }
  if(!strcmp(nm,"irandom_range")){ int lo=(int)floor(N(a,n,0)), hi=(int)floor(N(a,n,1));
    if(hi<lo){ int t=lo; lo=hi; hi=t; } return vreal(lo + (int)floor(gml_rng_value(vm)*(hi-lo+1))); }
  if(!strcmp(nm,"choose")){ if(n<=0) return vreal(0); int i=(int)floor(gml_rng_value(vm)*n); if(i<0)i=0; if(i>=n)i=n-1; return a[i]; }
  if(!strcmp(nm,"clamp")){ double x=N(a,n,0), lo=N(a,n,1), hi=N(a,n,2); if(x<lo)x=lo; if(x>hi)x=hi; return vreal(x); }
  if(!strcmp(nm,"lerp")){ double a0=N(a,n,0), a1=N(a,n,1), t=N(a,n,2); return vreal(a0 + (a1-a0)*t); }
  if(!strcmp(nm,"mean")){ double s=0; for(int i=0;i<n;i++) s+=N(a,n,i); return vreal(n? s/n : 0); }
  if(!strcmp(nm,"min")){ if(n<=0) return vreal(0); double v=N(a,n,0); for(int i=1;i<n;i++){ double x=N(a,n,i); if(x<v) v=x; } return vreal(v); }
  if(!strcmp(nm,"max")){ if(n<=0) return vreal(0); double v=N(a,n,0); for(int i=1;i<n;i++){ double x=N(a,n,i); if(x>v) v=x; } return vreal(v); }
  if(!strcmp(nm,"power")) return vreal(pow(N(a,n,0),N(a,n,1)));
  if(!strcmp(nm,"real")) return vreal(N(a,n,0));
  if(!strcmp(nm,"bool")){  /* GMS2.3: numeric -> 0/1; the strings "true"/"1" -> 1 */
    if(n>=1 && a[0].t==V_STR && a[0].s) return vreal(!strcmp(a[0].s,"true")||!strcmp(a[0].s,"1"));
    return vreal(N(a,n,0)!=0); }
  if(!strcmp(nm,"degtorad")) return vreal(N(a,n,0)*M_PI/180.0);
  if(!strcmp(nm,"median")){   /* middle value; even count -> lower middle (games: median(lo,v,hi) clamp) */
    double v[16]; int m=n>16?16:n;
    if(m<=0) return vreal(0);
    for(int i=0;i<m;i++) v[i]=N(a,n,i);
    for(int i=0;i<m;i++) for(int j=i+1;j<m;j++) if(v[j]<v[i]){ double t=v[i]; v[i]=v[j]; v[j]=t; }
    return vreal(v[(m-1)/2]); }
  if(!strcmp(nm,"mean")){ double t=0; for(int i=0;i<n;i++) t+=N(a,n,i); return vreal(n>0?t/n:0); }
  if(!strcmp(nm,"darctan2")) return vreal(atan2(N(a,n,0),N(a,n,1))*180.0/M_PI);
  if(!strcmp(nm,"arctan"))  return vreal(atan(N(a,n,0)));    /* radians */
  if(!strcmp(nm,"arcsin"))  return vreal(asin(N(a,n,0)));
  if(!strcmp(nm,"arccos"))  return vreal(acos(N(a,n,0)));
  if(!strcmp(nm,"arctan2")) return vreal(atan2(N(a,n,0),N(a,n,1)));
  if(!strcmp(nm,"radtodeg")) return vreal(N(a,n,0)*180.0/M_PI);
  if(!strcmp(nm,"degtorad")) return vreal(N(a,n,0)*M_PI/180.0);
  if(!strcmp(nm,"make_color_rgb"))
    return vreal((double)((int)N(a,n,0) + ((int)N(a,n,1)<<8) + ((int)N(a,n,2)<<16)));
  if(!strcmp(nm,"make_colour_rgb"))
    return vreal((double)((int)N(a,n,0) + ((int)N(a,n,1)<<8) + ((int)N(a,n,2)<<16)));
  if(!strcmp(nm,"make_color_hsv")||!strcmp(nm,"make_colour_hsv")){
    double h=fmod(N(a,n,0),255.0); if(h<0) h+=255.0; double s=N(a,n,1)/255.0, v=N(a,n,2)/255.0;
    double c=v*s, hp=h/42.5, x=c*(1-fabs(fmod(hp,2)-1)), m=v-c, r=0,g=0,b=0;
    if(hp<1){ r=c; g=x; } else if(hp<2){ r=x; g=c; } else if(hp<3){ g=c; b=x; }
    else if(hp<4){ g=x; b=c; } else if(hp<5){ r=x; b=c; } else { r=c; b=x; }
    return vreal((int)((r+m)*255) + ((int)((g+m)*255)<<8) + ((int)((b+m)*255)<<16));
  }
  if(!strcmp(nm,"merge_color")||!strcmp(nm,"merge_colour")){
    uint32_t c1=(uint32_t)N(a,n,0), c2=(uint32_t)N(a,n,1); double t=N(a,n,2); if(t<0)t=0; if(t>1)t=1;
    int r=(int)((c1&255)*(1-t)+(c2&255)*t), g=(int)(((c1>>8)&255)*(1-t)+((c2>>8)&255)*t), b=(int)(((c1>>16)&255)*(1-t)+((c2>>16)&255)*t);
    return vreal(r+(g<<8)+(b<<16));
  }
  if(!strcmp(nm,"point_direction")){ double dx=N(a,n,2)-N(a,n,0), dy=N(a,n,3)-N(a,n,1);
    double r=atan2(-dy,dx)*180.0/M_PI; if(r<0)r+=360; return vreal(r); }
  if(!strcmp(nm,"distance_to_point")){ GmlInstance*s=vm->cur_self; if(!s)return vreal(0);
    return vreal(hypot(N(a,n,0)-s->x,N(a,n,1)-s->y)); }
  if(!strcmp(nm,"point_distance")) return vreal(hypot(N(a,n,2)-N(a,n,0),N(a,n,3)-N(a,n,1)));
  /* Standard-GML pure predicates/getters require explicit dispatch rather than
   * the silent catch-all -> 0. GM colours are r + (g<<8) + (b<<16); HSV is 0-255 (see
   * make_color_hsv above: 6 sectors -> *42.5). */
  if(!strcmp(nm,"color_get_red")||!strcmp(nm,"colour_get_red")) return vreal((int)N(a,n,0)&255);
  if(!strcmp(nm,"color_get_green")||!strcmp(nm,"colour_get_green")) return vreal(((int)N(a,n,0)>>8)&255);
  if(!strcmp(nm,"color_get_blue")||!strcmp(nm,"colour_get_blue")) return vreal(((int)N(a,n,0)>>16)&255);
  if(!strcmp(nm,"color_get_hue")||!strcmp(nm,"colour_get_hue")||
     !strcmp(nm,"color_get_saturation")||!strcmp(nm,"colour_get_saturation")||
     !strcmp(nm,"color_get_value")||!strcmp(nm,"colour_get_value")){
    int col=(int)N(a,n,0); double r=(col&255)/255.0,g=((col>>8)&255)/255.0,b=((col>>16)&255)/255.0;
    double mx=r>g?(r>b?r:b):(g>b?g:b), mn=r<g?(r<b?r:b):(g<b?g:b), dl=mx-mn;
    if(strstr(nm,"value")) return vreal((int)(mx*255+0.5));
    if(strstr(nm,"saturation")) return vreal(mx<=0?0:(int)(dl/mx*255+0.5));
    double h=0; if(dl>0){ if(mx==r) h=fmod((g-b)/dl,6.0); else if(mx==g) h=(b-r)/dl+2.0; else h=(r-g)/dl+4.0; if(h<0)h+=6.0; }
    return vreal((int)(h*42.5+0.5)%255);   /* hue 0-255 */
  }
  if(!strcmp(nm,"point_in_rectangle")){ double px=N(a,n,0),py=N(a,n,1),x1=N(a,n,2),y1=N(a,n,3),x2=N(a,n,4),y2=N(a,n,5);
    return vreal((px>=x1&&py>=y1&&px<=x2&&py<=y2)?1:0); }
  if(!strcmp(nm,"point_in_circle")){ double px=N(a,n,0),py=N(a,n,1),cx=N(a,n,2),cy=N(a,n,3),rr=N(a,n,4);
    double dx=px-cx,dy=py-cy; return vreal((dx*dx+dy*dy)<=rr*rr?1:0); }
  if(!strcmp(nm,"point_in_triangle")){ double px=N(a,n,0),py=N(a,n,1),x1=N(a,n,2),y1=N(a,n,3),x2=N(a,n,4),y2=N(a,n,5),x3=N(a,n,6),y3=N(a,n,7);
    double d1=(px-x2)*(y1-y2)-(x1-x2)*(py-y2), d2=(px-x3)*(y2-y3)-(x2-x3)*(py-y3), d3=(px-x1)*(y3-y1)-(x3-x1)*(py-y1);
    int hasNeg=(d1<0)||(d2<0)||(d3<0), hasPos=(d1>0)||(d2>0)||(d3>0); return vreal(!(hasNeg&&hasPos)?1:0); }
  if(!strcmp(nm,"rectangle_in_triangle")){
    double rx1=N(a,n,0),ry1=N(a,n,1),rx2=N(a,n,2),ry2=N(a,n,3);
    double x1=N(a,n,4),y1=N(a,n,5),x2=N(a,n,6),y2=N(a,n,7),x3=N(a,n,8),y3=N(a,n,9);
    if(rx1>rx2){ double t=rx1; rx1=rx2; rx2=t; } if(ry1>ry2){ double t=ry1; ry1=ry2; ry2=t; }
    double px[4]={rx1,rx2,rx1,rx2}, py[4]={ry1,ry1,ry2,ry2};
    for(int i=0;i<4;i++){
      double d1=(px[i]-x2)*(y1-y2)-(x1-x2)*(py[i]-y2), d2=(px[i]-x3)*(y2-y3)-(x2-x3)*(py[i]-y3);
      double d3=(px[i]-x1)*(y3-y1)-(x3-x1)*(py[i]-y1);
      int hasNeg=(d1<0)||(d2<0)||(d3<0), hasPos=(d1>0)||(d2>0)||(d3>0);
      if(!(hasNeg&&hasPos)) return vreal(1);
    }
    return vreal(point_in_poly(x1,y1,px,py,4)||point_in_poly(x2,y2,px,py,4)||point_in_poly(x3,y3,px,py,4));
  }
  if(!strcmp(nm,"angle_difference")){ double d=fmod(N(a,n,0)-N(a,n,1),360.0); if(d<-180.0)d+=360.0; if(d>180.0)d-=360.0; return vreal(d); }
  if(!strcmp(nm,"frac")) { double v=N(a,n,0); return vreal(v-floor(v)); }   /* fractional part */
  if(!strcmp(nm,"array_length_1d")) return vreal(n>0?gml_val_array_length(a[0]):0);
  /* Return the top-level array length, including the row count for a two-dimensional array. */
  if(!strcmp(nm,"array_length")) return vreal(n>0?gml_val_array_length(a[0]):0);
  /* GMS2.3 array-function forms. array_create(size,[val]); get/set(arr,i[,v]); push/pop(arr[,v]);
   * resize(arr,size); copy(dst,di,src,si,count). Operate on the array VALUE (a reference), so set/
   * push/resize mutate the caller's array in place — the same object arr[i] syntax touches. */
  if(!strcmp(nm,"array_create")){ int sz=n>0?(int)N(a,n,0):0; GmlVal fill=n>1?a[1]:vreal(0); return gml_arr_new(sz,fill); }
  if(!strcmp(nm,"array_get")){ return n>1?gml_arr_get(a[0],(int)N(a,n,1)):vreal(0); }
  if(!strcmp(nm,"array_set")){ if(n>2) gml_arr_set(a[0],(int)N(a,n,1),a[2]); return vreal(0); }
  if(!strcmp(nm,"array_push")){ if(n>1){ for(int i=1;i<n;i++) gml_arr_push(a[0],a[i]); } return vreal(0); }
  if(!strcmp(nm,"array_pop")){ return n>0?gml_arr_pop(a[0]):vreal(0); }
  if(!strcmp(nm,"array_resize")){ if(n>1) gml_arr_resize(a[0],(int)N(a,n,1)); return vreal(0); }
  if(!strcmp(nm,"array_copy")){ if(n>4) gml_arr_copy(a[0],(int)N(a,n,1),a[2],(int)N(a,n,3),(int)N(a,n,4)); return vreal(0); }
  if(!strcmp(nm,"array_sort")){ if(n>0) gml_array_sort(a[0], n<2 || N(a,n,1)!=0); return vreal(0); }
  /* array_delete(arr,index,number): remove `number` elements at `index`, shifting the tail down
   * (GMS2.3). Negative number deletes that many BEFORE index. Was a silent no-op. */
  if(!strcmp(nm,"array_delete")){
    if(n>2 && a[0].t==V_ARR && a[0].arr){ GmlArr *A=a[0].arr; int idx=(int)N(a,n,1), cnt=(int)N(a,n,2);
      if(cnt<0){ idx+=cnt; cnt=-cnt; }            /* delete before index */
      if(idx<0){ cnt+=idx; idx=0; }
      if(cnt>0 && idx<A->len){ if(idx+cnt>A->len) cnt=A->len-idx;
        for(int i=idx; i+cnt<A->len; i++) A->data[i]=A->data[i+cnt];
        A->len-=cnt; } }
    return vreal(0);
  }
  if(!strcmp(nm,"array_height_2d")) return vreal(n>0?gml_val_array_height_2d(a[0]):0);
  if(!strcmp(nm,"array_length_2d")){
    if(n<=0) return vreal(0);
    return vreal(gml_val_array_length_2d(a[0],(int)N(a,n,1)));
  }
  if(!strcmp(nm,"lengthdir_x")) return vreal(N(a,n,0)*cos(N(a,n,1)*M_PI/180.0));
  if(!strcmp(nm,"lengthdir_y")) return vreal(-N(a,n,0)*sin(N(a,n,1)*M_PI/180.0));  /* GM y down */
  if(!strcmp(nm,"distance_to_object")){ GmlInstance*s=vm->cur_self; if(!s) return vreal(0);
    int obj=(int)N(a,n,0); double best=1e18;
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(o==s || !target_matches_instance(vm,s,o,obj)) continue;
      double d=hypot(o->x-s->x,o->y-s->y); if(d<best) best=d; }
    return vreal(best>1e17?-1:best); }
  /* move_towards_point(x,y,sp): head toward (x,y) at speed sp (sets direction+speed → hspeed/vspeed). */
  if(!strcmp(nm,"move_towards_point")){ GmlInstance*s=vm->cur_self; if(s){
      double dir=atan2(-(N(a,n,1)-s->y),N(a,n,0)-s->x)*180.0/M_PI; double sp=N(a,n,2);
      s->direction=dir; s->speed=sp; s->hspeed=sp*cos(dir*M_PI/180.0); s->vspeed=-sp*sin(dir*M_PI/180.0); }
    return vreal(0); }
  if(!strcmp(nm,"motion_set")){ GmlInstance*s=vm->cur_self; if(s){
      s->direction=N(a,n,0); s->speed=N(a,n,1);
      s->hspeed=s->speed*cos(s->direction*M_PI/180.0);
      s->vspeed=-s->speed*sin(s->direction*M_PI/180.0); }
    return vreal(0); }
  if(!strcmp(nm,"motion_add")){ GmlInstance*s=vm->cur_self; if(s){
      double dir=N(a,n,0)*M_PI/180.0, amount=N(a,n,1);
      s->hspeed+=amount*cos(dir); s->vspeed-=amount*sin(dir);
      motion_from_components(s); }
    return vreal(0); }
  /* ---- mp_grid: GM motion-planning grids (A*). Transient AI aids (games rebuild them per
   * room/level), kept in a static pool like particles — not serialized into save-states. ---- */
  if(!strcmp(nm,"mp_grid_create")||!strcmp(nm,"mp_grid_destroy")||
     !strcmp(nm,"mp_grid_path")||!strcmp(nm,"mp_grid_add_instances")||
     !strncmp(nm,"mp_grid_",8)){
    typedef struct { int live; double left, top; int hc, vc, cw, ch; uint8_t *cell; } MpGrid;
    static MpGrid mp[8];
    const char *sub=nm+8;
    if(!strcmp(sub,"create")){
      int hc=(int)N(a,n,2), vc=(int)N(a,n,3);
      if(hc<1||vc<1||(long)hc*vc>1<<22) return vreal(-1);
      for(int i=0;i<8;i++) if(!mp[i].live){
        mp[i].live=1; mp[i].left=N(a,n,0); mp[i].top=N(a,n,1);
        mp[i].hc=hc; mp[i].vc=vc; mp[i].cw=(int)N(a,n,4); mp[i].ch=(int)N(a,n,5);
        if(mp[i].cw<1) mp[i].cw=1; if(mp[i].ch<1) mp[i].ch=1;
        free(mp[i].cell); mp[i].cell=calloc((size_t)hc*vc,1);
        return vreal(i);
      }
      return vreal(-1);
    }
    int gi=(int)N(a,n,0);
    MpGrid *g = (gi>=0 && gi<8 && mp[gi].live) ? &mp[gi] : NULL;
    if(!g) return vreal(0);
    if(!strcmp(sub,"destroy")){ free(g->cell); memset(g,0,sizeof *g); return vreal(0); }
    if(!strcmp(sub,"clear_all")){ memset(g->cell,0,(size_t)g->hc*g->vc); return vreal(0); }
    if(!strcmp(sub,"add_cell")||!strcmp(sub,"clear_cell")){
      int h=(int)N(a,n,1), v=(int)N(a,n,2);
      if(h>=0&&h<g->hc&&v>=0&&v<g->vc) g->cell[v*g->hc+h]=(sub[0]=='a');
      return vreal(0); }
    if(!strcmp(sub,"get_cell")){ int h=(int)N(a,n,1), v=(int)N(a,n,2);
      return vreal((h>=0&&h<g->hc&&v>=0&&v<g->vc)? -(double)g->cell[v*g->hc+h] : -1); }
    if(!strcmp(sub,"add_rectangle")||!strcmp(sub,"clear_rectangle")){
      int h0=(int)floor((N(a,n,1)-g->left)/g->cw), v0=(int)floor((N(a,n,2)-g->top)/g->ch);
      int h1=(int)floor((N(a,n,3)-g->left)/g->cw), v1=(int)floor((N(a,n,4)-g->top)/g->ch);
      if(h0>h1){int t=h0;h0=h1;h1=t;} if(v0>v1){int t=v0;v0=v1;v1=t;}
      uint8_t val = (sub[0]=='a');
      for(int v=v0;v<=v1;v++) for(int h=h0;h<=h1;h++)
        if(h>=0&&h<g->hc&&v>=0&&v<g->vc) g->cell[v*g->hc+h]=val;
      return vreal(0); }
    if(!strcmp(sub,"add_instances")){
      int obj=(int)N(a,n,1);
      for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
        if(!o->active||o->marked||o->deactivated) continue;
        if(!target_matches_instance(vm,vm->cur_self,o,obj)) continue;
        double l,t,r,b; if(!inst_bbox(vm,o,o->x,o->y,&l,&t,&r,&b)) continue;
        int h0=(int)floor((l-g->left)/g->cw), v0=(int)floor((t-g->top)/g->ch);
        int h1=(int)floor((r-g->left)/g->cw), v1=(int)floor((b-g->top)/g->ch);
        for(int v=v0;v<=v1;v++) for(int h=h0;h<=h1;h++)
          if(h>=0&&h<g->hc&&v>=0&&v<g->vc) g->cell[v*g->hc+h]=1;
      }
      return vreal(0); }
    if(!strcmp(sub,"path")){
      /* mp_grid_path(grid, path, x0, y0, x1, y1, allowdiag) -> bool; fills the path with a route */
      int pi=(int)N(a,n,1), diag=(int)N(a,n,6)!=0;
      double x0=N(a,n,2), y0=N(a,n,3), x1=N(a,n,4), y1=N(a,n,5);
      if(pi<0||pi>=vm->n_paths) return vreal(0);
      int sh=(int)floor((x0-g->left)/g->cw), sv=(int)floor((y0-g->top)/g->ch);
      int gh=(int)floor((x1-g->left)/g->cw), gv=(int)floor((y1-g->top)/g->ch);
      GmlPath *p=&vm->paths[pi];
      if(sh<0||sh>=g->hc||sv<0||sv>=g->vc||gh<0||gh>=g->hc||gv<0||gv>=g->vc) return vreal(0);
      if(g->cell[sv*g->hc+sh]||g->cell[gv*g->hc+gh]) return vreal(0);
      int nc=g->hc*g->vc;
      int *prev=malloc((size_t)nc*sizeof(int));
      int *cost=malloc((size_t)nc*sizeof(int));
      int *heap=malloc((size_t)(nc+1)*sizeof(int));   /* binary heap of cell ids keyed by f */
      int *fsco=malloc((size_t)nc*sizeof(int));
      if(!prev||!cost||!heap||!fsco){ free(prev);free(cost);free(heap);free(fsco); return vreal(0); }
      for(int i=0;i<nc;i++){ prev[i]=-1; cost[i]=INT_MAX; }
      int start=sv*g->hc+sh, goal=gv*g->hc+gh, hn=0;
      #define MPH(c) (diag ? 10*((abs((c)%g->hc-gh)>abs((c)/g->hc-gv))?abs((c)%g->hc-gh):abs((c)/g->hc-gv)) \
                           : 10*(abs((c)%g->hc-gh)+abs((c)/g->hc-gv)))
      cost[start]=0; fsco[start]=MPH(start); heap[++hn]=start;
      int found=0;
      while(hn>0){
        int cur=heap[1];                             /* pop-min */
        heap[1]=heap[hn--];
        for(int i2=1;;){ int lch=2*i2, rch=lch+1, sm=i2;
          if(lch<=hn && fsco[heap[lch]]<fsco[heap[sm]]) sm=lch;
          if(rch<=hn && fsco[heap[rch]]<fsco[heap[sm]]) sm=rch;
          if(sm==i2) break; int t=heap[i2]; heap[i2]=heap[sm]; heap[sm]=t; i2=sm; }
        if(cur==goal){ found=1; break; }
        int ch2=cur%g->hc, cv2=cur/g->hc;
        static const int DX[8]={1,-1,0,0,1,1,-1,-1}, DY[8]={0,0,1,-1,1,-1,1,-1};
        int ndirs = diag?8:4;
        for(int k=0;k<ndirs;k++){
          int nh=ch2+DX[k], nv2=cv2+DY[k];
          if(nh<0||nh>=g->hc||nv2<0||nv2>=g->vc) continue;
          if(g->cell[nv2*g->hc+nh]) continue;
          if(k>=4 && (g->cell[cv2*g->hc+nh]||g->cell[nv2*g->hc+ch2])) continue;  /* no corner cutting */
          int nb=nv2*g->hc+nh, w=(k>=4)?14:10;
          if(cost[cur]+w < cost[nb]){
            cost[nb]=cost[cur]+w; prev[nb]=cur; fsco[nb]=cost[nb]+MPH(nb);
            heap[++hn]=nb;                            /* push (dupes ok: worse copies skipped) */
            for(int i2=hn; i2>1 && fsco[heap[i2]]<fsco[heap[i2/2]]; i2/=2){
              int t=heap[i2]; heap[i2]=heap[i2/2]; heap[i2/2]=t; }
          }
        }
      }
      #undef MPH
      if(found){
        int rn=0; for(int c=goal;c>=0;c=prev[c]) rn++;
        GmlPathPt *pts=calloc((size_t)rn>0?(size_t)rn:1,sizeof(GmlPathPt));
        int idx=rn-1;
        for(int c=goal;c>=0;c=prev[c],idx--){
          pts[idx].x=g->left+(c%g->hc+0.5)*g->cw;
          pts[idx].y=g->top+(c/g->hc+0.5)*g->ch;
          pts[idx].sp=100;
        }
        pts[0].x=x0; pts[0].y=y0;                     /* exact endpoints, GM-style */
        pts[rn-1].x=x1; pts[rn-1].y=y1;
        free(p->pts); p->pts=pts; p->n=rn; p->kind=0; p->closed=0;
        double L=0; p->pts[0].clen=0;
        for(int k=1;k<p->n;k++){ double ddx=p->pts[k].x-p->pts[k-1].x, ddy=p->pts[k].y-p->pts[k-1].y;
          L+=sqrt(ddx*ddx+ddy*ddy); p->pts[k].clen=L; }
        p->len=L;
      }
      free(prev); free(cost); free(heap); free(fsco);
      return vreal(found);
    }
    if(!strcmp(sub,"draw")) return vreal(0);
    return vreal(0);
  }
  /* runtime paths (path_add / mp_grid_path targets). Appended to vm->paths; indices stay stable. */
  if(!strcmp(nm,"path_add")){
    GmlPath *np=realloc(vm->paths,(size_t)(vm->n_paths+1)*sizeof(GmlPath));
    if(!np) return vreal(-1);
    vm->paths=np;
    GmlPath *p=&vm->paths[vm->n_paths];
    memset(p,0,sizeof *p);
    p->pts=calloc(1,sizeof(GmlPathPt));
    p->precision=4;
    return vreal(vm->n_paths++);
  }
  if(!strcmp(nm,"path_add_point")){
    int pi=(int)N(a,n,0);
    if(pi>=0 && pi<vm->n_paths){
      GmlPath *p=&vm->paths[pi];
      GmlPathPt *np=realloc(p->pts,(size_t)(p->n+1)*sizeof(GmlPathPt));
      if(np){ p->pts=np;
        p->pts[p->n].x=N(a,n,1); p->pts[p->n].y=N(a,n,2); p->pts[p->n].sp=N(a,n,3); p->n++;
        double L=0; p->pts[0].clen=0;
        for(int k=1;k<p->n;k++){ double dx=p->pts[k].x-p->pts[k-1].x, dy=p->pts[k].y-p->pts[k-1].y;
          L+=sqrt(dx*dx+dy*dy); p->pts[k].clen=L; }
        p->len=L; }
    }
    return vreal(0);
  }
  if(!strcmp(nm,"path_clear_points")){
    int pi=(int)N(a,n,0);
    if(pi>=0 && pi<vm->n_paths){ vm->paths[pi].n=0; vm->paths[pi].len=0; }
    return vreal(0);
  }
  if(!strcmp(nm,"path_get_number")){ int pi=(int)N(a,n,0);
    return vreal((pi>=0&&pi<vm->n_paths)?vm->paths[pi].n:0); }
  if(!strcmp(nm,"path_get_closed")){ int pi=(int)N(a,n,0);
    return vreal((pi>=0&&pi<vm->n_paths)?vm->paths[pi].closed:0); }
  if(!strcmp(nm,"path_get_length")){ int pi=(int)N(a,n,0);
    return vreal((pi>=0&&pi<vm->n_paths)?vm->paths[pi].len:0); }
  if(!strcmp(nm,"path_get_x")||!strcmp(nm,"path_get_y")){
    int pi=(int)N(a,n,0); double t=N(a,n,1);
    if(pi<0||pi>=vm->n_paths) return vreal(0);
    double px,py; gml_path_eval_public(vm,pi,t,&px,&py);
    return vreal(nm[9]=='x'?px:py); }
  if(!strcmp(nm,"path_get_point_x")){ int pi=(int)N(a,n,0), k=(int)N(a,n,1);
    return vreal((pi>=0&&pi<vm->n_paths&&k>=0&&k<vm->paths[pi].n)?vm->paths[pi].pts[k].x:0); }
  if(!strcmp(nm,"path_get_point_y")){ int pi=(int)N(a,n,0), k=(int)N(a,n,1);
    return vreal((pi>=0&&pi<vm->n_paths&&k>=0&&k<vm->paths[pi].n)?vm->paths[pi].pts[k].y:0); }
  if(!strcmp(nm,"path_exists")){ int pi=(int)N(a,n,0); return vreal(pi>=0&&pi<vm->n_paths); }
  if(!strcmp(nm,"path_delete")){ int pi=(int)N(a,n,0);
    if(pi>=0&&pi<vm->n_paths){ vm->paths[pi].n=0; vm->paths[pi].len=0; } return vreal(0); }
  /* mp_linear_step(x,y,speed,checkall): step the current instance toward (x,y) by `speed`; returns 1
   * once it arrives. We move directly (no obstacle stop yet — better than the no-op that never moved). */
  if(!strcmp(nm,"mp_linear_step")){ GmlInstance*s=vm->cur_self; if(!s) return vreal(0);
    double tx=N(a,n,0), ty=N(a,n,1), sp=N(a,n,2); double dx=tx-s->x, dy=ty-s->y, d=hypot(dx,dy);
    if(d<=sp || d<1e-9){ s->x=tx; s->y=ty; gml_colgrid_touch(s); return vreal(1); }
    s->x += dx/d*sp; s->y += dy/d*sp; gml_colgrid_touch(s); return vreal(0); }
  if(!strcmp(nm,"mp_potential_step")){
    GmlInstance*s=vm->cur_self; if(!s) return vreal(0);
    double tx=N(a,n,0), ty=N(a,n,1), amount=fabs(N(a,n,2));
    double dx=tx-s->x, dy=ty-s->y, distance=hypot(dx,dy);
    if(distance<=amount || distance<1e-9){ s->x=tx; s->y=ty; gml_colgrid_touch(s); return vreal(1); }
    double heading=atan2(dy,dx), step=amount;
    int all=N(a,n,3)!=0.0, moved=0;
    static const int turns[]={0,10,-10,20,-20,30,-30};
    for(size_t i=0;i<sizeof(turns)/sizeof(turns[0]);i++){
      double angle=heading+(double)turns[i]*M_PI/180.0;
      double nx=s->x+cos(angle)*step, ny=s->y+sin(angle)*step;
      if(collision_at(vm,nx,ny,all?IT_ALL:0,!all)) continue;
      s->x=nx; s->y=ny;
      s->direction=-angle*180.0/M_PI;
      while(s->direction<0) s->direction+=360.0;
      while(s->direction>=360.0) s->direction-=360.0;
      gml_colgrid_touch(s); moved=1; break;
    }
    return vreal(moved && distance<=amount);
  }
  if(!strcmp(nm,"mp_potential_step_object")){
    GmlInstance*s=vm->cur_self; if(!s) return vreal(0);
    double tx=N(a,n,0), ty=N(a,n,1), amount=fabs(N(a,n,2));
    double dx=tx-s->x, dy=ty-s->y, distance=hypot(dx,dy);
    if(distance<=amount || distance<1e-9){ s->x=tx; s->y=ty; gml_colgrid_touch(s); return vreal(1); }
    double heading=atan2(dy,dx); int object=(int)N(a,n,3), moved=0;
    static const int turns[]={0,10,-10,20,-20,30,-30};
    for(size_t i=0;i<sizeof(turns)/sizeof(turns[0]);i++){
      double angle=heading+(double)turns[i]*M_PI/180.0;
      double nx=s->x+cos(angle)*amount, ny=s->y+sin(angle)*amount;
      if(collision_at(vm,nx,ny,object,0)) continue;
      s->x=nx; s->y=ny; s->direction=-angle*180.0/M_PI;
      while(s->direction<0) s->direction+=360.0;
      while(s->direction>=360.0) s->direction-=360.0;
      gml_colgrid_touch(s); moved=1; break;
    }
    return vreal(moved && distance<=amount);
  }
  if(!strcmp(nm,"action_potential_step")) return vreal(0);
  /* instance_nearest/furthest(x,y,obj): id of the nearest/furthest instance of obj (noone=-4). */
  if(!strcmp(nm,"instance_nearest")||!strcmp(nm,"instance_furthest")){
    int far=!strcmp(nm,"instance_furthest"); double px=N(a,n,0),py=N(a,n,1); int obj=(int)N(a,n,2);
    double best=far?-1:1e18; int bid=-4;
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(!target_matches_instance(vm,vm->cur_self,o,obj)) continue;
      double d=hypot(o->x-px,o->y-py); if((far&&d>best)||(!far&&d<best)){ best=d; bid=(int)o->id; } }
    return vreal(bid); }

  /* ---- strings ---- */
  if(!strcmp(nm,"string")) return vstr_owned(strdup(S(a,n,0)));
  if(!strcmp(nm,"string_length")) return vreal((double)strlen(S(a,n,0)));
  if(!strcmp(nm,"string_byte_length")) return vreal((double)strlen(S(a,n,0)));
  if(!strcmp(nm,"md5_string_utf8")){ const char *s=S(a,n,0); return md5_hex_val((const uint8_t*)s,strlen(s)); }
  if(!strcmp(nm,"string_count")) return vreal(gm_string_count(S(a,n,0),S(a,n,1)));
  if(!strcmp(nm,"string_split")){
    const char *src=S(a,n,0), *sep=S(a,n,1);
    int seplen=(int)strlen(sep);
    GmlVal arr=gml_arr_new(0,vreal(0));
    if(seplen<=0){ gml_arr_push(arr,vstr(src)); return arr; }
    const char *p=src;
    for(;;){
      const char *q=strstr(p,sep);
      size_t len=q ? (size_t)(q-p) : strlen(p);
      char *part=malloc(len+1);
      if(part){ memcpy(part,p,len); part[len]=0; gml_arr_push(arr,vstr(part)); free(part); }
      else gml_arr_push(arr,vstr(""));
      if(!q) break;
      p=q+seplen;
    }
    return arr;
  }
  if(!strcmp(nm,"string_copy")){ const char*s=S(a,n,0); int idx=(int)N(a,n,1), cnt=(int)N(a,n,2);
    int len=strlen(s); if(idx<1)idx=1; if(idx>len)return vstr("");
    if(cnt<0)cnt=0; if(idx-1+cnt>len)cnt=len-(idx-1);
    char*o=malloc(cnt+1); memcpy(o,s+idx-1,cnt); o[cnt]=0; return vstr_owned(o); }
  if(!strcmp(nm,"chr")){ int c=(int)N(a,n,0); char b[2]={(char)(c&0xff),0}; return vstr_owned(strdup(b)); }
  if(!strcmp(nm,"ord")){ const unsigned char*s=(const unsigned char*)S(a,n,0); return vreal(s[0]); }
  /* base64_encode(str)/base64_decode(str): standard RFC-4648 (was hitting the catch-all -> 0).
   * Operates on the string's bytes; decode stops a returned C-string at the first NUL, so it is for
   * text/base64 payloads (binary blobs use the buffer_* API). */
  if(!strcmp(nm,"base64_encode")||!strcmp(nm,"base64_encode_string")){
    static const char B64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *u=(const unsigned char*)S(a,n,0); size_t L=u?strlen((const char*)u):0;
    char *o=malloc((L+2)/3*4+1), *p=o; if(!o) return vstr("");
    size_t i=0;
    for(; i+3<=L; i+=3){ uint32_t v=((uint32_t)u[i]<<16)|((uint32_t)u[i+1]<<8)|u[i+2];
      *p++=B64[(v>>18)&63]; *p++=B64[(v>>12)&63]; *p++=B64[(v>>6)&63]; *p++=B64[v&63]; }
    if(L-i==1){ uint32_t v=(uint32_t)u[i]<<16; *p++=B64[(v>>18)&63]; *p++=B64[(v>>12)&63]; *p++='='; *p++='='; }
    else if(L-i==2){ uint32_t v=((uint32_t)u[i]<<16)|((uint32_t)u[i+1]<<8);
      *p++=B64[(v>>18)&63]; *p++=B64[(v>>12)&63]; *p++=B64[(v>>6)&63]; *p++='='; }
    *p=0; return vstr_owned(o);
  }
  if(!strcmp(nm,"base64_decode")||!strcmp(nm,"base64_decode_string")){
    int len=0; unsigned char *o=base64_decode_alloc(S(a,n,0),&len);
    if(!o) return vstr("");
    o[len]=0; return vstr_owned((char*)o);
  }
  if(!strcmp(nm,"string_char_at")){ const char*s=S(a,n,0); int idx=(int)N(a,n,1), len=(int)strlen(s);
    if(idx<1||idx>len) return vstr("");
    return vstr_owned(dup_n(s+idx-1,1)); }
  if(!strcmp(nm,"string_byte_at")){ const unsigned char*s=(const unsigned char*)S(a,n,0); int idx=(int)N(a,n,1), len=(int)strlen((const char*)s);
    return vreal((idx>=1&&idx<=len)?s[idx-1]:0); }
  if(!strcmp(nm,"string_ord_at")){ const unsigned char*s=(const unsigned char*)S(a,n,0); int idx=(int)N(a,n,1), len=(int)strlen((const char*)s);
    return vreal((idx>=1&&idx<=len)?s[idx-1]:0); }
  if(!strcmp(nm,"string_delete")){ const char*s=S(a,n,0); int idx=(int)N(a,n,1), cnt=(int)N(a,n,2), len=(int)strlen(s);
    if(idx<1) idx=1; if(cnt<0) cnt=0; if(idx>len||cnt==0) return vstr_owned(strdup(s));
    if(idx-1+cnt>len) cnt=len-(idx-1);
    char *o=malloc((size_t)(len-cnt)+1); if(!o) return vstr_owned(strdup(""));
    memcpy(o,s,(size_t)(idx-1)); memcpy(o+idx-1,s+idx-1+cnt,(size_t)(len-(idx-1+cnt)+1)); return vstr_owned(o); }
  if(!strcmp(nm,"string_insert")){  /* string_insert(substr, str, index): insert BEFORE 1-based index */
    const char*sub=S(a,n,0), *s=S(a,n,1); int idx=(int)N(a,n,2);
    int sl=(int)strlen(sub), len=(int)strlen(s);
    if(idx<1) idx=1; if(idx>len+1) idx=len+1;
    char *o=malloc((size_t)(len+sl)+1); if(!o) return vstr_owned(strdup(""));
    memcpy(o,s,(size_t)(idx-1)); memcpy(o+idx-1,sub,(size_t)sl);
    memcpy(o+idx-1+sl,s+idx-1,(size_t)(len-(idx-1))+1);
    return vstr_owned(o); }
  if(!strcmp(nm,"string_replace")||!strcmp(nm,"string_replace_all")){
    /* GM: string_replace replaces the FIRST occurrence; string_replace_all replaces every one. */
    const char*s=S(a,n,0), *sub=S(a,n,1), *rep=S(a,n,2);
    if(!sub||!sub[0]) return vstr_owned(strdup(s?s:""));
    int all=!strcmp(nm,"string_replace_all"), sl=(int)strlen(sub), rl=(int)strlen(rep?rep:"");
    /* count occurrences to size the output */
    int cnt=0; for(const char*p=s; (p=strstr(p,sub)); p+=sl){ cnt++; if(!all) break; }
    size_t out=strlen(s)+(size_t)cnt*(rl-sl>0?rl-sl:0)+1;
    char *o=malloc(out+ (size_t)cnt*(rl>sl?rl:0) +8); if(!o) return vstr_owned(strdup(""));
    char *w=o; const char*p=s; int done=0;
    while(*p){ if((all||!done) && !strncmp(p,sub,(size_t)sl)){ memcpy(w,rep?rep:"",(size_t)rl); w+=rl; p+=sl; done=1; }
      else *w++=*p++; }
    *w=0; return vstr_owned(o); }
  if(!strcmp(nm,"string_hash_to_newline")){ /* legacy text helper: keep the string intact (safer than 0) */
    const char*s=S(a,n,0); return vstr_owned(strdup(s?s:"")); }
  if(!strcmp(nm,"int64")){ return vreal((double)(int64_t)N(a,n,0)); }
  if(!strcmp(nm,"string_upper")){ const char*s=S(a,n,0); int len=(int)strlen(s); char *o=dup_n(s,len);
    for(int i=0;i<len;i++) o[i]=(char)toupper((unsigned char)o[i]); return vstr_owned(o); }
  if(!strcmp(nm,"string_lower")){ const char*s=S(a,n,0); int len=(int)strlen(s); char *o=dup_n(s,len);
    for(int i=0;i<len;i++) o[i]=(char)tolower((unsigned char)o[i]); return vstr_owned(o); }
  if(!strcmp(nm,"string_letters")) return string_filter_ascii(S(a,n,0),0);
  if(!strcmp(nm,"string_lettersdigits")) return string_filter_ascii(S(a,n,0),1);
  if(!strcmp(nm,"string_digits")){
    const char *s=S(a,n,0);
    size_t len=strlen(s);
    char *o=malloc(len+1);
    if(!o) return vstr("");
    size_t k=0;
    for(size_t i=0;i<len;i++)
      if(s[i]>='0'&&s[i]<='9') o[k++]=s[i];
    o[k]=0;
    return vstr_owned(o);
  }
  if(!strcmp(nm,"string_pos")){ const char*needle=S(a,n,0), *hay=S(a,n,1);
    if(!needle[0]) return vreal(0);
    char *p=strstr(hay,needle); return vreal(p ? (double)(p-hay+1) : 0); }
  if(!strcmp(nm,"string_format")){  /* string_format(val, tot, dec): width-padded fixed-point (GM) */
    double v=N(a,n,0); int tot=(int)N(a,n,1), dec=(int)N(a,n,2);
    if(dec<0) dec=0; if(dec>17) dec=17; if(tot<0) tot=0; if(tot>64) tot=64;
    char buf[96]; snprintf(buf,sizeof buf,"%*.*f",tot,dec,v);
    return vstr_owned(strdup(buf)); }
  if(!strcmp(nm,"string_repeat")){ const char*s=S(a,n,0); int count=(int)N(a,n,1); if(count<0) count=0;
    size_t len=strlen(s), total=len*(size_t)count; if(len && total/len!=(size_t)count) total=1024*1024;
    if(total>1024*1024) total=1024*1024;
    char *o=malloc(total+1); if(!o) return vstr_owned(strdup(""));
    size_t pos=0; for(int i=0;i<count && pos+len<=total;i++){ memcpy(o+pos,s,len); pos+=len; }
    o[pos]=0; return vstr_owned(o); }
  if(!strcmp(nm,"string_replace_all")){ const char*src=S(a,n,0), *from=S(a,n,1), *to=S(a,n,2);
    size_t fl=strlen(from), tl=strlen(to), sl=strlen(src);
    if(!fl) return vstr_owned(strdup(src));
    size_t hits=0; for(const char*p=src;(p=strstr(p,from));p+=fl) hits++;
    size_t outlen=sl + hits*(tl>fl ? tl-fl : 0);
    if(outlen>1024*1024) outlen=1024*1024;
    char *o=malloc(outlen+1); if(!o) return vstr_owned(strdup(""));
    size_t pos=0; const char*p=src;
    while(*p && pos<outlen){ const char*q=strstr(p,from);
      if(!q){ size_t ncopy=strlen(p); if(pos+ncopy>outlen) ncopy=outlen-pos; memcpy(o+pos,p,ncopy); pos+=ncopy; break; }
      size_t pre=(size_t)(q-p); if(pos+pre>outlen) pre=outlen-pos; memcpy(o+pos,p,pre); pos+=pre;
      size_t rep=tl; if(pos+rep>outlen) rep=outlen-pos; memcpy(o+pos,to,rep); pos+=rep; p=q+fl;
    }
    o[pos]=0; return vstr_owned(o); }

  /* ---- ini persistence ---- */
  if(!strcmp(nm,"ini_open")||!strcmp(nm,"FS_ini_open")) return builtin_ini_open_file(vm,a,n);
  if(!strcmp(nm,"ini_open_from_string")||!strcmp(nm,"FS_ini_open_from_string")){
    ini_reset(vm);
    vm->ini_open=1;
    ini_parse_text(vm,S(a,n,0));
    return vreal(1);
  }
  if(!strcmp(nm,"ini_close")||!strcmp(nm,"FS_ini_close")){
    if(!vm->ini_open) return vreal(0);
    vm->ini_open=0;
    /* write the current key-value table back to the .ini file */
    if(vm->ini_path[0]){
      FILE *f=fopen(vm->ini_path,"w");
      if(f){
        const char *last_sec=NULL;
        for(int i=0;i<vm->ini_n;i++){
          if(!last_sec||strcmp(vm->ini_kv[i].section,last_sec)){
            fprintf(f,"[%s]\n",vm->ini_kv[i].section); last_sec=vm->ini_kv[i].section;
          }
          if(vm->ini_kv[i].is_str) fprintf(f,"%s=%s\n",vm->ini_kv[i].key,vm->ini_kv[i].sval?vm->ini_kv[i].sval:"");
          else fprintf(f,"%s=%g\n",vm->ini_kv[i].key,vm->ini_kv[i].val);
        }
        fclose(f);
      }
    }
    ini_reset(vm);
    return vreal(0);
  }
  if(!strcmp(nm,"ini_write_real")||!strcmp(nm,"FS_ini_write_real")){
    if(!vm->ini_open||vm->ini_n>=256) return vreal(0);
    const char *sec=S(a,n,0), *key=S(a,n,1); double val=N(a,n,2);
    /* replace existing key under the same section, or append */
    for(int i=0;i<vm->ini_n;i++)
      if(!strcmp(vm->ini_kv[i].section,sec) && !strcmp(vm->ini_kv[i].key,key)){
        free(vm->ini_kv[i].sval); vm->ini_kv[i].sval=NULL; vm->ini_kv[i].val=val; vm->ini_kv[i].is_str=0; return vreal(0); }
    vm->ini_kv[vm->ini_n++]=(typeof(vm->ini_kv[0])){.section=strdup(sec),.key=strdup(key),.val=val,.is_str=0};
    return vreal(0);
  }
  if(!strcmp(nm,"ini_write_string")||!strcmp(nm,"FS_ini_write_string")){
    if(!vm->ini_open||vm->ini_n>=256) return vreal(0);
    const char *sec=S(a,n,0), *key=S(a,n,1), *val=S(a,n,2);
    for(int i=0;i<vm->ini_n;i++)
      if(!strcmp(vm->ini_kv[i].section,sec) && !strcmp(vm->ini_kv[i].key,key)){
        free(vm->ini_kv[i].sval); vm->ini_kv[i].sval=strdup(val); vm->ini_kv[i].val=atof(val); vm->ini_kv[i].is_str=1; return vreal(0); }
    vm->ini_kv[vm->ini_n++]=(typeof(vm->ini_kv[0])){.section=strdup(sec),.key=strdup(key),.sval=strdup(val),.val=atof(val),.is_str=1};
    return vreal(0);
  }
  if(!strcmp(nm,"ini_read_real")||!strcmp(nm,"FS_ini_read_real")){
    if(!vm->ini_open) return vreal(N(a,n,2));  /* default */
    const char *sec=S(a,n,0), *key=S(a,n,1); double def=N(a,n,2);
    /* search the key-value table backwards so later writes override */
    for(int i=vm->ini_n-1;i>=0;i--)
      if(!strcmp(vm->ini_kv[i].section,sec) && !strcmp(vm->ini_kv[i].key,key))
        return vreal(vm->ini_kv[i].val);
    return vreal(def);
  }
  if(!strcmp(nm,"ini_read_string")||!strcmp(nm,"FS_ini_read_string")){
    if(!vm->ini_open) return ini_default_string(a,n);
    const char *sec=S(a,n,0), *key=S(a,n,1);
    for(int i=vm->ini_n-1;i>=0;i--) if(!strcmp(vm->ini_kv[i].section,sec) && !strcmp(vm->ini_kv[i].key,key)){
      /* Return an OWNED copy, not a pointer into ini_kv[].sval: a later ini_close()/ini_open() frees
       * that storage, and GML code routinely keeps the read value across such calls (use-after-free). */
      if(vm->ini_kv[i].is_str){ const char *s=vm->ini_kv[i].sval?vm->ini_kv[i].sval:""; char *c=strdup(s); return c?vstr_owned(c):vstr(""); }
      char b[64]; snprintf(b,sizeof b,"%g",vm->ini_kv[i].val); return vstr_owned(strdup(b)); }
    return ini_default_string(a,n);
  }
  if(!strcmp(nm,"ini_section_exists")||!strcmp(nm,"FS_ini_section_exists")){
    if(!vm->ini_open) return vreal(0);
    const char *sec=S(a,n,0);
    for(int i=0;i<vm->ini_n;i++) if(!strcmp(vm->ini_kv[i].section,sec)) return vreal(1);
    return vreal(0);
  }
  if(!strcmp(nm,"ini_key_exists")||!strcmp(nm,"FS_ini_key_exists")){
    if(!vm->ini_open) return vreal(0);
    const char *sec=S(a,n,0), *key=S(a,n,1);
    for(int i=vm->ini_n-1;i>=0;i--)
      if(!strcmp(vm->ini_kv[i].section,sec) && !strcmp(vm->ini_kv[i].key,key))
        return vreal(1);
    return vreal(0);
  }
  if(!strcmp(nm,"ini_key_delete")||!strcmp(nm,"FS_ini_key_delete")){
    if(!vm->ini_open) return vreal(0);
    const char *sec=S(a,n,0), *key=S(a,n,1);
    for(int i=0;i<vm->ini_n;i++){
      if(strcmp(vm->ini_kv[i].section,sec) || strcmp(vm->ini_kv[i].key,key)) continue;
      free(vm->ini_kv[i].section);
      free(vm->ini_kv[i].key);
      free(vm->ini_kv[i].sval);
      if(i+1<vm->ini_n) memmove(&vm->ini_kv[i],&vm->ini_kv[i+1],(size_t)(vm->ini_n-i-1)*sizeof(vm->ini_kv[0]));
      vm->ini_n--;
      memset(&vm->ini_kv[vm->ini_n],0,sizeof(vm->ini_kv[0]));
      return vreal(0);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"ini_section_delete")||!strcmp(nm,"FS_ini_section_delete")){
    if(!vm->ini_open) return vreal(0);
    const char *sec=S(a,n,0);
    for(int i=0;i<vm->ini_n;){
      if(strcmp(vm->ini_kv[i].section,sec)){ i++; continue; }
      free(vm->ini_kv[i].section);
      free(vm->ini_kv[i].key);
      free(vm->ini_kv[i].sval);
      if(i+1<vm->ini_n) memmove(&vm->ini_kv[i],&vm->ini_kv[i+1],(size_t)(vm->ini_n-i-1)*sizeof(vm->ini_kv[0]));
      vm->ini_n--;
      memset(&vm->ini_kv[vm->ini_n],0,sizeof(vm->ini_kv[0]));
    }
    return vreal(0);
  }

  /* ---- rooms / flow ---- */
  if(!strcmp(nm,"room_get_name")){ GmlRoom rr; int ri=(int)N(a,n,0);
    return vstr(gml_room_get(vm->win,ri,&rr)==0 && rr.name ? rr.name : ""); }
  if(!strcmp(nm,"room_exists")){ int ri=(int)N(a,n,0); return vreal(ri>=0 && ri<gml_room_count(vm->win)); }
  if(!strcmp(nm,"room_set_viewport")){
    /* room_set_viewport(rm, vind, visible, xport, yport, wport, hport): like GMS, edits the
     * room's viewport config; applied when that room is entered (gml_room_enter). */
    int rm=(int)N(a,n,0), vind=(int)N(a,n,1);
    int nrooms=gml_room_count(vm->win);
    if(rm<0||rm>=nrooms||vind<0||vind>7) return vreal(-1);
    if(!vm->view_ovr){
      vm->view_ovr=calloc((size_t)nrooms*8,sizeof(*vm->view_ovr));
      if(!vm->view_ovr) return vreal(-1);
      vm->n_view_ovr=nrooms*8;
    }
    struct GmlViewOvr *o=&vm->view_ovr[rm*8+vind];
    o->set=1; o->vis=N(a,n,2)>=0.5;
    o->x=(int)N(a,n,3); o->y=(int)N(a,n,4); o->w=(int)N(a,n,5); o->h=(int)N(a,n,6);
    if(getenv("GML_LOG_VIEW")) fprintf(stderr,"[view] room_set_viewport rm=%d v=%d vis=%d port=(%d,%d,%d,%d)\n",
      rm,vind,o->vis,o->x,o->y,o->w,o->h);
    return vreal(0);
  }
  if(!strcmp(nm,"room_get_viewport")){
    int rm=(int)N(a,n,0), vind=(int)N(a,n,1);
    if(vm->view_ovr && rm>=0 && vind>=0 && vind<8 && rm*8+vind<vm->n_view_ovr && vm->view_ovr[rm*8+vind].set){
      struct GmlViewOvr *o=&vm->view_ovr[rm*8+vind];
      GmlVal v=arr_newv(5); if(v.t!=V_ARR) return v;
      GmlArr *A=(GmlArr*)v.arr;
      A->data[0]=vreal(o->vis); A->data[1]=vreal(o->x); A->data[2]=vreal(o->y);
      A->data[3]=vreal(o->w);   A->data[4]=vreal(o->h);
      return v;
    }
    return arr_newv(5);
  }
  if(!strcmp(nm,"room_goto")||!strcmp(nm,"room_restart")||!strcmp(nm,"room_goto_next")){
    if(getenv("GML_LOG_ROOMGOTO")){ const char*w=(vm->cur_self&&vm->cur_self->obj>=0&&vm->cur_self->obj<vm->n_objects)?vm->objects[vm->cur_self->obj].name:"?";
      fprintf(stderr,"[rg] %s by=%s from=%d\n",nm,w,vm->room_index); } }
  if(!strcmp(nm,"room_goto")){
    int target=(int)N(a,n,0);
    gml_vm_warm_audio_for_room(vm,target);
    vm->pending_room=target;
    return vreal(0); }
  if(!strcmp(nm,"room_goto_next")||!strcmp(nm,"room_next")){
    int cur = !strcmp(nm,"room_next")? (int)N(a,n,0) : vm->room_index;
    int pos=order_pos(vm,cur);
    int nxt = (pos>=0 && pos+1<vm->win->n_room_order)? (int)vm->win->room_order[pos+1] : -1;
    if(!strcmp(nm,"room_next")) return vreal(nxt);
    if(nxt>=0){ gml_vm_warm_audio_for_room(vm,nxt); vm->pending_room=nxt; }
    return vreal(0); }
  if(!strcmp(nm,"room_goto_previous")||!strcmp(nm,"room_previous")){
    int cur = !strcmp(nm,"room_previous")? (int)N(a,n,0) : vm->room_index;
    int pos=order_pos(vm,cur);
    int prv = (pos>0)? (int)vm->win->room_order[pos-1] : -1;
    if(!strcmp(nm,"room_previous")) return vreal(prv);
    if(prv>=0){ gml_vm_warm_audio_for_room(vm,prv); vm->pending_room=prv; }
    return vreal(0); }
  if(!strcmp(nm,"room_restart")){
    gml_vm_warm_audio_for_room(vm,vm->room_index);
    vm->pending_room=vm->room_index;
    return vreal(0); }
  if(!strcmp(nm,"room_set_persistent")) return vreal(0);
  if(!strcmp(nm,"game_end")){ vm->game_end=1; return vreal(0); }
  if(!strcmp(nm,"game_restart")){ vm->game_end=2; return vreal(0); }

  /* ---- instances ---- */
  if(!strcmp(nm,"instance_create")){ GmlInstance*in=gml_instance_create(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2));
    return vreal(in?(double)in->id:-4); }
  /* GMS2 creation: instance_create_depth(x,y,depth,obj) / instance_create_layer(x,y,layer,obj). The
   * object is the 4th arg (not the 3rd); without these, GMS2 games spawn nothing dynamically. */
  if(!strcmp(nm,"instance_create_depth")){ GmlInstance*in=gml_instance_create_depth(vm,N(a,n,0),N(a,n,1),(int)N(a,n,3),1,N(a,n,2));
    return vreal(in?(double)in->id:-4); }
  if(!strcmp(nm,"instance_create_layer")){ GmlInstance*in=gml_instance_create(vm,N(a,n,0),N(a,n,1),(int)N(a,n,3));
    return vreal(in?(double)in->id:-4); }
  /* ---- GMS2.3 internal function/struct machinery (partial stubs — enough for common init paths) ---- */
  if(!strcmp(nm,"@@This@@"))   return vreal(vm->cur_self ? (double)vm->cur_self->id : -1);
  if(!strcmp(nm,"@@Other@@"))  return vreal(vm->cur_other? (double)vm->cur_other->id : -2);
  if(!strcmp(nm,"@@Global@@")) return vreal(-5);      /* global scope sentinel */
  if(!strcmp(nm,"@@NullObject@@")) return vreal(-4);  /* noone */
  if(!strcmp(nm,"@@NewGMLArray@@")){ GmlArr *A=calloc(1,sizeof(*A)); if(!A) return vreal(0);
    /* Copy stored strings and mark nested arrays escaped, as for other array stores. */
    if(n>0){ A->data=calloc(n,sizeof(GmlVal)); if(A->data){ A->len=A->cap=n; for(int i=0;i<n;i++) A->data[i]=gml_arr_store_clone(a[i]); } }
    GmlVal v=vreal(0); v.t=V_ARR; v.arr=A; return v; }
  /* GMS2.3 method(scope, func): a BOUND method — a struct carrying the function and the self it runs
   * with. OP_CALLV dispatches it with self=__self. (Returning the bare func lost the scope, so any
   * bound input-check/handler that reads its own struct fields ran on the wrong self and read 0.) */
  if(!strcmp(nm,"method")){
    if(n<2) return n>0?a[0]:vreal(0);
    int fv=(int)N(a,n,1);
    /* only wrap real function values; method(self, non-func) is a no-op passthrough */
    if(!GML_IS_FUNCVAL(fv) && !GML_IS_STRUCT_ID(N(a,n,1))) return a[1];
    GmlInstance *bm=gml_struct_new(vm); if(!bm) return a[1];
    /* Resolve the self NOW (bind time). GMS2.3 method() binds to the CURRENT self; keeping the scope
     * sentinel (-1 self / -2 other) would re-resolve it against whoever runs the method LATER — so a
     * binding built in a constructor would read the caller's fields at dispatch, not its own. */
    GmlVal selfv=a[0];
    if(selfv.t==V_REAL && (selfv.d==-1.0 || selfv.d==-2.0 || selfv.d==-16.0)){
      /* -1 self, -2 other, -16 static (a struct method binds to the struct being constructed = the
       * current self at bind time). Resolve to a concrete id so dispatch reaches the right object. */
      GmlInstance *si = selfv.d==-2.0 ? vm->cur_other : vm->cur_self;
      if(si) selfv=vreal((double)si->id);
    }
    *gml_varmap_put(&bm->vars,"__fn")=gml_arr_store_clone(a[1]);
    *gml_varmap_put(&bm->vars,"__self")=gml_arr_store_clone(selfv);
    if(GML_IS_FUNCVAL(fv)){
      bm->method_bound=1;
      bm->method_fci=fv & 0x00FFFFFF;
      bm->method_self=selfv;
    }
    return vreal((double)bm->id);
  }
  /* GMS2.3 struct construction: @@NewGMLObject@@(constructor_func [, ctor_args...]). Allocate a struct
   * and run the constructor with self=the new struct so it populates its fields; return the struct. */
  if(!strcmp(nm,"@@NewGMLObject@@")){
    GmlInstance *st=gml_struct_new(vm); if(!st) return vreal(0);
    if(n>=1){ double a0=N(a,n,0); int fci=-1;
      /* The constructor arrives either as a raw funcval (a named `new Foo()`) OR as a bound
       * method struct (an INLINE constructor defined via method(self,ctor) — GMS2.3 emits this
       * for anonymous struct classes). Unwrap the bound method's __fn so its body actually runs;
       * otherwise the struct is created but never initialised (empty fields). */
      if(GML_IS_FUNCVAL((int)a0)) fci=(int)a0 & 0x00FFFFFF;
      else if(GML_IS_STRUCT_ID(a0)){ GmlInstance *bm=gml_struct_find(vm,(unsigned)a0);
        if(bm){ GmlVal *pf=gml_varmap_get(&bm->vars,"__fn"); if(pf && pf->t==V_REAL){ int f=(int)pf->d; if(GML_IS_FUNCVAL(f)) fci=f & 0x00FFFFFF; } } }
      if(fci>=0){
        *gml_varmap_put(&st->vars,"__ctor")=vreal((double)fci);
        const char *cn=(vm->win&&fci<vm->win->n_code&&vm->win->code[fci].name)?vm->win->code[fci].name:"";
        if(!strncmp(cn,"gml_Script_",11)) cn+=11;
        *gml_varmap_put(&st->vars,"__name")=vstr_owned(strdup(cn));
        gml_vm_run_code(vm,fci,st,vm->cur_self,(n>1)?a+1:0,n-1);
      } }
    return vreal((double)st->id);
  }
  if(!strcmp(nm,"instanceof")){
    GmlInstance *st=(n>0 && a[0].t==V_REAL && GML_IS_STRUCT_ID(a[0].d))?gml_struct_find(vm,(unsigned)a[0].d):NULL;
    GmlVal *pc=st?gml_varmap_get(&st->vars,"__ctor"):NULL;
    int have=(pc && pc->t==V_REAL);
    if(n<2){
      if(!st) return vstr("");
      GmlVal *pn=gml_varmap_get(&st->vars,"__name");
      if(pn && pn->t==V_STR && pn->s) return vstr(pn->s);
      int ci=have?(int)pc->d:-1;
      return vstr((vm->win&&ci>=0&&ci<vm->win->n_code&&vm->win->code[ci].name)?vm->win->code[ci].name:"");
    }
    int want=-1;
    if(a[1].t==V_REAL){
      int iv=(int)a[1].d;
      if(GML_IS_FUNCVAL(iv)) want=iv & 0x00FFFFFF;
      else if(GML_IS_STRUCT_ID(a[1].d)){ GmlInstance *bm=gml_struct_find(vm,(unsigned)a[1].d);
        GmlVal *pf=bm?gml_varmap_get(&bm->vars,"__fn"):NULL;
        if(pf && pf->t==V_REAL){ int f=(int)pf->d; if(GML_IS_FUNCVAL(f)) want=f & 0x00FFFFFF; }
      }
    }
    return vreal(have && want>=0 && (int)pc->d==want);
  }
  if(!strcmp(nm,"@@SetStatic@@")||!strcmp(nm,"@@GetStatic@@")||!strcmp(nm,"@@CopyStatic@@")||
     !strcmp(nm,"@@try_hook@@")||!strcmp(nm,"@@try_unhook@@")||
     !strcmp(nm,"@@finish_catch@@")||!strcmp(nm,"@@finally@@")||
     !strcmp(nm,"@@throw@@")) return vreal(0);
  if(!strcmp(nm,"instance_destroy")){
    /* instance_destroy([id_or_obj, execute_event]): no-arg destroys self; with an argument, destroy
     * the given instance or ALL instances of the given object (family-inclusive). The argument was
     * being IGNORED — it always destroyed self, so `instance_destroy(some_other_object)` killed the
     * CALLER instead of the target (an intro sequencer wiped itself while clearing a leftover
     * object, and the whole opening cutscene stalled). */
    if(n>=1){ int target=(int)N(a,n,0);
      for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
        if(target_matches_instance(vm,vm->cur_self,o,target)) gml_instance_destroy(vm,o); } }
    else if(vm->cur_self) gml_instance_destroy(vm,vm->cur_self);
    return vreal(0); }
  /* drag-and-drop actions (compiled to builtin calls) acting on the current instance */
  /* action_if(expr): D&D single-expression conditional. expr was evaluated on the stack by the
   * bytecode before this call → arg0 is the truth value. Returns the value (0 or 1). */
  if(!strcmp(nm,"action_if")) return vreal(N(a,n,0));
  /* action_if_variable(var, value, op): D&D compare-a-variable conditional. The bytecode has
   * already evaluated "var", so arg0 is the current value, not a variable name. */
  if(!strcmp(nm,"action_if_variable")){
    int op=(int)N(a,n,2);
    double vv=N(a,n,0), cv=N(a,n,1); int r=0;
    switch(op){ case 0: r=(vv==cv); break; case 1: r=(vv<cv); break; case 2: r=(vv>cv); break; }
    return vreal(r); }
  if(!strcmp(nm,"action_if_dice")){
    int sides=(int)floor(fabs(N(a,n,0)));
    return vreal(sides<=1 || (int)floor(gml_rng_value(vm)*sides)==0);
  }
  if(!strcmp(nm,"action_if_next_room")){
    int pos=order_pos(vm,vm->room_index);
    return vreal(pos>=0 && vm->win && pos+1<vm->win->n_room_order);
  }
  if(!strcmp(nm,"action_if_empty") || !strcmp(nm,"action_if_collision")){
    GmlInstance *s=vm->cur_self; double x=N(a,n,0),y=N(a,n,1);
    int relative=n>=4 ? N(a,n,3)!=0 : vm->action_relative;
    if(relative && s){ x+=s->x; y+=s->y; }
    int include_all=N(a,n,2)!=0;
    int hit=collision_at(vm,x,y,include_all?IT_ALL:0,include_all?0:1);
    return vreal(!strcmp(nm,"action_if_collision")?hit:!hit); }
  if(!strcmp(nm,"action_reverse_xdir")){ if(vm->cur_self){
      vm->cur_self->hspeed=-vm->cur_self->hspeed; motion_from_components(vm->cur_self); } return vreal(0); }
  if(!strcmp(nm,"action_reverse_ydir")){ if(vm->cur_self){
      vm->cur_self->vspeed=-vm->cur_self->vspeed; motion_from_components(vm->cur_self); } return vreal(0); }
  if(!strcmp(nm,"action_kill_object")){ if(vm->cur_self) gml_instance_destroy(vm,vm->cur_self); return vreal(0); }
  if(!strcmp(nm,"action_create_object")){ GmlInstance*s=vm->cur_self;
    double x=N(a,n,1), y=N(a,n,2);
    if(vm->action_relative && s){ x+=s->x; y+=s->y; }
    gml_instance_create(vm,x,y,(int)N(a,n,0)); return vreal(0); }
  if(!strcmp(nm,"action_create_object_motion")){ GmlInstance *s=vm->cur_self;
    double x=N(a,n,1),y=N(a,n,2); if(vm->action_relative && s){ x+=s->x; y+=s->y; }
    GmlInstance *created=gml_instance_create(vm,x,y,(int)N(a,n,0));
    if(created){ created->speed=N(a,n,3); created->direction=N(a,n,4);
      created->hspeed=created->speed*cos(created->direction*M_PI/180.0);
      created->vspeed=-created->speed*sin(created->direction*M_PI/180.0); }
    return vreal(0); }
  if(!strcmp(nm,"action_previous_room")){
    int pos=order_pos(vm,vm->room_index);
    if(pos>0){
      int target=(int)vm->win->room_order[pos-1];
      gml_vm_warm_audio_for_room(vm,target);
      vm->pending_room=target;
    }
    return vreal(0); }
  if(!strcmp(nm,"action_current_room")){ gml_vm_warm_audio_for_room(vm,vm->room_index); vm->pending_room=vm->room_index; return vreal(0); }
  if(!strcmp(nm,"action_next_room")){
    int pos=order_pos(vm,vm->room_index);
    if(vm->win && pos>=0 && pos+1<vm->win->n_room_order){
      int target=(int)vm->win->room_order[pos+1]; gml_vm_warm_audio_for_room(vm,target); vm->pending_room=target;
    }
    return vreal(0);
  }
  if(!strcmp(nm,"action_move_to")){ GmlInstance*s=vm->cur_self; if(s){
      double x=N(a,n,0), y=N(a,n,1);
      if(vm->action_relative){ s->x+=x; s->y+=y; } else { s->x=x; s->y=y; } gml_colgrid_touch(s); } return vreal(0); }
  if(!strcmp(nm,"action_move")||!strcmp(nm,"action_set_motion")){ GmlInstance*s=vm->cur_self; if(s){
      double direction=N(a,n,0), amount=N(a,n,1);
      if(!strcmp(nm,"action_move") && n>0 && a[0].t==V_STR){
        static const int directions[9]={225,270,315,180,-1,0,135,90,45};
        int choices[9], count=0;
        const char *mask=S(a,n,0);
        for(int i=0;i<9 && mask[i];i++) if(mask[i]!='0') choices[count++]=directions[i];
        if(count){ int choice=(int)floor(gml_rng_value(vm)*count); if(choice>=count) choice=count-1;
          direction=choices[choice]; if(direction<0) amount=0; }
      }
      if(vm->action_relative){ s->direction+=direction; s->speed+=amount; }
      else { s->direction=direction; s->speed=amount; }
      s->hspeed=s->speed*cos(s->direction*M_PI/180.0); s->vspeed=-s->speed*sin(s->direction*M_PI/180.0); }
    return vreal(0); }
  if(!strcmp(nm,"action_move_point")){ GmlInstance*s=vm->cur_self; if(s){
      double tx=N(a,n,0), ty=N(a,n,1);
      if(vm->action_relative){ tx+=s->x; ty+=s->y; }
      double dir=atan2(-(ty-s->y),tx-s->x)*180.0/M_PI; double sp=N(a,n,2);
      s->direction=dir; s->speed=sp; s->hspeed=sp*cos(dir*M_PI/180.0); s->vspeed=-sp*sin(dir*M_PI/180.0); }
    return vreal(0); }
  if(!strcmp(nm,"action_set_hspeed")){ GmlInstance*s=vm->cur_self; if(s){
      if(vm->action_relative) s->hspeed+=N(a,n,0); else s->hspeed=N(a,n,0);
      motion_from_components(s); } return vreal(0); }
  if(!strcmp(nm,"action_set_vspeed")){ GmlInstance*s=vm->cur_self; if(s){
      if(vm->action_relative) s->vspeed+=N(a,n,0); else s->vspeed=N(a,n,0);
      motion_from_components(s); } return vreal(0); }
  if(!strcmp(nm,"action_set_gravity")){ GmlInstance*s=vm->cur_self; if(s){
      if(vm->action_relative){ s->gravity+=N(a,n,0); s->gravity_direction+=N(a,n,1); }
      else { s->gravity=N(a,n,0); s->gravity_direction=N(a,n,1); } } return vreal(0); }
  if(!strcmp(nm,"action_set_friction")){ GmlInstance*s=vm->cur_self; if(s){
      if(vm->action_relative) s->friction+=N(a,n,0); else s->friction=N(a,n,0); } return vreal(0); }
  if(!strcmp(nm,"action_set_relative")){ vm->action_relative=N(a,n,0)>=0.5; return vreal(0); }
  if(!strcmp(nm,"action_sprite_set")){ GmlInstance *s=vm->cur_self; if(s){
      s->sprite_index=N(a,n,0); s->image_index=N(a,n,1); s->image_speed=N(a,n,2);
      gml_colgrid_touch(s);
      if(vm->render && (int)s->sprite_index>=0) gml_render_prefetch_sprite((GmlRender*)vm->render,(int)s->sprite_index);
    } return vreal(0); }
  if(!strcmp(nm,"action_sprite_color") || !strcmp(nm,"action_sprite_colour")){ GmlInstance *s=vm->cur_self; if(s){
      s->image_blend=N(a,n,0); s->image_alpha=N(a,n,1); } return vreal(0); }
  if(!strcmp(nm,"action_color") || !strcmp(nm,"action_colour")){ GmlRender *r=(GmlRender*)vm->render;
    if(r) r->color=(uint32_t)N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"action_font")){ GmlRender *r=(GmlRender*)vm->render; if(r){
      r->font=(int)N(a,n,0); r->halign=(int)N(a,n,1); } return vreal(0); }
  if(!strcmp(nm,"action_if_aligned")){ GmlInstance *s=vm->cur_self; if(!s) return vreal(0);
    double sx=fabs(N(a,n,0)),sy=fabs(N(a,n,1));
    int ax=sx<=0.0 || fabs(s->x/sx-round(s->x/sx))<1e-7;
    int ay=sy<=0.0 || fabs(s->y/sy-round(s->y/sy))<1e-7;
    return vreal(ax&&ay); }
  if(!strcmp(nm,"action_if_life")||!strcmp(nm,"action_if_score")||!strcmp(nm,"action_if_health")){
    const char *key=!strcmp(nm,"action_if_life")?"lives":!strcmp(nm,"action_if_score")?"score":"health";
    GmlVal *value=gml_varmap_get(&vm->globals,key);
    double current=value&&value->t==V_REAL?value->d:0.0, target=N(a,n,0);
    switch((int)N(a,n,1)){ case 1:return vreal(current<target); case 2:return vreal(current>target); default:return vreal(current==target); }
  }
  if(!strcmp(nm,"action_set_life")||!strcmp(nm,"action_set_score")||!strcmp(nm,"action_set_health")){
    const char *key=!strcmp(nm,"action_set_life")?"lives":!strcmp(nm,"action_set_score")?"score":"health";
    GmlVal *value=gml_varmap_put(&vm->globals,key);
    double next=N(a,n,0);
    if(vm->action_relative) next+=(value&&value->t==V_REAL)?value->d:0.0;
    if(value) *value=vreal(next);
    return vreal(0); }
  if(!strcmp(nm,"action_draw_score")||!strcmp(nm,"action_draw_life")||!strcmp(nm,"action_draw_health")){
    const char *key=!strcmp(nm,"action_draw_score")?"score":!strcmp(nm,"action_draw_life")?"lives":"health";
    GmlVal *value=gml_varmap_get(&vm->globals,key);
    char text[256]; snprintf(text,sizeof(text),"%s%.0f",S(a,n,2),value&&value->t==V_REAL?value->d:0.0);
    GmlRender *r=(GmlRender*)vm->render; if(r) gml_draw_text(r,N(a,n,0),N(a,n,1),text);
    return vreal(0); }
  if(!strcmp(nm,"action_draw_sprite")){ GmlVal draw_args[4]={vreal(N(a,n,0)),vreal(N(a,n,3)),vreal(N(a,n,1)),vreal(N(a,n,2))};
    return gml_builtin_call(vm,"draw_sprite",draw_args,4); }
  if(!strcmp(nm,"action_draw_variable")){ GmlVal draw_args[3]={vreal(N(a,n,1)),vreal(N(a,n,2)),n>0?a[0]:vreal(0)};
    return gml_builtin_call(vm,"draw_text",draw_args,3); }
  if(!strcmp(nm,"action_draw_text")){
    GmlRender *r=(GmlRender*)vm->render;
    if(r) gml_draw_text(r,N(a,n,1),N(a,n,2),S(a,n,0));
    return vreal(0);
  }
  if(!strcmp(nm,"action_draw_life_images")){
    GmlRender *r=(GmlRender*)vm->render; GmlVal *value=gml_varmap_get(&vm->globals,"lives");
    int count=(int)floor(value&&value->t==V_REAL?value->d:0), sprite=(int)N(a,n,2);
    int width=(r&&gml_sprite_exists(r,sprite))?r->spr[sprite].w:0;
    if(r) for(int i=0;i<count;i++) gml_draw_sprite(r,sprite,0,N(a,n,0)+i*width,N(a,n,1));
    return vreal(0);
  }
  if(!strcmp(nm,"action_set_cursor")){ vm->window_cursor=(int)N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"effect_create_above")||!strcmp(nm,"effect_create_below")){
    gml_effect_create(!strcmp(nm,"effect_create_above"),(int)N(a,n,0),N(a,n,1),N(a,n,2),(int)N(a,n,3),(uint32_t)N(a,n,4));
    return vreal(0);
  }
  if(!strcmp(nm,"action_effect")){
    double x=N(a,n,1),y=N(a,n,2); GmlInstance *s=vm->cur_self;
    if(N(a,n,5)!=0 && s){ x+=s->x; y+=s->y; }
    gml_effect_create(1,(int)N(a,n,0),x,y,(int)N(a,n,3),(uint32_t)N(a,n,4));
    return vreal(0);
  }
  if(!strcmp(nm,"action_wrap")){ GmlInstance *s=vm->cur_self; GmlRoom room; int dir=(int)N(a,n,0);
    if(s && gml_room_get(vm->win,vm->room_index,&room)==0){
      if((dir==0||dir==2) && room.width>0){ while(s->x<0)s->x+=room.width; while(s->x>=room.width)s->x-=room.width; }
      if((dir==1||dir==2) && room.height>0){ while(s->y<0)s->y+=room.height; while(s->y>=room.height)s->y-=room.height; }
      gml_colgrid_touch(s);
    } return vreal(0); }
  if(!strcmp(nm,"instance_number")) return vreal(gml_instance_number(vm,(int)N(a,n,0)));
  if(!strcmp(nm,"instance_exists")) return vreal(gml_instance_number(vm,(int)N(a,n,0))>0);
  if(!strcmp(nm,"instance_find")){
    int obj=(int)N(a,n,0), nth=(int)N(a,n,1), seen=0;
    if(nth<0) return vreal(-4);
    for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
      if(!target_matches_instance(vm,vm->cur_self,o,obj)) continue;
      if(seen++==nth) return vreal(o->id);
    }
    return vreal(-4);
  }
  /* instance (de)activation supports off-screen culling and pause behavior.
   * Deactivated instances keep active=0 so step/draw/query guards skip them. */
  if(!strcmp(nm,"instance_deactivate_object")){ int obj=(int)N(a,n,0);
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(target_matches_instance(vm,vm->cur_self,o,obj)){ o->active=0; o->deactivated=1; } } return vreal(0); }
  if(!strcmp(nm,"instance_deactivate_all")){ int notme=(int)N(a,n,0);
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(o->active&&!o->marked&&!(notme&&o==vm->cur_self)){ o->active=0; o->deactivated=1; } } return vreal(0); }
  if(!strcmp(nm,"instance_activate_object")){ int obj=(int)N(a,n,0);
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(o->deactivated && !o->marked && (obj==IT_ALL || obj==IT_SELF || obj==IT_OTHER ||
          (obj>=100000 && (int)o->id==obj) || (obj>=0 && gml_object_is(vm,o->obj,obj)))){
        o->active=1; o->deactivated=0;
      } } return vreal(0); }
  if(!strcmp(nm,"instance_activate_all")){
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(o->deactivated){ o->active=1; o->deactivated=0; } } return vreal(0); }
  if(!strcmp(nm,"instance_activate_region")||!strcmp(nm,"instance_deactivate_region")){
    double rx=N(a,n,0),ry=N(a,n,1),rw=N(a,n,2),rh=N(a,n,3); int inside=(int)N(a,n,4);
    int act=!strcmp(nm,"instance_activate_region");
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(act ? !o->deactivated : (!o->active||o->marked)) continue;
      int inreg=instance_region_hit(vm,o,rx,ry,rw,rh);
      if(inreg==(inside!=0)){ if(act){ o->active=1; o->deactivated=0; } else { o->active=0; o->deactivated=1; } } }
    return vreal(0); }
  if(!strcmp(nm,"instance_change")){
    if(vm->cur_self) gml_instance_change(vm,vm->cur_self,(int)N(a,n,0),N(a,n,1)>=0.5);
    return vreal(0);
  }
  /* instance_position(x,y,obj): id of the instance of obj whose mask covers (x,y), else noone(-4). */
  if(!strcmp(nm,"instance_position")){ GmlInstance *o=instance_at_point(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2));
    return vreal(o? (double)o->id : -4); }
  /* instance_place(x,y,obj): id of the first instance self would collide with at (x,y), else noone. */
  if(!strcmp(nm,"instance_place")){ GmlInstance *o=collision_instance_at(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2),0);
    return vreal(o? (double)o->id : -4); }
  if(!strcmp(nm,"instance_place_list")){
    GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,3));
    int r=collision_instance_list_at(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2),l,N(a,n,4)>=0.5);
    if(log_col_on()){ GmlInstance*cs=vm->cur_self; const char*cnm=(cs&&cs->obj>=0&&cs->obj<vm->n_objects)?vm->objects[cs->obj].name:"?";
      const char*tn=((int)N(a,n,2)>=0&&(int)N(a,n,2)<vm->n_objects)?vm->objects[(int)N(a,n,2)].name:"?";
      if(log_col_match(cnm)) fprintf(stderr,"[col] %s instance_place_list(%.0f,%.0f,%s)=%d\n",cnm,N(a,n,0),N(a,n,1),tn,r); }
    return vreal(r); }
  if(!strcmp(nm,"collision_line")){
    double x1=N(a,n,0), y1=N(a,n,1), x2=N(a,n,2), y2=N(a,n,3);
    int obj=(int)N(a,n,4), precise=N(a,n,5)>=0.5, notme=(int)N(a,n,6);
    static const char *colline_dbg=(const char*)-1;
    if(colline_dbg==(const char*)-1) colline_dbg=getenv("GML_DBG_COLLINE");
    GmlInstance *hit=collision_line_query(vm,x1,y1,x2,y2,obj,precise,notme);
    if(hit){ if(colline_dbg){ extern long g_vm_frame;
        fprintf(stderr,"[colline] f%ld (%.0f,%.0f)-(%.0f,%.0f) obj=%d HIT %s id=%u at(%.1f,%.1f)\n",
          g_vm_frame,x1,y1,x2,y2,obj,
          (hit->obj>=0&&hit->obj<vm->n_objects)?vm->objects[hit->obj].name:"?",hit->id,hit->x,hit->y); }
      return vreal(hit->id); }
    return vreal(-4);
  }
  if(!strcmp(nm,"collision_line_list")){
    GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,7));
    int r=collision_line_list_query(vm,N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),
                                    (int)N(a,n,4),N(a,n,5)>=0.5,(int)N(a,n,6),
                                    l,N(a,n,8)>=0.5);
    return vreal(r);
  }
  /* Step an overlapping instance along `direction` until it clears either solids or every
   * instance, subject to maxdist. Both variants use the same mask-precise collision path. */
  if(!strcmp(nm,"move_outside_solid")||!strcmp(nm,"move_outside_all")){ GmlInstance *s=vm->cur_self; if(s){
      int all=!strcmp(nm,"move_outside_all");
      double dir=N(a,n,0), md=N(a,n,1); if(md<=0) md=1000;
      double dx=cos(dir*M_PI/180.0), dy=-sin(dir*M_PI/180.0);
      for(int k=0;k<(int)md;k++){
        if(!collision_at(vm,s->x,s->y,all?IT_ALL:0,!all)) break;
        s->x+=dx; s->y+=dy;
      }
      gml_colgrid_touch(s); }
    return vreal(0); }
  if(!strcmp(nm,"move_bounce_solid")||!strcmp(nm,"move_bounce_all")){ GmlInstance *s=vm->cur_self; if(s){
      double hs=s->hspeed, vs=s->vspeed;
      int all=!strcmp(nm,"move_bounce_all");
      int bh=collision_at(vm,s->x+hs,s->y,all?IT_ALL:0,!all);
      int bv=collision_at(vm,s->x,s->y+vs,all?IT_ALL:0,!all);
      if(bh) s->hspeed=-hs; if(bv) s->vspeed=-vs;
      if(bh||bv) motion_from_components(s); }
    return vreal(0); }
  if(!strcmp(nm,"move_wrap")){ GmlInstance *s=vm->cur_self; GmlRoom room;
    if(s && gml_room_get(vm->win,vm->room_index,&room)==0){
      int horizontal=N(a,n,0)!=0, vertical=N(a,n,1)!=0; double margin=fabs(N(a,n,2));
      if(horizontal){ if(s->x < -margin) s->x=room.width+margin; else if(s->x>room.width+margin) s->x=-margin; }
      if(vertical){ if(s->y < -margin) s->y=room.height+margin; else if(s->y>room.height+margin) s->y=-margin; }
      gml_colgrid_touch(s);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"move_snap")){ GmlInstance *s=vm->cur_self; if(s){
      double xs=N(a,n,0), ys=N(a,n,1); if(xs>0) s->x=round(s->x/xs)*xs; if(ys>0) s->y=round(s->y/ys)*ys; gml_colgrid_touch(s); }
    return vreal(0); }
  /* action_set_alarm(value,index): D&D Set Alarm → self.alarm[index] = value. */
  if(!strcmp(nm,"action_set_alarm")){ GmlInstance *s=vm->cur_self; int idx=(int)N(a,n,1);
    if(s && idx>=0 && idx<GML_ALARMS) s->alarm[idx]=N(a,n,0); return vreal(0); }
  /* action_bounce(advanced,against): D&D Bounce. against 0=solid, 1=all. Non-advanced: reverse the
   * velocity component(s) whose next step meets a blocker, then relink direction/speed. */
  if(!strcmp(nm,"action_bounce")){ GmlInstance *s=vm->cur_self; if(s){
      int solid=(int)N(a,n,1)==0; double hs=s->hspeed, vs=s->vspeed;
      int bh = solid ? collision_at(vm,s->x+hs,s->y,0,1) : collision_at(vm,s->x+hs,s->y,IT_ALL,0);
      int bv = solid ? collision_at(vm,s->x,s->y+vs,0,1) : collision_at(vm,s->x,s->y+vs,IT_ALL,0);
      if(bh) s->hspeed=-hs; if(bv) s->vspeed=-vs;
      if(bh||bv){ s->speed=hypot(s->hspeed,s->vspeed);
        s->direction=atan2(-s->vspeed,s->hspeed)*180.0/M_PI; if(s->direction<0) s->direction+=360; } }
    return vreal(0); }

  /* ---- drawing ---- */
  { GmlRender *R=(GmlRender*)vm->render;
    /* ---- particle system (part_type / part_system / part_emitter) ---- */
    if(!strncmp(nm,"part_",5)){
      if(!strcmp(nm,"part_type_create")) return vreal(gml_part_type_create());
      if(!strcmp(nm,"part_type_destroy")){ gml_part_type_destroy((int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_type_clear")){ gml_part_type_clear((int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_type_exists")) return vreal(gml_part_type_exists((int)N(a,n,0)));
      if(!strcmp(nm,"part_type_sprite")){ gml_part_type_sprite((int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),(int)N(a,n,3),(int)N(a,n,4)); return vreal(0); }
      if(!strcmp(nm,"part_type_shape")){ gml_part_type_shape((int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
      if(!strcmp(nm,"part_type_size")){ gml_part_type_size((int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4)); return vreal(0); }
      if(!strcmp(nm,"part_type_scale")){ gml_part_type_scale((int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
      if(!strcmp(nm,"part_type_speed")){ gml_part_type_speed((int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4)); return vreal(0); }
      if(!strcmp(nm,"part_type_direction")){ gml_part_type_direction((int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4)); return vreal(0); }
      if(!strcmp(nm,"part_type_gravity")){ gml_part_type_gravity((int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
      if(!strcmp(nm,"part_type_life")){ gml_part_type_life((int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
      if(!strcmp(nm,"part_type_orientation")){ gml_part_type_orientation((int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),(int)N(a,n,5)); return vreal(0); }
      if(!strcmp(nm,"part_type_death")) return vreal(0);
      if(!strcmp(nm,"part_type_color1")||!strcmp(nm,"part_type_colour1")){ gml_part_type_color((int)N(a,n,0),1,(uint32_t)N(a,n,1),0,0); return vreal(0); }
      if(!strcmp(nm,"part_type_color2")||!strcmp(nm,"part_type_colour2")){ gml_part_type_color((int)N(a,n,0),2,(uint32_t)N(a,n,1),(uint32_t)N(a,n,2),0); return vreal(0); }
      if(!strcmp(nm,"part_type_color3")||!strcmp(nm,"part_type_colour3")){ gml_part_type_color((int)N(a,n,0),3,(uint32_t)N(a,n,1),(uint32_t)N(a,n,2),(uint32_t)N(a,n,3)); return vreal(0); }
      if(!strcmp(nm,"part_type_color_rgb")||!strcmp(nm,"part_type_colour_rgb")){ gml_part_type_color_rgb((int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6)); return vreal(0); }
      if(!strcmp(nm,"part_type_color_mix")||!strcmp(nm,"part_type_colour_mix")){ gml_part_type_color_mix((int)N(a,n,0),(uint32_t)N(a,n,1),(uint32_t)N(a,n,2)); return vreal(0); }
      if(!strcmp(nm,"part_type_color_hsv")||!strcmp(nm,"part_type_colour_hsv")){ gml_part_type_color_hsv((int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6)); return vreal(0); }
      if(!strcmp(nm,"part_type_alpha1")){ gml_part_type_alpha((int)N(a,n,0),1,N(a,n,1),0,0); return vreal(0); }
      if(!strcmp(nm,"part_type_alpha2")){ gml_part_type_alpha((int)N(a,n,0),2,N(a,n,1),N(a,n,2),0); return vreal(0); }
      if(!strcmp(nm,"part_type_alpha3")){ gml_part_type_alpha((int)N(a,n,0),3,N(a,n,1),N(a,n,2),N(a,n,3)); return vreal(0); }
      if(!strcmp(nm,"part_type_blend")){ gml_part_type_blend((int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
      if(!strcmp(nm,"part_system_create")||!strcmp(nm,"part_system_create_layer")){ return vreal(gml_part_system_create()); }
      if(!strcmp(nm,"part_system_destroy")){ gml_part_system_destroy((int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_system_exists")) return vreal(gml_part_system_exists((int)N(a,n,0)));
      if(!strcmp(nm,"part_system_clear")){ gml_part_system_clear((int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_system_position")){ gml_part_system_position((int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
      if(!strcmp(nm,"part_system_automatic_update")){ gml_part_system_automatic_update((int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
      if(!strcmp(nm,"part_system_automatic_draw")){ gml_part_system_automatic_draw((int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
      if(!strcmp(nm,"part_system_update")){ gml_part_system_update((int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_system_drawit")||!strcmp(nm,"part_system_drawit_ext")){ if(R) gml_part_system_drawit(R,(int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_system_depth")){ gml_part_system_depth((int)N(a,n,0),N(a,n,1)); return vreal(0); }
      if(!strcmp(nm,"part_system_layer")){
        GmlRtLayer *l=(n>1 && a[1].t==V_STR && a[1].s) ? gml_rt_layer_find_by_name(vm,a[1].s) : gml_rt_layer_find(vm,(int)N(a,n,1));
        if(l) gml_part_system_depth((int)N(a,n,0),l->depth);
        return vreal(0);
      }
      if(!strcmp(nm,"part_particles_count")) return vreal(gml_part_system_count((int)N(a,n,0)));
      if(!strcmp(nm,"part_particles_create")){ gml_part_particles_create((int)N(a,n,0),N(a,n,1),N(a,n,2),(int)N(a,n,3),(int)N(a,n,4)); return vreal(0); }
      if(!strcmp(nm,"part_particles_create_color")||!strcmp(nm,"part_particles_create_colour")){ gml_part_particles_create_color((int)N(a,n,0),N(a,n,1),N(a,n,2),(int)N(a,n,3),(uint32_t)N(a,n,4),(int)N(a,n,5)); return vreal(0); }
      if(!strcmp(nm,"part_particles_clear")){ gml_part_system_clear((int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_emitter_create")){ return vreal(gml_part_emitter_create((int)N(a,n,0))); }
      if(!strcmp(nm,"part_emitter_exists")){ return vreal(gml_part_emitter_exists((int)N(a,n,0),(int)N(a,n,1))); }
      if(!strcmp(nm,"part_emitter_destroy")){ gml_part_emitter_destroy((int)N(a,n,1)); return vreal(0); }
      if(!strcmp(nm,"part_emitter_destroy_all")){ gml_part_emitter_destroy_all((int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_emitter_region")){ gml_part_emitter_region((int)N(a,n,0),(int)N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),(int)N(a,n,6),(int)N(a,n,7)); return vreal(0); }
      if(!strcmp(nm,"part_emitter_burst")){ gml_part_emitter_burst((int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),(int)N(a,n,3)); return vreal(0); }
      if(!strcmp(nm,"part_emitter_stream")){ gml_part_emitter_stream((int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),(int)N(a,n,3)); return vreal(0); }
      if(!strcmp(nm,"part_emitter_clear")){ gml_part_emitter_clear((int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
      if(!strncmp(nm,"part_system_drawit",18) || !strncmp(nm,"part_system_",12) || !strncmp(nm,"part_type_",10) || !strncmp(nm,"part_emitter_",13) || !strncmp(nm,"part_particles_",15))
        return vreal(0);   /* remaining part_* variants: safe no-op */
    }
    if(!strcmp(nm,"draw_sprite")){ if(R) gml_draw_sprite(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3)); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_ext")){ if(R) gml_draw_sprite_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),
        N(a,n,4),N(a,n,5),N(a,n,6),(uint32_t)N(a,n,7),N(a,n,8)); return vreal(0); }
    /* draw_sprite_tiled(sprite,subimg,x,y) / _ext(...,xs,ys,color,alpha): tile a sprite to fill the screen */
    if(!strcmp(nm,"draw_sprite_tiled")){ if(R) gml_draw_sprite_tiled_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),1,1,0xFFFFFF,R->alpha); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_tiled_ext")){ if(R) gml_draw_sprite_tiled_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),
        N(a,n,4),N(a,n,5),(uint32_t)N(a,n,6),N(a,n,7)); return vreal(0); }
    if(!strcmp(nm,"draw_self")){ GmlInstance*s=vm->cur_self; if(R&&s) gml_draw_sprite_ext(R,(int)s->sprite_index,
        (int)s->image_index,s->x,s->y,s->image_xscale,s->image_yscale,s->image_angle,(uint32_t)s->image_blend,s->image_alpha); return vreal(0); }
    if(!strcmp(nm,"draw_set_color")||!strcmp(nm,"draw_set_colour")){ if(R){ R->color=(uint32_t)N(a,n,0); } return vreal(0); }
    if(!strcmp(nm,"draw_set_alpha")){ if(R){ R->alpha=N(a,n,0); if(R->alpha<0) R->alpha=0; if(R->alpha>1) R->alpha=1; } return vreal(0); }
    if(!strcmp(nm,"draw_set_font")){ if(R){ R->font=(int)N(a,n,0); if((int)N(a,n,0)<0 && getenv("GML_LOG_FONT")) fprintf(stderr,"[font] draw_set_font(%d) — default-font request\n",(int)N(a,n,0)); } return vreal(0); }
    if(!strcmp(nm,"draw_set_halign")){ if(R){ R->halign=(int)N(a,n,0); } return vreal(0); }
    if(!strcmp(nm,"draw_set_valign")){ if(R){ R->valign=(int)N(a,n,0); } return vreal(0); }
    if(!strcmp(nm,"draw_background")){ if(getenv("GML_LOG_BG")) fprintf(stderr,"[bg] draw_background(%d,%g,%g)\n",(int)N(a,n,0),N(a,n,1),N(a,n,2)); if(R) gml_draw_background(R,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
    if(!strcmp(nm,"draw_background_tiled")){ int bd=(int)N(a,n,0),ht=1,vt=1; bg_layer_tiling(vm,bd,&ht,&vt);
      if(getenv("GML_LOG_BG")) fprintf(stderr,"[bg] draw_background_tiled(%d,%g,%g) h=%d v=%d\n",bd,N(a,n,1),N(a,n,2),ht,vt);
      if(R) gml_draw_background_tiled(R,bd,N(a,n,1),N(a,n,2),ht,vt); return vreal(0); }
    /* _ext = +xscale,yscale,colour,alpha (background_ext also has a rotation arg before colour). */
    if(!strcmp(nm,"draw_background_tiled_ext")){ int bd=(int)N(a,n,0),ht=1,vt=1; bg_layer_tiling(vm,bd,&ht,&vt);
      if(getenv("GML_LOG_BGA")) fprintf(stderr,"[bga] bg=%d x=%g y=%g alpha=%g\n",bd,N(a,n,1),N(a,n,2),N(a,n,6));
      if(R) gml_draw_background_tiled_ext(R,bd,N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),(uint32_t)N(a,n,5),N(a,n,6),ht,vt); return vreal(0); }
    if(!strcmp(nm,"draw_background_ext")){
      if(getenv("GML_LOG_BG")) fprintf(stderr,"[bg] draw_background_ext(%d,%g,%g,%g,%g,%g)\n",
        (int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,7));
      if(R) gml_draw_background_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),(uint32_t)N(a,n,6),N(a,n,7)); return vreal(0); }
    /* Surface / GUI draws. draw_surface_stretched(surf,x,y,w,h); _ext adds
     * (col,alpha); draw_sprite_stretched(spr,sub,x,y,w,h). */
    if(!strcmp(nm,"draw_surface_stretched")){ if(R) gml_draw_surface_stretched(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),0xFFFFFF,R->alpha); return vreal(0); }
    if(!strcmp(nm,"draw_surface_stretched_ext")){ if(R) gml_draw_surface_stretched(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),(uint32_t)N(a,n,5),N(a,n,6)); return vreal(0); }
    if(!strcmp(nm,"draw_surface")){ if(R){ int s=(int)N(a,n,0);
      /* drawing the surface ASSIGNED TO THE VIEW (view_surface_id) is "present the frame": the yy
       * compat layer draws its port-sized view surface at (0,0) expecting it to cover the window.
       * Our GUI target is the native view canvas, so fit it to the target instead of clipping. */
      if(s>0 && s==(int)gml_global_arr(vm,"view_surface_id",0) && N(a,n,1)==0 && N(a,n,2)==0)
        gml_draw_surface_stretched(R,s,R->cam_x,R->cam_y,R->fbw,R->fbh,0xFFFFFF,R->alpha);
      else
        gml_draw_surface_stretched(R,s,N(a,n,1),N(a,n,2),gml_surface_width(R,s),gml_surface_height(R,s),0xFFFFFF,R->alpha); } return vreal(0); }
    if(!strcmp(nm,"draw_surface_ext")){ if(R){ int s=(int)N(a,n,0); gml_draw_surface_stretched(R,s,N(a,n,1),N(a,n,2),gml_surface_width(R,s)*N(a,n,3),gml_surface_height(R,s)*N(a,n,4),(uint32_t)N(a,n,6),N(a,n,7)); } return vreal(0); }
    if(!strcmp(nm,"draw_surface_part_ext")){ if(R) gml_draw_surface_part_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),N(a,n,7),N(a,n,8),(uint32_t)N(a,n,9),N(a,n,10)); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_stretched")){ if(R) gml_draw_sprite_stretched(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),0xFFFFFF,R->alpha); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_stretched_ext")){ if(R) gml_draw_sprite_stretched(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),(uint32_t)N(a,n,6),N(a,n,7)); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_part")){ if(R) gml_draw_sprite_part_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),N(a,n,7),1,1,0xFFFFFF,R->alpha); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_part_ext")){ if(R) gml_draw_sprite_part_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),N(a,n,7),N(a,n,8),N(a,n,9),(uint32_t)N(a,n,10),N(a,n,11)); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_general")){
      /* Use the first corner color as a flat tint. The part-blit path ignores rotation. */
      if(R) gml_draw_sprite_part_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),
                                     N(a,n,6),N(a,n,7),N(a,n,8),N(a,n,9),(uint32_t)N(a,n,11),N(a,n,15));
      return vreal(0); }
    if(!strcmp(nm,"draw_background_stretched")){ if(R) gml_draw_background_stretched(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),0xFFFFFF,R->alpha); return vreal(0); }
    if(!strcmp(nm,"draw_background_stretched_ext")){ if(R) gml_draw_background_stretched(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),(uint32_t)N(a,n,5),N(a,n,6)); return vreal(0); }
    if(!strcmp(nm,"draw_background_part_ext")){ if(R) gml_draw_background_part_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),N(a,n,7),N(a,n,8),(uint32_t)N(a,n,9),N(a,n,10)); return vreal(0); }
    if(!strcmp(nm,"draw_rectangle")||!strcmp(nm,"draw_rectangle_colour")||!strcmp(nm,"draw_rectangle_color")){
      if(R){ int plain=!strcmp(nm,"draw_rectangle"); int outline=(int)N(a,n,plain?4:8);
        int x1=(int)floor(N(a,n,0)-R->cam_x), y1=(int)floor(N(a,n,1)-R->cam_y);
        int x2=(int)ceil(N(a,n,2)-R->cam_x), y2=(int)ceil(N(a,n,3)-R->cam_y);
        if(plain) draw_rect_prim(R,x1,y1,x2,y2,R->color,outline);
        else draw_rect_colour_prim(R,x1,y1,x2,y2,(uint32_t)N(a,n,4),(uint32_t)N(a,n,5),(uint32_t)N(a,n,6),(uint32_t)N(a,n,7),outline); }
      return vreal(0); }
    if(!strcmp(nm,"draw_point")){ if(R){ gml_render_maybe_prepare_draw(R); draw_px(R,(int)floor(N(a,n,0)-R->cam_x),(int)floor(N(a,n,1)-R->cam_y),R->color); } return vreal(0); }
    if(!strcmp(nm,"draw_point_color")||!strcmp(nm,"draw_point_colour")){ if(R){ gml_render_maybe_prepare_draw(R); draw_px(R,(int)floor(N(a,n,0)-R->cam_x),(int)floor(N(a,n,1)-R->cam_y),(uint32_t)N(a,n,2)); } return vreal(0); }
    if(!strcmp(nm,"draw_line")||!strcmp(nm,"draw_line_color")||!strcmp(nm,"draw_line_colour")||!strcmp(nm,"draw_line_width")||!strcmp(nm,"draw_line_width_color")||!strcmp(nm,"draw_line_width_colour")){
      if(R){ int has_col=strstr(nm,"color")||strstr(nm,"colour"); int has_w=strstr(nm,"width")!=NULL;
        uint32_t col=has_col?(uint32_t)N(a,n,has_w?5:4):R->color; int width=has_w?(int)N(a,n,4):1;
        draw_line_prim(R,(int)floor(N(a,n,0)-R->cam_x),(int)floor(N(a,n,1)-R->cam_y),(int)floor(N(a,n,2)-R->cam_x),(int)floor(N(a,n,3)-R->cam_y),col,width); }
      return vreal(0); }
    if(!strcmp(nm,"draw_path")){
      if(R){ int pi=(int)N(a,n,0), absolute=(int)N(a,n,3);
        if(pi>=0 && pi<vm->n_paths && vm->paths[pi].n>1 && vm->paths[pi].pts){
          GmlPath *p=&vm->paths[pi];
          double ox=absolute?0:N(a,n,1), oy=absolute?0:N(a,n,2);
          for(int k=1;k<p->n;k++)
            draw_line_prim(R,(int)floor(p->pts[k-1].x+ox-R->cam_x),(int)floor(p->pts[k-1].y+oy-R->cam_y),
                             (int)floor(p->pts[k].x+ox-R->cam_x),(int)floor(p->pts[k].y+oy-R->cam_y),R->color,1);
          if(p->closed)
            draw_line_prim(R,(int)floor(p->pts[p->n-1].x+ox-R->cam_x),(int)floor(p->pts[p->n-1].y+oy-R->cam_y),
                             (int)floor(p->pts[0].x+ox-R->cam_x),(int)floor(p->pts[0].y+oy-R->cam_y),R->color,1);
        }
      }
      return vreal(0); }
    if(!strcmp(nm,"draw_circle")){ if(R){ gml_render_maybe_prepare_draw(R); draw_circle_prim(R,(int)floor(N(a,n,0)-R->cam_x),(int)floor(N(a,n,1)-R->cam_y),(int)N(a,n,2),(int)N(a,n,2),R->color,(int)N(a,n,3)); } return vreal(0); }
    if(!strcmp(nm,"draw_circle_color")||!strcmp(nm,"draw_circle_colour")){ if(R){
        gml_render_maybe_prepare_draw(R);
        draw_circle_prim(R,(int)floor(N(a,n,0)-R->cam_x),(int)floor(N(a,n,1)-R->cam_y),(int)N(a,n,2),(int)N(a,n,2),(uint32_t)N(a,n,3),(int)N(a,n,5)); }
      return vreal(0); }
    if(!strcmp(nm,"draw_ellipse_color")||!strcmp(nm,"draw_ellipse_colour")){ if(R){
        gml_render_maybe_prepare_draw(R);
        int x1=(int)floor(N(a,n,0)-R->cam_x), y1=(int)floor(N(a,n,1)-R->cam_y), x2=(int)floor(N(a,n,2)-R->cam_x), y2=(int)floor(N(a,n,3)-R->cam_y);
        draw_circle_prim(R,(x1+x2)/2,(y1+y2)/2,abs(x2-x1)/2,abs(y2-y1)/2,(uint32_t)N(a,n,4),(int)N(a,n,6)); } return vreal(0); }
    if(!strcmp(nm,"draw_ellipse")){ if(R){
        gml_render_maybe_prepare_draw(R);
        int x1=(int)floor(N(a,n,0)-R->cam_x), y1=(int)floor(N(a,n,1)-R->cam_y), x2=(int)floor(N(a,n,2)-R->cam_x), y2=(int)floor(N(a,n,3)-R->cam_y);
        draw_circle_prim(R,(x1+x2)/2,(y1+y2)/2,abs(x2-x1)/2,abs(y2-y1)/2,R->color,(int)N(a,n,4)); } return vreal(0); }
    if(!strcmp(nm,"draw_roundrect")){ if(R) draw_rect_prim(R,(int)floor(N(a,n,0)-R->cam_x),(int)floor(N(a,n,1)-R->cam_y),(int)ceil(N(a,n,2)-R->cam_x),(int)ceil(N(a,n,3)-R->cam_y),R->color,(int)N(a,n,4)); return vreal(0); }
    if(!strcmp(nm,"draw_roundrect_color")||!strcmp(nm,"draw_roundrect_colour")||
       !strcmp(nm,"draw_roundrect_color_ext")||!strcmp(nm,"draw_roundrect_colour_ext")){
      if(R){ int ext=strstr(nm,"_ext")!=NULL; int outline=(int)N(a,n,ext?8:6);
        draw_rect_prim(R,(int)floor(N(a,n,0)-R->cam_x),(int)floor(N(a,n,1)-R->cam_y),
                       (int)ceil(N(a,n,2)-R->cam_x),(int)ceil(N(a,n,3)-R->cam_y),
                       (uint32_t)N(a,n,ext?6:4),outline); }
      return vreal(0); }
    if(!strcmp(nm,"draw_healthbar")){
      if(R){ double amt=N(a,n,4); if(amt<0) amt=0; if(amt>100) amt=100;
        int x1=(int)floor(N(a,n,0)-R->cam_x), y1=(int)floor(N(a,n,1)-R->cam_y);
        int x2=(int)ceil(N(a,n,2)-R->cam_x), y2=(int)ceil(N(a,n,3)-R->cam_y);
        if(x1>x2){ int t=x1; x1=x2; x2=t; } if(y1>y2){ int t=y1; y1=y2; y2=t; }
        if((int)N(a,n,9)) draw_rect_prim(R,x1,y1,x2,y2,(uint32_t)N(a,n,5),0);
        int dir=(int)N(a,n,8), fx1=x1,fy1=y1,fx2=x2,fy2=y2;
        if(dir==1) fx1=x2-(int)floor((x2-x1)*amt/100.0);
        else if(dir==2) fy2=y1+(int)floor((y2-y1)*amt/100.0);
        else if(dir==3) fy1=y2-(int)floor((y2-y1)*amt/100.0);
        else fx2=x1+(int)floor((x2-x1)*amt/100.0);
        draw_rect_prim(R,fx1,fy1,fx2,fy2,(uint32_t)N(a,n,7),0);
        if((int)N(a,n,10)) draw_rect_prim(R,x1,y1,x2,y2,0,1);
      }
      return vreal(0); }
    if(!strcmp(nm,"draw_triangle")||!strcmp(nm,"draw_triangle_color")||!strcmp(nm,"draw_triangle_colour")){
      if(R){ double x1=floor(N(a,n,0)-R->cam_x), y1=floor(N(a,n,1)-R->cam_y);
        double x2=floor(N(a,n,2)-R->cam_x), y2=floor(N(a,n,3)-R->cam_y);
        double x3=floor(N(a,n,4)-R->cam_x), y3=floor(N(a,n,5)-R->cam_y);
        uint32_t col=!strcmp(nm,"draw_triangle")?R->color:(uint32_t)N(a,n,6); int outline=(int)N(a,n,!strcmp(nm,"draw_triangle")?6:9);
        gml_render_maybe_prepare_draw(R);
        prim_tri_fill_ex(R,x1,y1,x2,y2,x3,y3,col,R->alpha,outline); }
      return vreal(0); }
    if(!strcmp(nm,"draw_clear")||!strcmp(nm,"draw_clear_alpha")){ if(R){
      /* Pass the requested alpha through when clearing the target surface. */
      uint32_t src=gm_color_to_xrgb((uint32_t)N(a,n,0));
      if(!strcmp(nm,"draw_clear_alpha") && n>=2){
        double ca=N(a,n,1); if(ca<0)ca=0; if(ca>1)ca=1;
        src=(src&0x00FFFFFFu)|((uint32_t)lround(ca*255.0)<<24);
      }
      gml_render_set_pending_fill(R,src);
    } return vreal(0); }
    if(!strcmp(nm,"draw_getpixel")){ if(R){ gml_render_maybe_prepare_draw(R); int x=(int)N(a,n,0), y=(int)N(a,n,1); if(x>=0&&y>=0&&x<R->fbw&&y<R->fbh){ uint32_t p=R->fb[(size_t)y*R->fbw+x]; return vreal(((p>>16)&0xff) | (p&0xff00) | ((p&0xff)<<16)); } } return vreal(0); }
    if(!strcmp(nm,"draw_enable_alphablend")){ if(R) R->alphablend=N(a,n,0)>=0.5; return vreal(0); }
    if(!strcmp(nm,"draw_enable_drawevent")){ vm->draw_events_off = N(a,n,0)<0.5; return vreal(0); }
    if(!strcmp(nm,"draw_surface_part")){ if(R) gml_draw_surface_part_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),1,1,0xFFFFFF,R->alpha); return vreal(0); }
    if(!strcmp(nm,"draw_get_font")) return vreal(R?R->font:-1);
    if(!strcmp(nm,"draw_get_color")||!strcmp(nm,"draw_get_colour")){ if(!R) return vreal(0);
      uint32_t c=R->color; return vreal((double)(((c>>16)&0xff)|(c&0xff00)|((c&0xff)<<16))); }
    if(!strcmp(nm,"draw_get_alpha")) return vreal(R?R->alpha:1.0);
    if(!strcmp(nm,"draw_get_halign")) return vreal(R?R->halign:0);
    if(!strcmp(nm,"draw_get_valign")) return vreal(R?R->valign:0);
    /* Draw points and lines directly; fill triangle lists, strips and fans with flat per-triangle color. */
    if(!strcmp(nm,"draw_primitive_begin")||!strcmp(nm,"draw_primitive_begin_texture")){
      g_prim_kind=(int)N(a,n,0); g_prim_n=0; return vreal(0); }
    if(!strcmp(nm,"draw_vertex")||!strcmp(nm,"draw_vertex_colour")||!strcmp(nm,"draw_vertex_color")||
       !strcmp(nm,"draw_vertex_texture")||!strcmp(nm,"draw_vertex_texture_colour")||!strcmp(nm,"draw_vertex_texture_color")){
      if(g_prim_n<GML_PRIM_MAX){
        int tex=!strncmp(nm,"draw_vertex_texture",19);
        int coli=tex?4:2;                                 /* texture form: (x,y,u,v[,col,alpha]) */
        g_prim_x[g_prim_n]=N(a,n,0); g_prim_y[g_prim_n]=N(a,n,1);
        g_prim_c[g_prim_n]=(n>coli)?(uint32_t)N(a,n,coli):(R?R->color:0xFFFFFF);
        g_prim_a[g_prim_n]=(n>coli+1)?N(a,n,coli+1):(R?R->alpha:1.0);
        g_prim_n++; }
      return vreal(0); }
    if(!strcmp(nm,"draw_primitive_end")){ if(R) prim_flush(R); g_prim_n=0; return vreal(0); }
    if(!strcmp(nm,"draw_set_circle_precision")){ if(R){ int p=(int)N(a,n,0); if(p<4) p=4; if(p>64) p=64; p=(p/4)*4; if(p<4) p=4; R->circle_precision=p; } return vreal(0); }
    if(!strcmp(nm,"application_surface")) return vreal(0);
    /* ---- FMOD Studio extension (fmod-gamemaker): games route ALL audio through it (their AUDO chunk
     * is empty; sound lives in external .bank files). We resolve the event path → GUID → FSB5 subsound
     * and play it through the FMOD software mixer (gml_fmod.c). If no bank set loaded (fb==NULL) we
     * still model the instance LIFECYCLE STATE so audio-gated sequences advance. Generic: any
     * GameMaker+FMOD title uses this same API. ---- */
    if(builtin_fmod_exact_name(nm)) return builtin_fmod(vm,nm,a,n);
    if(!strncmp(nm,"fmod_",5)) return builtin_fmod(vm,nm,a,n);
    if(!strcmp(nm,"surface_exists")) return vreal(R?gml_surface_exists(R,(int)N(a,n,0)):0);
    if(!strcmp(nm,"surface_create")) return vreal(R?gml_surface_create(R,(int)N(a,n,0),(int)N(a,n,1)):-1);
    if(!strcmp(nm,"surface_free")){ if(R) gml_surface_free(R,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"surface_get_target")) return vreal(R?gml_surface_get_target(R):-1);
    if(!strcmp(nm,"surface_get_texture")){
      int sid=(int)N(a,n,0);
      return vreal((R&&gml_surface_exists(R,sid))?(double)(GML_TEX_SURF_TAG | (sid&0xFFFF)):-1);
    }
    if(!strcmp(nm,"surface_set_target")){ if(getenv("GML_LOG_SURF"))fprintf(stderr,"[surf] set_target %d\n",(int)N(a,n,0)); return vreal(R?gml_surface_set_target(R,(int)N(a,n,0)):0); }
    if(!strcmp(nm,"surface_reset_target")){ if(getenv("GML_LOG_SURF"))fprintf(stderr,"[surf] reset_target\n"); if(R) gml_surface_reset_target(R); return vreal(0); }
    if(!strcmp(nm,"surface_resize")){ if(R) gml_surface_resize(R,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2)); return vreal(0); }
    if(!strcmp(nm,"surface_copy")){ if(R) gml_surface_copy(R,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),(int)N(a,n,3)); return vreal(0); }
    if(!strcmp(nm,"surface_save")||!strcmp(nm,"surface_save_part")) return vreal(0);
    if(!strcmp(nm,"application_surface_draw_enable")){ if(R) R->app_draw_enable=(int)N(a,n,0); return vreal(0); }
    if(!strcmp(nm,"application_surface_enable")){ if(R) R->app_draw_enable=(int)N(a,n,0); return vreal(0); }
    /* Map camera IDs to view indices and camera properties to view globals. */
    if(!strcmp(nm,"view_get_camera")||!strcmp(nm,"room_get_camera")) return vreal(N(a,n,0));
    if(!strcmp(nm,"view_set_camera")) return vreal(0);
    if(!strcmp(nm,"camera_create")) return vreal(0);
    if(!strcmp(nm,"camera_create_view")){ int c=0;
      gml_set_global_arr(vm,"view_xview",c,N(a,n,0)); gml_set_global_arr(vm,"view_yview",c,N(a,n,1));
      gml_set_global_arr(vm,"view_wview",c,N(a,n,2)); gml_set_global_arr(vm,"view_hview",c,N(a,n,3));
      if(n>5) gml_set_global_arr(vm,"view_object",c,N(a,n,5));
      if(n>6) gml_set_global_arr(vm,"view_hspeed",c,N(a,n,6));
      if(n>7) gml_set_global_arr(vm,"view_vspeed",c,N(a,n,7));
      if(n>8) gml_set_global_arr(vm,"view_hborder",c,N(a,n,8));
      if(n>9) gml_set_global_arr(vm,"view_vborder",c,N(a,n,9));
      return vreal(c); }
    if(!strcmp(nm,"camera_destroy")||!strcmp(nm,"room_set_camera")||!strcmp(nm,"camera_set_view_angle")||
       !strcmp(nm,"camera_apply")||!strcmp(nm,"camera_set_default")) return vreal(0);
    if(!strcmp(nm,"camera_set_view_pos")){ int c=(int)N(a,n,0); gml_set_global_arr(vm,"view_xview",c,N(a,n,1)); gml_set_global_arr(vm,"view_yview",c,N(a,n,2)); return vreal(0); }
    if(!strcmp(nm,"camera_set_view_target")){ gml_set_global_arr(vm,"view_object",(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    /* Store viewport assignments in the corresponding view globals. */
    if(!strcmp(nm,"view_set_wport")){ gml_set_global_arr(vm,"view_wport",(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"view_set_hport")){ gml_set_global_arr(vm,"view_hport",(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"view_set_xport")){ gml_set_global_arr(vm,"view_xport",(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"view_set_yport")){ gml_set_global_arr(vm,"view_yport",(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"view_set_visible")){ gml_set_global_arr(vm,"view_visible",(int)N(a,n,0),N(a,n,1)>=0.5?1:0); return vreal(0); }
    if(!strcmp(nm,"camera_set_view_size")){ int c=(int)N(a,n,0); double cw=N(a,n,1),chh=N(a,n,2);
      if(cw>0) gml_set_global_arr(vm,"view_wview",c,cw); if(chh>0) gml_set_global_arr(vm,"view_hview",c,chh); return vreal(0); }
    if(!strcmp(nm,"camera_set_view_border")){ int c=(int)N(a,n,0); gml_set_global_arr(vm,"view_hborder",c,N(a,n,1)); gml_set_global_arr(vm,"view_vborder",c,N(a,n,2)); return vreal(0); }
    if(!strcmp(nm,"camera_set_view_speed")){ int c=(int)N(a,n,0); gml_set_global_arr(vm,"view_hspeed",c,N(a,n,1)); gml_set_global_arr(vm,"view_vspeed",c,N(a,n,2)); return vreal(0); }
    if(!strcmp(nm,"camera_get_view_x")) return vreal(gml_global_arr(vm,"view_xview",(int)N(a,n,0)));
    if(!strcmp(nm,"camera_get_view_y")) return vreal(gml_global_arr(vm,"view_yview",(int)N(a,n,0)));
    if(!strcmp(nm,"camera_get_view_width")) return vreal(gml_global_arr(vm,"view_wview",(int)N(a,n,0)));
    if(!strcmp(nm,"camera_get_view_height")) return vreal(gml_global_arr(vm,"view_hview",(int)N(a,n,0)));
    if(!strcmp(nm,"camera_get_view_target")) return vreal(gml_global_arr(vm,"view_object",(int)N(a,n,0)));
    if(!strcmp(nm,"camera_get_view_border_x")) return vreal(gml_global_arr(vm,"view_hborder",(int)N(a,n,0)));
    if(!strcmp(nm,"camera_get_view_border_y")) return vreal(gml_global_arr(vm,"view_vborder",(int)N(a,n,0)));
    if(!strcmp(nm,"camera_get_view_speed_x")) return vreal(gml_global_arr(vm,"view_hspeed",(int)N(a,n,0)));
    if(!strcmp(nm,"camera_get_view_speed_y")) return vreal(gml_global_arr(vm,"view_vspeed",(int)N(a,n,0)));
    if(!strcmp(nm,"camera_get_view_angle")||!strcmp(nm,"camera_get_default")||!strcmp(nm,"camera_get_active")) return vreal(0);
    /* Read view and viewport properties; fall back to view dimensions for an unset port. */
    if(!strcmp(nm,"view_get_xview")) return vreal(gml_global_arr(vm,"view_xview",(int)N(a,n,0)));
    if(!strcmp(nm,"view_get_yview")) return vreal(gml_global_arr(vm,"view_yview",(int)N(a,n,0)));
    if(!strcmp(nm,"view_get_wview")) return vreal(gml_global_arr(vm,"view_wview",(int)N(a,n,0)));
    if(!strcmp(nm,"view_get_hview")) return vreal(gml_global_arr(vm,"view_hview",(int)N(a,n,0)));
    /* Return the room viewport dimensions used by presentation and composition. */
    if(!strcmp(nm,"view_get_xport")) return vreal(gml_global_arr(vm,"view_xport",(int)N(a,n,0)));
    if(!strcmp(nm,"view_get_yport")) return vreal(gml_global_arr(vm,"view_yport",(int)N(a,n,0)));
    if(!strcmp(nm,"view_get_wport")){ int c=(int)N(a,n,0); double v=gml_global_arr(vm,"view_wport",c); return vreal(v>0?v:gml_global_arr(vm,"view_wview",c)); }
    if(!strcmp(nm,"view_get_hport")){ int c=(int)N(a,n,0); double v=gml_global_arr(vm,"view_hport",c); return vreal(v>0?v:gml_global_arr(vm,"view_hview",c)); }
    if(!strcmp(nm,"view_get_visible")) return vreal(1);
    /* Store the assigned view surface for frontend frame mirroring after the room pass. */
    if(!strcmp(nm,"view_get_surface_id")){ double v=gml_global_arr(vm,"view_surface_id",(int)N(a,n,0));
      return vreal(v!=0?v:-1); }
    if(!strcmp(nm,"view_set_surface_id")){ gml_set_global_arr(vm,"view_surface_id",(int)N(a,n,0),N(a,n,1));
      return vreal(0); }
    /* display DPI: return a real 96 (not 0) so `x / display_get_dpi_x()` scaling code never divides by 0. */
    if(!strcmp(nm,"display_get_dpi_x")||!strcmp(nm,"display_get_dpi_y")) return vreal(96);
    if(!strcmp(nm,"display_get_frequency")) return vreal(60.0);
    if(!strcmp(nm,"display_get_orientation")) return vreal(0);
    if(!strcmp(nm,"os_lock_orientation")) return vreal(0);
    if(!strcmp(nm,"game_set_speed")){
      double spd=N(a,n,0), mode=N(a,n,1);
      builtin_set_game_speed(vm, mode>=0.5 ? (spd>0 ? 1000000.0/spd : 60.0) : spd);
      return vreal(0);
    }
    if(!strcmp(nm,"game_get_speed")){
      double fps=builtin_game_speed(vm), mode=N(a,n,0);
      return vreal(mode>=0.5 ? 1000000.0/fps : fps);
    }
    /* Display/window size is the selected presentation size from the first startup call onward.
     * This keeps window-sized surfaces aligned with the frontend framebuffer across live changes. */
    if(!strcmp(nm,"display_get_width")||!strcmp(nm,"window_get_width"))
      return vreal(presentation_size(vm,R,0));
    if(!strcmp(nm,"display_get_height")||!strcmp(nm,"window_get_height"))
      return vreal(presentation_size(vm,R,1));
    if(!strcmp(nm,"display_get_gui_width"))
      return vreal(vm->gui_w>0? vm->gui_w : ((R&&R->fbw>0)? R->fbw : (vm->win&&vm->win->disp_w? (int)vm->win->disp_w : 288)));
    if(!strcmp(nm,"display_get_gui_height"))
      return vreal(vm->gui_h>0? vm->gui_h : ((R&&R->fbh>0)? R->fbh : (vm->win&&vm->win->disp_h? (int)vm->win->disp_h : 216)));
    if(!strcmp(nm,"get_timer")){
      /* Return the frame-based timer plus intra-frame CPU time in microseconds. */
      extern double gml_vm_get_timer_us(GmlVM*);
      return vreal(gml_vm_get_timer_us(vm));
    }
    if(!strcmp(nm,"surface_get_width")) return vreal(R?gml_surface_width(R,(int)N(a,n,0)):0);
    if(!strcmp(nm,"surface_get_height")) return vreal(R?gml_surface_height(R,(int)N(a,n,0)):0);
    if(!strcmp(nm,"surface_getpixel")){ int sid=(int)N(a,n,0), x=(int)N(a,n,1), y=(int)N(a,n,2);
      uint32_t p=0; int ok=0;
      if(R && sid==0 && R->app_surface && x>=0 && y>=0 && x<R->app_w && y<R->app_h){ p=R->app_surface[(size_t)y*R->app_w+x]; ok=1; }
      else if(R && sid>0 && sid<=GML_MAX_SURFACES){ GmlSurface *sf=&R->surface[sid-1];
        if(sf->live && sf->px && x>=0 && y>=0 && x<sf->w && y<sf->h){ p=sf->px[(size_t)y*sf->w+x]; ok=1; } }
      return vreal(ok?(((p>>16)&0xff)|(p&0xff00)|((p&0xff)<<16)):0); }
    if(!strcmp(nm,"sprite_get_width")){ int spr=(int)N(a,n,0); return vreal((R&&gml_sprite_exists(R,spr))?R->spr[spr].w:0); }
    if(!strcmp(nm,"sprite_get_height")){ int spr=(int)N(a,n,0); return vreal((R&&gml_sprite_exists(R,spr))?R->spr[spr].h:0); }
    if(!strcmp(nm,"sprite_get_number")){ int spr=(int)N(a,n,0); return vreal((R&&spr>=0&&spr<R->n_spr)?R->spr[spr].n_frames:0); }
    if(!strcmp(nm,"sprite_get_xoffset")){ int spr=(int)N(a,n,0); return vreal((R&&spr>=0&&spr<R->n_spr)?R->spr[spr].originx:0); }
    if(!strcmp(nm,"sprite_get_yoffset")){ int spr=(int)N(a,n,0); return vreal((R&&spr>=0&&spr<R->n_spr)?R->spr[spr].originy:0); }
    if(!strcmp(nm,"sprite_get_bbox_left")){ int spr=(int)N(a,n,0); return vreal((R&&spr>=0&&spr<R->n_spr)?R->spr[spr].ml:0); }
    if(!strcmp(nm,"sprite_get_bbox_top")){ int spr=(int)N(a,n,0); return vreal((R&&spr>=0&&spr<R->n_spr)?R->spr[spr].mt:0); }
    if(!strcmp(nm,"sprite_get_bbox_right")){ int spr=(int)N(a,n,0); return vreal((R&&spr>=0&&spr<R->n_spr)?R->spr[spr].mr:0); }
    if(!strcmp(nm,"sprite_get_bbox_bottom")){ int spr=(int)N(a,n,0); return vreal((R&&spr>=0&&spr<R->n_spr)?R->spr[spr].mb:0); }
    if(!strcmp(nm,"sprite_get_uvs")) return sprite_uvs(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)));
    if(!strcmp(nm,"texture_get_width")){ double uw; return vreal(texture_info(R,(int)N(a,n,0),&uw,NULL,NULL,NULL)?uw:0); }
    if(!strcmp(nm,"texture_get_height")){ double uh; return vreal(texture_info(R,(int)N(a,n,0),NULL,&uh,NULL,NULL)?uh:0); }
    if(!strcmp(nm,"texture_get_texel_width")){ double tw; return vreal(texture_info(R,(int)N(a,n,0),NULL,NULL,&tw,NULL)?tw:0); }
    if(!strcmp(nm,"texture_get_texel_height")){ double th; return vreal(texture_info(R,(int)N(a,n,0),NULL,NULL,NULL,&th)?th:0); }
    if(!strcmp(nm,"texture_get_uvs")) return texture_uvs(R,(int)N(a,n,0));
    if(!strcmp(nm,"sprite_exists")){ int spr=(int)N(a,n,0); return vreal(R&&gml_sprite_exists(R,spr)); }
    if(!strcmp(nm,"sprite_get_name")){ int spr=(int)N(a,n,0); return vstr((R&&spr>=0&&spr<R->n_spr&&R->spr[spr].name)?R->spr[spr].name:""); }
    if(!strcmp(nm,"sprite_add")){
      /* sprite_add(fname, imgnum, removeback, smooth, xorig, yorig): load a loose image
       * (localized charsets/signs in ports) as a runtime sprite; -1 when missing, like GM. */
      char *p=resolve_content_path(vm,S(a,n,0));
      int id = p? gml_sprite_add_file(R,p,(int)N(a,n,1),(int)N(a,n,2)>=1,(int)N(a,n,4),(int)N(a,n,5)) : -1;
      if(getenv("GML_LOG_SPRADD")) fprintf(stderr,"[sprite_add] '%s' -> %d\n",p?p:"?",id);
      free(p);
      return vreal(id);
    }
    if(!strcmp(nm,"font_add")){
      /* font_add(file, size, bold, italic, first, last): rasterize a loose TTF from the game
       * dir (localized fonts in ports). -1 when missing/unreadable, like GM. */
      char *p=resolve_content_path(vm,S(a,n,0));
      int id = p? gml_font_add_file(R,p,N(a,n,1)) : -1;
      free(p);
      return vreal(id);
    }
    /* Return the parsed parent object index, or the no-parent sentinel. */
  if(!strcmp(nm,"object_get_parent")){ int ob=(int)N(a,n,0);
    if(ob<0||ob>=vm->n_objects) return vreal(-100);
    int par=vm->objects[ob].parent;
    return vreal((par>=0&&par<vm->n_objects)?par:-100); }
  if(!strcmp(nm,"object_get_name")){ int ob=(int)N(a,n,0);
    return vstr((ob>=0&&ob<vm->n_objects&&vm->objects[ob].name)?vm->objects[ob].name:""); }
  if(!strcmp(nm,"object_get_sprite")){ int ob=(int)N(a,n,0);
    return vreal((ob>=0&&ob<vm->n_objects)?vm->objects[ob].sprite_index:-1); }
  if(!strcmp(nm,"object_get_mask")){ int ob=(int)N(a,n,0);
    return vreal((ob>=0&&ob<vm->n_objects)?vm->objects[ob].mask_index:-1); }
  if(!strcmp(nm,"object_is_ancestor")){ int obj=(int)N(a,n,0), anc=(int)N(a,n,1);
    for(int p=obj; p>=0 && p<vm->n_objects; p=vm->objects[p].parent) if(p==anc) return vreal(1);
    return vreal(0); }
  if(!strcmp(nm,"object_exists")){ int ob=(int)N(a,n,0); return vreal(ob>=0&&ob<vm->n_objects); }
  if(!strcmp(nm,"object_get_physics")) return vreal(0);
  if(!strcmp(nm,"asset_get_index")||!strcmp(nm,"sprite_get_index")||!strcmp(nm,"object_get_index")){
      /* Resolve asset names by searching sprites before objects; return -1 if absent. */
      const char *an=(n>0 && a[0].t==V_STR && a[0].s)? a[0].s : "";
      if(!strcmp(nm,"object_get_index")){ int oi=gml_object_index_by_name(vm,an); return vreal(oi); }
      if(R) for(int i=0;i<R->n_spr;i++) if(R->spr[i].name && !strcmp(R->spr[i].name,an)) return vreal(i);
      if(!strcmp(nm,"sprite_get_index")) return vreal(-1);
      { int oi=gml_object_index_by_name(vm,an); if(oi>=0) return vreal(oi); }
      return vreal(-1);
    }
    if(!strcmp(nm,"sprite_create_from_surface")){
      return vreal(R?gml_sprite_create_from_surface(R,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),
        (int)N(a,n,3),(int)N(a,n,4),(int)N(a,n,5),(int)N(a,n,6),(int)N(a,n,7),(int)N(a,n,8)):-1);
    }
    if(!strcmp(nm,"sprite_replace")){
      char *path=resolve_content_path(vm,S(a,n,1));
      int ok=R?gml_sprite_replace_from_file(R,(int)N(a,n,0),path,(int)N(a,n,2),(int)N(a,n,3),(int)N(a,n,4),(int)N(a,n,5),(int)N(a,n,6)):0;
      free(path);
      return vreal(ok);
    }
    if(!strcmp(nm,"sprite_delete")){ if(R) gml_sprite_delete(R,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"sprite_duplicate")) return vreal(R?gml_sprite_duplicate(R,(int)N(a,n,0)):-1);
    if(!strcmp(nm,"sprite_set_alpha_from_sprite")) return vreal(R?gml_sprite_set_alpha_from_sprite(R,(int)N(a,n,0),(int)N(a,n,1)):0);
    if(!strcmp(nm,"sprite_set_offset")){ if(R) gml_sprite_set_offset(R,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2)); return vreal(0); }
    if(!strcmp(nm,"sprite_save")) return vreal(0);
    if(!strcmp(nm,"sprite_save_strip")) return vreal(0);
    if(!strcmp(nm,"sprite_collision_mask")){
      if(R) gml_sprite_collision_mask(R,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),
        (int)N(a,n,3),(int)N(a,n,4),(int)N(a,n,5),(int)N(a,n,6),(int)N(a,n,7),(int)N(a,n,8));
      return vreal(0);
    }
    if(!strcmp(nm,"sprite_prefetch")) return vreal(0);
    if(!strcmp(nm,"background_add")||!strcmp(nm,"background_create_color")) return vreal(-1);
    if(!strcmp(nm,"background_replace")||!strcmp(nm,"background_delete")||!strcmp(nm,"background_save")) return vreal(0);
    if(!strcmp(nm,"background_get_width")){ int bg=(int)N(a,n,0); if(R&&bg>=0&&bg<R->n_bg){ int ti=R->bg[bg].tpag; if(ti>=0&&ti<R->n_tpag) return vreal(R->tpag[ti].bw?R->tpag[ti].bw:R->tpag[ti].sw); } return vreal(0); }
    if(!strcmp(nm,"background_get_height")){ int bg=(int)N(a,n,0); if(R&&bg>=0&&bg<R->n_bg){ int ti=R->bg[bg].tpag; if(ti>=0&&ti<R->n_tpag) return vreal(R->tpag[ti].bh?R->tpag[ti].bh:R->tpag[ti].sh); } return vreal(0); }
    if(!strcmp(nm,"background_get_texture")){ int bg=(int)N(a,n,0);
      return vreal(R&&bg>=0&&bg<R->n_bg ? (double)(GML_TEX_BG_TAG|(bg&0x00FFFFFF)) : -1); }
    if(!strcmp(nm,"draw_text")){ if(R) gml_draw_text(R,N(a,n,0),N(a,n,1),S(a,n,2));
      if(getenv("GML_DBG_TEXT")){ extern long g_vm_frame;
        fprintf(stderr,"[text] f%ld font=%d (%.0f,%.0f) \"%s\"\n",g_vm_frame,R?R->font:-1,N(a,n,0),N(a,n,1),S(a,n,2)); }
      return vreal(0); }
    /* Draw text using the first corner color as a flat tint and the requested alpha. */
    if(!strcmp(nm,"draw_text_color")||!strcmp(nm,"draw_text_colour")){
      if(R){ uint32_t c=R->color; double al=R->alpha;
        R->color=(uint32_t)N(a,n,3); R->alpha=N(a,n,7);
        gml_draw_text(R,N(a,n,0),N(a,n,1),S(a,n,2));
        R->color=c; R->alpha=al; }
      return vreal(0); }
    if(!strcmp(nm,"draw_text_ext")){ if(R) gml_draw_text_ext(R,N(a,n,0),N(a,n,1),S(a,n,2),N(a,n,3),N(a,n,4)); return vreal(0); }
    if(!strcmp(nm,"draw_text_ext_colour")||!strcmp(nm,"draw_text_ext_color")){
      if(R){ uint32_t c=R->color; double al=R->alpha;
        R->color=(uint32_t)N(a,n,5); R->alpha=N(a,n,9);
        gml_draw_text_ext(R,N(a,n,0),N(a,n,1),S(a,n,2),N(a,n,3),N(a,n,4));
        R->color=c; R->alpha=al; }
      return vreal(0); }
    if(!strcmp(nm,"draw_text_transformed")){
      if(R) gml_draw_text_transformed(R,N(a,n,0),N(a,n,1),S(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),R->color,R->alpha);
      return vreal(0);
    }
    if(!strcmp(nm,"draw_text_transformed_color")||!strcmp(nm,"draw_text_transformed_colour")){
      if(R) gml_draw_text_transformed(R,N(a,n,0),N(a,n,1),S(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),(uint32_t)N(a,n,6),N(a,n,10));
      return vreal(0);
    }
    /* Extended text variants shift transform/color arguments after sep and width, which this path ignores. */
    if(!strcmp(nm,"draw_text_ext_transformed")){
      if(R) gml_draw_text_transformed(R,N(a,n,0),N(a,n,1),S(a,n,2),N(a,n,5),N(a,n,6),N(a,n,7),R->color,R->alpha);
      return vreal(0);
    }
    if(!strcmp(nm,"draw_text_ext_transformed_color")||!strcmp(nm,"draw_text_ext_transformed_colour")){
      if(R) gml_draw_text_transformed(R,N(a,n,0),N(a,n,1),S(a,n,2),N(a,n,5),N(a,n,6),N(a,n,7),(uint32_t)N(a,n,8),N(a,n,12));
      return vreal(0);
    }
    if(!strcmp(nm,"string_width")) return vreal(R?gml_text_width(R,S(a,n,0)):(int)strlen(S(a,n,0))*8);
    if(!strcmp(nm,"string_height")) return vreal(R?gml_text_height(R,S(a,n,0)):8);
    if(!strcmp(nm,"string_width_ext")) return vreal(R?gml_text_width(R,S(a,n,0)):(int)strlen(S(a,n,0))*8);
    if(!strcmp(nm,"string_height_ext")) return vreal(R?gml_text_height(R,S(a,n,0)):8);
    if(!strcmp(nm,"font_add_sprite")) return vreal(R? gml_font_add_sprite(R,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),(int)N(a,n,3)) : 0);
    if(!strcmp(nm,"font_add_sprite_ext"))
      return vreal(R? gml_font_add_sprite_ext(R,(int)N(a,n,0),S(a,n,1),(int)N(a,n,2),(int)N(a,n,3)) : 0);
    if(!strcmp(nm,"font_add_enable_aa")) return vreal(0);
    if(!strcmp(nm,"font_get_size")){
      int fid=(int)N(a,n,0);
      if(R && fid<0) return vreal(R->default_font.line_height);
      return vreal((R && fid>=0 && fid<R->n_fonts && R->fonts[fid].line_height>0)?R->fonts[fid].line_height:0);
    }
    if(!strcmp(nm,"font_delete")){
      int fid=(int)N(a,n,0);
      if(R && fid>=0 && fid<R->n_fonts && !R->fonts[fid].real){
        free(R->fonts[fid].map);
        free(R->fonts[fid].glyphs);
        memset(&R->fonts[fid],0,sizeof(R->fonts[fid]));
      }
      return vreal(0);
    }
    if(!strcmp(nm,"font_get_info")){
      GmlInstance *st=gml_struct_new(vm);
      if(!st) return vreal(0);
      int fid=(int)N(a,n,0), spr=-1, first=0, prop=0, sep=0;
      if(R && fid>=0 && fid<R->n_fonts && !R->fonts[fid].real){
        spr=R->fonts[fid].sprite;
        first=R->fonts[fid].first;
        prop=R->fonts[fid].prop;
        sep=R->fonts[fid].sep;
      }
      *gml_varmap_put(&st->vars,"spriteIndex")=vreal(spr);
      *gml_varmap_put(&st->vars,"first")=vreal(first);
      *gml_varmap_put(&st->vars,"prop")=vreal(prop);
      *gml_varmap_put(&st->vars,"sep")=vreal(sep);
      return vreal((double)st->id);
    }
  }

  /* ---- input (wired later; held/pressed/released) ---- */
  { GmlVal v; if(builtin_input_kbgp(nm,a,n,&v)) return v; }
  /* ---- mouse (RetroArch pointer/mouse device via gml_input_mouse) ---- */
  if(!strcmp(nm,"mouse_check_button"))          return vreal(mouse_btn_check((int)N(a,n,0),0));
  if(!strcmp(nm,"mouse_check_button_pressed"))  return vreal(mouse_btn_check((int)N(a,n,0),1));
  if(!strcmp(nm,"mouse_check_button_released")) return vreal(mouse_btn_check((int)N(a,n,0),2));
  if(!strcmp(nm,"device_mouse_check_button"))          return vreal(mouse_btn_check((int)N(a,n,1),0));
  if(!strcmp(nm,"device_mouse_check_button_pressed"))  return vreal(mouse_btn_check((int)N(a,n,1),1));
  if(!strcmp(nm,"device_mouse_check_button_released")) return vreal(mouse_btn_check((int)N(a,n,1),2));
  if(!strcmp(nm,"device_mouse_x")){ double v; gml_input_mouse(&v,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
  if(!strcmp(nm,"device_mouse_y")){ double v; gml_input_mouse(NULL,&v,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
  if(!strcmp(nm,"device_mouse_x_to_gui")){ double v; gml_input_mouse(NULL,NULL,&v,NULL,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
  if(!strcmp(nm,"device_mouse_y_to_gui")){ double v; gml_input_mouse(NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
  if(!strcmp(nm,"device_mouse_raw_x")||!strcmp(nm,"window_mouse_get_x")){ double v; gml_input_mouse(NULL,NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
  if(!strcmp(nm,"device_mouse_raw_y")||!strcmp(nm,"window_mouse_get_y")){ double v; gml_input_mouse(NULL,NULL,NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL); return vreal(v); }
  if(!strcmp(nm,"display_mouse_get_x")){ double v; gml_input_mouse(NULL,NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
  if(!strcmp(nm,"display_mouse_get_y")){ double v; gml_input_mouse(NULL,NULL,NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL); return vreal(v); }
  if(!strcmp(nm,"display_mouse_set")){ gml_input_mouse_set(N(a,n,0),N(a,n,1)); return vreal(0); }
  if(!strcmp(nm,"window_mouse_set")) return vreal(0);
  if(!strcmp(nm,"joystick_exists")){
    int joy=(int)N(a,n,0), dev=joy>0?joy-1:joy;
    return vreal(gml_input_gamepad_connected(dev)); }
  if(!strcmp(nm,"joystick_name")){
    int joy=(int)N(a,n,0), dev=joy>0?joy-1:joy;
    return vstr(gml_input_gamepad_connected(dev) ? "libretro" : ""); }
  if(!strcmp(nm,"joystick_buttons")) return vreal(16);
  if(!strcmp(nm,"joystick_axes")) return vreal(2);
  if(!strcmp(nm,"joystick_check_button")){ int b=(int)N(a,n,1);
    return vreal((b>=1 && b<=16) ? gml_input_gamepad(32768+b,0) : 0); }
  if(!strcmp(nm,"joystick_xpos")) return vreal(gml_input_gamepad(32784,0) - gml_input_gamepad(32783,0));
  if(!strcmp(nm,"joystick_ypos")) return vreal(gml_input_gamepad(32782,0) - gml_input_gamepad(32781,0));
  if(!strcmp(nm,"joystick_direction")){
    int x=gml_input_gamepad(32784,0) - gml_input_gamepad(32783,0);
    int y=gml_input_gamepad(32782,0) - gml_input_gamepad(32781,0);
    if(x<0 && y<0) return vreal(103); if(x>0 && y<0) return vreal(105);
    if(x<0 && y>0) return vreal(97);  if(x>0 && y>0) return vreal(99);
    if(x<0) return vreal(100); if(x>0) return vreal(102);
    if(y<0) return vreal(104); if(y>0) return vreal(98);
    return vreal(101);
  }
  if(!strcmp(nm,"joystick_zpos")||!strcmp(nm,"joystick_rpos")||!strcmp(nm,"joystick_upos")||!strcmp(nm,"joystick_vpos")) return vreal(0);
  if(!strcmp(nm,"joystick_has_pov")) return vreal(0);
  if(!strcmp(nm,"joystick_pov")) return vreal(-1);

  /* ---- file / buffer runtime I/O ---- */
  if(!strcmp(nm,"FS_set_gm_save_area")||!strcmp(nm,"FS_set_working_directory")) return vreal(1);
  if(!strcmp(nm,"FS_export_image")||!strcmp(nm,"FS_export_image_adv")||
     !strcmp(nm,"FS_export_raw")||!strcmp(nm,"FS_import_image")) return vreal(0);
  if(!strcmp(nm,"FS_unique_fname")){
    const char *raw=S(a,n,0);
    if(!raw[0]) return vstr("");
    char *path=resolve_content_path(vm,raw);
    struct stat st;
    if(!path || stat(path,&st)!=0){ free(path); return vstr_owned(strdup(raw)); }
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
      char *full=resolve_content_path(vm,cand);
      int exists=full && stat(full,&st)==0;
      free(full);
      if(!exists) return vstr_owned(cand);
      free(cand);
    }
    return vstr_owned(strdup(raw));
  }
  if(!strcmp(nm,"file_exists")||!strcmp(nm,"FS_file_exists")){ char *path=resolve_content_path(vm,S(a,n,0));
    /* must be a REGULAR file: fopen() on Linux happily opens directories, so an empty/garbage
     * path (e.g. from an unset variable) reported "exists" and games loaded phantom configs */
    struct stat st; int ok = path && stat(path,&st)==0 && S_ISREG(st.st_mode);
    free(path); return vreal(ok); }
  if(!strcmp(nm,"directory_exists")){ char *path=resolve_content_path(vm,S(a,n,0));
    struct stat st; int ok = path && stat(path,&st)==0 && S_ISDIR(st.st_mode);
    free(path); return vreal(ok); }
  if(!strcmp(nm,"directory_create")){ char *path=resolve_content_path(vm,S(a,n,0)); int ok=0;
    if(path && path[0]){
      ok=(GML_MKDIR(path)==0);
      if(!ok){ struct stat st; ok=stat(path,&st)==0 && S_ISDIR(st.st_mode); }
    }
    free(path); return vreal(ok); }
  if(!strcmp(nm,"directory_destroy")){ char *path=resolve_content_path(vm,S(a,n,0)); int ok=path&&GML_RMDIR(path)==0; free(path); return vreal(ok); }
  if(!strcmp(nm,"file_delete")){ char *path=resolve_content_path(vm,S(a,n,0)); int ok=remove(path)==0; free(path); return vreal(ok); }
  if(!strcmp(nm,"file_copy")||!strcmp(nm,"FS_file_copy")||!strcmp(nm,"FS_copy_fast")){
    char *src=resolve_content_path(vm,S(a,n,0)); char *dst=resolve_content_path(vm,S(a,n,1));
    int ok=copy_file_path(src,dst);
    free(src); free(dst); return vreal(ok); }
  /* Scan a directory with a glob mask, returning basenames or an empty string at the end. */
  if(!strcmp(nm,"file_find_first")){
    ff_reset();
    char *mask=resolve_content_path(vm,S(a,n,0));
    char *slash=strrchr(mask,'/');
    const char *pat = slash ? slash+1 : mask;
    char dirbuf[1024];
    if(slash){ size_t dl=(size_t)(slash-mask); if(dl>=sizeof dirbuf) dl=sizeof dirbuf-1;
      memcpy(dirbuf,mask,dl); dirbuf[dl]=0; } else snprintf(dirbuf,sizeof dirbuf,".");
    DIR *dd=opendir(dirbuf);
    if(dd){ struct dirent *de;
      while((de=readdir(dd)) && g_ff_n<GML_FF_MAX){
        if(de->d_name[0]=='.') continue;
        if(wild_match(pat,de->d_name)) g_ff_name[g_ff_n++]=strdup(de->d_name);
      }
      closedir(dd);
      /* deterministic order (readdir order is fs-dependent) */
      for(int i=0;i<g_ff_n;i++) for(int j=i+1;j<g_ff_n;j++)
        if(strcmp(g_ff_name[j],g_ff_name[i])<0){ char *t=g_ff_name[i]; g_ff_name[i]=g_ff_name[j]; g_ff_name[j]=t; }
    }
    free(mask); g_ff_idx=0;
    return g_ff_n>0 ? vstr_owned(strdup(g_ff_name[0])) : vstr("");
  }
  if(!strcmp(nm,"file_find_next")){
    g_ff_idx++;
    return (g_ff_idx<g_ff_n) ? vstr_owned(strdup(g_ff_name[g_ff_idx])) : vstr("");
  }
  if(!strcmp(nm,"file_find_close")){ ff_reset(); return vreal(0); }
  if(!strcmp(nm,"file_rename")){ char *oldp=resolve_content_path(vm,S(a,n,0)); char *newp=resolve_content_path(vm,S(a,n,1));
    int ok=rename(oldp,newp)==0; free(oldp); free(newp); return vreal(ok); }
  if(!strcmp(nm,"game_save")){
    char *path=resolve_content_path(vm,S(a,n,0));
    size_t size=gml_vm_state_size(vm), written=0; unsigned char *data=size?malloc(size):NULL;
    int ok=path && data && gml_vm_state_save(vm,data,size,&written) && written==size;
    FILE *file=ok?fopen(path,"wb"):NULL;
    if(!file || fwrite(data,1,written,file)!=written) ok=0;
    if(file) fclose(file); free(data); free(path); (void)ok;
    return vreal(0);
  }
  if(!strcmp(nm,"game_load")){
    char *path=resolve_content_path(vm,S(a,n,0)); FILE *file=path?fopen(path,"rb"):NULL;
    long length=-1; unsigned char *data=NULL; int ok=0;
    if(file && !fseek(file,0,SEEK_END) && (length=ftell(file))>0 && length<=268435456L &&
       !fseek(file,0,SEEK_SET)){
      data=(unsigned char*)malloc((size_t)length);
      if(data && fread(data,1,(size_t)length,file)==(size_t)length){ size_t used=0;
        ok=gml_vm_state_load(vm,data,(size_t)length,&used) && used==(size_t)length; }
    }
    if(file) fclose(file); free(data); free(path); (void)ok;
    return vreal(0);
  }
  if(!strcmp(nm,"file_bin_open")||!strcmp(nm,"FS_file_bin_open")){ char *path=resolve_content_path(vm,S(a,n,0)); int mode=(int)N(a,n,1);
    const char *fm = mode==1 ? "wb+" : (mode==2 ? "ab+" : "rb");
    int id=vm_file_open(vm,path,fm); if(id<0 && mode==1) id=vm_file_open(vm,path,"wb+"); free(path); return vreal(id); }
  if(!strcmp(nm,"file_bin_close")||!strcmp(nm,"FS_file_bin_close")){ int i=vm_file_slot(vm,(int)N(a,n,0));
    if(i>=0){ fclose((FILE*)vm->bin_file[i]); vm->bin_file[i]=NULL; } return vreal(0); }
  if(!strcmp(nm,"file_bin_size")||!strcmp(nm,"FS_file_bin_size")){ int i=vm_file_slot(vm,(int)N(a,n,0)); if(i<0) return vreal(0);
    FILE *f=(FILE*)vm->bin_file[i]; long p=ftell(f); fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,p,SEEK_SET);
    return vreal(sz<0?0:sz); }
  if(!strcmp(nm,"file_bin_seek")||!strcmp(nm,"FS_file_bin_seek")){ int i=vm_file_slot(vm,(int)N(a,n,0));
    if(i>=0){ long p=(long)N(a,n,1); if(p<0) p=0; fseek((FILE*)vm->bin_file[i],p,SEEK_SET); } return vreal(0); }
  if(!strcmp(nm,"file_bin_read_byte")||!strcmp(nm,"FS_file_bin_read_byte")){ int i=vm_file_slot(vm,(int)N(a,n,0)); if(i<0) return vreal(0);
    int c=fgetc((FILE*)vm->bin_file[i]); return vreal(c==EOF?0:(c&0xff)); }
  if(!strcmp(nm,"file_bin_write_byte")||!strcmp(nm,"FS_file_bin_write_byte")){ int i=vm_file_slot(vm,(int)N(a,n,0)); if(i>=0) fputc(((int)N(a,n,1))&0xff,(FILE*)vm->bin_file[i]); return vreal(0); }

  if(!strcmp(nm,"file_text_open_read")) return builtin_file_text_open_read(vm,a,n);
  if(!strcmp(nm,"file_text_open_write")){ char *path=resolve_content_path(vm,S(a,n,0)); int id=vm_file_open(vm,path,"w"); free(path); return vreal(id); }
  if(!strcmp(nm,"file_text_open_append")){ char *path=resolve_content_path(vm,S(a,n,0)); int id=vm_file_open(vm,path,"a+"); free(path); return vreal(id); }
  if(!strcmp(nm,"file_text_close")){ int i=vm_file_slot(vm,(int)N(a,n,0));
    if(i>=0){ fclose((FILE*)vm->bin_file[i]); vm->bin_file[i]=NULL; } return vreal(0); }
  if(!strcmp(nm,"file_text_eof")){ int i=vm_file_slot(vm,(int)N(a,n,0)); if(i<0) return vreal(1);
    FILE *f=(FILE*)vm->bin_file[i]; int c=fgetc(f); if(c==EOF) return vreal(1); ungetc(c,f); return vreal(0); }
  if(!strcmp(nm,"file_text_read_real")){ int i=vm_file_slot(vm,(int)N(a,n,0)); double v=0; if(i>=0) fscanf((FILE*)vm->bin_file[i],"%lf",&v); return vreal(v); }
  if(!strcmp(nm,"file_text_read_string")){
    /* Read the remainder of the current line, retaining leading spaces and leaving its newline unread. */
    return builtin_file_text_read_string(vm,a,n); }
  if(!strcmp(nm,"file_text_readln")){
    /* Return the remaining line content without its newline, and advance past that newline. */
    return builtin_file_text_readln(vm,a,n); }
  if(!strcmp(nm,"file_text_write_real")){ int i=vm_file_slot(vm,(int)N(a,n,0)); if(i>=0) fprintf((FILE*)vm->bin_file[i],"%g",N(a,n,1)); return vreal(0); }
  if(!strcmp(nm,"file_text_write_string")){ int i=vm_file_slot(vm,(int)N(a,n,0)); if(i>=0) fputs(S(a,n,1),(FILE*)vm->bin_file[i]); return vreal(0); }
  if(!strcmp(nm,"file_text_writeln")){ int i=vm_file_slot(vm,(int)N(a,n,0)); if(i>=0) fputc('\n',(FILE*)vm->bin_file[i]); return vreal(0); }

  if(!strcmp(nm,"buffer_create")){ int id=buffer_alloc(vm,(int)N(a,n,0)); return vreal(id); }
  if(!strcmp(nm,"buffer_base64_decode")){
    int len=0; unsigned char *bytes=base64_decode_alloc(S(a,n,0),&len);
    if(!bytes) return vreal(-1);
    int id=buffer_alloc(vm,len>0?len:1);
    int bi=vm_buffer_slot(vm,id);
    if(bi>=0){
      buffer_resize_slot(vm,bi,len);
      if(len>0) memcpy(vm->buffer[bi].data,bytes,(size_t)len);
      vm->buffer[bi].pos=0;
    }
    free(bytes);
    return vreal(id);
  }
  if(!strcmp(nm,"buffer_delete")){ int i=vm_buffer_slot(vm,(int)N(a,n,0));
    if(i>=0){ free(vm->buffer[i].data); memset(&vm->buffer[i],0,sizeof(vm->buffer[i])); } return vreal(0); }
  if(!strcmp(nm,"buffer_get_size")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); return vreal(i>=0?vm->buffer[i].size:0); }
  if(!strcmp(nm,"buffer_md5")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); if(i<0) return md5_hex_val(NULL,0);
    int off=(int)N(a,n,1), sz=(int)N(a,n,2);
    if(off<0) off=0;
    if(off>vm->buffer[i].size) off=vm->buffer[i].size;
    if(sz<0 || off+sz>vm->buffer[i].size) sz=vm->buffer[i].size-off;
    return md5_hex_val(vm->buffer[i].data+off,(size_t)sz); }
  if(!strcmp(nm,"buffer_get_address")){ int id=(int)N(a,n,0), i=vm_buffer_slot(vm,id);
    return vreal(i>=0 ? (double)(0xB0000000u | (uint32_t)(id&0xFFFF)) : 0); }
  if(!strcmp(nm,"buffer_tell")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); return vreal(i>=0?vm->buffer[i].pos:0); }
  if(!strcmp(nm,"buffer_resize")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); if(i>=0) buffer_resize_slot(vm,i,(int)N(a,n,1)); return vreal(0); }
  if(!strcmp(nm,"buffer_seek")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); if(i>=0){
      int base=(int)N(a,n,1), off=(int)N(a,n,2), pos=off;
      if(base==1) pos=vm->buffer[i].pos+off; else if(base==2) pos=vm->buffer[i].size+off;
      if(pos<0) pos=0; if(pos>vm->buffer[i].size) pos=vm->buffer[i].size; vm->buffer[i].pos=pos; }
    return vreal(0); }
  if(!strcmp(nm,"buffer_copy")){
    int si=vm_buffer_slot(vm,(int)N(a,n,0)), so=(int)N(a,n,1), bytes=(int)N(a,n,2);
    int di=vm_buffer_slot(vm,(int)N(a,n,3)), doff=(int)N(a,n,4);
    if(si>=0 && di>=0 && so>=0 && doff>=0 && bytes>0){
      if(so+bytes>vm->buffer[si].size) bytes=vm->buffer[si].size-so;
      if(bytes>0){
        unsigned char *tmp=malloc((size_t)bytes);
        if(tmp){ memcpy(tmp,vm->buffer[si].data+so,(size_t)bytes);
          buffer_write_at(vm,di,doff,tmp,bytes); free(tmp); }
      }
    }
    return vreal(0);
  }
  if(!strcmp(nm,"buffer_poke")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); if(i>=0)
      buffer_write_typed_at(vm,i,(int)N(a,n,1),(int)N(a,n,2),S(a,n,3),N(a,n,3));
    return vreal(0); }
  if(!strcmp(nm,"buffer_fill")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); int off=(int)N(a,n,1), type=(int)N(a,n,2), bytes=(int)N(a,n,4);
    if(i>=0 && off>=0 && bytes>0){
      int p=off, end=off+bytes;
      while(p<end){ int w=buffer_write_typed_at(vm,i,p,type,S(a,n,3),N(a,n,3)); if(w<=0) break; p+=w; }
    }
    return vreal(0); }
  /* Buffer types: u8, s8, u16, s16, u32, s32, f16, f32, f64, bool,
   * NUL-terminated string, u64 and unterminated text use IDs 1 through 13 respectively. */
  if(!strcmp(nm,"buffer_write")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); if(i<0) return vreal(0);
    int type=(int)N(a,n,1);
    if(type==11 || type==13){ const char *s=S(a,n,2); buffer_write_raw(vm,i,s,(int)strlen(s)+(type==11)); return vreal(0); }
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
    if(type==11){ int p=vm->buffer[i].pos, e=p;   /* buffer_string: NUL-terminated */
      while(e<vm->buffer[i].size && vm->buffer[i].data[e]) e++;
      char *s=dup_n((char*)vm->buffer[i].data+p,e-p);
      vm->buffer[i].pos=(e<vm->buffer[i].size)?e+1:e; return vstr_owned(s); }
    if(type==13){ int p=vm->buffer[i].pos, e=vm->buffer[i].size;   /* buffer_text: to end */
      char *s=dup_n((char*)vm->buffer[i].data+p,e-p);
      vm->buffer[i].pos=e; return vstr_owned(s); }
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
  /* Perform buffer file I/O synchronously, return a request ID, and queue Other_72
   * with its status for end-of-step dispatch. */
  if(!strcmp(nm,"buffer_save_async")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); const char *raw=S(a,n,1);
    int ok=0;
    if(i>=0 && raw[0]){ char *path=resolve_content_path(vm,raw); int off=(int)N(a,n,2), sz=(int)N(a,n,3);
      if(off<0) off=0;
      if(sz<=0||off+sz>vm->buffer[i].size) sz=vm->buffer[i].size-off;
      FILE *f=fopen(path,"wb"); if(f){ fwrite(vm->buffer[i].data+off,1,(size_t)sz,f); fclose(f); ok=1; }
      free(path); }
    int req=async_saveload_request(vm, ok);
    return vreal(req); }
  if(!strcmp(nm,"buffer_load_async")){ int i=vm_buffer_slot(vm,(int)N(a,n,0));
    char *path=resolve_content_path(vm,S(a,n,1)); int off=(int)N(a,n,2);
    if(off<0) off=0;
    int ok=0;
    FILE *f=path?fopen(path,"rb"):NULL;
    if(f && i>=0){
      fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
      if(sz==0){
        vm->buffer[i].pos=0;
        ok=1;
      } else if(sz>0){
        if(off+(int)sz > vm->buffer[i].cap){
          int nc=off+(int)sz; unsigned char *nd=realloc(vm->buffer[i].data,(size_t)nc);
          if(nd){ vm->buffer[i].data=nd;
            if(nc>vm->buffer[i].cap) memset(nd+vm->buffer[i].cap,0,(size_t)(nc-vm->buffer[i].cap));
            vm->buffer[i].cap=nc; }
        }
        if(off+(int)sz <= vm->buffer[i].cap){
          if(fread(vm->buffer[i].data+off,1,(size_t)sz,f)==(size_t)sz){
            if(off+(int)sz > vm->buffer[i].size) vm->buffer[i].size=off+(int)sz;
            vm->buffer[i].pos=0; ok=1;
          }
        }
      }
      fclose(f);
    } else if(f) fclose(f);
    free(path);
    int req=async_saveload_request(vm, ok);
    return vreal(req); }
  /* Synchronous buffer file I/O. */
  if(!strcmp(nm,"buffer_save")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); const char *raw=S(a,n,1);
    if(i>=0 && raw[0]){ char *path=resolve_content_path(vm,raw);
      FILE *f=fopen(path,"wb"); if(f){ fwrite(vm->buffer[i].data,1,(size_t)vm->buffer[i].size,f); fclose(f); }
      free(path); }
    return vreal(0); }
  if(!strcmp(nm,"buffer_save_ext")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); const char *raw=S(a,n,1);
    if(i>=0 && raw[0]){ char *path=resolve_content_path(vm,raw); int off=(int)N(a,n,2), sz=(int)N(a,n,3);
      if(off<0) off=0; if(sz<=0||off+sz>vm->buffer[i].size) sz=vm->buffer[i].size-off;
      FILE *f=fopen(path,"wb"); if(f && sz>0){ fwrite(vm->buffer[i].data+off,1,(size_t)sz,f); } if(f) fclose(f);
      free(path); }
    return vreal(0); }
  if(!strcmp(nm,"buffer_load")){ char *path=resolve_content_path(vm,S(a,n,0)); FILE *f=path?fopen(path,"rb"):NULL; if(!f){ free(path); return vreal(-1); }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); int id=buffer_alloc(vm,sz>0?(int)sz:1); int i=vm_buffer_slot(vm,id);
    if(i>=0 && sz>0){ if(fread(vm->buffer[i].data,1,(size_t)sz,f)!=(size_t)sz){} vm->buffer[i].size=(int)sz; vm->buffer[i].pos=0; }
    fclose(f); free(path); return vreal(id); }
  if(!strcmp(nm,"buffer_load_ext")){ int i=vm_buffer_slot(vm,(int)N(a,n,0)); char *path=resolve_content_path(vm,S(a,n,1)); int off=(int)N(a,n,2);
    FILE *f=path?fopen(path,"rb"):NULL; if(i<0||!f){ if(f)fclose(f); free(path); return vreal(0); }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(off<0)off=0;
    if(sz>0 && off+(int)sz<=vm->buffer[i].size){ if(fread(vm->buffer[i].data+off,1,(size_t)sz,f)!=(size_t)sz){} }
    fclose(f); free(path); return vreal(0); }
  if(!strcmp(nm,"buffer_async_group_begin")){
    vm->async_group_active=1;
    vm->async_group_id=++vm->async_seq;
    vm->async_group_status=1;
    vm->async_group_count=0;
    return vreal(0);
  }
  if(!strcmp(nm,"buffer_async_group_end")){
    if(!vm->async_group_active) return vreal(0);
    int req=vm->async_group_id;
    int ok=vm->async_group_status;
    int count=vm->async_group_count;
    vm->async_group_active=0;
    vm->async_group_id=0;
    vm->async_group_status=1;
    vm->async_group_count=0;
    if(count>0) async_saveload_queue(vm, req, ok);
    return vreal(req);
  }
  if(!strcmp(nm,"buffer_async_group_option")) return vreal(0);

  /* ---- script_execute(scriptid, args...) ---- */
  if(!strcmp(nm,"script_execute") || !strcmp(nm,"action_execute_script")){ int sid=(int)N(a,n,0);
    /* the argument may be a classic SCPT index or a GMS2.3 function value (tagged CODE index) */
    int ci = n>0 ? script_ref_code_of(vm,a[0]) : -1;
    if(getenv("GML_DBG_SCRIPTX")){ extern long g_vm_frame;
      fprintf(stderr,"[scriptx] f%ld sid=%d -> ci=%d (%s)\n",g_vm_frame,sid,ci,
        (ci>=0&&ci<vm->win->n_code)?vm->win->code[ci].name:"?"); }
    return gml_vm_run_code(vm,ci,vm->cur_self,vm->cur_other,a+1,n-1); }
  if(!strcmp(nm,"script_exists")){ int sid=(int)N(a,n,0);
    if(GML_IS_FUNCVAL(sid)) return vreal(1);
    return vreal(script_code_of(vm,sid)>=0); }
  if(!strcmp(nm,"script_get_name")){
    int sid=(int)N(a,n,0);
    int ci = GML_IS_FUNCVAL(sid) ? (sid & 0x00FFFFFF) : script_code_of(vm,sid);
    return vstr((vm->win&&ci>=0&&ci<vm->win->n_code&&vm->win->code[ci].name)?vm->win->code[ci].name:"");
  }

  /* ---- event dispatch ---- */
  if(!strcmp(nm,"event_inherited")||!strcmp(nm,"action_inherited")){
    gml_event_inherited(vm); return vreal(0); }
  if(!strcmp(nm,"event_user")){ char s[16]; snprintf(s,sizeof s,"Other_%d",10+(int)N(a,n,0));
    if(vm->cur_self) gml_run_event(vm,vm->cur_self,s); return vreal(0); }
  /* event_perform(type,numb): manually run one of THIS instance's events. GM event types:
   * 0 Create, 1 Destroy, 2 Alarm, 3 Step, 4 Collision, 7 Other, 8 Draw (numb = subtype/alarm/other obj). */
  if(!strcmp(nm,"event_perform")){
    if(vm->cur_self){ int ty=(int)N(a,n,0), nb=(int)N(a,n,1); const char *pre=0;
      switch(ty){ case 0:pre="Create";nb=0;break; case 1:pre="Destroy";nb=0;break; case 2:pre="Alarm";break;
        case 3:pre="Step";break; case 4:pre="Collision";break; case 7:pre="Other";break; case 8:pre="Draw";break; }
      if(pre){ char s[24]; snprintf(s,sizeof s,"%s_%d",pre,nb); gml_run_event(vm,vm->cur_self,s); } }
    return vreal(0); }
  if(!strcmp(nm,"event_perform_object")) return vreal(0);

  /* ---- audio ---- */
  { GmlAudio *AU=(GmlAudio*)vm->audio;
    if(!strcmp(nm,"action_sound")){ gml_audio_play(AU,(int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"audio_channel_num")){ gml_audio_channel_num(AU,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"sound_add")) return vreal(-1);
    if(!strcmp(nm,"sound_replace")) return vreal(0);
    if(!strcmp(nm,"audio_get_master_gain")) return vreal(gml_audio_get_master_gain(AU));
    if(!strcmp(nm,"audio_sound_get_gain")) return vreal(gml_audio_sound_get_gain(AU,(int)N(a,n,0)));
    if(!strcmp(nm,"audio_sound_get_pitch")) return vreal(gml_audio_sound_get_pitch(AU,(int)N(a,n,0)));
    if(!strcmp(nm,"audio_sound_get_track_position")) return vreal(gml_audio_sound_get_track_position(AU,(int)N(a,n,0)));
    if(!strcmp(nm,"audio_set_master_gain")){ gml_audio_set_master_gain(AU,n>=2?N(a,n,1):N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"audio_sound_gain")){ gml_audio_sound_gain(AU,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"audio_sound_pitch")){ gml_audio_sound_pitch(AU,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"audio_sound_set_track_position")){ gml_audio_sound_set_track_position(AU,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"audio_play_sound")||!strcmp(nm,"sound_play")||!strcmp(nm,"sound_loop")){
      int loop=!strcmp(nm,"sound_loop") ? 1 : (int)N(a,n,2);
      int h=gml_audio_play(AU,(int)N(a,n,0),loop);
      if(getenv("GML_LOG_AUDIO")) fprintf(stderr,"[audio] play_sound id=%d loop=%d -> handle=%d\n",(int)N(a,n,0),loop,h);
      return vreal(h); }
    if(!strcmp(nm,"audio_play_sound_at")){
      int h=gml_audio_play(AU,(int)N(a,n,0),(int)N(a,n,6));
      if(getenv("GML_LOG_AUDIO")) fprintf(stderr,"[audio] play_sound_at id=%d loop=%d -> handle=%d\n",(int)N(a,n,0),(int)N(a,n,6),h);
      return vreal(h); }
    if(!strcmp(nm,"audio_get_listener_count")) return vreal(1);
    if(!strcmp(nm,"audio_get_listener_info")) return arr8(0,0,0,0,0,1,0,1);
    /* Report audio groups as loaded through this status interface. */
    if(!strcmp(nm,"audio_group_is_loaded")) return vreal(1);
    if(!strcmp(nm,"audio_group_load_progress")) return vreal(1.0);
    if(!strcmp(nm,"audio_group_load")||!strcmp(nm,"audio_group_unload")) return vreal(1);
    if(!strcmp(nm,"audio_group_stop_all")) return vreal(0);
    if(!strcmp(nm,"audio_stop_sound")||!strcmp(nm,"sound_stop")){ gml_audio_stop(AU,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"audio_stop_all")||!strcmp(nm,"sound_stop_all")){ gml_audio_stop_all(AU); return vreal(0); }
    if(!strcmp(nm,"audio_pause_sound")){ gml_audio_pause_sound(AU,(int)N(a,n,0),1); return vreal(0); }
    if(!strcmp(nm,"audio_resume_sound")){ gml_audio_pause_sound(AU,(int)N(a,n,0),0); return vreal(0); }
    if(!strcmp(nm,"audio_pause_all")){ gml_audio_pause_all(AU,1); return vreal(0); }
    if(!strcmp(nm,"audio_resume_all")){ gml_audio_pause_all(AU,0); return vreal(0); }
    if(!strcmp(nm,"audio_is_playing")||!strcmp(nm,"sound_isplaying")) return vreal(gml_audio_is_playing(AU,(int)N(a,n,0)));

    /* Register external OGG audio through caster interfaces. The existing loader
     * takes the argument basename and prepends mus_ within the content directory. */
    if(!strcmp(nm,"caster_load")){
      const char *arg=S(a,n,0); const char *base=strrchr(arg,'/'); base=base?base+1:arg;
      char mus[300]; snprintf(mus,sizeof mus,"mus_%s",base);
      char *full=resolve_content_path(vm,mus); int handle=-1;
      FILE *f=full?fopen(full,"rb"):NULL;
      if(f){ fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        if(sz>0 && sz<64L*1024*1024){ uint8_t *buf=malloc((size_t)sz);
          if(buf && fread(buf,1,(size_t)sz,f)==(size_t)sz) handle=gml_audio_add_ogg(AU,buf,(int)sz);
          free(buf); }
        fclose(f); }
      if(getenv("GML_LOG_AUDIO")) fprintf(stderr,"[caster] load %s -> %s handle=%d\n",arg,full?full:"?",handle);
      free(full); return vreal(handle);
    }
    if(!strcmp(nm,"caster_play")||!strcmp(nm,"caster_play_l")||!strcmp(nm,"caster_loop")){
      int h=(int)N(a,n,0); double vol=n>=2?N(a,n,1):1.0, pit=n>=3?N(a,n,2):1.0;
      int loop=strcmp(nm,"caster_play")!=0;   /* caster_loop / caster_play_l repeat */
      gml_audio_sound_gain(AU,h,vol); gml_audio_sound_pitch(AU,h,pit);
      return vreal(gml_audio_play(AU,h,loop));
    }
    if(!strcmp(nm,"caster_stop")){ int h=(int)N(a,n,0); if(h<0) gml_audio_stop_all(AU); else gml_audio_stop(AU,h); return vreal(0); }
    if(!strcmp(nm,"caster_free")){ int h=(int)N(a,n,0); if(h<0) gml_audio_caster_free_all(AU); else gml_audio_caster_free(AU,h); return vreal(0); }
    if(!strcmp(nm,"caster_set_volume")){ gml_audio_sound_gain(AU,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"caster_get_volume")) return vreal(gml_audio_sound_get_gain(AU,(int)N(a,n,0)));
    if(!strcmp(nm,"caster_set_pitch")){ gml_audio_sound_pitch(AU,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"caster_get_pitch")) return vreal(gml_audio_sound_get_pitch(AU,(int)N(a,n,0)));
    if(!strcmp(nm,"caster_pause")){ int h=(int)N(a,n,0); if(h<0) gml_audio_pause_all(AU,1); else gml_audio_pause_sound(AU,h,1); return vreal(0); }
    if(!strcmp(nm,"caster_resume")){ int h=(int)N(a,n,0); if(h<0) gml_audio_pause_all(AU,0); else gml_audio_pause_sound(AU,h,0); return vreal(0); }
    if(!strcmp(nm,"caster_is_playing")) return vreal(gml_audio_is_playing(AU,(int)N(a,n,0)));
    if(!strncmp(nm,"caster_",7)) return vreal(0);   /* set_panning / get_length / etc.: graceful no-op */
    /* Model audio emitters as gain cells with create, gain, query and release operations. */
    if(!strcmp(nm,"audio_emitter_create")){
      for(int i=0;i<GML_MAX_EMITTERS;i++) if(!vm->emitter_live[i]){ vm->emitter_live[i]=1; vm->emitter_gain[i]=1.0; return vreal(3000000+i); }
      return vreal(-1); }
    if(!strcmp(nm,"audio_emitter_gain")){ int e=(int)N(a,n,0)-3000000;
      if(e>=0&&e<GML_MAX_EMITTERS&&vm->emitter_live[e]){ double g=N(a,n,1); vm->emitter_gain[e]=g<0?0:g; } return vreal(0); }
    if(!strcmp(nm,"audio_emitter_get_gain")){ int e=(int)N(a,n,0)-3000000;
      return vreal((e>=0&&e<GML_MAX_EMITTERS&&vm->emitter_live[e])?vm->emitter_gain[e]:1.0); }
    if(!strcmp(nm,"audio_emitter_free")){ int e=(int)N(a,n,0)-3000000;
      if(e>=0&&e<GML_MAX_EMITTERS) vm->emitter_live[e]=0; return vreal(0); }
    if(!strcmp(nm,"audio_emitter_exists")){ int e=(int)N(a,n,0)-3000000;
      return vreal(e>=0&&e<GML_MAX_EMITTERS&&vm->emitter_live[e]); }
    if(!strcmp(nm,"audio_play_sound_on")){   /* (emitter, snd, loop, priority) -> play scaled by emitter gain */
      int e=(int)N(a,n,0)-3000000; int h=gml_audio_play(AU,(int)N(a,n,1),(int)N(a,n,2));
      if(h>0&&e>=0&&e<GML_MAX_EMITTERS&&vm->emitter_live[e]) gml_audio_sound_gain(AU,h,vm->emitter_gain[e]);
      return vreal(h); }
    if(!strcmp(nm,"audio_is_paused")){
      /* paused state of a sound/voice: true only if some matching voice exists and is paused */
      return vreal(gml_audio_voice_paused(AU,(int)N(a,n,0))); }
    if(!strcmp(nm,"audio_get_type")) return vreal(0);          /* 0 = in-memory sample (all ours are) */
    if(!strcmp(nm,"audio_get_name")) return vstr("");
    if(!strcmp(nm,"audio_master_gain")){ gml_audio_set_master_gain(AU,N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"audio_group_set_gain")) return vreal(1);    /* groups not modeled; sounds embedded */
  }

  /* ---- paths (path_start / path_end): instance follows a PATH each step ---- */
  if(!strcmp(nm,"path_start")){ if(vm->cur_self)
      gml_path_start(vm,vm->cur_self,(int)N(a,n,0),N(a,n,1),N(a,n,2),(int)N(a,n,3)); return vreal(0); }
  if(!strcmp(nm,"action_path")){ if(vm->cur_self)
      gml_path_start(vm,vm->cur_self,(int)N(a,n,0),N(a,n,1),N(a,n,2),(int)N(a,n,3)); return vreal(0); }
  if(!strcmp(nm,"path_end")){ if(vm->cur_self) vm->cur_self->path_index=-1; return vreal(0); }

  /* ---- classic timelines ---- */
  if(!strcmp(nm,"timeline_exists")){ int ti=(int)N(a,n,0);
    return vreal(ti>=0 && ti<vm->n_timelines && vm->timelines[ti].name!=NULL); }
  if(!strcmp(nm,"timeline_get_name")){ int ti=(int)N(a,n,0);
    return vstr((ti>=0 && ti<vm->n_timelines && vm->timelines[ti].name)?vm->timelines[ti].name:""); }
  if(!strcmp(nm,"action_set_timeline")){ if(vm->cur_self){
      vm->cur_self->timeline_index=N(a,n,0); vm->cur_self->timeline_position=N(a,n,1);
      vm->cur_self->timeline_running=N(a,n,2)!=0; vm->cur_self->timeline_loop=N(a,n,3)!=0; }
    return vreal(0); }
  if(!strcmp(nm,"action_set_timeline_position")){ if(vm->cur_self) vm->cur_self->timeline_position=N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"action_set_timeline_speed")){ if(vm->cur_self) vm->cur_self->timeline_speed=N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"action_timeline_start")){ if(vm->cur_self) vm->cur_self->timeline_running=1; return vreal(0); }
  if(!strcmp(nm,"action_timeline_pause")){ if(vm->cur_self) vm->cur_self->timeline_running=0; return vreal(0); }
  if(!strcmp(nm,"action_timeline_stop")){ if(vm->cur_self){ vm->cur_self->timeline_running=0; vm->cur_self->timeline_position=0; } return vreal(0); }

  /* ---- tile-layer manipulation (mutations applied to the room's tiles at draw) ---- */
  if(!strcmp(nm,"tile_layer_delete")){ gml_tile_layer_delete(vm,(int)N(a,n,0)); return vreal(0); }
  if(!strcmp(nm,"tile_layer_depth")){ gml_tile_layer_depth(vm,(int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
  if(!strcmp(nm,"tile_layer_shift")){ gml_tile_layer_shift(vm,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
  if(!strcmp(nm,"tile_layer_delete_at")){ gml_tile_layer_delete_at(vm,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
  if(!strcmp(nm,"tile_layer_hide")){ gml_tile_layer_hide(vm,(int)N(a,n,0),1); return vreal(0); }
  if(!strcmp(nm,"tile_layer_show")){ gml_tile_layer_hide(vm,(int)N(a,n,0),0); return vreal(0); }

  /* ---- platform/service stubs: libretro/offline/software-renderer environment ---- */
  if(!strcmp(nm,"application_get_position")){
    GmlRender *R=(GmlRender*)vm->render;
    double w=(double)presentation_size(vm,R,0);
    double h=(double)presentation_size(vm,R,1);
    return array4(0,0,w,h);
  }
  if(!strcmp(nm,"date_current_datetime")) return vreal(25569.0 + (double)time(NULL)/86400.0);
  if(!strcmp(nm,"date_get_second")||!strcmp(nm,"date_get_minute")||!strcmp(nm,"date_get_hour")||
     !strcmp(nm,"date_get_day")||!strcmp(nm,"date_get_month")||!strcmp(nm,"date_get_year")){
    struct tm tmv; gm_datetime_tm(N(a,n,0),&tmv);
    if(!strcmp(nm,"date_get_second")) return vreal(tmv.tm_sec);
    if(!strcmp(nm,"date_get_minute")) return vreal(tmv.tm_min);
    if(!strcmp(nm,"date_get_hour")) return vreal(tmv.tm_hour);
    if(!strcmp(nm,"date_get_day")) return vreal(tmv.tm_mday);
    if(!strcmp(nm,"date_get_month")) return vreal(tmv.tm_mon+1);
    return vreal(tmv.tm_year+1900);
  }
  if(!strcmp(nm,"extension_stubfunc_real")) return vreal(0);
  if(!strcmp(nm,"extension_stubfunc_string")) return vstr("");
  if(!strcmp(nm,"extension_get_option_value")) return vstr("");
  if(!strcmp(nm,"get_string_async")) return vreal(0);
  if(!strcmp(nm,"get_string")) return vstr(n>=2?S(a,n,1):"");
  if(!strcmp(nm,"get_integer")) return vreal(n>=2?N(a,n,1):0);
  if(!strcmp(nm,"get_integer_async")) return vreal(0);
  if(!strcmp(nm,"get_open_filename")||!strcmp(nm,"get_save_filename")) return vstr("");
  if(!strcmp(nm,"execute_string")){
    int handled=classic_execute_assignment(vm,S(a,n,0));
    if(!handled && getenv("GML_LOG_UNKNOWN")) fprintf(stderr,"[gml] unsupported execute_string: %s\n",S(a,n,0));
    return vreal(0); }
  if(!strcmp(nm,"show_message")||!strcmp(nm,"show_message_async")||
     !strcmp(nm,"message_button")||!strcmp(nm,"message_background")||
     !strcmp(nm,"message_text_font")||!strcmp(nm,"message_button_font")||
     !strcmp(nm,"message_input_font")||!strcmp(nm,"message_alpha")||
     !strcmp(nm,"message_position")||!strcmp(nm,"message_caption")||
     !strcmp(nm,"message_size")||!strcmp(nm,"action_show_info")) return vreal(0);
  /* Host-process priority and MIDI tempo have no portable libretro control surface. They are
   * explicit compatibility no-ops rather than unresolved calls. */
  if(!strcmp(nm,"set_program_priority")||!strcmp(nm,"sound_background_tempo")) return vreal(0);
  if(!strcmp(nm,"show_info")) return vreal(0); /* classic game-information dialog: unavailable in libretro */
  if(!strcmp(nm,"parameter_count")) return vreal(0);
  if(!strcmp(nm,"parameter_string")) return vstr("");
  if(!strcmp(nm,"exception_unhandled_handler")) return vreal(0);
  if(!strcmp(nm,"io_clear")) return vreal(0);
  if(!strcmp(nm,"display_set_windows_alternate_sync")) return vreal(0);
  if(!strcmp(nm,"url_open")) return vreal(0);
  if(!strcmp(nm,"os_is_network_connected")) return vreal(0);
  if(!strcmp(nm,"network_resolve")) return vstr("");
  if(!strcmp(nm,"network_create_socket")||!strcmp(nm,"network_create_server")||
     !strcmp(nm,"network_connect")) return vreal(-1);
  if(!strcmp(nm,"network_send_packet")||!strcmp(nm,"network_destroy")) return vreal(0);
  if(!strcmp(nm,"external_define")||!strcmp(nm,"external_call")) return vreal(0);
  if(!strcmp(nm,"keyboard_virtual_show")||!strcmp(nm,"keyboard_virtual_hide")) return vreal(0);
  if(!strcmp(nm,"virtual_key_add")) return vreal(0);
  if(!strcmp(nm,"virtual_key_delete")) return vreal(0);
  if(!strcmp(nm,"achievement_available")||!strcmp(nm,"achievement_post_score")||
     !strcmp(nm,"achievement_show_leaderboards")) return vreal(0);
  if(!strcmp(nm,"analytics_addDesignEvent_windows")||
     !strcmp(nm,"addBusinessEventJson_windows")||
     !strcmp(nm,"addDesignEventWithValue_windows")||
     !strcmp(nm,"addDesignEvent_windows")||
     !strcmp(nm,"addErrorEvent_windows")||
     !strcmp(nm,"addProgressionEventWithScoreJson_windows")||
     !strcmp(nm,"addProgressionEvent_windows")||
     !strcmp(nm,"addResourceEventJson_windows")||
     !strcmp(nm,"analytics_configureAvailableCustomDimensions01_windows")||
     !strcmp(nm,"analytics_configureAvailableCustomDimensions02_windows")||
     !strcmp(nm,"analytics_configureAvailableCustomDimensions03_windows")||
     !strcmp(nm,"analytics_configureAvailableResourceCurrencies_windows")||
     !strcmp(nm,"analytics_configureAvailableResourceItemTypes_windows")||
     !strcmp(nm,"analytics_configureBuild_windows")||
     !strcmp(nm,"analytics_configureSdkGameEngineVersion_windows")||
     !strcmp(nm,"analytics_configureSdkWrapperVersion_windows")||
     !strcmp(nm,"analytics_configureUserId_windows")||
     !strcmp(nm,"analytics_initialize_windows")||
     !strcmp(nm,"analytics_setEnabledInfoLog_windows")||
     !strcmp(nm,"analytics_setEnabledManualSessionHandling_windows")||
     !strcmp(nm,"analytics_setEnabledVerboseLog_windows")||
     !strcmp(nm,"analytics_startSession_windows")||
     !strcmp(nm,"analytics_endSession_windows")||
     !strcmp(nm,"configureAvailableCustomDimensions01_windows")||
     !strcmp(nm,"configureAvailableCustomDimensions02_windows")||
     !strcmp(nm,"configureAvailableCustomDimensions03_windows")||
     !strcmp(nm,"configureAvailableResourceCurrencies_windows")||
     !strcmp(nm,"configureAvailableResourceItemTypes_windows")||
     !strcmp(nm,"configureBuild_windows")||
     !strcmp(nm,"configureSdkGameEngineVersion_windows")||
     !strcmp(nm,"configureUserId_windows")||
     !strcmp(nm,"endSession_windows")||
     !strcmp(nm,"native_ga_initialize_windows")||
     !strcmp(nm,"onResume_windows")||
     !strcmp(nm,"onStop_windows")||
     !strcmp(nm,"setCustomDimension01_windows")||
     !strcmp(nm,"setCustomDimension02_windows")||
     !strcmp(nm,"setCustomDimension03_windows")||
     !strcmp(nm,"setEnabledEventSubmission_windows")||
     !strcmp(nm,"setEnabledInfoLog_windows")||
     !strcmp(nm,"setEnabledManualSessionHandling_windows")||
     !strcmp(nm,"setEnabledVerboseLog_windows")||
     !strcmp(nm,"startSession_windows")) return vreal(0);
  if(!strcmp(nm,"isRemoteConfigsReady_windows")) return vreal(0);
  if(!strcmp(nm,"getRemoteConfigsContentAsString_windows")||
     !strcmp(nm,"getRemoteConfigsValueAsString_windows")) return vstr("");
  if(!strcmp(nm,"getRemoteConfigsValueAsStringWithDefaultValue_windows")) return vstr(n>=2?S(a,n,1):"");
  if(!strcmp(nm,"switch_get_operation_mode")) return vreal(0);
  if(!strcmp(nm,"switch_controller_support_set_player_max")||
     !strcmp(nm,"switch_controller_support_set_player_min")) return vreal(0);
  if(!strcmp(nm,"switch_save_data_commit")||
     !strcmp(nm,"switch_save_data_mount")) return vreal(1);
  if(!strcmp(nm,"switch_accounts_open_preselected_user")||
     !strcmp(nm,"switch_accounts_open_user")||
     !strcmp(nm,"switch_accounts_select_account")||
     !strcmp(nm,"switch_controller_joycon_set_holdtype")||
     !strcmp(nm,"switch_controller_set_supported_styles")||
     !strcmp(nm,"switch_controller_support_get_selected_id")||
     !strcmp(nm,"switch_controller_support_set_defaults")||
     !strcmp(nm,"switch_controller_support_set_singleplayer_only")||
     !strcmp(nm,"switch_controller_support_show")) return vreal(0);
  /* Gameframe is a native window-frame extension. A libretro core has no host HWND to style, so
   * report the extension unavailable and make its raw Win32 calls explicit no-ops. The GML wrapper
   * already has portable fallbacks for the unavailable path. */
  if(!strcmp(nm,"gameframe_check_native_extension")) return vreal(0);
  if(!strcmp(nm,"gameframe_init_raw_raw")||!strcmp(nm,"gameframe_set_shadow")||
     !strcmp(nm,"gameframe_syscommand_raw")) return vreal(0);
  if(!strcmp(nm,"gameframe_get_shadow")) return vreal(0);
  if(!strcmp(nm,"gameframe_mouse_in_window_raw")) return vreal(1);
  if(!strcmp(nm,"gameframe_is_natively_minimized_raw")) return vreal(0);
  if(!strcmp(nm,"gameframe_get_double_click_time_raw")) return vreal(500);
  if(!strcmp(nm,"gameframe_get_monitors_1")||!strcmp(nm,"gameframe_get_monitors_1_raw")) return vreal(1);
  if(!strcmp(nm,"gameframe_get_monitors_2")||!strcmp(nm,"gameframe_get_monitors_2_raw")){
    int bi=vm_buffer_slot_from_addr(vm,N(a,n,0));
    if(bi>=0){
      GmlRender *R=(GmlRender*)vm->render;
      int32_t w=R&&R->fbw>0 ? R->fbw : (vm&&vm->window_w>0 ? vm->window_w : 1280);
      int32_t h=R&&R->fbh>0 ? R->fbh : (vm&&vm->window_h>0 ? vm->window_h : 720);
      int32_t mon[9]={0,0,w,h,0,0,w,h,96};
      vm->buffer[bi].pos=0;
      buffer_write_raw(vm,bi,mon,(int)sizeof(mon));
      vm->buffer[bi].pos=0;
    }
    return vreal(1);
  }
  if(!strcmp(nm,"texturegroup_get_textures")) return arr_newv(0);
  if(!strcmp(nm,"texturegroup_get_status")) return vreal(3);   /* loaded */
  if(!strcmp(nm,"texture_is_ready")) return vreal(1);
  if(!strcmp(nm,"texture_prefetch")||!strcmp(nm,"texture_flush")||!strcmp(nm,"draw_texture_flush")||
     !strcmp(nm,"sprite_prefetch")||!strcmp(nm,"sprite_flush")||!strcmp(nm,"sprite_flush_multi")||
     !strcmp(nm,"sprite_prefetch_multi")) return vreal(0);
  if(!strcmp(nm,"ds_map_create")){
    return vreal((double)ds_map_create_id(vm));
  }
  if(!strcmp(nm,"ds_list_create")) return vreal((double)ds_list_create_id(vm));
  if(!strcmp(nm,"ds_list_destroy")){ GmlDSList *l=ds_list_slot(vm,(int)N(a,n,0));
    if(l){ free(l->item); memset(l,0,sizeof(*l)); } return vreal(0); }
  if(!strcmp(nm,"ds_list_clear")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); if(l) l->len=0; return vreal(0); }
  if(!strcmp(nm,"ds_list_add")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    for(int i=1;i<n;i++) ds_list_push(l,ds_val_clone(a[i])); return vreal(0); }
  if(!strcmp(nm,"ds_list_size")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return vreal(l?l->len:0); }
  if(!strcmp(nm,"ds_list_empty")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return vreal(!l||l->len==0); }
  if(!strcmp(nm,"ds_list_find_value")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); int p=(int)N(a,n,1);
    return (l && p>=0 && p<l->len)? ds_ret(l->item[p]) : vreal(0); }
  if(!strcmp(nm,"ds_list_find_index")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    if(l && n>=2) for(int i=0;i<l->len;i++){ GmlVal x=l->item[i];
      if(x.t==a[1].t && (x.t==V_STR? (x.s&&a[1].s&&!strcmp(x.s,a[1].s)) : x.d==a[1].d)) return vreal(i); }
    return vreal(-1); }
  if(!strcmp(nm,"ds_list_set")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); int p=(int)N(a,n,1);
    if(l && p>=0 && n>=3){ while(l->len<=p) ds_list_push(l,vreal(0)); l->item[p]=ds_val_clone(a[2]); } return vreal(0); }
  if(!strcmp(nm,"ds_list_replace")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); int p=(int)N(a,n,1);
    if(l && p>=0 && p<l->len && n>=3) l->item[p]=ds_val_clone(a[2]); return vreal(0); }
  if(!strcmp(nm,"ds_list_sort")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    if(l && l->len>1){ sort_dir=(n<2||N(a,n,1)!=0)?1:-1; qsort(l->item,(size_t)l->len,sizeof(GmlVal),gml_val_sort_cmp); }
    return vreal(0); }
  if(!strcmp(nm,"ds_list_shuffle")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    if(l) for(int i=l->len-1;i>0;i--){ int j=(int)floor(gml_rng_value(vm)*(i+1)); if(j<0) j=0; if(j>i) j=i; GmlVal t=l->item[i]; l->item[i]=l->item[j]; l->item[j]=t; }
    return vreal(0); }
  if(!strcmp(nm,"ds_list_delete")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); int p=(int)N(a,n,1);
    if(l && p>=0 && p<l->len){ memmove(&l->item[p],&l->item[p+1],(size_t)(l->len-p-1)*sizeof(GmlVal)); l->len--; } return vreal(0); }
  if(!strcmp(nm,"ds_list_insert")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); int p=(int)N(a,n,1);
    if(l && p>=0 && p<=l->len && n>=3){ ds_list_push(l,vreal(0));
      memmove(&l->item[p+1],&l->item[p],(size_t)(l->len-1-p)*sizeof(GmlVal)); l->item[p]=ds_val_clone(a[2]); } return vreal(0); }
  /* Implement FIFO queues and LIFO stacks through GmlDSList storage and shared IDs. */
  if(!strcmp(nm,"ds_queue_create")||!strcmp(nm,"ds_stack_create")) return vreal((double)ds_list_create_id(vm));
  if(!strcmp(nm,"ds_queue_destroy")||!strcmp(nm,"ds_stack_destroy")){ GmlDSList *l=ds_list_slot(vm,(int)N(a,n,0));
    if(l){ free(l->item); memset(l,0,sizeof(*l)); } return vreal(0); }
  if(!strcmp(nm,"ds_queue_clear")||!strcmp(nm,"ds_stack_clear")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); if(l) l->len=0; return vreal(0); }
  if(!strcmp(nm,"ds_queue_size")||!strcmp(nm,"ds_stack_size")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return vreal(l?l->len:0); }
  if(!strcmp(nm,"ds_queue_empty")||!strcmp(nm,"ds_stack_empty")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return vreal(!l||l->len==0); }
  if(!strcmp(nm,"ds_queue_enqueue")||!strcmp(nm,"ds_stack_push")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    for(int i=1;i<n;i++) ds_list_push(l,ds_val_clone(a[i])); return vreal(0); }
  if(!strcmp(nm,"ds_queue_head")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return (l&&l->len>0)?ds_ret(l->item[0]):vreal(0); }
  if(!strcmp(nm,"ds_queue_tail")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return (l&&l->len>0)?ds_ret(l->item[l->len-1]):vreal(0); }
  if(!strcmp(nm,"ds_stack_top")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return (l&&l->len>0)?ds_ret(l->item[l->len-1]):vreal(0); }
  if(!strcmp(nm,"ds_queue_dequeue")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    if(!l||l->len==0) return vreal(0); GmlVal v=l->item[0]; if(v.t==V_STR) v.d=0;
    memmove(&l->item[0],&l->item[1],(size_t)(l->len-1)*sizeof(GmlVal)); l->len--; return v; }
  if(!strcmp(nm,"ds_stack_pop")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    if(!l||l->len==0) return vreal(0); GmlVal v=l->item[--l->len]; if(v.t==V_STR) v.d=0; return v; }
  if(!strcmp(nm,"ds_priority_create")) return vreal((double)ds_list_create_id(vm));
  if(!strcmp(nm,"ds_priority_destroy")){ GmlDSList *l=ds_list_slot(vm,(int)N(a,n,0));
    if(l){ free(l->item); memset(l,0,sizeof(*l)); }
    return vreal(0); }
  if(!strcmp(nm,"ds_priority_clear")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    if(l) l->len=0;
    return vreal(0); }
  if(!strcmp(nm,"ds_priority_size")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return vreal(l?l->len:0); }
  if(!strcmp(nm,"ds_priority_empty")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return vreal(!l||l->len==0); }
  if(!strcmp(nm,"ds_priority_add")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    if(l && n>=3) ds_list_push(l,ds_priority_entry(a[1],N(a,n,2)));
    return vreal(0); }
  if(!strcmp(nm,"ds_priority_find_min")||!strcmp(nm,"ds_priority_find_max")){
    GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    int bi=ds_priority_best_index(l,!strcmp(nm,"ds_priority_find_max"));
    return bi>=0?ds_priority_value(l->item[bi]):vreal(0); }
  if(!strcmp(nm,"ds_priority_delete_min")||!strcmp(nm,"ds_priority_delete_max")){
    GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    int bi=ds_priority_best_index(l,!strcmp(nm,"ds_priority_delete_max"));
    if(bi<0) return vreal(0);
    GmlVal v=ds_priority_value(l->item[bi]);
    memmove(&l->item[bi],&l->item[bi+1],(size_t)(l->len-bi-1)*sizeof(GmlVal));
    l->len--;
    return v; }
  if(!strcmp(nm,"ds_exists")){ /* ds_exists(id, ds_type): type 0=map,1=list,4=grid — we track by id */
    int id=(int)N(a,n,0); return vreal(ds_list_slot_repair(vm,id)!=NULL || ds_map_slot(vm,id)!=NULL || ds_grid_slot(vm,id)!=NULL); }
  if(!strcmp(nm,"ds_grid_create")) return vreal((double)ds_grid_make(vm,(int)N(a,n,0),(int)N(a,n,1)));
  if(!strcmp(nm,"ds_grid_destroy")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0));
    if(g){ free(g->cell); memset(g,0,sizeof(*g)); } return vreal(0); }
  if(!strcmp(nm,"ds_grid_width")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0)); return vreal(g?g->w:0); }
  if(!strcmp(nm,"ds_grid_height")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0)); return vreal(g?g->h:0); }
  if(!strcmp(nm,"ds_grid_get")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0)); int x=(int)N(a,n,1),y=(int)N(a,n,2);
    return (g && g->cell && x>=0 && y>=0 && x<g->w && y<g->h)? ds_ret(g->cell[(size_t)y*g->w+x]) : vreal(0); }
  if(!strcmp(nm,"ds_grid_set")||!strcmp(nm,"ds_grid_set_post")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0)); int x=(int)N(a,n,1),y=(int)N(a,n,2);
    if(n>=4) ds_grid_store(g,x,y,a[3]); return n>=4?a[3]:vreal(0); }
  if(!strcmp(nm,"ds_grid_add")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0)); int x=(int)N(a,n,1),y=(int)N(a,n,2);
    if(g && g->cell && x>=0 && y>=0 && x<g->w && y<g->h && n>=4){
      GmlVal old=g->cell[(size_t)y*g->w+x];
      g->cell[(size_t)y*g->w+x]=ds_val_clone(vreal(N(&old,1,0)+N(a,n,3)));
    }
    return vreal(0); }
  if(!strcmp(nm,"ds_grid_value_x")||!strcmp(nm,"ds_grid_value_y")){
    GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0)); int x1=(int)N(a,n,1),y1=(int)N(a,n,2),x2=(int)N(a,n,3),y2=(int)N(a,n,4);
    if(x1>x2){ int t=x1; x1=x2; x2=t; } if(y1>y2){ int t=y1; y1=y2; y2=t; }
    if(g && g->cell){
      if(x1<0) x1=0; if(y1<0) y1=0; if(x2>=g->w) x2=g->w-1; if(y2>=g->h) y2=g->h-1;
      for(int yy=y1;yy<=y2;yy++) for(int xx=x1;xx<=x2;xx++)
        if(ds_val_equal(g->cell[(size_t)yy*g->w+xx],n>=6?a[5]:vreal(0))) return vreal(nm[14]=='x'?xx:yy);
    }
    return vreal(-1); }
  if(!strcmp(nm,"ds_grid_clear")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0)); GmlVal v=n>=2?a[1]:vreal(0);
    if(g && g->cell) for(size_t i=0;i<(size_t)g->w*g->h;i++) g->cell[i]=v; return vreal(0); }
  if(!strcmp(nm,"load_csv")){
    /* Read a CSV file into a ds_grid (GM semantics: each cell a string; numbers stay strings). Rows are
     * lines, columns are comma-separated; the grid is width=max columns, height=row count. */
    char *fn=resolve_content_path(vm,S(a,n,0)); FILE *f=fn?fopen(fn,"rb"):NULL; free(fn);
    if(!f) return vreal(ds_grid_make(vm,0,0));
    /* first pass: rows and max columns */
    int rows=0, maxcol=0, col=1, inq=0, c;
    long start=ftell(f);
    for(;;){ c=fgetc(f); if(c==EOF){ if(col>0||rows>0){ if(col>maxcol)maxcol=col; rows++; } break; }
      if(c=='"'){ inq=!inq; }
      else if(c==','&&!inq){ col++; }
      else if((c=='\n')&&!inq){ if(col>maxcol)maxcol=col; rows++; col=1; } else if(c=='\r'){} }
    if(rows<=0||maxcol<=0){ fclose(f); return vreal(ds_grid_make(vm,0,0)); }
    int gid=ds_grid_make(vm,maxcol,rows); GmlDSGrid *g=ds_grid_slot(vm,gid);
    if(!g||!g->cell){ fclose(f); return vreal(gid); }
    fseek(f,start,SEEK_SET);
    /* second pass: fill cells */
    char buf[1024]; int bl=0, cx=0, cy=0; inq=0;
    #define CSV_EMIT() do{ buf[bl]=0; if(cx<g->w && cy<g->h){ char *s=strdup(buf); if(s) g->cell[(size_t)cy*g->w+cx]=vstr_owned(s); } bl=0; }while(0)
    for(;;){ c=fgetc(f);
      if(c==EOF){ CSV_EMIT(); break; }
      if(c=='"'){ inq=!inq; }
      else if(c==','&&!inq){ CSV_EMIT(); cx++; }
      else if(c=='\n'&&!inq){ CSV_EMIT(); cx=0; cy++; }
      else if(c=='\r'){}
      else if(bl<(int)sizeof(buf)-1){ buf[bl++]=(char)c; } }
    #undef CSV_EMIT
    fclose(f);
    return vreal(gid);
  }
  if(!strcmp(nm,"ds_map_add")){
    if(n>=3) ds_map_put(vm,(int)N(a,n,0),a[1],a[2],0);
    return vreal(0);
  }
  if(!strcmp(nm,"ds_map_write")){
    return ds_map_write_text(vm,(int)N(a,n,0));
  }
  if(!strcmp(nm,"ds_map_read")){
    return vreal(ds_map_read_text(vm,(int)N(a,n,0),S(a,n,1)));
  }
  /* secure (encrypted) map save/load — no persistence yet: report success on save, hand back a fresh
   * EMPTY map on load (a valid id the game can query, i.e. "no saved data" rather than a bogus 0). */
  if(!strcmp(nm,"ds_map_secure_save")||!strcmp(nm,"ds_map_secure_save_buffer")) return vreal(1);
  if(!strcmp(nm,"ds_map_secure_load")||!strcmp(nm,"ds_map_secure_load_buffer")) return vreal((double)ds_map_create_id(vm));
  if(!strcmp(nm,"ds_map_exists")){ GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    DsKeyTemp kt={0}; const char *key=(n>=2)?ds_key_temp(a[1],&kt):NULL;
    int i=ds_map_find_entry(m,key); ds_key_temp_free(&kt); return vreal(i>=0); }
  if(!strcmp(nm,"ds_map_size")){ GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0)); return vreal(m?m->len:0); }
  if(!strcmp(nm,"ds_map_empty")){ GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0)); return vreal(!m||m->len==0); }
  if(!strcmp(nm,"ds_map_set")||!strcmp(nm,"ds_map_set_post")||!strcmp(nm,"ds_map_replace")){
    if(n>=3) ds_map_put(vm,(int)N(a,n,0),a[1],a[2],1);
    return n>=3 ? a[2] : vreal(0);
  }
  if(!strcmp(nm,"ds_map_delete")){
    GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    DsKeyTemp kt={0};
    const char *key=(n>=2)?ds_key_temp(a[1],&kt):NULL;
    int i=ds_map_find_entry(m,key);
    if(i>=0){
      ds_entry_free(&m->entry[i]);
      memmove(&m->entry[i],&m->entry[i+1],(size_t)(m->len-i-1)*sizeof(m->entry[0]));
      m->len--;
      m->hdirty=1;   /* indices shifted: rebuild the hash index on next lookup */
      m->last_lookup=-1;
    }
    ds_key_temp_free(&kt);
    return vreal(0);
  }
  if(!strcmp(nm,"ds_map_destroy")){
    GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    ds_map_destroy_live(m);
    return vreal(0);
  }
  if(!strcmp(nm,"ds_map_clear")){
    GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    ds_map_clear_entries(m);
    return vreal(0);
  }
  if(!strcmp(nm,"ds_list_copy")){          /* ds_list_copy(dest, src): dest := copy of src */
    GmlDSList *dst=ds_list_slot_repair(vm,(int)N(a,n,0)), *src=ds_list_slot_repair(vm,(int)N(a,n,1));
    if(dst && src){
      dst->len=0;
      for(int i=0;i<src->len;i++) ds_list_push(dst, ds_val_clone(src->item[i]));
    }
    return vreal(0);
  }
  if(!strcmp(nm,"ds_map_find_first")){
    GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    if(m && m->len>0) return ds_ret(m->entry[0].key_val);
    return vstr("");
  }
  if(!strcmp(nm,"ds_map_find_last")){
    GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    if(m && m->len>0) return ds_ret(m->entry[m->len-1].key_val);
    return vstr("");
  }
  /* Walk insertion-ordered map entries; return undefined beyond either end. */
  if(!strcmp(nm,"ds_map_find_next")||!strcmp(nm,"ds_map_find_previous")){
    GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    DsKeyTemp kt={0};
    const char *key=(n>=2)?ds_key_temp(a[1],&kt):NULL;
    int i=ds_map_find_entry(m,key); ds_key_temp_free(&kt);
    int j = (i<0) ? -1 : (nm[12]=='n' ? i+1 : i-1);
    if(m && j>=0 && j<m->len) return ds_ret(m->entry[j].key_val);
    return vundef();
  }
  /* ds_map_add_list/map mark the child for nested destroy/JSON in GM; storing the id keeps
   * lookups working (json_encode walks ids the same way). */
  if(!strcmp(nm,"ds_map_add_list")||!strcmp(nm,"ds_map_add_map")||!strcmp(nm,"ds_map_replace_list")||!strcmp(nm,"ds_map_replace_map")){
    if(n>=3) ds_map_put(vm,(int)N(a,n,0),a[1],a[2],1);
    return vreal(0);
  }
  if(!strcmp(nm,"ds_map_find_value")){
    int id=(int)N(a,n,0);
    return gml_ds_map_find_value_direct(vm,id,n>=2?a[1]:vundef(),n>=2);
  }
  if(!strcmp(nm,"json_decode")) return json_decode_text(vm,S(a,n,0));
  if(!strcmp(nm,"json_encode")) return json_encode_root(vm,n>0?a[0]:vundef());
  if(!strcmp(nm,"json_parse")) return json_decode_text_mode(vm,S(a,n,0),1);
  if(!strcmp(nm,"json_stringify")) return json_encode_root(vm,n>0?a[0]:vundef());
  if(!strcmp(nm,"is_undefined")) return vreal(n>0 && a[0].t==V_UNDEF);
  if(!strcmp(nm,"is_string")) return vreal(n>0 && a[0].t==V_STR);
  if(!strcmp(nm,"is_real")||!strcmp(nm,"is_numeric")) return vreal(n>0 && a[0].t==V_REAL);
  if(!strcmp(nm,"is_array")) return vreal(n>0 && a[0].t==V_ARR);
  if(!strcmp(nm,"is_struct")){
    return vreal(n>0 && a[0].t==V_REAL && GML_IS_STRUCT_ID(a[0].d) &&
                 gml_struct_find(vm,(unsigned)a[0].d)!=NULL);
  }
  if(!strcmp(nm,"is_bool")) return vreal(n>0 && a[0].t==V_REAL && (a[0].d==0||a[0].d==1));
  if(!strcmp(nm,"variable_global_exists")) return vreal(gml_varmap_get(&vm->globals,S(a,n,0))!=NULL);
  if(!strcmp(nm,"variable_global_get")){
    GmlVal *p=gml_varmap_get(&vm->globals,S(a,n,0));
    if(!p) return vundef();
    GmlVal out=*p; if(out.t==V_STR) out.d=0;
    return out;
  }
  if(!strcmp(nm,"variable_global_set")){
    if(n>1){
      const char *key=S(a,n,0);
      GmlVal *p=gml_varmap_get(&vm->globals,key);
      if(p) *p=var_store_clone(a[1]);
      else {
        char *owned=strdup(key?key:"");
        if(owned) *gml_varmap_put(&vm->globals,owned)=var_store_clone(a[1]);
      }
    }
    return vreal(0);
  }
  if(!strcmp(nm,"variable_instance_exists")||!strcmp(nm,"variable_instance_get")||
     !strcmp(nm,"variable_instance_set")){
    const char *key=S(a,n,1);
    if(!strcmp(nm,"variable_instance_exists")) return vreal(n>1 && gml_inst_var_exists(vm,a[0],key));
    if(!strcmp(nm,"variable_instance_get")){
      int ok=0; GmlVal out=(n>1)?gml_inst_var_get_val(vm,a[0],key,&ok):vundef();
      return ok?out:vundef();
    }
    if(n>2) gml_inst_var_set_val(vm,a[0],key,var_store_clone(a[2]));
    return vreal(0);
  }
  if(!strcmp(nm,"variable_struct_exists")||!strcmp(nm,"variable_struct_get")||
     !strcmp(nm,"variable_struct_set")||!strcmp(nm,"variable_struct_remove")){
    GmlInstance *st=(n>0 && a[0].t==V_REAL && GML_IS_STRUCT_ID(a[0].d))?gml_struct_find(vm,(unsigned)a[0].d):NULL;
    const char *key=S(a,n,1);
    if(!st) return !strcmp(nm,"variable_struct_get") ? vundef() : vreal(0);
    if(!strcmp(nm,"variable_struct_exists")) return vreal(gml_varmap_get(&st->vars,key)!=NULL);
    if(!strcmp(nm,"variable_struct_get")){
      GmlVal *p=gml_varmap_get(&st->vars,key);
      if(!p) return vundef();
      GmlVal out=*p; if(out.t==V_STR) out.d=0;
      return out;
    }
    if(!strcmp(nm,"variable_struct_set")){
      if(n>2){
        if(key && (!strcmp(key,"__fn") || !strcmp(key,"__self"))) st->method_bound=0;
        GmlVal *p=gml_varmap_get(&st->vars,key);
        if(p) *p=var_store_clone(a[2]);
        else {
          char *owned=strdup(key?key:"");
          if(owned) *gml_varmap_put(&st->vars,owned)=var_store_clone(a[2]);
        }
      }
      return vreal(0);
    }
    if(key && (!strcmp(key,"__fn") || !strcmp(key,"__self"))) st->method_bound=0;
    return vreal(varmap_delete_key(&st->vars,key));
  }
  if(!strcmp(nm,"struct_get_from_hash")){
    GmlInstance *st=(n>0 && a[0].t==V_REAL && GML_IS_STRUCT_ID(a[0].d))?gml_struct_find(vm,(unsigned)a[0].d):NULL;
    if(!st || st->vars.cap<=0) return vundef();
    double hv=N(a,n,1);
    uint32_t want = hv < 0 ? (uint32_t)(int32_t)hv : (uint32_t)hv;
    for(int i=0;i<st->vars.cap;i++){
      GmlVarSlot *slot=&st->vars.slots[i];
      if(slot->key && slot->hash==want){
        GmlVal out=slot->val; if(out.t==V_STR) out.d=0;
        return out;
      }
    }
    return vundef();
  }
  if(!strcmp(nm,"d3d_start")){ g_d3.active=1; g_d3.depth_frame=-1; return vreal(0); }
  if(!strcmp(nm,"d3d_end")){ g_d3.active=0; return vreal(0); }
  if(!strcmp(nm,"d3d_set_hidden")){
    g_d3.hidden=N(a,n,0)!=0.0 && !getenv("GML_D3D_NO_DEPTH");
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_zwriteenable")){ g_d3.zwrite=N(a,n,0)!=0; return vreal(0); }
  if(!strcmp(nm,"d3d_set_shading")){ g_d3.smooth=N(a,n,0)!=0; return vreal(0); }
  if(!strcmp(nm,"d3d_set_fog")){
    g_d3.fog=N(a,n,0)!=0; g_d3.fog_color=(uint32_t)N(a,n,1)&0xFFFFFFu;
    g_d3.fog_start=N(a,n,2); g_d3.fog_end=N(a,n,3);
    if(g_d3.fog_end<g_d3.fog_start){ double swap=g_d3.fog_start; g_d3.fog_start=g_d3.fog_end; g_d3.fog_end=swap; }
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_culling")){ g_d3.culling=N(a,n,0)!=0; return vreal(0); }
  if(!strcmp(nm,"d3d_set_lighting")){ g_d3.lighting=N(a,n,0)!=0; return vreal(0); }
  if(!strcmp(nm,"d3d_light_define_point")){
    int id=(int)N(a,n,0); if(id<0||id>=8) return vreal(0);
    g_d3.light[id].defined=1; g_d3.light[id].x=N(a,n,1); g_d3.light[id].y=N(a,n,2);
    g_d3.light[id].z=N(a,n,3); g_d3.light[id].range=fabs(N(a,n,4));
    g_d3.light[id].color=(uint32_t)N(a,n,5)&0xFFFFFFu;
    return vreal(1);
  }
  if(!strcmp(nm,"d3d_light_define_direction")){
    int id=(int)N(a,n,0); if(id<0||id>=8) return vreal(0);
    g_d3.light[id].defined=1; g_d3.light[id].x=N(a,n,1); g_d3.light[id].y=N(a,n,2);
    g_d3.light[id].z=N(a,n,3); g_d3.light[id].range=-1;
    g_d3.light[id].color=(uint32_t)N(a,n,4)&0xFFFFFFu;
    return vreal(1);
  }
  if(!strcmp(nm,"d3d_light_define_ambient")){
    g_d3.ambient_color=(uint32_t)N(a,n,0)&0xFFFFFFu;
    return vreal(1);
  }
  if(!strcmp(nm,"d3d_light_enable")){
    int id=(int)N(a,n,0); if(id>=0&&id<8) g_d3.light[id].enabled=N(a,n,1)!=0;
    return vreal(id>=0&&id<8);
  }
  if(!strcmp(nm,"d3d_set_projection")){
    d3_set_camera(N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),
                  N(a,n,6),N(a,n,7),N(a,n,8));
    g_d3.fov=41.2; g_d3.aspect=0; g_d3.near_clip=.05; g_d3.far_clip=32000;
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_projection_ext")){
    d3_set_camera(N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),
                  N(a,n,6),N(a,n,7),N(a,n,8));
    g_d3.fov=N(a,n,9); if(g_d3.fov<1||g_d3.fov>170) g_d3.fov=41.2;
    g_d3.aspect=N(a,n,10); if(g_d3.aspect<=0) g_d3.aspect=0;
    g_d3.near_clip=N(a,n,11); if(g_d3.near_clip<=1e-6) g_d3.near_clip=.05;
    g_d3.far_clip=N(a,n,12); if(g_d3.far_clip<=g_d3.near_clip) g_d3.far_clip=32000;
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_projection_ortho")){
    g_d3.ortho=1; g_d3.perspective=0; g_d3.ortho_x=N(a,n,0); g_d3.ortho_y=N(a,n,1);
    g_d3.ortho_w=N(a,n,2); g_d3.ortho_h=N(a,n,3); g_d3.ortho_angle=N(a,n,4);
    if(fabs(g_d3.ortho_w)<1e-9) g_d3.ortho_w=1;
    if(fabs(g_d3.ortho_h)<1e-9) g_d3.ortho_h=1;
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_projection_perspective")){
    double x=N(a,n,0),y=N(a,n,1),w=N(a,n,2),h=N(a,n,3),angle=N(a,n,4);
    if(fabs(w)<1e-9) w=1;
    if(fabs(h)<1e-9) h=1;
    double center_x=x+w*.5,center_y=y+h*.5,distance=fabs(w);
    double radians=angle*M_PI/180.0;
    d3_set_camera(center_x,center_y,distance,center_x,center_y,0,-sin(radians),cos(radians),0);
    g_d3.fov=2*atan(fabs(h)*.5/distance)*180.0/M_PI;
    if(g_d3.fov<1||g_d3.fov>170) g_d3.fov=41.2;
    g_d3.aspect=fabs(w/h); g_d3.near_clip=.05; g_d3.far_clip=32000;
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_perspective")){
    GmlRender *R=(GmlRender*)vm->render;
    if(N(a,n,0)!=0){
      double w=R&&R->fbw>0?R->fbw:640,h=R&&R->fbh>0?R->fbh:480;
      double args[5]={R?R->cam_x:0,R?R->cam_y:0,w,h,0};
      GmlVal values[5]; for(int i=0;i<5;i++) values[i]=vreal(args[i]);
      return builtin_call_impl(vm,"d3d_set_projection_perspective",values,5);
    }
    g_d3.ortho=1; g_d3.perspective=0;
    g_d3.ortho_x=R?R->cam_x:0; g_d3.ortho_y=R?R->cam_y:0;
    g_d3.ortho_w=R&&R->fbw>0?R->fbw:640; g_d3.ortho_h=R&&R->fbh>0?R->fbh:480; g_d3.ortho_angle=0;
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_depth")){ g_d3.draw_depth=N(a,n,0); return vreal(0); }
  if(!strncmp(nm,"d3d_transform_set_",18)||!strncmp(nm,"d3d_transform_add_",18)){
    int add=!strncmp(nm,"d3d_transform_add_",18);
    const char *op=nm+18;
    double matrix[16]; d3_matrix_identity(matrix);
    if(!strcmp(op,"identity")){ d3_matrix_identity(g_d3.transform); return vreal(0); }
    if(!strcmp(op,"translation")) d3_matrix_translation(matrix,N(a,n,0),N(a,n,1),N(a,n,2));
    else if(!strcmp(op,"scaling")) d3_matrix_scaling(matrix,N(a,n,0),N(a,n,1),N(a,n,2));
    else if(!strcmp(op,"rotation_x")) d3_matrix_rotation_axis(matrix,1,0,0,N(a,n,0));
    else if(!strcmp(op,"rotation_y")) d3_matrix_rotation_axis(matrix,0,1,0,N(a,n,0));
    else if(!strcmp(op,"rotation_z")) d3_matrix_rotation_axis(matrix,0,0,1,N(a,n,0));
    else if(!strcmp(op,"rotation_axis")) d3_matrix_rotation_axis(matrix,N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3));
    else return vreal(0);
    if(add) d3_matrix_prepend(matrix); else memcpy(g_d3.transform,matrix,sizeof(matrix));
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_transform_stack_clear")){ g_d3.transform_stack_n=0; return vreal(0); }
  if(!strcmp(nm,"d3d_transform_stack_empty")) return vreal(g_d3.transform_stack_n==0);
  if(!strcmp(nm,"d3d_transform_stack_push")){
    if(g_d3.transform_stack_n>=32) return vreal(0);
    memcpy(g_d3.transform_stack[g_d3.transform_stack_n++],g_d3.transform,sizeof(g_d3.transform));
    return vreal(1);
  }
  if(!strcmp(nm,"d3d_transform_stack_pop")){
    if(g_d3.transform_stack_n<=0) return vreal(0);
    memcpy(g_d3.transform,g_d3.transform_stack[--g_d3.transform_stack_n],sizeof(g_d3.transform));
    return vreal(1);
  }
  if(!strcmp(nm,"d3d_transform_stack_top")){
    if(g_d3.transform_stack_n<=0) return vreal(0);
    memcpy(g_d3.transform,g_d3.transform_stack[g_d3.transform_stack_n-1],sizeof(g_d3.transform));
    return vreal(1);
  }
  if(!strcmp(nm,"d3d_transform_stack_discard")){
    if(g_d3.transform_stack_n<=0) return vreal(0);
    g_d3.transform_stack_n--; return vreal(1);
  }
  if(!strcmp(nm,"d3d_model_create")){
    for(int id=0;id<GML_D3_MODEL_MAX;id++) if(!g_d3_model[id].used){
      d3_model_clear_data(&g_d3_model[id]); g_d3_model[id].used=1; g_d3_model[id].building=-1;
      return vreal(id);
    }
    return vreal(-1);
  }
  if(!strcmp(nm,"d3d_model_destroy")){
    int id=(int)N(a,n,0); if(id<0 || id>=GML_D3_MODEL_MAX || !g_d3_model[id].used) return vreal(0);
    d3_model_clear_data(&g_d3_model[id]); g_d3_model[id].used=0; return vreal(1);
  }
  if(!strcmp(nm,"d3d_model_clear")){
    int id=(int)N(a,n,0); if(id<0 || id>=GML_D3_MODEL_MAX || !g_d3_model[id].used) return vreal(0);
    d3_model_clear_data(&g_d3_model[id]); return vreal(1);
  }
  if(!strcmp(nm,"d3d_model_primitive_begin")){
    int id=(int)N(a,n,0),kind=(int)N(a,n,1);
    return vreal(id>=0&&id<GML_D3_MODEL_MAX&&d3_model_begin_batch(&g_d3_model[id],kind));
  }
  if(!strcmp(nm,"d3d_model_primitive_end")){
    int id=(int)N(a,n,0); if(id<0 || id>=GML_D3_MODEL_MAX || !g_d3_model[id].used) return vreal(0);
    g_d3_model[id].building=-1; return vreal(1);
  }
  if(!strncmp(nm,"d3d_model_vertex",16)){
    int id=(int)N(a,n,0); if(id<0 || id>=GML_D3_MODEL_MAX || !g_d3_model[id].used) return vreal(0);
    GmlRender *R=(GmlRender*)vm->render;
    GmlD3Vertex vertex; memset(&vertex,0,sizeof(vertex));
    vertex.x=N(a,n,1); vertex.y=N(a,n,2); vertex.z=N(a,n,3);
    uint32_t color=R?R->color:0xFFFFFFu; vertex.alpha=R?R->alpha:1;
    int normal=strstr(nm,"_normal")!=NULL;
    int texture=strstr(nm,"_texture")!=NULL;
    int colored=strstr(nm,"_color")!=NULL||strstr(nm,"_colour")!=NULL;
    int index=4;
    if(normal){ vertex.nx=N(a,n,index++); vertex.ny=N(a,n,index++); vertex.nz=N(a,n,index++); vertex.has_normal=1; }
    if(texture){ vertex.u=N(a,n,index++); vertex.v=N(a,n,index++); }
    if(colored){ color=(uint32_t)N(a,n,index++)&0xFFFFFFu; vertex.alpha=N(a,n,index); }
    vertex.r=color&255; vertex.g=(color>>8)&255; vertex.b=(color>>16)&255;
    return vreal(d3_model_append_vertex(&g_d3_model[id],vertex));
  }
  if(!strcmp(nm,"d3d_model_floor")||!strcmp(nm,"d3d_model_wall")||
     !strcmp(nm,"d3d_model_block")||!strcmp(nm,"d3d_model_cylinder")||
     !strcmp(nm,"d3d_model_cone")||!strcmp(nm,"d3d_model_ellipsoid")){
    int id=(int)N(a,n,0); if(id<0 || id>=GML_D3_MODEL_MAX || !g_d3_model[id].used) return vreal(0);
    GmlD3Model *model=&g_d3_model[id];
    if(!d3_model_begin_batch(model,4)) return vreal(0);
    double x1=N(a,n,1),y1=N(a,n,2),z1=N(a,n,3),x2=N(a,n,4),y2=N(a,n,5),z2=N(a,n,6);
    double hrepeat=N(a,n,7),vrepeat=N(a,n,8); int ok=1;
    if(!strcmp(nm,"d3d_model_floor")||!strcmp(nm,"d3d_model_wall")){
      double point[4][3];
      if(!strcmp(nm,"d3d_model_floor")){
        double shape[4][3]={{x1,y1,z1},{x2,y1,z1},{x2,y2,z2},{x1,y2,z2}};
        memcpy(point,shape,sizeof(point));
      } else {
        double shape[4][3]={{x1,y1,z1},{x2,y2,z1},{x2,y2,z2},{x1,y1,z2}};
        memcpy(point,shape,sizeof(point));
      }
      ok=d3_model_append_quad(model,point,hrepeat,vrepeat);
    } else if(!strcmp(nm,"d3d_model_block")){
      double face[6][4][3]={
        {{x1,y1,z1},{x2,y1,z1},{x2,y2,z1},{x1,y2,z1}},
        {{x1,y2,z2},{x2,y2,z2},{x2,y1,z2},{x1,y1,z2}},
        {{x1,y1,z2},{x2,y1,z2},{x2,y1,z1},{x1,y1,z1}},
        {{x1,y2,z1},{x2,y2,z1},{x2,y2,z2},{x1,y2,z2}},
        {{x1,y1,z1},{x1,y2,z1},{x1,y2,z2},{x1,y1,z2}},
        {{x2,y1,z2},{x2,y2,z2},{x2,y2,z1},{x2,y1,z1}}};
      for(int i=0;i<6&&ok;i++) ok=d3_model_append_quad(model,face[i],hrepeat,vrepeat);
    } else if(!strcmp(nm,"d3d_model_cylinder")||!strcmp(nm,"d3d_model_cone")){
      double cx=(x1+x2)*.5,cy=(y1+y2)*.5,rx=fabs(x2-x1)*.5,ry=fabs(y2-y1)*.5;
      int closed=N(a,n,9)!=0,steps=(int)N(a,n,10); if(steps<3) steps=3; if(steps>128) steps=128;
      int cone=!strcmp(nm,"d3d_model_cone");
      for(int i=0;i<steps&&ok;i++){
        double q0=2*M_PI*i/steps,q1=2*M_PI*(i+1)/steps;
        double top0x=cone?cx:cx+cos(q0)*rx,top0y=cone?cy:cy+sin(q0)*ry;
        double top1x=cone?cx:cx+cos(q1)*rx,top1y=cone?cy:cy+sin(q1)*ry;
        double side[4][3]={{cx+cos(q0)*rx,cy+sin(q0)*ry,z1},{cx+cos(q1)*rx,cy+sin(q1)*ry,z1},
                           {top1x,top1y,z2},{top0x,top0y,z2}};
        ok=d3_model_append_quad(model,side,hrepeat/steps,vrepeat);
        if(closed&&ok){
          double cap0[4][3]={{cx,cy,z1},{cx+cos(q1)*rx,cy+sin(q1)*ry,z1},
                              {cx+cos(q0)*rx,cy+sin(q0)*ry,z1},{cx,cy,z1}};
          ok=d3_model_append_quad(model,cap0,hrepeat,vrepeat);
          if(!cone&&ok){
            double cap1[4][3]={{cx,cy,z2},{cx+cos(q0)*rx,cy+sin(q0)*ry,z2},
                                {cx+cos(q1)*rx,cy+sin(q1)*ry,z2},{cx,cy,z2}};
            ok=d3_model_append_quad(model,cap1,hrepeat,vrepeat);
          }
        }
      }
    } else {
      double cx=(x1+x2)*.5,cy=(y1+y2)*.5,cz=(z1+z2)*.5;
      double rx=fabs(x2-x1)*.5,ry=fabs(y2-y1)*.5,rz=fabs(z2-z1)*.5;
      int steps=(int)N(a,n,9); if(steps<4) steps=4; if(steps>64) steps=64;
      for(int lat=0;lat<steps&&ok;lat++) for(int lon=0;lon<steps*2&&ok;lon++){
        double latitude0=-M_PI*.5+M_PI*lat/steps,latitude1=-M_PI*.5+M_PI*(lat+1)/steps;
        double longitude0=M_PI*lon/steps,longitude1=M_PI*(lon+1)/steps;
        double point[4][3]={{cx+rx*cos(latitude0)*cos(longitude0),cy+ry*cos(latitude0)*sin(longitude0),cz+rz*sin(latitude0)},
                            {cx+rx*cos(latitude0)*cos(longitude1),cy+ry*cos(latitude0)*sin(longitude1),cz+rz*sin(latitude0)},
                            {cx+rx*cos(latitude1)*cos(longitude1),cy+ry*cos(latitude1)*sin(longitude1),cz+rz*sin(latitude1)},
                            {cx+rx*cos(latitude1)*cos(longitude0),cy+ry*cos(latitude1)*sin(longitude0),cz+rz*sin(latitude1)}};
        ok=d3_model_append_quad(model,point,hrepeat/steps,vrepeat/steps);
      }
    }
    model->building=-1;
    if(!ok){
      GmlD3Batch *batch=&model->batch[model->batch_n-1];
      model->vertex_n=batch->first; model->batch_n--;
    }
    return vreal(ok);
  }
  if(!strcmp(nm,"d3d_model_draw")){
    int id=(int)N(a,n,0); if(id<0 || id>=GML_D3_MODEL_MAX || !g_d3_model[id].used) return vreal(0);
    d3_model_draw((GmlRender*)vm->render,&g_d3_model[id],N(a,n,1),N(a,n,2),N(a,n,3),(int)N(a,n,4));
    return vreal(1);
  }
  if(!strcmp(nm,"d3d_model_save")){
    int id=(int)N(a,n,0); if(id<0 || id>=GML_D3_MODEL_MAX || !g_d3_model[id].used) return vreal(0);
    char *path=resolve_content_path(vm,S(a,n,1)); int ok=d3_model_file_save(path,&g_d3_model[id]); free(path); return vreal(ok);
  }
  if(!strcmp(nm,"d3d_model_load")){
    int id=(int)N(a,n,0); if(id<0 || id>=GML_D3_MODEL_MAX || !g_d3_model[id].used) return vreal(0);
    char *path=resolve_content_path(vm,S(a,n,1)); int ok=d3_model_file_load(path,&g_d3_model[id]); free(path); return vreal(ok);
  }
  if(!strcmp(nm,"d3d_primitive_begin")||!strcmp(nm,"d3d_primitive_begin_texture")){
    g_d3_prim_kind=(int)N(a,n,0); g_d3_prim_n=0;
    g_d3_prim_texture=!strcmp(nm,"d3d_primitive_begin_texture")?(int)N(a,n,1):-1;
    return vreal(0);
  }
  if(!strncmp(nm,"d3d_vertex",10)){
    if(g_d3_prim_n>=GML_D3_PRIM_MAX) return vreal(0);
    GmlRender *R=(GmlRender*)vm->render;
    GmlD3Vertex vertex; memset(&vertex,0,sizeof(vertex));
    vertex.x=N(a,n,0); vertex.y=N(a,n,1); vertex.z=N(a,n,2);
    uint32_t color=R?R->color:0xFFFFFFu; vertex.alpha=R?R->alpha:1;
    int normal=strstr(nm,"_normal")!=NULL;
    int texture=strstr(nm,"_texture")!=NULL;
    int colored=strstr(nm,"_color")!=NULL||strstr(nm,"_colour")!=NULL;
    int index=3;
    if(normal){ vertex.nx=N(a,n,index++); vertex.ny=N(a,n,index++); vertex.nz=N(a,n,index++); vertex.has_normal=1; }
    if(texture){ vertex.u=N(a,n,index++); vertex.v=N(a,n,index++); }
    if(colored){ color=(uint32_t)N(a,n,index++)&0xFFFFFFu; vertex.alpha=N(a,n,index); }
    vertex.r=color&255; vertex.g=(color>>8)&255; vertex.b=(color>>16)&255;
    g_d3_prim[g_d3_prim_n++]=vertex;
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_primitive_end")){
    d3_primitive_flush((GmlRender*)vm->render); g_d3_prim_n=0; return vreal(0);
  }
  if(!strcmp(nm,"d3d_draw_block")){
    GmlRender *R=(GmlRender*)vm->render;
    if(R && g_d3.active){
      double x1=N(a,n,0),y1=N(a,n,1),z1=N(a,n,2),x2=N(a,n,3),y2=N(a,n,4),z2=N(a,n,5);
      double face[6][4][3]={
        {{x1,y1,z1},{x2,y1,z1},{x2,y2,z1},{x1,y2,z1}},
        {{x1,y2,z2},{x2,y2,z2},{x2,y1,z2},{x1,y1,z2}},
        {{x1,y1,z2},{x2,y1,z2},{x2,y1,z1},{x1,y1,z1}},
        {{x1,y2,z1},{x2,y2,z1},{x2,y2,z2},{x1,y2,z2}},
        {{x1,y1,z1},{x1,y2,z1},{x1,y2,z2},{x1,y1,z2}},
        {{x2,y1,z2},{x2,y2,z2},{x2,y2,z1},{x2,y1,z1}}};
      gml_render_maybe_prepare_draw(R);
      for(int i=0;i<6;i++) d3_draw_quad(R,face[i],(int)N(a,n,6),N(a,n,7),N(a,n,8));
    }
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_draw_cylinder")){
    GmlRender *R=(GmlRender*)vm->render;
    if(R && g_d3.active){
      double x1=N(a,n,0),y1=N(a,n,1),z1=N(a,n,2),x2=N(a,n,3),y2=N(a,n,4),z2=N(a,n,5);
      double cx=(x1+x2)*.5,cy=(y1+y2)*.5,rx=fabs(x2-x1)*.5,ry=fabs(y2-y1)*.5;
      int closed=N(a,n,9)!=0, steps=(int)N(a,n,10); if(steps<3)steps=3; if(steps>128)steps=128;
      gml_render_maybe_prepare_draw(R);
      for(int i=0;i<steps;i++){
        double q0=2*M_PI*i/steps,q1=2*M_PI*(i+1)/steps;
        double side[4][3]={{cx+cos(q0)*rx,cy+sin(q0)*ry,z1},{cx+cos(q1)*rx,cy+sin(q1)*ry,z1},
                           {cx+cos(q1)*rx,cy+sin(q1)*ry,z2},{cx+cos(q0)*rx,cy+sin(q0)*ry,z2}};
        d3_draw_quad(R,side,(int)N(a,n,6),N(a,n,7)/steps,N(a,n,8));
        if(closed){
          double cap0[4][3]={{cx,cy,z1},{cx+cos(q1)*rx,cy+sin(q1)*ry,z1},{cx+cos(q0)*rx,cy+sin(q0)*ry,z1},{cx,cy,z1}};
          double cap1[4][3]={{cx,cy,z2},{cx+cos(q0)*rx,cy+sin(q0)*ry,z2},{cx+cos(q1)*rx,cy+sin(q1)*ry,z2},{cx,cy,z2}};
          d3_draw_quad(R,cap0,(int)N(a,n,6),N(a,n,7),N(a,n,8));
          d3_draw_quad(R,cap1,(int)N(a,n,6),N(a,n,7),N(a,n,8));
        }
      }
    }
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_draw_cone")){
    GmlRender *R=(GmlRender*)vm->render;
    if(R && g_d3.active){
      double x1=N(a,n,0),y1=N(a,n,1),z1=N(a,n,2),x2=N(a,n,3),y2=N(a,n,4),z2=N(a,n,5);
      double cx=(x1+x2)*.5,cy=(y1+y2)*.5,rx=fabs(x2-x1)*.5,ry=fabs(y2-y1)*.5;
      int closed=N(a,n,9)!=0,steps=(int)N(a,n,10); if(steps<3)steps=3; if(steps>128)steps=128;
      gml_render_maybe_prepare_draw(R);
      for(int i=0;i<steps;i++){
        double q0=2*M_PI*i/steps,q1=2*M_PI*(i+1)/steps;
        double side[4][3]={{cx+cos(q0)*rx,cy+sin(q0)*ry,z1},{cx+cos(q1)*rx,cy+sin(q1)*ry,z1},
                           {cx,cy,z2},{cx,cy,z2}};
        d3_draw_quad(R,side,(int)N(a,n,6),N(a,n,7)/steps,N(a,n,8));
        if(closed){
          double cap[4][3]={{cx,cy,z1},{cx+cos(q1)*rx,cy+sin(q1)*ry,z1},
                            {cx+cos(q0)*rx,cy+sin(q0)*ry,z1},{cx,cy,z1}};
          d3_draw_quad(R,cap,(int)N(a,n,6),N(a,n,7),N(a,n,8));
        }
      }
    }
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_draw_ellipsoid")){
    GmlRender *R=(GmlRender*)vm->render;
    if(R && g_d3.active){
      double x1=N(a,n,0),y1=N(a,n,1),z1=N(a,n,2),x2=N(a,n,3),y2=N(a,n,4),z2=N(a,n,5);
      double cx=(x1+x2)*.5,cy=(y1+y2)*.5,cz=(z1+z2)*.5;
      double rx=fabs(x2-x1)*.5,ry=fabs(y2-y1)*.5,rz=fabs(z2-z1)*.5;
      int steps=(int)N(a,n,9); if(steps<4)steps=4; if(steps>64)steps=64;
      gml_render_maybe_prepare_draw(R);
      for(int lat=0;lat<steps;lat++) for(int lon=0;lon<steps*2;lon++){
        double a0=-M_PI*.5+M_PI*lat/steps,a1=-M_PI*.5+M_PI*(lat+1)/steps;
        double b0=M_PI*lon/steps,b1=M_PI*(lon+1)/steps;
        double p[4][3]={{cx+rx*cos(a0)*cos(b0),cy+ry*cos(a0)*sin(b0),cz+rz*sin(a0)},
                        {cx+rx*cos(a0)*cos(b1),cy+ry*cos(a0)*sin(b1),cz+rz*sin(a0)},
                        {cx+rx*cos(a1)*cos(b1),cy+ry*cos(a1)*sin(b1),cz+rz*sin(a1)},
                        {cx+rx*cos(a1)*cos(b0),cy+ry*cos(a1)*sin(b0),cz+rz*sin(a1)}};
        d3_draw_quad(R,p,(int)N(a,n,6),N(a,n,7)/steps,N(a,n,8)/steps);
      }
    }
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_draw_floor")||!strcmp(nm,"d3d_draw_wall")){
    GmlRender *R=(GmlRender*)vm->render;
    if(!strcmp(nm,"d3d_draw_wall") && getenv("GML_D3D_SKIP_WALLS")) return vreal(0);
    if(!strcmp(nm,"d3d_draw_wall")){
      const char *limit_text=getenv("GML_D3D_WALL_LIMIT");
      if(limit_text){ extern long g_vm_frame; static long limit_frame=-1; static int wall_count=0;
        if(limit_frame!=g_vm_frame){ limit_frame=g_vm_frame; wall_count=0; }
        if(wall_count++>=atoi(limit_text)) return vreal(0);
      }
    }
    if(R && g_d3.active){
      double x1=N(a,n,0),y1=N(a,n,1),z1=N(a,n,2),x2=N(a,n,3),y2=N(a,n,4),z2=N(a,n,5);
      double p[4][3];
      if(!strcmp(nm,"d3d_draw_floor")){
        double floor_points[4][3]={{x1,y1,z1},{x2,y1,z1},{x2,y2,z2},{x1,y2,z2}};
        memcpy(p,floor_points,sizeof(p));
      } else {
        double wall_points[4][3]={{x1,y1,z1},{x2,y2,z1},{x2,y2,z2},{x1,y1,z2}};
        memcpy(p,wall_points,sizeof(p));
      }
      gml_render_maybe_prepare_draw(R);
      if(getenv("GML_LOG_D3D")){ static int logged=0; if(logged++<16)
        fprintf(stderr,"[d3d] %s tex=%d rep=(%.3f,%.3f) p1=(%.1f,%.1f,%.1f) p2=(%.1f,%.1f,%.1f)\n",
                nm,(int)N(a,n,6),N(a,n,7),N(a,n,8),x1,y1,z1,x2,y2,z2); }
      d3_draw_quad(R,p,(int)N(a,n,6),N(a,n,7),N(a,n,8));
    }
    return vreal(0);
  }
  if(!strncmp(nm,"d3d_set_",8)) return vreal(0);
  if(!strcmp(nm,"shader_set")){ GmlRender *R=(GmlRender*)vm->render;
    if(R) R->active_shader=(int)N(a,n,0);
    if(getenv("GML_LOG_SHADER")){ static long c=0; if(c++<8){ extern long g_vm_frame;
      fprintf(stderr,"[shader] f%ld shader_set(%d)\n",g_vm_frame,(int)N(a,n,0)); } }
    return vreal(0); }
  if(!strcmp(nm,"shader_reset")){ GmlRender *R=(GmlRender*)vm->render;
    if(R) R->active_shader=-1; return vreal(0); }
  if(!strcmp(nm,"shader_current")){ GmlRender *R=(GmlRender*)vm->render; return vreal(R?R->active_shader:-1); }
  /* report palette shaders (template or LUT) as compiled so games keep using them (unknown -> 0) */
  if(!strcmp(nm,"shader_is_compiled")){ GmlRender *R=(GmlRender*)vm->render; int sid=(int)N(a,n,0);
    int ok = R && sid>=0 && sid<R->n_shader_pal && R->shader_pal &&
             (R->shader_pal[sid].has || R->shader_pal[sid].lut ||
              (R->shader_pal[sid].crt && R->crt_shader_enable));
    if(getenv("GML_LOG_SHADER")){ static long c=0; if(c++<6){ extern long g_vm_frame;
      fprintf(stderr,"[shader] f%ld shader_is_compiled(%d)=%d\n",g_vm_frame,sid,ok); } }
    return vreal(ok); }
  /* uniform/sampler handles: sh*16+slot. Slot 1 = the LUT row uniform (the only one the software
   * renderer models); anything else gets slot 15, whose sets are accepted and ignored. */
  if(!strcmp(nm,"shader_get_uniform")){ GmlRender *R=(GmlRender*)vm->render;
    return vreal(gml_shader_get_uniform(R,(int)N(a,n,0),S(a,n,1))); }
  if(!strcmp(nm,"shader_get_sampler_index")){ int sh=(int)N(a,n,0); return vreal(sh>=0? sh*16+2 : -1); }
  if(!strcmp(nm,"shader_set_uniform_f")||!strcmp(nm,"shader_set_uniform_f_array")){
    GmlRender *R=(GmlRender*)vm->render;
    gml_shader_set_uniform_f(R,(int)N(a,n,0),a,n);
    return vreal(0); }
  if(!strcmp(nm,"texture_set_stage")){ GmlRender *R=(GmlRender*)vm->render;
    int tex=(int)N(a,n,1);
    if(R && (((uint32_t)tex&GML_TEX_KIND_MASK)==GML_TEX_SPR_TAG)){
      R->lut_pal_sprite=(tex>>10)&0xFFFF; R->lut_pal_frame=tex&0x3FF;
      if(getenv("GML_LOG_SHADER")) fprintf(stderr,"[shader] palette texture: sprite %d frame %d\n",
        R->lut_pal_sprite,R->lut_pal_frame);
    }
    return vreal(0); }
  if(!strcmp(nm,"sprite_get_texture")){ int spr=(int)N(a,n,0), img=(int)N(a,n,1);
    if(spr<0) return vreal(-1);
    return vreal((double)(GML_TEX_SPR_TAG | ((spr&0xFFFF)<<10) | (img&0x3FF))); }
  /* animation curves: get_channel hands out a tagged handle; evaluate interpolates the knots. */
  if(!strcmp(nm,"animcurve_exists")) return vreal(acrv_curve_ptr(vm,(int)N(a,n,0))!=0);
  if(!strcmp(nm,"animcurve_get")) return vreal(N(a,n,0));   /* asset ref passes through */
  if(!strcmp(nm,"animcurve_get_channel")){
    int curve=(int)N(a,n,0), ch=0;
    if(n>=2 && a[1].t==V_STR){                    /* select channel by name */
      uint32_t cp=acrv_curve_ptr(vm,curve); ch=-1;
      if(cp){ const uint8_t *d=vm->win->data; uint32_t nch=u32(d,cp+8), p=cp+12;
        for(uint32_t i=0;i<nch;i++){ uint32_t nmp=u32(d,p), npts=u32(d,p+12);
          uint32_t ln=u32(d,nmp-4);
          if(ln==strlen(a[1].s) && !memcmp(vm->win->data+nmp,a[1].s,ln)){ ch=(int)i; break; }
          p+=16+npts*24; } }
      if(ch<0) ch=0;
    } else ch=(int)N(a,n,1);
    if(!acrv_channel_ptr(vm,curve,ch)) return vreal(-1);
    return vreal((double)(GML_ACRV_TAG | ((curve&0xFFF)<<8) | (ch&0xFF))); }
  if(!strcmp(nm,"animcurve_channel_evaluate")){
    int h=(int)N(a,n,0);
    if((h & 0x7F000000)!=GML_ACRV_TAG) return vreal(0);
    return vreal(acrv_evaluate(vm,(h>>8)&0xFFF,h&0xFF,N(a,n,1))); }
  if(!strncmp(nm,"shader_",7)) return vreal(0);
  if(!strcmp(nm,"steam_current_game_language")) return vstr("english");
  if(!strncmp(nm,"steam_",6)) return vreal(0);
  if(!strncmp(nm,"psn_",4)) return vreal(0);
  if(!strcmp(nm,"physics_fixture_create")){
    GmlPhysicsFixture *f=phys_fixture_new(vm);
    return vreal(f?(double)f->id:0);
  }
  if(!strcmp(nm,"physics_fixture_delete")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f) f->live=0;
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_bind")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f) f->bound_inst=(int)N(a,n,1);
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_add_point")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f && f->points<GML_PHYS_FIXTURE_POINTS){
      int p=f->points++;
      f->px[p]=N(a,n,1); f->py[p]=N(a,n,2);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_polygon_shape")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f){ f->shape=3; f->points=0; }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_edge_shape")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f){ f->shape=4; f->x1=N(a,n,1); f->y1=N(a,n,2); f->x2=N(a,n,3); f->y2=N(a,n,4); }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_circle_shape")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f){ f->shape=1; f->radius=N(a,n,1); }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_box_shape")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f){ f->shape=2; f->w=N(a,n,1); f->h=N(a,n,2); }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_density")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->density=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_friction")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->friction=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_restitution")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->restitution=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_linear_damping")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->lin_damp=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_angular_damping")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->ang_damp=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_awake")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->awake=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_joint_revolute_create")||!strcmp(nm,"physics_joint_prismatic_create")||
     !strcmp(nm,"physics_joint_wheel_create")||!strcmp(nm,"physics_joint_rope_create")){
    int type=!strcmp(nm,"physics_joint_revolute_create")?1:
             !strcmp(nm,"physics_joint_prismatic_create")?2:
             !strcmp(nm,"physics_joint_wheel_create")?3:4;
    GmlPhysicsJoint *j=phys_joint_new(vm,type);
    if(!j) return vreal(0);
    j->a=N(a,n,0); j->b=N(a,n,1); j->x1=N(a,n,2); j->y1=N(a,n,3);
    j->x2=N(a,n,4); j->y2=N(a,n,5);
    int pc=n-6; if(pc<0) pc=0; if(pc>24) pc=24;
    j->value_count=pc;
    for(int i=0;i<pc;i++) j->params[i]=N(a,n,6+i);
    return vreal((double)j->id);
  }
  if(!strcmp(nm,"physics_joint_delete")){
    GmlPhysicsJoint *j=phys_joint_find(vm,(int)N(a,n,0));
    if(j) j->live=0;
    return vreal(0);
  }
  if(!strcmp(nm,"physics_joint_set_value")){
    GmlPhysicsJoint *j=phys_joint_find(vm,(int)N(a,n,0));
    int idx=(int)N(a,n,1);
    if(j && idx>=0 && idx<24){ j->params[idx]=N(a,n,2); if(idx>=j->value_count) j->value_count=idx+1; }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_mass_properties")){
    if(vm->cur_self){
      *gml_varmap_put(&vm->cur_self->vars,"phy_mass")=vreal(N(a,n,0));
      *gml_varmap_put(&vm->cur_self->vars,"phy_inertia")=vreal(N(a,n,3));
    }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_world_create")){
    double scale=N(a,n,0);
    if(scale>0.0){
      *gml_varmap_put(&vm->globals,"__physics_world_scale")=vreal(scale);
      *gml_varmap_put(&vm->globals,"__physics_world_scale_room")=vreal(vm->room_index);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_world_gravity")){ vm->phys_gravity_x=N(a,n,0); vm->phys_gravity_y=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_world_update_speed")){ vm->phys_update_speed=N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"physics_world_update_iterations")){ vm->phys_update_iterations=(int)N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"physics_pause_enable")){ vm->phys_paused=(int)N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"physics_world_draw_debug")){ vm->phys_debug_draw=(int)N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"physics_apply_impulse")){
    /* Convert Box2D's metre/second impulse response to the lightweight backend's pixels/step.
     * The two-metre translation cap is the public engine's per-step tunnelling guard. */
    GmlInstance *s=vm->cur_self;
    if(s && !vm->phys_paused){
      double scale=physics_room_scale(vm);
      double mass=physics_instance_mass(vm,s,scale);
      double hz=gml_room_speed(vm);
      if(mass>0.0 && hz>0.0){
        double ix=N(a,n,2), iy=N(a,n,3);
        double vx=s->hspeed+ix/(mass*scale*hz);
        double vy=s->vspeed+iy/(mass*scale*hz);
        double max_step=2.0/scale, step=hypot(vx,vy);
        if(step>max_step){ double k=max_step/step; vx*=k; vy*=k; }
        s->hspeed=vx; s->vspeed=vy;
        motion_from_components(s);
        if(getenv("GML_LOG_PHYSICS"))
          fprintf(stderr,"[physics] impulse id=%u mass=%.6g scale=%.6g vel=(%.6g,%.6g)\n",
                  s->id,mass,scale,s->hspeed,s->vspeed);
      }
    }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_apply_local_force")) return vreal(0);
  if(!strncmp(nm,"physics_",8)) return vreal(0);
  if(!strncmp(nm,"skeleton_",9)) return builtin_skeleton(vm,nm,a,n);
  /* Report Galaxy initialization as successful; other Galaxy operations return zero.
   * This does not establish an online session or authenticate an account. */
  if(!strcmp(nm,"gog_init")||!strcmp(nm,"gog_is_initialised")||!strcmp(nm,"gog_is_initialized")) return vreal(1);
  if(!strcmp(nm,"gog_get_achievement")||!strcmp(nm,"gog_is_user_logged_on")||
     !strcmp(nm,"gog_set_achievement")||!strcmp(nm,"gog_stats_ready")||
     !strcmp(nm,"gog_update")) return vreal(0);
  if(!strncmp(nm,"gog_",4)) return vreal(0);
  if(!strcmp(nm,"show_debug_message")||!strcmp(nm,"show_debug_overlay")){
    if(getenv("GML_LOG_DEBUGMSG")) fprintf(stderr,"[gml debug] %s\n", S(a,n,0));
    return vreal(0); }
  if(!strncmp(nm,"xboxone_",8)) return vreal(0);
  if(!strcmp(nm,"screen_save")||!strcmp(nm,"screen_save_part")) return vreal(0);
  if(!strcmp(nm,"os_get_language")) return vstr("en");
  if(!strcmp(nm,"os_get_region")) return vstr("us");
  if(!strcmp(nm,"os_get_config")) return vstr("default");
  if(!strcmp(nm,"gml_release_mode")) return vreal(0);
  if(!strcmp(nm,"os_is_paused")) return vreal(0);
  if(!strcmp(nm,"window_has_focus")) return vreal(1);
  if(!strcmp(nm,"window_get_x")) return vreal(vm->window_x);
  if(!strcmp(nm,"window_get_y")) return vreal(vm->window_y);
  if(!strcmp(nm,"window_get_cursor")) return vreal(vm->window_cursor);
  if(!strcmp(nm,"window_get_caption")) return vstr("");
  if(!strcmp(nm,"window_handle")) return vreal(1);
  if(!strcmp(nm,"window_get_fullscreen")) return vreal(vm->window_fullscreen);
  if(!strcmp(nm,"window_set_fullscreen")){ vm->window_fullscreen=N(a,n,0)>=0.5; return vreal(0); }
  if(!strcmp(nm,"window_center")){ vm->window_x=0; vm->window_y=0; return vreal(0); }
  if(!strcmp(nm,"window_set_position")){ vm->window_x=N(a,n,0); vm->window_y=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"window_set_size")){ int w=(int)N(a,n,0), hh=(int)N(a,n,1);
    if(w>0 && hh>0 && w<=16384 && hh<=16384){ vm->window_w=w; vm->window_h=hh; }
    return vreal(0); }
  if(!strcmp(nm,"window_set_rectangle")){ int w=(int)N(a,n,2), hh=(int)N(a,n,3);
    vm->window_x=N(a,n,0); vm->window_y=N(a,n,1);
    if(w>0 && hh>0 && w<=16384 && hh<=16384){ vm->window_w=w; vm->window_h=hh; }
    return vreal(0); }
  if(!strcmp(nm,"window_set_cursor")){ vm->window_cursor=(int)N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"window_set_caption")) return vreal(0);
  if(!strcmp(nm,"display_set_gui_maximise")) return vreal(0);

  GmlVal layer_out;
  if(builtin_layer_exact(vm,nm,a,n,&layer_out)) return layer_out;

  /* ---- explicit no-ops (correct for libretro / SW renderer) ---- */
  if(!strcmp(nm,"display_set_gui_size")){ int w=(int)N(a,n,0), hh=(int)N(a,n,1);
    if(w>0 && hh>0 && w<=16384 && hh<=16384){ vm->gui_w=w; vm->gui_h=hh; }
    return vreal(0); }
  if(!strcmp(nm,"texture_set_interpolation")){ GmlRender *R=(GmlRender*)vm->render;
    if(R) R->interp = (N(a,n,0)!=0.0); return vreal(0); }
  if(!strcmp(nm,"texture_set_repeat")) return vreal(0);
  if(!strcmp(nm,"gpu_set_texfilter")||!strcmp(nm,"gpu_set_texfilter_ext")) return vreal(0);
  if(!strcmp(nm,"gpu_set_blendenable")){ GmlRender *R=(GmlRender*)vm->render; if(R) R->alphablend=N(a,n,0)>=0.5; return vreal(0); }
  if(!strcmp(nm,"gpu_get_blendenable")){ GmlRender *R=(GmlRender*)vm->render; return vreal(R?R->alphablend:1); }
  if(!strcmp(nm,"window_get_fullscreen")) return vreal(vm->window_fullscreen);
  if(!strcmp(nm,"window_set_fullscreen")){ vm->window_fullscreen=N(a,n,0)>=0.5; return vreal(0); }

  /* ---- fallback: a user script called by name. bc14-16 reference scripts by BARE name (scr_foo,
   * whose CODE entry is gml_Script_scr_foo); bc17/GMS2 references them ALREADY prefixed
   * (gml_Script_foo). Handle both so scripts dispatch instead of falling through to a no-op. ---- */
  int ci;
  if(!strncmp(nm,"gml_Script_",11)) ci=gml_code_index_by_name(vm->win,nm);
  else {
    char sn[160];
    snprintf(sn,sizeof sn,"gml_Script_%s",nm);
    ci=gml_code_index_by_name(vm->win,sn);
    if(ci<0){
      snprintf(sn,sizeof sn,"gml_GlobalScript_%s",nm);
      ci=gml_code_index_by_name(vm->win,sn);
    }
  }
  if(ci>=0){
    GmlVal rv=gml_vm_run_code(vm,ci,vm->cur_self,vm->cur_other,a,n);
    /* report the resolution for the caller's per-site cache — but never for names the chain
     * above may shadow natively later (the tile_* compat shadows only engage once runtime
     * layers exist, so caching them here would pin the slow bytecode path). Set AFTER the run:
     * nested calls inside the script clobber the side channel while it executes. */
    const char *bare2 = strncmp(nm,"gml_Script_",11)? nm : nm+11;
    if(strcmp(bare2,"tile_layer_find") && strcmp(bare2,"tile_delete"))
      vm->call_script_ci=ci;
    return rv;
  }

  /* Store runtime layer elements in vm->rte and layers in vm->rtl. Drawing merges
   * visible tile/background elements into the depth-sorted list. */
  /* Resolve tilemap IDs from layers and read raw tile data; tile_get_index extracts the tile index. */
  if(!strcmp(nm,"layer_tilemap_get_id")){ extern GmlTileMap *gml_tilemap_by_layer(GmlVM*,GmlVal);
    GmlTileMap *tm=gml_tilemap_by_layer(vm, n>0?a[0]:vreal(-1));
    if(log_tilecol_on()){ if(n>0&&a[0].t==V_STR) fprintf(stderr,"[tilecol] layer_tilemap_get_id(\"%s\") -> %d\n",a[0].s?a[0].s:"",tm?tm->id:-1);
      else fprintf(stderr,"[tilecol] layer_tilemap_get_id(%g) -> %d (%s)\n",N(a,n,0),tm?tm->id:-1,tm?tm->name:"none"); }
    return vreal(tm?tm->id:-1); }
  if(!strcmp(nm,"tilemap_get_cell_x_at_pixel")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0));
    double tx=0.0; gml_tilemap_effective(vm,tm,&tx,NULL,NULL,NULL);
    return vreal(tm&&tm->tw?floor((N(a,n,1)-tx)/tm->tw):0); }
  if(!strcmp(nm,"tilemap_get_cell_y_at_pixel")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0));
    double ty=0.0; gml_tilemap_effective(vm,tm,NULL,&ty,NULL,NULL);
    return vreal(tm&&tm->th?floor((N(a,n,2)-ty)/tm->th):0); }
  if(!strcmp(nm,"tilemap_get")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0));
    int cx=(int)N(a,n,1),cy=(int)N(a,n,2);
    if(!tm||cx<0||cy<0||cx>=tm->cols||cy>=tm->rows){ if(log_tilecol_on()) fprintf(stderr,"[tilecol] tilemap_get(id=%g,%d,%d) -> 0 (tm=%s oob)\n",N(a,n,0),cx,cy,tm?tm->name:"NULL"); return vreal(0); }
    const unsigned char *p=tm->tiles+((size_t)cy*tm->cols+cx)*4;
    uint32_t datum=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
    if(log_tilecol_on()) fprintf(stderr,"[tilecol] tilemap_get(%s,%d,%d) -> %u (idx=%u)\n",tm->name,cx,cy,datum,datum&0x7FFFF);
    return vreal((double)datum); }
  if(!strcmp(nm,"tilemap_get_at_pixel")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0));
    if(!tm||!tm->tw||!tm->th) return vreal(0);
    double tx,ty; gml_tilemap_effective(vm,tm,&tx,&ty,NULL,NULL);
    int cx=(int)floor((N(a,n,1)-tx)/tm->tw), cy=(int)floor((N(a,n,2)-ty)/tm->th);
    if(cx<0||cy<0||cx>=tm->cols||cy>=tm->rows) return vreal(0);
    const unsigned char *p=tm->tiles+((size_t)cy*tm->cols+cx)*4;
    return vreal((double)((uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24))); }
  if(!strcmp(nm,"tilemap_set")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0));
    gml_tilemap_set_cell(tm,(int)N(a,n,2),(int)N(a,n,3),(uint32_t)N(a,n,1)); return vreal(0); }
  if(!strcmp(nm,"tilemap_set_at_pixel")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0));
    if(tm&&tm->tw&&tm->th){
      double tx,ty; gml_tilemap_effective(vm,tm,&tx,&ty,NULL,NULL);
      int cx=(int)floor((N(a,n,2)-tx)/tm->tw), cy=(int)floor((N(a,n,3)-ty)/tm->th);
      gml_tilemap_set_cell(tm,cx,cy,(uint32_t)N(a,n,1));
    }
    return vreal(0); }
  if(!strcmp(nm,"tilemap_get_width")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0)); return vreal(tm?tm->cols:0); }
  if(!strcmp(nm,"tilemap_get_height")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0)); return vreal(tm?tm->rows:0); }
  if(!strcmp(nm,"tilemap_get_tileset")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0)); return vreal(tm?tm->tileset:-1); }
  if(!strcmp(nm,"tilemap_get_tile_width")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0)); return vreal(tm?tm->tw:0); }
  if(!strcmp(nm,"tilemap_get_tile_height")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0)); return vreal(tm?tm->th:0); }
  if(!strcmp(nm,"tilemap_get_x")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0)); double tx=0.0; gml_tilemap_effective(vm,tm,&tx,NULL,NULL,NULL); return vreal(tm?tx:0); }
  if(!strcmp(nm,"tilemap_get_y")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0)); double ty=0.0; gml_tilemap_effective(vm,tm,NULL,&ty,NULL,NULL); return vreal(tm?ty:0); }
  if(!strcmp(nm,"tilemap_get_frame")) return vreal(0);
  /* tile datum decode (GMS2 bit layout: index low 19 bits, then mirror/flip/rotate) */
  if(!strcmp(nm,"tile_get_index")){ uint32_t t=(uint32_t)N(a,n,0); return vreal(t & 0x7FFFF); }
  if(!strcmp(nm,"tile_get_empty")){ uint32_t t=(uint32_t)N(a,n,0); return vreal((t & 0x7FFFF)==0); }
  if(!strcmp(nm,"tile_get_mirror")){ uint32_t t=(uint32_t)N(a,n,0); return vreal((t>>28)&1); }
  if(!strcmp(nm,"tile_get_flip")){ uint32_t t=(uint32_t)N(a,n,0); return vreal((t>>29)&1); }
  if(!strcmp(nm,"tile_get_rotate")){ uint32_t t=(uint32_t)N(a,n,0); return vreal((t>>30)&1); }
  if(!strcmp(nm,"tile_set_empty")){ uint32_t t=(uint32_t)N(a,n,0); return vreal((double)(t & ~0x7FFFFu)); }
  if(!strcmp(nm,"tile_set_index")){ uint32_t t=(uint32_t)N(a,n,0), idx=(uint32_t)N(a,n,1);
    return vreal((double)((t & ~0x7FFFFu) | (idx & 0x7FFFFu))); }
  if(!strcmp(nm,"tile_set_mirror")){ uint32_t t=(uint32_t)N(a,n,0);
    if(N(a,n,1)>=0.5) t|=(1u<<28); else t&=~(1u<<28); return vreal((double)t); }
  if(!strcmp(nm,"tile_set_flip")){ uint32_t t=(uint32_t)N(a,n,0);
    if(N(a,n,1)>=0.5) t|=(1u<<29); else t&=~(1u<<29); return vreal((double)t); }
  if(!strcmp(nm,"tile_set_rotate")){ uint32_t t=(uint32_t)N(a,n,0);
    if(N(a,n,1)>=0.5) t|=(1u<<30); else t&=~(1u<<30); return vreal((double)t); }
  if(!strcmp(nm,"layer_script_begin")||!strcmp(nm,"layer_script_end")){
    GmlRtLayer *l=rt_layer_resolve(vm,a,n);
    if(l){
      int ci=(n>1)?script_ref_code_of(vm,a[1]):-1;
      if(!strcmp(nm,"layer_script_begin")) l->script_begin=ci;
      else l->script_end=ci;
    }
    return vreal(0);
  }
  if(!strcmp(nm,"layer_force_draw_depth")) return vreal(0);
  if(!strncmp(nm,"layer_",6)){
    const char *sub=nm+6;
    if(!strcmp(sub,"create")){
      GmlRtLayer *l=gml_rt_layer_new(vm);
      if(!l) return vreal(-1);
      l->depth=N(a,n,0);
      if(getenv("GML_LOG_RTL")){ extern long g_vm_frame;
        fprintf(stderr,"[rtl] f%ld layer_create depth=%f id=%d\n",g_vm_frame,l->depth,l->id); }
      snprintf(l->name,sizeof l->name,"%s",(n>=2 && a[1].t==V_STR && a[1].s)?a[1].s:"_rt_layer");
      return vreal(l->id);
    }
    if(!strcmp(sub,"destroy")){
      GmlRtLayer *l=gml_rt_layer_find(vm,(int)N(a,n,0));
      if(l){ for(int i=0;i<vm->n_rte;i++) if(vm->rte[i].used && vm->rte[i].layer==l->id) vm->rte[i].used=0;
        l->used=0; }
      return vreal(0);
    }
    if(!strcmp(sub,"get_all")){
      int cnt=0; for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used) cnt++;
      GmlVal v=arr_newv(cnt); if(v.t!=V_ARR) return v;
      GmlArr *A=(GmlArr*)v.arr; int k=0;
      for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used) A->data[k++]=vreal(vm->rtl[i].id);
      return v;
    }
    if(!strcmp(sub,"get_all_elements")){
      int lid=(int)N(a,n,0), cnt=0;
      for(int i=0;i<vm->n_rte;i++) if(vm->rte[i].used && vm->rte[i].layer==lid) cnt++;
      GmlVal v=arr_newv(cnt); if(v.t!=V_ARR) return v;
      GmlArr *A=(GmlArr*)v.arr; int k=0;
      for(int i=0;i<vm->n_rte;i++) if(vm->rte[i].used && vm->rte[i].layer==lid) A->data[k++]=vreal(vm->rte[i].id);
      return v;
    }
    if(!strcmp(sub,"get_element_type")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0));
      return vreal(e?e->type:-1); }
    if(!strcmp(sub,"get_element_layer")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0));
      return vreal(e?e->layer:-1); }
    if(!strcmp(sub,"sprite_get_id")){
      GmlRtLayer *l=rt_layer_resolve(vm,a,n);
      GmlRtElem *e=rt_sprite_for_layer_name(vm,l?l->id:-1,S(a,n,1));
      return vreal(e?e->id:-1);
    }
    if(!strcmp(sub,"sprite_create")){
      GmlRtLayer *l=rt_layer_resolve(vm,a,n);
      if(!l) return vreal(-1);
      GmlRtElem *e=gml_rt_elem_new(vm);
      if(!e) return vreal(-1);
      e->type=3; e->layer=l->id; e->x=N(a,n,1); e->y=N(a,n,2); e->sprite=(int)N(a,n,3);
      return vreal(e->id);
    }
    if(!strcmp(sub,"sprite_destroy")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->used=0; return vreal(0); }
    if(!strcmp(sub,"sprite_exists")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e && e->type==3); }
    if(!strcmp(sub,"sprite_x")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->x=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"sprite_y")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->y=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"sprite_alpha")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->alpha=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"sprite_speed")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->image_speed=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"sprite_index")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->image_index=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"sprite_change")||!strcmp(sub,"sprite_sprite")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->sprite=(int)N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"sprite_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->xs=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"sprite_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->ys=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"sprite_angle")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->image_angle=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"sprite_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->blend=(uint32_t)N(a,n,1)&0xFFFFFFu; return vreal(0); }
    if(!strcmp(sub,"sprite_visible")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->visible=(int)N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"sprite_get_x")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal((e && e->type==3)?e->x:0); }
    if(!strcmp(sub,"sprite_get_y")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal((e && e->type==3)?e->y:0); }
    if(!strcmp(sub,"sprite_get_sprite")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal((e && e->type==3)?e->sprite:-1); }
    if(!strcmp(sub,"sprite_get_alpha")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal((e && e->type==3)?e->alpha:0); }
    if(!strcmp(sub,"sprite_get_speed")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal((e && e->type==3)?e->image_speed:0); }
    if(!strcmp(sub,"sprite_get_index")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal((e && e->type==3)?e->image_index:0); }
    if(!strcmp(sub,"sprite_get_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal((e && e->type==3)?e->xs:1); }
    if(!strcmp(sub,"sprite_get_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal((e && e->type==3)?e->ys:1); }
    if(!strcmp(sub,"sprite_get_angle")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal((e && e->type==3)?e->image_angle:0); }
    if(!strcmp(sub,"sprite_get_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal((e && e->type==3)?(double)e->blend:0xFFFFFF); }
    if(!strcmp(sub,"sprite_get_visible")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e && e->type==3 && e->visible); }
    if(!strcmp(sub,"sprite_get_name")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return (e && e->type==3)?vstr_owned(strdup(e->name)):vstr(""); }
    if(!strcmp(sub,"background_get_id")){
      GmlRtLayer *l=rt_layer_resolve(vm,a,n);
      GmlRtElem *e=rt_background_for_layer(vm,l,1);
      return vreal(e?e->id:-1);
    }
    if(!strcmp(sub,"get_depth")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); return vreal(l?l->depth:0); }
    if(!strcmp(sub,"depth")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n);
      if(l){
        double old=l->depth;
        l->depth=N(a,n,1);
        if(getenv("GML_LOG_RTL")){ extern long g_vm_frame;
          fprintf(stderr,"[rtl] f%ld layer_depth %s id=%d %.0f -> %.0f\n",
                  g_vm_frame,l->name,l->id,old,l->depth); }
      }else if(getenv("GML_LOG_RTL")){
        extern long g_vm_frame;
        fprintf(stderr,"[rtl] f%ld layer_depth unresolved arg0=%s value=%.0f\n",
                g_vm_frame,S(a,n,0),N(a,n,1));
      }
      return vreal(0); }
    if(!strcmp(sub,"get_name")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n);
      return l?vstr_owned(strdup(l->name)):vstr(""); }
    if(!strcmp(sub,"get_id")){ for(int i=0;i<vm->n_rtl;i++)   /* by name */
        if(vm->rtl[i].used && !strcmp(vm->rtl[i].name,S(a,n,0))) return vreal(vm->rtl[i].id);
      return vreal(-1); }
    if(!strcmp(sub,"exists")){ return vreal(rt_layer_resolve(vm,a,n)!=NULL); }
    if(!strcmp(sub,"force_draw_depth")) return vreal(0);
    if(!strcmp(sub,"script_begin")||!strcmp(sub,"script_end")){
      GmlRtLayer *l=rt_layer_resolve(vm,a,n);
      if(l){
        int ci=(n>1)?script_ref_code_of(vm,a[1]):-1;
        if(!strcmp(sub,"script_begin")) l->script_begin=ci;
        else l->script_end=ci;
      }
      return vreal(0);
    }
    if(!strcmp(sub,"set_visible")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(l) l->visible=(int)N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"get_visible")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); return vreal(l?l->visible:0); }
    if(!strcmp(sub,"x")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(l){ rt_layer_touch(vm,l); l->x=N(a,n,1); } return vreal(0); }
    if(!strcmp(sub,"y")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(l){ rt_layer_touch(vm,l); l->y=N(a,n,1); } return vreal(0); }
    if(!strcmp(sub,"hspeed")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(l){ rt_layer_touch(vm,l); l->hs=N(a,n,1); } return vreal(0); }
    if(!strcmp(sub,"vspeed")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(l){ rt_layer_touch(vm,l); l->vs=N(a,n,1); } return vreal(0); }
    if(!strcmp(sub,"get_x")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(!l) return vreal(0);
      if(l->touched) return vreal(l->x);
      extern long g_vm_frame; long fin=g_vm_frame-vm->room_enter_frame; if(fin<0)fin=0; return vreal(l->x+l->hs*fin); }
    if(!strcmp(sub,"get_y")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(!l) return vreal(0);
      if(l->touched) return vreal(l->y);
      extern long g_vm_frame; long fin=g_vm_frame-vm->room_enter_frame; if(fin<0)fin=0; return vreal(l->y+l->vs*fin); }
    if(!strcmp(sub,"get_hspeed")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); return vreal(l?l->hs:0); }
    if(!strcmp(sub,"get_vspeed")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); return vreal(l?l->vs:0); }
    if(!strcmp(sub,"force_draw_depth")||!strcmp(sub,"reset_target_all")) return vreal(0);
    if(!strcmp(sub,"tile_create")){
      /* layer_tile_create(layer, x, y, sprite, left, top, width, height) -> element id */
      GmlRtElem *e=gml_rt_elem_new(vm);
      if(!e) return vreal(-1);
      e->type=7; e->layer=(int)N(a,n,0);
      e->x=N(a,n,1); e->y=N(a,n,2); e->sprite=(int)N(a,n,3);
      e->sx=(int)N(a,n,4); e->sy=(int)N(a,n,5); e->w=(int)N(a,n,6); e->h=(int)N(a,n,7);
      return vreal(e->id);
    }
    if(!strcmp(sub,"tile_destroy")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->used=0; return vreal(0); }
    if(!strcmp(sub,"tile_exists")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e && e->type==7); }
    if(!strcmp(sub,"tile_change")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->sprite=(int)N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"tile_x")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->x=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"tile_y")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->y=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"tile_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->xs=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"tile_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->ys=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"tile_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->blend=(uint32_t)N(a,n,1)&0xFFFFFF; return vreal(0); }
    if(!strcmp(sub,"tile_alpha")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->alpha=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"tile_visible")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->visible=(int)N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"tile_region")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0));
      if(e){ e->sx=(int)N(a,n,1); e->sy=(int)N(a,n,2); e->w=(int)N(a,n,3); e->h=(int)N(a,n,4); } return vreal(0); }
    if(!strcmp(sub,"tile_get_sprite")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->sprite:-1); }
    if(!strcmp(sub,"tile_get_x")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->x:0); }
    if(!strcmp(sub,"tile_get_y")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->y:0); }
    if(!strcmp(sub,"tile_get_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->xs:1); }
    if(!strcmp(sub,"tile_get_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->ys:1); }
    if(!strcmp(sub,"tile_get_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?(double)e->blend:0xFFFFFF); }
    if(!strcmp(sub,"tile_get_alpha")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->alpha:1); }
    if(!strcmp(sub,"tile_get_visible")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->visible:0); }
    if(!strcmp(sub,"tile_get_region")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0));
      return e?array4(e->sx,e->sy,e->w,e->h):array4(0,0,0,0); }
    if(!strcmp(sub,"background_create")){
      /* layer_background_create(layer, sprite) -> element id */
      GmlRtElem *e=gml_rt_elem_new(vm);
      if(!e) return vreal(-1);
      e->type=1; e->layer=(int)N(a,n,0); e->sprite=(int)N(a,n,1);
      return vreal(e->id);
    }
    if(!strcmp(sub,"background_destroy")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->used=0; return vreal(0); }
    if(!strcmp(sub,"background_exists")){  /* (layer_id, element_id) */
      GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,1));
      return vreal(e && e->type==1 && e->layer==(int)N(a,n,0)); }
    if(!strcmp(sub,"background_change")||!strcmp(sub,"background_sprite")){
      GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->sprite=(int)N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_visible")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->visible=(int)N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_alpha")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->alpha=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->blend=(uint32_t)N(a,n,1)&0xFFFFFF; return vreal(0); }
    if(!strcmp(sub,"background_htiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->htiled=(int)N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_vtiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->vtiled=(int)N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_stretch")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->stretch=(int)N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->xs=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->ys=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_index")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->sx=(int)N(a,n,1); return vreal(0); }  /* image frame */
    if(!strcmp(sub,"background_speed")) return vreal(0);
    if(!strcmp(sub,"background_get_sprite")||!strcmp(sub,"background_get_index")){
      GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->sprite:-1); }
    if(!strcmp(sub,"background_get_visible")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->visible:0); }
    if(!strcmp(sub,"background_get_alpha")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->alpha:1); }
    if(!strcmp(sub,"background_get_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?(double)e->blend:0xFFFFFF); }
    if(!strcmp(sub,"background_get_htiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->htiled:0); }
    if(!strcmp(sub,"background_get_vtiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->vtiled:0); }
    if(!strcmp(sub,"background_get_stretch")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->stretch:0); }
    if(!strcmp(sub,"background_get_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->xs:1); }
    if(!strcmp(sub,"background_get_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->ys:1); }
    if(!strcmp(sub,"background_get_speed")) return vreal(0);
    /* unhandled layer_* fall through to the audit log below */
  }

  /* ---- draw / audio / misc: stubbed until the renderer/audio land ----
   * Keep this after script fallback: user scripts can legally share prefixes
   * such as draw_ or audio_ and must resolve before broad no-op fallbacks. */
  /* Explicit no-ops: functions that are legitimately inert for a software-rendered libretro core.
   * Handling them here (instead of the catch-all) documents that intent and keeps them out of the
   * "unknown builtin" audit log. mouse_wheel_* have no input source → 0 (not scrolled). */
  if(!strcmp(nm,"mouse_wheel_up")){ int wh; gml_input_mouse(NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,&wh); return vreal(wh>0); }
  if(!strcmp(nm,"mouse_wheel_down")){ int wh; gml_input_mouse(NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,&wh); return vreal(wh<0); }
  if(!strcmp(nm,"device_get_tilt_x")||!strcmp(nm,"device_get_tilt_y")||!strcmp(nm,"device_get_tilt_z")) return vreal(0);
  /* Select additive or subtractive blending for their enum values; use normal alpha otherwise. */
  if(!strcmp(nm,"draw_set_blend_mode")||   /* Legacy API spelling. */
     !strcmp(nm,"gpu_set_blendmode")){ GmlRender *R2=(GmlRender*)vm->render; int bm=(int)N(a,n,0);
    if(getenv("GML_DBG_BM")) fprintf(stderr,"[bm] gpu_set_blendmode(%d)\n",bm);
    if(R2) R2->blendmode = (bm==1)?1 : (bm==3)?2 : 0; return vreal(0); }
  if(!strcmp(nm,"gpu_get_blendmode")){ GmlRender *R2=(GmlRender*)vm->render; return vreal(R2?R2->blendmode:0); }
  if(!strcmp(nm,"gpu_set_blendmode_ext")||!strcmp(nm,"gpu_set_blendmode_ext_sepalpha")||
     !strcmp(nm,"draw_set_blend_mode_ext")){
    builtin_set_blendmode_ext((GmlRender*)vm->render,(int)N(a,n,0),(int)N(a,n,1));
    return vreal(0);
  }
  if(!strcmp(nm,"gpu_set_sprite_cull")||!strcmp(nm,"gpu_set_alphatestenable")||
     !strcmp(nm,"gpu_set_tex_filter")||!strcmp(nm,"gpu_set_colorwriteenable")||
     !strcmp(nm,"display_reset")||!strcmp(nm,"display_set_gui_maximize")||
     !strcmp(nm,"window_set_cursor")||!strcmp(nm,"keyboard_set_map")||!strcmp(nm,"keyboard_unset_map")||
     !strcmp(nm,"device_mouse_dbclick_enable")||
     !strcmp(nm,"achievement_login")||!strcmp(nm,"achievement_logout")||
     !strncmp(nm,"native_ga_",10)||!strncmp(nm,"configureBuild",14)||!strncmp(nm,"configureSdk",12)||
     !strncmp(nm,"setEnabledInfoLog",17)||!strncmp(nm,"setEnabledVerboseLog",20))
    return vreal(0);

  /* GML_LOG_STUB (audit aid): list every builtin that hits the broad no-op / catch-all. Cache the
   * env check — this is a hot path in particle-heavy games (part_system_drawit etc. fall here). */
  static int log_stub = -1;
  if(log_stub<0) log_stub = getenv("GML_LOG_STUB")!=NULL;
  if(!strncmp(nm,"draw_",5)||!strncmp(nm,"audio_",6)||!strncmp(nm,"path_",5)||
     !strncmp(nm,"place_",6)||!strncmp(nm,"move_",5)||!strncmp(nm,"tile_",5)||
     !strncmp(nm,"font_",5)||!strncmp(nm,"window_",7)||!strncmp(nm,"instance_",9)){
    if(log_stub) fprintf(stderr,"[stub] %s\n",nm);
    return vreal(0);
  }

  /* unknown builtin: log once per run so coverage audits can spot regressions */
  static int unk_logged=0;
  if(log_stub) fprintf(stderr,"[unk] %s\n",nm);
  if(!unk_logged){ fprintf(stderr,"[gml] unknown builtin: %s\n",nm); unk_logged=1; }
  return vreal(0);
}
