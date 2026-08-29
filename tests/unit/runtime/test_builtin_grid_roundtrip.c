/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Round-trip a synthetic grid through ds_grid_write and ds_grid_read. Clearing
 * every cell before reading rules out a no-op reader, and the string cell exercises the
 * variable-length form alongside fixed-size numeric cells. */
#include "gml_vm.h"

#include <stdio.h>
#include <string.h>

GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);

int main(void){
  GmlVM vm;
  memset(&vm,0,sizeof vm);
  int ok=1;

  GmlVal make[]={vreal(3),vreal(2)};
  GmlVal grid=gml_builtin_call(&vm,"ds_grid_create",make,2);
  if(grid.t!=V_REAL){ fprintf(stderr,"could not create a grid\n"); return 1; }

  /* Reals in most cells and a string in one, so the variable-length entry is covered. */
  for(int x=0;x<3;x++) for(int y=0;y<2;y++){
    GmlVal set[]={grid,vreal(x),vreal(y),vreal(x*10+y)};
    gml_builtin_call(&vm,"ds_grid_set",set,4);
  }
  GmlVal set_text[]={grid,vreal(2),vreal(1),vstr("cake")};
  gml_builtin_call(&vm,"ds_grid_set",set_text,4);

  GmlVal one[]={grid};
  GmlVal written=gml_builtin_call(&vm,"ds_grid_write",one,1);
  if(!(written.t==V_STR && written.s && written.s[0])){
    fprintf(stderr,"ds_grid_write produced nothing\n");
    return 1;
  }

  /* Clear every cell, so a read that does nothing cannot pass by leaving the old values behind. */
  for(int x=0;x<3;x++) for(int y=0;y<2;y++){
    GmlVal set[]={grid,vreal(x),vreal(y),vreal(-1)};
    gml_builtin_call(&vm,"ds_grid_set",set,4);
  }

  GmlVal read_args[]={grid,written};
  GmlVal read_ok=gml_builtin_call(&vm,"ds_grid_read",read_args,2);
  if(!(read_ok.t==V_REAL && read_ok.d!=0.0)){
    fprintf(stderr,"ds_grid_read refused what ds_grid_write produced\n");
    ok=0;
  }

  for(int x=0;x<3;x++) for(int y=0;y<2;y++){
    GmlVal get[]={grid,vreal(x),vreal(y)};
    GmlVal back=gml_builtin_call(&vm,"ds_grid_get",get,3);
    if(x==2 && y==1){
      if(!(back.t==V_STR && back.s && !strcmp(back.s,"cake"))){
        fprintf(stderr,"the string cell did not survive the round trip\n");
        ok=0;
      }
      continue;
    }
    if(!(back.t==V_REAL && back.d==(double)(x*10+y))){
      fprintf(stderr,"cell %d,%d came back as %g\n",x,y,back.t==V_REAL?back.d:-999.0);
      ok=0;
    }
  }

  gml_values_release(&written,1);

  /* The priority queue shares the encoder and the same two value types, and its reader walks the
   * buffer in two passes - every priority first, then every value - so a writer that interleaved
   * them would produce something only it could read. Round-trip it the same way. */
  GmlVal pq=gml_builtin_call(&vm,"ds_priority_create",NULL,0);
  if(pq.t!=V_REAL){ fprintf(stderr,"could not create a priority queue\n"); return 1; }
  GmlVal add_a[]={pq,vstr("low"),vreal(1)};
  GmlVal add_b[]={pq,vreal(42),vreal(9)};
  gml_builtin_call(&vm,"ds_priority_add",add_a,3);
  gml_builtin_call(&vm,"ds_priority_add",add_b,3);

  GmlVal pq_one[]={pq};
  GmlVal pq_written=gml_builtin_call(&vm,"ds_priority_write",pq_one,1);
  if(!(pq_written.t==V_STR && pq_written.s && pq_written.s[0])){
    fprintf(stderr,"ds_priority_write produced nothing\n");
    ok=0;
  } else {
    GmlVal pq_clear[]={pq};
    gml_builtin_call(&vm,"ds_priority_clear",pq_clear,1);
    GmlVal pq_read[]={pq,pq_written};
    gml_builtin_call(&vm,"ds_priority_read",pq_read,2);
    GmlVal size=gml_builtin_call(&vm,"ds_priority_size",pq_one,1);
    if(!(size.t==V_REAL && size.d==2.0)){
      fprintf(stderr,"the priority queue came back holding %g entries\n",
              size.t==V_REAL?size.d:-1.0);
      ok=0;
    }
    /* The highest priority must still be the one that was added with it, so the values did not
     * drift against the priorities across the two passes. */
    GmlVal top=gml_builtin_call(&vm,"ds_priority_find_max",pq_one,1);
    if(!(top.t==V_REAL && top.d==42.0)){
      fprintf(stderr,"the top entry came back as type %d\n",(int)top.t);
      ok=0;
    }
    gml_values_release(&pq_written,1);
  }

  if(ok) printf("ds_grid and ds_priority: written and read back, values against priorities\n");
  return ok?0:1;
}
