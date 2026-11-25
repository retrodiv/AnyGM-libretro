/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "gml_vm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint32_t word;
  uint32_t operand;
  const char *name;
  int branch_plus_one;
  int has_operand;
} Instruction;

enum { MAX_INSTRUCTIONS=360, MAX_CODE=2 };

typedef struct {
  GmlWin win;
  GmlVM vm;
  GmlCode code[MAX_CODE];
  uint8_t bytes[MAX_CODE*MAX_INSTRUCTIONS*8];
  uint32_t ref_address[MAX_CODE*MAX_INSTRUCTIONS];
  const char *ref_name[MAX_CODE*MAX_INSTRUCTIONS];
  uint32_t next_byte;
  GmlInstance *receiver;
} Fixture;

#define WORD(op,type,low) (((uint32_t)(op)<<24)|((uint32_t)(type)<<16)|(uint16_t)(low))
#define SIMPLE(op) { .word=WORD(op,DT_VAR,0) }
#define VAR(op,scope,ref) { .word=WORD(op,DT_VAR|(DT_VAR<<4),scope), \
                           .operand=UINT32_C(0xa0000000), .name=ref, .has_operand=1 }
#define PUSH(scope,ref) { .word=WORD(OP_PUSH,DT_VAR,scope), \
                          .operand=UINT32_C(0xa0000000), .name=ref, .has_operand=1 }
#define ARG(index) PUSH(IT_ARG,"argument" #index)
#define SET_ARG(index) VAR(OP_POP,IT_ARG,"argument" #index)
#define CALL(ref,count) { .word=WORD(OP_CALL,DT_VAR,count), .name=ref, .has_operand=1 }
#define INTEGER(value) { .word=WORD(OP_PUSH,DT_INT16,value) }
#define INTEGER32(value) { .word=WORD(OP_PUSH,DT_INT32,0), .operand=value, .has_operand=1 }
#define STR(index) { .word=WORD(OP_PUSH,DT_STRING,0), .operand=index, .has_operand=1 }
#define CONV(from,to) { .word=WORD(OP_CONV,(from)|((to)<<4),0) }
#define ARRAY_ELEMENT(op,ref) { .word=WORD(op,DT_VAR|(DT_VAR<<4),IT_SELF), \
                                     .name=ref, .has_operand=1 }
#define PUSH_ELEMENT(ref) { .word=WORD(OP_PUSH,DT_VAR,IT_SELF), .name=ref, .has_operand=1 }
#define PUSH_MEMBER(ref) { .word=WORD(OP_PUSH,DT_VAR,IT_STACK), \
                                  .operand=UINT32_C(0x80000000), .name=ref, .has_operand=1 }
#define CALL_MEMBER(ref) { .word=WORD(OP_DUP,DT_VAR,0x0801) },SIMPLE(OP_DUP), \
                         PUSH(IT_STACK,ref),{ .word=WORD(OP_CALLV,DT_VAR,1) }
#define ADD_ONE_TO_LOCAL(ref) PUSH(IT_LOCAL,ref),INTEGER(1),SIMPLE(OP_ADD), \
                              VAR(OP_POP,IT_LOCAL,ref)
#define CMP(comparison) { .word=WORD(OP_CMP,DT_VAR|(DT_VAR<<4),(comparison)<<8) }
#define JUMP(op,target) { .word=WORD(op,0,0), .branch_plus_one=(target)+1 }
#define COUNT(array) (sizeof(array)/sizeof((array)[0]))

static int expect(int condition,const char *description){
  if(!condition) fprintf(stderr,"%s\n",description);
  return condition;
}

static void write_word(uint8_t *bytes,uint32_t value){
  for(unsigned i=0;i<4;i++) bytes[i]=(uint8_t)(value>>(i*8));
}

/* Build a real bytecode stream and occurrence table so the decoder and recognizer run. */
static void add_code(Fixture *fixture,const char *name,const Instruction *instructions,
                     size_t count){
  uint32_t pc[MAX_INSTRUCTIONS+1];
  pc[0]=fixture->next_byte;
  for(size_t i=0;i<count;i++) pc[i+1]=pc[i]+(instructions[i].has_operand?8u:4u);
  for(size_t i=0;i<count;i++){
    const Instruction *instruction=&instructions[i];
    uint32_t word=instruction->word;
    if(instruction->branch_plus_one){
      const uint32_t destination=pc[instruction->branch_plus_one-1];
      int32_t offset=((int32_t)destination-(int32_t)pc[i])/4;
      word|=(uint32_t)offset&UINT32_C(0x7fffff);
    }
    write_word(fixture->bytes+pc[i],word);
    if(instruction->has_operand)
      write_word(fixture->bytes+pc[i]+4,instruction->operand);
    if(instruction->name){
      int reference=fixture->win.n_refs++;
      fixture->ref_address[reference]=pc[i]+4;
      fixture->ref_name[reference]=instruction->name;
    }
  }
  GmlCode *code=&fixture->code[fixture->win.n_code++];
  code->name=name;
  code->start=pc[0];
  code->length=pc[count]-pc[0];
  fixture->win.size=fixture->next_byte=pc[count];
}

static int fixture_init(Fixture *fixture){
  fixture->win.data=fixture->bytes;
  fixture->win.bytecode=17;
  fixture->win.code=fixture->code;
  fixture->win.ref_addr=fixture->ref_address;
  fixture->win.ref_name=fixture->ref_name;
  fixture->vm.win=&fixture->win;
  fixture->vm.cur_code_index=-1;
  fixture->vm.math_epsilon=1e-9;
  fixture->vm.diagnostics.code_coverage=1;
  fixture->receiver=gml_struct_new(&fixture->vm);
  return expect(fixture->receiver!=NULL,"allocate synthetic receiver");
}

static void fixture_free(Fixture *fixture){
  gml_vm_free(&fixture->vm);
  for(int i=0;i<fixture->win.n_code;i++) gml_vm_code_cache_discard(&fixture->code[i]);
  free(fixture->win.code_hix);
  free(fixture->win.ref_hix);
}

static int select_dispatch(Fixture *fixture,int interpreted){
  GmlCode *code=&fixture->code[0];
  if(!expect(gml_vm_code_cache_ensure(&fixture->win,0) && code->micro_kind!=0,
             "bytecode fixture is recognized")) return 0;
  /* Keep the decoded bytecode identical and turn off only its derived shortcut. */
  if(interpreted) code->micro_kind=0;
  return 1;
}

static GmlVal invoke(Fixture *fixture,GmlVal *args,int count){
  return gml_vm_call_member_callable(&fixture->vm,vreal(fixture->receiver->id),
                                     vreal(GML_FUNCVAL_TAG),args,count);
}

static int interpreted_body_entered(const Fixture *fixture){
  const uint8_t *seen=fixture->vm.diagnostics.code_seen;
  return seen && seen[0];
}

static int same_value(GmlVal first,GmlVal second){
  if(first.t==V_UNDEF || second.t==V_UNDEF) return first.t==second.t;
  return first.t==V_REAL && second.t==V_REAL && first.d==second.d &&
    gml_val_number_kind(first)==gml_val_number_kind(second);
}

static int map_lookup_case(void){
  static const Instruction lookup[]={
    ARG(0),PUSH(IT_GLOBAL,"records"),CALL("ds_map_find_value",2),SIMPLE(OP_RET)
  };
  GmlVal found[2]={0},missing[2]={0};
  int ok=1;
  for(int interpreted=0;interpreted<2;interpreted++){
    Fixture fixture={0};
    if(!fixture_init(&fixture)){ fixture_free(&fixture); return 0; }
    add_code(&fixture,"gml_Script_lookup_record",lookup,COUNT(lookup));
    if(!select_dispatch(&fixture,interpreted)){ fixture_free(&fixture); return 0; }
    GmlVal map=gml_builtin_call(&fixture.vm,"ds_map_create",NULL,0);
    *gml_varmap_put(&fixture.vm.globals,"records")=map;
    GmlVal entry[]={map,vstr("row"),vreal(37.25)};
    gml_builtin_call(&fixture.vm,"ds_map_add",entry,COUNT(entry));
    found[interpreted]=invoke(&fixture,&entry[1],1);
    GmlVal absent=vstr("missing");
    missing[interpreted]=invoke(&fixture,&absent,1);
    ok &= expect(interpreted_body_entered(&fixture)==interpreted,
                 "map lookup uses the selected execution path");
    fixture_free(&fixture);
  }
  ok &= expect(same_value(found[0],found[1]) && found[0].t==V_REAL && found[0].d==37.25,
               "map lookup returns the same existing record");
  ok &= expect(same_value(missing[0],missing[1]) && missing[0].t==V_UNDEF,
               "map lookup returns undefined for an absent record");
  return ok;
}

static int approach_case(void){
  enum { RETURN_TARGET=12, FALLING=15, IN_RANGE=14, RETURN_CURRENT=25 };
  static const Instruction approach[]={
    ARG(0),ARG(1),CMP(CMP_LT),JUMP(OP_BF,FALLING),
    ARG(0),ARG(2),SIMPLE(OP_ADD),SET_ARG(0),
    ARG(0),ARG(1),CMP(CMP_GT),JUMP(OP_BF,IN_RANGE),
    ARG(1),SIMPLE(OP_RET),JUMP(OP_B,RETURN_CURRENT),
    ARG(0),ARG(2),SIMPLE(OP_SUB),SET_ARG(0),
    ARG(0),ARG(1),CMP(CMP_LT),JUMP(OP_BF,RETURN_CURRENT),
    ARG(1),SIMPLE(OP_RET),ARG(0),SIMPLE(OP_RET),SIMPLE(OP_EXIT)
  };
  static const double samples[][4]={
    {2.25,8.0,0.5,2.75}, {2.25,3.0,1.5,3.0},
    {8.75,2.0,0.5,8.25}, {8.75,8.0,1.5,8.0}, {4.0,4.0,1.0,4.0}
  };
  GmlVal outputs[2][COUNT(samples)]={0};
  int ok=1;
  for(int interpreted=0;interpreted<2;interpreted++){
    Fixture fixture={0};
    if(!fixture_init(&fixture)){ fixture_free(&fixture); return 0; }
    add_code(&fixture,"gml_Script_approach_limit",approach,COUNT(approach));
    if(!select_dispatch(&fixture,interpreted)){ fixture_free(&fixture); return 0; }
    for(size_t i=0;i<COUNT(samples);i++){
      GmlVal args[]={vreal(samples[i][0]),vreal(samples[i][1]),vreal(samples[i][2])};
      outputs[interpreted][i]=invoke(&fixture,args,COUNT(args));
    }
    ok &= expect(interpreted_body_entered(&fixture)==interpreted,
                 "approach uses the selected execution path");
    fixture_free(&fixture);
  }
  for(size_t i=0;i<COUNT(samples);i++)
    ok &= expect(same_value(outputs[0][i],outputs[1][i]) &&
                 outputs[0][i].t==V_REAL && outputs[0][i].d==samples[i][3],
                 "approach agrees for steps and saturation in either direction");

  Instruction altered[COUNT(approach)];
  memcpy(altered,approach,sizeof altered);
  altered[IN_RANGE].branch_plus_one=RETURN_TARGET+1;
  Fixture fixture={0};
  if(!fixture_init(&fixture)){ fixture_free(&fixture); return 0; }
  add_code(&fixture,"gml_Script_adjusted_limit",altered,COUNT(altered));
  ok &= expect(gml_vm_code_cache_ensure(&fixture.win,0) && fixture.code[0].micro_kind==0,
               "approach rejects an altered branch destination");
  GmlVal args[]={vreal(2.25),vreal(8.0),vreal(0.5)};
  ok &= expect(same_value(invoke(&fixture,args,COUNT(args)),vreal(8.0)) &&
               interpreted_body_entered(&fixture),"altered approach keeps its different return");
  fixture_free(&fixture);
  return ok;
}

static int global_script_case(void){
  static const Instruction forward[]={
    ARG(0),PUSH(IT_GLOBAL,"base_amount"),CALL("gml_Script_record_sum",2),SIMPLE(OP_RET)
  };
  static const Instruction record_sum[]={
    ARG(0),ARG(1),SIMPLE(OP_ADD),VAR(OP_POP,IT_SELF,"sum_recorded"),
    PUSH(IT_SELF,"sum_recorded"),SIMPLE(OP_RET)
  };
  GmlVal outputs[2]={0};
  int ok=1;
  for(int interpreted=0;interpreted<2;interpreted++){
    Fixture fixture={0};
    if(!fixture_init(&fixture)){ fixture_free(&fixture); return 0; }
    add_code(&fixture,"gml_Script_forward_sum",forward,COUNT(forward));
    add_code(&fixture,"gml_Script_record_sum",record_sum,COUNT(record_sum));
    if(!select_dispatch(&fixture,interpreted)){ fixture_free(&fixture); return 0; }
    *gml_varmap_put(&fixture.vm.globals,"base_amount")=vreal(10.25);
    GmlVal increment=vreal(2.5);
    outputs[interpreted]=invoke(&fixture,&increment,1);
    GmlVal *record=gml_varmap_get(&fixture.receiver->vars,"sum_recorded");
    ok &= expect(record && same_value(*record,outputs[interpreted]),
                 "forwarded call updates its receiver");
    ok &= expect(interpreted_body_entered(&fixture)==interpreted &&
                 fixture.vm.diagnostics.code_seen && fixture.vm.diagnostics.code_seen[1],
                 "forwarding uses the selected execution path and enters the callee");
    fixture_free(&fixture);
  }
  ok &= expect(same_value(outputs[0],outputs[1]) && outputs[0].t==V_REAL && outputs[0].d==12.75,
               "forwarding preserves the callee result");
  return ok;
}

static GmlVal create_map(GmlVM *vm){
  return gml_builtin_call(vm,"ds_map_create",NULL,0);
}

static void map_add(GmlVM *vm,GmlVal map,const char *key,GmlVal value){
  GmlVal args[]={map,vstr(key),value};
  gml_builtin_call(vm,"ds_map_add",args,COUNT(args));
}

static int nested_lookup_case(void){
  enum { TEST_RESULT=17, RETURN_RESULT=24 };
  static const Instruction lookup[]={
    ARG(1),ARG(0),PUSH(IT_GLOBAL,"categories"),CALL("ds_map_find_value",2),
    CALL("ds_map_find_value",2),VAR(OP_POP,IT_LOCAL,"entry"),
    PUSH(IT_LOCAL,"entry"),CALL("is_undefined",1),CONV(DT_VAR,DT_BOOL),JUMP(OP_BF,TEST_RESULT),
    ARG(1),STR(0),CONV(DT_STRING,DT_VAR),PUSH(IT_GLOBAL,"categories"),
    CALL("ds_map_find_value",2),CALL("ds_map_find_value",2),VAR(OP_POP,IT_LOCAL,"entry"),
    PUSH(IT_LOCAL,"entry"),CALL("is_undefined",1),CONV(DT_VAR,DT_BOOL),JUMP(OP_BF,RETURN_RESULT),
    INTEGER(-77),CONV(DT_INT16,DT_DOUBLE),SIMPLE(OP_RET),PUSH(IT_LOCAL,"entry"),SIMPLE(OP_RET)
  };
  char default_name[]="default";
  char *strings[]={default_name};
  GmlVal outputs[2][3]={0};
  int ok=1;
  for(int interpreted=0;interpreted<2;interpreted++){
    Fixture fixture={0};
    if(!fixture_init(&fixture)){ fixture_free(&fixture); return 0; }
    fixture.win.strs=strings;
    fixture.win.n_strs=COUNT(strings);
    add_code(&fixture,"gml_Script_lookup_category",lookup,COUNT(lookup));
    if(!select_dispatch(&fixture,interpreted)){ fixture_free(&fixture); return 0; }
    GmlVM *vm=&fixture.vm;
    GmlVal root=create_map(vm), primary=create_map(vm), fallback=create_map(vm);
    *gml_varmap_put(&vm->globals,"categories")=root;
    map_add(vm,root,"regional",primary);
    map_add(vm,root,default_name,fallback);
    map_add(vm,primary,"quota",vreal(18.25));
    map_add(vm,fallback,"quota",vreal(5.75));
    GmlVal args[]={vstr("regional"),vstr("quota")};
    outputs[interpreted][0]=invoke(&fixture,args,COUNT(args));
    args[0]=vstr("absent");
    outputs[interpreted][1]=invoke(&fixture,args,COUNT(args));
    ok &= expect(interpreted_body_entered(&fixture)==interpreted,
                 "nested lookup handles primary and default matches on the selected path");
    args[1]=vstr("unassigned");
    outputs[interpreted][2]=invoke(&fixture,args,COUNT(args));
    ok &= expect(interpreted_body_entered(&fixture),"nested lookup defers a complete miss");
    fixture_free(&fixture);
  }
  static const double expected[]={18.25,5.75,-77.0};
  for(size_t i=0;i<COUNT(expected);i++)
    ok &= expect(same_value(outputs[0][i],outputs[1][i]) &&
                 outputs[0][i].t==V_REAL && outputs[0][i].d==expected[i],
                 "nested lookup returns the selected category or explicit miss value");
  return ok;
}

static const Instruction count_visit[]={
  PUSH(IT_SELF,"visits"),INTEGER(1),SIMPLE(OP_ADD),VAR(OP_POP,IT_SELF,"visits"),
  ARG(0),VAR(OP_POP,IT_SELF,"last_argument"),SIMPLE(OP_EXIT)
};

static GmlInstance *new_member(GmlVM *vm){
  GmlInstance *member=gml_struct_new(vm);
  if(member){
    *gml_varmap_put(&member->vars,"refresh")=vreal(GML_FUNCVAL_TAG|1);
    *gml_varmap_put(&member->vars,"visits")=vreal(0);
  }
  return member;
}

static int field_is(GmlInstance *instance,const char *name,double expected){
  GmlVal *value=gml_varmap_get(&instance->vars,name);
  return value && value->t==V_REAL && value->d==expected;
}

static int map_method_loop_case(void){
  enum { APPLY_SETTINGS=6, TEST_INDEX=18, DONE=40 };
  static const Instruction apply[]={
    ARG(0),PUSH(IT_BUILTIN,"undefined"),CMP(CMP_EQ),JUMP(OP_BF,APPLY_SETTINGS),INTEGER(-1),SET_ARG(0),
    PUSH(IT_GLOBAL,"deadzone"),ARG(0),CALL("gamepad_set_axis_deadzone",2),SIMPLE(OP_POPZ),
    PUSH(IT_GLOBAL,"members"),CALL("ds_map_find_first",1),VAR(OP_POP,IT_LOCAL,"key"),
    PUSH(IT_GLOBAL,"members"),CALL("ds_map_size",1),VAR(OP_POP,IT_LOCAL,"length"),
    INTEGER(0),VAR(OP_POP,IT_LOCAL,"index"),PUSH(IT_LOCAL,"index"),PUSH(IT_LOCAL,"length"),
    CMP(CMP_LT),JUMP(OP_BF,DONE),
    PUSH(IT_LOCAL,"key"),PUSH(IT_GLOBAL,"members"),CALL("ds_map_find_value",2),ARG(0),
    CALL_MEMBER("refresh"),SIMPLE(OP_POPZ),
    PUSH(IT_LOCAL,"key"),PUSH(IT_GLOBAL,"members"),CALL("ds_map_find_next",2),VAR(OP_POP,IT_LOCAL,"key"),
    ADD_ONE_TO_LOCAL("index"),JUMP(OP_B,TEST_INDEX),SIMPLE(OP_EXIT)
  };
  int ok=1;
  for(int interpreted=0;interpreted<2;interpreted++){
    Fixture fixture={0};
    if(!fixture_init(&fixture)){ fixture_free(&fixture); return 0; }
    add_code(&fixture,"gml_Script_refresh_member_map",apply,COUNT(apply));
    add_code(&fixture,"gml_Script_count_visit",count_visit,COUNT(count_visit));
    if(!select_dispatch(&fixture,interpreted)){ fixture_free(&fixture); return 0; }
    GmlVM *vm=&fixture.vm;
    GmlInstance *first=new_member(vm), *second=new_member(vm);
    if(!expect(first && second,"allocate two map members")){ fixture_free(&fixture); return 0; }
    GmlVal map=create_map(vm);
    map_add(vm,map,"first",vreal(first->id));
    map_add(vm,map,"second",vreal(second->id));
    *gml_varmap_put(&vm->globals,"members")=map;
    *gml_varmap_put(&vm->globals,"deadzone")=vreal(0.625);
    GmlVal device=vreal(1);
    ok &= expect(same_value(invoke(&fixture,&device,1),vreal(0)) &&
                 field_is(first,"visits",1) && field_is(second,"visits",1) &&
                 field_is(first,"last_argument",1) && field_is(second,"last_argument",1),
                 "map loop calls each member once with the selected device");
    ok &= expect(same_value(gml_builtin_call(vm,"gamepad_get_axis_deadzone",&device,1),vreal(0.625)),
                 "map loop updates the selected device's dead zone");
    device=vundef();
    ok &= expect(same_value(invoke(&fixture,&device,1),vreal(0)) &&
                 field_is(first,"visits",2) && field_is(second,"visits",2) &&
                 field_is(first,"last_argument",-1) && field_is(second,"last_argument",-1),
                 "map loop forwards its default device to both members");
    ok &= expect(interpreted_body_entered(&fixture)==interpreted,
                 "map method loop uses the selected execution path");
    fixture_free(&fixture);
  }
  return ok;
}

static int array_method_loop_case(void){
  enum { RESET=6, TEST_INDEX=16, TEST_SECOND=40, TEST_THIRD=47, WRITE_MEMBER=54, DONE=66 };
  static const Instruction apply[]={
    ARG(0),PUSH(IT_BUILTIN,"undefined"),CMP(CMP_EQ),JUMP(OP_BF,RESET),INTEGER(-1),SET_ARG(0),
    INTEGER(0),VAR(OP_POP,IT_SELF,"flag_a"),INTEGER(0),VAR(OP_POP,IT_SELF,"flag_b"),
    INTEGER(0),VAR(OP_POP,IT_SELF,"flag_c"),INTEGER(0),VAR(OP_POP,IT_SELF,"summary"),
    INTEGER(0),VAR(OP_POP,IT_LOCAL,"index"),PUSH(IT_LOCAL,"index"),PUSH(IT_SELF,"members"),
    CALL("array_length",1),CMP(CMP_LT),JUMP(OP_BF,DONE),
    INTEGER(-1),PUSH(IT_LOCAL,"index"),CONV(DT_VAR,DT_INT32),PUSH_ELEMENT("members"),
    VAR(OP_POP,IT_LOCAL,"member"),PUSH(IT_LOCAL,"member"),ARG(0),CALL_MEMBER("refresh"),SIMPLE(OP_POPZ),
    PUSH(IT_LOCAL,"member"),INTEGER(-9),PUSH_MEMBER("flag_a"),CONV(DT_VAR,DT_BOOL),JUMP(OP_BF,TEST_SECOND),
    INTEGER(1),VAR(OP_POP,IT_SELF,"flag_a"),
    PUSH(IT_LOCAL,"member"),INTEGER(-9),PUSH_MEMBER("flag_b"),CONV(DT_VAR,DT_BOOL),JUMP(OP_BF,TEST_THIRD),
    INTEGER(1),VAR(OP_POP,IT_SELF,"flag_b"),
    PUSH(IT_LOCAL,"member"),INTEGER(-9),PUSH_MEMBER("flag_c"),CONV(DT_VAR,DT_BOOL),JUMP(OP_BF,WRITE_MEMBER),
    INTEGER(1),VAR(OP_POP,IT_SELF,"flag_c"),INTEGER32(1),{ .word=WORD(OP_BREAK,DT_INT32,-5) },
    PUSH(IT_LOCAL,"member"),INTEGER(-1),PUSH(IT_LOCAL,"index"),CONV(DT_VAR,DT_INT32),
    ARRAY_ELEMENT(OP_POP,"members"),ADD_ONE_TO_LOCAL("index"),JUMP(OP_B,TEST_INDEX),SIMPLE(OP_EXIT)
  };
  int ok=1;
  for(int interpreted=0;interpreted<2;interpreted++){
    Fixture fixture={0};
    if(!fixture_init(&fixture)){ fixture_free(&fixture); return 0; }
    add_code(&fixture,"gml_Script_refresh_member_array",apply,COUNT(apply));
    add_code(&fixture,"gml_Script_count_visit",count_visit,COUNT(count_visit));
    if(!select_dispatch(&fixture,interpreted)){ fixture_free(&fixture); return 0; }
    GmlVM *vm=&fixture.vm;
    GmlInstance *first=new_member(vm), *second=new_member(vm);
    if(!expect(first && second,"allocate two array members")){ fixture_free(&fixture); return 0; }
    *gml_varmap_put(&first->vars,"flag_a")=vreal(1);
    *gml_varmap_put(&second->vars,"flag_c")=vreal(1);
    GmlVarMap *fields=&fixture.receiver->vars;
    *gml_varmap_put(fields,"flag_b")=vreal(1);
    *gml_varmap_put(fields,"summary")=vreal(42);
    GmlVal members=gml_arr_new(2,vreal(first->id));
    gml_arr_set(members,1,vreal(second->id));
    *gml_varmap_put(fields,"members")=members;
    GmlVal argument=vreal(7);
    ok &= expect(same_value(invoke(&fixture,&argument,1),vreal(0)) &&
                 field_is(first,"visits",1) && field_is(second,"visits",1) &&
                 field_is(first,"last_argument",7) && field_is(second,"last_argument",7),
                 "array loop calls each member in the array");
    ok &= expect(field_is(fixture.receiver,"flag_a",1) && field_is(fixture.receiver,"flag_b",0) &&
                 field_is(fixture.receiver,"flag_c",1) && field_is(fixture.receiver,"summary",0),
                 "array loop resets and aggregates flags");
    GmlVal *actual=gml_varmap_get(fields,"members");
    ok &= expect(actual && gml_val_array_length(*actual)==2 &&
                 same_value(gml_arr_get(*actual,0),vreal(first->id)) &&
                 same_value(gml_arr_get(*actual,1),vreal(second->id)),
                 "array loop preserves the members and order it writes back");
    ok &= expect(interpreted_body_entered(&fixture)==interpreted,
                 "array method loop uses the selected execution path");
    fixture_free(&fixture);
  }
  return ok;
}

typedef struct { int phase, second_pad; } Input;

static int key_state(void *userdata,int key,int edge){
  const Input *input=userdata;
  if(key!=68) return 0;
  return input->phase==0 ? edge!=2 : edge==2;
}

static int pad_connected(void *userdata,int device){
  const Input *input=userdata;
  return device==0 || (device==1 && input->second_pad);
}

static int pad_button(void *userdata,int device,int button,int edge){
  const Input *input=userdata;
  if(button!=32769 || !pad_connected(userdata,device)) return 0;
  return device==input->second_pad && (input->phase==0 ? edge!=2 : edge==2);
}

static int input_fields(GmlInstance *receiver,int axis,int pressed,int held,int released){
  return field_is(receiver,"axis",axis) && field_is(receiver,"pressed",pressed) &&
    field_is(receiver,"held",held) && field_is(receiver,"released",released);
}

#define BINDING_ELEMENT(index) INTEGER(-1),INTEGER(index),PUSH_ELEMENT("binding")
#define CALL_KEY_BINDING(name) PUSH(IT_SELF,"binding"),CALL(name,1)
#define CALL_PAD_BINDING(name) PUSH(IT_SELF,"binding"),ARG(0),CALL(name,2)
#define TEST_AXIS(value) PUSH(IT_SELF,"axis"),INTEGER(0),CMP(value)

static int input_action_case(void){
  enum {
    PICK_DEVICE=6, KEYBOARD=20, KEY_RELEASE_CURRENT=59, KEY_STORE_RELEASE=60,
    KEY_SCALAR=62, KEY_DONE=71, PAD=72, PAD_RELEASE_CURRENT=115, PAD_STORE_RELEASE=116,
    PAD_SCALAR=118, PAD_DONE=130, OTHER=131, DONE=345
  };
  static const Instruction update[]={
    ARG(0),PUSH(IT_BUILTIN,"undefined"),CMP(CMP_EQ),JUMP(OP_BF,PICK_DEVICE),INTEGER(-1),SET_ARG(0),
    PUSH(IT_SELF,"device_type"),SIMPLE(OP_DUP),INTEGER(0),CMP(CMP_EQ),JUMP(OP_BT,KEYBOARD),
    SIMPLE(OP_DUP),INTEGER(1),CMP(CMP_EQ),JUMP(OP_BT,PAD),
    SIMPLE(OP_DUP),INTEGER(2),CMP(CMP_EQ),JUMP(OP_BT,OTHER),JUMP(OP_B,DONE),

    CALL_KEY_BINDING("is_array"),CONV(DT_VAR,DT_BOOL),JUMP(OP_BF,KEY_SCALAR),
    PUSH(IT_SELF,"held"),VAR(OP_POP,IT_LOCAL,"was_held"),
    BINDING_ELEMENT(1),CALL("keyboard_check",1),BINDING_ELEMENT(0),CALL("keyboard_check",1),
    SIMPLE(OP_SUB),VAR(OP_POP,IT_SELF,"axis"),
    BINDING_ELEMENT(1),CALL("keyboard_check_pressed",1),BINDING_ELEMENT(0),CALL("keyboard_check_pressed",1),
    SIMPLE(OP_SUB),INTEGER(0),CMP(CMP_NEQ),VAR(OP_POP,IT_SELF,"pressed"),
    TEST_AXIS(CMP_NEQ),VAR(OP_POP,IT_SELF,"held"),TEST_AXIS(CMP_EQ),JUMP(OP_BF,KEY_RELEASE_CURRENT),
    PUSH(IT_LOCAL,"was_held"),CONV(DT_VAR,DT_BOOL),JUMP(OP_B,KEY_STORE_RELEASE),
    INTEGER(0),VAR(OP_POP,IT_SELF,"released"),JUMP(OP_B,KEY_DONE),
    CALL_KEY_BINDING("keyboard_check_pressed"),VAR(OP_POP,IT_SELF,"pressed"),
    CALL_KEY_BINDING("keyboard_check"),VAR(OP_POP,IT_SELF,"held"),
    CALL_KEY_BINDING("keyboard_check_released"),VAR(OP_POP,IT_SELF,"released"),JUMP(OP_B,DONE),

    CALL_KEY_BINDING("is_array"),CONV(DT_VAR,DT_BOOL),JUMP(OP_BF,PAD_SCALAR),
    PUSH(IT_SELF,"held"),VAR(OP_POP,IT_LOCAL,"was_held"),
    BINDING_ELEMENT(1),ARG(0),CALL("gamepad_button_check",2),BINDING_ELEMENT(0),ARG(0),CALL("gamepad_button_check",2),
    SIMPLE(OP_SUB),VAR(OP_POP,IT_SELF,"axis"),
    BINDING_ELEMENT(1),ARG(0),CALL("gamepad_button_check_pressed",2),
    BINDING_ELEMENT(0),ARG(0),CALL("gamepad_button_check_pressed",2),
    SIMPLE(OP_SUB),INTEGER(0),CMP(CMP_NEQ),VAR(OP_POP,IT_SELF,"pressed"),
    TEST_AXIS(CMP_NEQ),VAR(OP_POP,IT_SELF,"held"),TEST_AXIS(CMP_EQ),JUMP(OP_BF,PAD_RELEASE_CURRENT),
    PUSH(IT_LOCAL,"was_held"),CONV(DT_VAR,DT_BOOL),JUMP(OP_B,PAD_STORE_RELEASE),
    INTEGER(0),VAR(OP_POP,IT_SELF,"released"),JUMP(OP_B,PAD_DONE),
    CALL_PAD_BINDING("gamepad_button_check_pressed"),VAR(OP_POP,IT_SELF,"pressed"),
    CALL_PAD_BINDING("gamepad_button_check"),VAR(OP_POP,IT_SELF,"held"),
    CALL_PAD_BINDING("gamepad_button_check_released"),VAR(OP_POP,IT_SELF,"released"),JUMP(OP_B,DONE),

    /* The recognizer keeps the common exit at its decoded index. This fixture's
     * unsupported input type exits without using the untested space between. */
    [OTHER]=JUMP(OP_B,DONE), [OTHER+1 ... DONE-1]=SIMPLE(OP_EXIT),
    [DONE]=SIMPLE(OP_POPZ),SIMPLE(OP_EXIT)
  };
  int ok=1;
  for(int interpreted=0;interpreted<2;interpreted++){
    Fixture fixture={0};
    if(!fixture_init(&fixture)){ fixture_free(&fixture); return 0; }
    add_code(&fixture,"gml_Script_update_input_fields",update,COUNT(update));
    if(!select_dispatch(&fixture,interpreted)){ fixture_free(&fixture); return 0; }
    Input input={0};
    fixture.vm.input.userdata=&input;
    fixture.vm.input.key=key_state;
    fixture.vm.input.gamepad=pad_button;
    fixture.vm.input.gamepad_connected=pad_connected;
    gml_keyboard_unset_map(&fixture.vm);
    GmlInstance *receiver=fixture.receiver;
    GmlVarMap *fields=&receiver->vars;
    *gml_varmap_put(fields,"device_type")=vreal(0);
    *gml_varmap_put(fields,"held")=vreal(0);
    GmlVal binding=gml_arr_new(2,vreal(65));
    gml_arr_set(binding,1,vreal(68));
    *gml_varmap_put(fields,"binding")=binding;
    GmlVal device=vreal(0);
    ok &= expect(same_value(invoke(&fixture,&device,1),vreal(0)) && input_fields(receiver,1,1,1,0),
                 "keyboard axis reports its positive direction and rising edge");
    input.phase=1;
    ok &= expect(same_value(invoke(&fixture,&device,1),vreal(0)) && input_fields(receiver,0,0,0,1),
                 "keyboard axis reports release after the axis clears");

    /* Move the array's owning entry instead of dropping it when switching to a scalar. */
    *gml_varmap_put(fields,"axis_binding")=binding;
    *gml_varmap_put(fields,"binding")=vreal(32769);
    *gml_varmap_put(fields,"device_type")=vreal(1);
    input.phase=0;
    ok &= expect(same_value(invoke(&fixture,&device,1),vreal(0)) && input_fields(receiver,0,1,1,0),
                 "single-pad scalar binding updates its edges");
    ok &= expect(interpreted_body_entered(&fixture)==interpreted,
                 "single-device input uses the selected execution path");
    GmlVal default_device=vundef();
    ok &= expect(same_value(invoke(&fixture,&default_device,1),vreal(0)) &&
                 input_fields(receiver,0,0,0,0) && interpreted_body_entered(&fixture),
                 "an omitted device uses the bytecode's negative device selector");
    input.second_pad=1;
    input.phase=1;
    device=vreal(1);
    if(fixture.vm.diagnostics.code_seen) fixture.vm.diagnostics.code_seen[0]=0;
    ok &= expect(same_value(invoke(&fixture,&device,1),vreal(0)) && input_fields(receiver,0,0,0,1) &&
                 interpreted_body_entered(&fixture),
                 "a second pad uses the interpreter and reads the requested device");
    fixture_free(&fixture);
  }
  return ok;
}

static void recursion_limit_log(void *userdata,AnygmLogLevel level,const char *message){
  unsigned *warnings=userdata;
  if(level==ANYGM_LOG_WARN && strstr(message,"script recursion limit reached")) (*warnings)++;
}

static int recursive_frame_case(void){
  static const Instruction recursive[]={
    ARG(0),INTEGER(0),CMP(CMP_EQ),JUMP(OP_BT,14),
    ARG(0),ARG(0),INTEGER(1),SIMPLE(OP_SUB),CALL("gml_Script_recursive_sum",1),
    SIMPLE(OP_ADD),ARG(0),SIMPLE(OP_ADD),SIMPLE(OP_RET),SIMPLE(OP_EXIT),
    INTEGER(1),SIMPLE(OP_RET)
  };
  Fixture fixture={0};
  if(!fixture_init(&fixture)){ fixture_free(&fixture); return 0; }
  unsigned warnings=0;
  AnygmHostServices host={.userdata=&warnings,.log=recursion_limit_log};
  fixture.vm.host=&host;
  add_code(&fixture,"gml_Script_recursive_sum",recursive,COUNT(recursive));

  GmlVal depth=vreal(299);
  int ok=expect(invoke(&fixture,&depth,1).d==89701 && !warnings,
                "nested calls preserve the caller operand and argument up to the limit");
  depth=vreal(300);
  ok &= expect(invoke(&fixture,&depth,1).d==90300 && warnings==1,
               "the recursion limit produces zero for the rejected call and warns once");
  depth=vreal(301);
  ok &= expect(invoke(&fixture,&depth,1).d==90900 && warnings==1,
               "the recursion limit remains quiet on the next rejected call");
  gml_vm_code_cache_discard(&fixture.code[0]);
  fixture.code[0].cache_bad=1;
  depth=vreal(299);
  ok &= expect(invoke(&fixture,&depth,1).d==89701 && warnings==1,
               "raw bytecode reaches the limit with the full native resolver on each call");
  depth=vreal(3);
  ok &= expect(invoke(&fixture,&depth,1).d==13 && !fixture.vm.execution_depth &&
               !fixture.vm.active_execution_frame && !fixture.vm.exception_context,
               "later invocations reuse scratch with the initial frame state restored");
  fixture_free(&fixture);
  return ok;
}

int main(void){
  int ok=map_lookup_case();
  ok &= approach_case();
  ok &= global_script_case();
  ok &= nested_lookup_case();
  ok &= map_method_loop_case();
  ok &= array_method_loop_case();
  ok &= input_action_case();
  ok &= recursive_frame_case();
  if(ok) puts("VM micro-operation contracts passed");
  return ok?0:1;
}
