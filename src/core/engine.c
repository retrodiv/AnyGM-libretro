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
#include "gml_render_state.h"
#include "gml_audio.h"
#include "gmlc_package.h"
#include "gmlc_classic_project.h"
#include "gmlc_classic_import.h"
#include "gmlc_project.h"
#include "content_router.h"
#include "anygm_compatibility.h"
#include "anygm_host.h"
#include "engine_internal.h"

/* Does a comma-separated list of frame indices name this one? An empty list names none, and the
 * single word "all" names every frame, which is what a caller wants when hunting for the frame a
 * divergence begins on rather than checking one already known. */
static int frame_list_selects(const char *list, long frame){
  if(!list || !*list) return 0;
  if(!strcmp(list,"all")) return 1;
  for(const char *at = list; *at; ){
    while(*at==' '||*at==',') at++;
    if(!*at) break;
    char *end = NULL;
    long value = strtol(at,&end,10);
    if(end==at) break;
    if(value==frame) return 1;
    at = end;
  }
  return 0;
}

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
  const char *fmt = "[profile] frames=%d avg_ms total=%.3f input=%.3f step=%.3f clear=%.3f draw=%.3f gui=%.3f video=%.3f audio=%.3f present=%.3f max=%.2fms@f%ld\n";
  engine_logf(engine,ANYGM_LOG_INFO,fmt,engine->profile.frames,engine->profile.total_ms/f,engine->profile.input_ms/f,
              engine->profile.step_ms/f,engine->profile.clear_ms/f,engine->profile.draw_ms/f,engine->profile.gui_ms/f,
              engine->profile.video_ms/f,engine->profile.audio_ms/f,engine->profile.present_ms/f,
              engine->profile.max_ms,engine->profile.max_frame);
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

static void boot_runtime_prepare(AnygmEngine *engine);
static void boot_runtime_start(AnygmEngine *engine);
static void boot_runtime(AnygmEngine *engine);
static AnygmResult engine_apply_game_change(AnygmEngine *engine,int *changed);

typedef struct {
  GmlWin win;
  AnygmContentFacts facts;
  AnygmCompatibilityProfile compatibility;
  char loaded_path[1024];
  /* Collected configuration text becomes an effective program before adoption.
   * Retain the launch envelope separately so payload selectors never migrate
   * across an internal content replacement. Both texts are bounded to 4 KiB
   * after semantic merging and validated before running content is torn down. */
  char content_overrides[ANYGM_CONTENT_MAX_OVERRIDE_LAYERS_BYTES];
  char anchor_overrides[ANYGM_CONTENT_MAX_OVERRIDE_LAYERS_BYTES];
  CheatSlot boot_cheats[GML_MAX_CHEATS];
  int boot_cheat_count;
} EnginePreparedContent;

static AnygmResult engine_prepare_content(AnygmEngine *engine,
                                          const AnygmContentSource *source,
                                          EnginePreparedContent *prepared,const char *inherited_overrides){
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
    AnygmContentRouter router={0};
    router.host=&engine->host;
    router.cache_directory=source->cache_directory;
    router.system_directory=source->system_directory;
    router.inherited_overrides=inherited_overrides;
    router.anchor_overrides=prepared->anchor_overrides;
    router.anchor_overrides_size=sizeof prepared->anchor_overrides;
    router.log=content_router_log;
    router.log_userdata=engine;
    /* Disabled overrides do not trigger adjacent-anchor discovery. */
    router.sibling_anchor_overrides=engine->config.content_overrides?1:0;
    /* Resolve an anchor's referenced payload identity before classifying classic format and choosing the content-file directory. Direct loads retain their own input path. */
    char origin_path[1536];
    const char *origin=source->path;
    {
      AnygmContentRouter origin_router={0};
      origin_router.host=&engine->host;
      origin_router.cache_directory=source->cache_directory;
      if(anygm_content_identity_path(&origin_router,source->path,origin_path,sizeof origin_path))
        origin=origin_path;
    }
    size_t plen=strlen(origin);
    classic_input=
        (plen>4 && !strcasecmp(origin+plen-4,".gmd")) ||
        (plen>4 && !strcasecmp(origin+plen-4,".gmk")) ||
        (plen>5 && !strcasecmp(origin+plen-5,".gm81")) ||
        (plen>4 && !strcasecmp(origin+plen-4,".gm6")) ||
        (plen>4 && !strcasecmp(origin+plen-4,".exe"));
    char asset_root[1024];
    AnygmContentResolveResult resolve_result=anygm_content_resolve_path(
      &router,source->path,content,sizeof content,asset_root,sizeof asset_root,
      prepared->content_overrides,sizeof prepared->content_overrides);
    if(resolve_result!=ANYGM_CONTENT_RESOLVE_OK){
      if(resolve_result==ANYGM_CONTENT_RESOLVE_UNSUPPORTED){
        engine_errorf(engine,ANYGM_ERROR_UNSUPPORTED,
                      "Unsupported executable Cabinet profile: %s",
                      origin);
        return ANYGM_ERROR_UNSUPPORTED;
      }
      engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,"Failed to resolve content path: %s",source->path);
      return ANYGM_ERROR_INVALID_CONTENT;
    }
    load_rc=anygm_content_load_win(&router,&prepared->win,content,asset_root,
                                   prepared->loaded_path,sizeof prepared->loaded_path);
    if(!load_rc){
      engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,"Failed to load content: %s",content);
      return ANYGM_ERROR_INVALID_CONTENT;
    }
    if(load_rc==2)
      engine_logf(engine,ANYGM_LOG_WARN,
                  "The selected payload has no executable code; using sibling payload: %s\n",
                  prepared->loaded_path);
    /* A generated payload does not sit beside the files the content opens by path. The classic
     * input keeps its own directory; a container reports where it left the extracted assets. */
    if(asset_root[0]){
      if(snprintf(prepared->win.content_dir,sizeof prepared->win.content_dir,"%s",asset_root)>=
         (int)sizeof prepared->win.content_dir){
        engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,
                      "The extracted asset directory path is too long: %s",asset_root);
        return ANYGM_ERROR_INVALID_CONTENT;
      }
    }else if(classic_input)
      anygm_content_path_parent(origin,prepared->win.content_dir,
                                sizeof prepared->win.content_dir);
  } else {
    AnygmContentRouter router={0};
    router.host=&engine->host;
    router.system_directory=source->system_directory;
    router.log=content_router_log;
    router.log_userdata=engine;
    router.inherited_overrides=inherited_overrides;
    router.anchor_overrides=prepared->anchor_overrides;
    router.anchor_overrides_size=sizeof prepared->anchor_overrides;
    uint8_t *normalized=NULL; size_t normalized_size=0;
    if(!anygm_content_prepare_memory(&router,source->data,source->size,&normalized,&normalized_size,
         prepared->content_overrides,sizeof prepared->content_overrides)){
      engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,"Memory content configuration was rejected");
      return ANYGM_ERROR_INVALID_CONTENT;
    }
    if(gml_win_from_mem(&prepared->win,normalized?normalized:(uint8_t *)(uintptr_t)source->data,
                        normalized?normalized_size:source->size,0)!=0){
      free(normalized);
      engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,
                    "The memory source is not a supported normalized content image");
      return ANYGM_ERROR_INVALID_CONTENT;
    }
    /* The reader borrows during validation. Transfer the private result only
     * after success, so early header rejection and partial-reader cleanup agree. */
    if(normalized) prepared->win.owns=1;
    prepared->win.host=&engine->host;
    snprintf(prepared->loaded_path,sizeof prepared->loaded_path,"%s",
             source->path&&source->path[0]?source->path:"memory image");
    if(source->path&&source->path[0])
      anygm_content_path_parent(source->path,prepared->win.content_dir,
                                sizeof prepared->win.content_dir);
  }
  /* Give each content identity a stable writable namespace under the host-provided root. An
   * anchor aliases its referenced payload, so both entry paths share the same save namespace. */
  {
    const char *base=source->save_directory;
    engine->state_peak_enabled=(base&&base[0])?1:0;
    if(base&&base[0]){
      const char *identity=source->path&&source->path[0]?source->path:NULL;
      char identity_path[1536];
      if(identity){
        AnygmContentRouter identity_router={0};
        identity_router.host=&engine->host;
        identity_router.cache_directory=source->cache_directory;
        if(anygm_content_identity_path(&identity_router,identity,identity_path,
                                       sizeof identity_path))
          identity=identity_path;
      }
      char label[128];
      if(identity) anygm_content_save_label(identity,label,sizeof label);
      else snprintf(label,sizeof label,"memory");
      uint32_t namespace_hash=identity?anygm_content_path_hash(identity):
        (uint32_t)state_hash_bytes(source->data,source->size);
      snprintf(prepared->win.save_dir,sizeof prepared->win.save_dir,"%s/AnyGM/%s-%08x",
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
  char override_error[256]={0};
  if(!engine_boot_overrides_parse(prepared->content_overrides,prepared->boot_cheats,
                                  &prepared->boot_cheat_count,
                                  override_error,sizeof override_error)){
    engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,"Content overrides rejected: %s",
                  override_error[0]?override_error:"unrecognized directive");
    gml_win_free(&prepared->win);
    return ANYGM_ERROR_INVALID_CONTENT;
  }
  if(!engine_boot_overrides_text(prepared->boot_cheats,prepared->boot_cheat_count,
       prepared->content_overrides,ANYGM_CONTENT_MAX_ANCHOR_BYTES)){
    engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,"Effective content overrides exceed 4 KiB");
    gml_win_free(&prepared->win); return ANYGM_ERROR_INVALID_CONTENT;
  }
  CheatSlot *anchor_slots=calloc(GML_MAX_CHEATS,sizeof *anchor_slots);
  int anchor_count=0;
  int anchor_ok=anchor_slots && engine_boot_overrides_parse(prepared->anchor_overrides,
    anchor_slots,&anchor_count,override_error,sizeof override_error) &&
    engine_boot_overrides_text(anchor_slots,anchor_count,prepared->anchor_overrides,
                               ANYGM_CONTENT_MAX_ANCHOR_BYTES);
  free(anchor_slots);
  if(!anchor_ok){
    engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,"Launch overrides rejected: %s",override_error);
    gml_win_free(&prepared->win); return ANYGM_ERROR_INVALID_CONTENT;
  }
  return ANYGM_OK;
}

static void engine_adopt_boot_overrides(AnygmEngine *engine,
                                        const EnginePreparedContent *prepared){
  memcpy(engine->boot_cheats,prepared->boot_cheats,sizeof engine->boot_cheats);
  engine->boot_cheat_count=prepared->boot_cheat_count;
  memcpy(engine->content_overrides_text,prepared->content_overrides,
         strlen(prepared->content_overrides)+1u);
  memcpy(engine->launch_overrides_text,prepared->anchor_overrides,
         strlen(prepared->anchor_overrides)+1u);
  engine->introskip_enabled=-1;
  engine->vm.os_type_declared=engine_overrides_declared_os_type(engine);
  engine_override_menu_refresh(engine);
  if(engine->boot_cheat_count)
    engine_logf(engine,ANYGM_LOG_INFO,"Content overrides: %d effective directive(s)%s\n",
                engine->boot_cheat_count,
                engine->config.content_overrides?"":" (disabled by configuration)");
}

static AnygmResult engine_load_content_prepare(AnygmEngine *engine,
                                               const AnygmContentSource *source,
                                               const AnygmLoadConfig *config,
                                               AnygmContentInfo *info) {
  engine->state_just_loaded = 0;
  snprintf(engine->language,sizeof engine->language,"%s",config&&config->language&&config->language[0]?config->language:"en");
  snprintf(engine->region,sizeof engine->region,"%s",config&&config->region&&config->region[0]?config->region:"US");
  snprintf(engine->language_tag,sizeof engine->language_tag,"%s",config&&config->language_tag&&config->language_tag[0]?config->language_tag:"en-US");
  EnginePreparedContent prepared;
  AnygmResult result=engine_prepare_content(engine,source,&prepared,NULL);
  if(result!=ANYGM_OK) return result;
  engine->win=prepared.win;
  engine->content_facts=prepared.facts;
  engine->compatibility=prepared.compatibility;
  engine->win.compatibility=&engine->compatibility;
  snprintf(engine->content_cache_directory,sizeof engine->content_cache_directory,"%s",
           source->cache_directory?source->cache_directory:"");
  snprintf(engine->content_system_directory,sizeof engine->content_system_directory,"%s",
           source->system_directory?source->system_directory:"");
  snprintf(engine->content_launch_path,sizeof engine->content_launch_path,"%s",
           source->kind==ANYGM_CONTENT_PATH && source->path?source->path:"");
  snprintf(engine->current_content_path,sizeof engine->current_content_path,"%s",
           prepared.loaded_path);
  snprintf(engine->content_program_directory,sizeof engine->content_program_directory,"%s",
           engine->win.content_dir);
  engine->state_content_locator[0]='\0';
  engine->launch_parameters[0]='\0';
  engine_adopt_boot_overrides(engine,&prepared);
  state_identity_refresh(engine);
  engine_state_peak_load(engine);
  engine->loaded = 1;
  engine_logf(engine,ANYGM_LOG_INFO,"Loaded content: bytecode=%u rooms=%d code=%d\n",
              engine->win.bytecode,gml_room_count(&engine->win),engine->win.n_code);
  engine->full_game_on_initial_boot=0;
  boot_runtime_prepare(engine);
  if(info){
    info->flags=gml_render_content_device_candidate_present(&engine->render)
      ?ANYGM_CONTENT_GLSL_DEVICE_CANDIDATE:0u;
  }
  return ANYGM_OK;
}
/* Prepare the runtime from the already-loaded data.win without executing content events. Keeping
 * this boundary before extension scripts and room entry lets a host inspect the shader catalogue,
 * negotiate a session-long context, and only then expose the final shader policy to Create. */
static void boot_runtime_prepare(AnygmEngine *engine) {
  /* A reset is a cold boot. Keep engine time on the same timeline as an initial load. */
  { engine->vm.frame = 0; }
  engine->classic_compositor = 0;
  engine->state_reapply_size = 0;
  engine->state_frame_available = 0;
  engine->input_continuity_pending = 0;
  engine->state_frame_width = engine->state_frame_height = 0;
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
  engine->mouse_warped = 0;
  engine->mouse_host_x = 0; engine->mouse_host_y = 0;
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
  /* Launch resets VM fields, so install the declaration before the first event. */
  engine->vm.os_type_declared=engine_overrides_declared_os_type(engine);
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
  engine->first_generation_app_owned=0;
  engine->wide_app_restore_width=engine->wide_app_restore_height=0;
  if(anygm_policy_uses_first_generation_studio(&engine->win)){
    if(gml_render_application_surface_ensure_owned(
         &engine->render,(int)engine->width,(int)engine->height))
      engine->first_generation_app_owned=1;
    else
      engine_logf(engine,ANYGM_LOG_WARN,
        "[anygm] could not allocate the first-generation application surface\n");
  }
  GmlRenderControl render_control={
    .monitor_width=core_opt_monitor_size(engine,0),
    .monitor_height=core_opt_monitor_size(engine,1),
    .shader_report_all_compiled=engine->config.report_all_shaders_compiled?1:0,
    .shader_device_expected=engine_graphics_device_expected(engine)
  };
  gml_render_control_update(&engine->render,&render_control,GML_RENDER_CONTROL_HOST_OPTIONS);
  if(anygm_host_development_setting(&engine->host,"GML_LOG_MONITOR"))
    engine_logf(engine,ANYGM_LOG_DEBUG,"[monitor] %dx%d\n",
                render_control.monitor_width,render_control.monitor_height);
  engine->vm.render = &engine->render;
  (void)gml_vm_software3d_ensure(&engine->vm);
  engine->vm.draw_event_hook = aspect_draw_event_hook;
  engine->vm.draw_event_hook_user = engine;
  engine->vm.room_layer_hook = screen_redraw_room_layer_hook;
  engine->vm.room_layer_hook_user = engine;
  engine->vm.present_latch_hook = screen_refresh_present_latch_hook;
  engine->vm.present_latch_hook_user = engine;
  engine->audio = gml_audio_create(&engine->win);
  engine->vm.audio = engine->audio;
}

/* Start the prepared runtime. Everything below this boundary can execute authored code. */
static void boot_runtime_start(AnygmEngine *engine) {
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
  /* Run declared extension initialization before entering the first room. */
  gml_vm_run_extension_init_scripts(&engine->vm);
  int initial_boot_guard = engine->full_game_on_initial_boot;
  core_opt_redirect_room_order(engine);
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

/* A reset and an in-content game change already own their session policy and context, so they run
 * the two phases back to back. Initial host loading uses the phases separately. */
static void boot_runtime(AnygmEngine *engine) {
  boot_runtime_prepare(engine);
  boot_runtime_start(engine);
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

int engine_state_content_locator_valid(const char *locator){
  if(!locator) return 0;
  if(!locator[0]) return 1;
  if(locator[0]=='/' || locator[0]=='\\') return 0;
  const char *segment=locator;
  for(const char *cursor=locator;;cursor++){
    unsigned char c=(unsigned char)*cursor;
    if(c && (c=='\\' || c<0x20 || c==':' || c==0x7f)) return 0;
    if(c=='/' || !c){
      size_t length=(size_t)(cursor-segment);
      if(!length || (length==1 && segment[0]=='.') ||
         (length==2 && segment[0]=='.' && segment[1]=='.')) return 0;
      if(!c) break;
      segment=cursor+1;
    }
  }
  return 1;
}

static int engine_state_locator_from_target(const char *program_directory,const char *target,
                                            char *locator,size_t capacity){
  if(!program_directory || !program_directory[0] || !target || !locator || !capacity) return 0;
  size_t prefix=strlen(program_directory);
  while(prefix && (program_directory[prefix-1]=='/' || program_directory[prefix-1]=='\\'))
    prefix--;
  if(!prefix || strncmp(program_directory,target,prefix) ||
     (target[prefix]!='/' && target[prefix]!='\\')) return 0;
  const char *relative=target+prefix;
  while(*relative=='/' || *relative=='\\') relative++;
  size_t written=0;
  int previous_separator=0;
  for(;*relative;relative++){
    char c=*relative;
    if(c=='/' || c=='\\'){
      if(previous_separator) continue;
      c='/';
      previous_separator=1;
    } else previous_separator=0;
    if(written+1>=capacity) return 0;
    locator[written++]=c;
  }
  if(written && locator[written-1]=='/') written--;
  locator[written]='\0';
  return engine_state_content_locator_valid(locator);
}

static void engine_rebind_moved_runtime(AnygmEngine *engine){
  if(!engine) return;
  engine->win.host=&engine->host;
  engine->win.compatibility=&engine->compatibility;
  gml_vm_rebind(&engine->vm,&engine->win,&engine->host);
  engine_input_bind(engine);
  gml_render_rebind_content(&engine->render,&engine->win);
  engine->vm.render=&engine->render;
  gml_render_bind_software3d(&engine->render,engine->vm.software3d);
  gml_audio_rebind_content(engine->audio,&engine->win);
  engine->vm.audio=engine->audio;
  engine->vm.draw_event_hook=aspect_draw_event_hook;
  engine->vm.draw_event_hook_user=engine;
  engine->vm.room_layer_hook=screen_redraw_room_layer_hook;
  engine->vm.room_layer_hook_user=engine;
  engine->vm.present_latch_hook=screen_refresh_present_latch_hook;
  engine->vm.present_latch_hook_user=engine;
}

AnygmResult engine_state_stage_content(AnygmEngine *engine,const char *locator,
                                       const char *parameters,AnygmEngine **out_staged){
  if(!engine || !out_staged || !engine_state_content_locator_valid(locator))
    return ANYGM_ERROR_INVALID_STATE;
  *out_staged=NULL;
  char target[2048];
  if(locator[0]){
    if(!game_change_target_path(engine->content_program_directory,"",locator,
                                target,sizeof target))
      return ANYGM_ERROR_INVALID_STATE;
  } else {
    if(!engine->content_launch_path[0]) return ANYGM_ERROR_INVALID_STATE;
    snprintf(target,sizeof target,"%s",engine->content_launch_path);
  }

  AnygmEngine *staged=NULL;
  AnygmResult result=anygm_create(&engine->host,&staged);
  if(result!=ANYGM_OK) return result;
  staged->config=engine->config;
  staged->locale_from_host=engine->locale_from_host;
  snprintf(staged->language,sizeof staged->language,"%s",engine->language);
  snprintf(staged->region,sizeof staged->region,"%s",engine->region);
  snprintf(staged->language_tag,sizeof staged->language_tag,"%s",engine->language_tag);
  staged->frame_flags=engine->frame_flags;
  staged->profile_enabled=engine->profile_enabled;
  staged->profile=engine->profile;
  staged->diagnostics=engine->diagnostics;
  staged->host_frame_generation=engine->host_frame_generation;
  staged->frame_materializations=engine->frame_materializations;
  staged->screen_pass_frames=engine->screen_pass_frames;
  staged->canvas_pass_frames=engine->canvas_pass_frames;
  if(!ensure_primary_buffers(staged)){
    anygm_destroy(staged);
    return ANYGM_ERROR_OUT_OF_MEMORY;
  }

  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=target;
  source.cache_directory=engine->content_cache_directory[0]
    ?engine->content_cache_directory:NULL;
  source.system_directory=engine->content_system_directory[0]
    ?engine->content_system_directory:NULL;
  EnginePreparedContent prepared;
  result=engine_prepare_content(staged,&source,&prepared,engine->launch_overrides_text);
  if(result!=ANYGM_OK){
    anygm_destroy(staged);
    return result;
  }

  staged->win=prepared.win;
  staged->content_facts=prepared.facts;
  staged->compatibility=prepared.compatibility;
  staged->win.compatibility=&staged->compatibility;
  snprintf(staged->win.save_dir,sizeof staged->win.save_dir,"%s",engine->win.save_dir);
  snprintf(staged->content_cache_directory,sizeof staged->content_cache_directory,"%s",
           engine->content_cache_directory);
  snprintf(staged->content_system_directory,sizeof staged->content_system_directory,"%s",
           engine->content_system_directory);
  snprintf(staged->content_launch_path,sizeof staged->content_launch_path,"%s",
           engine->content_launch_path);
  snprintf(staged->current_content_path,sizeof staged->current_content_path,"%s",
           prepared.loaded_path);
  snprintf(staged->content_program_directory,sizeof staged->content_program_directory,"%s",
           engine->content_program_directory);
  snprintf(staged->state_content_locator,sizeof staged->state_content_locator,"%s",locator);
  snprintf(staged->launch_parameters,sizeof staged->launch_parameters,"%s",
           parameters?parameters:"");
  engine_adopt_boot_overrides(staged,&prepared);
  state_identity_refresh(staged);
  /* A rejected staging engine must not publish cache observations. The accepted engine receives
   * the live session's cache state immediately before commit. */
  staged->state_peak_enabled=0;
  staged->state_peak_hint=engine->state_peak_hint;
  staged->state_peak_persisted=engine->state_peak_persisted;
  staged->state_resume_peak_hint=engine->state_resume_peak_hint;
  staged->state_resume_peak_persisted=engine->state_resume_peak_persisted;
  staged->loaded=1;
  staged->lifecycle=ENGINE_LOADED;
  staged->full_game_on_initial_boot=locator[0]?1:0;
  boot_runtime(staged);
  run_selftest(staged);
  for(int index=0;index<engine->cheat_count;index++){
    const CheatSlot *slot=&engine->cheats[index];
    if(slot->continuation_of) continue;
    engine_override_set(staged,(unsigned)index,slot->enabled!=0,slot->code);
  }
  *out_staged=staged;
  return ANYGM_OK;
}

void engine_state_commit_staged_content(AnygmEngine *engine,AnygmEngine *staged){
  if(!engine || !staged) return;
  /* The graphics target belongs to the frontend session rather than to either content runtime.
   * Move it to the accepted runtime; the rejected shell is then an ordinary software-only engine
   * that can be destroyed without issuing calls into a frontend graphics context. */
  staged->gpu=engine->gpu;
  engine->gpu=NULL;
  AnygmEngine previous=*engine;
  AnygmEngine replacement=*staged;
  *engine=replacement;
  *staged=previous;
  engine_rebind_moved_runtime(engine);
  engine_rebind_moved_runtime(staged);
  anygm_destroy(staged);
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
  char state_locator[sizeof engine->state_content_locator];
  if(!game_change_payload_name(parameters,payload,sizeof payload) ||
     !game_change_target_path(engine->win.content_dir,directory,payload,target,sizeof target) ||
     !engine_state_locator_from_target(engine->content_program_directory,target,
                                       state_locator,sizeof state_locator)){
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
  source.system_directory=engine->content_system_directory[0]
    ?engine->content_system_directory:NULL;
  EnginePreparedContent prepared;
  AnygmResult result=engine_prepare_content(engine,&source,&prepared,engine->launch_overrides_text);
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
  snprintf(engine->state_content_locator,sizeof engine->state_content_locator,"%s",
           state_locator);
  snprintf(engine->launch_parameters,sizeof engine->launch_parameters,"%s",parameters);
  engine_adopt_boot_overrides(engine,&prepared);
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
  engine->state_frame_available=0; engine->input_continuity_pending=0;
  engine->state_frame_width=engine->state_frame_height=0;
  engine->content_fingerprint=0; engine->compatibility_fingerprint=0;
  engine->runtime_ended=0; engine->shutdown_sent=0; engine->loaded=0;
  engine->monitor_override_pending=0;
}

/* Apply the current neutral configuration before the frame. */
static void poll_option_updates(AnygmEngine *engine) {
  GmlRenderControl control={
    .monitor_width=core_opt_monitor_size(engine,0),
    .monitor_height=core_opt_monitor_size(engine,1),
    .shader_report_all_compiled=engine->config.report_all_shaders_compiled?1:0,
    .shader_device_expected=engine_graphics_device_expected(engine)
  };
  gml_render_control_update(&engine->render,&control,GML_RENDER_CONTROL_HOST_OPTIONS);
  if(anygm_host_development_setting(&engine->host,"GML_LOG_MONITOR"))
    engine_logf(engine,ANYGM_LOG_DEBUG,"[monitor] %dx%d\n",
                control.monitor_width,control.monitor_height);
}
/* Fast-forward is an optimization hint only. */
static void poll_fast_forward(AnygmEngine *engine) {
  GmlRenderControl control={.fast_forward=engine->config.fast_forward?1:0};
  gml_render_control_update(&engine->render,&control,GML_RENDER_CONTROL_FAST_FORWARD);
}
static AnygmResult engine_run_frame(AnygmEngine *engine) {
  engine->vm.draw_phase=0;
  engine->audio_frames=0;
  if(engine->vm.game_change_pending)
    return engine_apply_game_change_and_run_frame(engine);
  /* A state carries the completed application frame. Present it once without running game code:
   * the canonical simulation state is from after Post Draw, so executing Draw again cannot
   * reconstruct a transient that was removed at that boundary. The next call resumes normally
   * from the restored post-frame state. */
  if(engine->state_just_loaded && engine->state_frame_available){
    engine->output_width=engine->state_frame_width;
    engine->output_height=engine->state_frame_height;
    engine->state_just_loaded=0;
    engine->state_reapply_size=0;
    engine->state_frame_available=0;
    engine->have_presented_frame=1;
    engine->fps_room=-1;
    return ANYGM_OK;
  }
  engine_overrides_presentation_apply(engine);
  /* A live monitor-size change arrives before input. Resolve its host-to-content transform now so
   * the first pointer sample under the new geometry is mapped through the same canvas as video. */
  if(engine->fps_room<0 || !engine->output_width || !engine->output_height)
    sync_room_fps(engine,1);
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
  /* Hosts apply live presentation changes through anygm_set_config; only the fast-forward
   * optimization hint still needs polling every frame. */
  poll_fast_forward(engine);     /* optimization hint; never changes presentation state */
  int prof = profile_enabled(engine);
  double t_total = prof ? profile_now_ms(engine) : 0.0;
  double sp_step = engine->profile.step_ms, sp_draw = engine->profile.draw_ms, sp_gui = engine->profile.gui_ms, sp_video = engine->profile.video_ms;
  double t0 = t_total, t1 = t_total;
  /* Snapshot neutral input, retaining the previous frame for edge detection. */
  memcpy(engine->pad_previous, engine->pad_current, sizeof(engine->pad_current));
  memcpy(engine->key_previous, engine->key_current, sizeof(engine->key_current));
  memcpy(engine->axis_previous, engine->axis_current, sizeof(engine->axis_current));
  /* Copy every reported host port and leave all remaining rows at rest. */
  unsigned pads=engine->input.connected_gamepads;
  if(pads>ANYGM_MAX_GAMEPADS) pads=ANYGM_MAX_GAMEPADS;
  for(unsigned device=0;device<ANYGM_MAX_GAMEPADS;device++){
    for (int b = 0; b < NPAD; b++)
      engine->pad_current[device][b]=
        device<pads && engine->input.gamepad_buttons[device][b] ? 1 : 0;
    for(int axis=0;axis<4;axis++)
      engine->axis_current[device][axis]=
        device<pads ? engine->input.gamepad_axes[device][axis] : 0.0;
  }
  engine_input_poll_keyboard(engine);
  engine_input_poll_mouse(engine);
  engine_input_release_cleared_keys(engine);   /* a cleared key comes back once it has been up */
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
    memset(engine->key_press_raised, 0, sizeof(engine->key_press_raised));
    memset(engine->key_press_carry, 0, sizeof(engine->key_press_carry));
    memset(engine->key_press_step, 0, sizeof(engine->key_press_step));
    memset(engine->key_release_defer, 0, sizeof(engine->key_release_defer));
    memset(engine->hardware_key_current, 0, sizeof(engine->hardware_key_current));
    memset(engine->hardware_key_previous, 0, sizeof(engine->hardware_key_previous));
    memset(engine->event_vk_current, 0, sizeof(engine->event_vk_current));
    memset(engine->event_vk_previous, 0, sizeof(engine->event_vk_previous));
    memset(engine->event_key_current, 0, sizeof(engine->event_key_current));
    memset(engine->event_key_previous, 0, sizeof(engine->event_key_previous));
    memset(engine->key_cleared, 0, sizeof(engine->key_cleared));
    memset(engine->event_key_cleared, 0, sizeof(engine->event_key_cleared));
    memset(engine->axis_current, 0, sizeof(engine->axis_current));
    memset(engine->axis_previous, 0, sizeof(engine->axis_previous));
    memset(engine->mouse_button_current, 0, sizeof(engine->mouse_button_current));
    memset(engine->mouse_button_previous, 0, sizeof(engine->mouse_button_previous));
    engine->mouse_wheel = 0;
  } else if(engine->input_continuity_pending){
    /* First advancing frame after a load: the current arrays were just polled from the live host,
     * while the previous halves still hold the zeros the load wrote. A key held across the load is
     * a continuation, not a new press, so previous mirrors current for exactly this frame. */
    engine->input_continuity_pending = 0;
    memcpy(engine->hardware_key_previous, engine->hardware_key_current, sizeof(engine->hardware_key_current));
    memcpy(engine->event_vk_previous, engine->event_vk_current, sizeof(engine->event_vk_current));
    memcpy(engine->event_key_previous, engine->event_key_current, sizeof(engine->event_key_current));
    memcpy(engine->mouse_button_previous, engine->mouse_button_current, sizeof(engine->mouse_button_current));
    engine->mouse_wheel = 0;
  }
  if (anygm_host_development_setting(&engine->host,"GML_DBG_PAD")) { engine->diagnostics.pad_frame++;
    unsigned bits = 0; for (int b = 0; b < NPAD; b++) if (engine->pad_current[0][b]) bits |= 1u << b;
    if (bits) engine_logf(engine,ANYGM_LOG_DEBUG, "[pad] f%d bits=%04x\n", engine->diagnostics.pad_frame, bits); }
  if(prof){ t1 = profile_now_ms(engine); engine->profile.input_ms += t1 - t0; t0 = t1; }
  engine->state_just_loaded = 0;
  sync_room_fps(engine,1);
  apply_monitor_overrides(engine);
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
    gml_render_control_update(&engine->render,&control,GML_RENDER_CONTROL_WIDE_ASPECT);
    /* A core-owned first-generation application surface follows the reported widened window
     * extent while wide aspect is active. Restore the prior extent when widening stops. */
    if(engine->first_generation_app_owned){
      GmlRenderPresentationMetrics wide_presentation;
      gml_render_presentation_metrics(&engine->render,&wide_presentation);
      if(fw && wide_presentation.application_owned && engine->width>0 && engine->height>0 &&
         (wide_presentation.application_width!=(int)engine->width ||
          wide_presentation.application_height!=(int)engine->height)){
        if(engine->wide_app_restore_width<=0){
          engine->wide_app_restore_width=wide_presentation.application_width;
          engine->wide_app_restore_height=wide_presentation.application_height;
        }
        (void)gml_render_application_surface_ensure_owned(
          &engine->render,(int)engine->width,(int)engine->height);
      } else if(!fw && engine->wide_app_restore_width>0){
        if(wide_presentation.application_owned)
          (void)gml_render_application_surface_ensure_owned(
            &engine->render,engine->wide_app_restore_width,engine->wide_app_restore_height);
        engine->wide_app_restore_width=engine->wide_app_restore_height=0;
      }
    } }
  /* Record whether a wait preceded this frame. A newly parked event may complete its draw; later held frames retain the completed picture. */
  int wait_held_frame = engine->vm.wait.active;
  if(run_step){
    AspectViewOverlay step_ov;
    aspect_view_overlay_begin(engine,&step_ov, 0, ASPECT_VIEW_TRACKING);
    /* For a classic frame with a retained target, bind the new presented base before Step. The active target remains bound until the program resets it. */
    if(anygm_policy_uses_classic_runtime(&engine->win) &&
       gml_surface_get_target(&engine->render)>=0 && engine->screen &&
       engine->output_width>0 && engine->output_height>0)
      gml_render_begin_retaining_target(&engine->render,engine->screen,
                                        (int)engine->output_width,(int)engine->output_height,
                                        0.0,0.0);
    gml_vm_step(&engine->vm);
    if(anygm_policy_uses_classic_runtime(&engine->win) && room_before_step>=0 && engine->vm.room_index!=room_before_step){
      int kind=(int)lround(gml_global_num(&engine->vm,"transition_kind"));
      int steps=(int)lround(gml_global_num(&engine->vm,"transition_steps"));
      if(kind!=21 || !classic_transition_start(engine,transition_old_w,transition_old_h,steps))
        gml_set_global_scalar(&engine->vm,"transition_kind",0);
    }
    aspect_view_overlay_end(engine,&step_ov, 1);
    /* Before the geometry this frame publishes is read, not after: see the function's own note. */
    engine_overrides_room_scope_apply(engine);
    sync_room_fps(engine,1);
    apply_sticky_cheats(engine);   /* generic freeze cheats (user-supplied global writes) */
    menu_run(engine);              /* generic pause-menu editor (inject entries + handle input) */
    room_skip_hook(engine);        /* generic room-skip button (Select/Start, any room) */
    introskip_hook(engine);        /* A/B skip, only in a user-supplied intro-room list (GML_INTROSKIP) */
  }
  /* While a prior wait remains active, skip Step and Draw while mixing audio and updating input history. */
  if(wait_held_frame && engine->vm.wait.active){
    engine->audio_accumulator += 44100.0 / (engine->fps>0.0?engine->fps:60.0);
    int held_frames = (int)engine->audio_accumulator;
    engine->audio_accumulator -= held_frames;
    if(held_frames>4096) held_frames=4096;
    if(held_frames>0){
      gml_audio_mix(engine->audio,engine->audio_output,held_frames);
      engine->audio_frames=(size_t)held_frames;
    }
    memcpy(engine->event_vk_previous, engine->event_vk_current, sizeof(engine->event_vk_current));
    memcpy(engine->event_key_previous, engine->event_key_current, sizeof(engine->event_key_current));
    return ANYGM_OK;
  }
  /* Everything past the step draws. The deterministic clock keeps running through it so a Draw
   * event can still time out of a loop, but from here its advance is scratch: the frame a state
   * draws must not depend on whether the step ran, because after a load it did not. */
  engine->vm.time_sample_draw_ms=engine->vm.time_sample_cpu_ms;
  /* Content that composes its own screen leaves a surface bound when its step events end, so the
   * draw phase is meant to fill that surface rather than the framebuffer. gml_render_begin drops
   * the binding, and this line is how you see that it was there: a target other than -1 here names
   * a frame whose author expected to compose it, and everything such content draws into its own
   * surface is discarded. */
  if(anygm_host_development_setting(&engine->host,"GML_LOG_STEPTARGET")){
    GmlRenderTargetMetrics m_={0};
    gml_render_target_metrics(&engine->render,&m_);
    engine_logf(engine,ANYGM_LOG_DEBUG,
      "[steptarget] f%ld target=%d fbsize=%dx%d enginefb=%p %ux%u screen=%p out=%ux%u\n",
      engine->vm.frame,gml_surface_get_target(&engine->render),m_.width,m_.height,
      (const void*)engine->fb,engine->width,engine->height,(const void*)engine->screen,
      engine->output_width,engine->output_height);
  }
  /* A classic surface target held after Step directs the draw phase into that target. The base canvas retains the program's own composition for this frame. */
  engine->composed_frame=anygm_policy_uses_classic_runtime(&engine->win) &&
                         gml_surface_get_target(&engine->render)>=0 &&
                         !anygm_host_development_setting(&engine->host,"GML_NO_CRT");
  /* A classic surface target held after Step directs the draw phase into that target. Preserve its explicit composition for the current frame while keeping automatic application drawing enabled for later frames. */
  if(engine->composed_frame){ engine->content_presented=1; engine->classic_compositor=1; }
  engine->vm.draw_phase=1;
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
    /* A digest of what the runtime holds, for the frames a caller names. Pixels do not see
     * everything: a change to event ordering or to how much of the random sequence is consumed can
     * leave every compared frame identical and still put the run somewhere else entirely a few
     * hundred frames later. This is the one measurement that catches that.
     *
     * Object identity is numeric here on purpose. The index is what the runtime has; interpreting
     * it belongs to the caller-supplied content and stays outside this generic diagnostic. */
    const char *frames = anygm_host_development_setting(&engine->host,"GML_STATE_DIGEST");
    long digest_frame = engine->diagnostics.state_digest_frame++;
    if (frames && *frames && frame_list_selects(frames, digest_frame)) {
      int instances = 0, alarms = 0;
      for (int i = 0; i < engine->vm.inst_count; i++) {
        GmlInstance *o = &engine->vm.inst[i];
        if (!o->active || o->marked) continue;
        instances++;
        for (int a = 0; a < GML_ALARMS; a++)
          if (o->alarm[a] >= 0) { alarms++; break; }
      }
      GmlPresentView views[8];
      int canvas_width = 0, canvas_height = 0;
      int view_count = present_view_count(engine, views, &canvas_width, &canvas_height);
      GmlPresentView view;
      memset(&view,0,sizeof view);
      if (view_count > 0) view = views[0];
      engine_logf(engine,ANYGM_LOG_DEBUG,
        "[state] frame %ld  room=%d  instances=%d  rng_calls=%llu  view=%d,%d,%d,%d  alarms=%d\n",
        digest_frame, engine->vm.room_index, instances,
        (unsigned long long)engine->vm.diagnostics.rng_calls,
        (int)view.x, (int)view.y, (int)view.w, (int)view.h, alarms);
      /* Counts per object index, ascending, so two digests compare as text without being sorted
       * again by whoever reads them. */
      if (engine->vm.n_objects > 0) {
        int *counts = (int*)calloc((size_t)engine->vm.n_objects, sizeof(int));
        if (counts) {
          for (int i = 0; i < engine->vm.inst_count; i++) {
            GmlInstance *o = &engine->vm.inst[i];
            if (!o->active || o->marked || o->obj < 0 || o->obj >= engine->vm.n_objects) continue;
            counts[o->obj]++;
          }
          char line[1024];
          size_t at = 0;
          at += (size_t)snprintf(line + at, sizeof(line) - at, "[state]   counts");
          for (int i = 0; i < engine->vm.n_objects && at + 24 < sizeof(line); i++)
            if (counts[i])
              at += (size_t)snprintf(line + at, sizeof(line) - at, " %d:%d", i, counts[i]);
          engine_logf(engine,ANYGM_LOG_DEBUG,"%s\n",line);
          free(counts);
        }
      }
    }
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
          /* GML_LOG_OBJ_VARS names comma-separated instance variables to print beside the fixed
           * fields, with "bbox" reserved for collision bounds. This exposes optional content-side
           * state alongside the engine fields selected by GML_LOG_OBJ. */
          {
            const char *want = anygm_host_development_setting(&engine->host,"GML_LOG_OBJ_VARS");
            if (want && *want) {
              char line[1024]; size_t at = 0; line[0]=0;
              const char *cursor = want;
              while (*cursor && at + 64 < sizeof line) {
                const char *comma = strchr(cursor, ',');
                size_t len = comma ? (size_t)(comma - cursor) : strlen(cursor);
                char key[96];
                if (len >= sizeof key) len = sizeof key - 1;
                memcpy(key, cursor, len); key[len] = 0;
                if (!strcmp(key, "bbox")) {
                  double bl,bt,br,bb;
                  if (gml_vm_instance_bbox(&engine->vm,o,&bl,&bt,&br,&bb))
                    at += (size_t)snprintf(line+at, sizeof line-at, " bbox=[%.1f %.1f %.1f %.1f]", bl,bt,br,bb);
                  else at += (size_t)snprintf(line+at, sizeof line-at, " bbox=<none>");
                  cursor = comma ? comma + 1 : cursor + strlen(cursor);
                  continue;
                }
                GmlVal *slot = gml_varmap_get(&o->vars, key);
                if (!slot) at += (size_t)snprintf(line+at, sizeof line-at, " %s=<absent>", key);
                else if (slot->t == V_REAL) at += (size_t)snprintf(line+at, sizeof line-at, " %s=%.4g", key, slot->d);
                else if (slot->t == V_STR) at += (size_t)snprintf(line+at, sizeof line-at, " %s=\"%s\"", key, slot->s?slot->s:"");
                else if (slot->t == V_ARR) at += (size_t)snprintf(line+at, sizeof line-at, " %s=[%d]", key, gml_val_array_length(*slot));
                else at += (size_t)snprintf(line+at, sizeof line-at, " %s=undefined", key);
                cursor = comma ? comma + 1 : cursor + strlen(cursor);
              }
              engine_logf(engine,ANYGM_LOG_DEBUG, "[objv] f%d %s id=%u%s\n",
                          engine->diagnostics.object_frame, on, o->id, line);
            }
          }
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
  aspect_surface_canvas_update(engine,cam_x);
  engine->aspect_draw_full_x = cam_x;
  engine->aspect_draw_full_y = cam_y;
  engine->aspect_event_view_stack_pointer = 0;
  engine->aspect_event_view_overflow = 0;
  { GmlRenderControl control={.fast_alpha_cull=core_opt_fast_alpha_cull(engine)};
    gml_render_control_update(&engine->render,&control,GML_RENDER_CONTROL_FAST_ALPHA); }
  if(!engine->output_width || !engine->output_height) compute_present(engine);
  GmlRenderPresentationMetrics render_presentation={0};
  gml_render_presentation_metrics(&engine->render,&render_presentation);
  if(!render_presentation.application_owned && frame_view_count==1 &&
     default_application_surface_uses_full_view_port(
       engine,frame_view_count,
       frame_views[0].px,frame_views[0].py,frame_views[0].pw,frame_views[0].ph,
       (int)engine->width,(int)engine->height,
       render_presentation.application_width,render_presentation.application_height) &&
     gml_render_application_surface_ensure_owned(
       &engine->render,frame_views[0].pw,frame_views[0].ph))
    gml_render_presentation_metrics(&engine->render,&render_presentation);
  /* With the application surface enabled, Studio's Draw events target that surface and the runner
   * presents it. We used to fill the base canvas and leave surface 0 untouched, so content that
   * composites surface 0 itself composited a surface nothing had painted. GML_NO_APP_SURFACE_TARGET
   * restores the older narrowing, which is kept because a policy this wide should be answerable
   * without a rebuild. */
  int app_surface_is_draw_target =
    anygm_host_development_setting(&engine->host,"GML_NO_APP_SURFACE_TARGET")==NULL;
  /* The block above forces ownership only for the one shape it recognises, a sole view whose port
   * covers the default surface. This widens it to every enabled application surface with modern
   * layer semantics, which is what the policy above says. Owning the surface is only half of it:
   * the world still renders into the base canvas unless direct_owned_world fires below. */
  if(!render_presentation.application_owned &&
     render_presentation.application_draw_enabled &&
     anygm_policy_has_modern_layer_semantics(&engine->win) &&
     app_surface_is_draw_target &&
     gml_render_application_surface_ensure_owned(
       &engine->render,(int)engine->width,(int)engine->height))
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
  if(engine->aspect_force_active && aspect_surface_canvas_size(engine,NULL,NULL) &&
     frame_view_count==1 && render_presentation.application_owned &&
     gml_render_application_surface_owned_view(&engine->render,&direct_world_view))
    direct_owned_world=1;
  if (!multiview_rendered) {
  *gml_varmap_put(&engine->vm.globals,"view_current")=vreal(frame_view_index);
  int translated_full_port=frame_view_count==1 &&
    (frame_views[0].px!=0 || frame_views[0].py!=0) &&
    frame_views[0].pw==(int)engine->width && frame_views[0].ph==(int)engine->height &&
    render_presentation.application_width==(int)engine->width &&
    render_presentation.application_height==(int)engine->height;
  if(!engine->aspect_force_active && frame_view_count<=1 &&
     !translated_full_port &&
     render_presentation.application_owned &&
     !(view_surface>0 && gml_surface_exists(&engine->render,view_surface))){
    GmlPresentView *view=&frame_views[0];
    /* A viewless modern room draws into its owned application surface. Drawing only to a
     * smaller logical framebuffer can leave retained pixels outside that raster. */
    int viewless_owned_world = frame_view_count==0 &&
      anygm_policy_has_modern_layer_semantics(&engine->win);
    /* An enabled application surface remains the final draw target. A translated full-size port
     * uses the staged composition below so its offset is applied once, rather than lost by
     * drawing directly over the whole owned surface. This 1:1 translation does not change the
     * established target selection for resized application surfaces or partial ports. */
    direct_owned_world=(viewless_owned_world || app_surface_is_draw_target ||
      (frame_view_count==1 &&
      (default_application_surface_uses_full_view_port(
      engine,frame_view_count,view->px,view->py,view->pw,view->ph,
      (int)engine->width,(int)engine->height,
      render_presentation.application_width,render_presentation.application_height) ||
      application_surface_scales_full_view_port(
      engine,frame_view_count,view->px,view->py,view->pw,view->ph,
      render_presentation.application_width,render_presentation.application_height) ||
      application_surface_matches_first_generation_view_port(
        engine,frame_view_count,view->px,view->py,view->pw,view->ph,
        render_presentation.application_width,render_presentation.application_height)))) &&
      gml_render_application_surface_owned_view(&engine->render,&direct_world_view);
  }
  if(engine->vm.frame<6 &&
     anygm_host_development_setting(&engine->host,"GML_LOG_APPTARGET"))
    engine_logf(engine,ANYGM_LOG_DEBUG,
      "[apptarget] f%ld direct_owned_world=%d multiview=%d view_surface=%d views=%d owned=%d\n",
      engine->vm.frame,direct_owned_world,multiview_rendered,view_surface,frame_view_count,
      render_presentation.application_owned);
  world_pixels=direct_owned_world?direct_world_view.pixels:engine->fb;
  world_width=direct_owned_world?direct_world_view.width:(int)engine->width;
  world_height=direct_owned_world?direct_world_view.height:(int)engine->height;
  if(engine->composed_frame)
    gml_render_begin_retaining_target(&engine->render,world_pixels,world_width,world_height,
                                      cam_x,cam_y);
  else
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
   * application-surface clear. Where the generation allows it and both are disabled, drawing
   * retains the completed framebuffer. */
  if (!have_room || room_clears_application_surface(&engine->win, &rm))
    gml_render_set_pending_fill(&engine->render, engine->background);
  /* Some games draw room backgrounds themselves from GML. When a launcher supplies that renderer
   * object's name, defer to it and avoid double-drawing the engine's static fallback. */
  const char *bg_renderer = anygm_host_development_setting(&engine->host,"GML_BG_RENDERER_OBJ");
  int pobj = (bg_renderer && *bg_renderer) ? gml_object_index_by_name(&engine->vm, bg_renderer) : -1;
  int gml_draws_bg = (pobj >= 0 && gml_find_instance(&engine->vm, pobj) != NULL);
  if (have_room && !gml_draws_bg) draw_runtime_backgrounds(engine,0);
  /* Draw-phase overrides apply only until the draw pass ends. */
  engine_overrides_draw_hold_begin(engine);
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
  if(engine->content_presented){
  } else if(direct_owned_world){
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
        /* When the owned application surface covers the forced framebuffer, a sole full-window
         * view port describes placement within that frame rather than a new fit rectangle. */
        int wide_window_raster = engine->aspect_force_active &&
                                 aw==(int)engine->width && ah==(int)engine->height;
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
                                 scaled_full_port || wide_window_raster);
        if(full_logical_view){ dx=dy=0; dw=aw; dh=ah; }
        /* Camera shake may translate a sole widened port. Its extent still represents the whole
         * widened raster; preserve the translation without fitting that raster back into the
         * authored narrow width. */
        if(one_view && wide_window_raster){ dw=aw; dh=ah; }
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
		  /* An owned application surface that covers the declared window remains the GUI draw
		   * target when GUI coordinates are changed; the coordinates do not create a new raster. */
		  int gui_uses_window_target = !aspect_gui_center && !engine->canvas_mode &&
		    render_presentation.application_owned && render_presentation.application_draw_enabled &&
		    render_presentation.application_width==(int)ow &&
		    render_presentation.application_height==(int)oh &&
		    engine->vm.gui_w>0 && engine->vm.gui_h>0 && frame_view_count==1 &&
		    application_surface_matches_first_generation_view_port(
		      engine,frame_view_count,frame_views[0].px,frame_views[0].py,
		      frame_views[0].pw,frame_views[0].ph,
		      render_presentation.application_width,render_presentation.application_height);
			  int gui_indirect = !aspect_gui_center && !engine->canvas_mode && !gui_uses_window_target &&
			                     (gsw != (int)ow || gsh != (int)oh);
			  if ((aspect_gui_center || gui_indirect) && !ensure_scratch_buffer(engine,&engine->gui_buffer)) {
			    engine_errorf(engine,ANYGM_ERROR_OUT_OF_MEMORY,"Could not allocate the GUI scratch buffer");
			    return ANYGM_ERROR_OUT_OF_MEMORY;
			  }
			  uint32_t *gtarget = (aspect_gui_center || gui_indirect) ? engine->gui_buffer : engine->screen;
			  int gtw = (aspect_gui_center || gui_indirect) ? gsw : (int)ow;
			  int gth = (aspect_gui_center || gui_indirect) ? gsh : (int)oh;
			  {
			    double screen_cam_x=(engine->aspect_force_active || aspect_gui_center)
			                        ? 0.0 : -(double)engine->gui_offset_x;
			    double screen_cam_y=(engine->aspect_force_active || aspect_gui_center)
			                        ? 0.0 : -(double)engine->gui_offset_y;
			    if(engine->composed_frame)
			      gml_render_begin_retaining_target(&engine->render, gtarget, gtw, gth,
			                                        screen_cam_x, screen_cam_y);
			    else
			      gml_render_begin(&engine->render, gtarget, gtw, gth,
			                       screen_cam_x, screen_cam_y);
			  }
	  if(!engine->content_presented) gml_render_set_pending_fill(&engine->render, 0);
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
	  if (!engine->aspect_force_active && anygm_policy_has_modern_screen_stage(&engine->win) &&
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
	  else if (render_presentation.application_owned &&
	           ((int)lround(pw_)!=render_presentation.application_width ||
	            (int)lround(ph_)!=render_presentation.application_height) &&
	           application_surface_matches_first_generation_view_port(
	             engine,frame_view_count,port_x_,port_y_,
	             (int)lround(pw_),(int)lround(ph_),
	             render_presentation.application_width,
	             render_presentation.application_height)) {
	    /* Present the complete window-owned target rather than letterboxing its automatic blit
	     * through a smaller view port. GUI size still governs subsequent draw coordinates. */
	    prx = pry = 0; prw = gtw; prh = gth;
	  }
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
	    /* A self-compositor can address either its logical application surface or a complete
	     * content-owned window raster. Keep its GUI coordinates paired with the physical target:
	     * scaling only one of them either shrinks the complete compositor into the fitted viewport
	     * or displaces HUD elements which use window_get_width/height. */
	    int gui_target_width=prw,gui_target_height=prh;
	    int gui_logical_width,gui_logical_height;
	    screen_stage_gui_geometry(
	      engine,&render_presentation,gtw,gth,
	      &gui_target_width,&gui_target_height,&gui_logical_width,&gui_logical_height);
	    gml_render_gui_begin(&engine->render,gui_target_width,gui_target_height);
	    gml_render_gui_set_size(&engine->render,gui_logical_width,gui_logical_height);
	  } else {
	  gml_render_gui_begin(&engine->render,
	    gui_uses_window_target?gtw:gsw,gui_uses_window_target?gth:gsh);
	    /* display_set_gui_size() is normally called from Create/room setup, before the GUI pass
	     * starts. Seed the renderer from that persistent VM state as well as accepting live changes
	     * during Draw GUI. */
	    if((anygm_policy_has_modern_layer_semantics(&engine->win) || gui_uses_window_target) &&
	       engine->vm.gui_w>0 && engine->vm.gui_h>0)
	      gml_render_gui_set_size(&engine->render,engine->vm.gui_w,engine->vm.gui_h);
	    /* Without an explicit GUI size, first-generation layer semantics use the first
	     * room's view port as logical GUI coordinates. The physical target and its
	     * presentation extents remain unchanged; later layers use the live target. */
	    /* A full-width compositor explicitly addresses the widened window, so its implicit GUI
	     * coordinates must follow that target too. Keep this projection local to the draw pass:
	     * persisting it as a content-declared GUI size feeds it back into canvas/aspect selection
	     * and leaves the previous forced extent behind when the frontend changes modes. */
	    else if(!(engine->aspect_force_active && compositor_fullwidth) &&
	            !anygm_policy_has_modern_layer_semantics(&engine->win) &&
	            engine->vm.gui_w<=0 && engine->vm.gui_h<=0 &&
	            engine->vm.gui_boot_w>=16 && engine->vm.gui_boot_h>=16 &&
	            (engine->vm.gui_boot_w!=(gui_uses_window_target?gtw:gsw) ||
	             engine->vm.gui_boot_h!=(gui_uses_window_target?gth:gsh))){
	      gml_render_gui_set_size(&engine->render,engine->vm.gui_boot_w,engine->vm.gui_boot_h);
	      if(anygm_host_development_setting(&engine->host,"GML_LOG_GUISIZE"))
	        engine_logf(engine,ANYGM_LOG_DEBUG,"[guisize] f%ld logical %dx%d target %dx%d\n",
	          engine->vm.frame,engine->vm.gui_boot_w,engine->vm.gui_boot_h,
	          gui_uses_window_target?gtw:gsw,gui_uses_window_target?gth:gsh);
	    }
	  }
	  if(engine->vm.gui_maximise_active)
	    gml_render_gui_set_maximise(&engine->render,1,
	      engine->vm.gui_maximise_xscale,engine->vm.gui_maximise_yscale,
	      engine->vm.gui_maximise_xoffset,engine->vm.gui_maximise_yoffset,
	      (int)engine->output_width,(int)engine->output_height);
	  /* GM screen-stage events: Pre-Draw -> [default app-surface blit] -> Post-Draw -> GUI.
	   * Content that composites the application surface itself, for example through a presentation
	   * object applying a palette shader in Post-Draw) disable the default blit and draw here.
	   * Post-Draw remains anchored to the fitted application viewport: negative coordinates may
	   * intentionally spill into the surrounding window margins. Applying that viewport origin is
	   * what keeps a self-compositor centered when its explicit window is wider than the view. */
		  log_present_pass(engine,"gui-begin",gtarget,gtw,gth);
		  GmlRenderTargetMetrics screen_target={0};
		  gml_render_target_metrics(&engine->render,&screen_target);
		  int screen_viewport_offset=screen_default_gui && !engine->screen_stage_window_raster &&
		                             (prx!=0 || pry!=0);
		  if(screen_viewport_offset){
		    GmlRenderTargetMetrics offset_target=screen_target;
		    offset_target.camera_x=-(double)prx;
		    offset_target.camera_y=-(double)pry;
		    gml_render_target_metrics_update(
		      &engine->render,&offset_target,GML_RENDER_TARGET_CAMERA);
		  }
		  gml_render_application_surface_presentation_settle(&engine->render,1);
		  gml_vm_draw_pass(&engine->vm, "Draw_77");   /* Post-Draw (GMS2 event 77) can replace the default
		                                         * application-surface blit with a custom composite. */
		  gml_render_application_surface_presentation_settle(&engine->render,0);
		  log_present_pass(engine,"post-draw",gtarget,gtw,gth);
		  gml_render_presentation_metrics(&engine->render,&render_presentation);
		  if ((render_presentation.application_draw_enabled && !engine->composed_frame) ||
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
  engine_overrides_draw_hold_end(engine);
  if(screen_viewport_offset)
    gml_render_target_metrics_update(
      &engine->render,&screen_target,GML_RENDER_TARGET_CAMERA);
  gml_render_gui_end(&engine->render);
  gml_render_flush_pending_underlay(&engine->render);
  gml_render_flush_pending_fill(&engine->render);
  log_present_pass(engine,"flushed",gtarget,gtw,gth);
  if (anygm_host_development_setting(&engine->host,"GML_LOG_PRESENT")) {
    if (engine->diagnostics.present_frame++ % 120 == 0) engine_logf(engine,ANYGM_LOG_DEBUG, "[present] out=%ux%u canvas=%d gui=%dx%d indirect=%d port=%dx%d prect=(%d,%d %dx%d) off=(%d,%d) host=%ux%u fit=(%d,%d %dx%d)\n",
      ow, oh, engine->canvas_mode, gsw, gsh, gui_indirect, (int)pw_, (int)ph_, prx, pry, prw, prh, engine->gui_offset_x, engine->gui_offset_y,
      engine->host_output_width,engine->host_output_height,
      engine->host_canvas_x,engine->host_canvas_y,
      engine->host_canvas_width,engine->host_canvas_height); }
  /* Fallback: content can disable the automatic application-surface blit
   * intending to composite it itself in a Draw GUI event. If that compositor paints nothing to the
   * screen here (unsupported GUI-space transform, absent object, etc.) the frame would be black — so
   * if the screen is still empty but the app-surface has pixels, blit it so the render isn't lost. */
  gml_render_presentation_metrics(&engine->render,&render_presentation);
  /* A deferred presentation answers this question without asking it. The record exists only when
   * an opaque draw of the application surface covers the whole target, so the target is empty
   * exactly when that surface has no lit pixel — which is the same condition the recovery below
   * then tests before doing anything. Both answers are therefore "do nothing", and scanning a
   * monitor-sized target to arrive at it would also force the frame back onto the processor. */
  if (!render_presentation.application_draw_enabled && !engine->composed_frame &&
      !anygm_host_development_setting(&engine->host,"GML_NO_CRT") &&
      !gml_render_deferred_presentation(&engine->render,NULL)) {
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
			  if (aspect_gui_center || gui_indirect) engine_materialize_completed_frame(engine);
			  if (aspect_gui_center) {
			    /* Composite the complete logical frame first: the world raster with the centered GUI
			     * crop laid back over it. At logical output this lands in the screen directly; a
			     * window-scaled output then takes one nearest upscale of that finished frame, the same
			     * transform the un-forced window-raster path performs. */
			    uint32_t *logical = ow == engine->width && oh == engine->height
			                      ? engine->screen : engine->fb;
			    if (logical == engine->screen)
			      memcpy(engine->screen, engine->fb, (size_t)ow * oh * sizeof(uint32_t));
			    for (int yy = 0; yy < aspect_gui_h && aspect_gui_y + yy < (int)engine->height; yy++) {
			      if (aspect_gui_x >= (int)engine->width) continue;
		      int copy_w = aspect_gui_w;
		      if (aspect_gui_x + copy_w > (int)engine->width) copy_w = (int)engine->width - aspect_gui_x;
		      if (copy_w > 0)
		        memcpy(logical + (size_t)(aspect_gui_y + yy) * engine->width + aspect_gui_x,
		               engine->gui_buffer + (size_t)yy * aspect_gui_w,
		               (size_t)copy_w * sizeof(uint32_t));
		    }
		    if (logical != engine->screen) {
		      unsigned lw = engine->width, lh = engine->height;
		      unsigned sy = 0, yacc = lh;
		      for (unsigned oy2 = 0; oy2 < oh; oy2++) {
		        const uint32_t *src = logical + (size_t)sy * lw;
		        uint32_t *dst = engine->screen + (size_t)oy2 * ow;
		        unsigned sx = 0, xacc = lw;
		        for (unsigned ox2 = 0; ox2 < ow; ox2++) {
		          dst[ox2] = src[sx] & 0xFFFFFFu;
		          xacc += lw * 2u;
		          if (xacc >= ow * 2u) { xacc -= ow * 2u; sx++; }
		        }
		        yacc += lh * 2u;
		        if (yacc >= oh * 2u) { yacc -= oh * 2u; sy++; }
		      }
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
	  if(engine->classic_transition.active || engine->vm.classic_info_active)
	    engine_materialize_completed_frame(engine);
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
    engine->profile.frame_ms=t1-t_total;
    engine->profile.total_ms += engine->profile.frame_ms;
    { double ft = t1 - t_total;
      /* GML_PROFILE_SPIKE=<ms>: dump the phase split of any frame that exceeds the threshold */
      if(engine->diagnostics.profile_spike_ms < -1){ const char *sp=anygm_host_development_setting(&engine->host,"GML_PROFILE_SPIKE"); engine->diagnostics.profile_spike_ms = sp? atof(sp) : -1; }
      if(engine->diagnostics.profile_spike_ms > 0 && ft > engine->diagnostics.profile_spike_ms)
        engine_logf(engine,ANYGM_LOG_DEBUG, "[spike] f%ld total=%.2fms step=%.2f draw=%.2f gui=%.2f video=%.2f\n",
                engine->vm.frame, ft, engine->profile.step_ms - sp_step, engine->profile.draw_ms - sp_draw,
                engine->profile.gui_ms - sp_gui, engine->profile.video_ms - sp_video); }
    engine->profile.frames++;
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
  a[0]=b64buf; a[1]=vreal(0); a[2]=vreal(-1); GmlVal b64enc=BI("buffer_base64_encode",3);
  CHK("buffer_base64_encode", b64enc.t==V_STR && b64enc.s && !strcmp(b64enc.s,"SGk="));
  /* A negative size means the rest of the buffer, so asking for it from zero
   * covers the full buffer. Verify the encoded result before deleting it. */
  a[0]=b64buf; a[1]=vreal(1); a[2]=vreal(-1); GmlVal b64tail=BI("buffer_base64_encode",3);
  CHK("buffer_base64_encode offset", b64tail.t==V_STR && b64tail.s && !strcmp(b64tail.s,"aQ=="));
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
  engine->config.fast_alpha_cull=0;
  engine->config.start_room=-1;
  engine->config.report_all_shaders_compiled=1;
  engine->config.content_overrides=1;
  snprintf(engine->language,sizeof engine->language,"en");
  snprintf(engine->region,sizeof engine->region,"US");
  snprintf(engine->language_tag,sizeof engine->language_tag,"en-US");
  engine->width=288; engine->height=216; engine->base_width=288; engine->base_height=216;
  engine->fps=60.0; engine->fps_room=-1;
  engine->player_object=-1;
  engine->mouse_pixel_x=-1.0; engine->mouse_pixel_y=-1.0;
  engine->mouse_warped=0; engine->mouse_host_x=0; engine->mouse_host_y=0;
  engine->profile_enabled=-1;
  engine->diagnostics.key_enabled=-1;
  engine->diagnostics.force_present_view=-1;
  engine->diagnostics.mouse_frame=-1;
  engine->diagnostics.cursor_frame=-1;
  engine->diagnostics.multiview_frame=-1;
  engine->diagnostics.profile_spike_ms=-2.0;
  engine->introskip_enabled=-1;
  engine->vm.os_type_declared=engine_overrides_declared_os_type(engine);
  engine->last_error[0]=0;
  *out_engine=engine;
  return ANYGM_OK;
}

void anygm_destroy(AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD) return;
  if(engine->lifecycle!=ENGINE_EMPTY) anygm_unload(engine);
  free(engine->state_reapply);
  free(engine->classic_phase_mem);
  free(engine->fb);
  free(engine->screen);
  free(engine->gui_buffer);
  free(engine->app_crop);
  free(engine->host_screen);
  free(engine->present_shift_screen);
  /* Destroying an engine outside a current graphics context releases CPU metadata and forgets the
   * handles; it must not issue a call into a context that is gone. */
  engine_graphics_release(engine,0);
  engine->guard=0;
  free(engine);
}

AnygmResult anygm_load_prepare(AnygmEngine *engine,const AnygmContentSource *source,
                               const AnygmLoadConfig *config,AnygmContentInfo *info){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || !source ||
     source->struct_size<sizeof(AnygmContentSource)) return ANYGM_ERROR_INVALID_ARGUMENT;
  if(config && config->struct_size<sizeof(AnygmLoadConfig)) return ANYGM_ERROR_INCOMPATIBLE_ABI;
  if(info && info->struct_size<sizeof *info) return ANYGM_ERROR_INCOMPATIBLE_ABI;
  if(engine->lifecycle!=ENGINE_EMPTY)
    return ANYGM_ERROR_INVALID_STATE;
  if(info) info->flags=0;
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
      resolved_config.region=host_region[0]?host_region:"US";
      resolved_config.language_tag=host_tag[0]?host_tag:"en-US";
      load_config=&resolved_config;
    }
  }
  AnygmResult result=engine_load_content_prepare(engine,source,load_config,info);
  if(result==ANYGM_OK){
    engine->lifecycle=ENGINE_PREPARED;
    engine->locale_from_host=(load_config==&resolved_config);
  }
  return result;
}

AnygmResult anygm_load_start(AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_PREPARED)
    return ANYGM_ERROR_INVALID_STATE;
  boot_runtime_start(engine);
  run_selftest(engine);
  {
    GmlRenderResourceMetrics render_resources;
    gml_render_resource_metrics(&engine->render,&render_resources);
    engine_logf(engine,ANYGM_LOG_INFO,
                "Runtime booted: atlases=%d sprites=%d texture-pages=%d\n",
                render_resources.atlas_count,render_resources.sprite_count,
                render_resources.texture_page_count);
  }
  engine->lifecycle=ENGINE_LOADED;
  return ANYGM_OK;
}

AnygmResult anygm_load(AnygmEngine *engine,const AnygmContentSource *source,
                       const AnygmLoadConfig *config){
  AnygmResult result=anygm_load_prepare(engine,source,config,NULL);
  if(result!=ANYGM_OK) return result;
  result=anygm_load_start(engine);
  if(result!=ANYGM_OK) anygm_unload(engine);
  return result;
}

void anygm_unload(AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle==ENGINE_EMPTY) return;
  if(engine->lifecycle==ENGINE_LOADED){
    /* A session that never saved still teaches the cache: measure once at teardown. */
    engine_state_peak_note(engine,engine_state_size(engine));
    engine_state_resume_peak_note(engine,engine_state_resume_size(engine));
    engine_state_peak_flush(engine);
  }
  engine_graphics_report(engine);
  engine_unload(engine);
  engine_override_reset(engine);
  engine->lifecycle=ENGINE_EMPTY;
}

AnygmResult anygm_reset(AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED)
    return ANYGM_ERROR_INVALID_STATE;
  /* Loads that took their locale from the host keep following it on reset. An explicit load
   * configuration remains pinned for the lifetime of the load. */
  if(engine->locale_from_host && engine->host.locale){
    char language[16]={0},region[16]={0},tag[32]={0};
    if(engine->host.locale(engine->host.userdata,language,sizeof language,
                           region,sizeof region,tag,sizeof tag)==ANYGM_OK){
      snprintf(engine->language,sizeof engine->language,"%s",language[0]?language:"en");
      snprintf(engine->region,sizeof engine->region,"%s",region[0]?region:"US");
      snprintf(engine->language_tag,sizeof engine->language_tag,"%s",tag[0]?tag:"en-US");
    }
  }
  gml_audio_free(engine->audio); engine->audio=NULL; engine->vm.audio=NULL;
  gml_render_discard_deferred_presentation(&engine->render);
  engine->frame_authority=ENGINE_FRAME_CPU_MATERIALIZED;
  gml_vm_free(&engine->vm);
  gml_render_free(&engine->render);
  boot_runtime(engine);
  return ANYGM_OK;
}

AnygmResult anygm_get_av_info(const AnygmEngine *engine,AnygmAvInfo *info){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED || !info)
    return ANYGM_ERROR_INVALID_STATE;
  if(info->struct_size<sizeof *info) return ANYGM_ERROR_INCOMPATIBLE_ABI;
  info->base_width=engine->host_output_width?engine->host_output_width:
                   (engine->output_width?engine->output_width:engine->width);
  info->base_height=engine->host_output_height?engine->host_output_height:
                    (engine->output_height?engine->output_height:engine->height);
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

AnygmResult anygm_get_room_count(const AnygmEngine *engine,uint32_t *count){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED || !count)
    return ANYGM_ERROR_INVALID_STATE;
  int rooms=gml_room_count(&engine->win);
  *count=rooms>0?(uint32_t)rooms:0u;
  return ANYGM_OK;
}

AnygmResult anygm_get_room_name(const AnygmEngine *engine,uint32_t index,
                                char *name,size_t capacity){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED ||
     !name || !capacity)
    return ANYGM_ERROR_INVALID_STATE;
  name[0]='\0';
  int rooms=gml_room_count(&engine->win);
  if(rooms<=0 || index>=(uint32_t)rooms) return ANYGM_ERROR_INVALID_ARGUMENT;
  GmlRoom room;
  if(gml_room_get(&engine->win,(int)index,&room)!=0) return ANYGM_ERROR_INVALID_CONTENT;
  snprintf(name,capacity,"%s",room.name?room.name:"");
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
  /* The previous frame's deferred presentation describes a target that is about to be rebuilt, and
   * content code runs before anything would flush it. Forget it here, ahead of all of that. */
  gml_render_discard_deferred_presentation(&engine->render);
  engine->frame_authority=ENGINE_FRAME_CPU_MATERIALIZED;
  /* Hybrid presentation and content GLSL may each need the terminal draw, independently. */
  gml_render_set_deferred_presentation(
    &engine->render,
    engine_hybrid_presentation_active(engine)||engine_content_shaders_active(engine));
  /* A content program in the middle of a frame runs only when that separate policy is active. */
  gml_render_set_shader_executor(&engine->render,
                                 engine_content_shaders_active(engine)
                                   ?engine_execute_content_shader:NULL,
                                 engine);
  gml_render_set_shader_device(&engine->render,engine_content_shaders_active(engine));
  /* A frame that spent anything on read-backs is one frame of the budget's measurement; the count
   * is closed here, where the frame begins, rather than from the renderer's own counter. */
  engine_readback_open_frame(engine);
  engine->frame_flags=0;
  AnygmResult result=engine_run_frame(engine);
  if(result!=ANYGM_OK) return result;
  const uint32_t *presented_pixels=NULL;
  unsigned presented_width=0,presented_height=0;
  int hardware_frame=0;
  int prof=profile_enabled(engine);
  double present_started=prof?profile_now_ms(engine):0.0;
  /* The completed frame's pixels are new, whatever carries them from here. */
  engine->host_frame_generation++;
  /* The shortest path first: when a graphics target can produce the final presentation from the
   * completed frame itself, the host-sized copy is never built and never uploaded. */
  hardware_frame=engine_present_hardware_screen(engine,&presented_width,&presented_height);
  if(!hardware_frame)
    hardware_frame=engine_present_hardware_canvas(engine,&presented_width,&presented_height);
  if(!hardware_frame){
    if(!resolve_host_frame(
         engine,&presented_pixels,&presented_width,&presented_height)){
      engine_errorf(engine,ANYGM_ERROR_OUT_OF_MEMORY,"Could not allocate the host presentation buffer");
      return ANYGM_ERROR_OUT_OF_MEMORY;
    }
    /* An accepted graphics target still needs a valid picture when the pass was not one the device
     * reproduces exactly, so the complete software frame is carried there unchanged. */
    hardware_frame=engine_present_hardware_frame(engine,presented_pixels,
                                                 presented_width,presented_height);
  }
  if(prof){
    double present_ms=profile_now_ms(engine)-present_started;
    engine->profile.present_ms+=present_ms;
    engine->profile.total_ms+=present_ms;
    double frame_ms=engine->profile.frame_ms+present_ms;
    if(frame_ms>engine->profile.max_ms){
      engine->profile.max_ms=frame_ms;
      engine->profile.max_frame=engine->vm.frame;
    }
    profile_report(engine,0);
  }
  if(anygm_host_development_setting(&engine->host,"GML_LOG_PRESENTED")){
    size_t lit=0;
    if(presented_pixels)
      for(size_t i=0;i<(size_t)presented_width*presented_height;i++)
        if(presented_pixels[i]&0xFFFFFF) lit++;
    engine_logf(engine,ANYGM_LOG_DEBUG,"[presented] screen lit=%zu presented=%d\n",lit,engine->content_presented);
  }
  engine->content_presented=0;
  engine->composed_frame=0;
  gml_render_clear_content_composited_screen(&engine->render);
  output->pixels=presented_pixels;
  output->width=presented_width;
  output->height=presented_height;
  output->pitch=(size_t)output->width*sizeof(uint32_t);
  output->pixel_format=ANYGM_PIXEL_XRGB8888;
  output->audio=engine->audio_output;
  output->audio_frames=engine->audio_frames;
  output->audio_rate=44100;
  output->flags=engine->frame_flags;
  if(hardware_frame){
    /* The frame is on the host's own target. Its pixel view is withdrawn deliberately: two
     * authoritative copies of one frame is exactly how a stale buffer gets presented. */
    output->pixels=NULL;
    output->pitch=0;
    output->flags|=ANYGM_FRAME_HARDWARE_TARGET;
  }
  return ANYGM_OK;
}

int engine_graphics_device_expected(const AnygmEngine *engine){
  return engine && engine->config.content_shader_device_expected?1:0;
}

static void release_fullwidth_gui_extent(AnygmEngine *engine,unsigned next_mode){
  if(engine->lifecycle!=ENGINE_LOADED || next_mode==engine->config.aspect_mode ||
     anygm_policy_has_modern_layer_semantics(&engine->win) ||
     !engine->aspect_force_active || !aspect_compositor_fullwidth_gen(engine)) return;
  if(engine->first_generation_app_owned && engine->wide_app_restore_width>0){
    GmlRenderPresentationMetrics presentation;
    gml_render_presentation_metrics(&engine->render,&presentation);
    if(presentation.application_owned)
      (void)gml_render_application_surface_ensure_owned(
        &engine->render,engine->wide_app_restore_width,engine->wide_app_restore_height);
    engine->wide_app_restore_width=engine->wide_app_restore_height=0;
  }
  if(engine->vm.gui_w!=(int)engine->width || engine->vm.gui_h!=(int)engine->height ||
     engine->vm.gui_boot_w<16 || engine->vm.gui_boot_h<16) return;
  /* A first-generation compositor may derive display_set_gui_size() from the live window during
   * Draw GUI. That declaration is authoritative for the frame being drawn, but its forced extent
   * is stale as soon as the frontend selects another mode. Its core-owned application surface was
   * widened from the same old extent, so restore both before presentation is recomputed; the next
   * GUI event can then declare the new live extent. */
  engine->vm.gui_w=engine->vm.gui_boot_w;
  engine->vm.gui_h=engine->vm.gui_boot_h;
}

AnygmResult anygm_set_config(AnygmEngine *engine,const AnygmConfigDelta *delta){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || !delta) return ANYGM_ERROR_INVALID_ARGUMENT;
  if(delta->struct_size<sizeof *delta || delta->values.struct_size<sizeof delta->values)
    return ANYGM_ERROR_INCOMPATIBLE_ABI;
  uint64_t f=delta->fields;
  int presentation_changed=0;
  if(f&ANYGM_CONFIG_MONITOR_WIDTH){
    presentation_changed|=engine->config.monitor_width!=delta->values.monitor_width;
    engine->config.monitor_width=delta->values.monitor_width;
  }
  if(f&ANYGM_CONFIG_MONITOR_HEIGHT){
    presentation_changed|=engine->config.monitor_height!=delta->values.monitor_height;
    engine->config.monitor_height=delta->values.monitor_height;
  }
  if(f&ANYGM_CONFIG_ASPECT_MODE){
    release_fullwidth_gui_extent(engine,delta->values.aspect_mode);
    engine->config.aspect_mode=delta->values.aspect_mode;
  }
  if(f&ANYGM_CONFIG_MOUSE_MODE) engine->config.mouse_mode=delta->values.mouse_mode;
  if(f&ANYGM_CONFIG_ROOM_SKIP_BUTTON) engine->config.room_skip_button=delta->values.room_skip_button;
  if(f&ANYGM_CONFIG_GOD_MODE) engine->config.god_mode=delta->values.god_mode;
  if(f&ANYGM_CONFIG_REPORT_ALL_SHADERS_COMPILED)
    engine->config.report_all_shaders_compiled=delta->values.report_all_shaders_compiled?1u:0u;
  if(f&ANYGM_CONFIG_CONTENT_SHADER_READBACK)
    engine->config.content_shader_readback=delta->values.content_shader_readback;
  if(f&ANYGM_CONFIG_CONTENT_SHADER_DEVICE_EXPECTED)
    engine->config.content_shader_device_expected=delta->values.content_shader_device_expected?1u:0u;
  if(f&ANYGM_CONFIG_HYBRID_GPU_PRESENTATION)
    engine->config.hybrid_gpu_presentation=delta->values.hybrid_gpu_presentation?1u:0u;
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
  if(presentation_changed){
    engine->fps_room=-1;
    if(engine->lifecycle==ENGINE_LOADED) engine->monitor_override_pending=1;
  }
  if(f&ANYGM_CONFIG_CLEAR_LOCAL_DATA)
    engine->config.clear_local_data=delta->values.clear_local_data?1u:0u;
  if(f&ANYGM_CONFIG_CONTENT_OVERRIDES){
    uint32_t want=delta->values.content_overrides?1u:0u;
    if(want!=engine->config.content_overrides){
      engine->config.content_overrides=want;
      /* The development menu and the intro-skip list can be declared by content directives;
       * both follow the toggle. */
      engine->introskip_enabled=-1;
      engine->vm.os_type_declared=engine_overrides_declared_os_type(engine);
      if(engine->lifecycle==ENGINE_LOADED){
        engine_override_menu_refresh(engine);
        /* Enabling a monitor program after a frontend transition brings its cached content state
         * to the monitor that is already active. Disabling it leaves authored state untouched. */
        if(want) engine->monitor_override_pending=1;
      }
    }
  }
  if(engine->lifecycle==ENGINE_PREPARED){
    GmlRenderControl control={
      .monitor_width=core_opt_monitor_size(engine,0),
      .monitor_height=core_opt_monitor_size(engine,1),
      .shader_report_all_compiled=engine->config.report_all_shaders_compiled?1:0,
      .shader_device_expected=engine_graphics_device_expected(engine)
    };
    gml_render_control_update(&engine->render,&control,GML_RENDER_CONTROL_HOST_OPTIONS);
    gml_render_set_shader_device(&engine->render,engine_content_shaders_active(engine));
  }
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

size_t anygm_state_resume_size(AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED) return 0;
  return engine_state_resume_size(engine);
}

size_t anygm_state_resume_capacity_hint(const AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED) return 0;
  size_t hint=engine->state_resume_peak_hint;
  GmlRenderStateProfileMetrics metrics={0};
  if(gml_render_state_profile_metrics(&engine->render,&metrics)){
    /* A surface can exist at load with its authored dimensions but an empty, cheaply encoded
     * picture, then acquire all of its pixels in gameplay. A fixed frontend allocates its ring
     * before that transition. Include the already-known mutable raster capacity in the required
     * state hint so the compact completed-frame policy never makes the frame-free fallback itself
     * too small. File-backed runtime sprites restore from their path and do not need inline RGBA. */
    size_t dynamic=metrics.surface_bytes;
    if(SIZE_MAX-dynamic<metrics.inline_runtime_sprite_bytes) dynamic=SIZE_MAX;
    else dynamic+=metrics.inline_runtime_sprite_bytes;
    if(dynamic>hint) hint=dynamic;
  }
  return hint;
}

uint32_t anygm_state_capacity_flags(const AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED) return 0;
  for(int i=0;i<engine->win.n_refs;i++){
    if(engine->win.ref_kind && engine->win.ref_kind[i]!=GML_REF_FUNCTION) continue;
    const char *name=engine->win.ref_name[i];
    if(name && (!strcmp(name,"surface_create") || !strcmp(name,"surface_create_ext") ||
                !strcmp(name,"surface_resize") || !strcmp(name,"execute_string") ||
                !strcmp(name,"execute_file") || !strcmp(name,"game_change")))
      return ANYGM_STATE_CAPACITY_DYNAMIC_SURFACES;
  }
  return 0;
}

size_t anygm_state_capacity_hint(const AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED) return 0;
  /* Complete states need the frame slot's ceiling even before the first frame exists. The
   * remembered complete peak raises it for content whose runtime allocation grows further; hosts
   * that explicitly want frame-free resume points have the separate compact hint above. */
  size_t hint=engine->state_peak_hint;
  size_t frame=engine_state_frame_capacity(engine);
  return frame>hint?frame:hint;
}

AnygmResult anygm_state_save(AnygmEngine *engine,void *data,size_t capacity,size_t *written){
  size_t local_written=0;
  if(!written) written=&local_written;
  *written=0;
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED || !data)
    return ANYGM_ERROR_INVALID_STATE;
  if(!engine_state_save(engine,data,capacity,written)) return ANYGM_ERROR_OUT_OF_MEMORY;
  engine_state_peak_note(engine,*written);
  return ANYGM_OK;
}

AnygmResult anygm_state_save_for_resume(AnygmEngine *engine,void *data,size_t capacity,
                                        size_t *written){
  size_t local_written=0;
  if(!written) written=&local_written;
  *written=0;
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED || !data)
    return ANYGM_ERROR_INVALID_STATE;
  if(!engine_state_save_for_resume(engine,data,capacity,written))
    return ANYGM_ERROR_OUT_OF_MEMORY;
  engine_state_resume_peak_note(engine,*written);
  return ANYGM_OK;
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
