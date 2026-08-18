/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Keyboard, gamepad, pointer, joystick, and display-coordinate adaptation. */
#include "gml_builtin_internal.h"
#include "gml_render.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

enum {
  GML_EXTERNAL_INPUT_HANDLE_BASE=0x4A200000,
  GML_EXTERNAL_INPUT_RUMBLE=1,
  GML_EXTERNAL_INPUT_LEFT_TRIGGER,
  GML_EXTERNAL_INPUT_RIGHT_TRIGGER,
  GML_EXTERNAL_INPUT_LEFT_X,
  GML_EXTERNAL_INPUT_LEFT_Y,
  GML_EXTERNAL_INPUT_RIGHT_X,
  GML_EXTERNAL_INPUT_RIGHT_Y,
  GML_EXTERNAL_INPUT_BUTTON_STATE,
  GML_EXTERNAL_INPUT_CHECK_BUTTON,
  GML_EXTERNAL_INPUT_CONTROLLER_STATE,
  GML_EXTERNAL_INPUT_OPERATION_LIMIT
};

typedef struct { const char *name; int operation; } GmlExternalInputSymbol;
static const GmlExternalInputSymbol external_input_symbols[]={
  {"setRumble",GML_EXTERNAL_INPUT_RUMBLE},
  {"leftTrigger",GML_EXTERNAL_INPUT_LEFT_TRIGGER},
  {"rightTrigger",GML_EXTERNAL_INPUT_RIGHT_TRIGGER},
  {"leftThumbX",GML_EXTERNAL_INPUT_LEFT_X},
  {"leftThumbY",GML_EXTERNAL_INPUT_LEFT_Y},
  {"rightThumbX",GML_EXTERNAL_INPUT_RIGHT_X},
  {"rightThumbY",GML_EXTERNAL_INPUT_RIGHT_Y},
  {"getButtonState",GML_EXTERNAL_INPUT_BUTTON_STATE},
  {"checkButton",GML_EXTERNAL_INPUT_CHECK_BUTTON},
  {"getCtrlState",GML_EXTERNAL_INPUT_CONTROLLER_STATE},
};

static int external_input_ascii_equal(const char *left,const char *right){
  if(!left || !right) return 0;
  while(*left && *right){
    if(tolower((unsigned char)*left)!=tolower((unsigned char)*right)) return 0;
    left++; right++;
  }
  return *left==0 && *right==0;
}

int builtin_external_input_define(const char *library,const char *symbol){
  if(!library || !symbol) return 0;
  const char *base=library;
  for(const char *cursor=library;*cursor;cursor++)
    if(*cursor=='/' || *cursor=='\\') base=cursor+1;
  if(!external_input_ascii_equal(base,"GMXInput.dll")) return 0;
  for(size_t i=0;i<sizeof(external_input_symbols)/sizeof(external_input_symbols[0]);i++)
    if(!strcmp(symbol,external_input_symbols[i].name))
      return GML_EXTERNAL_INPUT_HANDLE_BASE+external_input_symbols[i].operation;
  return 0;
}

static int external_input_button_mask(GmlVM *vm){
  int mask=0;
  if(gml_input_gamepad(vm,32781,0)) mask|=0x0001;
  if(gml_input_gamepad(vm,32782,0)) mask|=0x0002;
  if(gml_input_gamepad(vm,32783,0)) mask|=0x0004;
  if(gml_input_gamepad(vm,32784,0)) mask|=0x0008;
  if(gml_input_gamepad(vm,32778,0)) mask|=0x0010;
  if(gml_input_gamepad(vm,32777,0)) mask|=0x0020;
  if(gml_input_gamepad(vm,32779,0)) mask|=0x0040;
  if(gml_input_gamepad(vm,32780,0)) mask|=0x0080;
  if(gml_input_gamepad(vm,32773,0)) mask|=0x0100;
  if(gml_input_gamepad(vm,32774,0)) mask|=0x0200;
  if(gml_input_gamepad(vm,32769,0)) mask|=0x1000;
  if(gml_input_gamepad(vm,32770,0)) mask|=0x2000;
  if(gml_input_gamepad(vm,32771,0)) mask|=0x4000;
  if(gml_input_gamepad(vm,32772,0)) mask|=0x8000;
  return mask;
}

static int joy_device(double value){
  return (int)value;
}

static int joy_button_code(int button){
  return button>=0 && button<16 ? 32769+button : 0;
}

static double joy_axis_value(GmlVM *vm,int device,int axis){
  int code=axis==0?32785:axis==1?32786:axis==2?32787:axis==3?32788:0;
  if(!code) return 0.0;
  double value=gml_input_gamepad_axis(vm,device,code);
  /* A neutral axis can coexist with digital pad directions. Reuse the
   * gamepad-axis digital fallback for this extension; the right stick has
   * no digital counterpart. Do not apply the gamepad deadzone here: this
   * adapter reports the normalized transport value without that filter. */
  if(value==0.0) value=gp_axis_digital_fallback(vm,code);
  if(!isfinite(value)) return 0.0;
  if(value<-1.0) value=-1.0; else if(value>1.0) value=1.0;
  return value;
}

GmlVal builtin_external_input_call(GmlVM *vm,int handle,
                                   GmlVal *args,int count,int *handled){
  if(handled) *handled=0;
  int operation=handle-GML_EXTERNAL_INPUT_HANDLE_BASE;
  if(operation<=0 || operation>=GML_EXTERNAL_INPUT_OPERATION_LIMIT) return vreal(0);
  if(handled) *handled=1;
  int device=(int)N(args,count,0);
  if(operation==GML_EXTERNAL_INPUT_RUMBLE){
    double low=N(args,count,1)/65535.0,high=N(args,count,2)/65535.0;
    gml_input_gamepad_set_vibration(vm,device,low,high);
    return vreal(1);
  }
  if(operation==GML_EXTERNAL_INPUT_CONTROLLER_STATE)
    return vreal(gml_input_gamepad_connected(vm,device));
  if(operation==GML_EXTERNAL_INPUT_BUTTON_STATE)
    return vreal(gml_input_gamepad_connected(vm,device)?external_input_button_mask(vm):0);
  if(operation==GML_EXTERNAL_INPUT_CHECK_BUTTON){
    int requested=(int)N(args,count,1);
    return vreal(gml_input_gamepad_connected(vm,device) &&
                 (external_input_button_mask(vm)&requested)!=0);
  }
  if(operation==GML_EXTERNAL_INPUT_LEFT_TRIGGER ||
     operation==GML_EXTERNAL_INPUT_RIGHT_TRIGGER){
    int button=operation==GML_EXTERNAL_INPUT_LEFT_TRIGGER?32775:32776;
    return vreal(gml_input_gamepad(vm,button,0)?255:0);
  }
  int axis=operation==GML_EXTERNAL_INPUT_LEFT_X?32785:
    operation==GML_EXTERNAL_INPUT_LEFT_Y?32786:
    operation==GML_EXTERNAL_INPUT_RIGHT_X?32787:32788;
  double value=gml_input_gamepad_axis(vm,device,axis);
  if(!isfinite(value)) value=0.0;
  if(value<-1.0) value=-1.0; else if(value>1.0) value=1.0;
  if(operation==GML_EXTERNAL_INPUT_LEFT_Y || operation==GML_EXTERNAL_INPUT_RIGHT_Y)
    value=-value;
  return vreal(value<0.0?value*32768.0:value*32767.0);
}

GmlVal gml_builtin_try_input(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- input (wired later; held/pressed/released) ---- */
  if(!strcmp(nm,"keyboard_set_map")){
    gml_keyboard_set_map(vm,(int)N(a,n,0),(int)N(a,n,1));
    return vreal(0);
  }
  if(!strcmp(nm,"keyboard_get_map"))
    return vreal(gml_keyboard_get_map(vm,(int)N(a,n,0)));
  if(!strcmp(nm,"keyboard_unset_map")){
    gml_keyboard_unset_map(vm);
    return vreal(0);
  }
  { GmlVal v; if(builtin_input_kbgp(vm,nm,a,n,&v)) return v; }
  /* ---- mouse (the host pointer/mouse device via gml_input_mouse) ---- */
  if(!strcmp(nm,"mouse_check_button"))          return vreal(mouse_btn_check(vm,(int)N(a,n,0),0));
  if(!strcmp(nm,"mouse_check_button_pressed"))  return vreal(mouse_btn_check(vm,(int)N(a,n,0),1));
  if(!strcmp(nm,"mouse_check_button_released")) return vreal(mouse_btn_check(vm,(int)N(a,n,0),2));
  if(!strcmp(nm,"device_mouse_check_button"))          return vreal(mouse_btn_check(vm,(int)N(a,n,1),0));
  if(!strcmp(nm,"device_mouse_check_button_pressed"))  return vreal(mouse_btn_check(vm,(int)N(a,n,1),1));
  if(!strcmp(nm,"device_mouse_check_button_released")) return vreal(mouse_btn_check(vm,(int)N(a,n,1),2));
  if(!strcmp(nm,"device_mouse_x")){ double v; gml_input_mouse(vm,&v,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
  if(!strcmp(nm,"device_mouse_y")){ double v; gml_input_mouse(vm,NULL,&v,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
  if(!strcmp(nm,"device_mouse_x_to_gui")){ double v; gml_input_mouse(vm,NULL,NULL,&v,NULL,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
  if(!strcmp(nm,"device_mouse_y_to_gui")){ double v; gml_input_mouse(vm,NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
  if(!strcmp(nm,"device_mouse_raw_x")||!strcmp(nm,"window_mouse_get_x")){ double v; gml_input_mouse(vm,NULL,NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
  if(!strcmp(nm,"device_mouse_raw_y")||!strcmp(nm,"window_mouse_get_y")){ double v; gml_input_mouse(vm,NULL,NULL,NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL); return vreal(v); }
  if(!strcmp(nm,"display_mouse_get_x")){ double v; gml_input_mouse(vm,NULL,NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL,NULL);
    return vreal(classic_display_mouse_coord(vm,(GmlRender*)vm->render,v,0,0)); }
  if(!strcmp(nm,"display_mouse_get_y")){ double v; gml_input_mouse(vm,NULL,NULL,NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL);
    return vreal(classic_display_mouse_coord(vm,(GmlRender*)vm->render,v,1,0)); }
  if(!strcmp(nm,"display_mouse_set")){
    GmlRender *render=(GmlRender*)vm->render;
    gml_input_mouse_set(vm,classic_display_mouse_coord(vm,render,N(a,n,0),0,1),
                        classic_display_mouse_coord(vm,render,N(a,n,1),1,1)); return vreal(0); }
  /* The legacy Studio desktop API exposes display_mouse_lock(x,y,w,h) and
   * display_mouse_unlock() to confine/release the host cursor.  A host core does not own the
   * operating-system cursor: the host already clips its absolute pointer to the presented
   * game rectangle and supplies only that normalized position.  Consequently confinement is
   * already true at this boundary, while unlock must not make the core capture a desktop cursor.
   * Keep both calls as explicit platform operations instead of letting them fall through the
   * unknown-builtin path every frame. */
  if(!strcmp(nm,"display_mouse_lock")||!strcmp(nm,"display_mouse_unlock")) return vreal(0);
  if(!strcmp(nm,"window_mouse_set")) return vreal(0);
  /* The common SDL joydll extension uses zero-based devices, axes and buttons. Translate its
   * polling surface to the same normalized gamepad snapshot as GameMaker's joystick builtins.
   * Trackballs are not represented by the host input contract and hats use the first d-pad. */
  if(!strcmp(nm,"joy_init")||!strcmp(nm,"joy_update")||!strcmp(nm,"joy_close"))
    return vreal(1);
  if(!strcmp(nm,"joy_count")){
    int count=0;
    for(int device=0;device<4;device++) if(gml_input_gamepad_connected(vm,device)) count++;
    return vreal(count);
  }
  if(!strcmp(nm,"joy_find")){
    for(int device=0;device<4;device++) if(gml_input_gamepad_connected(vm,device)) return vreal(device);
    return vreal(-1);
  }
  if(!strcmp(nm,"joy_name"))
    return vstr(gml_input_gamepad_connected(vm,joy_device(N(a,n,0)))?"AnyGM Gamepad":"");
  if(!strcmp(nm,"joy_axes")) return vreal(4);
  if(!strcmp(nm,"joy_axis"))
    return vreal(joy_axis_value(vm,joy_device(N(a,n,0)),(int)N(a,n,1)));
  if(!strcmp(nm,"joy_buttons")) return vreal(16);
  if(!strcmp(nm,"joy_button")){
    int device=joy_device(N(a,n,0));
    int code=joy_button_code((int)N(a,n,1));
    return vreal(device==0&&code?gml_input_gamepad(vm,code,0):0);
  }
  if(!strcmp(nm,"joy_hats")) return vreal(1);
  if(!strcmp(nm,"joy_hat")){
    int device=joy_device(N(a,n,0));
    int up=device==0&&gml_input_gamepad(vm,32781,0);
    int down=device==0&&gml_input_gamepad(vm,32782,0);
    int left=device==0&&gml_input_gamepad(vm,32783,0);
    int right=device==0&&gml_input_gamepad(vm,32784,0);
    if(up&&right) return vreal(1);
    if(right&&down) return vreal(3);
    if(down&&left) return vreal(5);
    if(left&&up) return vreal(7);
    if(up) return vreal(0);
    if(right) return vreal(2);
    if(down) return vreal(4);
    if(left) return vreal(6);
    return vreal(-1);
  }
  if(!strcmp(nm,"joy_balls")) return vreal(0);
  if(!strcmp(nm,"joy_ball_x")||!strcmp(nm,"joy_ball_y")) return vreal(0);
  if(!strcmp(nm,"joystick_exists")){
    int joy=(int)N(a,n,0), dev=joy>0?joy-1:joy;
    return vreal(gml_input_gamepad_connected(vm,dev)); }
  if(!strcmp(nm,"joystick_name")){
    int joy=(int)N(a,n,0), dev=joy>0?joy-1:joy;
    return vstr(gml_input_gamepad_connected(vm,dev) ? "AnyGM Gamepad" : ""); }
  if(!strcmp(nm,"joystick_buttons")) return vreal(16);
  if(!strcmp(nm,"joystick_axes")) return vreal(2);
  if(!strcmp(nm,"joystick_check_button")){ int b=(int)N(a,n,1);
    return vreal((b>=1 && b<=16) ? gml_input_gamepad(vm,32768+b,0) : 0); }
  if(!strcmp(nm,"joystick_xpos")) return vreal(gml_input_gamepad(vm,32784,0) - gml_input_gamepad(vm,32783,0));
  if(!strcmp(nm,"joystick_ypos")) return vreal(gml_input_gamepad(vm,32782,0) - gml_input_gamepad(vm,32781,0));
  if(!strcmp(nm,"joystick_direction")){
    int x=gml_input_gamepad(vm,32784,0) - gml_input_gamepad(vm,32783,0);
    int y=gml_input_gamepad(vm,32782,0) - gml_input_gamepad(vm,32781,0);
    if(x<0 && y<0) return vreal(103);
    if(x>0 && y<0) return vreal(105);
    if(x<0 && y>0) return vreal(97);
    if(x>0 && y>0) return vreal(99);
    if(x<0) return vreal(100);
    if(x>0) return vreal(102);
    if(y<0) return vreal(104);
    if(y>0) return vreal(98);
    return vreal(101);
  }
  if(!strcmp(nm,"joystick_zpos")||!strcmp(nm,"joystick_rpos")||!strcmp(nm,"joystick_upos")||!strcmp(nm,"joystick_vpos")) return vreal(0);
  if(!strcmp(nm,"joystick_has_pov")) return vreal(0);
  if(!strcmp(nm,"joystick_pov")) return vreal(-1);

  return gml_builtin_try_io(vm,nm,a,n);
}
