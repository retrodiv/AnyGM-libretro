/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gml_vm.h"

#include <stdio.h>
#include <string.h>

int gml_input_key(int key, int edge){ (void)key; (void)edge; return 0; }
int gml_input_gamepad(int button, int edge){ (void)button; (void)edge; return 0; }
GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);

static GmlVal call(GmlVM *vm, const char *name, GmlVal *args, int count){
  return gml_builtin_call(vm,name,args,count);
}

static int expect_real(const char *label, GmlVal actual, double expected){
  if(actual.t==V_REAL && actual.d==expected) return 1;
  fprintf(stderr,"%s: expected %.17g, got type %d value %.17g\n",
          label,expected,actual.t,actual.d);
  return 0;
}

static int grid_value_fixtures(void){
  GmlVM vm;
  memset(&vm,0,sizeof(vm));

  GmlVal create_args[]={vreal(4),vreal(3)};
  GmlVal created=call(&vm,"ds_grid_create",create_args,2);
  if(created.t!=V_REAL || created.d<0){
    fprintf(stderr,"ds_grid_create failed\n");
    return 0;
  }
  GmlVal grid=created;
  GmlVal set_origin[]={grid,vreal(0),vreal(0),vreal(11)};
  GmlVal set_middle[]={grid,vreal(2),vreal(1),vreal(23)};
  GmlVal set_corner[]={grid,vreal(3),vreal(2),vreal(42)};
  GmlVal set_string[]={grid,vreal(1),vreal(2),vstr("marker")};
  call(&vm,"ds_grid_set",set_origin,4);
  call(&vm,"ds_grid_set",set_middle,4);
  call(&vm,"ds_grid_set",set_corner,4);
  call(&vm,"ds_grid_set",set_string,4);

  GmlVal whole_grid[]={grid,vreal(0),vreal(0),vreal(3),vreal(2),vreal(42)};
  GmlVal excludes_right[]={grid,vreal(0),vreal(0),vreal(2),vreal(2),vreal(42)};
  GmlVal excludes_bottom[]={grid,vreal(0),vreal(0),vreal(3),vreal(1),vreal(42)};
  GmlVal one_cell[]={grid,vreal(2),vreal(1),vreal(2),vreal(1),vreal(23)};
  GmlVal reversed_region[]={grid,vreal(3),vreal(2),vreal(2),vreal(1),vreal(23)};
  GmlVal clipped_region[]={grid,vreal(-5),vreal(-4),vreal(8),vreal(7),vreal(11)};
  GmlVal string_value[]={grid,vreal(0),vreal(2),vreal(2),vreal(2),vstr("marker")};
  GmlVal absent[]={grid,vreal(0),vreal(0),vreal(3),vreal(2),vreal(99)};
  GmlVal invalid[]={vreal(9999),vreal(0),vreal(0),vreal(3),vreal(2),vreal(42)};

  int ok=1;
  ok&=expect_real("inclusive bottom-right",call(&vm,"ds_grid_value_exists",whole_grid,6),1);
  ok&=expect_real("right boundary excluded",call(&vm,"ds_grid_value_exists",excludes_right,6),0);
  ok&=expect_real("bottom boundary excluded",call(&vm,"ds_grid_value_exists",excludes_bottom,6),0);
  ok&=expect_real("single-cell region",call(&vm,"ds_grid_value_exists",one_cell,6),1);
  ok&=expect_real("reversed region",call(&vm,"ds_grid_value_exists",reversed_region,6),1);
  ok&=expect_real("region clipped to grid",call(&vm,"ds_grid_value_exists",clipped_region,6),1);
  ok&=expect_real("string value",call(&vm,"ds_grid_value_exists",string_value,6),1);
  ok&=expect_real("missing value",call(&vm,"ds_grid_value_exists",absent,6),0);
  ok&=expect_real("invalid grid",call(&vm,"ds_grid_value_exists",invalid,6),0);
  ok&=expect_real("value x shares search",call(&vm,"ds_grid_value_x",whole_grid,6),3);
  ok&=expect_real("value y shares search",call(&vm,"ds_grid_value_y",whole_grid,6),2);

  GmlVal destroy_args[]={grid};
  call(&vm,"ds_grid_destroy",destroy_args,1);
  ok&=expect_real("destroyed grid",call(&vm,"ds_grid_value_exists",whole_grid,6),0);
  return ok;
}

int main(void){
  if(!grid_value_fixtures()) return 1;
  puts("ds_grid value search fixtures: ok");
  return 0;
}
