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

static int add_shader(GmlcProject *p, GmlcShader *s){
  if(p->n_shaders>=p->cap_shaders){
    int nc=p->cap_shaders?p->cap_shaders*2:16;
    GmlcShader *ns=(GmlcShader*)realloc(p->shaders,(size_t)nc*sizeof(*ns));
    if(!ns) return 0;
    p->shaders=ns; p->cap_shaders=nc;
  }
  p->shaders[p->n_shaders++]=*s;
  return 1;
}

static int add_font(GmlcProject *p, GmlcFont *f){
  if(p->n_fonts>=p->cap_fonts){
    int nc=p->cap_fonts?p->cap_fonts*2:8;
    GmlcFont *nf=(GmlcFont*)realloc(p->fonts,(size_t)nc*sizeof(*nf));
    if(!nf) return 0;
    p->fonts=nf; p->cap_fonts=nc;
  }
  p->fonts[p->n_fonts++]=*f;
  return 1;
}

static int add_tileset(GmlcProject *p, GmlcTileset *t){
  if(p->n_tilesets>=p->cap_tilesets){
    int nc=p->cap_tilesets?p->cap_tilesets*2:8;
    GmlcTileset *nt=(GmlcTileset*)realloc(p->tilesets,(size_t)nc*sizeof(*nt));
    if(!nt) return 0;
    p->tilesets=nt; p->cap_tilesets=nc;
  }
  p->tilesets[p->n_tilesets++]=*t;
  return 1;
}

#define DEFINE_APPLY_RESOURCE_ORDER(fn, Type, field, count_field, label) \
static int fn(GmlcProject *p, char *err, size_t errcap){ \
  int count=p->count_field; \
  if(count<=1 || p->n_resource_order<=0) return 1; \
  Type *ordered=(Type*)calloc((size_t)count,sizeof(*ordered)); \
  unsigned char *used=(unsigned char*)calloc((size_t)count,1); \
  if(!ordered || !used){ \
    free(ordered); free(used); \
    snprintf(err,errcap,"out of memory while ordering %s",label); \
    return 0; \
  } \
  int n=0; \
  for(int oi=0; oi<p->n_resource_order; oi++){ \
    const char *id=p->resource_order_ids[oi]; \
    for(int i=0;i<count;i++){ \
      if(!used[i] && p->field[i].id && id && !strcmp(p->field[i].id,id)){ \
        ordered[n++]=p->field[i]; \
        used[i]=1; \
        break; \
      } \
    } \
  } \
  for(int i=0;i<count;i++) if(!used[i]) ordered[n++]=p->field[i]; \
  free(used); \
  free(p->field); \
  p->field=ordered; \
  return 1; \
}

DEFINE_APPLY_RESOURCE_ORDER(apply_sprite_order, GmlcSprite, sprites, n_sprites, "sprites")
DEFINE_APPLY_RESOURCE_ORDER(apply_sound_order, GmlcSound, sounds, n_sounds, "sounds")
DEFINE_APPLY_RESOURCE_ORDER(apply_script_resource_order, GmlcScript, scripts, n_scripts, "scripts")
DEFINE_APPLY_RESOURCE_ORDER(apply_shader_order, GmlcShader, shaders, n_shaders, "shaders")
DEFINE_APPLY_RESOURCE_ORDER(apply_font_order, GmlcFont, fonts, n_fonts, "fonts")
DEFINE_APPLY_RESOURCE_ORDER(apply_tileset_order, GmlcTileset, tilesets, n_tilesets, "tilesets")
DEFINE_APPLY_RESOURCE_ORDER(apply_room_order, GmlcRoom, rooms, n_rooms, "rooms")

static int font_add_glyph(GmlcFont *f, GmlcFontGlyph *g){
  if(f->n_glyphs>=f->cap_glyphs){
    int nc=f->cap_glyphs?f->cap_glyphs*2:128;
    GmlcFontGlyph *ng=(GmlcFontGlyph*)realloc(f->glyphs,(size_t)nc*sizeof(*ng));
    if(!ng) return 0;
    f->glyphs=ng; f->cap_glyphs=nc;
  }
  f->glyphs[f->n_glyphs++]=*g;
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

static int room_add_layer(GmlcRoom *r, GmlcRoomLayer *ly){
  if(r->n_layers>=r->cap_layers){
    int nc=r->cap_layers?r->cap_layers*2:8;
    GmlcRoomLayer *nl=(GmlcRoomLayer*)realloc(r->layers,(size_t)nc*sizeof(*nl));
    if(!nl) return 0;
    r->layers=nl; r->cap_layers=nc;
  }
  r->layers[r->n_layers++]=*ly;
  return 1;
}

static int layer_add_instance_id(GmlcRoomLayer *ly, uint32_t id){
  if(ly->n_instance_ids>=ly->cap_instance_ids){
    int nc=ly->cap_instance_ids?ly->cap_instance_ids*2:16;
    uint32_t *ni=(uint32_t*)realloc(ly->instance_ids,(size_t)nc*sizeof(*ni));
    if(!ni) return 0;
    ly->instance_ids=ni; ly->cap_instance_ids=nc;
  }
  ly->instance_ids[ly->n_instance_ids++]=id;
  return 1;
}

static int layer_add_asset(GmlcRoomLayer *ly, GmlcRoomAsset *a){
  if(ly->n_assets>=ly->cap_assets){
    int nc=ly->cap_assets?ly->cap_assets*2:8;
    GmlcRoomAsset *na=(GmlcRoomAsset*)realloc(ly->assets,(size_t)nc*sizeof(*na));
    if(!na) return 0;
    ly->assets=na; ly->cap_assets=nc;
  }
  ly->assets[ly->n_assets++]=*a;
  return 1;
}

static void free_room_layer_fields(GmlcRoomLayer *ly){
  if(!ly) return;
  free(ly->id); free(ly->name);
  free(ly->tile_data);
  free(ly->instance_ids);
  for(int a=0;a<ly->n_assets;a++) free(ly->assets[a].name);
  free(ly->assets);
}

static void free_room_fields(GmlcRoom *r){
  if(!r) return;
  free(r->id); free(r->name); free(r->creation_code_path);
  for(int i=0;i<r->n_instances;i++){
    free(r->instances[i].id);
    free(r->instances[i].name);
    free(r->instances[i].creation_code_path);
  }
  free(r->instances);
  for(int l=0;l<r->n_layers;l++) free_room_layer_fields(&r->layers[l]);
  free(r->layers);
}

static char *read_text_file(const char *path){
  FILE *f=fopen(path,"rb");
  if(!f) return NULL;
  fseek(f,0,SEEK_END);
  long sz=ftell(f);
  rewind(f);
  if(sz<0){ fclose(f); return NULL; }
  char *buf=(char*)malloc((size_t)sz+1);
  if(!buf){ fclose(f); return NULL; }
  if(fread(buf,1,(size_t)sz,f)!=(size_t)sz){ fclose(f); free(buf); return NULL; }
  fclose(f);
  buf[sz]=0;
  return buf;
}

static uint32_t be32u(const unsigned char *p){
  return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | (uint32_t)p[3];
}

static int png_file_dims(const char *path, int *w, int *h){
  unsigned char hdr[24];
  FILE *f=fopen(path,"rb");
  if(!f) return 0;
  size_t n=fread(hdr,1,sizeof(hdr),f);
  fclose(f);
  if(n<24 || hdr[0]!=0x89 || hdr[1]!='P' || hdr[2]!='N' || hdr[3]!='G' || memcmp(hdr+12,"IHDR",4)) return 0;
  uint32_t pw=be32u(hdr+16), ph=be32u(hdr+20);
  if(pw==0 || ph==0 || pw>65535 || ph>65535) return 0;
  *w=(int)pw; *h=(int)ph;
  return 1;
}

static int parse_u32_color(const GmlcJson *v, uint32_t fallback){
  const GmlcJson *vv=gmlc_json_obj(v,"Value");
  if(vv) return (uint32_t)gmlc_json_num(vv,(double)fallback);
  return fallback;
}

static int layer_type_from_model(const char *model){
  if(!model) return 0;
  if(!strcmp(model,"GMRBackgroundLayer")) return 1;
  if(!strcmp(model,"GMRInstanceLayer")) return 2;
  if(!strcmp(model,"GMRAssetLayer")) return 3;
  if(!strcmp(model,"GMRTileLayer")) return 4;
  return 0;
}

static int scan_room_layers(GmlcProject *p, GmlcRoom *r, const GmlcJson *layers, const char *room_dir, char *err, size_t errcap){
  if(!layers || layers->type!=GMLC_JSON_ARRAY) return 1;
  for(const GmlcJson *ly=layers->child;ly;ly=ly->next){
    const GmlcJson *sub=gmlc_json_obj(ly,"layers");
    if(sub && sub->type==GMLC_JSON_ARRAY && !scan_room_layers(p,r,sub,room_dir,err,errcap)) return 0;
    int type=layer_type_from_model(gmlc_json_str(gmlc_json_obj(ly,"modelName"),NULL));
    if(type<=0) continue;
    GmlcRoomLayer out;
    memset(&out,0,sizeof(out));
    out.id=gmlc_strdup(gmlc_json_str(gmlc_json_obj(ly,"id"),""));
    out.name=gmlc_strdup(gmlc_json_str(gmlc_json_obj(ly,"name"),""));
    if(!out.id || !out.name){
      snprintf(err,errcap,"out of memory while loading room layer");
      free_room_layer_fields(&out);
      return 0;
    }
    out.layer_id=p->next_layer_id++;
    out.type=type;
    out.depth=gmlc_json_int(gmlc_json_obj(ly,"depth"),0);
    out.visible=gmlc_json_bool(gmlc_json_obj(ly,"visible"),1);
    out.x=(float)gmlc_json_num(gmlc_json_obj(ly,"x"),0.0);
    out.y=(float)gmlc_json_num(gmlc_json_obj(ly,"y"),0.0);
    out.hspeed=(float)gmlc_json_num(gmlc_json_obj(ly,"hspeed"),0.0);
    out.vspeed=(float)gmlc_json_num(gmlc_json_obj(ly,"vspeed"),0.0);
    out.bg_sprite_id=-1;
    out.tile_tileset_id=-1;
    out.bg_color=0xFFFFFFFFu;
    if(type==1){
      out.bg_sprite_id=gmlc_project_find_sprite(p,gmlc_json_str(gmlc_json_obj(ly,"spriteId"),NULL));
      out.bg_htiled=gmlc_json_bool(gmlc_json_obj(ly,"htiled"),0);
      out.bg_vtiled=gmlc_json_bool(gmlc_json_obj(ly,"vtiled"),0);
      out.bg_stretch=gmlc_json_bool(gmlc_json_obj(ly,"stretch"),0);
      out.bg_color=parse_u32_color(gmlc_json_obj(ly,"colour"),0xFFFFFFFFu);
      out.bg_frame=(float)gmlc_json_num(gmlc_json_obj(ly,"frameIndex"),0.0);
      out.bg_speed=(float)gmlc_json_num(gmlc_json_obj(ly,"animationFPS"),0.0);
    } else if(type==2){
      const GmlcJson *arr=gmlc_json_obj(ly,"instances");
      if(arr && arr->type==GMLC_JSON_ARRAY){
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
          if(p->next_instance_id<100000) p->next_instance_id=100000;
          in.instance_id=p->next_instance_id++;
          in.sx=(float)gmlc_json_num(gmlc_json_obj(ji,"scaleX"),1.0);
          in.sy=(float)gmlc_json_num(gmlc_json_obj(ji,"scaleY"),1.0);
          in.rotation=(float)gmlc_json_num(gmlc_json_obj(ji,"rotation"),0.0);
          in.color=parse_u32_color(gmlc_json_obj(ji,"colour"),0xFFFFFFFFu);
          const char *cc=gmlc_json_str(gmlc_json_obj(ji,"creationCodeFile"),"");
          if(cc && *cc){
            in.creation_code_path=gmlc_path_join(room_dir,cc);
            if(!in.creation_code_path){
              snprintf(err,errcap,"out of memory while loading instance creation code path");
              free(in.id); free(in.name);
              free_room_layer_fields(&out);
              return 0;
            }
          }
          if(!layer_add_instance_id(&out,(uint32_t)in.instance_id) || !room_add_instance(r,&in)){
            snprintf(err,errcap,"out of memory while loading room instances");
            free(in.id); free(in.name); free(in.creation_code_path);
            free_room_layer_fields(&out);
            return 0;
          }
        }
      }
    } else if(type==3){
      const GmlcJson *assets=gmlc_json_obj(ly,"assets");
      if(assets && assets->type==GMLC_JSON_ARRAY){
        for(const GmlcJson *ja=assets->child;ja;ja=ja->next){
          if(gmlc_json_bool(gmlc_json_obj(ja,"ignore"),0)) continue;
          GmlcRoomAsset a;
          memset(&a,0,sizeof(a));
          a.name=gmlc_strdup(gmlc_json_str(gmlc_json_obj(ja,"name"),""));
          a.sprite_id=gmlc_project_find_sprite(p,gmlc_json_str(gmlc_json_obj(ja,"spriteId"),NULL));
          a.x=gmlc_json_int(gmlc_json_obj(ja,"x"),0);
          a.y=gmlc_json_int(gmlc_json_obj(ja,"y"),0);
          a.sx=(float)gmlc_json_num(gmlc_json_obj(ja,"scaleX"),1.0);
          a.sy=(float)gmlc_json_num(gmlc_json_obj(ja,"scaleY"),1.0);
          a.rotation=(float)gmlc_json_num(gmlc_json_obj(ja,"rotation"),0.0);
          a.frame=(float)gmlc_json_num(gmlc_json_obj(ja,"frameIndex"),0.0);
          a.speed=(float)gmlc_json_num(gmlc_json_obj(ja,"animationFPS"),0.0);
          a.color=parse_u32_color(gmlc_json_obj(ja,"colour"),0xFFFFFFFFu);
          if(!layer_add_asset(&out,&a)){
            snprintf(err,errcap,"out of memory while loading room asset layer");
            free(a.name);
            free_room_layer_fields(&out);
            return 0;
          }
        }
      }
    } else if(type==4){
      out.tile_tileset_id=gmlc_project_find_tileset(p,gmlc_json_str(gmlc_json_obj(ly,"tilesetId"),NULL));
      const GmlcJson *tiles=gmlc_json_obj(ly,"tiles");
      int cols=gmlc_json_int(gmlc_json_obj(tiles,"SerialiseWidth"),gmlc_json_int(gmlc_json_obj(tiles,"serialiseWidth"),0));
      int rows=gmlc_json_int(gmlc_json_obj(tiles,"SerialiseHeight"),gmlc_json_int(gmlc_json_obj(tiles,"serialiseHeight"),0));
      if(cols<0 || rows<0 || cols>8192 || rows>8192 || (uint64_t)cols*(uint64_t)rows>16000000ull){
        snprintf(err,errcap,"invalid tile layer dimensions");
        free_room_layer_fields(&out);
        return 0;
      }
      out.tile_cols=cols;
      out.tile_rows=rows;
      if(cols>0 && rows>0){
        size_t cells=(size_t)cols*(size_t)rows;
        out.tile_data=(uint32_t*)calloc(cells,sizeof(uint32_t));
        if(!out.tile_data){
          snprintf(err,errcap,"out of memory while loading tile layer");
          free_room_layer_fields(&out);
          return 0;
        }
        const GmlcJson *data=gmlc_json_obj(tiles,"TileSerialiseData");
        if(!data) data=gmlc_json_obj(tiles,"tileSerialiseData");
        if(data && data->type==GMLC_JSON_ARRAY){
          size_t idx=0;
          for(const GmlcJson *jt=data->child;jt && idx<cells;jt=jt->next,idx++)
            out.tile_data[idx]=(uint32_t)gmlc_json_num(jt,0.0);
        }
      }
    }
    if(!room_add_layer(r,&out)){
      snprintf(err,errcap,"out of memory while loading room layers");
      free_room_layer_fields(&out);
      return 0;
    }
  }
  return 1;
}

static int parse_shader(GmlcProject *p, const GmlcResource *res, const GmlcJson *yy, char *err, size_t errcap){
  GmlcShader s;
  memset(&s,0,sizeof(s));
  s.id=gmlc_strdup(res->id);
  s.name=gmlc_strdup(gmlc_json_str(gmlc_json_obj(yy,"name"),res->name?res->name:""));
  char *dir=gmlc_path_dirname(res->abs_path);
  size_t n=strlen(s.name?s.name:"")+5;
  char *vfile=(char*)malloc(n);
  char *ffile=(char*)malloc(n);
  if(vfile) snprintf(vfile,n,"%s.vsh",s.name?s.name:"");
  if(ffile) snprintf(ffile,n,"%s.fsh",s.name?s.name:"");
  char *vpath=gmlc_path_join(dir,vfile?vfile:"");
  char *fpath=gmlc_path_join(dir,ffile?ffile:"");
  s.vertex_source=read_text_file(vpath);
  s.fragment_source=read_text_file(fpath);
  if(!s.id || !s.name || !s.vertex_source || !s.fragment_source || !add_shader(p,&s)){
    snprintf(err,errcap,"failed to load shader source for %s",s.name?s.name:"shader resource");
    free(s.id); free(s.name); free(s.vertex_source); free(s.fragment_source);
    free(dir); free(vfile); free(ffile); free(vpath); free(fpath);
    return 0;
  }
  free(dir); free(vfile); free(ffile); free(vpath); free(fpath);
  return 1;
}

static int parse_font(GmlcProject *p, const GmlcResource *res, const GmlcJson *yy, char *err, size_t errcap){
  GmlcFont f;
  memset(&f,0,sizeof(f));
  f.id=gmlc_strdup(res->id);
  f.name=gmlc_strdup(gmlc_json_str(gmlc_json_obj(yy,"name"),res->name?res->name:""));
  char *dir=gmlc_path_dirname(res->abs_path);
  size_t n=strlen(f.name?f.name:"")+5;
  char *file=(char*)malloc(n);
  if(file) snprintf(file,n,"%s.png",f.name?f.name:"");
  f.png_path=gmlc_path_join(dir,file?file:"");
  f.em_size=gmlc_json_int(gmlc_json_obj(yy,"size"),12);
  if(!f.id || !f.name || !f.png_path || !png_file_dims(f.png_path,&f.width,&f.height)){
    snprintf(err,errcap,"failed to load font texture for %s",f.name?f.name:"font resource");
    free(f.id); free(f.name); free(f.png_path); free(dir); free(file);
    return 0;
  }
  const GmlcJson *glyphs=gmlc_json_obj(yy,"glyphs");
  if(glyphs && glyphs->type==GMLC_JSON_ARRAY){
    for(const GmlcJson *jg=glyphs->child;jg;jg=jg->next){
      const GmlcJson *gv=gmlc_json_obj(jg,"Value");
      GmlcFontGlyph g;
      memset(&g,0,sizeof(g));
      g.ch=gmlc_json_int(gmlc_json_obj(gv,"character"),gmlc_json_int(gmlc_json_obj(jg,"Key"),0));
      g.x=gmlc_json_int(gmlc_json_obj(gv,"x"),0);
      g.y=gmlc_json_int(gmlc_json_obj(gv,"y"),0);
      g.w=gmlc_json_int(gmlc_json_obj(gv,"w"),0);
      g.h=gmlc_json_int(gmlc_json_obj(gv,"h"),0);
      g.shift=gmlc_json_int(gmlc_json_obj(gv,"shift"),0);
      g.offset=gmlc_json_int(gmlc_json_obj(gv,"offset"),0);
      if(g.h>f.em_size) f.em_size=g.h;
      if(!font_add_glyph(&f,&g)){
        snprintf(err,errcap,"out of memory while loading font glyphs");
        free(f.id); free(f.name); free(f.png_path); free(f.glyphs); free(dir); free(file);
        return 0;
      }
    }
  }
  if(!add_font(p,&f)){
    snprintf(err,errcap,"out of memory while loading font resource");
    free(f.id); free(f.name); free(f.png_path); free(f.glyphs); free(dir); free(file);
    return 0;
  }
  free(dir); free(file);
  return 1;
}

static int parse_tileset(GmlcProject *p, const GmlcResource *res, const GmlcJson *yy, char *err, size_t errcap){
  GmlcTileset t;
  memset(&t,0,sizeof(t));
  t.id=gmlc_strdup(res->id);
  t.name=gmlc_strdup(gmlc_json_str(gmlc_json_obj(yy,"name"),res->name?res->name:""));
  t.sprite_id=gmlc_project_find_sprite(p,gmlc_json_str(gmlc_json_obj(yy,"spriteId"),NULL));
  t.tile_width=gmlc_json_int(gmlc_json_obj(yy,"tilewidth"),gmlc_json_int(gmlc_json_obj(yy,"tileWidth"),16));
  t.tile_height=gmlc_json_int(gmlc_json_obj(yy,"tileheight"),gmlc_json_int(gmlc_json_obj(yy,"tileHeight"),16));
  t.border_x=gmlc_json_int(gmlc_json_obj(yy,"out_tilehborder"),gmlc_json_int(gmlc_json_obj(yy,"tilehborder"),0));
  t.border_y=gmlc_json_int(gmlc_json_obj(yy,"out_tilevborder"),gmlc_json_int(gmlc_json_obj(yy,"tilevborder"),0));
  t.columns=gmlc_json_int(gmlc_json_obj(yy,"tile_columns"),0);
  t.tile_count=gmlc_json_int(gmlc_json_obj(yy,"tile_count"),gmlc_json_int(gmlc_json_obj(yy,"tilecount"),0));
  if(t.sprite_id>=0 && t.sprite_id<p->n_sprites){
    const GmlcSprite *sp=&p->sprites[t.sprite_id];
    int pitch_x=t.tile_width + 2*t.border_x;
    int pitch_y=t.tile_height + 2*t.border_y;
    if(t.columns<=0 && pitch_x>0) t.columns=sp->width/pitch_x;
    if(t.tile_count<=0 && pitch_x>0 && pitch_y>0){
      int rows=sp->height/pitch_y;
      t.tile_count=t.columns>0 ? t.columns*rows : 0;
    }
  }
  if(t.tile_width<=0) t.tile_width=16;
  if(t.tile_height<=0) t.tile_height=16;
  if(!t.id || !t.name || !add_tileset(p,&t)){
    snprintf(err,errcap,"out of memory while loading tileset resource");
    free(t.id); free(t.name);
    return 0;
  }
  return 1;
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
  s.bbox_mode=gmlc_json_int(gmlc_json_obj(yy,"bboxmode"),0);
  s.col_kind=gmlc_json_int(gmlc_json_obj(yy,"colkind"),0);
  s.col_tolerance=gmlc_json_int(gmlc_json_obj(yy,"coltolerance"),0);
  s.sep_masks=gmlc_json_bool(gmlc_json_obj(yy,"sepmasks"),0);
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
  o.mask_id=gmlc_project_find_sprite(p,gmlc_json_str(gmlc_json_obj(yy,"maskSpriteId"),gmlc_json_str(gmlc_json_obj(yy,"spriteMaskId"),NULL)));
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
      ev.event_type=gmlc_json_int(gmlc_json_obj(je,"eventtype"),gmlc_json_int(gmlc_json_obj(je,"eventType"),0));
      ev.event_number=gmlc_json_int(gmlc_json_obj(je,"enumb"),gmlc_json_int(gmlc_json_obj(je,"eventNum"),0));
      const char *event_uuid=gmlc_json_str(gmlc_json_obj(je,"id"),NULL);
      ev.id=gmlc_strdup(event_uuid?event_uuid:"");
      const char *col_uuid=gmlc_json_str(gmlc_json_obj(je,"collisionObjectId"),NULL);
      int col_zero=(!col_uuid || !strcmp(col_uuid,"00000000-0000-0000-0000-000000000000"));
      ev.collision_object_id=(ev.event_type==4 && col_zero) ? ev.event_number : gmlc_project_find_object(p,col_uuid);
      const char *col_name_id=col_uuid;
      if(ev.event_type==4 && col_zero && ev.collision_object_id>=0 && ev.collision_object_id<p->n_objects)
        col_name_id=p->objects[ev.collision_object_id].id;
      ev.collision_id=gmlc_strdup(col_name_id?col_name_id:"");
      char file[256];
      switch(ev.event_type){
        case 0: snprintf(file,sizeof(file),"Create_%d.gml",ev.event_number); break;
        case 1: snprintf(file,sizeof(file),"Destroy_%d.gml",ev.event_number); break;
        case 2: snprintf(file,sizeof(file),"Alarm_%d.gml",ev.event_number); break;
        case 3: snprintf(file,sizeof(file),"Step_%d.gml",ev.event_number); break;
        case 4: snprintf(file,sizeof(file),"Collision_%s.gml",(event_uuid&&*event_uuid)?event_uuid:(col_name_id?col_name_id:"")); break;
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
        free(o.id); free(o.name); free(ev.id); free(ev.collision_id); free(ev.source_path); free(o.events);
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
  char *dir=gmlc_path_dirname(res->abs_path);
  const char *cc=gmlc_json_str(gmlc_json_obj(yy,"creationCodeFile"),"");
  if(cc && *cc){
    r.creation_code_path=gmlc_path_join(dir,cc);
    if(!r.creation_code_path){
      free(dir);
      snprintf(err,errcap,"out of memory while loading room creation code path");
      free_room_fields(&r);
      return 0;
    }
  }
  if(!scan_room_layers(p,&r,gmlc_json_obj(yy,"layers"),dir,err,errcap)){
    free(dir);
    free_room_fields(&r);
    return 0;
  }
  free(dir);
  if(!r.id || !r.name || !add_room(p,&r)){
    snprintf(err,errcap,"out of memory while loading room resource");
    free_room_fields(&r);
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
    case GMLC_RES_SHADER: ok=parse_shader(p,r,yy,err,errcap); break;
    case GMLC_RES_FONT: ok=parse_font(p,r,yy,err,errcap); break;
    case GMLC_RES_TILESET: ok=parse_tileset(p,r,yy,err,errcap); break;
    default: break;
  }
  gmlc_json_free(yy);
  return ok;
}

static int apply_script_order(GmlcProject *p, char *err, size_t errcap){
  if(!p->script_order_ids || p->n_script_order<=0 || p->n_scripts<=1) return 1;
  GmlcScript *ordered=(GmlcScript*)calloc((size_t)p->n_scripts,sizeof(*ordered));
  unsigned char *used=(unsigned char*)calloc((size_t)p->n_scripts,1);
  if(!ordered || !used){
    free(ordered); free(used);
    snprintf(err,errcap,"out of memory while ordering scripts");
    return 0;
  }
  int n=0;
  for(int oi=0; oi<p->n_script_order; oi++){
    const char *id=p->script_order_ids[oi];
    for(int si=0; si<p->n_scripts; si++){
      if(!used[si] && p->scripts[si].id && id && !strcmp(p->scripts[si].id,id)){
        ordered[n++]=p->scripts[si];
        used[si]=1;
        break;
      }
    }
  }
  for(int si=0; si<p->n_scripts; si++){
    if(!used[si]) ordered[n++]=p->scripts[si];
  }
  free(used);
  free(p->scripts);
  p->scripts=ordered;
  return 1;
}

static int apply_object_order(GmlcProject *p, char *err, size_t errcap){
  int count=p->n_objects;
  if(count<=1 || p->n_resource_order<=0) return 1;
  GmlcObject *ordered=(GmlcObject*)calloc((size_t)count,sizeof(*ordered));
  unsigned char *used=(unsigned char*)calloc((size_t)count,1);
  int *old_to_new=(int*)malloc((size_t)count*sizeof(*old_to_new));
  if(!ordered || !used || !old_to_new){
    free(ordered); free(used); free(old_to_new);
    snprintf(err,errcap,"out of memory while ordering objects");
    return 0;
  }
  for(int i=0;i<count;i++) old_to_new[i]=i;
  int n=0;
  for(int oi=0; oi<p->n_resource_order; oi++){
    const char *id=p->resource_order_ids[oi];
    for(int i=0;i<count;i++){
      if(!used[i] && p->objects[i].id && id && !strcmp(p->objects[i].id,id)){
        old_to_new[i]=n;
        ordered[n++]=p->objects[i];
        used[i]=1;
        break;
      }
    }
  }
  for(int i=0;i<count;i++){
    if(!used[i]){
      old_to_new[i]=n;
      ordered[n++]=p->objects[i];
    }
  }
  free(used);
  free(p->objects);
  p->objects=ordered;
  for(int i=0;i<count;i++){
    GmlcObject *o=&p->objects[i];
    if(o->parent_id>=0 && o->parent_id<count) o->parent_id=old_to_new[o->parent_id];
    for(int e=0;e<o->n_events;e++){
      if(o->events[e].collision_object_id>=0 && o->events[e].collision_object_id<count)
        o->events[e].collision_object_id=old_to_new[o->events[e].collision_object_id];
    }
  }
  free(old_to_new);
  return 1;
}

int gmlc_assets_load(GmlcProject *p, char *err, size_t errcap){
  for(int pass=0; pass<3; pass++){
    for(int i=0;i<p->n_resources;i++){
      GmlcResource *r=&p->resources[i];
      if((pass==0 && (r->kind==GMLC_RES_SPRITE || r->kind==GMLC_RES_SOUND || r->kind==GMLC_RES_SCRIPT || r->kind==GMLC_RES_SHADER || r->kind==GMLC_RES_FONT)) ||
         (pass==1 && (r->kind==GMLC_RES_OBJECT || r->kind==GMLC_RES_TILESET)) ||
         (pass==2 && r->kind==GMLC_RES_ROOM)){
        if(!load_one(p,r,err,errcap)) return 0;
      }
    }
    if(pass==0){
      if(!apply_sprite_order(p,err,errcap) ||
         !apply_sound_order(p,err,errcap) ||
         !apply_script_resource_order(p,err,errcap) ||
         !apply_script_order(p,err,errcap) ||
         !apply_shader_order(p,err,errcap) ||
         !apply_font_order(p,err,errcap)) return 0;
    } else if(pass==1){
      if(!apply_object_order(p,err,errcap) ||
         !apply_tileset_order(p,err,errcap)) return 0;
    } else if(pass==2){
      if(!apply_room_order(p,err,errcap)) return 0;
    }
  }
  return 1;
}
