/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Runtime override parsing, persistent override policy, development menu,
 * room-skip hooks, and intro-skip hooks. */
#include "engine_internal.h"
#include "anygm_host.h"


int core_opt_god(AnygmEngine *engine) {
  return engine->config.god_mode?1:0;
}
int core_opt_start_room(AnygmEngine *engine,int *room_idx) {
  if(!room_idx || engine->config.start_room<0) return 0;
  *room_idx=engine->config.start_room;
  return 1;
}


/* ============================================================================================
 * Generic data-driven cheat engine.
 *
 * A cheat code is one caller-supplied action per line:
 *
 *   room=N                      one-shot: warp to room index N
 *   name=V   | name[i]=V        freeze global ARRAY element   (GM8 / indexed reads)
 *   $name=V                     freeze global SCALAR          (GMS scalar reads; avoids V_ARR->0)
 *   obj:var=V | obj:var[i]=V    freeze a numeric var on every instance of object `obj`
 *   @field=V                    engine state and data-declared aspect behavior
 *   obj@suffix->mode            Draw-GUI route: mode = full_view | backdrop | default
 *
 * A line may carry a SCOPE prefix:
 *   ?aspect      ...            only while an Aspect Ratio Force is active
 *   ?aspect=4:3  ...            only while that specific force mode is active (4:3|16:9|21:9)
 * Un-scoped freezes run every frame after the step (god mode, lives, ...). Aspect-scoped writes
 * run from the aspect hook while a force is active (screensizer / view / engine reshaping).
 *
 * A value V is a NUMBER or a token expression, evaluated left-to-right with * / + - :
 *   $base_w  $base_h            the native room resolution
 *   $forced_w $forced_h         the forced-aspect framebuffer resolution
 *   $extra_w $extra_h           forced minus base (the added width / height)
 * e.g.  some_object:some_width=$forced_w            view_wport[0]=$forced_w
 *       some_object:some_x=$forced_w-115            some_object:some_offset=$extra_w*0.5
 * ============================================================================================ */
#define CHEAT_NAMECH(c) (((c)>='a'&&(c)<='z')||((c)>='A'&&(c)<='Z')||((c)>='0'&&(c)<='9')||(c)=='_')

static void cheat_parse_val(const char *s, CheatVal *v){
  memset(v,0,sizeof *v);
  while(*s==' ') s++;
  if(*s=='$'){
    s++; const char *n=s; while(*s && CHEAT_NAMECH(*s)) s++;
    size_t L=(size_t)(s-n);
    if     (L==6 && !strncmp(n,"base_w",6))   v->tok=TK_BASE_W;
    else if(L==6 && !strncmp(n,"base_h",6))   v->tok=TK_BASE_H;
    else if(L==8 && !strncmp(n,"forced_w",8)) v->tok=TK_FORCED_W;
    else if(L==8 && !strncmp(n,"forced_h",8)) v->tok=TK_FORCED_H;
    else if(L==7 && !strncmp(n,"extra_w",7))  v->tok=TK_EXTRA_W;
    else if(L==7 && !strncmp(n,"extra_h",7))  v->tok=TK_EXTRA_H;
    else { v->tok=TK_LIT; v->lit=0; return; }
    while(*s && v->nop<6){
      while(*s==' ') s++;
      char o=*s; if(o!='*'&&o!='/'&&o!='+'&&o!='-') break;
      s++; char *end; double num=strtod(s,&end); if(end==s) break;
      v->op[v->nop]=o; v->num[v->nop]=num; v->nop++; s=end;
    }
  } else { v->tok=TK_LIT; v->lit=atof(s); }
}
static double cheat_val_eval(AnygmEngine *engine,const CheatVal *v){
  double x;
  switch(v->tok){
    case TK_BASE_W:   x=(double)engine->base_width; break;
    case TK_BASE_H:   x=(double)engine->base_height; break;
    case TK_FORCED_W: x=(double)engine->width; break;
    case TK_FORCED_H: x=(double)engine->height; break;
    case TK_EXTRA_W:  x=(double)engine->width-(double)engine->base_width; break;
    case TK_EXTRA_H:  x=(double)engine->height-(double)engine->base_height; break;
    default:          return v->lit;
  }
  for(int i=0;i<v->nop;i++){ double n=v->num[i];
    switch(v->op[i]){ case '*': x*=n; break; case '/': if(n!=0) x/=n; break;
                      case '+': x+=n; break; case '-': x-=n; break; } }
  return x;
}
static void cheat_parse(const char *code, CheatAct *a){
  memset(a,0,sizeof *a);
  const char *s=code; while(*s==' '||*s=='\t') s++;
  if(!strncmp(s,"?aspect",7)){
    a->scope_aspect=1; s+=7;
    if(*s=='='){ s++;
      if     (!strncmp(s,"4:3",3)) { a->scope_mode=GMC_ASPECT_FORCE_4_3;  s+=3; }
      else if(!strncmp(s,"16:9",4)){ a->scope_mode=GMC_ASPECT_FORCE_16_9; s+=4; }
      else if(!strncmp(s,"21:9",4)){ a->scope_mode=GMC_ASPECT_FORCE_21_9; s+=4; }
    }
    while(*s==' '||*s=='\t') s++;
  }
  if(*s=='@'){                                   /* engine: @field=V */
    s++; const char *n=s; while(*s && *s!='=' && *s!=' ') s++;
    size_t L=(size_t)(s-n); char f[32]; if(L>=sizeof f) L=sizeof f-1; memcpy(f,n,L); f[L]=0;
    a->kind=CK_ENGINE;
    if     (!strcmp(f,"window_w")) a->eng=EF_WINDOW_W;
    else if(!strcmp(f,"window_h")) a->eng=EF_WINDOW_H;
    else if(!strcmp(f,"gui_w"))    a->eng=EF_GUI_W;
    else if(!strcmp(f,"gui_h"))    a->eng=EF_GUI_H;
    else if(!strcmp(f,"fbw"))      a->eng=EF_FBW;
    else if(!strcmp(f,"fbh"))      a->eng=EF_FBH;
    else if(!strcmp(f,"compositor_fullwidth")) a->eng=EF_COMPOSITOR;
    else if(!strcmp(f,"center_view_target")) a->eng=EF_CENTER_VIEW_TARGET;
    else if(!strcmp(f,"wide_gameplay_view")) a->eng=EF_WIDE_GAMEPLAY_VIEW;
    else { a->kind=CK_NONE; return; }
    while(*s==' ') s++;
    if(*s=='='){ s++; cheat_parse_val(s,&a->val); } else { a->val.tok=TK_LIT; a->val.lit=1; }
    return;
  }
  if(*s=='$'){                                   /* global scalar: $name=V */
    s++; const char *n=s; while(*s && CHEAT_NAMECH(*s)) s++;
    size_t L=(size_t)(s-n); if(L==0 || L>=sizeof a->obj){ a->kind=CK_NONE; return; }
    memcpy(a->obj,n,L); a->obj[L]=0;
    while(*s==' ') s++;
    if(*s!='='){ a->kind=CK_NONE; return; }
    s++; cheat_parse_val(s,&a->val); a->kind=CK_GSCALAR; return;
  }
  const char *n=s; while(*s && CHEAT_NAMECH(*s)) s++;      /* leading NAME */
  size_t L=(size_t)(s-n); if(L==0){ a->kind=CK_NONE; return; }
  if(L>=sizeof a->obj) L=sizeof a->obj-1;
  memcpy(a->obj,n,L); a->obj[L]=0;
  if(!strcmp(a->obj,"room") && *s=='='){ a->kind=CK_ROOM; return; }   /* one-shot, via gml_cheat_apply */
  if(*s==':'){                                   /* instance: obj:var[idx]=V */
    s++; const char *vn=s; while(*s && CHEAT_NAMECH(*s)) s++;
    size_t VL=(size_t)(s-vn); if(VL>=sizeof a->var) VL=sizeof a->var-1; memcpy(a->var,vn,VL); a->var[VL]=0;
    if(*s=='['){ s++; a->idx=atoi(s); while(*s && *s!=']') s++; if(*s==']') s++; }
    while(*s==' ') s++;
    if(*s!='='){ a->kind=CK_NONE; return; }
    s++; cheat_parse_val(s,&a->val); a->kind=CK_INST; return;
  }
  if(*s=='@'){                                   /* route: obj@suffix->mode */
    s++; const char *sf=s; while(*s && *s!='-' && *s!=' ') s++;
    size_t SL=(size_t)(s-sf); if(SL>=sizeof a->var) SL=sizeof a->var-1; memcpy(a->var,sf,SL); a->var[SL]=0;
    while(*s==' ') s++;
    if(s[0]=='-' && s[1]=='>'){ s+=2; while(*s==' ') s++;
      if     (!strncmp(s,"backdrop",8))  a->route_mode=GMC_ASPECT_DRAW_FULL_VIEW_BACKDROP;
      else if(!strncmp(s,"full_view",9)) a->route_mode=GMC_ASPECT_DRAW_FULL_VIEW;
      else                                a->route_mode=GMC_ASPECT_DRAW_DEFAULT;
      a->kind=CK_ROUTE; return;
    }
    a->kind=CK_NONE; return;
  }
  if(*s=='['){ s++; a->idx=atoi(s); while(*s && *s!=']') s++; if(*s==']') s++; }  /* global array */
  while(*s==' ') s++;
  if(*s!='='){ a->kind=CK_NONE; return; }
  s++; cheat_parse_val(s,&a->val); a->kind=CK_GARR;
}
static void cheat_apply_one(AnygmEngine *engine,const CheatAct *a){
  switch(a->kind){
    case CK_GSCALAR: gml_set_global_scalar(&engine->vm, a->obj, cheat_val_eval(engine,&a->val)); break;
    case CK_GARR:    gml_set_global_arr(&engine->vm, a->obj, a->idx, cheat_val_eval(engine,&a->val)); break;
    case CK_INST:    gml_set_inst_var_all(&engine->vm, a->obj, a->var, cheat_val_eval(engine,&a->val)); break;
    case CK_ENGINE: { int iv=(int)cheat_val_eval(engine,&a->val);
      switch(a->eng){
        case EF_WINDOW_W: engine->vm.window_w=iv; break;
        case EF_WINDOW_H: engine->vm.window_h=iv; break;
        case EF_GUI_W:    engine->vm.gui_w=iv; break;
        case EF_GUI_H:    engine->vm.gui_h=iv; break;
        case EF_FBW: {
          GmlRenderTargetMetrics target;
          if(gml_render_target_metrics(&engine->render,&target)){
            target.width=iv;
            gml_render_target_metrics_update(&engine->render,&target,GML_RENDER_TARGET_WIDTH);
          }
          break; }
        case EF_FBH: {
          GmlRenderTargetMetrics target;
          if(gml_render_target_metrics(&engine->render,&target)){
            target.height=iv;
            gml_render_target_metrics_update(&engine->render,&target,GML_RENDER_TARGET_HEIGHT);
          }
          break; }
        case EF_COMPOSITOR: case EF_CENTER_VIEW_TARGET: case EF_WIDE_GAMEPLAY_VIEW: break; /* queried by aspect helpers */
      }
      break; }
    default: break;   /* CK_ROOM handled at set-time; CK_ROUTE consulted in the draw hook */
  }
}
/* aspect_phase: 0 = normal post-step sticky pass, 1 = aspect reshaping pass. */
static int cheat_scope_ok(AnygmEngine *engine,const CheatAct *a, int aspect_phase){
  if(a->scope_aspect){
    if(!aspect_phase || !engine->aspect_force_active) return 0;
    if(a->scope_mode && a->scope_mode!=engine->aspect_force_mode) return 0;
    return 1;
  }
  return !aspect_phase;
}
/* ============================================================================================
 * Generic pause-menu editor. Configuration uses the caller-supplied .cht data and `|` separators
 * so labels may contain spaces:
 *   menu|<object>|<labelvar>|<indexvar>|<1d|2d>   declare which menu to edit (1D string array or 2D)
 *   mtoggle|<LABEL>|<target>|<onval>              ON/OFF entry; when ON, freezes target=onval
 *   mrange|<LABEL>|<target>|<min>|<max>|[start]   left/right adjusts target in [min,max] (frozen)
 *   mwarp|<LABEL>|<roomName1,roomName2,...>        left/right picks a room; confirm warps there
 * <target> = $global  or  obj:var . Labels render live: "GOD MODE: ON", "LIVES: 9", "HEARTS: 3".
 * Only the boot path builds the menu, so API-level runtime override resets do not remove it. */
static void menu_target_parse(const char *s, char *obj, char *var){
  obj[0]=0; var[0]=0;
  if(*s=='$'){ snprintf(var,48,"%s",s+1); return; }
  const char *c=strchr(s,':');
  if(c){ int L=(int)(c-s); if(L>47) L=47; memcpy(obj,s,(size_t)L); obj[L]=0; snprintf(var,48,"%s",c+1); }
  else snprintf(var,48,"%s",s);
}
static void menu_copy(char *destination,size_t capacity,const char *source){
  if(!destination || !capacity) return;
  size_t length=source?strlen(source):0;
  if(length>=capacity) length=capacity-1;
  if(length) memcpy(destination,source,length);
  destination[length]='\0';
}
static int menu_split(const char *code, char f[6][128]){
  int n=0; const char *s=code;
  while(n<6){
    const char *bar=strchr(s,'|');
    int L = bar ? (int)(bar-s) : (int)strlen(s); if(L>127) L=127;
    memcpy(f[n],s,(size_t)L); f[n][L]=0; n++;
    if(!bar) break;
    s=bar+1;
  }
  return n;
}
/* Returns 1 if the line was a menu directive (and consumed it), 0 otherwise. */
static int menu_directive_parse(AnygmEngine *engine,const char *code){
  while(*code==' '||*code=='\t') code++;
  char f[6][128];
  if(!strncmp(code,"menu|",5)){
    int n=menu_split(code,f);
    if(n>=4){
      menu_copy(engine->menu.obj,sizeof engine->menu.obj,f[1]);
      menu_copy(engine->menu.labelvar,sizeof engine->menu.labelvar,f[2]);
      menu_copy(engine->menu.idxvar,sizeof engine->menu.idxvar,f[3]);
      engine->menu.is2d = (n>=5 && (!strcmp(f[4],"2d")||!strcmp(f[4],"2D")));
      engine->menu.lift = (n>=6) ? atoi(f[5]) : 0;   /* pixels to raise the menu on open, so extra rows fit */
      engine->menu.active=1;
    }
    return 1;
  }
  if(!strncmp(code,"mset|",5)){        /* force an instance var on the menu object while it is open */
    int n=menu_split(code,f);
    if(n>=3 && engine->menu.ntweaks<8){
      menu_copy(engine->menu.tweaks[engine->menu.ntweaks].var,
                sizeof engine->menu.tweaks[engine->menu.ntweaks].var,f[1]);
      engine->menu.tweaks[engine->menu.ntweaks].val=atof(f[2]);
      engine->menu.ntweaks++;
    }
    return 1;
  }
  if(!strncmp(code,"mtoggle|",8) || !strncmp(code,"mrange|",7) || !strncmp(code,"mwarp|",6)){
    if(engine->menu.nitems>=16) return 1;
    int n=menu_split(code,f);
    MenuItem *it=&engine->menu.items[engine->menu.nitems]; memset(it,0,sizeof *it); it->from_boot=1;
    menu_copy(it->label,sizeof it->label,f[1]);
    if(!strcmp(f[0],"mtoggle") && n>=4){ it->kind=MI_TOGGLE; menu_target_parse(f[2],it->tobj,it->tvar); it->onval=atof(f[3]); }
    else if(!strcmp(f[0],"mrange") && n>=5){ it->kind=MI_RANGE; menu_target_parse(f[2],it->tobj,it->tvar);
      it->vmin=atoi(f[3]); it->vmax=atoi(f[4]); it->value = (n>=6)?atoi(f[5]):it->vmin;
      if(it->value<it->vmin) it->value=it->vmin;
      if(it->value>it->vmax) it->value=it->vmax; }
    else if(!strcmp(f[0],"mwarp") && n>=3){ it->kind=MI_WARP; snprintf(it->roomcsv,sizeof it->roomcsv,"%s",f[2]); }
    else return 1;
    engine->menu.nitems++;
    return 1;
  }
  return 0;
}
static int menu_decl_code(const char *code){
  if(!code) return 0;
  while(*code==' '||*code=='\t') code++;
  return !strncmp(code,"menu|",5);
}
/* Rebuild the editor from enabled cheat slots. A host-supplied menu declaration replaces the
 * boot menu declaration and its directives; ordinary host cheats still layer over boot cheats.
 * This keeps manual override-file loading functional without duplicating boot rows. */
static void menu_rebuild(AnygmEngine *engine){
  int host_menu=0;
  for(int i=0;i<engine->cheat_count;i++)
    if(engine->cheats[i].enabled && menu_decl_code(engine->cheats[i].code)){ host_menu=1; break; }
  memset(&engine->menu,0,sizeof engine->menu);
  const CheatSlot *arr=host_menu?engine->cheats:engine->boot_cheats;
  int n=host_menu?engine->cheat_count:engine->boot_cheat_count;
  for(int i=0;i<n;i++) if(arr[i].enabled) menu_directive_parse(engine,arr[i].code);
}
void engine_override_reset(AnygmEngine *engine){
  engine->cheat_count = 0; memset(engine->cheats, 0, sizeof engine->cheats);
  menu_rebuild(engine);
}
void engine_override_set(AnygmEngine *engine,unsigned i,bool e,const char *c){
  if(!c || i >= GML_MAX_CHEATS) return;
  if((int)i >= engine->cheat_count) engine->cheat_count = (int)i + 1;
  engine->cheats[i].enabled = e ? 1 : 0;
  snprintf(engine->cheats[i].code, sizeof engine->cheats[i].code, "%s", c);
  cheat_parse(engine->cheats[i].code, &engine->cheats[i].act);
  menu_rebuild(engine);
  if(e && engine->loaded && engine->cheats[i].act.kind==CK_ROOM)
    gml_cheat_apply(&engine->vm, engine->cheats[i].code);   /* fire the one-shot warp now */
}
/* Consumer passes run over both API-provided overrides and boot overrides. Boot entries remain
 * active when the caller resets the API-provided slots. */
static void cheat_sticky_pass(AnygmEngine *engine,const CheatSlot *arr, int n){
  for(int i=0;i<n;i++){
    if(!arr[i].enabled) continue;
    const CheatAct *a=&arr[i].act;
    if(a->kind==CK_ROOM || a->kind==CK_NONE) continue;
    if(!cheat_scope_ok(engine,a,0)) continue;
    cheat_apply_one(engine,a);
  }
}
/* Re-apply un-scoped freeze cheats — called every frame after the game step. */
void apply_sticky_cheats(AnygmEngine *engine){
  cheat_sticky_pass(engine,engine->cheats, engine->cheat_count);
  cheat_sticky_pass(engine,engine->boot_cheats, engine->boot_cheat_count);
}
static void cheat_aspect_pass(AnygmEngine *engine,const CheatSlot *arr, int n){
  for(int i=0;i<n;i++){
    if(!arr[i].enabled) continue;
    const CheatAct *a=&arr[i].act;
    if(!a->scope_aspect || a->kind==CK_ROUTE) continue;
    if(!cheat_scope_ok(engine,a,1)) continue;
    cheat_apply_one(engine,a);
  }
}
/* Apply aspect-scoped writes to screen, view, and engine values on every forced-aspect frame. The
 * executable core contains only the generic interpreter; content-specific values are external. */
void aspect_apply_program(AnygmEngine *engine){
  if(!engine->aspect_force_active) return;
  cheat_aspect_pass(engine,engine->cheats, engine->cheat_count);
  cheat_aspect_pass(engine,engine->boot_cheats, engine->boot_cheat_count);
}
static int cheat_compositor_pass(AnygmEngine *engine,const CheatSlot *arr, int n){
  for(int i=0;i<n;i++){
    if(!arr[i].enabled) continue;
    const CheatAct *a=&arr[i].act;
    if(a->kind!=CK_ENGINE || a->eng!=EF_COMPOSITOR || !a->scope_aspect) continue;
    if(a->scope_mode && a->scope_mode!=engine->aspect_force_mode) continue;
    if(cheat_val_eval(engine,&a->val)!=0.0) return 1;
  }
  return 0;
}
/* Any enabled `?aspect @compositor_fullwidth=1` for the active mode runs the Draw-GUI
 * compositor at the full forced-wide resolution instead of a centered sub-rect. */
int aspect_compositor_fullwidth_gen(AnygmEngine *engine){
  return cheat_compositor_pass(engine,engine->cheats, engine->cheat_count)
      || cheat_compositor_pass(engine,engine->boot_cheats, engine->boot_cheat_count);
}
static int cheat_center_target_pass(AnygmEngine *engine,const CheatSlot *arr, int n){
  for(int i=0;i<n;i++){
    if(!arr[i].enabled) continue;
    const CheatAct *a=&arr[i].act;
    if(a->kind!=CK_ENGINE || a->eng!=EF_CENTER_VIEW_TARGET || !a->scope_aspect) continue;
    if(a->scope_mode && a->scope_mode!=engine->aspect_force_mode) continue;
    if(cheat_val_eval(engine,&a->val)!=0.0) return 1;
  }
  return 0;
}
int aspect_center_view_target_gen(AnygmEngine *engine){
  return cheat_center_target_pass(engine,engine->cheats, engine->cheat_count)
      || cheat_center_target_pass(engine,engine->boot_cheats, engine->boot_cheat_count);
}
static int cheat_wide_gameplay_pass(AnygmEngine *engine,const CheatSlot *arr, int n){
  for(int i=0;i<n;i++){
    if(!arr[i].enabled) continue;
    const CheatAct *a=&arr[i].act;
    if(a->kind!=CK_ENGINE || a->eng!=EF_WIDE_GAMEPLAY_VIEW || !a->scope_aspect) continue;
    if(a->scope_mode && a->scope_mode!=engine->aspect_force_mode) continue;
    if(cheat_val_eval(engine,&a->val)!=0.0) return 1;
  }
  return 0;
}
int aspect_wide_gameplay_view_gen(AnygmEngine *engine){
  return cheat_wide_gameplay_pass(engine,engine->cheats, engine->cheat_count)
      || cheat_wide_gameplay_pass(engine,engine->boot_cheats, engine->boot_cheat_count);
}
static int cheat_route_pass(AnygmEngine *engine,const CheatSlot *arr, int n, const char *obj, const char *suffix){
  for(int i=0;i<n;i++){
    if(!arr[i].enabled) continue;
    const CheatAct *a=&arr[i].act;
    if(a->kind!=CK_ROUTE) continue;
    if(a->scope_mode && a->scope_mode!=engine->aspect_force_mode) continue;
    if(strcmp(a->obj,obj) || strcmp(a->var,suffix)) continue;
    return a->route_mode;
  }
  return -1;
}
/* Draw-GUI route table: return the routing mode for an instance object and event suffix, or the
 * default. Routing applies only when the frame is widened. */
int aspect_draw_full_view_gen(AnygmEngine *engine,const GmlInstance *in, const char *suffix){
  if(!in || !suffix || engine->width<=engine->base_width) return GMC_ASPECT_DRAW_DEFAULT;
  const char *obj = (in->obj>=0 && in->obj<engine->vm.n_objects) ? engine->vm.objects[in->obj].name : NULL;
  if(!obj) return GMC_ASPECT_DRAW_DEFAULT;
  int r = cheat_route_pass(engine,engine->cheats, engine->cheat_count, obj, suffix);
  if(r < 0) r = cheat_route_pass(engine,engine->boot_cheats, engine->boot_cheat_count, obj, suffix);
  return r < 0 ? GMC_ASPECT_DRAW_DEFAULT : r;
}
/* Fresh press this frame from either the RetroPad button or a keyboard key. */
static int menu_edge(AnygmEngine *engine,int pad, int vk){
  int cur  = engine->pad_current[pad]  | ((vk>=0&&vk<NKEY)?(engine->key_current[vk]|engine->hardware_key_current[vk]|engine->event_vk_current[vk]):0);
  int prev = engine->pad_previous[pad] | ((vk>=0&&vk<NKEY)?(engine->key_previous[vk]|engine->hardware_key_previous[vk]|engine->event_vk_previous[vk]):0);
  return cur && !prev;
}
static void menu_set_target(AnygmEngine *engine,const MenuItem *it, double v){
  if(it->tobj[0]) gml_set_inst_var_all(&engine->vm, it->tobj, it->tvar, v);
  else            gml_set_global_scalar(&engine->vm, it->tvar, v);
}
/* Apply enabled item effects every frame. While the configured pause menu is open, inject entries
 * and handle left, right, and confirm input. */
void menu_run(AnygmEngine *engine){
  if(!engine->menu.active || !engine->loaded) return;
  /* Toggles (god mode) freeze their target every frame while ON. Ranges are set-on-change (below),
   * not frozen, so LIVES/HEARTS act as setters/refills you can still play against. */
  for(int i=0;i<engine->menu.nitems;i++){ MenuItem *it=&engine->menu.items[i]; if(it->kind==MI_TOGGLE && it->on) menu_set_target(engine,it,it->onval); }

  int oi=gml_object_index_by_name(&engine->vm,engine->menu.obj);
  GmlInstance *in = oi>=0 ? gml_find_instance(&engine->vm,oi) : NULL;
  if(!in){ engine->menu.open_prev=0; return; }
  if(!engine->menu.open_prev){
    engine->menu.base = gml_inst_array_count(in,engine->menu.labelvar);
    if(engine->menu.lift) in->y -= (double)engine->menu.lift;   /* raise the menu once so the injected rows fit on screen */
    engine->menu.open_prev=1;
  }
  /* Force configured layout variables every frame so cached menu layouts retain the added rows. */
  for(int t=0;t<engine->menu.ntweaks;t++) gml_set_inst_var_all(&engine->vm, engine->menu.obj, engine->menu.tweaks[t].var, engine->menu.tweaks[t].val);
  int base=engine->menu.base;
  if(base<0 || base>200) return;

  int sel = gml_inst_get_num(in,engine->menu.idxvar);
  int mi = sel - base;
  int left  = menu_edge(engine,ANYGM_PAD_LEFT, 37);
  int right = menu_edge(engine,ANYGM_PAD_RIGHT, 39);
  int conf  = menu_edge(engine,ANYGM_PAD_FACE_RIGHT, 13) || menu_edge(engine,ANYGM_PAD_FACE_BOTTOM, -1);
  if(mi>=0 && mi<engine->menu.nitems){
    MenuItem *it=&engine->menu.items[mi];
    if(it->kind==MI_TOGGLE){ if(conf) it->on=!it->on; }
    else if(it->kind==MI_RANGE){
      int old=it->value;
      if(left  && it->value>it->vmin) it->value--;
      if(right && it->value<it->vmax) it->value++;
      if(it->value!=old || conf) menu_set_target(engine,it,(double)it->value);   /* set / refill on change or confirm */
    }
    else if(it->kind==MI_WARP){
      if(!it->resolved){
        char csv[128]; snprintf(csv,sizeof csv,"%s",it->roomcsv); char *sv=NULL;
        for(char *t=strtok_r(csv,",",&sv); t && it->nrooms<24; t=strtok_r(NULL,",",&sv)){
          int ri=gml_room_index_by_name(engine->vm.win,t); if(ri>=0) it->rooms[it->nrooms++]=ri;
        }
        it->resolved=1;
      }
      if(it->nrooms>0){
        if(left)  it->warpsel=(it->warpsel-1+it->nrooms)%it->nrooms;
        if(right) it->warpsel=(it->warpsel+1)%it->nrooms;
        if(conf){ char rb[24]; snprintf(rb,sizeof rb,"room=%d",it->rooms[it->warpsel]); gml_cheat_apply(&engine->vm,rb); }
      }
    }
  }
  /* (re)write the injected entries at rows [base .. base+nitems-1] every frame so labels stay live */
  for(int k=0;k<engine->menu.nitems && base+k<254;k++){
    MenuItem *it=&engine->menu.items[k];
    char buf[48];
    if(it->kind==MI_TOGGLE)      snprintf(buf,sizeof buf,"%s: %s", it->label, it->on?"ON":"OFF");
    else if(it->kind==MI_RANGE)  snprintf(buf,sizeof buf,"%s: %d", it->label, it->value);
    else                          snprintf(buf,sizeof buf,"%s: %d", it->label, it->warpsel+1);
    int row=base+k;
    gml_inst_array_set_str(in,engine->menu.labelvar,engine->menu.is2d,row,0,buf);
    if(engine->menu.is2d) gml_inst_array_set_num(in,engine->menu.labelvar,engine->menu.is2d,row,1, 900.0+k);   /* ignored sentinel code */
  }
}
/* Generic room skip: Select or Start advances to the next room in play order on a fresh press.
 * Face buttons remain untouched. */
void room_skip_hook(AnygmEngine *engine){
  int btn=-1;
  if(engine->config.room_skip_button==1u) btn=ANYGM_PAD_SELECT;
  else if(engine->config.room_skip_button==2u) btn=ANYGM_PAD_START;
  if(btn < 0) return;
  if(engine->pad_current[btn] && !engine->pad_previous[btn] && engine->vm.pending_room < 0){
    int room = engine->vm.room_index, ord = -1;
    for(int i=0;i<engine->win.n_room_order;i++) if((int)engine->win.room_order[i]==room){ ord=i; break; }
    if(ord>=0 && ord+1<engine->win.n_room_order) gml_vm_goto_room_order(&engine->vm, ord+1);
  }
}


/* GML_INTROSKIP="1,2,4,5" or "1-4,9": in the listed room indices, A or B advances to the next
 * room in play order. The room list is supplied at launch. */
static void introskip_parse(AnygmEngine *engine,const char *s){
  memset(engine->introskip_set, 0, sizeof engine->introskip_set);
  engine->introskip_enabled = (s && *s) ? 1 : 0;
  if(!engine->introskip_enabled) return;
  while(*s){
    while(*s==' '||*s==',') s++;
    if(!*s) break;
    int a=atoi(s); while(*s && *s!=',' && *s!='-' && *s!=' ') s++;
    int b=a;
    if(*s=='-'){ s++; b=atoi(s); while(*s && *s!=',' && *s!=' ') s++; }
    if(a>b){ int t=a; a=b; b=t; }
    for(int r=a; r<=b && r<1024; r++) if(r>=0) engine->introskip_set[r>>3] |= (uint8_t)(1u<<(r&7));
  }
}
void introskip_hook(AnygmEngine *engine){
  if(engine->introskip_enabled < 0) introskip_parse(engine,anygm_host_development_setting(&engine->host,"GML_INTROSKIP"));
  if(!engine->introskip_enabled) return;
  int room = engine->vm.room_index;
  if(room < 0 || room >= 1024) return;
  if(!(engine->introskip_set[room>>3] & (1u<<(room&7)))) return;   /* not a listed intro room */
  int a = engine->pad_current[ANYGM_PAD_FACE_RIGHT] && !engine->pad_previous[ANYGM_PAD_FACE_RIGHT];
  int b = engine->pad_current[ANYGM_PAD_FACE_BOTTOM] && !engine->pad_previous[ANYGM_PAD_FACE_BOTTOM];
  if((a||b) && engine->vm.pending_room < 0){
    int ord=-1; for(int i=0;i<engine->win.n_room_order;i++) if((int)engine->win.room_order[i]==room){ ord=i; break; }
    if(ord>=0 && ord+1<engine->win.n_room_order) gml_vm_goto_room_order(&engine->vm, ord+1);
  }
}
