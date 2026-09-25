/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Existing skeletal sprite decode, pose preparation, and neutral draw items. */
#include "gml_render_internal.h"
#include "gmlc_json.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

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
