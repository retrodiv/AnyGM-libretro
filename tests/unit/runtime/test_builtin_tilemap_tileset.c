/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Exercise the tilemap_tileset setter against its getter with a synthetic map.
 * Check the stored value, return value and missing-map sentinel; zero remains a valid
 * tileset index rather than a missing-map result. */
#include "gml_vm.h"

#include <stdio.h>
#include <string.h>

GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);

static int expect(const char *label, GmlVal actual, double want){
  if(actual.t==V_REAL && actual.d==want) return 1;
  fprintf(stderr,"%s: expected %g, got %g\n",label,want,actual.t==V_REAL?actual.d:-999.0);
  return 0;
}

int main(void){
  GmlVM vm;
  GmlTileMap map;
  memset(&vm,0,sizeof vm);
  memset(&map,0,sizeof map);
  map.used = 1;
  map.id = 7;
  map.tileset = 3;
  vm.tilemaps = &map;
  vm.n_tilemaps = 1;
  int ok = 1;

  GmlVal read[] = { vreal(7) };
  ok &= expect("the getter reads the tileset it was given",
               gml_builtin_call(&vm,"tilemap_get_tileset",read,1), 3);

  GmlVal set[] = { vreal(7), vreal(9) };
  ok &= expect("the setter answers the tileset now in force",
               gml_builtin_call(&vm,"tilemap_tileset",set,2), 9);
  ok &= expect("and the getter agrees afterwards",
               gml_builtin_call(&vm,"tilemap_get_tileset",read,1), 9);
  if(map.tileset != 9){
    fprintf(stderr,"the stored field was not updated: %d\n", map.tileset);
    ok = 0;
  }

  /* Zero is a real tileset index, so a missing tilemap must not answer it. */
  GmlVal missing[] = { vreal(999), vreal(4) };
  ok &= expect("a tilemap that does not exist answers -1, not a plausible 0",
               gml_builtin_call(&vm,"tilemap_tileset",missing,2), -1);

  if(ok) printf("tilemap_tileset: set and get agree, and a missing tilemap says so\n");
  return ok ? 0 : 1;
}
