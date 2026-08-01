/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_builtin.c — GameMaker built-in function dispatch. */
#include "gml_builtin_internal.h"
#include "anygm_compatibility.h"
#include "gml_render.h"
#include "gml_particle.h"
#include "gml_audio.h"
#include "gml_fmod.h"
#include "anygm_host.h"
#include "anygm_vfs.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <ctype.h>
#include <inttypes.h>

const char *builtin_setting(const GmlVM *vm,const char *name){
  return anygm_host_development_setting(vm?vm->host:NULL,name);
}

int builtin_log_tile_collision(GmlVM *vm){
  GmlBuiltinState *state=builtin_state_ensure(vm);
  if(!state) return builtin_setting(vm,"GML_LOG_TILECOL")!=NULL;
  if(state->log_tile_collision<0) state->log_tile_collision=builtin_setting(vm,"GML_LOG_TILECOL")!=NULL;
  return state->log_tile_collision;
}
GmlRenderDrawState builtin_draw_state(const GmlRender *render){
  GmlRenderDrawState state;
  (void)gml_render_draw_state_get(render,&state);
  return state;
}
GmlRenderTargetMetrics builtin_target_metrics(const GmlRender *render){
  GmlRenderTargetMetrics metrics;
  (void)gml_render_target_metrics(render,&metrics);
  return metrics;
}
GmlRenderPresentationMetrics builtin_presentation_metrics(const GmlRender *render){
  GmlRenderPresentationMetrics metrics;
  (void)gml_render_presentation_metrics(render,&metrics);
  return metrics;
}
void builtin_set_draw_color(GmlRender *render,uint32_t color){
  GmlRenderDrawState state={0};
  state.color=color;
  gml_render_draw_state_update(render,&state,GML_RENDER_DRAW_STATE_COLOR);
}
void builtin_set_draw_alpha(GmlRender *render,double alpha){
  GmlRenderDrawState state={0};
  if(alpha<0.0) alpha=0.0;
  if(alpha>1.0) alpha=1.0;
  state.alpha=alpha;
  gml_render_draw_state_update(render,&state,GML_RENDER_DRAW_STATE_ALPHA);
}
void builtin_set_draw_font(GmlVM *vm,GmlRender *render,int font){
  GmlRenderDrawState state={0};
  state.font=font;
  gml_render_draw_state_update(render,&state,GML_RENDER_DRAW_STATE_FONT);
  if(render && font<0 && builtin_setting(vm,"GML_LOG_FONT"))
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
                    "[font] draw_set_font(%d) — default-font request\n",font);
}
void builtin_set_draw_halign(GmlRender *render,int alignment){
  GmlRenderDrawState state={0};
  state.horizontal_alignment=alignment;
  gml_render_draw_state_update(render,&state,GML_RENDER_DRAW_STATE_HORIZONTAL_ALIGNMENT);
}
void builtin_set_draw_valign(GmlRender *render,int alignment){
  GmlRenderDrawState state={0};
  state.vertical_alignment=alignment;
  gml_render_draw_state_update(render,&state,GML_RENDER_DRAW_STATE_VERTICAL_ALIGNMENT);
}
void builtin_set_alpha_blend(GmlRender *render,int enabled){
  GmlRenderDrawState state={0};
  state.alpha_blend=enabled;
  gml_render_draw_state_update(render,&state,GML_RENDER_DRAW_STATE_ALPHA_BLEND);
}
void builtin_set_interpolation(GmlRender *render,int enabled){
  GmlRenderDrawState state={0};
  state.interpolation=enabled;
  gml_render_draw_state_update(render,&state,GML_RENDER_DRAW_STATE_INTERPOLATION);
}
void builtin_set_blendmode_ext(GmlVM *vm,GmlRender *R,int src,int dst){
  if(!R) return;
  /* Documented factor constants: zero=1, one=2, src_alpha=5, inv_src_alpha=6,
   * dest_colour=9. Preserve the common preset-equivalent pairs and the fixed-function multiply
   * pair used for light/shadow masks. Unknown pairs still replace the old state with normal. */
  GmlRenderDrawState state={0};
  if(src==9 && dst==1) state.blend_mode=3;             /* src * destination colour */
  else if(src==5 && dst==2) state.blend_mode=1;        /* additive src-alpha */
  else if(src==1 && dst==4) state.blend_mode=2;        /* zero, inverse source colour */
  else if(src==5 && dst==4) state.blend_mode=4;        /* source alpha, inverse source colour */
  else state.blend_mode=0;                             /* includes normal (5,6) */
  gml_render_draw_state_update(R,&state,GML_RENDER_DRAW_STATE_BLEND_MODE);
  if(builtin_setting(vm,"GML_DBG_BM"))
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
                    "[bm] gpu_set_blendmode_ext(%d,%d) -> mode %d\n",
                    src,dst,state.blend_mode);
}
void builtin_set_blendmode(GmlRender *R, int bm){
  if(!R) return;
  GmlRenderDrawState state={0};
  state.blend_mode=(bm==1)?1:(bm==2)?4:(bm==3)?2:0;
  /* A preset blend mode also selects its equation; the software presets above
   * encode add/subtract themselves, so the independent equation returns to add. */
  state.blend_equation=state.blend_equation_alpha=1;
  gml_render_draw_state_update(R,&state,
    GML_RENDER_DRAW_STATE_BLEND_MODE|
    GML_RENDER_DRAW_STATE_BLEND_EQUATION|
    GML_RENDER_DRAW_STATE_BLEND_EQUATION_ALPHA);
}
static int presentation_base_size(const GmlVM *vm, const GmlRender *r, int height){
  GmlRenderPresentationMetrics metrics=builtin_presentation_metrics(r);
  if(r && metrics.wide_aspect_active){
    int wide=height?metrics.wide_height:metrics.wide_width;
    if(wide>0) return wide;
  }
  if(vm && vm->win){
    int native=height?(int)vm->win->disp_h:(int)vm->win->disp_w;
    if(native>0) return native;
  }
  return height?216:288;
}
int presentation_size(const GmlVM *vm, const GmlRender *r, int height){
  GmlRenderPresentationMetrics metrics=builtin_presentation_metrics(r);
  int effective=r?(height?metrics.effective_height:metrics.effective_width):0;
  if(effective>0) return effective;
  int configured=r?(height?metrics.requested_height:metrics.requested_width):0;
  if(configured>0) return configured;
  return presentation_base_size(vm,r,height);
}
/* GM7/8 distinguishes the desktop display from the game window. A host core has no host
 * desktop to query, so expose the same deterministic virtual display required by the classic presentation contract.
 * Keep window_get_* tied to the presented framebuffer; modern projects also rely on that size. */
int display_size(const GmlVM *vm, const GmlRender *r, int height){
  if(vm && vm->win && anygm_policy_uses_classic_runtime(vm->win)) return height?720:1280;
  return presentation_size(vm,r,height);
}
/* GM7/8 display_mouse_* is expressed in desktop-display coordinates, while window_mouse_* and
 * the host pointer are expressed in presented-window pixels.  Keeping both spaces identical
 * made a centred absolute pointer look off-centre whenever the deterministic classic virtual
 * display differed from the game window (continuous rotation in mouse-look projects). */
double classic_display_mouse_coord(const GmlVM *vm,const GmlRender *r,double value,
                                   int height,int to_window){
  if(!vm || !vm->win || !anygm_policy_uses_classic_runtime(vm->win)) return value;
  int display=display_size(vm,r,height), window=presentation_size(vm,r,height);
  if(display<=0 || window<=0) return value;
  return to_window ? value*(double)window/(double)display
                   : value*(double)display/(double)window;
}
const char *gm_string_format(GmlVal v,char buffer[64]){
  if(v.t==V_STR) return v.s?v.s:"";
  if(v.t==V_UNDEF || v.t==V_ARR) return "";
  double d=v.t==V_REAL?v.d:0;
  if(d==floor(d)&&fabs(d)<1e15) snprintf(buffer,64,"%.0f",d);
  else snprintf(buffer,64,"%.2f",d);
  return buffer;
}
static const char *gm_string_tmp(GmlVM *vm,GmlVal v){
  GmlBuiltinState *state=builtin_state_ensure(vm);
  if(!state) return "";
  char *b=state->string_ring[state->string_ring_index++ & 7u];
  return gm_string_format(v,b);
}
const char *S(GmlVM *vm,GmlVal *a,int n,int i){
  return (i<n)?gm_string_tmp(vm,a[i]):"";
}
/* Shader handles are renderer-owned and opaque to language code. */
double gml_shader_get_uniform(GmlRender *R, int sh, const char *un){
  return gml_render_shader_uniform_handle(R,sh,un);
}
static double gml_shader_uniform_component(GmlVal *args, int count, int component){
  if(count==2 && args[1].t==V_ARR){
    GmlVal value=gml_arr_get(args[1],component);
    return N(&value,1,0);
  }
  return N(args,count,component+1);
}
void gml_shader_set_uniform_f(GmlRender *R, int h, GmlVal *a, int n){
  if(!R || h<0) return;
  double values[4];
  for(int component=0;component<4;component++)
    values[component]=gml_shader_uniform_component(a,n,component);
  gml_render_shader_uniform_set(R,h,values);
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
/* GameMaker treats a negative draw_sprite subimage, idiomatically -1, as the drawing instance's
 * current image_index. Passing -1 through literally freezes animation. */
int gml_draw_subimg(GmlVM *vm, double raw){
  if(raw < 0){ GmlInstance *s=vm->cur_self; return s? (int)s->image_index : 0; }
  return (int)raw;
}
GmlVal array4(double a, double b, double c, double d){
  GmlArr *A=calloc(1,sizeof(*A));
  if(!A) return vreal(0);
  A->data=calloc(4,sizeof(GmlVal));
  if(!A->data){ free(A); return vreal(0); }
  A->len=A->cap=4;
  A->data[0]=vreal(a); A->data[1]=vreal(b); A->data[2]=vreal(c); A->data[3]=vreal(d);
  GmlVal v=vreal(0); v.t=V_ARR; v.arr=A; return v;
}
GmlVal arr_newv(int n){
  GmlArr *A=calloc(1,sizeof(*A));
  if(!A) return vreal(0);
  A->data=calloc((size_t)(n>0?n:1),sizeof(GmlVal));
  if(!A->data){ free(A); return vreal(0); }
  A->len=A->cap=n;
  GmlVal v=vreal(0); v.t=V_ARR; v.arr=A; return v;
}
GmlVal arr8(double a0,double a1,double a2,double a3,double a4,double a5,double a6,double a7){
  GmlVal v=arr_newv(8);
  if(v.t!=V_ARR) return v;
  GmlArr *A=(GmlArr*)v.arr;
  A->data[0]=vreal(a0); A->data[1]=vreal(a1); A->data[2]=vreal(a2); A->data[3]=vreal(a3);
  A->data[4]=vreal(a4); A->data[5]=vreal(a5); A->data[6]=vreal(a6); A->data[7]=vreal(a7);
  return v;
}
int texture_info(GmlRender *R, int tex, double *uw, double *uh, double *tw, double *th){
  if(uw) *uw=0;
  if(uh) *uh=0;
  if(tw) *tw=0;
  if(th) *th=0;
  GmlRenderTextureMetrics metrics;
  if(gml_render_texture_metrics(R,tex,&metrics) &&
     (metrics.kind==GML_RENDER_TEXTURE_SPRITE ||
      metrics.kind==GML_RENDER_TEXTURE_SURFACE)){
    if((metrics.runtime || metrics.kind==GML_RENDER_TEXTURE_SURFACE) &&
       metrics.full_width>0 && metrics.full_height>0){
      if(uw) *uw=1;
      if(uh) *uh=1;
      if(tw) *tw=1.0/metrics.full_width;
      if(th) *th=1.0/metrics.full_height;
      return 1;
    }
    if(metrics.kind==GML_RENDER_TEXTURE_SPRITE &&
       metrics.full_width>0 && metrics.full_height>0){
      if(uw) *uw=(double)metrics.width/metrics.full_width;
      if(uh) *uh=(double)metrics.height/metrics.full_height;
      if(tw) *tw=1.0/metrics.full_width;
      if(th) *th=1.0/metrics.full_height;
      return 1;
    }
    return 0;
  }
  return 0;
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
/* Studio layer functions accept either a numeric layer id or a layer name. Resolve both forms. */
GmlRtLayer *rt_layer_resolve(GmlVM *vm, GmlVal *a, int n){
  if(n>0 && a[0].t==V_STR && a[0].s) return gml_rt_layer_find_by_name(vm,a[0].s);
  return gml_rt_layer_find(vm,(int)N(a,n,0));
}
/* First time GML sets a layer's x/y/hspeed/vspeed at runtime, freeze its accumulated scroll position
 * (until now the draw derived it as x0 + hspeed*elapsed from the room definition) and switch it to
 * per-step accumulation. Untouched layers keep the exact room-def model, so non-scripted games stay
 * byte-identical. */
void rt_layer_touch(GmlVM *vm, GmlRtLayer *l){
  if(!l || l->touched) return;
   long fin=vm->frame - vm->room_enter_frame; if(fin<0) fin=0;
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
GmlRtElem *rt_sprite_for_layer_name(GmlVM *vm, int layer_id, const char *name){
  if(!vm || !name) return NULL;
  for(int i=0;i<vm->n_rte;i++){
    GmlRtElem *e=&vm->rte[i];
    if(e->used && e->type==3 && e->layer==layer_id && !strcmp(e->name,name)) return e;
  }
  return NULL;
}
GmlRtElem *rt_background_for_layer(GmlVM *vm, GmlRtLayer *l, int create_from_room){
  if(!vm || !l) return NULL;
  for(int i=0;i<vm->n_rte;i++){
    GmlRtElem *e=&vm->rte[i];
    if(e->used && e->type==1 && e->layer==l->id) return e;
  }
  if(!create_from_room || !vm->win || !anygm_policy_has_modern_function_values(vm->win) || vm->room_index<0) return NULL;
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
int log_col_on(GmlVM *vm){
  GmlBuiltinState *state=builtin_state_ensure(vm);
  if(!state) return (builtin_setting(vm,"GML_LOG_COL") || builtin_setting(vm,"GML_LOG_COL_OBJ")) ? 1 : 0;
  if(state->log_collision<0){
    const char *filter=builtin_setting(vm,"GML_LOG_COL_OBJ");
    state->log_collision=(builtin_setting(vm,"GML_LOG_COL") || filter) ? 1 : 0;
    snprintf(state->collision_filter,sizeof state->collision_filter,"%s",filter?filter:"");
  }
  return state->log_collision;
}
int log_col_match(GmlVM *vm,const char *obj_name){
  if(!log_col_on(vm)) return 0;
  GmlBuiltinState *state=builtin_state_ensure(vm);
  const char *f=state?state->collision_filter:"";
  return !f || !*f || (obj_name && strstr(obj_name,f));
}
char *dup_n(const char *s, int n){
  if(n<0) n=0;
  char *o=malloc((size_t)n+1);
  if(!o) return strdup("");
  if(n) memcpy(o,s,(size_t)n);
  o[n]=0;
  return o;
}
int ds_val_equal(GmlVal a, GmlVal b){
  if(a.t==V_UNDEF || b.t==V_UNDEF) return a.t==b.t;
  /* Arrays compare by identity in GML.  Treating every array as numeric zero made
   * array searches (and DS containers holding arrays) report unrelated arrays equal. */
  if(a.t==V_ARR || b.t==V_ARR) return a.t==V_ARR && b.t==V_ARR && a.arr==b.arr;
  if(a.t==V_STR || b.t==V_STR){
    char ab[64],bb[64];
    return !strcmp(gm_string_format(a,ab),gm_string_format(b,bb));
  }
  return fabs((a.t==V_REAL?a.d:0.0)-(b.t==V_REAL?b.d:0.0))<1e-9;
}
int gml_array_search_index(GmlVal *args, int count){
  if(count<2 || args[0].t!=V_ARR || !args[0].arr) return -1;
  GmlArr *array=(GmlArr*)args[0].arr;
  if(array->len<=0) return -1;

  /* Array range functions clamp their starting offset to the available indices. Negative
   * offsets count from the end, while the sign of length selects the traversal direction. */
  double raw_offset=count>2?N(args,count,2):0.0;
  int offset=0;
  if(isnan(raw_offset)) raw_offset=0.0;
  if(isinf(raw_offset)) offset=raw_offset<0.0?0:array->len-1;
  else {
    double integral=trunc(raw_offset);
    if(integral<0.0) integral+=(double)array->len;
    if(integral<=0.0) offset=0;
    else if(integral>=(double)(array->len-1)) offset=array->len-1;
    else offset=(int)integral;
  }

  int direction=1;
  int length=array->len-offset;
  if(count>3){
    double raw_length=N(args,count,3);
    if(isnan(raw_length)) raw_length=0.0;
    direction=raw_length<0.0?-1:1;
    int available=direction>0?array->len-offset:offset+1;
    if(isinf(raw_length)) length=available;
    else {
      double integral=trunc(fabs(raw_length));
      length=integral>=(double)available?available:(int)integral;
    }
  }
  for(int visited=0,index=offset;visited<length;visited++,index+=direction)
    if(ds_val_equal(array->data[index],args[1])) return index;
  return -1;
}
typedef struct { const GmlArr *left, *right; } GmlArrayPair;
typedef struct { GmlArrayPair *pair; int len, cap; } GmlArrayEqualCtx;
static int array_value_equal(GmlVM *vm, GmlVal left, GmlVal right,
                             GmlArrayEqualCtx *ctx, int depth){
  if(left.t!=right.t) return 0;
  if(left.t==V_UNDEF) return 1;
  if(left.t==V_STR) return !strcmp(left.s?left.s:"",right.s?right.s:"");
  if(left.t==V_REAL)
    return gml_real_compare_epsilon(left.d,right.d,CMP_EQ,vm?vm->math_epsilon:1e-5);
  if(left.t!=V_ARR || !left.arr || !right.arr) return left.arr==right.arr;
  GmlArr *a=left.arr, *b=right.arr;
  if(a==b) return 1;
  if(a->len!=b->len || depth>4096) return 0;
  for(int i=0;i<ctx->len;i++)
    if(ctx->pair[i].left==a && ctx->pair[i].right==b) return 1;
  if(ctx->len>=ctx->cap){
    int next=ctx->cap?ctx->cap*2:16;
    GmlArrayPair *grown=realloc(ctx->pair,(size_t)next*sizeof(*grown));
    if(!grown) return 0;
    ctx->pair=grown; ctx->cap=next;
  }
  ctx->pair[ctx->len++]=(GmlArrayPair){a,b};
  for(int i=0;i<a->len;i++)
    if(!array_value_equal(vm,a->data[i],b->data[i],ctx,depth+1)) return 0;
  return 1;
}
int array_equals_recursive(GmlVM *vm, GmlVal left, GmlVal right){
  if(left.t!=V_ARR || right.t!=V_ARR) return 0;
  GmlArrayEqualCtx ctx={0};
  int equal=array_value_equal(vm,left,right,&ctx,0);
  free(ctx.pair);
  return equal;
}
static int gml_val_sort_rank(GmlVal v){
  if(v.t==V_REAL) return 0;
  if(v.t==V_STR) return 1;
  if(v.t==V_ARR) return 2;
  return 3;
}
int gml_val_sort_compare(GmlVal a, GmlVal b){
  int r=0;
  if(a.t==V_REAL && b.t==V_REAL){
    double ad=a.d, bd=b.d;
    if(isnan(ad) && isnan(bd)) r=0;
    else if(isnan(ad)) r=1;
    else if(isnan(bd)) r=-1;
    else r=(ad>bd)-(ad<bd);
  } else if(a.t==V_STR && b.t==V_STR){
    r=strcmp(a.s?a.s:"",b.s?b.s:"");
  } else {
    int ar=gml_val_sort_rank(a), br=gml_val_sort_rank(b);
    r=(ar>br)-(ar<br);
  }
  return r;
}
static int gml_val_sort_cmp(const void *pa, const void *pb){
  return gml_val_sort_compare(*(const GmlVal*)pa,*(const GmlVal*)pb);
}
void gml_val_sort_reverse(GmlVal *value, int count){
  for(int lo=0,hi=count-1;lo<hi;lo++,hi--){
    GmlVal swap=value[lo]; value[lo]=value[hi]; value[hi]=swap;
  }
}
void gml_array_sort(GmlVal arr, int ascending){
  if(arr.t!=V_ARR || !arr.arr) return;
  GmlArr *A=(GmlArr*)arr.arr;
  if(A->len<=1 || !A->data) return;
  qsort(A->data,(size_t)A->len,sizeof(GmlVal),gml_val_sort_cmp);
  if(!ascending) gml_val_sort_reverse(A->data,A->len);
}
GmlVal gml_array_shuffle_copy(GmlVM *vm,GmlVal source,GmlVal *args,int count){
  if(source.t!=V_ARR || !source.arr) return gml_arr_new(0,vreal(0));
  GmlArr *input=(GmlArr*)source.arr;
  int total=input->len;
  if(total<=0) return gml_arr_new(0,vreal(0));

  double offset_value=count>1?N(args,count,1):0;
  int offset;
  if(!isfinite(offset_value)) offset=offset_value<0?0:total-1;
  else {
    double floored=floor(offset_value);
    if(floored<-(double)total) offset=0;
    else if(floored>(double)(total-1)) offset=total-1;
    else { offset=(int)floored; if(offset<0) offset+=total; }
  }

  int direction=1, length=total-offset;
  if(count>2){
    double length_value=N(args,count,2);
    direction=length_value<0?-1:1;
    if(!isfinite(length_value)) length=direction>0?total-offset:offset+1;
    else {
      double magnitude=fabs(length_value);
      length=magnitude>(double)total?total:(int)floor(magnitude);
      int available=direction>0?total-offset:offset+1;
      if(length>available) length=available;
    }
  }
  if(length<0) length=0;
  GmlVal output=gml_arr_new(length,vreal(0));
  for(int i=0,index=offset;i<length;i++,index+=direction)
    gml_arr_set(output,i,input->data[index]);
  if(output.t==V_ARR && output.arr){
    GmlArr *shuffled=(GmlArr*)output.arr;
    for(int i=shuffled->len-1;i>0;i--){
      int j=(int)floor(gml_rng_value(vm)*(i+1));
      if(j<0) j=0;
      if(j>i) j=i;
      GmlVal temporary=shuffled->data[i];
      shuffled->data[i]=shuffled->data[j];
      shuffled->data[j]=temporary;
    }
  }
  return output;
}

GmlSoftware3D *graphics_state_for_vm(GmlVM *vm){
  return gml_vm_software3d_ensure(vm);
}
/* GM round(): round half to even (banker's). */
double gm_round(double x){
  double f=floor(x), diff=x-f;
  if(diff<0.5) return f;
  if(diff>0.5) return f+1;
  return (fmod(f,2.0)==0.0)? f : f+1;
}
double builtin_math_sqrt(GmlVM *vm,double value){
  double epsilon=vm?vm->math_epsilon:1e-5;
  if(value<0.0 && value>=-epsilon) value=0.0;
  return sqrt(value);
}
void builtin_math_set_epsilon(GmlVM *vm,double epsilon){
  if(vm && isfinite(epsilon) && epsilon>=0.0 && epsilon<1.0) vm->math_epsilon=epsilon;
}
double gm_sign(double x){ return x>0?1:(x<0?-1:0); }
void motion_from_components(GmlInstance *in){
  in->speed=hypot(in->hspeed,in->vspeed);
  in->direction=atan2(-in->vspeed,in->hspeed)*180.0/M_PI;
  if(in->direction<0) in->direction+=360;
}
void motion_from_speed_direction(GmlVM *vm,GmlInstance *in){
  if(vm && vm->win && anygm_policy_uses_classic_runtime(vm->win)){
    in->direction=fmod(in->direction,360.0);
    if(in->direction<0) in->direction+=360.0;
  }
  in->hspeed=in->speed*cos(in->direction*M_PI/180.0);
  in->vspeed=-in->speed*sin(in->direction*M_PI/180.0);
  if(vm && vm->win && anygm_policy_uses_classic_runtime(vm->win)){
    double rounded=round(in->hspeed);
    if(fabs(rounded-in->hspeed)<0.0001) in->hspeed=rounded;
    rounded=round(in->vspeed);
    if(fabs(rounded-in->vspeed)<0.0001) in->vspeed=rounded;
  }
}

#define GML_GP_AXIS_LH 32785
#define GML_GP_AXIS_LV 32786
#define GML_GP_AXIS_RH 32787
#define GML_GP_AXIS_RV 32788
static int gp_axis_const(int ax){
  switch(ax){
    case 0: case GML_GP_AXIS_LH: return GML_GP_AXIS_LH;
    case 1: case GML_GP_AXIS_LV: return GML_GP_AXIS_LV;
    case 2: case GML_GP_AXIS_RH: return GML_GP_AXIS_RH;
    case 3: case GML_GP_AXIS_RV: return GML_GP_AXIS_RV;
    default: return 0;
  }
}
static double gp_deadzone_get(GmlVM *vm, int dev){
  GmlBuiltinState *state=builtin_state_ensure(vm);
  if(state && dev>=0 && dev<GML_GP_MAX_DEV && state->gamepad_deadzone_set[dev])
    return state->gamepad_deadzone[dev];
  return 0.2;  /* GameMaker's standard default deadzone. */
}
void gp_deadzone_set(GmlVM *vm, int dev, double dz){
  GmlBuiltinState *state=builtin_state_ensure(vm);
  if(!state) return;
  if(dev < 0 || dev >= GML_GP_MAX_DEV) return;
  if(!isfinite(dz)) dz = 0.0;
  if(dz < 0.0) dz = 0.0;
  if(dz > 1.0) dz = 1.0;
  state->gamepad_deadzone[dev] = dz;
  state->gamepad_deadzone_set[dev] = 1;
}
void gml_gamepad_set_axis_deadzone_direct(GmlVM *vm, int device, double dz){
  gp_deadzone_set(vm,device,dz);
}
static double gp_axis_digital_fallback(GmlVM *vm,int ax){
  if(ax == GML_GP_AXIS_LH) return gml_input_gamepad(vm,32784,0) - gml_input_gamepad(vm,32783,0);  /* R - L */
  if(ax == GML_GP_AXIS_LV) return gml_input_gamepad(vm,32782,0) - gml_input_gamepad(vm,32781,0);  /* D - U */
  return 0.0;
}
double gp_axis_value_filtered(GmlVM *vm, int dev, int ax){
  ax = gp_axis_const(ax);
  if(!ax) return 0.0;
  double v = gml_input_gamepad_axis(vm,dev, ax);
  if(v == 0.0)
    v = gp_axis_digital_fallback(vm,ax);   /* Preserve the existing digital transport. */
  if(!isfinite(v)) v = 0.0;
  if(v < -1.0) v = -1.0;
  if(v > 1.0) v = 1.0;
  double dz = gp_deadzone_get(vm,dev);
  return fabs(v) < dz ? 0.0 : v;
}
/* GM mouse button constant -> state bitmask: mb_left=1,mb_right=2,mb_middle=3,mb_any=-1,mb_none=0 */
static int mb_mask(int mb){
  switch(mb){ case 1: return 1; case 2: return 2; case 3: return 4; case -1: return 7; default: return 0; }
}
int mouse_btn_check(GmlVM *vm,int mb, int edge){
  int held, pressed, released;
  gml_input_mouse(vm,NULL,NULL,NULL,NULL,NULL,NULL,&held,&pressed,&released,NULL);
  int m = mb_mask(mb);
  int v = edge==1 ? pressed : edge==2 ? released : held;
  int r = (mb==0) ? !(v&7) : ((v & m) != 0);
  return r;
}

/* current room's ROOM index -> play-order position */
int order_pos(GmlVM *vm, int room_idx){
  for(int i=0;i<vm->win->n_room_order;i++) if((int)vm->win->room_order[i]==room_idx) return i;
  return -1;
}
int script_code_of(GmlVM *vm, int sid){
  const GmlChunk *c=gml_chunk(vm->win,"SCPT"); if(!c) return -1;
  const uint8_t *d=vm->win->data; uint32_t n=u32(d,c->off);
  if(sid<0||(uint32_t)sid>=n) return -1;
  uint32_t p=u32(d,c->off+4+sid*4);
  return (int)u32(d,p+4);  /* codeId */
}

int script_ref_code_of(GmlVM *vm, GmlVal v){
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

GmlVal gml_array_create_ext(GmlVM *vm, GmlVal *args, int count){
  int size=count>0?(int)N(args,count,0):0;
  GmlVal result=gml_arr_new(size,vreal(0));
  if(count<2 || result.t!=V_ARR || !result.arr) return result;
  int length=gml_val_array_length(result);
  for(int index=0;index<length;index++){
    GmlVal callback_arg=vreal(index);
    GmlVal value=gml_vm_call_callable(vm,args[1],&callback_arg,1);
    gml_arr_set(result,index,value);
    if(value.t==V_STR && value.s && value.d!=0) free((void*)value.s);
  }
  return result;
}

/* Mutating array-map operation.  Offset is clamped to an existing
 * element, negative offsets count from the end, and the sign of length selects the
 * traversal direction.  The callback receives (element, zero-based index); its result
 * replaces that element without resizing the array. */
GmlVal gml_array_map_ext(GmlVM *vm, GmlVal *args, int count){
  if(count<2 || args[0].t!=V_ARR || !args[0].arr) return vreal(0);
  GmlArr *array=(GmlArr*)args[0].arr;
  int array_length=array->len;
  if(array_length<=0) return vreal(0);

  double raw_offset=count>2?N(args,count,2):0.0;
  if(isnan(raw_offset)) raw_offset=0.0;
  int offset;
  if(isinf(raw_offset)) offset=raw_offset<0.0?0:array_length-1;
  else {
    double integral=trunc(raw_offset);
    if(integral<0.0) integral+=(double)array_length;
    if(integral<=0.0) offset=0;
    else if(integral>=(double)(array_length-1)) offset=array_length-1;
    else offset=(int)integral;
  }

  int direction=1;
  int length=array_length-offset;
  if(count>3){
    double raw_length=N(args,count,3);
    if(isnan(raw_length)) raw_length=0.0;
    direction=raw_length<0.0?-1:1;
    int available=direction>0?array_length-offset:offset+1;
    if(isinf(raw_length)) length=available;
    else {
      double integral=trunc(fabs(raw_length));
      length=integral>=(double)available?available:(int)integral;
    }
  }

  for(int visited=0,index=offset;visited<length;visited++,index+=direction){
    GmlVal element=(index>=0 && index<array->len)?array->data[index]:vundef();
    if(element.t==V_STR) element.d=0;
    GmlVal callback_args[2]={element,vreal(index)};
    GmlVal mapped=gml_vm_call_callable(vm,args[1],callback_args,2);
    gml_arr_set(args[0],index,mapped);
    if(mapped.t==V_STR && mapped.s && mapped.d!=0) free((void*)mapped.s);
  }
  return vreal(length);
}

GmlVal gml_array_foreach(GmlVM *vm, GmlVal *args, int count){
  if(count<2 || args[0].t!=V_ARR || !args[0].arr) return vundef();
  GmlArr *array=(GmlArr*)args[0].arr;
  int array_length=array->len;
  if(array_length<=0) return vundef();

  double raw_offset=count>2?N(args,count,2):0.0;
  if(isnan(raw_offset)) raw_offset=0.0;
  int offset;
  if(isinf(raw_offset)) offset=raw_offset<0.0?0:array_length-1;
  else {
    double integral=trunc(raw_offset);
    if(integral<0.0) integral+=(double)array_length;
    if(integral<=0.0) offset=0;
    else if(integral>=(double)(array_length-1)) offset=array_length-1;
    else offset=(int)integral;
  }

  int direction=1;
  int length=array_length-offset;
  if(count>3){
    double raw_length=N(args,count,3);
    if(isnan(raw_length)) raw_length=0.0;
    direction=raw_length<0.0?-1:1;
    int available=direction>0?array_length-offset:offset+1;
    if(isinf(raw_length)) length=available;
    else {
      double integral=trunc(fabs(raw_length));
      length=integral>=(double)available?available:(int)integral;
    }
  }

  for(int visited=0,index=offset;visited<length;visited++,index+=direction){
    GmlVal element=(index>=0 && index<array->len)?array->data[index]:vundef();
    if(element.t==V_STR) element.d=0;
    GmlVal callback_args[2]={element,vreal(index)};
    GmlVal result=gml_vm_call_callable(vm,args[1],callback_args,2);
    if(result.t==V_STR && result.s && result.d!=0) free((void*)result.s);
  }
  return vundef();
}

GmlTimeSource *time_source_find(GmlVM *vm, int id){
  if(!vm || id<(int)GML_TIME_SOURCE_ID_BASE) return NULL;
  for(int i=0;i<GML_TIME_SOURCE_MAX;i++)
    if(vm->builtins->time_source[i].live && (int)vm->builtins->time_source[i].id==id) return &vm->builtins->time_source[i];
  return NULL;
}

static int time_source_ancestor_active(GmlVM *vm, GmlTimeSource *source,
                                       int *root, int depth){
  if(!source || depth>GML_TIME_SOURCE_MAX) return 0;
  if(source->parent==0){ if(root) *root=0; return 1; }
  if(source->parent==1){ if(root) *root=1; return vm->builtins->time_source_game_state==1; }
  GmlTimeSource *parent=time_source_find(vm,source->parent);
  return parent && parent->state==1 &&
    time_source_ancestor_active(vm,parent,root,depth+1);
}

static int compare_time_source_id(const void *left, const void *right){
  uint32_t a=*(const uint32_t*)left, b=*(const uint32_t*)right;
  return (a>b)-(a<b);
}

void gml_time_sources_tick(GmlVM *vm){
  if(!vm) return;
  uint32_t ids[GML_TIME_SOURCE_MAX];
  unsigned char roots[GML_TIME_SOURCE_MAX];
  int count=0;
  /* Snapshot eligibility before any callback can stop a parent or create a new child. */
  for(int i=0;i<GML_TIME_SOURCE_MAX;i++){
    GmlTimeSource *source=&vm->builtins->time_source[i];
    int root=0;
    if(source->live && source->state==1 && time_source_ancestor_active(vm,source,&root,0)){
      ids[count]=source->id; roots[count]=(unsigned char)root; count++;
    }
  }
  /* Process the global tree first, then the game tree. IDs preserve creation order
   * even when a previously freed pool slot is reused. */
  for(int root=0;root<=1;root++){
    uint32_t ordered[GML_TIME_SOURCE_MAX]; int ordered_count=0;
    for(int i=0;i<count;i++) if(roots[i]==root) ordered[ordered_count++]=ids[i];
    qsort(ordered,(size_t)ordered_count,sizeof(*ordered),compare_time_source_id);
    for(int i=0;i<ordered_count;i++){
      GmlTimeSource *source=time_source_find(vm,(int)ordered[i]);
      if(!source || source->state!=1) continue;
      double step=source->units==1?1.0:1.0/gml_room_speed(vm);
      source->remaining-=step;
      int expired;
      if(source->units==1) expired=source->remaining<=0.0;
      else if(source->expiry_type==0) expired=source->remaining<=step*0.5;
      else expired=source->remaining<0.0;
      if(!expired) continue;

      source->reps_completed++;
      if(source->reps_remaining>0) source->reps_remaining--;
      int repeats=source->reps_remaining<0 || source->reps_remaining>0;
      if(repeats){
        source->remaining+=source->period;
        if(source->remaining<=0.0) source->remaining=source->period;
      } else {
        source->remaining=0.0;
        source->state=3;
      }

      GmlVal callback=source->callback;
      GmlVal callback_args[16]; int callback_count=0;
      if(source->args.t==V_ARR && source->args.arr){
        int available=gml_val_array_length(source->args);
        callback_count=available<16?available:16;
        for(int argument=0;argument<callback_count;argument++)
          callback_args[argument]=gml_arr_get(source->args,argument);
      }
      GmlVal result=gml_vm_call_callable(vm,callback,callback_args,callback_count);
      if(result.t==V_STR && result.s && result.d!=0) free((void*)result.s);
    }
  }
}
int gml_colgrid_mode(GmlVM *vm){
  GmlBuiltinState *state=builtin_state_ensure(vm);
  if(!state) return builtin_setting(vm,"GML_NO_COLGRID")?0:(builtin_setting(vm,"GML_DBG_GRIDCHECK")?2:1);
  if(state->collision_grid_mode<0)
    state->collision_grid_mode=builtin_setting(vm,"GML_NO_COLGRID")?0:(builtin_setting(vm,"GML_DBG_GRIDCHECK")?2:1);
  return state->collision_grid_mode;
}

/* Fire GM's "gamepad discovered" async event (ev_async_system / Other_75) so games that only enable
 * gamepad input once a pad is detected recognize our single virtual pad. GM delivers it via the
 * async_load ds_map; we build {event_type:"gamepad discovered", pad_index:0}, point the async_load
 * global at it, dispatch to every live instance, then tear it down. Called on room enter (the handler
 * object may only exist in some rooms). Skipped entirely if no code defines a _Other_75 handler. */
void gml_fire_gamepad_connected(GmlVM *vm){
  if(!vm || !vm->win) return;
  if(!gml_input_gamepad_connected(vm,0)) return;
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
  ds_map_destroy_id(vm,id);
  if(builtin_setting(vm,"GML_LOG_INST")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gamepad] fired Other_75 to %d instances; joyid=%g gamepadOn=%g\n",
    n0, gml_global_num(vm,"joyid"), gml_global_num(vm,"gamepadOn"));
}

/* Fire GM's Async Save/Load event (Other_72) for every buffer_*_async request queued this step.
 * async_load = {id, status, error:0}, dispatched to all live instances, then torn down. */
void gml_fire_async_saveload(GmlVM *vm){
  if(!vm || vm->builtins->n_async_sl<=0) return;
  int n=vm->builtins->n_async_sl; vm->builtins->n_async_sl=0;
  if(builtin_setting(vm,"GML_LOG_ASYNC")){
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[async] f%ld drain n=%d\n",vm->frame,n); }
  for(int k=0;k<n;k++){
    int id=ds_map_create_id(vm); if(id<0) return;
    ds_map_put(vm,id,vstr("id"),vreal(vm->builtins->async_sl_q[k]),1);
    ds_map_put(vm,id,vstr("status"),vreal(vm->builtins->async_sl_status[k]),1);
    ds_map_put(vm,id,vstr("error"),vreal(0),1);
    *gml_varmap_put(&vm->globals,"async_load")=vreal(id);
    int n0=vm->inst_count;
    for(int i=0;i<n0;i++) if(vm->inst[i].active && !vm->inst[i].marked)
      gml_run_event(vm,&vm->inst[i],"Other_72");
    *gml_varmap_put(&vm->globals,"async_load")=vreal(-1);
    ds_map_destroy_id(vm,id);
  }
}

/* Fire GM's Async HTTP event (Other_62) for every http_* request made this step. No network in
 * the core: every response is a failure ({id, status:-1, http_status:0, result:""}), so games
 * with online scoreboards fall into their error path instead of stalling on "downloading". */
void gml_fire_async_http(GmlVM *vm){
  if(!vm || vm->builtins->n_async_http<=0) return;
  int n=vm->builtins->n_async_http; vm->builtins->n_async_http=0;
  if(builtin_setting(vm,"GML_LOG_ASYNC")){
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[async] f%ld http drain n=%d\n",vm->frame,n); }
  for(int k=0;k<n;k++){
    int id=ds_map_create_id(vm); if(id<0) return;
    ds_map_put(vm,id,vstr("id"),vreal(vm->builtins->async_http_q[k]),1);
    ds_map_put(vm,id,vstr("status"),vreal(-1),1);
    ds_map_put(vm,id,vstr("http_status"),vreal(0),1);
    ds_map_put(vm,id,vstr("result"),vstr(""),1);
    *gml_varmap_put(&vm->globals,"async_load")=vreal(id);
    int n0=vm->inst_count;
    for(int i=0;i<n0;i++) if(vm->inst[i].active && !vm->inst[i].marked)
      gml_run_event(vm,&vm->inst[i],"Other_62");
    *gml_varmap_put(&vm->globals,"async_load")=vreal(-1);
    ds_map_destroy_id(vm,id);
  }
}
GmlVal builtin_http_request_stub(GmlVM *vm){
  int req=++vm->builtins->async_seq;
  if(vm->builtins->n_async_http<16) vm->builtins->async_http_q[vm->builtins->n_async_http++]=req;
  if(builtin_setting(vm,"GML_LOG_ASYNC")){
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[async] f%ld http queue id=%d (offline: will fail)\n",vm->frame,req); }
  return vreal(req);
}

static int hotprof_enabled(void){
  return 0;
}
static double hotprof_now(void){
  return 0.0;
}
static void hotprof_add(const char *name, double ms){
  (void)name; (void)ms;
}

double draw_gui_x(GmlRender *render,double value){
  gml_render_gui_map_point(render,&value,NULL);
  return value;
}
double draw_gui_y(GmlRender *render,double value){
  gml_render_gui_map_point(render,NULL,&value);
  return value;
}
double draw_gui_w(GmlRender *render,double value){
  gml_render_gui_map_scale(render,&value,NULL);
  return value;
}
double draw_gui_h(GmlRender *render,double value){
  gml_render_gui_map_scale(render,NULL,&value);
  return value;
}
static int d3_try_draw_2d_builtin(GmlVM *vm,const char *name,GmlVal *args,int count){
  GmlRender *R=vm?(GmlRender*)vm->render:NULL;
  GmlSoftware3DStatus status={0};
  if(!R || !name || strncmp(name,"draw_",5) ||
     !gml_software3d_status_get(GML_GRAPHICS,&status) || !status.active)
    return 0;
  GmlRenderDrawState draw=builtin_draw_state(R);
  double alpha=draw.alpha; gml_render_maybe_prepare_draw(R);
  if(!strcmp(name,"draw_point")||!strcmp(name,"draw_point_color")||!strcmp(name,"draw_point_colour")){
    uint32_t color=!strcmp(name,"draw_point")?draw.color:(uint32_t)N(args,count,2);
    gml_software3d_draw_point_2d(
      R,draw_gui_x(R,N(args,count,0)),draw_gui_y(R,N(args,count,1)),
      color,alpha);
    return 1;
  }
  if(!strcmp(name,"draw_line")||!strcmp(name,"draw_line_color")||!strcmp(name,"draw_line_colour")||
     !strcmp(name,"draw_line_width")||!strcmp(name,"draw_line_width_color")||!strcmp(name,"draw_line_width_colour")){
    int colored=strstr(name,"color")||strstr(name,"colour"),wide=strstr(name,"width")!=NULL;
    int color_index=wide?5:4; uint32_t c1=colored?(uint32_t)N(args,count,color_index):draw.color;
    uint32_t c2=colored?(uint32_t)N(args,count,color_index+1):c1;
    gml_software3d_draw_line_2d(
      R,draw_gui_x(R,N(args,count,0)),draw_gui_y(R,N(args,count,1)),
      draw_gui_x(R,N(args,count,2)),draw_gui_y(R,N(args,count,3)),c1,c2,alpha,
      wide?draw_gui_w(R,N(args,count,4)):1); return 1;
  }
  if(!strcmp(name,"draw_rectangle")||!strcmp(name,"draw_rectangle_color")||!strcmp(name,"draw_rectangle_colour")){
    int plain=!strcmp(name,"draw_rectangle"); uint32_t colors[4];
    for(int i=0;i<4;i++) colors[i]=plain?draw.color:(uint32_t)N(args,count,4+i);
    gml_software3d_draw_rectangle_2d(
      R,draw_gui_x(R,N(args,count,0)),draw_gui_y(R,N(args,count,1)),
      draw_gui_x(R,N(args,count,2)),draw_gui_y(R,N(args,count,3)),colors,alpha,
      (int)N(args,count,plain?4:8)); return 1;
  }
  if(!strcmp(name,"draw_triangle")||!strcmp(name,"draw_triangle_color")||!strcmp(name,"draw_triangle_colour")){
    int plain=!strcmp(name,"draw_triangle"); uint32_t c1=plain?draw.color:(uint32_t)N(args,count,6);
    uint32_t c2=plain?c1:(uint32_t)N(args,count,7),c3=plain?c1:(uint32_t)N(args,count,8);
    double points[3][2]={
      {draw_gui_x(R,N(args,count,0)),draw_gui_y(R,N(args,count,1))},
      {draw_gui_x(R,N(args,count,2)),draw_gui_y(R,N(args,count,3))},
      {draw_gui_x(R,N(args,count,4)),draw_gui_y(R,N(args,count,5))}
    };
    uint32_t colors[3]={c1,c2,c3};
    gml_software3d_draw_triangle_2d(
      R,points,colors,alpha,(int)N(args,count,plain?6:9));
    return 1;
  }
  if(!strcmp(name,"draw_circle")||!strcmp(name,"draw_circle_color")||!strcmp(name,"draw_circle_colour")){
    int plain=!strcmp(name,"draw_circle"); uint32_t inner=plain?draw.color:(uint32_t)N(args,count,3);
    uint32_t outer=plain?inner:(uint32_t)N(args,count,4);
    gml_software3d_draw_ellipse_2d(
      R,draw_gui_x(R,N(args,count,0)),draw_gui_y(R,N(args,count,1)),
      fabs(draw_gui_w(R,N(args,count,2))),fabs(draw_gui_h(R,N(args,count,2))),
      inner,outer,alpha,(int)N(args,count,plain?3:5)); return 1;
  }
  if(!strcmp(name,"draw_ellipse")||!strcmp(name,"draw_ellipse_color")||!strcmp(name,"draw_ellipse_colour")){
    int plain=!strcmp(name,"draw_ellipse");
    double x1=draw_gui_x(R,N(args,count,0)),y1=draw_gui_y(R,N(args,count,1));
    double x2=draw_gui_x(R,N(args,count,2)),y2=draw_gui_y(R,N(args,count,3));
    uint32_t inner=plain?draw.color:(uint32_t)N(args,count,4),outer=plain?inner:(uint32_t)N(args,count,5);
    gml_software3d_draw_ellipse_2d(
      R,(x1+x2)*.5,(y1+y2)*.5,fabs(x2-x1)*.5,fabs(y2-y1)*.5,
      inner,outer,alpha,(int)N(args,count,plain?4:6));
    return 1;
  }
  if(!strcmp(name,"draw_roundrect")||!strcmp(name,"draw_roundrect_color")||!strcmp(name,"draw_roundrect_colour")||
     !strcmp(name,"draw_roundrect_color_ext")||!strcmp(name,"draw_roundrect_colour_ext")){
    int plain=!strcmp(name,"draw_roundrect"),extended=strstr(name,"_ext")!=NULL;
    uint32_t c1=plain?draw.color:(uint32_t)N(args,count,extended?6:4);
    uint32_t c2=plain?c1:(uint32_t)N(args,count,extended?7:5),colors[4]={c1,c1,c2,c2};
    gml_software3d_draw_rectangle_2d(
      R,draw_gui_x(R,N(args,count,0)),draw_gui_y(R,N(args,count,1)),
                    draw_gui_x(R,N(args,count,2)),draw_gui_y(R,N(args,count,3)),colors,alpha,
                    (int)N(args,count,plain?4:(extended?8:6))); return 1;
  }
  if(!strcmp(name,"draw_healthbar")){
    double x1=draw_gui_x(R,N(args,count,0)),y1=draw_gui_y(R,N(args,count,1));
    double x2=draw_gui_x(R,N(args,count,2)),y2=draw_gui_y(R,N(args,count,3));
    double amount=N(args,count,4); if(amount<0) amount=0; if(amount>100) amount=100;
    uint32_t back=(uint32_t)N(args,count,5),fill=(uint32_t)N(args,count,7),solid[4];
    if((int)N(args,count,9)){
      for(int i=0;i<4;i++) solid[i]=back;
      gml_software3d_draw_rectangle_2d(R,x1,y1,x2,y2,solid,alpha,0);
    }
    double fx1=x1,fy1=y1,fx2=x2,fy2=y2; int direction=(int)N(args,count,8);
    if(direction==1) fx1=x2-(x2-x1)*amount/100.0;
    else if(direction==2) fy2=y1+(y2-y1)*amount/100.0;
    else if(direction==3) fy1=y2-(y2-y1)*amount/100.0;
    else fx2=x1+(x2-x1)*amount/100.0;
    for(int i=0;i<4;i++) solid[i]=fill;
    gml_software3d_draw_rectangle_2d(R,fx1,fy1,fx2,fy2,solid,alpha,0);
    if((int)N(args,count,10)){
      for(int i=0;i<4;i++) solid[i]=0;
      gml_software3d_draw_rectangle_2d(R,x1,y1,x2,y2,solid,alpha,1);
    }
    return 1;
  }
  if(!strcmp(name,"draw_path")){
    int path=(int)N(args,count,0),absolute=(int)N(args,count,3);
    if(path>=0&&path<vm->n_paths&&vm->paths[path].n>1&&vm->paths[path].pts){
      GmlPath *p=&vm->paths[path]; double ox=absolute?0:N(args,count,1),oy=absolute?0:N(args,count,2);
      for(int i=1;i<p->n;i++) gml_software3d_draw_line_2d(R,
        draw_gui_x(R,p->pts[i-1].x+ox),draw_gui_y(R,p->pts[i-1].y+oy),
        draw_gui_x(R,p->pts[i].x+ox),draw_gui_y(R,p->pts[i].y+oy),
        draw.color,draw.color,alpha,1);
      if(p->closed) gml_software3d_draw_line_2d(R,
        draw_gui_x(R,p->pts[p->n-1].x+ox),draw_gui_y(R,p->pts[p->n-1].y+oy),
        draw_gui_x(R,p->pts[0].x+ox),draw_gui_y(R,p->pts[0].y+oy),
        draw.color,draw.color,alpha,1);
    }
    return 1;
  }
  return 0;
}

/* Portable contract for the legacy shadow helpers distributed as GML extension
 * functions. Their geometry uses the unscaled metrics exposed by
 * sprite_width and sprite_height. */
void draw_legacy_sprite_shadow(GmlVM *vm,double vertical_extra,double alpha){
  GmlRender *R=vm?(GmlRender*)vm->render:NULL;
  GmlInstance *self=vm?vm->cur_self:NULL;
  if(!R) return;
  if(alpha<0) alpha=0;
  if(alpha>1) alpha=1;
  builtin_set_draw_alpha(R,alpha);
  if(self){
    double width=0,height=0;
    int sprite=(int)self->sprite_index;
    GmlRenderSpriteMetrics sprite_metrics;
    if(gml_render_sprite_metrics(R,sprite,&sprite_metrics)){
      width=sprite_metrics.width;
      height=sprite_metrics.height;
    }
    double x1=self->x-width*.5,y1=self->y;
    double x2=self->x+width*.5,y2=self->y+height*.5+vertical_extra;
    gml_render_maybe_prepare_draw(R);
    GmlSoftware3DStatus status={0};
    if(gml_software3d_status_get(GML_GRAPHICS,&status) && status.active){
      gml_software3d_draw_ellipse_2d(
        R,(x1+x2)*.5,(y1+y2)*.5,fabs(x2-x1)*.5,
        fabs(y2-y1)*.5,0x404040u,0x404040u,alpha,0);
    } else {
      GmlRenderTargetMetrics target=builtin_target_metrics(R);
      int left=(int)floor(x1-target.camera_x),top=(int)floor(y1-target.camera_y);
      int right=(int)floor(x2-target.camera_x),bottom=(int)floor(y2-target.camera_y);
      gml_render_primitive_circle(R,(left+right)/2,(top+bottom)/2,
                       abs(right-left)/2,abs(bottom-top)/2,0x404040u,0);
    }
  }
  /* These helpers intentionally leave the global draw alpha at one. */
  builtin_set_draw_alpha(R,1.0);
}

/* Four-direction movement contract used by classic GML extension packages.  The
 * public function declares five arguments; the optional sixth selector is zero
 * when omitted, which selects the cursor keys. */
void legacy_move_rpg(GmlVM *vm,GmlVal *args,int count){
  GmlInstance *self=vm?vm->cur_self:NULL;
  if(!self || (count!=5 && count!=6)) return;
  double selector=count==6?N(args,count,5):0;
  if(selector!=0 && selector!=1) return;
  int left=selector==0?37:'A';
  int up=selector==0?38:'W';
  int right=selector==0?39:'D';
  int down=selector==0?40:'S';
  double pace=N(args,count,3),animation=N(args,count,4);
  int motion_changed=0;
  if(gml_keyboard_check(vm,left,0) && self->vspeed==0){
    self->hspeed=-pace; self->sprite_index=N(args,count,1);
    self->image_xscale=-1; self->image_speed=animation; motion_changed=1;
  }
  if(gml_keyboard_check(vm,right,0) && self->vspeed==0){
    self->hspeed=pace; self->sprite_index=N(args,count,1);
    self->image_xscale=1; self->image_speed=animation; motion_changed=1;
  }
  if(gml_keyboard_check(vm,up,0) && self->hspeed==0){
    self->vspeed=-pace; self->sprite_index=N(args,count,0);
    self->image_speed=animation; motion_changed=1;
  }
  if(gml_keyboard_check(vm,down,0) && self->hspeed==0){
    self->vspeed=pace; self->sprite_index=N(args,count,2);
    self->image_speed=animation; motion_changed=1;
  }
  if(gml_keyboard_check(vm,left,2) || gml_keyboard_check(vm,right,2)){
    self->image_speed=0; self->image_index=0; self->hspeed=0; motion_changed=1;
  }
  if(gml_keyboard_check(vm,up,2) || gml_keyboard_check(vm,down,2)){
    self->image_speed=0; self->image_index=0; self->vspeed=0; motion_changed=1;
  }
  if(motion_changed) motion_from_components(self);
}

/* Additional contracts exported by common classic GML extension packages.  Keep these as
 * behavior-level helpers: classic projects frequently retain the public calls while omitting the
 * package source that originally routed them to ordinary project scripts. */
void legacy_direction_rpg(GmlVM *vm,GmlVal *args,int count){
  GmlInstance *self=vm?vm->cur_self:NULL;
  if(!self || count!=4) return;
  double direction=fmod(self->direction,360.0);
  if(direction<0) direction+=360.0;
  if(direction<=45.0 || direction>315.0){
    self->sprite_index=N(args,count,0);
    self->image_xscale=1;
  } else if(direction<=135.0){
    self->sprite_index=N(args,count,1);
  } else if(direction<=225.0){
    self->sprite_index=N(args,count,0);
    self->image_xscale=-1;
  } else {
    self->sprite_index=N(args,count,2);
  }
  self->image_speed=N(args,count,3);
}

GmlVal builtin_call_impl(GmlVM *vm,const char *nm,GmlVal *args,int count);
void legacy_friction_platform(GmlVM *vm,GmlVal *args,int count){
  (void)args;
  GmlInstance *self=vm?vm->cur_self:NULL;
  if(!self || count!=0) return;
  GmlVal contact[2]={vreal(self->direction),vreal(12)};
  (void)builtin_call_impl(vm,"move_contact_solid",contact,2);
  self->vspeed=0;
}

void legacy_destroy_self(GmlVM *vm,int count){
  if(vm && vm->cur_self && count==0) gml_instance_destroy(vm,vm->cur_self);
}

int gp_debug_on(GmlVM *vm){
  GmlBuiltinState *state=builtin_state_ensure(vm);
  if(!state) return builtin_setting(vm,"GML_DBG_GP")!=NULL;
  if(state->gamepad_debug<0) state->gamepad_debug=builtin_setting(vm,"GML_DBG_GP")!=NULL;
  return state->gamepad_debug;
}

/* keyboard/gamepad dispatch shared by the generic chain and the cached fast path — the
 * bodies are the chain handlers verbatim (input polls run hundreds of times per frame in
 * GMS2 input wrappers, and each one otherwise walks most of the name chain). */
int builtin_input_kbgp(GmlVM *vm, const char *nm, GmlVal *a, int n, GmlVal *out){
  if(!strcmp(nm,"keyboard_check")){          *out=vreal(gml_keyboard_check(vm,(int)N(a,n,0),0)); return 1; }
  if(!strcmp(nm,"keyboard_check_pressed")){  *out=vreal(gml_keyboard_check(vm,(int)N(a,n,0),1)); return 1; }
  if(!strcmp(nm,"keyboard_check_released")){ *out=vreal(gml_keyboard_check(vm,(int)N(a,n,0),2)); return 1; }
  if(!strcmp(nm,"keyboard_check_direct")){   *out=vreal(gml_input_key(vm,(int)N(a,n,0),0)); return 1; }
  if(!strcmp(nm,"keyboard_clear")){ gml_keyboard_clear(vm,(int)N(a,n,0)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"keyboard_key_press")){ gml_input_key_press(vm,(int)N(a,n,0)); *out=vreal(0); return 1; }
  if(!strcmp(nm,"keyboard_key_release")){ gml_input_key_release(vm,(int)N(a,n,0)); *out=vreal(0); return 1; }
  /* gamepad button/axis reads expose the single RetroPad slot; connection/discovery is separate. */
  {
    if(gp_debug_on(vm) && !strncmp(nm,"gamepad_button",14)){

      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gp] f%ld %s n=%d a0=%.0f a1=%.0f -> %d\n",vm->frame,nm,n,N(a,n,0),N(a,n,1),
        gml_input_gamepad(vm,(int)N(a,n,1),0)); } }
  if(!strcmp(nm,"gamepad_button_check")){          *out=vreal(gml_input_gamepad(vm,(int)N(a,n,1),0)); return 1; }
  if(!strcmp(nm,"gamepad_button_value")){          *out=vreal(gml_input_gamepad(vm,(int)N(a,n,1),0) ? 1.0 : 0.0); return 1; }
  if(!strcmp(nm,"gamepad_button_check_pressed")){  *out=vreal(gml_input_gamepad(vm,(int)N(a,n,1),1)); return 1; }
  if(!strcmp(nm,"gamepad_button_check_released")){ *out=vreal(gml_input_gamepad(vm,(int)N(a,n,1),2)); return 1; }
  if(!strcmp(nm,"gamepad_is_connected")){          *out=vreal(gml_input_gamepad_connected(vm,(int)N(a,n,0))); return 1; }
  if(!strcmp(nm,"gamepad_is_supported")){          *out=vreal(1); return 1; }
  if(!strcmp(nm,"gamepad_get_device_count")){      *out=vreal(gml_input_gamepad_device_count(vm)); return 1; }
  if(!strcmp(nm,"gamepad_button_count")){          *out=vreal(16); return 1; }
  if(!strcmp(nm,"gamepad_axis_count")){            *out=vreal(4); return 1; }
  if(!strcmp(nm,"gamepad_get_description")){       *out=vstr(gml_input_gamepad_connected(vm,(int)N(a,n,0)) ? "AnyGM Gamepad" : ""); return 1; }
  if(!strcmp(nm,"gamepad_set_axis_deadzone")){
    gp_deadzone_set(vm,(int)N(a,n,0),N(a,n,1)); *out=vreal(0); return 1;
  }
  if(!strcmp(nm,"gamepad_set_button_threshold")){ *out=vreal(0); return 1; }
  if(!strcmp(nm,"gamepad_set_vibration")){
    gml_input_gamepad_set_vibration(vm,(int)N(a,n,0), N(a,n,1), N(a,n,2));
    *out=vreal(0); return 1;
  }
  if(!strcmp(nm,"gamepad_axis_value")){
    *out=vreal(gp_axis_value_filtered(vm,(int)N(a,n,0),(int)N(a,n,1))); return 1; }
  if(!strncmp(nm,"gamepad_",8)){ *out=vreal(0); return 1; }
  return 0;
}

GmlVal builtin_fmod(GmlVM *vm, const char *nm, GmlVal *a, int n);
GmlVal builtin_call_impl(GmlVM *vm, const char *nm, GmlVal *a, int n);

/* full FMOD extension dispatch, shared by the generic chain and the cached fast path
 * so extension calls use the same ordered dispatch in either path. */
GmlVal builtin_fmod(GmlVM *vm, const char *nm, GmlVal *a, int n){
      GmlBuiltinState *state=builtin_state_ensure(vm);
      if(!state) return vreal(0);
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
        if(!strcmp(f,"event_instance_set_parameter")){ gml_fmod_set_param(fb,(int)N(a,n,0),S(vm,a,n,1),N(a,n,2)); return vreal(0); }
        if(!strcmp(f,"event_instance_get_parameter")) return vreal(gml_fmod_get_param(fb,(int)N(a,n,0),S(vm,a,n,1)));
        if(!strcmp(f,"set_parameter")){ gml_fmod_set_global_param(fb,S(vm,a,n,0),N(a,n,1)); return vreal(0); }
        if(!strcmp(f,"get_parameter")) return vreal(gml_fmod_get_global_param(fb,S(vm,a,n,0)));
        if(!strcmp(f,"bank_load")||!strcmp(f,"bank_load_sample_data")||
           !strcmp(f,"init")||!strcmp(f,"studio_init")) return vreal(1);
        if(!strcmp(f,"destroy")){ gml_fmod_stop_all(fb); return vreal(0); }
        if(!strcmp(f,"set_num_listeners")||!strcmp(f,"update")) return vreal(0);
        if(!strcmp(f,"set_listener_attributes")){ gml_fmod_set_listener(fb,N(a,n,1),N(a,n,2)); return vreal(0); }
        if(!strcmp(f,"event_instance_set_3d_attributes")){ gml_fmod_set_3d(fb,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
        if(builtin_setting(vm,"GML_DBG_FMOD_CALLS")){ anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[fmodcall] %s(",f);
          for(int i=0;i<n;i++){ if(a[i].t==V_STR) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"%s\"%s\"",i?",":"",a[i].s?a[i].s:""); else anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"%s%g",i?",":"",N(a,n,i)); }
          anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,")\n"); }
        return vreal(0);   /* remaining FMOD calls: no-op but harmless */
      }
      /* fallback lifecycle model (no bank set): keep audio-gated logic advancing */
      if(!strcmp(f,"event_create_instance")||!strncmp(f,"event_one_shot",14)){
        for(int i=1;i<512;i++) if(!state->fallback_audio_event[i].used){ state->fallback_audio_event[i].used=1; state->fallback_audio_event[i].playing=1; state->fallback_audio_event[i].paused=0; return vreal(i); }
        return vreal(0); }
      if(!strcmp(f,"event_instance_play")){ int id=(int)N(a,n,0); if(id>0&&id<512){ state->fallback_audio_event[id].used=1; state->fallback_audio_event[id].playing=1; state->fallback_audio_event[id].paused=0; } return vreal(0); }
      if(!strcmp(f,"event_instance_is_playing")){ int id=(int)N(a,n,0); return vreal((id>0&&id<512&&state->fallback_audio_event[id].used&&state->fallback_audio_event[id].playing)?1:0); }
      if(!strcmp(f,"event_instance_get_paused")){ int id=(int)N(a,n,0); return vreal((id>0&&id<512&&state->fallback_audio_event[id].paused)?1:0); }
      if(!strcmp(f,"event_instance_set_paused")){ int id=(int)N(a,n,0); if(id>0&&id<512) state->fallback_audio_event[id].paused=(N(a,n,1)>=0.5); return vreal(0); }
      if(!strcmp(f,"event_instance_stop")||!strcmp(f,"event_instance_release")){ int id=(int)N(a,n,0); if(id>0&&id<512){ state->fallback_audio_event[id].playing=0; if(!strcmp(f,"event_instance_release")) state->fallback_audio_event[id].used=0; } return vreal(0); }
      if(!strcmp(f,"event_instance_set_parameter")){ int id=(int)N(a,n,0); const char *pn=S(vm,a,n,1);
        if(id>0&&id<512&&pn&&*pn){ int pi=-1; for(int i=0;i<state->fallback_audio_event[id].nparam;i++) if(!strcmp(state->fallback_audio_event[id].param[i].name,pn)){ pi=i; break; }
          if(pi<0 && state->fallback_audio_event[id].nparam<16){ pi=state->fallback_audio_event[id].nparam++; snprintf(state->fallback_audio_event[id].param[pi].name,sizeof(state->fallback_audio_event[id].param[pi].name),"%s",pn); }
          if(pi>=0) state->fallback_audio_event[id].param[pi].value=N(a,n,2); } return vreal(0); }
      if(!strcmp(f,"event_instance_get_parameter")){ int id=(int)N(a,n,0); const char *pn=S(vm,a,n,1);
        if(id>0&&id<512&&pn&&*pn) for(int i=0;i<state->fallback_audio_event[id].nparam;i++) if(!strcmp(state->fallback_audio_event[id].param[i].name,pn)) return vreal(state->fallback_audio_event[id].param[i].value);
        return vreal(0); }
      if(!strcmp(f,"set_parameter")){ const char *pn=S(vm,a,n,0);
        if(pn&&*pn){ int pi=-1; for(int i=0;i<state->fallback_audio_global_parameter_count;i++) if(!strcmp(state->fallback_audio_global_parameter[i].name,pn)){ pi=i; break; }
          if(pi<0 && state->fallback_audio_global_parameter_count<16){ pi=state->fallback_audio_global_parameter_count++; snprintf(state->fallback_audio_global_parameter[pi].name,sizeof(state->fallback_audio_global_parameter[pi].name),"%s",pn); }
          if(pi>=0) state->fallback_audio_global_parameter[pi].value=N(a,n,1); } return vreal(0); }
      if(!strcmp(f,"get_parameter")){ const char *pn=S(vm,a,n,0);
        if(pn&&*pn) for(int i=0;i<state->fallback_audio_global_parameter_count;i++) if(!strcmp(state->fallback_audio_global_parameter[i].name,pn)) return vreal(state->fallback_audio_global_parameter[i].value);
        return vreal(0); }
      if(!strcmp(f,"bank_load")||!strcmp(f,"bank_load_sample_data")||!strcmp(f,"init")||!strcmp(f,"studio_init")) return vreal(1);
      if(!strcmp(f,"destroy")){ memset(state->fallback_audio_event,0,sizeof(state->fallback_audio_event)); state->fallback_audio_global_parameter_count=0; return vreal(0); }
      if(!strcmp(f,"set_num_listeners")||!strcmp(f,"set_listener_attributes")||
         !strcmp(f,"event_instance_set_3d_attributes")||!strcmp(f,"update")) return vreal(0);
      return vreal(0);
}

GmlVal builtin_call_impl(GmlVM *vm, const char *nm, GmlVal *a, int n);
GmlVal gml_builtin_call(GmlVM *vm, const char *nm, GmlVal *a, int n){
  if(!gml_builtin_state_ensure(vm)) return vundef();
  if(!hotprof_enabled()) return builtin_call_impl(vm,nm,a,n);
  double t0=hotprof_now();
  GmlVal v=builtin_call_impl(vm,nm,a,n);
  hotprof_add(nm,(hotprof_now()-t0)*1000.0);
  return v;
}
GmlVal builtin_call_impl(GmlVM *vm, const char *nm, GmlVal *a, int n){
  int id=gml_builtin_fast_id(vm,nm);
  if(id>0) return gml_builtin_call_fast_id(vm,id,nm,a,n);
  GmlRender *R=(GmlRender*)vm->render;
  (void)graphics_state_for_vm(vm);
  if(d3_try_draw_2d_builtin(vm,nm,a,n)) return vreal(0);
  /* Classic dynamic room tiles are runtime objects rather than modern tilemaps. Keep them in
   * the runtime-layer store for room-entry cleanup, state serialization and depth ordering. A
   * Classic background id names a BGND resource rather than a sprite resource. */
  if(vm->win && anygm_policy_uses_classic_runtime(vm->win) && !strcmp(nm,"tile_add")){
    double dep=N(a,n,7);
    GmlRtLayer *l=NULL;
    for(int i=0;i<vm->n_rtl;i++)
      if(vm->rtl[i].used && vm->rtl[i].depth==dep){ l=&vm->rtl[i]; break; }
    if(!l){
      l=gml_rt_layer_new(vm);
      if(!l) return vreal(-1);
      l->depth=dep;
      snprintf(l->name,sizeof l->name,"__classic_tile_depth_%d",(int)dep);
      if(builtin_setting(vm,"GML_LOG_RTL")){
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rtl] f%ld classic tile layer depth=%g id=%d\n",vm->frame,dep,l->id); }
    }
    GmlRtElem *e=gml_rt_elem_new(vm);
    if(!e) return vreal(-1);
    e->type=7; e->layer=l->id;
    e->sprite=(int)N(a,n,0);
    e->sx=(int)N(a,n,1); e->sy=(int)N(a,n,2);
    e->w=(int)N(a,n,3); e->h=(int)N(a,n,4);
    e->x=N(a,n,5); e->y=N(a,n,6);
    if(builtin_setting(vm,"GML_LOG_RTL")){
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rtl] f%ld tile_add bg=%d src=(%d,%d %dx%d) pos=(%g,%g) depth=%g id=%d\n",
              vm->frame,e->sprite,e->sx,e->sy,e->w,e->h,e->x,e->y,dep,e->id); }
    return vreal(e->id);
  }
  /* Neutralize a compatibility sleep() busy-wait implemented as a GlobalScript. In a standalone runtime
   * it halts the execution thread, but inside anygm_run_frame it blocks the host callback.
   * Matched only with the gml_(Global)Script_ prefix so GM8's real `sleep` builtin is untouched. */
  { const char *sb=NULL;
    if(!strncmp(nm,"gml_GlobalScript_",17)) sb=nm+17;
    else if(!strncmp(nm,"gml_Script_",11)) sb=nm+11;
    if(sb && !strcmp(sb,"sleep")) return vreal(0); }
  /* Native shadows of the stock GM8-compat tile scripts. tile_layer_find otherwise iterates
   * every runtime-layer element in bytecode with 3-6 accessor builtins per element, each doing
   * an O(n) id lookup, producing quadratic work on large runtime tile layers.
   * Semantics replicated exactly from the stock compat script (layer slot order, element slot
   * order, the >=0 / negative scale branches, half-open right/bottom edges). Names may arrive
   * gml_Script_-prefixed (bc17 FUNC naming). */
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
  (void)R;
  return gml_builtin_try_collision(vm,nm,a,n);
}

GmlVal gml_builtin_continue_values_strings(GmlVM *vm, const char *nm,
                                           GmlVal *a, int n){
  return gml_builtin_try_values_strings(vm,nm,a,n);
}

GmlVal gml_builtin_try_scripts_fallback(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
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

  return gml_builtin_try_layers(vm,nm,a,n);
}

GmlVal gml_builtin_try_platform_tail(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- draw / audio / misc: stubbed until the renderer/audio land ----
   * Keep this after script fallback: user scripts can legally share prefixes
   * such as draw_ or audio_ and must resolve before broad no-op fallbacks. */
  /* Explicit no-ops: functions that are legitimately inert for a software-rendered host core.
   * Handling them here (instead of the catch-all) documents that intent and keeps them out of the
   * "unknown builtin" audit log. mouse_wheel_* have no input source → 0 (not scrolled). */
  if(!strcmp(nm,"mouse_wheel_up")){ int wh; gml_input_mouse(vm,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,&wh); return vreal(wh>0); }
  if(!strcmp(nm,"mouse_wheel_down")){ int wh; gml_input_mouse(vm,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,&wh); return vreal(wh<0); }
  if(!strcmp(nm,"device_get_tilt_x")||!strcmp(nm,"device_get_tilt_y")||!strcmp(nm,"device_get_tilt_z")) return vreal(0);
  /* gpu_set_blendmode(bm): GM simple enum — bm_normal(0), bm_add(1), bm_max(2), bm_subtract(3).
   * The software preset for subtract is the documented fixed-function pair
   * (bm_zero,bm_inv_src_colour), distinct from the arithmetic subtract equation. */
  if(!strcmp(nm,"draw_set_blend_mode")||   /* GMS1.x compatibility name */
     !strcmp(nm,"gpu_set_blendmode")){ GmlRender *R2=(GmlRender*)vm->render; int bm=(int)N(a,n,0);
    if(builtin_setting(vm,"GML_DBG_BM")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[bm] gpu_set_blendmode(%d)\n",bm);
    builtin_set_blendmode(R2,bm); return vreal(0); }
  if(!strcmp(nm,"gpu_get_blendmode")){
    GmlRender *R2=(GmlRender*)vm->render;
    int internal=builtin_draw_state(R2).blend_mode;
    int preset=internal==0?0:internal==1?1:internal==4?2:internal==2?3:-1;
    return vreal(preset);
  }
  if(!strcmp(nm,"gpu_get_colorwriteenable")){
    GmlRender *R2=(GmlRender*)vm->render;
    unsigned mask=builtin_draw_state(R2).color_write_mask;
    GmlVal out=gml_arr_new(4,vreal(0));
    for(int i=0;i<4;i++) gml_arr_set(out,i,vreal((mask>>i)&1u));
    return out;
  }
  if(!strcmp(nm,"gpu_set_colorwriteenable")){
    GmlRender *R2=(GmlRender*)vm->render;
    if(R2 && n>0){
      unsigned mask=0;
      if(a[0].t==V_ARR){
        for(int i=0;i<4;i++){
          GmlVal channel=gml_arr_get(a[0],i);
          if(N(&channel,1,0)!=0.0) mask|=1u<<i;
        }
      } else {
        for(int i=0;i<4 && i<n;i++) if(N(a,n,i)!=0.0) mask|=1u<<i;
      }
      GmlRenderDrawState draw={0};
      draw.color_write_mask=mask;
      gml_render_draw_state_update(R2,&draw,GML_RENDER_DRAW_STATE_COLOR_WRITE_MASK);
      GmlBuiltinState *state=builtin_state_ensure(vm);
      if(builtin_setting(vm,"GML_LOG_GPU") && (!state || state->gpu_log_count++<24))
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gpu] f%ld colorwrite=%X\n",vm->frame,mask);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"gpu_set_blendmode_ext")||!strcmp(nm,"gpu_set_blendmode_ext_sepalpha")||
     !strcmp(nm,"draw_set_blend_mode_ext")){
    builtin_set_blendmode_ext(vm,(GmlRender*)vm->render,(int)N(a,n,0),(int)N(a,n,1));
    return vreal(0);
  }
  if(!strcmp(nm,"gpu_set_blendequation")||!strcmp(nm,"gpu_set_blendequation_sepalpha")){
    GmlRender *R2=(GmlRender*)vm->render;
    if(R2){
      int rgb=(int)N(a,n,0), al=!strcmp(nm,"gpu_set_blendequation_sepalpha")?(int)N(a,n,1):rgb;
      if(rgb<1 || rgb>5) rgb=1;
      if(al<1 || al>5) al=1;
      GmlRenderDrawState draw={0};
      draw.blend_equation=rgb; draw.blend_equation_alpha=al;
      gml_render_draw_state_update(R2,&draw,GML_RENDER_DRAW_STATE_BLEND_EQUATION|
                                            GML_RENDER_DRAW_STATE_BLEND_EQUATION_ALPHA);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"gpu_push_state")){
    GmlRender *R2=(GmlRender*)vm->render;
    (void)gml_render_gpu_state_push(R2);
    return vreal(0);
  }
  if(!strcmp(nm,"gpu_pop_state")){
    GmlRender *R2=(GmlRender*)vm->render;
    (void)gml_render_gpu_state_pop(R2);
    return vreal(0);
  }
  if(!strcmp(nm,"gpu_set_alphatestenable")){ GmlRender *R2=(GmlRender*)vm->render;
    GmlRenderDrawState draw={0}; draw.alpha_test_enable=N(a,n,0)>=0.5;
    gml_render_draw_state_update(R2,&draw,GML_RENDER_DRAW_STATE_ALPHA_TEST_ENABLE);
    return vreal(0); }
  if(!strcmp(nm,"gpu_get_alphatestenable")){ GmlRender *R2=(GmlRender*)vm->render;
    return vreal(builtin_draw_state(R2).alpha_test_enable); }
  if(!strcmp(nm,"gpu_set_alphatestref")){ GmlRender *R2=(GmlRender*)vm->render; int ref=(int)N(a,n,0);
    GmlRenderDrawState draw={0}; if(ref<0) ref=0; else if(ref>255) ref=255; draw.alpha_test_reference=ref;
    gml_render_draw_state_update(R2,&draw,GML_RENDER_DRAW_STATE_ALPHA_TEST_REFERENCE); return vreal(0); }
  if(!strcmp(nm,"gpu_get_alphatestref")){ GmlRender *R2=(GmlRender*)vm->render;
    return vreal(builtin_draw_state(R2).alpha_test_reference); }
  if(!strcmp(nm,"gpu_set_sprite_cull")||
     !strcmp(nm,"gpu_set_tex_filter")||
     !strcmp(nm,"display_reset")||
     !strcmp(nm,"window_set_cursor")||
     !strcmp(nm,"device_mouse_dbclick_enable")||
     !strcmp(nm,"achievement_login")||!strcmp(nm,"achievement_logout")||
     !strncmp(nm,"native_ga_",10)||!strncmp(nm,"configureBuild",14)||!strncmp(nm,"configureSdk",12)||
     !strncmp(nm,"setEnabledInfoLog",17)||!strncmp(nm,"setEnabledVerboseLog",20))
    return vreal(0);

  /* GML_LOG_STUB lists every builtin that reaches the broad no-op or catch-all. Cache the
   * development-setting check because particle-heavy execution can make this a hot path. */
  GmlBuiltinState *state=builtin_state_ensure(vm);
  int log_stub=builtin_setting(vm,"GML_LOG_STUB")!=NULL;
  if(state){
    if(state->log_stub<0) state->log_stub=log_stub;
    log_stub=state->log_stub;
  }
  if(!strncmp(nm,"draw_",5)||!strncmp(nm,"audio_",6)||!strncmp(nm,"path_",5)||
     !strncmp(nm,"place_",6)||!strncmp(nm,"move_",5)||!strncmp(nm,"tile_",5)||
     !strncmp(nm,"font_",5)||!strncmp(nm,"window_",7)||!strncmp(nm,"instance_",9)){
    if(log_stub) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[stub] %s\n",nm);
    return vreal(0);
  }

  /* unknown builtin: log once per run so coverage audits can spot regressions */
  if(log_stub) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[unk] %s\n",nm);
  if(!state || !state->unknown_builtin_logged){
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gml] unknown builtin: %s\n",nm);
    if(state) state->unknown_builtin_logged=1;
  }
  return vreal(0);
}
