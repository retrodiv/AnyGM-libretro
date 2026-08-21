/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* The configured count exposes each lower port as an independent row and leaves every higher
 * row at rest. Occupancy is not inferred because an idle device and an absent one both report zero. */
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

static void snapshot_with_ports(AnygmInputFrame *input,uint32_t ports){
  memset(&g_libretro,0,sizeof g_libretro);
  g_libretro.environment=environment_callback;
  g_libretro.input_state=input_state_callback;
  g_libretro.config.gamepad_connected=ANYGM_GAMEPAD_AUTO;
  g_libretro.gamepad_ports=ports;
  libretro_input_register();
  memset(input,0,sizeof *input);
  libretro_input_snapshot(input,320,240);
}

static void expect(int condition,const char *message){
  if(condition) return;
  fprintf(stderr,"libretro gamepad ports: %s\n",message);
  failures++;
}

static void one_port_is_the_shape_every_game_already_had(void){
  AnygmInputFrame input;
  snapshot_with_ports(&input,1);
  expect(input.connected_gamepads==1u,"a single port did not report one pad");
  expect(input.gamepad_buttons[0][ANYGM_PAD_RIGHT]==1u,
         "the first pad's own direction did not arrive");
  /* The ports above the count answer nothing, rather than repeating the pad below them. */
  expect(input.gamepad_buttons[1][ANYGM_PAD_LEFT]==0u,
         "a port above the count carried input the game was not told about");
}

static void every_stated_port_carries_its_own_device(void){
  AnygmInputFrame input;
  snapshot_with_ports(&input,4);
  expect(input.connected_gamepads==4u,"four ports did not report four pads");
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

static void two_ports_leave_the_rest_at_rest(void){
  AnygmInputFrame input;
  snapshot_with_ports(&input,2);
  expect(input.connected_gamepads==2u,"two ports did not report two pads");
  expect(input.gamepad_buttons[1][ANYGM_PAD_LEFT]==1u,"the second stated port was empty");
  expect(input.gamepad_buttons[2][ANYGM_PAD_UP]==0u,
         "a third port the player never declared was delivered anyway");
}

/* Ownership still decides whether content sees a pad at all. A player who put the RetroPad on the
 * keyboard gets no pads however many ports are stated, because the pad is the keyboard then. */
static void keyboard_emulation_reports_no_pads_at_any_count(void){
  AnygmInputFrame input;
  memset(&g_libretro,0,sizeof g_libretro);
  g_libretro.environment=environment_callback;
  g_libretro.input_state=input_state_callback;
  g_libretro.config.gamepad_connected=0u;
  g_libretro.gamepad_ports=4u;
  libretro_input_register();
  memset(&input,0,sizeof input);
  libretro_input_snapshot(&input,320,240);
  expect(input.connected_gamepads==0u,"keyboard emulation still reported pads");
}

/* An unset count is the shape the adapter had before ports existed, not zero ports. */
static void an_unset_count_still_serves_the_first_pad(void){
  AnygmInputFrame input;
  snapshot_with_ports(&input,0);
  expect(input.connected_gamepads==1u,"an unset count did not fall back to one pad");
  expect(input.gamepad_buttons[0][ANYGM_PAD_RIGHT]==1u,
         "an unset count left the first pad empty");
}

int main(void){
  one_port_is_the_shape_every_game_already_had();
  two_ports_leave_the_rest_at_rest();
  every_stated_port_carries_its_own_device();
  keyboard_emulation_reports_no_pads_at_any_count();
  an_unset_count_still_serves_the_first_pad();
  if(failures) return 1;
  puts("libretro gamepad ports: ok");
  return 0;
}
