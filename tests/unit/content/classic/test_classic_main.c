/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "classic_test_fixture.h"

#include <errno.h>
#include <limits.h>
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

/* Read a whole source file so the caller can embed it in a container. The classic families keep
 * code as text and compile it at load, so there is nothing to build here — only to carry. */
static char *read_source_file(const char *path){
  FILE *file=fopen(path,"rb");
  if(!file){ fprintf(stderr,"cannot open source: %s\n",path); return NULL; }
  if(fseek(file,0,SEEK_END)!=0){ fclose(file); fprintf(stderr,"cannot size source: %s\n",path); return NULL; }
  long size=ftell(file);
  if(size<0 || fseek(file,0,SEEK_SET)!=0){ fclose(file); fprintf(stderr,"cannot size source: %s\n",path); return NULL; }
  char *text=(char*)malloc((size_t)size+1);
  if(!text){ fclose(file); fprintf(stderr,"out of memory reading source: %s\n",path); return NULL; }
  if(size && fread(text,1,(size_t)size,file)!=(size_t)size){
    free(text); fclose(file); fprintf(stderr,"cannot read source: %s\n",path); return NULL;
  }
  text[size]='\0';
  fclose(file);
  return text;
}

/* Wide enough for a program whose actor declares more collision targets than any per-object
 * shortcut may quietly cap. */
#define FIXTURE_MAX_OBJECTS 64
#define FIXTURE_MAX_EVENTS 128
#define FIXTURE_MAX_ACTIONS 512
#define FIXTURE_MAX_INSTANCES 96
#define FIXTURE_MAX_TIMELINES 8
#define FIXTURE_MAX_MOMENTS 64
#define FIXTURE_MAX_FONTS 8

typedef struct {
  FixtureProgram program;
  FixtureObject objects[FIXTURE_MAX_OBJECTS];
  FixtureEvent events[FIXTURE_MAX_EVENTS];
  FixtureAction actions[FIXTURE_MAX_ACTIONS];
  int action_count;
  FixtureInstance instances[FIXTURE_MAX_INSTANCES];
  FixtureTimeline timelines[FIXTURE_MAX_TIMELINES];
  FixtureTimelineMoment moments[FIXTURE_MAX_MOMENTS];
  FixtureFont fonts[FIXTURE_MAX_FONTS];
  int event_starts[FIXTURE_MAX_OBJECTS];
  char *owned[FIXTURE_MAX_EVENTS + FIXTURE_MAX_OBJECTS + FIXTURE_MAX_ACTIONS +
              FIXTURE_MAX_TIMELINES + FIXTURE_MAX_MOMENTS + FIXTURE_MAX_FONTS + 2];
  int owned_count;
} ProgramFile;

static void program_free(ProgramFile *p){
  for(int i=0;i<p->owned_count;i++) free(p->owned[i]);
  p->owned_count=0;
}

static char *program_keep(ProgramFile *p, char *text){
  if(!text) return NULL;
  if(p->owned_count>=(int)(sizeof(p->owned)/sizeof(p->owned[0]))){ free(text); return NULL; }
  p->owned[p->owned_count++]=text;
  return text;
}

/* An authored action list exercises the importer, not a hand-lowered imitation
 * of it. Source paths are relative to this list, as event paths are to a program. */
static int program_actions_read(const char *path, ProgramFile *p, FixtureEvent *event){
  FILE *file=fopen(path,"rb");
  if(!file){ fprintf(stderr,"cannot open action list: %s\n",path); return 0; }
  char directory[800],line[512];
  snprintf(directory,sizeof directory,"%s",path);
  char *slash=strrchr(directory,'/');
  if(slash) slash[1]='\0'; else directory[0]='\0';
  int start=p->action_count,ok=1;
  event->actions=&p->actions[start];
  while(ok && fgets(line,sizeof line,file)){
    char keyword[32],source[256],extra[2];
    if(sscanf(line,"%31s",keyword)!=1 || keyword[0]=='#') continue;
    if(p->action_count>=FIXTURE_MAX_ACTIONS){ ok=0; break; }
    FixtureAction *action=&p->actions[p->action_count++];
    action->target=-1;
    int fields=0;
    if(!strcmp(keyword,"code")){
      action->kind=7;
      fields=sscanf(line,"%31s %255s %1s",keyword,source,extra);
      ok=fields==2;
    } else if(!strcmp(keyword,"question")){
      fields=sscanf(line,"%31s %d %u %255s %1s",keyword,&action->target,
                    &action->negate,source,extra);
      ok=fields==4 && action->negate<=1;
    } else {
      if(!strcmp(keyword,"begin")) action->kind=1;
      else if(!strcmp(keyword,"end")) action->kind=2;
      else if(!strcmp(keyword,"else")) action->kind=3;
      else ok=0;
      if(sscanf(line,"%31s %1s",keyword,extra)!=1) ok=0;
    }
    if(ok && fields){
      char full[1100]; snprintf(full,sizeof full,"%s%s",directory,source);
      action->source=program_keep(p,read_source_file(full));
      ok=action->source!=NULL;
    }
    if(!ok) fprintf(stderr,"cannot read action line: %s",line);
  }
  if(ferror(file)) ok=0;
  fclose(file);
  event->action_count=p->action_count-start;
  return ok && event->action_count>0;
}

/* A program file describes the smallest project that can exercise a rule needing instances. It
 * carries no identity of its own: names, sources and placements all come from the caller, so this
 * writer stays a container exercise and the tests that use it live elsewhere.
 *
 *   room <width> <height>
 *   caption <text>|"<text>"      the room's authored caption; quote it to carry outer spaces
 *   sprite <edge> [blank-second-frame]
 *   background <edge> [second-edge]
 *   font <name> <size> <bold:0|1> <italic:0|1> <first> <last>
 *   startup <source.gml>
 *   object <name> <sprite-slot|-1>
 *   object-flags <solid:0|1> <persistent:0|1>   applies to the most recent object
 *   event <type> <number> <source.gml>     applies to the most recent object
 *   event-actions <type> <number> <list>  code/question/begin/end/else action list
 *   instance <object-slot> <x> <y>
 */
static int program_read(const char *path, ProgramFile *p){
  memset(p,0,sizeof(*p));
  p->program.room_width=320; p->program.room_height=240;
  p->program.objects=p->objects; p->program.instances=p->instances;
  p->program.timelines=p->timelines;
  p->program.fonts=p->fonts;
  FILE *file=fopen(path,"rb");
  if(!file){ fprintf(stderr,"cannot open program: %s\n",path); return 0; }
  char line[512];
  int current=-1, events=0, current_timeline=-1, moments=0;
  int ok=1;
  char directory[512];
  snprintf(directory,sizeof(directory),"%s",path);
  char *slash=strrchr(directory,'/');
  if(slash) slash[1]='\0'; else directory[0]='\0';
  while(ok && fgets(line,sizeof(line),file)){
    char keyword[32], a[256], b[64], c[64];
    if(line[0]=='#' || line[0]=='\n' || line[0]=='\r') continue;
    if(sscanf(line,"%31s",keyword)!=1) continue;
    if(!strcmp(keyword,"room")){
      if(sscanf(line,"%31s %d %d",keyword,&p->program.room_width,&p->program.room_height)!=3) ok=0;
    } else if(!strcmp(keyword,"caption")){
      /* Keep the complete line, including optional outer spaces; quotes preserve those spaces in a text fixture. */
      char *text=line+strlen(keyword);
      while(*text==' ' || *text=='\t') text++;
      size_t length=strlen(text);
      while(length && (text[length-1]=='\n' || text[length-1]=='\r')) length--;
      if(length>=2 && text[0]=='"' && text[length-1]=='"'){ text++; length-=2; }
      char *caption=(char*)malloc(length+1);
      if(!caption) ok=0;
      else { memcpy(caption,text,length); caption[length]='\0';
             p->program.room_caption=program_keep(p,caption); ok=p->program.room_caption!=NULL; }
    } else if(!strcmp(keyword,"sprite")){
      p->program.sprite_blank_frame=0;
      if(sscanf(line,"%31s %d %d",keyword,&p->program.sprite_size,
                &p->program.sprite_blank_frame)<2) ok=0;
    } else if(!strcmp(keyword,"background")){
      int fields=sscanf(line,"%31s %255s %63s %63s",keyword,a,b,c);
      p->program.second_background_size=0;
      if(fields<2 || fields>3) ok=0;
      for(int i=0;ok && i<fields-1;i++){
        char *end=NULL; errno=0;
        long edge=strtol(i?b:a,&end,10);
        if(errno || !end || *end || edge<=0 || edge>63) ok=0;
        else if(i) p->program.second_background_size=(int)edge;
        else p->program.background_size=(int)edge;
      }
    } else if(!strcmp(keyword,"font")){
      char args[5][64],extra[2]; int values[5];
      if(p->program.font_count>=FIXTURE_MAX_FONTS ||
         sscanf(line,"%31s %255s %63s %63s %63s %63s %63s %1s",keyword,a,
                args[0],args[1],args[2],args[3],args[4],extra)!=7) ok=0;
      for(int i=0;ok && i<5;i++){
        char *end=NULL; errno=0;
        long value=strtol(args[i],&end,10);
        if(errno || end==args[i] || *end || value<INT_MIN || value>INT_MAX) ok=0;
        else values[i]=(int)value;
      }
      if(ok){
        FixtureFont *font=&p->fonts[p->program.font_count++];
        *font=(FixtureFont){program_keep(p,strdup(a)),values[0],values[1],values[2],values[3],values[4]};
        ok=font->name!=NULL;
      }
    } else if(!strcmp(keyword,"startup")){
      if(sscanf(line,"%31s %255s",keyword,a)!=2){ ok=0; }
      else {
        char full[800]; snprintf(full,sizeof(full),"%s%s",directory,a);
        p->program.startup=program_keep(p,read_source_file(full));
        ok=p->program.startup!=NULL;
      }
    } else if(!strcmp(keyword,"timeline")){
      if(p->program.timeline_count>=FIXTURE_MAX_TIMELINES ||
         sscanf(line,"%31s %255s",keyword,a)!=2) ok=0;
      else {
        current_timeline=p->program.timeline_count++;
        FixtureTimeline *timeline=&p->timelines[current_timeline];
        timeline->name=program_keep(p,strdup(a));
        timeline->moments=&p->moments[moments];
        ok=timeline->name!=NULL;
      }
    } else if(!strcmp(keyword,"moment")){
      if(current_timeline<0 || moments>=FIXTURE_MAX_MOMENTS ||
         sscanf(line,"%31s %63s %255s",keyword,b,a)!=3) ok=0;
      else {
        char *end=NULL; errno=0;
        long step=strtol(b,&end,10);
        if(errno || !end || end==b || *end || step<0 || step>INT_MAX) ok=0;
        else {
          char full[800]; snprintf(full,sizeof(full),"%s%s",directory,a);
          FixtureTimelineMoment *moment=&p->moments[moments];
          moment->step=(int)step;
          moment->source=program_keep(p,read_source_file(full));
          ok=moment->source!=NULL;
          if(ok){ moments++; p->timelines[current_timeline].moment_count++; }
        }
      }
    } else if(!strcmp(keyword,"object")){
      if(p->program.object_count>=FIXTURE_MAX_OBJECTS){ fprintf(stderr,"too many objects\n"); ok=0; }
      else if(sscanf(line,"%31s %255s %63s",keyword,a,b)!=3){ ok=0; }
      else {
        current=p->program.object_count++;
        p->objects[current].name=program_keep(p,strdup(a));
        p->objects[current].sprite=atoi(b);
        p->objects[current].events=&p->events[events];
        p->objects[current].event_count=0;
        p->event_starts[current]=events;
        ok=p->objects[current].name!=NULL;
      }
    } else if(!strcmp(keyword,"object-flags")){
      int solid=0, persistent=0;
      if(current<0 || sscanf(line,"%31s %d %d",keyword,&solid,&persistent)!=3 ||
         (solid!=0 && solid!=1) || (persistent!=0 && persistent!=1)) ok=0;
      else { p->objects[current].solid=solid; p->objects[current].persistent=persistent; }
    } else if(!strcmp(keyword,"event") || !strcmp(keyword,"event-actions")){
      if(current<0){ fprintf(stderr,"event before any object\n"); ok=0; }
      else if(events>=FIXTURE_MAX_EVENTS){ fprintf(stderr,"too many events\n"); ok=0; }
      else if(sscanf(line,"%31s %63s %63s %255s",keyword,b,c,a)!=4){ ok=0; }
      else {
        char full[800]; snprintf(full,sizeof(full),"%s%s",directory,a);
        p->events[events].event_type=atoi(b);
        p->events[events].event_number=atoi(c);
        if(!strcmp(keyword,"event-actions")) ok=program_actions_read(full,p,&p->events[events]);
        else {
          p->events[events].source=program_keep(p,read_source_file(full));
          ok=p->events[events].source!=NULL;
        }
        if(ok){ events++; p->objects[current].event_count=events-p->event_starts[current]; }
      }
    } else if(!strcmp(keyword,"instance")){
      if(p->program.instance_count>=FIXTURE_MAX_INSTANCES){ fprintf(stderr,"too many instances\n"); ok=0; }
      else if(sscanf(line,"%31s %63s %63s %255s",keyword,b,c,a)!=4){ ok=0; }
      else {
        FixtureInstance *in=&p->instances[p->program.instance_count++];
        in->object=atoi(b); in->x=atoi(c); in->y=atoi(a);
      }
    } else {
      fprintf(stderr,"unknown program directive: %s\n",keyword);
      ok=0;
    }
    if(!ok) fprintf(stderr,"cannot read program line: %s",line);
  }
  fclose(file);
  if(!ok) program_free(p);
  return ok;
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
  if(argc==5 && !strcmp(argv[1],"--write-program-fixture")){
    unsigned version=(unsigned)strtoul(argv[2],NULL,10);
    ProgramFile program;
    if(!program_read(argv[4],&program)) return 1;
    Fixture project;
    int built=build_project_fixture_program(version,&program.program,&project);
    program_free(&program);
    if(!built){
      fprintf(stderr,"unsupported program fixture version: %s\n",argv[2]);
      return 1;
    }
    FILE *file=fopen(argv[3],"wb");
    int ok=file && fwrite(project.data,1,project.size,file)==project.size;
    if(file && fclose(file)!=0) ok=0;
    if(!ok){ fprintf(stderr,"cannot write program fixture: %s\n",argv[3]); return 1; }
    printf("wrote program fixture %u: %s (%zu bytes)\n",version,argv[3],project.size);
    return 0;
  }
  if((argc==4 || argc==5) && !strcmp(argv[1],"--write-project-fixture")){
    unsigned version=(unsigned)strtoul(argv[2],NULL,10);
    Fixture project;
    char *source=NULL;
    if(argc==5 && !(source=read_source_file(argv[4]))) return 1;
    int built=build_project_fixture_source(version,source,&project);
    free(source);
    if(!built){
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
    if(!gmlc_classic_manifest_file(NULL,classic_fixture_host(),argv[i],
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
