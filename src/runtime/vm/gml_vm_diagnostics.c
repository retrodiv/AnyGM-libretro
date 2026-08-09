/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_vm_internal.h"
#include "gml_value_internal.h"

#include "anygm_host.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  GML_VM_TRACE_OPCODE=1u<<0,
  GML_VM_TRACE_EVENT=1u<<1,
  GML_VM_TRACE_COLLISION=1u<<2,
  GML_VM_TRACE_VARIABLE=1u<<3
};

typedef enum {
  GML_VM_TRACE_INSTANCE_NONE=0,
  GML_VM_TRACE_INSTANCE_ID,
  GML_VM_TRACE_INSTANCE_GLOBAL
} GmlVmTraceInstanceKind;

typedef struct GmlVmFineTrace {
  unsigned enabled;
  long frame_first;
  long frame_last;
  unsigned long limit;
  unsigned long records;
  int initialized;
  int limited;
  GmlVmTraceInstanceKind instance_kind;
  uint32_t instance_id;
  char code[128];
  char object[128];
  char variable[128];
} GmlVmFineTrace;

static int trace_copy_selector(char *output,size_t output_size,
                               const char *input){
  if(!output || !output_size) return 0;
  output[0]=0;
  if(!input) return 1;
  size_t length=strlen(input);
  if(!length || length>=output_size) return 0;
  for(size_t index=0;index<length;index++){
    unsigned char value=(unsigned char)input[index];
    if(value<0x20u || value>0x7eu) return 0;
  }
  memcpy(output,input,length+1u);
  return 1;
}

static int trace_parse_long(const char *text,long minimum,long maximum,
                            long *value){
  if(!text || !*text || !value) return 0;
  errno=0;
  char *end=NULL;
  long parsed=strtol(text,&end,10);
  if(errno || !end || *end || parsed<minimum || parsed>maximum) return 0;
  *value=parsed;
  return 1;
}

static int trace_parse_frames(const char *text,long *first,long *last){
  if(!text || !*text || !first || !last) return 0;
  const char *separator=strchr(text,':');
  if(!separator || separator==text || separator[1]==0 ||
     strchr(separator+1,':')) return 0;
  size_t left_length=(size_t)(separator-text);
  if(left_length>=32u || strlen(separator+1)>=32u) return 0;
  char left[32];
  memcpy(left,text,left_length);
  left[left_length]=0;
  if(!trace_parse_long(left,0,LONG_MAX,first) ||
     !trace_parse_long(separator+1,0,LONG_MAX,last) ||
     *last<*first) return 0;
  return 1;
}

static int trace_parse_types(const char *text,unsigned *types){
  if(!text || !*text || !types || strlen(text)>=64u) return 0;
  char copy[64];
  memcpy(copy,text,strlen(text)+1u);
  unsigned parsed=0;
  char *cursor=copy;
  while(cursor && *cursor){
    char *next=strchr(cursor,',');
    if(next) *next++=0;
    unsigned bit=!strcmp(cursor,"opcode")?GML_VM_TRACE_OPCODE:
                 !strcmp(cursor,"event")?GML_VM_TRACE_EVENT:
                 !strcmp(cursor,"collision")?GML_VM_TRACE_COLLISION:
                 !strcmp(cursor,"variable")?GML_VM_TRACE_VARIABLE:0u;
    if(!bit || (parsed&bit)) return 0;
    parsed|=bit;
    cursor=next;
  }
  if(!parsed) return 0;
  *types=parsed;
  return 1;
}

static void trace_config_error(GmlVM *vm,const char *setting,
                               const char *reason){
  anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_DEBUG,
    "{\"schema\":\"anygm.vm.trace\",\"version\":1,\"sequence\":0,"
    "\"kind\":\"config_error\",\"setting\":\"%s\",\"reason\":\"%s\"}\n",
    setting,reason);
}

static int trace_parse_instance(GmlVmFineTrace *trace,const char *text){
  if(!text) return 1;
  if(!strcmp(text,"global")){
    trace->instance_kind=GML_VM_TRACE_INSTANCE_GLOBAL;
    return 1;
  }
  if(!*text) return 0;
  for(const char *cursor=text;*cursor;cursor++)
    if(*cursor<'0' || *cursor>'9') return 0;
  errno=0;
  char *end=NULL;
  unsigned long value=strtoul(text,&end,10);
  if(errno || !end || *end || value>UINT32_MAX) return 0;
  trace->instance_kind=GML_VM_TRACE_INSTANCE_ID;
  trace->instance_id=(uint32_t)value;
  return 1;
}

static GmlVmFineTrace *trace_get(GmlVM *vm){
  if(!vm) return NULL;
  if(!vm->fine_trace){
    vm->fine_trace=calloc(1,sizeof(*vm->fine_trace));
    if(!vm->fine_trace) return NULL;
  }
  GmlVmFineTrace *trace=vm->fine_trace;
  if(trace->initialized) return trace;
  trace->initialized=1;
  const char *types=anygm_host_development_setting(vm->host,"GML_DIAGNOSTICS");
  if(!types || !*types) return trace;
  if(!trace_parse_types(types,&trace->enabled)){
    trace_config_error(vm,"GML_DIAGNOSTICS","invalid trace type list");
    trace->enabled=0;
    return trace;
  }
  const char *frames=anygm_host_development_setting(
      vm->host,"GML_DIAGNOSTICS_FRAMES");
  if(!trace_parse_frames(frames,&trace->frame_first,&trace->frame_last)){
    trace_config_error(vm,"GML_DIAGNOSTICS_FRAMES",
                       frames?"invalid inclusive frame range":"required");
    trace->enabled=0;
    return trace;
  }
  const char *limit=anygm_host_development_setting(
      vm->host,"GML_DIAGNOSTICS_LIMIT");
  long parsed_limit=0;
  if(!trace_parse_long(limit,1,1000000,&parsed_limit)){
    trace_config_error(vm,"GML_DIAGNOSTICS_LIMIT",
                       limit?"expected integer in [1,1000000]":"required");
    trace->enabled=0;
    return trace;
  }
  trace->limit=(unsigned long)parsed_limit;
  if(!trace_copy_selector(
       trace->code,sizeof(trace->code),
       anygm_host_development_setting(vm->host,"GML_DIAGNOSTICS_CODE"))){
    trace_config_error(vm,"GML_DIAGNOSTICS_CODE",
                       "expected 1 to 127 printable ASCII bytes");
    trace->enabled=0;
    return trace;
  }
  if(!trace_copy_selector(
       trace->object,sizeof(trace->object),
       anygm_host_development_setting(vm->host,"GML_DIAGNOSTICS_OBJECT"))){
    trace_config_error(vm,"GML_DIAGNOSTICS_OBJECT",
                       "expected 1 to 127 printable ASCII bytes");
    trace->enabled=0;
    return trace;
  }
  if(!trace_copy_selector(
       trace->variable,sizeof(trace->variable),
       anygm_host_development_setting(vm->host,"GML_DIAGNOSTICS_VARIABLE"))){
    trace_config_error(vm,"GML_DIAGNOSTICS_VARIABLE",
                       "expected 1 to 127 printable ASCII bytes");
    trace->enabled=0;
    return trace;
  }
  const char *instance=anygm_host_development_setting(
      vm->host,"GML_DIAGNOSTICS_INSTANCE");
  if(!trace_parse_instance(trace,instance)){
    trace_config_error(vm,"GML_DIAGNOSTICS_INSTANCE",
                       "expected global or an unsigned decimal id");
    trace->enabled=0;
    return trace;
  }
  if((trace->enabled&GML_VM_TRACE_OPCODE) && !trace->code[0]){
    trace_config_error(vm,"GML_DIAGNOSTICS_CODE",
                       "required for opcode tracing");
    trace->enabled=0;
    return trace;
  }
  if((trace->enabled&(GML_VM_TRACE_EVENT|GML_VM_TRACE_COLLISION)) &&
     !trace->object[0] &&
     trace->instance_kind!=GML_VM_TRACE_INSTANCE_ID){
    trace_config_error(vm,"GML_DIAGNOSTICS_OBJECT",
                       "object or numeric instance selector required");
    trace->enabled=0;
    return trace;
  }
  if((trace->enabled&GML_VM_TRACE_VARIABLE) &&
     (!trace->variable[0] ||
      (!trace->object[0] &&
       trace->instance_kind==GML_VM_TRACE_INSTANCE_NONE))){
    trace_config_error(vm,"GML_DIAGNOSTICS_VARIABLE",
                       "variable plus object, global, or instance selector required");
    trace->enabled=0;
  }
  if((trace->enabled&(GML_VM_TRACE_EVENT|GML_VM_TRACE_COLLISION)) &&
     trace->instance_kind==GML_VM_TRACE_INSTANCE_GLOBAL){
    trace_config_error(vm,"GML_DIAGNOSTICS_INSTANCE",
                       "global is valid only for variable tracing");
    trace->enabled=0;
  }
  return trace;
}

static const char *trace_code_name(GmlVM *vm,int code_index){
  if(!vm || !vm->win || code_index<0 || code_index>=vm->win->n_code)
    return "";
  return vm->win->code[code_index].name?vm->win->code[code_index].name:"";
}

static int trace_frame_matches(const GmlVM *vm,const GmlVmFineTrace *trace){
  return vm && trace && trace->enabled && !trace->limited &&
         vm->frame>=trace->frame_first && vm->frame<=trace->frame_last;
}

static int trace_code_matches(const GmlVmFineTrace *trace,const char *name){
  return trace && (!trace->code[0] ||
         (name && strstr(name,trace->code)!=NULL));
}

static const char *trace_object_name(const GmlVM *vm,
                                     const GmlInstance *instance){
  if(!vm || !instance || instance->obj<0 || instance->obj>=vm->n_objects)
    return "";
  return vm->objects[instance->obj].name?vm->objects[instance->obj].name:"";
}

static int trace_instance_matches(const GmlVM *vm,
                                  const GmlVmFineTrace *trace,
                                  const GmlInstance *instance){
  if(!trace || !instance) return 0;
  if(trace->instance_kind==GML_VM_TRACE_INSTANCE_ID &&
     instance->id!=trace->instance_id) return 0;
  if(trace->object[0] &&
     strcmp(trace_object_name(vm,instance),trace->object)) return 0;
  return 1;
}

static int trace_pair_matches(const GmlVM *vm,const GmlVmFineTrace *trace,
                              const GmlInstance *first,
                              const GmlInstance *second){
  if(!trace || !first || !second) return 0;
  if(trace->instance_kind==GML_VM_TRACE_INSTANCE_ID &&
     first->id!=trace->instance_id && second->id!=trace->instance_id)
    return 0;
  if(trace->object[0] &&
     strcmp(trace_object_name(vm,first),trace->object) &&
     strcmp(trace_object_name(vm,second),trace->object))
    return 0;
  return 1;
}

static size_t trace_json_string(char *output,size_t output_size,
                                const char *input,int *truncated){
  if(truncated) *truncated=0;
  if(!output || !output_size) return 0;
  size_t used=0;
  if(!input) input="";
  for(size_t index=0;input[index];index++){
    unsigned char value=(unsigned char)input[index];
    char escape[7];
    size_t escape_size=0;
    if(value=='"' || value=='\\'){
      escape[0]='\\'; escape[1]=(char)value; escape_size=2;
    } else if(value=='\b'){
      escape[0]='\\'; escape[1]='b'; escape_size=2;
    } else if(value=='\f'){
      escape[0]='\\'; escape[1]='f'; escape_size=2;
    } else if(value=='\n'){
      escape[0]='\\'; escape[1]='n'; escape_size=2;
    } else if(value=='\r'){
      escape[0]='\\'; escape[1]='r'; escape_size=2;
    } else if(value=='\t'){
      escape[0]='\\'; escape[1]='t'; escape_size=2;
    } else if(value<0x20u || value>=0x7fu){
      snprintf(escape,sizeof escape,"\\u%04x",(unsigned)value);
      escape_size=6;
    } else {
      escape[0]=(char)value; escape_size=1;
    }
    if(used+escape_size>=output_size){
      if(truncated) *truncated=1;
      break;
    }
    memcpy(output+used,escape,escape_size);
    used+=escape_size;
  }
  output[used]=0;
  return used;
}

static int trace_reserve(GmlVM *vm,GmlVmFineTrace *trace){
  if(!trace || trace->limited) return 0;
  if(trace->records+1u>=trace->limit){
    trace->records++;
    trace->limited=1;
    anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_DEBUG,
      "{\"schema\":\"anygm.vm.trace\",\"version\":1,\"sequence\":%lu,"
      "\"kind\":\"limit\",\"frame\":%ld,\"limit\":%lu}\n",
      trace->records-1u,vm?vm->frame:0,trace->limit);
    return 0;
  }
  trace->records++;
  return 1;
}

static unsigned long trace_sequence(const GmlVmFineTrace *trace){
  return trace&&trace->records?trace->records-1u:0u;
}

static void trace_escape_names(const char *first,const char *second,
                               char first_json[384],char second_json[384]){
  trace_json_string(first_json,384,first,NULL);
  trace_json_string(second_json,384,second,NULL);
}

void gml_vm_diagnostics_opcode(GmlVM *vm,const char *code_name,
                               uint32_t offset,const char *opcode,
                               int stack_depth){
  GmlVmFineTrace *trace=trace_get(vm);
  if(!trace_frame_matches(vm,trace) ||
     !(trace->enabled&GML_VM_TRACE_OPCODE) ||
     !trace_code_matches(trace,code_name) ||
     !trace_reserve(vm,trace)) return;
  char code_json[384],opcode_json[384];
  trace_escape_names(code_name,opcode,code_json,opcode_json);
  uint32_t self_id=vm&&vm->cur_self?vm->cur_self->id:0u;
  anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_DEBUG,
    "{\"schema\":\"anygm.vm.trace\",\"version\":1,\"sequence\":%lu,"
    "\"kind\":\"opcode\",\"frame\":%ld,\"code\":\"%s\","
    "\"offset\":%u,\"opcode\":\"%s\",\"stack_depth\":%d,"
    "\"self\":%u}\n",
    trace_sequence(trace),vm->frame,code_json,offset,opcode_json,
    stack_depth,self_id);
}

/* Optional host-backed array growth reporting at doubling thresholds. */
static GmlVM *growth_vm=NULL;
static int growth_threshold=0;
static int growth_next=0;
static void growth_report(int index,int length){
  if(!growth_vm || index<growth_threshold || index<growth_next) return;
  growth_next = index ? index*2 : 1;
  anygm_host_logf(growth_vm->host,ANYGM_LOG_DEBUG,
    "[arrgrow] index %d (len was %d) in %s\n",
    index,length,trace_code_name(growth_vm,growth_vm->cur_code_index));
}
void gml_vm_diagnostics_array_growth_init(GmlVM *vm){
  if(!vm) return;
  const char *v=anygm_host_development_setting(vm->host,"GML_LOG_ARR_GROWTH");
  if(!v || !*v){ gml_arr_growth_hook=NULL; growth_vm=NULL; return; }
  int want=atoi(v);
  growth_threshold = want>0 ? want : 1024;
  growth_next = growth_threshold;
  growth_vm = vm;
  gml_arr_growth_hook = growth_report;
}

int gml_vm_diagnostics_opcode_enabled(GmlVM *vm,const char *code_name){
  GmlVmFineTrace *trace=trace_get(vm);
  return trace_frame_matches(vm,trace) &&
         (trace->enabled&GML_VM_TRACE_OPCODE) &&
         trace_code_matches(trace,code_name);
}

void gml_vm_diagnostics_event(GmlVM *vm,const GmlInstance *instance,
                              const char *event_name,int code_index){
  GmlVmFineTrace *trace=trace_get(vm);
  const char *code_name=trace_code_name(vm,code_index);
  if(!trace_frame_matches(vm,trace) ||
     !(trace->enabled&GML_VM_TRACE_EVENT) ||
     !trace_instance_matches(vm,trace,instance) ||
     !trace_code_matches(trace,code_name) ||
     !trace_reserve(vm,trace)) return;
  char object_json[384],event_json[384],code_json[384];
  trace_escape_names(trace_object_name(vm,instance),event_name,
                     object_json,event_json);
  trace_json_string(code_json,sizeof code_json,code_name,NULL);
  anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_DEBUG,
    "{\"schema\":\"anygm.vm.trace\",\"version\":1,\"sequence\":%lu,"
    "\"kind\":\"event\",\"frame\":%ld,\"instance\":%u,"
    "\"object\":\"%s\",\"event\":\"%s\",\"code\":\"%s\"}\n",
    trace_sequence(trace),vm->frame,instance->id,object_json,event_json,
    code_json);
}

void gml_vm_diagnostics_collision(GmlVM *vm,const GmlInstance *first,
                                  const GmlInstance *second,int code_index,
                                  int hit){
  GmlVmFineTrace *trace=trace_get(vm);
  const char *code_name=trace_code_name(vm,code_index);
  if(!trace_frame_matches(vm,trace) ||
     !(trace->enabled&GML_VM_TRACE_COLLISION) ||
     !trace_pair_matches(vm,trace,first,second) ||
     !trace_code_matches(trace,code_name) ||
     !trace_reserve(vm,trace)) return;
  char first_json[384],second_json[384],code_json[384];
  trace_escape_names(trace_object_name(vm,first),
                     trace_object_name(vm,second),
                     first_json,second_json);
  trace_json_string(code_json,sizeof code_json,code_name,NULL);
  anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_DEBUG,
    "{\"schema\":\"anygm.vm.trace\",\"version\":1,\"sequence\":%lu,"
    "\"kind\":\"collision\",\"frame\":%ld,\"first_instance\":%u,"
    "\"first_object\":\"%s\",\"second_instance\":%u,"
    "\"second_object\":\"%s\",\"hit\":%s,\"code\":\"%s\"}\n",
    trace_sequence(trace),vm->frame,first->id,first_json,second->id,
    second_json,hit?"true":"false",code_json);
}

static const GmlInstance *trace_scope_instance(GmlVM *vm,int scope){
  if(!vm) return NULL;
  if(scope==IT_SELF) return vm->cur_self;
  if(scope==IT_OTHER) return vm->cur_other;
  if(scope>=0){
    for(int index=0;index<vm->inst_count;index++){
      GmlInstance *instance=&vm->inst[index];
      if(instance->active && !instance->marked &&
         (instance->id==(uint32_t)scope || instance->obj==scope))
        return instance;
    }
  }
  return NULL;
}

static int trace_variable_matches(GmlVM *vm,GmlVmFineTrace *trace,
                                  int global,
                                  const GmlInstance *instance,
                                  const char *name){
  if(!trace || !name || strcmp(name,trace->variable)) return 0;
  if(trace->instance_kind==GML_VM_TRACE_INSTANCE_GLOBAL) return global;
  if(global) return 0;
  return trace_instance_matches(vm,trace,instance);
}

static void trace_variable_value(char output[512],GmlVal value){
  if(value.t==V_REAL){
    uint64_t bits=0;
    memcpy(&bits,&value.d,sizeof bits);
    snprintf(output,512,
      "\"value_type\":\"real\",\"value_bits\":\"%016llx\"",
      (unsigned long long)bits);
  } else if(value.t==V_STR){
    char string_json[384];
    int truncated=0;
    trace_json_string(string_json,sizeof string_json,value.s,&truncated);
    snprintf(output,512,
      "\"value_type\":\"string\",\"value\":\"%s\",\"truncated\":%s",
      string_json,truncated?"true":"false");
  } else if(value.t==V_ARR){
    GmlArr *array=(GmlArr*)value.arr;
    int length=array?array->len:0;
    snprintf(output,512,
      "\"value_type\":\"array\",\"length\":%d",length);
  } else {
    snprintf(output,512,"\"value_type\":\"undefined\"");
  }
}

static void trace_variable_emit(GmlVM *vm,int global,
                                const GmlInstance *instance,
                                const char *name,int array_index,
                                GmlVal value){
  GmlVmFineTrace *trace=trace_get(vm);
  const char *code_name=trace_code_name(vm,vm?vm->cur_code_index:-1);
  if(!trace_frame_matches(vm,trace) ||
     !(trace->enabled&GML_VM_TRACE_VARIABLE) ||
     !trace_variable_matches(vm,trace,global,instance,name) ||
     !trace_code_matches(trace,code_name) ||
     !trace_reserve(vm,trace)) return;
  char name_json[384],object_json[384],code_json[384],value_json[512];
  trace_json_string(name_json,sizeof name_json,name,NULL);
  trace_json_string(object_json,sizeof object_json,
                    global?"":trace_object_name(vm,instance),NULL);
  trace_json_string(code_json,sizeof code_json,code_name,NULL);
  trace_variable_value(value_json,value);
  if(array_index>=0){
    anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_DEBUG,
      "{\"schema\":\"anygm.vm.trace\",\"version\":1,\"sequence\":%lu,"
      "\"kind\":\"variable\",\"frame\":%ld,\"scope\":\"%s\","
      "\"instance\":%u,\"object\":\"%s\",\"name\":\"%s\","
      "\"index\":%d,\"code\":\"%s\",%s}\n",
      trace_sequence(trace),vm->frame,global?"global":"instance",
      instance?instance->id:0u,object_json,name_json,array_index,
      code_json,value_json);
  } else {
    anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_DEBUG,
      "{\"schema\":\"anygm.vm.trace\",\"version\":1,\"sequence\":%lu,"
      "\"kind\":\"variable\",\"frame\":%ld,\"scope\":\"%s\","
      "\"instance\":%u,\"object\":\"%s\",\"name\":\"%s\","
      "\"code\":\"%s\",%s}\n",
      trace_sequence(trace),vm->frame,global?"global":"instance",
      instance?instance->id:0u,object_json,name_json,code_json,value_json);
  }
}

void gml_vm_diagnostics_variable_scope(GmlVM *vm,int scope,
                                       const char *name,int array_index,
                                       GmlVal value){
  trace_variable_emit(vm,scope==IT_GLOBAL,
                      scope==IT_GLOBAL?NULL:trace_scope_instance(vm,scope),
                      name,array_index,value);
}

void gml_vm_diagnostics_variable_instance(GmlVM *vm,
                                          const GmlInstance *instance,
                                          const char *name,int array_index,
                                          GmlVal value){
  trace_variable_emit(vm,0,instance,name,array_index,value);
}

void gml_vm_diagnostics_destroy(GmlVM *vm){
  if(!vm) return;
  free(vm->fine_trace);
  vm->fine_trace=NULL;
}
