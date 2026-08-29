/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <string.h>

static const struct retro_controller_description g_controller_types[]={
  {"RetroPad",RETRO_DEVICE_JOYPAD}
};

/* Advertise one controller description for each supported port. This table states capacity,
 * while the configured active count states use. */
static const struct retro_controller_info g_controller_info[]={
  {g_controller_types,sizeof g_controller_types/sizeof g_controller_types[0]},
  {g_controller_types,sizeof g_controller_types/sizeof g_controller_types[0]},
  {g_controller_types,sizeof g_controller_types/sizeof g_controller_types[0]},
  {g_controller_types,sizeof g_controller_types/sizeof g_controller_types[0]},
  {NULL,0}
};

/* Describe the same bindings for every supported pad port. Keyboard descriptors remain on
 * port zero because the adapter exposes one keyboard. */
static const struct retro_input_descriptor g_input_descriptors[]={
  /* port 0 */
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP,"Up"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN,"Down"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT,"Left"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT,"Right"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_B,"Button 1 / South"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A,"Button 2 / East"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_Y,"Button 3 / West"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_X,"Button 4 / North"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L,"Left Shoulder"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R,"Right Shoulder"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L2,"Left Trigger"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R2,"Right Trigger"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L3,"Left Stick"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R3,"Right Stick"},
  {0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_X,"Left Stick X"},
  {0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_Y,"Left Stick Y"},
  {0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_RIGHT,RETRO_DEVICE_ID_ANALOG_X,"Right Stick X"},
  {0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_RIGHT,RETRO_DEVICE_ID_ANALOG_Y,"Right Stick Y"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_SELECT,"Select"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_START,"Start"},
  /* port 1 */
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP,"Up"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN,"Down"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT,"Left"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT,"Right"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_B,"Button 1 / South"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A,"Button 2 / East"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_Y,"Button 3 / West"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_X,"Button 4 / North"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L,"Left Shoulder"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R,"Right Shoulder"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L2,"Left Trigger"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R2,"Right Trigger"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L3,"Left Stick"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R3,"Right Stick"},
  {1,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_X,"Left Stick X"},
  {1,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_Y,"Left Stick Y"},
  {1,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_RIGHT,RETRO_DEVICE_ID_ANALOG_X,"Right Stick X"},
  {1,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_RIGHT,RETRO_DEVICE_ID_ANALOG_Y,"Right Stick Y"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_SELECT,"Select"},
  {1,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_START,"Start"},
  /* port 2 */
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP,"Up"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN,"Down"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT,"Left"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT,"Right"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_B,"Button 1 / South"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A,"Button 2 / East"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_Y,"Button 3 / West"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_X,"Button 4 / North"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L,"Left Shoulder"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R,"Right Shoulder"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L2,"Left Trigger"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R2,"Right Trigger"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L3,"Left Stick"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R3,"Right Stick"},
  {2,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_X,"Left Stick X"},
  {2,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_Y,"Left Stick Y"},
  {2,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_RIGHT,RETRO_DEVICE_ID_ANALOG_X,"Right Stick X"},
  {2,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_RIGHT,RETRO_DEVICE_ID_ANALOG_Y,"Right Stick Y"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_SELECT,"Select"},
  {2,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_START,"Start"},
  /* port 3 */
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP,"Up"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN,"Down"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT,"Left"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT,"Right"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_B,"Button 1 / South"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A,"Button 2 / East"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_Y,"Button 3 / West"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_X,"Button 4 / North"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L,"Left Shoulder"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R,"Right Shoulder"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L2,"Left Trigger"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R2,"Right Trigger"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L3,"Left Stick"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R3,"Right Stick"},
  {3,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_X,"Left Stick X"},
  {3,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_Y,"Left Stick Y"},
  {3,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_RIGHT,RETRO_DEVICE_ID_ANALOG_X,"Right Stick X"},
  {3,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_RIGHT,RETRO_DEVICE_ID_ANALOG_Y,"Right Stick Y"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_SELECT,"Select"},
  {3,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_START,"Start"},
  /* No keyboard entries. SET_INPUT_DESCRIPTORS describes what a player may remap, and frontends
   * drop RETRO_DEVICE_KEYBOARD rows from that list without saying so, so listing them described
   * nothing while suggesting the keys were configurable. Content still receives every key through
   * the keyboard callback below, which is not a remapping surface and does not need one. */
  {0}
};

static void RETRO_CALLCONV keyboard_event(bool down,unsigned keycode,uint32_t character,
                                          uint16_t modifiers){
  (void)character;
  (void)modifiers;
  if(keycode<ANYGM_MAX_KEYS) g_libretro.keyboard_events[keycode]=down?1u:0u;
}

void libretro_input_register(void){
  if(!g_libretro.environment) return;
  g_libretro.environment(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO,(void *)g_controller_info);
  g_libretro.environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,(void *)g_input_descriptors);
  /* A frontend that accepts this callback becomes the authority on which keys belong to the
   * content: it withholds whatever it has bound to a command of its own, whatever key that is, and
   * hands the whole keyboard over once the player gives the content focus. Remember the answer,
   * because it decides where the keyboard is read from below. */
  struct retro_keyboard_callback callback={keyboard_event};
  g_libretro.keyboard_events_accepted=
      g_libretro.environment(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK,&callback);
}

static int16_t input_state(unsigned port,unsigned device,unsigned index,unsigned id){
  return g_libretro.input_state?g_libretro.input_state(port,device,index,id):0;
}

static float normalized_axis(int16_t value){
  return value>=0?(float)value/32767.0f:(float)value/32768.0f;
}

static void poll_key(AnygmInputFrame *input,unsigned key){
  if(key<ANYGM_MAX_KEYS && input_state(0,RETRO_DEVICE_KEYBOARD,0,key)) input->keys[key]=1;
}

void libretro_input_snapshot(AnygmInputFrame *input,uint32_t width,uint32_t height){
  memset(input,0,sizeof *input);
  input->struct_size=sizeof *input;
  input->pointer_x=-1;
  input->pointer_y=-1;
  if(g_libretro.input_poll) g_libretro.input_poll();

  /* Snapshot every supported port. Capacity and controller declarations do not establish
   * occupancy, and idle and absent ports both report zero input. Ownership still controls whether
   * pad rows are exposed; keyboard emulation reports none. */
  unsigned ports=ANYGM_MAX_GAMEPADS;
  input->connected_gamepads=g_libretro.config.gamepad_connected?ports:0u;
  for(unsigned port=0;port<ports;port++){
    /* One call per port where the frontend offers it, sixteen where it does not. The values are
     * the same either way: the mask is the frontend's own answer for each of those buttons. */
    if(g_libretro.input_bitmasks){
      int16_t mask=input_state(port,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_MASK);
      for(unsigned button=0;button<ANYGM_MAX_GAMEPAD_BUTTONS;button++)
        input->gamepad_buttons[port][button]=(mask&(1<<button))?1u:0u;
    }
    else for(unsigned button=0;button<ANYGM_MAX_GAMEPAD_BUTTONS;button++)
      input->gamepad_buttons[port][button]=
          input_state(port,RETRO_DEVICE_JOYPAD,0,button)?1u:0u;
    input->gamepad_axes[port][0]=normalized_axis(input_state(
        port,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_X));
    input->gamepad_axes[port][1]=normalized_axis(input_state(
        port,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_Y));
    input->gamepad_axes[port][2]=normalized_axis(input_state(
        port,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_RIGHT,RETRO_DEVICE_ID_ANALOG_X));
    input->gamepad_axes[port][3]=normalized_axis(input_state(
        port,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_RIGHT,RETRO_DEVICE_ID_ANALOG_Y));
  }

  /* Reading the keyboard device directly returns every key the hardware reports, including the one
   * the frontend just used to open its menu, rewind, or write a state. Content that binds the same
   * key to a soft reset or a display toggle then answers a command that was never meant for it, and
   * a soft reset armed that way is carried into every state written afterwards.
   *
   * So the device is read only when the frontend refused the callback and nothing else can supply a
   * keyboard. Naming the reserved keys here instead would be guesswork: they are the frontend's to
   * choose, and it already answers the question by what it delivers. */
  memcpy(input->keys,g_libretro.keyboard_events,sizeof input->keys);
  if(g_libretro.keyboard_events_accepted) goto pointer_state;
  static const unsigned special_keys[]={
    RETROK_BACKSPACE,RETROK_TAB,RETROK_RETURN,RETROK_PAUSE,RETROK_ESCAPE,RETROK_SPACE,
    RETROK_DELETE,RETROK_UP,RETROK_DOWN,RETROK_RIGHT,RETROK_LEFT,RETROK_INSERT,
    RETROK_HOME,RETROK_END,RETROK_PAGEUP,RETROK_PAGEDOWN,RETROK_NUMLOCK,
    RETROK_CAPSLOCK,RETROK_SCROLLOCK,RETROK_RSHIFT,RETROK_LSHIFT,RETROK_RCTRL,
    RETROK_LCTRL,RETROK_RALT,RETROK_LALT
  };
  for(unsigned i=0;i<sizeof special_keys/sizeof special_keys[0];i++) poll_key(input,special_keys[i]);
  for(unsigned key=RETROK_0;key<=RETROK_9;key++) poll_key(input,key);
  for(unsigned key=RETROK_a;key<=RETROK_z;key++) poll_key(input,key);
  for(unsigned key=RETROK_F1;key<=RETROK_F12;key++) poll_key(input,key);
  for(unsigned key=RETROK_KP0;key<=RETROK_KP_EQUALS;key++) poll_key(input,key);

pointer_state:
  ;
  int pointer_x=input_state(0,RETRO_DEVICE_POINTER,0,RETRO_DEVICE_ID_POINTER_X);
  int pointer_y=input_state(0,RETRO_DEVICE_POINTER,0,RETRO_DEVICE_ID_POINTER_Y);
  int pointer_pressed=input_state(0,RETRO_DEVICE_POINTER,0,RETRO_DEVICE_ID_POINTER_PRESSED);
  if(pointer_x || pointer_y || pointer_pressed) g_libretro.pointer_seen=1;
  if(g_libretro.pointer_seen){
    input->pointer_x=(int32_t)((pointer_x+32767)*(int64_t)(width?width:1)/65534);
    input->pointer_y=(int32_t)((pointer_y+32767)*(int64_t)(height?height:1)/65534);
    input->pointer_pressed=pointer_pressed?1u:0u;
  }
  input->mouse_delta_x=input_state(0,RETRO_DEVICE_MOUSE,0,RETRO_DEVICE_ID_MOUSE_X);
  input->mouse_delta_y=input_state(0,RETRO_DEVICE_MOUSE,0,RETRO_DEVICE_ID_MOUSE_Y);
  input->mouse_buttons[0]=input_state(0,RETRO_DEVICE_MOUSE,0,RETRO_DEVICE_ID_MOUSE_LEFT)?1u:0u;
  input->mouse_buttons[1]=input_state(0,RETRO_DEVICE_MOUSE,0,RETRO_DEVICE_ID_MOUSE_RIGHT)?1u:0u;
  input->mouse_buttons[2]=input_state(0,RETRO_DEVICE_MOUSE,0,RETRO_DEVICE_ID_MOUSE_MIDDLE)?1u:0u;
  if(input_state(0,RETRO_DEVICE_MOUSE,0,RETRO_DEVICE_ID_MOUSE_WHEELUP)) input->wheel_delta++;
  if(input_state(0,RETRO_DEVICE_MOUSE,0,RETRO_DEVICE_ID_MOUSE_WHEELDOWN)) input->wheel_delta--;
}
