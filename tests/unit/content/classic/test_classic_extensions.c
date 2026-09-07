/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "classic_test_fixture.h"
#include "content_transform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

static Fixture extension_fixture(const char *package_name,
                                 const char *file_name,
                                 unsigned kind,
                                 unsigned convention,
                                 const char *public_name,
                                 const char *target_name,
                                 const char *source,
                                 const char *constant_name,
                                 const char *constant_value){
  Fixture plain={{0},0}, encoded={{0},0};
  fixture_u32(&plain,653); fixture_u32(&plain,0);
  fixture_string(&plain,package_name);
  for(int i=1;i<8;i++) fixture_string(&plain,"");
  fixture_u32(&plain,0); /* hidden */
  fixture_u32(&plain,0); /* uses */
  fixture_u32(&plain,1); /* files */
  fixture_u32(&plain,700);
  fixture_string(&plain,file_name); fixture_string(&plain,file_name);
  fixture_u32(&plain,kind);
  fixture_string(&plain,""); fixture_string(&plain,"");
  fixture_u32(&plain,1); /* functions */
  fixture_u32(&plain,700);
  fixture_string(&plain,public_name); fixture_string(&plain,target_name);
  fixture_u32(&plain,convention);
  fixture_string(&plain,"");
  fixture_u32(&plain,0); fixture_u32(&plain,0);
  fixture_zero(&plain,18u*4u);
  fixture_u32(&plain,constant_name ? 1u : 0u); /* constants */
  if(constant_name){
    fixture_u32(&plain,700);
    fixture_string(&plain,constant_name);
    fixture_string(&plain,constant_value ? constant_value : "");
    fixture_u32(&plain,0); /* visible */
  }
  fixture_compressed(&plain,(const unsigned char*)source,(int)strlen(source));

  fixture_u32(&encoded,GMLC_CLASSIC_MAGIC);
  fixture_u32(&encoded,701);
  if(encoded.size+plain.size>sizeof(encoded.data)) abort();
  memcpy(encoded.data+encoded.size,plain.data,plain.size);
  encoded.size+=plain.size;
  return encoded;
}

static int write_fixture_file(const char *path, const Fixture *fixture, size_t limit){
  FILE *file=fopen(path,"wb");
  size_t size=fixture->size<limit ? fixture->size : limit;
  int ok=file && fwrite(fixture->data,1,size,file)==size;
  if(file && fclose(file)!=0) ok=0;
  return ok;
}

static int check_extension_alias_import(const AnygmContentTransforms *transforms){
  AnygmHostServices host={0};
  host.struct_size=sizeof host;
  host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&host);
  const char *dir="tmp/classic_extension_fixture";
#ifdef _WIN32
  _mkdir("tmp"); _mkdir(dir);
#else
  mkdir("tmp",0777); mkdir(dir,0777);
#endif
  const char *paths[]={
    "tmp/classic_extension_fixture/base.gex",
    "tmp/classic_extension_fixture/secondary.GEX",
    "tmp/classic_extension_fixture/conflict.gex",
    "tmp/classic_extension_fixture/unrelated.gex",
    "tmp/classic_extension_fixture/truncated.gex",
    "tmp/classic_extension_fixture/embedded.gex",
    "tmp/classic_extension_fixture/binary.gex",
    "tmp/classic_extension_fixture/project.gmk.classic-extension-000.gex"
  };
  Fixture fixtures[]={
    extension_fixture("fixtureextension","fixture.gml",2,2,
                      "fixture_route","fixture_target",
                      "#define fixture_target\nreturn 1;\n","extension_truth","1"),
    extension_fixture("Fixture Extension","fixture.gml",2,2,
                      "fixture_action","fixture_target",
                      "#define fixture_target\nreturn 2;\n","project_truth","9"),
    extension_fixture("Fixture Extension","fixture.gml",2,2,
                      "fixture_route","alternate_target",
                      "#define alternate_target\nreturn 3;\n",NULL,NULL),
    extension_fixture("Different Extension","fixture.gml",2,2,
                      "unrelated_action","fixture_target",
                      "#define fixture_target\nreturn 4;\n","unrelated_constant","9"),
    extension_fixture("Fixture Extension","fixture.gml",2,2,
                      "broken_action","fixture_target",
                      "#define fixture_target\nreturn 5;\n","broken_constant","5"),
    extension_fixture("Fixture Extension","fixture.gml",2,2,
                      "embedded_action","embedded_target",
                      "#define helper_target\nreturn 6;\n#define embedded_target\nreturn 37;\n",
                      NULL,NULL),
    extension_fixture("Fixture Extension","saudio.dll",1,11,
                      "package_audio_open","open","",NULL,NULL),
    extension_fixture("Fixture Extension","fixture.gml",2,2,
                      "ordinal_placeholder_action","fixture_target",
                      "#define fixture_target\nreturn 8;\n",NULL,NULL)
  };
  int files_ok=1;
  for(int i=0;i<8;i++)
    files_ok &= write_fixture_file(paths[i],&fixtures[i],
                                   i==4 ? fixtures[i].size-3u : SIZE_MAX);
  uint64_t dependency_before=0,dependency_repeat=0,dependency_after=0;
  int dependency_ok=files_ok &&
    gmlc_classic_extension_dependency_hash(&host,dir,123u,&dependency_before) &&
    gmlc_classic_extension_dependency_hash(&host,dir,123u,&dependency_repeat) &&
    dependency_before==dependency_repeat;

  char *extension_names[]={(char*)"Fixture Extension"};
  GmlcClassicManifest manifest;
  memset(&manifest,0,sizeof(manifest));
  manifest.extension_names=extension_names;
  manifest.extension_count=1;
  GmlcClassicExtensionFunction direct_function={0};
  direct_function.name=(char*)"direct_audio_open";
  direct_function.external_name=(char*)"open";
  GmlcClassicExtensionFile direct_file={0};
  direct_file.name=(char*)"saudio.dll";
  direct_file.kind=1;
  direct_file.functions=&direct_function;
  direct_file.function_count=1;
  GmlcClassicExtension direct_extension={0};
  direct_extension.files=&direct_file;
  direct_extension.file_count=1;
  manifest.extensions=&direct_extension;
  manifest.extension_detail_count=1;
  GmlcProject project;
  gmlc_project_init(&project);
  project.host=&host;
  project.prefer_memory_files=1;
  project.scripts=(GmlcScript*)calloc(2,sizeof(*project.scripts));
  project.n_scripts=project.cap_scripts=2;
  if(project.scripts){
    project.scripts[0].name=gmlc_strdup("fixture_target");
    project.scripts[1].name=gmlc_strdup("alternate_target");
  }
  project.constants=(GmlcProjectConstant*)calloc(1,sizeof(*project.constants));
  project.n_constants=project.cap_constants=project.constants ? 1 : 0;
  if(project.constants){
    project.constants[0].name=gmlc_strdup("project_truth");
    project.constants[0].expression=gmlc_strdup("42");
  }
  char err[256]={0};
  int imported=files_ok && project.scripts && project.scripts[0].name && project.scripts[1].name &&
    project.constants && project.constants[0].name && project.constants[0].expression &&
    gmlc_classic_import_extension_aliases(transforms,&manifest,&project,dir,err,sizeof(err));
  int found_action=0, found_ambiguous=0, found_unrelated=0, found_broken=0;
  int found_embedded=0, embedded_script=0, found_extension_constant=0;
  int found_package_binary=0,found_direct_binary=0,found_ordinal_placeholder=0;
  int found_project_constant=0, found_unrelated_constant=0, found_broken_constant=0;
  for(int i=0;i<project.n_function_aliases;i++){
    GmlcFunctionAlias *alias=&project.function_aliases[i];
    if(!strcmp(alias->public_name,"fixture_action") &&
       !strcmp(alias->target_name,"fixture_target") && !alias->ambiguous) found_action=1;
    if(!strcmp(alias->public_name,"fixture_route") && alias->ambiguous) found_ambiguous=1;
    if(!strcmp(alias->public_name,"unrelated_action")) found_unrelated=1;
    if(!strcmp(alias->public_name,"broken_action")) found_broken=1;
    if(!strcmp(alias->public_name,"embedded_action") &&
       !strcmp(alias->target_name,"embedded_target") && !alias->ambiguous) found_embedded=1;
    if(!strcmp(alias->public_name,"ordinal_placeholder_action")) found_ordinal_placeholder=1;
    if(!strcmp(alias->target_name,
       "__anygm_external_73617564696f2e646c6c_6f70656e") && !alias->ambiguous){
      if(!strcmp(alias->public_name,"package_audio_open")) found_package_binary=1;
      if(!strcmp(alias->public_name,"direct_audio_open")) found_direct_binary=1;
    }
  }
  for(int i=0;i<project.n_scripts;i++){
    GmlcScript *script=&project.scripts[i];
    if(!script->name || strcmp(script->name,"embedded_target")) continue;
    char *source=gmlc_project_read_source(&project,script->source_path);
    embedded_script=source && !strcmp(source,"return 37;\n");
    free(source);
  }
  for(int i=0;i<project.n_constants;i++){
    GmlcProjectConstant *constant=&project.constants[i];
    if(!strcmp(constant->name,"extension_truth") &&
       !strcmp(constant->expression,"1")) found_extension_constant=1;
    if(!strcmp(constant->name,"project_truth") &&
       !strcmp(constant->expression,"42")) found_project_constant=1;
    if(!strcmp(constant->name,"unrelated_constant")) found_unrelated_constant=1;
    if(!strcmp(constant->name,"broken_constant")) found_broken_constant=1;
  }
  FILE *changed=fopen(paths[5],"ab");
  int changed_ok=changed && fputc(0x5a,changed)!=EOF;
  if(changed && fclose(changed)!=0) changed_ok=0;
  dependency_ok = dependency_ok && changed_ok &&
    gmlc_classic_extension_dependency_hash(&host,dir,123u,&dependency_after) &&
    dependency_after!=dependency_before;
  int ok=imported && !err[0] && project.n_function_aliases==5 && project.n_scripts==3 &&
         found_action && found_ambiguous && !found_unrelated && !found_broken &&
         found_embedded && !found_ordinal_placeholder &&
         found_package_binary && found_direct_binary && embedded_script &&
         found_extension_constant && found_project_constant &&
         !found_unrelated_constant && !found_broken_constant && project.n_constants==2 &&
         dependency_ok;
  if(!ok) fprintf(stderr,"extension alias fixture failed: aliases=%d error=%s\n",
                  project.n_function_aliases,err);
  gmlc_project_free(&project);
  for(int i=0;i<8;i++) remove(paths[i]);
#ifdef _WIN32
  _rmdir(dir);
#else
  rmdir(dir);
#endif
  return ok;
}

static int expect_extension_alias_import(void){ return check_extension_alias_import(NULL); }

static int expect_extension_record_context(void){
  const char config[]="[transforms]\nrecord=buffer package(){"
    "if(metadata_size!=16 || read32(metadata,0)!=0x43534c43 || read32(metadata,4)!=3 ||"
    "read32(metadata,8)!=701 || read32(metadata,12))reject();return slice(0,input_size);}\n";
  AnygmContentTransforms *set=anygm_content_transforms_create(); char error[256]={0};
  int ok=set && anygm_content_transforms_parse(set,config,sizeof config-1,error,sizeof error) &&
    check_extension_alias_import(set);
  anygm_content_transforms_destroy(set);
  if(!ok) fprintf(stderr,"extension record context: %s\n",error);
  return ok;
}

AnygmTestGroup classic_test_extensions_group(void){
  static const AnygmTestCase cases[]={
    {"alias-import",expect_extension_alias_import},
    {"record-context",expect_extension_record_context},
  };
  const AnygmTestGroup group={
    "classic.extensions",cases,sizeof cases/sizeof cases[0]
  };
  return group;
}
