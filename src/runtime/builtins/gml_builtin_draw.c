/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Draw, camera, vertex, model, and fixed-function builtin argument adaptation. */
#include "gml_builtin_internal.h"
#include "anygm_compatibility.h"
#include "anygm_host.h"

#include <ctype.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static double builtin_game_speed(GmlVM *vm){
  GmlVal *p=vm?gml_varmap_get(&vm->globals,"__game_speed_fps"):NULL;
  double v=p?N(p,1,0):60.0;
  return v>0 ? v : 60.0;
}
static int primitive_raster_coordinate(const GmlVM *vm,double coordinate){
  if(vm && vm->win && anygm_policy_uses_first_generation_studio(vm->win))
    return (int)ceil(coordinate)+1;
  return (int)floor(coordinate);
}
static void builtin_set_game_speed(GmlVM *vm, double fps){
  if(!vm) return;
  if(fps<=0) fps=60.0;
  if(fabs(fps-60.0)<0.000001 && !gml_varmap_get(&vm->globals,"__game_speed_fps")) return;
  *gml_varmap_put(&vm->globals,"__game_speed_fps")=vreal(fps);
}
static double gml_shader_get_sampler(GmlRender *R, int sh, const char *name){
  return gml_render_shader_sampler_handle(R,sh,name);
}
static GmlVal sprite_uvs(GmlRender *R, int spr, int img){
  int handle=gml_render_sprite_texture_handle(spr,img);
  GmlRenderTextureMetrics metrics;
  if(!gml_render_texture_metrics(R,handle,&metrics) ||
     metrics.kind!=GML_RENDER_TEXTURE_SPRITE)
    return arr8(0,0,0,0,0,0,0,0);
  if(metrics.runtime && metrics.logical_width>0 && metrics.logical_height>0)
    return arr8(0,0,1,1,0,0,1,1);
  if(!metrics.atlas_backed || metrics.full_width<=0 || metrics.full_height<=0)
    return arr8(0,0,0,0,0,0,0,0);
  double aw=metrics.full_width, ah=metrics.full_height;
  double saved_w=metrics.logical_width>0?
    (double)metrics.width/metrics.logical_width:0.0;
  double saved_h=metrics.logical_height>0?
    (double)metrics.height/metrics.logical_height:0.0;
  return arr8(metrics.source_x/aw,metrics.source_y/ah,
              (metrics.source_x+metrics.width)/aw,
              (metrics.source_y+metrics.height)/ah,
              metrics.trim_x,metrics.trim_y,saved_w,saved_h);
}
static GmlVal texture_uvs(GmlRender *R, int tex){
  GmlRenderTextureMetrics metrics;
  if(!gml_render_texture_metrics(R,tex,&metrics))
    return array4(0,0,0,0);
  if(metrics.kind==GML_RENDER_TEXTURE_SURFACE)
    return array4(0,0,1,1);
  if(metrics.kind==GML_RENDER_TEXTURE_FONT)
    return array4(0,0,1,1);
  if(metrics.kind==GML_RENDER_TEXTURE_SPRITE){
    if(metrics.runtime) return arr8(0,0,1,1,0,0,1,1);
    if(metrics.atlas_backed && metrics.full_width>0 &&
       metrics.full_height>0){
      double saved_w=metrics.logical_width>0?
        (double)metrics.width/metrics.logical_width:0.0;
      double saved_h=metrics.logical_height>0?
        (double)metrics.height/metrics.logical_height:0.0;
      return arr8(
        (double)metrics.source_x/metrics.full_width,
        (double)metrics.source_y/metrics.full_height,
        (double)(metrics.source_x+metrics.width)/metrics.full_width,
        (double)(metrics.source_y+metrics.height)/metrics.full_height,
        metrics.trim_x,metrics.trim_y,saved_w,saved_h);
    }
  }
  return array4(0,0,0,0);
}
static void font_glyph_key(unsigned codepoint,char key[5]){
  if(codepoint<=0x7f){
    key[0]=(char)codepoint; key[1]=0;
  }else if(codepoint<=0x7ff){
    key[0]=(char)(0xc0|(codepoint>>6));
    key[1]=(char)(0x80|(codepoint&0x3f)); key[2]=0;
  }else if(codepoint<=0xffff && (codepoint<0xd800 || codepoint>0xdfff)){
    key[0]=(char)(0xe0|(codepoint>>12));
    key[1]=(char)(0x80|((codepoint>>6)&0x3f));
    key[2]=(char)(0x80|(codepoint&0x3f)); key[3]=0;
  }else if(codepoint<=0x10ffff){
    key[0]=(char)(0xf0|(codepoint>>18));
    key[1]=(char)(0x80|((codepoint>>12)&0x3f));
    key[2]=(char)(0x80|((codepoint>>6)&0x3f));
    key[3]=(char)(0x80|(codepoint&0x3f)); key[4]=0;
  }else{
    key[0]=(char)0xef; key[1]=(char)0xbf; key[2]=(char)0xbd; key[3]=0;
  }
}
static int vm_file_read_line_into(GmlVM *vm,int slot,char *buffer,size_t capacity){
  if(!buffer || !capacity) return 0;
  size_t length=0;
  int c=EOF;
  while((c=vm_file_getc(vm,slot))!=EOF && c!='\n'){
    if(c!='\r' && length+1<capacity) buffer[length++]=(char)c;
  }
  buffer[length]=0;
  return c!=EOF || length>0;
}

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

static int gm_matrix_read(GmlVal value,double out[16]){
  if(value.t!=V_ARR || !value.arr || gml_val_array_length(value)<16) return 0;
  for(int i=0;i<16;i++){
    GmlVal element=gml_arr_get(value,i);
    out[i]=N(&element,1,0);
  }
  return 1;
}
static GmlVal gm_matrix_write(const double matrix[16],GmlVal reuse){
  GmlVal result=(reuse.t==V_ARR && reuse.arr)?reuse:gml_arr_new(16,vreal(0));
  for(int i=0;i<16;i++) gml_arr_set(result,i,vreal(matrix[i]));
  return result;
}

static GmlVal gm_matrix_builtin(GmlRender *R,const char *name,GmlVal *args,int count){
  double result[16];
  if(!strcmp(name,"matrix_inverse")){
    double input[16];
    if(count<1 || !gm_matrix_read(args[0],input) ||
       !gml_software3d_matrix_inverse(input,result)) return vundef();
    return gm_matrix_write(result,count>1?args[1]:vundef());
  }
  if(!strcmp(name,"matrix_build_identity")){
    gml_software3d_matrix_identity(result);
    return gm_matrix_write(result,count>0?args[0]:vundef());
  }
  if(!strcmp(name,"matrix_get")){
    int type=(int)N(args,count,0);
    if(!gml_software3d_matrix_get(GML_GRAPHICS,type,result))
      gml_software3d_matrix_identity(result);
    return gm_matrix_write(result,vundef());
  }
  if(!strcmp(name,"matrix_set")){
    int type=(int)N(args,count,0); double matrix[16];
    if(count>1 && gm_matrix_read(args[1],matrix))
      (void)gml_software3d_matrix_set(R,type,matrix);
    return vreal(0);
  }
  if(!strcmp(name,"matrix_multiply")){
    double first[16],second[16];
    if(count<2 || !gm_matrix_read(args[0],first) || !gm_matrix_read(args[1],second))
      gml_software3d_matrix_identity(result);
    else
      /* The documented order is result = matrix2 * matrix1. */
      gml_software3d_matrix_multiply(second,first,result);
    return gm_matrix_write(result,count>2?args[2]:vundef());
  }
  if(!strcmp(name,"matrix_build")){
    double translation[16],scale[16],rx[16],ry[16],rz[16],rotation_yx[16],rotation[16],scaled[16];
    gml_software3d_matrix_translation(translation,N(args,count,0),N(args,count,1),N(args,count,2));
    gml_software3d_matrix_scaling(scale,N(args,count,6),N(args,count,7),N(args,count,8));
    gml_software3d_matrix_rotation_axis(rx,1,0,0,N(args,count,3));
    gml_software3d_matrix_rotation_axis(ry,0,1,0,N(args,count,4));
    gml_software3d_matrix_rotation_axis(rz,0,0,1,N(args,count,5));
    /* Documented Y-X-Z rotation order, followed by scale and translation. */
    gml_software3d_matrix_multiply(rx,ry,rotation_yx);
    gml_software3d_matrix_multiply(rz,rotation_yx,rotation);
    gml_software3d_matrix_multiply(scale,rotation,scaled);
    gml_software3d_matrix_multiply(translation,scaled,result);
    return gm_matrix_write(result,count>9?args[9]:vundef());
  }
  if(!strcmp(name,"matrix_transform_vertex")){
    double matrix[16];
    if(count<4 || !gm_matrix_read(args[0],matrix)) gml_software3d_matrix_identity(matrix);
    double x=N(args,count,1),y=N(args,count,2),z=N(args,count,3),w=1;
    GmlVal reuse=vundef();
    int four=0;
    if(count>4){
      if(args[4].t==V_ARR){ reuse=args[4]; four=1; }
      else { w=N(args,count,4); four=1; if(count>5) reuse=args[5]; }
    }
    double transformed[4]={
      matrix[0]*x+matrix[4]*y+matrix[8]*z+matrix[12]*w,
      matrix[1]*x+matrix[5]*y+matrix[9]*z+matrix[13]*w,
      matrix[2]*x+matrix[6]*y+matrix[10]*z+matrix[14]*w,
      matrix[3]*x+matrix[7]*y+matrix[11]*z+matrix[15]*w
    };
    int length=four?4:3;
    GmlVal output=(reuse.t==V_ARR && reuse.arr)?reuse:gml_arr_new(length,vreal(0));
    for(int i=0;i<length;i++) gml_arr_set(output,i,vreal(transformed[i]));
    return output;
  }
  if(!strcmp(name,"matrix_build_lookat")){
    double eye[3]={N(args,count,0),N(args,count,1),N(args,count,2)};
    double forward[3]={N(args,count,3)-eye[0],N(args,count,4)-eye[1],N(args,count,5)-eye[2]};
    double supplied_up[3]={N(args,count,6),N(args,count,7),N(args,count,8)};
    double right[3],up[3];
    if(!gml_software3d_normalize(forward)){ forward[0]=0; forward[1]=0; forward[2]=1; }
    gml_software3d_cross(supplied_up,forward,right);
    if(!gml_software3d_normalize(right)){
      double fallback[3]={fabs(forward[2])<.999?0:1,0,fabs(forward[2])<.999?1:0};
      gml_software3d_cross(fallback,forward,right);
      gml_software3d_normalize(right);
    }
    gml_software3d_cross(forward,right,up);
    gml_software3d_normalize(up);
    gml_software3d_matrix_identity(result);
    result[0]=right[0]; result[4]=right[1]; result[8]=right[2];
    result[1]=up[0]; result[5]=up[1]; result[9]=up[2];
    result[2]=forward[0]; result[6]=forward[1]; result[10]=forward[2];
    result[12]=-gml_software3d_dot(right,eye);
    result[13]=-gml_software3d_dot(up,eye);
    result[14]=-gml_software3d_dot(forward,eye);
    return gm_matrix_write(result,count>9?args[9]:vundef());
  }
  if(!strcmp(name,"matrix_build_projection_ortho")){
    double width=fabs(N(args,count,0)),height=fabs(N(args,count,1));
    double near_clip=N(args,count,2),far_clip=N(args,count,3);
    if(width<1e-12) width=1;
    if(height<1e-12) height=1;
    if(fabs(far_clip-near_clip)<1e-12) far_clip=near_clip+1;
    gml_software3d_matrix_identity(result);
    result[0]=2.0/width;
    result[5]=2.0/height;
    result[10]=1.0/(far_clip-near_clip);
    result[14]=-near_clip/(far_clip-near_clip);
    return gm_matrix_write(result,count>4?args[4]:vundef());
  }
  if(!strcmp(name,"matrix_build_projection_perspective_fov")){
    double fov=fabs(N(args,count,0)),aspect=fabs(N(args,count,1));
    double near_clip=N(args,count,2),far_clip=N(args,count,3);
    if(fov<1e-6) fov=1e-6;
    if(fov>179.999999) fov=179.999999;
    if(aspect<1e-12) aspect=1;
    if(fabs(far_clip-near_clip)<1e-12) far_clip=near_clip+1;
    double y_scale=1.0/tan(fov*M_PI/360.0);
    memset(result,0,sizeof result);
    result[0]=y_scale/aspect;
    result[5]=y_scale;
    result[10]=far_clip/(far_clip-near_clip);
    result[11]=1.0;
    result[14]=-(near_clip*far_clip)/(far_clip-near_clip);
    return gm_matrix_write(result,count>4?args[4]:vundef());
  }
  return vreal(0);
}
static uint32_t d3_vertex_color(GmlVM *vm,uint32_t color,int explicitly_colored){
  if(!vm || !vm->win || !anygm_policy_uses_classic_runtime(vm->win)) return color&0xFFFFFFu;
  return explicitly_colored ? (color|0x010000u) : (color&0xFFFFFFu);
}
static int d3_model_file_save(GmlVM *vm,const char *path,
                              GmlSoftware3D *graphics,int model_id){
  int vertex_count=0,batch_count=0;
  if(!vm || !path ||
     !gml_software3d_model_info(graphics,model_id,&vertex_count,&batch_count))
    return 0;
  int id=vm_file_open(vm,path,"w");
  if(id<0) return 0;
  int slot=id-1;
  size_t records=(size_t)vertex_count+(size_t)batch_count*2;
  int ok=vm_file_writef(vm,slot,"100\n%" PRIu64 "\n",(uint64_t)records);
  for(int b=0;b<batch_count&&ok;b++){
    GmlSoftware3DBatch batch;
    ok=gml_software3d_model_batch_get(graphics,model_id,b,&batch) &&
      vm_file_writef(vm,slot,"0 %d\n",batch.kind)>0;
    for(int i=0;i<batch.count&&ok;i++){
      GmlSoftware3DVertex vertex;
      if(!gml_software3d_model_vertex_get(
           graphics,model_id,batch.first+i,&vertex)){ ok=0; break; }
      int red=(int)lround(vertex.r),green=(int)lround(vertex.g),blue=(int)lround(vertex.b);
      if(red<0) red=0; else if(red>255) red=255;
      if(green<0) green=0; else if(green>255) green=255;
      if(blue<0) blue=0; else if(blue>255) blue=255;
      uint32_t color=(uint32_t)red|((uint32_t)green<<8)|((uint32_t)blue<<16);
      ok=vm_file_writef(vm,slot,"9 %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %u %.17g\n",
        vertex.x,vertex.y,vertex.z,vertex.nx,vertex.ny,vertex.nz,
        vertex.u,vertex.v,color,vertex.alpha)>0;
    }
    if(ok) ok=vm_file_writef(vm,slot,"1\n");
  }
  vm_file_close(vm,slot);
  return ok;
}
static int d3_model_file_load(GmlVM *vm,const char *path,
                              GmlSoftware3D *graphics,int model_id){
  if(!vm || !path || !gml_software3d_model_clear(graphics,model_id)) return 0;
  int id=vm_file_open(vm,path,"r");
  if(id<0) return 0;
  int slot=id-1;
  double header_value=0,record_value=0;
  int ok=vm_file_read_real(vm,slot,&header_value) && header_value==100 &&
         vm_file_read_real(vm,slot,&record_value) && record_value>=0 &&
         record_value<=GML_SOFTWARE3D_MODEL_VERTEX_MAX*2u;
  unsigned record_count=(unsigned)record_value;
  char line[2048]; if(ok) (void)vm_file_read_line_into(vm,slot,line,sizeof line);
  for(unsigned record=0;record<record_count&&ok;record++){
    if(!vm_file_read_line_into(vm,slot,line,sizeof line)){ ok=0; break; }
    char *cursor=line,*end=NULL; double token[16]; int token_n=0;
    while(token_n<16){
      while(*cursor==' '||*cursor=='\t'||*cursor=='\r'||*cursor=='\n') cursor++;
      if(!*cursor) break;
      token[token_n]=strtod(cursor,&end); if(end==cursor){ ok=0; break; }
      token_n++; cursor=end;
    }
    if(!ok || token_n<1) { ok=0; break; }
    int opcode=(int)token[0];
    if(opcode==0){
      ok=token_n>=2&&gml_software3d_model_begin(
        graphics,model_id,(int)token[1]);
      continue;
    }
    if(opcode==1){
      ok=gml_software3d_model_end(graphics,model_id);
      continue;
    }
    if(opcode>=2&&opcode<=9){
      GmlSoftware3DVertex vertex={0};
      vertex.r=vertex.g=vertex.b=255; vertex.alpha=1;
      if(token_n<4){ ok=0; break; }
      vertex.x=token[1]; vertex.y=token[2]; vertex.z=token[3]; int index=4;
      int normal=opcode>=6,texture=opcode==4||opcode==5||opcode==8||opcode==9;
      int colored=opcode==3||opcode==5||opcode==7||opcode==9;
      if(normal){ if(token_n<index+3){ ok=0; break; }
        vertex.nx=token[index++]; vertex.ny=token[index++]; vertex.nz=token[index++]; vertex.has_normal=1; }
      if(texture){ if(token_n<index+2){ ok=0; break; } vertex.u=token[index++]; vertex.v=token[index++]; }
      if(colored){ if(token_n<index+2){ ok=0; break; } uint32_t color=(uint32_t)token[index++];
        vertex.r=color&255; vertex.g=(color>>8)&255; vertex.b=(color>>16)&255; vertex.alpha=token[index++]; }
      ok=gml_software3d_model_append_vertex(graphics,model_id,vertex);
      continue;
    }
    if(opcode>=10&&opcode<=15){
      if(token_n<9){ ok=0; break; }
      double vrepeat=token[8];
      int closed=1,steps=16;
      if(opcode==11||opcode==12){
        int base=9;
        if(token_n>=12){ vrepeat=token[9]; base=10; }
        if(token_n<=base+1){ ok=0; break; }
        closed=(int)token[base]; steps=(int)token[base+1];
      } else if(opcode==13){
        int at=9; if(token_n>=11){ vrepeat=token[9]; at=10; }
        if(token_n<=at){ ok=0; break; } steps=(int)token[at];
      } else if(token_n>=10) vrepeat=token[9];
      ok=gml_software3d_model_add_shape(
        graphics,model_id,opcode,token[1],token[2],token[3],
        token[4],token[5],token[6],token[7],vrepeat,closed,steps);
      continue;
    }
    ok=0;
  }
  vm_file_close(vm,slot);
  if(!ok) (void)gml_software3d_model_clear(graphics,model_id);
  return ok;
}

/* Asset-type values are serialized into current bytecode formats as ordinary numbers.  Keep lookup
 * name-based because resource indices overlap between asset classes. */
enum {
  GML_ASSET_UNKNOWN=-1,
  GML_ASSET_OBJECT=0,
  GML_ASSET_SPRITE=1,
  GML_ASSET_SOUND=2,
  GML_ASSET_ROOM=3,
  GML_ASSET_PATH=4,
  GML_ASSET_SCRIPT=5,
  GML_ASSET_FONT=6,
  GML_ASSET_TIMELINE=7,
  GML_ASSET_SHADER=8,
  GML_ASSET_SEQUENCE=9,
  GML_ASSET_ANIMATION_CURVE=10,
  GML_ASSET_PARTICLE_SYSTEM=11,
  GML_ASSET_TILESET=13
};

/* Resource-list chunks store a count and absolute record pointers; modern list families add a
 * version word before the count.  Every native record starts with its STRG name pointer. */
/* Reverse gml_win_asset_index_by_name by returning the stored name at a given index. Return NULL
 * for an absent index so callers can distinguish lookup failure from an empty stored name. */
static const char *chunk_asset_name_by_index(const GmlWin *w, const char *chunk, int versioned,
                                             int index){
  const GmlChunk *c=(w&&index>=0)?gml_chunk(w,chunk):NULL;
  if(!c || (size_t)c->off+c->size>w->size) return NULL;
  size_t count_at=(size_t)c->off+(versioned?4u:0u), end=(size_t)c->off+c->size;
  if(count_at+4>end) return NULL;
  uint32_t count=u32(w->data,(uint32_t)count_at);
  size_t table=count_at+4;
  if(count>(end-table)/4u || (uint32_t)index>=count) return NULL;
  uint32_t record=u32(w->data,(uint32_t)(table+(size_t)index*4u));
  if((size_t)record+4>w->size) return NULL;
  return gml_str_by_ptr(w,u32(w->data,record));
}

static int asset_index_and_type_by_name(GmlVM *vm, const char *name, int *out_type){
  int index=-1, type=GML_ASSET_UNKNOWN;
  GmlRender *render=vm?vm->render:NULL;
  if(!vm || !vm->win || !name || !*name) goto done;

  index=gml_render_named_sprite(render,name);
  if(index>=0){ type=GML_ASSET_SPRITE; goto done; }
  index=gml_win_asset_index_by_name(vm->win,"SPRT",0,name);
  if(index>=0){ type=GML_ASSET_SPRITE; goto done; }
  index=gml_win_asset_index_by_name(vm->win,"SOND",0,name);
  if(index>=0){ type=GML_ASSET_SOUND; goto done; }
  index=gml_win_asset_index_by_name(vm->win,"BGND",0,name);
  if(index>=0){ type=GML_ASSET_TILESET; goto done; }
  index=gml_win_asset_index_by_name(vm->win,"PATH",0,name);
  if(index>=0){ type=GML_ASSET_PATH; goto done; }
  index=gml_win_asset_index_by_name(vm->win,"SCPT",0,name);
  if(index<0){
    /* Script records may use the gml_Script_ prefix associated with their CODE entries.
     * Try that spelling only after the exact SCPT name fails, preserving exact-match priority. */
    char prefixed[256];
    if(snprintf(prefixed,sizeof prefixed,"gml_Script_%s",name)<(int)sizeof prefixed)
      index=gml_win_asset_index_by_name(vm->win,"SCPT",0,prefixed);
  }
  if(index>=0){ type=GML_ASSET_SCRIPT; goto done; }
  index=gml_win_asset_index_by_name(vm->win,"FONT",0,name);
  if(index>=0){ type=GML_ASSET_FONT; goto done; }
  for(int i=0;i<vm->n_timelines;i++)
    if(vm->timelines[i].name && !strcmp(vm->timelines[i].name,name)){
      index=i; type=GML_ASSET_TIMELINE; goto done;
    }
  index=gml_object_index_by_name(vm,name);
  if(index>=0){ type=GML_ASSET_OBJECT; goto done; }
  index=gml_room_index_by_name(vm->win,name);
  if(index>=0){ type=GML_ASSET_ROOM; goto done; }
  index=gml_win_asset_index_by_name(vm->win,"SHDR",0,name);
  if(index>=0){ type=GML_ASSET_SHADER; goto done; }
  index=gml_win_asset_index_by_name(vm->win,"SEQN",1,name);
  if(index>=0){ type=GML_ASSET_SEQUENCE; goto done; }
  index=gml_win_asset_index_by_name(vm->win,"ACRV",1,name);
  if(index>=0){ type=GML_ASSET_ANIMATION_CURVE; goto done; }
  index=gml_win_asset_index_by_name(vm->win,"PSYS",1,name);
  if(index>=0){ type=GML_ASSET_PARTICLE_SYSTEM; goto done; }
done:
  if(out_type) *out_type=type;
  return index;
}

/* Camera handles are opaque resources, not view indices. Keeping their compact runtime
 * records in reserved global arrays gives them the same rewind/save-state lifetime as the GML
 * view_camera[] bindings without growing the VM state format.  Older single-view code that never
 * creates a camera continues to use the legacy view_* arrays directly. */
static const char *const gml_camera_field_name[] = {
  "__gml_camera_x", "__gml_camera_y", "__gml_camera_w", "__gml_camera_h",
  "__gml_camera_angle", "__gml_camera_target", "__gml_camera_xspeed",
  "__gml_camera_yspeed", "__gml_camera_xborder", "__gml_camera_yborder"
};
enum {
  GML_CAM_X, GML_CAM_Y, GML_CAM_W, GML_CAM_H, GML_CAM_ANGLE,
  GML_CAM_TARGET, GML_CAM_XSPEED, GML_CAM_YSPEED, GML_CAM_XBORDER, GML_CAM_YBORDER
};
static int gml_camera_live(GmlVM *vm,int id){
  return vm && id>=0 && id<GML_CAMERA_LIMIT &&
         gml_global_arr(vm,"__gml_camera_live",id)>=0.5;
}
static double gml_camera_field(GmlVM *vm,int id,int field,double fallback){
  if(!gml_camera_live(vm,id) || field<0 || field>GML_CAM_YBORDER) return fallback;
  return gml_global_arr(vm,gml_camera_field_name[field],id);
}
static void gml_camera_field_set(GmlVM *vm,int id,int field,double value){
  if(!vm || id<0 || id>=GML_CAMERA_LIMIT || field<0 || field>GML_CAM_YBORDER) return;
  gml_set_global_arr(vm,gml_camera_field_name[field],id,value);
}
int gml_camera_override_mask(GmlVM *vm,uint64_t camera_mask,int field,double value){
  if(!vm || field<GML_CAM_X || field>GML_CAM_H) return 0;
  int changed=0;
  for(int camera=0;camera<GML_CAMERA_LIMIT;camera++){
    if(!(camera_mask&(UINT64_C(1)<<camera)) || !gml_camera_live(vm,camera)) continue;
    gml_camera_field_set(vm,camera,field,value);
    if(field==GML_CAM_X || field==GML_CAM_Y)
      gml_set_global_arr(vm,"__gml_camera_matrix_eye_valid",camera,0);
    changed++;
  }
  return changed;
}
static int gml_camera_alloc(GmlVM *vm){
  static const double defaults[10]={0,0,0,0,0,-1,-1,-1,0,0};
  int first=vm && vm->win && anygm_policy_has_modern_layer_semantics(vm->win)
    ?GML_ROOM_CAMERA_COUNT:0;
  int available=GML_CAMERA_LIMIT-first;
  int hint=(int)gml_global_num(vm,"__gml_camera_next");
  if(hint<first || hint>=GML_CAMERA_LIMIT) hint=first;
  for(int pass=0;pass<available;pass++){
    int id=first+(hint-first+pass)%available;
    if(!gml_camera_live(vm,id)){
      gml_set_global_arr(vm,"__gml_camera_live",id,1);
      for(int field=GML_CAM_X;field<=GML_CAM_YBORDER;field++)
        gml_camera_field_set(vm,id,field,defaults[field]);
      gml_set_global_arr(vm,"__gml_camera_matrix_eye_x",id,0);
      gml_set_global_arr(vm,"__gml_camera_matrix_eye_y",id,0);
      gml_set_global_arr(vm,"__gml_camera_matrix_eye_valid",id,0);
      gml_set_global_scalar(vm,"__gml_camera_next",
        first+(id-first+1)%available);
      return id;
    }
  }
  return -1;
}
static int gml_view_camera_id(GmlVM *vm,int view){
  if(!vm || view<0 || view>=8) return view;
  int id=(int)gml_global_arr(vm,"view_camera",view);
  return gml_camera_live(vm,id)?id:view;
}

/* Matrix cameras describe an eye at the centre of an orthographic projection, while the
 * presentation code consumes the top-left rectangle used by camera_create_view. Preserve the
 * matrix-only centre in serialized globals and derive the common rectangle after either setter;
 * either setter may arrive first. */
static void gml_camera_apply_matrix_rect(GmlVM *vm,int camera){
  if(!gml_camera_live(vm,camera) ||
     gml_global_arr(vm,"__gml_camera_matrix_eye_valid",camera)<0.5) return;
  double width=gml_camera_field(vm,camera,GML_CAM_W,0);
  double height=gml_camera_field(vm,camera,GML_CAM_H,0);
  double centre_x=gml_global_arr(vm,"__gml_camera_matrix_eye_x",camera);
  double centre_y=gml_global_arr(vm,"__gml_camera_matrix_eye_y",camera);
  gml_camera_field_set(vm,camera,GML_CAM_X,centre_x-(width>0?width*.5:0));
  gml_camera_field_set(vm,camera,GML_CAM_Y,centre_y-(height>0?height*.5:0));
}

static int gml_camera_set_view_matrix(GmlVM *vm,int camera,GmlVal matrix_value){
  double matrix[16];
  if(!gml_camera_live(vm,camera) || !gm_matrix_read(matrix_value,matrix)) return 0;
  /* matrix_build_lookat puts the camera axes in the first three rows.  For an orthonormal view,
   * inverse(rotation) * -translation recovers the world-space eye. */
  double eye_x=-(matrix[0]*matrix[12]+matrix[1]*matrix[13]+matrix[2]*matrix[14]);
  double eye_y=-(matrix[4]*matrix[12]+matrix[5]*matrix[13]+matrix[6]*matrix[14]);
  gml_set_global_arr(vm,"__gml_camera_matrix_eye_x",camera,eye_x);
  gml_set_global_arr(vm,"__gml_camera_matrix_eye_y",camera,eye_y);
  gml_set_global_arr(vm,"__gml_camera_matrix_eye_valid",camera,1);
  gml_camera_field_set(vm,camera,GML_CAM_ANGLE,atan2(matrix[4],matrix[0])*180.0/M_PI);
  gml_camera_apply_matrix_rect(vm,camera);
  return 1;
}

static int gml_camera_set_projection_matrix(GmlVM *vm,int camera,GmlVal matrix_value){
  double matrix[16];
  if(!gml_camera_live(vm,camera) || !gm_matrix_read(matrix_value,matrix)) return 0;
  /* Orthographic matrices retain W in 2/m00 and H in 2/m11.  Perspective matrices have m15=0
   * and do not describe a finite 2-D view rectangle, so they leave the existing dimensions. */
  if(fabs(matrix[15]-1.0)<1e-8 && fabs(matrix[0])>1e-12 && fabs(matrix[5])>1e-12){
    gml_camera_field_set(vm,camera,GML_CAM_W,fabs(2.0/matrix[0]));
    gml_camera_field_set(vm,camera,GML_CAM_H,fabs(2.0/matrix[5]));
    gml_camera_apply_matrix_rect(vm,camera);
    return 1;
  }
  return 0;
}
GmlVal gml_builtin_try_draw(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- drawing ---- */
  { GmlRender *R=(GmlRender*)vm->render;
    /* Vertex format/buffer API. All paths feed the same bounded software
     * vertex store and renderer; aliases cover API generations that exposed
     * colour/color and the early textcoord spelling. */
    if(!strcmp(nm,"vertex_format_begin")){
      gml_software3d_vertex_format_begin(GML_GRAPHICS); return vreal(0); }
    if(!strcmp(nm,"vertex_format_add_position")){
      gml_software3d_vertex_format_add(
        GML_GRAPHICS,GML_SOFTWARE3D_VERTEX_POSITION2,2,1,2,8);
      return vreal(0); }
    if(!strcmp(nm,"vertex_format_add_position_3d")){
      gml_software3d_vertex_format_add(
        GML_GRAPHICS,GML_SOFTWARE3D_VERTEX_POSITION3,3,1,3,12);
      return vreal(0); }
    if(!strcmp(nm,"vertex_format_add_colour")||!strcmp(nm,"vertex_format_add_color")){
      gml_software3d_vertex_format_add(
        GML_GRAPHICS,GML_SOFTWARE3D_VERTEX_COLOR,5,2,4,4);
      return vreal(0); }
    if(!strcmp(nm,"vertex_format_add_normal")){
      gml_software3d_vertex_format_add(
        GML_GRAPHICS,GML_SOFTWARE3D_VERTEX_NORMAL,3,3,3,12);
      return vreal(0); }
    if(!strcmp(nm,"vertex_format_add_texcoord")||!strcmp(nm,"vertex_format_add_textcoord")){
      gml_software3d_vertex_format_add(
        GML_GRAPHICS,GML_SOFTWARE3D_VERTEX_TEXCOORD,2,4,2,8);
      return vreal(0); }
    if(!strcmp(nm,"vertex_format_add_custom")){
      int type=(int)N(a,n,0),usage=(int)N(a,n,1),components=type>=1&&type<=4?type:4;
      int bytes=(type>=1&&type<=4)?type*4:4;
      /* A custom attribute does not become a fixed-function vertex field. */
      gml_software3d_vertex_format_add(
        GML_GRAPHICS,GML_SOFTWARE3D_VERTEX_CUSTOM,type,usage,components,bytes);
      return vreal(0); }
    if(!strcmp(nm,"vertex_format_end")) return vreal(gml_software3d_vertex_format_finish(GML_GRAPHICS));
    if(!strcmp(nm,"vertex_format_delete")){
      gml_software3d_vertex_format_delete(GML_GRAPHICS,(int)N(a,n,0));
      return vreal(0); }
    if(!strcmp(nm,"vertex_create_buffer")) return vreal(gml_software3d_vertex_buffer_create(GML_GRAPHICS,0));
    if(!strcmp(nm,"vertex_create_buffer_ext")){
      double requested=N(a,n,0); int bytes=requested>INT_MAX?INT_MAX:(requested>0?(int)requested:0);
      return vreal(gml_software3d_vertex_buffer_create(GML_GRAPHICS,bytes)); }
    if(!strcmp(nm,"vertex_delete_buffer")){
      gml_software3d_vertex_buffer_delete(GML_GRAPHICS,(int)N(a,n,0));
      return vreal(0); }
    if(!strcmp(nm,"vertex_begin")) return vreal(gml_software3d_vertex_buffer_begin(GML_GRAPHICS,(int)N(a,n,0),(int)N(a,n,1))?0:-1);
    if(!strcmp(nm,"vertex_end")){
      gml_software3d_vertex_buffer_end(GML_GRAPHICS,(int)N(a,n,0));
      return vreal(0); }
    if(!strcmp(nm,"vertex_position")||!strcmp(nm,"vertex_position_3d")||
       !strcmp(nm,"vertex_normal")||!strcmp(nm,"vertex_texcoord")||!strcmp(nm,"vertex_textcoord")){
      double value[4]={N(a,n,1),N(a,n,2),N(a,n,3),0};
      int kind=GML_SOFTWARE3D_VERTEX_POSITION2;
      if(!strcmp(nm,"vertex_position_3d"))
        kind=GML_SOFTWARE3D_VERTEX_POSITION3;
      else if(!strcmp(nm,"vertex_normal"))
        kind=GML_SOFTWARE3D_VERTEX_NORMAL;
      else if(strstr(nm,"texcoord")||strstr(nm,"textcoord"))
        kind=GML_SOFTWARE3D_VERTEX_TEXCOORD;
      gml_software3d_vertex_buffer_attribute(GML_GRAPHICS,(int)N(a,n,0),kind,value,n-1); return vreal(0); }
    if(!strcmp(nm,"vertex_colour")||!strcmp(nm,"vertex_color")){
      double value[4]={N(a,n,1),N(a,n,2),0,0};
      gml_software3d_vertex_buffer_attribute(
        GML_GRAPHICS,(int)N(a,n,0),GML_SOFTWARE3D_VERTEX_COLOR,value,2);
      return vreal(0); }
    if(!strcmp(nm,"vertex_argb")){
      /* The low 24 bits use the ordinary colour layout with red in the low byte and blue
       * in the high byte; the top byte supplies coverage. Do not exchange red and blue. */
      uint32_t argb=NU32(a,n,1);
      double value[4]={(double)(argb&0xFFFFFFu),
                       ((argb>>24)&255)/255.0,0,0};
      gml_software3d_vertex_buffer_attribute(
        GML_GRAPHICS,(int)N(a,n,0),GML_SOFTWARE3D_VERTEX_COLOR,value,2);
      return vreal(0); }
    if(!strcmp(nm,"vertex_float1")||!strcmp(nm,"vertex_float2")||
       !strcmp(nm,"vertex_float3")||!strcmp(nm,"vertex_float4")){
      int count=nm[strlen(nm)-1]-'0'; double value[4]={0,0,0,0};
      for(int i=0;i<count;i++) value[i]=N(a,n,i+1);
      gml_software3d_vertex_buffer_attribute(
        GML_GRAPHICS,(int)N(a,n,0),GML_SOFTWARE3D_VERTEX_CUSTOM,value,count);
      return vreal(0); }
    if(!strcmp(nm,"vertex_ubyte4")){
      double value[4]={N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4)};
      (void)gml_software3d_vertex_buffer_attribute_ubyte4(
        GML_GRAPHICS,(int)N(a,n,0),value);
      return vreal(0); }
    if(!strcmp(nm,"vertex_freeze")){
      return vreal(gml_software3d_vertex_buffer_freeze(
        GML_GRAPHICS,(int)N(a,n,0))?0:-1); }
    if(!strcmp(nm,"vertex_get_number")){
      return vreal(gml_software3d_vertex_buffer_number(
        GML_GRAPHICS,(int)N(a,n,0))); }
    if(!strcmp(nm,"vertex_get_buffer_size")){
      return vreal((double)gml_software3d_vertex_buffer_size(
        GML_GRAPHICS,(int)N(a,n,0))); }
    if(!strcmp(nm,"vertex_submit")||!strcmp(nm,"vertex_submit_ext")){
      int first=0,number=-1;
      if(!strcmp(nm,"vertex_submit_ext")){ first=(int)N(a,n,3); number=(int)N(a,n,4); }
      gml_software3d_vertex_submit_buffer(
        R,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),first,number);
      return vreal(0); }
    if(!strcmp(nm,"draw_sprite")){ if(R) gml_draw_sprite(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3)); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_ext")){ if(R) gml_draw_sprite_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),
        N(a,n,4),N(a,n,5),N(a,n,6),NU32(a,n,7),N(a,n,8)); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_pos")){
      if(R){ double x[4]={N(a,n,2),N(a,n,4),N(a,n,6),N(a,n,8)};
        double y[4]={N(a,n,3),N(a,n,5),N(a,n,7),N(a,n,9)};
        gml_draw_sprite_pos(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),x,y,N(a,n,10)); }
      return vreal(0); }
    /* draw_sprite_tiled(sprite,subimg,x,y) / _ext(...,xs,ys,color,alpha): tile a sprite to fill the screen */
    if(!strcmp(nm,"draw_sprite_tiled")){ GmlRenderDrawState draw=builtin_draw_state(R); if(R) gml_draw_sprite_tiled_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),1,1,0xFFFFFF,draw.alpha); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_tiled_ext")){ if(R) gml_draw_sprite_tiled_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),
        N(a,n,4),N(a,n,5),NU32(a,n,6),N(a,n,7)); return vreal(0); }
    if(!strcmp(nm,"draw_self") || (!strcmp(nm,"draw_full_sprite") && n==0)){
      GmlInstance *s=vm->cur_self;
      if(R&&s) gml_vm_draw_instance_sprite(
        vm,s,!strcmp(nm,"draw_full_sprite")?1:s->image_alpha);
      return vreal(0); }
    if(!strcmp(nm,"draw_set_color")||!strcmp(nm,"draw_set_colour")){ builtin_set_draw_color(R,NU32(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"draw_set_alpha")){ builtin_set_draw_alpha(R,N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"draw_set_font")){ builtin_set_draw_font(vm,R,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"draw_set_halign")){ builtin_set_draw_halign(R,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"draw_set_valign")){ builtin_set_draw_valign(R,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"draw_background")){ if(builtin_setting(vm,"GML_LOG_BG")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[bg] draw_background(%d,%g,%g)\n",(int)N(a,n,0),N(a,n,1),N(a,n,2)); if(R) gml_draw_background(R,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
    if(!strcmp(nm,"draw_background_tiled")){ int bd=(int)N(a,n,0),ht=1,vt=1; bg_layer_tiling(vm,bd,&ht,&vt);
      if(builtin_setting(vm,"GML_LOG_BG")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[bg] draw_background_tiled(%d,%g,%g) h=%d v=%d\n",bd,N(a,n,1),N(a,n,2),ht,vt);
      if(R) gml_draw_background_tiled(R,bd,N(a,n,1),N(a,n,2),ht,vt);
      return vreal(0); }
    /* _ext = +xscale,yscale,colour,alpha (background_ext also has a rotation arg before colour). */
    if(!strcmp(nm,"draw_background_tiled_ext")){ int bd=(int)N(a,n,0),ht=1,vt=1; bg_layer_tiling(vm,bd,&ht,&vt);
      if(builtin_setting(vm,"GML_LOG_BGA")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[bga] bg=%d x=%g y=%g alpha=%g\n",bd,N(a,n,1),N(a,n,2),N(a,n,6));
      if(R) gml_draw_background_tiled_ext(R,bd,N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),NU32(a,n,5),N(a,n,6),ht,vt);
      return vreal(0); }
    if(!strcmp(nm,"draw_background_ext")){
      if(builtin_setting(vm,"GML_LOG_BG")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[bg] draw_background_ext(%d,%g,%g,%g,%g,%g)\n",
        (int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,7));
      if(R) gml_draw_background_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),NU32(a,n,6),N(a,n,7));
      return vreal(0); }
    /* Surface / GUI draws. draw_surface_stretched(surf,x,y,w,h); _ext adds
     * (col,alpha); draw_sprite_stretched(spr,sub,x,y,w,h). */
    if(!strcmp(nm,"draw_surface_stretched")){ GmlRenderDrawState draw=builtin_draw_state(R); if(R) gml_draw_surface_stretched(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),0xFFFFFF,draw.alpha); return vreal(0); }
    if(!strcmp(nm,"draw_surface_stretched_ext")){ if(R) gml_draw_surface_stretched(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),NU32(a,n,5),N(a,n,6)); return vreal(0); }
    if(!strcmp(nm,"draw_surface")){ if(R){ int s=(int)N(a,n,0);
      GmlRenderDrawState draw=builtin_draw_state(R);
      GmlRenderTargetMetrics target=builtin_target_metrics(R);
      /* drawing the surface ASSIGNED TO THE VIEW (view_surface_id) is "present the frame": the yy
       * compat layer draws its port-sized view surface at (0,0) expecting it to cover the window.
       * Our GUI target is the native view canvas, so fit it to the target instead of clipping. */
      if(s>0 && s==(int)gml_global_arr(vm,"view_surface_id",0) && N(a,n,1)==0 && N(a,n,2)==0)
        gml_draw_surface_stretched(R,s,
          gml_render_gui_logical_x(R,target.camera_x),gml_render_gui_logical_y(R,target.camera_y),
          gml_render_gui_logical_width(R),gml_render_gui_logical_height(R),0xFFFFFF,draw.alpha);
      else
        gml_draw_surface_stretched(R,s,N(a,n,1),N(a,n,2),gml_surface_width(R,s),gml_surface_height(R,s),0xFFFFFF,draw.alpha); } return vreal(0); }
    if(!strcmp(nm,"draw_surface_tiled") || !strcmp(nm,"draw_surface_tiled_ext")){
      int extended=!strcmp(nm,"draw_surface_tiled_ext");
      double surface=N(a,n,0);
      if(!R || n<(extended?7:3) || !isfinite(surface) || surface<0 || surface>INT_MAX)
        return vreal(0);
      GmlRenderDrawState draw=builtin_draw_state(R);
      gml_draw_surface_tiled_ext(R,(int)surface,N(a,n,1),N(a,n,2),
        extended?N(a,n,3):1,extended?N(a,n,4):1,
        extended?NU32(a,n,5):0xFFFFFFu,extended?N(a,n,6):draw.alpha);
      return vreal(0);
    }
    if(!strcmp(nm,"draw_surface_ext")){ if(R) gml_draw_surface_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),NU32(a,n,6),N(a,n,7)); return vreal(0); }
    if(!strcmp(nm,"draw_surface_part_ext")){ if(R) gml_draw_surface_part_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),N(a,n,7),N(a,n,8),NU32(a,n,9),N(a,n,10)); return vreal(0); }
    if(!strcmp(nm,"draw_surface_general")){
      /* (surf,left,top,width,height,x,y,xscale,yscale,rot,c1,c2,c3,c4,alpha).
       * Approximate corner colours by c1 as draw_sprite_general does. Route complete
       * surfaces through rotation; partial regions retain the nonrotating part blit. */
      if(R){ int surf=(int)N(a,n,0);
        double left=N(a,n,1),top=N(a,n,2),width=N(a,n,3),height=N(a,n,4);
        int whole=left<=0.0 && top<=0.0 &&
          width>=(double)gml_surface_width(R,surf) && height>=(double)gml_surface_height(R,surf);
        if(whole)
          gml_draw_surface_ext(R,surf,N(a,n,5),N(a,n,6),N(a,n,7),N(a,n,8),N(a,n,9),
                               NU32(a,n,10),N(a,n,14));
        else
          gml_draw_surface_part_ext(R,surf,left,top,width,height,N(a,n,5),N(a,n,6),
                                    N(a,n,7),N(a,n,8),NU32(a,n,10),N(a,n,14));
      }
      return vreal(0); }
    if(!strcmp(nm,"draw_sprite_stretched")){ GmlRenderDrawState draw=builtin_draw_state(R); if(R) gml_draw_sprite_stretched(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),0xFFFFFF,draw.alpha); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_stretched_ext")){ if(R) gml_draw_sprite_stretched(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),NU32(a,n,6),N(a,n,7)); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_part")){ GmlRenderDrawState draw=builtin_draw_state(R); if(R) gml_draw_sprite_part_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),N(a,n,7),1,1,0xFFFFFF,draw.alpha); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_part_ext")){ if(R) gml_draw_sprite_part_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),N(a,n,7),N(a,n,8),N(a,n,9),NU32(a,n,10),N(a,n,11)); return vreal(0); }
    if(!strcmp(nm,"draw_sprite_general")){
      /* (sprite,subimg,left,top,width,height,x,y,xscale,yscale,rot,c1,c2,c3,c4,alpha).
       * Per-corner colors are approximated by c1; the part-blit path does not rotate. */
      if(R) gml_draw_sprite_part_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),
                                     N(a,n,6),N(a,n,7),N(a,n,8),N(a,n,9),NU32(a,n,11),N(a,n,15));
      return vreal(0); }
    if(!strcmp(nm,"draw_background_stretched")){ GmlRenderDrawState draw=builtin_draw_state(R); if(R) gml_draw_background_stretched(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),0xFFFFFF,draw.alpha); return vreal(0); }
    if(!strcmp(nm,"draw_background_stretched_ext")){ if(R) gml_draw_background_stretched(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),NU32(a,n,5),N(a,n,6)); return vreal(0); }
    if(!strcmp(nm,"draw_background_part_ext")){ if(R) gml_draw_background_part_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),N(a,n,7),N(a,n,8),NU32(a,n,9),N(a,n,10)); return vreal(0); }
    if(!strcmp(nm,"draw_background_part")){
      if(R){ GmlRenderDrawState draw=builtin_draw_state(R);
        gml_draw_background_part_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),
                                    N(a,n,4),N(a,n,5),N(a,n,6),1,1,0xFFFFFF,draw.alpha); }
      return vreal(0); }
    if(!strcmp(nm,"draw_rectangle")||!strcmp(nm,"draw_rectangle_colour")||!strcmp(nm,"draw_rectangle_color")){
      if(R){ int plain=!strcmp(nm,"draw_rectangle"); int outline=(int)N(a,n,plain?4:8);
        GmlRenderTargetMetrics target=builtin_target_metrics(R);
        GmlRenderDrawState draw=builtin_draw_state(R);
        uint32_t plain_color=draw.color;
        if(plain) (void)builtin_classic_hollow_rectangle(vm,n,&plain_color,&outline);
        double raster_x1=draw_gui_x(R,N(a,n,0))-target.camera_x;
        double raster_y1=draw_gui_y(R,N(a,n,1))-target.camera_y;
        double raster_x2=draw_gui_x(R,N(a,n,2))-target.camera_x;
        double raster_y2=draw_gui_y(R,N(a,n,3))-target.camera_y;
        int first_generation_outline=outline && vm->win &&
          anygm_policy_uses_first_generation_studio(vm->win);
        int x1=first_generation_outline?primitive_raster_coordinate(vm,raster_x1)
                                       :(int)floor(raster_x1);
        int y1=first_generation_outline?primitive_raster_coordinate(vm,raster_y1)
                                       :(int)floor(raster_y1);
        int x2=first_generation_outline?primitive_raster_coordinate(vm,raster_x2)
                                       :(int)ceil(raster_x2);
        int y2=first_generation_outline?primitive_raster_coordinate(vm,raster_y2)
                                       :(int)ceil(raster_y2);
        /* Modern outlines stroke the ring immediately outside the pixels filled by the same
         * coordinates. Classic and first-generation paths retain their own edge conventions. */
        if(outline && !first_generation_outline && !gml_render_is_classic(R)){ x1--; y1--; x2++; y2++; }
        if(plain) gml_render_primitive_rectangle(R,x1,y1,x2,y2,plain_color,outline);
        else gml_render_primitive_rectangle_color(R,x1,y1,x2,y2,NU32(a,n,4),NU32(a,n,5),NU32(a,n,6),NU32(a,n,7),outline); }
      return vreal(0); }
    if(!strcmp(nm,"draw_point")){ if(R){ GmlRenderTargetMetrics target=builtin_target_metrics(R); GmlRenderDrawState draw=builtin_draw_state(R);
        gml_render_primitive_point(R,primitive_raster_coordinate(vm,draw_gui_x(R,N(a,n,0))-target.camera_x),primitive_raster_coordinate(vm,draw_gui_y(R,N(a,n,1))-target.camera_y),draw.color); } return vreal(0); }
    if(!strcmp(nm,"draw_point_color")||!strcmp(nm,"draw_point_colour")){ if(R){ GmlRenderTargetMetrics target=builtin_target_metrics(R);
        gml_render_primitive_point(R,primitive_raster_coordinate(vm,draw_gui_x(R,N(a,n,0))-target.camera_x),primitive_raster_coordinate(vm,draw_gui_y(R,N(a,n,1))-target.camera_y),NU32(a,n,2)); } return vreal(0); }
    if(!strcmp(nm,"draw_line")||!strcmp(nm,"draw_line_color")||!strcmp(nm,"draw_line_colour")||!strcmp(nm,"draw_line_width")||!strcmp(nm,"draw_line_width_color")||!strcmp(nm,"draw_line_width_colour")){
      if(R){ int has_col=strstr(nm,"color")||strstr(nm,"colour"); int has_w=strstr(nm,"width")!=NULL;
        GmlRenderTargetMetrics target=builtin_target_metrics(R);
        GmlRenderDrawState draw=builtin_draw_state(R);
        uint32_t first=has_col?NU32(a,n,has_w?5:4):draw.color;
        uint32_t second=has_col?NU32(a,n,has_w?6:5):first;
        int width=has_w?(int)lround(draw_gui_w(R,N(a,n,4))):1; if(width<1) width=1;
        double x1=draw_gui_x(R,N(a,n,0))-target.camera_x;
        double y1=draw_gui_y(R,N(a,n,1))-target.camera_y;
        double x2=draw_gui_x(R,N(a,n,2))-target.camera_x;
        double y2=draw_gui_y(R,N(a,n,3))-target.camera_y;
        if(vm->win && anygm_policy_uses_first_generation_studio(vm->win)){
          double bias_x=fabs(draw_gui_w(R,1.0));
          double bias_y=fabs(draw_gui_h(R,1.0));
          gml_render_primitive_line_color_subpixel(
            R,x1+bias_x,y1+bias_y,x2+bias_x,y2+bias_y,first,second,width);
        } else {
          gml_render_primitive_line_color(R,(int)floor(x1),(int)floor(y1),
                                         (int)floor(x2),(int)floor(y2),
                                         first,second,width);
        } }
      return vreal(0); }
    if(!strcmp(nm,"draw_arrow")){
      if(R){
        GmlRenderTargetMetrics target=builtin_target_metrics(R);
        GmlRenderDrawState draw=builtin_draw_state(R);
        double x1=draw_gui_x(R,N(a,n,0))-target.camera_x, y1=draw_gui_y(R,N(a,n,1))-target.camera_y;
        double x2=draw_gui_x(R,N(a,n,2))-target.camera_x, y2=draw_gui_y(R,N(a,n,3))-target.camera_y;
        double head=(fabs(draw_gui_w(R,N(a,n,4)))+fabs(draw_gui_h(R,N(a,n,4))))*.5;
        double dx=x2-x1, dy=y2-y1, len=hypot(dx,dy);
        gml_render_primitive_line(R,(int)floor(x1),(int)floor(y1),(int)floor(x2),(int)floor(y2),draw.color,1);
        if(len>0.0 && head>0.0){
          double ux=dx/len, uy=dy/len, px=-uy, py=ux;
          double bx=x2-ux*head, by=y2-uy*head, wing=head*0.5;
          gml_render_primitive_line(R,(int)floor(x2),(int)floor(y2),(int)floor(bx+px*wing),(int)floor(by+py*wing),draw.color,1);
          gml_render_primitive_line(R,(int)floor(x2),(int)floor(y2),(int)floor(bx-px*wing),(int)floor(by-py*wing),draw.color,1);
        }
      }
      return vreal(0);
    }
    if(!strcmp(nm,"draw_path")){
      if(R){ int pi=(int)N(a,n,0), absolute=(int)N(a,n,3);
        if(pi>=0 && pi<vm->n_paths && vm->paths[pi].n>1 && vm->paths[pi].pts){
          GmlRenderTargetMetrics target=builtin_target_metrics(R);
          GmlRenderDrawState draw=builtin_draw_state(R);
          GmlPath *p=&vm->paths[pi];
          /* Relative mode places the path's first point at the requested position rather than
           * adding the position to every point. Subtract the first point to obtain the offset. */
          double ox=absolute?0:N(a,n,1)-p->pts[0].x, oy=absolute?0:N(a,n,2)-p->pts[0].y;
          for(int k=1;k<p->n;k++)
            gml_render_primitive_line(R,(int)floor(draw_gui_x(R,p->pts[k-1].x+ox)-target.camera_x),(int)floor(draw_gui_y(R,p->pts[k-1].y+oy)-target.camera_y),
                             (int)floor(draw_gui_x(R,p->pts[k].x+ox)-target.camera_x),(int)floor(draw_gui_y(R,p->pts[k].y+oy)-target.camera_y),draw.color,1);
          if(p->closed)
            gml_render_primitive_line(R,(int)floor(draw_gui_x(R,p->pts[p->n-1].x+ox)-target.camera_x),(int)floor(draw_gui_y(R,p->pts[p->n-1].y+oy)-target.camera_y),
                             (int)floor(draw_gui_x(R,p->pts[0].x+ox)-target.camera_x),(int)floor(draw_gui_y(R,p->pts[0].y+oy)-target.camera_y),draw.color,1);
        }
      }
      return vreal(0); }
    if(!strcmp(nm,"draw_circle")){ if(R){ GmlRenderDrawState draw=builtin_draw_state(R);
        double cx,cy,rx,ry;
        gml_render_circle_geometry(R,N(a,n,0),N(a,n,1),N(a,n,2),&cx,&cy,&rx,&ry);
        gml_render_maybe_prepare_draw(R); gml_render_primitive_circle_subpixel(R,cx,cy,rx,ry,draw.color,(int)N(a,n,3)); } return vreal(0); }
    if(!strcmp(nm,"draw_circle_color")||!strcmp(nm,"draw_circle_colour")){ if(R){
        double cx,cy,rx,ry;
        gml_render_circle_geometry(R,N(a,n,0),N(a,n,1),N(a,n,2),&cx,&cy,&rx,&ry);
        gml_render_maybe_prepare_draw(R);
        gml_render_primitive_circle_color_subpixel(R,cx,cy,rx,ry,NU32(a,n,3),NU32(a,n,4),(int)N(a,n,5)); }
      return vreal(0); }
    if(!strcmp(nm,"draw_ellipse_color")||!strcmp(nm,"draw_ellipse_colour")){ if(R){
        GmlRenderTargetMetrics target=builtin_target_metrics(R);
        gml_render_maybe_prepare_draw(R);
        int x1=(int)floor(draw_gui_x(R,N(a,n,0))-target.camera_x), y1=(int)floor(draw_gui_y(R,N(a,n,1))-target.camera_y), x2=(int)floor(draw_gui_x(R,N(a,n,2))-target.camera_x), y2=(int)floor(draw_gui_y(R,N(a,n,3))-target.camera_y);
        gml_render_primitive_circle_color(R,(x1+x2)/2,(y1+y2)/2,abs(x2-x1)/2,abs(y2-y1)/2,NU32(a,n,4),NU32(a,n,5),(int)N(a,n,6)); } return vreal(0); }
    if(!strcmp(nm,"draw_ellipse")){ if(R){
        GmlRenderTargetMetrics target=builtin_target_metrics(R); GmlRenderDrawState draw=builtin_draw_state(R);
        gml_render_maybe_prepare_draw(R);
        int x1=(int)floor(draw_gui_x(R,N(a,n,0))-target.camera_x), y1=(int)floor(draw_gui_y(R,N(a,n,1))-target.camera_y), x2=(int)floor(draw_gui_x(R,N(a,n,2))-target.camera_x), y2=(int)floor(draw_gui_y(R,N(a,n,3))-target.camera_y);
        gml_render_primitive_circle(R,(x1+x2)/2,(y1+y2)/2,abs(x2-x1)/2,abs(y2-y1)/2,draw.color,(int)N(a,n,4)); } return vreal(0); }
    if(!strcmp(nm,"draw_roundrect")){ if(R){ GmlRenderTargetMetrics target=builtin_target_metrics(R); GmlRenderDrawState draw=builtin_draw_state(R);
        gml_render_primitive_rectangle(R,(int)floor(draw_gui_x(R,N(a,n,0))-target.camera_x),(int)floor(draw_gui_y(R,N(a,n,1))-target.camera_y),(int)ceil(draw_gui_x(R,N(a,n,2))-target.camera_x),(int)ceil(draw_gui_y(R,N(a,n,3))-target.camera_y),draw.color,(int)N(a,n,4)); } return vreal(0); }
    if(!strcmp(nm,"draw_roundrect_ext")||!strcmp(nm,"draw_roundrect_color_ext")||
       !strcmp(nm,"draw_roundrect_colour_ext")){
      if(R){
        int plain=!strcmp(nm,"draw_roundrect_ext");
        GmlRenderDrawState draw=builtin_draw_state(R);
        uint32_t inner=plain?draw.color:NU32(a,n,6),outer=plain?inner:NU32(a,n,7);
        gml_software3d_draw_roundrect_2d(R,N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),
          N(a,n,4),N(a,n,5),inner,outer,draw.alpha,N(a,n,plain?6:8)>=.5);
      }
      return vreal(0); }
    if(!strcmp(nm,"draw_roundrect_color")||!strcmp(nm,"draw_roundrect_colour")){
      if(R){ int outline=(int)N(a,n,6);
        GmlRenderTargetMetrics target=builtin_target_metrics(R);
        gml_render_primitive_rectangle(R,(int)floor(draw_gui_x(R,N(a,n,0))-target.camera_x),(int)floor(draw_gui_y(R,N(a,n,1))-target.camera_y),
                       (int)ceil(draw_gui_x(R,N(a,n,2))-target.camera_x),(int)ceil(draw_gui_y(R,N(a,n,3))-target.camera_y),
                       NU32(a,n,4),outline); }
      return vreal(0); }
    if(!strcmp(nm,"draw_healthbar")){
      if(R){ double amt=N(a,n,4); if(amt<0) amt=0; if(amt>100) amt=100;
        GmlRenderTargetMetrics target=builtin_target_metrics(R);
        int x1=(int)floor(draw_gui_x(R,N(a,n,0))-target.camera_x), y1=(int)floor(draw_gui_y(R,N(a,n,1))-target.camera_y);
        int x2=(int)ceil(draw_gui_x(R,N(a,n,2))-target.camera_x), y2=(int)ceil(draw_gui_y(R,N(a,n,3))-target.camera_y);
        if(x1>x2){ int t=x1; x1=x2; x2=t; } if(y1>y2){ int t=y1; y1=y2; y2=t; }
        if((int)N(a,n,9)) gml_render_primitive_rectangle(R,x1,y1,x2,y2,NU32(a,n,5),0);
        int dir=(int)N(a,n,8), fx1=x1,fy1=y1,fx2=x2,fy2=y2;
        if(dir==1) fx1=x2-(int)floor((x2-x1)*amt/100.0);
        else if(dir==2) fy2=y1+(int)floor((y2-y1)*amt/100.0);
        else if(dir==3) fy1=y2-(int)floor((y2-y1)*amt/100.0);
        else fx2=x1+(int)floor((x2-x1)*amt/100.0);
        gml_render_primitive_rectangle(R,fx1,fy1,fx2,fy2,NU32(a,n,7),0);
        if((int)N(a,n,10)) gml_render_primitive_rectangle(R,x1,y1,x2,y2,0,1);
      }
      return vreal(0); }
    if(!strcmp(nm,"draw_triangle")||!strcmp(nm,"draw_triangle_color")||!strcmp(nm,"draw_triangle_colour")){
      if(R){ GmlRenderTargetMetrics target=builtin_target_metrics(R); GmlRenderDrawState draw=builtin_draw_state(R);
        double x1=floor(draw_gui_x(R,N(a,n,0))-target.camera_x), y1=floor(draw_gui_y(R,N(a,n,1))-target.camera_y);
        double x2=floor(draw_gui_x(R,N(a,n,2))-target.camera_x), y2=floor(draw_gui_y(R,N(a,n,3))-target.camera_y);
        double x3=floor(draw_gui_x(R,N(a,n,4))-target.camera_x), y3=floor(draw_gui_y(R,N(a,n,5))-target.camera_y);
        uint32_t col=!strcmp(nm,"draw_triangle")?draw.color:NU32(a,n,6); int outline=(int)N(a,n,!strcmp(nm,"draw_triangle")?6:9);
        gml_render_maybe_prepare_draw(R);
        gml_render_primitive_triangle_alpha(R,x1,y1,x2,y2,x3,y3,col,draw.alpha,outline); }
      return vreal(0); }
    if(!strcmp(nm,"draw_clear")||!strcmp(nm,"draw_clear_alpha")){ if(R){
      if(builtin_setting(vm,"GML_LOG_SURF"))
        anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_DEBUG,"[surf] clear colour=%06x\n",
                        (unsigned)(NU32(a,n,0)&0xFFFFFFu));
      /* Pass the requested alpha through when clearing the target surface. */
      double clear_alpha=!strcmp(nm,"draw_clear_alpha")&&n>=2?
        N(a,n,1):1.0;
      gml_render_clear(R,NU32(a,n,0),clear_alpha);
    } return vreal(0); }
    if(!strcmp(nm,"draw_getpixel")){ if(R){ uint32_t p=0; int x=(int)draw_gui_x(R,N(a,n,0)), y=(int)draw_gui_y(R,N(a,n,1)); if(gml_render_target_pixel(R,x,y,&p)) return vreal(((p>>16)&0xff) | (p&0xff00) | ((p&0xff)<<16)); } return vreal(0); }
    if(!strcmp(nm,"draw_enable_alphablend")){ builtin_set_alpha_blend(R,N(a,n,0)>=0.5); return vreal(0); }
    if(!strcmp(nm,"draw_enable_drawevent")){ vm->draw_events_off = N(a,n,0)<0.5; return vreal(0); }
    if(!strcmp(nm,"draw_surface_part")){ if(R){ GmlRenderDrawState draw=builtin_draw_state(R); gml_draw_surface_part_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),1,1,0xFFFFFF,draw.alpha); } return vreal(0); }
    if(!strcmp(nm,"draw_get_font")) return vreal(builtin_draw_state(R).font);
    if(!strcmp(nm,"draw_get_color")||!strcmp(nm,"draw_get_colour")){ if(!R) return vreal(0);
      uint32_t c=builtin_draw_state(R).color;
      double out=(double)(((c>>16)&0xff)|(c&0xff00)|((c&0xff)<<16));
      /* Optional direct logging of the current draw-colour query result. */
      if(builtin_setting(vm,"GML_LOG_DRAW_COLOR"))
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[drawcolor] f%ld get=%.0f raw=%08x\n",
                        vm?vm->frame:0,out,c);
      return vreal(out); }
    if(!strcmp(nm,"draw_get_alpha")) return vreal(builtin_draw_state(R).alpha);
    if(!strcmp(nm,"draw_get_halign")) return vreal(builtin_draw_state(R).horizontal_alignment);
    if(!strcmp(nm,"draw_get_valign")) return vreal(builtin_draw_state(R).vertical_alignment);
    /* Immediate-mode primitives: draw_primitive_begin(kind), draw_vertex variants, and end.
     * SW raster: points/lines direct; triangle list/strip/fan via the same barycentric fill as
     * draw_triangle. Per-vertex colours flat-shaded with the first colour of each triangle
     * using the first color of each triangle. */
    if(!strcmp(nm,"draw_primitive_begin")||!strcmp(nm,"draw_primitive_begin_texture")){
      gml_software3d_primitive_2d_begin(
        GML_GRAPHICS,(int)N(a,n,0),
        !strcmp(nm,"draw_primitive_begin_texture")?(int)N(a,n,1):-1);
      return vreal(0); }
    if(!strcmp(nm,"draw_vertex")||!strcmp(nm,"draw_vertex_colour")||!strcmp(nm,"draw_vertex_color")||
       !strcmp(nm,"draw_vertex_texture")||!strcmp(nm,"draw_vertex_texture_colour")||!strcmp(nm,"draw_vertex_texture_color")){
      GmlRenderDrawState draw=builtin_draw_state(R);
      int tex=!strncmp(nm,"draw_vertex_texture",19);
      int coli=tex?4:2;                                 /* texture form: (x,y,u,v[,col,alpha]) */
      (void)gml_software3d_primitive_2d_append(
        GML_GRAPHICS,N(a,n,0),N(a,n,1),
        tex?N(a,n,2):0,tex?N(a,n,3):0,
        (n>coli)?NU32(a,n,coli):draw.color,
        (n>coli+1)?N(a,n,coli+1):draw.alpha);
      return vreal(0); }
    if(!strcmp(nm,"draw_primitive_end")){
      if(R) gml_software3d_primitive_2d_end(R);
      return vreal(0); }
    if(!strcmp(nm,"draw_get_circle_precision")) return vreal(builtin_draw_state(R).circle_precision);
    if(!strcmp(nm,"draw_set_circle_precision")){ if(R){ GmlRenderDrawState draw={0}; int p=(int)N(a,n,0); if(p<4) p=4; if(p>64) p=64; p=(p/4)*4; if(p<4) p=4; draw.circle_precision=p; gml_render_draw_state_update(R,&draw,GML_RENDER_DRAW_STATE_CIRCLE_PRECISION); } return vreal(0); }
    if(!strcmp(nm,"application_surface")) return vreal(0);
    /* ---- FMOD Studio extension (fmod-gamemaker): games route ALL audio through it (their AUDO chunk
     * is empty; sound lives in external .bank files). We resolve the event path → GUID → FSB5 subsound
     * and play it through the FMOD software mixer (gml_fmod.c). If no bank set loaded (fb==NULL) we
     * still model the instance LIFECYCLE STATE so audio-gated sequences advance. Generic: any
     * GameMaker+FMOD title uses this same API. ---- */
    if(!strncmp(nm,"fmod_",5)) return builtin_fmod(vm,nm,a,n);
    if(!strcmp(nm,"surface_exists")) return vreal(R?gml_surface_exists(R,(int)N(a,n,0)):0);
    /* Opt-in lifecycle trace complements renderer coverage diagnostics. */
    if(!strcmp(nm,"surface_create")){
      int made=R?gml_surface_create(R,(int)N(a,n,0),(int)N(a,n,1)):-1;
      if(R && builtin_setting(vm,"GML_LOG_SURF"))
        anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_DEBUG,"[surf] create id=%d %dx%d\n",
                        made,(int)N(a,n,0),(int)N(a,n,1));
      return vreal(made);
    }
    if(!strcmp(nm,"surface_free")){
      if(R && builtin_setting(vm,"GML_LOG_SURF"))
        anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_DEBUG,"[surf] free id=%d\n",(int)N(a,n,0));
      if(R) gml_surface_free(R,(int)N(a,n,0));
      return vreal(0);
    }
    if(!strcmp(nm,"surface_get_target")) return vreal(R?gml_surface_get_target(R):-1);
    if(!strcmp(nm,"surface_get_texture")){
      int sid=(int)N(a,n,0);
      return vreal((R&&gml_surface_exists(R,sid))?
        gml_render_surface_texture_handle(sid):-1);
    }
    if(!strcmp(nm,"surface_set_target")){ if(builtin_setting(vm,"GML_LOG_SURF"))anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[surf] set_target %d\n",(int)N(a,n,0)); return vreal(R?gml_surface_set_target(R,(int)N(a,n,0)):0); }
    if(!strcmp(nm,"surface_reset_target")){ if(builtin_setting(vm,"GML_LOG_SURF"))anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[surf] reset_target\n"); if(R) gml_surface_reset_target(R); return vreal(0); }
    if(!strcmp(nm,"surface_resize")){ if(R) gml_surface_resize(R,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2)); return vreal(0); }
    if(!strcmp(nm,"surface_copy")){ if(R) gml_surface_copy(R,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),(int)N(a,n,3)); return vreal(0); }
    if(!strcmp(nm,"surface_copy_part")){ if(R) gml_surface_copy_part(R,
      (int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),(int)N(a,n,3),
      (int)N(a,n,4),(int)N(a,n,5),(int)N(a,n,6),(int)N(a,n,7)); return vreal(0); }
    if(!strcmp(nm,"surface_save")||!strcmp(nm,"surface_save_part")) return vreal(0);
    /* Without a per-surface depth buffer, store the toggle as a latch.
     * Its getter reads back the most recently set value. */
    if(!strcmp(nm,"surface_depth_disable")){ vm->builtins->surface_depth_disabled=(int)N(a,n,0); return vreal(0); }
    if(!strcmp(nm,"surface_get_depth_disable")) return vreal(vm->builtins->surface_depth_disabled);
    /* Post-Draw runs after the automatic application-surface presentation decision. A disable
     * during that pass cannot cancel the presentation already selected for this frame; calls
     * before or after that pass retain their ordinary effect. */
    if(!strcmp(nm,"application_surface_draw_enable")){
      int on=(int)N(a,n,0);
      if(on || !gml_render_application_surface_presentation_settled(R))
        gml_render_application_surface_set_draw_enabled(R,on);
      return vreal(0);
    }
    if(!strcmp(nm,"application_surface_enable")){ gml_render_application_surface_set_draw_enabled(R,(int)N(a,n,0)); return vreal(0); }
    /* ---- Studio camera API mapped onto legacy view globals and explicit room bindings. ---- */
    if(!strcmp(nm,"view_get_camera")) return vreal(gml_view_camera_id(vm,(int)N(a,n,0)));
    if(!strcmp(nm,"room_get_camera"))
      return vreal(gml_vm_room_camera_get(vm,(int)N(a,n,0),(int)N(a,n,1)));
    if(!strcmp(nm,"view_set_camera")){ int view=(int)N(a,n,0), camera=(int)N(a,n,1);
      if(view>=0 && view<8) gml_set_global_arr(vm,"view_camera",view,camera);
      return vreal(0); }
    if(!strcmp(nm,"camera_create")){ int c=gml_camera_alloc(vm);
      if(c>=0){
        gml_camera_field_set(vm,c,GML_CAM_TARGET,-1);
        gml_camera_field_set(vm,c,GML_CAM_XSPEED,-1); gml_camera_field_set(vm,c,GML_CAM_YSPEED,-1);
      }
      return vreal(c); }
    if(!strcmp(nm,"camera_create_view")){ int c=gml_camera_alloc(vm);
      if(c<0) return vreal(-1);
      gml_camera_field_set(vm,c,GML_CAM_X,N(a,n,0)); gml_camera_field_set(vm,c,GML_CAM_Y,N(a,n,1));
      gml_camera_field_set(vm,c,GML_CAM_W,N(a,n,2)); gml_camera_field_set(vm,c,GML_CAM_H,N(a,n,3));
      gml_camera_field_set(vm,c,GML_CAM_ANGLE,N(a,n,4));
      gml_camera_field_set(vm,c,GML_CAM_TARGET,n>5?N(a,n,5):-1);
      gml_camera_field_set(vm,c,GML_CAM_XSPEED,n>6?N(a,n,6):-1);
      gml_camera_field_set(vm,c,GML_CAM_YSPEED,n>7?N(a,n,7):-1);
      gml_camera_field_set(vm,c,GML_CAM_XBORDER,n>8?N(a,n,8):0);
      gml_camera_field_set(vm,c,GML_CAM_YBORDER,n>9?N(a,n,9):0);
      if(builtin_setting(vm,"GML_LOG_VIEW")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[camera] create id=%d world=(%.0f,%.0f %.0fx%.0f)\n",
        c,N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3));
      return vreal(c); }
    if(!strcmp(nm,"camera_destroy")){ int c=(int)N(a,n,0);
      if(c>=0 && c<GML_CAMERA_LIMIT){
        gml_set_global_arr(vm,"__gml_camera_live",c,0);
        gml_set_global_arr(vm,"__gml_camera_matrix_eye_valid",c,0);
      }
      return vreal(0); }
    if(!strcmp(nm,"room_set_camera"))
      return vreal(gml_vm_room_camera_set(
        vm,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2))?0:-1);
    if(!strcmp(nm,"camera_set_view_angle")){ int c=(int)N(a,n,0);
      if(gml_camera_live(vm,c)) gml_camera_field_set(vm,c,GML_CAM_ANGLE,N(a,n,1));
      return vreal(0); }
    if(!strcmp(nm,"camera_set_view_mat")){
      if(n>1) gml_camera_set_view_matrix(vm,(int)N(a,n,0),a[1]);
      return vreal(0); }
    if(!strcmp(nm,"camera_set_proj_mat")){
      if(n>1) gml_camera_set_projection_matrix(vm,(int)N(a,n,0),a[1]);
      return vreal(0); }
    if(!strcmp(nm,"camera_apply")||!strcmp(nm,"camera_set_default")) return vreal(0);
    if(!strcmp(nm,"camera_set_view_pos")){ int c=(int)N(a,n,0);
      if(gml_camera_live(vm,c)){
        gml_camera_field_set(vm,c,GML_CAM_X,N(a,n,1)); gml_camera_field_set(vm,c,GML_CAM_Y,N(a,n,2));
        gml_set_global_arr(vm,"__gml_camera_matrix_eye_valid",c,0);
      } else if(c>=0 && c<8){
        gml_set_global_arr(vm,"view_xview",c,N(a,n,1)); gml_set_global_arr(vm,"view_yview",c,N(a,n,2));
      }
      return vreal(0); }
    if(!strcmp(nm,"camera_set_view_target")){ int c=(int)N(a,n,0);
      if(gml_camera_live(vm,c)) gml_camera_field_set(vm,c,GML_CAM_TARGET,N(a,n,1));
      else if(c>=0 && c<8) gml_set_global_arr(vm,"view_object",c,N(a,n,1));
      return vreal(0); }
    /* Legacy view-port setters update the view_*port globals so readers and host scaling
     * use the requested window region. */
    if(!strcmp(nm,"view_set_wport")){ gml_set_global_arr(vm,"view_wport",(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"view_set_hport")){ gml_set_global_arr(vm,"view_hport",(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"view_set_xport")){ gml_set_global_arr(vm,"view_xport",(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"view_set_yport")){ gml_set_global_arr(vm,"view_yport",(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"view_set_visible")){ gml_set_global_arr(vm,"view_visible",(int)N(a,n,0),N(a,n,1)>=0.5?1:0); return vreal(0); }
    if(!strcmp(nm,"camera_set_view_size")){ int c=(int)N(a,n,0); double cw=N(a,n,1),chh=N(a,n,2);
      if(gml_camera_live(vm,c)){
        if(cw>0) gml_camera_field_set(vm,c,GML_CAM_W,cw);
        if(chh>0) gml_camera_field_set(vm,c,GML_CAM_H,chh);
      } else if(c>=0 && c<8){
        if(cw>0) gml_set_global_arr(vm,"view_wview",c,cw);
        if(chh>0) gml_set_global_arr(vm,"view_hview",c,chh);
      }
      return vreal(0); }
    if(!strcmp(nm,"camera_set_view_border")){ int c=(int)N(a,n,0);
      if(gml_camera_live(vm,c)){
        gml_camera_field_set(vm,c,GML_CAM_XBORDER,N(a,n,1)); gml_camera_field_set(vm,c,GML_CAM_YBORDER,N(a,n,2));
      } else if(c>=0 && c<8){
        gml_set_global_arr(vm,"view_hborder",c,N(a,n,1)); gml_set_global_arr(vm,"view_vborder",c,N(a,n,2));
      }
      return vreal(0); }
    if(!strcmp(nm,"camera_set_view_speed")){ int c=(int)N(a,n,0);
      if(gml_camera_live(vm,c)){
        gml_camera_field_set(vm,c,GML_CAM_XSPEED,N(a,n,1)); gml_camera_field_set(vm,c,GML_CAM_YSPEED,N(a,n,2));
      } else if(c>=0 && c<8){
        gml_set_global_arr(vm,"view_hspeed",c,N(a,n,1)); gml_set_global_arr(vm,"view_vspeed",c,N(a,n,2));
      }
      return vreal(0); }
    if(!strcmp(nm,"camera_copy_transforms")){
      int dst=(int)N(a,n,0), src=(int)N(a,n,1);
      if(gml_camera_live(vm,dst)){
        static const char *const legacy[10]={
          "view_xview","view_yview","view_wview","view_hview",NULL,
          "view_object","view_hspeed","view_vspeed","view_hborder","view_vborder"
        };
        static const double defaults[10]={0,0,0,0,0,-1,-1,-1,0,0};
        for(int field=GML_CAM_X;field<=GML_CAM_YBORDER;field++){
          double fallback=defaults[field];
          if(src>=0 && src<8 && legacy[field]) fallback=gml_global_arr(vm,legacy[field],src);
          gml_camera_field_set(vm,dst,field,gml_camera_field(vm,src,field,fallback));
        }
        gml_set_global_arr(vm,"__gml_camera_matrix_eye_valid",dst,0);
      }
      return vreal(0); }
    if(!strcmp(nm,"camera_get_view_x")){ int c=(int)N(a,n,0); return vreal(gml_camera_field(vm,c,GML_CAM_X,c>=0&&c<8?gml_global_arr(vm,"view_xview",c):0)); }
    if(!strcmp(nm,"camera_get_view_y")){ int c=(int)N(a,n,0); return vreal(gml_camera_field(vm,c,GML_CAM_Y,c>=0&&c<8?gml_global_arr(vm,"view_yview",c):0)); }
    if(!strcmp(nm,"camera_get_view_width")){ int c=(int)N(a,n,0); return vreal(gml_camera_field(vm,c,GML_CAM_W,c>=0&&c<8?gml_global_arr(vm,"view_wview",c):0)); }
    if(!strcmp(nm,"camera_get_view_height")){ int c=(int)N(a,n,0); return vreal(gml_camera_field(vm,c,GML_CAM_H,c>=0&&c<8?gml_global_arr(vm,"view_hview",c):0)); }
    if(!strcmp(nm,"camera_get_view_target")){ int c=(int)N(a,n,0); return vreal(gml_camera_field(vm,c,GML_CAM_TARGET,c>=0&&c<8?gml_global_arr(vm,"view_object",c):-1)); }
    if(!strcmp(nm,"camera_get_view_border_x")){ int c=(int)N(a,n,0); return vreal(gml_camera_field(vm,c,GML_CAM_XBORDER,c>=0&&c<8?gml_global_arr(vm,"view_hborder",c):0)); }
    if(!strcmp(nm,"camera_get_view_border_y")){ int c=(int)N(a,n,0); return vreal(gml_camera_field(vm,c,GML_CAM_YBORDER,c>=0&&c<8?gml_global_arr(vm,"view_vborder",c):0)); }
    if(!strcmp(nm,"camera_get_view_speed_x")){ int c=(int)N(a,n,0); return vreal(gml_camera_field(vm,c,GML_CAM_XSPEED,c>=0&&c<8?gml_global_arr(vm,"view_hspeed",c):-1)); }
    if(!strcmp(nm,"camera_get_view_speed_y")){ int c=(int)N(a,n,0); return vreal(gml_camera_field(vm,c,GML_CAM_YSPEED,c>=0&&c<8?gml_global_arr(vm,"view_vspeed",c):-1)); }
    if(!strcmp(nm,"camera_get_view_angle")){ int c=(int)N(a,n,0); return vreal(gml_camera_field(vm,c,GML_CAM_ANGLE,0)); }
    if(!strcmp(nm,"camera_get_default")||!strcmp(nm,"camera_get_active")) return vreal(0);
    /* Legacy pre-camera view accessors. view_* is the world region and *port is the on-screen
     * region; fall the port back to the view size. */
    if(!strcmp(nm,"view_get_xview")) return vreal(gml_global_arr(vm,"view_xview",(int)N(a,n,0)));
    if(!strcmp(nm,"view_get_yview")) return vreal(gml_global_arr(vm,"view_yview",(int)N(a,n,0)));
    if(!strcmp(nm,"view_get_wview")) return vreal(gml_global_arr(vm,"view_wview",(int)N(a,n,0)));
    if(!strcmp(nm,"view_get_hview")) return vreal(gml_global_arr(vm,"view_hview",(int)N(a,n,0)));
    /* Ports report the room's true on-window region. The presentation layer maps it to the output
     * canvas, while GUI and offscreen surfaces can derive their dimensions from it. */
    if(!strcmp(nm,"view_get_xport")) return vreal(gml_global_arr(vm,"view_xport",(int)N(a,n,0)));
    if(!strcmp(nm,"view_get_yport")) return vreal(gml_global_arr(vm,"view_yport",(int)N(a,n,0)));
    if(!strcmp(nm,"view_get_wport")){ int c=(int)N(a,n,0); double v=gml_global_arr(vm,"view_wport",c); return vreal(v>0?v:gml_global_arr(vm,"view_wview",c)); }
    if(!strcmp(nm,"view_get_hport")){ int c=(int)N(a,n,0); double v=gml_global_arr(vm,"view_hport",c); return vreal(v>0?v:gml_global_arr(vm,"view_hview",c)); }
    if(!strcmp(nm,"view_get_visible")) return vreal(1);
    /* view_surface_id directs a view into a surface that can later be composited in Draw GUI.
     * Store the assignment; the engine mirrors the drawn frame into that surface after the room
     * pass. */
    if(!strcmp(nm,"view_get_surface_id")){ double v=gml_global_arr(vm,"view_surface_id",(int)N(a,n,0));
      return vreal(v!=0?v:-1); }
    if(!strcmp(nm,"view_set_surface_id")){ gml_set_global_arr(vm,"view_surface_id",(int)N(a,n,0),N(a,n,1));
      return vreal(0); }
    /* display DPI: return a real 96 (not 0) so `x / display_get_dpi_x()` scaling code never divides by 0. */
    if(!strcmp(nm,"display_get_dpi_x")||!strcmp(nm,"display_get_dpi_y")) return vreal(96);
    if(!strcmp(nm,"display_get_frequency")) return vreal(60.0);
    if(!strcmp(nm,"display_get_orientation")) return vreal(0);
    if(!strcmp(nm,"os_lock_orientation")) return vreal(0);
    if(!strcmp(nm,"game_set_speed")){
      double spd=N(a,n,0), mode=N(a,n,1);
      builtin_set_game_speed(vm, mode>=0.5 ? (spd>0 ? 1000000.0/spd : 60.0) : spd);
      return vreal(0);
    }
    if(!strcmp(nm,"game_get_speed")){
      double fps=builtin_game_speed(vm), mode=N(a,n,0);
      return vreal(mode>=0.5 ? 1000000.0/fps : fps);
    }
    /* Window size follows the selected presentation from startup onward; classic display size
     * remains the separate virtual desktop reported by display_size(). */
    if(!strcmp(nm,"display_get_width")) return vreal(display_size(vm,R,0));
    if(!strcmp(nm,"display_get_height")) return vreal(display_size(vm,R,1));
    if(!strcmp(nm,"window_get_width")) return vreal(presentation_size(vm,R,0));
    if(!strcmp(nm,"window_get_height")) return vreal(presentation_size(vm,R,1));
    /* The host owns the native window. Recognize minimum-size hints explicitly while leaving
     * presentation dimensions under host control. */
    if(!strcmp(nm,"window_set_min_width")||!strcmp(nm,"window_set_min_height")) return vreal(0);
    if(!strcmp(nm,"display_get_gui_width")){
      GmlRenderTargetMetrics target=builtin_target_metrics(R);
      return vreal(vm->gui_w>0?vm->gui_w:(target.width>0?target.width:(vm->win&&vm->win->disp_w?(int)vm->win->disp_w:288))); }
    if(!strcmp(nm,"display_get_gui_height")){
      GmlRenderTargetMetrics target=builtin_target_metrics(R);
      return vreal(vm->gui_h>0?vm->gui_h:(target.height>0?target.height:(vm->win&&vm->win->disp_h?(int)vm->win->disp_h:216))); }
    if(!strcmp(nm,"get_timer")){
      /* Microseconds. GM code uses get_timer for timing and busy-wait frame limiters, so it must
       * advance within a single VM frame. A frame counter would spin forever, while a raw clock
       * depends on host load. Use a hybrid:
       * frame-locked base + intra-frame CPU advance (see gml_vm_get_timer_us). */
      return vreal(gml_vm_get_timer_us(vm));
    }
    if(!strcmp(nm,"surface_get_width")) return vreal(R?gml_surface_width(R,(int)N(a,n,0)):0);
    if(!strcmp(nm,"surface_get_height")) return vreal(R?gml_surface_height(R,(int)N(a,n,0)):0);
    if(!strcmp(nm,"surface_getpixel")){ int sid=(int)N(a,n,0), x=(int)N(a,n,1), y=(int)N(a,n,2);
      uint32_t p=0; int w=0,h=0,ok=0;
      const uint32_t *pixels=gml_surface_pixels_read(R,sid,&w,&h);
      if(pixels && x>=0 && y>=0 && x<w && y<h){ p=pixels[(size_t)y*w+x]; ok=1; }
      return vreal(ok?(((p>>16)&0xff)|(p&0xff00)|((p&0xff)<<16)):0); }
    if(!strcmp(nm,"sprite_get_width")){ GmlRenderSpriteMetrics sprite; return vreal(gml_render_sprite_metrics(R,(int)N(a,n,0),&sprite)?sprite.width:0); }
    if(!strcmp(nm,"sprite_get_height")){ GmlRenderSpriteMetrics sprite; return vreal(gml_render_sprite_metrics(R,(int)N(a,n,0),&sprite)?sprite.height:0); }
    if(!strcmp(nm,"sprite_get_number")){ GmlRenderSpriteMetrics sprite; return vreal(gml_render_sprite_metrics(R,(int)N(a,n,0),&sprite)?sprite.frame_count:0); }
    if(!strcmp(nm,"sprite_get_speed")){ int spr=(int)N(a,n,0);
      GmlRenderSpriteMetrics sprite; return vreal(gml_render_sprite_metrics(R,spr,&sprite)&&sprite.playback_speed_valid?sprite.playback_speed:1); }
    if(!strcmp(nm,"sprite_get_speed_type")){ int spr=(int)N(a,n,0);
      GmlRenderSpriteMetrics sprite; return vreal(gml_render_sprite_metrics(R,spr,&sprite)?sprite.playback_speed_type:1); }
    if(!strcmp(nm,"sprite_set_speed")){ int spr=(int)N(a,n,0);
      (void)gml_render_sprite_set_playback(R,spr,N(a,n,1),(int)N(a,n,2));
      return vreal(0); }
    if(!strcmp(nm,"sprite_get_xoffset")){ GmlRenderSpriteMetrics sprite; return vreal(gml_render_sprite_metrics(R,(int)N(a,n,0),&sprite)?sprite.origin_x:0); }
    if(!strcmp(nm,"sprite_get_yoffset")){ GmlRenderSpriteMetrics sprite; return vreal(gml_render_sprite_metrics(R,(int)N(a,n,0),&sprite)?sprite.origin_y:0); }
    if(!strcmp(nm,"sprite_get_bbox_left")){ GmlRenderSpriteMetrics sprite; return vreal(gml_render_sprite_metrics(R,(int)N(a,n,0),&sprite)?sprite.collision_left:0); }
    if(!strcmp(nm,"sprite_get_bbox_top")){ GmlRenderSpriteMetrics sprite; return vreal(gml_render_sprite_metrics(R,(int)N(a,n,0),&sprite)?sprite.collision_top:0); }
    if(!strcmp(nm,"sprite_get_bbox_right")){ GmlRenderSpriteMetrics sprite; return vreal(gml_render_sprite_metrics(R,(int)N(a,n,0),&sprite)?sprite.collision_right:0); }
    if(!strcmp(nm,"sprite_get_bbox_bottom")){ GmlRenderSpriteMetrics sprite; return vreal(gml_render_sprite_metrics(R,(int)N(a,n,0),&sprite)?sprite.collision_bottom:0); }
    if(!strcmp(nm,"sprite_get_uvs")) return sprite_uvs(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)));
    if(!strcmp(nm,"texture_get_width")){ double uw; return vreal(texture_info(R,(int)N(a,n,0),&uw,NULL,NULL,NULL)?uw:0); }
    if(!strcmp(nm,"texture_get_height")){ double uh; return vreal(texture_info(R,(int)N(a,n,0),NULL,&uh,NULL,NULL)?uh:0); }
    if(!strcmp(nm,"texture_get_texel_width")){ double tw; return vreal(texture_info(R,(int)N(a,n,0),NULL,NULL,&tw,NULL)?tw:0); }
    if(!strcmp(nm,"texture_get_texel_height")){ double th; return vreal(texture_info(R,(int)N(a,n,0),NULL,NULL,NULL,&th)?th:0); }
    if(!strcmp(nm,"texture_get_uvs")) return texture_uvs(R,(int)N(a,n,0));
    if(!strcmp(nm,"sprite_exists")){ int spr=(int)N(a,n,0); return vreal(R&&gml_sprite_exists(R,spr)); }
    /* Tilesets use the BGND resource list in this format. */
    if(!strcmp(nm,"tileset_get_name")){
      const char *tn=chunk_asset_name_by_index(vm?vm->win:NULL,"BGND",0,(int)N(a,n,0));
      return vstr(tn?tn:"");
    }
    if(!strcmp(nm,"sprite_get_name")){ GmlRenderSpriteMetrics sprite; return vstr(gml_render_sprite_metrics(R,(int)N(a,n,0),&sprite)&&sprite.name?sprite.name:""); }
    if(!strcmp(nm,"sprite_add")){
      /* sprite_add(fname, imgnum, removeback, smooth, xorig, yorig): load a loose image
       * (localized charsets/signs in ports) as a runtime sprite; -1 when missing, like GM. */
      char *p=resolve_read_path(vm,S(vm,a,n,0));
      int id = p? gml_sprite_add_file(R,p,(int)N(a,n,1),(int)N(a,n,2)>=1,(int)N(a,n,4),(int)N(a,n,5)) : -1;
      if(builtin_setting(vm,"GML_LOG_SPRADD")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[sprite_add] '%s' -> %d\n",p?p:"?",id);
      free(p);
      return vreal(id);
    }
    if(!strcmp(nm,"font_add")){
      /* font_add(file, size, bold, italic, first, last): rasterize a loose TTF from the game
       * dir (localized fonts in ports). -1 when missing/unreadable, like GM. */
      char *p=resolve_read_path(vm,S(vm,a,n,0));
      if(builtin_setting(vm,"GML_LOG_FONT")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
        "[font_add] path=%s size=%.1f bold=%d italic=%d first=%d last=%d argc=%d\n",
        p?p:"?",N(a,n,1),(int)N(a,n,2),(int)N(a,n,3),
        n>4?(int)N(a,n,4):32,n>5?(int)N(a,n,5):255,n);
      int first=n>4?(int)N(a,n,4):32, last=n>5?(int)N(a,n,5):255;
      int id = p? gml_font_add_file(R,p,N(a,n,1),first,last) : -1;
      free(p);
      return vreal(id);
    }
    /* object_get_parent returns the OBJT parent index, or -100 for no parent. */
  if(!strcmp(nm,"object_get_parent")){ int ob=(int)N(a,n,0);
    if(ob<0||ob>=vm->n_objects) return vreal(-100);
    int par=vm->objects[ob].parent;
    return vreal((par>=0&&par<vm->n_objects)?par:-100); }
  if(!strcmp(nm,"object_set_parent")){
    gml_object_set_parent(vm,(int)N(a,n,0),(int)N(a,n,1));
    return vreal(0); }
  if(!strcmp(nm,"object_get_name")){ int ob=(int)N(a,n,0);
    return vstr((ob>=0&&ob<vm->n_objects&&vm->objects[ob].name)?vm->objects[ob].name:""); }
  if(!strcmp(nm,"object_get_sprite")){ int ob=(int)N(a,n,0);
    return vreal((ob>=0&&ob<vm->n_objects)?vm->objects[ob].sprite_index:-1); }
  if(!strcmp(nm,"object_get_mask")){ int ob=(int)N(a,n,0);
    return vreal((ob>=0&&ob<vm->n_objects)?vm->objects[ob].mask_index:-1); }
  if(!strcmp(nm,"object_get_visible")){ int ob=(int)N(a,n,0);
    return vreal((ob>=0&&ob<vm->n_objects)?vm->objects[ob].visible:0); }
  if(!strcmp(nm,"object_get_solid") || !strcmp(nm,"object_get_persistent")){
    double index=N(a,n,0);
    if(n<1 || !isfinite(index) || index<0 || index>=vm->n_objects) return vreal(0);
    const GmlObject *object=&vm->objects[(int)index];
    return vreal(!strcmp(nm,"object_get_solid")?object->solid!=0:object->persistent!=0);
  }
  if(!strcmp(nm,"object_set_visible")){ int ob=(int)N(a,n,0);
    if(ob>=0&&ob<vm->n_objects) vm->objects[ob].visible=N(a,n,1)!=0.0;
    return vreal(0); }
  if(!strcmp(nm,"object_is_ancestor")){ int obj=(int)N(a,n,0), anc=(int)N(a,n,1);
    for(int p=obj; p>=0 && p<vm->n_objects; p=vm->objects[p].parent) if(p==anc) return vreal(1);
    return vreal(0); }
  if(!strcmp(nm,"object_exists")){ int ob=(int)N(a,n,0); return vreal(ob>=0&&ob<vm->n_objects); }
  if(!strcmp(nm,"object_get_physics")) return vreal(0);
  if(!strcmp(nm,"asset_get_index")||!strcmp(nm,"sprite_get_index")||!strcmp(nm,"object_get_index")){
      /* Resolve an asset name to its resource index. GM returns -1 when absent; returning zero
       * aliases asset zero and can send the name through a large linear script scan. Scripts matter
       * here too: data-driven systems can store a
       * script name in external room data, then call script_execute(asset_get_index(name)). */
      const char *an=(n>0 && a[0].t==V_STR && a[0].s)? a[0].s : "";
      if(!strcmp(nm,"object_get_index")){ int oi=gml_object_index_by_name(vm,an); return vreal(oi); }
      int sprite=gml_render_named_sprite(R,an); if(sprite>=0) return vreal(sprite);
      if(!strcmp(nm,"sprite_get_index")) return vreal(-1);
      return vreal(asset_index_and_type_by_name(vm,an,NULL));
    }
    /* TAGS data is not parsed here yet. Return the array-shaped result that
     * callers expect rather than the numeric fallback for an unknown builtin. */
    if(!strcmp(nm,"asset_get_tags")||!strcmp(nm,"tag_get_assets")||!strcmp(nm,"tag_get_asset_ids")){
      (void)0;
      return arr_newv(0);
    }
    if(!strcmp(nm,"asset_get_type")){
      const char *an=(n>0 && a[0].t==V_STR && a[0].s)?a[0].s:"";
      int type=GML_ASSET_UNKNOWN;
      (void)asset_index_and_type_by_name(vm,an,&type);
      return vreal(type);
    }
    if(!strcmp(nm,"sprite_create_from_surface")){
      return vreal(R?gml_sprite_create_from_surface(R,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),
        (int)N(a,n,3),(int)N(a,n,4),(int)N(a,n,5),(int)N(a,n,6),(int)N(a,n,7),(int)N(a,n,8)):-1);
    }
    if(!strcmp(nm,"sprite_replace")){
      const char *requested=S(vm,a,n,1);
      char *path=resolve_read_path(vm,requested);
      int classic=vm && vm->win && anygm_policy_uses_classic_runtime(vm->win);
      /* Classic content can use either the nine-argument form or the retained seven-argument
       * form. Select the ABI from the supplied arity without shifting the origin fields. */
      int legacy_signature=classic && n>=9;
      int precise=legacy_signature && N(a,n,3)!=0.0;
      int removeback=(int)N(a,n,legacy_signature?4:3);
      int smooth=(int)N(a,n,legacy_signature?5:4);
      int xorigin=(int)N(a,n,legacy_signature?7:5);
      int yorigin=(int)N(a,n,legacy_signature?8:6);
      int sprite=(int)N(a,n,0);
      int ok=R?gml_sprite_replace_from_file(
          R,sprite,path,(int)N(a,n,2),removeback,smooth,xorigin,yorigin):0;
      if(ok && legacy_signature)
        ok=gml_sprite_collision_mask(
            R,sprite,0,0,0,0,0,0,precise?0:1,0);
      if(builtin_setting(vm,"GML_LOG_SPRREP"))
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
          "[sprite-replace] sprite=%d argc=%d frames=%d precise=%d removeback=%d "
          "smooth=%d origin=%d,%d ok=%d asked='%s' path='%s'\n",
          sprite,n,(int)N(a,n,2),precise,removeback,smooth,xorigin,yorigin,ok,
          requested?requested:"",path?path:"");
      free(path);
      return vreal(ok);
    }
    if(!strcmp(nm,"sprite_delete")){ if(R) gml_sprite_delete(R,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"sprite_duplicate")) return vreal(R?gml_sprite_duplicate(R,(int)N(a,n,0)):-1);
    if(!strcmp(nm,"sprite_assign"))
      return vreal(R?gml_sprite_assign(R,(int)N(a,n,0),(int)N(a,n,1)):0);
    /* A packaged sprite remains immutable when no runtime replacement is active. */
    if(!strcmp(nm,"sprite_restore")) return vreal(R&&gml_sprite_exists(R,(int)N(a,n,0)));
    if(!strcmp(nm,"sprite_set_alpha_from_sprite")) return vreal(R?gml_sprite_set_alpha_from_sprite(R,(int)N(a,n,0),(int)N(a,n,1)):0);
    if(!strcmp(nm,"sprite_set_offset")){ if(R) gml_sprite_set_offset(R,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2)); return vreal(0); }
    if(!strcmp(nm,"sprite_save")) return vreal(0);
    if(!strcmp(nm,"sprite_save_strip")) return vreal(0);
    if(!strcmp(nm,"sprite_collision_mask")){
      if(R) gml_sprite_collision_mask(R,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),
        (int)N(a,n,3),(int)N(a,n,4),(int)N(a,n,5),(int)N(a,n,6),(int)N(a,n,7),(int)N(a,n,8));
      return vreal(0);
    }
    if(!strcmp(nm,"sprite_prefetch")) return vreal(0);
    if(!strcmp(nm,"background_add")||!strcmp(nm,"background_create_color")) return vreal(-1);
    if(!strcmp(nm,"background_replace")){
      char *path=resolve_read_path(vm,S(vm,a,n,1));
      int ok=R?gml_background_replace_from_file(R,(int)N(a,n,0),path,
                                                 (int)N(a,n,2),(int)N(a,n,3)):0;
      free(path);
      return vreal(ok);
    }
    if(!strcmp(nm,"background_delete")||!strcmp(nm,"background_save")) return vreal(0);
    if(!strcmp(nm,"background_restore")){
      GmlRenderBackgroundMetrics background;
      return vreal(gml_render_background_metrics(R,(int)N(a,n,0),&background));
    }
    if(!strcmp(nm,"background_exists"))
      return vreal(gml_render_background_metrics(R,(int)N(a,n,0),NULL));
    if(!strcmp(nm,"background_get_width")){ GmlRenderBackgroundMetrics background; return vreal(gml_render_background_metrics(R,(int)N(a,n,0),&background)?background.logical_width:0); }
    if(!strcmp(nm,"background_get_height")){ GmlRenderBackgroundMetrics background; return vreal(gml_render_background_metrics(R,(int)N(a,n,0),&background)?background.logical_height:0); }
    if(!strcmp(nm,"background_get_name")){ GmlRenderBackgroundMetrics background;
      return vstr(gml_render_background_metrics(R,(int)N(a,n,0),&background)&&background.name?background.name:""); }
    /* Return the authored background flags through the corresponding queries. */
    if(!strcmp(nm,"background_get_transparent")){ GmlRenderBackgroundMetrics background;
      return vreal(gml_render_background_metrics(R,(int)N(a,n,0),&background)?background.transparent:0); }
    if(!strcmp(nm,"background_get_smooth")){ GmlRenderBackgroundMetrics background;
      return vreal(gml_render_background_metrics(R,(int)N(a,n,0),&background)?background.smooth:0); }
    if(!strcmp(nm,"background_get_preload")){ GmlRenderBackgroundMetrics background;
      return vreal(gml_render_background_metrics(R,(int)N(a,n,0),&background)?background.preload:0); }
    if(!strcmp(nm,"background_get_texture")) return vreal(gml_render_background_texture_handle(R,(int)N(a,n,0)));
    if(!strcmp(nm,"draw_text")){ if(R) gml_draw_text(R,N(a,n,0),N(a,n,1),S(vm,a,n,2));
      if(builtin_setting(vm,"GML_DBG_TEXT")){ 
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[text] f%ld font=%d (%.0f,%.0f) \"%s\"\n",vm->frame,builtin_draw_state(R).font,N(a,n,0),N(a,n,1),S(vm,a,n,2)); }
      return vreal(0); }
    /* draw_text_color takes four corner colors plus alpha. The software approximation paints the
     * flat top-left color. */
    if(!strcmp(nm,"draw_text_color")||!strcmp(nm,"draw_text_colour")){
      if(R){ GmlRenderDrawState saved=builtin_draw_state(R),temporary=saved;
        temporary.color=NU32(a,n,3); temporary.alpha=N(a,n,7);
        gml_render_draw_state_update(R,&temporary,GML_RENDER_DRAW_STATE_COLOR|GML_RENDER_DRAW_STATE_ALPHA);
        gml_draw_text(R,N(a,n,0),N(a,n,1),S(vm,a,n,2));
        gml_render_draw_state_update(R,&saved,GML_RENDER_DRAW_STATE_COLOR|GML_RENDER_DRAW_STATE_ALPHA); }
      return vreal(0); }
    if(!strcmp(nm,"draw_text_ext")){ if(R) gml_draw_text_ext(R,N(a,n,0),N(a,n,1),S(vm,a,n,2),N(a,n,3),N(a,n,4)); return vreal(0); }
    if(!strcmp(nm,"draw_text_sprite")){
      if(R) gml_draw_text_sprite(R,N(a,n,0),N(a,n,1),S(vm,a,n,2),N(a,n,3),N(a,n,4),
                                 (int)N(a,n,5),(int)N(a,n,6),N(a,n,7));
      return vreal(0);
    }
    if(!strcmp(nm,"draw_text_ext_colour")||!strcmp(nm,"draw_text_ext_color")){
      if(R){ GmlRenderDrawState saved=builtin_draw_state(R),temporary=saved;
        temporary.color=NU32(a,n,5); temporary.alpha=N(a,n,9);
        gml_render_draw_state_update(R,&temporary,GML_RENDER_DRAW_STATE_COLOR|GML_RENDER_DRAW_STATE_ALPHA);
        gml_draw_text_ext(R,N(a,n,0),N(a,n,1),S(vm,a,n,2),N(a,n,3),N(a,n,4));
        gml_render_draw_state_update(R,&saved,GML_RENDER_DRAW_STATE_COLOR|GML_RENDER_DRAW_STATE_ALPHA); }
      return vreal(0); }
    if(!strcmp(nm,"draw_text_transformed")){
      if(R){ GmlRenderDrawState draw=builtin_draw_state(R); gml_draw_text_transformed(R,N(a,n,0),N(a,n,1),S(vm,a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),draw.color,draw.alpha); }
      return vreal(0);
    }
    if(!strcmp(nm,"draw_text_transformed_color")||!strcmp(nm,"draw_text_transformed_colour")){
      if(R) gml_draw_text_transformed(R,N(a,n,0),N(a,n,1),S(vm,a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),NU32(a,n,6),N(a,n,10));
      return vreal(0);
    }
    if(!strcmp(nm,"draw_text_ext_transformed")){
      if(R){ GmlRenderDrawState draw=builtin_draw_state(R); gml_draw_text_ext_transformed(R,N(a,n,0),N(a,n,1),S(vm,a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),N(a,n,7),draw.color,draw.alpha); }
      return vreal(0);
    }
    if(!strcmp(nm,"draw_text_ext_transformed_color")||!strcmp(nm,"draw_text_ext_transformed_colour")){
      if(R) gml_draw_text_ext_transformed(R,N(a,n,0),N(a,n,1),S(vm,a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),N(a,n,7),NU32(a,n,8),N(a,n,12));
      return vreal(0);
    }
    if(!strcmp(nm,"string_width")) return vreal(R?gml_text_width(R,S(vm,a,n,0)):(int)strlen(S(vm,a,n,0))*8);
    if(!strcmp(nm,"string_height")) return vreal(R?gml_text_height(R,S(vm,a,n,0)):8);
    if(!strcmp(nm,"string_width_ext")) return vreal(R?gml_text_width_ext(R,S(vm,a,n,0),N(a,n,1),N(a,n,2)):(int)strlen(S(vm,a,n,0))*8);
    if(!strcmp(nm,"string_height_ext")) return vreal(R?gml_text_height_ext(R,S(vm,a,n,0),N(a,n,1),N(a,n,2)):8);
    if(!strcmp(nm,"font_add_sprite")) return vreal(R? gml_font_add_sprite(R,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),(int)N(a,n,3)) : 0);
    if(!strcmp(nm,"font_add_sprite_ext"))
      return vreal(R? gml_font_add_sprite_ext(R,(int)N(a,n,0),S(vm,a,n,1),(int)N(a,n,2),(int)N(a,n,3)) : 0);
    if(!strcmp(nm,"font_add_enable_aa")) return vreal(0);
    if(!strcmp(nm,"font_exists"))
      return vreal(gml_render_font_exists(R,(int)N(a,n,0)));
    if(!strcmp(nm,"font_get_name")){
      const char *name=chunk_asset_name_by_index(vm?vm->win:NULL,"FONT",0,(int)N(a,n,0));
      return vstr(name?name:"");
    }
    if(!strcmp(nm,"font_get_texture"))
      return vreal(gml_render_font_texture_handle(R,(int)N(a,n,0)));
    if(!strcmp(nm,"font_get_uvs"))
      return texture_uvs(R,gml_render_font_texture_handle(R,(int)N(a,n,0)));
    if(!strcmp(nm,"font_get_size")){
      GmlRenderFontMetrics font;
      return vreal(gml_render_font_metrics(R,(int)N(a,n,0),&font)&&font.line_height>0?font.line_height:0);
    }
    if(!strcmp(nm,"font_delete")){
      int fid=(int)N(a,n,0);
      if(R) gml_font_delete(R,fid);
      return vreal(0);
    }
    if(!strcmp(nm,"font_get_info")){
      GmlInstance *st=gml_struct_new(vm);
      if(!st) return vreal(0);
      int fid=(int)N(a,n,0), spr=-1, first=0, prop=0, sep=0, size=0;
      int ascender=0, ascender_offset=0, sdf_spread=0;
      GmlRenderFontMetrics font;
      GmlInstance *glyphs=gml_struct_new(vm);
      if(fid>=0 && gml_render_font_metrics(R,fid,&font)){
        size=font.line_height;
        ascender=font.ascender;
        ascender_offset=font.ascender_offset;
        sdf_spread=font.sdf_spread;
        if(font.sprite_backed){
          spr=font.sprite; first=font.first;
          prop=font.proportional; sep=font.separation;
        }
        if(glyphs){
          for(int index=0;index<font.glyph_count;index++){
            GmlRenderFontGlyphMetrics glyph;
            if(!gml_render_font_glyph_metrics(R,fid,index,&glyph)) continue;
            GmlInstance *record=gml_struct_new(vm);
            if(!record) break;
            *gml_varmap_put(&record->vars,"char")=vreal(glyph.character);
            *gml_varmap_put(&record->vars,"x")=vreal(glyph.x);
            *gml_varmap_put(&record->vars,"y")=vreal(glyph.y);
            *gml_varmap_put(&record->vars,"w")=vreal(glyph.width);
            *gml_varmap_put(&record->vars,"h")=vreal(glyph.height);
            *gml_varmap_put(&record->vars,"shift")=vreal(glyph.shift);
            *gml_varmap_put(&record->vars,"offset")=vreal(glyph.offset);
            char key[5]; font_glyph_key(glyph.character,key);
            char *owned_key=strdup(key);
            if(!owned_key) break;
            *gml_varmap_put_owned(&glyphs->vars,owned_key)=vreal((double)record->id);
          }
        }
      }
      const char *font_name=chunk_asset_name_by_index(vm?vm->win:NULL,"FONT",0,fid);
      *gml_varmap_put(&st->vars,"name")=vstr(font_name?font_name:"");
      *gml_varmap_put(&st->vars,"size")=vreal(size);
      *gml_varmap_put(&st->vars,"ascender")=vreal(ascender);
      *gml_varmap_put(&st->vars,"ascenderOffset")=vreal(ascender_offset);
      *gml_varmap_put(&st->vars,"glyphs")=glyphs?vreal((double)glyphs->id):vundef();
      *gml_varmap_put(&st->vars,"spriteIndex")=vreal(spr);
      *gml_varmap_put(&st->vars,"sdfEnabled")=vreal(sdf_spread>0);
      *gml_varmap_put(&st->vars,"sdfSpread")=vreal(sdf_spread);
      *gml_varmap_put(&st->vars,"first")=vreal(first);
      *gml_varmap_put(&st->vars,"prop")=vreal(prop);
      *gml_varmap_put(&st->vars,"sep")=vreal(sep);
      return vreal((double)st->id);
    }
  }

  return gml_builtin_try_input(vm,nm,a,n);
}

GmlVal gml_builtin_try_draw_3d(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  GmlSoftware3D *graphics=graphics_state_for_render(R);
  GmlSoftware3DStatus status={0};
  GmlSoftware3DControl control={0};
  (void)gml_software3d_status_get(graphics,&status);
  if(!strncmp(nm,"d3d_",4))
    gml_software3d_set_classic(
      graphics,vm->win&&anygm_policy_uses_classic_runtime(vm->win));
  if(!strcmp(nm,"d3d_start")){
    GmlRenderTargetMetrics target=builtin_target_metrics(R);
    double width=target.width>0?target.width:640,height=target.height>0?target.height:480;
    gml_software3d_begin(
      R,target.camera_x,target.camera_y,width,height,0);
    return vreal(1);
  }
  if(!strcmp(nm,"d3d_end")){
    gml_software3d_end(graphics);
    return vreal(1);
  }
  /* The GPU and D3D names share one depth-comparison control; the rasterizer's
   * hidden flag decides whether a fragment behind the depth buffer is dropped. */
  if(!strcmp(nm,"d3d_set_hidden")||!strcmp(nm,"gpu_set_ztestenable")){
    control.hidden=N(a,n,0)!=0.0 &&
      !builtin_setting(vm,"GML_D3D_NO_DEPTH");
    gml_software3d_control_update(
      graphics,&control,GML_SOFTWARE3D_CONTROL_HIDDEN);
    return vreal(0);
  }
  /* Both names set the flag checked before storing a depth value. */
  if(!strcmp(nm,"d3d_set_zwriteenable")||!strcmp(nm,"gpu_set_zwriteenable")){
    control.zwrite=N(a,n,0)!=0;
    gml_software3d_control_update(
      graphics,&control,GML_SOFTWARE3D_CONTROL_ZWRITE);
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_shading")){
    control.smooth=N(a,n,0)!=0;
    gml_software3d_control_update(
      graphics,&control,GML_SOFTWARE3D_CONTROL_SMOOTH);
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_fog")||!strcmp(nm,"gpu_set_fog")){
    control.fog=N(a,n,0)!=0;
    control.fog_color=NU32(a,n,1)&0xFFFFFFu;
    control.fog_start=N(a,n,2);
    control.fog_end=N(a,n,3);
    gml_software3d_control_update(
      graphics,&control,GML_SOFTWARE3D_CONTROL_FOG);
    /* A degenerate fog range fully fogs ordinary sprite fragments as well as
     * three-dimensional geometry, regardless of depth. */
    gml_render_set_flat_fog(R,control.fog && control.fog_end<=control.fog_start,control.fog_color);
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_culling")){
    control.culling=N(a,n,0)!=0;
    gml_software3d_control_update(
      graphics,&control,GML_SOFTWARE3D_CONTROL_CULLING);
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_lighting")){
    control.lighting=N(a,n,0)!=0;
    gml_software3d_control_update(
      graphics,&control,GML_SOFTWARE3D_CONTROL_LIGHTING);
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_light_define_point")){
    return vreal(gml_software3d_light_define_point(
      graphics,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),
      N(a,n,4),NU32(a,n,5)));
  }
  if(!strcmp(nm,"d3d_light_define_direction")){
    return vreal(gml_software3d_light_define_direction(
      graphics,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),
      NU32(a,n,4)));
  }
  if(!strcmp(nm,"d3d_light_define_ambient")){
    control.ambient_color=NU32(a,n,0)&0xFFFFFFu;
    gml_software3d_control_update(
      graphics,&control,GML_SOFTWARE3D_CONTROL_AMBIENT);
    return vreal(1);
  }
  if(!strcmp(nm,"d3d_light_enable")){
    return vreal(gml_software3d_light_enable(
      graphics,(int)N(a,n,0),N(a,n,1)!=0));
  }
  if(!strcmp(nm,"d3d_set_projection")){
    gml_software3d_set_camera(
      R,N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),
      N(a,n,6),N(a,n,7),N(a,n,8));
    /* The legacy overload uses the classic fixed projection aperture. Its single-precision
     * matrix lands slightly below the ideal 640-pixel focal length at a 4:3 framebuffer. */
    uint32_t mask=GML_SOFTWARE3D_CONTROL_CLIP;
    if(vm->win&&anygm_policy_uses_classic_runtime(vm->win)){
      control.fov=41.1125;
      mask|=GML_SOFTWARE3D_CONTROL_FOV;
    }
    control.aspect=0;
    control.near_clip=.05;
    control.far_clip=32000;
    gml_software3d_control_update(graphics,&control,mask);
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_projection_ext")){
    gml_software3d_set_camera(
      R,N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),
      N(a,n,6),N(a,n,7),N(a,n,8));
    control.fov=N(a,n,9);
    control.aspect=N(a,n,10);
    control.near_clip=N(a,n,11);
    control.far_clip=N(a,n,12);
    gml_software3d_control_update(
      graphics,&control,
      GML_SOFTWARE3D_CONTROL_FOV|GML_SOFTWARE3D_CONTROL_CLIP);
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_projection_ortho")){
    control.ortho_x=N(a,n,0);
    control.ortho_y=N(a,n,1);
    control.ortho_w=N(a,n,2);
    control.ortho_h=N(a,n,3);
    control.ortho_angle=N(a,n,4);
    gml_software3d_control_update(
      graphics,&control,GML_SOFTWARE3D_CONTROL_ORTHOGRAPHIC);
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_projection_perspective")){
    double x=N(a,n,0),y=N(a,n,1),w=N(a,n,2),h=N(a,n,3),angle=N(a,n,4);
    gml_software3d_set_default_projection(R,x,y,w,h,angle);
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_perspective")){
    GmlRenderTargetMetrics target=builtin_target_metrics(R);
    if(N(a,n,0)!=0){
      double w=target.width>0?target.width:640,h=target.height>0?target.height:480;
      gml_software3d_set_default_projection(
        R,target.camera_x,target.camera_y,w,h,0);
      return vreal(0);
    }
    control.ortho_x=target.camera_x;
    control.ortho_y=target.camera_y;
    control.ortho_w=target.width>0?target.width:640;
    control.ortho_h=target.height>0?target.height:480;
    control.ortho_angle=0;
    gml_software3d_control_update(
      graphics,&control,GML_SOFTWARE3D_CONTROL_ORTHOGRAPHIC);
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_set_depth")){
    control.draw_depth=N(a,n,0);
    gml_software3d_control_update(
      graphics,&control,GML_SOFTWARE3D_CONTROL_DEPTH);
    return vreal(0);
  }
  if(!strncmp(nm,"d3d_transform_set_",18)||!strncmp(nm,"d3d_transform_add_",18)){
    GmlRender *R=(GmlRender*)vm->render;
    int add=!strncmp(nm,"d3d_transform_add_",18);
    const char *op=nm+18;
    double matrix[16]; gml_software3d_matrix_identity(matrix);
    if(!strcmp(op,"identity")){
      gml_software3d_matrix_identity(matrix);
      (void)gml_software3d_matrix_set(
        R,GML_SOFTWARE3D_MATRIX_TRANSFORM,matrix);
      return vreal(0);
    }
    if(!strcmp(op,"translation")) gml_software3d_matrix_translation(matrix,N(a,n,0),N(a,n,1),N(a,n,2));
    else if(!strcmp(op,"scaling")) gml_software3d_matrix_scaling(matrix,N(a,n,0),N(a,n,1),N(a,n,2));
    else if(!strcmp(op,"rotation_x")) gml_software3d_matrix_rotation_axis(matrix,1,0,0,N(a,n,0));
    else if(!strcmp(op,"rotation_y")) gml_software3d_matrix_rotation_axis(matrix,0,1,0,N(a,n,0));
    else if(!strcmp(op,"rotation_z")) gml_software3d_matrix_rotation_axis(matrix,0,0,1,N(a,n,0));
    else if(!strcmp(op,"rotation_axis")) gml_software3d_matrix_rotation_axis(matrix,N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3));
    else return vreal(0);
    if(add) (void)gml_software3d_matrix_prepend(R,matrix);
    else (void)gml_software3d_matrix_set(
      R,GML_SOFTWARE3D_MATRIX_TRANSFORM,matrix);
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_transform_stack_clear")){
    (void)gml_software3d_transform_stack(
      R,GML_SOFTWARE3D_STACK_CLEAR);
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_transform_stack_empty"))
    return vreal(gml_software3d_transform_stack(
      R,GML_SOFTWARE3D_STACK_EMPTY));
  if(!strcmp(nm,"d3d_transform_stack_push"))
    return vreal(gml_software3d_transform_stack(
      R,GML_SOFTWARE3D_STACK_PUSH));
  if(!strcmp(nm,"d3d_transform_stack_pop"))
    return vreal(gml_software3d_transform_stack(
      R,GML_SOFTWARE3D_STACK_POP));
  if(!strcmp(nm,"d3d_transform_stack_top"))
    return vreal(gml_software3d_transform_stack(
      R,GML_SOFTWARE3D_STACK_TOP));
  if(!strcmp(nm,"d3d_transform_stack_discard"))
    return vreal(gml_software3d_transform_stack(
      R,GML_SOFTWARE3D_STACK_DISCARD));
  if(!strcmp(nm,"d3d_model_create"))
    return vreal(gml_software3d_model_create(graphics));
  if(!strcmp(nm,"d3d_model_destroy"))
    return vreal(gml_software3d_model_destroy(
      graphics,(int)N(a,n,0)));
  if(!strcmp(nm,"d3d_model_clear"))
    return vreal(gml_software3d_model_clear(
      graphics,(int)N(a,n,0)));
  if(!strcmp(nm,"d3d_model_primitive_begin")){
    int id=(int)N(a,n,0),kind=(int)N(a,n,1);
    return vreal(gml_software3d_model_begin(graphics,id,kind));
  }
  if(!strcmp(nm,"d3d_model_primitive_end"))
    return vreal(gml_software3d_model_end(
      graphics,(int)N(a,n,0)));
  if(!strncmp(nm,"d3d_model_vertex",16)){
    GmlRenderDrawState draw=builtin_draw_state(R);
    GmlSoftware3DVertex vertex; memset(&vertex,0,sizeof(vertex));
    vertex.x=N(a,n,1); vertex.y=N(a,n,2); vertex.z=N(a,n,3);
    uint32_t color=draw.color; vertex.alpha=draw.alpha;
    int normal=strstr(nm,"_normal")!=NULL;
    int texture=strstr(nm,"_texture")!=NULL;
    int colored=strstr(nm,"_color")!=NULL||strstr(nm,"_colour")!=NULL;
    int index=4;
    if(normal){ vertex.nx=N(a,n,index++); vertex.ny=N(a,n,index++); vertex.nz=N(a,n,index++); vertex.has_normal=1; }
    if(texture){ vertex.u=N(a,n,index++); vertex.v=N(a,n,index++); }
    if(colored){ color=NU32(a,n,index++)&0xFFFFFFu; vertex.alpha=N(a,n,index); }
    vertex.r=color&255; vertex.g=(color>>8)&255; vertex.b=(color>>16)&255;
    return vreal(gml_software3d_model_append_vertex(
      graphics,(int)N(a,n,0),vertex));
  }
  if(!strcmp(nm,"d3d_model_floor")||!strcmp(nm,"d3d_model_wall")||
     !strcmp(nm,"d3d_model_block")||!strcmp(nm,"d3d_model_cylinder")||
     !strcmp(nm,"d3d_model_cone")||!strcmp(nm,"d3d_model_ellipsoid")){
    int shape=!strcmp(nm,"d3d_model_block")?10:
      !strcmp(nm,"d3d_model_cylinder")?11:
      !strcmp(nm,"d3d_model_cone")?12:
      !strcmp(nm,"d3d_model_ellipsoid")?13:
      !strcmp(nm,"d3d_model_wall")?14:15;
    int closed=(shape==11||shape==12)?N(a,n,9)!=0:1;
    int steps=shape==13?(int)N(a,n,9):
      ((shape==11||shape==12)?(int)N(a,n,10):16);
    return vreal(gml_software3d_model_add_shape(
      graphics,(int)N(a,n,0),shape,
      N(a,n,1),N(a,n,2),N(a,n,3),
      N(a,n,4),N(a,n,5),N(a,n,6),
      N(a,n,7),N(a,n,8),closed,steps));
  }
  if(!strcmp(nm,"d3d_model_draw")){
    int id=(int)N(a,n,0);
    if(!gml_software3d_model_is_live(graphics,id)) return vreal(0);
    gml_software3d_model_draw(
      R,id,N(a,n,1),N(a,n,2),N(a,n,3),(int)N(a,n,4));
    return vreal(1);
  }
  if(!strcmp(nm,"d3d_model_save")){
    int id=(int)N(a,n,0);
    if(!gml_software3d_model_is_live(graphics,id)) return vreal(0);
    char *path=resolve_write_path(vm,S(vm,a,n,1));
    int ok=d3_model_file_save(vm,path,graphics,id);
    free(path);
    return vreal(ok);
  }
  if(!strcmp(nm,"d3d_model_load")){
    int id=(int)N(a,n,0);
    if(!gml_software3d_model_is_live(graphics,id)) return vreal(0);
    char *path=resolve_read_path(vm,S(vm,a,n,1));
    int ok=d3_model_file_load(vm,path,graphics,id);
    free(path);
    return vreal(ok);
  }
  if(!strcmp(nm,"d3d_primitive_begin")||!strcmp(nm,"d3d_primitive_begin_texture")){
    gml_software3d_primitive_3d_begin(
      graphics,(int)N(a,n,0),
      !strcmp(nm,"d3d_primitive_begin_texture")?(int)N(a,n,1):-1);
    return vreal(0);
  }
  if(!strncmp(nm,"d3d_vertex",10)){
    GmlRenderDrawState draw=builtin_draw_state(R);
    GmlSoftware3DVertex vertex; memset(&vertex,0,sizeof(vertex));
    vertex.x=N(a,n,0); vertex.y=N(a,n,1); vertex.z=N(a,n,2);
    uint32_t color=draw.color; vertex.alpha=draw.alpha;
    int normal=strstr(nm,"_normal")!=NULL;
    int texture=strstr(nm,"_texture")!=NULL;
    int colored=strstr(nm,"_color")!=NULL||strstr(nm,"_colour")!=NULL;
    int index=3;
    if(normal){ vertex.nx=N(a,n,index++); vertex.ny=N(a,n,index++); vertex.nz=N(a,n,index++); vertex.has_normal=1; }
    if(texture){ vertex.u=N(a,n,index++); vertex.v=N(a,n,index++); }
    if(colored){ color=NU32(a,n,index++)&0xFFFFFFu; vertex.alpha=N(a,n,index); }
    color=d3_vertex_color(vm,color,colored);
    vertex.r=color&255; vertex.g=(color>>8)&255; vertex.b=(color>>16)&255;
    (void)gml_software3d_primitive_3d_append(graphics,vertex);
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_primitive_end")){
    gml_software3d_primitive_3d_end(R);
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_draw_block")){
    if(R && status.active){
      double x1=N(a,n,0),y1=N(a,n,1),z1=N(a,n,2),x2=N(a,n,3),y2=N(a,n,4),z2=N(a,n,5);
      double face[6][4][3]={
        {{x1,y1,z1},{x1,y2,z1},{x2,y2,z1},{x2,y1,z1}},
        {{x1,y1,z2},{x2,y1,z2},{x2,y2,z2},{x1,y2,z2}},
        {{x1,y2,z1},{x1,y2,z2},{x2,y2,z2},{x2,y2,z1}},
        {{x2,y2,z1},{x2,y2,z2},{x2,y1,z2},{x2,y1,z1}},
        {{x2,y1,z1},{x2,y1,z2},{x1,y1,z2},{x1,y1,z1}},
        {{x1,y1,z1},{x1,y1,z2},{x1,y2,z2},{x1,y2,z1}}};
      uint32_t color=d3_vertex_color(vm,builtin_draw_state(R).color,0);
      gml_render_maybe_prepare_draw(R);
      for(int i=0;i<6;i++) gml_software3d_draw_quad(
        R,face[i],(int)N(a,n,6),N(a,n,7),N(a,n,8),color,0,4);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_draw_cylinder")){
    if(R && status.active){
      double x1=N(a,n,0),y1=N(a,n,1),z1=N(a,n,2),x2=N(a,n,3),y2=N(a,n,4),z2=N(a,n,5);
      double cx=(x1+x2)*.5,cy=(y1+y2)*.5,rx=fabs(x2-x1)*.5,ry=fabs(y2-y1)*.5;
      int closed=N(a,n,9)!=0, steps=(int)N(a,n,10); if(steps<3)steps=3; if(steps>128)steps=128;
      uint32_t color=d3_vertex_color(vm,builtin_draw_state(R).color,0);
      gml_render_maybe_prepare_draw(R);
      for(int i=0;i<steps;i++){
        double q0=2*M_PI*i/steps,q1=2*M_PI*(i+1)/steps;
        double side[4][3]={{cx+cos(q0)*rx,cy+sin(q0)*ry,z1},{cx+cos(q1)*rx,cy+sin(q1)*ry,z1},
                           {cx+cos(q1)*rx,cy+sin(q1)*ry,z2},{cx+cos(q0)*rx,cy+sin(q0)*ry,z2}};
        gml_software3d_draw_quad(
          R,side,(int)N(a,n,6),N(a,n,7)/steps,N(a,n,8),color,0,0);
        if(closed){
          double cap0[4][3]={{cx,cy,z1},{cx+cos(q1)*rx,cy+sin(q1)*ry,z1},{cx+cos(q0)*rx,cy+sin(q0)*ry,z1},{cx,cy,z1}};
          double cap1[4][3]={{cx,cy,z2},{cx+cos(q0)*rx,cy+sin(q0)*ry,z2},{cx+cos(q1)*rx,cy+sin(q1)*ry,z2},{cx,cy,z2}};
          gml_software3d_draw_quad(
            R,cap0,(int)N(a,n,6),N(a,n,7),N(a,n,8),color,0,0);
          gml_software3d_draw_quad(
            R,cap1,(int)N(a,n,6),N(a,n,7),N(a,n,8),color,0,0);
        }
      }
    }
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_draw_cone")){
    if(R && status.active){
      double x1=N(a,n,0),y1=N(a,n,1),z1=N(a,n,2),x2=N(a,n,3),y2=N(a,n,4),z2=N(a,n,5);
      double cx=(x1+x2)*.5,cy=(y1+y2)*.5,rx=fabs(x2-x1)*.5,ry=fabs(y2-y1)*.5;
      int closed=N(a,n,9)!=0,steps=(int)N(a,n,10); if(steps<3)steps=3; if(steps>128)steps=128;
      uint32_t color=d3_vertex_color(vm,builtin_draw_state(R).color,0);
      gml_render_maybe_prepare_draw(R);
      for(int i=0;i<steps;i++){
        double q0=2*M_PI*i/steps,q1=2*M_PI*(i+1)/steps;
        double side[4][3]={{cx+cos(q0)*rx,cy+sin(q0)*ry,z1},{cx+cos(q1)*rx,cy+sin(q1)*ry,z1},
                           {cx,cy,z2},{cx,cy,z2}};
        gml_software3d_draw_quad(
          R,side,(int)N(a,n,6),N(a,n,7)/steps,N(a,n,8),color,0,0);
        if(closed){
          double cap[4][3]={{cx,cy,z1},{cx+cos(q1)*rx,cy+sin(q1)*ry,z1},
                            {cx+cos(q0)*rx,cy+sin(q0)*ry,z1},{cx,cy,z1}};
          gml_software3d_draw_quad(
            R,cap,(int)N(a,n,6),N(a,n,7),N(a,n,8),color,0,0);
        }
      }
    }
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_draw_ellipsoid")){
    if(R && status.active){
      uint32_t color=d3_vertex_color(vm,builtin_draw_state(R).color,0);
      gml_render_maybe_prepare_draw(R);
      gml_software3d_draw_ellipsoid(
        R,N(a,n,0),N(a,n,1),N(a,n,2),
        N(a,n,3),N(a,n,4),N(a,n,5),
        (int)N(a,n,6),N(a,n,7),N(a,n,8),(int)N(a,n,9),color);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"d3d_draw_floor")||!strcmp(nm,"d3d_draw_wall")){
    if(!strcmp(nm,"d3d_draw_wall") && builtin_setting(vm,"GML_D3D_SKIP_WALLS")) return vreal(0);
    if(!strcmp(nm,"d3d_draw_wall")){
      const char *limit_text=builtin_setting(vm,"GML_D3D_WALL_LIMIT");
      if(limit_text){ GmlBuiltinState *state=builtin_state_ensure(vm);
        if(state && state->d3_wall_limit_frame!=vm->frame){ state->d3_wall_limit_frame=vm->frame; state->d3_wall_count=0; }
        if(state && state->d3_wall_count++>=atoi(limit_text)) return vreal(0);
      }
    }
    if(R && status.active){
      double x1=N(a,n,0),y1=N(a,n,1),z1=N(a,n,2),x2=N(a,n,3),y2=N(a,n,4),z2=N(a,n,5);
      double p[4][3];
      if(!strcmp(nm,"d3d_draw_floor")){
        double floor_points[4][3]={{x1,y1,z1},{x2,y1,z1},{x2,y2,z2},{x1,y2,z2}};
        memcpy(p,floor_points,sizeof(p));
      } else {
        double wall_points[4][3]={{x1,y1,z1},{x2,y2,z1},{x2,y2,z2},{x1,y1,z2}};
        memcpy(p,wall_points,sizeof(p));
      }
      uint32_t color=d3_vertex_color(vm,builtin_draw_state(R).color,0);
      gml_render_maybe_prepare_draw(R);
      GmlBuiltinState *state=builtin_state_ensure(vm);
      if(builtin_setting(vm,"GML_LOG_D3D") && (!state || state->d3_draw_log_count++<16))
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[d3d] %s tex=%d rep=(%.3f,%.3f) p1=(%.1f,%.1f,%.1f) p2=(%.1f,%.1f,%.1f)\n",
                nm,(int)N(a,n,6),N(a,n,7),N(a,n,8),x1,y1,z1,x2,y2,z2);
      gml_software3d_draw_quad(
        R,p,(int)N(a,n,6),N(a,n,7),N(a,n,8),color,0,0);
    }
    return vreal(0);
  }
  if(!strncmp(nm,"d3d_set_",8)) return vreal(0);
  if(!strcmp(nm,"shader_set")){ GmlRender *R=(GmlRender*)vm->render;
    gml_render_shader_set_current(R,(int)N(a,n,0));
    GmlBuiltinState *state=builtin_state_ensure(vm);
    if(builtin_setting(vm,"GML_LOG_SHADER") && (!state || state->shader_set_log_count++<8))
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[shader] f%ld shader_set(%d)\n",vm->frame,(int)N(a,n,0));
    return vreal(0); }
  if(!strcmp(nm,"shader_reset")){ GmlRender *R=(GmlRender*)vm->render;
    gml_render_shader_set_current(R,-1);
    return vreal(0); }
  if(!strcmp(nm,"shader_current")){ GmlRender *R=(GmlRender*)vm->render; return vreal(gml_render_shader_current(R)); }
  /* The portable renderer exposes a shader pipeline. Individual assets are still queried through
   * shader_is_compiled(), which rejects templates the software evaluator does not recognize. */
  if(!strcmp(nm,"shaders_are_supported")) return vreal(vm->render!=NULL);
  /* report palette shaders (template or LUT) as compiled so games keep using them (unknown -> 0) */
  if(!strcmp(nm,"shader_is_compiled")){ GmlRender *R=(GmlRender*)vm->render; int sid=(int)N(a,n,0);
    int ok=gml_render_shader_is_compiled(R,sid);
    GmlBuiltinState *state=builtin_state_ensure(vm);
    if(builtin_setting(vm,"GML_LOG_SHADER") && (!state || state->shader_compiled_log_count++<6))
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[shader] f%ld shader_is_compiled(%d)=%d\n",vm->frame,sid,ok);
    return vreal(ok); }
  /* Uniform and sampler handles are opaque to GML; the software renderer maps recognized shader
   * controls onto its private slots and accepts unknown controls without changing output. */
  if(!strcmp(nm,"shader_get_name")){
    double id=N(a,n,0);
    const char *name=n>0 && isfinite(id) && id>=0 && id<=INT_MAX
      ?chunk_asset_name_by_index(vm?vm->win:NULL,"SHDR",0,(int)id):NULL;
    return name?vstr_owned(strdup(name)):vstr("");
  }
  if(!strcmp(nm,"shader_get_uniform")){ GmlRender *R=(GmlRender*)vm->render;
    return vreal(gml_shader_get_uniform(R,(int)N(a,n,0),S(vm,a,n,1))); }
  if(!strcmp(nm,"shader_get_sampler_index")){
    return vreal(gml_shader_get_sampler((GmlRender*)vm->render,(int)N(a,n,0),S(vm,a,n,1))); }
  if(!strcmp(nm,"shader_set_uniform_f")||!strcmp(nm,"shader_set_uniform_f_array")||
     !strcmp(nm,"shader_set_uniform_i")||!strcmp(nm,"shader_set_uniform_i_array")||
     !strcmp(nm,"shader_set_uniform_matrix_array")){
    GmlRender *R=(GmlRender*)vm->render;
    gml_shader_set_uniform_values(R,(int)N(a,n,0),a,n,nm[18]=='i');
    return vreal(0); }
  if(!strcmp(nm,"texture_set_stage")){ GmlRender *R=(GmlRender*)vm->render;
    int stage=(int)N(a,n,0);
    int tex=(int)N(a,n,1);
    GmlRenderShaderTextureBinding binding;
    if(gml_render_shader_texture_stage_set(R,stage,tex,&binding)){
      if(binding.kind==GML_RENDER_SHADER_TEXTURE_PALETTE &&
                builtin_setting(vm,"GML_LOG_SHADER")){
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
          "[shader] palette texture: sprite %d frame %d\n",
          binding.sprite,binding.frame);
      }
    }
    return vreal(0); }
  if(!strcmp(nm,"sprite_get_texture")){ int spr=(int)N(a,n,0), img=(int)N(a,n,1);
    return vreal(gml_render_sprite_texture_handle(spr,img)); }
  if(!strcmp(nm,"texture_debug_messages")) return vreal(0);
  if(!strcmp(nm,"matrix_build_identity")||!strcmp(nm,"matrix_get")||!strcmp(nm,"matrix_set")||
     !strcmp(nm,"matrix_multiply")||!strcmp(nm,"matrix_build")||!strcmp(nm,"matrix_inverse")||
     !strcmp(nm,"matrix_transform_vertex")||!strcmp(nm,"matrix_build_lookat")||
     !strcmp(nm,"matrix_build_projection_ortho")||
     !strcmp(nm,"matrix_build_projection_perspective_fov"))
    return gm_matrix_builtin((GmlRender*)vm->render,nm,a,n);
  return gml_builtin_try_animation(vm,nm,a,n);
}
