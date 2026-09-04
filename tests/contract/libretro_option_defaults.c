/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* What a host is told about the settings, and what it gets back when the player answers.
 *
 * A frontend reports a setting only once the player has changed it: until then every query is
 * answered with "unset", and the core supplies the value itself. Those fallbacks are behaviour,
 * not presentation, and content can be unreachable when one of them is wrong.
 *
 * The grouped declaration is only understood by newer hosts, so the same settings are declared
 * twice, in two shapes, from one table. Both shapes are exercised here: the one that carries
 * categories and a fixed number of values per setting, and the flat one that carries neither. */
#include "libretro_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *answered_value;
static const char *answered_raster_value;
static const char *answered_monitor_value;
static const char *answered_aspect_value;
static const char *answered_page_value;
static const char *answered_hybrid_gpu_value;
static const char *answered_game_shaders_value;
static const char *answered_unsupported_shaders_value;
static bool (*menu_time_visibility)(void);
static const struct retro_core_option_v2_category *declared_categories;
static const struct retro_core_option_v2_definition *declared_definitions;
static const struct retro_variable *declared_variables;
static unsigned host_options_version=2;
static unsigned grouped_publish_count;
static AnygmConfigDelta applied_config;
static unsigned config_apply_count;

/* Whatever the core last told the host to show or hide, by key. */
#define MAX_VISIBILITY 32
static struct { char key[64]; bool visible; } visibility[MAX_VISIBILITY];
static size_t visibility_count;

static void record_visibility(const struct retro_core_option_display *display){
  if(!display || !display->key) return;
  for(size_t i=0;i<visibility_count;i++)
    if(!strcmp(visibility[i].key,display->key)){ visibility[i].visible=display->visible; return; }
  if(visibility_count>=MAX_VISIBILITY) return;
  snprintf(visibility[visibility_count].key,sizeof visibility[0].key,"%s",display->key);
  visibility[visibility_count].visible=display->visible;
  visibility_count++;
}

static int shown(const char *key){
  for(size_t i=0;i<visibility_count;i++)
    if(!strcmp(visibility[i].key,key)) return visibility[i].visible?1:0;
  return -1;
}

static bool environment_callback(unsigned command,void *data){
  switch(command){
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
      *(unsigned*)data=host_options_version;
      return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:{
      const struct retro_core_options_v2 *options=data;
      declared_categories=options?options->categories:NULL;
      declared_definitions=options?options->definitions:NULL;
      grouped_publish_count++;
      /* A fresh declaration starts with every option visible. Frontends own their copy and are not
       * required to carry display hints across replacement of that copy. */
      visibility_count=0;
      return true; }
    case RETRO_ENVIRONMENT_SET_VARIABLES:
      declared_variables=(const struct retro_variable*)data;
      return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:{
      const struct retro_core_options_update_display_callback *c=data;
      menu_time_visibility=c?c->callback:NULL;
      return true; }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
      record_visibility((const struct retro_core_option_display*)data);
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:{
      struct retro_variable *variable=data;
      if(!variable) return false;
      const char *value=answered_value;
      if(variable->key && !strcmp(variable->key,"anygm_render_game_resolution") &&
         answered_raster_value)
        value=answered_raster_value;
      if(variable->key &&
         (!strcmp(variable->key,"anygm_width_resolution") ||
          !strcmp(variable->key,"anygm_height_resolution")) && answered_monitor_value)
        value=answered_monitor_value;
      if(variable->key && !strcmp(variable->key,"anygm_aspect_ratio_force") &&
         answered_aspect_value)
        value=answered_aspect_value;
      if(variable->key && !strcmp(variable->key,"anygm_start_room_page") &&
         answered_page_value)
        value=answered_page_value;
      if(variable->key && !strcmp(variable->key,"anygm_hybrid_gpu") &&
         answered_hybrid_gpu_value)
        value=answered_hybrid_gpu_value;
      if(variable->key && !strcmp(variable->key,"anygm_content_shader_readback") &&
         answered_game_shaders_value)
        value=answered_game_shaders_value;
      if(variable->key && !strcmp(variable->key,"anygm_report_shaders_compiled") &&
         answered_unsupported_shaders_value)
        value=answered_unsupported_shaders_value;
      variable->value=value;
      return value!=NULL; }
    default:
      return false;
  }
}

/* The adapter's global context and the engine entry points it drives both live outside this
 * translation unit's dependency set; the option parsing under test needs only their presence. */
LibretroAdapter g_libretro;
AnygmResult anygm_set_config(AnygmEngine *engine,const AnygmConfigDelta *delta){
  (void)engine;
  if(delta){
    applied_config=*delta;
    config_apply_count++;
  }
  return ANYGM_OK;
}

/* Enough rooms to overflow one grouped list, so the stretch selector has work to do. */
static uint32_t stub_room_count=3;

AnygmResult anygm_get_room_count(const AnygmEngine *engine,uint32_t *count){
  (void)engine;
  if(!count) return ANYGM_ERROR_INVALID_STATE;
  *count=stub_room_count;
  return ANYGM_OK;
}

AnygmResult anygm_get_room_name(const AnygmEngine *engine,uint32_t index,
                                char *name,size_t capacity){
  (void)engine;
  if(!name || !capacity) return ANYGM_ERROR_INVALID_STATE;
  if(index>=stub_room_count) return ANYGM_ERROR_INVALID_ARGUMENT;
  snprintf(name,capacity,"room_%u",(unsigned)index);
  return ANYGM_OK;
}

void libretro_log(enum retro_log_level level,const char *format,...){
  (void)level;
  (void)format;
}

static int failures;

static void expect(const char *label,unsigned actual,unsigned expected){
  if(actual==expected) return;
  fprintf(stderr,"libretro option defaults: %s expected %u, got %u\n",label,expected,actual);
  failures++;
}

static void complain(const char *message){
  fprintf(stderr,"libretro option defaults: %s\n",message);
  failures++;
}

/* libretro_options_apply refuses to run without a loaded engine. The pointer is only tested for
 * presence, so a non-null placeholder is enough to reach the option parsing under test. */
static AnygmEngine *const engine_placeholder=(AnygmEngine*)&failures;

static const struct retro_core_option_v2_definition *definition(const char *key){
  for(const struct retro_core_option_v2_definition *d=declared_definitions;d&&d->key;d++)
    if(!strcmp(d->key,key)) return d;
  return NULL;
}

static const char *flat_text(const char *key){
  for(const struct retro_variable *v=declared_variables;v&&v->key;v++)
    if(!strcmp(v->key,key)) return v->value;
  return NULL;
}

static unsigned value_count(const struct retro_core_option_v2_definition *d){
  unsigned n=0;
  while(d && d->values[n].value) n++;
  return n;
}

static void begin(unsigned version,uint32_t rooms){
  host_options_version=version;
  stub_room_count=rooms;
  declared_categories=NULL;
  declared_definitions=NULL;
  declared_variables=NULL;
  visibility_count=0;
  answered_value=NULL;
  answered_raster_value=NULL;
  answered_monitor_value=NULL;
  answered_aspect_value=NULL;
  answered_page_value=NULL;
  answered_hybrid_gpu_value=NULL;
  answered_game_shaders_value=NULL;
  answered_unsupported_shaders_value=NULL;
  menu_time_visibility=NULL;
  grouped_publish_count=0;
  memset(&applied_config,0,sizeof applied_config);
  config_apply_count=0;
  memset(&g_libretro.config,0,sizeof g_libretro.config);
  g_libretro.environment=environment_callback;
  g_libretro.engine=engine_placeholder;
  libretro_options_register();
}

/* Grouping is the whole point of the newer declaration: a setting with no group lands loose in
 * the host's menu, beside the groups rather than inside one. */
static void every_setting_sits_in_a_group(void){
  begin(2,3);
  if(!declared_categories || !declared_definitions){
    complain("the grouped declaration was never made");
    return;
  }
  static const char *const expected[]={"video","input","development",NULL};
  size_t index=0;
  for(const struct retro_core_option_v2_category *c=declared_categories;c->key;c++,index++){
    if(!expected[index] || strcmp(c->key,expected[index])){
      complain("the groups are not the expected ones, in order");
      return;
    }
    if(!c->desc || !c->desc[0] || !c->info || !c->info[0]){
      complain("a group carries no name or no description");
      return;
    }
  }
  if(expected[index]) complain("a group is missing from the declaration");

  for(const struct retro_core_option_v2_definition *d=declared_definitions;d->key;d++){
    if(!d->category_key || !d->category_key[0]){
      fprintf(stderr,"libretro option defaults: \"%s\" belongs to no group\n",d->key);
      failures++;
      continue;
    }
    int known=0;
    for(size_t i=0;expected[i];i++) if(!strcmp(d->category_key,expected[i])) known=1;
    if(!known){
      fprintf(stderr,"libretro option defaults: \"%s\" names an unknown group \"%s\"\n",
              d->key,d->category_key);
      failures++;
    }
    if(!d->info || !d->info[0]){
      fprintf(stderr,"libretro option defaults: \"%s\" carries no help text\n",d->key);
      failures++;
    }
    if(value_count(d)>=RETRO_NUM_CORE_OPTION_VALUES_MAX){
      fprintf(stderr,"libretro option defaults: \"%s\" fills the value array\n",d->key);
      failures++;
    }
  }
}

/* The graphics-device choice makes these two settings exact complements. With a device, its
 * shaders are available and the useful choice is how mid-frame results are read back. Without
 * one, no read-back policy can act and the useful choice is what unsupported shaders tell the
 * content. Hidden selections stay with the frontend, but they must not keep acting in the engine. */
static void shader_settings_follow_the_graphics_device(void){
  begin(2,3);
  const struct retro_core_option_v2_definition *game=definition("anygm_content_shader_readback");
  const struct retro_core_option_v2_definition *unsupported=
    definition("anygm_report_shaders_compiled");
  if(!unsupported){ complain("the unsupported-shader setting is missing"); return; }
#if ANYGM_HARDWARE_RENDER
  if(!game){ complain("a hardware build offers no game-shader setting"); return; }
  if(!definition("anygm_hybrid_gpu")) complain("a hardware build offers no graphics device");
#else
  if(game) complain("a software-only build offers an inoperative game-shader setting");
  if(definition("anygm_hybrid_gpu")) complain("a software-only build offers a graphics device");
#endif
#if ANYGM_HARDWARE_RENDER
  if(strcmp(game->desc,"Game shaders") || strcmp(game->category_key,"video") ||
     value_count(game)!=3 || strcmp(game->values[0].value,"Performance") ||
     strcmp(game->values[1].value,"Exact") || strcmp(game->values[2].value,"Off") ||
     strcmp(game->default_value,"Performance"))
    complain("the game-shader setting does not expose Performance, Exact and Off in Video");
  if(!strstr(game->info,"previous frame") || !strstr(game->info,"Exact waits") ||
     !strstr(game->info,"Off draws") || !strstr(game->info,"frontend shaders are unaffected"))
    complain("the game-shader setting does not explain its three policies and boundaries");
#endif
  if(strcmp(unsupported->desc,"Unsupported game shaders") ||
     strcmp(unsupported->category_key,"video") || value_count(unsupported)!=2 ||
     strcmp(unsupported->values[0].value,"Report available") ||
     strcmp(unsupported->values[1].value,"Report unavailable") ||
     strcmp(unsupported->default_value,"Report available"))
    complain("the unsupported-shader setting does not expose its reporting policy in Video");
  if(!strstr(unsupported->info,"shader_is_compiled()") ||
     !strstr(unsupported->info,"Report available") ||
     !strstr(unsupported->info,"Report unavailable") ||
     !strstr(unsupported->info,"Hybrid GPU rendering is None"))
    complain("the unsupported-shader setting does not explain its reporting policies and scope");
  if(!menu_time_visibility){ complain("no way was offered to update shader-option visibility"); return; }

  answered_hybrid_gpu_value="None";
  answered_game_shaders_value="Exact";
  answered_unsupported_shaders_value="Report unavailable";
  menu_time_visibility();
#if ANYGM_HARDWARE_RENDER
  if(shown("anygm_content_shader_readback")!=0 ||
     shown("anygm_report_shaders_compiled")!=1)
    complain("the shader settings are not split onto the no-device path");
#else
  if(shown("anygm_content_shader_readback")!=-1 ||
     shown("anygm_report_shaders_compiled")!=1)
    complain("the software-only build publishes an inoperative shader setting");
#endif
  libretro_options_apply(true);
  expect("no-device shader execution",applied_config.values.content_shader_readback,
         ANYGM_SHADER_READBACK_NEVER);
  expect("restored unavailable report",applied_config.values.report_all_shaders_compiled,0u);
  expect("no graphics device expected",applied_config.values.content_shader_device_expected,0u);

#if !ANYGM_HARDWARE_RENDER
  /* A frontend can retain a value once published by another build. It cannot turn support which
   * was compiled out back on, expose an inoperative setting, or change what content is told. */
  answered_hybrid_gpu_value="OpenGL";
  if(menu_time_visibility())
    complain("a stale graphics-device value changed software-only option visibility");
  libretro_options_apply(false);
  expect("software-only shader execution",applied_config.values.content_shader_readback,
         ANYGM_SHADER_READBACK_NEVER);
  expect("software-only unavailable report",applied_config.values.report_all_shaders_compiled,0u);
  expect("software-only graphics device",applied_config.values.content_shader_device_expected,0u);
  return;
#else
  /* The menu exposes the choice for the next load immediately, but the loaded software session
   * cannot acquire a graphics device halfway through its frame pipeline. */
  g_libretro.loaded=true;
  answered_hybrid_gpu_value="OpenGL";
  if(!menu_time_visibility())
    complain("changing graphics device was reported as no visibility change");
  if(shown("anygm_content_shader_readback")!=1 ||
     shown("anygm_report_shaders_compiled")!=0)
    complain("the shader settings are not split onto the graphics-device path");
  libretro_options_apply(false);
  expect("loaded software shader execution",applied_config.values.content_shader_readback,
         ANYGM_SHADER_READBACK_NEVER);
  expect("loaded software report",applied_config.values.report_all_shaders_compiled,0u);
  expect("loaded software device",applied_config.values.content_shader_device_expected,0u);

  /* Closing the content makes the selected graphics path effective and recovers its saved mode. */
  g_libretro.loaded=false;
  libretro_options_apply(true);
  expect("exact shader execution",applied_config.values.content_shader_readback,
         ANYGM_SHADER_READBACK_ALWAYS);
  expect("hidden unavailable report",applied_config.values.report_all_shaders_compiled,1u);
  expect("graphics device expected",applied_config.values.content_shader_device_expected,1u);

  answered_game_shaders_value="Performance";
  libretro_options_apply(false);
  expect("performance shader execution",applied_config.values.content_shader_readback,
         ANYGM_SHADER_READBACK_BUDGETED);
  answered_game_shaders_value="Off";
  libretro_options_apply(false);
  expect("disabled shader execution",applied_config.values.content_shader_readback,
         ANYGM_SHADER_READBACK_NEVER);

  /* Returning to software exposes its reporting choice, but a loaded GPU session keeps running
   * until content closes. The next load recovers the choice which was hidden, not overwritten. */
  g_libretro.loaded=true;
  answered_hybrid_gpu_value="None";
  menu_time_visibility();
  libretro_options_apply(false);
  expect("loaded GPU shader execution",applied_config.values.content_shader_readback,
         ANYGM_SHADER_READBACK_NEVER);
  expect("loaded GPU report",applied_config.values.report_all_shaders_compiled,1u);
  expect("loaded GPU device",applied_config.values.content_shader_device_expected,1u);
  g_libretro.loaded=false;
  libretro_options_apply(true);
  expect("recovered unavailable report",applied_config.values.report_all_shaders_compiled,0u);
  expect("hidden disabled shader execution",applied_config.values.content_shader_readback,
         ANYGM_SHADER_READBACK_NEVER);
#endif
}

static void unset_settings_keep_content_reachable(void){
  begin(2,3);
  answered_value=NULL;
  libretro_options_apply(true);
  /* The default resolves per content: a modern title that can read a pad gets one, and a
   * classic keyboard-era title keeps the RetroPad as its keyboard. */
  expect("no host value for the pad",g_libretro.config.gamepad_connected,ANYGM_GAMEPAD_AUTO);
  /* Resolving an unknown amount to none would leave the slowest setting in place unasked. */
  expect("no host value for the culling",g_libretro.config.fast_alpha_cull,24u);
  expect("no host value for the raster",g_libretro.config.present_logical_raster,1u);
}

/* These dimensions describe the monitor content can query. They retain their stable frontend keys,
 * but the adapter must not translate them back into the former presentation-size API. */
static void monitor_dimensions_reach_the_virtual_monitor_fields(void){
  begin(2,3);
  const struct retro_core_option_v2_definition *width=definition("anygm_width_resolution");
  const struct retro_core_option_v2_definition *height=definition("anygm_height_resolution");
  if(!width || !height || strcmp(width->desc,"Monitor width") ||
     strcmp(height->desc,"Monitor height"))
    complain("the virtual-monitor dimensions are presented as framebuffer dimensions");

  answered_value="1920";
  libretro_options_apply(true);
  if(config_apply_count!=1 ||
     (applied_config.fields&(ANYGM_CONFIG_MONITOR_WIDTH|ANYGM_CONFIG_MONITOR_HEIGHT)) !=
       (ANYGM_CONFIG_MONITOR_WIDTH|ANYGM_CONFIG_MONITOR_HEIGHT))
    complain("the virtual-monitor fields were not sent to the engine");
  expect("configured monitor width",applied_config.values.monitor_width,1920u);
  expect("configured monitor height",applied_config.values.monitor_height,1920u);

  answered_value="Game Base";
  libretro_options_apply(false);
  expect("fallback monitor width",applied_config.values.monitor_width,0u);
  expect("fallback monitor height",applied_config.values.monitor_height,0u);
}

/* A logical-raster frame has no monitor-sized drawing space, so the monitor dimensions neither
 * act nor occupy the menu while that path is selected. The frontend still owns their selected
 * values: returning to the window-raster path must recover them without asking the player again. */
static void monitor_dimensions_follow_the_window_raster(void){
  begin(2,3);
  if(!menu_time_visibility){
    complain("no way was offered to update monitor-option visibility");
    return;
  }
  answered_value="On";
  answered_monitor_value="1920";
  answered_raster_value="On";
  menu_time_visibility();
  if(shown("anygm_width_resolution")!=0 || shown("anygm_height_resolution")!=0)
    complain("monitor dimensions stay offered while rendering at game resolution");
  libretro_options_apply(true);
  expect("logical-raster monitor width",applied_config.values.monitor_width,0u);
  expect("logical-raster monitor height",applied_config.values.monitor_height,0u);
  expect("logical-raster selection",applied_config.values.present_logical_raster,1u);

  answered_raster_value="Off";
  if(!menu_time_visibility())
    complain("showing monitor dimensions was reported as no visibility change");
  if(shown("anygm_width_resolution")!=1 || shown("anygm_height_resolution")!=1)
    complain("monitor dimensions stay hidden while rendering at the presentation window");
  libretro_options_apply(false);
  expect("window-raster monitor width",applied_config.values.monitor_width,1920u);
  expect("window-raster monitor height",applied_config.values.monitor_height,1920u);
  expect("window-raster selection",applied_config.values.present_logical_raster,0u);

  answered_raster_value="On";
  menu_time_visibility();
  libretro_options_apply(false);
  answered_raster_value="Off";
  menu_time_visibility();
  libretro_options_apply(false);
  expect("restored monitor width",applied_config.values.monitor_width,1920u);
  expect("restored monitor height",applied_config.values.monitor_height,1920u);
}

/* The forced shape reshapes the game's own raster, which only the logical-raster path delivers.
 * It is therefore the exact complement of the monitor dimensions: one of the two is offered, never
 * both and never neither. A selection made on one path must also stop acting once the player
 * leaves it, or a shape they can no longer see keeps reshaping the frame. */
static void the_forced_shape_follows_the_logical_raster(void){
  begin(2,3);
  if(!menu_time_visibility){
    complain("no way was offered to update the forced shape's visibility");
    return;
  }
  answered_value="On";
  answered_aspect_value="16:9";
  answered_monitor_value="1920";

  answered_raster_value="Off";
  menu_time_visibility();
  answered_raster_value="On";
  if(!menu_time_visibility())
    complain("showing the forced shape was reported as no visibility change");
  if(shown("anygm_aspect_ratio_force")!=1)
    complain("the forced shape stays hidden while rendering at game resolution");
  if(shown("anygm_width_resolution")!=0)
    complain("the forced shape and the monitor dimensions are offered together");
  libretro_options_apply(false);
  expect("forced shape at game resolution",applied_config.values.aspect_mode,2u);

  answered_raster_value="Off";
  menu_time_visibility();
  if(shown("anygm_aspect_ratio_force")!=0)
    complain("the forced shape stays offered while rendering at the presentation window");
  if(shown("anygm_width_resolution")!=1)
    complain("neither the forced shape nor the monitor dimensions are offered");
  libretro_options_apply(false);
  expect("forced shape at the presentation window",applied_config.values.aspect_mode,0u);

  /* The frontend keeps what the player chose, so returning recovers it without asking again. */
  answered_raster_value="On";
  menu_time_visibility();
  libretro_options_apply(false);
  expect("restored forced shape",applied_config.values.aspect_mode,2u);
}

/* The names describe how much is dropped, and the thresholds have to rise with them. */
static void culling_names_rise_with_their_thresholds(void){
  begin(2,3);
  static const struct { const char *name; unsigned threshold; } steps[]={
    {"None",0u},{"Light",1u},{"Medium",4u},{"High",24u},{"Maximum",32u}
  };
  unsigned previous=0;
  for(size_t i=0;i<sizeof steps/sizeof steps[0];i++){
    answered_value=steps[i].name;
    libretro_options_apply(true);
    expect(steps[i].name,g_libretro.config.fast_alpha_cull,steps[i].threshold);
    if(i && steps[i].threshold<=previous) complain("the amounts do not rise with their names");
    previous=steps[i].threshold;
  }
  /* A name from an earlier revision must not resolve to the far end of the scale. */
  answered_value="Performance";
  libretro_options_apply(true);
  expect("a name this revision does not know",g_libretro.config.fast_alpha_cull,24u);
}


/* The choices only exist once content is loaded, and the index a player picks has to survive the
 * trip through the value text back to the engine. */
static void loaded_content_names_its_rooms(void){
  begin(2,3);
  const struct retro_core_option_v2_definition *before=definition("anygm_start_room");
  if(!before || value_count(before)!=1) complain("rooms were offered before content was loaded");
  libretro_options_publish_rooms();
  const struct retro_core_option_v2_definition *after=definition("anygm_start_room");
  expect("rooms offered after loading",value_count(after),4u);
  if(!after || strcmp(after->values[0].value,"Full game") ||
     strcmp(after->values[3].value,"2: room_2"))
    complain("the room chooser does not name the content's rooms");
  if(shown("anygm_start_room_page")!=0)
    complain("a range selector is offered for content that needs none");

  answered_value="2: room_2";
  g_libretro.config.start_room=-1;
  libretro_options_apply(true);
  expect("the chosen index",(unsigned)g_libretro.config.start_room,2u);
  answered_value="Full game";
  libretro_options_apply(true);
  if(g_libretro.config.start_room!=-1)
    complain("the whole game did not stay the neutral choice");
}

/* A grouped declaration holds a fixed number of values, which no game is obliged to fit inside.
 * The rooms past that are offered a stretch at a time; none of them may become unreachable. */
static void rooms_past_one_list_stay_reachable(void){
  const uint32_t rooms=300;
  begin(2,rooms);
  libretro_options_publish_rooms();
  const struct retro_core_option_v2_definition *chooser=definition("anygm_start_room");
  const struct retro_core_option_v2_definition *ranges=definition("anygm_start_room_page");
  if(!chooser || !ranges){ complain("the room chooser vanished"); return; }
  if(shown("anygm_start_room_page")!=1)
    complain("no range selector is offered for content that overflows one list");

  unsigned span=value_count(chooser)-1u;
  unsigned stretches=value_count(ranges);
  if((unsigned long)span*stretches<rooms)
    complain("the offered stretches cannot cover every room");
  if(strcmp(chooser->values[1].value,"0: room_0"))
    complain("the first stretch does not start at the first room");

  /* Choosing the last stretch has to move the chooser onto it, and the index has to stay
   * absolute: a room named relative to its stretch would boot the wrong one. */
  char last[64];
  snprintf(last,sizeof last,"%s",ranges->values[stretches-1].value);
  answered_value=last;
  libretro_options_apply(true);
  chooser=definition("anygm_start_room");
  unsigned long first_offered=strtoul(chooser->values[1].value,NULL,10);
  if(first_offered!=(unsigned long)span*(stretches-1))
    complain("the last stretch does not begin where the one before it ended");
  unsigned offered=value_count(chooser);
  unsigned long final=strtoul(chooser->values[offered-1].value,NULL,10);
  expect("the last room offered",(unsigned)final,rooms-1u);
}

/* Replacing the option definitions also replaces the frontend's default-visible display state.
 * Loading content does exactly that to add its room choices, so every dependent visibility hint
 * has to be sent again even when the selected values themselves did not move. */
static void room_publication_reapplies_visibility(void){
  begin(2,3);
  answered_raster_value="On";
  libretro_options_publish_rooms();
  if(shown("anygm_width_resolution")!=0 || shown("anygm_height_resolution")!=0)
    complain("monitor dimensions were not hidden before room publication");
  libretro_options_publish_rooms();
  if(shown("anygm_width_resolution")!=0 || shown("anygm_height_resolution")!=0)
    complain("room publication reset the monitor dimensions to visible");
  if(shown("anygm_aspect_ratio_force")!=1)
    complain("room publication lost the logical-raster option visibility");
#if ANYGM_HARDWARE_RENDER
  if(shown("anygm_content_shader_readback")!=0 ||
     shown("anygm_report_shaders_compiled")!=1)
    complain("room publication lost the software shader-option visibility");
#else
  if(shown("anygm_content_shader_readback")!=-1 ||
     shown("anygm_report_shaders_compiled")!=1)
    complain("room publication introduced a software-only shader option");
#endif
}

/* A paged room chooser changes its first declaration from the placeholder "0" to the canonical
 * range label for page zero. They select the same page. Applying an unrelated live option must not
 * replace the complete option table while a frontend menu is using it. */
static void an_unrelated_update_keeps_paged_definitions_stable(void){
  begin(2,300);
  answered_page_value="0";
  answered_raster_value="On";
  libretro_options_publish_rooms();
  const struct retro_core_option_v2_definition *ranges=definition("anygm_start_room_page");
  if(!ranges || !ranges->default_value){
    complain("the paged room chooser has no default range");
    return;
  }
  char selected_range[64];
  snprintf(selected_range,sizeof selected_range,"%s",ranges->default_value);
  answered_page_value=selected_range;
  unsigned publications=grouped_publish_count;
  answered_raster_value="Off";
  libretro_options_apply(false);
  if(grouped_publish_count!=publications)
    complain("an unrelated live option republished the paged definitions");
}

/* Hosts that never learned about groups are given the flat declaration, which carries no ceiling
 * on values and therefore needs no stretches. */
static void older_hosts_get_every_room_at_once(void){
  const uint32_t rooms=300;
  begin(0,rooms);
  if(declared_definitions) complain("a host without groups was given the grouped declaration");
  if(!declared_variables){ complain("a host without groups was given nothing"); return; }
  libretro_options_publish_rooms();
  if(flat_text("anygm_start_room_page"))
    complain("a stretch selector is offered where nothing limits the list");
  const char *chooser=flat_text("anygm_start_room");
  if(!chooser){ complain("the flat declaration holds no room chooser"); return; }
  unsigned choices=1;
  for(const char *cursor=chooser;*cursor;cursor++) if(*cursor=='|') choices++;
  expect("rooms named at once",choices,rooms+1u);
  if(!strstr(chooser,"|299: room_299")) complain("the last room is missing from the flat list");

  /* The flat form takes its default from whichever value leads, so the default has to lead. */
  const char *culling=flat_text("anygm_alpha_cull");
  if(!culling || !strstr(culling,"; High|"))
    complain("the flat declaration does not lead with the shipped amount");
}

/* Tearing the core down releases what the declaration holds. Starting it again has to leave the
 * host with the settings declared, whether or not it hands its callback over a second time. */
static void a_restarted_core_declares_its_settings_again(void){
  begin(2,300);
  libretro_options_publish_rooms();
  libretro_options_release();
  declared_definitions=NULL;
  declared_variables=NULL;
  libretro_options_register();
  if(!definition("anygm_start_room"))
    complain("a core started again offers no settings at all");
  libretro_options_publish_rooms();
  const struct retro_core_option_v2_definition *chooser=definition("anygm_start_room");
  if(value_count(chooser)<2) complain("a core started again names no rooms");
}

int main(void){
  every_setting_sits_in_a_group();
  shader_settings_follow_the_graphics_device();
  unset_settings_keep_content_reachable();
  monitor_dimensions_reach_the_virtual_monitor_fields();
  monitor_dimensions_follow_the_window_raster();
  the_forced_shape_follows_the_logical_raster();
  culling_names_rise_with_their_thresholds();
  loaded_content_names_its_rooms();
  rooms_past_one_list_stay_reachable();
  room_publication_reapplies_visibility();
  an_unrelated_update_keeps_paged_definitions_stable();
  older_hosts_get_every_room_at_once();
  a_restarted_core_declares_its_settings_again();
  /* Everything the declarations hold is released here, so a leak checker running this case sees
   * whatever the teardown forgot. */
  libretro_options_release();
  if(failures) return 1;
  puts("libretro option defaults: ok");
  return 0;
}
