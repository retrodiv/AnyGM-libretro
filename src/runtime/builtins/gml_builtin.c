/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_builtin.c — GameMaker built-in function dispatch. */
#include "gml_vm.h"
#include "gml_render.h"
#include "gml_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o]|(uint32_t)d[o+1]<<8|(uint32_t)d[o+2]<<16|(uint32_t)d[o+3]<<24;
}
static double N(GmlVal *a, int n, int i){ return (i<n)? (a[i].t==V_REAL?a[i].d:(a[i].s?atof(a[i].s):0)) : 0; }
static const char *S(GmlVal *a, int n, int i){ return (i<n && a[i].t==V_STR)? a[i].s : ""; }

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

/* GM round(): round half to even (banker's). */
static double gm_round(double x){
  double f=floor(x), diff=x-f;
  if(diff<0.5) return f; if(diff>0.5) return f+1;
  return (fmod(f,2.0)==0.0)? f : f+1;
}
static double gm_sign(double x){ return x>0?1:(x<0?-1:0); }

/* hooks provided elsewhere */
extern int gml_input_key(int vk, int edge);       /* edge: 0=held,1=pressed,2=released; ret 0/1 */
extern int gml_input_gamepad(int button, int edge);

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

static int g_logged[1]; /* placeholder */

/* mask bbox (world coords) of an instance placed at (atx,aty), from its sprite margins. */
static int inst_bbox(GmlVM *vm, GmlInstance *in, double atx, double aty,
                     double *l, double *t, double *r, double *b){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return 0;
  int si=(int)in->sprite_index; if(si<0||si>=R->n_spr) return 0;
  GmlSprite *s=&R->spr[si]; if(s->mr<s->ml || s->mb<s->mt) return 0;
  *l = atx - s->originx + s->ml; *r = atx - s->originx + s->mr;
  *t = aty - s->originy + s->mt; *b = aty - s->originy + s->mb;
  return 1;
}
static int bbox_overlap(double l1,double t1,double r1,double b1, double l2,double t2,double r2,double b2){
  return l1<=r2 && l2<=r1 && t1<=b2 && t2<=b1;
}
/* per-pixel (precise) mask overlap of self placed at (sx,sy) vs o at its own position, scanned
 * over their bbox intersection. Lets the player walk up diagonal slopes whose bbox is a full
 * rectangle but whose pixel mask is triangular. Falls back to "overlap" if a sprite is missing. */
static int masks_overlap(GmlVM *vm, GmlInstance *self, double sx, double sy, GmlInstance *o){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return 1;
  int ss=(int)self->sprite_index, os=(int)o->sprite_index; if(ss<0||os<0) return 1;
  GmlSprite *sp=&R->spr[ss], *op=&R->spr[os];
  double sl,st,sr,sb,ol,ot,orr,ob;
  if(!inst_bbox(vm,self,sx,sy,&sl,&st,&sr,&sb)) return 0;
  if(!inst_bbox(vm,o,o->x,o->y,&ol,&ot,&orr,&ob)) return 0;
  int x0=(int)floor(fmax(sl,ol)), x1=(int)ceil(fmin(sr,orr));
  int y0=(int)floor(fmax(st,ot)), y1=(int)ceil(fmin(sb,ob));
  /* GM bbox coordinates are inclusive. A common grounding probe is place_meeting(x,y+1,...),
   * where the moved mask's bottom row exactly equals the wall's top row. The bbox precheck
   * treats that as overlap, so the mask scan must include that single edge row/column too. */
  if(x1==x0) x1++;
  if(y1==y0) y1++;
  double sox=sx-sp->originx, soy=sy-sp->originy;        /* world pos of each sprite's (0,0) */
  double oox=o->x-op->originx, ooy=o->y-op->originy;
  for(int wy=y0; wy<y1; wy++) for(int wx=x0; wx<x1; wx++){
    if(!gml_sprite_collision(R,ss,(int)self->image_index,(int)(wx-sox),(int)(wy-soy))) continue;
    if( gml_sprite_collision(R,os,(int)o->image_index,(int)(wx-oox),(int)(wy-ooy))) return 1;
  }
  return 0;
}
/* solid=1 -> test only solid instances (place_free); else test instances matching obj. */
static int collision_at(GmlVM *vm, double x, double y, int obj, int solid_only){
  GmlInstance *self=vm->cur_self; if(!self) return 0;
  double sl,st,sr,sb; if(!inst_bbox(vm,self,x,y,&sl,&st,&sr,&sb)) return 0;
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
    if(!o->active||o->marked||o==self) continue;
    if(solid_only){ if(o->solid<0.5) continue; }
    else if(!gml_object_is(vm,o->obj,obj)) continue;
    double ol,ot,orr,ob; if(!inst_bbox(vm,o,o->x,o->y,&ol,&ot,&orr,&ob)) continue;
    if(bbox_overlap(sl,st,sr,sb, ol,ot,orr,ob) && masks_overlap(vm,self,x,y,o)) return 1;
  }
  return 0;
}
static void snap_contact_axis(GmlVM *vm, GmlInstance *s, double dx, double dy){
  double nx=s->x, ny=s->y;
  if(fabs(dx)<1e-9 && fabs(dy)>0.999999)
    ny = dy>0 ? floor(s->y+1e-9) : ceil(s->y-1e-9);
  else if(fabs(dy)<1e-9 && fabs(dx)>0.999999)
    nx = dx>0 ? floor(s->x+1e-9) : ceil(s->x-1e-9);
  else
    return;
  if(!collision_at(vm,nx,ny,0,1)){ s->x=nx; s->y=ny; }
}
static int resolve_landing_overlap(GmlVM *vm, GmlInstance *s, int md){
  if(s->vspeed<=0) return 0;
  double ox=s->x, oy=s->y;
  int limit=md + (int)ceil(fabs(s->vspeed)) + 2;
  if(limit<1) limit=1;
  for(int k=0;k<=limit;k++){
    if(!collision_at(vm,s->x,s->y,0,1)){
      snap_contact_axis(vm,s,0,1);
      return 1;
    }
    s->y-=1.0;
  }
  s->x=ox; s->y=oy;
  return 0;
}

/* first active instance of `obj` whose mask covers world point (px,py), or NULL. */
static GmlInstance *instance_at_point(GmlVM *vm, double px, double py, int obj){
  GmlRender *R=(GmlRender*)vm->render;
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
    if(!o->active||o->marked||!gml_object_is(vm,o->obj,obj)) continue;
    double ol,ot,orr,ob; if(!inst_bbox(vm,o,o->x,o->y,&ol,&ot,&orr,&ob)) continue;
    if(px<ol||px>orr||py<ot||py>ob) continue;
    /* bbox hit; refine with the per-pixel mask when available */
    if(R){ int si=(int)o->sprite_index;
      if(si>=0 && si<R->n_spr){ GmlSprite *s=&R->spr[si];
        if(!gml_sprite_collision(R,si,(int)o->image_index,(int)(px-(o->x-s->originx)),(int)(py-(o->y-s->originy)))) continue; } }
    return o; }
  return NULL;
}

GmlVal gml_builtin_call(GmlVM *vm, const char *nm, GmlVal *a, int n){
  /* ---- collision ---- */
  if(!strcmp(nm,"place_meeting")) return vreal(collision_at(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2),0));
  if(!strcmp(nm,"place_free"))    return vreal(!collision_at(vm,N(a,n,0),N(a,n,1),0,1));
  if(!strcmp(nm,"place_empty"))   return vreal(!collision_at(vm,N(a,n,0),N(a,n,1),0,1));
  if(!strcmp(nm,"move_contact_solid")){ GmlInstance *s=vm->cur_self; if(!s) return vreal(0);
    double dir=N(a,n,0), md=N(a,n,1); if(md<0) md=1000;
    double dx=cos(dir*M_PI/180.0), dy=-sin(dir*M_PI/180.0);
    if(collision_at(vm,s->x,s->y,0,1)){
      if(resolve_landing_overlap(vm,s,(int)md)) return vreal(0);
      for(int k=0;k<(int)md;k++){ if(!collision_at(vm,s->x,s->y,0,1)) break; s->x-=dx; s->y-=dy; }
      snap_contact_axis(vm,s,dx,dy);
      return vreal(0);
    }
    for(int k=0;k<(int)md;k++){ if(collision_at(vm,s->x+dx,s->y+dy,0,1)) break; s->x+=dx; s->y+=dy; }
    return vreal(0); }
  /* ---- math ---- */
  if(!strcmp(nm,"floor")) return vreal(floor(N(a,n,0)));
  if(!strcmp(nm,"ceil"))  return vreal(ceil(N(a,n,0)));
  if(!strcmp(nm,"round")) return vreal(gm_round(N(a,n,0)));
  if(!strcmp(nm,"sign"))  return vreal(gm_sign(N(a,n,0)));
  if(!strcmp(nm,"abs"))   return vreal(fabs(N(a,n,0)));
  if(!strcmp(nm,"random")) return vreal(gml_rng_value(vm) * N(a,n,0));  /* GM WELL512: (next/2^32)*x */
  if(!strcmp(nm,"make_color_rgb"))
    return vreal((double)((int)N(a,n,0) + ((int)N(a,n,1)<<8) + ((int)N(a,n,2)<<16)));
  if(!strcmp(nm,"point_direction")){ double dx=N(a,n,2)-N(a,n,0), dy=N(a,n,3)-N(a,n,1);
    double r=atan2(-dy,dx)*180.0/M_PI; if(r<0)r+=360; return vreal(r); }
  if(!strcmp(nm,"distance_to_point")){ GmlInstance*s=vm->cur_self; if(!s)return vreal(0);
    return vreal(hypot(N(a,n,0)-s->x,N(a,n,1)-s->y)); }
  if(!strcmp(nm,"frac")) { double v=N(a,n,0); return vreal(v-floor(v)); }   /* fractional part */
  if(!strcmp(nm,"array_length_1d")) return vreal(n>0?gml_val_array_length(a[0]):0);
  if(!strcmp(nm,"lengthdir_x")) return vreal(N(a,n,0)*cos(N(a,n,1)*M_PI/180.0));
  if(!strcmp(nm,"lengthdir_y")) return vreal(-N(a,n,0)*sin(N(a,n,1)*M_PI/180.0));  /* GM y down */
  if(!strcmp(nm,"distance_to_object")){ GmlInstance*s=vm->cur_self; if(!s) return vreal(0);
    int obj=(int)N(a,n,0); double best=1e18;
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(o==s||!o->active||o->marked||!gml_object_is(vm,o->obj,obj)) continue;
      double d=hypot(o->x-s->x,o->y-s->y); if(d<best) best=d; }
    return vreal(best>1e17?-1:best); }
  /* move_towards_point(x,y,sp): head toward (x,y) at speed sp (sets direction+speed → hspeed/vspeed). */
  if(!strcmp(nm,"move_towards_point")){ GmlInstance*s=vm->cur_self; if(s){
      double dir=atan2(-(N(a,n,1)-s->y),N(a,n,0)-s->x)*180.0/M_PI; double sp=N(a,n,2);
      s->direction=dir; s->speed=sp; s->hspeed=sp*cos(dir*M_PI/180.0); s->vspeed=-sp*sin(dir*M_PI/180.0); }
    return vreal(0); }
  /* instance_nearest/furthest(x,y,obj): id of the nearest/furthest instance of obj (noone=-4). */
  if(!strcmp(nm,"instance_nearest")||!strcmp(nm,"instance_furthest")){
    int far=!strcmp(nm,"instance_furthest"); double px=N(a,n,0),py=N(a,n,1); int obj=(int)N(a,n,2);
    double best=far?-1:1e18; int bid=-4;
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(!o->active||o->marked||!gml_object_is(vm,o->obj,obj)) continue;
      double d=hypot(o->x-px,o->y-py); if((far&&d>best)||(!far&&d<best)){ best=d; bid=(int)o->id; } }
    return vreal(bid); }

  /* ---- strings ---- */
  if(!strcmp(nm,"string")){ if(n>0&&a[0].t==V_STR) return a[0];
    double v=N(a,n,0); char b[64]; if(v==floor(v)&&fabs(v)<1e15) snprintf(b,sizeof b,"%.0f",v); else snprintf(b,sizeof b,"%.2f",v);
    return vstr(strdup(b)); }
  if(!strcmp(nm,"string_length")) return vreal((double)strlen(S(a,n,0)));
  if(!strcmp(nm,"string_copy")){ const char*s=S(a,n,0); int idx=(int)N(a,n,1), cnt=(int)N(a,n,2);
    int len=strlen(s); if(idx<1)idx=1; if(idx>len)return vstr("");
    if(cnt<0)cnt=0; if(idx-1+cnt>len)cnt=len-(idx-1);
    char*o=malloc(cnt+1); memcpy(o,s+idx-1,cnt); o[cnt]=0; return vstr(o); }

  /* ---- ini persistence ---- */
  if(!strcmp(nm,"ini_open")){
    vm->ini_n=0; vm->ini_open=1; const char *fn=S(a,n,0);
    snprintf(vm->ini_path,sizeof vm->ini_path,"%s",fn);
    /* load existing .ini into the key-value table */
    FILE *f=fopen(fn,"r"); if(!f) return vreal(1);
    char line[512], *sec=NULL;
    while(fgets(line,sizeof line,f)){
      int len=strlen(line); while(len>0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
      if(line[0]=='['){ char *e=strchr(line,']'); if(e){ *e=0; free(sec); sec=strdup(line+1); } continue; }
      if(!sec) continue;
      char *eq=strchr(line,'='); if(!eq) continue; *eq=0;
      if(vm->ini_n<256){
        vm->ini_kv[vm->ini_n]=(typeof(vm->ini_kv[0])){.section=strdup(sec),.key=strdup(line),.val=atof(eq+1)};
        vm->ini_n++;
      }
    }
    fclose(f); /* sec is stored in ini_kv entries, do NOT free */
    return vreal(1);
  }
  if(!strcmp(nm,"ini_close")){
    if(!vm->ini_open) return vreal(0); vm->ini_open=0;
    /* write the current key-value table back to the .ini file */
    FILE *f=fopen(vm->ini_path,"w"); if(!f) return vreal(0);
    const char *last_sec=NULL;
    for(int i=0;i<vm->ini_n;i++){
      if(!last_sec||strcmp(vm->ini_kv[i].section,last_sec)){
        fprintf(f,"[%s]\n",vm->ini_kv[i].section); last_sec=vm->ini_kv[i].section;
      }
      fprintf(f,"%s=%g\n",vm->ini_kv[i].key,vm->ini_kv[i].val);
    }
    fclose(f);
    for(int i=0;i<vm->ini_n;i++){ free((void*)vm->ini_kv[i].section); free((void*)vm->ini_kv[i].key); }
    vm->ini_n=0;
    return vreal(0);
  }
  if(!strcmp(nm,"ini_write_real")){
    if(!vm->ini_open||vm->ini_n>=256) return vreal(0);
    const char *sec=S(a,n,0), *key=S(a,n,1); double val=N(a,n,2);
    /* replace existing key under the same section, or append */
    for(int i=0;i<vm->ini_n;i++)
      if(!strcmp(vm->ini_kv[i].section,sec) && !strcmp(vm->ini_kv[i].key,key)){
        vm->ini_kv[i].val=val; return vreal(0); }
    vm->ini_kv[vm->ini_n++]=(typeof(vm->ini_kv[0])){.section=strdup(sec),.key=strdup(key),.val=val};
    return vreal(0);
  }
  if(!strcmp(nm,"ini_read_real")){
    if(!vm->ini_open) return vreal(N(a,n,2));  /* default */
    const char *sec=S(a,n,0), *key=S(a,n,1); double def=N(a,n,2);
    /* search the key-value table backwards so later writes override */
    for(int i=vm->ini_n-1;i>=0;i--)
      if(!strcmp(vm->ini_kv[i].section,sec) && !strcmp(vm->ini_kv[i].key,key))
        return vreal(vm->ini_kv[i].val);
    return vreal(def);
  }

  /* ---- rooms / flow ---- */
  if(!strcmp(nm,"room_goto")){ vm->pending_room=(int)N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"room_goto_next")||!strcmp(nm,"room_next")){
    int cur = !strcmp(nm,"room_next")? (int)N(a,n,0) : vm->room_index;
    int pos=order_pos(vm,cur);
    int nxt = (pos>=0 && pos+1<vm->win->n_room_order)? (int)vm->win->room_order[pos+1] : cur;
    if(!strcmp(nm,"room_next")) return vreal(nxt);
    vm->pending_room=nxt; return vreal(0); }
  if(!strcmp(nm,"room_goto_previous")||!strcmp(nm,"room_previous")){
    int cur = !strcmp(nm,"room_previous")? (int)N(a,n,0) : vm->room_index;
    int pos=order_pos(vm,cur);
    int prv = (pos>0)? (int)vm->win->room_order[pos-1] : cur;
    if(!strcmp(nm,"room_previous")) return vreal(prv);
    vm->pending_room=prv; return vreal(0); }
  if(!strcmp(nm,"game_end")){ vm->game_end=1; return vreal(0); }
  if(!strcmp(nm,"game_restart")){ vm->game_end=2; return vreal(0); }

  /* ---- instances ---- */
  if(!strcmp(nm,"instance_create")){ GmlInstance*in=gml_instance_create(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2));
    return vreal(in?in->id:-4); }
  if(!strcmp(nm,"instance_destroy")){ if(vm->cur_self) gml_instance_destroy(vm,vm->cur_self); return vreal(0); }
  /* drag-and-drop actions (compiled to builtin calls) acting on the current instance */
  /* action_if(expr): D&D single-expression conditional. expr was evaluated on the stack by the
   * bytecode before this call → arg0 is the truth value. Returns the value (0 or 1). */
  if(!strcmp(nm,"action_if")) return vreal(N(a,n,0));
  /* action_if_variable(var, value, op): D&D variable comparison. op 0=equals, 1=lt, 2=gt. */
  if(!strcmp(nm,"action_if_variable")){
    GmlInstance*s=vm->cur_self; int op=(int)N(a,n,2);
    double vv=s?gml_inst_var_get(vm,s,S(a,n,0)):0;
    double cv=N(a,n,1); int r=0;
    switch(op){ case 0: r=(vv==cv); break; case 1: r=(vv<cv); break; case 2: r=(vv>cv); break; }
    return vreal(r); }
  if(!strcmp(nm,"action_reverse_xdir")){ if(vm->cur_self) vm->cur_self->hspeed=-vm->cur_self->hspeed; return vreal(0); }
  if(!strcmp(nm,"action_reverse_ydir")){ if(vm->cur_self) vm->cur_self->vspeed=-vm->cur_self->vspeed; return vreal(0); }
  if(!strcmp(nm,"action_kill_object")){ if(vm->cur_self) gml_instance_destroy(vm,vm->cur_self); return vreal(0); }
  if(!strcmp(nm,"instance_number")) return vreal(gml_instance_number(vm,(int)N(a,n,0)));
  /* Deactivated instances keep their slots with active=0 and deactivated=1;
   * step, draw and query guards skip them until reactivation. */
  if(!strcmp(nm,"instance_deactivate_object")){ int obj=(int)N(a,n,0);
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(o->active&&!o->marked&&gml_object_is(vm,o->obj,obj)){ o->active=0; o->deactivated=1; } } return vreal(0); }
  if(!strcmp(nm,"instance_deactivate_all")){ int notme=(int)N(a,n,0);
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(o->active&&!o->marked&&!(notme&&o==vm->cur_self)){ o->active=0; o->deactivated=1; } } return vreal(0); }
  if(!strcmp(nm,"instance_activate_object")){ int obj=(int)N(a,n,0);
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(o->deactivated&&gml_object_is(vm,o->obj,obj)){ o->active=1; o->deactivated=0; } } return vreal(0); }
  if(!strcmp(nm,"instance_activate_all")){
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(o->deactivated){ o->active=1; o->deactivated=0; } } return vreal(0); }
  if(!strcmp(nm,"instance_activate_region")||!strcmp(nm,"instance_deactivate_region")){
    double rx=N(a,n,0),ry=N(a,n,1),rw=N(a,n,2),rh=N(a,n,3); int inside=(int)N(a,n,4);
    int act=!strcmp(nm,"instance_activate_region");
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(act ? !o->deactivated : (!o->active||o->marked)) continue;
      int inreg=(o->x>=rx && o->x<=rx+rw && o->y>=ry && o->y<=ry+rh);
      if(inreg==(inside!=0)){ if(act){ o->active=1; o->deactivated=0; } else { o->active=0; o->deactivated=1; } } }
    return vreal(0); }
  if(!strcmp(nm,"instance_change")){ if(vm->cur_self){ vm->cur_self->obj=(int)N(a,n,0);
      if(N(a,n,1)>=0.5) gml_run_event(vm,vm->cur_self,"Create_0"); } return vreal(0); }
  /* instance_position(x,y,obj): id of the instance of obj whose mask covers (x,y), else noone(-4). */
  if(!strcmp(nm,"instance_position")){ GmlInstance *o=instance_at_point(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2));
    return vreal(o? (double)o->id : -4); }
  /* move_outside_solid(direction,maxdist): step the instance along `direction` until it no longer
   * meets a solid (or maxdist px reached) — un-sticks an instance spawned inside a wall/floor. */
  if(!strcmp(nm,"move_outside_solid")){ GmlInstance *s=vm->cur_self; if(s){
      double dir=N(a,n,0), md=N(a,n,1); if(md<1) md=1;
      double dx=cos(dir*M_PI/180.0), dy=-sin(dir*M_PI/180.0);
      for(int k=0;k<(int)md;k++){ if(!collision_at(vm,s->x,s->y,0,1)) break; s->x+=dx; s->y+=dy; } }
    return vreal(0); }
  /* action_set_alarm(value,index): D&D Set Alarm → self.alarm[index] = value. */
  if(!strcmp(nm,"action_set_alarm")){ GmlInstance *s=vm->cur_self; int idx=(int)N(a,n,1);
    if(s && idx>=0 && idx<GML_ALARMS) s->alarm[idx]=N(a,n,0); return vreal(0); }
  /* action_bounce(advanced,against): D&D Bounce. against 0=solid, 1=all. Non-advanced: reverse the
   * velocity component(s) whose next step meets a blocker, then relink direction/speed. */
  if(!strcmp(nm,"action_bounce")){ GmlInstance *s=vm->cur_self; if(s){
      int solid=(int)N(a,n,1)==0; double hs=s->hspeed, vs=s->vspeed;
      int bh = solid? collision_at(vm,s->x+hs,s->y,0,1) : 0;
      int bv = solid? collision_at(vm,s->x,s->y+vs,0,1) : 0;
      if(bh) s->hspeed=-hs; if(bv) s->vspeed=-vs;
      if(bh||bv){ s->speed=hypot(s->hspeed,s->vspeed);
        s->direction=atan2(-s->vspeed,s->hspeed)*180.0/M_PI; if(s->direction<0) s->direction+=360; } }
    return vreal(0); }

  /* ---- drawing ---- */
  { GmlRender *R=(GmlRender*)vm->render;
    if(!strcmp(nm,"draw_sprite")){ if(R) gml_draw_sprite(R,(int)N(a,n,0),(int)N(a,n,1),N(a,n,2),N(a,n,3)); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_ext")){ if(R) gml_draw_sprite_ext(R,(int)N(a,n,0),(int)N(a,n,1),N(a,n,2),N(a,n,3),
        N(a,n,4),N(a,n,5),N(a,n,6),(uint32_t)N(a,n,7),N(a,n,8)); return vreal(0); }
    /* draw_sprite_tiled(sprite,subimg,x,y) / _ext(...,xs,ys,color,alpha): tile a sprite to fill the screen */
    if(!strcmp(nm,"draw_sprite_tiled")){ if(R) gml_draw_sprite_tiled_ext(R,(int)N(a,n,0),(int)N(a,n,1),N(a,n,2),N(a,n,3),1,1,0xFFFFFF,1); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_tiled_ext")){ if(R) gml_draw_sprite_tiled_ext(R,(int)N(a,n,0),(int)N(a,n,1),N(a,n,2),N(a,n,3),
        N(a,n,4),N(a,n,5),(uint32_t)N(a,n,6),N(a,n,7)); return vreal(0); }
    if(!strcmp(nm,"draw_self")){ GmlInstance*s=vm->cur_self; if(R&&s) gml_draw_sprite_ext(R,(int)s->sprite_index,
        (int)s->image_index,s->x,s->y,s->image_xscale,s->image_yscale,s->image_angle,(uint32_t)s->image_blend,s->image_alpha); return vreal(0); }
    if(!strcmp(nm,"draw_set_color")){ if(R){ R->color=(uint32_t)N(a,n,0); } return vreal(0); }
    if(!strcmp(nm,"draw_set_alpha")){ if(R){ R->alpha=N(a,n,0); } return vreal(0); }
    if(!strcmp(nm,"draw_set_font")){ if(R){ R->font=(int)N(a,n,0); } return vreal(0); }
    if(!strcmp(nm,"draw_set_halign")){ if(R){ R->halign=(int)N(a,n,0); } return vreal(0); }
    if(!strcmp(nm,"draw_set_valign")){ if(R){ R->valign=(int)N(a,n,0); } return vreal(0); }
    if(!strcmp(nm,"draw_background")){ if(getenv("GML_LOG_BG")) fprintf(stderr,"[bg] draw_background(%d,%g,%g)\n",(int)N(a,n,0),N(a,n,1),N(a,n,2)); if(R) gml_draw_background(R,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
    if(!strcmp(nm,"draw_background_tiled")){ int bd=(int)N(a,n,0),ht=1,vt=1; bg_layer_tiling(vm,bd,&ht,&vt);
      if(getenv("GML_LOG_BG")) fprintf(stderr,"[bg] draw_background_tiled(%d,%g,%g) h=%d v=%d\n",bd,N(a,n,1),N(a,n,2),ht,vt);
      if(R) gml_draw_background_tiled(R,bd,N(a,n,1),N(a,n,2),ht,vt); return vreal(0); }
    /* _ext = +xscale,yscale,colour,alpha (background_ext also has a rotation arg before colour). */
    if(!strcmp(nm,"draw_background_tiled_ext")){ int bd=(int)N(a,n,0),ht=1,vt=1; bg_layer_tiling(vm,bd,&ht,&vt);
      if(getenv("GML_LOG_BGA")) fprintf(stderr,"[bga] background=%d x=%g y=%g alpha=%g\n",bd,N(a,n,1),N(a,n,2),N(a,n,6));
      if(R) gml_draw_background_tiled_ext(R,bd,N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),(uint32_t)N(a,n,5),N(a,n,6),ht,vt); return vreal(0); }
    if(!strcmp(nm,"draw_background_ext")){ if(R) gml_draw_background_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),(uint32_t)N(a,n,6),N(a,n,7)); return vreal(0); }
    /* Surface and GUI composition operations.
     * draw_surface_stretched(surf,x,y,w,h); _ext adds (col,alpha); draw_sprite_stretched(spr,sub,x,y,w,h). */
    if(!strcmp(nm,"draw_surface_stretched")){ if(R) gml_draw_surface_stretched(R,N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),0xFFFFFF,1); return vreal(0); }
    if(!strcmp(nm,"draw_surface_stretched_ext")){ if(R) gml_draw_surface_stretched(R,N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),(uint32_t)N(a,n,5),N(a,n,6)); return vreal(0); }
    if(!strcmp(nm,"draw_surface")){ if(R) gml_draw_surface_stretched(R,N(a,n,1),N(a,n,2),R->fbw,R->fbh,0xFFFFFF,1); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_stretched")){ if(R) gml_draw_sprite_stretched(R,(int)N(a,n,0),(int)N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),0xFFFFFF,1); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_stretched_ext")){ if(R) gml_draw_sprite_stretched(R,(int)N(a,n,0),(int)N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),(uint32_t)N(a,n,6),N(a,n,7)); return vreal(0); }
    if(!strcmp(nm,"application_surface")) return vreal(0);                /* the (only) surface id */
    if(!strcmp(nm,"surface_exists")) return vreal(1);
    if(!strcmp(nm,"application_surface_draw_enable")){ if(R) R->app_draw_enable=(int)N(a,n,0); return vreal(0); }
    if(!strcmp(nm,"display_get_width")||!strcmp(nm,"window_get_width")||!strcmp(nm,"surface_get_width")) return vreal(R?R->fbw:288);
    if(!strcmp(nm,"display_get_height")||!strcmp(nm,"window_get_height")||!strcmp(nm,"surface_get_height")) return vreal(R?R->fbh:216);
    if(!strcmp(nm,"draw_text")){ if(R) gml_draw_text(R,N(a,n,0),N(a,n,1),S(a,n,2)); return vreal(0); }
    if(!strcmp(nm,"font_add_sprite")) return vreal(R? gml_font_add_sprite(R,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),(int)N(a,n,3)) : 0);
  }

  /* ---- input (wired later; held/pressed/released) ---- */
  if(!strcmp(nm,"keyboard_check"))          return vreal(gml_input_key((int)N(a,n,0),0));
  if(!strcmp(nm,"keyboard_check_pressed"))  return vreal(gml_input_key((int)N(a,n,0),1));
  if(!strcmp(nm,"keyboard_check_released")) return vreal(gml_input_key((int)N(a,n,0),2));
  if(!strcmp(nm,"keyboard_check_direct"))   return vreal(gml_input_key((int)N(a,n,0),0));
  /* gamepad: device is arg0 (ignored, single pad), button/axis is arg1 */
  if(!strcmp(nm,"gamepad_button_check"))          return vreal(gml_input_gamepad((int)N(a,n,1),0));
  if(!strcmp(nm,"gamepad_button_check_pressed"))  return vreal(gml_input_gamepad((int)N(a,n,1),1));
  if(!strcmp(nm,"gamepad_button_check_released")) return vreal(gml_input_gamepad((int)N(a,n,1),2));
  if(!strcmp(nm,"gamepad_is_connected"))          return vreal(1);
  if(!strcmp(nm,"gamepad_get_device_count"))      return vreal(1);
  if(!strcmp(nm,"gamepad_axis_value")){ int ax=(int)N(a,n,1);  /* gp_axislh=32785, gp_axislv=32786 */
    if(ax==32785) return vreal(gml_input_gamepad(32784,0) - gml_input_gamepad(32783,0));  /* R - L */
    if(ax==32786) return vreal(gml_input_gamepad(32782,0) - gml_input_gamepad(32781,0));  /* D - U */
    return vreal(0); }
  if(!strncmp(nm,"gamepad_",8))             return vreal(0);

  /* ---- script_execute(scriptid, args...) ---- */
  if(!strcmp(nm,"script_execute")){ int ci=script_code_of(vm,(int)N(a,n,0));
    return gml_vm_run_code(vm,ci,vm->cur_self,vm->cur_other,a+1,n-1); }

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

  /* ---- audio ---- */
  { GmlAudio *AU=(GmlAudio*)vm->audio;
    if(!strcmp(nm,"audio_play_sound")||!strcmp(nm,"sound_play")){
      gml_audio_play(AU,(int)N(a,n,0),(int)N(a,n,2)); return vreal(0); }
    if(!strcmp(nm,"audio_stop_sound")||!strcmp(nm,"sound_stop")){ gml_audio_stop(AU,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"audio_stop_all")||!strcmp(nm,"sound_stop_all")){ gml_audio_stop_all(AU); return vreal(0); }
    if(!strcmp(nm,"audio_pause_all")){ gml_audio_pause_all(AU,1); return vreal(0); }
    if(!strcmp(nm,"audio_resume_all")){ gml_audio_pause_all(AU,0); return vreal(0); }
    if(!strcmp(nm,"audio_is_playing")||!strcmp(nm,"sound_isplaying")) return vreal(gml_audio_is_playing(AU,(int)N(a,n,0)));
  }

  /* ---- paths (path_start / path_end): instance follows a PATH each step ---- */
  if(!strcmp(nm,"path_start")){ if(vm->cur_self)
      gml_path_start(vm,vm->cur_self,(int)N(a,n,0),N(a,n,1),N(a,n,2),(int)N(a,n,3)); return vreal(0); }
  if(!strcmp(nm,"path_end")){ if(vm->cur_self) vm->cur_self->path_index=-1; return vreal(0); }

  /* ---- tile-layer manipulation (mutations applied to the room's tiles at draw) ---- */
  if(!strcmp(nm,"tile_layer_delete")){ gml_tile_layer_delete(vm,(int)N(a,n,0)); return vreal(0); }
  if(!strcmp(nm,"tile_layer_depth")){ gml_tile_layer_depth(vm,(int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
  if(!strcmp(nm,"tile_layer_shift")){ gml_tile_layer_shift(vm,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
  if(!strcmp(nm,"tile_layer_delete_at")){ gml_tile_layer_delete_at(vm,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }

  /* ---- draw / audio / misc: stubbed until the renderer/audio land ---- */
  if(!strncmp(nm,"draw_",5)||!strncmp(nm,"audio_",6)||!strncmp(nm,"path_",5)||
     !strncmp(nm,"place_",6)||!strncmp(nm,"move_",5)||!strncmp(nm,"tile_",5)||
     !strncmp(nm,"font_",5)||!strncmp(nm,"window_",7)||!strncmp(nm,"instance_",9))
    return vreal(0);

  /* ---- fallback: a user script called by name (gml_Script_<name>) ---- */
  char sn[160]; snprintf(sn,sizeof sn,"gml_Script_%s",nm);
  int ci=gml_code_index_by_name(vm->win,sn);
  if(ci>=0) return gml_vm_run_code(vm,ci,vm->cur_self,vm->cur_other,a,n);

  (void)g_logged;
  return vreal(0);
}
