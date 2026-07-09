/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_assets.h"
#include "gmlc_json.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int add_sprite(GmlcProject *p, GmlcSprite *s){
  if(p->n_sprites>=p->cap_sprites){
    int nc=p->cap_sprites?p->cap_sprites*2:32;
    GmlcSprite *nv=(GmlcSprite*)realloc(p->sprites,(size_t)nc*sizeof(*nv));
    if(!nv) return 0;
    p->sprites=nv; p->cap_sprites=nc;
  }
  p->sprites[p->n_sprites++]=*s;
  return 1;
}

static int add_sound(GmlcProject *p, GmlcSound *s){
  if(p->n_sounds>=p->cap_sounds){
    int nc=p->cap_sounds?p->cap_sounds*2:32;
    GmlcSound *nv=(GmlcSound*)realloc(p->sounds,(size_t)nc*sizeof(*nv));
    if(!nv) return 0;
    p->sounds=nv; p->cap_sounds=nc;
  }
  p->sounds[p->n_sounds++]=*s;
  return 1;
}

static int add_script(GmlcProject *p, GmlcScript *s){
  if(p->n_scripts>=p->cap_scripts){
    int nc=p->cap_scripts?p->cap_scripts*2:32;
    GmlcScript *nv=(GmlcScript*)realloc(p->scripts,(size_t)nc*sizeof(*nv));
    if(!nv) return 0;
    p->scripts=nv; p->cap_scripts=nc;
  }
  p->scripts[p->n_scripts++]=*s;
  return 1;
}

static int add_object(GmlcProject *p, GmlcObject *o){
  if(p->n_objects>=p->cap_objects){
    int nc=p->cap_objects?p->cap_objects*2:32;
    GmlcObject *nv=(GmlcObject*)realloc(p->objects,(size_t)nc*sizeof(*nv));
    if(!nv) return 0;
    p->objects=nv; p->cap_objects=nc;
  }
  p->objects[p->n_objects++]=*o;
  return 1;
}

static int object_add_event(GmlcObject *o, GmlcObjectEvent *ev){
  if(o->n_events>=o->cap_events){
    int nc=o->cap_events?o->cap_events*2:8;
    GmlcObjectEvent *nv=(GmlcObjectEvent*)realloc(o->events,(size_t)nc*sizeof(*nv));
    if(!nv) return 0;
    o->events=nv; o->cap_events=nc;
  }
  o->events[o->n_events++]=*ev;
  return 1;
}

static int add_room(GmlcProject *p, GmlcRoom *r){
  if(p->n_rooms>=p->cap_rooms){
    int nc=p->cap_rooms?p->cap_rooms*2:16;
    GmlcRoom *nv=(GmlcRoom*)realloc(p->rooms,(size_t)nc*sizeof(*nv));
    if(!nv) return 0;
    p->rooms=nv; p->cap_rooms=nc;
  }
  p->rooms[p->n_rooms++]=*r;
  return 1;
}

static int room_add_instance(GmlcRoom *r, GmlcRoomInstance *in){
  if(r->n_instances>=r->cap_instances){
    int nc=r->cap_instances?r->cap_instances*2:32;
    GmlcRoomInstance *nv=(GmlcRoomInstance*)realloc(r->instances,(size_t)nc*sizeof(*nv));
    if(!nv) return 0;
    r->instances=nv; r->cap_instances=nc;
  }
  r->instances[r->n_instances++]=*in;
  return 1;
}

static int parse_u32_color(const GmlcJson *v, uint32_t fallback){
  const GmlcJson *vv=gmlc_json_obj(v,"Value");
  if(vv) return (uint32_t)gmlc_json_num(vv,(double)fallback);
  return fallback;
}

static void scan_instances(GmlcProject *p, GmlcRoom *r, const GmlcJson *layers, int *next_id){
  if(!layers || layers->type!=GMLC_JSON_ARRAY) return;
  for(const GmlcJson *ly=layers->child;ly;ly=ly->next){
    const GmlcJson *sub=gmlc_json_obj(ly,"layers");
    if(sub) scan_instances(p,r,sub,next_id);
    const GmlcJson *arr=gmlc_json_obj(ly,"instances");
    if(!arr || arr->type!=GMLC_JSON_ARRAY) continue;
    for(const GmlcJson *ji=arr->child;ji;ji=ji->next){
      const char *oid=gmlc_json_str(gmlc_json_obj(ji,"objId"),NULL);
      int obj=gmlc_project_find_object(p,oid);
      if(obj<0) continue;
      GmlcRoomInstance in;
      memset(&in,0,sizeof(in));
      in.id=gmlc_strdup(gmlc_json_str(gmlc_json_obj(ji,"id"),""));
      in.name=gmlc_strdup(gmlc_json_str(gmlc_json_obj(ji,"name"),""));
      in.x=gmlc_json_int(gmlc_json_obj(ji,"x"),0);
      in.y=gmlc_json_int(gmlc_json_obj(ji,"y"),0);
      in.object_id=obj;
      in.instance_id=(*next_id)++;
      in.sx=(float)gmlc_json_num(gmlc_json_obj(ji,"scaleX"),1.0);
      in.sy=(float)gmlc_json_num(gmlc_json_obj(ji,"scaleY"),1.0);
      in.rotation=(float)gmlc_json_num(gmlc_json_obj(ji,"rotation"),0.0);
      in.color=parse_u32_color(gmlc_json_obj(ji,"colour"),0xFFFFFFFFu);
      room_add_instance(r,&in);
    }
  }
}

static int parse_sprite(GmlcProject *p, const GmlcResource *res, const GmlcJson *yy, char *err, size_t errcap){
  GmlcSprite s;
  memset(&s,0,sizeof(s));
  s.id=gmlc_strdup(res->id);
  s.name=gmlc_strdup(gmlc_json_str(gmlc_json_obj(yy,"name"),res->name?res->name:""));
  s.width=gmlc_json_int(gmlc_json_obj(yy,"width"),0);
  s.height=gmlc_json_int(gmlc_json_obj(yy,"height"),0);
  s.xorig=gmlc_json_int(gmlc_json_obj(yy,"xorig"),0);
  s.yorig=gmlc_json_int(gmlc_json_obj(yy,"yorig"),0);
  s.bbox_left=gmlc_json_int(gmlc_json_obj(yy,"bbox_left"),0);
  s.bbox_right=gmlc_json_int(gmlc_json_obj(yy,"bbox_right"),s.width?s.width-1:0);
  s.bbox_top=gmlc_json_int(gmlc_json_obj(yy,"bbox_top"),0);
  s.bbox_bottom=gmlc_json_int(gmlc_json_obj(yy,"bbox_bottom"),s.height?s.height-1:0);
  const GmlcJson *frames=gmlc_json_obj(yy,"frames");
  s.n_frames=gmlc_json_len(frames);
  if(s.n_frames>0){
    s.frame_paths=(char**)calloc((size_t)s.n_frames,sizeof(char*));
    char *dir=gmlc_path_dirname(res->abs_path);
    int fi=0;
    for(const GmlcJson *fr=frames?frames->child:NULL; fr && fi<s.n_frames; fr=fr->next,fi++){
      const char *fid=gmlc_json_str(gmlc_json_obj(fr,"id"),"");
      size_t n=strlen(fid)+5;
      char *file=(char*)malloc(n);
      if(file) snprintf(file,n,"%s.png",fid);
      s.frame_paths[fi]=gmlc_path_join(dir,file?file:"");
      free(file);
    }
    free(dir);
  }
  if(!s.id || !s.name || !add_sprite(p,&s)){
    snprintf(err,errcap,"out of memory while loading sprite resource");
    free(s.id); free(s.name);
    for(int f=0;f<s.n_frames;f++) free(s.frame_paths ? s.frame_paths[f] : NULL);
    free(s.frame_paths);
    return 0;
  }
  return 1;
}

static int parse_sound(GmlcProject *p, const GmlcResource *res, const GmlcJson *yy, char *err, size_t errcap){
  GmlcSound s;
  memset(&s,0,sizeof(s));
  s.id=gmlc_strdup(res->id);
  s.name=gmlc_strdup(gmlc_json_str(gmlc_json_obj(yy,"name"),res->name?res->name:""));
  s.volume=(float)gmlc_json_num(gmlc_json_obj(yy,"volume"),1.0);
  s.pitch=1.0f;
  char *dir=gmlc_path_dirname(res->abs_path);
  s.data_path=gmlc_path_join(dir,s.name);
  free(dir);
  if(!s.id || !s.name || !s.data_path || !add_sound(p,&s)){
    snprintf(err,errcap,"out of memory while loading sound resource");
    free(s.id); free(s.name); free(s.data_path);
    return 0;
  }
  return 1;
}

static int parse_script(GmlcProject *p, const GmlcResource *res, const GmlcJson *yy, char *err, size_t errcap){
  GmlcScript s;
  memset(&s,0,sizeof(s));
  s.id=gmlc_strdup(res->id);
  s.name=gmlc_strdup(gmlc_json_str(gmlc_json_obj(yy,"name"),res->name?res->name:""));
  char *dir=gmlc_path_dirname(res->abs_path);
  size_t n=strlen(s.name?s.name:"")+5;
  char *file=(char*)malloc(n);
  if(file) snprintf(file,n,"%s.gml",s.name?s.name:"");
  s.source_path=gmlc_path_join(dir,file?file:"");
  free(file); free(dir);
  if(!s.id || !s.name || !s.source_path || !add_script(p,&s)){
    snprintf(err,errcap,"out of memory while loading script resource");
    free(s.id); free(s.name); free(s.source_path);
    return 0;
  }
  return 1;
}

static int parse_object(GmlcProject *p, const GmlcResource *res, const GmlcJson *yy, char *err, size_t errcap){
  GmlcObject o;
  memset(&o,0,sizeof(o));
  o.id=gmlc_strdup(res->id);
  o.name=gmlc_strdup(gmlc_json_str(gmlc_json_obj(yy,"name"),res->name?res->name:""));
  o.sprite_id=gmlc_project_find_sprite(p,gmlc_json_str(gmlc_json_obj(yy,"spriteId"),NULL));
  o.mask_id=gmlc_project_find_sprite(p,gmlc_json_str(gmlc_json_obj(yy,"maskSpriteId"),NULL));
  o.parent_id=gmlc_project_find_object(p,gmlc_json_str(gmlc_json_obj(yy,"parentObjectId"),NULL));
  o.visible=gmlc_json_bool(gmlc_json_obj(yy,"visible"),1);
  o.solid=gmlc_json_bool(gmlc_json_obj(yy,"solid"),0);
  o.persistent=gmlc_json_bool(gmlc_json_obj(yy,"persistent"),0);
  const GmlcJson *events=gmlc_json_obj(yy,"eventList");
  if(events && events->type==GMLC_JSON_ARRAY){
    char *dir=gmlc_path_dirname(res->abs_path);
    for(const GmlcJson *je=events->child;je;je=je->next){
      GmlcObjectEvent ev;
      memset(&ev,0,sizeof(ev));
      ev.event_type=gmlc_json_int(gmlc_json_obj(je,"eventtype"),0);
      ev.event_number=gmlc_json_int(gmlc_json_obj(je,"enumb"),0);
      const char *event_uuid=gmlc_json_str(gmlc_json_obj(je,"id"),NULL);
      const char *col_uuid=gmlc_json_str(gmlc_json_obj(je,"collisionObjectId"),NULL);
      ev.collision_object_id=gmlc_project_find_object(p,col_uuid);
      char file[256];
      switch(ev.event_type){
        case 0: snprintf(file,sizeof(file),"Create_%d.gml",ev.event_number); break;
        case 1: snprintf(file,sizeof(file),"Destroy_%d.gml",ev.event_number); break;
        case 2: snprintf(file,sizeof(file),"Alarm_%d.gml",ev.event_number); break;
        case 3: snprintf(file,sizeof(file),"Step_%d.gml",ev.event_number); break;
        case 4: snprintf(file,sizeof(file),"Collision_%s.gml",event_uuid?event_uuid:""); break;
        case 5: snprintf(file,sizeof(file),"Keyboard_%d.gml",ev.event_number); break;
        case 6: snprintf(file,sizeof(file),"Mouse_%d.gml",ev.event_number); break;
        case 7: snprintf(file,sizeof(file),"Other_%d.gml",ev.event_number); break;
        case 8: snprintf(file,sizeof(file),"Draw_%d.gml",ev.event_number); break;
        case 9: snprintf(file,sizeof(file),"KeyPress_%d.gml",ev.event_number); break;
        case 10: snprintf(file,sizeof(file),"KeyRelease_%d.gml",ev.event_number); break;
        default: snprintf(file,sizeof(file),"Other_%d.gml",ev.event_number); break;
      }
      ev.source_path=gmlc_path_join(dir,file);
      if(!object_add_event(&o,&ev)){
        snprintf(err,errcap,"out of memory while loading object events");
        free(o.id); free(o.name); free(ev.source_path); free(o.events);
        free(dir);
        return 0;
      }
    }
    free(dir);
  }
  if(!o.id || !o.name || !add_object(p,&o)){
    snprintf(err,errcap,"out of memory while loading object resource");
    free(o.id); free(o.name); free(o.events);
    return 0;
  }
  return 1;
}

static int parse_room(GmlcProject *p, const GmlcResource *res, const GmlcJson *yy, char *err, size_t errcap){
  GmlcRoom r;
  memset(&r,0,sizeof(r));
  r.id=gmlc_strdup(res->id);
  r.name=gmlc_strdup(gmlc_json_str(gmlc_json_obj(yy,"name"),res->name?res->name:""));
  const GmlcJson *settings=gmlc_json_obj(yy,"roomSettings");
  r.width=gmlc_json_int(gmlc_json_obj(settings,"Width"),640);
  r.height=gmlc_json_int(gmlc_json_obj(settings,"Height"),480);
  r.speed=60;
  const GmlcJson *view_settings=gmlc_json_obj(yy,"viewSettings");
  r.view_enabled=gmlc_json_bool(gmlc_json_obj(view_settings,"enableViews"),0);
  const GmlcJson *v0=gmlc_json_index(gmlc_json_obj(yy,"views"),0);
  r.view_w=gmlc_json_int(gmlc_json_obj(v0,"wview"),r.width);
  r.view_h=gmlc_json_int(gmlc_json_obj(v0,"hview"),r.height);
  r.port_w=gmlc_json_int(gmlc_json_obj(v0,"wport"),r.width);
  r.port_h=gmlc_json_int(gmlc_json_obj(v0,"hport"),r.height);
  const char *cc=gmlc_json_str(gmlc_json_obj(yy,"creationCodeFile"),"");
  if(cc && *cc){
    char *dir=gmlc_path_dirname(res->abs_path);
    r.creation_code_path=gmlc_path_join(dir,cc);
    free(dir);
  }
  int next_id=100000;
  scan_instances(p,&r,gmlc_json_obj(yy,"layers"),&next_id);
  if(!r.id || !r.name || !add_room(p,&r)){
    snprintf(err,errcap,"out of memory while loading room resource");
    free(r.id); free(r.name); free(r.instances);
    return 0;
  }
  return 1;
}

static char *basename_no_ext(const char *path){
  const char *b=strrchr(path,'/');
  const char *bs=strrchr(path,'\\');
  if(bs && (!b || bs>b)) b=bs;
  b=b?b+1:path;
  const char *dot=strrchr(b,'.');
  size_t n=dot && dot>b ? (size_t)(dot-b) : strlen(b);
  char *out=(char*)malloc(n+1);
  if(out){ memcpy(out,b,n); out[n]=0; }
  return out;
}

static int load_one(GmlcProject *p, GmlcResource *r, char *err, size_t errcap){
  if(r->kind==GMLC_RES_OTHER) return 1;
  GmlcJson *yy=gmlc_json_parse_file(r->abs_path,err,errcap);
  if(!yy) return 0;
  const char *nm=gmlc_json_str(gmlc_json_obj(yy,"name"),NULL);
  r->name=nm ? gmlc_strdup(nm) : basename_no_ext(r->path);
  int ok=1;
  switch(r->kind){
    case GMLC_RES_SPRITE: ok=parse_sprite(p,r,yy,err,errcap); break;
    case GMLC_RES_SOUND: ok=parse_sound(p,r,yy,err,errcap); break;
    case GMLC_RES_SCRIPT: ok=parse_script(p,r,yy,err,errcap); break;
    case GMLC_RES_OBJECT: ok=parse_object(p,r,yy,err,errcap); break;
    case GMLC_RES_ROOM: ok=parse_room(p,r,yy,err,errcap); break;
    case GMLC_RES_SHADER: p->n_shaders++; break;
    case GMLC_RES_FONT: p->n_fonts++; break;
    default: break;
  }
  gmlc_json_free(yy);
  return ok;
}

int gmlc_assets_load(GmlcProject *p, char *err, size_t errcap){
  for(int pass=0; pass<3; pass++){
    for(int i=0;i<p->n_resources;i++){
      GmlcResource *r=&p->resources[i];
      if((pass==0 && (r->kind==GMLC_RES_SPRITE || r->kind==GMLC_RES_SOUND || r->kind==GMLC_RES_SCRIPT || r->kind==GMLC_RES_SHADER || r->kind==GMLC_RES_FONT)) ||
         (pass==1 && r->kind==GMLC_RES_OBJECT) ||
         (pass==2 && r->kind==GMLC_RES_ROOM)){
        if(!load_one(p,r,err,errcap)) return 0;
      }
    }
  }
  return 1;
}
