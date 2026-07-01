/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* test_vm — enter the first room in play order, step bounded frames, and report
 * globals, room transitions and instance state through the bytecode-14 interpreter. */
#include "gml_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gml_input_key(int vk, int edge){ (void)vk;(void)edge; return 0; }  /* no input in test */
int gml_input_gamepad(int button, int edge){ (void)button;(void)edge; return 0; }

static void dump_globals(GmlVM *vm, const char *tag){
  printf("  globals after %s:\n", tag);
  for(int i=0;i<vm->globals.cap;i++){ GmlVarSlot *s=&vm->globals.slots[i]; if(!s->key) continue;
    if(s->val.t==V_REAL) printf("    %-22s = %g\n", s->key, s->val.d);
    else                 printf("    %-22s = \"%s\"\n", s->key, s->val.s?s->val.s:""); }
}
static const char *room_name(GmlVM *vm){
  GmlRoom r; return (gml_room_get(vm->win,vm->room_index,&r)==0)? r.name : "?";
}

int main(int argc,char**argv){
  const char *path=argc>1?argv[1]:"media/data.win";
  GmlWin w; if(gml_win_load(&w,path)){ fprintf(stderr,"load failed\n"); return 1; }
  GmlVM vm; gml_vm_init(&vm,&w);
  printf("# objects=%d rooms=%d room_order=%d\n", vm.n_objects, gml_room_count(&w), w.n_room_order);
  printf("# event and script code blocks:\n");
  for(int i=0;i<w.n_code;i++)
      printf("    [%d] %s\n", i, w.code[i].name);

  const char *sr=getenv("GML_START_ROOM");
  gml_vm_goto_room_order(&vm, sr?atoi(sr):0);
  printf("\n[enter order0] room='%s' idx=%d instances=%d pending=%d\n",
         room_name(&vm), vm.room_index, vm.inst_count, vm.pending_room);
  dump_globals(&vm,"room0 Create");

  int nf = argc>2?atoi(argv[2]):8;
  for(int f=0;f<nf;f++){
    gml_vm_step(&vm);
    int alive=0; for(int i=0;i<vm.inst_count;i++) if(vm.inst[i].active) alive++;
    if(f<8 || f==nf-1)
      printf("[step %d] room='%s' idx=%d alive=%d pending=%d game_end=%d\n",
             f, room_name(&vm), vm.room_index, alive, vm.pending_room, vm.game_end);
  }
  printf("\n## view: xview=%g yview=%g | room '%s':\n",
         gml_global_arr(&vm,"view_xview",0), gml_global_arr(&vm,"view_yview",0), room_name(&vm));
  for(int i=0;i<vm.inst_count;i++){ GmlInstance*in=&vm.inst[i]; if(!in->active||in->marked) continue;
    const char *on = (in->obj>=0&&in->obj<vm.n_objects)? vm.objects[in->obj].name : "?";
    char dn[160]; snprintf(dn,sizeof dn,"gml_Object_%s_Draw_0",on);
    int hasdraw = gml_code_index_by_name(&w,dn)>=0;
    printf("   %-26s spr=%-4d img=%.0f pos=(%.0f,%.0f) depth=%.0f vis=%.0f draw=%d  alarms[%.0f %.0f %.0f %.0f %.0f]\n",
           on,(int)in->sprite_index,in->image_index,in->x,in->y,in->depth,in->visible,hasdraw,
           in->alarm[0],in->alarm[1],in->alarm[2],in->alarm[3],in->alarm[4]);
  }
  gml_vm_free(&vm); gml_win_free(&w);
  return 0;
}
