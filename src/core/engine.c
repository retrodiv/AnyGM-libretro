/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* AnyGM engine coordination. Loads supported content, advances the shared runtime implementation,
 * and publishes software video plus interleaved PCM through the framework-neutral public API. */
#include "anygm.h"
#include "gml_win.h"
#include "gml_vm.h"
#include "gml_builtin.h"
#include "gml_render.h"
#include "gml_audio.h"
#include "gmlc_package.h"
#include "gmlc_classic_project.h"
#include "gmlc_classic_import.h"
#include "gmlc_project.h"
#include "content_router.h"
#include "anygm_compatibility.h"
#include "anygm_host.h"
#include "engine_internal.h"

void engine_logf(AnygmEngine *engine,AnygmLogLevel level,const char *fmt,...){
  char message[2048];
  va_list ap;
  va_start(ap,fmt);
  vsnprintf(message,sizeof message,fmt,ap);
  va_end(ap);
  if(engine->host.log) engine->host.log(engine->host.userdata,level,message);
}

static void engine_errorf(AnygmEngine *engine,AnygmResult result,const char *fmt,...){
  (void)result;
  va_list ap;
  va_start(ap,fmt);
  vsnprintf(engine->last_error,sizeof engine->last_error,fmt,ap);
  va_end(ap);
  engine_logf(engine,ANYGM_LOG_ERROR,"%s",engine->last_error);
}
/* Draw events can mutate runtime state, including consuming RNG. A load-state frame is
 * rendered without a Step so rewind can display it, then the exact state is reapplied after
 * presentation to discard those render-only side effects before simulation resumes. */
static AnygmResult engine_run_frame(AnygmEngine *engine);

static int profile_enabled(AnygmEngine *engine){
  if(engine->profile_enabled < 0) engine->profile_enabled = anygm_host_development_setting(&engine->host,"GML_PROFILE") != NULL;
  return engine->profile_enabled;
}

static int substr_list_match(const char *list, const char *name){
  if(!list || !*list || !name) return 0;
  const char *p=list;
  while(*p){
    while(*p==' ' || *p=='\t' || *p==',' || *p==';') p++;
    const char *s=p;
    while(*p && *p!=',' && *p!=';') p++;
    const char *e=p;
    while(e>s && (e[-1]==' ' || e[-1]=='\t')) e--;
    if(e>s){
      char tok[128];
      size_t n=(size_t)(e-s);
      if(n>=sizeof tok) n=sizeof tok-1;
      memcpy(tok,s,n); tok[n]=0;
      if(strstr(name,tok)) return 1;
    }
  }
  return 0;
}
static double profile_now_ms(AnygmEngine *engine){
  return (double)anygm_host_monotonic_time_ns(engine?&engine->host:NULL)/1000000.0;
}
static void profile_report(AnygmEngine *engine,int force){
  if(!engine->profile.frames || (!force && engine->profile.frames < 300)) return;
  double f = (double)engine->profile.frames;
  const char *fmt = "[profile] frames=%d avg_ms total=%.3f input=%.3f step=%.3f clear=%.3f draw=%.3f gui=%.3f video=%.3f audio=%.3f max=%.2fms@f%ld\n";
  engine_logf(engine,ANYGM_LOG_INFO,fmt,engine->profile.frames,engine->profile.total_ms/f,engine->profile.input_ms/f,
              engine->profile.step_ms/f,engine->profile.clear_ms/f,engine->profile.draw_ms/f,engine->profile.gui_ms/f,
              engine->profile.video_ms/f,engine->profile.audio_ms/f,engine->profile.max_ms,engine->profile.max_frame);
  memset(&engine->profile, 0, sizeof(engine->profile));
}

static void run_selftest(AnygmEngine *engine);
static void setup_platform_locale(AnygmEngine *engine,GmlVM *vm){
  size_t language_size=strnlen(engine->language,sizeof vm->os_language-1);
  size_t region_size=strnlen(engine->region,sizeof vm->os_region-1);
  size_t tag_size=strnlen(engine->language_tag,sizeof vm->language_tag-1);
  memcpy(vm->os_language,engine->language,language_size);
  vm->os_language[language_size]='\0';
  memcpy(vm->os_region,engine->region,region_size);
  vm->os_region[region_size]='\0';
  memcpy(vm->language_tag,engine->language_tag,tag_size);
  vm->language_tag[tag_size]='\0';
}

static void boot_runtime(AnygmEngine *engine);
static AnygmResult engine_apply_game_change(AnygmEngine *engine,int *changed);

typedef struct {
  GmlWin win;
  AnygmContentFacts facts;
  AnygmCompatibilityProfile compatibility;
  char loaded_path[1024];
} EnginePreparedContent;

static AnygmResult engine_prepare_content(AnygmEngine *engine,
                                          const AnygmContentSource *source,
                                          EnginePreparedContent *prepared){
  if(!prepared) return ANYGM_ERROR_INVALID_ARGUMENT;
  memset(prepared,0,sizeof *prepared);
  int path_source=source && source->kind==ANYGM_CONTENT_PATH;
  int memory_source=source && source->kind==ANYGM_CONTENT_MEMORY;
  if((path_source && (!source->path || !source->path[0])) ||
     (memory_source && (!source->data || !source->size)) ||
     (!path_source && !memory_source)){
    engine_errorf(engine,ANYGM_ERROR_INVALID_ARGUMENT,"A readable path or memory image is required");
    return ANYGM_ERROR_INVALID_ARGUMENT;
  }
  int load_rc=0;
  int classic_input=0;
  if(path_source){
    char content[1024];
    size_t plen=strlen(source->path);
    classic_input=
        (plen>4 && !strcasecmp(source->path+plen-4,".gmk")) ||
        (plen>5 && !strcasecmp(source->path+plen-5,".gm81")) ||
        (plen>4 && !strcasecmp(source->path+plen-4,".gm6")) ||
        (plen>4 && !strcasecmp(source->path+plen-4,".exe"));
    AnygmContentRouter router={0};
    router.host=&engine->host;
    router.cache_directory=source->cache_directory;
    router.log=content_router_log;
    router.log_userdata=engine;
    if(!anygm_content_resolve_path(&router,source->path,content,sizeof content)){
      engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,"Failed to resolve content path: %s",source->path);
      return ANYGM_ERROR_INVALID_CONTENT;
    }
    load_rc=anygm_content_load_win(&router,&prepared->win,content,
                                   prepared->loaded_path,sizeof prepared->loaded_path);
    if(!load_rc){
      engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,"Failed to load content: %s",content);
      return ANYGM_ERROR_INVALID_CONTENT;
    }
    if(load_rc==2)
      engine_logf(engine,ANYGM_LOG_WARN,
                  "The selected payload has no executable code; using sibling payload: %s\n",
                  prepared->loaded_path);
    if(classic_input)
      anygm_content_path_parent(source->path,prepared->win.content_dir,
                                sizeof prepared->win.content_dir);
  } else {
    if(gml_win_from_mem(&prepared->win,(uint8_t *)(uintptr_t)source->data,
                        source->size,0)!=0){
      engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,
                    "The memory source is not a supported normalized content image");
      return ANYGM_ERROR_INVALID_CONTENT;
    }
    prepared->win.host=&engine->host;
    snprintf(prepared->loaded_path,sizeof prepared->loaded_path,"%s",
             source->path&&source->path[0]?source->path:"memory image");
    if(source->path&&source->path[0])
      anygm_content_path_parent(source->path,prepared->win.content_dir,
                                sizeof prepared->win.content_dir);
  }
  /* Give each content identity a stable writable namespace under the host-provided root. */
  {
    const char *base=source->save_directory;
    if(base&&base[0]){
      const char *identity=source->path&&source->path[0]?source->path:NULL;
      char label[128];
      if(identity) anygm_content_save_label(identity,label,sizeof label);
      else snprintf(label,sizeof label,"memory");
      uint32_t namespace_hash=identity?anygm_content_path_hash(identity):
        (uint32_t)state_hash_bytes(source->data,source->size);
      snprintf(prepared->win.save_dir,sizeof prepared->win.save_dir,"%s/anygm/%s-%08x",
               base,label,namespace_hash);
      /* Opt-in fresh start: drop everything this content generated before it can read any of it,
       * so the run that follows behaves like the first one this machine ever performed. */
      if(engine->config.clear_local_data){
        int cleared=anygm_content_directory_remove(&engine->host,prepared->win.save_dir);
        engine_logf(engine,cleared?ANYGM_LOG_INFO:ANYGM_LOG_WARN,
                    cleared?"local data cleared: %s\n":"could not clear local data: %s\n",
                    prepared->win.save_dir);
      }
      anygm_content_directory_create(&engine->host,prepared->win.save_dir);
    } else {
      snprintf(prepared->win.save_dir,sizeof prepared->win.save_dir,"%s",
               prepared->win.content_dir);
    }
  }
  if(prepared->win.n_code<=0){
    engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,"The payload has no executable code: %s",
                  prepared->loaded_path);
    gml_win_free(&prepared->win);
    return ANYGM_ERROR_INVALID_CONTENT;
  }
  char compatibility_error[256]={0};
  if(!anygm_content_facts_detect(&prepared->win,&prepared->facts,compatibility_error,
                                 sizeof compatibility_error) ||
     !anygm_compatibility_resolve(&prepared->facts,&prepared->compatibility,
                                  compatibility_error,sizeof compatibility_error)){
    engine_errorf(engine,ANYGM_ERROR_UNSUPPORTED,"Unsupported content semantics: %s",
                  compatibility_error[0]?compatibility_error:"unknown compatibility facts");
    gml_win_free(&prepared->win);
    return ANYGM_ERROR_UNSUPPORTED;
  }
  return ANYGM_OK;
}

static AnygmResult engine_load_content(AnygmEngine *engine,const AnygmContentSource *source,
                                       const AnygmLoadConfig *config) {
  engine->state_just_loaded = 0;
  snprintf(engine->language,sizeof engine->language,"%s",config&&config->language&&config->language[0]?config->language:"en");
  snprintf(engine->region,sizeof engine->region,"%s",config&&config->region&&config->region[0]?config->region:"us");
  snprintf(engine->language_tag,sizeof engine->language_tag,"%s",config&&config->language_tag&&config->language_tag[0]?config->language_tag:"en-US");
  EnginePreparedContent prepared;
  AnygmResult result=engine_prepare_content(engine,source,&prepared);
  if(result!=ANYGM_OK) return result;
  engine->win=prepared.win;
  engine->content_facts=prepared.facts;
  engine->compatibility=prepared.compatibility;
  engine->win.compatibility=&engine->compatibility;
  snprintf(engine->content_cache_directory,sizeof engine->content_cache_directory,"%s",
           source->cache_directory?source->cache_directory:"");
  snprintf(engine->current_content_path,sizeof engine->current_content_path,"%s",
           prepared.loaded_path);
  snprintf(engine->content_program_directory,sizeof engine->content_program_directory,"%s",
           engine->win.content_dir);
  engine->launch_parameters[0]='\0';
  state_identity_refresh(engine);
  engine->loaded = 1;
  engine_logf(engine,ANYGM_LOG_INFO,"Loaded content: bytecode=%u rooms=%d code=%d\n",
              engine->win.bytecode,gml_room_count(&engine->win),engine->win.n_code);
  engine->full_game_on_initial_boot=0;
  boot_runtime(engine);
  run_selftest(engine);
  GmlRenderResourceMetrics render_resources;
  gml_render_resource_metrics(&engine->render,&render_resources);
  engine_logf(engine,ANYGM_LOG_INFO,"Runtime booted: atlases=%d sprites=%d texture-pages=%d\n",
              render_resources.atlas_count,render_resources.sprite_count,
              render_resources.texture_page_count);
  return ANYGM_OK;
}
/* Cold-boot the runtime from the already-loaded data.win: fresh VM/render/audio + the same
 * configured start-room logic as first load. */
static void boot_runtime(AnygmEngine *engine) {
  /* A reset is a cold boot. Keep engine time on the same timeline as an initial load. */
  { engine->vm.frame = 0; }
  engine->state_reapply_size = 0;
  memset(engine->pad_current, 0, sizeof(engine->pad_current));
  memset(engine->pad_previous, 0, sizeof(engine->pad_previous));
  memset(engine->axis_current, 0, sizeof(engine->axis_current));
  memset(engine->axis_previous, 0, sizeof(engine->axis_previous));
  memset(engine->key_current, 0, sizeof(engine->key_current));
  memset(engine->key_previous, 0, sizeof(engine->key_previous));
  memset(engine->hardware_key_current, 0, sizeof(engine->hardware_key_current));
  memset(engine->hardware_key_previous, 0, sizeof(engine->hardware_key_previous));
  memset(engine->event_vk_current, 0, sizeof(engine->event_vk_current));
  memset(engine->event_vk_previous, 0, sizeof(engine->event_vk_previous));
  memset(engine->event_key_current, 0, sizeof(engine->event_key_current));
  memset(engine->event_key_previous, 0, sizeof(engine->event_key_previous));
  engine->mouse_pixel_x = -1; engine->mouse_pixel_y = -1;
  engine->pointer_active = 0;
  memset(engine->mouse_button_current, 0, sizeof(engine->mouse_button_current));
  memset(engine->mouse_button_previous, 0, sizeof(engine->mouse_button_previous));
  engine->mouse_wheel = 0;
  engine->present_mouse_valid = 0;
  engine->follow_player = 0; engine->player_object = -1; engine->audio_accumulator = 0.0; engine->fps = 60.0; engine->fps_room = -1;
  engine->state_just_loaded = 0;
  engine->runtime_ended = 0;
  engine->shutdown_sent = 0;
  classic_transition_reset(engine);
  engine->have_presented_frame = 0;
  setup_display(engine);
  gml_vm_init_launch(&engine->vm,&engine->win,&engine->host,
                     engine->content_program_directory,engine->current_content_path,
                     engine->launch_parameters);
  engine_input_bind(engine);
  setup_platform_locale(engine,&engine->vm);
  gml_render_init(&engine->render, &engine->win);
  /* application_surface exists before the first Create event. GML may resize it there; the
   * renderer promotes the borrowed framebuffer to an independently owned surface when that
   * happens. */
  gml_render_application_surface_bind(
    &engine->render,engine->fb,(int)engine->width,(int)engine->height,0);
  /* First-generation presentation keeps the default application surface at the exported
   * display raster even when a room uses a smaller logical view. Own that stable raster so
   * later room-size changes do not collapse surface 0 to the camera dimensions. */
  if(anygm_policy_uses_first_generation_studio(&engine->win) &&
     !gml_render_application_surface_ensure_owned(
       &engine->render,(int)engine->width,(int)engine->height))
    engine_logf(engine,ANYGM_LOG_WARN,
      "[anygm] could not allocate the first-generation application surface\n");
  GmlRenderControl render_control={
    .requested_width=core_opt_resolution(engine,0),
    .requested_height=core_opt_resolution(engine,1),
    .crt_shader_enabled=core_opt_embedded_shaders(engine),
    .crt_mask_enabled=core_opt_crt_mask(engine),
    .crt_scanlines_enabled=core_opt_onoff(engine,"anygm_crt_scanlines", "ANYGM_CRT_SCANLINES", 1),
    .crt_gamma_enabled=core_opt_onoff(engine,"anygm_crt_gamma", "ANYGM_CRT_GAMMA", 1),
    .crt_curvature=core_opt_crt_tristate(engine,"anygm_crt_curvature", "ANYGM_CRT_CURVATURE"),
    .crt_vignette=core_opt_crt_tristate(engine,"anygm_crt_vignette", "ANYGM_CRT_VIGNETTE")
  };
  gml_render_control_update(&engine->render,&render_control,GML_RENDER_CONTROL_HOST_OPTIONS);
  engine->vm.render = &engine->render;
  (void)gml_vm_software3d_ensure(&engine->vm);
  engine->vm.draw_event_hook = aspect_draw_event_hook;
  engine->vm.draw_event_hook_user = engine;
  engine->audio = gml_audio_create(&engine->win);
  engine->vm.audio = engine->audio;
  /* Boot the normal entry point unless the host supplied a neutral start-room override. */
  int start_order=0,selected_room=-1,spawn=0;
  double spawn_x=64.0,spawn_y=100.0;
  char spawn_object[128]={0};
  const char *spawn_setting=anygm_host_development_setting(&engine->host,"GML_SPAWN_OBJ");
  if(spawn_setting && spawn_setting[0]){
    spawn=1;
    const char *colon=strchr(spawn_setting,':');
    size_t length=colon?(size_t)(colon-spawn_setting):strlen(spawn_setting);
    if(length>=sizeof spawn_object) length=sizeof spawn_object-1;
    memcpy(spawn_object,spawn_setting,length);
    if(colon){
      const char *comma=strchr(colon+1,',');
      if(comma){ spawn_x=atof(colon+1); spawn_y=atof(comma+1); }
    }
  }else{
    const char *position=anygm_host_development_setting(&engine->host,"GML_SPAWN_PLAYER");
    const char *object=anygm_host_development_setting(&engine->host,"GML_SPAWN_PLAYER_OBJ");
    if(position && object && object[0]){
      spawn=1;
      snprintf(spawn_object,sizeof spawn_object,"%s",object);
      const char *comma=strchr(position,',');
      if(comma){ spawn_x=atof(position); spawn_y=atof(comma+1); }
    }
  }
  int initial_boot_guard = engine->full_game_on_initial_boot;
  if(!initial_boot_guard) core_opt_start_room(engine,&selected_room);
  engine->full_game_on_initial_boot = 0;
  /* always run the first room before a selected start room so game globals, fonts, and
   * input-controller instances are initialized. */
  if (start_order != 0 || selected_room >= 0) gml_vm_goto_room_order(&engine->vm, 0);
  if (selected_room >= 0) gml_room_enter(&engine->vm, selected_room);
  else gml_vm_goto_room_order(&engine->vm, start_order);
  sync_room_fps(engine,0);
  engine->vm.god_mode = core_opt_god(engine);
  if(spawn && spawn_object[0]){
    engine->player_object=gml_object_index_by_name(&engine->vm,spawn_object);
    const char *suppress=anygm_host_development_setting(&engine->host,"GML_SPAWN_SUPPRESS");
    if(suppress && suppress[0]) for(int i=0;i<engine->vm.inst_count;i++){
      GmlInstance *instance=&engine->vm.inst[i];
      if(!instance->active || instance->obj<0) continue;
      if(substr_list_match(suppress,engine->vm.objects[instance->obj].name)) instance->active=0;
    }
    if(engine->player_object>=0){
      GmlInstance *player=NULL;
      for(int i=0;i<engine->vm.inst_count;i++){
        GmlInstance *instance=&engine->vm.inst[i];
        if(!instance->active || instance->marked || instance->obj<0 ||
           !gml_object_is(&engine->vm,instance->obj,engine->player_object)) continue;
        if(!player) player=instance;
        else instance->active=0;
      }
      if(!player) player=gml_instance_create(&engine->vm,spawn_x,spawn_y,engine->player_object);
      if(player){
        player->x=player->xprevious=player->xstart=spawn_x;
        player->y=player->yprevious=player->ystart=spawn_y;
        player->hspeed=player->vspeed=player->speed=0.0;
      }
      engine->follow_player=1;
    }
    engine_logf(engine,ANYGM_LOG_INFO,
                "Development spawn configured object %d at %.3f,%.3f\n",
                engine->player_object,spawn_x,spawn_y);
  }
  engine->background = cur_room_bg(engine);
}

static int game_change_next_argument(const char **cursor,char *output,size_t capacity){
  if(!cursor || !*cursor || !output || !capacity) return 0;
  const char *source=*cursor;
  while(isspace((unsigned char)*source)) source++;
  if(!*source){ output[0]='\0'; *cursor=source; return 0; }
  size_t written=0;
  int quote=0;
  while(*source){
    unsigned char c=(unsigned char)*source;
    if(!quote && isspace(c)) break;
    source++;
    if(c=='\'' || c=='"'){
      if(!quote){ quote=c; continue; }
      if(quote==c){ quote=0; continue; }
    }
    if(c=='\\' && *source && ((quote && *source==quote) || *source=='\\'))
      c=(unsigned char)*source++;
    if(written+1>=capacity){ output[0]='\0'; return -1; }
    output[written++]=(char)c;
  }
  if(quote){ output[0]='\0'; return -1; }
  output[written]='\0';
  while(isspace((unsigned char)*source)) source++;
  *cursor=source;
  return 1;
}

static int game_change_payload_name(const char *parameters,char *output,size_t capacity){
  const char *cursor=parameters?parameters:"";
  char token[GML_GAME_CHANGE_TEXT_MAX];
  int found=0;
  while(*cursor){
    int parsed=game_change_next_argument(&cursor,token,sizeof token);
    if(parsed<0) return 0;
    if(!parsed) break;
    if(!strcasecmp(token,"-game")){
      parsed=game_change_next_argument(&cursor,output,capacity);
      if(parsed<=0 || !output[0]) return 0;
      found=1;
      break;
    }
  }
  if(!found) snprintf(output,capacity,"data.win");
  return output[0]!='\0';
}

static int game_change_target_path(const char *content_directory,const char *directory,
                                   const char *payload,char *output,size_t capacity){
  if(!content_directory || !content_directory[0] || !payload || !payload[0] ||
     !output || !capacity) return 0;
  while(*directory=='/' || *directory=='\\') directory++;
  while(*payload=='/' || *payload=='\\') payload++;
  if(!payload[0]) return 0;
  for(const char *path=directory;path&&*path;){
    while(*path=='/' || *path=='\\') path++;
    const char *segment=path;
    while(*path && *path!='/' && *path!='\\'){
      if((unsigned char)*path<0x20 || *path==':') return 0;
      path++;
    }
    size_t size=(size_t)(path-segment);
    if((size==1 && segment[0]=='.') ||
       (size==2 && segment[0]=='.' && segment[1]=='.')) return 0;
  }
  for(const char *path=payload;*path;){
    while(*path=='/' || *path=='\\') path++;
    const char *segment=path;
    while(*path && *path!='/' && *path!='\\'){
      if((unsigned char)*path<0x20 || *path==':') return 0;
      path++;
    }
    size_t size=(size_t)(path-segment);
    if((size==1 && segment[0]=='.') ||
       (size==2 && segment[0]=='.' && segment[1]=='.')) return 0;
  }
  int length=directory[0]
    ?snprintf(output,capacity,"%s/%s/%s",content_directory,directory,payload)
    :snprintf(output,capacity,"%s/%s",content_directory,payload);
  return length>=0 && (size_t)length<capacity;
}

static AnygmResult engine_apply_game_change(AnygmEngine *engine,int *changed){
  if(changed) *changed=0;
  if(!engine || !engine->vm.game_change_pending) return ANYGM_OK;
  int valid=engine->vm.game_change_pending>0;
  char directory[GML_GAME_CHANGE_TEXT_MAX];
  char parameters[GML_GAME_CHANGE_TEXT_MAX];
  snprintf(directory,sizeof directory,"%s",engine->vm.game_change_directory);
  snprintf(parameters,sizeof parameters,"%s",engine->vm.game_change_parameters);
  engine->vm.game_change_pending=0;
  engine->vm.game_change_directory[0]='\0';
  engine->vm.game_change_parameters[0]='\0';
  if(!valid){
    engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,
                  "The requested game change path or parameter list is too long");
    return ANYGM_ERROR_INVALID_CONTENT;
  }
  char payload[GML_GAME_CHANGE_TEXT_MAX];
  char target[2048];
  if(!game_change_payload_name(parameters,payload,sizeof payload) ||
     !game_change_target_path(engine->win.content_dir,directory,payload,target,sizeof target)){
    engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,
                  "The requested game change path could not be resolved");
    return ANYGM_ERROR_INVALID_CONTENT;
  }
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=target;
  source.cache_directory=engine->content_cache_directory[0]
    ?engine->content_cache_directory:NULL;
  EnginePreparedContent prepared;
  AnygmResult result=engine_prepare_content(engine,&source,&prepared);
  if(result!=ANYGM_OK) return result;

  char save_directory[sizeof engine->win.save_dir];
  snprintf(save_directory,sizeof save_directory,"%s",engine->win.save_dir);
  profile_report(engine,1);
  gml_vm_fire_game_end(&engine->vm);
  gml_audio_free(engine->audio); engine->audio=NULL; engine->vm.audio=NULL;
  gml_vm_free(&engine->vm);
  gml_render_free(&engine->render);
  gml_win_free(&engine->win);

  engine->win=prepared.win;
  engine->content_facts=prepared.facts;
  engine->compatibility=prepared.compatibility;
  engine->win.compatibility=&engine->compatibility;
  snprintf(engine->win.save_dir,sizeof engine->win.save_dir,"%s",save_directory);
  snprintf(engine->current_content_path,sizeof engine->current_content_path,"%s",
           prepared.loaded_path);
  snprintf(engine->launch_parameters,sizeof engine->launch_parameters,"%s",parameters);
  state_identity_refresh(engine);
  engine_logf(engine,ANYGM_LOG_INFO,"Changed content: bytecode=%u rooms=%d code=%d\n",
              engine->win.bytecode,gml_room_count(&engine->win),engine->win.n_code);
  engine->full_game_on_initial_boot=1;
  boot_runtime(engine);
  run_selftest(engine);
  GmlRenderResourceMetrics render_resources;
  gml_render_resource_metrics(&engine->render,&render_resources);
  engine_logf(engine,ANYGM_LOG_INFO,
              "Runtime booted: atlases=%d sprites=%d texture-pages=%d\n",
              render_resources.atlas_count,render_resources.sprite_count,
              render_resources.texture_page_count);
  if(changed) *changed=1;
  return ANYGM_OK;
}

static AnygmResult engine_apply_game_change_and_run_frame(AnygmEngine *engine){
  if(engine->game_change_depth>=8){
    engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,
                  "The game change chain exceeded the supported nesting limit");
    return ANYGM_ERROR_INVALID_CONTENT;
  }
  int changed=0;
  AnygmResult result=engine_apply_game_change(engine,&changed);
  if(result!=ANYGM_OK || !changed) return result;
  engine->game_change_depth++;
  result=engine_run_frame(engine);
  engine->game_change_depth--;
  return result;
}

static void engine_unload(AnygmEngine *engine){
  if(!engine->loaded) return;
  profile_report(engine,1);
  gml_audio_free(engine->audio); engine->audio=NULL; engine->vm.audio=NULL;
  gml_vm_free(&engine->vm);
  gml_render_free(&engine->render);
  gml_win_free(&engine->win);
  classic_transition_release(engine);
  engine->have_presented_frame=0; engine->audio_accumulator=0.0; engine->fps=60.0; engine->fps_room=-1;
  engine->state_just_loaded=0; engine->state_reapply_size=0;
  engine->content_fingerprint=0; engine->compatibility_fingerprint=0;
  engine->runtime_ended=0; engine->shutdown_sent=0; engine->loaded=0;
}

/* Apply the current neutral configuration before the frame. */
static void poll_option_updates(AnygmEngine *engine) {
  GmlRenderControl control={
    .requested_width=core_opt_resolution(engine,0),
    .requested_height=core_opt_resolution(engine,1),
    .crt_shader_enabled=core_opt_embedded_shaders(engine),
    .crt_mask_enabled=core_opt_crt_mask(engine),
    .crt_scanlines_enabled=core_opt_onoff(engine,"anygm_crt_scanlines", "ANYGM_CRT_SCANLINES", 1),
    .crt_gamma_enabled=core_opt_onoff(engine,"anygm_crt_gamma", "ANYGM_CRT_GAMMA", 1),
    .crt_curvature=core_opt_crt_tristate(engine,"anygm_crt_curvature", "ANYGM_CRT_CURVATURE"),
    .crt_vignette=core_opt_crt_tristate(engine,"anygm_crt_vignette", "ANYGM_CRT_VIGNETTE")
  };
  gml_render_control_update(&engine->render,&control,GML_RENDER_CONTROL_HOST_OPTIONS);
}
/* Fast-forward is an optimization hint only. */
static void poll_fast_forward(AnygmEngine *engine) {
  GmlRenderControl control={.fast_forward=engine->config.fast_forward?1:0};
  gml_render_control_update(&engine->render,&control,GML_RENDER_CONTROL_FAST_FORWARD);
}
static AnygmResult engine_run_frame(AnygmEngine *engine) {
  if(engine->vm.game_change_pending)
    return engine_apply_game_change_and_run_frame(engine);
  engine->audio_frames=0;
  /* Keep supplying the last completed picture and silence after the shutdown request without
   * advancing Step, Draw, particles, or animation. */
  if(engine->runtime_ended){
    memset(engine->audio_output,0,sizeof engine->audio_output);
    engine->audio_accumulator+=44100.0/(engine->fps>0.0?engine->fps:60.0);
    int nf=(int)engine->audio_accumulator; engine->audio_accumulator-=nf;
    if(nf>4096) nf=4096;
    if(nf>0) engine->audio_frames=(size_t)nf;
    engine->frame_flags|=ANYGM_FRAME_SHUTDOWN_REQUESTED;
    return ANYGM_OK;
  }
  /* Hosts apply live presentation changes through anygm_set_config. Keeping option resolution out
   * of the frame loop makes the framework seam a cold control path rather than recurring work. */
  poll_fast_forward(engine);     /* optimization hint; never changes presentation state */
  int prof = profile_enabled(engine);
  double t_total = prof ? profile_now_ms(engine) : 0.0;
  double sp_step = engine->profile.step_ms, sp_draw = engine->profile.draw_ms, sp_gui = engine->profile.gui_ms, sp_video = engine->profile.video_ms;
  double t0 = t_total, t1 = t_total;
  /* Snapshot neutral input, retaining the previous frame for edge detection. */
  memcpy(engine->pad_previous, engine->pad_current, sizeof(engine->pad_current));
  memcpy(engine->key_previous, engine->key_current, sizeof(engine->key_current));
  memcpy(engine->axis_previous, engine->axis_current, sizeof(engine->axis_current));
  for (int b = 0; b < NPAD; b++)
    engine->pad_current[b]=engine->input.gamepad_buttons[0][b]?1:0;
  engine_input_poll_keyboard(engine);
  for(int axis=0;axis<4;axis++) engine->axis_current[axis]=engine->input.gamepad_axes[0][axis];
  engine_input_poll_mouse(engine);
  int room_before_step = engine->vm.room_index;
  unsigned transition_old_w = engine->output_width ? engine->output_width : engine->width;
  unsigned transition_old_h = engine->output_height ? engine->output_height : engine->height;
  int state_restore_frame = engine->state_just_loaded;
  int run_step = !state_restore_frame && !engine->classic_transition.active;
  if(state_restore_frame){
    memset(engine->pad_current, 0, sizeof(engine->pad_current));
    memset(engine->pad_previous, 0, sizeof(engine->pad_previous));
    memset(engine->key_current, 0, sizeof(engine->key_current));
    memset(engine->key_previous, 0, sizeof(engine->key_previous));
    memset(engine->hardware_key_current, 0, sizeof(engine->hardware_key_current));
    memset(engine->hardware_key_previous, 0, sizeof(engine->hardware_key_previous));
    memset(engine->event_vk_current, 0, sizeof(engine->event_vk_current));
    memset(engine->event_vk_previous, 0, sizeof(engine->event_vk_previous));
    memset(engine->event_key_current, 0, sizeof(engine->event_key_current));
    memset(engine->event_key_previous, 0, sizeof(engine->event_key_previous));
    memset(engine->axis_current, 0, sizeof(engine->axis_current));
    memset(engine->axis_previous, 0, sizeof(engine->axis_previous));
    memset(engine->mouse_button_current, 0, sizeof(engine->mouse_button_current));
    memset(engine->mouse_button_previous, 0, sizeof(engine->mouse_button_previous));
    engine->mouse_wheel = 0;
  }
  if (anygm_host_development_setting(&engine->host,"GML_DBG_PAD")) { engine->diagnostics.pad_frame++;
    unsigned bits = 0; for (int b = 0; b < NPAD; b++) if (engine->pad_current[b]) bits |= 1u << b;
    if (bits) engine_logf(engine,ANYGM_LOG_DEBUG, "[pad] f%d bits=%04x\n", engine->diagnostics.pad_frame, bits); }
  if(prof){ t1 = profile_now_ms(engine); engine->profile.input_ms += t1 - t0; t0 = t1; }
  engine->state_just_loaded = 0;
  sync_room_fps(engine,1);
  aspect_apply_program(engine);
  sync_room_fps(engine,1);
  /* For a full-width compositor under a forced-wide aspect, report widened window dimensions so
   * its CRT surface spans the whole frame. Set before
   * the step so the window-size-change trigger fires with the (already-widened) view in scope. */
  { int fw = engine->aspect_force_active && aspect_compositor_fullwidth_gen(engine)
             && ((engine->base_width > 0 && (int)engine->base_width < (int)engine->width) || (engine->base_height > 0 && (int)engine->base_height < (int)engine->height));
    GmlRenderControl control={
      .wide_aspect_active=fw,
      .wide_width=fw ? (int)engine->width : 0,
      .wide_height=fw ? (int)engine->height : 0
    };
    gml_render_control_update(&engine->render,&control,GML_RENDER_CONTROL_WIDE_ASPECT); }
  if(run_step){
    AspectViewOverlay step_ov;
    aspect_view_overlay_begin(engine,&step_ov, 0, ASPECT_VIEW_TRACKING);
    gml_vm_step(&engine->vm);
    if(anygm_policy_uses_classic_runtime(&engine->win) && room_before_step>=0 && engine->vm.room_index!=room_before_step){
      int kind=(int)lround(gml_global_num(&engine->vm,"transition_kind"));
      int steps=(int)lround(gml_global_num(&engine->vm,"transition_steps"));
      if(kind!=21 || !classic_transition_start(engine,transition_old_w,transition_old_h,steps))
        gml_set_global_scalar(&engine->vm,"transition_kind",0);
    }
    aspect_view_overlay_end(engine,&step_ov, 1);
    sync_room_fps(engine,1);
    apply_sticky_cheats(engine);   /* generic freeze cheats (user-supplied global writes) */
    menu_run(engine);              /* generic pause-menu editor (inject entries + handle input) */
    room_skip_hook(engine);        /* generic room-skip button (Select/Start, any room) */
    introskip_hook(engine);        /* A/B skip, only in a user-supplied intro-room list (GML_INTROSKIP) */
  }
  if(engine->vm.game_change_pending)
    return engine_apply_game_change_and_run_frame(engine);
  /* The VM has already fired the final event; translate its lifecycle request to engine output. */
  if(engine->vm.game_end){
    if(engine->vm.game_end == 2){
      engine->vm.game_end = 0;
      gml_audio_free(engine->audio); engine->audio=NULL; engine->vm.audio=NULL;
      gml_vm_free(&engine->vm);
      gml_render_free(&engine->render);
      boot_runtime(engine);
    } else {
      engine->runtime_ended = 1;
      if(!engine->shutdown_sent){
        engine->shutdown_sent = 1;
        engine->frame_flags|=ANYGM_FRAME_SHUTDOWN_REQUESTED;
      }
    }
  }
  if (anygm_host_development_setting(&engine->host,"GML_LOG_PLAYER")) {
    const char *log_obj = anygm_host_development_setting(&engine->host,"GML_LOG_PLAYER");
    int po = engine->player_object;
    if (log_obj && *log_obj && strcmp(log_obj, "1")) po = gml_object_index_by_name(&engine->vm, log_obj);
    GmlInstance *p = po >= 0 ? gml_find_instance(&engine->vm, po) : NULL;
    if (p) engine_logf(engine,ANYGM_LOG_DEBUG, "[player] f%d x=%.1f y=%.1f hsp=%.2f vsp=%.2f img=%.1f spr=%d\n",
                   engine->diagnostics.player_frame, p->x, p->y, p->hspeed, p->vspeed, p->image_index, (int)p->sprite_index);
    engine->diagnostics.player_frame++;
  }
  {
    const char *target = anygm_host_development_setting(&engine->host,"GML_LOG_RNG_FRAME");
    if (target && atoi(target) == engine->diagnostics.rng_frame)
      engine_logf(engine,ANYGM_LOG_DEBUG, "[rng] seed=%u\n", anygm_policy_uses_classic_runtime(engine->vm.win)
        ? engine->vm.rng_classic_state : engine->vm.rng_state);
    engine->diagnostics.rng_frame++;
  }
  {
    const char *needle = anygm_host_development_setting(&engine->host,"GML_LOG_OBJ");
    if (needle && *needle) {
      for (int i = 0; i < engine->vm.inst_count; i++) {
        GmlInstance *o = &engine->vm.inst[i];
        if (!o->active || o->marked || o->obj < 0 || o->obj >= engine->vm.n_objects) continue;
        const char *on = engine->vm.objects[o->obj].name;
        if (on && (!strcmp(needle, "*") || strstr(on, needle))) {
          char dn[160];
          snprintf(dn, sizeof dn, "gml_Object_%s_Draw_0", on);
          engine_logf(engine,ANYGM_LOG_DEBUG, "[obj] f%d %s obj=%d id=%u x=%.1f y=%.1f hsp=%.2f vsp=%.2f dir=%.1f img=%.1f spr=%d solid=%.0f vis=%.0f draw=%d xs=%.1f al=[%.0f %.0f]\n",
                  engine->diagnostics.object_frame, on, o->obj, o->id, o->x, o->y, o->hspeed, o->vspeed, o->direction, o->image_index,
                  (int)o->sprite_index, o->solid, o->visible, gml_code_index_by_name(&engine->win, dn) >= 0,
                  o->image_xscale, o->alarm[0], o->alarm[1]);
        }
      }
      engine->diagnostics.object_frame++;
    }
  }
  if(prof){ t1 = profile_now_ms(engine); engine->profile.step_ms += t1 - t0; t0 = t1; }
  AspectViewOverlay draw_ov;
  if (!engine->follow_player) aspect_view_overlay_begin(engine,&draw_ov, 1, ASPECT_VIEW_FORCED);
  else memset(&draw_ov, 0, sizeof(draw_ov));
  /* Pre-Draw is a screen-stage event which runs once before the visible frame is snapshotted
   * viewports. It may deliberately change view visibility or camera state for the regular draw
   * phases, so it cannot live inside the per-view loop. */
  if(gml_vm_draw_pass_active(&engine->vm,"Draw_76")){
    if(!engine->output_width || !engine->output_height) compute_present(engine);
    gml_render_begin(&engine->render,engine->screen,
                     (int)engine->output_width,(int)engine->output_height,0.0,0.0);
    gml_render_set_pending_fill(&engine->render,0);
    gml_vm_draw_pass(&engine->vm,"Draw_76");
    gml_render_flush_pending_fill(&engine->render);
    sync_room_fps(engine,1);
  }
  GmlPresentView frame_views[8];
  int frame_view_count=present_view_count(engine,frame_views,NULL,NULL);
  int frame_view_index=frame_view_count>0?frame_views[0].index:0;
  engine->background = cur_room_bg(engine);
  double cam_x, cam_y;
  if (engine->follow_player) {
    GmlInstance *pl = gml_find_instance(&engine->vm, engine->player_object);
    GmlRoom rmc; int hr = (gml_vm_room_get(&engine->vm, engine->vm.room_index, &rmc) == 0);
    if (pl && hr) {
      cam_x = pl->x - engine->width / 2.0; cam_y = pl->y - engine->height / 2.0;
      clamp_camera_to_current_room(engine,&cam_x, &cam_y, engine->aspect_force_active);
    } else { cam_x = cam_y = 0; }
  } else {
    if (draw_ov.active) {
      cam_x = draw_ov.render_x;
      cam_y = draw_ov.render_y;
    } else {
      /* Studio cameras are opaque handles bound through view_camera[].  Their world rectangle
       * lives in the camera resource, while the legacy view_xview/yview arrays may remain at the
       * room defaults forever.  The multiview compositor already resolves this indirection; the
       * common single-view path must use the same resolved rectangle or every world instance is
       * rendered relative to (0,0) even though camera_get_view_x/y report the moving camera. */
      if (frame_view_count > 0) {
        cam_x = frame_views[0].x;
        cam_y = frame_views[0].y;
      } else {
        cam_x = gml_global_arr(&engine->vm, "view_xview", 0);
        cam_y = gml_global_arr(&engine->vm, "view_yview", 0);
      }
    }
    if (!draw_ov.active && engine->aspect_force_active)
      aspect_forced_camera(engine,cam_x, cam_y, &cam_x, &cam_y);
  }
  if (anygm_host_development_setting(&engine->host,"GML_LOG_CAM")) {   /* aligns with the host dump frame index */
    int bound = (int)lround(gml_global_arr(&engine->vm, "view_camera", 0));
    int bound_live = bound >= 0 && bound < GML_CAMERA_LIMIT &&
                     gml_global_arr(&engine->vm, "__gml_camera_live", bound) >= 0.5;
    engine_logf(engine,ANYGM_LOG_DEBUG, "[cam] %d %d %d %d base=%ux%u forced=%ux%u delta=%.1f,%.1f view0=%.0f "
                    "bound=%d live=%d resource=(%.0f,%.0f %.0fx%.0f) port=(%.0f,%.0f)\n",
            engine->diagnostics.camera_frame++, engine->vm.room_index, (int)cam_x, (int)cam_y,
            engine->base_width, engine->base_height, engine->width, engine->height, engine->aspect_cam_dx, engine->aspect_cam_dy,
            gml_global_arr(&engine->vm, "view_visible", 0), bound, bound_live,
            bound_live ? gml_global_arr(&engine->vm, "__gml_camera_x", bound) : 0.0,
            bound_live ? gml_global_arr(&engine->vm, "__gml_camera_y", bound) : 0.0,
            bound_live ? gml_global_arr(&engine->vm, "__gml_camera_w", bound) : 0.0,
            bound_live ? gml_global_arr(&engine->vm, "__gml_camera_h", bound) : 0.0,
            gml_global_arr(&engine->vm, "view_wport", 0), gml_global_arr(&engine->vm, "view_hport", 0)); }
  if(prof){ t1 = profile_now_ms(engine); engine->profile.clear_ms += t1 - t0; t0 = t1; }
  engine->aspect_draw_full_context = engine->aspect_force_active;
  engine->aspect_draw_full_x = cam_x;
  engine->aspect_draw_full_y = cam_y;
  engine->aspect_event_view_stack_pointer = 0;
  engine->aspect_event_view_overflow = 0;
  { GmlRenderControl control={.fast_alpha_cull=core_opt_fast_alpha_cull(engine)};
    gml_render_control_update(&engine->render,&control,GML_RENDER_CONTROL_FAST_ALPHA); }
  if(!engine->output_width || !engine->output_height) compute_present(engine);
  GmlRenderPresentationMetrics render_presentation={0};
  gml_render_presentation_metrics(&engine->render,&render_presentation);
  GmlRenderSamplePlanes sample_planes={0};
  gml_render_sample_planes_update(
    &engine->render,&sample_planes,GML_RENDER_SAMPLE_PLANES_APPLICATION);
  size_t classic_pixels=(size_t)engine->width*(size_t)engine->height;
  int classic_phase = anygm_policy_classic_modern_presentation(&engine->win) &&
                      !render_presentation.interpolation &&
                      engine->output_width==engine->width*2 && engine->output_height==engine->height*2 &&
                      !engine->canvas_mode && !engine->aspect_force_active &&
                      ensure_classic_phase(engine,classic_pixels);
  int classic_interp_phase = anygm_policy_classic_modern_presentation(&engine->win) &&
                             render_presentation.interpolation &&
                             engine->output_width==engine->width*2 && engine->output_height==engine->height*2 &&
                             !engine->canvas_mode && !engine->aspect_force_active &&
                             classic_pixels<=SIZE_MAX/3 &&
                             ensure_classic_phase(engine,classic_pixels*3);
  int view_surface = (int)gml_global_arr(&engine->vm, "view_surface_id", frame_view_index);
  int multiview_rendered = !engine->aspect_force_active && render_multiview_application(engine);
  GmlRenderApplicationWriteView direct_world_view={0};
  int direct_owned_world=0;
  uint32_t *world_pixels=engine->fb;
  int world_width=(int)engine->width;
  int world_height=(int)engine->height;
  if (!multiview_rendered) {
  *gml_varmap_put(&engine->vm.globals,"view_current")=vreal(frame_view_index);
  if(!engine->aspect_force_active && frame_view_count==1 &&
     render_presentation.application_owned &&
     !(view_surface>0 && gml_surface_exists(&engine->render,view_surface))){
    GmlPresentView *view=&frame_views[0];
    direct_owned_world=(application_surface_scales_full_view_port(
      engine,frame_view_count,view->px,view->py,view->pw,view->ph,
      render_presentation.application_width,render_presentation.application_height) ||
      application_surface_matches_first_generation_view_port(
        engine,frame_view_count,view->px,view->py,view->pw,view->ph,
        render_presentation.application_width,render_presentation.application_height)) &&
      gml_render_application_surface_owned_view(&engine->render,&direct_world_view);
  }
  world_pixels=direct_owned_world?direct_world_view.pixels:engine->fb;
  world_width=direct_owned_world?direct_world_view.width:(int)engine->width;
  world_height=direct_owned_world?direct_world_view.height:(int)engine->height;
  gml_render_begin(&engine->render,world_pixels,world_width,world_height,cam_x,cam_y);
  if(direct_owned_world)
    gml_render_world_set_logical_extent(
      &engine->render,(int)engine->width,(int)engine->height);
  if(classic_phase) sample_planes.classic_vertical=engine->classic_phase_mem;
  if(classic_interp_phase){
    sample_planes.classic_interpolated[0]=engine->classic_phase_mem;
    sample_planes.classic_interpolated[1]=engine->classic_phase_mem+classic_pixels;
    sample_planes.classic_interpolated[2]=engine->classic_phase_mem+classic_pixels*2;
  }
  gml_render_sample_planes_update(
    &engine->render,&sample_planes,GML_RENDER_SAMPLE_PLANES_CLASSIC);
  GmlRoom rm;
  int have_room = (gml_vm_room_get(&engine->vm, engine->vm.room_index, &rm) == 0);
  /* The legacy background-color field and the distinct room flag can each request an
   * application-surface clear. If both are disabled, drawing retains the completed framebuffer. */
  if (!have_room || room_clears_application_surface(&rm))
    gml_render_set_pending_fill(&engine->render, engine->background);
  /* Some games draw room backgrounds themselves from GML. When a launcher supplies that renderer
   * object's name, defer to it and avoid double-drawing the engine's static fallback. */
  const char *bg_renderer = anygm_host_development_setting(&engine->host,"GML_BG_RENDERER_OBJ");
  int pobj = (bg_renderer && *bg_renderer) ? gml_object_index_by_name(&engine->vm, bg_renderer) : -1;
  int gml_draws_bg = (pobj >= 0 && gml_find_instance(&engine->vm, pobj) != NULL);
  if (have_room && !gml_draws_bg) draw_runtime_backgrounds(engine,0);
  gml_vm_draw_pass(&engine->vm, "Draw_72");   /* Draw Begin */
  gml_vm_draw(&engine->vm);   /* instances + room tiles, interleaved by depth */
  gml_vm_draw_pass(&engine->vm, "Draw_73");   /* Draw End */
  if (have_room && !gml_draws_bg) draw_runtime_backgrounds(engine,1);
  *gml_varmap_put(&engine->vm.globals,"view_current")=vreal(0);
  engine->aspect_draw_full_context = 0;
  engine->aspect_event_view_stack_pointer = 0;
  engine->aspect_event_view_overflow = 0;
  gml_render_flush_pending_fill(&engine->render);
  if(direct_owned_world &&
     gml_render_application_surface_owned_view(&engine->render,&direct_world_view)){
    world_pixels=direct_world_view.pixels;
    world_width=direct_world_view.width;
    world_height=direct_world_view.height;
  }
  aspect_mask_outside_room(engine,cam_x, cam_y);
  if(prof){ t1 = profile_now_ms(engine); engine->profile.draw_ms += t1 - t0; t0 = t1; }
  gml_render_presentation_metrics(&engine->render,&render_presentation);
  if(anygm_host_development_setting(&engine->host,"GML_LOG_DRAW")){ int nz=0; for(size_t i=0;i<(size_t)world_width*world_height;i++) if(world_pixels[i]&0xFFFFFF) nz++;
    if(engine->diagnostics.draw_frame<3||engine->diagnostics.draw_frame%200==0){ int live=0,deactivated=0,dormant=0,highest=-1;
      for(int i=0;i<engine->vm.inst_count;i++){ GmlInstance *in=&engine->vm.inst[i];
        if(in->active&&!in->marked){ live++; highest=i; }
        else if(in->deactivated){ deactivated++; highest=i; }
        else if(in->room_dormant){ dormant++; highest=i; } }
      engine_logf(engine,ANYGM_LOG_DEBUG,"[draw] f=%d room=%d inst=%d live=%d deact=%d dormant=%d high=%d res=%ux%u fb_nonblack=%d app_draw_en=%d\n",
              engine->diagnostics.draw_frame,engine->vm.room_index,engine->vm.inst_count,live,deactivated,dormant,highest,world_width,world_height,nz,
              render_presentation.application_draw_enabled); }
    engine->diagnostics.draw_frame++; }
  /* Studio view-to-surface: when view 0 targets a surface, mirror the rendered frame into it so
   * Draw GUI can composite the surface. */
  {
    int vs = view_surface;
    GmlRenderTargetCoverage render_coverage={0};
    gml_render_target_coverage(&engine->render,&render_coverage);
    if (vs > 0 && gml_render_surface_mirror_pixels(
          &engine->render,vs,world_pixels,world_width,world_height,
          render_coverage.opaque_known && render_coverage.all_opaque)) {
      if(anygm_host_development_setting(&engine->host,"GML_LOG_SHADER")){
        int sw=0,sh=0; const uint32_t *pixels=gml_surface_pixels_read(&engine->render,vs,&sw,&sh);
        if(engine->diagnostics.shader_count++<8) engine_logf(engine,ANYGM_LOG_DEBUG,"[shader] f%ld mirrored view surface=%d %dx%d center=%08x\n",
          engine->vm.frame,vs,sw,sh,pixels&&sw>0&&sh>0?pixels[(size_t)(sh/2)*sw+sw/2]:0);
      }
    }
  }
  }
  /* The frame buffer is now the application surface. Run Draw GUI events into the screen buffer so a
   * presentation object can composite the frame and overlays. If nothing draws the surface,
   * auto-blit it. */
  GmlRenderApplicationWriteView app_view={0};
  if(direct_owned_world){
    GmlRenderTargetCoverage render_coverage={0};
    gml_render_target_coverage(&engine->render,&render_coverage);
    gml_render_application_surface_select_owned(
      &engine->render,render_coverage.opaque_known && render_coverage.all_opaque);
  } else if(gml_render_application_surface_owned_clear(&engine->render,engine->background,&app_view)){
    int aw=app_view.width, ah=app_view.height;
    if(multiview_rendered)
      compose_view_rect(engine->fb,(int)engine->width,(int)engine->height,
                        app_view.pixels,aw,ah,0,0,aw,ah);
    else if(!(view_surface>0 && gml_surface_exists(&engine->render,view_surface))){
      GmlPresentView v;
      if(frame_view_count>0){
        v=frame_views[0];
        int dx=v.px, dy=v.py, dw=v.pw, dh=v.ph;
        /* A sole view which covered the logical application surface before an explicit resize
         * continues to cover the resized surface.  Its room/view coordinates stay logical while
         * the complete viewport scales to the new render resolution. Preserving the
         * old pixel-sized port here instead placed the whole scene in one corner of the larger
         * surface before presentation.  Multi-view and deliberately inset ports retain their
         * authored rectangles. */
        int one_view = frame_view_count==1;
        int port_is_logical = abs(dw-(int)engine->width)<=1 && abs(dh-(int)engine->height)<=1;
        /* A room can retain a full-window port authored for a larger display after game code has
         * selected a smaller window/application surface (for example 1920x1080 -> 960x540). The
         * full-origin viewport scales with the window; treating its raw pixel size as
         * a literal rectangle inside the smaller application surface samples only the upper-left
         * quadrant and produces a false zoom. Only normalize an oversized, same-aspect, sole view;
         * smaller/inset and multi-view ports keep their authored rectangles. */
        int oversized_full_port = dw>0 && dh>0 && aw>0 && ah>0 &&
                                  dw>=aw && dh>=ah &&
                                  fabs((double)dw/(double)dh-(double)aw/(double)ah)<0.0005;
        int resized_full_port = stale_full_view_port(engine,one_view,dx,dy,dw,dh,
                                                     (int)engine->width,(int)engine->height,aw,ah);
        int scaled_full_port = application_surface_scales_full_view_port(
          engine,frame_view_count,dx,dy,dw,dh,aw,ah);
        int full_logical_view = one_view && dx==0 && dy==0 &&
                                (port_is_logical || oversized_full_port || resized_full_port ||
                                 scaled_full_port);
        if(full_logical_view){ dx=dy=0; dw=aw; dh=ah; }
        compose_view_rect(engine->fb,(int)engine->width,(int)engine->height,app_view.pixels,aw,ah,
                          dx,dy,dw,dh);
      } else
        compose_view_rect(engine->fb,(int)engine->width,(int)engine->height,app_view.pixels,aw,ah,
                          0,0,aw,ah);
    }
    gml_render_application_surface_select_owned(&engine->render,1);
  } else {
    GmlRenderTargetCoverage render_coverage={0};
    gml_render_target_coverage(&engine->render,&render_coverage);
    gml_render_application_surface_bind(
      &engine->render,engine->fb,(int)engine->width,(int)engine->height,
      render_coverage.opaque_known && render_coverage.all_opaque);
  }
  gml_render_presentation_metrics(&engine->render,&render_presentation);
  sample_planes.application_vertical=NULL;
  for(int i=0;i<3;i++) sample_planes.application_interpolated[i]=NULL;
  if(!render_presentation.application_owned && classic_phase)
    sample_planes.application_vertical=engine->classic_phase_mem;
  if(!render_presentation.application_owned && classic_interp_phase){
    sample_planes.application_interpolated[0]=engine->classic_phase_mem;
    sample_planes.application_interpolated[1]=engine->classic_phase_mem+classic_pixels;
    sample_planes.application_interpolated[2]=engine->classic_phase_mem+classic_pixels*2;
  }
  gml_render_sample_planes_update(
    &engine->render,&sample_planes,GML_RENDER_SAMPLE_PLANES_APPLICATION);
  if (!engine->output_width || !engine->output_height) compute_present(engine);
  unsigned ow = engine->output_width, oh = engine->output_height;
	  /* GUI target: the output canvas directly when GUI space matches it (canvas mode, or GUI==view);
	   * otherwise draw the GUI at its own dimensions into a scratch buffer and downscale to the
	   * native output. */
		  int aspect_gui_x = 0, aspect_gui_y = 0, aspect_gui_w = 0, aspect_gui_h = 0;
		  int aspect_gui_center = 0;
		  /* Games the aspect hook flags as full-width-compositor opt out of the centered-4:3 GUI pass:
		   * their Draw-GUI compositor runs at the full width instead, so the CRT covers the entire
		   * 16:9/21:9 frame rather than a centered 4:3 sub-rect. */
		  int compositor_fullwidth = aspect_compositor_fullwidth_gen(engine);
		  if (engine->aspect_force_active && !compositor_fullwidth &&
		      ((engine->base_width > 0 && engine->base_width < engine->width) || (engine->base_height > 0 && engine->base_height < engine->height))) {
		    aspect_hud_rect(engine,&aspect_gui_x, &aspect_gui_y, &aspect_gui_w, &aspect_gui_h);
		    aspect_gui_center = aspect_gui_w > 0 && aspect_gui_h > 0;
		  }
		  if (aspect_gui_center) {
		    if (!ensure_scratch_buffer(engine,&engine->app_crop)) {
		      engine_errorf(engine,ANYGM_ERROR_OUT_OF_MEMORY,"Could not allocate the application crop buffer");
		      return ANYGM_ERROR_OUT_OF_MEMORY;
		    }
	    for (int yy = 0; yy < aspect_gui_h; yy++) {
		      memcpy(engine->app_crop + (size_t)yy * aspect_gui_w,
		             engine->fb + (size_t)(aspect_gui_y + yy) * engine->width + aspect_gui_x,
	             (size_t)aspect_gui_w * sizeof(uint32_t));
	    }
	    GmlRenderTargetCoverage render_coverage={0};
	    gml_render_target_coverage(&engine->render,&render_coverage);
	    gml_render_application_surface_bind(
	      &engine->render,engine->app_crop,aspect_gui_w,aspect_gui_h,
	      render_coverage.opaque_known && render_coverage.all_opaque);
		    aspect_view_overlay_end(engine,&draw_ov, 0);
		    draw_ov.active = 0;
		  }
		  int gsw = aspect_gui_center ? aspect_gui_w : (engine->gui_space_width > 0 ? engine->gui_space_width : (int)ow);
		  int gsh = aspect_gui_center ? aspect_gui_h : (engine->gui_space_height > 0 ? engine->gui_space_height : (int)oh);
			  int gui_indirect = !aspect_gui_center && !engine->canvas_mode && (gsw != (int)ow || gsh != (int)oh);
			  if ((aspect_gui_center || gui_indirect) && !ensure_scratch_buffer(engine,&engine->gui_buffer)) {
			    engine_errorf(engine,ANYGM_ERROR_OUT_OF_MEMORY,"Could not allocate the GUI scratch buffer");
			    return ANYGM_ERROR_OUT_OF_MEMORY;
			  }
			  uint32_t *gtarget = (aspect_gui_center || gui_indirect) ? engine->gui_buffer : engine->screen;
			  int gtw = (aspect_gui_center || gui_indirect) ? gsw : (int)ow;
			  int gth = (aspect_gui_center || gui_indirect) ? gsh : (int)oh;
			  gml_render_begin(&engine->render, gtarget, gtw, gth,
		                     (engine->aspect_force_active || aspect_gui_center) ? 0.0 : -(double)engine->gui_offset_x,
		                     (engine->aspect_force_active || aspect_gui_center) ? 0.0 : -(double)engine->gui_offset_y);
	  gml_render_set_pending_fill(&engine->render, 0);
	  /* The app surface normally presents into the view PORT fitted into GUI space. A classic
	   * runtime window resize preserves the declared port rectangle and clears the new margins. */
	  double pvis_ = gml_global_arr(&engine->vm, "view_visible", 0);
	  double pw_ = 0, ph_ = 0;
	  int port_x_ = 0, port_y_ = 0;
	  if (pvis_ >= 0.5) {
	    port_x_ = (int)lround(gml_global_arr(&engine->vm,"view_xport",0));
	    port_y_ = (int)lround(gml_global_arr(&engine->vm,"view_yport",0));
	    pw_ = gml_global_arr(&engine->vm, "view_wport", 0);
	    ph_ = gml_global_arr(&engine->vm, "view_hport", 0);
	  }
	  /* Multiple visible views define one application-surface canvas. Present that complete
	   * canvas, not the first camera's port; otherwise a side HUD plus world view is uniformly
	   * fitted through the narrower world port and gains false bars. */
	  if (frame_view_count > 1) {
	    port_x_ = port_y_ = 0;
	    pw_ = engine->width;
	    ph_ = engine->height;
	  }
	  gml_render_presentation_metrics(&engine->render,&render_presentation);
	  if (render_presentation.application_owned &&
	      stale_full_view_port(engine,present_view_count(engine,NULL,NULL,NULL),port_x_,port_y_,
	                           (int)lround(pw_),(int)lround(ph_),(int)engine->width,(int)engine->height,
	                           render_presentation.application_width,
	                           render_presentation.application_height)) {
	    port_x_ = port_y_ = 0;
	    pw_ = render_presentation.application_width;
	    ph_ = render_presentation.application_height;
	  }
	  /* compute_present(engine) may have selected an explicitly owned, window-sized application
	   * surface as the final Studio raster.  Its view port describes the world inside that
	   * already-composed surface; fitting the complete surface through the view's slightly
	   * different aspect would introduce a one-pixel border. */
	  if (!engine->aspect_force_active && anygm_policy_has_modern_layer_semantics(&engine->win) &&
	      render_presentation.application_owned &&
	      render_presentation.application_draw_enabled &&
	      engine->vm.gui_w <= 0 && engine->vm.gui_h <= 0 &&
	      render_presentation.application_width == (int)ow &&
	      render_presentation.application_height == (int)oh &&
	      engine->vm.window_w == render_presentation.application_width &&
	      engine->vm.window_h == render_presentation.application_height) {
	    port_x_ = port_y_ = 0;
	    pw_ = render_presentation.application_width;
	    ph_ = render_presentation.application_height;
	  }
		  if (aspect_gui_center) { pw_ = aspect_gui_w; ph_ = aspect_gui_h; }
	  else if (engine->aspect_force_active) { pw_ = engine->width; ph_ = engine->height; }
  if (pw_ <= 0 || ph_ <= 0) { pw_ = engine->width; ph_ = engine->height; }
	  int prw, prh, prx, pry;
	  if (engine->canvas_mode) { prw = gsw; prh = gsh; prx = 0; pry = 0; }
	  else {
	    int explicit_window_ = engine->vm.window_w>0 && engine->vm.window_h>0;
	    if (!gml_classic_present_explicit_port(&engine->win,explicit_window_,gtw,gth,
	                                            port_x_,port_y_,(int)lround(pw_),(int)lround(ph_),
	                                            &prx,&pry,&prw,&prh)) {
	      double fit = (double)gtw / pw_;
	      if (ph_ * fit > (double)gth) fit = (double)gth / ph_;
	      /* Studio 2 truncates the fitted viewport extent and biases an odd remainder toward the
	       * positive axis. Studio 1 rounds the extent and uses the older integer centring phase.
	       * Keeping this generation distinction avoids a one-pixel screen-stage stretch in
	       * modern projects without shifting otherwise identical pre-bytecode-17 presentations. */
	      if (anygm_policy_has_modern_layer_semantics(&engine->win)) {
	        prw = (int)floor(pw_ * fit + 1e-9); prh = (int)floor(ph_ * fit + 1e-9);
	        prx = (gtw - prw + 1) / 2; pry = (gth - prh + 1) / 2;
	      } else {
	        prw = (int)lround(pw_ * fit); prh = (int)lround(ph_ * fit);
	        prx = (gtw - prw + 1) / 2; pry = (gth - prh + 1) / 2;
	      }
	    }
	  }
	  /* Preserve the classic fixed-function viewport phase when the logical
	   * view is presented into a larger port. */
  if (!engine->canvas_mode && !engine->aspect_force_active &&
      !render_presentation.application_phase_active)
	    gml_classic_present_adjust(
	      &engine->win,(int)engine->width,(int)engine->height,prw,prh,
	      render_presentation.interpolation,&prx,&pry);
	  engine->present_mouse_valid=prw>0 && prh>0 && pw_>0 && ph_>0;
	  engine->present_mouse_x=prx; engine->present_mouse_y=pry;
	  engine->present_mouse_width=prw; engine->present_mouse_height=prh;
	  engine->present_mouse_source_width=(int)lround(pw_);
	  engine->present_mouse_source_height=(int)lround(ph_);
	  /* Without an explicit display_set_gui_size, Studio's screen-stage projection is the logical
	   * application surface fitted into its viewport. A self-compositor relies on both pieces: its
	   * ordinary coordinates scale with the fitted viewport, while negative/overflowing coordinates
	   * draw into the window margins. Begin with the viewport as the physical GUI base, then expose
	   * the application-surface dimensions as its logical coordinate system. */
	  int screen_default_gui=!render_presentation.application_draw_enabled &&
	                         !engine->canvas_mode &&
	                         !engine->aspect_force_active && engine->vm.gui_w<=0 && engine->vm.gui_h<=0 &&
	                         prw>0 && prh>0;
	  if(screen_default_gui){
	    gml_render_gui_begin(&engine->render,prw,prh);
	    /* A self-compositor can own an application surface that already is the explicit window
	     * raster while its camera remains much smaller. Post-Draw addresses that complete raster
	     * in window coordinates; applying the camera-to-port scale again turns a full-surface blit
	     * into an oversized, clipped image. A compositor which keeps a logical application surface
	     * still uses the camera-sized coordinate system below. */
	    int owned_window_raster=render_presentation.application_owned &&
	                            render_presentation.application_width>0 &&
	                            render_presentation.application_height>0 &&
	                            render_presentation.application_width==engine->vm.window_w &&
	                            render_presentation.application_height==engine->vm.window_h &&
	                            render_presentation.application_width==gtw &&
	                            render_presentation.application_height==gth;
	    /* Studio 1.x self-compositors can deliberately keep the application surface at the
	     * logical view size while deriving every screen-stage coordinate from the explicit window
	     * resolution. At an integer same-aspect host scale, that authored coordinate system is
	     * already the final raster. Applying the view-to-window transform again doubles positions
	     * and sizes. Modern formats expose this intent through independently owned surfaces;
	     * classic runtimes do not have the Studio screen-stage contract. */
	    int legacy_window_raster=screen_stage_uses_requested_raster(
	      &engine->win,&render_presentation,
	      (int)engine->width,(int)engine->height,gtw,gth);
	    gml_render_gui_set_size(&engine->render,
	      owned_window_raster?render_presentation.application_width:
	      legacy_window_raster?gtw:(int)engine->width,
	      owned_window_raster?render_presentation.application_height:
	      legacy_window_raster?gth:(int)engine->height);
	  } else {
	  gml_render_gui_begin(&engine->render,gsw,gsh);
	    /* display_set_gui_size() is normally called from Create/room setup, before the GUI pass
	     * starts. Seed the renderer from that persistent VM state as well as accepting live changes
	     * during Draw GUI. */
	    if(anygm_policy_has_modern_layer_semantics(&engine->win) && engine->vm.gui_w>0 && engine->vm.gui_h>0)
	      gml_render_gui_set_size(&engine->render,engine->vm.gui_w,engine->vm.gui_h);
	  }
	  if(engine->vm.gui_maximise_active)
	    gml_render_gui_set_maximise(&engine->render,1,
	      engine->vm.gui_maximise_xscale,engine->vm.gui_maximise_yscale,
	      engine->vm.gui_maximise_xoffset,engine->vm.gui_maximise_yoffset,
	      engine->vm.window_w>0?engine->vm.window_w:(int)engine->win.disp_w,
	      engine->vm.window_h>0?engine->vm.window_h:(int)engine->win.disp_h);
	  /* GM screen-stage events: Pre-Draw -> [default app-surface blit] -> Post-Draw -> GUI.
	   * Content that composites the application surface itself, for example through a presentation
	   * object applying a palette shader in Post-Draw) disable the default blit and draw here.
	   * Post-Draw remains anchored to the fitted application viewport: negative coordinates may
	   * intentionally spill into the surrounding window margins. Applying that viewport origin is
	   * what keeps a self-compositor centered when its explicit window is wider than the view. */
		  log_present_pass(engine,"gui-begin",gtarget,gtw,gth);
		  GmlRenderTargetMetrics screen_target={0};
		  gml_render_target_metrics(&engine->render,&screen_target);
		  int screen_viewport_offset=screen_default_gui && (prx!=0 || pry!=0);
		  if(screen_viewport_offset){
		    GmlRenderTargetMetrics offset_target=screen_target;
		    offset_target.camera_x=-(double)prx;
		    offset_target.camera_y=-(double)pry;
		    gml_render_target_metrics_update(
		      &engine->render,&offset_target,GML_RENDER_TARGET_CAMERA);
		  }
		  gml_vm_draw_pass(&engine->vm, "Draw_77");   /* Post-Draw (GMS2 event 77) can replace the default
		                                         * application-surface blit with a custom composite. */
		  log_present_pass(engine,"post-draw",gtarget,gtw,gth);
		  gml_render_presentation_metrics(&engine->render,&render_presentation);
		  if (render_presentation.application_draw_enabled ||
		      anygm_host_development_setting(&engine->host,"GML_NO_CRT"))
		    gml_render_set_pending_underlay(&engine->render, prx, pry, prw, prh);
      if (engine->aspect_force_active)
        gml_render_flush_pending_underlay(&engine->render);
		      if (aspect_gui_center) {
		        GmlRenderTargetMetrics gui_target={0};
		        gml_render_target_metrics(&engine->render,&gui_target);
		        gui_target.camera_x=0.0;
		        gui_target.camera_y=0.0;
		        gml_render_target_metrics_update(
		          &engine->render,&gui_target,GML_RENDER_TARGET_CAMERA);
	      } else if (engine->aspect_force_active) {
	        /* A full-width compositor draws in the forced framebuffer's own coordinate space.
	         * Retaining the centered native-GUI offset translates shader surface draws and clips
	         * the trailing edge. Non-compositor GUI passes keep the native centered offset. */
	        GmlRenderTargetMetrics gui_target={0};
	        gml_render_target_metrics(&engine->render,&gui_target);
	        gui_target.camera_x=compositor_fullwidth ? 0.0 : -(double)engine->gui_offset_x;
	        gui_target.camera_y=compositor_fullwidth ? 0.0 : -(double)engine->gui_offset_y;
	        gml_render_target_metrics_update(
	          &engine->render,&gui_target,GML_RENDER_TARGET_CAMERA);
	      }
		  if (!anygm_host_development_setting(&engine->host,"GML_NO_CRT")) {
		    gml_vm_draw_pass(&engine->vm, "Draw_74");  /* Draw GUI Begin */
		    log_present_pass(engine,"gui-pre",gtarget,gtw,gth);
		    gml_vm_draw_pass(&engine->vm, "Draw_65");
		    gml_vm_draw_gui(&engine->vm);
		    log_present_pass(engine,"gui",gtarget,gtw,gth);
		    gml_vm_draw_pass(&engine->vm, "Draw_66");
		    gml_vm_draw_pass(&engine->vm, "Draw_75");  /* Draw GUI End */
		    log_present_pass(engine,"gui-end",gtarget,gtw,gth);
		  }
  if(screen_viewport_offset)
    gml_render_target_metrics_update(
      &engine->render,&screen_target,GML_RENDER_TARGET_CAMERA);
  gml_render_gui_end(&engine->render);
  gml_render_flush_pending_underlay(&engine->render);
  gml_render_flush_pending_fill(&engine->render);
  log_present_pass(engine,"flushed",gtarget,gtw,gth);
  if (anygm_host_development_setting(&engine->host,"GML_LOG_PRESENT")) {
    if (engine->diagnostics.present_frame++ % 120 == 0) engine_logf(engine,ANYGM_LOG_DEBUG, "[present] out=%ux%u canvas=%d gui=%dx%d indirect=%d port=%dx%d prect=(%d,%d %dx%d) off=(%d,%d)\n",
      ow, oh, engine->canvas_mode, gsw, gsh, gui_indirect, (int)pw_, (int)ph_, prx, pry, prw, prh, engine->gui_offset_x, engine->gui_offset_y); }
  /* Fallback: content can disable the automatic application-surface blit
   * intending to composite it itself in a Draw GUI event. If that compositor paints nothing to the
   * screen here (unsupported GUI-space transform, absent object, etc.) the frame would be black — so
   * if the screen is still empty but the app-surface has pixels, blit it so the render isn't lost. */
  gml_render_presentation_metrics(&engine->render,&render_presentation);
  if (!render_presentation.application_draw_enabled &&
      !anygm_host_development_setting(&engine->host,"GML_NO_CRT")) {
    int screen_empty = 1;
    for (int i = 0; i < gtw * gth; i++) if (gtarget[i] & 0xFFFFFF) { screen_empty = 0; break; }
    if (screen_empty) {
      int fb_has = 0,app_width=0,app_height=0;
      const uint32_t *app_pixels=
        gml_surface_pixels_read(&engine->render,0,&app_width,&app_height);
      for(size_t i=0;app_pixels && i<(size_t)app_width*app_height;i++)
        if(app_pixels[i]&0xFFFFFF){ fb_has=1; break; }
	      if (fb_has) {
          if (engine->aspect_force_active) {
            GmlRenderTargetMetrics fallback_target={0};
            gml_render_target_metrics(&engine->render,&fallback_target);
            GmlRenderTargetMetrics zero_camera=fallback_target;
            zero_camera.camera_x=zero_camera.camera_y=0.0;
            gml_render_target_metrics_update(
              &engine->render,&zero_camera,GML_RENDER_TARGET_CAMERA);
            gml_draw_surface_stretched(&engine->render, 0, prx, pry, prw, prh, 0xFFFFFF, 1.0);
            gml_render_target_metrics_update(
              &engine->render,&fallback_target,GML_RENDER_TARGET_CAMERA);
          } else {
	          gml_draw_surface_stretched(&engine->render, 0, prx, pry, prw, prh, 0xFFFFFF, 1.0);
          }
	      }
	    }
	  }
			  if (aspect_gui_center) {
			    if (ow == engine->width && oh == engine->height)
			      memcpy(engine->screen, engine->fb, (size_t)ow * oh * sizeof(uint32_t));
			    else
			      memset(engine->screen, 0, (size_t)ow * oh * sizeof(uint32_t));
			    for (int yy = 0; yy < aspect_gui_h && aspect_gui_y + yy < (int)oh; yy++) {
			      if (aspect_gui_x >= (int)ow) continue;
		      int copy_w = aspect_gui_w;
		      if (aspect_gui_x + copy_w > (int)ow) copy_w = (int)ow - aspect_gui_x;
		      if (copy_w > 0)
		        memcpy(engine->screen + (size_t)(aspect_gui_y + yy) * ow + aspect_gui_x,
		               engine->gui_buffer + (size_t)yy * aspect_gui_w,
		               (size_t)copy_w * sizeof(uint32_t));
		    }
	  } else if (gui_indirect) {
		    if (ow >= (unsigned)gtw && oh >= (unsigned)gth) {
		      /* The generic box path degenerates to one source pixel during an upscale. Walk the
		       * pixel-centred floor((x+.5)*src/dst) mapping incrementally, without per-pixel
		       * division or averaging. The doubled accumulator preserves half steps for odd sizes. */
		      unsigned sy = 0, yacc = (unsigned)gth;
		      for (unsigned oy2 = 0; oy2 < oh; oy2++) {
		        const uint32_t *src = engine->gui_buffer + (size_t)sy * gtw;
		        uint32_t *dst = engine->screen + (size_t)oy2 * ow;
		        unsigned sx = 0, xacc = (unsigned)gtw;
		        for (unsigned ox2 = 0; ox2 < ow; ox2++) {
		          dst[ox2] = src[sx] & 0xFFFFFFu;
		          xacc += (unsigned)gtw * 2u;
		          if (xacc >= ow * 2u) { xacc -= ow * 2u; sx++; }
		        }
		        yacc += (unsigned)gth * 2u;
		        if (yacc >= oh * 2u) { yacc -= oh * 2u; sy++; }
		      }
		    } else {
		      /* Box-downscale the GUI-space frame onto the native output. */
		      for (unsigned oy2 = 0; oy2 < oh; oy2++) {
        unsigned sy0 = oy2 * gth / oh, sy1 = (oy2 + 1) * gth / oh; if (sy1 <= sy0) sy1 = sy0 + 1;
        for (unsigned ox2 = 0; ox2 < ow; ox2++) {
          unsigned sx0 = ox2 * gtw / ow, sx1 = (ox2 + 1) * gtw / ow; if (sx1 <= sx0) sx1 = sx0 + 1;
          unsigned rs = 0, gs = 0, bs = 0, n = 0;
          for (unsigned sy2 = sy0; sy2 < sy1 && sy2 < (unsigned)gth; sy2++)
            for (unsigned sx2 = sx0; sx2 < sx1 && sx2 < (unsigned)gtw; sx2++) {
              uint32_t v = engine->gui_buffer[sy2 * gtw + sx2];
              rs += (v >> 16) & 0xFF; gs += (v >> 8) & 0xFF; bs += v & 0xFF; n++;
            }
          if (!n) n = 1;
          engine->screen[oy2 * ow + ox2] = ((rs / n) << 16) | ((gs / n) << 8) | (bs / n);
	        }
	      }
	    }
	  }
	  classic_transition_apply(engine,engine->screen,ow,oh);
	  if(engine->vm.classic_info_active && engine->win.classic_game_information_size)
	    gml_draw_classic_game_information(&engine->render,engine->screen,(int)ow,(int)oh,
	                                      engine->win.classic_game_information,
	                                      engine->win.classic_game_information_size);
	  if(prof){ t1 = profile_now_ms(engine); engine->profile.gui_ms += t1 - t0; t0 = t1; }
  aspect_view_overlay_end(engine,&draw_ov, 0);
  draw_game_cursor(engine,ow,oh);
  if(run_step) gml_vm_post_draw(&engine->vm);
  engine->have_presented_frame=1;
  if(prof){ t1 = profile_now_ms(engine); engine->profile.video_ms += t1 - t0; t0 = t1; }
  /* audio: emit ~44100/fps stereo frames this tick, mixing active voices (acc handles the remainder) */
  {
    engine->audio_accumulator += 44100.0 / engine->fps; int nf = (int)engine->audio_accumulator; engine->audio_accumulator -= nf;
    if (nf > 4096) nf = 4096;
    if(nf>0){ gml_audio_mix(engine->audio,engine->audio_output,nf); engine->audio_frames=(size_t)nf; }
  }
  if(prof){
    t1 = profile_now_ms(engine);
    engine->profile.audio_ms += t1 - t0;
    engine->profile.total_ms += t1 - t_total;
    { double ft = t1 - t_total;
      if(ft > engine->profile.max_ms){ engine->profile.max_ms = ft; engine->profile.max_frame = engine->vm.frame; }
      /* GML_PROFILE_SPIKE=<ms>: dump the phase split of any frame that exceeds the threshold */
      if(engine->diagnostics.profile_spike_ms < -1){ const char *sp=anygm_host_development_setting(&engine->host,"GML_PROFILE_SPIKE"); engine->diagnostics.profile_spike_ms = sp? atof(sp) : -1; }
      if(engine->diagnostics.profile_spike_ms > 0 && ft > engine->diagnostics.profile_spike_ms)
        engine_logf(engine,ANYGM_LOG_DEBUG, "[spike] f%ld total=%.2fms step=%.2f draw=%.2f gui=%.2f video=%.2f\n",
                engine->vm.frame, ft, engine->profile.step_ms - sp_step, engine->profile.draw_ms - sp_draw,
                engine->profile.gui_ms - sp_gui, engine->profile.video_ms - sp_video); }
    engine->profile.frames++;
    profile_report(engine,0);
  }
  /* Snapshot key state after the step consumed it. */
  memcpy(engine->event_vk_previous, engine->event_vk_current, sizeof(engine->event_vk_current));
  memcpy(engine->event_key_previous, engine->event_key_current, sizeof(engine->event_key_current));
  if(state_restore_frame && engine->state_reapply_size){
    size_t restore_size=engine->state_reapply_size;
    engine->state_reapply_size=0;
    if(!state_unserialize_impl(engine,engine->state_reapply,restore_size,0))
      engine_logf(engine,ANYGM_LOG_ERROR,"[anygm] could not reapply loaded state after presentation\n");
    /* state_unserialize_impl deliberately leaves the composited pixels alone.  They are the
     * visible rewind/load frame even though the simulation was put back at its pre-Draw state. */
    engine->state_just_loaded=0;
    engine->have_presented_frame=1;
  }
  if(state_restore_frame){
    engine->vm.game_change_pending=0;
    engine->vm.game_change_directory[0]='\0';
    engine->vm.game_change_parameters[0]='\0';
  } else if(engine->vm.game_change_pending){
    return engine_apply_game_change_and_run_frame(engine);
  }
  return ANYGM_OK;
}
/* GML_SELFTEST checks pure builtins with known inputs. */
static void run_selftest(AnygmEngine *engine){
  if(!anygm_host_development_setting(&engine->host,"GML_SELFTEST")) return;
  int pass=0, tot=0; GmlVal a[12];
  #define BI(nm,cnt) gml_builtin_call(&engine->vm,(nm),a,(cnt))
  #define CHK(desc,cond) do{ tot++; if(cond) pass++; else engine_logf(engine,ANYGM_LOG_DEBUG,"[selftest] FAIL %s\n",desc); }while(0)
  a[0]=vreal(200); a[1]=vreal(100); a[2]=vreal(50);
  int col=(int)BI("make_color_rgb",3).d;
  CHK("make_color legacy alias", (int)BI("make_color",3).d==col);
  a[0]=vreal(col);
  CHK("color_get_red",   (int)BI("color_get_red",1).d==200);
  CHK("color_get_green", (int)BI("color_get_green",1).d==100);
  CHK("color_get_blue",  (int)BI("color_get_blue",1).d==50);
  a[0]=vreal(5);a[1]=vreal(5);a[2]=vreal(0);a[3]=vreal(0);a[4]=vreal(10);a[5]=vreal(10);
  CHK("point_in_rectangle in",  (int)BI("point_in_rectangle",6).d==1);
  a[0]=vreal(15);
  CHK("point_in_rectangle out", (int)BI("point_in_rectangle",6).d==0);
  a[0]=vreal(2);a[1]=vreal(2);a[2]=vreal(0);a[3]=vreal(0);a[4]=vreal(10);a[5]=vreal(0);a[6]=vreal(0);a[7]=vreal(10);
  CHK("point_in_triangle", (int)BI("point_in_triangle",8).d==1);
  a[0]=vreal(10);a[1]=vreal(350);
  CHK("angle_difference",  (int)BI("angle_difference",2).d==20);
  a[0]=vstr("Hello, World!"); GmlVal enc=BI("base64_encode",1);
  CHK("base64_encode", enc.t==V_STR && enc.s && !strcmp(enc.s,"SGVsbG8sIFdvcmxkIQ=="));
  a[0]=enc; GmlVal dec=BI("base64_decode",1);
  CHK("base64_roundtrip", dec.t==V_STR && dec.s && !strcmp(dec.s,"Hello, World!"));
  a[0]=vstr("SGk="); GmlVal b64buf=BI("buffer_base64_decode",1);
  a[0]=b64buf; CHK("buffer_base64_decode size", (int)BI("buffer_get_size",1).d==2);
  a[0]=b64buf; a[1]=vreal(0); a[2]=vreal(0); BI("buffer_seek",3);
  a[0]=b64buf; a[1]=vreal(1); GmlVal b64a=BI("buffer_read",2);
  a[0]=b64buf; a[1]=vreal(1); GmlVal b64b=BI("buffer_read",2);
  CHK("buffer_base64_decode bytes", (int)b64a.d=='H' && (int)b64b.d=='i');
  a[0]=b64buf; BI("buffer_delete",1);
  a[0]=vstr("{\"a\":1,\"b\":\"x\"}"); GmlVal jsobj=BI("json_parse",1);
  CHK("json_parse struct", jsobj.t==V_REAL && GML_IS_STRUCT_ID(jsobj.d));
  a[0]=jsobj; a[1]=vstr("a"); GmlVal jsa=BI("variable_struct_get",2);
  CHK("variable_struct_get", jsa.t==V_REAL && (int)jsa.d==1);
  a[0]=jsobj; a[1]=vstr("c"); a[2]=vreal(3); BI("variable_struct_set",3);
  a[0]=jsobj; a[1]=vstr("c");
  CHK("variable_struct_exists", (int)BI("variable_struct_exists",2).d==1);
  a[0]=jsobj; GmlVal jss=BI("json_stringify",1);
  CHK("json_stringify struct", jss.t==V_STR && jss.s && strstr(jss.s,"\"a\":1") && strstr(jss.s,"\"c\":3"));
  a[0]=jsobj; a[1]=vstr("c"); BI("variable_struct_remove",2);
  CHK("variable_struct_remove", (int)BI("variable_struct_exists",2).d==0);
  a[0]=vstr("[1,\"z\",true]"); GmlVal jsarr=BI("json_parse",1); a[0]=jsarr; GmlVal arrs=BI("json_stringify",1);
  CHK("json_parse array", arrs.t==V_STR && arrs.s && !strcmp(arrs.s,"[1,\"z\",1]"));
  a[0]=vstr("[3,1,2]"); GmlVal sortarr=BI("json_parse",1);
  a[0]=sortarr; a[1]=vreal(1); BI("array_sort",2); a[0]=sortarr; GmlVal sorts=BI("json_stringify",1);
  CHK("array_sort ascending", sorts.t==V_STR && sorts.s && !strcmp(sorts.s,"[1,2,3]"));
  a[0]=vstr("gml_selftest_global"); a[1]=vreal(42); BI("variable_global_set",2);
  a[0]=vstr("gml_selftest_global"); GmlVal ggot=BI("variable_global_get",1);
  CHK("variable_global_get", ggot.t==V_REAL && (int)ggot.d==42);
  a[0]=vstr("AZ"); a[1]=vreal(2); CHK("string_byte_at", (int)BI("string_byte_at",2).d==90);
  a[0]=vstr("alpha beta gamma delta");
  double text_plain_w=BI("string_width",1).d;
  a[0]=vstr("alpha beta gamma delta"); a[1]=vreal(-1); a[2]=vreal(text_plain_w*0.60);
  double text_wrap_w=BI("string_width_ext",3).d;
  a[0]=vstr("alpha beta gamma delta");
  double text_plain_h=BI("string_height",1).d;
  a[0]=vstr("alpha beta gamma delta"); a[1]=vreal(-1); a[2]=vreal(text_plain_w*0.60);
  double text_wrap_h=BI("string_height_ext",3).d;
  CHK("string_width_ext wraps", text_plain_w>0 && text_wrap_w>0 && text_wrap_w<text_plain_w);
  CHK("string_height_ext wraps", text_plain_h>0 && text_wrap_h>text_plain_h);
  a[0]=vreal(1);a[1]=vreal(1);a[2]=vreal(2);a[3]=vreal(2);a[4]=vreal(0);a[5]=vreal(0);a[6]=vreal(5);a[7]=vreal(0);a[8]=vreal(0);a[9]=vreal(5);
  CHK("rectangle_in_triangle", (int)BI("rectangle_in_triangle",10).d==1);
  GmlVal lid=BI("ds_list_create",0);
  a[0]=lid; a[1]=vreal(3); a[2]=vreal(1); BI("ds_list_add",3);
  a[0]=lid; a[1]=vreal(1); a[2]=vreal(2); BI("ds_list_replace",3);
  a[0]=lid; a[1]=vreal(0); BI("ds_list_sort",2);
  a[0]=lid; a[1]=vreal(0); CHK("ds_list_sort", (int)BI("ds_list_find_value",2).d==3);
  GmlVal mid=BI("ds_map_create",0); a[0]=mid; CHK("ds_map_empty true", (int)BI("ds_map_empty",1).d==1);
  a[0]=mid; a[1]=vstr("k"); a[2]=vreal(7); BI("ds_map_set",3);
  a[0]=mid; CHK("ds_map_empty false", (int)BI("ds_map_empty",1).d==0);
  a[0]=vreal(3); a[1]=vreal(3); GmlVal gid=BI("ds_grid_create",2);
  a[0]=gid; a[1]=vreal(1); a[2]=vreal(2); a[3]=vreal(5); BI("ds_grid_set_post",4);
  a[0]=gid; a[1]=vreal(1); a[2]=vreal(2); a[3]=vreal(2); BI("ds_grid_add",4);
  a[0]=gid; a[1]=vreal(1); a[2]=vreal(2); CHK("ds_grid_add", (int)BI("ds_grid_get",3).d==7);
  a[0]=gid; a[1]=vreal(0); a[2]=vreal(0); a[3]=vreal(2); a[4]=vreal(2); a[5]=vreal(7);
  CHK("ds_grid_value_x", (int)BI("ds_grid_value_x",6).d==1);
  CHK("ds_grid_value_y", (int)BI("ds_grid_value_y",6).d==2);
  GmlInstance *ti=NULL;
  for(int i=0;i<engine->vm.inst_count;i++) if(engine->vm.inst[i].active && !engine->vm.inst[i].marked){ ti=&engine->vm.inst[i]; break; }
  if(ti){
    a[0]=vreal((double)ti->id); a[1]=vstr("x");
    CHK("variable_instance_exists builtin", (int)BI("variable_instance_exists",2).d==1);
    a[0]=vreal((double)ti->id); a[1]=vstr("gml_selftest_field"); a[2]=vstr("ok"); BI("variable_instance_set",3);
    a[0]=vreal((double)ti->id); a[1]=vstr("gml_selftest_field"); GmlVal igot=BI("variable_instance_get",2);
    CHK("variable_instance_get", igot.t==V_STR && igot.s && !strcmp(igot.s,"ok"));
  } else CHK("variable_instance target", 0);
  engine_logf(engine,ANYGM_LOG_DEBUG,"[selftest] %d/%d builtin checks passed\n", pass, tot);
  #undef CHK
  #undef BI
}

uint32_t anygm_api_version(void){
  return ANYGM_API_VERSION;
}

AnygmResult anygm_create(const AnygmHostServices *services,AnygmEngine **out_engine){
  if(!out_engine) return ANYGM_ERROR_INVALID_ARGUMENT;
  *out_engine=NULL;
  if(services && (services->abi_version!=ANYGM_HOST_SERVICES_VERSION ||
                  services->struct_size<sizeof(AnygmHostServices)))
    return ANYGM_ERROR_INCOMPATIBLE_ABI;
  if(services && ((!services->file_map)!=(!services->file_unmap)))
    return ANYGM_ERROR_INVALID_ARGUMENT;
  AnygmEngine *engine=calloc(1,sizeof *engine);
  if(!engine) return ANYGM_ERROR_OUT_OF_MEMORY;
  engine->guard=ANYGM_ENGINE_GUARD;
  if(services) memcpy(&engine->host,services,sizeof engine->host);
  else memset(&engine->host,0,sizeof engine->host);
  memset(&engine->config,0,sizeof engine->config);
  engine->config.struct_size=sizeof engine->config;
  engine->config.embedded_shaders=1;
  engine->config.crt_mask=1;
  engine->config.crt_scanlines=1;
  engine->config.crt_gamma=1;
  engine->config.crt_curvature=-1;
  engine->config.crt_vignette=-1;
  engine->config.fast_alpha_cull=0;
  engine->config.start_room=-1;
  snprintf(engine->language,sizeof engine->language,"en");
  snprintf(engine->region,sizeof engine->region,"us");
  snprintf(engine->language_tag,sizeof engine->language_tag,"en-US");
  engine->width=288; engine->height=216; engine->base_width=288; engine->base_height=216;
  engine->fps=60.0; engine->fps_room=-1;
  engine->player_object=-1;
  engine->mouse_pixel_x=-1.0; engine->mouse_pixel_y=-1.0;
  engine->profile_enabled=-1;
  engine->diagnostics.key_enabled=-1;
  engine->diagnostics.force_present_view=-1;
  engine->diagnostics.mouse_frame=-1;
  engine->diagnostics.cursor_frame=-1;
  engine->diagnostics.multiview_frame=-1;
  engine->diagnostics.profile_spike_ms=-2.0;
  engine->introskip_enabled=-1;
  engine->last_error[0]=0;
  *out_engine=engine;
  return ANYGM_OK;
}

void anygm_destroy(AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD) return;
  if(engine->lifecycle==ENGINE_LOADED) anygm_unload(engine);
  free(engine->state_reapply);
  free(engine->classic_phase_mem);
  free(engine->fb);
  free(engine->screen);
  free(engine->gui_buffer);
  free(engine->app_crop);
  engine->guard=0;
  free(engine);
}

AnygmResult anygm_load(AnygmEngine *engine,const AnygmContentSource *source,
                       const AnygmLoadConfig *config){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || !source ||
     source->struct_size<sizeof(AnygmContentSource)) return ANYGM_ERROR_INVALID_ARGUMENT;
  if(config && config->struct_size<sizeof(AnygmLoadConfig)) return ANYGM_ERROR_INCOMPATIBLE_ABI;
  if(engine->lifecycle!=ENGINE_EMPTY)
    return ANYGM_ERROR_INVALID_STATE;
  if(!ensure_primary_buffers(engine)){
    engine_errorf(engine,ANYGM_ERROR_OUT_OF_MEMORY,"Could not allocate the primary frame buffers");
    return ANYGM_ERROR_OUT_OF_MEMORY;
  }
  AnygmLoadConfig resolved_config={0};
  char host_language[16]={0},host_region[16]={0},host_tag[32]={0};
  const AnygmLoadConfig *load_config=config;
  if(!config && engine->host.locale){
    resolved_config.struct_size=sizeof resolved_config;
    if(engine->host.locale(engine->host.userdata,host_language,sizeof host_language,
                     host_region,sizeof host_region,host_tag,sizeof host_tag)==ANYGM_OK){
      resolved_config.language=host_language[0]?host_language:"en";
      resolved_config.region=host_region[0]?host_region:"us";
      resolved_config.language_tag=host_tag[0]?host_tag:"en-US";
      load_config=&resolved_config;
    }
  }
  AnygmResult result=engine_load_content(engine,source,load_config);
  if(result==ANYGM_OK) engine->lifecycle=ENGINE_LOADED;
  return result;
}

void anygm_unload(AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED) return;
  engine_unload(engine);
  engine_override_reset(engine);
  engine->lifecycle=ENGINE_EMPTY;
}

AnygmResult anygm_reset(AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED)
    return ANYGM_ERROR_INVALID_STATE;
  gml_audio_free(engine->audio); engine->audio=NULL; engine->vm.audio=NULL;
  gml_vm_free(&engine->vm);
  gml_render_free(&engine->render);
  boot_runtime(engine);
  return ANYGM_OK;
}

AnygmResult anygm_get_av_info(const AnygmEngine *engine,AnygmAvInfo *info){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED || !info)
    return ANYGM_ERROR_INVALID_STATE;
  if(info->struct_size<sizeof *info) return ANYGM_ERROR_INCOMPATIBLE_ABI;
  info->base_width=engine->output_width?engine->output_width:engine->width;
  info->base_height=engine->output_height?engine->output_height:engine->height;
  info->max_width=FB_MAX_W;
  info->max_height=FB_MAX_H;
  /* The ratio describes the frame actually handed over, so a host scaling to it preserves the
   * shape. Taking it from the window extent instead reshapes the picture: content routinely asks
   * for a window whose proportions differ from its view, and a 4:3 raster announced as the 16:9
   * window it requested arrives at the player stretched. */
  info->aspect_ratio=info->base_height?(double)info->base_width/info->base_height:4.0/3.0;
  info->frames_per_second=engine->fps;
  info->audio_rate=44100;
  return ANYGM_OK;
}

AnygmResult anygm_run_frame(AnygmEngine *engine,const AnygmInputFrame *input,
                            AnygmFrameOutput *output){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED || !output)
    return ANYGM_ERROR_INVALID_STATE;
  if(output->struct_size<sizeof *output || (input && input->struct_size<sizeof *input))
    return ANYGM_ERROR_INCOMPATIBLE_ABI;
  if(input) memcpy(&engine->input,input,sizeof engine->input);
  else { memset(&engine->input,0,sizeof engine->input); engine->input.pointer_x=-1; engine->input.pointer_y=-1; }
  engine->frame_flags=0;
  AnygmResult result=engine_run_frame(engine);
  if(result!=ANYGM_OK) return result;
  output->pixels=engine->screen;
  output->width=engine->output_width?engine->output_width:engine->width;
  output->height=engine->output_height?engine->output_height:engine->height;
  output->pitch=(size_t)output->width*sizeof(uint32_t);
  output->pixel_format=ANYGM_PIXEL_XRGB8888;
  output->audio=engine->audio_output;
  output->audio_frames=engine->audio_frames;
  output->audio_rate=44100;
  output->flags=engine->frame_flags;
  return ANYGM_OK;
}

AnygmResult anygm_set_config(AnygmEngine *engine,const AnygmConfigDelta *delta){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || !delta) return ANYGM_ERROR_INVALID_ARGUMENT;
  if(delta->struct_size<sizeof *delta || delta->values.struct_size<sizeof delta->values)
    return ANYGM_ERROR_INCOMPATIBLE_ABI;
  uint64_t f=delta->fields;
  if(f&ANYGM_CONFIG_PRESENT_WIDTH) engine->config.present_width=delta->values.present_width;
  if(f&ANYGM_CONFIG_PRESENT_HEIGHT) engine->config.present_height=delta->values.present_height;
  if(f&ANYGM_CONFIG_ASPECT_MODE) engine->config.aspect_mode=delta->values.aspect_mode;
  if(f&ANYGM_CONFIG_MOUSE_MODE) engine->config.mouse_mode=delta->values.mouse_mode;
  if(f&ANYGM_CONFIG_ROOM_SKIP_BUTTON) engine->config.room_skip_button=delta->values.room_skip_button;
  if(f&ANYGM_CONFIG_GOD_MODE) engine->config.god_mode=delta->values.god_mode;
  if(f&ANYGM_CONFIG_CRT_MASK) engine->config.crt_mask=delta->values.crt_mask;
  if(f&ANYGM_CONFIG_CRT_SCANLINES) engine->config.crt_scanlines=delta->values.crt_scanlines;
  if(f&ANYGM_CONFIG_CRT_GAMMA) engine->config.crt_gamma=delta->values.crt_gamma;
  if(f&ANYGM_CONFIG_CRT_CURVATURE) engine->config.crt_curvature=delta->values.crt_curvature;
  if(f&ANYGM_CONFIG_CRT_VIGNETTE) engine->config.crt_vignette=delta->values.crt_vignette;
  if(f&ANYGM_CONFIG_EMBEDDED_SHADERS) engine->config.embedded_shaders=delta->values.embedded_shaders;
  if(f&ANYGM_CONFIG_GAMEPAD_CONNECTED) engine->config.gamepad_connected=delta->values.gamepad_connected;
  if(f&ANYGM_CONFIG_FAST_ALPHA_CULL) engine->config.fast_alpha_cull=delta->values.fast_alpha_cull;
  if(f&ANYGM_CONFIG_FAST_FORWARD) engine->config.fast_forward=delta->values.fast_forward;
  if(f&ANYGM_CONFIG_START_ROOM) engine->config.start_room=delta->values.start_room;
  if(f&ANYGM_CONFIG_PRESENT_LOGICAL_RASTER){
    uint32_t want=delta->values.present_logical_raster?1u:0u;
    if(want!=engine->config.present_logical_raster){
      engine->config.present_logical_raster=want;
      /* The presented extent is derived state; force the next frame to republish it. */
      engine->fps_room=-1;
    }
  }
  if(f&ANYGM_CONFIG_CLEAR_LOCAL_DATA)
    engine->config.clear_local_data=delta->values.clear_local_data?1u:0u;
  if(engine->lifecycle==ENGINE_LOADED){
    poll_option_updates(engine);
    engine->vm.god_mode=core_opt_god(engine);
  }
  return ANYGM_OK;
}

AnygmResult anygm_set_runtime_override(AnygmEngine *engine,uint32_t slot,uint32_t enabled,
                                       const char *expression){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED)
    return ANYGM_ERROR_INVALID_STATE;
  if(slot>=GML_MAX_CHEATS ||
     (enabled && (!expression || !expression[0] ||
                  strlen(expression)>ANYGM_MAX_RUNTIME_OVERRIDE_EXPRESSION)))
    return ANYGM_ERROR_INVALID_ARGUMENT;
  engine_override_set(engine,slot,enabled!=0,expression?expression:"");
  return ANYGM_OK;
}

size_t anygm_state_size(AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED) return 0;
  return engine_state_size(engine);
}

AnygmResult anygm_state_save(AnygmEngine *engine,void *data,size_t capacity,size_t *written){
  if(written) *written=0;
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED || !data)
    return ANYGM_ERROR_INVALID_STATE;
  return engine_state_save(engine,data,capacity,written)?ANYGM_OK:ANYGM_ERROR_OUT_OF_MEMORY;
}

AnygmResult anygm_state_load(AnygmEngine *engine,const void *data,size_t size){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED || !data)
    return ANYGM_ERROR_INVALID_STATE;
  return engine_state_load(engine,data,size)?ANYGM_OK:ANYGM_ERROR_STATE_MISMATCH;
}

size_t anygm_get_last_error(const AnygmEngine *engine,char *message,size_t capacity){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD) return 0;
  size_t required=strlen(engine->last_error)+1;
  if(message && capacity){
    size_t copy=required<capacity?required:capacity;
    memcpy(message,engine->last_error,copy-1);
    message[copy-1]=0;
  }
  return required;
}
