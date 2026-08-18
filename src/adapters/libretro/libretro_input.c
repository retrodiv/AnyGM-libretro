/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <string.h>

static const struct retro_controller_description g_controller_types[]={
  {"RetroPad",RETRO_DEVICE_JOYPAD}
};

static const struct retro_controller_info g_controller_info[]={
  {g_controller_types,sizeof g_controller_types/sizeof g_controller_types[0]},
  {NULL,0}
};

static const struct retro_input_descriptor g_input_descriptors[]={
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP,"Up"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN,"Down"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT,"Left"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT,"Right"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_B,"Button 1 / Confirm"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A,"Button 2 / Back"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_Y,"Button 3"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_X,"Button 4"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L,"Left Shoulder"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R,"Right Shoulder"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_SELECT,"Select"},
  {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_START,"Start"},
  {0,RETRO_DEVICE_KEYBOARD,0,RETROK_UP,"Keyboard Up"},
  {0,RETRO_DEVICE_KEYBOARD,0,RETROK_DOWN,"Keyboard Down"},
  {0,RETRO_DEVICE_KEYBOARD,0,RETROK_LEFT,"Keyboard Left"},
  {0,RETRO_DEVICE_KEYBOARD,0,RETROK_RIGHT,"Keyboard Right"},
  {0,RETRO_DEVICE_KEYBOARD,0,RETROK_z,"Keyboard Z"},
  {0,RETRO_DEVICE_KEYBOARD,0,RETROK_x,"Keyboard X"},
  {0,RETRO_DEVICE_KEYBOARD,0,RETROK_c,"Keyboard C"},
  {0,RETRO_DEVICE_KEYBOARD,0,RETROK_SPACE,"Keyboard Space"},
  {0,RETRO_DEVICE_KEYBOARD,0,RETROK_RETURN,"Keyboard Enter"},
  {0,RETRO_DEVICE_KEYBOARD,0,RETROK_ESCAPE,"Keyboard Escape"},
  {0,RETRO_DEVICE_KEYBOARD,0,RETROK_LSHIFT,"Keyboard Shift"},
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

  input->connected_gamepads=g_libretro.config.gamepad_connected?1u:0u;
  for(unsigned button=0;button<ANYGM_MAX_GAMEPAD_BUTTONS;button++)
    input->gamepad_buttons[0][button]=
        input_state(0,RETRO_DEVICE_JOYPAD,0,button)?1u:0u;
  input->gamepad_axes[0][0]=normalized_axis(input_state(
      0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_X));
  input->gamepad_axes[0][1]=normalized_axis(input_state(
      0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_LEFT,RETRO_DEVICE_ID_ANALOG_Y));
  input->gamepad_axes[0][2]=normalized_axis(input_state(
      0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_RIGHT,RETRO_DEVICE_ID_ANALOG_X));
  input->gamepad_axes[0][3]=normalized_axis(input_state(
      0,RETRO_DEVICE_ANALOG,RETRO_DEVICE_INDEX_ANALOG_RIGHT,RETRO_DEVICE_ID_ANALOG_Y));

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
