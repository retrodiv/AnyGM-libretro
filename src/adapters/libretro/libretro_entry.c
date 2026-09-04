/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LibretroAdapter g_libretro;

void libretro_log(enum retro_log_level level,const char *format,...){
  char message[2048];
  va_list arguments;
  va_start(arguments,format);
  vsnprintf(message,sizeof message,format,arguments);
  va_end(arguments);
  if(g_libretro.log) g_libretro.log(level,"%s",message);
}

/* Pair each language with a matching region so a language chosen on its own remains internally
 * consistent. The locale contract uses lowercase language and uppercase region strings. */
static const struct { const char *iso,*region; } k_language_regions[]={
  {"en","US"},{"ja","JP"},{"fr","FR"},{"es","ES"},{"de","DE"},{"it","IT"},{"nl","NL"},
  {"pt","PT"},{"ru","RU"},{"ko","KR"},{"zh","CN"},{"pl","PL"},{"vi","VN"},{"ar","SA"},
  {"el","GR"},{"tr","TR"},{"fi","FI"},{"sv","SE"},{"uk","UA"},{"cs","CZ"},{"ca","ES"},
  {"hu","HU"},{"no","NO"},{"th","TH"},{NULL,NULL}
};

static void update_locale(void){
  unsigned language=RETRO_LANGUAGE_ENGLISH;
  if(g_libretro.environment)
    g_libretro.environment(RETRO_ENVIRONMENT_GET_LANGUAGE,&language);
  const char *iso="en",*region="US";
  switch(language){
    case RETRO_LANGUAGE_JAPANESE: iso="ja"; region="JP"; break;
    case RETRO_LANGUAGE_FRENCH: iso="fr"; region="FR"; break;
    case RETRO_LANGUAGE_SPANISH: iso="es"; region="ES"; break;
    case RETRO_LANGUAGE_GERMAN: iso="de"; region="DE"; break;
    case RETRO_LANGUAGE_ITALIAN: iso="it"; region="IT"; break;
    case RETRO_LANGUAGE_DUTCH: iso="nl"; region="NL"; break;
    case RETRO_LANGUAGE_PORTUGUESE_BRAZIL: iso="pt"; region="BR"; break;
    case RETRO_LANGUAGE_PORTUGUESE_PORTUGAL: iso="pt"; region="PT"; break;
    case RETRO_LANGUAGE_RUSSIAN: iso="ru"; region="RU"; break;
    case RETRO_LANGUAGE_KOREAN: iso="ko"; region="KR"; break;
    case RETRO_LANGUAGE_CHINESE_TRADITIONAL: iso="zh"; region="TW"; break;
    case RETRO_LANGUAGE_CHINESE_SIMPLIFIED: iso="zh"; region="CN"; break;
    case RETRO_LANGUAGE_POLISH: iso="pl"; region="PL"; break;
    case RETRO_LANGUAGE_VIETNAMESE: iso="vi"; region="VN"; break;
    case RETRO_LANGUAGE_ARABIC: iso="ar"; region="SA"; break;
    case RETRO_LANGUAGE_GREEK: iso="el"; region="GR"; break;
    case RETRO_LANGUAGE_TURKISH: iso="tr"; region="TR"; break;
    case RETRO_LANGUAGE_FINNISH: iso="fi"; region="FI"; break;
    case RETRO_LANGUAGE_SWEDISH: iso="sv"; region="SE"; break;
    case RETRO_LANGUAGE_UKRAINIAN: iso="uk"; region="UA"; break;
    case RETRO_LANGUAGE_CZECH: iso="cs"; region="CZ"; break;
    case RETRO_LANGUAGE_CATALAN_VALENCIA:
    case RETRO_LANGUAGE_CATALAN: iso="ca"; region="ES"; break;
    case RETRO_LANGUAGE_BRITISH_ENGLISH: iso="en"; region="GB"; break;
    case RETRO_LANGUAGE_HUNGARIAN: iso="hu"; region="HU"; break;
    case RETRO_LANGUAGE_NORWEGIAN: iso="no"; region="NO"; break;
    case RETRO_LANGUAGE_THAI: iso="th"; region="TH"; break;
    default: break;
  }
  /* A chosen language brings its paired region rather than retaining the frontend's region; an
   * explicit region may then narrow that choice. Copy values before the next frontend query. */
  char chosen_language[8],chosen_region[8];
  const char *value=libretro_options_value("anygm_language");
  if(value && value[0] && strcmp(value,"Auto")){
    snprintf(chosen_language,sizeof chosen_language,"%s",value);
    iso=chosen_language;
    for(size_t i=0;k_language_regions[i].iso;i++)
      if(!strcmp(k_language_regions[i].iso,iso)){ region=k_language_regions[i].region; break; }
  }
  value=libretro_options_value("anygm_region");
  if(value && value[0] && strcmp(value,"Auto")){
    snprintf(chosen_region,sizeof chosen_region,"%s",value);
    region=chosen_region;
  }
  snprintf(g_libretro.language,sizeof g_libretro.language,"%s",iso);
  snprintf(g_libretro.region,sizeof g_libretro.region,"%s",region);
  /* The fuller tag only differs where one language writes differently by region. */
  if(!strcmp(iso,"zh"))
    snprintf(g_libretro.language_tag,sizeof g_libretro.language_tag,"zh-%s",
             !strcmp(region,"TW")?"Hant":"Hans");
  else if((!strcmp(iso,"en") && (!strcmp(region,"US")||!strcmp(region,"GB"))) ||
          (!strcmp(iso,"pt") && !strcmp(region,"BR")))
    snprintf(g_libretro.language_tag,sizeof g_libretro.language_tag,"%s-%s",iso,region);
  else
    snprintf(g_libretro.language_tag,sizeof g_libretro.language_tag,"%s",iso);
}

static bool create_engine(void){
  if(g_libretro.engine) return true;
  AnygmHostServices services;
  libretro_host_services_init(&services);
  AnygmResult result=anygm_create(&services,&g_libretro.engine);
  if(result!=ANYGM_OK){
    libretro_log(RETRO_LOG_ERROR,"Could not create the AnyGM engine (%d)\n",result);
    return false;
  }
  g_libretro.config.struct_size=sizeof g_libretro.config;
  libretro_options_apply(true);
  return true;
}

/* Declare the variable-size capability and remember whether the frontend accepts it. A frontend
 * that does not set FRONT_VARIABLE_SIZE has the baseline libretro contract: the first capacity is
 * a ceiling for the whole loaded session. */
static void negotiate_serialization(void){
  uint64_t quirks=RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE;
  g_libretro.variable_state_supported=false;
  if(g_libretro.environment &&
     g_libretro.environment(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS,&quirks))
    g_libretro.variable_state_supported=
      (quirks&RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE)!=0;
}

static size_t fixed_state_capacity(size_t actual,bool compact_startup){
  const size_t margin=512u*1024u;
  /* Before the first frame, a cold ordinary raster can still understate the render and run-time
   * tables that gameplay will populate. Fixed frontends cannot enlarge the ring they allocate
   * from this answer, so keep the established 4 MiB session reserve even when the cold state is
   * small. A deliberately compact large-frame ring starts at one MiB, then its frame-free hint
   * and any known mutable render allocations raise it only as required. */
  size_t floor=compact_startup?1u*1024u*1024u:4u*1024u*1024u;
  if(actual>(SIZE_MAX-margin)/2u) return SIZE_MAX;
  size_t capacity=actual*2u+margin;
  return capacity<floor?floor:capacity;
}

void retro_set_environment(retro_environment_t callback){
  g_libretro.environment=callback;
  if(!callback) return;
  /* SET_SUPPORT_NO_GAME is deliberately not sent: it is only meaningful when asserting that the
   * core runs with no content, and this one always needs a payload. Sending false is a no-op that
   * reads like a decision. */
  libretro_options_register();
  libretro_input_register();
}

void retro_set_video_refresh(retro_video_refresh_t callback){ g_libretro.video=callback; }
void retro_set_audio_sample(retro_audio_sample_t callback){ g_libretro.audio_sample=callback; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback){ g_libretro.audio_batch=callback; }
void retro_set_input_poll(retro_input_poll_t callback){ g_libretro.input_poll=callback; }
void retro_set_input_state(retro_input_state_t callback){ g_libretro.input_state=callback; }

/* The renderer emits XRGB8888 and has no other output format. A frontend that refuses it reads
 * whatever we send as 0RGB1555, which is not a degraded picture but a garbled one, so a refusal is
 * carried to retro_load_game and answered there with a message the player can act on rather than
 * with a warning in a log nobody opens. The request is made in retro_init because that is where
 * RetroArch expects it, and repeated at load because libretro.h says load or get_system_av_info is
 * the correct place and a stricter frontend may only honour it there. */
static bool request_pixel_format(void){
  if(!g_libretro.environment) return false;
  enum retro_pixel_format format=RETRO_PIXEL_FORMAT_XRGB8888;
  if(g_libretro.environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT,&format)) return true;
  libretro_log(RETRO_LOG_ERROR,"The frontend rejected XRGB8888, the only format this core emits\n");
  return false;
}

void retro_init(void){
  /* The settings are declared when the host hands over its callback, and released when the core is
   * torn down. A host that starts the core again without handing the callback over a second time
   * would otherwise be left with nothing declared, so the declaration is made again here; saying
   * it twice costs nothing. */
  libretro_options_register();
  if(g_libretro.environment){
    struct retro_log_callback log_callback;
    if(g_libretro.environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE,&log_callback))
      g_libretro.log=log_callback.log;
    g_libretro.pixel_format_accepted=request_pixel_format();
    memset(&g_libretro.rumble,0,sizeof g_libretro.rumble);
    g_libretro.rumble_available=
        g_libretro.environment(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE,&g_libretro.rumble) &&
        g_libretro.rumble.set_rumble_state;
    /* A software rasterizer that also runs a bytecode interpreter is not a light core; saying so
     * lets a frontend choose its scheduling before the first frame instead of after a stutter. */
    unsigned performance=8;
    g_libretro.environment(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL,&performance);
    /* Whether a repeated frame may be sent as a null pointer. Without it every skipped or failed
     * frame has to carry a full framebuffer the frontend is going to discard. */
    bool can_dupe=false;
    g_libretro.can_dupe=
        g_libretro.environment(RETRO_ENVIRONMENT_GET_CAN_DUPE,&can_dupe) && can_dupe;
    g_libretro.video_enabled=true;
    g_libretro.audio_enabled=true;
    /* One environment call per port instead of one per button. */
    g_libretro.input_bitmasks=
        g_libretro.environment(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS,NULL)!=false;
  }
  negotiate_serialization();
  libretro_vfs_request();
  update_locale();
  create_engine();
}

void retro_deinit(void){
  libretro_hw_render_release();
  if(g_libretro.engine){
    anygm_destroy(g_libretro.engine);
    g_libretro.engine=NULL;
  }
  libretro_options_release();
  memset(g_libretro.keyboard_events,0,sizeof g_libretro.keyboard_events);
  memset(g_libretro.override_used,0,sizeof g_libretro.override_used);
  g_libretro.prepared=false;
  g_libretro.loaded=false;
  g_libretro.provisional_av_exposed=false;
  g_libretro.startup_av_notification_pending=false;
}

unsigned retro_api_version(void){ return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info){
  memset(info,0,sizeof *info);
  info->library_name="AnyGM";
  info->library_version="0.1.0";
  info->valid_extensions="win|droid|zip|port|apk|yyp|yyz|gmd|gmk|gm6|gm81|exe|anygm";
  info->need_fullpath=true;
  info->block_extract=true;
}

static void fill_av_info(struct retro_system_av_info *info){
  memset(info,0,sizeof *info);
  info->geometry.base_width=g_libretro.av.base_width?g_libretro.av.base_width:288;
  info->geometry.base_height=g_libretro.av.base_height?g_libretro.av.base_height:216;
  info->geometry.max_width=g_libretro.av.max_width?g_libretro.av.max_width:3840;
  info->geometry.max_height=g_libretro.av.max_height?g_libretro.av.max_height:2160;
  info->geometry.aspect_ratio=g_libretro.av.aspect_ratio>0?(float)g_libretro.av.aspect_ratio:4.0f/3.0f;
  info->timing.fps=g_libretro.av.frames_per_second>0?g_libretro.av.frames_per_second:60.0;
  info->timing.sample_rate=g_libretro.av.audio_rate?g_libretro.av.audio_rate:44100.0;
}

void libretro_update_av(void){
  if(!g_libretro.engine || !g_libretro.loaded) return;
  memset(&g_libretro.av,0,sizeof g_libretro.av);
  g_libretro.av.struct_size=sizeof g_libretro.av;
  anygm_get_av_info(g_libretro.engine,&g_libretro.av);
}

void retro_get_system_av_info(struct retro_system_av_info *info){
  /* A hardware frontend may ask for AV information after retro_load_game returns but before it
   * creates the requested context. The prepared runtime cannot run authored boot code yet, so the
   * fallback answer is provisional and the exact result is published from the first frame. */
  if(g_libretro.prepared) g_libretro.provisional_av_exposed=true;
  fill_av_info(info);
}

void retro_set_controller_port_device(unsigned port,unsigned device){
  if(port<ANYGM_MAX_GAMEPADS) g_libretro.port_device[port]=device;
  libretro_input_register();
}

void retro_reset(void){
  /* Options are otherwise picked up from inside the frame loop, and a player choosing one reaches
   * the restart without the core having run a frame in between: hosts hold the core still while
   * their menu is open. The choice that prompted the restart would then apply to the boot after
   * the next one. Settings read at boot, the start room among them, are read here first. */
  if(g_libretro.loaded){
    /* Refresh the service fields first so a locale option selected before restart is applied to
     * the next reset. */
    update_locale();
    libretro_options_apply(true);
    if(anygm_reset(g_libretro.engine)==ANYGM_OK){
      /* RetroArch keeps its rewind ring across Reset and checks its rewind chord before the newly
       * reset core runs. If the chord used while leaving the menu is still active, an old compact
       * ring state would replace the reset immediately. A separately sized complete state remains
       * distinguishable and valid; the ambiguous compact-capacity load is held behind the first
       * new frame. */
      g_libretro.reset_pending_frame=true;
      g_libretro.reset_ring_rejection_reported=false;
    }
  }
  libretro_update_av();
}

/* Keep persistent content data under the save root and rebuildable loader data
 * under a separate cache subdirectory. */
#define ANYGM_CACHE_SUBDIRECTORY "anygm-cache"

/* Join a frontend-named root and one suffix, refusing the result rather than truncating it. A
 * truncated join names a prefix of the root, which is how a cache directory ends up being the save
 * root itself. */
static bool join_root(char *out,size_t size,const char *root,const char *suffix){
  if(!root || !root[0]) return false;
  int written=snprintf(out,size,"%s/%s",root,suffix);
  if(written<0 || (size_t)written>=size){ out[0]=0; return false; }
  return true;
}

static const char *frontend_directory(unsigned command){
  const char *directory=NULL;
  if(g_libretro.environment && g_libretro.environment(command,&directory) &&
     directory && directory[0])
    return directory;
  return NULL;
}

/* Where this core is allowed to write. The save root is the frontend's, and the extracted-payload
 * cache is a subdirectory of it so that deleting the cache costs re-extraction and nothing else.
 *
 * When a frontend answers neither, the loader's own last-resort roots are a relative tmp/ under
 * the frontend's working directory and the directory the content sits in - read-only on many
 * setups and impossible under Android's scoped storage. The system directory is the conventional
 * answer for that case, so it is asked for before the loader is left to fall back. */
static void update_directories(void){
  g_libretro.save_directory[0]=0;
  g_libretro.cache_directory[0]=0;
  const char *saves=frontend_directory(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY);
  const char *system=frontend_directory(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY);
  if(saves){
    snprintf(g_libretro.save_directory,sizeof g_libretro.save_directory,"%s",saves);
    join_root(g_libretro.cache_directory,sizeof g_libretro.cache_directory,
              saves,ANYGM_CACHE_SUBDIRECTORY);
  }
  else if(system){
    libretro_log(RETRO_LOG_WARN,
                 "The frontend named no save directory; using the system directory instead\n");
    join_root(g_libretro.save_directory,sizeof g_libretro.save_directory,system,"anygm");
    join_root(g_libretro.cache_directory,sizeof g_libretro.cache_directory,
              system,ANYGM_CACHE_SUBDIRECTORY);
  }
  if(!g_libretro.cache_directory[0] && system)
    join_root(g_libretro.cache_directory,sizeof g_libretro.cache_directory,
              system,ANYGM_CACHE_SUBDIRECTORY);
}

bool libretro_content_start(void){
  if(g_libretro.loaded) return true;
  if(!g_libretro.prepared || !g_libretro.engine) return false;
  bool provisional_av_exposed=g_libretro.provisional_av_exposed;
  AnygmResult result=anygm_load_start(g_libretro.engine);
  if(result!=ANYGM_OK){
    char error[512];
    anygm_get_last_error(g_libretro.engine,error,sizeof error);
    libretro_log(RETRO_LOG_ERROR,"Prepared content startup failed (%d): %s\n",result,error);
    return false;
  }
  g_libretro.prepared=false;
  g_libretro.loaded=true;
  g_libretro.provisional_av_exposed=false;
  g_libretro.frame_completed=false;
  g_libretro.reset_pending_frame=false;
  g_libretro.reset_ring_rejection_reported=false;
  g_libretro.startup_ring_compact=false;
  g_libretro.fixed_state_capacity=0;
  g_libretro.startup_resume_capacity=0;
  g_libretro.state_capacity_growth_reported=false;
  /* Preparation consulted one-shot setting names. Clear them before the first frame so live names
   * always find a free cache slot. */
  memset(g_libretro.setting_cache,0,sizeof g_libretro.setting_cache);
  g_libretro.setting_cache_count=0;
  libretro_options_publish_rooms();
  libretro_update_av();
  g_libretro.startup_av_notification_pending=provisional_av_exposed;
  return true;
}

bool retro_load_game(const struct retro_game_info *info){
  if(!info || !info->path || !create_engine()) return false;
  if(!g_libretro.pixel_format_accepted) g_libretro.pixel_format_accepted=request_pixel_format();
  if(!g_libretro.pixel_format_accepted){
    libretro_log(RETRO_LOG_ERROR,
                 "Refusing to load: this core renders XRGB8888 and the frontend will not take it\n");
    return false;
  }
  if(g_libretro.loaded || g_libretro.prepared) retro_unload_game();
  g_libretro.provisional_av_exposed=false;
  g_libretro.startup_av_notification_pending=false;
  memset(g_libretro.setting_cache,0,sizeof g_libretro.setting_cache);
  g_libretro.setting_cache_count=0;
  update_directories();
  update_locale();
  libretro_options_apply(true);
  AnygmContentSource source;
  memset(&source,0,sizeof source);
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=info->path;
  source.cache_directory=g_libretro.cache_directory[0]?g_libretro.cache_directory:NULL;
  source.save_directory=g_libretro.save_directory[0]?g_libretro.save_directory:NULL;
  /* With no explicit locale, the engine queries the host service and refreshes it on reset.
   * Passing these current fields instead would pin the first answer for the lifetime of the load. */
  AnygmContentInfo content_info;
  memset(&content_info,0,sizeof content_info);
  content_info.struct_size=sizeof content_info;
  AnygmResult result=anygm_load_prepare(g_libretro.engine,&source,NULL,&content_info);
  if(result!=ANYGM_OK){
    char error[512];
    anygm_get_last_error(g_libretro.engine,error,sizeof error);
    libretro_log(RETRO_LOG_ERROR,"Content load failed (%d): %s\n",result,error);
    /* A refusal that only reaches the log is a black screen as far as the player is concerned.
     * SET_MESSAGE_EXT puts the reason on screen where a frontend supports it; a frontend that
     * does not simply leaves the log entry, which is what happened before. */
    if(g_libretro.environment){
      struct retro_message_ext message;
      memset(&message,0,sizeof message);
      /* Deliberately truncating: a notification has room for a sentence, and the whole
       * diagnostic is in the log line above. */
      char shown[576];
      snprintf(shown,sizeof shown,"AnyGM cannot load this content: %s",error);
      message.msg=shown;
      message.duration=6000;
      message.priority=3;
      message.level=RETRO_LOG_ERROR;
      message.target=RETRO_MESSAGE_TARGET_ALL;
      message.type=RETRO_MESSAGE_TYPE_NOTIFICATION;
      message.progress=-1;
      g_libretro.environment(RETRO_ENVIRONMENT_SET_MESSAGE_EXT,&message);
    }
    return false;
  }
  g_libretro.prepared=true;
  g_libretro.content_glsl_candidate=
    (content_info.flags&ANYGM_CONTENT_GLSL_DEVICE_CANDIDATE)!=0;
  bool needs_context=g_libretro.hybrid_gpu_selected ||
    (g_libretro.content_glsl_selected && g_libretro.content_glsl_candidate);
  if(needs_context){
    /* Some frontends may invoke context_reset before the environment call returns. Publish the
     * accepted policy first; a rejection below replaces it before software startup. */
    libretro_options_finalize_graphics(true);
    if(libretro_hw_render_request(true)) return true;
  }
  libretro_options_finalize_graphics(false);
  return libretro_content_start();
}

bool retro_load_game_special(unsigned type,const struct retro_game_info *info,size_t count){
  (void)type; (void)info; (void)count;
  return false;
}

void retro_unload_game(void){
  if(!g_libretro.loaded && !g_libretro.prepared) return;
  libretro_hw_render_release();
  anygm_unload(g_libretro.engine);
  g_libretro.prepared=false;
  g_libretro.loaded=false;
  g_libretro.provisional_av_exposed=false;
  g_libretro.startup_av_notification_pending=false;
  g_libretro.content_glsl_candidate=false;
  g_libretro.frame_completed=false;
  g_libretro.reset_pending_frame=false;
  g_libretro.reset_ring_rejection_reported=false;
  g_libretro.startup_ring_compact=false;
  g_libretro.fixed_state_capacity=0;
  g_libretro.startup_resume_capacity=0;
  g_libretro.state_capacity_growth_reported=false;
  g_libretro.frame_failure_reports=0;
  memset(g_libretro.override_used,0,sizeof g_libretro.override_used);
  memset(&g_libretro.frame,0,sizeof g_libretro.frame);
  memset(&g_libretro.av,0,sizeof g_libretro.av);
  /* A key held while one game is unloaded is not held by the next one. Left latched, the second
   * load starts with input the player never gave it. */
  memset(g_libretro.keyboard_events,0,sizeof g_libretro.keyboard_events);
  g_libretro.pointer_seen=0;
}

unsigned retro_get_region(void){ return RETRO_REGION_NTSC; }

static void apply_live_options(void){
  bool updated=false;
  if(g_libretro.environment &&
     g_libretro.environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE,&updated) && updated)
    libretro_options_apply(false);
  bool fast_forward=false;
  if(g_libretro.environment)
    g_libretro.environment(RETRO_ENVIRONMENT_GET_FASTFORWARDING,&fast_forward);
  uint32_t fast_forward_value=fast_forward?1u:0u;
  if(g_libretro.config.fast_forward==fast_forward_value) return;
  AnygmConfigDelta delta;
  memset(&delta,0,sizeof delta);
  delta.struct_size=sizeof delta;
  delta.fields=ANYGM_CONFIG_FAST_FORWARD;
  delta.values=g_libretro.config;
  delta.values.struct_size=sizeof delta.values;
  delta.values.fast_forward=fast_forward_value;
  g_libretro.config.fast_forward=delta.values.fast_forward;
  anygm_set_config(g_libretro.engine,&delta);
}

void retro_run(void){
  if(!g_libretro.loaded){
    if(g_libretro.video) g_libretro.video(NULL,288,216,0);
    return;
  }
  apply_live_options();
  /* Fast-forward and run-ahead throw frames away. A frontend that says so in advance saves this
   * core the most expensive work it does, and there is no other way for a software rasterizer to
   * find out. Both default to on, so a frontend that answers nothing loses nothing. */
  if(g_libretro.environment){
    int enable=0;
    if(g_libretro.environment(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE,&enable)){
      g_libretro.video_enabled=(enable&1)!=0;
      g_libretro.audio_enabled=(enable&2)!=0;
    }
  }
  AnygmInputFrame input;
  uint32_t width=g_libretro.frame.width?g_libretro.frame.width:g_libretro.av.base_width;
  uint32_t height=g_libretro.frame.height?g_libretro.frame.height:g_libretro.av.base_height;
  libretro_input_snapshot(&input,width,height);
  memset(&g_libretro.frame,0,sizeof g_libretro.frame);
  g_libretro.frame.struct_size=sizeof g_libretro.frame;
  AnygmResult result=anygm_run_frame(g_libretro.engine,&input,&g_libretro.frame);
  if(result!=ANYGM_OK){
    /* Reported a bounded number of times: a frame that fails usually fails on every frame after
     * it too, and a message per frame buries the first one under thousands of copies. */
    const unsigned report_limit=8;
    if(g_libretro.frame_failure_reports<report_limit){
      g_libretro.frame_failure_reports++;
      libretro_log(RETRO_LOG_ERROR,"Frame execution failed (%d)%s\n",result,
                   g_libretro.frame_failure_reports==report_limit?
                     "; further failures of this kind are not reported":"");
    }
    /* The frontend still needs a frame. Starving it stalls its own timing, so the previous
     * picture is repeated where the frontend accepts a duplicate and resent otherwise. */
    if(g_libretro.video)
      g_libretro.video(g_libretro.can_dupe?NULL:g_libretro.frame.pixels,
                       g_libretro.av.base_width,g_libretro.av.base_height,
                       g_libretro.can_dupe?0:g_libretro.frame.pitch);
    return;
  }
  g_libretro.frame_failure_reports=0;
  g_libretro.frame_completed=true;
  g_libretro.reset_pending_frame=false;
  /* Announced before the frame it describes, not after it: the frontend sizes what it is about
   * to receive from the geometry it currently holds. The dimensions here are bounded by the
   * constant maxima declared in retro_get_system_av_info, so this can never enlarge them. */
  bool timing_changed=(g_libretro.frame.flags&ANYGM_FRAME_TIMING_CHANGED)!=0;
  bool geometry_changed=(g_libretro.frame.flags&ANYGM_FRAME_GEOMETRY_CHANGED)!=0;
  bool av_refreshed=false;
  if(g_libretro.startup_av_notification_pending){
    /* The only AV answer available before context adoption was fill_av_info's documented
     * fallback. Correct it once authored startup and the first frame have settled. A max-size or
     * timing change needs the full callback; a nominal-size/aspect change uses the lighter one. */
    libretro_update_av();
    av_refreshed=true;
    timing_changed=timing_changed ||
      g_libretro.av.max_width!=3840u || g_libretro.av.max_height!=2160u ||
      g_libretro.av.frames_per_second!=60.0 || g_libretro.av.audio_rate!=44100u;
    geometry_changed=geometry_changed ||
      g_libretro.av.base_width!=288u || g_libretro.av.base_height!=216u ||
      (float)g_libretro.av.aspect_ratio!=4.0f/3.0f;
    g_libretro.startup_av_notification_pending=false;
  }
  if((geometry_changed || timing_changed) && g_libretro.environment){
    if(!av_refreshed) libretro_update_av();
    struct retro_system_av_info av;
    fill_av_info(&av);
    if(timing_changed)
      g_libretro.environment(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO,&av);
    else
      g_libretro.environment(RETRO_ENVIRONMENT_SET_GEOMETRY,&av.geometry);
  }
  if(g_libretro.video && g_libretro.video_enabled){
    /* The engine reports which target it rendered into. The hardware sentinel says the frame is
     * already on the frontend's own framebuffer; without it a complete CPU frame is available and
     * the ordinary pixel callback carries it. The two are never both authoritative. */
    if(g_libretro.frame.flags&ANYGM_FRAME_HARDWARE_TARGET)
      g_libretro.video(RETRO_HW_FRAME_BUFFER_VALID,g_libretro.frame.width,
                       g_libretro.frame.height,0);
    else
      g_libretro.video(g_libretro.frame.pixels,g_libretro.frame.width,g_libretro.frame.height,
                       g_libretro.frame.pitch);
  }
  else if(g_libretro.video && g_libretro.can_dupe)
    g_libretro.video(NULL,g_libretro.frame.width,g_libretro.frame.height,0);
  if(g_libretro.frame.audio && g_libretro.frame.audio_frames && g_libretro.audio_enabled){
    if(g_libretro.audio_batch)
      g_libretro.audio_batch(g_libretro.frame.audio,g_libretro.frame.audio_frames);
    else if(g_libretro.audio_sample)
      for(size_t i=0;i<g_libretro.frame.audio_frames;i++)
        g_libretro.audio_sample(g_libretro.frame.audio[i*2],g_libretro.frame.audio[i*2+1]);
  }
  if((g_libretro.frame.flags&ANYGM_FRAME_SHUTDOWN_REQUESTED) && g_libretro.environment)
    g_libretro.environment(RETRO_ENVIRONMENT_SHUTDOWN,NULL);
}

size_t retro_serialize_size(void){
  if(!g_libretro.loaded) return 0;
  /* A frontend that did not acknowledge variable sizes owns one fixed allocation for the loaded
   * session. Re-measuring the complete runtime graph cannot change the answer in that contract,
   * and doing it before every rewind snapshot duplicates the most expensive half of saving it. */
  if(g_libretro.fixed_state_capacity && !g_libretro.variable_state_supported)
    return g_libretro.fixed_state_capacity;
  /* A large completed-frame ceiling may establish a smaller pre-frame rewind ring from only the
   * required sections. A fixed frontend keeps that capacity for the complete loaded session;
   * acknowledged variable-size frontends may grow later. In either contract, an earlier compact
   * ring is recognized by the capacity it passes to retro_serialize. */
  size_t actual=anygm_state_resume_size(g_libretro.engine);
  size_t resume_hint=anygm_state_resume_capacity_hint(g_libretro.engine);
  if(resume_hint>actual) actual=resume_hint;
  bool compact_startup=false;
  /* Preserve completed-frame rewind while its worst-case storage is modest. Once the optional
   * picture adds at least the ordinary four-MiB session reserve, copying its pessimistic ceiling
   * into every fixed rewind slot dominates the high-frequency transport. A smaller picture stays
   * in that ordinary reserve so a fixed frontend preserves exact visible rewind and still covers
   * language-level growth that a cold state cannot predict. */
  if(!g_libretro.frame_completed){
    const size_t minimum_saving=4u*1024u*1024u;
    size_t complete_hint=anygm_state_capacity_hint(g_libretro.engine);
    compact_startup=complete_hint>actual && complete_hint-actual>=minimum_saving;
    if(!compact_startup && complete_hint>actual) actual=complete_hint;
  }
  if(g_libretro.frame_completed){
    size_t hint=anygm_state_capacity_hint(g_libretro.engine);
    if(hint>actual) actual=hint;
  }
  /* Grow only after an explicit frontend acknowledgement. RetroArch re-queries this answer before
   * every rewind snapshot but keeps the ring allocated from the first one; returning the complete
   * frame ceiling later makes RetroArch reject the snapshot before it calls retro_serialize(), so
   * the compact small-buffer path never gets a chance to run. */
  if(!g_libretro.fixed_state_capacity ||
     (g_libretro.variable_state_supported && actual>g_libretro.fixed_state_capacity)){
    size_t previous=g_libretro.fixed_state_capacity;
    g_libretro.fixed_state_capacity=fixed_state_capacity(actual,compact_startup);
    /* Acknowledged growth is unusual enough to report once without flooding repeated queries. */
    if(previous && !g_libretro.state_capacity_growth_reported){
      g_libretro.state_capacity_growth_reported=true;
      libretro_log(RETRO_LOG_INFO,
                   "State capacity grew after variable-size frontend acknowledgement (%llu -> "
                   "%llu bytes)\n",
                   (unsigned long long)previous,
                   (unsigned long long)g_libretro.fixed_state_capacity);
    }
  }
  if(!g_libretro.frame_completed){
    g_libretro.startup_resume_capacity=g_libretro.fixed_state_capacity;
    g_libretro.startup_ring_compact=compact_startup;
  }
  return g_libretro.fixed_state_capacity;
}

bool retro_serialize(void *data,size_t size){
  size_t written=0;
  if(!g_libretro.loaded) return false;
  bool startup_slot=g_libretro.frame_completed && g_libretro.startup_ring_compact &&
                    g_libretro.startup_resume_capacity &&
                    size<=g_libretro.startup_resume_capacity;
  AnygmResult result;
  if(startup_slot){
    /* The conservative completed-frame ceiling chose this compact ring, but the encoded frame can
     * still fit: repeated rows and runs often reduce a large raster below that ceiling. Preserve
     * exact rewind whenever the real complete state fits the frontend's slot. Only fall back to
     * the explicit frame-free form when the complete writer actually refuses the offered bytes. */
    result=anygm_state_save(g_libretro.engine,data,size,&written);
    if(result!=ANYGM_OK || written>size){
      written=0;
      result=anygm_state_save_for_resume(g_libretro.engine,data,size,&written);
    }
  } else if(size<g_libretro.fixed_state_capacity){
    result=anygm_state_save_for_resume(g_libretro.engine,data,size,&written);
  } else {
    result=anygm_state_save(g_libretro.engine,data,size,&written);
  }
  if(result!=ANYGM_OK || written>size) return false;
  /* libretro persists the full advertised buffer, while AnyGM records its exact logical size in
   * the state header. Clear the capacity tail so files and rewind deltas never contain stale host
   * memory and remain deterministic for an identical runtime state. */
  if(written<size) memset((uint8_t*)data+written,0,size-written);
  return true;
}

bool retro_unserialize(const void *data,size_t size){
  if(!g_libretro.loaded) return false;
  if(g_libretro.reset_pending_frame && g_libretro.startup_ring_compact &&
     g_libretro.startup_resume_capacity &&
     size<=g_libretro.startup_resume_capacity){
    if(!g_libretro.reset_ring_rejection_reported){
      g_libretro.reset_ring_rejection_reported=true;
      libretro_log(RETRO_LOG_INFO,
                   "Ignored a pre-reset rewind slot until the restarted runtime produced its "
                   "first frame\n");
    }
    return false;
  }
  return anygm_state_load(g_libretro.engine,data,size)==ANYGM_OK;
}

void retro_cheat_reset(void){
  if(!g_libretro.loaded) return;
  for(uint32_t i=0;i<ANYGM_MAX_RUNTIME_OVERRIDES;i++) if(g_libretro.override_used[i]){
    anygm_set_runtime_override(g_libretro.engine,i,0,NULL);
    g_libretro.override_used[i]=0;
  }
}

void retro_cheat_set(unsigned index,bool enabled,const char *code){
  if(!g_libretro.loaded || index>=ANYGM_MAX_RUNTIME_OVERRIDES) return;
  if(anygm_set_runtime_override(g_libretro.engine,index,enabled?1u:0u,code)==ANYGM_OK)
    g_libretro.override_used[index]=enabled?1u:0u;
}

void *retro_get_memory_data(unsigned id){ (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id){ (void)id; return 0; }
