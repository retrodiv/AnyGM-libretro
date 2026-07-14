/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_particle.c - Particle types, systems and emitters.
 * Static pools hold types, systems and emitters; each system owns its particles.
 * Updates apply velocity, gravity and lifetime increments. Drawing uses the
 * sprite renderer or clipped squares. Color and alpha interpolate over age.
 * State serialization includes the pools and the private random generator.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <limits.h>
#include "gml_render.h"
#include "gml_particle.h"
#include "gml_vm.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PT_MAX 256
#define PS_MAX 64
#define PE_MAX 128
#define DEG2RAD(d) ((d)*M_PI/180.0)

typedef struct {
  int used;
  int sprite, spr_animate, spr_stretch, spr_random;
  int shape;                                  /* pt_shape_* (unused for custom sprites) */
  double sz_min, sz_max, sz_incr, sz_wig;
  double xscale, yscale;
  double sp_min, sp_max, sp_incr, sp_wig;
  double dir_min, dir_max, dir_incr, dir_wig;
  double grav_amt, grav_dir;
  double life_min, life_max;
  uint32_t col[3]; int ncol;                  /* colour keys over life (GM order: BBGGRR) */
  int color_mode;                             /* 0=key colours, 1=random RGB, 2=random mix, 3=random HSV */
  uint32_t mix_a, mix_b;
  double cmin[3], cmax[3];                    /* RGB or HSV component ranges */
  double alpha[3]; int nalpha;
  double ori_min, ori_max, ori_incr, ori_wig; int ori_rel;
  int additive;
} PType;

typedef struct {
  int used;
  int sprite, spr_animate, spr_stretch, spr_random;
  int shape;
  double sz_min, sz_max, sz_incr, sz_wig;
  double xscale, yscale;
  double sp_min, sp_max, sp_incr, sp_wig;
  double dir_min, dir_max, dir_incr, dir_wig;
  double grav_amt, grav_dir;
  double life_min, life_max;
  uint32_t col[3]; int ncol;
  double alpha[3]; int nalpha;
  double ori_min, ori_max, ori_incr, ori_wig; int ori_rel;
  int additive;
} PTypeV1;

typedef struct {
  double x, y, speed, dir, grav_amt, grav_dir;
  double life, life0, size, size_incr, ori, ori_incr;
  int type;
  int has_col; uint32_t col_over;   /* part_particles_create_color override (else use the type's colour) */
} Part;

typedef struct {
  int used, auto_update, auto_draw;
  double depth;
  double px, py;
  Part *parts; int n, cap;
} PSys;

typedef struct {
  int used, sys;
  double xmin, xmax, ymin, ymax;
  int shape, dist;
  int stream_type, stream_number;
} PEmit;

typedef struct { int used, sys; double xmin, xmax, ymin, ymax; int shape, dist; } PEmitV1;

static void ptype_from_v1(PType *t, const PTypeV1 *o){
  memset(t,0,sizeof(*t));
  t->used=o->used;
  t->sprite=o->sprite; t->spr_animate=o->spr_animate; t->spr_stretch=o->spr_stretch; t->spr_random=o->spr_random;
  t->shape=o->shape;
  t->sz_min=o->sz_min; t->sz_max=o->sz_max; t->sz_incr=o->sz_incr; t->sz_wig=o->sz_wig;
  t->xscale=o->xscale; t->yscale=o->yscale;
  t->sp_min=o->sp_min; t->sp_max=o->sp_max; t->sp_incr=o->sp_incr; t->sp_wig=o->sp_wig;
  t->dir_min=o->dir_min; t->dir_max=o->dir_max; t->dir_incr=o->dir_incr; t->dir_wig=o->dir_wig;
  t->grav_amt=o->grav_amt; t->grav_dir=o->grav_dir;
  t->life_min=o->life_min; t->life_max=o->life_max;
  t->col[0]=o->col[0]; t->col[1]=o->col[1]; t->col[2]=o->col[2]; t->ncol=o->ncol;
  t->alpha[0]=o->alpha[0]; t->alpha[1]=o->alpha[1]; t->alpha[2]=o->alpha[2]; t->nalpha=o->nalpha;
  t->ori_min=o->ori_min; t->ori_max=o->ori_max; t->ori_incr=o->ori_incr; t->ori_wig=o->ori_wig; t->ori_rel=o->ori_rel;
  t->additive=o->additive;
}
static void pemit_from_v1(PEmit *e, const PEmitV1 *o){
  memset(e,0,sizeof(*e));
  e->used=o->used; e->sys=o->sys; e->xmin=o->xmin; e->xmax=o->xmax; e->ymin=o->ymin; e->ymax=o->ymax; e->shape=o->shape; e->dist=o->dist;
}

static PType g_pt[PT_MAX];
static PSys  g_ps[PS_MAX];
static PEmit g_pe[PE_MAX];
static int g_effect_sys[2];
static int g_effect_type[2][12][3];
static int g_effect_explosion_core[2][3];

static GmlVM *g_particle_vm;
static uint32_t g_prng = 0x2545F491u;
void gml_part_bind_vm(GmlVM *vm){ g_particle_vm=vm; }
static double prnd(void){
  if(g_particle_vm && g_particle_vm->win && g_particle_vm->win->classic_version)
    return gml_rng_value(g_particle_vm);
  g_prng = g_prng*1664525u + 1013904223u;
  return ((g_prng>>8) & 0xFFFFFF)/(double)0x1000000;
}
static double prnd_r(double a, double b){ return b>a ? a + prnd()*(b-a) : a; }
/* A fixed particle property is assigned directly; only a real interval samples the RNG. */
static double particle_range(double a,double b){ return b>a ? a+prnd()*(b-a) : a; }
static int clamp255(double v){ if(v<0) return 0; if(v>255) return 255; return (int)(v+0.5); }
static uint32_t rgb_col(int r,int g,int b){ return ((uint32_t)clamp255(b)<<16)|((uint32_t)clamp255(g)<<8)|(uint32_t)clamp255(r); }
static uint32_t mix_col(uint32_t a, uint32_t b, double t){
  if(t<0)t=0; if(t>1)t=1;
  double ar=a&0xFF, ag=(a>>8)&0xFF, ab=(a>>16)&0xFF;
  double br=b&0xFF, bg=(b>>8)&0xFF, bb=(b>>16)&0xFF;
  return rgb_col((int)(ar+(br-ar)*t+0.5),(int)(ag+(bg-ag)*t+0.5),(int)(ab+(bb-ab)*t+0.5));
}
static uint32_t hsv_col(double h,double s,double v){
  h=fmod(h,256.0); if(h<0) h+=256.0;
  s=clamp255(s)/255.0; v=clamp255(v)/255.0;
  double hp=h*(6.0/256.0), c=v*s, x=c*(1.0-fabs(fmod(hp,2.0)-1.0)), m=v-c;
  double r=0,g=0,b=0;
  if(hp<1){ r=c; g=x; }
  else if(hp<2){ r=x; g=c; }
  else if(hp<3){ g=c; b=x; }
  else if(hp<4){ g=x; b=c; }
  else if(hp<5){ r=x; b=c; }
  else { r=c; b=x; }
  return rgb_col((int)((r+m)*255.0+0.5),(int)((g+m)*255.0+0.5),(int)((b+m)*255.0+0.5));
}

static PType *pt(int id){ int i=id-1; return (i>=0 && i<PT_MAX && g_pt[i].used) ? &g_pt[i] : NULL; }
static PSys  *ps(int id){ int i=id-1; return (i>=0 && i<PS_MAX && g_ps[i].used) ? &g_ps[i] : NULL; }
static PEmit *pe(int id){ int i=id-1; return (i>=0 && i<PE_MAX && g_pe[i].used) ? &g_pe[i] : NULL; }

void gml_part_reset_all(void){
  for(int i=0;i<PS_MAX;i++){ free(g_ps[i].parts); }
  memset(g_pt,0,sizeof g_pt); memset(g_ps,0,sizeof g_ps); memset(g_pe,0,sizeof g_pe);
  memset(g_effect_sys,0,sizeof g_effect_sys); memset(g_effect_type,0,sizeof g_effect_type);
  memset(g_effect_explosion_core,0,sizeof g_effect_explosion_core);
  g_prng=0x2545F491u;
}

int gml_part_type_create(void){
  for(int i=0;i<PT_MAX;i++) if(!g_pt[i].used){
    PType *t=&g_pt[i]; memset(t,0,sizeof *t); t->used=1;
    t->sprite=-1; t->sz_min=t->sz_max=1; t->xscale=t->yscale=1;
    t->life_min=t->life_max=100; t->col[0]=0xFFFFFF; t->ncol=1; t->alpha[0]=1; t->nalpha=1;
    return i+1;
  }
  return 0;
}
int gml_part_type_exists(int id){ return pt(id)!=NULL; }
void gml_part_type_destroy(int id){ PType *t=pt(id); if(t){ t->used=0; } }
void gml_part_type_clear(int id){ PType *t=pt(id); if(t){ int u=t->used; memset(t,0,sizeof *t); t->used=u;
  t->sprite=-1; t->sz_min=t->sz_max=1; t->xscale=t->yscale=1; t->life_min=t->life_max=100; t->col[0]=0xFFFFFF; t->ncol=1; t->alpha[0]=1; t->nalpha=1; } }

void gml_part_type_sprite(int id,int spr,int animate,int stretch,int random){ PType *t=pt(id); if(t){ t->sprite=spr; t->spr_animate=animate; t->spr_stretch=stretch; t->spr_random=random; } }
void gml_part_type_shape(int id,int shape){ PType *t=pt(id); if(t) t->shape=shape; }
void gml_part_type_size(int id,double mn,double mx,double incr,double wig){ PType *t=pt(id); if(t){ t->sz_min=mn; t->sz_max=mx; t->sz_incr=incr; t->sz_wig=wig; } }
void gml_part_type_scale(int id,double xs,double ys){ PType *t=pt(id); if(t){ t->xscale=xs; t->yscale=ys; } }
void gml_part_type_speed(int id,double mn,double mx,double incr,double wig){ PType *t=pt(id); if(t){ t->sp_min=mn; t->sp_max=mx; t->sp_incr=incr; t->sp_wig=wig; } }
void gml_part_type_direction(int id,double mn,double mx,double incr,double wig){ PType *t=pt(id); if(t){ t->dir_min=mn; t->dir_max=mx; t->dir_incr=incr; t->dir_wig=wig; } }
void gml_part_type_gravity(int id,double amt,double dir){ PType *t=pt(id); if(t){ t->grav_amt=amt; t->grav_dir=dir; } }
void gml_part_type_life(int id,double mn,double mx){ PType *t=pt(id); if(t){ t->life_min=mn; t->life_max=mx; } }
void gml_part_type_orientation(int id,double mn,double mx,double incr,double wig,int rel){ PType *t=pt(id); if(t){ t->ori_min=mn; t->ori_max=mx; t->ori_incr=incr; t->ori_wig=wig; t->ori_rel=rel; } }
void gml_part_type_color(int id,int ncol,uint32_t c1,uint32_t c2,uint32_t c3){ PType *t=pt(id); if(t){ t->color_mode=0; t->ncol=ncol<1?1:(ncol>3?3:ncol); t->col[0]=c1; t->col[1]=c2; t->col[2]=c3; } }
void gml_part_type_color_rgb(int id,double rmin,double rmax,double gmin,double gmax,double bmin,double bmax){
  PType *t=pt(id); if(t){ t->color_mode=1; t->cmin[0]=rmin; t->cmax[0]=rmax; t->cmin[1]=gmin; t->cmax[1]=gmax; t->cmin[2]=bmin; t->cmax[2]=bmax; }
}
void gml_part_type_color_mix(int id,uint32_t c1,uint32_t c2){ PType *t=pt(id); if(t){ t->color_mode=2; t->mix_a=c1; t->mix_b=c2; } }
void gml_part_type_color_hsv(int id,double hmin,double hmax,double smin,double smax,double vmin,double vmax){
  PType *t=pt(id); if(t){ t->color_mode=3; t->cmin[0]=hmin; t->cmax[0]=hmax; t->cmin[1]=smin; t->cmax[1]=smax; t->cmin[2]=vmin; t->cmax[2]=vmax; }
}
void gml_part_type_alpha(int id,int na,double a1,double a2,double a3){ PType *t=pt(id); if(t){ t->nalpha=na<1?1:(na>3?3:na); t->alpha[0]=a1; t->alpha[1]=a2; t->alpha[2]=a3; } }
void gml_part_type_blend(int id,int additive){ PType *t=pt(id); if(t) t->additive=additive; }

int gml_part_system_create(void){
  for(int i=0;i<PS_MAX;i++) if(!g_ps[i].used){ PSys *s=&g_ps[i]; memset(s,0,sizeof *s); s->used=1; s->auto_update=1; s->auto_draw=1;
    if(getenv("GML_LOG_PART")) fprintf(stderr,"[part] system create %d\n",i+1);
    return i+1; }
  return 0;
}
int gml_part_system_exists(int id){ return ps(id)!=NULL; }
void gml_part_system_destroy(int id){ PSys *s=ps(id); if(s){ free(s->parts); memset(s,0,sizeof *s); } }
void gml_part_system_clear(int id){ PSys *s=ps(id); if(s) s->n=0; }
void gml_part_system_position(int id,double x,double y){ PSys *s=ps(id); if(s){ s->px=x; s->py=y; } }
void gml_part_system_automatic_update(int id,int on){ PSys *s=ps(id); if(s) s->auto_update=on?1:0; }
void gml_part_system_automatic_draw(int id,int on){ PSys *s=ps(id); if(s) s->auto_draw=on?1:0; }
void gml_part_system_depth(int id,double depth){ PSys *s=ps(id); if(s){ s->depth=depth;
  if(getenv("GML_LOG_PART")) fprintf(stderr,"[part] system %d depth %.0f\n",id,depth); } }
int  gml_part_system_count(int id){ PSys *s=ps(id); return s? s->n : 0; }
int  gml_part_system_auto_draw_nth(int nth,int *id,double *depth){
  if(nth<0) return 0;
  for(int i=0;i<PS_MAX;i++) if(g_ps[i].used && g_ps[i].auto_draw){
    if(nth--==0){
      if(id) *id=i+1;
      if(depth) *depth=g_ps[i].depth;
      return 1;
    }
  }
  return 0;
}

static void sys_spawn(PSys *s, double x, double y, int type, int number, int col){
  PType *t=pt(type); if(!t || number<=0) return;
  if(number>4000) number=4000;
  for(int k=0;k<number;k++){
    if(s->n>=s->cap){ int nc=s->cap? s->cap*2:64; Part *np=realloc(s->parts,(size_t)nc*sizeof(Part)); if(!np) return; s->parts=np; s->cap=nc; }
    Part *p=&s->parts[s->n++]; memset(p,0,sizeof *p);
    p->x=x; p->y=y; p->type=type;
    p->speed=particle_range(t->sp_min,t->sp_max);
    p->dir=particle_range(t->dir_min,t->dir_max);
    p->ori=particle_range(t->ori_min,t->ori_max); p->ori_incr=t->ori_incr;
    p->life=p->life0=floor(particle_range(t->life_min,t->life_max)+0.5); if(p->life<1) p->life=p->life0=1;
    if(t->color_mode==1){ p->has_col=1; p->col_over=rgb_col((int)floor(particle_range(t->cmin[0],t->cmax[0])+0.5),(int)floor(particle_range(t->cmin[1],t->cmax[1])+0.5),(int)floor(particle_range(t->cmin[2],t->cmax[2])+0.5)); }
    else if(t->color_mode==2){ p->has_col=1; p->col_over=mix_col(t->mix_a,t->mix_b,prnd()); }
    else if(t->color_mode==3){ p->has_col=1; p->col_over=hsv_col(particle_range(t->cmin[0],t->cmax[0]),particle_range(t->cmin[1],t->cmax[1]),particle_range(t->cmin[2],t->cmax[2])); }
    if(col>=0){ p->has_col=1; p->col_over=(uint32_t)col; }
    p->size=particle_range(t->sz_min,t->sz_max); p->size_incr=t->sz_incr;
    if(t->sprite>=0 && t->spr_random) (void)prnd();
    (void)prnd(); /* per-particle wiggle/animation phase */
    p->grav_amt=t->grav_amt; p->grav_dir=t->grav_dir;
  }
}
void gml_part_particles_create(int sysid,double x,double y,int type,int number){ PSys *s=ps(sysid); if(s) sys_spawn(s,x+s->px,y+s->py,type,number,-1); }
void gml_part_particles_create_color(int sysid,double x,double y,int type,uint32_t col,int number){ PSys *s=ps(sysid); if(s) sys_spawn(s,x+s->px,y+s->py,type,number,(int)(col&0xFFFFFF)); }

static void effect_room_metrics(int *width,int *height,int *speed){
  *width=640; *height=480; *speed=30;
  if(!g_particle_vm) return;
  GmlRoom room;
  if(g_particle_vm->win && gml_room_get(g_particle_vm->win,g_particle_vm->room_index,&room)==0){
    if(room.width>0) *width=room.width;
    if(room.height>0) *height=room.height;
    if(room.speed>0) *speed=room.speed;
  } else if(g_particle_vm->render){
    GmlRender *render=(GmlRender*)g_particle_vm->render;
    if(render->fbw>0) *width=render->fbw;
    if(render->fbh>0) *height=render->fbh;
  }
}

void gml_effect_create(int above,int kind,double x,double y,int size,uint32_t color){
  int layer=above?1:0; if(kind<0)kind=0; if(kind>11)kind=11; if(size<0)size=0; if(size>2)size=2;
  if(getenv("GML_LOG_PART"))
    fprintf(stderr,"[part] effect kind=%d pos=(%.1f,%.1f) size=%d color=%06x layer=%d\n",
            kind,x,y,size,color&0xFFFFFF,layer);
  if(!g_effect_sys[layer]){
    g_effect_sys[layer]=gml_part_system_create();
    gml_part_system_depth(g_effect_sys[layer],above?-100000.0:100000.0);
  }
  int type=g_effect_type[layer][kind][size];
  if(!type){
    type=gml_part_type_create(); if(!type) return;
    g_effect_type[layer][kind][size]=type;
    double scale=size==0?.8:(size==1?1.6:2.8);
    gml_part_type_shape(type,0);
    gml_part_type_size(type,scale,scale*1.8,kind==6||kind==7?-.03:.02,0);
    gml_part_type_life(type,kind==4||kind==5?35:18,kind==4||kind==5?60:34);
    gml_part_type_alpha(type,3,0.0,0.9,0.0);
    gml_part_type_direction(type,0,360,0,0);
    if(kind==4||kind==5){ gml_part_type_speed(type,.2*scale,1.0*scale,-.01,0); gml_part_type_gravity(type,.025,90); }
    else if(kind==10){ gml_part_type_speed(type,4*scale,7*scale,0,0); gml_part_type_direction(type,250,290,0,0); }
    else if(kind==11){ gml_part_type_speed(type,.3*scale,1.2*scale,0,0); gml_part_type_direction(type,240,300,0,0); }
    else gml_part_type_speed(type,.5*scale,2.5*scale,-.03,0);
  }
  if(kind==0){
    PType *burst=pt(type); if(!burst) return;
    int width,height,speed; effect_room_metrics(&width,&height,&speed);
    (void)width; (void)height;
    double cadence=fmax(30.0/speed,1.0);
    static const double initial_size[3]={.1,.3,.4};
    static const double size_growth[3]={.05,.10,.20};
    static const double initial_speed[3]={2,4,7};
    static const double speed_decay[3]={-.10,-.18,-.20};
    static const double life_min[3]={10,12,15};
    static const double life_max[3]={15,17,20};
    burst->sprite=-1; burst->shape=10;
    burst->sz_min=burst->sz_max=initial_size[size]; burst->sz_incr=size_growth[size]*cadence; burst->sz_wig=0;
    burst->xscale=burst->yscale=1;
    burst->sp_min=burst->sp_max=initial_speed[size]*cadence; burst->sp_incr=speed_decay[size]*cadence; burst->sp_wig=0;
    burst->dir_min=0; burst->dir_max=360; burst->dir_incr=burst->dir_wig=0;
    burst->grav_amt=0; burst->grav_dir=270;
    burst->life_min=floor(life_min[size]/cadence+.5); burst->life_max=floor(life_max[size]/cadence+.5);
    burst->alpha[0]=.6; burst->alpha[1]=.3; burst->alpha[2]=0; burst->nalpha=3;
    burst->ori_min=0; burst->ori_max=360; burst->ori_incr=burst->ori_wig=0; burst->ori_rel=0;
    burst->additive=0;

    int core_id=g_effect_explosion_core[layer][size];
    if(!core_id){ core_id=gml_part_type_create(); g_effect_explosion_core[layer][size]=core_id; }
    PType *core=pt(core_id); if(!core) return;
    static const double core_growth[3]={.10,.20,.40};
    static const double core_life[3]={15,17,20};
    core->sprite=-1; core->shape=10;
    core->sz_min=core->sz_max=initial_size[size]; core->sz_incr=core_growth[size]*cadence; core->sz_wig=0;
    core->xscale=core->yscale=1;
    core->sp_min=core->sp_max=core->sp_incr=core->sp_wig=0;
    core->dir_min=core->dir_max=core->dir_incr=core->dir_wig=0;
    core->grav_amt=0; core->grav_dir=270;
    core->life_min=core->life_max=floor(core_life[size]/cadence+.5);
    core->alpha[0]=.8; core->alpha[1]=.4; core->alpha[2]=0; core->nalpha=3;
    core->ori_min=0; core->ori_max=360; core->ori_incr=core->ori_wig=0; core->ori_rel=0;
    core->additive=0;
    gml_part_particles_create_color(g_effect_sys[layer],x,y,type,color,20);
    gml_part_particles_create_color(g_effect_sys[layer],x,y,core_id,0,1);
    return;
  }
  if(kind==1 || kind==2){
    PType *wave=pt(type); if(!wave) return;
    int width,height,speed; effect_room_metrics(&width,&height,&speed);
    (void)width; (void)height;
    double cadence=fmax(30.0/speed,1.0);
    static const double growth[2][3]={{.15,.25,.40},{.20,.35,.60}};
    static const double life_min[3]={10,13,18};
    static const double life_max[3]={12,15,20};
    wave->sprite=-1; wave->shape=6;
    wave->sz_min=wave->sz_max=0; wave->sz_incr=growth[kind-1][size]*cadence; wave->sz_wig=0;
    wave->xscale=1; wave->yscale=kind==2?.5:1;
    wave->sp_min=wave->sp_max=wave->sp_incr=wave->sp_wig=0;
    wave->dir_min=wave->dir_max=wave->dir_incr=wave->dir_wig=0;
    wave->grav_amt=0; wave->grav_dir=270;
    wave->life_min=floor(life_min[size]/cadence+.5);
    wave->life_max=floor(life_max[size]/cadence+.5);
    wave->alpha[0]=1; wave->alpha[1]=.5; wave->alpha[2]=0; wave->nalpha=3;
    wave->ori_min=wave->ori_max=wave->ori_incr=wave->ori_wig=0; wave->ori_rel=0;
    wave->additive=0;
    gml_part_particles_create_color(g_effect_sys[layer],x,y,type,color,1);
    return;
  }
  if(kind==3){
    PType *firework=pt(type); if(!firework) return;
    int width,height,speed; effect_room_metrics(&width,&height,&speed);
    (void)width; (void)height;
    double cadence=fmax(30.0/speed,1.0);
    static const double max_speed[3]={3,6,8};
    static const double life_min[3]={15,20,30};
    static const double life_max[3]={25,30,40};
    static const double gravity[3]={.10,.15,.17};
    static const int count[3]={75,150,250};
    firework->sprite=-1; firework->shape=8;
    firework->sz_min=.1; firework->sz_max=.2; firework->sz_incr=firework->sz_wig=0;
    firework->xscale=firework->yscale=1;
    firework->sp_min=.5*cadence; firework->sp_max=max_speed[size]*cadence;
    firework->sp_incr=firework->sp_wig=0;
    firework->dir_min=0; firework->dir_max=360; firework->dir_incr=firework->dir_wig=0;
    firework->grav_amt=gravity[size]; firework->grav_dir=270;
    firework->life_min=floor(life_min[size]/cadence+.5);
    firework->life_max=floor(life_max[size]/cadence+.5);
    firework->alpha[0]=1; firework->alpha[1]=.7; firework->alpha[2]=.4; firework->nalpha=3;
    firework->ori_min=firework->ori_max=firework->ori_incr=firework->ori_wig=0;
    firework->ori_rel=0; firework->additive=0;
    gml_part_particles_create_color(g_effect_sys[layer],x,y,type,color,count[size]);
    return;
  }
  if(kind==4 || kind==5){
    PType *smoke=pt(type); if(!smoke) return;
    int width,height,speed; effect_room_metrics(&width,&height,&speed);
    (void)width; (void)height;
    double cadence=fmax(30.0/speed,1.0);
    static const double min_size[3]={.2,.4,.4};
    static const double max_size[3]={.4,.7,1.0};
    static const double lifetime[3]={25,30,50};
    static const double rise_min[3]={3,5,6};
    static const double rise_max[3]={4,6,7};
    static const int count[3]={6,11,16};
    static const int spread[3]={10,30,60};
    smoke->sprite=-1; smoke->shape=10;
    smoke->sz_min=min_size[size]; smoke->sz_max=max_size[size];
    smoke->sz_incr=-.01*cadence; smoke->sz_wig=0;
    smoke->xscale=smoke->yscale=1;
    if(kind==5){
      smoke->sp_min=rise_min[size]*cadence; smoke->sp_max=rise_max[size]*cadence;
      smoke->dir_min=smoke->dir_max=90;
    } else {
      smoke->sp_min=smoke->sp_max=0;
      smoke->dir_min=smoke->dir_max=0;
    }
    smoke->sp_incr=smoke->sp_wig=0;
    smoke->dir_incr=smoke->dir_wig=0;
    smoke->grav_amt=0; smoke->grav_dir=270;
    smoke->life_min=smoke->life_max=fmax(1.0,floor(lifetime[size]/cadence+.5));
    smoke->alpha[0]=.4; smoke->alpha[1]=.2; smoke->alpha[2]=0; smoke->nalpha=3;
    smoke->ori_min=smoke->ori_max=smoke->ori_incr=smoke->ori_wig=0; smoke->ori_rel=0;
    smoke->additive=0;
    PSys *system=ps(g_effect_sys[layer]);
    int half=spread[size]/2;
    for(int i=0;system && i<count[size];i++){
      double dx=floor(prnd()*spread[size])-half;
      double dy=floor(prnd()*spread[size])-half;
      sys_spawn(system,x+dx,y+dy,type,1,(int)(color&0xFFFFFF));
    }
    return;
  }
  if(kind==6 || kind==7 || kind==8){
    PType *flash=pt(type); if(!flash) return;
    int width,height,speed; effect_room_metrics(&width,&height,&speed);
    (void)width; (void)height;
    double cadence=fmax(30.0/speed,1.0);
    static const double initial_size[3]={.4,.75,1.2};
    static const double shrink[3]={-.02,-.03,-.04};
    static const double lifetime[3]={20,25,30};
    flash->sprite=-1; flash->shape=kind==6?4:(kind==7?9:8);
    flash->sz_min=flash->sz_max=initial_size[size]; flash->sz_incr=shrink[size]*cadence; flash->sz_wig=0;
    flash->xscale=flash->yscale=1;
    flash->sp_min=flash->sp_max=flash->sp_incr=flash->sp_wig=0;
    flash->dir_min=flash->dir_max=flash->dir_incr=flash->dir_wig=0;
    flash->grav_amt=0; flash->grav_dir=270;
    flash->life_min=flash->life_max=floor(lifetime[size]/cadence+.5);
    flash->alpha[0]=flash->alpha[1]=flash->alpha[2]=1; flash->nalpha=3;
    flash->ori_min=0; flash->ori_max=360; flash->ori_incr=flash->ori_wig=0; flash->ori_rel=0;
    flash->additive=0;
    gml_part_particles_create_color(g_effect_sys[layer],x,y,type,color,1);
    return;
  }
  if(kind==9){
    PType *cloud=pt(type); if(!cloud) return;
    int width,height,speed; effect_room_metrics(&width,&height,&speed);
    (void)width; (void)height;
    double cadence=fmax(30.0/speed,1.0);
    static const double cloud_size[3]={2,4,8};
    cloud->sprite=-1; cloud->shape=10;
    cloud->sz_min=cloud->sz_max=cloud_size[size]; cloud->sz_incr=cloud->sz_wig=0;
    cloud->xscale=1; cloud->yscale=.5;
    cloud->sp_min=cloud->sp_max=cloud->sp_incr=cloud->sp_wig=0;
    cloud->dir_min=cloud->dir_max=cloud->dir_incr=cloud->dir_wig=0;
    cloud->grav_amt=0; cloud->grav_dir=270;
    cloud->life_min=cloud->life_max=fmax(1.0,floor(100.0/cadence+.5));
    cloud->alpha[0]=0; cloud->alpha[1]=.3; cloud->alpha[2]=0; cloud->nalpha=3;
    cloud->ori_min=cloud->ori_max=cloud->ori_incr=cloud->ori_wig=0; cloud->ori_rel=0;
    cloud->additive=0;
    gml_part_particles_create_color(g_effect_sys[layer],x,y,type,color,1);
    return;
  }
  if(kind==10){
    PType *rain=pt(type); if(!rain) return;
    int width,height,speed; effect_room_metrics(&width,&height,&speed);
    double cadence=fmax(30.0/speed,1.0);
    rain->shape=3;
    rain->sz_min=.2; rain->sz_max=.3; rain->sz_incr=rain->sz_wig=0;
    rain->sp_min=rain->sp_max=7*cadence; rain->sp_incr=rain->sp_wig=0;
    rain->dir_min=rain->dir_max=260; rain->dir_incr=rain->dir_wig=0;
    rain->ori_min=rain->ori_max=rain->ori_incr=rain->ori_wig=0; rain->ori_rel=1;
    rain->alpha[0]=rain->alpha[1]=rain->alpha[2]=.4; rain->nalpha=3;
    rain->life_min=rain->life_max=fmax(1.0,floor(.2*height/cadence+.5));
    int number=size==0?2:(size==1?5:9);
    PSys *system=ps(g_effect_sys[layer]);
    for(int i=0;system && i<number;i++)
      sys_spawn(system,prnd()*width*1.2,-30+floor(prnd()*20),type,1,(int)(color&0xFFFFFF));
    return;
  }
  int number=size==0?8:(size==1?16:28); if(kind==10||kind==11) number*=2;
  gml_part_particles_create_color(g_effect_sys[layer],x,y,type,color,number);
}

int  gml_part_emitter_create(int sysid){ (void)sysid; for(int i=0;i<PE_MAX;i++) if(!g_pe[i].used){ memset(&g_pe[i],0,sizeof g_pe[i]); g_pe[i].used=1; g_pe[i].sys=sysid; return i+1; } return 0; }
int  gml_part_emitter_exists(int sysid,int em){ PEmit *e=pe(em); return e && (sysid<=0 || e->sys==sysid); }
void gml_part_emitter_destroy(int em){ PEmit *e=pe(em); if(e) e->used=0; }
void gml_part_emitter_destroy_all(int sysid){ for(int i=0;i<PE_MAX;i++) if(g_pe[i].used && (sysid<=0 || g_pe[i].sys==sysid)) g_pe[i].used=0; }
void gml_part_emitter_clear(int sysid,int em){ PEmit *e=pe(em); if(e && (sysid<=0 || e->sys==sysid)){ int used=e->used, sys=e->sys; memset(e,0,sizeof(*e)); e->used=used; e->sys=sys; } }
void gml_part_emitter_region(int sysid,int em,double xmin,double xmax,double ymin,double ymax,int shape,int dist){ (void)sysid; PEmit *e=pe(em); if(e){ e->xmin=xmin; e->xmax=xmax; e->ymin=ymin; e->ymax=ymax; e->shape=shape; e->dist=dist; } }
static void emit_point(PEmit *e, double *ox, double *oy){
  /* Sample the bounding rectangle for every shape value. */
  *ox=prnd_r(e->xmin,e->xmax); *oy=prnd_r(e->ymin,e->ymax);
}
static void emitter_burst(PSys *s,PEmit *e,int type,int number){
  if(!s||!e||number<=0) return;
  if(number>4000) number=4000;
  for(int k=0;k<number;k++){ double x,y; emit_point(e,&x,&y); sys_spawn(s,x+s->px,y+s->py,type,1,-1); }
}
void gml_part_emitter_burst(int sysid,int em,int type,int number){ PSys *s=ps(sysid); PEmit *e=pe(em); emitter_burst(s,e,type,number); }
void gml_part_emitter_stream(int sysid,int em,int type,int number){ PEmit *e=pe(em); if(e && (sysid<=0 || e->sys==sysid)){ e->sys=sysid; e->stream_type=type; e->stream_number=number; } }

static void emit_streams(int sysid, PSys *s){
  for(int i=0;i<PE_MAX;i++){
    PEmit *e=&g_pe[i];
    if(!e->used || e->sys!=sysid || e->stream_number==0) continue;
    if(e->stream_number>0) emitter_burst(s,e,e->stream_type,e->stream_number);
    else { int den=-e->stream_number; if(den>0 && prnd() < 1.0/(double)den) emitter_burst(s,e,e->stream_type,1); }
  }
}

static void update_sys(int sysid, PSys *s){
  emit_streams(sysid,s);
  for(int i=0;i<s->n;){
    Part *p=&s->parts[i]; PType *t=pt(p->type);
    if(t){
      p->speed += t->sp_incr; if(p->speed<0) p->speed=0;
      p->dir += t->dir_incr; p->ori += p->ori_incr;
    }
    /* Step increments affect this step's velocity; gravity then adjusts its vector. */
    double vx=p->speed*cos(DEG2RAD(p->dir)), vy=-p->speed*sin(DEG2RAD(p->dir));
    if(p->grav_amt!=0){ vx += p->grav_amt*cos(DEG2RAD(p->grav_dir)); vy += -p->grav_amt*sin(DEG2RAD(p->grav_dir));
      p->speed=hypot(vx,vy); p->dir = atan2(-vy,vx)*180.0/M_PI; }
    p->x += vx; p->y += vy;
    if(t) p->size += p->size_incr;
    if(p->size<0) p->size=0;
    p->life -= 1;
    if(p->life<=0){ s->parts[i]=s->parts[--s->n]; continue; }   /* swap-remove */
    i++;
  }
}
void gml_part_system_update(int id){ PSys *s=ps(id); if(s) update_sys(id,s); }
void gml_part_update_all(void){ for(int i=0;i<PS_MAX;i++) if(g_ps[i].used && g_ps[i].auto_update) update_sys(i+1,&g_ps[i]); }

/* interpolate a channel over the particle's age (0 at birth → 1 at death) across up to 3 keys. */
static double keyf(double age, int nk, double k0, double k1, double k2){
  if(nk<=1) return k0;
  if(nk==2) return k0 + (k1-k0)*age;
  if(age<0.5) return k0 + (k1-k0)*(age*2.0);
  return k1 + (k2-k1)*((age-0.5)*2.0);
}
static uint32_t keyc(double age, PType *t){
  if(t->ncol<=1) return t->col[0];
  double r0=t->col[0]&0xFF,g0=(t->col[0]>>8)&0xFF,b0=(t->col[0]>>16)&0xFF;   /* GM BBGGRR */
  double r1=t->col[1]&0xFF,g1=(t->col[1]>>8)&0xFF,b1=(t->col[1]>>16)&0xFF;
  double r2=t->col[2]&0xFF,g2=(t->col[2]>>8)&0xFF,b2=(t->col[2]>>16)&0xFF;
  double r=keyf(age,t->ncol,r0,r1,r2), g=keyf(age,t->ncol,g0,g1,g2), b=keyf(age,t->ncol,b0,b1,b2);
  return ((uint32_t)(b+0.5)<<16)|((uint32_t)(g+0.5)<<8)|(uint32_t)(r+0.5);
}

/* plot a filled clipped square for shape/pixel particles (no custom sprite) */
static void plot_square(GmlRender *r, int cx, int cy, int half, uint32_t col, double a){
  if(!r||!r->fb||a<=0) return; if(a>1) a=1;
  int br=col&0xFF, bg=(col>>8)&0xFF, bb=(col>>16)&0xFF;   /* GM BBGGRR → r,g,b */
  int x0=cx-half, x1=cx+half+1, y0=cy-half, y1=cy+half+1;
  if(x0<0)x0=0; if(y0<0)y0=0; if(x1>r->fbw)x1=r->fbw; if(y1>r->fbh)y1=r->fbh;
  if(x1<=x0 || y1<=y0) return;
  int ia=(int)(a*255+0.5), iia=255-ia;
  uint32_t src=0xFF000000u|((uint32_t)br<<16)|((uint32_t)bg<<8)|(uint32_t)bb;
  if(ia>=255 || !r->alphablend){
    int n=x1-x0;
    for(int y=y0;y<y1;y++){ uint32_t *dp=r->fb+(size_t)y*r->fbw+x0;
      for(int x=0;x<n;x++) dp[x]=src; }
    return;
  }
  int pixels=(x1-x0)*(y1-y0);
  if(pixels>=64){
    uint8_t lr[256], lg[256], lb[256], la[256];
    for(int d=0; d<256; d++){
      lr[d]=(uint8_t)((br*ia+d*iia)/255);
      lg[d]=(uint8_t)((bg*ia+d*iia)/255);
      lb[d]=(uint8_t)((bb*ia+d*iia)/255);
      la[d]=(uint8_t)(ia+(d*iia)/255);
    }
    for(int y=y0;y<y1;y++){ uint32_t *dp=r->fb+(size_t)y*r->fbw;
      for(int x=x0;x<x1;x++){ uint32_t dv=dp[x];
        dp[x]=((uint32_t)la[(dv>>24)&0xFF]<<24)|((uint32_t)lr[(dv>>16)&0xFF]<<16)|
              ((uint32_t)lg[(dv>>8)&0xFF]<<8)|(uint32_t)lb[dv&0xFF]; } }
    return;
  }
  for(int y=y0;y<y1;y++){ uint32_t *dp=r->fb+(size_t)y*r->fbw;
    for(int x=x0;x<x1;x++){ uint32_t dv=dp[x];
      int dr=(dv>>16)&0xFF,dg=(dv>>8)&0xFF,db=dv&0xFF;
      int da=(dv>>24)&0xFF,oa=ia+(da*iia)/255;
      dp[x]=((uint32_t)oa<<24)|(((br*ia+dr*iia)/255)<<16)|
            (((bg*ia+dg*iia)/255)<<8)|((bb*ia+db*iia)/255); } }
}

static void plot_circle_shape(GmlRender *r,double cx,double cy,double xs,double ys,
                              uint32_t color,double alpha,int hollow){
  double rx=32.0*fabs(xs),ry=32.0*fabs(ys);
  if(!r || rx<0.25 || ry<0.25 || alpha<=0) return;
  int x0=(int)floor(cx-rx),x1=(int)ceil(cx+rx);
  int y0=(int)floor(cy-ry),y1=(int)ceil(cy+ry);
  if(x0<0)x0=0; if(y0<0)y0=0; if(x1>=r->fbw)x1=r->fbw-1; if(y1>=r->fbh)y1=r->fbh-1;
  for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
    double nx=(x+0.5-cx)/rx,ny=(y+0.5-cy)/ry,d=sqrt(nx*nx+ny*ny);
    double coverage;
    if(hollow){
      double thickness=fmax(1.5/fmin(rx,ry),0.10);
      coverage=1.0-fabs(d-0.78)/thickness;
    } else coverage=(1.0-d)*fmin(rx,ry);
    if(coverage<=0) continue;
    if(coverage>1) coverage=1;
    plot_square(r,x,y,0,color,alpha*coverage);
  }
}

static void plot_line_shape(GmlRender *r,double cx,double cy,double size,double angle,
                            uint32_t color,double alpha){
  /* The classic line primitive lives inside a 64x64 particle cell, but its
   * visible bar spans only the central 56 texels.  Treating the transparent
   * cell padding as line geometry makes small rain drops visibly too long.
   * The outer two texels at each end are a soft coverage ramp. */
  double length=56.0*fabs(size);
  if(!r || length<.5 || alpha<=0) return;
  double rad=DEG2RAD(angle), dx=cos(rad)*length, dy=-sin(rad)*length;
  int steps=(int)ceil(fmax(fabs(dx),fabs(dy))); if(steps<1) steps=1;
  double x0=cx-dx*.5,y0=cy-dy*.5;
  int last_x=INT_MIN,last_y=INT_MIN;
  for(int step=0;step<=steps;step++){
    int x=(int)floor(x0+dx*step/steps+.5),y=(int)floor(y0+dy*step/steps+.5);
    if(x==last_x && y==last_y) continue;
    double u=-28.0+56.0*step/steps;
    double coverage=(28.0-fabs(u))*.5;
    if(coverage>1) coverage=1;
    if(coverage>0) plot_square(r,x,y,0,color,alpha*coverage);
    last_x=x; last_y=y;
  }
}

/* The textured classic shapes occupy a 64x64 cell. Generate the soft,
 * irregular explosion field procedurally at startup. Keeping the field in a
 * small mask also avoids evaluating trigonometry for every live particle and
 * every frame. */
static uint8_t explosion_shape_mask[64*64];
static int explosion_shape_mask_ready;
static uint8_t glint_shape_mask[3][64*64];
static unsigned glint_shape_mask_ready;

static double unit_clamp(double v){ return v<0?0:(v>1?1:v); }
static double smooth_unit(double v){ v=unit_clamp(v); return v*v*(3.0-2.0*v); }

static double point_segment_distance(double px,double py,double ax,double ay,double bx,double by){
  double dx=bx-ax,dy=by-ay,den=dx*dx+dy*dy;
  double t=den>0?((px-ax)*dx+(py-ay)*dy)/den:0;
  if(t<0)t=0; else if(t>1)t=1;
  return hypot(px-(ax+t*dx),py-(ay+t*dy));
}

/* Generate the classic glint family procedurally inside transparent 64x64
 * particle cells: a faceted five-point star, a soft radial flare and a fine
 * multi-ray spark. */
static void prepare_glint_shape_mask(int shape){
  int slot=shape==4?0:(shape==8?1:2);
  unsigned bit=1u<<slot;
  if(glint_shape_mask_ready&bit) return;
  uint8_t *mask=glint_shape_mask[slot];
  for(int y=0;y<64;y++) for(int x=0;x<64;x++){
    double px=x+.5-32.0,py=y+.5-32.0;
    double radius=hypot(px,py),angle=atan2(py,px);
    double coverage=0;
    if(shape==4){
      double vx[10],vy[10];
      int inside=0;
      double edge=64;
      for(int i=0;i<10;i++){
        double a=-M_PI*.5+i*M_PI/5.0;
        double r=(i&1)?12.0:28.0;
        vx[i]=cos(a)*r; vy[i]=sin(a)*r;
      }
      for(int i=0,j=9;i<10;j=i++){
        if(((vy[i]>py)!=(vy[j]>py)) &&
           px<(vx[j]-vx[i])*(py-vy[i])/(vy[j]-vy[i])+vx[i]) inside=!inside;
        double d=point_segment_distance(px,py,vx[i],vy[i],vx[j],vy[j]);
        if(d<edge) edge=d;
      }
      double antialias=smooth_unit((inside?edge:-edge)+.75);
      double facets=.58+.42*(.5+.5*cos(angle*5.0+M_PI*.5));
      double centre=.72+.28*smooth_unit(radius/15.0);
      coverage=antialias*facets*centre;
    } else if(shape==8){
      double glow=exp(-(radius*radius)/(2.0*11.5*11.5))*.72;
      double core=smooth_unit((4.7-radius)*.55+.5);
      double major=exp(-pow(radius*fabs(sin(angle*4.0))/.85,2.0))
                  *smooth_unit((30.0-radius)/8.0);
      double minor=exp(-pow(radius*fabs(sin(angle*8.0))/.65,2.0))
                  *smooth_unit((23.0-radius)/7.0)*.45;
      double envelope=smooth_unit((31.5-radius)/3.0);
      coverage=fmax(core,fmax(glow,fmax(major*.70,minor)))*envelope;
    } else {
      double core=exp(-(radius*radius)/(2.0*3.2*3.2));
      double long_rays=exp(-pow(radius*fabs(sin(angle*8.0))/.42,2.0))
                      *smooth_unit((31.0-radius)/9.0);
      double fine_rays=exp(-pow(radius*fabs(sin(angle*12.0))/.32,2.0))
                      *smooth_unit((25.0-radius)/8.0)*.65;
      double irregular=.72+.28*(.5+.5*sin(angle*37.0+1.1));
      coverage=fmax(core,fmax(long_rays,fine_rays))*irregular;
    }
    mask[y*64+x]=(uint8_t)(255.0*unit_clamp(coverage)+.5);
  }
  glint_shape_mask_ready|=bit;
}

static void prepare_explosion_shape_mask(void){
  if(explosion_shape_mask_ready) return;
  for(int y=0;y<64;y++) for(int x=0;x<64;x++){
    double nx=(x+.5-32.0)/32.0,ny=(y+.5-32.0)/32.0;
    double radius=hypot(nx,ny),angle=atan2(ny,nx);
    double rim=.84 + .065*sin(angle*5.0+.4) + .045*sin(angle*9.0-1.1)
                    + .025*sin(angle*17.0+.8);
    double edge=smooth_unit((rim-radius)*4.0+.5);
    double grain=.80 + .12*sin(nx*13.0+ny*7.0+.6)
                       *sin(nx*5.0-ny*17.0-.3)
                       + .08*cos(nx*21.0+ny*11.0);
    double centre=.82+.18*smooth_unit(radius*3.0);
    explosion_shape_mask[y*64+x]=(uint8_t)(255.0*unit_clamp(edge*grain*centre*.92)+.5);
  }
  explosion_shape_mask_ready=1;
}

static double sample_explosion_shape(double x,double y){
  prepare_explosion_shape_mask();
  double tx=x+31.5,ty=y+31.5;
  if(tx<0 || ty<0 || tx>63 || ty>63) return 0;
  int x0=(int)floor(tx),y0=(int)floor(ty);
  int x1=x0<63?x0+1:x0,y1=y0<63?y0+1:y0;
  double fx=tx-x0,fy=ty-y0;
  double a=explosion_shape_mask[y0*64+x0];
  double b=explosion_shape_mask[y0*64+x1];
  double c=explosion_shape_mask[y1*64+x0];
  double d=explosion_shape_mask[y1*64+x1];
  return ((a+(b-a)*fx)*(1.0-fy)+(c+(d-c)*fx)*fy)/255.0;
}

static double sample_glint_shape(int shape,double x,double y){
  prepare_glint_shape_mask(shape);
  int slot=shape==4?0:(shape==8?1:2);
  const uint8_t *mask=glint_shape_mask[slot];
  double tx=x+31.5,ty=y+31.5;
  if(tx<0 || ty<0 || tx>63 || ty>63) return 0;
  int x0=(int)floor(tx),y0=(int)floor(ty);
  int x1=x0<63?x0+1:x0,y1=y0<63?y0+1:y0;
  double fx=tx-x0,fy=ty-y0;
  double a=mask[y0*64+x0],b=mask[y0*64+x1];
  double c=mask[y1*64+x0],d=mask[y1*64+x1];
  return ((a+(b-a)*fx)*(1.0-fy)+(c+(d-c)*fx)*fy)/255.0;
}

static void plot_explosion_shape(GmlRender *r,double cx,double cy,double xs,double ys,
                                 double angle,uint32_t color,double alpha){
  if(!r || !r->fb || fabs(xs)<1.0/128.0 || fabs(ys)<1.0/128.0 || alpha<=0) return;
  double rad=DEG2RAD(angle),co=cos(rad),si=sin(rad);
  double sx=32.0*fabs(xs),sy=32.0*fabs(ys);
  double ex=fabs(co)*sx+fabs(si)*sy,ey=fabs(si)*sx+fabs(co)*sy;
  int x0=(int)floor(cx-ex),x1=(int)ceil(cx+ex);
  int y0=(int)floor(cy-ey),y1=(int)ceil(cy+ey);
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>=r->fbw) x1=r->fbw-1;
  if(y1>=r->fbh) y1=r->fbh-1;
  for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
    double dx=x+.5-cx,dy=y+.5-cy;
    double lx=co*dx-si*dy,ly=si*dx+co*dy;
    double coverage=sample_explosion_shape(lx/xs,ly/ys);
    if(coverage>0) plot_square(r,x,y,0,color,alpha*coverage);
  }
}

static void plot_glint_shape(GmlRender *r,int shape,double cx,double cy,double xs,double ys,
                             double angle,uint32_t color,double alpha){
  if(!r || !r->fb || fabs(xs)<1.0/128.0 || fabs(ys)<1.0/128.0 || alpha<=0) return;
  double rad=DEG2RAD(angle),co=cos(rad),si=sin(rad);
  double sx=32.0*fabs(xs),sy=32.0*fabs(ys);
  double ex=fabs(co)*sx+fabs(si)*sy,ey=fabs(si)*sx+fabs(co)*sy;
  int x0=(int)floor(cx-ex),x1=(int)ceil(cx+ex);
  int y0=(int)floor(cy-ey),y1=(int)ceil(cy+ey);
  if(x0<0)x0=0;
  if(y0<0)y0=0;
  if(x1>=r->fbw)x1=r->fbw-1;
  if(y1>=r->fbh)y1=r->fbh-1;
  for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
    double dx=x+.5-cx,dy=y+.5-cy;
    double lx=co*dx-si*dy,ly=si*dx+co*dy;
    double coverage=sample_glint_shape(shape,lx/xs,ly/ys);
    if(coverage>0) plot_square(r,x,y,0,color,alpha*coverage);
  }
}

void gml_part_system_drawit(GmlRender *r, int id){
  PSys *s=ps(id); if(!s||!r) return;
  for(int i=0;i<s->n;i++){ Part *p=&s->parts[i]; PType *t=pt(p->type); if(!t) continue;
    double age = p->life0>0 ? (p->life0-p->life)/p->life0 : 0; if(age<0)age=0; if(age>1)age=1;
    if(t->sprite>=0){
      int frames=gml_sprite_frames(r,t->sprite); int sub = (t->spr_animate&&frames>0)? (int)(age*frames)%frames : 0;
      double xs=p->size*t->xscale, ys=p->size*t->yscale;
      if(t->sprite<r->n_spr && frames>0){
        GmlSprite *spr=&r->spr[t->sprite];
        double ax=fabs(xs), ay=fabs(ys);
        double rx=fmax((double)spr->originx,(double)(spr->w-spr->originx))*ax;
        double ry=fmax((double)spr->originy,(double)(spr->h-spr->originy))*ay;
        double rad=hypot(rx,ry) + 2.0;
        double sx=p->x-r->cam_x, sy=p->y-r->cam_y;
        if(sx+rad<0 || sy+rad<0 || sx-rad>=r->fbw || sy-rad>=r->fbh) continue;
      }
      double alpha=keyf(age,t->nalpha,t->alpha[0],t->alpha[1],t->alpha[2]);
      if(alpha<=0) continue;
      uint32_t col = p->has_col ? p->col_over : keyc(age,t);
      /* gml_draw_sprite_ext applies the camera itself → pass world (x,y), NOT camera-relative. */
      gml_draw_sprite_ext(r,t->sprite,sub, p->x, p->y,
                          xs, ys, p->ori, col, alpha);
    } else {
      int half=(int)(p->size)+0; if(half<0)half=0; if(half>64)half=64;
      int cx=(int)(p->x - r->cam_x), cy=(int)(p->y - r->cam_y);
      double shape_rx=(t->shape==1||t->shape==5||t->shape==6||t->shape==7)?
        32.0*fabs(p->size*t->xscale):half;
      double shape_ry=(t->shape==1||t->shape==5||t->shape==6||t->shape==7)?
        32.0*fabs(p->size*t->yscale):half;
      if(t->shape==4 || t->shape==8 || t->shape==9 || t->shape==10){
        shape_rx=shape_ry=32.0*hypot(p->size*t->xscale,p->size*t->yscale);
      }
      if(cx+shape_rx<0 || cy+shape_ry<0 || cx-shape_rx>=r->fbw || cy-shape_ry>=r->fbh) continue;
      double alpha=keyf(age,t->nalpha,t->alpha[0],t->alpha[1],t->alpha[2]);
      if(alpha<=0) continue;
      uint32_t col = p->has_col ? p->col_over : keyc(age,t);
      gml_render_maybe_prepare_draw(r);
      if(t->shape==1 || t->shape==5 || t->shape==6 || t->shape==7){
        plot_circle_shape(r,p->x-r->cam_x,p->y-r->cam_y,
                          p->size*t->xscale,p->size*t->yscale,col,alpha,t->shape==5||t->shape==6);
      } else if(t->shape==3){
        double angle=p->ori+(t->ori_rel?p->dir:0);
        plot_line_shape(r,p->x-r->cam_x,p->y-r->cam_y,p->size,angle,col,alpha);
      } else if(t->shape==4 || t->shape==8 || t->shape==9){
        double angle=p->ori+(t->ori_rel?p->dir:0);
        plot_glint_shape(r,t->shape,p->x-r->cam_x,p->y-r->cam_y,
                         p->size*t->xscale,p->size*t->yscale,angle,col,alpha);
      } else if(t->shape==10){
        double angle=p->ori+(t->ori_rel?p->dir:0);
        plot_explosion_shape(r,p->x-r->cam_x,p->y-r->cam_y,
                             p->size*t->xscale,p->size*t->yscale,angle,col,alpha);
      } else if(!gml_d3_draw_rectangle_2d(r,p->x-half,p->y-half,p->x+half+1,p->y+half+1,col,alpha,0))
        plot_square(r,cx,cy,half,col,alpha);
    }
  }
}
void gml_part_system_draw_all(GmlRender *r){ for(int i=0;i<PS_MAX;i++) if(g_ps[i].used && g_ps[i].auto_draw) gml_part_system_drawit(r,i+1); }

typedef struct { uint8_t *data; size_t cap, pos; int ok; } PartW;
typedef struct { const uint8_t *data; size_t cap, pos; int ok; } PartR;

static void pw_raw(PartW *w, const void *p, size_t n){
  if(w->data){
    if(w->pos+n<=w->cap) memcpy(w->data+w->pos,p,n);
    else w->ok=0;
  }
  w->pos+=n;
}
static void pr_raw(PartR *r, void *p, size_t n){
  if(r->pos+n<=r->cap) memcpy(p,r->data+r->pos,n);
  else { memset(p,0,n); r->ok=0; }
  r->pos+=n;
}
static void pw_u32(PartW *w, uint32_t v){ pw_raw(w,&v,sizeof(v)); }
static void pw_i32(PartW *w, int v){ int32_t x=(int32_t)v; pw_raw(w,&x,sizeof(x)); }
static void pw_d(PartW *w, double v){ pw_raw(w,&v,sizeof(v)); }
static uint32_t pr_u32(PartR *r){ uint32_t v=0; pr_raw(r,&v,sizeof(v)); return v; }
static int pr_i32(PartR *r){ int32_t v=0; pr_raw(r,&v,sizeof(v)); return (int)v; }
static double pr_d(PartR *r){ double v=0; pr_raw(r,&v,sizeof(v)); return v; }

static void part_state_write(PartW *w){
  pw_u32(w,0x32545250u); /* PTR2: PTR1 + particle colour modes + emitter stream state */
  pw_u32(w,g_prng);
  int nt=0; for(int i=0;i<PT_MAX;i++) if(g_pt[i].used) nt++;
  pw_i32(w,nt);
  for(int i=0;i<PT_MAX;i++) if(g_pt[i].used){ pw_i32(w,i+1); pw_raw(w,&g_pt[i],sizeof(g_pt[i])); }
  int ns=0; for(int i=0;i<PS_MAX;i++) if(g_ps[i].used) ns++;
  pw_i32(w,ns);
  for(int i=0;i<PS_MAX;i++) if(g_ps[i].used){
    PSys *s=&g_ps[i];
    pw_i32(w,i+1); pw_i32(w,s->auto_update); pw_i32(w,s->auto_draw);
    pw_d(w,s->depth); pw_d(w,s->px); pw_d(w,s->py);
    pw_i32(w,s->n);
    if(s->n>0) pw_raw(w,s->parts,(size_t)s->n*sizeof(Part));
  }
  int ne=0; for(int i=0;i<PE_MAX;i++) if(g_pe[i].used) ne++;
  pw_i32(w,ne);
  for(int i=0;i<PE_MAX;i++) if(g_pe[i].used){ pw_i32(w,i+1); pw_raw(w,&g_pe[i],sizeof(g_pe[i])); }
}

size_t gml_part_state_size(void){
  PartW w={0}; w.ok=1; part_state_write(&w); return w.pos;
}
int gml_part_state_save(void *data, size_t len, size_t *written){
  PartW w={(uint8_t*)data,len,0,1};
  part_state_write(&w);
  if(written) *written=w.pos;
  return w.ok && w.pos<=len;
}
int gml_part_state_load(const void *data, size_t len, size_t *used){
  PartR r={(const uint8_t*)data,len,0,1};
  uint32_t magic=pr_u32(&r);
  int v2=(magic==0x32545250u), v1=(magic==0x31545250u);
  if(!v1 && !v2){ if(used) *used=r.pos; return 0; }
  gml_part_reset_all();
  g_prng=pr_u32(&r);
  int nt=pr_i32(&r);
  if(nt<0 || nt>PT_MAX) r.ok=0;
  for(int k=0;k<nt;k++){
    int id=pr_i32(&r);
    PType tmp; memset(&tmp,0,sizeof(tmp));
    if(v2) pr_raw(&r,&tmp,sizeof(tmp));
    else { PTypeV1 old; memset(&old,0,sizeof(old)); pr_raw(&r,&old,sizeof(old)); ptype_from_v1(&tmp,&old); }
    if(id>=1 && id<=PT_MAX){ g_pt[id-1]=tmp; g_pt[id-1].used=1; }
  }
  int ns=pr_i32(&r);
  if(ns<0 || ns>PS_MAX) r.ok=0;
  for(int k=0;k<ns;k++){
    int id=pr_i32(&r);
    int au=pr_i32(&r), ad=pr_i32(&r);
    double depth=pr_d(&r), px=pr_d(&r), py=pr_d(&r);
    int n=pr_i32(&r);
    if(n<0 || n>200000){ r.ok=0; n=0; }
    size_t bytes=(size_t)n*sizeof(Part);
    Part *parts=n?calloc((size_t)n,sizeof(Part)):NULL;
    if(n && !parts){
      r.ok=0;
      if(r.pos+bytes<=r.cap) r.pos+=bytes; else { r.pos+=bytes; r.ok=0; }
      n=0;
    } else if(n) {
      pr_raw(&r,parts,bytes);
    }
    if(id>=1 && id<=PS_MAX){
      PSys *s=&g_ps[id-1]; memset(s,0,sizeof(*s));
      s->used=1; s->auto_update=au?1:0; s->auto_draw=ad?1:0;
      s->depth=depth; s->px=px; s->py=py; s->parts=parts; s->n=s->cap=n;
      parts=NULL;
    }
    free(parts);
  }
  int ne=pr_i32(&r);
  if(ne<0 || ne>PE_MAX) r.ok=0;
  for(int k=0;k<ne;k++){
    int id=pr_i32(&r);
    PEmit tmp; memset(&tmp,0,sizeof(tmp));
    if(v2) pr_raw(&r,&tmp,sizeof(tmp));
    else { PEmitV1 old; memset(&old,0,sizeof(old)); pr_raw(&r,&old,sizeof(old)); pemit_from_v1(&tmp,&old); }
    if(id>=1 && id<=PE_MAX){ g_pe[id-1]=tmp; g_pe[id-1].used=1; }
  }
  if(used) *used=r.pos;
  return r.ok && r.pos<=len;
}
