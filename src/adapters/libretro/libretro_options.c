/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct retro_variable g_variables[]={
  {"anygm_start_room","Start room; Full game"},
  {"anygm_gamepad","Gamepad connected; On|Off"},
  {"anygm_god","God mode; Off|On"},
  {"anygm_room_skip","Room-skip button; Off|Select|Start"},
  {"anygm_mouse","Mouse input; Auto|Absolute (pointer)|Relative (delta)"},
  {"anygm_aspect_ratio_force","Aspect ratio force; None|4:3|16:9|21:9"},
  {"anygm_width_resolution","Width resolution; Game Base|64|128|144|160|176|192|200|224|240|256|288|300|320|352|360|384|400|416|448|480|512|560|576|600|640|704|720|768|800|832|854|896|960|1024|1080|1152|1200|1280|1360|1366|1400|1440|1536|1600|1680|1728|1792|1920|2048|2160|2304|2400|2560|2880|3200|3440|3840"},
  {"anygm_height_resolution","Height resolution; Game Base|64|128|144|160|176|180|192|200|216|224|240|256|270|288|300|320|350|360|384|400|432|448|450|480|512|540|576|600|640|720|768|800|864|900|960|1024|1050|1080|1152|1200|1280|1350|1440|1536|1600|1800|1920|2160"},
  {"anygm_fast_alpha_cull","Renderer low-alpha cull; Performance|Exact|Light|Medium|Aggressive"},
  {"anygm_render_game_resolution","Render at game resolution; On|Off"},
  {"anygm_clear_local_data","Clear local data on load; Off|On"},
  {"anygm_embedded_shaders","Embedded CRT shader; On|Off"},
  {"anygm_crt_scanlines","CRT scanlines; On|Off"},
  {"anygm_crt_mask","CRT aperture mask; On|Off"},
  {"anygm_crt_gamma","CRT gamma; On|Off"},
  {"anygm_crt_curvature","CRT curvature; Auto|On|Off"},
  {"anygm_crt_vignette","CRT corner vignette; Auto|On|Off"},
  {NULL,NULL}
};

static const char *option_value(const char *key){
  struct retro_variable variable={key,NULL};
  if(!g_libretro.environment ||
     !g_libretro.environment(RETRO_ENVIRONMENT_GET_VARIABLE,&variable)) return NULL;
  return variable.value;
}

static uint32_t option_on(const char *key,uint32_t fallback){
  const char *value=option_value(key);
  if(!value) return fallback;
  return !strcmp(value,"On")?1u:0u;
}

static int32_t option_tristate(const char *key){
  const char *value=option_value(key);
  if(!value || !strcmp(value,"Auto")) return -1;
  return !strcmp(value,"On")?1:0;
}

static uint32_t option_resolution(const char *key){
  const char *value=option_value(key);
  if(!value || !strcmp(value,"Game Base")) return 0;
  long parsed=strtol(value,NULL,10);
  return parsed>0?(uint32_t)parsed:0;
}

void libretro_options_register(void){
  if(g_libretro.environment)
    g_libretro.environment(RETRO_ENVIRONMENT_SET_VARIABLES,g_variables);
}

/* The start room is the one option whose choices only exist once content is loaded. Declaring it
 * with the entry point alone leaves a host offering a single value, so the chooser a player sees
 * is empty of rooms; the list is published again with the content's own names behind it.
 *
 * Each choice carries its index first because that is what the option reader converts back, and
 * the name after it because an index alone names nothing. */
static char *g_start_room_values;

static void append_room_choice(char *out,size_t capacity,size_t *written,
                               uint32_t index,const char *name){
  char choice[160];
  int length=snprintf(choice,sizeof choice,"|%u: %s",(unsigned)index,name);
  if(length<=0) return;
  /* A separator inside a value would split it into two choices the reader cannot convert. */
  for(char *cursor=choice;*cursor;cursor++) if(*cursor=='|' && cursor!=choice) *cursor='/';
  if(*written+(size_t)length+1>capacity) return;
  memcpy(out+*written,choice,(size_t)length+1);
  *written+=(size_t)length;
}

void libretro_options_publish_rooms(void){
  if(!g_libretro.environment || !g_libretro.engine) return;
  uint32_t count=0;
  if(anygm_get_room_count(g_libretro.engine,&count)!=ANYGM_OK || !count) return;
  size_t capacity=64+(size_t)count*160;
  char *values=malloc(capacity);
  if(!values) return;
  size_t written=(size_t)snprintf(values,capacity,"Start room; Full game");
  uint32_t published=0;
  for(uint32_t index=0;index<count;index++){
    char name[128];
    if(anygm_get_room_name(g_libretro.engine,index,name,sizeof name)!=ANYGM_OK) continue;
    size_t before=written;
    append_room_choice(values,capacity,&written,index,name[0]?name:"room");
    if(written!=before) published++;
  }
  if(published<count)
    libretro_log(RETRO_LOG_WARN,"Start room lists %u of %u rooms; the rest did not fit\n",
                 (unsigned)published,(unsigned)count);
  for(size_t i=0;g_variables[i].key;i++){
    if(strcmp(g_variables[i].key,"anygm_start_room")) continue;
    g_variables[i].value=values;
    free(g_start_room_values);
    g_start_room_values=values;
    libretro_options_register();
    return;
  }
  free(values);
}

void libretro_options_apply(bool all_fields){
  if(!g_libretro.engine) return;
  AnygmConfig *config=&g_libretro.config;
  config->struct_size=sizeof *config;
  config->present_width=option_resolution("anygm_width_resolution");
  config->present_height=option_resolution("anygm_height_resolution");
  const char *value=option_value("anygm_aspect_ratio_force");
  config->aspect_mode=value&&!strcmp(value,"4:3")?1u:
                      value&&!strcmp(value,"16:9")?2u:
                      value&&!strcmp(value,"21:9")?3u:0u;
  value=option_value("anygm_mouse");
  config->mouse_mode=value&&strstr(value,"Absolute")?1u:
                     value&&strstr(value,"Relative")?2u:0u;
  value=option_value("anygm_room_skip");
  config->room_skip_button=value&&!strcmp(value,"Select")?1u:
                           value&&!strcmp(value,"Start")?2u:0u;
  config->god_mode=option_on("anygm_god",0);
  config->crt_mask=option_on("anygm_crt_mask",1);
  config->crt_scanlines=option_on("anygm_crt_scanlines",1);
  config->crt_gamma=option_on("anygm_crt_gamma",1);
  config->crt_curvature=option_tristate("anygm_crt_curvature");
  config->crt_vignette=option_tristate("anygm_crt_vignette");
  config->embedded_shaders=option_on("anygm_embedded_shaders",1);
  /* Content that offers a gamepad-only path checks this before the player can reach any menu, so
   * a host that never sets the option must still report a pad. */
  config->gamepad_connected=option_on("anygm_gamepad",1);
  value=option_value("anygm_fast_alpha_cull");
  config->fast_alpha_cull=!value||!strcmp(value,"Performance")?24u:
                          value&&!strcmp(value,"Light")?1u:
                          value&&!strcmp(value,"Medium")?4u:
                          value&&!strcmp(value,"Aggressive")?32u:0u;
  value=option_value("anygm_start_room");
  config->start_room=value&&strcmp(value,"Full game")?(int32_t)strtol(value,NULL,10):-1;
  /* Matching a requested window larger than the view costs a full software upscale carrying no
     detail the host would not produce itself, and reshapes content whose window proportions
     differ from its view. Rasterizing at the view is the default; the window-matching path stays
     available for content that depends on it. */
  config->present_logical_raster=option_on("anygm_render_game_resolution",1);
  config->clear_local_data=option_on("anygm_clear_local_data",0);

  AnygmConfigDelta delta;
  memset(&delta,0,sizeof delta);
  delta.struct_size=sizeof delta;
  delta.values=*config;
  delta.fields=all_fields?UINT64_MAX:
      ANYGM_CONFIG_PRESENT_WIDTH|ANYGM_CONFIG_PRESENT_HEIGHT|ANYGM_CONFIG_ASPECT_MODE|
      ANYGM_CONFIG_MOUSE_MODE|ANYGM_CONFIG_ROOM_SKIP_BUTTON|ANYGM_CONFIG_GOD_MODE|
      ANYGM_CONFIG_CRT_MASK|ANYGM_CONFIG_CRT_SCANLINES|ANYGM_CONFIG_CRT_GAMMA|
      ANYGM_CONFIG_CRT_CURVATURE|ANYGM_CONFIG_CRT_VIGNETTE|
      ANYGM_CONFIG_EMBEDDED_SHADERS|ANYGM_CONFIG_GAMEPAD_CONNECTED|
      ANYGM_CONFIG_FAST_ALPHA_CULL|ANYGM_CONFIG_START_ROOM|
      ANYGM_CONFIG_PRESENT_LOGICAL_RASTER|ANYGM_CONFIG_CLEAR_LOCAL_DATA;
  anygm_set_config(g_libretro.engine,&delta);
}
