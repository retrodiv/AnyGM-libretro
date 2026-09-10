/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "anygm_test_runner.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  GmlVM vm;
  GmlTileMap maps[2];
  GmlRtLayer layer;
  unsigned char cells[16];
} PositionFixture;

static void setup(PositionFixture *f){
  memset(f,0,sizeof *f);
  f->cells[0]=1; f->cells[4]=2; f->cells[8]=3; f->cells[12]=4;
  f->maps[0]=(GmlTileMap){.used=1,.id=7,.order=2,.x=40,.y=50,
    .visible=1,.depth=17,.tw=2,.th=3,.cols=2,.rows=2,.tiles=f->cells};
  f->maps[1]=f->maps[0]; f->maps[1].id=8; f->maps[1].x=3; f->maps[1].y=5;
  f->layer=(GmlRtLayer){.used=1,.id=70,.order=2,.x=11,.y=13,
    .depth=17,.visible=1,.script_begin=-1,.script_end=-1};
  f->vm=(GmlVM){.tilemaps=f->maps,.n_tilemaps=2,.rtl=&f->layer,.n_rtl=1,
    .room_index=-1,.pending_room=-1};
}
static void cleanup(PositionFixture *f){
  for(int i=0;i<2;i++) free(f->maps[i].owned_tiles);
  f->vm.tilemaps=NULL; f->vm.n_tilemaps=0; f->vm.rtl=NULL; f->vm.n_rtl=0;
  gml_vm_free(&f->vm);
}
static GmlVal call(PositionFixture *f,const char *name,double id,double x,double y,int n){
  GmlVal args[]={vreal(id),vreal(x),vreal(y)};
  return gml_builtin_call(&f->vm,name,args,n);
}
static int number(const char *label,GmlVal actual,double expected){
  if(actual.t==V_REAL && actual.d==expected) return 1;
  fprintf(stderr,"%s: expected %.17g, got type %d / %.17g\n",
    label,expected,actual.t,actual.t==V_REAL?actual.d:0.0);
  return 0;
}
static int local_getters(void){
  PositionFixture f; setup(&f);
  int ok=number("local x excludes parent",call(&f,"tilemap_get_x",7,0,0,1),40);
  ok &= number("local y excludes parent",call(&f,"tilemap_get_y",7,0,0,1),50);
  f.layer.x=31; f.layer.y=-7;
  ok &= number("parent movement preserves local x",call(&f,"tilemap_get_x",7,0,0,1),40);
  ok &= number("parent movement preserves local y",call(&f,"tilemap_get_y",7,0,0,1),50);
  cleanup(&f); return ok;
}
static int isolated_setters(void){
  PositionFixture f,other; setup(&f); setup(&other);
  GmlRtLayer parent=f.layer; GmlTileMap sibling=f.maps[1];
  call(&f,"tilemap_x",7,-5.25,0,2);
  int ok=f.maps[0].x==-5.25 && f.maps[0].y==50;
  call(&f,"tilemap_y",7,2.5,0,2);
  ok &= f.maps[0].x==-5.25 && f.maps[0].y==2.5;
  ok &= !memcmp(&parent,&f.layer,sizeof parent) && !memcmp(&sibling,&f.maps[1],sizeof sibling);
  ok &= other.maps[0].x==40 && other.maps[0].y==50;
  if(!ok) fputs("setters must change only the selected map coordinate\n",stderr);
  cleanup(&other); cleanup(&f); return ok;
}
static int effective_position(void){
  PositionFixture f; setup(&f); double x,y,depth; int visible;
  gml_tilemap_effective(&f.vm,&f.maps[0],&x,&y,&depth,&visible);
  int ok=x==51 && y==63 && depth==17 && visible;
  f.vm.frame=10; f.vm.room_enter_frame=4; f.layer.hs=2; f.layer.vs=-1;
  gml_tilemap_effective(&f.vm,&f.maps[0],&x,&y,NULL,NULL);
  ok &= x==63 && y==57;
  f.layer.touched=1; f.layer.x=31; f.layer.y=-7;
  gml_tilemap_effective(&f.vm,&f.maps[0],&x,&y,NULL,NULL);
  ok &= x==71 && y==43;
  gml_tilemap_effective(&f.vm,&f.maps[1],&x,&y,NULL,NULL);
  ok &= x==34 && y==-2;
  f.vm.n_rtl=0;
  gml_tilemap_effective(&f.vm,&f.maps[0],&x,&y,NULL,NULL);
  ok &= x==40 && y==50;
  if(!ok) fputs("effective position must add local and current parent coordinates exactly once\n",stderr);
  cleanup(&f); return ok;
}
static int pixel_queries(void){
  PositionFixture f; setup(&f);
  int ok=number("first column at world origin",call(&f,"tilemap_get_cell_x_at_pixel",7,51,63,3),0);
  ok &= number("first row at world origin",call(&f,"tilemap_get_cell_y_at_pixel",7,51,63,3),0);
  ok &= number("second column",call(&f,"tilemap_get_cell_x_at_pixel",7,54,67,3),1);
  ok &= number("second row",call(&f,"tilemap_get_cell_y_at_pixel",7,54,67,3),1);
  ok &= number("datum at translated pixel",call(&f,"tilemap_get_at_pixel",7,54,67,3),4);
  GmlVal set[]={vreal(7),vreal(9),vreal(54),vreal(67)};
  gml_builtin_call(&f.vm,"tilemap_set_at_pixel",set,4);
  ok &= number("pixel mutation reaches translated cell",call(&f,"tilemap_get",7,1,1,3),9);
  ok &= number("sibling cell storage is not mutated",call(&f,"tilemap_get",8,1,1,3),4);
  cleanup(&f); return ok;
}
static int invalid_identity(void){
  PositionFixture f; setup(&f); int ok=1;
  const int ids[]={-1,999,70};
  for(size_t i=0;i<sizeof ids/sizeof ids[0];i++){
    call(&f,"tilemap_x",ids[i],1,0,2); call(&f,"tilemap_y",ids[i],2,0,2);
    ok &= number("invalid x getter",call(&f,"tilemap_get_x",ids[i],0,0,1),-1);
    ok &= number("invalid y getter",call(&f,"tilemap_get_y",ids[i],0,0,1),-1);
  }
  ok &= f.maps[0].x==40 && f.maps[0].y==50 && f.layer.x==11 && f.layer.y==13;
  cleanup(&f); return ok;
}
static int zero_local_origin(void){
  PositionFixture f; setup(&f); double x,y;
  f.maps[0].x=f.maps[0].y=0;
  gml_tilemap_effective(&f.vm,&f.maps[0],&x,&y,NULL,NULL);
  int ok=x==11 && y==13;
  f.layer.hs=2; f.layer.vs=-1; f.vm.frame=6;
  gml_tilemap_effective(&f.vm,&f.maps[0],&x,&y,NULL,NULL);
  ok &= x==23 && y==7;
  if(!ok) fputs("zero-local maps must retain existing parent-only placement\n",stderr);
  cleanup(&f); return ok;
}
int main(int argc,char **argv){
  const char *filter=NULL;
  if(argc==3 && !strcmp(argv[1],"--case")) filter=argv[2];
  else if(argc!=1) return 2;
  static const AnygmTestCase cases[]={
    {"local_getters",local_getters},{"isolated_setters",isolated_setters},
    {"effective_position",effective_position},{"pixel_queries",pixel_queries},
    {"invalid_identity",invalid_identity},{"zero_local_origin",zero_local_origin}
  };
  const AnygmTestGroup group={"tilemap_position",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result={0};
  return anygm_test_run_groups(&group,1,filter,&result)?0:1;
}
