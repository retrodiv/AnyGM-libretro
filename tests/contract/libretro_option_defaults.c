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
static bool (*menu_time_visibility)(void);
static const struct retro_core_option_v2_category *declared_categories;
static const struct retro_core_option_v2_definition *declared_definitions;
static const struct retro_variable *declared_variables;
static unsigned host_options_version=2;
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
  menu_time_visibility=NULL;
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
  static const char *const expected[]={"video","input","shaders","development",NULL};
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

/* The parts of an effect cannot act while the effect that draws them is off. A player turns that
 * effect off from inside the host's menu, which is precisely when no frame is running to notice,
 * so the answer has to be reachable without one: the five parts must leave the menu still open in
 * front of the player, not the next one they open. */
static void hidden_settings_are_the_ones_that_cannot_act(void){
  begin(2,3);
  if(!menu_time_visibility){
    complain("no way was offered to answer while the host's menu is open");
    return;
  }
  answered_value="On";
  menu_time_visibility();
  if(shown("anygm_crt_scanlines")!=1 || shown("anygm_crt_vignette")!=1)
    complain("the CRT parts stay hidden while the shader that draws them is on");

  /* Nothing is applied here on purpose: this is the state a paused core is in. */
  answered_value="Off";
  if(!menu_time_visibility())
    complain("a changed visibility was reported as no change");
  if(shown("anygm_crt_scanlines")!=0 || shown("anygm_crt_vignette")!=0)
    complain("the CRT parts stay offered while the shader that draws them is off");
  if(menu_time_visibility())
    complain("an unchanged visibility was reported as a change");
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
  unset_settings_keep_content_reachable();
  monitor_dimensions_reach_the_virtual_monitor_fields();
  monitor_dimensions_follow_the_window_raster();
  the_forced_shape_follows_the_logical_raster();
  culling_names_rise_with_their_thresholds();
  hidden_settings_are_the_ones_that_cannot_act();
  loaded_content_names_its_rooms();
  rooms_past_one_list_stay_reachable();
  older_hosts_get_every_room_at_once();
  a_restarted_core_declares_its_settings_again();
  /* Everything the declarations hold is released here, so a leak checker running this case sees
   * whatever the teardown forgot. */
  libretro_options_release();
  if(failures) return 1;
  puts("libretro option defaults: ok");
  return 0;
}
