/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Every supported port is snapshotted independently. Synthetic per-port directions expose row
 * aliasing, while the ownership-disabled case verifies that no pad rows are reported. */
#include "libretro_internal.h"

#include <stdio.h>
#include <string.h>

/* Each port holds a different button, so an answer taken from the wrong one is visible rather than
 * merely unproven. */
static int16_t input_state_callback(unsigned port,unsigned device,unsigned index,unsigned id){
  (void)index;
  if(device!=RETRO_DEVICE_JOYPAD) return 0;
  switch(port){
    case 0: return id==RETRO_DEVICE_ID_JOYPAD_RIGHT;
    case 1: return id==RETRO_DEVICE_ID_JOYPAD_LEFT;
    case 2: return id==RETRO_DEVICE_ID_JOYPAD_UP;
    case 3: return id==RETRO_DEVICE_ID_JOYPAD_DOWN;
    default: return 0;
  }
}

static bool environment_callback(unsigned command,void *data){
  (void)command; (void)data;
  return true;
}

LibretroAdapter g_libretro;

static int failures;

static void snapshot(AnygmInputFrame *input,unsigned ownership){
  memset(&g_libretro,0,sizeof g_libretro);
  g_libretro.environment=environment_callback;
  g_libretro.input_state=input_state_callback;
  g_libretro.config.gamepad_connected=ownership;
  libretro_input_register();
  memset(input,0,sizeof *input);
  libretro_input_snapshot(input,320,240);
}

static void expect(int condition,const char *message){
  if(condition) return;
  fprintf(stderr,"libretro gamepad ports: %s\n",message);
  failures++;
}

static void every_port_carries_its_own_device(void){
  AnygmInputFrame input;
  snapshot(&input,ANYGM_GAMEPAD_AUTO);
  expect(input.connected_gamepads==ANYGM_MAX_GAMEPADS,
         "the adapter did not present every supported port");
  expect(input.gamepad_buttons[0][ANYGM_PAD_RIGHT]==1u,"player one lost its direction");
  expect(input.gamepad_buttons[1][ANYGM_PAD_LEFT]==1u,"player two did not arrive");
  expect(input.gamepad_buttons[2][ANYGM_PAD_UP]==1u,"player three did not arrive");
  expect(input.gamepad_buttons[3][ANYGM_PAD_DOWN]==1u,"player four did not arrive");
  /* The whole point: the rows disagree, so none of them is a copy of the first. */
  expect(input.gamepad_buttons[1][ANYGM_PAD_RIGHT]==0u,
         "player two answered with player one's direction");
  expect(input.gamepad_buttons[0][ANYGM_PAD_LEFT]==0u,
         "player one answered with player two's direction");
}

/* Ownership decides whether pad rows are exposed. Keyboard emulation reports none. */
static void keyboard_emulation_reports_no_pads(void){
  AnygmInputFrame input;
  snapshot(&input,0u);
  expect(input.connected_gamepads==0u,"keyboard emulation still reported pads");
}

int main(void){
  every_port_carries_its_own_device();
  keyboard_emulation_reports_no_pads();
  if(failures) return 1;
  puts("libretro gamepad ports: ok");
  return 0;
}
