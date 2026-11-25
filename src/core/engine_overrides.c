/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Runtime override parsing, persistent override policy, development menu,
 * room-skip hooks, and intro-skip hooks. */
#include "engine_internal.h"
#include "anygm_host.h"
#include "gml_builtin.h"

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

static bool parse_room_list(const char **cursor,uint8_t *rooms){
  const char *scan=*cursor;
  for(;;){
    int first,last;
    if(!parse_nonnegative_index(&scan,&first) || first>=ENGINE_ROOM_RULE_LIMIT) return false;
    last=first;
    if(*scan=='-'){
      scan++;
      if(!parse_nonnegative_index(&scan,&last) || last<first || last>=ENGINE_ROOM_RULE_LIMIT)
        return false;
    }
    for(int room=first;room<=last;room++) rooms[room/8]|=(uint8_t)(1u<<(room%8));
    if(*scan!=','){
      if(*scan!='|') return false;
      *cursor=scan+1;
      return true;
    }
    scan++;
  }
}

int core_opt_diagnostic_start_room(AnygmEngine *engine,int *room_idx){
  const char *setting=anygm_host_development_setting(
    &engine->host,"GML_DIAGNOSTIC_START_ROOM");
  if(!setting || !setting[0]) return 0;
  const char *cursor=setting;
  int room=-1;
  if(!parse_nonnegative_index(&cursor,&room) || *cursor ||
     room>=gml_room_count(&engine->win)) return -1;
  *room_idx=room;
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
 *   set|obj|var|V               one-shot: assign one instance variable on every matching instance
 *   name=V   | name[i]=V        freeze global ARRAY element   (GM8 / indexed reads)
 *   $name=V                     freeze global SCALAR          (GMS scalar reads; avoids V_ARR->0)
 *   obj:var=V | obj:var[i]=V    freeze a numeric var on every instance of object `obj`
 *   alarmpause|obj|i            hold alarm i still on every instance of `obj` and its descendants
 *   drawhold|name|V             hold global `name` at V for the length of one frame's drawing
 *   call|script                 invoke a zero-argument content script once after the first Step
 *   setcall|global|V|script      hold a scalar setting; recalculate only on changes or restoration
 *   afterdraw|obj|script|V      invoke a script with one numeric argument after ordinary Draw
 *   listset|target|var[i]|j|V   keep a DS-list item at V; target is an object or `global`
 *   camera[LIST]:field=V        write x, y, width, or height on selected live camera handles
 *   surface|obj|var|W|H         resize surfaces named by an instance variable
 *   surface_canvas|obj|var|W|H  centred native HUD and full-width application composition
 *   monitorview|H|MIN|MAX       declare a monitor-derived logical view (ratios use W:H)
 *   ?crt crtmode|W|H|HZ         declare the padded raster and refresh handed to a CRT host
 *   ?crt windowraster|W|H       select the effective content window without changing its request
 *   ?crt roomspeed|rooms|FPS    replace the cadence of the listed rooms without writing content
 *   roomskip|rooms|trigger|next     advance in play order or to an explicit room
 *   roomalarm|rooms|trigger|obj|i   advance one content-owned alarm
 *   roomcounter|rooms|trigger|obj|phase=V|counter=N   adjust a content-owned counter
 *   @field=V                    engine state and data-declared aspect behavior
 *   obj@suffix->mode            event route: full_view | visible_view | backdrop | default
 *
 * A line may carry a SCOPE prefix:
 *   ?global.name ...            only while that content global is non-zero (may follow another
 *                               scope, e.g. `?gameres ?global.overlay @present_shift_y=...`)
 *   ?aspect      ...            only while an Aspect Ratio Force is active
 *   ?aspect=4:3  ...            only while that specific force mode is active (4:3|16:9|21:9)
 *   ?monitor     ...            once, on each live virtual-monitor size change
 *   ?gameres     ...            only while the logical-raster presentation is selected
 *   ?crt         ...            only while effective CRT adjustment is enabled
 *   ?crt=horizontal / vertical  limit draw holds and window rasters to the effective CRT axes
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
 *   $port_w  $port_h            current view_wport/view_hport, independent of camera extent
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
                                  char parts[CHEAT_MAX_DIRECTIVES][CHEAT_DIRECTIVE_BYTES],
                                  bool strict){
  int n=0;
  const char *s=code?code:"";
  while(*s){
    while(*s==';'||*s==' '||*s=='\t') s++;
    if(!*s) break;
    if(n==CHEAT_MAX_DIRECTIVES) return strict?-1:n;
    const char *end=strchr(s,';');
    size_t length=end?(size_t)(end-s):strlen(s);
    while(length && (s[length-1]==' '||s[length-1]=='\t'||s[length-1]=='\r')) length--;
    if(length){
      if(length>=CHEAT_DIRECTIVE_BYTES){
        if(strict) return -1;
        length=CHEAT_DIRECTIVE_BYTES-1;
      }
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
    /* A dot separates the "global." prefix from the name it addresses; every other value token is
     * a bare name, so accepting it here costs nothing and keeps one scanner. */
    s++; const char *n=s; while(*s && (CHEAT_NAMECH(*s) || *s=='.')) s++;
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
    else if(L==6 && !strncmp(n,"port_w",6)) v->tok=TK_PORT_W;
    else if(L==6 && !strncmp(n,"port_h",6)) v->tok=TK_PORT_H;
    /* Global values are read when presentation directives are evaluated, so dependent
     * targets follow the current frame's value. */
    else if(L>7 && !strncmp(n,"global.",7)){
      size_t len=L-7; if(len>=sizeof v->name) len=sizeof v->name-1;
      memcpy(v->name,n+7,len); v->name[len]=0;
      v->tok=TK_GLOBAL;
    }
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
    case TK_PORT_W: x=gml_global_arr(&engine->vm,"view_wport",0); break;
    case TK_PORT_H: x=gml_global_arr(&engine->vm,"view_hport",0); break;
    case TK_GLOBAL: x=gml_global_num(&engine->vm,v->name); break;
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
    if(!parse_nonnegative_index(&s,&first) || first>=GML_CAMERA_OVERRIDE_COUNT) return 0;
    last=first;
    if(*s=='-'){
      s++;
      if(!parse_nonnegative_index(&s,&last) || last<first || last>=GML_CAMERA_OVERRIDE_COUNT) return 0;
    }
    for(int camera=first;camera<=last;camera++) *mask|=UINT64_C(1)<<camera;
    if(*s==',') s++;
    else if(*s!=']') return 0;
  }
  if(*s!=']' || !*mask) return 0;
  *cursor=s+1;
  return 1;
}

static void cheat_parse_body(const char *code, CheatAct *a){
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
  } else if(!strncmp(s,"?crt",4) && (s[4]==' ' || s[4]=='\t' || s[4]=='=')){
    s+=4;
    a->scope_crt=CRT_SCOPE_ANY;
    if(*s=='='){
      if(!strncmp(s,"=horizontal",11)){ a->scope_crt=CRT_SCOPE_HORIZONTAL; s+=11; }
      else if(!strncmp(s,"=vertical",9)){ a->scope_crt=CRT_SCOPE_VERTICAL; s+=9; }
      else return;
      if(*s!=' ' && *s!='\t') return;
    }
    while(*s==' '||*s=='\t') s++;
  }
  /* A value scope may follow the first scope. A zero global disables the
   * directive and restores its captured destination value. */
  if(!strncmp(s,"?global.",8)){
    s+=8; const char *n=s; while(*s && CHEAT_NAMECH(*s)) s++;
    size_t L=(size_t)(s-n);
    if(L==0 || L>=sizeof a->scope_global){ a->kind=CK_NONE; return; }
    memcpy(a->scope_global,n,L); a->scope_global[L]=0;
    while(*s==' '||*s=='\t') s++;
  }
  if(!strncmp(s,"?instance.",10)){
    s+=10; const char *name=s;while(*s && CHEAT_NAMECH(*s)) s++;
    size_t length=(size_t)(s-name);
    if(!length || length>=sizeof a->scope_instance) return;
    memcpy(a->scope_instance,name,length);a->scope_instance[length]=0;
    while(*s==' ' || *s=='\t') s++;
  }
  if(!strncmp(s,"sync|",5)){ a->scope_sync=1; s+=5; }
  if(!strncmp(s,"windowraster|",13)){
    int width,height;
    s+=13;
    if(!a->scope_crt || a->scope_sync || a->scope_global[0] || a->scope_instance[0] ||
       !parse_nonnegative_index(&s,&width) || *s++!='|' ||
       !parse_nonnegative_index(&s,&height) || *s ||
       width<1 || width>FB_MAX_W || height<1 || height>FB_MAX_H) return;
    a->crt_width=width;
    a->crt_height=height;
    a->kind=CK_WINDOW_RASTER;
    return;
  }
  if(!strncmp(s,"crtmode|",8) || !strncmp(s,"roomspeed|",10)){
    /* Host policy follows the CRT selection alone. A content condition could change the mode in
     * the middle of a room. */
    if(a->scope_crt!=CRT_SCOPE_ANY || a->scope_sync || a->scope_global[0] ||
       a->scope_instance[0]) return;
    if(s[0]=='c'){
      int width,height,hz;
      s+=8;
      if(!parse_nonnegative_index(&s,&width) || *s++!='|' ||
         !parse_nonnegative_index(&s,&height) || *s++!='|' ||
         !parse_nonnegative_index(&s,&hz) || *s ||
         width<16 || width>640 || height<16 || height>480 || hz<2 || hz>300) return;
      a->crt_width=width; a->crt_height=height; a->val.lit=hz;
      a->kind=CK_CRT_MODE; return;
    }
    uint8_t rooms[ENGINE_ROOM_RULE_LIMIT/8]={0};
    const char *list=s+=10;
    if(!parse_room_list(&s,rooms)) return;
    size_t length=(size_t)(s-1-list);
    int fps;
    if(length+1u>=sizeof a->var || !parse_nonnegative_index(&s,&fps) || *s || fps<1 || fps>300)
      return;
    memcpy(a->var,list,length+1u);   /* retain the separator that closes a room list */
    a->var[length+1u]=0;
    a->val.lit=fps;
    a->kind=CK_ROOM_SPEED; return;
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
  /* A draw hold temporarily substitutes a global only during the draw pass.
   * Other execution phases retain the pre-draw value. */
  if(!strncmp(s,"drawhold|",9)){
    s+=9; const char *bar=strchr(s,'|');
    if(!bar || bar==s || (size_t)(bar-s)>=sizeof a->obj){ a->kind=CK_NONE; return; }
    for(const char *scan=s;scan<bar;scan++)
      if(!CHEAT_NAMECH(*scan)){ a->kind=CK_NONE; return; }
    memcpy(a->obj,s,(size_t)(bar-s)); a->obj[bar-s]=0; s=bar+1;
    if(!*s){ a->kind=CK_NONE; return; }
    cheat_parse_val(s,&a->val);
    a->kind=CK_DRAW_HOLD; return;
  }
  if(!strncmp(s,"afterdraw|",10)){
    s+=10;
    char *names[]={a->obj,a->var};
    for(int i=0;i<2;i++){
      const char *start=s;
      while(*s && CHEAT_NAMECH(*s)) s++;
      size_t length=(size_t)(s-start);
      if(!length || length>=64 || *s!='|'){ a->kind=CK_NONE; return; }
      memcpy(names[i],start,length); names[i][length]=0; s++;
    }
    char *end=NULL;
    a->val.lit=strtod(s,&end);
    if(end==s || *end || !isfinite(a->val.lit) || a->scope_monitor){
      a->kind=CK_NONE; return;
    }
    a->kind=CK_AFTER_DRAW; return;
  }
  if(!strncmp(s,"setcall|",8)){
    s+=8; const char *name=s;
    while(*s && CHEAT_NAMECH(*s)) s++;
    size_t length=(size_t)(s-name);
    if(!length || length>=sizeof a->obj || *s!='|') return;
    memcpy(a->obj,name,length); a->obj[length]=0;
    char *end=NULL;
    a->val.lit=strtod(s+1,&end);
    if(end==s+1 || *end!='|' || !isfinite(a->val.lit)) return;
    s=end+1; name=s;
    while(*s && CHEAT_NAMECH(*s)) s++;
    length=(size_t)(s-name);
    if(!length || length>=sizeof a->var || (*s && *s!='|')) return;
    memcpy(a->var,name,length); a->var[length]=0;
    if(*s=='|'){
      char *names[]={a->refresh_object,a->refresh_script};
      for(int i=0;i<2;i++){
        name=++s;
        while(*s && CHEAT_NAMECH(*s)) s++;
        length=(size_t)(s-name);
        if(!length || length>=64 || (i==0?*s!='|':*s!=0)) return;
        memcpy(names[i],name,length); names[i][length]=0;
      }
    }
    a->kind=CK_GSCALAR_CALL; return;
  }
  if(!strncmp(s,"layer_x|",8) || !strncmp(s,"layer_y|",8)){
    a->idx=s[6]=='y';s+=8;
    char *names[]={a->obj,a->var};
    for(int i=0;i<2;i++){
      const char *name=s;while(*s && CHEAT_NAMECH(*s)) s++;
      size_t length=(size_t)(s-name);
      if(!length || length>=64 || *s!='|') return;
      memcpy(names[i],name,length);names[i][length]=0;s++;
    }
    if(!*s) return;
    cheat_parse_val(s,&a->val);a->kind=CK_LAYER_OFFSET;return;
  }
  if(!strncmp(s,"call|",5)){
    s+=5; const char *name=s; while(*s && CHEAT_NAMECH(*s)) s++;
    size_t length=(size_t)(s-name);
    if(length==0 || length>=sizeof a->obj || *s){ a->kind=CK_NONE; return; }
    memcpy(a->obj,name,length); a->obj[length]=0;
    a->kind=CK_SCRIPT; return;
  }
  if(!strncmp(s,"listset|",8)){
    s+=8; const char *bar=strchr(s,'|');
    if(!bar || bar==s || (size_t)(bar-s)>=sizeof a->obj){ a->kind=CK_NONE; return; }
    for(const char *scan=s;scan<bar;scan++)
      if(!CHEAT_NAMECH(*scan)){ a->kind=CK_NONE; return; }
    memcpy(a->obj,s,(size_t)(bar-s)); a->obj[bar-s]=0; s=bar+1;
    const char *name=s; while(*s && CHEAT_NAMECH(*s)) s++;
    size_t length=(size_t)(s-name);
    if(length==0 || length>=sizeof a->var){ a->kind=CK_NONE; return; }
    memcpy(a->var,name,length); a->var[length]=0;
    if(*s=='['){
      s++;
      if(!parse_nonnegative_index(&s,&a->idx2) || *s!=']'){
        a->kind=CK_NONE; return;
      }
      s++;
      a->has_index=1;
    }
    if(*s!='|'){ a->kind=CK_NONE; return; }
    s++;
    if(!parse_nonnegative_index(&s,&a->idx) || *s!='|'){
      a->kind=CK_NONE; return;
    }
    s++;
    if(!*s || strchr(s,'|')){ a->kind=CK_NONE; return; }
    cheat_parse_val(s,&a->val);
    a->kind=CK_LIST_SET; return;
  }
  if(!strncmp(s,"set|",4)){
    s+=4; const char *bar=strchr(s,'|');
    if(!bar || bar==s || (size_t)(bar-s)>=sizeof a->obj){ a->kind=CK_NONE; return; }
    memcpy(a->obj,s,(size_t)(bar-s)); a->obj[bar-s]=0; s=bar+1;
    bar=strchr(s,'|');
    if(!bar || bar==s || (size_t)(bar-s)>=sizeof a->var){ a->kind=CK_NONE; return; }
    memcpy(a->var,s,(size_t)(bar-s)); a->var[bar-s]=0; s=bar+1;
    if(!*s || strchr(s,'|')){ a->kind=CK_NONE; return; }
    cheat_parse_val(s,&a->val); a->kind=CK_INST_SET; return;
  }
  if(!strncmp(s,"surface|",8) || !strncmp(s,"surface_canvas|",15)){
    int canvas=!strncmp(s,"surface_canvas|",15);
    s+=canvas?15:8; const char *bar=strchr(s,'|');
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
    if(canvas){
      const char *first_end=first,*second_end=bar+1;
      int width=0,height=0;
      if(!parse_nonnegative_index(&first_end,&width) || *first_end ||
         !parse_nonnegative_index(&second_end,&height) || *second_end){
        a->kind=CK_NONE; return;
      }
    }
    cheat_parse_val(first,&a->val); cheat_parse_val(bar+1,&a->val2);
    if(canvas && (!a->scope_aspect || a->scope_global[0] ||
       a->val.tok!=TK_LIT || a->val2.tok!=TK_LIT || a->val.nop || a->val2.nop ||
       !isfinite(a->val.lit) || !isfinite(a->val2.lit) ||
       a->val.lit<16 || a->val.lit>FB_MAX_W || a->val2.lit<16 || a->val2.lit>FB_MAX_H ||
       floor(a->val.lit)!=a->val.lit || floor(a->val2.lit)!=a->val2.lit)){
      a->kind=CK_NONE; return;
    }
    a->kind=canvas?CK_SURFACE_CANVAS:CK_SURFACE; return;
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
    else if(!strcmp(f,"present_shift_x")) a->eng=EF_PRESENT_SHIFT_X;
    else if(!strcmp(f,"present_shift_y")) a->eng=EF_PRESENT_SHIFT_Y;
    else if(!strcmp(f,"present_crop_left")) a->eng=EF_PRESENT_CROP_LEFT;
    else if(!strcmp(f,"present_crop_top")) a->eng=EF_PRESENT_CROP_TOP;
    else if(!strcmp(f,"present_crop_right")) a->eng=EF_PRESENT_CROP_RIGHT;
    else if(!strcmp(f,"present_crop_bottom")) a->eng=EF_PRESENT_CROP_BOTTOM;
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
      else if(!strcmp(s,"visible_view")) a->route_mode=GMC_ASPECT_DRAW_VISIBLE_VIEW;
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
static void cheat_parse(const char *code,CheatAct *a){
  cheat_parse_body(code,a);
  if(a->scope_crt>CRT_SCOPE_ANY && a->kind!=CK_WINDOW_RASTER && a->kind!=CK_DRAW_HOLD)
    a->kind=CK_NONE;
  /* Synchronizing layout recomputes on release instead of restoring a stale coordinate.
   * Restrict it to explicit presentation scopes and numeric instance/layer operations. */
  if(a->scope_sync && (!(a->scope_crt || a->scope_gameres) ||
     (a->kind!=CK_INST && a->kind!=CK_LAYER_OFFSET))) a->kind=CK_NONE;
  if(a->scope_sync && a->kind==CK_INST && strcmp(a->var,"x") && strcmp(a->var,"y"))
    a->kind=CK_NONE;
  if(a->kind==CK_LAYER_OFFSET && !a->scope_sync) a->kind=CK_NONE;
}
/* Apply a setting through the content's own zero-argument recalculation script, only when the
 * stored value changes. Restoration uses the same path; stable frames incur no script call. */
static void cheat_setting_apply(AnygmEngine *engine,const CheatAct *a,double value){
  double previous=gml_global_num(&engine->vm,a->obj);
  gml_set_global_scalar(&engine->vm,a->obj,value);
  if(previous!=value){
    (void)gml_vm_run_script_named(&engine->vm,a->var);
    if(a->refresh_object[0]){
      int object=gml_object_index_by_name(&engine->vm,a->refresh_object);
      GmlInstance *receiver=object>=0?gml_find_instance(&engine->vm,object):NULL;
      if(receiver)
        (void)gml_vm_run_script_on_instance(&engine->vm,a->refresh_script,receiver,NULL,0);
    }
  }
}
static void cheat_apply_one(AnygmEngine *engine,const CheatAct *a){
  switch(a->kind){
    case CK_GSCALAR: gml_set_global_scalar(&engine->vm, a->obj, cheat_val_eval(engine,&a->val)); break;
    case CK_GSCALAR_CALL: cheat_setting_apply(engine,a,a->val.lit); break;
    case CK_LAYER_OFFSET: {
      double handle=0,value=cheat_val_eval(engine,&a->val);
      if(!gml_inst_var_first_real(&engine->vm,a->obj,a->var,&handle) ||
         !isfinite(handle) || handle<0 || handle>INT_MAX || floor(handle)!=handle ||
         !isfinite(value)) break;
      GmlRtLayer *layer=gml_rt_layer_find(&engine->vm,(int)handle);
      if(!layer || (a->idx?layer->y:layer->x)==value) break;
      GmlVal args[]={vreal(handle),vreal(value)};
      (void)gml_builtin_call(&engine->vm,a->idx?"layer_y":"layer_x",args,2);
      break; }
    case CK_GARR:    gml_set_global_arr(&engine->vm, a->obj, a->idx, cheat_val_eval(engine,&a->val)); break;
    case CK_INST:
    case CK_INST_SET: gml_set_inst_var_all(&engine->vm, a->obj, a->var,
                                           cheat_val_eval(engine,&a->val)); break;
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
    case CK_LIST_SET: {
      int found=0;
      GmlVal list=vundef();
      if(!strcmp(a->obj,"global")){
        GmlVal *value=gml_varmap_get(&engine->vm.globals,a->var);
        if(value){ list=*value; found=1; }
      } else {
        int object=gml_object_index_by_name(&engine->vm,a->obj);
        GmlInstance *instance=object>=0?gml_find_instance(&engine->vm,object):NULL;
        if(instance) list=gml_inst_var_get_val(
          &engine->vm,vreal((double)instance->id),a->var,&found);
      }
      if(found && a->has_index) list=gml_arr_get(list,a->idx2);
      if(found && list.t==V_REAL && isfinite(list.d) && list.d>=0 && list.d<=INT_MAX &&
         floor(list.d)==list.d){
        GmlVal args[3]={list,vreal((double)a->idx),vreal(cheat_val_eval(engine,&a->val))};
        (void)gml_builtin_call(&engine->vm,"ds_list_replace",args,3);
      }
      break; }
    case CK_ENGINE: { int iv=(int)cheat_val_eval(engine,&a->val);
      switch(a->eng){
        case EF_PRESENT_SHIFT_X: engine->present_shift_x=iv; break;
        case EF_PRESENT_SHIFT_Y: engine->present_shift_y=iv; break;
        case EF_PRESENT_CROP_LEFT: engine->present_crop_left=iv; break;
        case EF_PRESENT_CROP_TOP: engine->present_crop_top=iv; break;
        case EF_PRESENT_CROP_RIGHT: engine->present_crop_right=iv; break;
        case EF_PRESENT_CROP_BOTTOM: engine->present_crop_bottom=iv; break;
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
static int cheat_scope_instance_ok(AnygmEngine *engine,const CheatAct *a){
  if(!a->scope_instance[0]) return 1;
  if(!engine->loaded) return 0;
  int object=gml_object_index_by_name(&engine->vm,a->scope_instance);
  return object>=0 && gml_find_instance(&engine->vm,object)!=NULL;
}
static bool cheat_crt_scope_ok(const AnygmEngine *engine,CheatCrtScope scope){
  return engine->config.present_logical_raster && engine->config.adjust_crt_tv &&
    (scope==CRT_SCOPE_ANY || (scope==CRT_SCOPE_VERTICAL)==engine_crt_is_vertical(engine));
}
/* phase: 0 = normal post-step sticky pass, 1 = aspect reshaping, 2 = monitor-change edge. */
static int cheat_scope_ok(AnygmEngine *engine,const CheatAct *a,int phase){
  if(!cheat_scope_instance_ok(engine,a)) return 0;
  /* Before content is loaded there is no global to read, and a directive that answers to one
   * has nothing to answer yet. */
  if(a->scope_global[0] &&
     (!engine->loaded || gml_global_num(&engine->vm,a->scope_global)==0.0)) return 0;
  if(a->scope_monitor) return phase==2;
  if(a->scope_aspect){
    if(phase!=1 || !engine->aspect_force_active) return 0;
    if(a->scope_mode && a->scope_mode!=engine->aspect_force_mode) return 0;
    return 1;
  }
  /* Read live rather than latched: the selection can change between two frames, and the pass that
   * gives the content its own numbers back is the same pass that took them. */
  if(a->scope_gameres) return phase==0 && engine->config.present_logical_raster!=0;
  if(a->scope_crt) return phase==0 && cheat_crt_scope_ok(engine,a->scope_crt);
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
  int n=cheat_split_directives(code,parts,false);
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
    int np=cheat_split_directives(arr[i].code,parts,false);
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

static int state_reserve_parse(const char *code,size_t *bytes){
  if(strncmp(code,"statereserve|",13)) return 0;
  const char *scan=code+13;
  const size_t limit=(size_t)1u<<30;
  size_t value=0;
  if(!*scan) return 0;
  for(;*scan;scan++){
    if(*scan<'0' || *scan>'9') return 0;
    unsigned digit=(unsigned)(*scan-'0');
    if(value>(limit-digit)/10u) return 0;
    value=value*10u+digit;
  }
  if(!value) return 0;
  *bytes=value;
  return 1;
}

static unsigned override_pad_button_bit(const char *name,size_t length){
  static const struct { const char *name; int button; } names[]={
    {"b",ANYGM_PAD_FACE_BOTTOM},{"a",ANYGM_PAD_FACE_RIGHT},
    {"y",ANYGM_PAD_FACE_LEFT},{"x",ANYGM_PAD_FACE_TOP},
    {"start",ANYGM_PAD_START},{"select",ANYGM_PAD_SELECT},
    {"up",ANYGM_PAD_UP},{"down",ANYGM_PAD_DOWN},
    {"left",ANYGM_PAD_LEFT},{"right",ANYGM_PAD_RIGHT},
    {"l",ANYGM_PAD_LEFT_SHOULDER},{"r",ANYGM_PAD_RIGHT_SHOULDER},
    {"l2",ANYGM_PAD_LEFT_TRIGGER},{"r2",ANYGM_PAD_RIGHT_TRIGGER},
    {"l3",ANYGM_PAD_LEFT_STICK},{"r3",ANYGM_PAD_RIGHT_STICK}
  };
  for(size_t i=0;i<sizeof names/sizeof names[0];i++)
    if(strlen(names[i].name)==length && !memcmp(name,names[i].name,length))
      return 1u<<(unsigned)names[i].button;
  return 0;
}

enum { ROOM_RULE_NONE, ROOM_RULE_SKIP, ROOM_RULE_COUNTER, ROOM_RULE_ALARM };

typedef struct {
  unsigned pads[ANYGM_MAX_GAMEPADS];
  uint8_t keys[(ANYGM_KEY_LAST+7)/8];
  bool room_enter;
} RoomTrigger;

typedef struct {
  int kind,destination,alarm;
  uint8_t rooms[ENGINE_ROOM_RULE_LIMIT/8];
  RoomTrigger trigger;
  char object[64],guard[64],counter[64];
  int32_t expected,value;
} RoomRule;

static int room_rule_kind(const char *code){
  if(!strncmp(code,"roomskip|",9)) return ROOM_RULE_SKIP;
  if(!strncmp(code,"roomcounter|",12)) return ROOM_RULE_COUNTER;
  if(!strncmp(code,"roomalarm|",10)) return ROOM_RULE_ALARM;
  return ROOM_RULE_NONE;
}

static bool room_rule_contains(const RoomRule *rule,int room){
  return room>=0 && room<ENGINE_ROOM_RULE_LIMIT &&
    (rule->rooms[room/8] & (1u<<(room%8)))!=0;
}

static int room_key_code(const char *name,size_t length){
  static const struct { const char *name; int code; } keys[]={
    {"escape",ANYGM_KEY_ESCAPE},{"enter",ANYGM_KEY_RETURN},{"space",ANYGM_KEY_SPACE},
    {"backspace",ANYGM_KEY_BACKSPACE},{"tab",ANYGM_KEY_TAB},{"delete",ANYGM_KEY_DELETE},
    {"insert",ANYGM_KEY_INSERT},{"home",ANYGM_KEY_HOME},{"end",ANYGM_KEY_END},
    {"pageup",ANYGM_KEY_PAGEUP},{"pagedown",ANYGM_KEY_PAGEDOWN},
    {"up",ANYGM_KEY_UP},{"down",ANYGM_KEY_DOWN},{"left",ANYGM_KEY_LEFT},{"right",ANYGM_KEY_RIGHT},
    {"lshift",ANYGM_KEY_LSHIFT},{"rshift",ANYGM_KEY_RSHIFT},
    {"lctrl",ANYGM_KEY_LCTRL},{"rctrl",ANYGM_KEY_RCTRL},
    {"lalt",ANYGM_KEY_LALT},{"ralt",ANYGM_KEY_RALT},
  };
  if(length==1 && ((*name>='a' && *name<='z') || (*name>='0' && *name<='9')))
    return (unsigned char)*name;
  if(length>=2 && length<=3 && *name=='f' && name[1]>='1' && name[1]<='9'){
    const char *number=name+1;
    int value;
    if(parse_nonnegative_index(&number,&value) && number==name+length && value<=12)
      return ANYGM_KEY_F1+value-1;
  }
  for(size_t i=0;i<sizeof keys/sizeof keys[0];i++)
    if(strlen(keys[i].name)==length && !memcmp(keys[i].name,name,length)) return keys[i].code;
  return -1;
}

static bool room_trigger_parse(const char **cursor,RoomTrigger *trigger){
  const char *scan=*cursor;
  if(!strncmp(scan,"room-enter|",11)){
    trigger->room_enter=true;
    *cursor=scan+11;
    return true;
  }
  for(;;){
    size_t length=strcspn(scan,",|");
    if(length>=6 && !memcmp(scan,"pad",3) && scan[3]>='1' &&
       (unsigned)(scan[3]-'1')<ANYGM_MAX_GAMEPADS && scan[4]==':'){
      unsigned port=(unsigned)(scan[3]-'1');
      unsigned bit=override_pad_button_bit(scan+5,length-5);
      if(!bit || (trigger->pads[port]&bit)) return false;
      trigger->pads[port]|=bit;
    } else if(length>4 && !memcmp(scan,"key:",4)){
      int key=room_key_code(scan+4,length-4);
      if(key<0) return false;
      unsigned bit=1u<<(key%8);
      if(trigger->keys[key/8]&bit) return false;
      trigger->keys[key/8]|=(uint8_t)bit;
    } else return false;
    scan+=length;
    if(*scan!=','){
      if(*scan!='|') return false;
      *cursor=scan+1;
      return true;
    }
    scan++;
  }
}

static bool room_field_name(const char **cursor,char *name,size_t capacity,char separator){
  const char *start=*cursor,*scan=start;
  while(CHEAT_NAMECH(*scan)) scan++;
  size_t length=(size_t)(scan-start);
  if(!length || length>=capacity || *scan!=separator) return false;
  memcpy(name,start,length);
  name[length]=0;
  *cursor=scan+1;
  return true;
}

static bool room_field_integer(const char **cursor,int32_t *value,char separator){
  const char *digits=**cursor=='-'?*cursor+1:*cursor;
  if(*digits<'0' || *digits>'9') return false;
  char *end;
  long long parsed=strtoll(*cursor,&end,10);
  if(parsed<INT32_MIN || parsed>INT32_MAX || *end!=separator) return false;
  *value=(int32_t)parsed;
  *cursor=end+(separator!=0);
  return true;
}

static bool room_rule_parse(const char *code,RoomRule *rule){
  int kind=room_rule_kind(code);
  if(!kind) return false;
  memset(rule,0,sizeof *rule);
  rule->kind=kind;
  const char *scan=strchr(code,'|')+1;
  if(!parse_room_list(&scan,rule->rooms) || !room_trigger_parse(&scan,&rule->trigger)) return false;
  if(kind==ROOM_RULE_SKIP){
    if(!strcmp(scan,"next")){
      rule->destination=-1;
      return true;
    }
    return parse_nonnegative_index(&scan,&rule->destination) && !*scan &&
      (!rule->trigger.room_enter || !room_rule_contains(rule,rule->destination));
  }
  if(rule->trigger.room_enter ||
     !room_field_name(&scan,rule->object,sizeof rule->object,'|')) return false;
  if(kind==ROOM_RULE_ALARM)
    return parse_nonnegative_index(&scan,&rule->alarm) && !*scan && rule->alarm<GML_ALARMS;
  return room_field_name(&scan,rule->guard,sizeof rule->guard,'=') &&
    room_field_integer(&scan,&rule->expected,'|') &&
    room_field_name(&scan,rule->counter,sizeof rule->counter,'=') &&
    strcmp(rule->guard,rule->counter) && room_field_integer(&scan,&rule->value,0);
}

static bool room_rule_same_target(const RoomRule *a,const RoomRule *b){
  return a->kind==b->kind && !memcmp(a->rooms,b->rooms,sizeof a->rooms) &&
    a->trigger.room_enter==b->trigger.room_enter &&
    !memcmp(a->trigger.pads,b->trigger.pads,sizeof a->trigger.pads) &&
    !memcmp(a->trigger.keys,b->trigger.keys,sizeof a->trigger.keys) &&
    a->alarm==b->alarm && a->expected==b->expected &&
    !strcmp(a->object,b->object) && !strcmp(a->guard,b->guard) && !strcmp(a->counter,b->counter);
}

static const char *keyboard_binding_keys(const char *code,unsigned *port){
  if(strncmp(code,"keybind|",8)) return NULL;
  const char *scan=code+8;
  if(*scan<'0' || *scan>'9') return NULL;
  unsigned selected=0;
  while(*scan>='0' && *scan<='9'){
    selected=selected*10u+(unsigned)(*scan++-'0');
    if(selected>ANYGM_MAX_GAMEPADS) return NULL;
  }
  if(!selected || *scan!='|') return NULL;
  if(port) *port=selected-1u;
  return scan+1;
}

static int keyboard_binding_key(const char **cursor){
  const char *scan=*cursor;
  if(*scan<'0' || *scan>'9') return -1;
  unsigned key=0;
  while(*scan>='0' && *scan<='9'){
    key=key*10u+(unsigned)(*scan++-'0');
    if(key>255u) return -1;
  }
  if(key<2u) return -1;
  *cursor=scan;
  return (int)key;
}

/* Auto restores the ordinary keyboard mapping. A nonempty mask replaces it. */
static int keyboard_binding_buttons(const char *scan,unsigned *buttons,bool *turbo){
  unsigned mask=0;
  bool repeat=false;
  if(strcmp(scan,"auto")){
    for(;;){
      size_t length=strcspn(scan,",|");
      unsigned bit=override_pad_button_bit(scan,length);
      if(!bit || (mask&bit)) return 0;
      mask|=bit;
      scan+=length;
      if(!*scan) break;
      if(*scan=='|'){
        if(strcmp(scan+1,"turbo")) return 0;
        repeat=true;
        break;
      }
      scan++;
    }
  }
  if(buttons) *buttons=mask;
  if(turbo) *turbo=repeat;
  return 1;
}

/* Boot parsing stores exactly one destination key per slot. */
static int keyboard_binding_parse(const char *code,int *virtual_key,unsigned *buttons,unsigned *port,
                                  bool *turbo){
  const char *scan=keyboard_binding_keys(code,port);
  if(!scan) return 0;
  int key=keyboard_binding_key(&scan);
  if(key<0 || *scan!='|' || !keyboard_binding_buttons(scan+1,buttons,turbo)) return 0;
  *virtual_key=key;
  return 1;
}

/* Compare destinations and conditions, never the value being assigned. Resolve
 * layers before any slot captures live state, runs a script, or builds a menu. */
static int boot_same_target(const CheatSlot *left,const CheatSlot *right){
  const CheatAct *a=&left->act,*b=&right->act;
  if(a->kind!=b->kind || a->scope_aspect!=b->scope_aspect ||
     a->scope_monitor!=b->scope_monitor || a->scope_gameres!=b->scope_gameres ||
     a->scope_crt!=b->scope_crt || a->scope_sync!=b->scope_sync ||
     a->scope_mode!=b->scope_mode || strcmp(a->scope_global,b->scope_global) ||
     strcmp(a->scope_instance,b->scope_instance)) return 0;
  if(a->kind==CK_GSCALAR_CALL) return !strcmp(a->obj,b->obj);
  if(a->kind!=CK_NONE)
    return !strcmp(a->obj,b->obj) && !strcmp(a->var,b->var) &&
      a->idx==b->idx && a->idx2==b->idx2 && a->has_index==b->has_index &&
      a->eng==b->eng && a->camera_field==b->camera_field && a->camera_mask==b->camera_mask;
  static const char *singletons[]={"ostype|","renderer|","gamepad|","menu|"};
  for(size_t i=0;i<sizeof singletons/sizeof singletons[0];i++){
    size_t length=strlen(singletons[i]);
    if(!strncmp(left->code,singletons[i],length) && !strncmp(right->code,singletons[i],length)) return 1;
  }
  int left_key,right_key;
  if(keyboard_binding_parse(left->code,&left_key,NULL,NULL,NULL) &&
     keyboard_binding_parse(right->code,&right_key,NULL,NULL,NULL))
    return left_key==right_key;
  RoomRule lrule,rrule;
  if(room_rule_parse(left->code,&lrule) && room_rule_parse(right->code,&rrule))
    return room_rule_same_target(&lrule,&rrule);
  if(boot_line_is_menu(left->code) && boot_line_is_menu(right->code)){
    char l[6][128],r[6][128];
    int ln=menu_split(left->code,l),rn=menu_split(right->code,r);
    if(ln<2 || rn<2) return !strcmp(left->code,right->code);
    int lt=!strcmp(l[0],"mset"),rt=!strcmp(r[0],"mset");
    return lt==rt && !strcmp(l[1],r[1]);
  }
  return !strcmp(left->code,right->code);
}

static int boot_store_override(CheatSlot *slots,int *count,const CheatSlot *candidate,
                               char *error,size_t error_capacity){
  for(int i=0;i<*count;i++){
    if(!boot_same_target(&slots[i],candidate)) continue;
    memmove(&slots[i],&slots[i+1],(size_t)(*count-i-1)*sizeof *slots);
    (*count)--;
    break;
  }
  if(*count>=GML_MAX_CHEATS){
    snprintf(error,error_capacity,"more than %d effective directives",GML_MAX_CHEATS);
    return 0;
  }
  slots[(*count)++]=*candidate;
  return 1;
}

static int boot_store_key_binding(const CheatSlot *candidate,CheatSlot *slots,int *count,
                                  int line_number,char *error,size_t error_capacity){
  unsigned port;
  const char *scan=keyboard_binding_keys(candidate->code,&port);
  if(scan){
    int keys[GML_MAX_CHEATS];
    unsigned char seen[256]={0};
    int key_count=0;
    do {
      int key=keyboard_binding_key(&scan);
      if(key<0 || seen[key] || key_count>=GML_MAX_CHEATS) break;
      seen[key]=1;
      keys[key_count++]=key;
      if(*scan=='|' && keyboard_binding_buttons(scan+1,NULL,NULL)){
        if(key_count==1)
          return boot_store_override(slots,count,candidate,error,error_capacity);
        /* Expand a chord into the same effective directives as its individual
         * bindings. Layering and state identity continue to resolve by key. */
        CheatSlot binding={0};
        binding.enabled=1;
        for(int i=0;i<key_count;i++){
          snprintf(binding.code,sizeof binding.code,"keybind|%u|%d|%s",port+1u,keys[i],scan+1);
          if(!boot_store_override(slots,count,&binding,error,error_capacity)) return 0;
        }
        return 1;
      }
    } while(*scan++==',');
  }
  snprintf(error,error_capacity,
           "directive %d: keybind| takes port 1 to %u, keys 2 to 255 and a button list with optional |turbo, or auto",
           line_number,(unsigned)ANYGM_MAX_GAMEPADS);
  return 0;
}

int engine_boot_overrides_text(const CheatSlot *slots,int count,char *text,size_t capacity){
  size_t used=0;
  if(!text || !capacity) return 0;
  for(int i=0;i<count;i++){
    size_t length=strlen(slots[i].code);
    if(length+2u>capacity-used) return 0;
    memcpy(text+used,slots[i].code,length); used+=length; text[used++]='\n';
  }
  text[used]=0;
  return 1;
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
      /* Keep one complete directive per slot so runtime consumers can read each independently. */
      char line[CHEAT_DIRECTIVE_BYTES*CHEAT_MAX_DIRECTIVES];
      if(length>=sizeof line){
        snprintf(error,error_capacity,"directive %d is longer than %llu bytes",
                 line_number,(unsigned long long)(sizeof line-1u));
        return 0;
      }
      memcpy(line,cursor,length);
      line[length]=0;
      char parts[CHEAT_MAX_DIRECTIVES][CHEAT_DIRECTIVE_BYTES];
      int part_count=cheat_split_directives(line,parts,true);
      if(part_count<0){
        snprintf(error,error_capacity,"directive %d exceeds the chain or directive length limit",
                 line_number);
        return 0;
      }
      for(int p=0;p<part_count;p++){
        CheatSlot candidate={0};
        CheatSlot *slot=&candidate;
        _Static_assert(CHEAT_DIRECTIVE_BYTES<=sizeof slot->code,"override text must fit its slot");
        memcpy(slot->code,parts[p],strlen(parts[p])+1);
        slot->enabled=1;
        if(!strncmp(slot->code,"ostype|",7)){
          /* Declares which system this content is told it stands on: one non-negative os_* value,
           * using the runtime's os_type numbering. */
          const char *value=slot->code+7;
          int digits=0,value_ok=value[0]!=0;
          for(const char *scan=value;*scan && value_ok;scan++){
            if(*scan>='0' && *scan<='9') digits=1;
            else if(*scan!=' ' && *scan!='\t') value_ok=0;
          }
          if(!value_ok || !digits){
            snprintf(error,error_capacity,
                     "directive %d: ostype| takes one non-negative os_type value",line_number);
            return 0;
          }
        } else if(!strncmp(slot->code,"renderer|",9)){
          const char *value=slot->code+9;
          if(strcmp(value,"cpu") && strcmp(value,"opengl")){
            snprintf(error,error_capacity,
                     "directive %d: renderer| takes cpu or opengl",
                     line_number);
            return 0;
          }
        } else if(!strncmp(slot->code,"statereserve|",13)){
          size_t bytes=0;
          if(!state_reserve_parse(slot->code,&bytes)){
            snprintf(error,error_capacity,
                     "directive %d: statereserve| takes positive decimal bytes up to 1073741824",
                     line_number);
            return 0;
          }
        } else if(!strncmp(slot->code,"emptyscreen|",12)){
          if(strcmp(slot->code+12,"keep")){
            snprintf(error,error_capacity,"directive %d: emptyscreen| takes keep",line_number);
            return 0;
          }
        } else if(!strncmp(slot->code,"gamepad|",8)){
          if(strcmp(slot->code+8,"keyboard") && strcmp(slot->code+8,"single") &&
             strcmp(slot->code+8,"auto")){
            snprintf(error,error_capacity,
                     "directive %d: gamepad| takes keyboard, single or auto",line_number);
            return 0;
          }
        } else if(!strncmp(slot->code,"keybind|",8)){
          if(!boot_store_key_binding(slot,slots,count,line_number,error,error_capacity))
            return 0;
          continue;
        } else if(room_rule_kind(slot->code)){
          RoomRule rule;
          if(!room_rule_parse(slot->code,&rule)){
            snprintf(error,error_capacity,"directive %d: invalid room override",line_number);
            return 0;
          }
        } else if(!boot_line_is_menu(slot->code)){
          cheat_parse(slot->code,&slot->act);
          if(slot->act.kind==CK_NONE){
            snprintf(error,error_capacity,"directive %d is not a recognized override",line_number);
            return 0;
          }
          if(slot->act.kind==CK_ROOM || slot->act.kind==CK_INST_SET){
            snprintf(error,error_capacity,
                     "directive %d: one-shot actions are frontend runtime overrides, not anchors",
                     line_number);
            return 0;
          }
        }
        if(!boot_store_override(slots,count,slot,error,error_capacity)) return 0;
      }
    }
    if(!line_end) break;
    cursor=line_end+1;
  }
  return 1;
}
int engine_boot_cheats_active(const AnygmEngine *engine){
  return engine->config.content_overrides?engine->boot_cheat_count:0;
}
size_t engine_overrides_state_reserve(const AnygmEngine *engine){
  size_t reserve=0;
  int active=engine_boot_cheats_active(engine);
  for(int i=0;i<active;i++){
    size_t bytes=0;
    if(engine->boot_cheats[i].enabled &&
       state_reserve_parse(engine->boot_cheats[i].code,&bytes) && bytes>reserve)
      reserve=bytes;
  }
  return reserve;
}
/* The content composes its own screen: a frame it leaves without colour is shown as it is,
 * without the application-surface fallback and the whole-screen test that decides it. */
int engine_overrides_keep_empty_screen(const AnygmEngine *engine){
  int active=engine_boot_cheats_active(engine);
  for(int i=0;i<active;i++)
    if(engine->boot_cheats[i].enabled && !strcmp(engine->boot_cheats[i].code,"emptyscreen|keep"))
      return 1;
  return 0;
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
  return a->kind==CK_ENGINE && a->eng>=EF_WINDOW_W && a->eng<=EF_PRESENT_CROP_BOTTOM;
}
static int cheat_slot_capturable(const CheatAct *a){
  if(a->scope_sync) return 0;
  if(a->kind==CK_SURFACE_CANVAS) return 1;
  if(a->kind==CK_GSCALAR || a->kind==CK_GSCALAR_CALL || a->kind==CK_GARR) return 1;
  return (a->scope_gameres || a->scope_crt) &&
         (a->kind==CK_INST || a->kind==CK_SURFACE || cheat_engine_field_mutable(a));
}
static void cheat_slot_capture(AnygmEngine *engine,CheatSlot *slot){
  slot->saved_valid=0;
  if(!engine->loaded || !cheat_slot_capturable(&slot->act)) return;
  switch(slot->act.kind){
    case CK_GSCALAR_CALL:
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
    case CK_SURFACE_CANVAS:
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
        case EF_PRESENT_SHIFT_X: slot->saved=engine->present_shift_x; slot->saved_valid=1; break;
        case EF_PRESENT_SHIFT_Y: slot->saved=engine->present_shift_y; slot->saved_valid=1; break;
        case EF_PRESENT_CROP_LEFT:
          slot->saved=engine->present_crop_left; slot->saved_valid=1; break;
        case EF_PRESENT_CROP_TOP:
          slot->saved=engine->present_crop_top; slot->saved_valid=1; break;
        case EF_PRESENT_CROP_RIGHT:
          slot->saved=engine->present_crop_right; slot->saved_valid=1; break;
        case EF_PRESENT_CROP_BOTTOM:
          slot->saved=engine->present_crop_bottom; slot->saved_valid=1; break;
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
  if(slot->act.scope_sync){
    if(slot->applied && engine->loaded && cheat_scope_instance_ok(engine,&slot->act))
      cheat_apply_one(engine,&slot->act);
    return;
  }
  if(!slot->saved_valid) return;
  if(!keep) slot->saved_valid=0;
  if(!engine->loaded) return;
  switch(slot->act.kind){
    case CK_GSCALAR: gml_set_global_scalar(&engine->vm, slot->act.obj, slot->saved); break;
    case CK_GSCALAR_CALL: cheat_setting_apply(engine,&slot->act,slot->saved); break;
    case CK_GARR:    gml_set_global_arr(&engine->vm, slot->act.obj, slot->act.idx, slot->saved);
                     break;
    case CK_INST:    gml_set_inst_var_all(&engine->vm, slot->act.obj, slot->act.var, slot->saved);
                     break;
    case CK_SURFACE_CANVAS:
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
  int part_count=cheat_split_directives(slot->code,parts,false);
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
      else if(one.kind==CK_INST_SET) cheat_apply_one(engine,&one);
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
    if(a->kind==CK_ROOM || a->kind==CK_INST_SET || a->kind==CK_NONE) continue;
    /* A hold belongs to the drawing, not to the post-step pass: writing it here would make it the
     * freeze it exists not to be. */
    if(a->kind==CK_DRAW_HOLD || a->kind==CK_SURFACE_CANVAS || a->kind==CK_AFTER_DRAW ||
       a->kind==CK_WINDOW_RASTER) continue;
    if(room_owned_only && !cheat_slot_is_room_owned(a)) continue;
    int applies=channel_on && slot->enabled && cheat_scope_ok(engine,a,0);
    /* A script override is an edge at fresh startup, not a value to restore when its channel is
     * toggled.  Keep its completion marker so re-enabling content overrides cannot run setup a
     * second time in the same load. */
    if(a->kind==CK_SCRIPT){
      if(applies && !slot->applied){
        (void)gml_vm_run_script_named(&engine->vm,a->obj);
      }
      if(applies) slot->applied=1;
      continue;
    }
    if(applies){
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
    if((!slot->act.scope_gameres && !slot->act.scope_crt) ||
       slot->act.kind==CK_NONE || slot->act.kind==CK_WINDOW_RASTER) continue;
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
int engine_override_is_host_policy(const char *code){
  if(strncmp(code,"?crt",4) || (code[4]!=' ' && code[4]!='\t')) return 0;
  code+=4;
  while(*code==' ' || *code=='\t') code++;
  return !strncmp(code,"crtmode|",8) || !strncmp(code,"roomspeed|",10);
}
/* Resolve last-wins precedence without writing the game's window request. A native size change
 * remains available as soon as the CRT window is withdrawn, including after a state restore. */
bool engine_overrides_window_raster(const AnygmEngine *engine,int *width,int *height){
  if(!engine->config.present_logical_raster || !engine->config.adjust_crt_tv) return false;
  const CheatSlot *tables[]={engine->cheats,engine->boot_cheats};
  const int counts[]={engine->cheat_count,engine_boot_cheats_active(engine)};
  for(int table=0;table<2;table++) for(int i=counts[table]-1;i>=0;i--){
    const CheatSlot *slot=&tables[table][i];
    if(!slot->enabled || slot->act.kind!=CK_WINDOW_RASTER ||
       !cheat_crt_scope_ok(engine,slot->act.scope_crt)) continue;
    if(width) *width=slot->act.crt_width;
    if(height) *height=slot->act.crt_height;
    return true;
  }
  return false;
}
/* Resolve both declarations from live options and slots. Holding a room_speed global would
 * enter states and outlive the option, so the runtime borrows a lookup table instead. Runtime
 * slots are read last and therefore take precedence over boot declarations. */
void engine_overrides_host_policy_refresh(AnygmEngine *engine){
  engine->crt_mode_width=engine->crt_mode_height=0;
  engine->crt_mode_hz=0;
  engine->vm.room_speed_override=NULL;
  engine->vm.room_speed_override_count=0;
  if(!engine->config.present_logical_raster || !engine->config.adjust_crt_tv) return;
  const CheatSlot *tables[]={engine->boot_cheats,engine->cheats};
  const int counts[]={engine_boot_cheats_active(engine),engine->cheat_count};
  int speeds=0;
  for(int table=0;table<2;table++) for(int i=0;i<counts[table];i++){
    const CheatSlot *slot=&tables[table][i];
    if(!slot->enabled) continue;
    if(slot->act.kind==CK_CRT_MODE){
      engine->crt_mode_width=(unsigned)slot->act.crt_width;
      engine->crt_mode_height=(unsigned)slot->act.crt_height;
      engine->crt_mode_hz=(uint32_t)slot->act.val.lit;
    } else if(slot->act.kind==CK_ROOM_SPEED){
      uint8_t rooms[ENGINE_ROOM_RULE_LIMIT/8]={0};
      const char *list=slot->act.var;
      if(!parse_room_list(&list,rooms)) continue;
      if(!speeds){
        memset(engine->room_speed_overrides,0,sizeof engine->room_speed_overrides);
        speeds=1;
      }
      for(int room=0;room<ENGINE_ROOM_RULE_LIMIT;room++)
        if(rooms[room/8]&(1u<<(room%8)))
          engine->room_speed_overrides[room]=(uint16_t)slot->act.val.lit;
    }
  }
  if(!speeds) return;
  engine->vm.room_speed_override=engine->room_speed_overrides;
  engine->vm.room_speed_override_count=ENGINE_ROOM_RULE_LIMIT;
}
/* Presentation-scoped targets must settle before geometry is read. The ordinary sticky pass runs
 * after Step so simulation freezes observe the usual event boundary; using this narrow pass at
 * the start of a frame lets a live presentation switch affect that same frame without moving any
 * unscoped gameplay write earlier. */
void engine_overrides_presentation_apply(AnygmEngine *engine){
  if(!engine) return;
  engine_overrides_host_policy_refresh(engine);
  cheat_gameres_pass(engine,engine->cheats,engine->cheat_count,1);
  cheat_gameres_pass(engine,engine->boot_cheats,engine->boot_cheat_count,
                     engine_boot_cheats_active(engine)>0);
}
/* A hold is written in front of the frame's drawing and taken back once the drawing is done. It
 * needs no arm-time capture: what it puts back is what content left in the global on this frame,
 * read one instruction before the hold replaces it, so a value the content recomputed since the
 * scope opened survives. Nothing of it outlives the frame, which is why a scope change has nothing
 * of its own to restore and why it never reaches a savestate. */
static void cheat_draw_hold_pass(AnygmEngine *engine,CheatSlot *arr,int n,int channel_on,int begin){
  for(int i=0;i<n;i++){
    CheatSlot *slot=&arr[begin?i:n-1-i];
    if(slot->act.kind!=CK_DRAW_HOLD) continue;
    if(begin){
      slot->held_valid=0;
      if(!channel_on || !slot->enabled || !engine->loaded) continue;
      if(!cheat_scope_ok(engine,&slot->act,0)) continue;
      slot->held=gml_global_num(&engine->vm,slot->act.obj);
      slot->held_valid=1;
      gml_set_global_scalar(&engine->vm,slot->act.obj,cheat_val_eval(engine,&slot->act.val));
    } else if(slot->held_valid){
      slot->held_valid=0;
      gml_set_global_scalar(&engine->vm,slot->act.obj,slot->held);
    }
  }
}
void engine_overrides_draw_hold_begin(AnygmEngine *engine){
  if(!engine) return;
  cheat_draw_hold_pass(engine,engine->cheats,engine->cheat_count,1,1);
  cheat_draw_hold_pass(engine,engine->boot_cheats,engine->boot_cheat_count,
                       engine_boot_cheats_active(engine)>0,1);
}
void engine_overrides_draw_hold_end(AnygmEngine *engine){
  if(!engine) return;
  /* Both tables are walked whole and without the channel gate: this undoes only what begin marked,
   * and a channel switched off between the two would otherwise leave the held value behind. */
  cheat_draw_hold_pass(engine,engine->boot_cheats,engine->boot_cheat_count,1,0);
  cheat_draw_hold_pass(engine,engine->cheats,engine->cheat_count,1,0);
}
/* Reuse the event's target and receiver, not a second global frame pass. Resolve by id because
 * the authored event can destroy its receiver or grow the instance pool. Nested screen_redraw
 * runs the ordinary content events but cannot recursively append this policy. */
void engine_overrides_after_draw(AnygmEngine *engine,GmlVM *vm,unsigned instance_id){
  if(!engine || !vm || engine->after_draw_active || !engine->loaded) return;
  int room=vm->room_index;
  engine->after_draw_active=1;
  for(int table=0;table<2 && vm->room_index==room;table++){
    CheatSlot *slots=table?engine->boot_cheats:engine->cheats;
    int count=table?engine_boot_cheats_active(engine):engine->cheat_count;
    for(int i=0;i<count && vm->room_index==room;i++){
      CheatSlot *slot=&slots[i];
      CheatAct *a=&slot->act;
      if(!slot->enabled || a->kind!=CK_AFTER_DRAW ||
         !cheat_scope_ok(engine,a,a->scope_aspect?1:0)) continue;
      GmlInstance *in=gml_vm_instance_by_id(vm,instance_id);
      if(!in || !in->active || in->visible<0.5 || in->obj<0 || in->obj>=vm->n_objects) continue;
      const char *name=vm->objects[in->obj].name;
      if(!name || strcmp(name,a->obj)) continue;
      GmlVal arg=vreal(a->val.lit);
      (void)gml_vm_run_script_on_instance(vm,a->var,in,&arg,1);
    }
  }
  engine->after_draw_active=0;
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
      if(slots[i].enabled && (slots[i].act.scope_gameres || slots[i].act.scope_crt) &&
         !slots[i].saved_valid)
        cheat_slot_capture(engine,&slots[i]);
  }
}
/* Loading a state restores the values a scoped override wrote into the one being saved, so every
 * slot that could have written owes an answer again. Marking them applied lets the next pass make
 * it: the scope decides, and a directive that no longer applies gives its captured value back. */
void engine_overrides_note_state_load(AnygmEngine *engine){
  if(!engine) return;
  for(int i=0;i<engine->cheat_count;i++)
    if(engine->cheats[i].saved_valid || engine->cheats[i].act.scope_sync ||
       engine->cheats[i].act.kind==CK_SCRIPT)
      engine->cheats[i].applied=1;
  for(int i=0;i<engine->boot_cheat_count;i++)
    if(engine->boot_cheats[i].saved_valid || engine->boot_cheats[i].act.scope_sync ||
       engine->boot_cheats[i].act.kind==CK_SCRIPT)
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
/* A declared singleton surface holds a fixed-coordinate HUD and a full-width application image.
 * Its authored canvas chooses the final aspect, independently of the shorter world camera. */
static CheatSlot *surface_canvas_slot(AnygmEngine *engine){
  CheatSlot *tables[]={engine->cheats,engine->boot_cheats};
  int counts[]={engine->cheat_count,engine_boot_cheats_active(engine)};
  for(int table=0;table<2;table++) for(int i=0;i<counts[table];i++){
    CheatSlot *slot=&tables[table][i];
    if(slot->enabled && slot->act.kind==CK_SURFACE_CANVAS &&
       (!slot->act.scope_mode || slot->act.scope_mode==engine->aspect_force_mode)) return slot;
  }
  return NULL;
}
int aspect_surface_canvas_size(AnygmEngine *engine,int *width,int *height){
  CheatSlot *slot=surface_canvas_slot(engine);
  if(!slot) return 0;
  if(width) *width=(int)slot->act.val.lit;
  if(height) *height=(int)slot->act.val2.lit;
  return 1;
}
void aspect_surface_canvas_update(AnygmEngine *engine,double camera_x){
  GmlRenderControl control={0};
  CheatSlot *selected=engine->aspect_force_active?surface_canvas_slot(engine):NULL;
  CheatSlot *tables[]={engine->cheats,engine->boot_cheats};
  int counts[]={engine->cheat_count,engine->boot_cheat_count};
  for(int table=0;table<2;table++) for(int i=0;i<counts[table];i++){
    CheatSlot *slot=&tables[table][i];
    if(slot->act.kind!=CK_SURFACE_CANVAS) continue;
    if(slot!=selected){
      if(slot->applied) cheat_slot_restore_keep(engine,slot,0);
      slot->applied=0;
      continue;
    }
    if(!slot->saved_valid) cheat_slot_capture(engine,slot);
    double handle=0;
    if(!slot->saved_valid ||
       !gml_inst_var_first_real(&engine->vm,slot->act.obj,slot->act.var,&handle) ||
       !isfinite(handle) || handle<1 || handle>GML_MAX_SURFACES ||
       !gml_surface_exists(&engine->render,(int)handle)) continue;
    if(gml_surface_width(&engine->render,(int)handle)!=(int)engine->width ||
       gml_surface_height(&engine->render,(int)handle)!=(int)slot->act.val2.lit)
      gml_surface_resize(&engine->render,(int)handle,(int)engine->width,(int)slot->act.val2.lit);
    slot->applied=1;
    control.surface_canvas=(int)handle;
    control.surface_canvas_width=(int)slot->act.val.lit;
    control.surface_canvas_height=(int)slot->act.val2.lit;
    control.surface_canvas_world_width=(int)engine->width;
    control.surface_canvas_room_width=(int)engine->width;
    GmlRoom room;
    if(gml_vm_room_get(&engine->vm,engine->vm.room_index,&room)==0 &&
       room.width>0 && room.width<engine->width && isfinite(camera_x)){
      double left=fmax(0.0,fmin((double)engine->width,-camera_x));
      double right=fmax(left,fmin((double)engine->width,(double)room.width-camera_x));
      control.surface_canvas_room_x=(int)lround(left);
      control.surface_canvas_room_width=(int)lround(right)-control.surface_canvas_room_x;
    }
  }
  gml_render_control_update(&engine->render,&control,GML_RENDER_CONTROL_SURFACE_CANVAS);
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
  if(aspect_surface_canvas_size(engine,NULL,NULL)) return 1;
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

/* Return the declared os_type, or -1 when no active directive supplies one. */
int engine_overrides_declared_os_type(AnygmEngine *engine){
  int active=engine_boot_cheats_active(engine);
  for(int i=0;i<active;i++)
    if(engine->boot_cheats[i].enabled && !strncmp(engine->boot_cheats[i].code,"ostype|",7))
      return atoi(engine->boot_cheats[i].code+7);
  return -1;
}
uint32_t engine_overrides_declared_renderer(AnygmEngine *engine){
  int active=engine_boot_cheats_active(engine);
  for(int i=0;i<active;i++){
    const CheatSlot *slot=&engine->boot_cheats[i];
    if(!slot->enabled || strncmp(slot->code,"renderer|",9)) continue;
    const char *value=slot->code+9;
    if(!strcmp(value,"cpu")) return ANYGM_SCENE_RENDERER_CPU;
    if(!strcmp(value,"opengl")) return ANYGM_SCENE_RENDERER_OPENGL_NO_FALLBACK;
  }
  return engine->config.scene_renderer;
}
int engine_overrides_keyboard_emulation(AnygmEngine *engine){
  int active=engine_boot_cheats_active(engine);
  for(int i=0;i<active;i++)
    if(engine->boot_cheats[i].enabled &&
       !strcmp(engine->boot_cheats[i].code,"gamepad|keyboard")) return 1;
  return 0;
}
unsigned engine_overrides_gamepad_limit(AnygmEngine *engine){
  int active=engine_boot_cheats_active(engine);
  for(int i=0;i<active;i++)
    if(engine->boot_cheats[i].enabled &&
       !strcmp(engine->boot_cheats[i].code,"gamepad|single")) return 1u;
  return ANYGM_MAX_GAMEPADS;
}
/* Binding declarations never select input ownership or alter physical keyboard state. */
unsigned engine_overrides_key_button_mask(AnygmEngine *engine,int virtual_key,unsigned *port,
                                          bool *turbo){
  if(port) *port=0;
  if(turbo) *turbo=false;
  int active=engine_boot_cheats_active(engine);
  for(int i=0;i<active;i++){
    int key;unsigned buttons,selected;
    bool repeat;
    if(engine->boot_cheats[i].enabled &&
       keyboard_binding_parse(engine->boot_cheats[i].code,&key,&buttons,&selected,&repeat) &&
       key==virtual_key){
      if(port && buttons) *port=selected;
      if(turbo) *turbo=repeat;
      return buttons;
    }
  }
  return 0;
}
/* Only explicitly bound ports add keyboard sources beyond the ordinary first pad. */
unsigned engine_overrides_keyboard_bindings(AnygmEngine *engine,
                                           unsigned buttons[ANYGM_MAX_GAMEPADS]){
  if(buttons) memset(buttons,0,sizeof(unsigned)*ANYGM_MAX_GAMEPADS);
  unsigned ports=0;
  int active=engine_boot_cheats_active(engine);
  for(int i=0;i<active;i++){
    int key;unsigned mask,port;
    if(engine->boot_cheats[i].enabled &&
       keyboard_binding_parse(engine->boot_cheats[i].code,&key,&mask,&port,NULL) && mask){
      ports|=1u<<port;
      if(buttons) buttons[port]|=mask;
    }
  }
  return ports;
}
static bool room_trigger_pressed(const AnygmEngine *engine,const RoomTrigger *trigger){
  for(unsigned port=0;port<ANYGM_MAX_GAMEPADS;port++){
    unsigned mask=trigger->pads[port];
    for(unsigned button=0;mask;button++,mask>>=1)
      if((mask&1u) && engine->pad_current[port][button] && !engine->pad_previous[port][button])
        return true;
  }
  for(int byte=0;byte<(int)sizeof trigger->keys;byte++){
    unsigned mask=trigger->keys[byte];
    for(int bit=0;mask;bit++,mask>>=1){
      int key=8*byte+bit;
      if((mask&1u) && engine->event_key_current[key] && !engine->event_key_previous[key])
        return true;
    }
  }
  return false;
}

static bool room_rule_transition(AnygmEngine *engine,int room,int destination){
  if(destination>=0){
    if(destination>=gml_room_count(&engine->win)) return false;
    gml_audio_stop_all(engine->audio);
    gml_room_enter(&engine->vm,destination);
    return true;
  }
  for(int i=0;i+1<engine->win.n_room_order;i++){
    if((int)engine->win.room_order[i]!=room) continue;
    gml_vm_goto_room_order(&engine->vm,i+1);
    return true;
  }
  return false;
}

static bool room_rule_active(const CheatSlot *slot,int kind,int room,RoomRule *rule){
  return slot->enabled && room_rule_kind(slot->code)==kind &&
    room_rule_parse(slot->code,rule) && room_rule_contains(rule,room);
}

static void room_counter_apply(GmlVM *vm,const RoomRule *rule){
  int target=gml_object_index_by_name(vm,rule->object);
  if(target<0) return;
  for(int i=0;i<vm->inst_count;i++){
    GmlInstance *instance=&vm->inst[i];
    if(!instance->active || instance->marked || !gml_object_is(vm,instance->obj,target)) continue;
    GmlVal *guard=gml_varmap_get(&instance->vars,rule->guard);
    if(!guard || guard->t!=V_REAL || guard->d!=rule->expected) continue;
    GmlVal *counter=gml_varmap_get(&instance->vars,rule->counter);
    if(!counter || counter->t!=V_REAL || !isfinite(counter->d)) continue;
    /* Only replace an existing numeric slot. Keep the content's numeric kind. */
    switch(gml_val_number_kind(*counter)){
      case GML_NUM_INT32: *counter=vint32(rule->value);break;
      case GML_NUM_INT64: *counter=vint64(rule->value);break;
      default: *counter=vreal(rule->value);break;
    }
  }
}

/* Read the phase from the frame the player saw before the current Step. */
void room_counter_hook(AnygmEngine *engine){
  if(engine->vm.pending_room>=0) return;
  int active=engine_boot_cheats_active(engine);
  for(int i=0;i<active;i++){
    RoomRule rule;
    if(room_rule_active(&engine->boot_cheats[i],ROOM_RULE_COUNTER,engine->vm.room_index,&rule) &&
       room_trigger_pressed(engine,&rule.trigger)) room_counter_apply(&engine->vm,&rule);
  }
}

void room_overrides_after_step(AnygmEngine *engine,int previous_room){
  GmlVM *vm=&engine->vm;
  if(vm->pending_room>=0) return;
  int active=engine_boot_cheats_active(engine),room=vm->room_index;
  /* An immediate jump after the previous Step still gives the entered room its
   * first Step this frame. This also covers an initial room created at load. */
  bool entered=room!=previous_room || vm->room_enter_frame==vm->frame ||
    (vm->frame>0 && vm->room_enter_frame==vm->frame-1);
  for(int i=active-1;entered && i>=0;i--){
    RoomRule rule;
    if(room_rule_active(&engine->boot_cheats[i],ROOM_RULE_SKIP,room,&rule) &&
       rule.trigger.room_enter && room_rule_transition(engine,room,rule.destination)) return;
  }
  if(room!=previous_room || (vm->frame>1 && vm->room_enter_frame==vm->frame)) return;

  /* The development setting supplies an explicit replacement for input-driven jumps. */
  const char *setting=anygm_host_development_setting(&engine->host,"GML_ROOMSKIP");
  RoomRule diagnostic;
  bool replace=setting && room_rule_parse(setting,&diagnostic) &&
    diagnostic.kind==ROOM_RULE_SKIP && !diagnostic.trigger.room_enter;
  if(replace && room_rule_contains(&diagnostic,room) && room_trigger_pressed(engine,&diagnostic.trigger) &&
     room_rule_transition(engine,room,diagnostic.destination)) return;

  for(int i=active-1;!replace && i>=0;i--){
    RoomRule rule;
    if(room_rule_active(&engine->boot_cheats[i],ROOM_RULE_SKIP,room,&rule) &&
       room_trigger_pressed(engine,&rule.trigger) &&
       room_rule_transition(engine,room,rule.destination)) return;
  }
  for(int i=0;i<active;i++){
    RoomRule rule;
    if(!room_rule_active(&engine->boot_cheats[i],ROOM_RULE_ALARM,room,&rule) ||
       !room_trigger_pressed(engine,&rule.trigger)) continue;
    int target=gml_object_index_by_name(vm,rule.object);
    if(target<0) continue;
    for(int j=0;j<vm->inst_count;j++){
      GmlInstance *instance=&vm->inst[j];
      if(instance->active && !instance->marked && gml_object_is(vm,instance->obj,target))
        instance->alarm[rule.alarm]=1.0;
    }
  }
}
