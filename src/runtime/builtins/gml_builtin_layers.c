/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Runtime layer, element, and tilemap builtin adapters. */
#include "gml_builtin_internal.h"
#include "gml_render.h"
#include "anygm_host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int log_tilecol_on(GmlVM *vm){
  return builtin_log_tile_collision(vm);
}
int builtin_layer_exact(GmlVM *vm, const char *nm, GmlVal *a, int n, GmlVal *out){
  if(!vm || !nm || !out) return 0;
  if(!strcmp(nm,"layer_create")){
    GmlRtLayer *l=gml_rt_layer_new(vm);
    if(!l){ *out=vreal(-1); return 1; }
    l->depth=N(a,n,0);
    if(builtin_setting(vm,"GML_LOG_RTL")){ 
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rtl] f%ld layer_create depth=%f id=%d\n",vm->frame,l->depth,l->id); }
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
      if(builtin_setting(vm,"GML_LOG_RTL")){ 
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rtl] f%ld layer_depth %s id=%d %.0f -> %.0f\n",
                vm->frame,l->name,l->id,old,l->depth); }
    }else if(builtin_setting(vm,"GML_LOG_RTL")){
      
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rtl] f%ld layer_depth unresolved arg0=%s value=%.0f\n",
              vm->frame,S(vm,a,n,0),N(a,n,1));
    }
    *out=vreal(0); return 1;
  }
  if(!strcmp(nm,"layer_get_name")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n);
    *out=l?vstr_owned(strdup(l->name)):vstr(""); return 1; }
  if(!strcmp(nm,"layer_get_id")){
    const char *name=S(vm,a,n,0);
    for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used && !strcmp(vm->rtl[i].name,name)){ *out=vreal(vm->rtl[i].id); return 1; }
    *out=vreal(-1); return 1;
  }
  if(!strcmp(nm,"layer_exists")){ *out=vreal(rt_layer_resolve(vm,a,n)!=NULL); return 1; }
  /* Effect structures are not modeled here. Preserve the absent-effect sentinel
   * without changing layer visibility. */
  if(!strcmp(nm,"layer_get_fx")){ *out=vreal(-1); return 1; }
  if(!strcmp(nm,"layer_enable_fx")){ *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_set_visible")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n);
    if(builtin_setting(vm,"GML_LOG_RTL")){
      if(l) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
        "[rtl] f%ld layer_set_visible %s id=%d -> %d\n",vm->frame,l->name,l->id,(int)N(a,n,1));
      else anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
        "[rtl] f%ld layer_set_visible unresolved arg0=%s(%.0f) -> %d\n",
        vm->frame,a&&n>0&&a[0].t==V_STR&&a[0].s?a[0].s:"<num>",N(a,n,0),(int)N(a,n,1));
    }
    if(l) l->visible=(int)N(a,n,1);
    *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_get_visible")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); *out=vreal(l?l->visible:0); return 1; }
  if(!strcmp(nm,"layer_add_instance")){
    GmlRtLayer *l=rt_layer_resolve(vm,a,n);
    GmlInstance *in=NULL;
    int instance_id=(int)N(a,n,1);
    for(int i=0;n>1 && i<vm->inst_count;i++){
      GmlInstance *candidate=&vm->inst[i];
      if((candidate->active || candidate->deactivated) && !candidate->marked &&
         (int)candidate->id==instance_id){ in=candidate; break; }
    }
    if(l && in){
      in->depth=l->depth;
      in->draw_layer_order=l->order;
      in->draw_layer_element_order=-1;
    }
    *out=vreal(0); return 1;
  }
  if(!strcmp(nm,"instance_activate_layer")||!strcmp(nm,"instance_deactivate_layer")){
    GmlRtLayer *l=rt_layer_resolve(vm,a,n);
    int activate=!strcmp(nm,"instance_activate_layer");
    if(l) for(int i=0;i<vm->inst_count;i++){
      GmlInstance *in=&vm->inst[i];
      if(in->marked || in->draw_layer_order!=l->order) continue;
      if(activate){ if(in->deactivated){ in->active=1; in->deactivated=0; } }
      else if(in->active){ in->active=0; in->deactivated=1; }
    }
    *out=vreal(0); return 1;
  }
  if(!strcmp(nm,"layer_x")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(l){ rt_layer_touch(vm,l); l->x=N(a,n,1); } *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_y")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(l){ rt_layer_touch(vm,l); l->y=N(a,n,1); } *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_hspeed")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(l){ rt_layer_touch(vm,l); l->hs=N(a,n,1); } *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_vspeed")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(l){ rt_layer_touch(vm,l); l->vs=N(a,n,1); } *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_get_x")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(!l){ *out=vreal(0); return 1; }
    if(l->touched){ *out=vreal(l->x); return 1; }
     long fin=vm->frame-vm->room_enter_frame; if(fin<0)fin=0; *out=vreal(l->x+l->hs*fin); return 1; }
  if(!strcmp(nm,"layer_get_y")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(!l){ *out=vreal(0); return 1; }
    if(l->touched){ *out=vreal(l->y); return 1; }
     long fin=vm->frame-vm->room_enter_frame; if(fin<0)fin=0; *out=vreal(l->y+l->vs*fin); return 1; }
  if(!strcmp(nm,"layer_get_hspeed")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); *out=vreal(l?l->hs:0); return 1; }
  if(!strcmp(nm,"layer_get_vspeed")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); *out=vreal(l?l->vs:0); return 1; }
  if(!strcmp(nm,"layer_shader")||!strcmp(nm,"layer_get_shader")){
    GmlRtLayer *l=rt_layer_resolve(vm,a,n);
    /* Compatibility scripts can pass an element handle; resolve it to the owning layer before
     * applying the same layer-scoped state. */
    if(!l && n>0 && a[0].t==V_REAL){
      GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0));
      if(e) l=gml_rt_layer_find(vm,e->layer);
    }
    int slot=(l && vm->rtl)?(int)(l-vm->rtl):-1;
    if(!strcmp(nm,"layer_get_shader")){
      double encoded=slot>=0?gml_global_arr(vm,"__gml_layer_shader",slot):0;
      *out=vreal(encoded!=0?encoded-1:-1); return 1;
    }
    if(slot>=0){ int shader=(int)N(a,n,1);
      gml_set_global_arr(vm,"__gml_layer_shader",slot,shader>=0?shader+1:0); }
    *out=vreal(0); return 1;
  }
  if(!strcmp(nm,"layer_force_draw_depth")||!strcmp(nm,"layer_reset_target_all")){
    *out=vreal(0); return 1;
  }

  if(!strcmp(nm,"layer_sprite_get_id")){
    GmlRtLayer *l=rt_layer_resolve(vm,a,n);
    GmlRtElem *e=rt_sprite_for_layer_name(vm,l?l->id:-1,S(vm,a,n,1));
    *out=vreal(e?e->id:-1); return 1;
  }
  /* Playing sequences use one runtime layer element each. Type 9 identifies it; `sprite` holds
   * the sequence and `image_index` its head in the existing element-state representation. The
   * frame advance updates that head and the layer draw samples the graphic tracks at it. */
  if(!strcmp(nm,"layer_sequence_create")){
    GmlRtLayer *l=rt_layer_resolve(vm,a,n);
    int seq=(int)N(a,n,3);
    if(seq<0 || seq>=vm->n_sequences){ *out=vreal(-1); return 1; }
    GmlRtElem *e=gml_rt_elem_new(vm);
    if(!e){ *out=vreal(-1); return 1; }
    e->type=9; e->layer=l?l->id:0; e->x=N(a,n,1); e->y=N(a,n,2);
    e->sprite=seq; e->image_index=0.0; e->alpha=1.0; e->xs=e->ys=1.0; e->visible=1;
    e->blend=0xFFFFFFu;
    *out=vreal(e->id); return 1;
  }
  if(!strcmp(nm,"layer_sequence_destroy")){
    GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==9) e->used=0;
    *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_sequence_exists")){
    GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,n>1?1:0));
    *out=vreal(e && e->type==9 && e->used); return 1; }
  if(!strcmp(nm,"layer_sequence_is_finished")){
    GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0));
    if(!e || e->type!=9 || e->sprite<0 || e->sprite>=vm->n_sequences){ *out=vreal(1); return 1; }
    *out=vreal(e->image_index>=vm->sequences[e->sprite].length); return 1; }
  if(!strcmp(nm,"layer_sequence_headpos")){
    GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0));
    if(n>1){ if(e && e->type==9) e->image_index=N(a,n,1); *out=vreal(0); }
    else *out=vreal((e && e->type==9)?e->image_index:0);
    return 1; }
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
  if(!strcmp(nm,"layer_sprite_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->blend=NU32(a,n,1)&0xFFFFFFu; *out=vreal(0); return 1; }
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
  if(!strcmp(nm,"layer_tile_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->blend=NU32(a,n,1)&0xFFFFFF; *out=vreal(0); return 1; }
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
  if(!strcmp(nm,"layer_background_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->blend=NU32(a,n,1)&0xFFFFFF; *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_htiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->htiled=(int)N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_vtiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->vtiled=(int)N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_stretch")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->stretch=(int)N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->xs=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->ys=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_index")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->image_index=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_speed")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->image_speed=N(a,n,1); *out=vreal(0); return 1; }
  if(!strcmp(nm,"layer_background_get_sprite")){
    GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->sprite:-1); return 1; }
  if(!strcmp(nm,"layer_background_get_index")){
    GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->image_index:0); return 1; }
  if(!strcmp(nm,"layer_background_get_visible")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->visible:0); return 1; }
  if(!strcmp(nm,"layer_background_get_alpha")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->alpha:1); return 1; }
  if(!strcmp(nm,"layer_background_get_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?(double)e->blend:0xFFFFFF); return 1; }
  if(!strcmp(nm,"layer_background_get_htiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->htiled:0); return 1; }
  if(!strcmp(nm,"layer_background_get_vtiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->vtiled:0); return 1; }
  if(!strcmp(nm,"layer_background_get_stretch")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->stretch:0); return 1; }
  if(!strcmp(nm,"layer_background_get_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->xs:1); return 1; }
  if(!strcmp(nm,"layer_background_get_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->ys:1); return 1; }
  if(!strcmp(nm,"layer_background_get_speed")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); *out=vreal(e?e->image_speed:0); return 1; }
  return 0;
}

GmlVal gml_builtin_try_layers_early(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- tile-layer manipulation (mutations applied to the room's tiles at draw) ---- */
  if(!strcmp(nm,"tile_layer_delete")){ gml_tile_layer_delete(vm,(int)N(a,n,0)); return vreal(0); }
  if(!strcmp(nm,"tile_layer_depth")){ gml_tile_layer_depth(vm,(int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
  if(!strcmp(nm,"tile_layer_shift")){ gml_tile_layer_shift(vm,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
  if(!strcmp(nm,"tile_layer_delete_at")){ gml_tile_layer_delete_at(vm,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
  if(!strcmp(nm,"tile_layer_hide")){ gml_tile_layer_hide(vm,(int)N(a,n,0),1); return vreal(0); }
  if(!strcmp(nm,"tile_layer_show")){ gml_tile_layer_hide(vm,(int)N(a,n,0),0); return vreal(0); }

  return gml_builtin_try_platform(vm,nm,a,n);
}

GmlVal gml_builtin_try_layers(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- runtime layers and elements (Studio layer_* API) ----
   * Compatibility scripts for converted projects implement tile and background operations on top
   * of this model. Elements live in vm->rte, layers in
   * vm->rtl; the room-draw pass merges visible tile/background elements into the unified
   * depth-sorted list. */
  /* ---- Studio tile layers to tile-based collision. Tile-map ids come from layer_tilemap_get_id;
   * tilemap_get returns the raw tile datum (0 = empty cell), tile_get_index masks its index.
   * Collision helpers use these values to locate solid cells. */
  if(!strcmp(nm,"layer_tilemap_get_id")){
    GmlTileMap *tm=gml_tilemap_by_layer(vm, n>0?a[0]:vreal(-1));
    if(log_tilecol_on(vm)){ if(n>0&&a[0].t==V_STR) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[tilecol] layer_tilemap_get_id(\"%s\") -> %d\n",a[0].s?a[0].s:"",tm?tm->id:-1);
      else anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[tilecol] layer_tilemap_get_id(%g) -> %d (%s)\n",N(a,n,0),tm?tm->id:-1,tm?tm->name:"none"); }
    return vreal(tm?tm->id:-1); }
  if(!strcmp(nm,"tilemap_get_cell_x_at_pixel")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0));
    double tx=0.0; gml_tilemap_effective(vm,tm,&tx,NULL,NULL,NULL);
    return vreal(tm&&tm->tw?floor((N(a,n,1)-tx)/tm->tw):0); }
  if(!strcmp(nm,"tilemap_get_cell_y_at_pixel")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0));
    double ty=0.0; gml_tilemap_effective(vm,tm,NULL,&ty,NULL,NULL);
    return vreal(tm&&tm->th?floor((N(a,n,2)-ty)/tm->th):0); }
  if(!strcmp(nm,"tilemap_get")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0));
    int cx=(int)N(a,n,1),cy=(int)N(a,n,2);
    if(!tm||cx<0||cy<0||cx>=tm->cols||cy>=tm->rows){ if(log_tilecol_on(vm)) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[tilecol] tilemap_get(id=%g,%d,%d) -> 0 (tm=%s oob)\n",N(a,n,0),cx,cy,tm?tm->name:"NULL"); return vreal(0); }
    const unsigned char *p=tm->tiles+((size_t)cy*tm->cols+cx)*4;
    uint32_t datum=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
    if(log_tilecol_on(vm)) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[tilecol] tilemap_get(%s,%d,%d) -> %u (idx=%u)\n",tm->name,cx,cy,datum,datum&0x7FFFF);
    return vreal((double)datum); }
  if(!strcmp(nm,"tilemap_get_at_pixel")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0));
    if(!tm||!tm->tw||!tm->th) return vreal(0);
    double tx,ty; gml_tilemap_effective(vm,tm,&tx,&ty,NULL,NULL);
    int cx=(int)floor((N(a,n,1)-tx)/tm->tw), cy=(int)floor((N(a,n,2)-ty)/tm->th);
    if(cx<0||cy<0||cx>=tm->cols||cy>=tm->rows) return vreal(0);
    const unsigned char *p=tm->tiles+((size_t)cy*tm->cols+cx)*4;
    return vreal((double)((uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24))); }
  if(!strcmp(nm,"tilemap_set")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0));
    gml_tilemap_set_cell(tm,(int)N(a,n,2),(int)N(a,n,3),NU32(a,n,1)); return vreal(0); }
  if(!strcmp(nm,"tilemap_set_at_pixel")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0));
    if(tm&&tm->tw&&tm->th){
      double tx,ty; gml_tilemap_effective(vm,tm,&tx,&ty,NULL,NULL);
      int cx=(int)floor((N(a,n,2)-tx)/tm->tw), cy=(int)floor((N(a,n,3)-ty)/tm->th);
      gml_tilemap_set_cell(tm,cx,cy,NU32(a,n,1));
    }
    return vreal(0); }
  if(!strcmp(nm,"tilemap_get_width")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0)); return vreal(tm?tm->cols:0); }
  if(!strcmp(nm,"tilemap_get_height")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0)); return vreal(tm?tm->rows:0); }
  if(!strcmp(nm,"tilemap_get_tileset")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0)); return vreal(tm?tm->tileset:-1); }
  if(!strcmp(nm,"tilemap_get_tile_width")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0)); return vreal(tm?tm->tw:0); }
  if(!strcmp(nm,"tilemap_get_tile_height")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0)); return vreal(tm?tm->th:0); }
  if(!strcmp(nm,"tilemap_get_x")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0)); double tx=0.0; gml_tilemap_effective(vm,tm,&tx,NULL,NULL,NULL); return vreal(tm?tx:0); }
  if(!strcmp(nm,"tilemap_get_y")){ GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0)); double ty=0.0; gml_tilemap_effective(vm,tm,NULL,&ty,NULL,NULL); return vreal(tm?ty:0); }
  if(!strcmp(nm,"tilemap_get_frame")){
    GmlTileMap *tm=gml_tilemap_find(vm,(int)N(a,n,0));
    double speed=gml_room_speed(vm);
    double elapsed_seconds=speed>0.0?vm->frame/speed:0.0;
    return vreal(tm?gml_render_background_tile_animation_frame(
                      R,tm->tileset,elapsed_seconds):0);
  }
  /* tile datum decode (GMS2 bit layout: index low 19 bits, then mirror/flip/rotate) */
  if(!strcmp(nm,"tile_get_index")){ uint32_t t=NU32(a,n,0); return vreal(t & 0x7FFFF); }
  if(!strcmp(nm,"tile_get_empty")){ uint32_t t=NU32(a,n,0); return vreal((t & 0x7FFFF)==0); }
  if(!strcmp(nm,"tile_get_mirror")){ uint32_t t=NU32(a,n,0); return vreal((t>>28)&1); }
  if(!strcmp(nm,"tile_get_flip")){ uint32_t t=NU32(a,n,0); return vreal((t>>29)&1); }
  if(!strcmp(nm,"tile_get_rotate")){ uint32_t t=NU32(a,n,0); return vreal((t>>30)&1); }
  if(!strcmp(nm,"tile_set_empty")){ uint32_t t=NU32(a,n,0); return vreal((double)(t & ~0x7FFFFu)); }
  if(!strcmp(nm,"tile_set_index")){ uint32_t t=NU32(a,n,0), idx=NU32(a,n,1);
    return vreal((double)((t & ~0x7FFFFu) | (idx & 0x7FFFFu))); }
  if(!strcmp(nm,"tile_set_mirror")){ uint32_t t=NU32(a,n,0);
    if(N(a,n,1)>=0.5) t|=(1u<<28); else t&=~(1u<<28); return vreal((double)t); }
  if(!strcmp(nm,"tile_set_flip")){ uint32_t t=NU32(a,n,0);
    if(N(a,n,1)>=0.5) t|=(1u<<29); else t&=~(1u<<29); return vreal((double)t); }
  if(!strcmp(nm,"tile_set_rotate")){ uint32_t t=NU32(a,n,0);
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
      if(builtin_setting(vm,"GML_LOG_RTL")){ 
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rtl] f%ld layer_create depth=%f id=%d\n",vm->frame,l->depth,l->id); }
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
      GmlRtElem *e=rt_sprite_for_layer_name(vm,l?l->id:-1,S(vm,a,n,1));
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
    if(!strcmp(sub,"sprite_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e && e->type==3) e->blend=NU32(a,n,1)&0xFFFFFFu; return vreal(0); }
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
        if(builtin_setting(vm,"GML_LOG_RTL")){ 
          anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rtl] f%ld layer_depth %s id=%d %.0f -> %.0f\n",
                  vm->frame,l->name,l->id,old,l->depth); }
      }else if(builtin_setting(vm,"GML_LOG_RTL")){
        
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rtl] f%ld layer_depth unresolved arg0=%s value=%.0f\n",
                vm->frame,S(vm,a,n,0),N(a,n,1));
      }
      return vreal(0); }
    if(!strcmp(sub,"get_name")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n);
      return l?vstr_owned(strdup(l->name)):vstr(""); }
    if(!strcmp(sub,"get_id")){ for(int i=0;i<vm->n_rtl;i++)   /* by name */
        if(vm->rtl[i].used && !strcmp(vm->rtl[i].name,S(vm,a,n,0))) return vreal(vm->rtl[i].id);
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
       long fin=vm->frame-vm->room_enter_frame; if(fin<0)fin=0; return vreal(l->x+l->hs*fin); }
    if(!strcmp(sub,"get_y")){ GmlRtLayer *l=rt_layer_resolve(vm,a,n); if(!l) return vreal(0);
      if(l->touched) return vreal(l->y);
       long fin=vm->frame-vm->room_enter_frame; if(fin<0)fin=0; return vreal(l->y+l->vs*fin); }
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
    if(!strcmp(sub,"tile_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->blend=NU32(a,n,1)&0xFFFFFF; return vreal(0); }
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
    if(!strcmp(sub,"background_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->blend=NU32(a,n,1)&0xFFFFFF; return vreal(0); }
    if(!strcmp(sub,"background_htiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->htiled=(int)N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_vtiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->vtiled=(int)N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_stretch")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->stretch=(int)N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->xs=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->ys=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_index")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->image_index=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_speed")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); if(e) e->image_speed=N(a,n,1); return vreal(0); }
    if(!strcmp(sub,"background_get_sprite")){
      GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->sprite:-1); }
    if(!strcmp(sub,"background_get_index")){
      GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->image_index:0); }
    if(!strcmp(sub,"background_get_visible")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->visible:0); }
    if(!strcmp(sub,"background_get_alpha")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->alpha:1); }
    if(!strcmp(sub,"background_get_blend")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?(double)e->blend:0xFFFFFF); }
    if(!strcmp(sub,"background_get_htiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->htiled:0); }
    if(!strcmp(sub,"background_get_vtiled")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->vtiled:0); }
    if(!strcmp(sub,"background_get_stretch")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->stretch:0); }
    if(!strcmp(sub,"background_get_xscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->xs:1); }
    if(!strcmp(sub,"background_get_yscale")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->ys:1); }
    if(!strcmp(sub,"background_get_speed")){ GmlRtElem *e=gml_rt_elem_find(vm,(int)N(a,n,0)); return vreal(e?e->image_speed:0); }
    /* unhandled layer_* fall through to the audit log below */
  }

  return gml_builtin_try_platform_tail(vm,nm,a,n);
}
