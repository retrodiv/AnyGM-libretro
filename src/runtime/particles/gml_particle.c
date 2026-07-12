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
static double particle_range(double a,double b){ return a+prnd()*(b-a); }
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

void gml_effect_create(int above,int kind,double x,double y,int size,uint32_t color){
  int layer=above?1:0; if(kind<0)kind=0; if(kind>11)kind=11; if(size<0)size=0; if(size>2)size=2;
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
    /* velocity from speed+direction, then add gravity, then move */
    double vx=p->speed*cos(DEG2RAD(p->dir)), vy=-p->speed*sin(DEG2RAD(p->dir));
    if(p->grav_amt!=0){ vx += p->grav_amt*cos(DEG2RAD(p->grav_dir)); vy += -p->grav_amt*sin(DEG2RAD(p->grav_dir));
      p->speed=hypot(vx,vy); p->dir = atan2(-vy,vx)*180.0/M_PI; }
    p->x += vx; p->y += vy;
    if(t){ p->speed += t->sp_incr; p->dir += t->dir_incr; p->size += p->size_incr; p->ori += p->ori_incr; }
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
  uint32_t src=((uint32_t)br<<16)|((uint32_t)bg<<8)|(uint32_t)bb;
  if(ia>=255 || !r->alphablend){
    int n=x1-x0;
    for(int y=y0;y<y1;y++){ uint32_t *dp=r->fb+(size_t)y*r->fbw+x0;
      for(int x=0;x<n;x++) dp[x]=src; }
    return;
  }
  int pixels=(x1-x0)*(y1-y0);
  if(pixels>=64){
    uint8_t lr[256], lg[256], lb[256];
    for(int d=0; d<256; d++){
      lr[d]=(uint8_t)((br*ia+d*iia)/255);
      lg[d]=(uint8_t)((bg*ia+d*iia)/255);
      lb[d]=(uint8_t)((bb*ia+d*iia)/255);
    }
    for(int y=y0;y<y1;y++){ uint32_t *dp=r->fb+(size_t)y*r->fbw;
      for(int x=x0;x<x1;x++){ uint32_t dv=dp[x];
        dp[x]=((uint32_t)lr[(dv>>16)&0xFF]<<16)|((uint32_t)lg[(dv>>8)&0xFF]<<8)|(uint32_t)lb[dv&0xFF]; } }
    return;
  }
  for(int y=y0;y<y1;y++){ uint32_t *dp=r->fb+(size_t)y*r->fbw;
    for(int x=x0;x<x1;x++){ uint32_t dv=dp[x];
      int dr=(dv>>16)&0xFF,dg=(dv>>8)&0xFF,db=dv&0xFF;
      dp[x]=(((br*ia+dr*iia)/255)<<16)|(((bg*ia+dg*iia)/255)<<8)|((bb*ia+db*iia)/255); } }
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
      if(cx+shape_rx<0 || cy+shape_ry<0 || cx-shape_rx>=r->fbw || cy-shape_ry>=r->fbh) continue;
      double alpha=keyf(age,t->nalpha,t->alpha[0],t->alpha[1],t->alpha[2]);
      if(alpha<=0) continue;
      uint32_t col = p->has_col ? p->col_over : keyc(age,t);
      gml_render_maybe_prepare_draw(r);
      if(t->shape==1 || t->shape==5 || t->shape==6 || t->shape==7){
        plot_circle_shape(r,p->x-r->cam_x,p->y-r->cam_y,
                          p->size*t->xscale,p->size*t->yscale,col,alpha,t->shape==5||t->shape==6);
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
