/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "classic_test_fixture.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

static void discard_imported_objects(GmlcProject *project, int remove_sources){
  for(int i = 0; i < project->n_objects; ++i){
    free(project->objects[i].id);
    free(project->objects[i].name);
    for(int event = 0; event < project->objects[i].n_events; ++event){
      free(project->objects[i].events[event].id);
      free(project->objects[i].events[event].collision_id);
      if(remove_sources && project->objects[i].events[event].source_path)
        remove(project->objects[i].events[event].source_path);
      free(project->objects[i].events[event].source_path);
    }
    free(project->objects[i].events);
  }
  free(project->objects);
  project->objects = NULL;
  project->n_objects = project->cap_objects = 0;
}

static void discard_imported_backgrounds(GmlcProject *project, int remove_sources){
  for(int i = 0; i < project->n_sprites; ++i){
    free(project->sprites[i].id); free(project->sprites[i].name);
    for(int frame = 0; frame < project->sprites[i].n_frames; ++frame){
      if(remove_sources && project->sprites[i].frame_paths && project->sprites[i].frame_paths[frame])
        remove(project->sprites[i].frame_paths[frame]);
      free(project->sprites[i].frame_paths ? project->sprites[i].frame_paths[frame] : NULL);
    }
    free(project->sprites[i].frame_paths);
  }
  free(project->sprites);
  for(int i = 0; i < project->n_tilesets; ++i){
    free(project->tilesets[i].id); free(project->tilesets[i].name);
  }
  free(project->tilesets);
  memset(project, 0, sizeof(*project));
}

static void discard_imported_timelines(GmlcProject *project, int remove_sources){
  for(int i=0;i<project->n_timelines;i++){
    free(project->timelines[i].id); free(project->timelines[i].name);
    for(int m=0;m<project->timelines[i].n_moments;m++){
      if(remove_sources && project->timelines[i].moments[m].source_path)
        remove(project->timelines[i].moments[m].source_path);
      free(project->timelines[i].moments[m].source_path);
    }
    free(project->timelines[i].moments);
  }
  free(project->timelines);
  project->timelines=NULL; project->n_timelines=project->cap_timelines=0;
}

static void discard_imported_rooms(GmlcProject *project, int remove_sources){
  for(int i = 0; i < project->n_rooms; ++i){
    GmlcRoom *room = &project->rooms[i];
    if(remove_sources && room->creation_code_path) remove(room->creation_code_path);
    free(room->creation_code_path); free(room->id); free(room->name);
    for(int instance = 0; instance < room->n_instances; ++instance){
      if(remove_sources && room->instances[instance].creation_code_path)
        remove(room->instances[instance].creation_code_path);
      free(room->instances[instance].creation_code_path);
      free(room->instances[instance].id); free(room->instances[instance].name);
    }
    free(room->instances); free(room->backgrounds); free(room->tiles);
  }
  free(project->rooms);
  memset(project, 0, sizeof(*project));
}

static int prepare_test_directory(void){
#ifdef _WIN32
  if(_mkdir("tmp")==0 || errno==EEXIST) return 1;
#else
  if(mkdir("tmp",0777)==0 || errno==EEXIST) return 1;
#endif
  fprintf(stderr,"cannot create classic test directory\n");
  return 0;
}

int main(int argc, char **argv){
  classic_fixture_host_init();
  if(argc==3 && !strcmp(argv[1],"--write-legacy-exe-fixture")){
    Fixture executable;
    if(!build_legacy_executable_fixture(&executable)) return 1;
    FILE *file=fopen(argv[2],"wb");
    int ok=file && fwrite(executable.data,1,executable.size,file)==executable.size;
    if(file && fclose(file)!=0) ok=0;
    if(!ok){ fprintf(stderr,"cannot write legacy executable fixture: %s\n",argv[2]); return 1; }
    printf("wrote legacy executable fixture: %s (%zu bytes)\n",argv[2],executable.size);
    return 0;
  }
  if(argc==3 && !strcmp(argv[1],"--write-exe-fixture")){
    Fixture executable;
    if(!build_executable_fixture(&executable)) return 1;
    FILE *file=fopen(argv[2],"wb");
    int ok=file && fwrite(executable.data,1,executable.size,file)==executable.size;
    if(file && fclose(file)!=0) ok=0;
    if(!ok){ fprintf(stderr,"cannot write executable fixture: %s\n",argv[2]); return 1; }
    printf("wrote executable fixture: %s (%zu bytes)\n",argv[2],executable.size);
    return 0;
  }
  if(argc==4 && !strcmp(argv[1],"--write-project-fixture")){
    unsigned version=(unsigned)strtoul(argv[2],NULL,10);
    Fixture project;
    if(!build_project_fixture(version,&project)){
      fprintf(stderr,"unsupported project fixture version: %s\n",argv[2]);
      return 1;
    }
    FILE *file=fopen(argv[3],"wb");
    int ok=file && fwrite(project.data,1,project.size,file)==project.size;
    if(file && fclose(file)!=0) ok=0;
    if(!ok){ fprintf(stderr,"cannot write project fixture: %s\n",argv[3]); return 1; }
    printf("wrote project fixture %u: %s (%zu bytes)\n",version,argv[3],project.size);
    return 0;
  }
  if(!prepare_test_directory()) return EXIT_FAILURE;
  const char *case_filter=NULL;
  for(int i=1;i<argc;++i){
    if(strcmp(argv[i],"--case")) continue;
    if(i+1>=argc){
      fprintf(stderr,"--case requires a filter\n");
      return EXIT_FAILURE;
    }
    case_filter=argv[++i];
  }
  const AnygmTestGroup groups[]={
    classic_test_format_group(),
    classic_test_code_group(),
    classic_test_extensions_group(),
    classic_test_images_group(),
    classic_test_fonts_group(),
    classic_test_audio_group(),
    classic_test_legacy_media_group(),
    classic_test_paths_group(),
    classic_test_timelines_group(),
    classic_test_objects_group(),
    classic_test_rooms_group(),
  };
  AnygmTestResult result;
  anygm_test_run_groups(
    groups,sizeof groups/sizeof groups[0],case_filter,&result
  );
  int passed=result.passed;
  int failed=result.failed;

  for(int i = 1; i < argc; ++i){
    if(!strcmp(argv[i],"--case")){
      ++i;
      continue;
    }
    GmlcClassicInventory in;
    GmlcClassicHeader h;
    GmlcClassicManifest manifest;
    char err[512];
    if(!gmlc_classic_manifest_file(classic_fixture_host(),argv[i],
                                   &manifest,err,sizeof(err))){
      fprintf(stderr, "%s: %s\n", argv[i], err);
      ++failed;
      continue;
    }
    in=manifest.inventory;
    h=in.header;
      printf("%s\t%u\t%s\tsettings=%u swapcreate=%d sounds=%u/%u sprites=%u/%u backgrounds=%u/%u paths=%u/%u scripts=%u/%u fonts=%u/%u timelines=%u/%u objects=%u/%u rooms=%u/%u\n",
             argv[i], (unsigned)h.version, gmlc_classic_version_name(h.version), in.settings_version,
             in.settings.swap_creation_events,
             manifest.existing[GMLC_CLASSIC_SOUND], in.resource_slots[GMLC_CLASSIC_SOUND],
             manifest.existing[GMLC_CLASSIC_SPRITE], in.resource_slots[GMLC_CLASSIC_SPRITE],
             manifest.existing[GMLC_CLASSIC_BACKGROUND], in.resource_slots[GMLC_CLASSIC_BACKGROUND],
             manifest.existing[GMLC_CLASSIC_PATH], in.resource_slots[GMLC_CLASSIC_PATH],
             manifest.existing[GMLC_CLASSIC_SCRIPT], in.resource_slots[GMLC_CLASSIC_SCRIPT],
             manifest.existing[GMLC_CLASSIC_FONT], in.resource_slots[GMLC_CLASSIC_FONT],
             manifest.existing[GMLC_CLASSIC_TIMELINE], in.resource_slots[GMLC_CLASSIC_TIMELINE],
             manifest.existing[GMLC_CLASSIC_OBJECT], in.resource_slots[GMLC_CLASSIC_OBJECT],
             manifest.existing[GMLC_CLASSIC_ROOM], in.resource_slots[GMLC_CLASSIC_ROOM]);
      GmlcProject project;
      fixture_project_clear(&project);
      const char *object_dir = "tmp/classic_object_corpus";
#ifdef _WIN32
      _mkdir(object_dir);
#else
      mkdir(object_dir, 0777);
#endif
      if(!gmlc_classic_import_objects(&manifest, &project, object_dir, err, sizeof(err))){
        fprintf(stderr, "%s: object import: %s\n", argv[i], err);
        gmlc_classic_manifest_free(&manifest);
        ++failed;
        continue;
      }
      discard_imported_objects(&project, 1);
      if(!gmlc_classic_import_timelines(&manifest, &project, object_dir, err, sizeof(err))){
        fprintf(stderr, "%s: timeline import: %s\n", argv[i], err);
        gmlc_classic_manifest_free(&manifest);
        ++failed;
        continue;
      }
      discard_imported_timelines(&project, 1);
      if(h.version >= GMLC_CLASSIC_GM8 &&
         !gmlc_classic_import_backgrounds(&manifest, &project, object_dir, err, sizeof(err))){
        fprintf(stderr, "%s: background import: %s\n", argv[i], err);
        gmlc_classic_manifest_free(&manifest);
        ++failed;
        continue;
      }
      discard_imported_backgrounds(&project, 1);
      if(!gmlc_classic_import_rooms(&manifest, &project, object_dir, err, sizeof(err))){
        fprintf(stderr, "%s: room import: %s\n", argv[i], err);
        gmlc_classic_manifest_free(&manifest);
        ++failed;
        continue;
      }
      discard_imported_rooms(&project, 1);
      gmlc_classic_manifest_free(&manifest);
    ++passed;
  }
  printf("classic probe: passed=%d failed=%d\n", passed, failed);
  return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
