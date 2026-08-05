/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* A frontend reports an option only once the player has changed it: until then every query is
 * answered with "unset", and the core must supply the value itself. Content that gates its title
 * screen on a connected pad is unreachable when that fallback reports no pad, so the fallback is
 * behaviour rather than presentation and is pinned here.
 *
 * The declared value order matters for the same reason: a frontend that writes its own default
 * takes the first listed value, so the list must open with the connected state. */
#include "libretro_internal.h"

#include <stdio.h>
#include <string.h>

static const char *answered_value;
static int variables_declared;
static const struct retro_variable *declared_variables;

static bool environment_callback(unsigned command,void *data){
  if(command==RETRO_ENVIRONMENT_SET_VARIABLES){
    declared_variables=(const struct retro_variable*)data;
    variables_declared=1;
    return true;
  }
  if(command==RETRO_ENVIRONMENT_GET_VARIABLE){
    struct retro_variable *variable=(struct retro_variable*)data;
    if(!variable) return false;
    variable->value=answered_value;
    return answered_value!=NULL;
  }
  return false;
}

/* The adapter's global context and the engine entry points it drives both live outside this
 * translation unit's dependency set; the option parsing under test needs only their presence. */
LibretroAdapter g_libretro;
AnygmResult anygm_set_config(AnygmEngine *engine,const AnygmConfigDelta *delta){
  (void)engine;
  (void)delta;
  return ANYGM_OK;
}

static const char *const stub_rooms[]={"room_a","room_b","room_c"};
#define STUB_ROOM_COUNT (sizeof stub_rooms/sizeof stub_rooms[0])

AnygmResult anygm_get_room_count(const AnygmEngine *engine,uint32_t *count){
  (void)engine;
  if(!count) return ANYGM_ERROR_INVALID_STATE;
  *count=(uint32_t)STUB_ROOM_COUNT;
  return ANYGM_OK;
}

AnygmResult anygm_get_room_name(const AnygmEngine *engine,uint32_t index,
                                char *name,size_t capacity){
  (void)engine;
  if(!name || !capacity) return ANYGM_ERROR_INVALID_STATE;
  if(index>=STUB_ROOM_COUNT) return ANYGM_ERROR_INVALID_ARGUMENT;
  snprintf(name,capacity,"%s",stub_rooms[index]);
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

/* libretro_options_apply refuses to run without a loaded engine. The pointer is only tested for
 * presence, so a non-null placeholder is enough to reach the option parsing under test. */
static AnygmEngine *const engine_placeholder=(AnygmEngine*)&failures;

static unsigned gamepad_for(const char *host_value){
  answered_value=host_value;
  g_libretro.config.gamepad_connected=0xFFu;
  libretro_options_apply(true);
  return g_libretro.config.gamepad_connected;
}

static void unset_option_reports_a_pad(void){
  expect("no host value",gamepad_for(NULL),1u);
}

static void host_value_still_decides(void){
  expect("host says Off",gamepad_for("Off"),0u);
  expect("host says On",gamepad_for("On"),1u);
}

static void declared_order_opens_connected(void){
  if(!variables_declared || !declared_variables){
    fprintf(stderr,"libretro option defaults: the core never declared its variables\n");
    failures++;
    return;
  }
  for(const struct retro_variable *variable=declared_variables;variable->key;variable++){
    if(strcmp(variable->key,"anygm_gamepad")) continue;
    const char *values=strchr(variable->value,';');
    if(!values || strncmp(values,"; On",4)){
      fprintf(stderr,"libretro option defaults: anygm_gamepad must list On first, got \"%s\"\n",
              variable->value);
      failures++;
    }
    return;
  }
  fprintf(stderr,"libretro option defaults: anygm_gamepad is not declared\n");
  failures++;
}

/* The choices for this option only exist once content is loaded, so the entry-point declaration
 * can only offer the whole game. A host asking the player which room to enter has nothing to show
 * until the list is published again, and the index it collects has to survive the round trip
 * through the value text back to the reader. */
static const char *declared_start_room(void){
  for(const struct retro_variable *variable=declared_variables;variable&&variable->key;variable++)
    if(!strcmp(variable->key,"anygm_start_room")) return variable->value;
  return NULL;
}

static void loaded_content_names_its_rooms(void){
  const char *before=declared_start_room();
  if(!before || strstr(before,"a room")){
    fprintf(stderr,"libretro option defaults: rooms were offered before content was loaded\n");
    failures++;
    return;
  }
  libretro_options_publish_rooms();
  const char *after=declared_start_room();
  if(!after || !strstr(after,"Full game") || !strstr(after,"|2: a room")){
    fprintf(stderr,"libretro option defaults: the room chooser is empty, got \"%s\"\n",
            after?after:"(nothing)");
    failures++;
    return;
  }
  /* What the player picks is text; what the engine takes is the index in front of it. */
  answered_value="2: a room";
  g_libretro.config.start_room=-1;
  libretro_options_apply(true);
  expect("chosen room index",(unsigned)g_libretro.config.start_room,2u);
  answered_value="Full game";
  libretro_options_apply(true);
  if(g_libretro.config.start_room!=-1){
    fprintf(stderr,"libretro option defaults: the whole game did not stay the neutral choice\n");
    failures++;
  }
}

int main(void){
  g_libretro.environment=environment_callback;
  g_libretro.engine=engine_placeholder;
  libretro_options_register();
  declared_order_opens_connected();
  loaded_content_names_its_rooms();
  unset_option_reports_a_pad();
  host_value_still_decides();
  if(failures) return 1;
  puts("libretro option defaults: ok");
  return 0;
}
