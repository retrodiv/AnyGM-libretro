/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Renderer-owned asset lookup and record operations. */
#include "gml_render_backend.h"
#include "gml_render_internal.h"
#include "gml_image_codec.h"
#include "gmlc_json.h"
#include "anygm_host.h"
#include "anygm_vfs.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void backend_texture_view_reset(GmlRenderBackendTextureView *view){
  if(view) memset(view,0,sizeof(*view));
}

static char *spine_decode_text(const uint8_t *source,size_t length){
  char *text=(char*)malloc(length+1);
  if(!text) return NULL;
  uint32_t key=42;
  for(size_t i=0;i<length;i++){
    text[i]=(char)(source[i]-(uint8_t)key);
    key*=key+1;
  }
  text[length]=0;
  return text;
}

static char *spine_trim(char *text){
  if(!text) return text;
  while(*text==' ' || *text=='\t') text++;
  char *end=text+strlen(text);
  while(end>text && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r')) *--end=0;
  return text;
}

static int spine_parse_ints(const char *text,int *value,int count){
  for(int i=0;i<count;i++){
    while(*text==' ' || *text=='\t' || *text==',') text++;
    char *end=NULL;
    long parsed=strtol(text,&end,10);
    if(end==text || parsed<INT_MIN || parsed>INT_MAX) return 0;
    value[i]=(int)parsed;
    text=end;
  }
  return 1;
}

static int spine_region_push(GmlSpine *spine,const GmlSpineRegion *source){
  if(!spine || !source || !source->name) return 0;
  GmlSpineRegion *grown=(GmlSpineRegion*)realloc(
    spine->region,(size_t)(spine->region_count+1)*sizeof(*grown));
  if(!grown) return 0;
  spine->region=grown;
  spine->region[spine->region_count++]=*source;
  return 1;
}

static int spine_parse_atlas(GmlSpine *spine){
  if(!spine || !spine->atlas_text) return 0;
  char *copy=strdup(spine->atlas_text);
  if(!copy) return 0;
  GmlSpineRegion current={0};
  int have_region=0;
  char *save=NULL;
  for(char *line=strtok_r(copy,"\n",&save);line;line=strtok_r(NULL,"\n",&save)){
    char *trimmed=spine_trim(line);
    if(!*trimmed) continue;
    char *colon=strchr(trimmed,':');
    if(!colon){
      if(have_region){
        if(!spine_region_push(spine,&current)){ free(current.name); free(copy); return 0; }
        memset(&current,0,sizeof(current));
      }
      /* The first bare line names the atlas page. Every later bare line starts a region. */
      if(!strchr(trimmed,'.') || spine->region_count || have_region){
        current.name=strdup(trimmed);
        if(!current.name){ free(copy); return 0; }
        have_region=1;
      }
      continue;
    }
    if(!have_region) continue;
    *colon=0;
    char *key=spine_trim(trimmed),*value=spine_trim(colon+1);
    int values[4];
    if(!strcmp(key,"bounds") && spine_parse_ints(value,values,4)){
      current.x=values[0]; current.y=values[1];
      current.width=values[2]; current.height=values[3];
    } else if(!strcmp(key,"offsets") && spine_parse_ints(value,values,4)){
      current.offset_x=values[0]; current.offset_y=values[1];
      current.original_width=values[2]; current.original_height=values[3];
    } else if(!strcmp(key,"rotate")){
      current.rotate=!strcmp(value,"true") || atoi(value)==90;
    }
  }
  if(have_region && !spine_region_push(spine,&current)){
    free(current.name); free(copy); return 0;
  }
  free(copy);
  return spine->region_count>0;
}

static const GmlcJson *spine_json_name(const GmlcJson *object,const char *name){
  if(!object || !name) return NULL;
  return gmlc_json_obj(object,name);
}

static const char *spine_json_string(const GmlcJson *object,const char *name,
                                     const char *fallback){
  return gmlc_json_str(spine_json_name(object,name),fallback);
}

static double spine_json_number(const GmlcJson *object,const char *name,double fallback){
  return gmlc_json_num(spine_json_name(object,name),fallback);
}

static const GmlcJson *spine_named_child(const GmlcJson *object,const char *name){
  if(!object || object->type!=GMLC_JSON_OBJECT || !name) return NULL;
  for(const GmlcJson *entry=object->child;entry;entry=entry->next)
    if(entry->name && !strcmp(entry->name,name)) return entry;
  return NULL;
}

static double spine_json_max_time(const GmlcJson *value){
  if(!value) return 0;
  double maximum=0;
  if(value->type==GMLC_JSON_OBJECT){
    const GmlcJson *time=spine_json_name(value,"time");
    if(time && time->type==GMLC_JSON_NUMBER && time->n>maximum) maximum=time->n;
  }
  for(const GmlcJson *child=value->child;child;child=child->next){
    double candidate=spine_json_max_time(child);
    if(candidate>maximum) maximum=candidate;
  }
  return maximum;
}

static const GmlcJson *spine_animation(
  const GmlSpine *spine,const char *name,const char **resolved){
  const GmlcJson *root=(const GmlcJson*)spine->json;
  const GmlcJson *animations=spine_json_name(root,"animations");
  const GmlcJson *animation=NULL;
  if(name && *name) animation=spine_named_child(animations,name);
  if(!animation && animations) animation=animations->child;
  if(resolved) *resolved=animation&&animation->name?animation->name:"";
  return animation;
}

int gml_render_parse_spine(GmlRender *r,GmlSprite *sprite,uint32_t record,
                           uint32_t header){
  if(!r || !sprite || !r->win || header+20>r->win->size) return 0;
  const uint8_t *data=r->win->data;
  uint32_t cursor=header;
  uint32_t version=u32(data,cursor); cursor+=4;
  if(version<1 || version>3) return 0;
  if(version>=3){
    if(cursor+4>r->win->size || u32(data,cursor)!=1) return 0;
    cursor+=4;
  }
  if(cursor+12>r->win->size) return 0;
  uint32_t json_length=u32(data,cursor),atlas_length=u32(data,cursor+4);
  uint32_t texture_count=u32(data,cursor+8); cursor+=12;
  if(!json_length || !atlas_length || json_length>16u*1024u*1024u ||
     atlas_length>4u*1024u*1024u ||
     (uint64_t)cursor+json_length+atlas_length>r->win->size) return 0;
  GmlSpine *spine=(GmlSpine*)calloc(1,sizeof(*spine));
  if(!spine) return 0;
  spine->texture_page=-1;
  spine->json_text=spine_decode_text(data+cursor,json_length);
  spine->atlas_text=spine_decode_text(data+cursor+json_length,atlas_length);
  if(!spine->json_text || !spine->atlas_text) goto fail;
  char error[160]={0};
  spine->json=gmlc_json_parse_text(spine->json_text,"embedded skeletal sprite",
                                   error,sizeof(error));
  if(!spine->json || !spine_parse_atlas(spine)) goto fail;
  const GmlcJson *animations=spine_json_name((const GmlcJson*)spine->json,"animations");
  spine->default_animation=animations&&animations->child&&animations->child->name
    ? animations->child->name : "";
  /* Current runners serialize an ordinary whole-page texture list immediately before the
   * skeletal header. Older embedded-texture variants retain no TPAG pointer here. */
  if(header>=record+8){
    uint32_t list=record+84;
    if(list+8<=header && u32(data,list)>0 && u32(data,list)<=texture_count){
      int page=tpag_index_for_ptr(r,u32(data,list+4));
      if(page>=0) spine->texture_page=page;
    }
  }
  if(spine->texture_page<0) goto fail;
  sprite->spine=spine;
  return 1;
fail:
  gml_render_free_spine(spine);
  return 0;
}

void gml_render_free_spine(GmlSpine *spine){
  if(!spine) return;
  if(spine->json) gmlc_json_free((GmlcJson*)spine->json);
  for(int i=0;i<spine->region_count;i++) free(spine->region[i].name);
  free(spine->region);
  free(spine->json_text);
  free(spine->atlas_text);
  free(spine);
}

static const GmlSpineRegion *spine_find_region(const GmlSpine *spine,const char *name){
  if(!spine || !name) return NULL;
  for(int i=0;i<spine->region_count;i++)
    if(spine->region[i].name && !strcmp(spine->region[i].name,name))
      return &spine->region[i];
  return NULL;
}

static const GmlcJson *spine_find_named_array_item(const GmlcJson *array,const char *name){
  if(!array || !name) return NULL;
  for(const GmlcJson *entry=array->child;entry;entry=entry->next){
    const char *candidate=spine_json_string(entry,"name","");
    if(!strcmp(candidate,name)) return entry;
  }
  return NULL;
}

static double spine_timeline_value(const GmlcJson *timeline,double time,
                                   const char *field,double fallback){
  if(!timeline || timeline->type!=GMLC_JSON_ARRAY || !timeline->child) return fallback;
  const GmlcJson *first=timeline->child,*left=first,*right=NULL;
  for(const GmlcJson *key=first->next;key;key=key->next){
    if(spine_json_number(key,"time",0)>time){ right=key; break; }
    left=key;
  }
  double a=spine_json_number(left,field,fallback);
  if(!right) return a;
  const GmlcJson *curve=spine_json_name(left,"curve");
  if(curve && curve->type==GMLC_JSON_STRING && curve->s &&
     !strcmp(curve->s,"stepped")) return a;
  double t0=spine_json_number(left,"time",0),t1=spine_json_number(right,"time",t0);
  if(t1<=t0) return a;
  double amount=(time-t0)/(t1-t0);
  if(amount<0) amount=0; else if(amount>1) amount=1;
  double b=spine_json_number(right,field,fallback);
  return a+(b-a)*amount;
}

static const char *spine_timeline_attachment(const GmlcJson *timeline,double time,
                                             const char *fallback){
  if(!timeline || timeline->type!=GMLC_JSON_ARRAY) return fallback;
  const char *attachment=fallback;
  for(const GmlcJson *key=timeline->child;key;key=key->next){
    if(spine_json_number(key,"time",0)>time) break;
    attachment=spine_json_string(key,"name","");
  }
  return attachment;
}

static int spine_override(const GmlRender *r,const char *kind,const char *bone,
                          const char *field,double *value){
  if(!r || !r->skeleton_state_active || !r->skeleton_state.bone) return 0;
  return r->skeleton_state.bone(
    r->skeleton_state.context,kind,bone,field,value);
}

static const GmlcJson *spine_skin(const GmlSpine *spine,const char *requested){
  const GmlcJson *skins=spine_json_name((const GmlcJson*)spine->json,"skins");
  if(!skins) return NULL;
  if(skins->type==GMLC_JSON_ARRAY){
    const GmlcJson *skin=(requested&&*requested)
      ? spine_find_named_array_item(skins,requested) : NULL;
    if(!skin) skin=spine_find_named_array_item(skins,"default");
    return skin?skin:skins->child;
  }
  if(skins->type==GMLC_JSON_OBJECT){
    const GmlcJson *skin=(requested&&*requested)?spine_named_child(skins,requested):NULL;
    if(!skin) skin=spine_named_child(skins,"default");
    return skin?skin:skins->child;
  }
  return NULL;
}

static const GmlcJson *spine_skin_attachments(const GmlcJson *skin){
  const GmlcJson *attachments=spine_json_name(skin,"attachments");
  return attachments?attachments:skin;
}

static uint32_t spine_hex_colour(const char *text,double *alpha){
  if(alpha) *alpha=1;
  if(!text || strlen(text)<6) return 0xFFFFFFu;
  char part[3]={0};
  part[0]=text[0]; part[1]=text[1]; unsigned red=(unsigned)strtoul(part,NULL,16);
  part[0]=text[2]; part[1]=text[3]; unsigned green=(unsigned)strtoul(part,NULL,16);
  part[0]=text[4]; part[1]=text[5]; unsigned blue=(unsigned)strtoul(part,NULL,16);
  if(alpha && strlen(text)>=8){
    part[0]=text[6]; part[1]=text[7];
    *alpha=(double)strtoul(part,NULL,16)/255.0;
  }
  return red|(green<<8)|(blue<<16);
}

static uint32_t spine_multiply_colour(uint32_t first,uint32_t second){
  unsigned r=((first&255u)*(second&255u)+127u)/255u;
  unsigned g=(((first>>8)&255u)*((second>>8)&255u)+127u)/255u;
  unsigned b=(((first>>16)&255u)*((second>>16)&255u)+127u)/255u;
  return r|(g<<8)|(b<<16);
}

static int spine_pose_bones(GmlRender *r,const GmlSpine *spine,
                            GmlSpinePoseBone *pose,int capacity){
  const GmlcJson *root=(const GmlcJson*)spine->json;
  const GmlcJson *bones=spine_json_name(root,"bones");
  int count=gmlc_json_len(bones);
  if(count<=0 || count>capacity) return 0;
  const char *animation_name=r->skeleton_state_active?r->skeleton_state.animation:NULL;
  const GmlcJson *animation=spine_animation(spine,animation_name,NULL);
  const GmlcJson *animation_bones=spine_json_name(animation,"bones");
  double duration=spine_json_max_time(animation);
  double time=r->skeleton_state_active?r->skeleton_state.time:0;
  if(duration>0){
    time=fmod(time,duration);
    if(time<0) time+=duration;
  }
  int index=0;
  for(const GmlcJson *bone=bones->child;bone && index<count;bone=bone->next,index++){
    GmlSpinePoseBone *out=&pose[index];
    memset(out,0,sizeof(*out));
    out->name=spine_json_string(bone,"name","");
    out->parent=-1;
    const char *parent=spine_json_string(bone,"parent","");
    for(int p=0;p<index;p++) if(!strcmp(pose[p].name,parent)){ out->parent=p; break; }
    out->x=spine_json_number(bone,"x",0);
    out->y=spine_json_number(bone,"y",0);
    out->rotation=spine_json_number(bone,"rotation",0);
    out->scale_x=spine_json_number(bone,"scaleX",1);
    out->scale_y=spine_json_number(bone,"scaleY",1);
    spine_override(r,"bone_data",out->name,"x",&out->x);
    spine_override(r,"bone_data",out->name,"y",&out->y);
    spine_override(r,"bone_data",out->name,"angle",&out->rotation);
    spine_override(r,"bone_data",out->name,"xscale",&out->scale_x);
    spine_override(r,"bone_data",out->name,"yscale",&out->scale_y);
    const GmlcJson *timeline=spine_named_child(animation_bones,out->name);
    out->rotation+=spine_timeline_value(spine_json_name(timeline,"rotate"),time,"angle",0);
    out->x+=spine_timeline_value(spine_json_name(timeline,"translate"),time,"x",0);
    out->y+=spine_timeline_value(spine_json_name(timeline,"translate"),time,"y",0);
    out->scale_x*=spine_timeline_value(spine_json_name(timeline,"scale"),time,"x",1);
    out->scale_y*=spine_timeline_value(spine_json_name(timeline,"scale"),time,"y",1);
    spine_override(r,"bone_state",out->name,"x",&out->x);
    spine_override(r,"bone_state",out->name,"y",&out->y);
    spine_override(r,"bone_state",out->name,"angle",&out->rotation);
    spine_override(r,"bone_state",out->name,"xscale",&out->scale_x);
    spine_override(r,"bone_state",out->name,"yscale",&out->scale_y);
    double radians=out->rotation*M_PI/180.0;
    double la=cos(radians)*out->scale_x,lc=sin(radians)*out->scale_x;
    double lb=-sin(radians)*out->scale_y,ld=cos(radians)*out->scale_y;
    if(out->parent<0){
      out->world_x=out->x; out->world_y=out->y;
      out->a=la; out->b=lb; out->c=lc; out->d=ld;
    } else {
      const GmlSpinePoseBone *parent_pose=&pose[out->parent];
      out->world_x=parent_pose->a*out->x+parent_pose->b*out->y+parent_pose->world_x;
      out->world_y=parent_pose->c*out->x+parent_pose->d*out->y+parent_pose->world_y;
      out->a=parent_pose->a*la+parent_pose->b*lc;
      out->b=parent_pose->a*lb+parent_pose->b*ld;
      out->c=parent_pose->c*la+parent_pose->d*lc;
      out->d=parent_pose->c*lb+parent_pose->d*ld;
    }
  }
  return count;
}

int gml_render_spine_build_items(
  GmlRender *r,const GmlSprite *sprite,double x,double y,double xscale,
  double yscale,double rotation,GmlSpineDrawItem *items,int capacity){
  if(!r || !sprite || !sprite->spine || !items || capacity<=0) return 0;
  const GmlSpine *spine=sprite->spine;
  if(spine->texture_page<0 || spine->texture_page>=r->n_tpag) return 0;
  const GmlTpag *page=&r->tpag[spine->texture_page];
  if(page->atlas<0 || page->atlas>=r->n_atlas) return 0;
  const GmlAtlas *atlas=&r->atlas[page->atlas];
  if(atlas->w<=0 || atlas->h<=0) return 0;
  GmlSpinePoseBone pose[256];
  int bone_count=spine_pose_bones(r,spine,pose,256);
  if(bone_count<=0) return 0;
  const GmlcJson *root=(const GmlcJson*)spine->json;
  const GmlcJson *slots=spine_json_name(root,"slots");
  const char *animation_name=r->skeleton_state_active?r->skeleton_state.animation:NULL;
  const GmlcJson *animation=spine_animation(spine,animation_name,NULL);
  const GmlcJson *animation_slots=spine_json_name(animation,"slots");
  double animation_time=r->skeleton_state_active?r->skeleton_state.time:0;
  double animation_duration=spine_json_max_time(animation);
  if(animation_duration>0){
    animation_time=fmod(animation_time,animation_duration);
    if(animation_time<0) animation_time+=animation_duration;
  }
  const char *skin_name=r->skeleton_state_active?r->skeleton_state.skin:NULL;
  const GmlcJson *skin=spine_skin(spine,skin_name);
  const GmlcJson *attachments=spine_skin_attachments(skin);
  double instance_radians=rotation*M_PI/180.0;
  double instance_cos=cos(instance_radians),instance_sin=sin(instance_radians);
  int count=0;
  for(const GmlcJson *slot=slots?slots->child:NULL;slot && count<capacity;slot=slot->next){
    const char *slot_name=spine_json_string(slot,"name","");
    const char *attachment=spine_json_string(slot,"attachment","");
    const GmlcJson *slot_timeline=spine_named_child(animation_slots,slot_name);
    attachment=spine_timeline_attachment(
      spine_json_name(slot_timeline,"attachment"),animation_time,attachment);
    if(r->skeleton_state_active && r->skeleton_state.attachment){
      const char *override=r->skeleton_state.attachment(
        r->skeleton_state.context,slot_name);
      if(override) attachment=override;
    }
    if(!attachment || !*attachment) continue;
    const char *bone_name=spine_json_string(slot,"bone","");
    int bone=-1;
    for(int i=0;i<bone_count;i++) if(!strcmp(pose[i].name,bone_name)){ bone=i; break; }
    if(bone<0) continue;
    const GmlcJson *slot_attachments=spine_named_child(attachments,slot_name);
    const GmlcJson *definition=spine_named_child(slot_attachments,attachment);
    if(!definition) continue;
    const char *path=spine_json_string(definition,"path",attachment);
    const GmlSpineRegion *region=spine_find_region(spine,path);
    if(!region || region->width<=0 || region->height<=0 ||
       region->original_width<=0 || region->original_height<=0) continue;
    double attachment_x=spine_json_number(definition,"x",0);
    double attachment_y=spine_json_number(definition,"y",0);
    double attachment_rotation=spine_json_number(definition,"rotation",0)*M_PI/180.0;
    double attachment_scale_x=spine_json_number(definition,"scaleX",1);
    double attachment_scale_y=spine_json_number(definition,"scaleY",1);
    double attachment_width=spine_json_number(
      definition,"width",(double)region->original_width);
    double attachment_height=spine_json_number(
      definition,"height",(double)region->original_height);
    double region_scale_x=attachment_width/region->original_width*attachment_scale_x;
    double region_scale_y=attachment_height/region->original_height*attachment_scale_y;
    double local_x=-attachment_width*.5*attachment_scale_x+
      region->offset_x*region_scale_x;
    double local_y=-attachment_height*.5*attachment_scale_y+
      region->offset_y*region_scale_y;
    double local_x2=local_x+region->width*region_scale_x;
    double local_y2=local_y+region->height*region_scale_y;
    double local[4][2]={{local_x,local_y},{local_x2,local_y},
                        {local_x2,local_y2},{local_x,local_y2}};
    double ar_cos=cos(attachment_rotation),ar_sin=sin(attachment_rotation);
    GmlSpineDrawItem *item=&items[count];
    memset(item,0,sizeof(*item));
    item->texture_page=spine->texture_page;
    for(int vertex=0;vertex<4;vertex++){
      double lx=local[vertex][0]*ar_cos-local[vertex][1]*ar_sin+attachment_x;
      double ly=local[vertex][0]*ar_sin+local[vertex][1]*ar_cos+attachment_y;
      double world_x=pose[bone].a*lx+pose[bone].b*ly+pose[bone].world_x;
      double world_y=pose[bone].c*lx+pose[bone].d*ly+pose[bone].world_y;
      double screen_x=world_x*xscale,screen_y=-world_y*yscale;
      item->x[vertex]=x+screen_x*instance_cos+screen_y*instance_sin;
      item->y[vertex]=y-screen_x*instance_sin+screen_y*instance_cos;
    }
    double atlas_x=page->sx+region->x,atlas_y=page->sy+region->y;
    if(region->rotate){
      double physical_width=region->height,physical_height=region->width;
      item->u[0]=atlas_x/atlas->w;                  item->v[0]=atlas_y/atlas->h;
      item->u[1]=atlas_x/atlas->w;                  item->v[1]=(atlas_y+physical_height)/atlas->h;
      item->u[2]=(atlas_x+physical_width)/atlas->w; item->v[2]=(atlas_y+physical_height)/atlas->h;
      item->u[3]=(atlas_x+physical_width)/atlas->w; item->v[3]=atlas_y/atlas->h;
    } else {
      item->u[0]=atlas_x/atlas->w;                 item->v[0]=(atlas_y+region->height)/atlas->h;
      item->u[1]=(atlas_x+region->width)/atlas->w; item->v[1]=(atlas_y+region->height)/atlas->h;
      item->u[2]=(atlas_x+region->width)/atlas->w; item->v[2]=atlas_y/atlas->h;
      item->u[3]=atlas_x/atlas->w;                 item->v[3]=atlas_y/atlas->h;
    }
    double slot_alpha=1,attachment_alpha=1;
    uint32_t slot_colour=spine_hex_colour(spine_json_string(slot,"color","ffffffff"),
                                          &slot_alpha);
    uint32_t attachment_colour=spine_hex_colour(
      spine_json_string(definition,"color","ffffffff"),&attachment_alpha);
    item->colour=spine_multiply_colour(slot_colour,attachment_colour);
    item->alpha=slot_alpha*attachment_alpha;
    count++;
  }
  return count;
}

void gml_render_skeleton_state_set(GmlRender *r,
                                   const GmlRenderSkeletonState *state){
  if(!r) return;
  if(state){
    r->skeleton_state=*state;
    r->skeleton_state_active=1;
  } else {
    memset(&r->skeleton_state,0,sizeof(r->skeleton_state));
    r->skeleton_state_active=0;
  }
}

int gml_render_sprite_is_skeleton(const GmlRender *r,int sprite){
  return r && sprite>=0 && sprite<r->n_spr && r->spr[sprite].spine!=NULL;
}

double gml_render_skeleton_animation_duration(const GmlRender *r,int sprite,
                                              const char *animation){
  if(!gml_render_sprite_is_skeleton(r,sprite)) return 0;
  const GmlcJson *value=spine_animation(r->spr[sprite].spine,animation,NULL);
  return spine_json_max_time(value);
}

int gml_render_skeleton_bone_setup(const GmlRender *r,int sprite,
                                   const char *bone,const char *field,
                                   double *value){
  if(!value || !bone || !field || !gml_render_sprite_is_skeleton(r,sprite)) return 0;
  const GmlcJson *root=(const GmlcJson*)r->spr[sprite].spine->json;
  const GmlcJson *bones=spine_json_name(root,"bones");
  const GmlcJson *entry=spine_find_named_array_item(bones,bone);
  if(!entry) return 0;
  if(!strcmp(field,"x")) *value=spine_json_number(entry,"x",0);
  else if(!strcmp(field,"y")) *value=spine_json_number(entry,"y",0);
  else if(!strcmp(field,"angle")) *value=spine_json_number(entry,"rotation",0);
  else if(!strcmp(field,"xscale")) *value=spine_json_number(entry,"scaleX",1);
  else if(!strcmp(field,"yscale")) *value=spine_json_number(entry,"scaleY",1);
  else return 0;
  return 1;
}

int gml_render_sprite_metrics(const GmlRender *R,int sprite,
                              GmlRenderSpriteMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  if(!R || sprite<0 || sprite>=R->n_spr) return 0;
  const GmlSprite *source=&R->spr[sprite];
  if(metrics){
    metrics->width=source->w; metrics->height=source->h;
    metrics->origin_x=source->originx; metrics->origin_y=source->originy;
    metrics->frame_count=source->n_frames;
    metrics->collision_left=source->ml; metrics->collision_top=source->mt;
    metrics->collision_right=source->mr; metrics->collision_bottom=source->mb;
    metrics->collision_box=source->collision_kind==1;
    metrics->playback_speed=source->playback_speed;
    metrics->playback_speed_type=source->playback_speed_type;
    metrics->playback_speed_valid=source->playback_speed_valid;
    metrics->name=source->name;
  }
  return 1;
}

int gml_render_sprite_set_playback(GmlRender *R,int sprite,
                                   double speed,int speed_type){
  if(!R || sprite<0 || sprite>=R->n_spr) return 0;
  R->spr[sprite].playback_speed=(float)speed;
  R->spr[sprite].playback_speed_type=speed_type==0?0:1;
  R->spr[sprite].playback_speed_valid=1;
  return 1;
}

int gml_render_sprite_texture_handle(int sprite,int image){
  if(sprite<0) return -1;
  return (int)(GML_TEX_SPR_TAG|((sprite&0xFFFF)<<10)|(image&0x3FF));
}
int gml_render_atlas_texture_handle(const GmlRender *r,int atlas){
  return r && atlas>=0 && atlas<r->n_atlas && atlas<=0x00FFFFFF
    ?(int)(GML_TEX_ATLAS_TAG|(uint32_t)atlas):-1;
}

int gml_render_surface_texture_handle(int surface){
  if(surface<0) return -1;
  return (int)(GML_TEX_SURF_TAG|((unsigned)surface&0xFFFFu));
}

int gml_render_background_metrics(const GmlRender *R,int background,
                                  GmlRenderBackgroundMetrics *metrics){
  if(metrics){ memset(metrics,0,sizeof(*metrics)); metrics->texture_page=-1; metrics->atlas=-1; }
  if(!R || background<0 || background>=R->n_bg) return 0;
  const GmlBg *source=&R->bg[background];
  if(metrics){
    metrics->tile_width=source->tile_w; metrics->tile_height=source->tile_h;
    metrics->tile_border_x=source->tile_border_x;
    metrics->tile_border_y=source->tile_border_y;
    metrics->tile_separation_x=source->tile_separation_x;
    metrics->tile_separation_y=source->tile_separation_y;
    metrics->tile_columns=source->tile_columns;
    metrics->transparent=source->transparent;
    metrics->smooth=source->smooth;
    metrics->preload=source->preload;
    metrics->name=source->name;
    if(source->tpag>=0 && source->tpag<R->n_tpag){
      const GmlTpag *page=&R->tpag[source->tpag];
      metrics->texture_page=source->tpag; metrics->atlas=page->atlas;
      metrics->packed_x=page->sx; metrics->packed_y=page->sy;
      metrics->packed_width=page->sw; metrics->packed_height=page->sh;
      metrics->trim_x=page->tx; metrics->trim_y=page->ty;
      metrics->declared_width=page->bw; metrics->declared_height=page->bh;
      metrics->logical_width=page->bw?page->bw:page->sw;
      metrics->logical_height=page->bh?page->bh:page->sh;
      if(page->atlas>=0 && page->atlas<R->n_atlas){
        const GmlAtlas *atlas=&R->atlas[page->atlas];
        metrics->content_texture=atlas->blob!=0 || atlas->external_blob!=NULL;
      }
    }
  }
  return 1;
}

int gml_render_background_texture_handle(const GmlRender *R,int background){
  return R && background>=0 && background<R->n_bg
    ? (int)(GML_TEX_BG_TAG|((unsigned)background&0x00FFFFFFu)) : -1;
}

int gml_render_background_tile_animation_frame(const GmlRender *R,int background,
                                                double elapsed_seconds){
  if(!R || background<0 || background>=R->n_bg || !isfinite(elapsed_seconds) ||
     elapsed_seconds<=0.0) return 0;
  const GmlBg *source=&R->bg[background];
  if(source->tile_items_per_tile<=1 || !source->tile_frame_length_us) return 0;
  double frame_seconds=(double)source->tile_frame_length_us/1000000.0;
  double period=frame_seconds*source->tile_items_per_tile;
  if(!isfinite(frame_seconds) || frame_seconds<=0.0 ||
     !isfinite(period) || period<=0.0) return 0;
  double within=fmod(elapsed_seconds,period);
  if(within<0.0) within+=period;
  int frame=(int)floor(within/frame_seconds);
  return frame>=source->tile_items_per_tile?source->tile_items_per_tile-1:frame;
}

int gml_render_background_tile_source_index(const GmlRender *R,int background,
                                             int tile_index,int animation_frame){
  if(!R || background<0 || background>=R->n_bg || tile_index<0) return -1;
  const GmlBg *source=&R->bg[background];
  if(source->tile_ids && source->tile_items_per_tile>0 && tile_index<source->tile_count){
    int frame=animation_frame%source->tile_items_per_tile;
    if(frame<0) frame+=source->tile_items_per_tile;
    const uint8_t *id=source->tile_ids+
      ((size_t)tile_index*(size_t)source->tile_items_per_tile+(size_t)frame)*4u;
    return (int)((uint32_t)id[0]|((uint32_t)id[1]<<8)|
                 ((uint32_t)id[2]<<16)|((uint32_t)id[3]<<24));
  }
  return source->tile_ids?tile_index:tile_index-1;
}

int gml_render_tileset_layout(const GmlRender *R,int background,
                              int fallback_width,int fallback_height,
                              GmlRenderTilesetLayout *layout){
  GmlRenderBackgroundMetrics metrics;
  if(!layout || !gml_render_background_metrics(R,background,&metrics) ||
     metrics.texture_page<0) return 0;
  int width=metrics.tile_width>0?metrics.tile_width:fallback_width;
  int height=metrics.tile_height>0?metrics.tile_height:fallback_height;
  int64_t pitch_x=(int64_t)width+2LL*metrics.tile_border_x+metrics.tile_separation_x;
  int64_t pitch_y=(int64_t)height+2LL*metrics.tile_border_y+metrics.tile_separation_y;
  if(width<=0 || height<=0 || pitch_x<=0 || pitch_x>INT_MAX ||
     pitch_y<=0 || pitch_y>INT_MAX || metrics.tile_border_x<0 || metrics.tile_border_y<0 ||
     metrics.logical_width<=0 || metrics.logical_height<=0) return 0;
  int columns=metrics.tile_columns>0?metrics.tile_columns:metrics.logical_width/(int)pitch_x;
  if(columns<=0) return 0;
  *layout=(GmlRenderTilesetLayout){background,width,height,
    metrics.tile_border_x,metrics.tile_border_y,(int)pitch_x,(int)pitch_y,columns,
    metrics.logical_width,metrics.logical_height};
  return 1;
}

int gml_render_tileset_source(const GmlRender *R,const GmlRenderTilesetLayout *layout,
                              uint32_t datum,int frame,GmlRenderTileSource *source){
  int index=(int)(datum&0x7ffffu);
  if(!R || !layout || !source || index==0 || layout->background<0 ||
     layout->background>=R->n_bg || layout->columns<=0) return 0;
  int mapped=gml_render_background_tile_source_index(R,layout->background,index,frame);
  /* An animated empty frame is not atlas cell zero. Unmapped legacy layouts,
   * however, legitimately use that cell for their first nonempty tile. */
  if(mapped<0 || (mapped==0 && R->bg[layout->background].tile_ids)) return 0;
  int64_t x=(int64_t)(mapped%layout->columns)*layout->pitch_x+layout->border_x;
  int64_t y=(int64_t)(mapped/layout->columns)*layout->pitch_y+layout->border_y;
  if(x<0 || y<0 || x>=layout->source_width || y>=layout->source_height) return 0;
  int width=layout->width,height=layout->height;
  if(width>layout->source_width-x) width=(int)(layout->source_width-x);
  if(height>layout->source_height-y) height=(int)(layout->source_height-y);
  if(width<=0 || height<=0) return 0;
  *source=(GmlRenderTileSource){(int)x,(int)y,width,height};
  return 1;
}

int gml_render_font_metrics(const GmlRender *R,int font,
                            GmlRenderFontMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  if(!R) return 0;
  const GmlFont *source;
  if(font<0) source=&R->default_font;
  else {
    if(font>=R->n_fonts) return 0;
    source=&R->fonts[font];
  }
  if(metrics){
    metrics->line_height=source->line_height;
    metrics->ascender=source->ascender;
    metrics->ascender_offset=source->ascender_offset;
    metrics->sdf_spread=source->sdf_spread;
    metrics->bold=source->bold;
    metrics->italic=source->italic;
    metrics->sprite=source->sprite;
    metrics->first=source->first;
    metrics->proportional=source->prop;
    metrics->separation=source->sep;
    metrics->sprite_backed=!source->real;
    metrics->glyph_count=source->n_glyphs;
  }
  return 1;
}

int gml_render_font_glyph_metrics(const GmlRender *R,int font,int glyph,
                                  GmlRenderFontGlyphMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  if(!R || font<0 || font>=R->n_fonts || glyph<0 ||
     glyph>=R->fonts[font].n_glyphs || !R->fonts[font].glyphs) return 0;
  const GmlGlyph *source=&R->fonts[font].glyphs[glyph];
  if(metrics){
    metrics->character=source->ch;
    metrics->x=source->sx; metrics->y=source->sy;
    metrics->width=source->w; metrics->height=source->h;
    metrics->shift=source->shift; metrics->offset=source->offset;
  }
  return 1;
}

int gml_render_font_texture_handle(const GmlRender *R,int font){
  if(!R || font<0 || font>=R->n_fonts) return -1;
  const GmlFont *source=&R->fonts[font];
  if(source->real && source->atlas>=0 && source->atlas<R->n_atlas)
    return (int)(GML_TEX_FONT_TAG|((unsigned)font&0x00FFFFFFu));
  if(source->sprite>=0) return gml_render_sprite_texture_handle(source->sprite,0);
  return -1;
}

int gml_render_font_exists(const GmlRender *R,int font){
  if(!R || font<0 || font>=R->n_fonts) return 0;
  const GmlFont *source=&R->fonts[font];
  return source->real || source->sprite>=0;
}

static void backend_texture_view_page(GmlRenderBackendTextureView *view,
                                      const GmlTpag *page,const GmlAtlas *atlas,
                                      int full_atlas){
  if(!view) return;
  view->pixel_kind=GML_RENDER_BACKEND_PIXELS_RGBA;
  view->trim_x=page->tx; view->trim_y=page->ty;
  view->width=full_atlas?atlas->w:page->sw;
  view->height=full_atlas?atlas->h:page->sh;
  view->stride=atlas->w;
  view->source_x=full_atlas?0:page->sx;
  view->source_y=full_atlas?0:page->sy;
  view->full_width=atlas->w; view->full_height=atlas->h;
  view->atlas_index=page->atlas;
  view->rgba=atlas->px;
}

int gml_render_backend_texture_view(GmlRender *R,int handle,int full_atlas,
                                    GmlRenderBackendTextureView *view){
  backend_texture_view_reset(view);
  if(!R) return 0;
  uint32_t encoded=(uint32_t)handle,kind=encoded&GML_TEX_KIND_MASK;
  if(kind==GML_TEX_ATLAS_TAG)
    return gml_render_backend_atlas_view(R,(int)(encoded&0x00FFFFFFu),view);
  if(kind==GML_TEX_SPR_TAG){
    int spr=(int)((encoded>>10)&0xFFFF),img=(int)(encoded&0x3FF);
    gml_render_warm_sprite(R,spr);
    if(spr<0 || spr>=R->n_spr) return 0;
    GmlSprite *sprite=&R->spr[spr];
    if(view){
      view->resource_index=spr;
      view->logical_width=sprite->w; view->logical_height=sprite->h;
      view->origin_x=sprite->originx; view->origin_y=sprite->originy;
    }
    if(sprite->runtime_rgba){
      int frames=sprite->n_frames>0?sprite->n_frames:1;
      int sub=((img%frames)+frames)%frames;
      if(view){
        view->pixel_kind=GML_RENDER_BACKEND_PIXELS_RGBA;
        view->runtime=1;
        view->width=view->full_width=sprite->w;
        view->height=view->full_height=sprite->h;
        view->stride=sprite->w;
        if(sprite->w>0 && sprite->h>0)
          view->rgba=sprite->runtime_rgba+(size_t)sub*sprite->w*sprite->h*4;
      }
      return 1;
    }
    if(sprite->n_frames<=0 || !sprite->frame) return 0;
    int sub=((img%sprite->n_frames)+sprite->n_frames)%sprite->n_frames;
    int page_index=sprite->frame[sub];
    if(page_index<0 || page_index>=R->n_tpag) return 0;
    GmlTpag *page=&R->tpag[page_index];
    if(page->atlas<0 || page->atlas>=R->n_atlas) return 0;
    GmlAtlas *atlas=&R->atlas[page->atlas];
    if(!atlas->px || atlas->w<=0 || atlas->h<=0) return 0;
    if(view){
      view->page_index=page_index;
      backend_texture_view_page(view,page,atlas,full_atlas);
    }
    return 1;
  }
  if(kind==GML_TEX_SURF_TAG){
    int surface=(int)(encoded&0xFFFF),width=0,height=0;
    const uint32_t *pixels=gml_surface_pixels_read(R,surface,&width,&height);
    if(!pixels || width<=0 || height<=0) return 0;
    if(view){
      view->pixel_kind=GML_RENDER_BACKEND_PIXELS_XRGB;
      view->logical_width=view->width=view->full_width=width;
      view->logical_height=view->height=view->full_height=height;
      view->stride=width; view->resource_index=surface; view->xrgb=pixels;
    }
    return 1;
  }
  if(kind==GML_TEX_FONT_TAG){
    int font=(int)(encoded&0x00FFFFFFu);
    if(font<0 || font>=R->n_fonts || !R->fonts[font].real) return 0;
    int atlas=R->fonts[font].atlas;
    int ok=gml_render_backend_atlas_view(R,atlas,view);
    if(ok && view) view->resource_index=font;
    return ok;
  }
  if(kind!=GML_TEX_BG_TAG){
    if(anygm_host_development_setting(R->win?R->win->host:NULL,"GML_LOG_D3D"))
      anygm_host_logf(R->win?R->win->host:NULL,ANYGM_LOG_DEBUG,
                      "[d3d] invalid texture handle %d\n",handle);
    return 0;
  }
  int background=(int)(encoded&0x00FFFFFF);
  gml_render_warm_bg(R,background);
  if(background<0 || background>=R->n_bg) return 0;
  int page_index=R->bg[background].tpag;
  if(page_index<0 || page_index>=R->n_tpag) return 0;
  GmlTpag *page=&R->tpag[page_index];
  if(page->atlas<0 || page->atlas>=R->n_atlas) return 0;
  GmlAtlas *atlas=&R->atlas[page->atlas];
  if(!atlas->px || page->sw<=0 || page->sh<=0) return 0;
  if(view){
    view->resource_index=background; view->page_index=page_index;
    view->logical_width=page->bw?page->bw:page->sw;
    view->logical_height=page->bh?page->bh:page->sh;
    backend_texture_view_page(view,page,atlas,full_atlas);
  }
  return 1;
}

int gml_render_texture_metrics(GmlRender *R,int handle,
                               GmlRenderTextureMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  GmlRenderBackendTextureView view;
  if(!metrics || !gml_render_backend_texture_view(R,handle,0,&view)) return 0;
  uint32_t kind=(uint32_t)handle&GML_TEX_KIND_MASK;
  if(kind==GML_TEX_SPR_TAG) metrics->kind=GML_RENDER_TEXTURE_SPRITE;
  else if(kind==GML_TEX_SURF_TAG) metrics->kind=GML_RENDER_TEXTURE_SURFACE;
  else if(kind==GML_TEX_BG_TAG) metrics->kind=GML_RENDER_TEXTURE_BACKGROUND;
  else if(kind==GML_TEX_FONT_TAG) metrics->kind=GML_RENDER_TEXTURE_FONT;
  else if(kind==GML_TEX_ATLAS_TAG) metrics->kind=GML_RENDER_TEXTURE_ATLAS;
  else return 0;
  metrics->runtime=view.runtime;
  metrics->atlas_backed=view.pixel_kind==GML_RENDER_BACKEND_PIXELS_RGBA &&
    !view.runtime;
  metrics->logical_width=view.logical_width;
  metrics->logical_height=view.logical_height;
  metrics->trim_x=view.trim_x;
  metrics->trim_y=view.trim_y;
  metrics->width=view.width;
  metrics->height=view.height;
  metrics->source_x=view.source_x;
  metrics->source_y=view.source_y;
  metrics->full_width=view.full_width;
  metrics->full_height=view.full_height;
  return 1;
}

int gml_render_backend_atlas_view(GmlRender *R,int atlas_index,
                                  GmlRenderBackendTextureView *view){
  backend_texture_view_reset(view);
  if(!R || atlas_index<0 || atlas_index>=R->n_atlas ||
     !gml_render_warm_atlas(R,atlas_index)) return 0;
  GmlAtlas *atlas=&R->atlas[atlas_index];
  if(!atlas->px || atlas->w<=0 || atlas->h<=0) return 0;
  if(view){
    view->pixel_kind=GML_RENDER_BACKEND_PIXELS_RGBA;
    view->logical_width=view->width=view->full_width=atlas->w;
    view->logical_height=view->height=view->full_height=atlas->h;
    view->stride=atlas->w; view->atlas_index=atlas_index; view->rgba=atlas->px;
  }
  return 1;
}

int gml_sprite_exists(GmlRender *r, int sprite){
  return r && sprite>=0 && sprite<r->n_spr &&
         (r->spr[sprite].n_frames>0 || r->spr[sprite].spine);
}
int gml_sprite_frames(GmlRender *r, int sprite){
  return r && sprite>=0 && sprite<r->n_spr ? r->spr[sprite].n_frames : 0;
}
static void sprite_backup(GmlSprite *s){
  if(s->base_valid) return;
  s->base_valid=1;
  s->base_originx=s->originx; s->base_originy=s->originy;
  s->base_w=s->w; s->base_h=s->h; s->base_n_frames=s->n_frames;
  s->base_ml=s->ml; s->base_mr=s->mr; s->base_mt=s->mt; s->base_mb=s->mb;
  s->base_mask=s->mask; s->base_mask_rowb=s->mask_rowb; s->base_mask_count=s->mask_count;
  s->base_collision_kind=s->collision_kind; s->base_collision_tolerance=s->collision_tolerance;
}
static void sprite_restore_base(GmlSprite *s){
  if(!s->base_valid) return;
  s->originx=s->base_originx; s->originy=s->base_originy;
  s->w=s->base_w; s->h=s->base_h; s->n_frames=s->base_n_frames;
  s->ml=s->base_ml; s->mr=s->base_mr; s->mt=s->base_mt; s->mb=s->base_mb;
  s->mask=s->base_mask; s->mask_rowb=s->base_mask_rowb; s->mask_count=s->base_mask_count;
  s->collision_kind=s->base_collision_kind; s->collision_tolerance=s->base_collision_tolerance;
  s->base_valid=0;
}
static void sprite_set_runtime_rgba(GmlSprite *s, uint8_t *rgba, int w, int h, int frames, int xorig, int yorig, int extra){
  if(frames<1) frames=1;
  if(!extra) sprite_backup(s);
  gml_render_sprite_cache_free(s);
  gml_render_sprite_state_cache_clear(s);
  free(s->runtime_rgba);
  free(s->runtime_mask);
  free(s->runtime_row_min);
  free(s->runtime_row_max);
  free(s->runtime_source_path);
  s->runtime_rgba=rgba;
  s->runtime_mask=NULL;
  s->runtime_row_min=NULL;
  s->runtime_row_max=NULL;
  s->runtime_source_path=NULL;
  s->runtime_source_imgnum=0;
  s->runtime_source_removeback=0;
  s->runtime_owned=1;
  s->runtime_extra=extra;
  s->runtime_opaque=1;
  s->w=w; s->h=h; s->originx=xorig; s->originy=yorig;
  s->n_frames=frames;
  s->ml=0; s->mt=0; s->mr=w>0?w-1:0; s->mb=h>0?h-1:0;
  s->mask=NULL; s->mask_rowb=0; s->mask_count=0;
  s->collision_kind=0; s->collision_tolerance=63;
  size_t rows=(size_t)frames*(size_t)h;
  s->runtime_row_min=malloc(rows*sizeof(int));
  s->runtime_row_max=malloc(rows*sizeof(int));
  if(s->runtime_row_min && s->runtime_row_max){
    for(int f=0; f<frames; f++) for(int yy=0; yy<h; yy++){
      int mn=w, mx=-1;
      const uint8_t *sp=rgba+((size_t)f*(size_t)w*(size_t)h+(size_t)yy*(size_t)w)*4;
      for(int xx=0; xx<w; xx++, sp+=4){
        if(sp[3]!=255) s->runtime_opaque=0;
        if(sp[3]){
          if(xx<mn) mn=xx;
          if(xx>mx) mx=xx;
        }
      }
      s->runtime_row_min[(size_t)f*(size_t)h+(size_t)yy]=mn;
      s->runtime_row_max[(size_t)f*(size_t)h+(size_t)yy]=mx;
    }
  } else {
    free(s->runtime_row_min); free(s->runtime_row_max);
    s->runtime_row_min=NULL; s->runtime_row_max=NULL;
  }
}
static void sprite_set_runtime_source(GmlSprite *s, const char *path, int imgnum, int removeback){
  if(!s) return;
  free(s->runtime_source_path);
  s->runtime_source_path=path?strdup(path):NULL;
  s->runtime_source_imgnum=imgnum;
  s->runtime_source_removeback=removeback?1:0;
}

void gml_render_apply_removeback_rgba(uint8_t *rgba, int w, int h, int removeback){
  if(!removeback || !rgba || w<=0 || h<=0) return;
  const uint8_t *key=rgba+(size_t)(h-1)*(size_t)w*4u;
  uint8_t rr=key[0], gg=key[1], bb=key[2];
  for(int i=0;i<w*h;i++){
    uint8_t *p=rgba+i*4;
    if(p[0]==rr && p[1]==gg && p[2]==bb) p[3]=0;
  }
}

int gml_sprite_append_from_rgba_frames(GmlRender *r, uint8_t *rgba, int w, int h, int frames, int xorig, int yorig, const char *name){
  if(!rgba || w<=0 || h<=0 || frames<=0) return -1;
  if(render_setting(r,"GML_LOG_SPRGEN") && ++r->generated_sprite_log_count%2000==0)
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[sprgen] %ld sprites appended (n_spr=%d)\n",r->generated_sprite_log_count,r->n_spr);
  int id=-1;
  /* Reuse a freed runtime slot only if one exists (spr_has_free); scanning all sprites on every
   * append is O(n) and turns bulk generation into
   * O(n²). Grow the array by doubling (spr_cap) instead of +1 for the same reason. */
  if(r->spr_has_free){
    int base=r->base_n_spr>0?r->base_n_spr:0;
    for(int i=base;i<r->n_spr;i++){
      if(r->spr[i].runtime_extra && !r->spr[i].runtime_rgba && r->spr[i].n_frames<=0){ id=i; break; }
    }
    if(id<0) r->spr_has_free=0;   /* none left; don't rescan until another is freed */
  }
  if(id<0){
    if(r->n_spr>=r->spr_cap){
      int nc=r->spr_cap>0?r->spr_cap*2:64;
      GmlSprite *ns=realloc(r->spr,(size_t)nc*sizeof(GmlSprite));
      if(!ns){ free(rgba); return -1; }
      r->spr=ns; r->spr_cap=nc;
    }
    id=r->n_spr++;
  }
  GmlSprite *s=&r->spr[id];
  memset(s,0,sizeof(*s));
  /* GameMaker gives every runtime-created sprite an addressable asset name.  Reusing a shared
   * placeholder makes sprite_get_name() return the same string for every surface sprite, so a
   * later asset_get_index(sprite_get_name(id)) aliases all of them to the first generated cell.
   * This pattern is used by runtime atlas splitters, among other things. */
  if(name) s->owned_name=strdup(name);
  else {
    char generated[64];
    snprintf(generated,sizeof generated,"__gml_runtime_sprite_%d",id);
    s->owned_name=strdup(generated);
  }
  s->name=s->owned_name?s->owned_name:"<runtime-sprite>";
  r->spr_name_gen++;
  sprite_set_runtime_rgba(s,rgba,w,h,frames,xorig,yorig,1);
  return id;
}
int gml_sprite_append_from_rgba(GmlRender *r, uint8_t *rgba, int w, int h, int xorig, int yorig, const char *name){
  return gml_sprite_append_from_rgba_frames(r,rgba,w,h,1,xorig,yorig,name);
}
static uint8_t *sprite_copy_rgba(GmlRender *r,int sprite,
                                 int *width,int *height,int *frame_count){
  if(!r || sprite<0 || sprite>=r->n_spr) return NULL;
  GmlSprite *s=&r->spr[sprite];
  if(s->w<=0 || s->h<=0 || s->n_frames<=0) return NULL;
  int w=s->w, h=s->h, frames=s->n_frames;
  size_t pixels=(size_t)w*(size_t)h*(size_t)frames;
  if(pixels==0 || pixels>SIZE_MAX/4) return NULL;
  uint8_t *rgba=calloc(pixels,4);
  if(!rgba) return NULL;

  for(int f=0; f<frames; f++){
    uint8_t *dst=rgba+(size_t)f*(size_t)w*(size_t)h*4;
    if(s->runtime_rgba){
      const uint8_t *fr=runtime_frame_rgba(s,f);
      if(fr) memcpy(dst,fr,(size_t)w*(size_t)h*4);
      continue;
    }
    int ti=s->frame ? s->frame[f] : -1;
    if(ti<0 || ti>=r->n_tpag) continue;
    GmlTpag *t=&r->tpag[ti];
    if(t->atlas<0 || t->atlas>=r->n_atlas) continue;
    GmlAtlas *a=&r->atlas[t->atlas];
    if(!atlas_pixels(r,t->atlas) || !a->px) continue;
    for(int yy=0; yy<h; yy++) for(int xx=0; xx<w; xx++){
      int ix=xx-t->tx, iy=yy-t->ty;
      if(ix<0 || iy<0 || ix>=t->sw || iy>=t->sh) continue;
      int ax=t->sx+ix, ay=t->sy+iy;
      if(ax<0 || ay<0 || ax>=a->w || ay>=a->h) continue;
      const uint8_t *sp=a->px+((size_t)ay*(size_t)a->w+(size_t)ax)*4;
      uint8_t *dp=dst+((size_t)yy*(size_t)w+(size_t)xx)*4;
      dp[0]=sp[0]; dp[1]=sp[1]; dp[2]=sp[2]; dp[3]=sp[3];
    }
  }
  if(width) *width=w;
  if(height) *height=h;
  if(frame_count) *frame_count=frames;
  return rgba;
}

static uint8_t *sprite_copy_collision_mask(const GmlSprite *sprite){
  if(!sprite || !sprite->mask || sprite->mask_rowb<=0 ||
     sprite->mask_count<=0 || sprite->h<=0) return NULL;
  size_t bytes=(size_t)sprite->mask_rowb*(size_t)sprite->h*(size_t)sprite->mask_count;
  uint8_t *copy=malloc(bytes);
  if(copy) memcpy(copy,sprite->mask,bytes);
  return copy;
}

int gml_sprite_duplicate(GmlRender *r, int sprite){
  int w=0,h=0,frames=0;
  uint8_t *rgba=sprite_copy_rgba(r,sprite,&w,&h,&frames);
  if(!rgba) return -1;
  GmlSprite *s=&r->spr[sprite];
  uint8_t *mask=sprite_copy_collision_mask(s);
  if(s->mask && !mask){ free(rgba); return -1; }

  int id=gml_sprite_append_from_rgba_frames(r,rgba,w,h,frames,s->originx,s->originy,
                                            s->name?s->name:"<sprite-copy>");
  if(id>=0 && id<r->n_spr){
    GmlSprite *src=&r->spr[sprite];
    GmlSprite *dst=&r->spr[id];
    dst->ml=src->ml; dst->mt=src->mt; dst->mr=src->mr; dst->mb=src->mb;
    dst->runtime_mask=mask;
    dst->mask=mask;
    dst->mask_rowb=src->mask_rowb;
    dst->mask_count=src->mask_count;
    dst->collision_kind=src->collision_kind;
    dst->collision_tolerance=src->collision_tolerance;
    dst->ns_enabled=src->ns_enabled;
    dst->ns_l=src->ns_l; dst->ns_t=src->ns_t; dst->ns_r=src->ns_r; dst->ns_b=src->ns_b;
    for(int i=0;i<5;i++) dst->ns_tile[i]=src->ns_tile[i];
  } else free(mask);
  return id;
}

int gml_sprite_assign(GmlRender *r,int destination,int source){
  if(!r || destination<0 || destination>=r->n_spr ||
     source<0 || source>=r->n_spr) return 0;
  if(destination==source) return gml_sprite_exists(r,source);
  int width=0,height=0,frames=0;
  uint8_t *rgba=sprite_copy_rgba(r,source,&width,&height,&frames);
  if(!rgba) return 0;
  GmlSprite *src=&r->spr[source];
  GmlSprite *dst=&r->spr[destination];
  uint8_t *mask=sprite_copy_collision_mask(src);
  if(src->mask && !mask){ free(rgba); return 0; }
  int extra=dst->runtime_extra;
  sprite_set_runtime_rgba(dst,rgba,width,height,frames,src->originx,src->originy,extra);
  dst->ml=src->ml; dst->mt=src->mt; dst->mr=src->mr; dst->mb=src->mb;
  /* sprite_assign replaces the whole sprite asset.  Its independently authored collision plane
   * follows the visible frames; falling back to copied-frame alpha changes precise collisions. */
  dst->runtime_mask=mask;
  dst->mask=mask;
  dst->mask_rowb=src->mask_rowb;
  dst->mask_count=src->mask_count;
  dst->collision_kind=src->collision_kind;
  dst->collision_tolerance=src->collision_tolerance;
  dst->playback_speed=src->playback_speed;
  dst->playback_speed_type=src->playback_speed_type;
  dst->playback_speed_valid=src->playback_speed_valid;
  dst->ns_enabled=src->ns_enabled;
  dst->ns_l=src->ns_l; dst->ns_t=src->ns_t; dst->ns_r=src->ns_r; dst->ns_b=src->ns_b;
  for(int index=0;index<5;index++) dst->ns_tile[index]=src->ns_tile[index];
  return 1;
}

static int sprite_read_rgba(GmlRender *r, GmlSprite *s, int frame, int x, int y, uint8_t out[4]){
  memset(out,0,4);
  if(!r||!s||x<0||y<0||x>=s->w||y>=s->h||s->n_frames<=0) return 0;
  int f=((frame%s->n_frames)+s->n_frames)%s->n_frames;
  if(s->runtime_rgba){ const uint8_t *rgba=runtime_frame_rgba(s,f); if(!rgba)return 0;
    memcpy(out,rgba+((size_t)y*s->w+x)*4,4); return 1; }
  int ti=s->frame?s->frame[f]:-1; if(ti<0||ti>=r->n_tpag)return 0;
  GmlTpag *t=&r->tpag[ti]; int ix=x-t->tx,iy=y-t->ty;
  if(ix<0||iy<0||ix>=t->sw||iy>=t->sh||t->atlas<0||t->atlas>=r->n_atlas)return 0;
  GmlAtlas *atlas=&r->atlas[t->atlas]; if(!atlas_pixels(r,t->atlas)||!atlas->px)return 0;
  int ax=t->sx+ix,ay=t->sy+iy; if(ax<0||ay<0||ax>=atlas->w||ay>=atlas->h)return 0;
  memcpy(out,atlas->px+((size_t)ay*atlas->w+ax)*4,4); return 1;
}

int gml_sprite_set_alpha_from_sprite(GmlRender *r, int sprite, int alpha_sprite){
  if(!r||sprite<0||sprite>=r->n_spr||alpha_sprite<0||alpha_sprite>=r->n_spr)return 0;
  GmlSprite *dst=&r->spr[sprite],*src=&r->spr[alpha_sprite];
  if(dst->w<=0||dst->h<=0||dst->n_frames<=0||src->n_frames<=0)return 0;
  size_t pixels=(size_t)dst->w*dst->h*dst->n_frames; if(!pixels||pixels>SIZE_MAX/4)return 0;
  uint8_t *rgba=malloc(pixels*4); if(!rgba)return 0;
  for(int f=0;f<dst->n_frames;f++) for(int y=0;y<dst->h;y++) for(int x=0;x<dst->w;x++){
    uint8_t dc[4],sc[4]; sprite_read_rgba(r,dst,f,x,y,dc); sprite_read_rgba(r,src,f,x,y,sc);
    uint8_t *p=rgba+(((size_t)f*dst->h+y)*dst->w+x)*4;
    p[0]=dc[0];p[1]=dc[1];p[2]=dc[2];p[3]=(uint8_t)(((int)sc[0]+sc[1]+sc[2])/3);
  }
  int w=dst->w,h=dst->h,frames=dst->n_frames,xorig=dst->originx,yorig=dst->originy,extra=dst->runtime_extra;
  sprite_set_runtime_rgba(dst,rgba,w,h,frames,xorig,yorig,extra);
  return 1;
}
void gml_sprite_set_offset(GmlRender *r, int sprite, int xorig, int yorig){
  if(!r || sprite<0 || sprite>=r->n_spr) return;
  r->spr[sprite].originx=xorig;
  r->spr[sprite].originy=yorig;
}
/* sprite_add(file, imgnum, removeback, smooth, xorig, yorig): load a loose image as a runtime
 * sprite. GM treats the image as a horizontal strip of imgnum equal frames. removeback keys
 * out the bottom-left pixel's color, GM8-style. Returns -1 when the file can't be decoded. */
static uint8_t *runtime_image_load(GmlRender *r,const char *path,
                                   int *width,int *height,int *components){
  if(!r || !r->win || !path) return NULL;
  uint8_t *encoded=NULL;
  size_t encoded_size=0;
  if(!anygm_vfs_read_all(r->win->host,path,&encoded,&encoded_size,(size_t)INT_MAX))
    return NULL;
  GmlMediaBuffer image={0};
  int decoded=gml_image_decode_rgba(encoded,encoded_size,&image,
                                    width,height,components);
  free(encoded);
  return decoded?image.data:NULL;
}
static uint8_t *runtime_image_load_frames(
    GmlRender *r,const char *path,int *width,int *height,
    int *frames,int *components){
  if(!r || !r->win || !path) return NULL;
  uint8_t *encoded=NULL;
  size_t encoded_size=0;
  if(!anygm_vfs_read_all(r->win->host,path,&encoded,&encoded_size,(size_t)INT_MAX))
    return NULL;
  GmlMediaBuffer image={0};
  int decoded=gml_image_decode_rgba_frames(
      encoded,encoded_size,&image,width,height,frames,components);
  free(encoded);
  return decoded?image.data:NULL;
}
static uint8_t *runtime_sprite_image_load(
    GmlRender *r,const char *path,int requested_frames,int removeback,
    int *frame_width,int *height,int *frame_count){
  int width=0,decoded_height=0,decoded_frames=0,components=0;
  uint8_t *decoded=runtime_image_load_frames(
      r,path,&width,&decoded_height,&decoded_frames,&components);
  (void)components;
  if(!decoded || width<=0 || decoded_height<=0 || decoded_frames<=0){
    free(decoded);
    return NULL;
  }
  int frames=1;
  int width_per_frame=width;
  uint8_t *rgba=decoded;
  if(decoded_frames>1){
    /* An animated image states its own subimage count. The requested count describes how to cut a
     * single-image strip; applying it to an animated file would collapse the animation to its first
     * frame whenever the caller requests one image. */
    frames=decoded_frames;
  } else {
    frames=requested_frames>0?requested_frames:1;
    if(frames>width || width%frames) frames=1;
    width_per_frame=width/frames;
    if(frames>1){
      if((size_t)frames*(size_t)width_per_frame>
         SIZE_MAX/(size_t)decoded_height/4u){
        free(decoded);
        return NULL;
      }
      rgba=malloc((size_t)frames*(size_t)width_per_frame*
                  (size_t)decoded_height*4u);
      if(!rgba){
        free(decoded);
        return NULL;
      }
      for(int frame=0;frame<frames;frame++)
        for(int y=0;y<decoded_height;y++)
          memcpy(rgba+((size_t)frame*decoded_height+y)*width_per_frame*4u,
                 decoded+((size_t)y*width+frame*width_per_frame)*4u,
                 (size_t)width_per_frame*4u);
      free(decoded);
    }
  }
  if(removeback){
    uint8_t *key=rgba+((size_t)(decoded_height-1)*width_per_frame)*4u;
    uint8_t red=key[0],green=key[1],blue=key[2];
    size_t pixels=(size_t)frames*(size_t)width_per_frame*
                  (size_t)decoded_height;
    for(size_t index=0;index<pixels;index++){
      uint8_t *pixel=rgba+index*4u;
      if(pixel[0]==red && pixel[1]==green && pixel[2]==blue)
        pixel[3]=0;
    }
  }
  if(frame_width) *frame_width=width_per_frame;
  if(height) *height=decoded_height;
  if(frame_count) *frame_count=frames;
  return rgba;
}

int gml_background_replace_from_rgba(GmlRender *r,int background,
                                     uint8_t *rgba,int width,int height){
  if(!r || background<0 || background>=r->n_bg || !rgba ||
     width<=0 || height<=0 || width>4096 || height>4096) return 0;
  int page_index=r->bg[background].tpag;
  int add_page=page_index<0 || page_index>=r->n_tpag;
  GmlAtlas *atlases=malloc((size_t)(r->n_atlas+1)*sizeof(*atlases));
  GmlTpag *pages=NULL;
  uint32_t *page_pointers=NULL;
  if(!atlases) return 0;
  if(r->n_atlas>0 && r->atlas)
    memcpy(atlases,r->atlas,(size_t)r->n_atlas*sizeof(*atlases));
  if(add_page){
    pages=malloc((size_t)(r->n_tpag+1)*sizeof(*pages));
    page_pointers=calloc((size_t)r->n_tpag+1,sizeof(*page_pointers));
    if(!pages || !page_pointers){
      free(atlases);
      free(pages);
      free(page_pointers);
      return 0;
    }
    if(r->n_tpag>0 && r->tpag)
      memcpy(pages,r->tpag,(size_t)r->n_tpag*sizeof(*pages));
    if(r->n_tpag>0 && r->tpag_ptr)
      memcpy(page_pointers,r->tpag_ptr,(size_t)r->n_tpag*sizeof(*page_pointers));
  }

  int atlas_index=r->n_atlas;
  memset(&atlases[atlas_index],0,sizeof(atlases[atlas_index]));
  atlases[atlas_index].px=rgba;
  atlases[atlas_index].w=width;
  atlases[atlas_index].h=height;
  atlases[atlas_index].decode_attempted=1;
  free(r->atlas);
  r->atlas=atlases;
  r->n_atlas++;

  if(add_page){
    page_index=r->n_tpag;
    memset(&pages[page_index],0,sizeof(pages[page_index]));
    free(r->tpag);
    free(r->tpag_ptr);
    r->tpag=pages;
    r->tpag_ptr=page_pointers;
    r->n_tpag++;
    r->bg[background].tpag=page_index;
  } else {
    gml_render_texture_page_cache_clear(r,&r->tpag[page_index]);
  }
  GmlTpag *page=&r->tpag[page_index];
  page->sx=page->sy=page->tx=page->ty=0;
  page->sw=page->bw=width;
  page->sh=page->bh=height;
  page->atlas=atlas_index;
  gml_render_interpolated_subrect_cache_clear(r);
  return 1;
}

int gml_background_replace_from_file(GmlRender *r,int background,const char *path,
                                     int removeback,int smooth){
  (void)smooth;
  if(!r || !path || !*path) return 0;
  int width=0,height=0,components=0;
  uint8_t *rgba=runtime_image_load(r,path,&width,&height,&components);
  (void)components;
  if(!rgba) return 0;
  gml_render_apply_removeback_rgba(rgba,width,height,removeback);
  if(gml_background_replace_from_rgba(r,background,rgba,width,height)) return 1;
  free(rgba);
  return 0;
}

int gml_sprite_add_file(GmlRender *r, const char *path, int imgnum, int removeback, int xorig, int yorig){
  if(!r || !path) return -1;
  int fw=0,H=0,frames=0;
  uint8_t *rgba=runtime_sprite_image_load(
      r,path,imgnum,removeback,&fw,&H,&frames);
  if(!rgba) return -1;
  int id=gml_sprite_append_from_rgba_frames(r,rgba,fw,H,frames,xorig,yorig,path);
  if(id>=0 && id<r->n_spr) sprite_set_runtime_source(&r->spr[id],path,imgnum,removeback);
  return id;
}
int gml_sprite_replace_from_rgba_frames(GmlRender *r, int sprite, uint8_t *rgba, int w, int h, int frames, int xorig, int yorig){
  if(sprite<0 || sprite>=r->n_spr || !rgba || w<=0 || h<=0 || frames<=0) return 0;
  sprite_set_runtime_rgba(&r->spr[sprite],rgba,w,h,frames,xorig,yorig,0);
  return 1;
}
int gml_sprite_replace_from_rgba(GmlRender *r, int sprite, uint8_t *rgba, int w, int h, int xorig, int yorig){
  return gml_sprite_replace_from_rgba_frames(r,sprite,rgba,w,h,1,xorig,yorig);
}
void gml_sprite_delete(GmlRender *r, int sprite){
  if(!r || sprite<0 || sprite>=r->n_spr) return;
  int base=r->base_n_spr>0?r->base_n_spr:r->n_spr;
  if(sprite<base) return;
  GmlSprite *s=&r->spr[sprite];
  if(!s->runtime_extra) return;
  gml_render_sprite_cache_free(s);
  gml_render_sprite_state_cache_clear(s);
  free(s->runtime_rgba);
  free(s->runtime_mask);
  free(s->runtime_row_min);
  free(s->runtime_row_max);
  free(s->owned_name);
  free(s->runtime_source_path);
  free(s->frame);
  memset(s,0,sizeof(*s));
  s->runtime_extra=1;
  r->spr_has_free=1;   /* a reusable freed slot now exists */
  r->spr_name_gen++;   /* the deleted sprite's name is gone */
  while(r->n_spr>base){
    GmlSprite *last=&r->spr[r->n_spr-1];
    if(!(last->runtime_extra && !last->runtime_rgba && last->n_frames<=0)) break;
    r->n_spr--;
  }
}
static void sprite_auto_bbox(GmlRender *r, int sprite, int tolerance, int *ml, int *mt, int *mr, int *mb){
  GmlSprite *s=&r->spr[sprite];
  int l=s->w, t=s->h, rr=-1, bb=-1;
  int frames=s->n_frames>0?s->n_frames:1;
  for(int f=0; f<frames; f++) for(int y=0; y<s->h; y++) for(int x=0; x<s->w; x++){
    if(gml_sprite_alpha(r,sprite,f,x,y)<=tolerance) continue;
    if(x<l) l=x;
    if(x>rr) rr=x;
    if(y<t) t=y;
    if(y>bb) bb=y;
  }
  if(rr<l || bb<t){ *ml=0; *mt=0; *mr=-1; *mb=-1; return; }
  *ml=l; *mt=t; *mr=rr; *mb=bb;
}
static int clamp_i(int v, int lo, int hi){
  return v<lo?lo:(v>hi?hi:v);
}
int gml_sprite_collision_mask(GmlRender *r, int sprite, int sepmasks, int bboxmode,
                              int bbleft, int bbtop, int bbright, int bbbottom,
                              int kind, int tolerance){
  (void)sepmasks;
  if(!r || sprite<0 || sprite>=r->n_spr) return 0;
  GmlSprite *s=&r->spr[sprite];
  if(!s->runtime_rgba || s->w<=0 || s->h<=0 || s->n_frames<=0) return 0;
  if(tolerance<0) tolerance=0;
  if(tolerance>255) tolerance=255;
  if(kind<0 || kind>3) kind=0;
  if(bboxmode==0){
    sprite_auto_bbox(r,sprite,tolerance,&s->ml,&s->mt,&s->mr,&s->mb);
  } else if(bboxmode==2){
    s->ml=clamp_i(bbleft,0,s->w-1);
    s->mt=clamp_i(bbtop,0,s->h-1);
    s->mr=clamp_i(bbright,0,s->w-1);
    s->mb=clamp_i(bbbottom,0,s->h-1);
  } else {
    s->ml=0; s->mt=0; s->mr=s->w-1; s->mb=s->h-1;
  }
  free(s->runtime_mask); s->runtime_mask=NULL;
  s->mask=NULL; s->mask_rowb=0; s->mask_count=0;
  s->collision_kind=kind;
  s->collision_tolerance=tolerance;
  return 1;
}
void gml_render_clear_runtime_sprites(GmlRender *r){
  int base=r->base_n_spr>0?r->base_n_spr:r->n_spr;
  if(base>r->n_spr) base=r->n_spr;
  for(int i=0;i<base;i++){
    GmlSprite *s=&r->spr[i];
    gml_render_sprite_cache_free(s);
    gml_render_sprite_state_cache_clear(s);
    free(s->runtime_rgba); free(s->runtime_mask); free(s->runtime_row_min); free(s->runtime_row_max); free(s->runtime_source_path);
    s->runtime_rgba=NULL; s->runtime_mask=NULL; s->runtime_row_min=NULL; s->runtime_row_max=NULL; s->runtime_source_path=NULL; s->runtime_owned=0; s->runtime_extra=0;
    s->runtime_opaque=0;
    s->runtime_source_imgnum=0; s->runtime_source_removeback=0;
    sprite_restore_base(s);
  }
  for(int i=base;i<r->n_spr;i++){
    gml_render_sprite_state_cache_clear(&r->spr[i]);
    free(r->spr[i].runtime_rgba);
    free(r->spr[i].runtime_mask);
    free(r->spr[i].runtime_row_min);
    free(r->spr[i].runtime_row_max);
    free(r->spr[i].owned_name);
    free(r->spr[i].runtime_source_path);
    free(r->spr[i].frame);
  }
  r->n_spr=base;
}
int gml_sprite_create_from_surface(GmlRender *r, int surf, int x, int y, int w, int h,
                                   int removeback, int smooth, int xorig, int yorig){
  (void)smooth;
  int sw=0, sh=0; uint32_t *src=surface_pixels(r,surf,&sw,&sh);
  if(!src || w<=0 || h<=0) return -1;
  if(src==r->fb) gml_render_maybe_prepare_draw(r);
  uint8_t *rgba=calloc((size_t)w*h*4,1);
  if(!rgba) return -1;
  for(int yy=0; yy<h; yy++) for(int xx=0; xx<w; xx++){
    int sx=x+xx, sy=y+yy;
    uint8_t *dp=rgba+((size_t)yy*w+xx)*4;
    if(sx>=0 && sy>=0 && sx<sw && sy<sh){
      uint32_t p=src[(size_t)sy*sw+sx];
      dp[0]=(uint8_t)((p>>16)&0xff);
      dp[1]=(uint8_t)((p>>8)&0xff);
      dp[2]=(uint8_t)(p&0xff);
      dp[3]=(uint8_t)(p>>24);
    }
  }
  gml_render_apply_removeback_rgba(rgba,w,h,removeback);
  return gml_sprite_append_from_rgba_frames(r,rgba,w,h,1,xorig,yorig,NULL);
}
int gml_sprite_replace_from_file(GmlRender *r, int sprite, const char *path, int imgnumb,
                                 int removeback, int smooth, int xorig, int yorig){
  (void)smooth;
  if(sprite<0 || sprite>=r->n_spr || !path || !*path) return 0;
  int w=0,h=0,frames=0;
  uint8_t *rgba=runtime_sprite_image_load(
      r,path,imgnumb,removeback,&w,&h,&frames);
  if(!rgba) return 0;
  int ok=gml_sprite_replace_from_rgba_frames(r,sprite,rgba,w,h,frames,xorig,yorig);
  if(ok) sprite_set_runtime_source(&r->spr[sprite],path,imgnumb,removeback);
  if(!ok) free(rgba);
  return ok;
}

/* A TPAG item may declare a logical target larger than its stored texel rectangle. Materialize
 * only those reduced-resolution sprites as runtime RGBA frames at the logical size, preserving
 * the specialized texel blits for other items. */
void gml_render_materialize_scaled_pages(GmlRender *r){
  if(!r || !r->spr || !r->tpag) return;
  for(int i=0;i<r->n_spr;i++){
    GmlSprite *s=&r->spr[i];
    if(s->runtime_rgba || !s->frame || s->n_frames<=0 || s->w<=0 || s->h<=0) continue;
    int scaled=0;
    for(int f=0;f<s->n_frames && !scaled;f++){
      int ti=s->frame[f];
      if(ti<0 || ti>=r->n_tpag) continue;
      GmlTpag *t=&r->tpag[ti];
      /* A smaller target may represent ordinary trim or padding. Materialize only when the
       * logical target is larger than the stored texel rectangle. */
      if(t->tw>t->sw || t->th>t->sh) scaled=1;
    }
    if(!scaled) continue;
    size_t frame_px=(size_t)s->w*(size_t)s->h;
    if(frame_px==0 || frame_px>(size_t)64*1024*1024) continue;
    uint8_t *rgba=calloc((size_t)s->n_frames*frame_px,4);
    if(!rgba) continue;
    int ok=1;
    for(int f=0;f<s->n_frames && ok;f++){
      int ti=s->frame[f];
      if(ti<0 || ti>=r->n_tpag) continue;
      GmlTpag *t=&r->tpag[ti];
      if(t->atlas<0 || t->atlas>=r->n_atlas || !atlas_pixels(r,t->atlas)){ ok=0; break; }
      GmlAtlas *a=&r->atlas[t->atlas];
      int tw=t->tw>0?t->tw:t->sw, th=t->th>0?t->th:t->sh;
      if(t->sw<=0 || t->sh<=0 || tw<=0 || th<=0) continue;
      uint8_t *dst_frame=rgba+(size_t)f*frame_px*4;
      for(int y=0;y<th;y++){
        int dy=t->ty+y;
        if(dy<0 || dy>=s->h) continue;
        int sy=t->sy+(int)(((int64_t)y*t->sh)/th);
        if(sy<0 || sy>=a->h) continue;
        const uint8_t *srow=a->px+(size_t)sy*a->w*4;
        for(int x=0;x<tw;x++){
          int dx=t->tx+x;
          if(dx<0 || dx>=s->w) continue;
          int sx=t->sx+(int)(((int64_t)x*t->sw)/tw);
          if(sx<0 || sx>=a->w) continue;
          uint8_t *dp=dst_frame+((size_t)dy*s->w+dx)*4;
          const uint8_t *sp=srow+(size_t)sx*4;
          dp[0]=sp[0]; dp[1]=sp[1]; dp[2]=sp[2]; dp[3]=sp[3];
        }
      }
    }
    if(!ok){ free(rgba); continue; }
    sprite_set_runtime_rgba(s,rgba,s->w,s->h,s->n_frames,s->originx,s->originy,0);
  }
}
