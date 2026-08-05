/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* A frontend binds keys to its own commands: a menu, a rewind, a state slot. Which keys those are
 * is the frontend's choice, and it answers the question by what it delivers: a key it reserved
 * never arrives as a key event until the player hands the keyboard to the content, while reading
 * the keyboard device returns it regardless.
 *
 * Content that binds the same key to a soft reset then acts on a command aimed at the frontend, and
 * a reset armed that way is written into every state that follows. The adapter therefore takes the
 * events as the authority whenever the frontend accepts them, and reads the device only for a
 * frontend that offers nothing else. Naming the reserved keys here instead would be guesswork.
 */
#include "libretro_internal.h"

#include <stdio.h>
#include <string.h>

static int keyboard_callback_accepted=1;
static unsigned polled_key;
/* The frontend is handed a stack-local descriptor, so keep the function it names, not the
 * address of the descriptor itself. */
static retro_keyboard_event_t registered_callback;

static bool environment_callback(unsigned command,void *data){
  if(command==RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK){
    if(!keyboard_callback_accepted) return false;
    registered_callback=data?((const struct retro_keyboard_callback*)data)->callback:NULL;
    return true;
  }
  return true;
}

static int16_t input_state_callback(unsigned port,unsigned device,unsigned index,unsigned id){
  (void)port;
  (void)index;
  if(device==RETRO_DEVICE_KEYBOARD && id==polled_key) return 1;
  return 0;
}

/* The adapter's context is shared; nothing else in this translation unit's dependency set is
 * needed to exercise the input snapshot. */
LibretroAdapter g_libretro;

static int failures;

static void snapshot(AnygmInputFrame *input){
  memset(input,0,sizeof *input);
  libretro_input_snapshot(input,320,240);
}

static void reserved_key_stays_with_the_frontend(void){
  memset(&g_libretro,0,sizeof g_libretro);
  g_libretro.environment=environment_callback;
  g_libretro.input_state=input_state_callback;
  keyboard_callback_accepted=1;
  registered_callback=NULL;
  libretro_input_register();
  /* The frontend consumed this key as one of its own commands, so it reports it on the device but
   * never delivers it as an event. */
  polled_key=RETROK_F1;
  AnygmInputFrame input;
  snapshot(&input);
  if(input.keys[RETROK_F1]){
    fprintf(stderr,"libretro keyboard source: a key the frontend withheld reached the content\n");
    failures++;
  }
}

static void delivered_key_reaches_the_content(void){
  memset(&g_libretro,0,sizeof g_libretro);
  g_libretro.environment=environment_callback;
  g_libretro.input_state=input_state_callback;
  keyboard_callback_accepted=1;
  registered_callback=NULL;
  libretro_input_register();
  if(!registered_callback){
    fprintf(stderr,"libretro keyboard source: the keyboard callback was not registered\n");
    failures++;
    return;
  }
  /* The player gave the content the keyboard, so the same key now arrives as an event. */
  polled_key=0;
  registered_callback(true,RETROK_F1,0,0);
  AnygmInputFrame input;
  snapshot(&input);
  if(!input.keys[RETROK_F1]){
    fprintf(stderr,"libretro keyboard source: a delivered key did not reach the content\n");
    failures++;
  }
  registered_callback(false,RETROK_F1,0,0);
  snapshot(&input);
  if(input.keys[RETROK_F1]){
    fprintf(stderr,"libretro keyboard source: a released key stayed down\n");
    failures++;
  }
}

static void device_still_serves_a_frontend_without_events(void){
  memset(&g_libretro,0,sizeof g_libretro);
  g_libretro.environment=environment_callback;
  g_libretro.input_state=input_state_callback;
  keyboard_callback_accepted=0;
  registered_callback=NULL;
  libretro_input_register();
  polled_key=RETROK_a;
  AnygmInputFrame input;
  snapshot(&input);
  if(!input.keys[RETROK_a]){
    fprintf(stderr,"libretro keyboard source: a frontend without events lost its keyboard\n");
    failures++;
  }
}

int main(void){
  reserved_key_stays_with_the_frontend();
  delivered_key_reaches_the_content();
  device_still_serves_a_frontend_without_events();
  if(failures) return 1;
  puts("libretro keyboard source: ok");
  return 0;
}
