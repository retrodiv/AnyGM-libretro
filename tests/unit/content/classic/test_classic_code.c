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

AnygmTestGroup classic_test_code_group(void){
  static const AnygmTestCase cases[]={
    {"script-import",expect_script_import},
  };
  const AnygmTestGroup group={
    "classic.code",cases,sizeof cases/sizeof cases[0]
  };
  return group;
}
