/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"
#include "engine_internal.h"
#include "gml_builtin.h"
#include "anygm_test_runner.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  AnygmSyntheticContent content;
  AnygmHostServices host;
  AnygmEngine *engine;
} MouseSession;

static int open_session(MouseSession *session){
  memset(session,0,sizeof *session);
  if(!anygm_synthetic_framebuffer_content_create(&session->content)) return 0;
  session->host.struct_size=sizeof session->host;
  session->host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&session->host);
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=session->content.path;
  source.cache_directory=session->content.directory;
  source.save_directory=session->content.directory;
  return anygm_create(&session->host,&session->engine)==ANYGM_OK &&
         anygm_load(session->engine,&source,NULL)==ANYGM_OK;
}
static void close_session(MouseSession *session){
  if(session->engine) anygm_destroy(session->engine);
  anygm_synthetic_content_destroy(&session->content);
}
static int advance(MouseSession *session,int buttons,int pointer){
  AnygmInputFrame input={0}; AnygmFrameOutput output={0};
  input.struct_size=sizeof input; output.struct_size=sizeof output;
  input.pointer_x=input.pointer_y=pointer?0:-1;
  input.pointer_pressed=pointer!=0;
  input.wheel_delta=2;
  for(unsigned i=0;i<ANYGM_MAX_MOUSE_BUTTONS;i++) input.mouse_buttons[i]=(buttons&(1<<i))!=0;
  return anygm_run_frame(session->engine,&input,&output)==ANYGM_OK;
}
static int observe(MouseSession *session,int held,int pressed,int released,const char *label){
  GmlVM *vm=&session->engine->vm;
  int h=-1,p=-1,r=-1,w=-1;
  gml_input_mouse(vm,NULL,NULL,NULL,NULL,NULL,NULL,&h,&p,&r,&w);
  int ok=h==held && p==pressed && r==released;
  static const char *names[]={"mouse_check_button","mouse_check_button_pressed",
                              "mouse_check_button_released"};
  int masks[]={held,pressed,released};
  for(int edge=0;edge<3;edge++) for(int button=1;button<=5;button++){
    GmlVal arg=vreal(button);
    GmlVal result=gml_builtin_call(vm,names[edge],&arg,1);
    if(result.t!=V_REAL || result.d!=((masks[edge]&(1<<(button-1)))!=0)) ok=0;
  }
  if(!ok) fprintf(stderr,"mouse input: %s: got %d/%d/%d, expected %d/%d/%d\n",
                  label,h,p,r,held,pressed,released);
  return ok;
}
static int clear_button(MouseSession *session,double button,int cached){
  GmlVM *vm=&session->engine->vm; GmlVal arg=vreal(button);
  if(cached){
    int id=gml_builtin_fast_id(vm,"mouse_clear");
    if(id<0){ fprintf(stderr,"mouse input: clear has no cached dispatch\n"); return 0; }
    if(gml_builtin_call_fast_id(vm,id,"mouse_clear",&arg,1).t!=V_UNDEF) return 0;
  } else if(gml_builtin_call(vm,"mouse_clear",&arg,1).t!=V_UNDEF) return 0;
  return 1;
}

#define REQUIRE(condition) do { if(!(condition)){ \
  fprintf(stderr,"mouse input failed at line %d: %s\n",__LINE__,#condition); \
  goto done; } } while(0)

static int temporal_case(void){
  for(int cached=0;cached<2;cached++){
    MouseSession session={0}; int ok=0;
    REQUIRE(open_session(&session));
    REQUIRE(advance(&session,3,0));
    REQUIRE(observe(&session,3,3,0,"initial two-button press"));
    REQUIRE(clear_button(&session,1,cached));
    REQUIRE(observe(&session,2,2,0,"clear only left immediately"));
    REQUIRE(advance(&session,3,0));
    REQUIRE(observe(&session,2,0,0,"held left stays suppressed"));
    REQUIRE(advance(&session,2,0));
    REQUIRE(observe(&session,2,0,1,"later left release remains visible"));
    REQUIRE(clear_button(&session,1,cached));
    REQUIRE(observe(&session,2,0,0,"clear an existing release edge"));
    REQUIRE(advance(&session,3,0));
    REQUIRE(observe(&session,3,1,0,"left repress reopens it"));
    REQUIRE(clear_button(&session,-1,cached));
    REQUIRE(observe(&session,0,0,0,"clear all"));
    REQUIRE(advance(&session,3,0));
    REQUIRE(observe(&session,0,0,0,"both held remain suppressed"));
    REQUIRE(advance(&session,0,0));
    REQUIRE(observe(&session,0,0,3,"both later releases remain visible"));
    REQUIRE(clear_button(&session,-1,cached));
    REQUIRE(observe(&session,0,0,0,"clear while up"));
    REQUIRE(advance(&session,5,0));
    REQUIRE(observe(&session,5,5,0,"fresh left and middle press"));
    const double ignored[]={0,6,-2,NAN,INFINITY,-INFINITY,1e100};
    for(size_t i=0;i<sizeof ignored/sizeof ignored[0];i++){
      REQUIRE(clear_button(&session,ignored[i],cached));
      REQUIRE(observe(&session,5,5,0,"invalid button is harmless"));
    }
    REQUIRE(clear_button(&session,3,cached));
    REQUIRE(observe(&session,1,1,0,"middle clears independently"));
    REQUIRE(session.engine->mouse_wheel==2);
    ok=1;
done:
    close_session(&session);
    if(!ok) return 0;
  }
  return 1;
}

static int isolation_case(void){
  MouseSession first={0},second={0}; int ok=0;
  REQUIRE(open_session(&first) && open_session(&second));
  REQUIRE(advance(&first,0,1) && advance(&second,1,0));
  REQUIRE(observe(&first,1,1,0,"pointer is a real left press"));
  REQUIRE(clear_button(&first,1,0));
  REQUIRE(observe(&first,0,0,0,"pointer is suppressed"));
  REQUIRE(observe(&second,1,1,0,"other engine remains pressed"));
  REQUIRE(advance(&first,1,1));
  REQUIRE(observe(&first,0,0,0,"mouse joins held pointer without reopening"));
  REQUIRE(advance(&first,1,0));
  REQUIRE(observe(&first,0,0,0,"pointer release is not union release"));
  REQUIRE(advance(&first,0,0));
  REQUIRE(observe(&first,0,0,1,"union releases after both sources"));
  REQUIRE(advance(&first,1,0));
  REQUIRE(observe(&first,1,1,0,"physical mouse can reopen"));
  ok=1;
done:
  close_session(&first); close_session(&second); return ok;
}

static uint8_t *save(MouseSession *session,size_t *size,int resume){
  size_t capacity=resume?anygm_state_resume_size(session->engine):anygm_state_size(session->engine);
  uint8_t *bytes=malloc(capacity?capacity:1);
  AnygmResult result=bytes?(resume?
    anygm_state_save_for_resume(session->engine,bytes,capacity,size):
    anygm_state_save(session->engine,bytes,capacity,size)):ANYGM_ERROR_INVALID_ARGUMENT;
  if(result!=ANYGM_OK){
    free(bytes); return NULL;
  }
  return bytes;
}
static int restoration_case(void){
  for(int resume=0;resume<2;resume++){
  MouseSession session={0}; int ok=0;
  uint8_t *saved=NULL,*again=NULL; size_t size=0,again_size=0;
  REQUIRE(open_session(&session));
  REQUIRE(advance(&session,1,0));
  REQUIRE(clear_button(&session,1,0));
  REQUIRE(observe(&session,0,0,0,"save a genuinely suppressed button"));
  saved=save(&session,&size,resume); REQUIRE(saved!=NULL);
  REQUIRE(advance(&session,0,0) && advance(&session,1,0));
  REQUIRE(observe(&session,1,1,0,"mutate away from the saved suppression"));
  REQUIRE(anygm_state_load(session.engine,saved,size)==ANYGM_OK);
  again=save(&session,&again_size,resume);
  REQUIRE(again && again_size==size && !memcmp(saved,again,size));
  if(!resume){
    REQUIRE(advance(&session,1,0));
    REQUIRE(observe(&session,0,0,0,"restoration presentation frame is inert"));
  }
  REQUIRE(advance(&session,1,0));
  REQUIRE(observe(&session,0,0,0,"held input after resume stays suppressed"));
  REQUIRE(advance(&session,0,0));
  REQUIRE(observe(&session,0,0,1,"release after resume survives"));
  REQUIRE(advance(&session,1,0));
  REQUIRE(observe(&session,1,1,0,"repress after resume works"));
  REQUIRE(clear_button(&session,1,0));
  REQUIRE(anygm_reset(session.engine)==ANYGM_OK);
  REQUIRE(advance(&session,1,0));
  REQUIRE(observe(&session,1,1,0,"reset discards suppression"));
  ok=1;
done:
  free(saved); free(again); close_session(&session);
  if(!ok) return 0;
  }
  return 1;
}

static int cold_restoration_case(void){
  MouseSession session={0}; int ok=0;
  uint8_t *saved=NULL,*again=NULL; size_t size=0,again_size=0;
  REQUIRE(open_session(&session));
  REQUIRE(clear_button(&session,-1,0));
  saved=save(&session,&size,1); REQUIRE(saved!=NULL);
  REQUIRE(advance(&session,7,0));
  REQUIRE(observe(&session,7,7,0,"clear before the first poll does not eat a press"));
  REQUIRE(clear_button(&session,-1,0));
  REQUIRE(anygm_state_load(session.engine,saved,size)==ANYGM_OK);
  again=save(&session,&again_size,1);
  REQUIRE(again && again_size==size && !memcmp(saved,again,size));
  REQUIRE(advance(&session,7,0));
  REQUIRE(observe(&session,7,0,0,"cold clear retains no held suppression on resume"));
  REQUIRE(advance(&session,0,0));
  REQUIRE(observe(&session,0,0,7,"all releases follow the resumed host baseline"));
  ok=1;
done:
  free(saved); free(again); close_session(&session); return ok;
}

static double selector(MouseSession *session,const char *name){
  return gml_global_num(&session->engine->vm,name);
}
static int selectors_case(void){
  MouseSession session={0},second={0}; int ok=0;
  uint8_t *saved=NULL,*again=NULL;size_t size=0,again_size=0;
  REQUIRE(open_session(&session) && open_session(&second));
  REQUIRE(selector(&session,"mouse_button")==0 && selector(&session,"mouse_lastbutton")==0);
  REQUIRE(advance(&session,1,0));
  REQUIRE(selector(&session,"mouse_button")==1 && selector(&session,"mouse_lastbutton")==1);
  REQUIRE(advance(&session,17,0));
  REQUIRE(observe(&session,17,16,0,"side two is independent of left"));
  REQUIRE(selector(&session,"mouse_button")==5 && selector(&session,"mouse_lastbutton")==5);
  gml_vm_assign_current_scope(&session.engine->vm,"mouse_button",vreal(2));
  REQUIRE(selector(&session,"mouse_button")==5);
  gml_vm_assign_current_scope(&session.engine->vm,"mouse_lastbutton",vreal(0));
  REQUIRE(advance(&session,17,0));
  REQUIRE(selector(&session,"mouse_lastbutton")==0);
  REQUIRE(advance(&session,1,0));
  REQUIRE(selector(&session,"mouse_button")==1 && selector(&session,"mouse_lastbutton")==0);
  REQUIRE(advance(&session,9,0));
  REQUIRE(observe(&session,9,8,0,"side one is independent of left"));
  REQUIRE(selector(&session,"mouse_button")==4 && selector(&session,"mouse_lastbutton")==4);
  REQUIRE(clear_button(&session,4,1));
  REQUIRE(selector(&session,"mouse_button")==0 && selector(&session,"mouse_lastbutton")==4);
  saved=save(&session,&size,1); REQUIRE(saved);
  REQUIRE(advance(&session,0,0) && advance(&session,16,0));
  REQUIRE(selector(&session,"mouse_lastbutton")==5);
  REQUIRE(anygm_state_load(session.engine,saved,size)==ANYGM_OK);
  again=save(&session,&again_size,1);
  REQUIRE(again && again_size==size && !memcmp(saved,again,size));
  REQUIRE(selector(&session,"mouse_button")==0 && selector(&session,"mouse_lastbutton")==4);
  REQUIRE(advance(&session,9,0));
  REQUIRE(observe(&session,1,0,0,"restored side suppression survives held input"));
  REQUIRE(advance(&session,1,0));
  REQUIRE(observe(&session,1,0,8,"side release remains visible"));
  REQUIRE(advance(&session,31,0));
  REQUIRE(observe(&session,31,30,0,"all five buttons are independent"));
  REQUIRE(selector(&session,"mouse_button")==5 && selector(&session,"mouse_lastbutton")==5);
  REQUIRE(clear_button(&session,-1,0));
  REQUIRE(observe(&session,0,0,0,"all-button clear includes both sides"));
  REQUIRE(selector(&session,"mouse_button")==0 && selector(&session,"mouse_lastbutton")==5);
  REQUIRE(selector(&second,"mouse_button")==0 && selector(&second,"mouse_lastbutton")==0);
  REQUIRE(anygm_reset(session.engine)==ANYGM_OK);
  REQUIRE(selector(&session,"mouse_button")==0 && selector(&session,"mouse_lastbutton")==0);
  ok=1;
done:
  free(saved);free(again);close_session(&session);close_session(&second);return ok;
}

static int action_purity_case(void){
  MouseSession session={0};int ok=0;uint8_t *before=NULL,*after=NULL;
  size_t size=0,after_size=0;
  REQUIRE(open_session(&session));
  REQUIRE(advance(&session,3,0) && advance(&session,2,0));
  REQUIRE(observe(&session,2,0,1,"mouse action has held right and released left"));
  before=save(&session,&size,1);REQUIRE(before);
  GmlVM *vm=&session.engine->vm;int id=gml_builtin_fast_id(vm,"action_if_mouse");
  REQUIRE(id>=0);
  for(int repeat=0;repeat<16;repeat++) for(int button=1;button<=3;button++){
    GmlVal arg=vreal(button);
    int expected=button==1 || button==2;
    GmlVal named=gml_builtin_call(vm,"action_if_mouse",&arg,1);
    GmlVal cached=gml_builtin_call_fast_id(vm,id,"action_if_mouse",&arg,1);
    REQUIRE(named.t==V_REAL && named.d==expected && cached.t==V_REAL && cached.d==expected);
  }
  REQUIRE(observe(&session,2,0,1,"action queries do not consume held or release observations"));
  after=save(&session,&after_size,1);
  REQUIRE(after && after_size==size && !memcmp(before,after,size));
  ok=1;
done:
  free(before);free(after);close_session(&session);return ok;
}

int main(int argc,char **argv){
  static const AnygmTestCase cases[]={
    {"temporal",temporal_case}, {"isolation",isolation_case},
    {"restoration",restoration_case}, {"cold_restoration",cold_restoration_case}, {"selectors",selectors_case}, {"action_purity",action_purity_case}
  };
  AnygmTestGroup group={"mouse",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result={0};
  const char *filter=argc==3 && !strcmp(argv[1],"--case")?argv[2]:NULL;
  return anygm_test_run_groups(&group,1,filter,&result)?0:1;
}
