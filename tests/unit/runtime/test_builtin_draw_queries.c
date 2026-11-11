/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "gml_vm_internal.h"
#include "gml_particle.h"
#include "gml_render_internal.h"
#include "gml_render_state.h"
#include "gml_software3d_internal.h"
#include "anygm_test_runner.h"
#include "memory_vfs.h"
#include "../media/font_test_fixture.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GmlVal query(GmlVM *vm,const char *name,GmlVal *args,int count,int cached,int *ok){
  if(!cached) return gml_builtin_call(vm,name,args,count);
  int id=gml_builtin_fast_id(vm,name);
  if(id<0){ *ok=0; return vundef(); }
  return gml_builtin_call_fast_id(vm,id,name,args,count);
}
static int real_is(GmlVal v,double number){ return v.t==V_REAL && v.d==number; }
static GmlInstance *query_struct(GmlVM *vm,GmlVal value){
  if(!gml_val_is_number_or_object(value) || !GML_IS_STRUCT_ID(value.d)) return NULL;
  return gml_struct_find(vm,(unsigned)value.d);
}
static int struct_number_is(GmlInstance *value,const char *name,double number){
  GmlVal *field=value?gml_varmap_get(&value->vars,name):NULL;
  return field && real_is(*field,number);
}
static int motion_grid_edits_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0};
    GmlVal create[]={vreal(10),vreal(20),vreal(4),vreal(3),vreal(3),vreal(2)};
    GmlVal grid=gml_builtin_call(&vm,"mp_grid_create",create,6);
    GmlVal cell[]={grid,vreal(2),vreal(1)};
    GmlVal empty=query(&vm,"mp_grid_get_cell",cell,3,cached,&ok);
    ok &= real_is(empty,0) && !signbit(empty.d);
    query(&vm,"mp_grid_add_cell",cell,3,cached,&ok);
    ok &= real_is(query(&vm,"mp_grid_get_cell",cell,3,cached,&ok),-1);
    query(&vm,"mp_grid_clear_cell",cell,3,cached,&ok);
    empty=query(&vm,"mp_grid_get_cell",cell,3,cached,&ok);
    ok &= real_is(empty,0) && !signbit(empty.d);
    GmlVal rectangle[]={grid,vreal(19),vreal(24),vreal(13),vreal(22)};
    query(&vm,"mp_grid_add_rectangle",rectangle,5,cached,&ok);
    for(int row=0;row<3;row++) for(int column=0;column<4;column++){
      cell[1]=vreal(column);cell[2]=vreal(row);
      ok &= real_is(query(&vm,"mp_grid_get_cell",cell,3,cached,&ok),
                    row>=1 && column>=1?-1:0);
    }
    rectangle[1]=rectangle[3]=vreal(16);rectangle[2]=rectangle[4]=vreal(22);
    query(&vm,"mp_grid_clear_rectangle",rectangle,5,cached,&ok);
    cell[1]=vreal(2);cell[2]=vreal(1);
    ok &= real_is(query(&vm,"mp_grid_get_cell",cell,3,cached,&ok),0);
    cell[1]=vreal(3);
    ok &= real_is(query(&vm,"mp_grid_get_cell",cell,3,cached,&ok),-1);
    query(&vm,"mp_grid_clear_all",&grid,1,cached,&ok);
    ok &= real_is(query(&vm,"mp_grid_get_cell",cell,3,cached,&ok),0);
    cell[1]=vreal(-1);
    ok &= real_is(query(&vm,"mp_grid_get_cell",cell,3,cached,&ok),-1);
    GmlVal huge[]={grid,vreal(-1e100),vreal(-1e100),vreal(1e100),vreal(1e100)};
    query(&vm,"mp_grid_add_rectangle",huge,5,cached,&ok);
    for(int row=0;row<3;row++) for(int column=0;column<4;column++){
      cell[1]=vreal(column);cell[2]=vreal(row);
      ok &= real_is(query(&vm,"mp_grid_get_cell",cell,3,cached,&ok),-1);
    }
    query(&vm,"mp_grid_clear_rectangle",huge,5,cached,&ok);
    for(int row=0;row<3;row++) for(int column=0;column<4;column++){
      cell[1]=vreal(column);cell[2]=vreal(row);
      ok &= real_is(query(&vm,"mp_grid_get_cell",cell,3,cached,&ok),0);
    }
    gml_vm_free(&vm);
  }
  return ok;
}
static int motion_grid_drawing_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++) for(int camera=0;camera<2;camera++){
    GmlRender *render=calloc(1,sizeof *render);
    if(!render || gml_render_init(render,NULL)){ free(render); return 0; }
    uint32_t screen[20*12];
    for(unsigned index=0;index<20u*12u;index++) screen[index]=0xFF26486Au;
    gml_render_begin(render,screen,20,12,camera,camera);
    render->color=0x123456u;render->alpha=1;
    GmlVM vm={.render=render};
    GmlVal create[]={vreal(3),vreal(2),vreal(4),vreal(3),vreal(3),vreal(2)};
    GmlVal grid=gml_builtin_call(&vm,"mp_grid_create",create,6);
    GmlVal cell[]={grid,vreal(1),vreal(1)};
    gml_builtin_call(&vm,"mp_grid_add_cell",cell,3);
    ok &= real_is(query(&vm,"mp_grid_draw",&grid,1,cached,&ok),1);
    gml_render_flush_pending_fill(render);
    for(int y=0;y<12;y++) for(int x=0;x<20;x++){
      int gx=x+camera-3,gy=y+camera-2;
      uint32_t expected=0xFF26486Au;
      if(gx>=0 && gx<12 && gy>=0 && gy<6)
        expected=gx/3==1 && gy/2==1?0xFFFF0000u:0xFF00FF00u;
      if(screen[y*20+x]!=expected) ok=0;
    }
    ok &= render->color==0x123456u && render->alpha==1;
    render->alpha=0;
    uint32_t before[20*12];memcpy(before,screen,sizeof before);
    ok &= real_is(query(&vm,"mp_grid_draw",&grid,1,cached,&ok),1);
    gml_render_flush_pending_fill(render);
    ok &= !memcmp(before,screen,sizeof before) && render->alpha==0;
    GmlVal invalid=vreal(-1);
    ok &= real_is(query(&vm,"mp_grid_draw",&invalid,1,cached,&ok),0);
    vm.render=NULL;gml_vm_free(&vm);gml_render_free(render);free(render);
  }
  return ok;
}
static int surface_pixel_alpha_case(void){
  int ok=1;
  const uint32_t pixels[]={0x00123456u,0x7FABCDEFu,0xFF102030u,0x800100FEu};
  const uint32_t packed[]={0x00563412u,0x7FEFCDABu,0xFF302010u,0x80FE0001u};
  for(int cached=0;cached<2;cached++){
    GmlRender *render=calloc(1,sizeof *render);
    if(!render || gml_render_init(render,NULL)){ free(render); return 0; }
    GmlVM vm={.render=render};
    int id=gml_surface_create(render,2,2);
    uint32_t *destination=surface_pixels(render,id,NULL,NULL);
    if(!destination){ gml_render_free(render); free(render); return 0; }
    memcpy(destination,pixels,sizeof pixels);
    for(int index=0;index<4;index++){
      GmlVal args[]={vreal(id),vreal(index%2),vreal(index/2)};
      ok &= real_is(query(&vm,"surface_getpixel_ext",args,3,cached,&ok),packed[index]);
      ok &= real_is(query(&vm,"surface_getpixel",args,3,cached,&ok),packed[index]&0xFFFFFFu);
    }
    GmlVal outside[]={vreal(id),vreal(2),vreal(0)};
    ok &= real_is(query(&vm,"surface_getpixel_ext",outside,3,cached,&ok),0);
    outside[0]=vreal(-1); outside[1]=vreal(0);
    ok &= real_is(query(&vm,"surface_getpixel_ext",outside,3,cached,&ok),0);
    vm.render=NULL; gml_vm_free(&vm); gml_render_free(render); free(render);
  }
  return ok;
}
static int buffer_surface_transfer_case(void){
  int ok=1;
  const unsigned char bytes[]={0x12,0x34,0x56,0x00,0xAB,0xCD,0xEF,0x7F,
                                0x10,0x20,0x30,0xFF,0x01,0x00,0xFE,0x80};
  const uint32_t expected[]={0x00123456u,0x7FABCDEFu,0xFF102030u,0x800100FEu};
  for(int cached=0;cached<2;cached++){
    GmlRender *render=calloc(1,sizeof *render);
    if(!render || gml_render_init(render,NULL)){ free(render); return 0; }
    GmlVM vm={.render=render};
    int id=gml_surface_create(render,2,2);
    GmlVal create[]={vreal(32),vreal(0),vreal(1)};
    GmlVal buffer=gml_builtin_call(&vm,"buffer_create",create,3);
    for(unsigned index=0;index<sizeof bytes;index++){
      GmlVal poke[]={buffer,vreal(8+index),vreal(1),vreal(bytes[index])};
      gml_builtin_call(&vm,"buffer_poke",poke,4);
    }
    GmlVal seek[]={buffer,vreal(0),vreal(19)};
    gml_builtin_call(&vm,"buffer_seek",seek,3);
    render->blendmode=1; render->alpha=0; render->color_write_mask=0;
    GmlVal upload[]={buffer,vreal(id),vreal(8)};
    query(&vm,"buffer_set_surface",upload,3,cached,&ok);
    int width=0,height=0;
    const uint32_t *observed=gml_surface_pixels_read(render,id,&width,&height);
    ok &= observed && width==2 && height==2 && !memcmp(observed,expected,sizeof expected);
    ok &= real_is(gml_builtin_call(&vm,"buffer_tell",&buffer,1),19);
    ok &= render->blendmode==1 && render->alpha==0 && render->color_write_mask==0;
    /* Every refusal must precede the first destination write. */
    const double offsets[]={17,-1,2147483648.0,NAN,INFINITY};
    for(unsigned index=0;index<sizeof offsets/sizeof offsets[0];index++){
      upload[2]=vreal(offsets[index]);
      query(&vm,"buffer_set_surface",upload,3,cached,&ok);
      observed=gml_surface_pixels_read(render,id,NULL,NULL);
      ok &= observed && !memcmp(observed,expected,sizeof expected);
    }
    upload[0]=vreal(-1); upload[2]=vreal(8);
    query(&vm,"buffer_set_surface",upload,3,cached,&ok);
    observed=gml_surface_pixels_read(render,id,NULL,NULL);
    ok &= observed && !memcmp(observed,expected,sizeof expected);
    ok &= real_is(gml_builtin_call(&vm,"buffer_tell",&buffer,1),19);
    vm.render=NULL; gml_vm_free(&vm); gml_render_free(render); free(render);
  }
  return ok;
}
static int vertex_format_information_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={.room_index=-1,.pending_room=-1,.next_creation_seq=1};
    vm.particles=gml_particle_state_create(&vm);
    if(!vm.particles) return 0;
    gml_vm_software3d_reset(&vm);
    gml_builtin_call(&vm,"vertex_format_begin",NULL,0);
    gml_builtin_call(&vm,"vertex_format_add_colour",NULL,0);
    gml_builtin_call(&vm,"vertex_format_add_position_3d",NULL,0);
    GmlVal custom[]={vreal(1),vreal(5)};
    gml_builtin_call(&vm,"vertex_format_add_custom",custom,2);
    gml_builtin_call(&vm,"vertex_format_add_texcoord",NULL,0);
    custom[0]=vreal(6); custom[1]=vreal(7);
    gml_builtin_call(&vm,"vertex_format_add_custom",custom,2);
    gml_builtin_call(&vm,"vertex_format_add_normal",NULL,0);
    GmlVal format=gml_builtin_call(&vm,"vertex_format_end",NULL,0);
    GmlVal result=query(&vm,"vertex_format_get_info",&format,1,cached,&ok);
    GmlInstance *info=query_struct(&vm,result);
    ok &= struct_number_is(info,"stride",44) && struct_number_is(info,"num_elements",6);
    GmlVal *elements=info?gml_varmap_get(&info->vars,"elements"):NULL;
    const int expected[][4]={{5,2,4,0},{3,1,12,4},{1,5,4,16},
                            {2,4,8,20},{6,7,4,28},{3,3,12,32}};
    ok &= elements && gml_val_array_length(*elements)==6;
    for(int i=0;elements && i<6;i++){
      GmlInstance *entry=query_struct(&vm,gml_arr_get(*elements,i));
      ok &= struct_number_is(entry,"type",expected[i][0]);
      ok &= struct_number_is(entry,"usage",expected[i][1]);
      ok &= struct_number_is(entry,"size",expected[i][2]);
      ok &= struct_number_is(entry,"offset",expected[i][3]);
    }
    GmlVal second=query(&vm,"vertex_format_get_info",&format,1,cached,&ok);
    GmlInstance *other=query_struct(&vm,second);
    GmlVal *other_elements=other?gml_varmap_get(&other->vars,"elements"):NULL;
    if(elements && other_elements){
      GmlInstance *entry=query_struct(&vm,gml_arr_get(*elements,0));
      if(entry) *gml_varmap_put(&entry->vars,"size")=vreal(99);
      ok &= struct_number_is(query_struct(&vm,gml_arr_get(*other_elements,0)),"size",4);
    }else ok=0;
    *gml_varmap_put(&vm.globals,"retained_info")=second;
    gml_builtin_call(&vm,"vertex_format_delete",&format,1);
    ok &= query(&vm,"vertex_format_get_info",&format,1,cached,&ok).t==V_UNDEF;
    gml_struct_gc(&vm);
    other=query_struct(&vm,second);
    ok &= struct_number_is(other,"stride",44);
    other_elements=other?gml_varmap_get(&other->vars,"elements"):NULL;
    ok &= other_elements && struct_number_is(query_struct(&vm,gml_arr_get(*other_elements,5)),"offset",32);
    size_t size=gml_vm_state_size(&vm),written=0,used=0;
    void *before=malloc(size?size:1),*after=malloc(size?size:1);
    int saved=before && after && gml_vm_state_save(&vm,before,size,&written) && written==size;
    ok &= saved;
    if(saved && other){
      *gml_varmap_put(&other->vars,"stride")=vreal(-99);
      int loaded=gml_vm_state_load(&vm,before,size,&used) && used==size;
      ok &= loaded;
      if(loaded){
        other=query_struct(&vm,second);
        other_elements=other?gml_varmap_get(&other->vars,"elements"):NULL;
        ok &= struct_number_is(other,"stride",44) && other_elements &&
              struct_number_is(query_struct(&vm,gml_arr_get(*other_elements,5)),"offset",32);
        ok &= gml_vm_state_save(&vm,after,size,&written) && written==size &&
              !memcmp(before,after,size);
      }
    }
    free(before); free(after);
    const double invalid[]={-1,128,2147483648.0,NAN,INFINITY,1e100};
    for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
      GmlVal id=vreal(invalid[i]);
      ok &= query(&vm,"vertex_format_get_info",&id,1,cached,&ok).t==V_UNDEF;
    }
    gml_vm_free(&vm);
  }
  return ok;
}
static int vertex_resource_existence_case(void){
  int ok=1;
  for(int bound=0;bound<2;bound++) for(int cached=0;cached<2;cached++){
    GmlRender render={0};
    GmlVM vm={.render=bound?&render:NULL};
    const char *queries[]={"vertex_buffer_exists","vertex_format_exists"};
    GmlVal zero=vreal(0),negative=vreal(-1);
    for(int i=0;i<2;i++){
      ok &= real_is(query(&vm,queries[i],&zero,1,cached,&ok),0);
      ok &= real_is(query(&vm,queries[i],&negative,1,cached,&ok),0);
    }
    GmlVal buffer=gml_builtin_call(&vm,"vertex_create_buffer",NULL,0);
    gml_builtin_call(&vm,"vertex_format_begin",NULL,0);
    gml_builtin_call(&vm,"vertex_format_add_position_3d",NULL,0);
    GmlVal format=gml_builtin_call(&vm,"vertex_format_end",NULL,0);
    ok &= buffer.t==V_REAL && buffer.d>=0 && format.t==V_REAL && format.d>=0;
    /* Allocated empty buffers exist even though their byte/vertex counts are zero. */
    ok &= real_is(query(&vm,queries[0],&buffer,1,cached,&ok),1);
    ok &= real_is(query(&vm,queries[1],&format,1,cached,&ok),1);
    GmlVal begin[]={buffer,format};
    gml_builtin_call(&vm,"vertex_begin",begin,2);
    GmlVal position[]={buffer,vreal(2),vreal(3),vreal(4)};
    gml_builtin_call(&vm,"vertex_position_3d",position,4);
    gml_builtin_call(&vm,"vertex_end",&buffer,1);
    gml_builtin_call(&vm,"vertex_freeze",&buffer,1);
    ok &= real_is(query(&vm,queries[0],&buffer,1,cached,&ok),1);
    ok &= real_is(query(&vm,"vertex_get_number",&buffer,1,cached,&ok),1);
    ok &= real_is(query(&vm,"vertex_get_buffer_size",&buffer,1,cached,&ok),12);
    if(!bound){
      vm.render=&render;
      ok &= real_is(query(&vm,queries[0],&buffer,1,cached,&ok),1);
      ok &= real_is(query(&vm,queries[1],&format,1,cached,&ok),1);
      vm.render=NULL;
    }
    gml_builtin_call(&vm,"vertex_format_delete",&format,1);
    ok &= real_is(query(&vm,queries[1],&format,1,cached,&ok),0);
    ok &= real_is(query(&vm,queries[0],&buffer,1,cached,&ok),1);
    gml_builtin_call(&vm,"vertex_delete_buffer",&buffer,1);
    ok &= real_is(query(&vm,queries[0],&buffer,1,cached,&ok),0);
    buffer=gml_builtin_call(&vm,"vertex_create_buffer",NULL,0);
    ok &= real_is(query(&vm,queries[0],&buffer,1,cached,&ok),1);
    gml_builtin_call(&vm,"vertex_format_begin",NULL,0);
    gml_builtin_call(&vm,"vertex_format_add_position",NULL,0);
    format=gml_builtin_call(&vm,"vertex_format_end",NULL,0);
    ok &= real_is(query(&vm,queries[1],&format,1,cached,&ok),1);
    const double invalid[]={-1,2147483647.0,2147483648.0,1e100,NAN,INFINITY,-INFINITY};
    for(int i=0;i<2;i++) for(size_t j=0;j<sizeof invalid/sizeof invalid[0];j++){
      GmlVal value=vreal(invalid[j]);
      ok &= real_is(query(&vm,queries[i],&value,1,cached,&ok),0);
    }
    gml_vm_free(&vm);
  }
  return ok;
}
static int string_is(GmlVal v,const char *text){
  int ok=v.t==V_STR && v.s && !strcmp(v.s,text);
  if(v.t==V_STR && v.d!=0) free((void *)v.s);
  else gml_values_release(&v,1);
  return ok;
}
typedef struct DisplayHost { uint32_t observed,requested; int asynchronous; } DisplayHost;
static uint32_t display_query(void *userdata){
  return ((DisplayHost*)userdata)->observed;
}
static AnygmResult display_request(void *userdata,uint32_t hz){
  DisplayHost *host=userdata;
  host->requested=hz;
  if(hz!=50 && hz!=60) return ANYGM_ERROR_UNSUPPORTED;
  if(!host->asynchronous) host->observed=hz;
  return ANYGM_OK;
}
static int display_frequency_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    DisplayHost display={.observed=60};
    AnygmHostServices host={.struct_size=sizeof host,.abi_version=ANYGM_HOST_SERVICES_VERSION,
      .userdata=&display,.display_frequency=display_query,
      .display_frequency_request=display_request};
    GmlVM vm={.host=&host,.frame=7};
    GmlVal speed[]={vreal(30),vreal(0)},frequency=vreal(50);
    query(&vm,"game_set_speed",speed,2,cached,&ok);
    ok &= real_is(query(&vm,"display_get_frequency",NULL,0,cached,&ok),60);
    ok &= real_is(query(&vm,"display_set_frequency",&frequency,1,cached,&ok),1);
    ok &= real_is(query(&vm,"display_get_frequency",NULL,0,cached,&ok),50);
    ok &= real_is(query(&vm,"game_get_speed",speed+1,1,cached,&ok),30) && vm.frame==7;
    frequency=vreal(53);
    ok &= real_is(query(&vm,"display_set_frequency",&frequency,1,cached,&ok),0);
    ok &= display.observed==50 && gml_varmap_get(&vm.globals,"__display_request_hz")->d==50;
    frequency=vreal(60);
    ok &= real_is(query(&vm,"display_set_frequency",&frequency,1,cached,&ok),1);
    display.asynchronous=1; frequency=vreal(50);
    ok &= real_is(query(&vm,"display_set_frequency",&frequency,1,cached,&ok),1);
    ok &= real_is(query(&vm,"display_get_frequency",NULL,0,cached,&ok),60);
    const double invalid[]={0,1,-50,NAN,INFINITY,-INFINITY,1e100};
    for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
      display.requested=0; frequency=vreal(invalid[i]);
      ok &= real_is(query(&vm,"display_set_frequency",&frequency,1,cached,&ok),0);
      ok &= display.requested==0;
    }
    vm.host=NULL; frequency=vreal(50);
    ok &= real_is(query(&vm,"display_get_frequency",NULL,0,cached,&ok),0);
    ok &= real_is(query(&vm,"display_set_frequency",&frequency,1,cached,&ok),0);
    gml_vm_free(&vm);
  }
  return ok;
}
static int explicit_default_speed_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlWin win={.game_speed=30};
    GmlVM vm={.win=&win,.frame=7};
    GmlVal speed[]={vreal(60),vreal(0)};
    query(&vm,"game_set_speed",speed,2,cached,&ok);
    GmlVal *explicit_speed=gml_varmap_get(&vm.globals,"__game_speed_fps");
    ok &= explicit_speed && real_is(*explicit_speed,60);
    ok &= gml_room_speed(&vm)==60 && vm.frame==7;
    ok &= real_is(query(&vm,"game_get_speed",speed+1,1,cached,&ok),60);
    speed[0]=vreal(30);
    query(&vm,"game_set_speed",speed,2,cached,&ok);
    ok &= gml_room_speed(&vm)==30;
    speed[0]=vreal(60);
    query(&vm,"game_set_speed",speed,2,cached,&ok);
    ok &= gml_room_speed(&vm)==60 && vm.frame==7;
    gml_vm_free(&vm);
  }
  return ok;
}
static int circle_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlRender render={0}; GmlVM vm={.render=&render};
    int values[]={4,12,32,64};
    for(int i=0;i<4;i++){
      GmlVal arg=vreal(values[i]);
      gml_builtin_call(&vm,"draw_set_circle_precision",&arg,1);
      ok &= real_is(query(&vm,"draw_get_circle_precision",NULL,0,cached,&ok),values[i]);
      GmlRenderDrawState state;
      gml_render_draw_state_get(&render,&state);
      ok &= state.circle_precision==values[i];
    }
    vm.render=NULL; gml_vm_free(&vm);
  }
  return ok;
}
static int application_surface_lifetime_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    uint32_t pixels[4]={0};
    GmlWin win={0}; GmlRender render;
    if(gml_render_init(&render,&win)!=0) return 0;
    GmlVM vm={.win=&win,.render=&render};
    gml_render_application_surface_bind(&render,pixels,2,2,0);
    GmlVal app=vreal(0);
    /* Backing pixels exist at boot, but content first observes the surface
     * when Draw begins. A custom surface remains visible independently. */
    GmlVal before=query(&vm,"surface_exists",&app,1,cached,&ok);
    if(!real_is(before,0)) fprintf(stderr,"surface lifetime before: cached=%d type=%d value=%g\n",cached,before.t,before.d);
    ok &= real_is(before,0);
    GmlVal dimensions[]={vreal(2),vreal(2)};
    GmlVal custom=query(&vm,"surface_create",dimensions,2,cached,&ok);
    GmlVal own=query(&vm,"surface_exists",&custom,1,cached,&ok);
    if(!(custom.d>0 && real_is(own,1))) fprintf(stderr,"surface lifetime custom: cached=%d id=%g exists=%g\n",cached,custom.d,own.d);
    ok &= custom.d>0 && real_is(own,1);
    gml_render_application_surface_publish(&render);
    GmlVal after=query(&vm,"surface_exists",&app,1,cached,&ok);
    if(!real_is(after,1)) fprintf(stderr,"surface lifetime after: cached=%d type=%d value=%g\n",cached,after.t,after.d);
    ok &= real_is(after,1);
    query(&vm,"surface_free",&custom,1,cached,&ok);
    ok &= real_is(query(&vm,"surface_exists",&custom,1,cached,&ok),0);
    vm.render=NULL; gml_vm_free(&vm); gml_render_free(&render);
  }
  return ok;
}
static void word(uint8_t *data,int offset,uint32_t value){
  for(int i=0;i<4;i++) data[offset+i]=(uint8_t)(value>>(i*8));
}
static int shader_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    uint8_t bytes[64]={0};
    char first[]="shade_first",second[]="shade_second";
    char *names[]={first,second}; uint32_t offsets[]={40,52};
    GmlWin win={.data=bytes,.size=sizeof bytes,.n_chunks=1,
      .strs=names,.str_charoff=offsets,.n_strs=2};
    win.chunks[0]=(GmlChunk){"SHDR",8,12};
    word(bytes,8,2); word(bytes,12,24); word(bytes,16,28);
    word(bytes,24,40); word(bytes,28,52);
    GmlVM vm={.win=&win};
    GmlVal arg=vreal(0);
    GmlVal name=query(&vm,"shader_get_name",&arg,1,cached,&ok);
    first[0]='X';
    ok &= string_is(name,"shade_first"); /* own the returned name, not borrowed content */
    arg=vreal(1); ok &= string_is(query(&vm,"shader_get_name",&arg,1,cached,&ok),second);
    double bad[]={-1,2,NAN,INFINITY,1e100};
    for(int i=0;i<5;i++){
      arg=vreal(bad[i]); ok &= string_is(query(&vm,"shader_get_name",&arg,1,cached,&ok),"");
    }
    vm.win=NULL; gml_vm_free(&vm);
  }
  return ok;
}
static int depth_matches(GmlVal result,const int *ids,int count){
  GmlArr *array=result.arr;
  int ok=result.t==V_ARR && array && gml_val_array_length(result)==(count?count:1);
  if(ok && !count) ok=real_is(array->data[0],-1);
  for(int i=0;ok && i<count;i++){
    int found=0;
    for(int j=0;j<count;j++) found+=real_is(array->data[j],ids[i]);
    ok &= found==1;
  }
  gml_values_release(&result,1); return ok;
}
static int layers_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal args[]={vreal(12.5),vstr("first")};
    GmlVal first=gml_builtin_call(&vm,"layer_create",args,2);
    args[1]=vstr("second"); GmlVal second=gml_builtin_call(&vm,"layer_create",args,2);
    args[0]=vreal(-20); args[1]=vstr("other"); gml_builtin_call(&vm,"layer_create",args,2);
    int ids[]={(int)first.d,(int)second.d}; GmlVal depth=vreal(12.5);
    ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),ids,2);
    GmlVal move[]={first,vreal(99)}; gml_builtin_call(&vm,"layer_depth",move,2);
    ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),ids+1,1);
    gml_builtin_call(&vm,"layer_destroy",&second,1);
    ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),NULL,0);
    depth=vreal(NAN);
    ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),NULL,0);
    gml_vm_free(&vm);
  }
  return ok;
}
static int forced_layer_depth_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlWin win={0};
    GmlVM vm={.win=&win};
    vm.particles=gml_particle_state_create(&vm);
    gml_vm_software3d_reset(&vm);
    if(!vm.particles){ gml_vm_free(&vm); return 0; }
    const char *depth="layer_get_forced_depth",*enabled="layer_is_draw_depth_forced";
    ok &= real_is(query(&vm,depth,NULL,0,cached,&ok),0);
    ok &= real_is(query(&vm,enabled,NULL,0,cached,&ok),0);
    GmlVal initial[]={vreal(81),vstr("sample")};
    GmlVal layer=query(&vm,"layer_create",initial,2,cached,&ok);
    GmlVal force[]={vreal(1),vreal(-12.75)};
    query(&vm,"layer_force_draw_depth",force,2,cached,&ok);
    ok &= real_is(query(&vm,depth,NULL,0,cached,&ok),-12);
    ok &= real_is(query(&vm,enabled,NULL,0,cached,&ok),1);
    ok &= real_is(query(&vm,"layer_get_depth",&layer,1,cached,&ok),81);

    size_t size=gml_vm_state_size(&vm),written=0,used=0;
    void *state=malloc(size);
    int saved=state && gml_vm_state_save(&vm,state,size,&written);
    force[0]=vreal(0); force[1]=vreal(20);
    query(&vm,"layer_force_draw_depth",force,2,cached,&ok);
    ok &= real_is(query(&vm,depth,NULL,0,cached,&ok),20);
    ok &= real_is(query(&vm,enabled,NULL,0,cached,&ok),0);
    force[0]=vreal(1); force[1]=vreal(NAN);
    query(&vm,"layer_force_draw_depth",force,2,cached,&ok);
    ok &= real_is(query(&vm,depth,NULL,0,cached,&ok),20);
    ok &= real_is(query(&vm,enabled,NULL,0,cached,&ok),0);
    ok &= saved && gml_vm_state_load(&vm,state,written,&used) && used==written;
    ok &= real_is(query(&vm,depth,NULL,0,cached,&ok),-12);
    ok &= real_is(query(&vm,enabled,NULL,0,cached,&ok),1);
    free(state);
    gml_vm_free(&vm);
  }
  if(ok) puts("forced layer depth: default, signed value, toggle and restore passed");
  return ok;
}
static int layer_instance_elements_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlWin win={.bytecode=17};
    GmlVM vm={.win=&win,.inst_cap=8,.n_objects=1,.next_id=100000};
    vm.inst=calloc((size_t)vm.inst_cap,sizeof *vm.inst);
    vm.objects=calloc(1,sizeof *vm.objects);
    vm.particles=gml_particle_state_create(&vm);
    gml_vm_software3d_reset(&vm);
    if(!vm.inst || !vm.objects || !vm.particles){ gml_vm_free(&vm); return 0; }
    vm.objects[0]=(GmlObject){.name="entity",.sprite_index=-1,.mask_index=-1,.parent=-1,.visible=1};
    GmlVal layer_args[]={vreal(20),vstr("source")};
    GmlVal source=query(&vm,"layer_create",layer_args,2,cached,&ok);
    layer_args[1]=vstr("destination");
    GmlVal destination=query(&vm,"layer_create",layer_args,2,cached,&ok);
    GmlRtElem *visual=gml_rt_elem_new(&vm);
    if(!visual){ gml_vm_free(&vm); return 0; }
    visual->type=3; visual->layer=(int)source.d;
    GmlVal visual_id=vreal(visual->id);
    GmlVal spawn[]={vreal(3),vreal(5),source,vreal(0)};
    GmlVal instance=query(&vm,"instance_create_layer",spawn,4,cached,&ok);
    GmlVal source_name=vstr("source");
    GmlVal elements=query(&vm,"layer_get_all_elements",&source_name,1,cached,&ok);
    GmlVal element=gml_arr_get(elements,1);
    const char *get_instance="layer_instance_get_instance";
    ok &= gml_val_array_length(elements)==2 && real_is(gml_arr_get(elements,0),visual_id.d);
    ok &= element.t==V_REAL && element.d>0 && element.d!=instance.d;
    ok &= real_is(query(&vm,"layer_get_element_type",&element,1,cached,&ok),2);
    ok &= real_is(query(&vm,"layer_get_element_layer",&element,1,cached,&ok),source.d);
    ok &= real_is(query(&vm,get_instance,&element,1,cached,&ok),instance.d);
    ok &= real_is(query(&vm,get_instance,&visual_id,1,cached,&ok),-4);
    ok &= real_is(query(&vm,get_instance,&instance,1,cached,&ok),-4);
    gml_values_release_owned(&elements,1);
    GmlVal region[]={element,vreal(20),vreal(30),vreal(4),vreal(4)};
    query(&vm,"layer_tile_region",region,5,cached,&ok);
    ok &= real_is(query(&vm,get_instance,&element,1,cached,&ok),instance.d);

    GmlVal assignment[]={destination,instance};
    query(&vm,"layer_add_instance",assignment,2,cached,&ok);
    query(&vm,"layer_destroy",&source,1,cached,&ok);
    elements=query(&vm,"layer_get_all_elements",&destination,1,cached,&ok);
    ok &= gml_val_array_length(elements)==1 && real_is(gml_arr_get(elements,0),element.d);
    ok &= real_is(query(&vm,"layer_get_element_layer",&element,1,cached,&ok),destination.d);
    gml_values_release_owned(&elements,1);
    query(&vm,"instance_deactivate_layer",&destination,1,cached,&ok);
    ok &= real_is(query(&vm,get_instance,&element,1,cached,&ok),instance.d);
    query(&vm,"instance_activate_layer",&destination,1,cached,&ok);
    GmlVal move[]={element,vstr("destination")};
    ok &= query(&vm,"layer_element_move",move,2,cached,&ok).t==V_UNDEF;
    GmlInstance *moved=gml_vm_instance_by_id(&vm,instance.d);
    int move_order=moved?moved->draw_layer_element_order:-1;
    ok &= moved && move_order>=GML_LAYER_MOVED_ORDER_BASE;

    size_t size=gml_vm_state_size(&vm),written=0,used=0;
    void *state=malloc(size);
    int saved=state && gml_vm_state_save(&vm,state,size,&written);
    query(&vm,"layer_destroy",&destination,1,cached,&ok);
    ok &= real_is(query(&vm,get_instance,&element,1,cached,&ok),-4);
    ok &= saved && gml_vm_state_load(&vm,state,written,&used) && used==written;
    free(state);
    ok &= real_is(query(&vm,get_instance,&element,1,cached,&ok),instance.d);
    GmlInstance *live=gml_vm_instance_by_id(&vm,instance.d);
    if(!live){ gml_vm_free(&vm); return 0; }
    ok &= live->draw_layer_element_order==move_order;
    gml_instance_destroy(&vm,live);
    gml_vm_instances_reap(&vm);
    spawn[2]=destination;
    GmlVal replacement=query(&vm,"instance_create_layer",spawn,4,cached,&ok);
    ok &= vm.inst_count==1 && replacement.d!=instance.d;
    ok &= real_is(query(&vm,get_instance,&element,1,cached,&ok),-4);
    elements=query(&vm,"layer_get_all_elements",&destination,1,cached,&ok);
    GmlVal new_element=gml_arr_get(elements,0);
    ok &= gml_val_array_length(elements)==1 && new_element.d!=element.d;
    ok &= real_is(query(&vm,get_instance,&new_element,1,cached,&ok),replacement.d);
    gml_values_release_owned(&elements,1);
    gml_vm_free(&vm);
  }
  if(ok) puts("layer instance elements: discovery, identity, movement, restore and expiry passed");
  return ok;
}
static int alpha_case(void){
  GmlRender render={0}; GmlVM vm={.render=&render}; int ok=1;
  for(int enabled=1;enabled>=0;enabled--){
    GmlVal arg=vreal(enabled);
    gml_builtin_call(&vm,"draw_set_alpha_test",&arg,1);
    ok &= real_is(gml_builtin_call(&vm,"gpu_get_alphatestenable",NULL,0),enabled);
  }
  int references[]={17,128,255,0};
  for(int i=0;i<4;i++){
    GmlVal arg=vreal(references[i]);
    gml_builtin_call(&vm,"draw_set_alpha_test_ref_value",&arg,1);
    ok &= real_is(gml_builtin_call(&vm,"gpu_get_alphatestref",NULL,0),references[i]);
  }
  ok &= gml_builtin_fast_id(&vm,"draw_set_alpha_test")==-1 &&
        gml_builtin_fast_id(&vm,"draw_set_alpha_test_ref_value")==-1;
  vm.render=NULL; gml_vm_free(&vm); return ok;
}
static int layer_integer_depth_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0};
    const double inputs[]={12.9,-12.9,0.9,-0.9,2147483647.9,2147483648.0,
                           -2147483649.0,4294967308.75};
    const double expected[]={12,-12,0,0,2147483647.0,-2147483648.0,
                             2147483647.0,12};
    for(size_t i=0;i<sizeof inputs/sizeof inputs[0];i++){
      GmlVal args[]={vreal(inputs[i]),vstr("normalized")};
      GmlVal layer=query(&vm,"layer_create",args,2,cached,&ok);
      ok &= real_is(query(&vm,"layer_get_depth",&layer,1,cached,&ok),expected[i]);
      int id=(int)layer.d;
      GmlVal depth=vreal(expected[i]);
      ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),&id,1);
      depth=vreal(inputs[i]);
      ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),&id,1);
      GmlVal move[]={layer,vreal(-22.75)};
      query(&vm,"layer_depth",move,2,cached,&ok);
      ok &= real_is(query(&vm,"layer_get_depth",&layer,1,cached,&ok),-22);
      depth=vreal(-22.5);
      ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),&id,1);
      const double invalid[]={NAN,INFINITY,-INFINITY};
      for(size_t j=0;j<sizeof invalid/sizeof invalid[0];j++){
        /* Invalid depth is rejected without publishing a layer or changing one. */
        int count=vm.n_rtl;
        args[0]=vreal(invalid[j]);
        ok &= real_is(query(&vm,"layer_create",args,2,cached,&ok),-1) && vm.n_rtl==count;
        move[1]=args[0]; query(&vm,"layer_depth",move,2,cached,&ok);
        ok &= real_is(query(&vm,"layer_get_depth",&layer,1,cached,&ok),-22);
        ok &= depth_matches(query(&vm,"layer_get_id_at_depth",args,1,cached,&ok),NULL,0);
      }
      query(&vm,"layer_destroy",&layer,1,cached,&ok);
    }
    gml_vm_free(&vm);
  }
  return ok;
}
static int alpha_reference_bounds_case(void){
  GmlRender render={0}; GmlVM vm={.render=&render}; int ok=1;
  const char *names[]={"draw_set_alpha_test_ref_value","gpu_set_alphatestref"};
  const double inputs[]={17.9,254.9,-0.9,-1,256,1e100,-1e100};
  const double expected[]={17,254,0,0,255,255,0};
  for(size_t alias=0;alias<sizeof names/sizeof names[0];alias++){
    for(size_t i=0;i<sizeof inputs/sizeof inputs[0];i++){
      GmlVal arg=vreal(inputs[i]); gml_builtin_call(&vm,names[alias],&arg,1);
      ok &= real_is(gml_builtin_call(&vm,"gpu_get_alphatestref",NULL,0),expected[i]);
    }
    const double invalid[]={NAN,INFINITY,-INFINITY};
    for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
      GmlVal arg=vreal(17); gml_builtin_call(&vm,names[alias],&arg,1);
      arg=vreal(invalid[i]); gml_builtin_call(&vm,names[alias],&arg,1);
      ok &= real_is(gml_builtin_call(&vm,"gpu_get_alphatestref",NULL,0),17);
    }
  }
  vm.render=NULL; gml_vm_free(&vm); return ok;
}
static int background_names_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlBg backgrounds[2]={{.tpag=-1,.name="neutral_background_a"},
                          {.tpag=-1,.name="neutral_background_b"}};
    GmlRender render={.bg=backgrounds,.n_bg=2}; GmlVM vm={.render=&render};
    for(int i=0;i<2;i++){
      GmlVal arg=vreal(i);
      ok &= string_is(query(&vm,"background_name",&arg,1,cached,&ok),backgrounds[i].name);
      ok &= string_is(query(&vm,"background_get_name",&arg,1,cached,&ok),backgrounds[i].name);
    }
    const double invalid[]={-1,2,INFINITY,-INFINITY,NAN,1e100};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++){
      GmlVal arg=vreal(invalid[i]);
      ok &= string_is(query(&vm,"background_name",&arg,1,cached,&ok),"");
      ok &= string_is(query(&vm,"background_get_name",&arg,1,cached,&ok),"");
    }
    /* Keep the existing numeric-index truncation and missing-index fallback. */
    GmlVal fractional=vreal(0.75);
    ok &= string_is(query(&vm,"background_name",&fractional,1,cached,&ok),backgrounds[0].name);
    ok &= string_is(query(&vm,"background_get_name",&fractional,1,cached,&ok),backgrounds[0].name);
    ok &= string_is(query(&vm,"background_name",NULL,0,cached,&ok),backgrounds[0].name);
    ok &= string_is(query(&vm,"background_get_name",NULL,0,cached,&ok),backgrounds[0].name);
    ok &= !strcmp(backgrounds[0].name,"neutral_background_a") &&
          !strcmp(backgrounds[1].name,"neutral_background_b");
    vm.render=NULL; gml_vm_free(&vm);
  }
  return ok;
}
static int background_part_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlWin win={0}; GmlRender render;
    if(gml_render_init(&render,&win)!=0) return 0;
    int atlas_id=render.n_atlas;
    GmlAtlas *atlases=realloc(render.atlas,(size_t)(atlas_id+1)*sizeof *atlases);
    if(!atlases) abort();
    render.atlas=atlases; render.n_atlas++;
    memset(&render.atlas[atlas_id],0,sizeof render.atlas[atlas_id]);
    render.tpag=calloc(1,sizeof *render.tpag); render.n_tpag=1;
    render.bg=calloc(1,sizeof *render.bg); render.n_bg=1;
    if(!render.atlas || !render.tpag || !render.bg) abort();
    render.atlas[atlas_id].px=malloc(4*4*4);
    render.atlas[atlas_id].w=render.atlas[atlas_id].h=4;
    if(!render.atlas[atlas_id].px) abort();
    for(int y=0;y<4;y++) for(int x=0;x<4;x++){
      uint8_t *pixel=render.atlas[atlas_id].px+(y*4+x)*4;
      pixel[0]=(uint8_t)(x*20+7); pixel[1]=(uint8_t)(y*30+11); pixel[2]=13; pixel[3]=255;
    }
    render.tpag[0]=(GmlTpag){.sw=4,.sh=4,.tw=4,.th=4,.bw=4,.bh=4,.atlas=atlas_id};
    uint32_t frame[16*16]={0};
    gml_render_begin(&render,frame,16,16,0,0);
    GmlVM vm={.render=&render};
    GmlVal args[]={vreal(0),vreal(1),vreal(2),vreal(2),vreal(1),vreal(5),vreal(6)};
    query(&vm,"draw_background_part",args,7,cached,&ok);
    ok &= (frame[6*16+5]&0xFFFFFFu)==0x1B470Du &&
          (frame[6*16+6]&0xFFFFFFu)==0x2F470Du && (frame[6*16+7]&0xFFFFFFu)==0;
    vm.render=NULL; gml_vm_free(&vm); gml_render_free(&render);
  }
  return ok;
}
static int font_styles_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    uint8_t bytes[576]={0};
    GmlWin win={.data=bytes,.size=sizeof bytes,.bytecode=15,.n_chunks=1};
    win.chunks[0]=(GmlChunk){"FONT",8,24};
    word(bytes,8,4);
    for(int i=0;i<4;i++){
      int record=64+i*96,glyph=record+60;
      word(bytes,12+i*4,(uint32_t)record);
      word(bytes,record+8,12);
      word(bytes,record+12,(uint32_t)(i&1));
      word(bytes,record+16,(uint32_t)(i>>1));
      word(bytes,record+28,512);
      word(bytes,record+32,0x3F800000u); word(bytes,record+36,0x3F800000u);
      word(bytes,record+40,1); word(bytes,record+44,(uint32_t)glyph);
      word(bytes,glyph,'A'); word(bytes,glyph+4,2u<<16);
      word(bytes,glyph+8,3u|(4u<<16));
    }
    GmlRender render={.win=&win};
    parse_font(&render);
    GmlVM vm={.render=&render,.win=&win};
    for(int i=0;i<4;i++){
      GmlVal arg=vreal(i);
      ok &= real_is(query(&vm,"font_get_bold",&arg,1,cached,&ok),i&1);
      ok &= real_is(query(&vm,"font_get_italic",&arg,1,cached,&ok),i>>1);
      render.font=i;
      ok &= gml_text_width(&render,"A")==4;
    }
    GmlVal fraction=vreal(1.75);
    ok &= real_is(query(&vm,"font_get_bold",&fraction,1,cached,&ok),1);
    const double invalid[]={-1,4,NAN,INFINITY,1e100};
    for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
      GmlVal arg=vreal(invalid[i]);
      ok &= real_is(query(&vm,"font_get_bold",&arg,1,cached,&ok),0);
      ok &= real_is(query(&vm,"font_get_italic",&arg,1,cached,&ok),0);
    }
    ok &= real_is(query(&vm,"font_get_bold",NULL,0,cached,&ok),0);
    vm.render=NULL;
    GmlVal arg=vreal(1);
    ok &= real_is(query(&vm,"font_get_italic",&arg,1,cached,&ok),0);
    vm.win=NULL; gml_vm_free(&vm); gml_render_free(&render);
  }
  return ok;
}

static int runtime_font_styles_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    AnygmMemoryVfs *memory=calloc(1,sizeof(*memory));
    if(!memory) return 0;
    AnygmHostServices host={0}; anygm_memory_vfs_init(memory,&host);
    uint8_t bytes[552]; size_t size=font_fixture_build(bytes);
    ok &= anygm_memory_vfs_add_file(memory,"/content/fixture.ttf",bytes,size);
    GmlWin win={.host=&host}; snprintf(win.content_dir,sizeof win.content_dir,"/content");
    GmlRender render={.win=&win}; GmlVM vm={.win=&win,.host=&host,.render=&render};
    for(int i=0;ok && i<4;i++){
      GmlVal args[]={vstr("fixture.ttf"),vreal(32),vreal(i&1),vreal(i>>1),vreal(65),vreal(65)};
      GmlVal font=query(&vm,"font_add",args,6,cached,&ok);
      ok &= real_is(font,i) && real_is(query(&vm,"font_get_bold",&font,1,cached,&ok),i&1) &&
        real_is(query(&vm,"font_get_italic",&font,1,cached,&ok),i>>1);
      if(!ok) fprintf(stderr,"runtime font creation/style: cached=%d style=%d id=%g\n",cached,i,font.d);
      query(&vm,"font_delete",&font,1,cached,&ok);
      ok &= real_is(query(&vm,"font_get_bold",&font,1,cached,&ok),0) &&
        real_is(query(&vm,"font_get_italic",&font,1,cached,&ok),0);
    }
    render.n_spr=1; render.spr=calloc(1,sizeof(*render.spr));
    if(!render.spr) ok=0;
    if(ok){
      render.spr[0].n_frames=1;
      GmlVal args[]={vreal(0),vstr("A"),vreal(1),vreal(0)};
      GmlVal font=query(&vm,"font_add_sprite_ext",args,4,cached,&ok);
      ok &= real_is(font,4) && real_is(query(&vm,"font_get_bold",&font,1,cached,&ok),0) &&
        real_is(query(&vm,"font_get_italic",&font,1,cached,&ok),0);
    }
    const double invalid[]={NAN,INFINITY,-INFINITY,1e100};
    for(size_t i=0;ok && i<sizeof invalid/sizeof invalid[0];i++){
      GmlVal args[]={vstr("fixture.ttf"),vreal(32),vreal(1),vreal(1),vreal(invalid[i]),vreal(65)};
      ok &= real_is(query(&vm,"font_add",args,6,cached,&ok),-1) && render.n_fonts==5;
      args[4]=vreal(65); args[5]=vreal(invalid[i]);
      ok &= real_is(query(&vm,"font_add",args,6,cached,&ok),-1) && render.n_fonts==5;
      args[5]=vreal(65); args[1]=vreal(invalid[i]);
      GmlVal result=query(&vm,"font_add",args,6,cached,&ok);
      /* Character indices reject an out-of-range integer; finite sizes retain the loader's
       * established clamp to 256 pixels rather than becoming an invalid font request. */
      if(isfinite(invalid[i]))
        ok &= real_is(result,5) && render.n_fonts==6 &&
          render.fonts[5].runtime_pixel_height==256;
      else ok &= real_is(result,-1) && render.n_fonts==5;
      if(!ok) fprintf(stderr,"runtime font numeric guard: cached=%d input=%g id=%g count=%d\n",
                       cached,invalid[i],result.d,render.n_fonts);
    }
    vm.render=NULL; vm.win=NULL; gml_vm_free(&vm); gml_render_free(&render);
    anygm_memory_vfs_destroy(memory); free(memory);
  }
  return ok;
}

static int inverse_destination_colour_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlWin win={.bytecode=17}; GmlRender render; GmlVM vm={.win=&win};
    if(gml_render_init(&render,&win)) return 0;
    vm.render=&render;
    uint32_t *pixels=malloc(512u*384u*sizeof *pixels);
    render.atlas=calloc(1,sizeof *render.atlas); render.n_atlas=1;
    render.tpag=calloc(1,sizeof *render.tpag); render.n_tpag=1;
    render.spr=calloc(1,sizeof *render.spr); render.n_spr=render.spr_cap=1;
    if(!pixels || !render.atlas || !render.tpag || !render.spr) return 0;
    render.atlas[0].px=malloc(4); render.atlas[0].w=render.atlas[0].h=1;
    render.spr[0].frame=malloc(sizeof(int));
    if(!render.atlas[0].px || !render.spr[0].frame) return 0;
    memset(render.atlas[0].px,255,4); render.spr[0].frame[0]=0;
    render.spr[0].w=render.spr[0].h=render.spr[0].n_frames=1;
    render.tpag[0]=(GmlTpag){.sw=1,.sh=1,.bw=1,.bh=1,.atlas=0};
    uint8_t *runtime_pixels=malloc(4); if(!runtime_pixels) return 0;
    memset(runtime_pixels,255,4);
    int runtime=gml_sprite_append_from_rgba(&render,runtime_pixels,1,1,0,0,NULL);
    int surface=gml_surface_create(&render,2,2);
    if(runtime<0 || surface<0) return 0;
    gml_surface_set_target(&render,surface);
    query(&vm,"draw_clear",(GmlVal[]){vreal(0xFFFFFF)},1,0,&ok);
    gml_surface_reset_target(&render);
    GmlVal factors[]={vreal(10),vreal(1)};
    for(int path=0;path<8;path++){
      for(int i=0;i<512*384;i++) pixels[i]=0xFF204080u;
      gml_render_begin(&render,pixels,512,384,0,0);
      query(&vm,"gpu_set_blendmode_ext",factors,2,cached,&ok);
      render.interp=path==4;
      if(path==0){
        GmlVal rectangle[]={vreal(0),vreal(0),vreal(511),vreal(383),vreal(0)};
        query(&vm,"draw_set_color",(GmlVal[]){vreal(0xFFFFFF)},1,cached,&ok);
        query(&vm,"draw_rectangle",rectangle,5,0,&ok);
      } else if(path>=5){
        if(path==5) gml_draw_surface_stretched(&render,surface,0,0,512,384,0xFFFFFF,1);
        else gml_draw_surface_ext(&render,surface,0,path==7?384:0,
                                 path==7?192:256,path==7?256:192,
                                 path==7?90:0,0xFFFFFF,1);
      } else {
        GmlVal sprite[]={vreal(path==1?runtime:0),vreal(0),vreal(0),vreal(path==3?384:0),
          vreal(path==3?384:512),vreal(path==3?512:384),vreal(path==3?90:0),
          vreal(0xFFFFFF),vreal(1)};
        query(&vm,"draw_sprite_ext",sprite,9,cached,&ok);
      }
      gml_render_prepare_draw(&render);
      if(pixels[160*512+240]!=0xFFDFBF7Fu){
        fprintf(stderr,"inverse destination colour: cached=%d path=%d got=%08x\n",
                cached,path,pixels[160*512+240]); ok=0;
      }
    }
    gml_surface_set_target(&render,surface);
    query(&vm,"draw_clear_alpha",(GmlVal[]){vreal(0x804020),vreal(64.0/255.0)},2,0,&ok);
    query(&vm,"gpu_set_blendmode_ext",factors,2,cached,&ok);
    query(&vm,"draw_set_color",(GmlVal[]){vreal(0xFF8040)},1,cached,&ok);
    query(&vm,"draw_set_alpha",(GmlVal[]){vreal(128.0/255.0)},1,cached,&ok);
    query(&vm,"draw_rectangle",(GmlVal[]){vreal(0),vreal(0),vreal(1),vreal(1),vreal(0)},5,0,&ok);
    gml_render_prepare_draw(&render);
    if(render.fb[0]!=0x6038607Fu || render.fb_all_opaque){
      fprintf(stderr,"inverse destination surface coverage: cached=%d got=%08x opaque=%d\n",
              cached,render.fb[0],render.fb_all_opaque); ok=0;
    }
    gml_surface_reset_target(&render);
    vm.render=NULL; vm.win=NULL; gml_vm_free(&vm); gml_render_free(&render); free(pixels);
  }
  return ok;
}

static int source_colour_source_alpha_case(void){
  static const unsigned alphas[]={0u,1u,64u,128u,255u};
  static const unsigned colours[]={0xFFFFFFu,0x654D2Du};
  static const char *setters[]={"draw_set_blend_mode_ext","gpu_set_blendmode_ext"};
  int ok=1;
  for(int cached=0;cached<2;cached++) for(unsigned setter=0;setter<2u;setter++){
    GmlWin win={.bytecode=15};
    GmlRender render;
    GmlVM vm={.win=&win};
    uint32_t screen[16]={0};
    if(gml_render_init(&render,&win)) return 0;
    vm.render=&render;
    gml_render_begin(&render,screen,4,4,0,0);
    int target=gml_surface_create(&render,4,4);
    int source=gml_surface_create(&render,1,1);
    render.atlas=calloc(1,sizeof *render.atlas); render.n_atlas=1;
    render.tpag=calloc(1,sizeof *render.tpag); render.n_tpag=1;
    render.spr=calloc(1,sizeof *render.spr);
    render.n_spr=render.spr_cap=render.base_n_spr=1;
    uint8_t *rgba=malloc(4);
    if(target<0 || source<0 || !rgba || !render.atlas || !render.tpag || !render.spr) return 0;
    render.atlas[0].px=malloc(4);
    render.atlas[0].w=render.atlas[0].h=1;
    render.spr[0].frame=malloc(sizeof(int));
    if(!render.atlas[0].px || !render.spr[0].frame) return 0;
    render.spr[0].frame[0]=0;
    render.spr[0].w=render.spr[0].h=render.spr[0].n_frames=1;
    render.tpag[0]=(GmlTpag){.sw=1,.sh=1,.tw=1,.th=1,.bw=1,.bh=1,.atlas=0};
    memset(rgba,255,4);
    int sprite=gml_sprite_append_from_rgba(&render,rgba,1,1,0,0,NULL);
    if(sprite<0) return 0;
    for(unsigned colour=0;colour<2u;colour++) for(unsigned alpha=0;alpha<5u;alpha++){
      unsigned channels[]={colours[colour]&255u,(colours[colour]>>8)&255u,
                           (colours[colour]>>16)&255u,alphas[alpha]};
      unsigned destination[]={32u,64u,96u,255u};
      unsigned expected[4];
      for(unsigned channel=0;channel<4u;channel++){
        unsigned sum=channels[channel]*channels[channel]+
                     destination[channel]*channels[3];
        expected[channel]=(sum+127u)/255u;
        if(expected[channel]>255u) expected[channel]=255u;
      }
      uint32_t wanted=(expected[3]<<24)|(expected[0]<<16)|
                       (expected[1]<<8)|expected[2];
      gml_render_sprite_cache_free(&render.spr[sprite]);
      gml_render_sprite_cache_free(&render.spr[0]);
      for(unsigned channel=0;channel<4u;channel++) rgba[channel]=(uint8_t)channels[channel];
      memcpy(render.atlas[0].px,rgba,4);
      if(!gml_surface_write_rgba8(&render,source,rgba,4)) return 0;
      for(int path=0;path<7;path++){
        if(!gml_surface_set_target(&render,target)) return 0;
        for(int pixel=0;pixel<16;pixel++) render.fb[pixel]=0xFF204060u;
        render.interp=0;
        query(&vm,setters[setter],(GmlVal[]){vreal(3),vreal(5)},2,cached,&ok);
        if(path==0){
          query(&vm,"draw_set_color",(GmlVal[]){vreal(colours[colour])},1,cached,&ok);
          query(&vm,"draw_set_alpha",(GmlVal[]){vreal(channels[3]/255.0)},1,cached,&ok);
          query(&vm,"draw_rectangle",(GmlVal[]){vreal(0),vreal(0),vreal(3),vreal(3),vreal(0)},5,0,&ok);
        }else if(path==1){
          gml_draw_sprite_ext(&render,sprite,0,0,0,4,4,0,0xFFFFFFu,1);
        }else if(path==2){
          gml_draw_sprite_ext(&render,sprite,0,4,0,-4,4,0,0xFFFFFFu,1);
        }else if(path==3){
          gml_draw_surface_stretched(&render,source,0,0,4,4,0xFFFFFFu,1);
        }else if(path==4){
          gml_draw_surface_ext(&render,source,0,0,4,4,0,0xFFFFFFu,1);
        }else{
          gml_draw_sprite_ext(&render,0,0,path==6?4:0,0,path==6?-4:4,4,0,0xFFFFFFu,1);
        }
        gml_render_prepare_draw(&render);
        if(render.fb[5]!=wanted){
          fprintf(stderr,"source-colour/source-alpha: cached=%d setter=%s path=%d alpha=%u colour=%06x actual=%08x expected=%08x\n",
                  cached,setters[setter],path,channels[3],colours[colour],render.fb[5],wanted);
          ok=0;
        }
        gml_surface_reset_target(&render);
      }
    }
    vm.render=NULL;vm.win=NULL;
    gml_vm_free(&vm);gml_render_free(&render);
  }
  GmlWin win={.bytecode=15};
  GmlRender render;
  GmlVM vm={.win=&win};
  if(gml_render_init(&render,&win)) return 0;
  vm.render=&render;
  query(&vm,setters[0],(GmlVal[]){vreal(3),vreal(5)},2,0,&ok);
  query(&vm,"gpu_push_state",NULL,0,0,&ok);
  size_t size=gml_render_state_size(&render,-1),written=0,used=0,repeated=0;
  unsigned char *before=malloc(size?size:1),*after=malloc(size?size:1);
  if(!size || !before || !after ||
     !gml_render_state_save(&render,-1,before,size,&written) || written!=size){
    ok=0;
  }else{
    query(&vm,"gpu_pop_state",NULL,0,0,&ok);
    query(&vm,setters[0],(GmlVal[]){vreal(5),vreal(6)},2,0,&ok);
    int restored=gml_render_state_load(&render,before,size,&used);
    ok &= restored && used==size && render.gpu_state_sp==1 &&
      gml_render_state_save(&render,-1,after,size,&repeated) && repeated==size &&
      !memcmp(before,after,size);
    if(restored){
      query(&vm,"gpu_pop_state",NULL,0,0,&ok);
      ok &= render.blendmode==GML_RENDER_BLEND_SOURCE_COLOUR_SOURCE_ALPHA &&
            render.gpu_state_sp==0;
    }
  }
  free(before);free(after);
  vm.render=NULL;vm.win=NULL;
  gml_vm_free(&vm);gml_render_free(&render);
  if(!ok) fprintf(stderr,"source-colour/source-alpha arithmetic or stacked state contract failed\n");
  return ok;
}

static int mirrored_source_over_target_policy_case(void){
  static const unsigned alphas[]={1u,64u,128u,254u};
  static const uint32_t colours[]={0xFFFFFFu,0x5B93CBu};
  int ok=1;
  for(int modern=0;modern<2;modern++) for(int application=0;application<2;application++)
    for(unsigned colour=0;colour<2u;colour++) for(unsigned alpha=0;alpha<4u;alpha++){
      GmlWin win={.bytecode=modern?17:15};
      GmlRender render;
      uint32_t pixels[16],reference=0;
      if(gml_render_init(&render,&win)) return 0;
      render.atlas=calloc(1,sizeof *render.atlas);
      render.tpag=calloc(1,sizeof *render.tpag);
      render.spr=calloc(1,sizeof *render.spr);
      render.n_atlas=render.n_tpag=render.n_spr=render.spr_cap=render.base_n_spr=1;
      if(!render.atlas || !render.tpag || !render.spr) return 0;
      render.atlas[0].px=malloc(4);
      render.spr[0].frame=malloc(sizeof(int));
      if(!render.atlas[0].px || !render.spr[0].frame) return 0;
      render.atlas[0].w=render.atlas[0].h=1;
      render.atlas[0].px[0]=(uint8_t)(colours[colour]>>16);
      render.atlas[0].px[1]=(uint8_t)(colours[colour]>>8);
      render.atlas[0].px[2]=(uint8_t)colours[colour];
      render.atlas[0].px[3]=(uint8_t)alphas[alpha];
      render.tpag[0]=(GmlTpag){.sw=1,.sh=1,.tw=1,.th=1,.bw=1,.bh=1,.atlas=0};
      render.spr[0].frame[0]=0;
      render.spr[0].w=render.spr[0].h=render.spr[0].n_frames=1;
      if(application){
        render.app_surface=pixels;render.app_w=render.app_h=4;
        render.app_surface_opaque=1;
      }
      gml_render_begin(&render,pixels,4,4,0,0);
      for(int mirror=0;mirror<4;mirror++){
        for(int pixel=0;pixel<16;pixel++) pixels[pixel]=0xFF15161Eu;
        int flip_x=mirror&1,flip_y=(mirror>>1)&1;
        gml_draw_sprite_ext(&render,0,0,flip_x,flip_y,
                            flip_x?-1:1,flip_y?-1:1,0,0xFFFFFFu,1);
        gml_render_prepare_draw(&render);
        if(!mirror) reference=pixels[0];
        if(pixels[0]!=reference){
          fprintf(stderr,"mirrored source-over policy: modern=%d application=%d colour=%06x alpha=%u mirror=%d actual=%08x ordinary=%08x\n",
                  modern,application,colours[colour],alphas[alpha],mirror,pixels[0],reference);
          ok=0;
        }
        if(!modern && !application && !colour && !alpha &&
           (pixels[0]&0xFFFFFFu)!=0x15161Eu){
          fprintf(stderr,"first-generation off-target truncation changed: %08x\n",pixels[0]);
          ok=0;
        }
      }
      render.app_surface=NULL;
      gml_render_free(&render);
    }
  return ok;
}

static int color_query_roundtrip_case(void){
  const char *setters[]={"draw_set_color","draw_set_colour"};
  const char *getters[]={"draw_get_color","draw_get_colour"};
  const uint32_t colors[]={0u,0xFFFFFFu,0x0000FFu,0xFF0000u,0x123456u,0x0A7B3Cu};
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlRender render={.alpha=0.375}; GmlVM vm={.render=&render};
    for(size_t setter=0;setter<sizeof setters/sizeof setters[0];setter++)
      for(size_t color=0;color<sizeof colors/sizeof colors[0];color++){
        GmlVal value=vreal(colors[color]);
        query(&vm,setters[setter],&value,1,cached,&ok);
        for(size_t getter=0;getter<sizeof getters/sizeof getters[0];getter++){
          GmlVal observed=query(&vm,getters[getter],NULL,0,cached,&ok);
          if(!real_is(observed,colors[color])){
            fprintf(stderr,"draw colour roundtrip: cached=%d set=%s get=%s expected=%06x got=%.0f\n",
                    cached,setters[setter],getters[getter],colors[color],observed.d);
            ok=0;
          }
          ok &= render.color==colors[color] && render.alpha==0.375;
        }
      }
    vm.render=NULL; gml_vm_free(&vm);
  }
  return ok;
}

static int camera_capacity_case(void){
  int ok=1;
  for(int modern=0;modern<2;modern++) for(int cached=0;cached<2;cached++){
    AnygmCompatibilityProfile profile={0};
    profile.has_modern_layer_semantics=modern;
    GmlWin win={.compatibility=&profile};
    GmlVM vm={.win=&win};
    int first=modern?GML_ROOM_CAMERA_COUNT:0;
    GmlVal last=vreal(-1);
    for(int index=0;index<160;index++){
      GmlVal args[]={vreal(index*3),vreal(index*2),vreal(384),vreal(216),vreal(0)};
      last=query(&vm,"camera_create_view",args,5,cached,&ok);
      if(!real_is(last,first+index)){
        fprintf(stderr,"camera capacity: modern=%d cached=%d index=%d handle=%.0f\n",
                modern,cached,index,last.t==V_REAL?last.d:-2.0);
        ok=0; break;
      }
    }
    if(ok){
      GmlVal binding[]={vreal(0),last};
      query(&vm,"view_set_camera",binding,2,cached,&ok);
      GmlVal view=vreal(0);
      ok &= real_is(query(&vm,"view_get_camera",&view,1,cached,&ok),last.d);
      ok &= real_is(query(&vm,"camera_get_view_x",&last,1,cached,&ok),477);
      /* The 64-bit configuration selector must not alias higher runtime handles. */
      GmlVal high=vreal(127),low=vreal(63);
      double before=query(&vm,"camera_get_view_x",&high,1,cached,&ok).d;
      ok &= gml_camera_override_mask(&vm,UINT64_C(1)<<63,0,700)==1;
      ok &= real_is(query(&vm,"camera_get_view_x",&low,1,cached,&ok),700);
      ok &= real_is(query(&vm,"camera_get_view_x",&high,1,cached,&ok),before);
      for(int index=first+160;index<GML_CAMERA_LIMIT;index++)
        ok &= real_is(query(&vm,"camera_create",NULL,0,cached,&ok),index);
      ok &= real_is(query(&vm,"camera_create",NULL,0,cached,&ok),-1);
      query(&vm,"camera_destroy",&high,1,cached,&ok);
      GmlVal reused=query(&vm,"camera_create",NULL,0,cached,&ok);
      ok &= real_is(reused,127);
      ok &= real_is(query(&vm,"camera_get_view_x",&reused,1,cached,&ok),0);
    }
    vm.win=NULL;
    gml_vm_free(&vm);
  }
  return ok;
}

static GmlVal vertex_copy_format(GmlVM *vm){
  gml_builtin_call(vm,"vertex_format_begin",NULL,0);
  gml_builtin_call(vm,"vertex_format_add_position",NULL,0);
  gml_builtin_call(vm,"vertex_format_add_colour",NULL,0);
  GmlVal custom[]={vreal(4),vreal(5)};
  gml_builtin_call(vm,"vertex_format_add_custom",custom,2);
  return gml_builtin_call(vm,"vertex_format_end",NULL,0);
}
static void vertex_copy_word(unsigned char *data,uint32_t value){
  for(int i=0;i<4;i++) data[i]=(unsigned char)(value>>(8*i));
}
static GmlVal vertex_copy_source(GmlVM *vm,unsigned char bytes[64]){
  for(int i=0;i<64;i++) bytes[i]=(unsigned char)(0xa0+i);
  vertex_copy_word(bytes,0x3fa00000u); vertex_copy_word(bytes+4,0x40200000u);
  vertex_copy_word(bytes+8,0x80563412u); vertex_copy_word(bytes+12,0x7fc12345u);
  vertex_copy_word(bytes+16,0x80000000u);
  vertex_copy_word(bytes+28,0x40c00000u); vertex_copy_word(bytes+32,0x40e00000u);
  vertex_copy_word(bytes+36,0xff654321u);
  GmlVal create[]={vreal(64),vreal(0),vreal(1)};
  GmlVal source=gml_builtin_call(vm,"buffer_create",create,3);
  for(int i=0;i<64;i++){
    GmlVal poke[]={source,vreal(i),vreal(1),vreal(bytes[i])};
    gml_builtin_call(vm,"buffer_poke",poke,4);
  }
  GmlVal seek[]={source,vreal(0),vreal(11)};
  gml_builtin_call(vm,"buffer_seek",seek,3);
  return source;
}
static int vertex_buffer_transfers_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; unsigned char bytes[64];
    GmlVal format=vertex_copy_format(&vm),source=vertex_copy_source(&vm,bytes);
    GmlVal create[]={source,format,vreal(28),vreal(-1)};
    GmlVal whole=query(&vm,"vertex_create_buffer_from_buffer",create,2,cached,&ok);
    GmlVal part=query(&vm,"vertex_create_buffer_from_buffer_ext",create,4,cached,&ok);
    if(whole.d<0 || part.d<0){ gml_vm_free(&vm); return 0; }
    GmlVertexBuffer *all=&vm.software3d->vertex_buffer[(int)whole.d];
    GmlVertexBuffer *one=&vm.software3d->vertex_buffer[(int)part.d];
    ok &= all->vertex_n==2 && all->data_size==56 && !memcmp(all->data,bytes,56);
    ok &= one->vertex_n==1 && one->data_size==28 && !memcmp(one->data,bytes+28,28);
    ok &= all->vertex[0].x==1.25 && all->vertex[0].y==2.5 && all->vertex[0].r==0x12;
    ok &= all->vertex[0].alpha==128.0/255 && one->vertex[0].x==6 && one->vertex[0].y==7;
    create[2]=vreal(-4); create[3]=vreal(10000);
    GmlVal clipped=query(&vm,"vertex_create_buffer_from_buffer_ext",create,4,cached,&ok);
    ok &= clipped.d>=0 && real_is(gml_builtin_call(&vm,"vertex_get_buffer_size",&clipped,1),56);
    GmlVal destination=gml_builtin_call(&vm,"vertex_create_buffer",NULL,0);
    GmlVal copy[]={destination,vreal(1),whole,vreal(0),vreal(99)};
    ok &= query(&vm,"vertex_update_buffer_from_vertex",copy,5,cached,&ok).t==V_UNDEF;
    GmlVertexBuffer *target=&vm.software3d->vertex_buffer[(int)destination.d];
    if(target->data_size<84){ gml_vm_free(&vm); return 0; }
    ok &= target->format==(int)format.d && target->vertex_n==3 && target->data_size==84;
    for(int i=0;i<28;i++) ok &= target->data[i]==0;
    ok &= !memcmp(target->data+28,bytes,56) && target->vertex[0].alpha==0;
    GmlVal update[]={whole,vreal(8),source,vreal(36),vreal(4)};
    query(&vm,"vertex_update_buffer_from_buffer",update,5,cached,&ok);
    ok &= !memcmp(all->data+8,bytes+36,4) && !memcmp(all->data+12,bytes+12,16);
    ok &= all->vertex[0].x==1.25 && all->vertex[0].r==0x21 && all->vertex[0].alpha==1;
    update[0]=destination; update[1]=vreal(84); update[3]=vreal(0);
    query(&vm,"vertex_update_buffer_from_buffer",update,5,cached,&ok);
    ok &= target->data_size==88 && target->vertex_n==3;
    update[1]=vreal(88); update[3]=vreal(4); update[4]=vreal(24);
    query(&vm,"vertex_update_buffer_from_buffer",update,5,cached,&ok);
    ok &= target->data_size==112 && target->vertex_n==4 && !memcmp(target->data+84,bytes,28);
    ok &= target->vertex[3].x==1.25 && target->vertex[3].alpha==128.0/255;
    GmlVal seek=gml_builtin_call(&vm,"buffer_tell",&source,1); ok &= real_is(seek,11);
    unsigned char before[112]; memcpy(before,target->data,sizeof before);
    const double invalid[]={-1,NAN,INFINITY,2147483648.0};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++){
      update[1]=vreal(invalid[i]);
      query(&vm,"vertex_update_buffer_from_buffer",update,5,cached,&ok);
      ok &= target->data_size==112 && !memcmp(target->data,before,sizeof before);
      create[0]=vreal(invalid[i]);
      ok &= real_is(query(&vm,"vertex_create_buffer_from_buffer",create,2,cached,&ok),-1);
    }
    gml_builtin_call(&vm,"vertex_freeze",&destination,1);
    update[1]=vreal(0); query(&vm,"vertex_update_buffer_from_buffer",update,3,cached,&ok);
    ok &= !memcmp(target->data,before,sizeof before);
    gml_builtin_call(&vm,"vertex_freeze",&whole,1);
    copy[0]=part; copy[1]=vreal(0);
    query(&vm,"vertex_update_buffer_from_vertex",copy,3,cached,&ok);
    ok &= one->data_size==28 && !memcmp(one->data,bytes+28,28);
    gml_vm_free(&vm);
  }
  if(ok) puts("vertex transfers: copied bytes, custom payloads, ranges, growth, gaps, startup and frozen resources passed");
  return ok;
}
static int vertex_buffer_state_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlWin win={0}; GmlVM vm={.win=&win,.room_index=-1,.pending_room=-1};
    vm.particles=gml_particle_state_create(&vm); gml_vm_software3d_reset(&vm);
    unsigned char bytes[64];
    GmlVal format=vertex_copy_format(&vm),source=vertex_copy_source(&vm,bytes);
    GmlVal create[]={source,format};
    GmlVal whole=query(&vm,"vertex_create_buffer_from_buffer",create,2,cached,&ok);
    if(whole.t!=V_REAL || whole.d<0){ gml_vm_free(&vm); return 0; }
    GmlVal pending=gml_builtin_call(&vm,"vertex_create_buffer",NULL,0);
    GmlVal begin[]={pending,format}; gml_builtin_call(&vm,"vertex_begin",begin,2);
    GmlVal position[]={pending,vreal(17.25),vreal(19.5)};
    gml_builtin_call(&vm,"vertex_position",position,3);
    gml_builtin_call(&vm,"vertex_freeze",&whole,1);
    gml_builtin_call(&vm,"vertex_format_begin",NULL,0);
    gml_builtin_call(&vm,"vertex_format_add_position",NULL,0);
    size_t resource_size=gml_software3d_resources_state_size(vm.software3d);
    unsigned char *resource=malloc(resource_size),*check=malloc(resource_size);
    if(!resource || !check){ free(resource); free(check); gml_vm_free(&vm); return 0; }
    ok &= gml_software3d_resources_state_save(vm.software3d,resource,resource_size);
    resource[4]=1;
    ok &= !gml_software3d_resources_state_load(vm.software3d,resource,resource_size);
    resource[4]=2;
    ok &= gml_software3d_resources_state_save(vm.software3d,check,resource_size);
    ok &= !memcmp(resource,check,resource_size);
    ok &= !gml_software3d_resources_state_load(vm.software3d,resource,resource_size-1);
    free(resource); free(check);
    size_t capacity=gml_vm_state_size(&vm),written=0,used=0,again=0;
    unsigned char *state=malloc(capacity),*after=malloc(capacity);
    int saved=state && after && gml_vm_state_save(&vm,state,capacity,&written);
    if(!saved){ free(state); free(after); gml_vm_free(&vm); return 0; }
    gml_builtin_call(&vm,"vertex_delete_buffer",&whole,1);
    gml_builtin_call(&vm,"vertex_delete_buffer",&pending,1);
    gml_builtin_call(&vm,"vertex_format_delete",&format,1);
    if(!gml_vm_state_load(&vm,state,written,&used)){
      free(state); free(after); gml_vm_free(&vm); return 0;
    }
    ok &= used==written;
    ok &= gml_vm_state_save(&vm,after,capacity,&again) && again==written && !memcmp(state,after,written);
    free(state); free(after);
    GmlVertexBuffer *restored=&vm.software3d->vertex_buffer[(int)whole.d];
    ok &= restored->used && restored->frozen && restored->vertex_n==2 && !memcmp(restored->data,bytes,56);
    GmlVertexBuffer *partial=&vm.software3d->vertex_buffer[(int)pending.d];
    ok &= partial->attr_index==1 && partial->partial.x==17.25 && partial->partial.y==19.5;
    gml_builtin_call(&vm,"vertex_format_add_colour",NULL,0);
    GmlVal built=gml_builtin_call(&vm,"vertex_format_end",NULL,0);
    int stride=0; ok &= gml_software3d_vertex_format_info(vm.software3d,(int)built.d,&stride,NULL) && stride==12;
    GmlVal colour[]={pending,vreal(0x563412),vreal(0.5)};
    gml_builtin_call(&vm,"vertex_colour",colour,3);
    GmlVal custom[]={pending,vreal(1),vreal(2),vreal(3),vreal(4)};
    gml_builtin_call(&vm,"vertex_float4",custom,5);
    ok &= partial->vertex_n==1 && partial->vertex[0].x==17.25 && partial->vertex[0].alpha==0.5;
    gml_vm_free(&vm);
  }
  if(ok) puts("vertex state: exact bytes, formats, frozen resources, partial builders and transactional rejection passed");
  return ok;
}

static int blend_array_is(GmlVal value,const int *factors,int count){
  if(value.t!=V_ARR || gml_val_array_length(value)!=count)return 0;
  for(int i=0;i<count;i++)if(!real_is(gml_arr_get(value,i),factors[i]))return 0;
  return 1;
}

static int extended_blend_queries_case(void){
  const int normal[]={5,6,5,6},custom[]={2,1,1,2};
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlWin win={.bytecode=17};GmlRender render;GmlVM vm={.win=&win};
    if(gml_render_init(&render,&win))return 0;
    vm.render=&render;
    GmlVal retained=query(&vm,"gpu_get_blendmode_ext",NULL,0,cached,&ok);
    GmlVal first=query(&vm,"gpu_get_blendmode_ext_sepalpha",NULL,0,cached,&ok);
    ok &= blend_array_is(retained,normal,2) && blend_array_is(first,normal,4);
    if(first.t==V_ARR)for(int i=0;i<4;i++)gml_arr_set(first,i,vreal(custom[i]));
    query(&vm,"gpu_set_blendmode_ext_sepalpha",&first,1,cached,&ok);
    GmlVal current=query(&vm,"gpu_get_blendmode_ext_sepalpha",NULL,0,cached,&ok);
    ok &= blend_array_is(current,custom,4) && blend_array_is(retained,normal,2);
    query(&vm,"gpu_push_state",NULL,0,0,&ok);
    query(&vm,"gpu_set_blendmode",(GmlVal[]){vreal(1)},1,cached,&ok);
    query(&vm,"gpu_pop_state",NULL,0,0,&ok);
    GmlVal popped=query(&vm,"gpu_get_blendmode_ext_sepalpha",NULL,0,cached,&ok);
    ok &= blend_array_is(popped,custom,4);
    int target=gml_surface_create(&render,2,2);
    ok &= target>0 && gml_surface_set_target(&render,target);
    query(&vm,"draw_clear_alpha",(GmlVal[]){vreal(0x604020),vreal(64.0/255.0)},2,0,&ok);
    query(&vm,"draw_set_colour",(GmlVal[]){vreal(0x305070)},1,0,&ok);
    query(&vm,"draw_set_alpha",(GmlVal[]){vreal(128.0/255.0)},1,0,&ok);
    query(&vm,"draw_rectangle",(GmlVal[]){vreal(0),vreal(0),vreal(1),vreal(1),vreal(0)},5,0,&ok);
    gml_render_prepare_draw(&render);
    /* RGB replaces the source, alpha retains the destination: independent factors. */
    ok &= render.fb[0]==UINT32_C(0x40705030);
    gml_surface_reset_target(&render);
    size_t size=gml_render_state_size(&render,-1),written=0,used=0;
    uint8_t *state=malloc(size),*again=malloc(size);
    ok &= state && again && gml_render_state_save(&render,-1,state,size,&written) && written==size;
    query(&vm,"gpu_set_blendmode",(GmlVal[]){vreal(0)},1,cached,&ok);
    ok &= state && gml_render_state_load(&render,state,size,&used) && used==size;
    GmlVal restored=query(&vm,"gpu_get_blendmode_ext_sepalpha",NULL,0,cached,&ok);
    ok &= blend_array_is(restored,custom,4);
    ok &= again && gml_render_state_save(&render,-1,again,size,&written) && written==size && !memcmp(state,again,size);
    free(state);free(again);
    GmlVal values[]={retained,first,current,popped,restored};gml_values_release_owned(values,5);
    vm.render=NULL;vm.win=NULL;gml_vm_free(&vm);gml_render_free(&render);
  }
  return ok;
}

static int extended_blend_factor_matrix_case(void){
  const int preserved[][2]={{5,6},{5,2},{1,4},{9,1},{5,4},{3,2},{2,1},
    {8,7},{10,1},{9,3},{9,9},{7,9},{3,5}};
  const unsigned alphas[]={0,128,255};
  GmlWin win={.bytecode=17};GmlRender render;GmlVM vm={.win=&win};int ok=1;
  if(gml_render_init(&render,&win))return 0;
  vm.render=&render;
  int target=gml_surface_create(&render,1,1);
  if(target<=0 || !gml_surface_set_target(&render,target)){gml_render_free(&render);return 0;}
  for(int source=1;source<=11 && ok;source++)for(int dest=1;dest<=11 && ok;dest++)
  for(int source_alpha=1;source_alpha<=11 && ok;source_alpha++)
  for(int dest_alpha=1;dest_alpha<=11 && ok;dest_alpha++){
    int factors[]={source,dest,source_alpha,dest_alpha},legacy=0;
    for(unsigned i=0;i<sizeof preserved/sizeof preserved[0];i++)
      legacy|=source==source_alpha && dest==dest_alpha &&
        source==preserved[i][0] && dest==preserved[i][1];
    GmlVal args[]={vreal(source),vreal(dest),vreal(source_alpha),vreal(dest_alpha)};
    query(&vm,"gpu_set_blendmode_ext_sepalpha",args,4,1,&ok);
    GmlVal value=query(&vm,"gpu_get_blendmode_ext_sepalpha",NULL,0,1,&ok);
    ok &= blend_array_is(value,factors,4);gml_values_release_owned(&value,1);
    /* Existing preset rounding remains protected by its original controls. */
    if(legacy)continue;
    for(unsigned a=0;a<sizeof alphas/sizeof alphas[0] && ok;a++){
      const unsigned sc[]={112,80,48,alphas[a]},dc[]={32,64,96,64};
      unsigned result[4];
      for(unsigned c=0;c<4u;c++){
        long double sf[]={0,0,1,sc[c]/255.0L,1-sc[c]/255.0L,sc[3]/255.0L,
          1-sc[3]/255.0L,dc[3]/255.0L,1-dc[3]/255.0L,dc[c]/255.0L,
          1-dc[c]/255.0L,c==3u?1:fminl(sc[3],255u-dc[3])/255.0L};
        long double channel=sc[c]*sf[factors[c==3u?2:0]]+dc[c]*sf[factors[c==3u?3:1]];
        result[c]=(unsigned)llroundl(fminl(channel,255));
      }
      uint32_t expected=(result[3]<<24)|(result[0]<<16)|(result[1]<<8)|result[2];
      render.fb[0]=0x40204060u;render.color=0x305070u;render.alpha=alphas[a]/255.0;
      gml_builtin_call(&vm,"draw_point",(GmlVal[]){vreal(0),vreal(0)},2);
      if(render.fb[0]!=expected){
        fprintf(stderr,"extended factor matrix: %d,%d,%d,%d alpha=%u got=%08x expected=%08x\n",
          source,dest,source_alpha,dest_alpha,alphas[a],render.fb[0],expected);ok=0;
      }
    }
  }
  gml_surface_reset_target(&render);vm.render=NULL;vm.win=NULL;gml_vm_free(&vm);gml_render_free(&render);
  return ok;
}

int main(int argc,char **argv){
  const char *filter=NULL;
  if(argc==3 && !strcmp(argv[1],"--case")) filter=argv[2];
  else if(argc!=1) return EXIT_FAILURE;
  const AnygmTestCase cases[]={{"motion_grid_edits",motion_grid_edits_case},
    {"motion_grid_drawing",motion_grid_drawing_case},
    {"surface_pixel_alpha",surface_pixel_alpha_case},
    {"buffer_surface_transfer",buffer_surface_transfer_case},
    {"vertex_format_information",vertex_format_information_case},
    {"vertex_resource_existence",vertex_resource_existence_case},
    {"vertex_buffer_transfers",vertex_buffer_transfers_case},
    {"vertex_buffer_state",vertex_buffer_state_case},
    {"display_frequency",display_frequency_case},
    {"explicit_default_speed",explicit_default_speed_case},{"circle_precision",circle_case},
    {"application_surface_lifetime",application_surface_lifetime_case},{"shader_asset_names",shader_case},
    {"all_live_layers_at_depth",layers_case},{"forced_layer_depth",forced_layer_depth_case},
    {"layer_instance_elements",layer_instance_elements_case},
    {"retired_alpha_controls",alpha_case},
    {"integer_layer_depth",layer_integer_depth_case},{"alpha_reference_bounds",alpha_reference_bounds_case},
    {"background_source_region",background_part_case},{"background_name_alias",background_names_case},
    {"authored_font_styles",font_styles_case},{"runtime_font_styles",runtime_font_styles_case},
    {"inverse_destination_colour",inverse_destination_colour_case},
    {"source_colour_source_alpha",source_colour_source_alpha_case},
    {"extended_blend_queries",extended_blend_queries_case},
    {"extended_blend_factor_matrix",extended_blend_factor_matrix_case},
    {"mirrored_source_over_target_policy",mirrored_source_over_target_policy_case},
    {"color_query_roundtrip",color_query_roundtrip_case},
    {"camera_capacity",camera_capacity_case}};
  const AnygmTestGroup group={"draw_queries",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result;
  anygm_test_run_groups(&group,1,filter,&result);
  printf("draw queries: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
