/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* test_vm - Run a supplied data file and report globals, rooms and instances.
 * Optional environment controls select code calls, spawning and diagnostic output.
 */
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
static void dump_backgrounds(GmlVM *vm, const char *tag){
  if(!getenv("GML_LOG_BG_GLOBALS")) return;
  printf("  backgrounds after %s:\n", tag);
  for(int i=0;i<8;i++){
    int bg=(int)gml_global_arr(vm,"background_index",i);
    if(bg<0) continue;
    printf("    bg[%d] index=%d pos=(%g,%g) speed=(%g,%g) tile=(%g,%g) fg=%g vis=%g\n",
      i,bg,
      gml_global_arr(vm,"background_x",i), gml_global_arr(vm,"background_y",i),
      gml_global_arr(vm,"background_hspeed",i), gml_global_arr(vm,"background_vspeed",i),
      gml_global_arr(vm,"background_htiled",i), gml_global_arr(vm,"background_vtiled",i),
      gml_global_arr(vm,"background_foreground",i), gml_global_arr(vm,"background_visible",i));
  }
}

static void apply_spawn_set(GmlInstance *in, const char *spec){
  if(!in||!spec||!*spec) return;
  char buf[1024], *save=NULL;
  strncpy(buf,spec,sizeof buf-1); buf[sizeof buf-1]=0;
  for(char *tok=strtok_r(buf,";",&save); tok; tok=strtok_r(NULL,";",&save)){
    char *eq=strchr(tok,'=');
    if(!eq) continue;
    *eq=0;
    char *key=tok, *val=eq+1;
    while(*key==' '||*key=='\t') key++;
    while(*val==' '||*val=='\t') val++;
    if(!*key) continue;
    *gml_varmap_put(&in->vars,strdup(key))=vreal(atof(val));
  }
}

static void maybe_spawn_env_obj(GmlVM *vm){
  const char *spec=getenv("GML_SPAWN_OBJ");
  if(!spec||!*spec) return;
  char buf[512], *save=NULL;
  strncpy(buf,spec,sizeof buf-1); buf[sizeof buf-1]=0;
  char *name=strtok_r(buf,",",&save);
  char *xs=strtok_r(NULL,",",&save);
  char *ys=strtok_r(NULL,",",&save);
  if(!name||!xs||!ys) return;
  int obj=gml_object_index_by_name(vm,name);
  if(obj<0) obj=atoi(name);
  if(obj<0||obj>=vm->n_objects) return;
  GmlInstance *in=gml_instance_create(vm,atof(xs),atof(ys),obj);
  apply_spawn_set(in,getenv("GML_SPAWN_SET"));
  if(in) printf("[spawn] %s obj=%d id=%u pos=(%.1f,%.1f)\n", name,obj,in->id,in->x,in->y);
}

static void print_val(const char *tag, GmlVal v){
  if(v.t==V_STR) printf("%s\"%s\"\n", tag, v.s?v.s:"");
  else if(v.t==V_ARR) printf("%s<array len=%d>\n", tag, gml_val_array_length(v));
  else if(v.t==V_UNDEF) printf("%sundefined\n", tag);
  else printf("%s%g\n", tag, v.d);
}

static void maybe_call_env_code(GmlVM *vm, GmlWin *w){
  const char *spec=getenv("GML_CALL_CODE");
  if(!spec||!*spec) return;
  char buf[2048], *save=NULL;
  strncpy(buf,spec,sizeof buf-1); buf[sizeof buf-1]=0;
  for(char *tok=strtok_r(buf,";",&save); tok; tok=strtok_r(NULL,";",&save)){
    char *arg=strchr(tok,':');
    if(arg) *arg++=0;
    while(*tok==' '||*tok=='\t') tok++;
    if(!*tok) continue;
    int ci=gml_code_index_by_name(w,tok);
    char full[256];
    if(ci<0){
      snprintf(full,sizeof full,"gml_Script_%s",tok);
      ci=gml_code_index_by_name(w,full);
    }
    if(ci<0){
      fprintf(stderr,"[call] code not found: %s\n", tok);
      continue;
    }
    GmlVal argv[1]; int argc=0;
    if(arg){ argv[0]=vstr(arg); argc=1; }
    GmlVal rv=gml_vm_run_code(vm,ci,NULL,NULL,argv,argc);
    char tag[320]; snprintf(tag,sizeof tag,"[call] %s -> ", tok);
    print_val(tag,rv);
  }
}

static void maybe_dump_ds_maps(GmlVM *vm){
  if(!getenv("GML_DUMP_DS")) return;
  for(int i=0;i<64;i++) if(vm->ds_map[i].live){
    GmlDSMap *m=&vm->ds_map[i];
    printf("[ds_map] id=%u len=%d\n", m->id, m->len);
    for(int j=0;j<m->len && j<8;j++){
      printf("  key=");
      print_val("", m->entry[j].key_val);
      printf("    val=");
      print_val("", m->entry[j].val);
    }
  }
}

int main(int argc,char**argv){
  const char *path=argc>1?argv[1]:"data.win";
  GmlWin w; if(gml_win_load(&w,path)){ fprintf(stderr,"load failed\n"); return 1; }
  GmlVM vm; gml_vm_init(&vm,&w);
  printf("# objects=%d rooms=%d room_order=%d\n", vm.n_objects, gml_room_count(&w), w.n_room_order);
  const char *filter=getenv("GML_CODE_FILTER");
  if(filter && *filter){
    printf("# code blocks matching '%s':\n", filter);
    for(int i=0;i<w.n_code;i++)
      if(strstr(w.code[i].name,filter))
        printf("    [%d] %s\n", i, w.code[i].name);
  }

  const char *sr=getenv("GML_START_ROOM");
  gml_vm_goto_room_order(&vm, sr?atoi(sr):0);
  printf("\n[enter order0] room='%s' idx=%d instances=%d pending=%d\n",
         room_name(&vm), vm.room_index, vm.inst_count, vm.pending_room);
  maybe_spawn_env_obj(&vm);
  maybe_call_env_code(&vm,&w);
  maybe_dump_ds_maps(&vm);
  dump_globals(&vm,"room0 Create");
  dump_backgrounds(&vm,"room enter");

  int nf = argc>2?atoi(argv[2]):8;
  for(int f=0;f<nf;f++){
    gml_vm_step(&vm);
    int alive=0; for(int i=0;i<vm.inst_count;i++) if(vm.inst[i].active) alive++;
    if(f<8 || f==nf-1)
      printf("[step %d] room='%s' idx=%d alive=%d pending=%d game_end=%d\n",
             f, room_name(&vm), vm.room_index, alive, vm.pending_room, vm.game_end);
  }
  dump_backgrounds(&vm,"steps");
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
