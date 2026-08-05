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

/* The adapter's global context and the engine entry point it drives both live outside this
 * translation unit's dependency set; the option parsing under test needs only their presence. */
LibretroAdapter g_libretro;
AnygmResult anygm_set_config(AnygmEngine *engine,const AnygmConfigDelta *delta){
  (void)engine;
  (void)delta;
  return ANYGM_OK;
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

int main(void){
  g_libretro.environment=environment_callback;
  g_libretro.engine=engine_placeholder;
  libretro_options_register();
  declared_order_opens_connected();
  unset_option_reports_a_pad();
  host_value_still_decides();
  if(failures) return 1;
  puts("libretro option defaults: ok");
  return 0;
}
