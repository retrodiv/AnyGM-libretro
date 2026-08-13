/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Deterministic platform and offline-capability builtin adapters. */
#include "gml_builtin_internal.h"
#include "anygm_compatibility.h"
#include "anygm_host.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  GML_EXTERNAL_NOOP_HANDLE_BASE=0x4A300000,
  GML_EXTERNAL_NOOP_REAL=1,
  GML_EXTERNAL_NOOP_STRING=2
};

static int external_hex_value(unsigned char value){
  if(value>='0' && value<='9') return value-'0';
  if(value>='a' && value<='f') return value-'a'+10;
  if(value>='A' && value<='F') return value-'A'+10;
  return -1;
}

static char *external_hex_decode(const char *encoded,size_t size){
  enum { EXTERNAL_NAME_MAX=4096 };
  if(!encoded || !size || (size&1u) || size/2u>EXTERNAL_NAME_MAX) return NULL;
  char *decoded=(char*)malloc(size/2u+1u);
  if(!decoded) return NULL;
  for(size_t i=0;i<size;i+=2u){
    int high=external_hex_value((unsigned char)encoded[i]);
    int low=external_hex_value((unsigned char)encoded[i+1u]);
    if(high<0 || low<0 || (high==0 && low==0)){ free(decoded); return NULL; }
    decoded[i/2u]=(char)((high<<4)|low);
  }
  decoded[size/2u]='\0';
  return decoded;
}

static int external_noop_string_symbol(const char *symbol){
  return symbol && (
    strstr(symbol,"Name") || strstr(symbol,"Error") || strstr(symbol,"Data") ||
    strstr(symbol,"String"));
}

int builtin_external_define(const char *library,const char *symbol){
  int handle=builtin_external_audio_define(library,symbol);
  if(handle) return handle;
  handle=builtin_external_input_define(library,symbol);
  if(handle) return handle;
  if(anygm_external_library_policy(library)==ANYGM_EXTERNAL_LIBRARY_NOOP)
    return GML_EXTERNAL_NOOP_HANDLE_BASE+
      (external_noop_string_symbol(symbol)?GML_EXTERNAL_NOOP_STRING:GML_EXTERNAL_NOOP_REAL);
  return 0;
}

GmlVal builtin_external_call(GmlVM *vm,int handle,
                             GmlVal *args,int count,int *handled){
  GmlVal result=builtin_external_audio_call(vm,handle,args,count,handled);
  if(handled && *handled) return result;
  result=builtin_external_input_call(vm,handle,args,count,handled);
  if(handled && *handled) return result;
  int operation=handle-GML_EXTERNAL_NOOP_HANDLE_BASE;
  if(operation==GML_EXTERNAL_NOOP_REAL || operation==GML_EXTERNAL_NOOP_STRING){
    if(handled) *handled=1;
    return operation==GML_EXTERNAL_NOOP_STRING?vstr(""):vreal(0);
  }
  if(handled) *handled=0;
  return vreal(0);
}

GmlVal builtin_external_call_encoded(GmlVM *vm,const char *name,
                                     GmlVal *args,int count,int *handled){
  static const char prefix[]="__anygm_external_";
  if(handled) *handled=0;
  if(!name || strncmp(name,prefix,sizeof(prefix)-1u)) return vreal(0);
  const char *library_hex=name+sizeof(prefix)-1u;
  const char *separator=strchr(library_hex,'_');
  if(!separator || separator==library_hex || !separator[1]) return vreal(0);
  char *library=external_hex_decode(library_hex,(size_t)(separator-library_hex));
  char *symbol=external_hex_decode(separator+1u,strlen(separator+1u));
  int handle=(library&&symbol)?builtin_external_define(library,symbol):0;
  free(library); free(symbol);
  if(!handle) return vreal(0);
  return builtin_external_call(vm,handle,args,count,handled);
}

/* GameMaker's os_is_network_connected reports host connectivity, independently of whether a
 * platform service such as Steam is initialised. Host has no environment callback for this,
 * so inspect the host without sending traffic. The explicit override is useful to hosts that
 * deliberately sandbox networking and also makes the offline state reproducible in tests. */
static int host_network_connected(GmlVM *vm){
  const char *override=builtin_setting(vm,"GML_NETWORK_CONNECTED");
  if(override && *override){
    int first=tolower((unsigned char)override[0]);
    return first!='0' && first!='f' && first!='n';
  }
  return anygm_host_capability(vm?vm->host:NULL,
                               ANYGM_HOST_CAPABILITY_NETWORK_CONNECTED)!=0;
}

/* A bounded classic execute_string compatibility path. Old projects commonly construct a
 * resource assignment such as "sprite_index=spr_name" at runtime. Resolve that controlled
 * assignment without introducing a second runtime compiler into the core. */
static int classic_execute_assignment(GmlVM *vm, const char *source){
  if(!vm || !source) return 0;
  while(isspace((unsigned char)*source)) source++;
  const char *eq=strchr(source,'='); if(!eq) return 0;
  const char *lhs_end=eq; while(lhs_end>source && isspace((unsigned char)lhs_end[-1])) lhs_end--;
  const char *rhs=eq+1; while(isspace((unsigned char)*rhs)) rhs++;
  const char *rhs_end=rhs+strlen(rhs); while(rhs_end>rhs && (isspace((unsigned char)rhs_end[-1])||rhs_end[-1]==';')) rhs_end--;
  char lhs[192],value_name[192]; size_t ln=(size_t)(lhs_end-source),rn=(size_t)(rhs_end-rhs);
  if(!ln || ln>=sizeof(lhs) || !rn || rn>=sizeof(value_name)) return 0;
  memcpy(lhs,source,ln); lhs[ln]=0; memcpy(value_name,rhs,rn); value_name[rn]=0;
  char *dot=strrchr(lhs,'.'); const char *field=dot?dot+1:lhs;
  GmlInstance *target=vm->cur_self;
  if(dot){
    *dot=0; target=NULL;
    if(!strcmp(lhs,"self")) target=vm->cur_self;
    else if(!strcmp(lhs,"other")) target=vm->cur_other;
    else {
      GmlVal *ref=vm->cur_self?gml_varmap_get(&vm->cur_self->vars,lhs):NULL;
      if(ref && ref->t==V_REAL){
        int id=(int)ref->d;
        for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked && (int)vm->inst[i].id==id){ target=&vm->inst[i]; break; }
        if(!target && id>=0 && id<vm->n_objects) target=gml_find_instance(vm,id);
      }
      if(!target){ int object=gml_object_index_by_name(vm,lhs); if(object>=0) target=gml_find_instance(vm,object); }
    }
  }
  if(!target || !*field) return 0;
  char *end=NULL; double value=strtod(value_name,&end);
  if(!end || *end){
    value=0; int found=0; GmlRender *render=(GmlRender*)vm->render;
    int sprite=gml_render_named_sprite(render,value_name);
    if(sprite>=0){ value=sprite; found=1; }
    if(!found){ int object=gml_object_index_by_name(vm,value_name); if(object>=0){ value=object; found=1; } }
    if(!found) return 0;
  }
  return gml_inst_var_set_val(vm,vreal(target->id),field,vreal(value));
}
static const char *classic_globalvar_keyword(const char *source){
  if(!source) return NULL;
  while(isspace((unsigned char)*source)) source++;
  if(strncmp(source,"globalvar",9) ||
     (source[9] && !isspace((unsigned char)source[9]))) return NULL;
  return source+9;
}
static int classic_execute_globalvar_pass(
    GmlVM *vm,const char *source,int declare_identifiers){
  const char *cursor=classic_globalvar_keyword(source);
  if(!cursor) return 0;
  int count=0;
  for(;;){
    while(isspace((unsigned char)*cursor)) cursor++;
    const char *start=cursor;
    if(!(*cursor=='_' || isalpha((unsigned char)*cursor))) return 0;
    cursor++;
    while(*cursor=='_' || isalnum((unsigned char)*cursor)) cursor++;
    size_t length=(size_t)(cursor-start);
    if(length>127 || ++count>256) return 0;
    if(declare_identifiers){
      char name[128];
      memcpy(name,start,length);
      name[length]=0;
      if(!gml_vm_declare_globalvar(vm,name)) return 0;
    }
    while(isspace((unsigned char)*cursor)) cursor++;
    if(*cursor==','){
      cursor++;
      continue;
    }
    if(*cursor==';'){
      cursor++;
      while(isspace((unsigned char)*cursor)) cursor++;
    }
    return *cursor==0;
  }
}
static int classic_execute_globalvar(GmlVM *vm,const char *source){
  if(!vm || !vm->win || !anygm_policy_uses_classic_runtime(vm->win)) return 0;
  if(!classic_execute_globalvar_pass(vm,source,0)) return 0;
  return classic_execute_globalvar_pass(vm,source,1);
}
static int classic_execute_identifier(const char **cursor,char *name,size_t size){
  const char *source=*cursor;
  while(isspace((unsigned char)*source)) source++;
  if(!(*source=='_' || isalpha((unsigned char)*source))) return 0;
  const char *start=source++;
  while(*source=='_' || isalnum((unsigned char)*source)) source++;
  size_t length=(size_t)(source-start);
  if(!length || length>=size) return 0;
  memcpy(name,start,length);
  name[length]=0;
  *cursor=source;
  return 1;
}
static int classic_execute_call_value(GmlVM *vm,const char **cursor,GmlVal *value){
  const char *source=*cursor;
  while(isspace((unsigned char)*source)) source++;
  char *end=NULL;
  double number=strtod(source,&end);
  if(end && end>source){
    *value=vreal(number);
    *cursor=end;
    return 1;
  }
  char first[128];
  if(!classic_execute_identifier(&source,first,sizeof first)) return 0;
  while(isspace((unsigned char)*source)) source++;
  if(*source=='.'){
    source++;
    char field[128];
    if(!classic_execute_identifier(&source,field,sizeof field)) return 0;
    if(!strcmp(first,"global")){
      GmlVal *slot=gml_varmap_get(&vm->globals,field);
      *value=slot?*slot:vreal(0);
    } else {
      GmlVal scope;
      if(!strcmp(first,"self")) scope=vreal(IT_SELF);
      else if(!strcmp(first,"other")) scope=vreal(IT_OTHER);
      else return 0;
      int ok=0;
      *value=gml_inst_var_get_val(vm,scope,field,&ok);
      if(!ok) *value=vreal(0);
    }
  } else if(!strcmp(first,"true")){
    *value=vreal(1);
  } else if(!strcmp(first,"false")){
    *value=vreal(0);
  } else {
    *value=gml_vm_identifier_get(vm,first);
  }
  *cursor=source;
  return 1;
}
/* Old projects frequently construct a call whose function and real-valued arguments are known
 * resource/variable names. Execute that bounded shape through the existing script runtime rather
 * than embedding a second source compiler. Expressions, statements, nesting, and string literals
 * remain outside this deliberately narrow path. */
static int classic_execute_call(GmlVM *vm,const char *source){
  if(!vm || !vm->win || !source ||
     !anygm_policy_uses_classic_runtime(vm->win)) return 0;
  char name[128];
  if(!classic_execute_identifier(&source,name,sizeof name)) return 0;
  while(isspace((unsigned char)*source)) source++;
  if(*source!='(') return 0;
  source++;
  GmlVal arguments[16];
  int count=0;
  while(isspace((unsigned char)*source)) source++;
  if(*source!=')'){
    for(;;){
      if(count>=(int)(sizeof(arguments)/sizeof(arguments[0])) ||
         !classic_execute_call_value(vm,&source,&arguments[count])) return 0;
      count++;
      while(isspace((unsigned char)*source)) source++;
      if(*source==','){
        source++;
        continue;
      }
      if(*source!=')') return 0;
      break;
    }
  }
  source++;
  while(isspace((unsigned char)*source)) source++;
  if(*source==';'){
    source++;
    while(isspace((unsigned char)*source)) source++;
  }
  if(*source) return 0;
  char code_name[160];
  snprintf(code_name,sizeof code_name,"gml_Script_%s",name);
  int code_index=gml_code_index_by_name(vm->win,code_name);
  if(code_index<0){
    snprintf(code_name,sizeof code_name,"gml_GlobalScript_%s",name);
    code_index=gml_code_index_by_name(vm->win,code_name);
  }
  if(code_index<0) return 0;
  if(builtin_setting(vm,"GML_LOG_AUDIO"))
    anygm_host_logf(vm->host,ANYGM_LOG_DEBUG,
                    "[execute_string] call %s argc=%d\n",name,count);
  GmlVal result=gml_vm_run_code(
      vm,code_index,vm->cur_self,vm->cur_other,arguments,count);
  gml_values_release(&result,1);
  return 1;
}
static int gm_datetime_calendar(double serial,AnygmCalendarTime *out){
  if(!out||!isfinite(serial)||serial<-100000000.0||serial>100000000.0) return 0;
  double seconds=(serial-25569.0)*86400.0;
  return anygm_calendar_from_unix_seconds((int64_t)floor(seconds+0.5),out);
}
/* Proleptic Gregorian civil date to days since the Unix epoch.
 * The GM serial origin is 25569 days before that epoch. */
static int64_t gm_days_from_civil(int y,int m,int d){
  int era; unsigned yoe,doy,doe;
  y-=m<=2;
  era=(y>=0?y:y-399)/400;
  yoe=(unsigned)(y-era*400);
  doy=(153u*(unsigned)(m+(m>2?-3:9))+2u)/5u+(unsigned)d-1u;
  doe=yoe*365u+yoe/4u-yoe/100u+doy;
  return (int64_t)era*146097+(int64_t)doe-719468;
}
static void gm_datetime_format(GmlVM *vm,double serial,uint32_t style,
                               char *text,size_t text_size){
  AnygmCalendarTime calendar;
  if(!text||!text_size) return;
  text[0]=0;
  if(!gm_datetime_calendar(serial,&calendar)) return;
  if(vm&&vm->host&&vm->host->date_time_format&&
     vm->host->date_time_format(vm->host->userdata,&calendar,style,text,text_size)==ANYGM_OK){
    text[text_size-1]=0;
    return;
  }
  if(style==ANYGM_DATE_FORMAT_DATE)
    snprintf(text,text_size,"%02d/%02d/%04d",calendar.month,calendar.day,calendar.year);
  else if(style==ANYGM_DATE_FORMAT_TIME)
    snprintf(text,text_size,"%02d:%02d:%02d",calendar.hour,calendar.minute,calendar.second);
  else
    snprintf(text,text_size,"%02d/%02d/%04d %02d:%02d:%02d",
             calendar.month,calendar.day,calendar.year,
             calendar.hour,calendar.minute,calendar.second);
}
static int vm_buffer_slot_from_addr(GmlVM *vm, double addr){
  uint32_t a=(uint32_t)(uint64_t)addr;
  if((a & 0xFFFF0000u) != 0xB0000000u) return -1;
  return vm_buffer_slot(vm,(int)(a & 0xFFFFu));
}

GmlVal gml_builtin_try_platform(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- platform/service stubs: host/offline/software-renderer environment ---- */
  if(!strcmp(nm,"application_get_position")){
    GmlRender *R=(GmlRender*)vm->render;
    double w=(double)presentation_size(vm,R,0);
    double h=(double)presentation_size(vm,R,1);
    return array4(0,0,w,h);
  }
  if(!strcmp(nm,"date_current_datetime")){
    AnygmWallTime wall={0};
    wall.struct_size=sizeof wall;
    if(gml_host_wall_time(vm,&wall)!=ANYGM_OK) return vreal(25569.0);
    int64_t seconds=wall.unix_seconds;
    if(wall.flags&ANYGM_WALL_TIME_OFFSET_VALID)
      seconds+=(int64_t)wall.utc_offset_minutes*60;
    return vreal(25569.0+(double)seconds/86400.0);
  }
  if(!strcmp(nm,"date_create_datetime")){
    int y=(int)N(a,n,0), mo=(int)N(a,n,1), d=(int)N(a,n,2);
    double h=N(a,n,3), mi=N(a,n,4), s=N(a,n,5);
    if(mo<1) mo=1;
    if(mo>12) mo=12;
    if(d<1) d=1;
    if(d>31) d=31;
    return vreal(25569.0+(double)gm_days_from_civil(y,mo,d)+(h*3600.0+mi*60.0+s)/86400.0);
  }
  if(!strcmp(nm,"date_second_span")) return vreal(fabs(N(a,n,0)-N(a,n,1))*86400.0);
  if(!strcmp(nm,"date_minute_span")) return vreal(fabs(N(a,n,0)-N(a,n,1))*1440.0);
  if(!strcmp(nm,"date_hour_span")) return vreal(fabs(N(a,n,0)-N(a,n,1))*24.0);
  if(!strcmp(nm,"date_day_span")) return vreal(fabs(N(a,n,0)-N(a,n,1)));
  if(!strcmp(nm,"date_week_span")) return vreal(fabs(N(a,n,0)-N(a,n,1))/7.0);
  if(!strcmp(nm,"date_get_second")||!strcmp(nm,"date_get_minute")||!strcmp(nm,"date_get_hour")||
     !strcmp(nm,"date_get_day")||!strcmp(nm,"date_get_month")||!strcmp(nm,"date_get_year")||
     !strcmp(nm,"date_get_weekday")){
    AnygmCalendarTime calendar;
    if(!gm_datetime_calendar(N(a,n,0),&calendar)) return vreal(0);
    if(!strcmp(nm,"date_get_second")) return vreal(calendar.second);
    if(!strcmp(nm,"date_get_minute")) return vreal(calendar.minute);
    if(!strcmp(nm,"date_get_hour")) return vreal(calendar.hour);
    if(!strcmp(nm,"date_get_day")) return vreal(calendar.day);
    if(!strcmp(nm,"date_get_month")) return vreal(calendar.month);
    if(!strcmp(nm,"date_get_weekday")) return vreal(calendar.weekday);
    return vreal(calendar.year);
  }
  if(!strcmp(nm,"date_datetime_string")||!strcmp(nm,"date_date_string")||
     !strcmp(nm,"date_time_string")){
    char formatted[160];
    uint32_t style=!strcmp(nm,"date_datetime_string")?ANYGM_DATE_FORMAT_DATE_TIME:
                   (!strcmp(nm,"date_date_string")?ANYGM_DATE_FORMAT_DATE:
                                                      ANYGM_DATE_FORMAT_TIME);
    gm_datetime_format(vm,N(a,n,0),style,formatted,sizeof formatted);
    char *owned=strdup(formatted);
    return owned?vstr_owned(owned):vstr("");
  }
  if(!strcmp(nm,"environment_get_variable")){
    const char *value=builtin_setting(vm,S(vm,a,n,0));
    return vstr(value?value:"");
  }
  if(!strcmp(nm,"os_get_info")){
    /* Expose the documented Windows-shaped metadata map without leaking host identity or
     * pretending that software rendering has native D3D objects. Stable empty and zero values
     * keep optional device paths offline. */
    int id=ds_map_create_id(vm);
    if(id<=0) return vreal(-1);
    static const char *const text_keys[]={
      "udid","video_adapter_vendorid","video_adapter_deviceid","video_adapter_subsysid",
      "video_adapter_revision","video_adapter_description","video_adapter_dedicatedvideomemory",
      "video_adapter_dedicatedsystemmemory","video_adapter_sharedsystemmemory"
    };
    static const char *const pointer_keys[]={
      "video_d3d11_device","video_d3d11_context","video_d3d11_swapchain"
    };
    ds_map_put(vm,id,vstr("is64bit"),vreal(sizeof(void*)==8),1);
    for(size_t i=0;i<sizeof(text_keys)/sizeof(text_keys[0]);i++)
      ds_map_put(vm,id,vstr(text_keys[i]),vstr(""),1);
    for(size_t i=0;i<sizeof(pointer_keys)/sizeof(pointer_keys[0]);i++)
      ds_map_put(vm,id,vstr(pointer_keys[i]),vreal(0),1);
    return vreal(id);
  }
  /* No native extension is present. Return false so callers can select portable paths; an
   * unknown builtin returns undefined instead of boolean false. */
  if(!strcmp(nm,"extension_exists")) return vreal(0);
  if(!strcmp(nm,"extension_stubfunc_real")) return vreal(0);
  if(!strcmp(nm,"extension_stubfunc_string")) return vstr("");
  if(!strcmp(nm,"extension_get_option_value")) return vstr("");
  if(!strcmp(nm,"get_string_async")) return vreal(0);
  if(!strcmp(nm,"get_string")) return vstr(n>=2?S(vm,a,n,1):"");
  if(!strcmp(nm,"get_integer")) return vreal(n>=2?N(a,n,1):0);
  if(!strcmp(nm,"get_integer_async")) return vreal(0);
  if(!strcmp(nm,"get_open_filename")||!strcmp(nm,"get_save_filename")) return vstr("");
  /* A host core cannot open a modal desktop dialog. Preserve deterministic
   * control flow by accepting the first button the game actually supplied;
   * classic show_message_ext returns that button's one-based slot. */
  if(!strcmp(nm,"show_message_ext")){
    for(int button=1;button<=3 && button<n;button++)
      if(S(vm,a,n,button)[0]) return vreal(button);
    return vreal(0);
  }
  if(!strcmp(nm,"show_error")){
    int abort_game=N(a,n,1)!=0.0;
    anygm_host_logf(vm ? vm->host : NULL,
                    abort_game?ANYGM_LOG_ERROR:ANYGM_LOG_WARN,
                    "[gml error] %s\n",S(vm,a,n,0));
    if(abort_game && vm) vm->game_end=1;
    return vreal(0);
  }
  if(!strcmp(nm,"execute_string")){
    const char *source=S(vm,a,n,0);
    int handled=classic_execute_globalvar(vm,source) ||
                classic_execute_assignment(vm,source) ||
                classic_execute_call(vm,source);
    if(!handled && builtin_setting(vm,"GML_LOG_UNKNOWN")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gml] unsupported execute_string: %s\n",S(vm,a,n,0));
    return vreal(0); }
  if(!strcmp(nm,"show_message")||!strcmp(nm,"show_message_async")||!strcmp(nm,"show_question")||!strcmp(nm,"action_message")||
     !strcmp(nm,"wd_message_simple")||
     !strcmp(nm,"message_button")||!strcmp(nm,"message_background")||
     !strcmp(nm,"message_text_font")||!strcmp(nm,"message_button_font")||
     !strcmp(nm,"message_input_font")||!strcmp(nm,"message_alpha")||
     !strcmp(nm,"message_position")||!strcmp(nm,"message_caption")||
     !strcmp(nm,"message_size")) return vreal(0);
  /* Host-process priority and MIDI tempo have no portable host control surface. They are
   * explicit compatibility no-ops rather than unresolved calls. */
  if(!strcmp(nm,"set_program_priority")||!strcmp(nm,"sound_background_tempo")) return vreal(0);
  if(!strcmp(nm,"show_info")||!strcmp(nm,"action_show_info")){
    if(vm && vm->win && anygm_policy_uses_classic_runtime(vm->win) &&
       vm->win->classic_game_information_size)
      vm->classic_info_active=1;
    return vreal(0);
  }
  if(!strcmp(nm,"parameter_count")) return vreal(vm?vm->parameter_count:0);
  if(!strcmp(nm,"parameter_string")){
    int index=(int)N(a,n,0);
    if(!vm || index<0 || index>vm->parameter_count) return vstr("");
    return vstr(index?vm->parameter_value[index-1]:vm->parameter_executable);
  }
  if(!strcmp(nm,"exception_unhandled_handler")) return vreal(0);
  if(!strcmp(nm,"io_clear")||!strcmp(nm,"keyboard_wait")) return vreal(0);
  if(!strcmp(nm,"display_set_windows_alternate_sync")) return vreal(0);
  if(!strcmp(nm,"url_open")) return vreal(0);
  if(!strcmp(nm,"os_is_network_connected")) return vreal(host_network_connected(vm));
  if(!strcmp(nm,"network_resolve")) return vstr("");
  if(!strcmp(nm,"network_create_socket")||!strcmp(nm,"network_create_server")||
     !strcmp(nm,"network_connect")) return vreal(-1);
  if(!strcmp(nm,"network_send_packet")||!strcmp(nm,"network_destroy")) return vreal(0);
  if(!strcmp(nm,"external_define"))
    return vreal(builtin_external_define(
        S(vm,a,n,0),S(vm,a,n,1)));
  if(!strcmp(nm,"external_call")){
    int handled=0;
    GmlVal result=builtin_external_call(
        vm,(int)N(a,n,0),n>1?a+1:NULL,n>1?n-1:0,&handled);
    return handled?result:vreal(0);
  }
  if(!strcmp(nm,"external_free")) return vreal(0);
  /* Optional desktop services have no process, window or network authority in the portable
   * runtime. Their adapters report a stable unavailable/offline state instead of pretending to
   * connect, launching a process, or falling through as an unresolved function. */
  if(!strncmp(nm,"FOCAL_Network",13)){
    if(strstr(nm,"MyName")||strstr(nm,"OpponentName")) return vstr("");
    if(strstr(nm,"MyID")||strstr(nm,"OpponentID")||strstr(nm,"Player")) return vreal(-1);
    return vreal(0);
  }
  if(!strncmp(nm,"FOCAL_Steam",11)) return vreal(0);
  if(!strncmp(nm,"Discord_",8)||!strncmp(nm,"discord_",8)||
     !strncmp(nm,"rousr",5)||!strncmp(nm,"NekoPresence",12)||
     !strncmp(nm,"tsuspresence",12)) return vreal(0);
  if(!strncmp(nm,"execute_shell_simple",20)||!strncmp(nm,"drago_",6)) return vreal(0);
  if(!strncmp(nm,"file_drop",9))
    return strstr(nm,"get")?vstr(""):vreal(0);
  if(!strncmp(nm,"HumbleAPI_",10)) return vreal(0);
  if(!strncmp(nm,"gmsched_",8)||!strncmp(nm,"scheduler_resolution_",21)) return vreal(0);
  if(!strncmp(nm,"window_command_",15)||!strncmp(nm,"Borderless",10)||
     !strncmp(nm,"borderless",10)||!strncmp(nm,"gamepad_force_focus",19)||
     !strncmp(nm,"catch_error",11)||!strncmp(nm,"CleanMem",8)||
     !strncmp(nm,"ram_",4)) return vreal(0);
  if(!strcmp(nm,"keyboard_virtual_show")||!strcmp(nm,"keyboard_virtual_hide")) return vreal(0);
  if(!strcmp(nm,"virtual_key_add")) return vreal(0);
  if(!strcmp(nm,"virtual_key_delete")) return vreal(0);
  if(!strcmp(nm,"achievement_available")||!strcmp(nm,"achievement_post_score")||
     !strcmp(nm,"achievement_show_leaderboards")) return vreal(0);
  if(!strcmp(nm,"analytics_addDesignEvent_windows")||
     !strcmp(nm,"addBusinessEventJson_windows")||
     !strcmp(nm,"addDesignEventWithValue_windows")||
     !strcmp(nm,"addDesignEvent_windows")||
     !strcmp(nm,"addErrorEvent_windows")||
     !strcmp(nm,"addProgressionEventWithScoreJson_windows")||
     !strcmp(nm,"addProgressionEvent_windows")||
     !strcmp(nm,"addResourceEventJson_windows")||
     !strcmp(nm,"analytics_configureAvailableCustomDimensions01_windows")||
     !strcmp(nm,"analytics_configureAvailableCustomDimensions02_windows")||
     !strcmp(nm,"analytics_configureAvailableCustomDimensions03_windows")||
     !strcmp(nm,"analytics_configureAvailableResourceCurrencies_windows")||
     !strcmp(nm,"analytics_configureAvailableResourceItemTypes_windows")||
     !strcmp(nm,"analytics_configureBuild_windows")||
     !strcmp(nm,"analytics_configureSdkGameEngineVersion_windows")||
     !strcmp(nm,"analytics_configureSdkWrapperVersion_windows")||
     !strcmp(nm,"analytics_configureUserId_windows")||
     !strcmp(nm,"analytics_initialize_windows")||
     !strcmp(nm,"analytics_setEnabledInfoLog_windows")||
     !strcmp(nm,"analytics_setEnabledManualSessionHandling_windows")||
     !strcmp(nm,"analytics_setEnabledVerboseLog_windows")||
     !strcmp(nm,"analytics_startSession_windows")||
     !strcmp(nm,"analytics_endSession_windows")||
     !strcmp(nm,"configureAvailableCustomDimensions01_windows")||
     !strcmp(nm,"configureAvailableCustomDimensions02_windows")||
     !strcmp(nm,"configureAvailableCustomDimensions03_windows")||
     !strcmp(nm,"configureAvailableResourceCurrencies_windows")||
     !strcmp(nm,"configureAvailableResourceItemTypes_windows")||
     !strcmp(nm,"configureBuild_windows")||
     !strcmp(nm,"configureSdkGameEngineVersion_windows")||
     !strcmp(nm,"configureUserId_windows")||
     !strcmp(nm,"endSession_windows")||
     !strcmp(nm,"native_ga_initialize_windows")||
     !strcmp(nm,"onResume_windows")||
     !strcmp(nm,"onStop_windows")||
     !strcmp(nm,"setCustomDimension01_windows")||
     !strcmp(nm,"setCustomDimension02_windows")||
     !strcmp(nm,"setCustomDimension03_windows")||
     !strcmp(nm,"setEnabledEventSubmission_windows")||
     !strcmp(nm,"setEnabledInfoLog_windows")||
     !strcmp(nm,"setEnabledManualSessionHandling_windows")||
     !strcmp(nm,"setEnabledVerboseLog_windows")||
     !strcmp(nm,"startSession_windows")) return vreal(0);
  if(!strcmp(nm,"isRemoteConfigsReady_windows")) return vreal(0);
  if(!strcmp(nm,"getRemoteConfigsContentAsString_windows")||
     !strcmp(nm,"getRemoteConfigsValueAsString_windows")) return vstr("");
  if(!strcmp(nm,"getRemoteConfigsValueAsStringWithDefaultValue_windows")) return vstr(n>=2?S(vm,a,n,1):"");
  if(!strcmp(nm,"switch_get_operation_mode")) return vreal(0);
  if(!strcmp(nm,"switch_controller_support_set_player_max")||
     !strcmp(nm,"switch_controller_support_set_player_min")) return vreal(0);
  if(!strcmp(nm,"switch_save_data_commit")||
     !strcmp(nm,"switch_save_data_mount")) return vreal(1);
  if(!strcmp(nm,"switch_accounts_open_preselected_user")||
     !strcmp(nm,"switch_accounts_open_user")||
     !strcmp(nm,"switch_accounts_select_account")||
     !strcmp(nm,"switch_controller_joycon_set_holdtype")||
     !strcmp(nm,"switch_controller_set_supported_styles")||
     !strcmp(nm,"switch_controller_support_get_selected_id")||
     !strcmp(nm,"switch_controller_support_set_defaults")||
     !strcmp(nm,"switch_controller_support_set_singleplayer_only")||
     !strcmp(nm,"switch_controller_support_show")) return vreal(0);
  /* Gameframe is a native window-frame extension. A host core has no host HWND to style, so
   * report the extension unavailable and make its raw Win32 calls explicit no-ops. The GML wrapper
   * already has portable fallbacks for the unavailable path. */
  if(!strcmp(nm,"gameframe_check_native_extension")) return vreal(0);
  if(!strcmp(nm,"gameframe_init_raw_raw")||!strcmp(nm,"gameframe_set_shadow")||
     !strcmp(nm,"gameframe_syscommand_raw")) return vreal(0);
  if(!strcmp(nm,"gameframe_get_shadow")) return vreal(0);
  if(!strcmp(nm,"gameframe_mouse_in_window_raw")) return vreal(1);
  if(!strcmp(nm,"gameframe_is_natively_minimized_raw")) return vreal(0);
  if(!strcmp(nm,"gameframe_get_double_click_time_raw")) return vreal(500);
  if(!strcmp(nm,"gameframe_get_monitors_1")||!strcmp(nm,"gameframe_get_monitors_1_raw")) return vreal(1);
  if(!strcmp(nm,"gameframe_get_monitors_2")||!strcmp(nm,"gameframe_get_monitors_2_raw")){
    int bi=vm_buffer_slot_from_addr(vm,N(a,n,0));
    if(bi>=0){
      GmlRender *R=(GmlRender*)vm->render;
      GmlRenderTargetMetrics target=builtin_target_metrics(R);
      int32_t w=target.width>0 ? target.width : (vm&&vm->window_w>0 ? vm->window_w : 1280);
      int32_t h=target.height>0 ? target.height : (vm&&vm->window_h>0 ? vm->window_h : 720);
      int32_t mon[9]={0,0,w,h,0,0,w,h,96};
      vm->builtins->buffer[bi].pos=0;
      buffer_write_raw(vm,bi,mon,(int)sizeof(mon));
      vm->builtins->buffer[bi].pos=0;
    }
    return vreal(1);
  }
  if(!strcmp(nm,"texturegroup_get_textures")) return arr_newv(0);
  if(!strcmp(nm,"texturegroup_get_status")) return vreal(3);   /* loaded */
  if(!strcmp(nm,"texturegroup_load")||!strcmp(nm,"texturegroup_unload")||
     !strcmp(nm,"texturegroup_set_mode")) return vreal(0);
  if(!strcmp(nm,"texture_is_ready")) return vreal(1);
  if(!strcmp(nm,"texture_prefetch")||!strcmp(nm,"texture_flush")||!strcmp(nm,"draw_texture_flush")||
     !strcmp(nm,"sprite_prefetch")||!strcmp(nm,"sprite_flush")||!strcmp(nm,"sprite_flush_multi")||
     !strcmp(nm,"sprite_prefetch_multi")) return vreal(0);
  return gml_builtin_try_ds(vm,nm,a,n);
}

GmlVal gml_builtin_try_platform_extensions(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  if(!strncmp(nm,"skeleton_",9)) return builtin_skeleton(vm,nm,a,n);
  /* GOG Galaxy compatibility reports initialized but signed out so content can take its offline
   * path instead of waiting for an unavailable service. */
  if(!strcmp(nm,"gog_init")||!strcmp(nm,"gog_is_initialised")||!strcmp(nm,"gog_is_initialized")) return vreal(1);
  if(!strcmp(nm,"gog_get_achievement")||!strcmp(nm,"gog_is_user_logged_on")||
     !strcmp(nm,"gog_set_achievement")||!strcmp(nm,"gog_stats_ready")||
     !strcmp(nm,"gog_update")) return vreal(0);
  if(!strncmp(nm,"gog_",4)) return vreal(0);
  if(!strcmp(nm,"GOG_GetError")||!strcmp(nm,"GOG_ProcessData")||!strcmp(nm,"GOG_Shutdown")||
     !strcmp(nm,"GOG_Stats_GetAchievement")||!strcmp(nm,"GOG_Stats_RequestUserStatsAndAchievements")||
     !strcmp(nm,"GOG_Stats_SetAchievement")||!strcmp(nm,"GOG_Stats_StoreStatsAndAchievements")||
     !strcmp(nm,"GOG_User_SignInGalaxy")||!strcmp(nm,"GOG_User_SignedIn")) return vreal(0);
  if(!strncmp(nm,"GOG_",4)) return vreal(0);
  /* No debug overlay is present, so report it closed. This preserves a boolean result for callers
   * deciding whether to draw diagnostics. */
  if(!strcmp(nm,"is_debug_overlay_open")) return vreal(0);
  if(!strcmp(nm,"show_debug_message")||!strcmp(nm,"show_debug_overlay")){
    const char *message=S(vm,a,n,0);
    /* A suite that asserts on what the content printed needs a stream carrying nothing else, so
     * this development setting marks the line with a stable prefix and the frontend routes it to
     * standard output while every other diagnostic stays on the log. Portable code cannot touch
     * stdio — it used to write here directly, which is a rule the architecture check enforces and
     * this violated — and the prefix is what carries the routing decision across that boundary.
     * Only the message call is marked: the overlay call takes a flag, not text. */
    if(!strcmp(nm,"show_debug_message") && builtin_setting(vm,"GML_TEST_PRINT"))
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_INFO,"[gml-test] %s\n",message);
    if(builtin_setting(vm,"GML_LOG_DEBUGMSG")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gml debug] %s\n", message);
    return vreal(0); }
  if(!strncmp(nm,"xboxone_",8)) return vreal(0);
  /* The host receives one completed video frame per anygm_run_frame. Desktop refresh/vsync calls
   * cannot expose an intermediate buffer here, and waiting would only stall emulation. */
  if(!strcmp(nm,"screen_wait_vsync")||
     !strcmp(nm,"screen_save")||!strcmp(nm,"screen_save_part")) return vreal(0);
  if(!strcmp(nm,"screen_refresh")){
    if(vm && vm->win && anygm_policy_uses_classic_runtime(vm->win)){
      if(vm->present_latch_hook) vm->present_latch_hook(vm,vm->present_latch_hook_user);
    }
    return vreal(0);
  }
  /* Repeat the room draw sequence in the current target without presenting or
   * waiting. Nested redraw requests must not recurse. */
  if(!strcmp(nm,"screen_redraw")){
    if(!vm || !vm->render || vm->in_screen_redraw) return vreal(0);
    vm->in_screen_redraw=1;
    if(vm->room_layer_hook) vm->room_layer_hook(vm,0,vm->room_layer_hook_user);
    gml_vm_draw_pass(vm,"Draw_72");
    gml_vm_draw(vm);
    gml_vm_draw_pass(vm,"Draw_73");
    if(vm->room_layer_hook) vm->room_layer_hook(vm,1,vm->room_layer_hook_user);
    vm->in_screen_redraw=0;
    return vreal(0);
  }
  if(!strcmp(nm,"os_get_language")) return vstr(vm->os_language[0]?vm->os_language:"en");
  /* The locale contract exposes language lowercase and region uppercase. */
  if(!strcmp(nm,"os_get_region")) return vstr(vm->os_region[0]?vm->os_region:"US");
  /* Console language extensions expose the same user preference in a fuller tag.
   * Treating this as platform metadata keeps extension-bearing desktop exports on
   * their normal localization path without emulating a console service. */
  if(!strcmp(nm,"switch_language_get_desired_language"))
    return vstr(vm->language_tag[0]?vm->language_tag:"en-US");
  if(!strcmp(nm,"os_get_config")) return vstr("default");
  if(!strcmp(nm,"gml_release_mode")) return vreal(0);
  if(!strcmp(nm,"os_is_paused")) return vreal(0);
  if(!strcmp(nm,"window_has_focus")) return vreal(1);
  if(!strcmp(nm,"window_get_x")) return vreal(vm->window_x);
  if(!strcmp(nm,"window_get_y")) return vreal(vm->window_y);
  if(!strcmp(nm,"window_get_cursor")) return vreal(vm->window_cursor);
  if(!strcmp(nm,"window_get_caption")) return vstr("");
  if(!strcmp(nm,"window_handle")) return vreal(1);
  if(!strcmp(nm,"window_get_fullscreen")) return vreal(vm->window_fullscreen);
  if(!strcmp(nm,"window_set_fullscreen")){ vm->window_fullscreen=N(a,n,0)>=0.5; return vreal(0); }
  if(!strcmp(nm,"window_enable_borderless_fullscreen")) return vreal(0);
  if(!strcmp(nm,"action_fullscreen")){
    int mode=(int)N(a,n,0);
    vm->window_fullscreen=mode==2?!vm->window_fullscreen:(mode!=0);
    return vreal(0);
  }
  if(!strcmp(nm,"window_center")){ vm->window_x=0; vm->window_y=0; return vreal(0); }
  if(!strcmp(nm,"window_set_position")){ vm->window_x=N(a,n,0); vm->window_y=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"window_set_size")){ int w=(int)N(a,n,0), hh=(int)N(a,n,1);
    if(w>0 && hh>0 && w<=16384 && hh<=16384){ vm->window_w=w; vm->window_h=hh; }
    return vreal(0); }
  if(!strcmp(nm,"window_set_rectangle")){ int w=(int)N(a,n,2), hh=(int)N(a,n,3);
    vm->window_x=N(a,n,0); vm->window_y=N(a,n,1);
    if(w>0 && hh>0 && w<=16384 && hh<=16384){ vm->window_w=w; vm->window_h=hh; }
    return vreal(0); }
  if(!strcmp(nm,"window_set_cursor")){ vm->window_cursor=(int)N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"window_set_caption")) return vreal(0);
  if(!strcmp(nm,"display_set_gui_maximise") || !strcmp(nm,"display_set_gui_maximize")){
    GmlRender *R=(GmlRender*)vm->render;
    GmlRenderPresentationMetrics presentation=builtin_presentation_metrics(R);
    if(n<=0){
      vm->gui_maximise_active=1;
      vm->gui_maximise_xscale=vm->gui_maximise_yscale=0.0;
      vm->gui_maximise_xoffset=vm->gui_maximise_yoffset=0.0;
    } else {
      double xs=N(a,n,0), ys=n>1?N(a,n,1):xs;
      if(xs<0.0 || ys<0.0){
        vm->gui_maximise_active=0;
        vm->gui_maximise_xscale=vm->gui_maximise_yscale=0.0;
        vm->gui_maximise_xoffset=vm->gui_maximise_yoffset=0.0;
      } else if(isfinite(xs) && isfinite(ys) && xs>0.0 && ys>0.0){
        double xo=n>2?N(a,n,2):0.0, yo=n>3?N(a,n,3):0.0;
        vm->gui_maximise_active=1;
        vm->gui_maximise_xscale=xs;
        vm->gui_maximise_yscale=ys;
        vm->gui_maximise_xoffset=isfinite(xo)?xo:0.0;
        vm->gui_maximise_yoffset=isfinite(yo)?yo:0.0;
      }
    }
    if(R && presentation.gui_pass_active)
      gml_render_gui_set_maximise(R,vm->gui_maximise_active,
        vm->gui_maximise_xscale,vm->gui_maximise_yscale,
        vm->gui_maximise_xoffset,vm->gui_maximise_yoffset,
        vm->window_w>0?vm->window_w:(vm->win?(int)vm->win->disp_w:0),
        vm->window_h>0?vm->window_h:(vm->win?(int)vm->win->disp_h:0));
    return vreal(0);
  }

  GmlVal layer_out;
  if(builtin_layer_exact(vm,nm,a,n,&layer_out)) return layer_out;

  return gml_builtin_try_platform_noops(vm,nm,a,n);
}

GmlVal gml_builtin_try_platform_noops(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- explicit no-ops (correct for host / SW renderer) ---- */
  if(!strcmp(nm,"display_set_gui_size")){ int w=(int)N(a,n,0), hh=(int)N(a,n,1);
    if((w>0 && hh>0 && w<=16384 && hh<=16384) || (w<0 && hh<0)){
      GmlRender *R=(GmlRender*)vm->render;
      GmlRenderPresentationMetrics presentation=builtin_presentation_metrics(R);
      vm->gui_w=w>0?w:0; vm->gui_h=hh>0?hh:0;
      /* GUI size owns the transform when it is called after GUI maximise. */
      vm->gui_maximise_active=0;
      vm->gui_maximise_xscale=vm->gui_maximise_yscale=0.0;
      vm->gui_maximise_xoffset=vm->gui_maximise_yoffset=0.0;
      if(R && presentation.gui_pass_active)
        gml_render_gui_set_maximise(R,0,0.0,0.0,0.0,0.0,0,0);
      /* Modern format semantics apply GUI-size changes immediately inside an active Draw GUI
       * event. Classic and Studio 1 retain the legacy presentation path; transforming earlier
       * draws would break the fixed-width transition and compositor semantics. */
      if(vm->win && anygm_policy_has_modern_function_values(vm->win))
        gml_render_gui_set_size(R,w>0?w:presentation.gui_base_width,
                                 hh>0?hh:presentation.gui_base_height);
    }
    return vreal(0); }
  if(!strcmp(nm,"texture_set_interpolation")){ GmlRender *R=(GmlRender*)vm->render;
    builtin_set_interpolation(R,N(a,n,0)!=0.0);
    return vreal(0); }
  if(!strcmp(nm,"texture_set_repeat")) return vreal(0);
  if(!strcmp(nm,"gpu_set_texfilter")||!strcmp(nm,"gpu_set_texfilter_ext")||
     !strcmp(nm,"gpu_set_tex_filter")||!strcmp(nm,"gpu_set_tex_filter_ext")){
    int enable=(!strcmp(nm,"gpu_set_texfilter_ext")||!strcmp(nm,"gpu_set_tex_filter_ext")) && n>1 ? 1 : 0;
    GmlRender *R=(GmlRender*)vm->render; builtin_set_interpolation(R,N(a,n,enable)!=0.0); return vreal(0); }
  if(!strcmp(nm,"gpu_get_texfilter")||!strcmp(nm,"gpu_get_tex_filter")){
    GmlRender *R=(GmlRender*)vm->render; return vreal(builtin_draw_state(R).interpolation); }
  if(!strcmp(nm,"gpu_set_tex_repeat")||!strcmp(nm,"gpu_set_tex_repeat_ext")||
     !strcmp(nm,"gpu_set_texrepeat")||!strcmp(nm,"gpu_set_texrepeat_ext")) return vreal(0);
  if(!strcmp(nm,"gpu_set_blendenable")){ GmlRender *R=(GmlRender*)vm->render; builtin_set_alpha_blend(R,N(a,n,0)>=0.5); return vreal(0); }
  if(!strcmp(nm,"gpu_get_blendenable")){ GmlRender *R=(GmlRender*)vm->render; return vreal(builtin_draw_state(R).alpha_blend); }
  if(!strcmp(nm,"window_get_fullscreen")) return vreal(vm->window_fullscreen);
  if(!strcmp(nm,"window_set_fullscreen")){ vm->window_fullscreen=N(a,n,0)>=0.5; return vreal(0); }

  return gml_builtin_try_scripts_fallback(vm,nm,a,n);
}
/* Platform and offline-capability implementation. */
