/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "classic_test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

static int expect_script_import(void){
  Fixture fixture = manifest_fixture(800);
  GmlcClassicManifest manifest;
  GmlcProject project;
  fixture_project_clear(&project);
  char err[256], dir[128] = "tmp/classic_script_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  if(!gmlc_classic_manifest(test_transforms(),fixture.data, fixture.size, &manifest, err, sizeof(err))){
    fprintf(stderr, "script import manifest failed: %s\n", err);
    return 0;
  }
  int ok = gmlc_classic_import_scripts(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "script import failed: %s\n", err);
  if(ok){
    FILE *file = fopen(project.scripts[0].source_path, "rb");
    char text[32] = {0};
    size_t got = file ? fread(text, 1, sizeof(text) - 1, file) : 0;
    if(file) fclose(file);
    ok = project.n_scripts == 1 && project.n_script_order == 1 && got == 9 &&
         !strcmp(project.scripts[0].name, "resource_script") && !strcmp(text, "return 7;");
    remove(project.scripts[0].source_path);
  }
  for(int i = 0; i < project.n_scripts; ++i){
    free(project.scripts[i].id); free(project.scripts[i].name); free(project.scripts[i].source_path);
  }
  free(project.scripts);
  for(int i = 0; i < project.n_script_order; ++i) free(project.script_order_ids[i]);
  free(project.script_order_ids);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static void fixture_action(Fixture *payload,unsigned kind,unsigned question,
                           int target,unsigned relative,const char *function,
                           const char *first,const char *second){
  unsigned count=second?2u:first?1u:0u;
  fixture_u32(payload,440); fixture_u32(payload,1); fixture_u32(payload,0);
  fixture_u32(payload,kind); fixture_u32(payload,1); fixture_u32(payload,question);
  fixture_u32(payload,1); fixture_u32(payload,1);
  fixture_string(payload,function); fixture_string(payload,"");
  fixture_u32(payload,count); fixture_u32(payload,count);
  for(unsigned i=0;i<count;i++) fixture_u32(payload,0);
  fixture_u32(payload,(unsigned)target); fixture_u32(payload,relative);
  fixture_u32(payload,count);
  if(first) fixture_string(payload,first);
  if(second) fixture_string(payload,second);
  fixture_u32(payload,0);
}

static int expect_relative_action_statement(void){
  /* A question or repeat owns one action, including its relative-mode setup
   * and teardown. Else must still attach to that question after lowering. */
  Fixture payload={{0},0};
  fixture_u32(&payload,(unsigned)-1); fixture_u32(&payload,0); fixture_u32(&payload,1);
  fixture_u32(&payload,0); fixture_u32(&payload,0); fixture_u32(&payload,(unsigned)-100);
  fixture_u32(&payload,(unsigned)-1); fixture_u32(&payload,0);
  fixture_u32(&payload,0); /* Create event */
  fixture_u32(&payload,400); fixture_u32(&payload,8);
  fixture_action(&payload,0,1,-1,0,"action_if","false",NULL);
  fixture_action(&payload,0,0,-1,1,"action_move_to","2","3");
  fixture_action(&payload,3,0,-1,0,"",NULL,NULL);
  fixture_action(&payload,0,0,-1,1,"action_move_to","4","5");
  fixture_action(&payload,5,0,-1,0,"","3",NULL);
  fixture_action(&payload,0,0,-1,1,"action_move_to","6","7");
  fixture_action(&payload,0,1,-1,0,"action_if","true",NULL);
  fixture_action(&payload,0,0,0,1,"action_move_to","8","9");
  fixture_u32(&payload,(unsigned)-1);

  GmlcClassicResourceSlot slot={0};
  slot.exists=1; slot.name="resource_actor";
  slot.payload=payload.data; slot.payload_size=payload.size;
  GmlcClassicManifest manifest={0};
  manifest.inventory.resource_slots[GMLC_CLASSIC_OBJECT]=1;
  manifest.slots[GMLC_CLASSIC_OBJECT]=&slot;
  GmlcProject project; fixture_project_clear(&project);
  project.prefer_memory_files=1;
  char error[256]={0};
  int ok=gmlc_classic_import_objects(&manifest,&project,"memory",error,sizeof error);
  char *source=ok?gmlc_project_read_source(&project,project.objects[0].events[0].source_path):NULL;
  const char *expected=
    "if (action_if(false,0))\n"
    "{\naction_set_relative(1);\naction_move_to(2,3);\naction_set_relative(0);\n}\n"
    "else\n"
    "{\naction_set_relative(1);\naction_move_to(4,5);\naction_set_relative(0);\n}\n"
    "repeat (3)\n"
    "{\naction_set_relative(1);\naction_move_to(6,7);\naction_set_relative(0);\n}\n"
    "if (action_if(true,0))\nwith (0) {\n"
    "{\naction_set_relative(1);\naction_move_to(8,9);\naction_set_relative(0);\n}\n}\n";
  ok=ok && source && !strcmp(source,expected);
  if(!ok) fprintf(stderr,"relative action statement: %s\n%s\n",error,source?source:"<no source>");
  free(source);
  gmlc_project_free(&project);
  return ok;
}

static int expect_other_question_context(void){
  for(unsigned negate=0;negate<2;negate++){
    Fixture payload={{0},0};
    fixture_u32(&payload,(unsigned)-1); fixture_u32(&payload,0); fixture_u32(&payload,1);
    fixture_u32(&payload,0); fixture_u32(&payload,0); fixture_u32(&payload,(unsigned)-100);
    fixture_u32(&payload,(unsigned)-1); fixture_u32(&payload,0);
    fixture_u32(&payload,0);
    fixture_u32(&payload,400); fixture_u32(&payload,4);
    fixture_action(&payload,0,1,-2,0,"action_if","object_index == 2",NULL);
    payload.data[payload.size-4]=(unsigned char)negate;
    fixture_action(&payload,0,0,-1,0,"action_move_to","5","6");
    fixture_action(&payload,3,0,-1,0,"",NULL,NULL);
    fixture_action(&payload,0,0,-1,0,"action_move_to","7","8");
    fixture_u32(&payload,(unsigned)-1);
    GmlcClassicResourceSlot slot={0};
    slot.exists=1; slot.name="resource_actor";
    slot.payload=payload.data; slot.payload_size=payload.size;
    GmlcClassicManifest manifest={0};
    manifest.inventory.resource_slots[GMLC_CLASSIC_OBJECT]=1;
    manifest.slots[GMLC_CLASSIC_OBJECT]=&slot;
    GmlcProject project; fixture_project_clear(&project);
    project.prefer_memory_files=1;
    char error[256]={0},expected[512];
    snprintf(expected,sizeof expected,
      "if (%s(function(){\nwith (-2) {\nreturn action_if(object_index == 2,0);\n}\n"
      "return false;\n})())\naction_move_to(5,6);\nelse\naction_move_to(7,8);\n",
      negate?"!":"");
    int ok=gmlc_classic_import_objects(&manifest,&project,"memory",error,sizeof error);
    char *source=ok?gmlc_project_read_source(&project,project.objects[0].events[0].source_path):NULL;
    ok=ok && source && !strcmp(source,expected);
    if(!ok) fprintf(stderr,"other question context: %s\n%s\n",error,source?source:"<no source>");
    free(source);
    gmlc_project_free(&project);
    if(!ok) return 0;
  }
  return 1;
}

static int expect_object_question_context(void){
  for(unsigned negate=0;negate<2;negate++){
    Fixture payload={{0},0};
    fixture_u32(&payload,(unsigned)-1); fixture_u32(&payload,0); fixture_u32(&payload,1);
    fixture_u32(&payload,0); fixture_u32(&payload,0); fixture_u32(&payload,(unsigned)-100);
    fixture_u32(&payload,(unsigned)-1); fixture_u32(&payload,0);
    fixture_u32(&payload,0);
    fixture_u32(&payload,400); fixture_u32(&payload,4);
    fixture_action(&payload,0,1,2,0,"action_if","x > 0",NULL);
    payload.data[payload.size-4]=(unsigned char)negate;
    fixture_action(&payload,0,0,-1,0,"action_move_to","5","6");
    fixture_action(&payload,3,0,-1,0,"",NULL,NULL);
    fixture_action(&payload,0,0,-1,0,"action_move_to","7","8");
    fixture_u32(&payload,(unsigned)-1);
    GmlcClassicResourceSlot slot={0};
    slot.exists=1; slot.name="resource_actor";
    slot.payload=payload.data; slot.payload_size=payload.size;
    GmlcClassicManifest manifest={0};
    manifest.inventory.resource_slots[GMLC_CLASSIC_OBJECT]=1;
    manifest.slots[GMLC_CLASSIC_OBJECT]=&slot;
    GmlcProject project; fixture_project_clear(&project);
    project.prefer_memory_files=1;
    char error[256]={0},expected[512];
    snprintf(expected,sizeof expected,
      "if ((function(){\nwith (2) {\nif (%s(action_if(x > 0,0))) return false;\n}\n"
      "return true;\n})())\naction_move_to(5,6);\nelse\naction_move_to(7,8);\n",
      negate?"":"!");
    int ok=gmlc_classic_import_objects(&manifest,&project,"memory",error,sizeof error);
    char *source=ok?gmlc_project_read_source(&project,project.objects[0].events[0].source_path):NULL;
    ok=ok && source && !strcmp(source,expected);
    if(!ok) fprintf(stderr,"object question context: %s\n%s\n",error,source?source:"<no source>");
    free(source);
    gmlc_project_free(&project);
    if(!ok) return 0;
  }
  return 1;
}

AnygmTestGroup classic_test_code_group(void){
  static const AnygmTestCase cases[]={
    {"script-import",expect_script_import},
    {"relative-action-statement",expect_relative_action_statement},
    {"other-question-context",expect_other_question_context},
    {"object-question-context",expect_object_question_context},
  };
  const AnygmTestGroup group={
    "classic.code",cases,sizeof cases/sizeof cases[0]
  };
  return group;
}
