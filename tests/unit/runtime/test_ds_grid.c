/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_vm.h"

#include <stdio.h>
#include <string.h>

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

  GmlVal grow_args[]={grid,vreal(6),vreal(5)};
  ok&=expect_real("grow succeeds",call(&vm,"ds_grid_resize",grow_args,3),1);
  GmlVal width_arg[]={grid}, get_origin[]={grid,vreal(0),vreal(0)};
  GmlVal get_middle[]={grid,vreal(2),vreal(1)}, get_new[]={grid,vreal(5),vreal(4)};
  ok&=expect_real("grown width",call(&vm,"ds_grid_width",width_arg,1),6);
  ok&=expect_real("grown height",call(&vm,"ds_grid_height",width_arg,1),5);
  ok&=expect_real("grow preserves origin",call(&vm,"ds_grid_get",get_origin,3),11);
  ok&=expect_real("grow preserves middle",call(&vm,"ds_grid_get",get_middle,3),23);
  ok&=expect_real("grow initializes new cells",call(&vm,"ds_grid_get",get_new,3),0);

  GmlVal shrink_args[]={grid,vreal(3),vreal(2)};
  ok&=expect_real("shrink succeeds",call(&vm,"ds_grid_resize",shrink_args,3),1);
  ok&=expect_real("shrunk width",call(&vm,"ds_grid_width",width_arg,1),3);
  ok&=expect_real("shrunk height",call(&vm,"ds_grid_height",width_arg,1),2);
  ok&=expect_real("shrink preserves overlap",call(&vm,"ds_grid_get",get_middle,3),23);

  GmlVal zero_args[]={grid,vreal(0),vreal(0)};
  ok&=expect_real("zero resize succeeds",call(&vm,"ds_grid_resize",zero_args,3),1);
  ok&=expect_real("zero width",call(&vm,"ds_grid_width",width_arg,1),0);
  ok&=expect_real("zero height",call(&vm,"ds_grid_height",width_arg,1),0);
  GmlVal regrow_args[]={grid,vreal(1),vreal(1)};
  ok&=expect_real("regrow succeeds",call(&vm,"ds_grid_resize",regrow_args,3),1);
  ok&=expect_real("regrow initializes cell",call(&vm,"ds_grid_get",get_origin,3),0);

  GmlVal destroy_args[]={grid};
  call(&vm,"ds_grid_destroy",destroy_args,1);
  ok&=expect_real("destroyed grid",call(&vm,"ds_grid_value_exists",whole_grid,6),0);
  return ok;
}

/* Region aggregates return numeric results and disk variants visit a circle
 * rather than its bounding rectangle. Assert cells where those shapes differ. */
static int grid_region_aggregates(void){
  GmlVM vm; memset(&vm,0,sizeof vm);
  int ok=1;
  GmlVal make[]={vreal(3),vreal(3)};
  GmlVal grid=call(&vm,"ds_grid_create",make,2);
  double cells[3][3]={{1,2,3},{4,5,6},{7,8,9}};
  for(int y=0;y<3;y++) for(int x=0;x<3;x++){
    GmlVal set[]={grid,vreal(x),vreal(y),vreal(cells[y][x])};
    call(&vm,"ds_grid_set",set,4);
  }
  GmlVal whole[]={grid,vreal(0),vreal(0),vreal(2),vreal(2)};
  ok&=expect_real("max over the whole grid",call(&vm,"ds_grid_get_max",whole,5),9);
  ok&=expect_real("min over the whole grid",call(&vm,"ds_grid_get_min",whole,5),1);
  ok&=expect_real("sum over the whole grid",call(&vm,"ds_grid_get_sum",whole,5),45);
  ok&=expect_real("mean over the whole grid",call(&vm,"ds_grid_get_mean",whole,5),5);

  GmlVal corner[]={grid,vreal(0),vreal(0),vreal(1),vreal(1)};
  ok&=expect_real("max over a sub-rectangle",call(&vm,"ds_grid_get_max",corner,5),5);
  ok&=expect_real("sum over a sub-rectangle",call(&vm,"ds_grid_get_sum",corner,5),12);

  /* Reversed bounds name the same rectangle. */
  GmlVal reversed[]={grid,vreal(1),vreal(1),vreal(0),vreal(0)};
  ok&=expect_real("reversed bounds are the same rectangle",
                  call(&vm,"ds_grid_get_sum",reversed,5),12);

  /* Out of range is clamped rather than faulting. */
  GmlVal huge[]={grid,vreal(-5),vreal(-5),vreal(50),vreal(50)};
  ok&=expect_real("bounds outside the grid clamp to it",call(&vm,"ds_grid_get_sum",huge,5),45);

  /* Radius one around the centre visits a plus shape and excludes the corners. */
  GmlVal disk[]={grid,vreal(1),vreal(1),vreal(1)};
  ok&=expect_real("disk sum excludes the corners",call(&vm,"ds_grid_get_disk_sum",disk,4),25);
  ok&=expect_real("disk max",call(&vm,"ds_grid_get_disk_max",disk,4),8);
  ok&=expect_real("disk min",call(&vm,"ds_grid_get_disk_min",disk,4),2);
  ok&=expect_real("disk mean over five cells",call(&vm,"ds_grid_get_disk_mean",disk,4),5);

  GmlVal destroy[]={grid};
  call(&vm,"ds_grid_destroy",destroy,1);
  return ok;
}

/* Check mutated cells, including cells outside a region or disk. */
static int grid_region_mutators(void){
  GmlVM vm; memset(&vm,0,sizeof vm);
  int ok=1;
  GmlVal make[]={vreal(3),vreal(3)};
  GmlVal grid=call(&vm,"ds_grid_create",make,2);
  GmlVal clear[]={grid,vreal(1)};
  call(&vm,"ds_grid_clear",clear,2);

  GmlVal set_region[]={grid,vreal(0),vreal(0),vreal(1),vreal(1),vreal(5)};
  call(&vm,"ds_grid_set_region",set_region,6);
  GmlVal at00[]={grid,vreal(0),vreal(0)}, at22[]={grid,vreal(2),vreal(2)};
  ok&=expect_real("set_region writes inside",call(&vm,"ds_grid_get",at00,3),5);
  ok&=expect_real("set_region leaves outside alone",call(&vm,"ds_grid_get",at22,3),1);

  GmlVal add_region[]={grid,vreal(0),vreal(0),vreal(1),vreal(1),vreal(2)};
  call(&vm,"ds_grid_add_region",add_region,6);
  ok&=expect_real("add_region adds to what was there",call(&vm,"ds_grid_get",at00,3),7);

  GmlVal mul_region[]={grid,vreal(0),vreal(0),vreal(1),vreal(1),vreal(3)};
  call(&vm,"ds_grid_multiply_region",mul_region,6);
  ok&=expect_real("multiply_region multiplies",call(&vm,"ds_grid_get",at00,3),21);

  /* A radius-one disk reaches an edge cell but skips the corner. */
  GmlVal grid2=call(&vm,"ds_grid_create",make,2);
  GmlVal clear2[]={grid2,vreal(0)};
  call(&vm,"ds_grid_clear",clear2,2);
  GmlVal set_disk[]={grid2,vreal(1),vreal(1),vreal(1),vreal(9)};
  call(&vm,"ds_grid_set_disk",set_disk,5);
  GmlVal g2_10[]={grid2,vreal(1),vreal(0)}, g2_00[]={grid2,vreal(0),vreal(0)};
  ok&=expect_real("set_disk reaches the edge cell",call(&vm,"ds_grid_get",g2_10,3),9);
  ok&=expect_real("set_disk skips the corner",call(&vm,"ds_grid_get",g2_00,3),0);

  /* A grid region reads the source and lands at the destination corner. */
  GmlVal grid3=call(&vm,"ds_grid_create",make,2);
  GmlVal clear3[]={grid3,vreal(0)};
  call(&vm,"ds_grid_clear",clear3,2);
  GmlVal seed[]={grid3,vreal(0),vreal(0),vreal(4)};
  call(&vm,"ds_grid_set",seed,4);
  GmlVal copy_region[]={grid2,grid3,vreal(0),vreal(0),vreal(0),vreal(0),vreal(2),vreal(2)};
  call(&vm,"ds_grid_set_grid_region",copy_region,8);
  GmlVal g2_22[]={grid2,vreal(2),vreal(2)};
  ok&=expect_real("set_grid_region lands at the destination corner",
                  call(&vm,"ds_grid_get",g2_22,3),4);

  GmlVal d1[]={grid}, d2[]={grid2}, d3[]={grid3};
  call(&vm,"ds_grid_destroy",d1,1); call(&vm,"ds_grid_destroy",d2,1); call(&vm,"ds_grid_destroy",d3,1);
  return ok;
}

int main(void){
  if(!grid_value_fixtures()) return 1;
  if(!grid_region_aggregates()) return 1;
  if(!grid_region_mutators()) return 1;
  puts("ds_grid value-search, resize, region-aggregate and region-mutator fixtures: ok");
  return 0;
}
