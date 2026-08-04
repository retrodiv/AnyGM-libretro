/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Legacy action-function argument adapters and ordered dispatch. */
#include "gml_builtin_internal.h"
#include "anygm_compatibility.h"
#include "gml_render.h"
#include "gml_particle.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

GmlVal gml_builtin_try_actions(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- portable legacy extension helpers ---- */
  if(!strcmp(nm,"depthy")){
    if(n==0 && vm->cur_self) vm->cur_self->depth=-(vm->cur_self->y/100.0);
    return vreal(0); }
  if(!strcmp(nm,"crear")){
    if(n==3) (void)gml_instance_create(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2));
    return vreal(0); }
  if(!strcmp(nm,"move_rpg")){
    legacy_move_rpg(vm,a,n);
    return vreal(0); }
  if(!strcmp(nm,"direction_rpg")){
    legacy_direction_rpg(vm,a,n);
    return vreal(0); }
  if(!strcmp(nm,"friction_platform")){
    legacy_friction_platform(vm,a,n);
    return vreal(0); }
  if(!strcmp(nm,"destruir")){
    legacy_destroy_self(vm,n);
    return vreal(0); }

  return gml_builtin_try_instances(vm,nm,a,n);
}

static void action_relative_point(GmlVM *vm,double *x,double *y){
  if(vm->action_relative && vm->cur_self){
    *x+=vm->cur_self->x;
    *y+=vm->cur_self->y;
  }
}

static uint32_t action_health_palette(int index){
  static const uint32_t colors[]={
    0x000000,0x808080,0xC0C0C0,0xFFFFFF,
    0x000080,0x008000,0x008080,0x800000,
    0x800080,0x808000,0x0000FF,0x00FF00,
    0x00FFFF,0xFF0000,0xFF00FF,0xFFFF00
  };
  return index>=0 && index<(int)(sizeof(colors)/sizeof(colors[0])) ? colors[index] : 0x800080;
}

static int action_health_component(double value){
  if(value<0) return 0;
  if(value>255) return 255;
  return (int)value;
}

GmlVal gml_builtin_try_actions_legacy(GmlVM *vm, const char *nm, GmlVal *a, int n){
  /* drag-and-drop actions (compiled to builtin calls) acting on the current instance */
  /* Desktop sleep (native or D&D) blocks only wall-clock time; no simulation ticks occur.
   * A host core must not stall the host thread, so consuming it without advancing game
   * state produces the same next rendered frame while preserving realtime performance. */
  if(!strcmp(nm,"action_sleep") || !strcmp(nm,"sleep")) return vreal(0);
  /* action_if(expr): D&D single-expression conditional. expr was evaluated on the stack by the
   * bytecode before this call → arg0 is the truth value. Returns the value (0 or 1). */
  if(!strcmp(nm,"action_if")) return vreal(N(a,n,0));
  /* action_if_variable(var, value, op): D&D compare-a-variable conditional. The bytecode has
   * already evaluated "var", so arg0 is the current value, not a variable name. */
  if(!strcmp(nm,"action_if_variable")){
    int op=(int)N(a,n,2);
    double vv=N(a,n,0), cv=N(a,n,1); int r=0;
    /* Legacy D&D comparison selector: equal, below, above, at-most, at-least, unequal.
     * Retained action calls can use the inclusive selectors directly. */
    switch(op){
      case 0: r=(vv==cv); break;
      case 1: r=(vv<cv); break;
      case 2: r=(vv>cv); break;
      case 3: r=(vv<=cv); break;
      case 4: r=(vv>=cv); break;
      case 5: r=(vv!=cv); break;
    }
    return vreal(r); }
  if(!strcmp(nm,"action_if_dice")){
    int sides=(int)floor(fabs(N(a,n,0)));
    return vreal(sides<=1 || (int)floor(gml_rng_value(vm)*sides)==0);
  }
  if(!strcmp(nm,"action_if_next_room")){
    int pos=order_pos(vm,vm->room_index);
    return vreal(pos>=0 && vm->win && pos+1<vm->win->n_room_order);
  }
  if(!strcmp(nm,"action_if_previous_room"))
    return vreal(order_pos(vm,vm->room_index)>0);
  if(!strcmp(nm,"action_if_object")){
    GmlInstance *s=vm->cur_self;
    double x=N(a,n,1), y=N(a,n,2);
    if(n>=4 && N(a,n,3)!=0 && s){ x+=s->x; y+=s->y; }
    return vreal(instance_at_point(vm,x,y,(int)N(a,n,0))!=NULL);
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
  if(!strcmp(nm,"action_another_room")){
    int target=(int)N(a,n,0);
    gml_set_global_scalar(vm,"transition_kind",N(a,n,1));
    if(vm->win && target>=0 && target<gml_room_count(vm->win)){
      gml_vm_warm_audio_for_room(vm,target);
      vm->pending_room=target;
    }
    return vreal(0); }
  if(!strcmp(nm,"action_previous_room")){
    gml_set_global_scalar(vm,"transition_kind",N(a,n,0));
    int pos=order_pos(vm,vm->room_index);
    if(pos>0){
      int target=(int)vm->win->room_order[pos-1];
      gml_vm_warm_audio_for_room(vm,target);
      vm->pending_room=target;
    }
    return vreal(0); }
  if(!strcmp(nm,"action_current_room")){
    gml_set_global_scalar(vm,"transition_kind",N(a,n,0));
    gml_vm_warm_audio_for_room(vm,vm->room_index); vm->pending_room=vm->room_index; return vreal(0); }
  if(!strcmp(nm,"action_next_room")){
    gml_set_global_scalar(vm,"transition_kind",N(a,n,0));
    int pos=order_pos(vm,vm->room_index);
    if(vm->win && pos>=0 && pos+1<vm->win->n_room_order){
      int target=(int)vm->win->room_order[pos+1]; gml_vm_warm_audio_for_room(vm,target); vm->pending_room=target;
    }
    return vreal(0);
  }
  if(!strcmp(nm,"action_move_to")){ GmlInstance*s=vm->cur_self; if(s){
      double x=N(a,n,0), y=N(a,n,1);
      if(vm->action_relative){ s->x+=x; s->y+=y; } else { s->x=x; s->y=y; } gml_colgrid_touch(vm,s); } return vreal(0); }
  if(!strcmp(nm,"action_move")||!strcmp(nm,"action_set_motion")){ GmlInstance*s=vm->cur_self; if(s){
      double direction=N(a,n,0), amount=N(a,n,1);
      if(!strcmp(nm,"action_move") && n>0 && a[0].t==V_STR){
        static const int directions[9]={225,270,315,180,-1,0,135,90,45};
        const char *mask=S(vm,a,n,0);
        int choices[9], count=0;
        for(int i=0;i<9 && mask[i];i++) if(mask[i]!='0') choices[count++]=i;
        if(count){ int choice;
          if(vm->win && anygm_policy_uses_classic_runtime(vm->win)){
            /* The classic action samples the whole 3x3 direction pad and retries disabled
             * cells. This deliberately consumes a variable number of RNG values. */
            do { choice=(int)floor(gml_rng_value(vm)*9.0); } while(choice<0 || choice>=9 || !mask[choice] || mask[choice]=='0');
          } else {
            int selected=(int)floor(gml_rng_value(vm)*count);
            if(selected<0) selected=0;
            if(selected>=count) selected=count-1;
            choice=choices[selected];
          }
          direction=directions[choice]; if(direction<0){ direction=0; amount=0; } }
      }
      if(vm->action_relative){
        double radians=direction*M_PI/180.0;
        s->hspeed+=amount*cos(radians); s->vspeed-=amount*sin(radians);
        motion_from_components(s);
        if(vm->win && anygm_policy_uses_classic_runtime(vm->win)){
          s->direction=fmod(s->direction,360.0); if(s->direction<0) s->direction+=360.0;
          double rounded=round(s->direction);
          if(fabs(rounded-s->direction)<0.0001) s->direction=rounded;
          if(s->direction>=360.0) s->direction-=360.0;
        }
      } else {
        s->direction=direction; s->speed=amount;
        motion_from_speed_direction(vm,s);
      } }
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
      if(vm->action_relative){ s->gravity_direction+=N(a,n,0); s->gravity+=N(a,n,1); }
      else { s->gravity_direction=N(a,n,0); s->gravity=N(a,n,1); } } return vreal(0); }
  if(!strcmp(nm,"action_set_friction")){ GmlInstance*s=vm->cur_self; if(s){
      if(vm->action_relative) s->friction+=N(a,n,0); else s->friction=N(a,n,0); } return vreal(0); }
  if(!strcmp(nm,"action_set_relative")){ vm->action_relative=N(a,n,0)>=0.5; return vreal(0); }
  if(!strcmp(nm,"action_sprite_set")){ GmlInstance *s=vm->cur_self; if(s){
      int sprite=(int)N(a,n,0); double subimage=N(a,n,1);
      s->sprite_index=sprite;
      /* The classic Change Sprite action uses a negative subimage as "keep the
       * current animation position".  It is not a drawable frame index.  When
       * the retained position does not exist in the new sprite,
       * start that sprite at frame zero. */
      if(subimage>=0) s->image_index=subimage;
      else {
        int frames=vm->render?gml_sprite_frames((GmlRender*)vm->render,sprite):0;
        if(frames>0 && floor(s->image_index)>=frames) s->image_index=0;
      }
      s->image_speed=N(a,n,2);
      gml_colgrid_touch(vm,s);
      if(vm->render && sprite>=0) gml_render_prefetch_sprite((GmlRender*)vm->render,sprite);
    } return vreal(0); }
  if(!strcmp(nm,"action_sprite_color") || !strcmp(nm,"action_sprite_colour")){ GmlInstance *s=vm->cur_self; if(s){
      s->image_blend=N(a,n,0); s->image_alpha=N(a,n,1); } return vreal(0); }
  if(!strcmp(nm,"action_color") || !strcmp(nm,"action_colour")){ GmlRender *r=(GmlRender*)vm->render;
    builtin_set_draw_color(r,(uint32_t)N(a,n,0));
    return vreal(0); }
  if(!strcmp(nm,"action_font")){ GmlRender *r=(GmlRender*)vm->render; if(r){
      GmlRenderDrawState state={0};
      state.font=(int)N(a,n,0); state.horizontal_alignment=(int)N(a,n,1);
      gml_render_draw_state_update(r,&state,
        GML_RENDER_DRAW_STATE_FONT|GML_RENDER_DRAW_STATE_HORIZONTAL_ALIGNMENT); }
    return vreal(0); }
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
  if(!strcmp(nm,"action_draw_score")||!strcmp(nm,"action_draw_life")){
    const char *key=!strcmp(nm,"action_draw_score")?"score":!strcmp(nm,"action_draw_life")?"lives":"health";
    GmlVal *value=gml_varmap_get(&vm->globals,key);
    double x=N(a,n,0),y=N(a,n,1); action_relative_point(vm,&x,&y);
    char text[256]; snprintf(text,sizeof(text),"%s%.0f",S(vm,a,n,2),value&&value->t==V_REAL?value->d:0.0);
    GmlRender *r=(GmlRender*)vm->render; if(r) gml_draw_text(r,x,y,text);
    return vreal(0); }
  if(!strcmp(nm,"action_draw_health")){
    GmlRender *r=(GmlRender*)vm->render;
    double x1=N(a,n,0),y1=N(a,n,1),x2=N(a,n,2),y2=N(a,n,3);
    action_relative_point(vm,&x1,&y1); action_relative_point(vm,&x2,&y2);
    GmlVal *value=gml_varmap_get(&vm->globals,"health");
    double health=value&&value->t==V_REAL?value->d:0.0,ratio=health/100.0;
    int back=(int)N(a,n,4),bar=(int)N(a,n,5);
    uint32_t fill;
    if(bar==0){
      int red,green;
      if(ratio>0.5){ red=action_health_component((1.0-ratio)*510.0); green=255; }
      else { red=255; green=action_health_component(ratio*510.0); }
      fill=(uint32_t)red|((uint32_t)green<<8);
    } else if(bar==1){
      int component=action_health_component(ratio*255.0);
      fill=(uint32_t)component|((uint32_t)component<<8)|((uint32_t)component<<16);
    } else fill=action_health_palette(bar-2);
    if(r){
      GmlRenderTargetMetrics target=builtin_target_metrics(r);
      int ix1=(int)floor(draw_gui_x(r,x1)-target.camera_x);
      int iy1=(int)floor(draw_gui_y(r,y1)-target.camera_y);
      int ix2=(int)ceil(draw_gui_x(r,x2)-target.camera_x);
      int iy2=(int)ceil(draw_gui_y(r,y2)-target.camera_y);
      if(back){
        gml_render_primitive_rectangle(r,ix1,iy1,ix2,iy2,action_health_palette(back-1),0);
        gml_render_primitive_rectangle(r,ix1,iy1,ix2,iy2,0,1);
      }
      int fill_x2=(int)ceil(
        draw_gui_x(r,x1+(x2-x1)*ratio)-target.camera_x);
      gml_render_primitive_rectangle(r,ix1,iy1,fill_x2,iy2,fill,0);
      gml_render_primitive_rectangle(r,ix1,iy1,fill_x2,iy2,0,1);
    }
    return vreal(0); }
  if(!strcmp(nm,"action_draw_sprite")){ double x=N(a,n,1),y=N(a,n,2);
    action_relative_point(vm,&x,&y);
    GmlVal draw_args[4]={vreal(N(a,n,0)),vreal(N(a,n,3)),vreal(x),vreal(y)};
    return gml_builtin_call(vm,"draw_sprite",draw_args,4); }
  if(!strcmp(nm,"action_draw_variable")){ double x=N(a,n,1),y=N(a,n,2); action_relative_point(vm,&x,&y);
    GmlVal draw_args[3]={vreal(x),vreal(y),n>0?a[0]:vreal(0)};
    return gml_builtin_call(vm,"draw_text",draw_args,3); }
  if(!strcmp(nm,"action_draw_text")){
    double x=N(a,n,1),y=N(a,n,2); action_relative_point(vm,&x,&y);
    GmlRender *r=(GmlRender*)vm->render;
    if(r) gml_draw_text(r,x,y,S(vm,a,n,0));
    return vreal(0);
  }
  if(!strcmp(nm,"action_draw_life_images")){
    GmlRender *r=(GmlRender*)vm->render; GmlVal *value=gml_varmap_get(&vm->globals,"lives");
    int count=(int)floor(value&&value->t==V_REAL?value->d:0), sprite=(int)N(a,n,2);
    double x=N(a,n,0),y=N(a,n,1); action_relative_point(vm,&x,&y);
    GmlRenderSpriteMetrics metrics;
    int width=gml_render_sprite_metrics(r,sprite,&metrics)?metrics.width:0;
    if(r) for(int i=0;i<count;i++) gml_draw_sprite(r,sprite,0,x+i*width,y);
    return vreal(0);
  }
  if(!strcmp(nm,"action_set_cursor")){ vm->window_cursor=(int)N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"effect_create_above")||!strcmp(nm,"effect_create_below")){
    gml_effect_create(vm->particles,!strcmp(nm,"effect_create_above"),(int)N(a,n,0),N(a,n,1),N(a,n,2),(int)N(a,n,3),(uint32_t)N(a,n,4));
    return vreal(0);
  }
  if(!strcmp(nm,"action_effect")){
    double x=N(a,n,1),y=N(a,n,2); GmlInstance *s=vm->cur_self;
    if(N(a,n,5)!=0 && s){ x+=s->x; y+=s->y; }
    gml_effect_create(vm->particles,1,(int)N(a,n,0),x,y,(int)N(a,n,3),(uint32_t)N(a,n,4));
    return vreal(0);
  }
  if(!strcmp(nm,"action_wrap")){ GmlInstance *s=vm->cur_self; GmlRoom room; int dir=(int)N(a,n,0);
    if(s && gml_vm_room_get(vm,vm->room_index,&room)==0){
      if((dir==0||dir==2) && room.width>0){ while(s->x<0)s->x+=room.width; while(s->x>=room.width)s->x-=room.width; }
      if((dir==1||dir==2) && room.height>0){ while(s->y<0)s->y+=room.height; while(s->y>=room.height)s->y-=room.height; }
      gml_colgrid_touch(vm,s);
    } return vreal(0); }
  return gml_builtin_try_instances_queries(vm,nm,a,n);
}
