/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Runtime override parsing, persistent override policy, development menu,
 * room-skip hooks, and intro-skip hooks. */
#include "engine_internal.h"
#include "anygm_host.h"

#include <math.h>


int core_opt_god(AnygmEngine *engine) {
  return engine->config.god_mode?1:0;
}
int core_opt_start_room(AnygmEngine *engine,int *room_idx) {
  if(!room_idx || engine->config.start_room<0) return 0;
  *room_idx=engine->config.start_room;
  return 1;
}

static int parse_nonnegative_index(const char **cursor,int *value){
  const char *text=*cursor;
  unsigned parsed=0;
  if(!text || *text<'0' || *text>'9') return 0;
  do{
    unsigned digit=(unsigned)(*text-'0');
    if(parsed>((unsigned)INT_MAX-digit)/10u) return 0;
    parsed=parsed*10u+digit;
    text++;
  }while(*text>='0' && *text<='9');
  *cursor=text;
  *value=(int)parsed;
  return 1;
}

int core_opt_redirect_room_order(AnygmEngine *engine) {
  const char *setting=anygm_host_development_setting(
    &engine->host,"GML_REDIRECT_ROOM_ORDER");
  if(!setting || !setting[0]) return 0;
  const char *cursor=setting;
  int slot=-1,room=-1;
  if(!parse_nonnegative_index(&cursor,&slot) || *cursor++!=':' ||
     !parse_nonnegative_index(&cursor,&room) || *cursor ||
     !engine->win.room_order || slot>=engine->win.n_room_order ||
     room>=gml_room_count(&engine->win)){
    engine_logf(engine,ANYGM_LOG_WARN,
      "Ignoring invalid GML_REDIRECT_ROOM_ORDER=%s (expected slot:room within this payload)",
      setting);
    return 0;
  }
  engine->win.room_order[slot]=(uint32_t)room;
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
 *   alarmpause|obj|i            hold alarm i still on every instance of `obj` and its descendants
 *   call|script                 invoke a zero-argument content script once after the first Step
 *   camera[LIST]:field=V        write x, y, width, or height on selected live camera handles
 *   surface|obj|var|W|H         resize surfaces named by an instance variable
 *   monitorview|H|MIN|MAX       declare a monitor-derived logical view (ratios use W:H)
 *   @field=V                    engine state and data-declared aspect behavior
 *   obj@suffix->mode            Draw-GUI route: mode = full_view | backdrop | default
 *
 * A line may carry a SCOPE prefix:
 *   ?aspect      ...            only while an Aspect Ratio Force is active
 *   ?aspect=4:3  ...            only while that specific force mode is active (4:3|16:9|21:9)
 *   ?monitor     ...            once, on each live virtual-monitor size change
 *   ?gameres     ...            only while the logical-raster presentation is selected
 * Un-scoped freezes run every frame after the step (god mode, lives, ...). Aspect-scoped writes
 * run from the aspect hook while a force is active (screensizer / view / engine reshaping).
 *
 * A ?gameres line scopes declared writes to logical-raster presentation. Targets may describe a presentation variable, view port or surface size. Closing the scope restores captured values.
 * That round trip is what separates this scope from an un-scoped freeze, and it is why every
 * scoped directive captures what it overwrote — see cheat_slot_capture.
 *
 * A line may chain several directives with ';', so one frontend cheat entry can drive more than
 * one target from a single ON/OFF toggle:
 *   $flag_1=1;$flag_2=1;$flag_3=1                       three freezes, one entry
 * Each directive is validated, armed, captured, and restored on its own; the chain as a whole
 * arms and disarms with the entry that carries it.
 *
 * A value V is a NUMBER or a token expression, evaluated left-to-right with * / + - :
 *   $base_w  $base_h            the native room resolution
 *   $forced_w $forced_h         the forced-aspect framebuffer resolution
 *   $extra_w $extra_h           forced minus base (the added width / height)
 *   $monitor_w $monitor_h       configured virtual-monitor resolution
 *   $monitor_view_w/_h          logical view declared by monitorview|
 *   $monitor_extra_w/_h         monitor view minus the native room resolution
 *   $view_w  $view_h            current room's authored view_wview/view_hview, read live
 *                                before room-scoped geometry is decided
 * e.g.  some_object:some_width=$forced_w            view_wport[0]=$forced_w
 *       some_object:some_x=$forced_w-115            some_object:some_offset=$extra_w*0.5
 * ============================================================================================ */
#define CHEAT_NAMECH(c) (((c)>='a'&&(c)<='z')||((c)>='A'&&(c)<='Z')||((c)>='0'&&(c)<='9')||(c)=='_')
#define CHEAT_MAX_DIRECTIVES 8
#define CHEAT_DIRECTIVE_BYTES 128

static void cheat_monitor_dimensions(AnygmEngine *engine,double *monitor_w,double *monitor_h,
                                     double *view_w,double *view_h);

/* Split a code into its `;`-separated directives, trimmed. Empty pieces are dropped, so a stray
 * or trailing ';' costs nothing, and an unchained code simply yields one part. */
static int cheat_split_directives(const char *code,
                                  char parts[CHEAT_MAX_DIRECTIVES][CHEAT_DIRECTIVE_BYTES]){
  int n=0;
  const char *s=code?code:"";
  while(*s && n<CHEAT_MAX_DIRECTIVES){
    while(*s==';'||*s==' '||*s=='\t') s++;
    if(!*s) break;
    const char *end=strchr(s,';');
    size_t length=end?(size_t)(end-s):strlen(s);
    while(length && (s[length-1]==' '||s[length-1]=='\t'||s[length-1]=='\r')) length--;
    if(length){
      if(length>=CHEAT_DIRECTIVE_BYTES) length=CHEAT_DIRECTIVE_BYTES-1;
      memcpy(parts[n],s,length); parts[n][length]=0; n++;
    }
    if(!end) break;
    s=end+1;
  }
  return n;
}

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
    else if(L==9 && !strncmp(n,"monitor_w",9)) v->tok=TK_MONITOR_W;
    else if(L==9 && !strncmp(n,"monitor_h",9)) v->tok=TK_MONITOR_H;
    else if(L==14 && !strncmp(n,"monitor_view_w",14)) v->tok=TK_MONITOR_VIEW_W;
    else if(L==14 && !strncmp(n,"monitor_view_h",14)) v->tok=TK_MONITOR_VIEW_H;
    else if(L==15 && !strncmp(n,"monitor_extra_w",15)) v->tok=TK_MONITOR_EXTRA_W;
    else if(L==15 && !strncmp(n,"monitor_extra_h",15)) v->tok=TK_MONITOR_EXTRA_H;
    else if(L==6 && !strncmp(n,"view_w",6)) v->tok=TK_VIEW_W;
    else if(L==6 && !strncmp(n,"view_h",6)) v->tok=TK_VIEW_H;
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
  double x,monitor_w=0,monitor_h=0,view_w=0,view_h=0;
  if(v->tok>=TK_MONITOR_W)
    cheat_monitor_dimensions(engine,&monitor_w,&monitor_h,&view_w,&view_h);
  switch(v->tok){
    case TK_BASE_W:   x=(double)engine->base_width; break;
    case TK_BASE_H:   x=(double)engine->base_height; break;
    case TK_FORCED_W: x=(double)engine->width; break;
    case TK_FORCED_H: x=(double)engine->height; break;
    case TK_EXTRA_W:  x=(double)engine->width-(double)engine->base_width; break;
    case TK_EXTRA_H:  x=(double)engine->height-(double)engine->base_height; break;
    case TK_MONITOR_W: x=monitor_w; break;
    case TK_MONITOR_H: x=monitor_h; break;
    case TK_MONITOR_VIEW_W: x=view_w; break;
    case TK_MONITOR_VIEW_H: x=view_h; break;
    case TK_MONITOR_EXTRA_W: x=view_w-(double)engine->base_width; break;
    case TK_MONITOR_EXTRA_H: x=view_h-(double)engine->base_height; break;
    /* Read the current room's authored view extent synchronously, before a room-owned
     * override settles its geometry. */
    case TK_VIEW_W: x=gml_global_arr(&engine->vm,"view_wview",0); break;
    case TK_VIEW_H: x=gml_global_arr(&engine->vm,"view_hview",0); break;
    default:          return v->lit;
  }
  for(int i=0;i<v->nop;i++){ double n=v->num[i];
    switch(v->op[i]){ case '*': x*=n; break; case '/': if(n!=0) x/=n; break;
                      case '+': x+=n; break; case '-': x-=n; break; } }
  return x;
}

static int cheat_parse_ratio(const char *text,double *ratio,const char **end_out){
  char *end=NULL;
  double numerator=strtod(text,&end);
  if(end==text || *end!=':' || !isfinite(numerator) || numerator<=0) return 0;
  char *denominator_end=NULL;
  double denominator=strtod(end+1,&denominator_end);
  if(denominator_end==end+1 || !isfinite(denominator) || denominator<=0) return 0;
  *ratio=numerator/denominator;
  if(end_out) *end_out=denominator_end;
  return 1;
}

static int cheat_parse_camera_mask(const char **cursor,uint64_t *mask){
  const char *s=*cursor;
  if(*s!='[') return 0;
  s++; *mask=0;
  while(*s && *s!=']'){
    int first=-1,last=-1;
    if(!parse_nonnegative_index(&s,&first) || first>=GML_CAMERA_LIMIT) return 0;
    last=first;
    if(*s=='-'){
      s++;
      if(!parse_nonnegative_index(&s,&last) || last<first || last>=GML_CAMERA_LIMIT) return 0;
    }
    for(int camera=first;camera<=last;camera++) *mask|=UINT64_C(1)<<camera;
    if(*s==',') s++;
    else if(*s!=']') return 0;
  }
  if(*s!=']' || !*mask) return 0;
  *cursor=s+1;
  return 1;
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
      else if(!strncmp(s,"16:10",5)){ a->scope_mode=GMC_ASPECT_FORCE_16_10; s+=5; }
    }
    while(*s==' '||*s=='\t') s++;
  } else if(!strncmp(s,"?monitor",8)){
    a->scope_monitor=1; s+=8;
    while(*s==' '||*s=='\t') s++;
  } else if(!strncmp(s,"?gameres",8)){
    a->scope_gameres=1; s+=8;
    while(*s==' '||*s=='\t') s++;
  }
  if(!strncmp(s,"monitorview|",12)){
    const char *height=s+12,*end=NULL;
    char *height_end=NULL;
    a->monitor_height=strtod(height,&height_end);
    if(height_end==height || *height_end!='|' || !isfinite(a->monitor_height) ||
       a->monitor_height<=0 ||
       !cheat_parse_ratio(height_end+1,&a->monitor_min_aspect,&end) || *end!='|' ||
       !cheat_parse_ratio(end+1,&a->monitor_max_aspect,&end) || *end ||
       a->monitor_max_aspect<a->monitor_min_aspect){
      a->kind=CK_NONE; return;
    }
    a->kind=CK_MONITOR_VIEW; return;
  }
  if(!strncmp(s,"alarmpause|",11)){
    s+=11; const char *bar=strchr(s,'|');
    if(!bar || bar==s || (size_t)(bar-s)>=sizeof a->obj){ a->kind=CK_NONE; return; }
    memcpy(a->obj,s,(size_t)(bar-s)); a->obj[bar-s]=0; s=bar+1;
    if(*s<'0' || *s>'9'){ a->kind=CK_NONE; return; }
    a->idx=atoi(s); while(*s>='0' && *s<='9') s++;
    if(*s || a->idx<0 || a->idx>=GML_ALARMS){ a->kind=CK_NONE; return; }
    a->kind=CK_ALARM_PAUSE; return;
  }
  if(!strncmp(s,"call|",5)){
    s+=5; const char *name=s; while(*s && CHEAT_NAMECH(*s)) s++;
    size_t length=(size_t)(s-name);
    if(length==0 || length>=sizeof a->obj || *s){ a->kind=CK_NONE; return; }
    memcpy(a->obj,name,length); a->obj[length]=0;
    a->kind=CK_SCRIPT; return;
  }
  if(!strncmp(s,"surface|",8)){
    s+=8; const char *bar=strchr(s,'|');
    if(!bar || bar==s || (size_t)(bar-s)>=sizeof a->obj){ a->kind=CK_NONE; return; }
    memcpy(a->obj,s,(size_t)(bar-s)); a->obj[bar-s]=0; s=bar+1;
    bar=strchr(s,'|');
    if(!bar || bar==s || (size_t)(bar-s)>=sizeof a->var){ a->kind=CK_NONE; return; }
    memcpy(a->var,s,(size_t)(bar-s)); a->var[bar-s]=0; s=bar+1;
    bar=strchr(s,'|');
    if(!bar || bar==s){ a->kind=CK_NONE; return; }
    char first[64]; size_t first_length=(size_t)(bar-s);
    if(first_length>=sizeof first){ a->kind=CK_NONE; return; }
    memcpy(first,s,first_length); first[first_length]=0;
    cheat_parse_val(first,&a->val); cheat_parse_val(bar+1,&a->val2);
    a->kind=CK_SURFACE; return;
  }
  if(!strncmp(s,"camera",6)){
    s+=6;
    if(!cheat_parse_camera_mask(&s,&a->camera_mask) || *s++!=':'){
      a->kind=CK_NONE; return;
    }
    const char *field=s; while(*s && CHEAT_NAMECH(*s)) s++;
    size_t length=(size_t)(s-field);
    if(length==1 && !strncmp(field,"x",1)) a->camera_field=CF_X;
    else if(length==1 && !strncmp(field,"y",1)) a->camera_field=CF_Y;
    else if(length==5 && !strncmp(field,"width",5)) a->camera_field=CF_WIDTH;
    else if(length==6 && !strncmp(field,"height",6)) a->camera_field=CF_HEIGHT;
    else { a->kind=CK_NONE; return; }
    while(*s==' ') s++;
    if(*s!='='){ a->kind=CK_NONE; return; }
    cheat_parse_val(s+1,&a->val); a->kind=CK_CAMERA; return;
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
    else if(!strcmp(f,"application_w")) a->eng=EF_APPLICATION_W;
    else if(!strcmp(f,"application_h")) a->eng=EF_APPLICATION_H;
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
    case CK_ALARM_PAUSE: gml_alarm_pause_add(&engine->vm, a->obj, a->idx); break;
    case CK_CAMERA:
      gml_camera_override_mask(&engine->vm,a->camera_mask,(int)a->camera_field,
                               cheat_val_eval(engine,&a->val));
      break;
    case CK_SURFACE:
      gml_resize_inst_surface_all(
        &engine->vm,a->obj,a->var,(int)cheat_val_eval(engine,&a->val),
        (int)cheat_val_eval(engine,&a->val2));
      break;
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
        case EF_APPLICATION_W: {
          GmlRenderPresentationMetrics presentation;
          if(gml_render_presentation_metrics(&engine->render,&presentation) &&
             presentation.application_height>0)
            (void)gml_render_application_surface_ensure_owned(
              &engine->render,iv,presentation.application_height);
          break; }
        case EF_APPLICATION_H: {
          GmlRenderPresentationMetrics presentation;
          if(gml_render_presentation_metrics(&engine->render,&presentation) &&
             presentation.application_width>0)
            (void)gml_render_application_surface_ensure_owned(
              &engine->render,presentation.application_width,iv);
          break; }
        case EF_COMPOSITOR: case EF_CENTER_VIEW_TARGET: case EF_WIDE_GAMEPLAY_VIEW: break; /* queried by aspect helpers */
      }
      break; }
    default: break;   /* CK_ROOM handled at set-time; CK_ROUTE consulted in the draw hook */
  }
}
/* phase: 0 = normal post-step sticky pass, 1 = aspect reshaping, 2 = monitor-change edge. */
static int cheat_scope_ok(AnygmEngine *engine,const CheatAct *a,int phase){
  if(a->scope_monitor) return phase==2;
  if(a->scope_aspect){
    if(phase!=1 || !engine->aspect_force_active) return 0;
    if(a->scope_mode && a->scope_mode!=engine->aspect_force_mode) return 0;
    return 1;
  }
  /* Read live rather than latched: the selection can change between two frames, and the pass that
   * gives the content its own numbers back is the same pass that took them. */
  if(a->scope_gameres) return phase==0 && engine->config.present_logical_raster!=0;
  return phase==0;
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
/* True when any directive the code carries declares the menu. Chained codes are searched part by
 * part so `menu|...` need not be the piece that happens to come first. */
static int menu_decl_code(const char *code){
  if(!code) return 0;
  char parts[CHEAT_MAX_DIRECTIVES][CHEAT_DIRECTIVE_BYTES];
  int n=cheat_split_directives(code,parts);
  for(int p=0;p<n;p++){
    const char *s=parts[p];
    while(*s==' '||*s=='\t') s++;
    if(!strncmp(s,"menu|",5)) return 1;
  }
  return 0;
}
/* Rebuild the editor from enabled cheat slots. A host-supplied menu declaration replaces the
 * boot menu declaration and its directives; ordinary host cheats still layer over boot cheats.
 * This keeps manual override-file loading functional without duplicating boot rows.
 *
 * A chained code is read from the slot the caller addressed, whose code still holds every
 * directive; its continuation slots are skipped so a chained menu row is not declared twice. */
static void menu_rebuild(AnygmEngine *engine){
  int host_menu=0;
  for(int i=0;i<engine->cheat_count;i++)
    if(engine->cheats[i].enabled && !engine->cheats[i].continuation_of &&
       menu_decl_code(engine->cheats[i].code)){ host_menu=1; break; }
  memset(&engine->menu,0,sizeof engine->menu);
  const CheatSlot *arr=host_menu?engine->cheats:engine->boot_cheats;
  int n=host_menu?engine->cheat_count:engine_boot_cheats_active(engine);
  for(int i=0;i<n;i++){
    if(!arr[i].enabled || arr[i].continuation_of) continue;
    char parts[CHEAT_MAX_DIRECTIVES][CHEAT_DIRECTIVE_BYTES];
    int np=cheat_split_directives(arr[i].code,parts);
    for(int p=0;p<np;p++) menu_directive_parse(engine,parts[p]);
  }
}
void engine_override_reset(AnygmEngine *engine){
  engine->cheat_count = 0; memset(engine->cheats, 0, sizeof engine->cheats);
  menu_rebuild(engine);
}
/* Menu declarations carry no CheatAct; menu_rebuild consumes their codes directly. */
static int boot_line_is_menu(const char *code){
  return !strncmp(code,"menu|",5) || !strncmp(code,"mset|",5) ||
         !strncmp(code,"mtoggle|",8) || !strncmp(code,"mrange|",7) || !strncmp(code,"mwarp|",6);
}
int engine_boot_overrides_parse(const char *text,CheatSlot *slots,int *count,
                                char *error,size_t error_capacity){
  *count=0;
  memset(slots,0,sizeof *slots*GML_MAX_CHEATS);
  if(!text || !text[0]) return 1;
  int line_number=0;
  const char *cursor=text;
  while(*cursor){
    const char *line_end=strchr(cursor,'\n');
    size_t length=line_end?(size_t)(line_end-cursor):strlen(cursor);
    line_number++;
    while(length && (cursor[0]==' '||cursor[0]=='\t')){ cursor++; length--; }
    while(length && (cursor[length-1]==' '||cursor[length-1]=='\t'||cursor[length-1]=='\r'))
      length--;
    if(length && cursor[0]!='#'){
      /* A chained line fans out into one slot per directive, so every consumer below — the
       * introskip anchor lookup, the menu builder, the per-frame passes — keeps reading exactly
       * one directive per slot and needs no chain handling of its own. */
      char line[CHEAT_DIRECTIVE_BYTES*CHEAT_MAX_DIRECTIVES];
      if(length>=sizeof line){
        snprintf(error,error_capacity,"directive %d is longer than %llu bytes",
                 line_number,(unsigned long long)(sizeof line-1u));
        return 0;
      }
      memcpy(line,cursor,length);
      line[length]=0;
      char parts[CHEAT_MAX_DIRECTIVES][CHEAT_DIRECTIVE_BYTES];
      int part_count=cheat_split_directives(line,parts);
      for(int p=0;p<part_count;p++){
        if(*count>=GML_MAX_CHEATS){
          snprintf(error,error_capacity,"more than %d directives",GML_MAX_CHEATS);
          return 0;
        }
        CheatSlot *slot=&slots[*count];
        if(strlen(parts[p])>=sizeof slot->code){
          snprintf(error,error_capacity,"directive %d is longer than %llu bytes",
                   line_number,(unsigned long long)(sizeof slot->code-1u));
          return 0;
        }
        snprintf(slot->code,sizeof slot->code,"%s",parts[p]);
        slot->enabled=1;
        if(!strncmp(slot->code,"introskip|",10)){
          const char *list=slot->code+10;
          int digits=0,list_ok=list[0]!=0;
          for(const char *scan=list;*scan && list_ok;scan++){
            if(*scan>='0' && *scan<='9') digits=1;
            else if(*scan!=',' && *scan!='-' && *scan!=' ' && *scan!='\t') list_ok=0;
          }
          if(!list_ok || !digits){
            snprintf(error,error_capacity,
                     "directive %d: introskip| takes a room index list such as 1,3-5",line_number);
            return 0;
          }
        } else if(!boot_line_is_menu(slot->code)){
          cheat_parse(slot->code,&slot->act);
          if(slot->act.kind==CK_NONE){
            snprintf(error,error_capacity,"directive %d is not a recognized override",line_number);
            return 0;
          }
          if(slot->act.kind==CK_ROOM){
            snprintf(error,error_capacity,
                     "directive %d: room= is a one-shot warp; declare an mwarp menu entry instead",
                     line_number);
            return 0;
          }
        }
        (*count)++;
      }
    }
    if(!line_end) break;
    cursor=line_end+1;
  }
  return 1;
}
int engine_boot_cheats_active(AnygmEngine *engine){
  return engine->config.content_overrides?engine->boot_cheat_count:0;
}
static void cheat_monitor_declaration(const CheatSlot *slots,int count,
                                      double *height,double *minimum,double *maximum){
  for(int i=0;i<count;i++){
    const CheatAct *action=&slots[i].act;
    if(!slots[i].enabled || action->kind!=CK_MONITOR_VIEW) continue;
    *height=action->monitor_height;
    *minimum=action->monitor_min_aspect;
    *maximum=action->monitor_max_aspect;
  }
}
static void cheat_monitor_dimensions(AnygmEngine *engine,double *monitor_w,double *monitor_h,
                                     double *view_w,double *view_h){
  GmlRenderPresentationMetrics presentation={0};
  (void)gml_render_presentation_metrics(&engine->render,&presentation);
  double mw=presentation.monitor_width>0 ? (double)presentation.monitor_width :
                                         (double)engine->config.monitor_width;
  double mh=presentation.monitor_height>0 ? (double)presentation.monitor_height :
                                          (double)engine->config.monitor_height;
  if(mw<=0) mw=engine->vm.window_w>0 ? (double)engine->vm.window_w :
                                      (double)engine->base_width;
  if(mh<=0) mh=engine->vm.window_h>0 ? (double)engine->vm.window_h :
                                      (double)engine->base_height;
  double height=0,minimum=0,maximum=0;
  cheat_monitor_declaration(engine->cheats,engine->cheat_count,&height,&minimum,&maximum);
  cheat_monitor_declaration(engine->boot_cheats,engine_boot_cheats_active(engine),
                            &height,&minimum,&maximum);
  double width=engine->base_width;
  if(height>0 && minimum>0 && maximum>=minimum && mh>0){
    double aspect=mw/mh;
    if(aspect<minimum) aspect=minimum;
    if(aspect>maximum) aspect=maximum;
    width=nearbyint(height*aspect);
  } else if(engine->base_height>0) {
    height=engine->base_height;
  }
  if(monitor_w) *monitor_w=mw;
  if(monitor_h) *monitor_h=mh;
  if(view_w) *view_w=width;
  if(view_h) *view_h=height;
}
void engine_override_menu_refresh(AnygmEngine *engine){
  menu_rebuild(engine);
}
/* Frontend toggles capture scalar or array globals. Population-wide instance and surface writes have no single previous value. A scoped logical-raster declaration may capture a target when its address has one meaningful previous value, including the bounded mutable presentation fields below. */
static int cheat_engine_field_mutable(const CheatAct *a){
  return a->kind==CK_ENGINE && a->eng>=EF_WINDOW_W && a->eng<=EF_APPLICATION_H;
}
static int cheat_slot_capturable(const CheatAct *a){
  if(a->kind==CK_GSCALAR || a->kind==CK_GARR) return 1;
  return a->scope_gameres &&
         (a->kind==CK_INST || a->kind==CK_SURFACE || cheat_engine_field_mutable(a));
}
static void cheat_slot_capture(AnygmEngine *engine,CheatSlot *slot){
  slot->saved_valid=0;
  if(!engine->loaded || !cheat_slot_capturable(&slot->act)) return;
  switch(slot->act.kind){
    case CK_GSCALAR:
      slot->saved=gml_global_num(&engine->vm, slot->act.obj);
      slot->saved_valid=1;
      break;
    case CK_GARR:
      slot->saved=gml_global_arr(&engine->vm, slot->act.obj, slot->act.idx);
      slot->saved_valid=1;
      break;
    case CK_INST: {
      double previous=0;
      if(gml_inst_var_first_real(&engine->vm,slot->act.obj,slot->act.var,&previous)){
        slot->saved=previous;
        slot->saved_valid=1;
      }
      break; }
    case CK_SURFACE: {
      int width=0,height=0;
      if(gml_inst_surface_size_first(&engine->vm,slot->act.obj,slot->act.var,&width,&height)){
        slot->saved=(double)width;
        slot->saved2=(double)height;
        slot->saved_valid=1;
      }
      break; }
    case CK_ENGINE:
      switch(slot->act.eng){
        case EF_WINDOW_W: slot->saved=engine->vm.window_w; slot->saved_valid=1; break;
        case EF_WINDOW_H: slot->saved=engine->vm.window_h; slot->saved_valid=1; break;
        case EF_GUI_W: slot->saved=engine->vm.gui_w; slot->saved_valid=1; break;
        case EF_GUI_H: slot->saved=engine->vm.gui_h; slot->saved_valid=1; break;
        case EF_FBW: case EF_FBH: {
          GmlRenderTargetMetrics target;
          if(gml_render_target_metrics(&engine->render,&target)){
            slot->saved=slot->act.eng==EF_FBW?target.width:target.height;
            slot->saved_valid=1;
          }
          break; }
        case EF_APPLICATION_W: case EF_APPLICATION_H: {
          GmlRenderPresentationMetrics presentation;
          if(gml_render_presentation_metrics(&engine->render,&presentation)){
            slot->saved=slot->act.eng==EF_APPLICATION_W?presentation.application_width:
                                                            presentation.application_height;
            slot->saved_valid=1;
          }
          break; }
        default: break;
      }
      break;
    default: break;
  }
}
/* keep: a scope that may swing back needs its captured value to survive the release, because the
 * content will not recompute it. A frontend slot being dropped keeps the old behaviour of
 * forgetting, so a re-armed slot captures the current value rather than a stale one. */
static void cheat_slot_restore_keep(AnygmEngine *engine,CheatSlot *slot,int keep){
  if(!slot->saved_valid) return;
  if(!keep) slot->saved_valid=0;
  if(!engine->loaded) return;
  switch(slot->act.kind){
    case CK_GSCALAR: gml_set_global_scalar(&engine->vm, slot->act.obj, slot->saved); break;
    case CK_GARR:    gml_set_global_arr(&engine->vm, slot->act.obj, slot->act.idx, slot->saved);
                     break;
    case CK_INST:    gml_set_inst_var_all(&engine->vm, slot->act.obj, slot->act.var, slot->saved);
                     break;
    case CK_SURFACE: gml_resize_inst_surface_all(&engine->vm, slot->act.obj, slot->act.var,
                                                 (int)slot->saved,(int)slot->saved2);
                     break;
    case CK_ENGINE: {
      CheatAct restored=slot->act;
      restored.val.tok=TK_LIT;
      restored.val.lit=slot->saved;
      restored.val.nop=0;
      cheat_apply_one(engine,&restored);
      break; }
    default: break;
  }
}
static void cheat_slot_restore(AnygmEngine *engine,CheatSlot *slot){
  cheat_slot_restore_keep(engine,slot,0);
}

/* Give back every slot the addressed entry chained, restoring what each of them froze. Dropping a
 * chain has to undo the parked directives the same way disarming the entry undoes its first. */
static void cheat_chain_release(AnygmEngine *engine,unsigned owner){
  for(int k=0;k<engine->cheat_count;k++){
    CheatSlot *link=&engine->cheats[k];
    if(link->continuation_of != (int)owner+1) continue;
    if(link->enabled) cheat_slot_restore(engine,link);
    memset(link,0,sizeof *link);
  }
}
/* Park a chained directive in a free slot, taken from the top of the table because the frontend
 * addresses slots from 0 upwards — the two allocations meet only when a caller fills the table. */
static CheatSlot *cheat_chain_alloc(AnygmEngine *engine,unsigned owner){
  for(int k=GML_MAX_CHEATS-1;k>=0;k--){
    if((unsigned)k==owner) continue;
    CheatSlot *link=&engine->cheats[k];
    if(link->enabled || link->code[0] || link->continuation_of) continue;
    memset(link,0,sizeof *link);
    link->continuation_of=(int)owner+1;
    if(k >= engine->cheat_count) engine->cheat_count = k+1;
    return link;
  }
  return NULL;   /* table full: the entry keeps the directives that did fit */
}
void engine_override_set(AnygmEngine *engine,unsigned i,bool e,const char *c){
  if(!c || i >= GML_MAX_CHEATS) return;
  if((int)i >= engine->cheat_count) engine->cheat_count = (int)i + 1;
  CheatSlot *slot=&engine->cheats[i];
  /* An index the caller addresses is never a continuation. Reclaim it — undoing what it froze —
   * so a chain that had parked here cannot be left half-owned by a slot it no longer holds. */
  if(slot->continuation_of){
    if(slot->enabled) cheat_slot_restore(engine,slot);
    memset(slot,0,sizeof *slot);
  }
  int was_enabled=slot->enabled;
  int repointed=strcmp(slot->code,c)!=0;
  /* Restore before the code is overwritten: the saved value belongs to the old target. */
  if(was_enabled && (!e || repointed)) cheat_slot_restore(engine,slot);
  if(!e || repointed) cheat_chain_release(engine,i);
  slot->enabled = e ? 1 : 0;
  snprintf(slot->code, sizeof slot->code, "%s", c);
  char parts[CHEAT_MAX_DIRECTIVES][CHEAT_DIRECTIVE_BYTES];
  int part_count=cheat_split_directives(slot->code,parts);
  cheat_parse(part_count>0?parts[0]:"", &slot->act);
  if(slot->enabled && (!was_enabled || repointed)){
    cheat_slot_capture(engine,slot);
    /* Menu directives carry no action; menu_rebuild reads them straight off this slot's code. */
    for(int p=1;p<part_count;p++){
      if(boot_line_is_menu(parts[p])) continue;
      CheatSlot *link=cheat_chain_alloc(engine,i);
      if(!link) break;
      link->enabled=1;
      snprintf(link->code,sizeof link->code,"%s",parts[p]);
      cheat_parse(link->code,&link->act);
      cheat_slot_capture(engine,link);
    }
  }
  menu_rebuild(engine);
  if(e && engine->loaded){                        /* fire the one-shot warps now */
    for(int p=0;p<part_count;p++){
      CheatAct one;
      cheat_parse(parts[p],&one);
      if(one.kind==CK_ROOM) gml_cheat_apply(&engine->vm, parts[p]);
    }
  }
}
/* Consumer passes run over both API-provided overrides and boot overrides. Boot entries remain
 * active when the caller resets the API-provided slots.
 *
 * A scope that can be false is an edge, not a filter. Skipping the write is enough to stop forcing
 * a value, but not to give the previous one back, and content that read its own setting once at
 * startup never writes it again — so a scope going false has to put the captured value back on
 * that same frame, exactly once. `applied` remembers whether this slot owes that. An un-scoped
 * directive never leaves the true branch and reaches none of it. */
static int cheat_slot_is_room_owned(const CheatAct *a){
  return a->kind==CK_GARR && gml_room_owned_global(a->obj);
}
static void cheat_sticky_pass(AnygmEngine *engine,CheatSlot *arr, int n, int channel_on,
                              int room_owned_only){
  for(int i=0;i<n;i++){
    CheatSlot *slot=&arr[i];
    const CheatAct *a=&slot->act;
    if(a->kind==CK_ROOM || a->kind==CK_NONE) continue;
    if(room_owned_only && !cheat_slot_is_room_owned(a)) continue;
    int applies=channel_on && slot->enabled && cheat_scope_ok(engine,a,0);
    if(applies){
      if(a->kind==CK_SCRIPT){
        if(!slot->applied) (void)gml_vm_run_script_named(&engine->vm,a->obj);
        slot->applied=1;
        continue;
      }
      /* Capture once per content load, before the first write: what is being preserved is the
       * value the content itself computed, not one an earlier arming already replaced. */
      if(!slot->saved_valid) cheat_slot_capture(engine,slot);
      slot->applied=1;
      cheat_apply_one(engine,a);
    } else if(slot->applied){
      cheat_slot_restore_keep(engine,slot,1);
      slot->applied=0;
    }
  }
}
static void cheat_gameres_pass(AnygmEngine *engine,CheatSlot *arr,int n,int channel_on){
  for(int i=0;i<n;i++){
    CheatSlot *slot=&arr[i];
    if(!slot->act.scope_gameres || slot->act.kind==CK_NONE) continue;
    int applies=channel_on && slot->enabled && cheat_scope_ok(engine,&slot->act,0);
    if(applies){
      if(!slot->saved_valid) cheat_slot_capture(engine,slot);
      slot->applied=1;
      cheat_apply_one(engine,&slot->act);
    } else if(slot->applied){
      cheat_slot_restore_keep(engine,slot,1);
      slot->applied=0;
    }
  }
}
/* Re-apply un-scoped freeze cheats — called every frame after the game step. Alarm pauses are
 * rebuilt here rather than accumulated: they are the one directive that changes what the engine
 * does instead of what a variable holds, so the table has to describe the cheats enabled right
 * now. Clearing first is what makes disarming free — no captured value, nothing to put back. */
/* Entering a room rewrites every room-owned global from that room's record, so a value captured
 * in one room is not the value the next one would have held. Dropping those captures on a room
 * change makes the next pass read the incoming room's own numbers before it overwrites them; the
 * capture is taken before the write, so what is preserved is still the content's. Targets the room
 * does not own — an object's own variable, a surface it created — are set once by the content and
 * keep the reading they already have. */
static void cheat_room_scope_refresh(CheatSlot *arr,int n){
  for(int i=0;i<n;i++){
    CheatSlot *slot=&arr[i];
    if(!slot->saved_valid || slot->act.kind!=CK_GARR) continue;
    if(gml_room_owned_global(slot->act.obj)) slot->saved_valid=0;
  }
}
/* Room-owned overrides have to be settled before the frame's geometry is read rather than after
 * it. Entering a room rewrites the view record a declaration is overriding, and the ordinary pass
 * runs past the point where the presented size is decided, so the frame a room is entered on would
 * publish the room's own numbers and the frame after it would publish the override — a resolution
 * that flickers once per room change. Self-guarded on the room, so calling it costs a comparison. */
void engine_overrides_room_scope_apply(AnygmEngine *engine){
  if(!engine || engine->vm.room_index==engine->override_room) return;
  engine->override_room=engine->vm.room_index;
  cheat_room_scope_refresh(engine->cheats,engine->cheat_count);
  cheat_room_scope_refresh(engine->boot_cheats,engine->boot_cheat_count);
  cheat_sticky_pass(engine,engine->cheats,engine->cheat_count,1,1);
  cheat_sticky_pass(engine,engine->boot_cheats,engine->boot_cheat_count,
                    engine_boot_cheats_active(engine)>0,1);
}
void apply_sticky_cheats(AnygmEngine *engine){
  gml_alarm_pause_reset(&engine->vm);
  engine_overrides_room_scope_apply(engine);
  cheat_sticky_pass(engine,engine->cheats, engine->cheat_count, 1, 0);
  /* Boot slots are walked whole rather than up to the active count, and the channel is passed as a
   * gate instead: a content-override channel switched off mid-run still has to hand back what it
   * forced, and engine_boot_cheats_active reports zero the moment it is switched off — walking to
   * that count would skip the very slots that owe a restore. */
  cheat_sticky_pass(engine,engine->boot_cheats, engine->boot_cheat_count,
                    engine_boot_cheats_active(engine)>0, 0);
}
/* Presentation-scoped targets must settle before geometry is read. The ordinary sticky pass runs
 * after Step so simulation freezes observe the usual event boundary; using this narrow pass at
 * the start of a frame lets a live presentation switch affect that same frame without moving any
 * unscoped gameplay write earlier. */
void engine_overrides_presentation_apply(AnygmEngine *engine){
  if(!engine) return;
  cheat_gameres_pass(engine,engine->cheats,engine->cheat_count,1);
  cheat_gameres_pass(engine,engine->boot_cheats,engine->boot_cheat_count,
                     engine_boot_cheats_active(engine)>0);
}
/* A state restores VM and renderer fields, but presentation policy belongs to the live host. Read
 * the current targets before those sections are replaced so a state made under the opposite scope
 * can be reconciled without importing its window or application-surface geometry. */
void engine_overrides_prepare_state_load(AnygmEngine *engine){
  if(!engine) return;
  for(int table=0;table<2;table++){
    CheatSlot *slots=table?engine->boot_cheats:engine->cheats;
    int count=table?engine->boot_cheat_count:engine->cheat_count;
    for(int i=0;i<count;i++)
      if(slots[i].enabled && slots[i].act.scope_gameres && !slots[i].saved_valid)
        cheat_slot_capture(engine,&slots[i]);
  }
}
/* Loading a state restores the values a scoped override wrote into the one being saved, so every
 * slot that could have written owes an answer again. Marking them applied lets the next pass make
 * it: the scope decides, and a directive that no longer applies gives its captured value back. */
void engine_overrides_note_state_load(AnygmEngine *engine){
  if(!engine) return;
  for(int i=0;i<engine->cheat_count;i++)
    if(engine->cheats[i].saved_valid || engine->cheats[i].act.kind==CK_SCRIPT)
      engine->cheats[i].applied=1;
  for(int i=0;i<engine->boot_cheat_count;i++)
    if(engine->boot_cheats[i].saved_valid || engine->boot_cheats[i].act.kind==CK_SCRIPT)
      engine->boot_cheats[i].applied=1;
  engine_overrides_presentation_apply(engine);
}
static void cheat_monitor_pass(AnygmEngine *engine,const CheatSlot *slots,int count){
  for(int i=0;i<count;i++){
    if(!slots[i].enabled || !cheat_scope_ok(engine,&slots[i].act,2)) continue;
    cheat_apply_one(engine,&slots[i].act);
  }
}
/* A frontend monitor-size transition is an edge, not a freeze. Content that cached its startup
 * geometry can opt into exactly the writes it needs without rerunning initialization or resetting
 * the game. */
void apply_monitor_overrides(AnygmEngine *engine){
  if(!engine->monitor_override_pending) return;
  engine->monitor_override_pending=0;
  cheat_monitor_pass(engine,engine->cheats,engine->cheat_count);
  cheat_monitor_pass(engine,engine->boot_cheats,engine_boot_cheats_active(engine));
  engine->fps_room=-1;
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
  cheat_aspect_pass(engine,engine->boot_cheats, engine_boot_cheats_active(engine));
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
      || cheat_compositor_pass(engine,engine->boot_cheats, engine_boot_cheats_active(engine));
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
      || cheat_center_target_pass(engine,engine->boot_cheats, engine_boot_cheats_active(engine));
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
      || cheat_wide_gameplay_pass(engine,engine->boot_cheats, engine_boot_cheats_active(engine));
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
  if(r < 0) r = cheat_route_pass(engine,engine->boot_cheats, engine_boot_cheats_active(engine), obj, suffix);
  return r < 0 ? GMC_ASPECT_DRAW_DEFAULT : r;
}
/* Fresh press this frame from either the RetroPad button or a keyboard key. */
static int menu_edge(AnygmEngine *engine,int pad, int vk){
  int cur  = engine->pad_current[0][pad]  | ((vk>=0&&vk<NKEY)?(engine->key_current[vk]|engine->hardware_key_current[vk]|engine->event_vk_current[vk]):0);
  int prev = engine->pad_previous[0][pad] | ((vk>=0&&vk<NKEY)?(engine->key_previous[vk]|engine->hardware_key_previous[vk]|engine->event_vk_previous[vk]):0);
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
  if(engine->pad_current[0][btn] && !engine->pad_previous[0][btn] && engine->vm.pending_room < 0){
    int room = engine->vm.room_index, ord = -1;
    for(int i=0;i<engine->win.n_room_order;i++) if((int)engine->win.room_order[i]==room){ ord=i; break; }
    if(ord>=0 && ord+1<engine->win.n_room_order) gml_vm_goto_room_order(&engine->vm, ord+1);
  }
}


/* "1,2,4,5" or "1-4,9": in the listed room indices, A, B, or Start advances to the next room in
 * play order. The list comes from the GML_INTROSKIP development setting, or — when the setting is
 * absent — from the loaded content's introskip| anchor directive, which follows the
 * content-override switch like every other content directive. */
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
  if(engine->introskip_enabled < 0){
    const char *setting=anygm_host_development_setting(&engine->host,"GML_INTROSKIP");
    if(!setting || !setting[0]){
      int active=engine_boot_cheats_active(engine);
      for(int i=0;i<active && (!setting || !setting[0]);i++)
        if(engine->boot_cheats[i].enabled &&
           !strncmp(engine->boot_cheats[i].code,"introskip|",10))
          setting=engine->boot_cheats[i].code+10;
    }
    introskip_parse(engine,setting);
  }
  if(!engine->introskip_enabled) return;
  int room = engine->vm.room_index;
  if(room < 0 || room >= 1024) return;
  if(!(engine->introskip_set[room>>3] & (1u<<(room&7)))) return;   /* not a listed intro room */
  int a = engine->pad_current[0][ANYGM_PAD_FACE_RIGHT] && !engine->pad_previous[0][ANYGM_PAD_FACE_RIGHT];
  int b = engine->pad_current[0][ANYGM_PAD_FACE_BOTTOM] && !engine->pad_previous[0][ANYGM_PAD_FACE_BOTTOM];
  int start = engine->pad_current[0][ANYGM_PAD_START] && !engine->pad_previous[0][ANYGM_PAD_START];
  if((a||b||start) && engine->vm.pending_room < 0){
    int ord=-1; for(int i=0;i<engine->win.n_room_order;i++) if((int)engine->win.room_order[i]==room){ ord=i; break; }
    if(ord>=0 && ord+1<engine->win.n_room_order) gml_vm_goto_room_order(&engine->vm, ord+1);
  }
}
