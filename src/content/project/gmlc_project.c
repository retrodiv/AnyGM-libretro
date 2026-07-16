/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_project.h"
#include "gmlc_assets.h"
#include "gmlc_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

char *gmlc_strdup(const char *s){
  if(!s) return NULL;
  size_t n=strlen(s);
  char *d=(char*)malloc(n+1);
  if(d) memcpy(d,s,n+1);
  return d;
}

#define GMLC_MEMORY_PATH_PREFIX "gmlc-memory://"

char *gmlc_project_add_memory_file(GmlcProject *p, const char *label,
                                   GmlcMemoryFileKind kind, const void *data,
                                   size_t size, int width, int height){
  if(!p || (size && !data)) return NULL;
  if(kind==GMLC_MEMORY_TEXT && size==SIZE_MAX) return NULL;
  if(p->n_memory_files>=p->cap_memory_files){
    if(p->cap_memory_files>INT_MAX/2) return NULL;
    int nc=p->cap_memory_files?p->cap_memory_files*2:64;
    GmlcMemoryFile *nf=(GmlcMemoryFile*)realloc(p->memory_files,(size_t)nc*sizeof(*nf));
    if(!nf) return NULL;
    p->memory_files=nf;
    p->cap_memory_files=nc;
  }
  size_t allocation=size+((kind==GMLC_MEMORY_TEXT)?1u:0u);
  uint8_t *copy=(uint8_t*)malloc(allocation?allocation:1u);
  if(!copy) return NULL;
  if(size) memcpy(copy,data,size);
  if(kind==GMLC_MEMORY_TEXT) copy[size]=0;
  int index=p->n_memory_files;
  p->memory_files[index]=(GmlcMemoryFile){copy,size,width,height,kind};
  p->n_memory_files++;
  const char *name=label&&*label?label:"resource";
  size_t cap=strlen(GMLC_MEMORY_PATH_PREFIX)+32+strlen(name)+1;
  char *path=(char*)malloc(cap);
  if(!path){
    free(copy);
    p->n_memory_files--;
    return NULL;
  }
  snprintf(path,cap,GMLC_MEMORY_PATH_PREFIX "%d/%s",index,name);
  return path;
}

const GmlcMemoryFile *gmlc_project_find_memory_file(const GmlcProject *p,
                                                    const char *path){
  if(!p || !path || strncmp(path,GMLC_MEMORY_PATH_PREFIX,
                             sizeof(GMLC_MEMORY_PATH_PREFIX)-1)) return NULL;
  const char *number=path+sizeof(GMLC_MEMORY_PATH_PREFIX)-1;
  char *end=NULL;
  long index=strtol(number,&end,10);
  if(end==number || (*end!='/' && *end!='\0') || index<0 || index>=p->n_memory_files)
    return NULL;
  return &p->memory_files[index];
}

char *gmlc_project_read_source(const GmlcProject *p, const char *path){
  const GmlcMemoryFile *memory=gmlc_project_find_memory_file(p,path);
  if(memory){
    if(memory->kind!=GMLC_MEMORY_TEXT) return NULL;
    return gmlc_strdup((const char*)memory->data);
  }
  FILE *f=path?fopen(path,"rb"):NULL;
  if(!f) return NULL;
  if(fseek(f,0,SEEK_END)!=0){ fclose(f); return NULL; }
  long size=ftell(f);
  if(size<0 || fseek(f,0,SEEK_SET)!=0){ fclose(f); return NULL; }
  char *text=(char*)malloc((size_t)size+1);
  if(!text){ fclose(f); return NULL; }
  int ok=fread(text,1,(size_t)size,f)==(size_t)size;
  if(fclose(f)!=0) ok=0;
  if(!ok){ free(text); return NULL; }
  text[size]=0;
  return text;
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
  free(p->root_dir); free(p->yyp_path); free(p->name); free(p->startup_code_path);
  free(p->classic_game_information);
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
  for(int i=0;i<p->n_function_aliases;i++){
    free(p->function_aliases[i].public_name);
    free(p->function_aliases[i].target_name);
  }
  free(p->function_aliases);
  for(int i=0;i<p->n_paths;i++){
    free(p->paths[i].id); free(p->paths[i].name); free(p->paths[i].points);
  }
  free(p->paths);
  for(int i=0;i<p->n_timelines;i++){
    free(p->timelines[i].id); free(p->timelines[i].name);
    for(int m=0;m<p->timelines[i].n_moments;m++) free(p->timelines[i].moments[m].source_path);
    free(p->timelines[i].moments);
  }
  free(p->timelines);
  for(int i=0;i<p->n_resource_order;i++) free(p->resource_order_ids[i]);
  free(p->resource_order_ids);
  for(int i=0;i<p->n_script_order;i++) free(p->script_order_ids[i]);
  free(p->script_order_ids);
  for(int i=0;i<p->n_objects;i++){
    free(p->objects[i].id); free(p->objects[i].name);
    free(p->objects[i].physics_points);
    for(int e=0;e<p->objects[i].n_events;e++){
      free(p->objects[i].events[e].id);
      free(p->objects[i].events[e].collision_id);
      free(p->objects[i].events[e].source_path);
    }
    free(p->objects[i].events);
  }
  free(p->objects);
  for(int i=0;i<p->n_rooms;i++){
    GmlcRoom *r=&p->rooms[i];
    free(r->id); free(r->name);
    free(r->creation_code_path);
    for(int k=0;k<r->n_instances;k++){
      free(r->instances[k].id); free(r->instances[k].name);
      free(r->instances[k].creation_code_path);
    }
    free(r->instances);
    free(r->backgrounds);
    free(r->tiles);
    for(int l=0;l<r->n_layers;l++){
      GmlcRoomLayer *ly=&r->layers[l];
      free(ly->id); free(ly->name);
      free(ly->tile_data);
      free(ly->instance_ids);
      for(int a=0;a<ly->n_assets;a++) free(ly->assets[a].name);
      free(ly->assets);
    }
    free(r->layers);
  }
  free(p->rooms);
  free(p->room_order);
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
  for(int i=0;i<p->n_tilesets;i++){
    free(p->tilesets[i].id); free(p->tilesets[i].name);
  }
  free(p->tilesets);
  for(int i=0;i<p->n_constants;i++){
    free(p->constants[i].name); free(p->constants[i].expression);
  }
  free(p->constants);
  for(int i=0;i<p->n_triggers;i++){
    free(p->triggers[i].name); free(p->triggers[i].condition_path);
  }
  free(p->triggers);
  for(int i=0;i<p->n_included_files;i++){
    free(p->included_files[i].file_name); free(p->included_files[i].custom_folder);
    free(p->included_files[i].data);
  }
  free(p->included_files);
  for(int i=0;i<p->n_memory_files;i++) free(p->memory_files[i].data);
  free(p->memory_files);
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
  if(!strcmp(type,"GMTileSet")) return GMLC_RES_TILESET;
  return GMLC_RES_OTHER;
}

static char *basename_no_ext(const char *path);

static const char *type_from_path(const char *path){
  if(!path) return "";
  if(!strncmp(path,"sprites/",8)) return "GMSprite";
  if(!strncmp(path,"sounds/",7)) return "GMSound";
  if(!strncmp(path,"scripts/",8)) return "GMScript";
  if(!strncmp(path,"objects/",8)) return "GMObject";
  if(!strncmp(path,"rooms/",6)) return "GMRoom";
  if(!strncmp(path,"shaders/",8)) return "GMShader";
  if(!strncmp(path,"fonts/",6)) return "GMFont";
  if(!strncmp(path,"tilesets/",9)) return "GMTileSet";
  return "";
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
  const char *actual_type=(type && *type) ? type : type_from_path(path);
  r->type_name=gmlc_strdup(actual_type);
  r->path=gmlc_strdup(path?path:"");
  if(r->path) gmlc_path_slashes(r->path);
  r->abs_path=gmlc_path_join(p->root_dir,r->path);
  r->kind=kind_from_type(actual_type);
  r->name=basename_no_ext(r->path?r->path:"");
  r->type_id=resource_type_counter(p,r->kind)-1;
  return r->id && r->name && r->type_name && r->path && r->abs_path;
}

static char *basename_no_ext(const char *path){
  const char *base=strrchr(path,'/');
  const char *bslash=strrchr(path,'\\');
  if(bslash && (!base || bslash>base)) base=bslash;
  base=base?base+1:path;
  size_t n=strlen(base);
  const char *dot=strrchr(base,'.');
  if(dot && dot>base) n=(size_t)(dot-base);
  char *out=(char*)malloc(n+1);
  if(!out) return NULL;
  memcpy(out,base,n);
  out[n]=0;
  return out;
}

typedef struct {
  char *id;
  char **children;
  int n_children;
} GmlcFolderOrder;

static void free_folder_orders(GmlcFolderOrder *folders, int n){
  if(!folders) return;
  for(int i=0;i<n;i++){
    free(folders[i].id);
    for(int c=0;c<folders[i].n_children;c++) free(folders[i].children[c]);
    free(folders[i].children);
  }
  free(folders);
}

static int folder_order_index(GmlcFolderOrder *folders, int n, const char *id){
  if(!id) return -1;
  for(int i=0;i<n;i++) if(folders[i].id && !strcmp(folders[i].id,id)) return i;
  return -1;
}

static int resource_order_has(const GmlcProject *p, const char *id){
  if(!id) return 1;
  for(int i=0;i<p->n_resource_order;i++)
    if(p->resource_order_ids[i] && !strcmp(p->resource_order_ids[i],id)) return 1;
  return 0;
}

static int append_resource_order(GmlcProject *p, const char *id){
  if(!id || !*id || resource_order_has(p,id)) return 1;
  char **nr=(char**)realloc(p->resource_order_ids,(size_t)(p->n_resource_order+1)*sizeof(*nr));
  if(!nr) return 0;
  p->resource_order_ids=nr;
  p->resource_order_ids[p->n_resource_order]=gmlc_strdup(id);
  if(!p->resource_order_ids[p->n_resource_order]) return 0;
  p->n_resource_order++;
  return 1;
}

static int traverse_folder_order(GmlcProject *p, GmlcFolderOrder *folders, int n, int idx){
  if(idx<0 || idx>=n) return 1;
  for(int i=0;i<folders[idx].n_children;i++){
    const char *child=folders[idx].children[i];
    int fi=folder_order_index(folders,n,child);
    if(fi>=0){
      if(!traverse_folder_order(p,folders,n,fi)) return 0;
    } else if(!append_resource_order(p,child)) return 0;
  }
  return 1;
}

static int load_resource_order(GmlcProject *p){
  int cap=0, n=0;
  GmlcFolderOrder *folders=NULL;
  for(int i=0;i<p->n_resources;i++){
    GmlcResource *r=&p->resources[i];
    if(!r->type_name || strcmp(r->type_name,"GMFolder")) continue;
    if(n>=cap){
      int nc=cap?cap*2:16;
      GmlcFolderOrder *nf=(GmlcFolderOrder*)realloc(folders,(size_t)nc*sizeof(*nf));
      if(!nf){ free_folder_orders(folders,n); return 0; }
      folders=nf;
      memset(&folders[cap],0,(size_t)(nc-cap)*sizeof(*folders));
      cap=nc;
    }
    char local_err[256]={0};
    GmlcJson *yy=gmlc_json_parse_file(r->abs_path,local_err,sizeof(local_err));
    if(!yy) continue;
    const char *id=gmlc_json_str(gmlc_json_obj(yy,"id"),r->id?r->id:"");
    folders[n].id=gmlc_strdup(id);
    const GmlcJson *children=gmlc_json_obj(yy,"children");
    int cn=(children && children->type==GMLC_JSON_ARRAY) ? gmlc_json_len(children) : 0;
    if(cn>0){
      folders[n].children=(char**)calloc((size_t)cn,sizeof(char*));
      if(!folders[n].children){
        gmlc_json_free(yy);
        free_folder_orders(folders,n+1);
        return 0;
      }
      for(int c=0;c<cn;c++){
        folders[n].children[c]=gmlc_strdup(gmlc_json_str(gmlc_json_index(children,c),""));
        if(!folders[n].children[c]){
          gmlc_json_free(yy);
          free_folder_orders(folders,n+1);
          return 0;
        }
        folders[n].n_children++;
      }
    }
    gmlc_json_free(yy);
    if(!folders[n].id){
      free_folder_orders(folders,n+1);
      return 0;
    }
    n++;
  }
  if(n<=0){
    free(folders);
    return 1;
  }
  unsigned char *is_child=(unsigned char*)calloc((size_t)n,1);
  if(!is_child){ free_folder_orders(folders,n); return 0; }
  for(int i=0;i<n;i++){
    for(int c=0;c<folders[i].n_children;c++){
      int fi=folder_order_index(folders,n,folders[i].children[c]);
      if(fi>=0) is_child[fi]=1;
    }
  }
  int roots=0, ok=1;
  for(int i=0;i<n;i++){
    if(is_child[i]) continue;
    roots++;
    if(!traverse_folder_order(p,folders,n,i)){ ok=0; break; }
  }
  if(ok && roots==0){
    for(int i=0;i<n;i++){
      if(!traverse_folder_order(p,folders,n,i)){ ok=0; break; }
    }
  }
  free(is_child);
  free_folder_orders(folders,n);
  return ok;
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
    p->name=basename_no_ext(path);
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
    const GmlcJson *idobj=gmlc_json_obj(it,"id");
    const char *rid=gmlc_json_str(gmlc_json_obj(it,"Key"),gmlc_json_str(gmlc_json_obj(val,"id"),""));
    if(!*rid) rid=gmlc_json_str(gmlc_json_obj(idobj,"name"),"");
    const char *rtype=gmlc_json_str(gmlc_json_obj(val,"resourceType"),"");
    const char *rpath=gmlc_json_str(gmlc_json_obj(val,"resourcePath"),"");
    if(!*rpath) rpath=gmlc_json_str(gmlc_json_obj(idobj,"path"),"");
    if(!*rpath) continue;
    if(!add_resource(p,rid,rtype,rpath)){
      gmlc_json_free(root);
      snprintf(err,errcap,"out of memory while loading resources");
      return 0;
    }
  }
  if(!load_resource_order(p)){
    gmlc_json_free(root);
    snprintf(err,errcap,"out of memory while loading resource order");
    return 0;
  }
  const GmlcJson *script_order=gmlc_json_obj(root,"script_order");
  if(script_order && script_order->type==GMLC_JSON_ARRAY){
    int n=gmlc_json_len(script_order);
    if(n>0){
      p->script_order_ids=(char**)calloc((size_t)n,sizeof(char*));
      if(!p->script_order_ids){
        gmlc_json_free(root);
        snprintf(err,errcap,"out of memory while loading script_order");
        return 0;
      }
      for(int i=0;i<n;i++){
        const char *sid=gmlc_json_str(gmlc_json_index(script_order,i),"");
        p->script_order_ids[i]=gmlc_strdup(sid);
        if(!p->script_order_ids[i]){
          gmlc_json_free(root);
          snprintf(err,errcap,"out of memory while loading script_order");
          return 0;
        }
        p->n_script_order++;
      }
    }
  }
  gmlc_json_free(root);
  if(!gmlc_assets_load(p,err,errcap)) return 0;
  return 1;
}

int gmlc_project_find_object(const GmlcProject *p, const char *id){
  if(!id || !strcmp(id,"00000000-0000-0000-0000-000000000000")) return -1;
  for(int i=0;i<p->n_objects;i++){
    if((p->objects[i].id && !strcmp(p->objects[i].id,id)) ||
       (p->objects[i].name && !strcmp(p->objects[i].name,id))) return i;
  }
  return -1;
}

int gmlc_project_find_sprite(const GmlcProject *p, const char *id){
  if(!id || !strcmp(id,"00000000-0000-0000-0000-000000000000")) return -1;
  for(int i=0;i<p->n_sprites;i++){
    if((p->sprites[i].id && !strcmp(p->sprites[i].id,id)) ||
       (p->sprites[i].name && !strcmp(p->sprites[i].name,id))) return i;
  }
  return -1;
}

int gmlc_project_sprite_runtime_id(const GmlcProject *p, int sprite_index){
  if(!p || sprite_index<0 || sprite_index>=p->n_sprites) return -1;
  return p->sprites[sprite_index].runtime_id;
}

int gmlc_project_runtime_sprite_count(const GmlcProject *p){
  int n=0;
  if(!p) return 0;
  for(int i=0;i<p->n_sprites;i++)
    if(p->sprites[i].runtime_id>=n) n=p->sprites[i].runtime_id+1;
  return n;
}

int gmlc_project_find_sound(const GmlcProject *p, const char *id){
  if(!id || !strcmp(id,"00000000-0000-0000-0000-000000000000")) return -1;
  for(int i=0;i<p->n_sounds;i++){
    if((p->sounds[i].id && !strcmp(p->sounds[i].id,id)) ||
       (p->sounds[i].name && !strcmp(p->sounds[i].name,id))) return i;
  }
  return -1;
}

int gmlc_project_find_room(const GmlcProject *p, const char *id){
  if(!id || !strcmp(id,"00000000-0000-0000-0000-000000000000")) return -1;
  for(int i=0;i<p->n_rooms;i++){
    if((p->rooms[i].id && !strcmp(p->rooms[i].id,id)) ||
       (p->rooms[i].name && !strcmp(p->rooms[i].name,id))) return i;
  }
  return -1;
}

int gmlc_project_find_tileset(const GmlcProject *p, const char *id){
  if(!id || !strcmp(id,"00000000-0000-0000-0000-000000000000")) return -1;
  for(int i=0;i<p->n_tilesets;i++){
    if((p->tilesets[i].id && !strcmp(p->tilesets[i].id,id)) ||
       (p->tilesets[i].name && !strcmp(p->tilesets[i].name,id))) return i;
  }
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
    case GMLC_RES_TILESET: return "tileset";
    default: return "other";
  }
}
