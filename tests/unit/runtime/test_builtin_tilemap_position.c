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

typedef struct {
  GmlVM vm;
  GmlWin win;
  unsigned char bytes[1024];
  char *strings[3];
  uint32_t offsets[3];
} RoomFixture;
static void word(unsigned char *data,size_t offset,uint32_t value){
  for(int i=0;i<4;i++) data[offset+i]=(unsigned char)(value>>(8*i));
}
static void real32(unsigned char *data,size_t offset,float value){
  uint32_t bits; memcpy(&bits,&value,sizeof bits); word(data,offset,bits);
}
static int room_setup(RoomFixture *f){
  memset(f,0,sizeof *f);
  word(f->bytes,0,2); word(f->bytes,4,32); word(f->bytes,8,160);
  const char *names[]={"map_room","empty_room","map_layer"};
  for(int i=0;i<3;i++){
    f->offsets[i]=700+16*i; f->strings[i]=(char*)f->bytes+f->offsets[i];
    strcpy(f->strings[i],names[i]);
  }
  for(int i=0;i<2;i++){
    size_t room=32+128*i;
    word(f->bytes,room,f->offsets[i]); word(f->bytes,room+8,96);
    word(f->bytes,room+12,96); word(f->bytes,room+16,60);
    word(f->bytes,room+32,UINT32_MAX); word(f->bytes,room+48,640);
  }
  word(f->bytes,32+88,300); word(f->bytes,300,1); word(f->bytes,304,384);
  word(f->bytes,384,f->offsets[2]); word(f->bytes,392,4); word(f->bytes,396,17);
  real32(f->bytes,400,11); real32(f->bytes,404,13); word(f->bytes,416,1);
  word(f->bytes,420,0); word(f->bytes,424,2); word(f->bytes,428,2);
  word(f->bytes,432,1); word(f->bytes,436,2); word(f->bytes,440,3); word(f->bytes,444,4);
  f->win=(GmlWin){.data=f->bytes,.size=sizeof f->bytes,.bytecode=17,.game_speed=60,
    .n_chunks=1,.strs=f->strings,.str_charoff=f->offsets,.n_strs=3};
  memcpy(f->win.chunks[0].name,"ROOM",5); f->win.chunks[0].size=sizeof f->bytes;
  if(gml_vm_init(&f->vm,&f->win,NULL)) return 0;
  gml_room_enter(&f->vm,0);
  if(f->vm.n_tilemaps==1 && f->vm.n_rtl==1) return 1;
  fputs("neutral authored map fixture failed to load\n",stderr);
  gml_vm_free(&f->vm); return 0;
}
static int authored_origin(void){
  RoomFixture f; if(!room_setup(&f)) return 0;
  double x,y; gml_tilemap_effective(&f.vm,&f.vm.tilemaps[0],&x,&y,NULL,NULL);
  int ok=f.vm.tilemaps[0].x==0 && f.vm.tilemaps[0].y==0 && x==11 && y==13;
  if(!ok) fputs("authored layer offsets must not become map-local offsets\n",stderr);
  gml_vm_free(&f.vm); return ok;
}
static int room_without_maps(void){
  RoomFixture f; if(!room_setup(&f)) return 0;
  int id=f.vm.tilemaps[0].id;
  gml_room_enter(&f.vm,1);
  int ok=f.vm.room_index==1 && f.vm.n_tilemaps==0 && !gml_tilemap_find(&f.vm,id);
  if(!ok) fputs("entering a room without maps must discard the previous room's maps\n",stderr);
  gml_vm_free(&f.vm); return ok;
}
static int state_position_and_identity(void){
  RoomFixture f; if(!room_setup(&f)) return 0;
  GmlTileMap *map=&f.vm.tilemaps[0]; int id=map->id, next=f.vm.next_tilemap_id;
  map->x=4; map->y=6; map->tileset=3;
  gml_tilemap_set_cell(map,1,1,9);
  size_t size=gml_vm_state_size(&f.vm),written=0,used=0;
  unsigned char *first=malloc(size?size:1),*second=malloc(size?size:1);
  int saved=first && second && size && gml_vm_state_save(&f.vm,first,size,&written) && written==size;
  int ok=saved;
  if(saved){
    map->x=-5.25; map->y=2.5; map->tileset=5;
    int differs=gml_vm_state_size(&f.vm)==size &&
      gml_vm_state_save(&f.vm,second,size,&written) && memcmp(first,second,size)!=0;
    if(!differs) fputs("map position and tileset changes must affect canonical state bytes\n",stderr);
    ok &= differs;
    for(int pass=0;pass<2;pass++){
      int loaded=gml_vm_state_load(&f.vm,first,size,&used) && used==size;
      map=gml_tilemap_find(&f.vm,id);
      int retained=loaded && map && map->x==4 && map->y==6 && map->tileset==3 &&
        f.vm.next_tilemap_id==next && map->tiles[12]==9;
      if(!retained) fputs("restoration must retain the same map handle, local position, tileset and edited cells\n",stderr);
      ok &= retained;
      int exact=loaded && gml_vm_state_size(&f.vm)==size &&
        gml_vm_state_save(&f.vm,second,size,&written) && written==size && !memcmp(first,second,size);
      if(!exact) fputs("map save/load/save must be byte-exact\n",stderr);
      ok &= exact;
      if(!loaded) break;
    }
  }
  free(first); free(second); gml_vm_free(&f.vm); return ok;
}
int main(int argc,char **argv){
  const char *filter=NULL;
  if(argc==3 && !strcmp(argv[1],"--case")) filter=argv[2];
  else if(argc!=1) return 2;
  static const AnygmTestCase cases[]={
    {"local_getters",local_getters},{"isolated_setters",isolated_setters},
    {"effective_position",effective_position},{"pixel_queries",pixel_queries},
    {"invalid_identity",invalid_identity},{"zero_local_origin",zero_local_origin},
    {"authored_origin",authored_origin},{"room_without_maps",room_without_maps},
    {"state_position_and_identity",state_position_and_identity}
  };
  const AnygmTestGroup group={"tilemap_position",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result={0};
  return anygm_test_run_groups(&group,1,filter,&result)?0:1;
}
