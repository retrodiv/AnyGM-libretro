/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm_test_runner.h"
#include "gml_vm_diagnostics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *types;
  const char *frames;
  const char *limit;
  const char *code;
  const char *object;
  const char *instance;
  const char *variable;
  char logs[16][2048];
  int log_count;
} DiagnosticFixture;

static const char *fixture_setting(void *userdata,const char *name){
  DiagnosticFixture *fixture=(DiagnosticFixture*)userdata;
  if(!strcmp(name,"GML_DIAGNOSTICS")) return fixture->types;
  if(!strcmp(name,"GML_DIAGNOSTICS_FRAMES")) return fixture->frames;
  if(!strcmp(name,"GML_DIAGNOSTICS_LIMIT")) return fixture->limit;
  if(!strcmp(name,"GML_DIAGNOSTICS_CODE")) return fixture->code;
  if(!strcmp(name,"GML_DIAGNOSTICS_OBJECT")) return fixture->object;
  if(!strcmp(name,"GML_DIAGNOSTICS_INSTANCE")) return fixture->instance;
  if(!strcmp(name,"GML_DIAGNOSTICS_VARIABLE")) return fixture->variable;
  return NULL;
}

static void fixture_log(void *userdata,AnygmLogLevel level,
                        const char *message){
  DiagnosticFixture *fixture=(DiagnosticFixture*)userdata;
  if(level!=ANYGM_LOG_DEBUG || fixture->log_count>=16) return;
  snprintf(fixture->logs[fixture->log_count],
           sizeof fixture->logs[fixture->log_count],"%s",message?message:"");
  fixture->log_count++;
}

static void fixture_vm(GmlVM *vm,AnygmHostServices *host,
                       DiagnosticFixture *fixture){
  memset(vm,0,sizeof *vm);
  memset(host,0,sizeof *host);
  host->struct_size=sizeof *host;
  host->userdata=fixture;
  host->log=fixture_log;
  host->development_setting=fixture_setting;
  vm->host=host;
}

static int expect_opcode_filter_and_limit(void){
  DiagnosticFixture fixture={
    .types="opcode",
    .frames="10:10",
    .limit="3",
    .code="neutral_code"
  };
  GmlVM vm;
  AnygmHostServices host;
  fixture_vm(&vm,&host,&fixture);
  vm.frame=9;
  gml_vm_diagnostics_opcode(&vm,"neutral_code",0,"push",0);
  vm.frame=10;
  gml_vm_diagnostics_opcode(&vm,"other_code",0,"push",0);
  gml_vm_diagnostics_opcode(&vm,"neutral_code",0,"push",0);
  gml_vm_diagnostics_opcode(&vm,"neutral_code",4,"pop",1);
  gml_vm_diagnostics_opcode(&vm,"neutral_code",8,"ret",0);
  gml_vm_diagnostics_opcode(&vm,"neutral_code",12,"exit",0);
  int ok=fixture.log_count==3 &&
    strstr(fixture.logs[0],
      "\"sequence\":0,\"kind\":\"opcode\",\"frame\":10,"
      "\"code\":\"neutral_code\",\"offset\":0,\"opcode\":\"push\"") &&
    strstr(fixture.logs[1],
      "\"sequence\":1,\"kind\":\"opcode\",\"frame\":10,"
      "\"code\":\"neutral_code\",\"offset\":4,\"opcode\":\"pop\"") &&
    strstr(fixture.logs[2],
      "\"sequence\":2,\"kind\":\"limit\",\"frame\":10,\"limit\":3");
  gml_vm_diagnostics_destroy(&vm);
  return ok;
}

static int expect_event_and_collision_filters(void){
  DiagnosticFixture fixture={
    .types="event,collision",
    .frames="4:4",
    .limit="8",
    .object="neutral\"target"
  };
  GmlVM vm;
  AnygmHostServices host;
  fixture_vm(&vm,&host,&fixture);
  GmlCode code[1]={{.name="neutral_event_code"}};
  GmlWin win={.code=code,.n_code=1};
  GmlObject objects[2]={{.name="neutral\"target"},{.name="neutral_other"}};
  GmlInstance instances[2]={
    {.active=1,.id=100001,.obj=0},
    {.active=1,.id=100002,.obj=1}
  };
  vm.win=&win;
  vm.objects=objects;
  vm.n_objects=2;
  vm.inst=instances;
  vm.inst_count=2;
  vm.frame=4;
  gml_vm_diagnostics_event(&vm,&instances[1],"Step_0",0);
  gml_vm_diagnostics_event(&vm,&instances[0],"Step_0",0);
  gml_vm_diagnostics_collision(&vm,&instances[0],&instances[1],0,1);
  int ok=fixture.log_count==2 &&
    strstr(fixture.logs[0],"\"kind\":\"event\"") &&
    strstr(fixture.logs[0],"\"object\":\"neutral\\\"target\"") &&
    strstr(fixture.logs[0],"\"event\":\"Step_0\"") &&
    strstr(fixture.logs[1],"\"kind\":\"collision\"") &&
    strstr(fixture.logs[1],"\"hit\":true") &&
    strstr(fixture.logs[1],"\"code\":\"neutral_event_code\"");
  gml_vm_diagnostics_destroy(&vm);
  return ok;
}

static int expect_variable_watchpoint(void){
  DiagnosticFixture fixture={
    .types="variable",
    .frames="8:8",
    .limit="5",
    .code="neutral_code",
    .instance="global",
    .variable="neutral_value"
  };
  GmlVM vm;
  AnygmHostServices host;
  fixture_vm(&vm,&host,&fixture);
  GmlCode code[1]={{.name="neutral_code"}};
  GmlWin win={.code=code,.n_code=1};
  vm.win=&win;
  vm.cur_code_index=0;
  vm.frame=8;
  gml_vm_diagnostics_variable_scope(
      &vm,IT_GLOBAL,"other_value",-1,vreal(2.0));
  gml_vm_diagnostics_variable_scope(
      &vm,IT_SELF,"neutral_value",-1,vreal(2.0));
  gml_vm_diagnostics_variable_scope(
      &vm,IT_GLOBAL,"neutral_value",-1,vreal(1.5));
  gml_vm_diagnostics_variable_scope(
      &vm,IT_GLOBAL,"neutral_value",3,vstr("line\nvalue"));
  int ok=fixture.log_count==2 &&
    strstr(fixture.logs[0],"\"kind\":\"variable\"") &&
    strstr(fixture.logs[0],"\"scope\":\"global\"") &&
    strstr(fixture.logs[0],"\"name\":\"neutral_value\"") &&
    strstr(fixture.logs[0],
           "\"value_type\":\"real\",\"value_bits\":\"3ff8000000000000\"") &&
    strstr(fixture.logs[1],"\"index\":3") &&
    strstr(fixture.logs[1],"\"value\":\"line\\nvalue\"");
  gml_vm_diagnostics_destroy(&vm);
  return ok;
}

static int expect_malformed_configuration_is_inert(void){
  DiagnosticFixture missing_limit={
    .types="opcode",
    .frames="0:1",
    .limit=NULL,
    .code="neutral_code"
  };
  GmlVM vm;
  AnygmHostServices host;
  fixture_vm(&vm,&host,&missing_limit);
  gml_vm_diagnostics_opcode(&vm,"neutral_code",0,"push",0);
  gml_vm_diagnostics_opcode(&vm,"neutral_code",4,"ret",0);
  int ok=missing_limit.log_count==1 &&
    strstr(missing_limit.logs[0],"\"kind\":\"config_error\"") &&
    strstr(missing_limit.logs[0],
           "\"setting\":\"GML_DIAGNOSTICS_LIMIT\",\"reason\":\"required\"");
  gml_vm_diagnostics_destroy(&vm);

  DiagnosticFixture invalid_instance={
    .types="event",
    .frames="0:1",
    .limit="2",
    .instance="+100001"
  };
  fixture_vm(&vm,&host,&invalid_instance);
  gml_vm_diagnostics_event(&vm,NULL,"Step_0",-1);
  ok=ok && invalid_instance.log_count==1 &&
    strstr(invalid_instance.logs[0],"\"kind\":\"config_error\"") &&
    strstr(invalid_instance.logs[0],
           "\"setting\":\"GML_DIAGNOSTICS_INSTANCE\"");
  gml_vm_diagnostics_destroy(&vm);
  return ok;
}

int main(void){
  static const AnygmTestCase cases[]={
    {"opcode_filter_and_limit",expect_opcode_filter_and_limit},
    {"event_and_collision_filters",expect_event_and_collision_filters},
    {"variable_watchpoint",expect_variable_watchpoint},
    {"malformed_configuration_is_inert",
     expect_malformed_configuration_is_inert}
  };
  const AnygmTestGroup group={
    "vm_diagnostics",cases,sizeof cases/sizeof cases[0]
  };
  AnygmTestResult result;
  anygm_test_run_groups(&group,1,NULL,&result);
  printf("VM diagnostic fixtures: passed=%d failed=%d\n",
         result.passed,result.failed);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
