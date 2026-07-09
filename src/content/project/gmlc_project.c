/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_project.h"
#include "gmlc_assets.h"
#include "gmlc_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *gmlc_strdup(const char *s){
  if(!s) return NULL;
  size_t n=strlen(s);
  char *d=(char*)malloc(n+1);
  if(d) memcpy(d,s,n+1);
  return d;
}

void gmlc_path_slashes(char *s){
  if(!s) return;
  for(;*s;s++) if(*s=='\\') *s='/';
}

char *gmlc_path_dirname(const char *path){
  const char *slash=strrchr(path,'/');
  const char *bslash=strrchr(path,'\\');
  if(bslash && (!slash || bslash>slash)) slash=bslash;
  if(!slash) return gmlc_strdup(".");
  size_t n=(size_t)(slash-path);
  char *out=(char*)malloc(n+1);
  if(!out) return NULL;
  memcpy(out,path,n); out[n]=0;
  return out;
}

char *gmlc_path_join(const char *a, const char *b){
  if(!a || !*a) return gmlc_strdup(b?b:"");
  if(!b || !*b) return gmlc_strdup(a);
  if(b[0]=='/' || (b[0] && b[1]==':')) return gmlc_strdup(b);
  size_t na=strlen(a), nb=strlen(b);
  int sep=na>0 && a[na-1]!='/' && a[na-1]!='\\';
  char *out=(char*)malloc(na+(sep?1:0)+nb+1);
  if(!out) return NULL;
  memcpy(out,a,na);
  size_t o=na;
  if(sep) out[o++]='/';
  memcpy(out+o,b,nb+1);
  gmlc_path_slashes(out);
  return out;
}

void gmlc_project_init(GmlcProject *p){
  memset(p,0,sizeof(*p));
  p->next_instance_id=100000;
  p->next_layer_id=0;
}

static void free_resource(GmlcResource *r){
  free(r->id); free(r->name); free(r->type_name); free(r->path); free(r->abs_path);
}

void gmlc_project_free(GmlcProject *p){
  if(!p) return;
  free(p->root_dir); free(p->yyp_path); free(p->name);
  for(int i=0;i<p->n_resources;i++) free_resource(&p->resources[i]);
  free(p->resources);
  for(int i=0;i<p->n_sprites;i++){
    free(p->sprites[i].id); free(p->sprites[i].name);
    for(int f=0;f<p->sprites[i].n_frames;f++) free(p->sprites[i].frame_paths ? p->sprites[i].frame_paths[f] : NULL);
    free(p->sprites[i].frame_paths);
  }
  free(p->sprites);
  for(int i=0;i<p->n_sounds;i++){ free(p->sounds[i].id); free(p->sounds[i].name); free(p->sounds[i].data_path); }
  free(p->sounds);
  for(int i=0;i<p->n_scripts;i++){ free(p->scripts[i].id); free(p->scripts[i].name); free(p->scripts[i].source_path); }
  free(p->scripts);
  for(int i=0;i<p->n_objects;i++){
    free(p->objects[i].id); free(p->objects[i].name);
    for(int e=0;e<p->objects[i].n_events;e++) free(p->objects[i].events[e].source_path);
    free(p->objects[i].events);
  }
  free(p->objects);
  for(int i=0;i<p->n_rooms;i++){
    GmlcRoom *r=&p->rooms[i];
    free(r->id); free(r->name);
    free(r->creation_code_path);
    for(int k=0;k<r->n_instances;k++){ free(r->instances[k].id); free(r->instances[k].name); }
    free(r->instances);
    for(int l=0;l<r->n_layers;l++){
      GmlcRoomLayer *ly=&r->layers[l];
      free(ly->id); free(ly->name);
      free(ly->instance_ids);
      for(int a=0;a<ly->n_assets;a++) free(ly->assets[a].name);
      free(ly->assets);
    }
    free(r->layers);
  }
  free(p->rooms);
  for(int i=0;i<p->n_shaders;i++){
    free(p->shaders[i].id); free(p->shaders[i].name);
    free(p->shaders[i].vertex_source); free(p->shaders[i].fragment_source);
  }
  free(p->shaders);
  for(int i=0;i<p->n_fonts;i++){
    free(p->fonts[i].id); free(p->fonts[i].name); free(p->fonts[i].png_path);
    free(p->fonts[i].glyphs);
  }
  free(p->fonts);
  memset(p,0,sizeof(*p));
}

static GmlcResKind kind_from_type(const char *type){
  if(!type) return GMLC_RES_OTHER;
  if(!strcmp(type,"GMSprite")) return GMLC_RES_SPRITE;
  if(!strcmp(type,"GMSound")) return GMLC_RES_SOUND;
  if(!strcmp(type,"GMScript")) return GMLC_RES_SCRIPT;
  if(!strcmp(type,"GMObject")) return GMLC_RES_OBJECT;
  if(!strcmp(type,"GMRoom")) return GMLC_RES_ROOM;
  if(!strcmp(type,"GMShader")) return GMLC_RES_SHADER;
  if(!strcmp(type,"GMFont")) return GMLC_RES_FONT;
  return GMLC_RES_OTHER;
}

static int resource_type_counter(GmlcProject *p, GmlcResKind k){
  int n=0;
  for(int i=0;i<p->n_resources;i++) if(p->resources[i].kind==k) n++;
  return n;
}

static int add_resource(GmlcProject *p, const char *id, const char *type, const char *path){
  if(p->n_resources>=p->cap_resources){
    int nc=p->cap_resources?p->cap_resources*2:64;
    GmlcResource *nr=(GmlcResource*)realloc(p->resources,(size_t)nc*sizeof(*nr));
    if(!nr) return 0;
    p->resources=nr; p->cap_resources=nc;
  }
  GmlcResource *r=&p->resources[p->n_resources++];
  memset(r,0,sizeof(*r));
  r->id=gmlc_strdup(id?id:"");
  r->type_name=gmlc_strdup(type?type:"");
  r->path=gmlc_strdup(path?path:"");
  if(r->path) gmlc_path_slashes(r->path);
  r->abs_path=gmlc_path_join(p->root_dir,r->path);
  r->kind=kind_from_type(type);
  r->type_id=resource_type_counter(p,r->kind)-1;
  return r->id && r->type_name && r->path && r->abs_path;
}

int gmlc_project_load_yyp(GmlcProject *p, const char *path, char *err, size_t errcap){
  gmlc_project_init(p);
  p->yyp_path=gmlc_strdup(path);
  p->root_dir=gmlc_path_dirname(path);
  if(!p->yyp_path || !p->root_dir){ snprintf(err,errcap,"out of memory"); return 0; }
  GmlcJson *root=gmlc_json_parse_file(path,err,errcap);
  if(!root) return 0;
  const char *nm=gmlc_json_str(gmlc_json_obj(root,"name"),NULL);
  if(!nm || !*nm){
    const char *base=strrchr(path,'/');
    const char *bslash=strrchr(path,'\\');
    if(bslash && (!base || bslash>base)) base=bslash;
    base=base?base+1:path;
    p->name=gmlc_strdup(base);
  } else {
    p->name=gmlc_strdup(nm);
  }
  const GmlcJson *resources=gmlc_json_obj(root,"resources");
  if(!resources || resources->type!=GMLC_JSON_ARRAY){
    gmlc_json_free(root);
    snprintf(err,errcap,"%s: resources[] missing",path);
    return 0;
  }
  for(const GmlcJson *it=resources->child;it;it=it->next){
    const GmlcJson *val=gmlc_json_obj(it,"Value");
    const char *rid=gmlc_json_str(gmlc_json_obj(it,"Key"),gmlc_json_str(gmlc_json_obj(val,"id"),""));
    const char *rtype=gmlc_json_str(gmlc_json_obj(val,"resourceType"),"");
    const char *rpath=gmlc_json_str(gmlc_json_obj(val,"resourcePath"),"");
    if(!*rpath) continue;
    if(!add_resource(p,rid,rtype,rpath)){
      gmlc_json_free(root);
      snprintf(err,errcap,"out of memory while loading resources");
      return 0;
    }
  }
  gmlc_json_free(root);
  if(!gmlc_assets_load(p,err,errcap)) return 0;
  return 1;
}

int gmlc_project_find_object(const GmlcProject *p, const char *id){
  if(!id || !strcmp(id,"00000000-0000-0000-0000-000000000000")) return -1;
  for(int i=0;i<p->n_objects;i++) if(p->objects[i].id && !strcmp(p->objects[i].id,id)) return i;
  return -1;
}

int gmlc_project_find_sprite(const GmlcProject *p, const char *id){
  if(!id || !strcmp(id,"00000000-0000-0000-0000-000000000000")) return -1;
  for(int i=0;i<p->n_sprites;i++) if(p->sprites[i].id && !strcmp(p->sprites[i].id,id)) return i;
  return -1;
}

int gmlc_project_find_sound(const GmlcProject *p, const char *id){
  if(!id || !strcmp(id,"00000000-0000-0000-0000-000000000000")) return -1;
  for(int i=0;i<p->n_sounds;i++) if(p->sounds[i].id && !strcmp(p->sounds[i].id,id)) return i;
  return -1;
}

int gmlc_project_find_room(const GmlcProject *p, const char *id){
  if(!id || !strcmp(id,"00000000-0000-0000-0000-000000000000")) return -1;
  for(int i=0;i<p->n_rooms;i++) if(p->rooms[i].id && !strcmp(p->rooms[i].id,id)) return i;
  return -1;
}

const char *gmlc_res_kind_name(GmlcResKind k){
  switch(k){
    case GMLC_RES_SPRITE: return "sprite";
    case GMLC_RES_SOUND: return "sound";
    case GMLC_RES_SCRIPT: return "script";
    case GMLC_RES_OBJECT: return "object";
    case GMLC_RES_ROOM: return "room";
    case GMLC_RES_SHADER: return "shader";
    case GMLC_RES_FONT: return "font";
    default: return "other";
  }
}
