/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* test_vm drives the VM for a supplied data file and observes globals, rooms,
 * instances and optional debug calls. It does not initialize a renderer: vm->render is NULL.
 * Renderer-dependent builtins therefore report no-renderer results, including false shader
 * status and absent surfaces. A route that branches on those values may behave differently
 * in a host with a renderer; this driver is a VM-only probe. */
#include "gml_vm.h"
#include "gml_builtin.h"
#include "stdio_vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


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

/* Dump selected instance variables after each step. GML_DUMP_VARS selects an object
 * by name; GML_DUMP_VARS_ONLY optionally filters its variable names. */
static void dump_instance_vars(GmlVM *vm, int frame){
  const char *want=getenv("GML_DUMP_VARS");
  if(!want||!*want) return;
  const char *only=getenv("GML_DUMP_VARS_ONLY");
  for(int i=0;i<vm->inst_count;i++){
    GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked) continue;
    const char *nm = (in->obj>=0 && in->obj<vm->n_objects && vm->objects[in->obj].name)
                     ? vm->objects[in->obj].name : "?";
    if(strcmp(nm,want)) continue;
    int seen_named=0;
    printf("[vars %d] %s:", frame, nm);
    { const char *g=getenv("GML_DUMP_GLOBALS_ONLY");
      if(g&&*g){
        char buf[512],*save=NULL;
        strncpy(buf,g,sizeof buf-1); buf[sizeof buf-1]=0;
        for(char *k=strtok_r(buf,",",&save); k; k=strtok_r(NULL,",",&save)){
          GmlVal *v=gml_varmap_get(&vm->globals,k);
          if(!v) printf(" global.%s=<absent>",k);
          else if(v->t==V_REAL) printf(" global.%s=%g",k,v->d);
          else printf(" global.%s=<t%d>",k,(int)v->t);
        }
      } }
    for(int s=0;s<in->vars.cap;s++){
      GmlVarSlot *slot=&in->vars.slots[s];
      if(!slot->key) continue;
      if(only){
        char pattern[256];
        snprintf(pattern,sizeof pattern,",%s,",slot->key);
        char list[512];
        snprintf(list,sizeof list,",%s,",only);
        if(!strstr(list,pattern)) continue;
        seen_named++;
      }
      if(slot->val.t==V_REAL)      printf(" %s=%g",slot->key,slot->val.d);
      else if(slot->val.t==V_STR)  printf(" %s=\"%s\"",slot->key,slot->val.s?slot->val.s:"");
      else                         printf(" %s=<t%d>",slot->key,(int)slot->val.t);
    }
    /* Mark a filtered request when no selected instance variable is present. */
    if(only && !seen_named) printf(" (none of the named instance variables exist)");
    printf("\n");
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
  for(int id=1;id<=64;id++){
    GmlVal map=vreal(id);
    GmlVal size=gml_builtin_call(vm,"ds_map_size",&map,1);
    if(size.t!=V_REAL || size.d<=0) continue;
    printf("[ds_map] id=%d len=%d\n",id,(int)size.d);
    GmlVal key=gml_builtin_call(vm,"ds_map_find_first",&map,1);
    for(int j=0;j<(int)size.d && j<8 && key.t!=V_UNDEF;j++){
      GmlVal find_args[2]={map,key};
      GmlVal value=gml_builtin_call(vm,"ds_map_find_value",find_args,2);
      printf("  key=");
      print_val("",key);
      printf("    val=");
      print_val("",value);
      GmlVal next_args[2]={map,key};
      GmlVal next=gml_builtin_call(vm,"ds_map_find_next",next_args,2);
      if(key.t==V_STR && key.d!=0) free((void *)key.s);
      if(value.t==V_STR && value.d!=0) free((void *)value.s);
      key=next;
    }
    if(key.t==V_STR && key.d!=0) free((void *)key.s);
  }
}

int main(int argc,char**argv){
  const char *path=argc>1?argv[1]:"data.win";
  GmlWin w; if(anygm_stdio_load_win(&w,path)){ fprintf(stderr,"load failed\n"); return 1; }
  GmlVM vm; gml_vm_init(&vm,&w,NULL);
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
  /* Generic timeline probe: index,position,speed,loop. Useful for exercising imported TMLN
   * records independently of a project's own Create-event playback setup. */
  { const char *tp=getenv("GML_START_TIMELINE");
    if(tp && *tp){ int ti=0,loop=0; double pos=-1,speed=1;
      if(sscanf(tp,"%d,%lf,%lf,%d",&ti,&pos,&speed,&loop)>=1){
        for(int i=0;i<vm.inst_count;i++) if(vm.inst[i].active && !vm.inst[i].marked){
          vm.inst[i].timeline_index=ti; vm.inst[i].timeline_position=pos;
          vm.inst[i].timeline_speed=speed; vm.inst[i].timeline_running=1; vm.inst[i].timeline_loop=loop;
          break;
        }
      }
    }
  }
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
    dump_instance_vars(&vm,f);
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
