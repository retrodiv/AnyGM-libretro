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
  GML_EXTERNAL_INPUT_JOY_READY,
  GML_EXTERNAL_INPUT_JOY_GREETING,
  GML_EXTERNAL_INPUT_JOY_COUNT,
  GML_EXTERNAL_INPUT_JOY_FIND,
  GML_EXTERNAL_INPUT_JOY_NAME,
  GML_EXTERNAL_INPUT_JOY_AXES,
  GML_EXTERNAL_INPUT_JOY_AXIS,
  GML_EXTERNAL_INPUT_JOY_BUTTONS,
  GML_EXTERNAL_INPUT_JOY_BUTTON,
  GML_EXTERNAL_INPUT_JOY_HATS,
  GML_EXTERNAL_INPUT_JOY_HAT,
  GML_EXTERNAL_INPUT_JOY_BALLS,
  GML_EXTERNAL_INPUT_JOY_BALL,
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

/* Map externally bound joystick symbols to the same input surface as the named builtins. */
static const GmlExternalInputSymbol external_joydll_symbols[]={
  {"joy_init",GML_EXTERNAL_INPUT_JOY_READY},
  {"joy_update",GML_EXTERNAL_INPUT_JOY_READY},
  {"joy_close",GML_EXTERNAL_INPUT_JOY_READY},
  {"joy_hi",GML_EXTERNAL_INPUT_JOY_GREETING},
  {"joy_count",GML_EXTERNAL_INPUT_JOY_COUNT},
  {"joy_find",GML_EXTERNAL_INPUT_JOY_FIND},
  {"joy_name",GML_EXTERNAL_INPUT_JOY_NAME},
  {"joy_axes",GML_EXTERNAL_INPUT_JOY_AXES},
  {"joy_axis",GML_EXTERNAL_INPUT_JOY_AXIS},
  {"joy_buttons",GML_EXTERNAL_INPUT_JOY_BUTTONS},
  {"joy_button",GML_EXTERNAL_INPUT_JOY_BUTTON},
  {"joy_hats",GML_EXTERNAL_INPUT_JOY_HATS},
  {"joy_hat",GML_EXTERNAL_INPUT_JOY_HAT},
  {"joy_balls",GML_EXTERNAL_INPUT_JOY_BALLS},
  {"joy_ball_x",GML_EXTERNAL_INPUT_JOY_BALL},
  {"joy_ball_y",GML_EXTERNAL_INPUT_JOY_BALL},
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
  const GmlExternalInputSymbol *table=NULL;
  size_t count=0;
  if(external_input_ascii_equal(base,"GMXInput.dll")){
    table=external_input_symbols;
    count=sizeof(external_input_symbols)/sizeof(external_input_symbols[0]);
  } else if(external_input_ascii_equal(base,"joydll.dll")){
    table=external_joydll_symbols;
    count=sizeof(external_joydll_symbols)/sizeof(external_joydll_symbols[0]);
  } else return 0;
  for(size_t i=0;i<count;i++)
    if(!strcmp(symbol,table[i].name))
      return GML_EXTERNAL_INPUT_HANDLE_BASE+table[i].operation;
  return 0;
}

static int external_input_button_mask(GmlVM *vm,int device){
  int mask=0;
  if(gml_input_gamepad(vm,device,32781,0)) mask|=0x0001;
  if(gml_input_gamepad(vm,device,32782,0)) mask|=0x0002;
  if(gml_input_gamepad(vm,device,32783,0)) mask|=0x0004;
  if(gml_input_gamepad(vm,device,32784,0)) mask|=0x0008;
  if(gml_input_gamepad(vm,device,32778,0)) mask|=0x0010;
  if(gml_input_gamepad(vm,device,32777,0)) mask|=0x0020;
  if(gml_input_gamepad(vm,device,32779,0)) mask|=0x0040;
  if(gml_input_gamepad(vm,device,32780,0)) mask|=0x0080;
  if(gml_input_gamepad(vm,device,32773,0)) mask|=0x0100;
  if(gml_input_gamepad(vm,device,32774,0)) mask|=0x0200;
  if(gml_input_gamepad(vm,device,32769,0)) mask|=0x1000;
  if(gml_input_gamepad(vm,device,32770,0)) mask|=0x2000;
  if(gml_input_gamepad(vm,device,32771,0)) mask|=0x4000;
  if(gml_input_gamepad(vm,device,32772,0)) mask|=0x8000;
  return mask;
}

static int joy_device(double value){
  return (int)value;
}

/* Convert the joystick family's one-based device number to a zero-based pad row. */
static int joystick_device(double value){
  int joy=(int)value;
  return joy>0?joy-1:joy;
}

static int joy_button_code(int button){
  return button>=0 && button<16 ? 32769+button : 0;
}

/* Share device queries between named and externally bound joystick calls. */
static int joydll_count(GmlVM *vm){
  int count=0;
  for(int device=0;device<4;device++) if(gml_input_gamepad_connected(vm,device)) count++;
  return count;
}

static int joydll_find(GmlVM *vm){
  for(int device=0;device<4;device++) if(gml_input_gamepad_connected(vm,device)) return device;
  return -1;
}

/* Report the conventional virtual-pad layout: four face buttons, two shoulders,
 * two sticks and a d-pad. */
static const char *joydll_name(void){ return "Xbox 360 Controller"; }

/* Convert d-pad state to clockwise compass degrees, with -1 for centred. */
static double joydll_hat_degrees(GmlVM *vm,int device){
  int up=gml_input_gamepad(vm,device,32781,0);
  int down=gml_input_gamepad(vm,device,32782,0);
  int left=gml_input_gamepad(vm,device,32783,0);
  int right=gml_input_gamepad(vm,device,32784,0);
  if(up&&right) return 45.0;
  if(right&&down) return 135.0;
  if(down&&left) return 225.0;
  if(left&&up) return 315.0;
  if(up) return 0.0;
  if(right) return 90.0;
  if(down) return 180.0;
  if(left) return 270.0;
  return -1.0;
}

static double joy_axis_value(GmlVM *vm,int device,int axis){
  int code=axis==0?32785:axis==1?32786:axis==2?32787:axis==3?32788:0;
  if(!code) return 0.0;
  double value=gml_input_gamepad_axis(vm,device,code);
  /* A stick is not the only transport for a stick reading. A player on a d-pad never moves an
   * axis, and a host that maps an analog stick onto the d-pad reports the stick centred and the
   * pad pressed in its place. gamepad_axis_value already answers from the pad when the axis reads
   * neutral; this extension asks the same question under another name and answers from the same
   * place, so a d-pad reaches content that reads its stick only through here.
   * The gamepad deadzone is deliberately not applied: this surface reports the device's own
   * values and content built on it filters them itself. */
  if(value==0.0) value=gp_axis_digital_fallback(vm,device,code);
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
  /* The joydll surface takes its device, axis and button numbers zero-based and answers exactly
   * what the named builtins answer, so the two never disagree about a pad. */
  switch(operation){
    case GML_EXTERNAL_INPUT_JOY_READY:   return vreal(1);
    case GML_EXTERNAL_INPUT_JOY_GREETING:return vstr("");
    case GML_EXTERNAL_INPUT_JOY_COUNT:   return vreal(joydll_count(vm));
    case GML_EXTERNAL_INPUT_JOY_FIND:    return vreal(joydll_find(vm));
    case GML_EXTERNAL_INPUT_JOY_NAME:
      return vstr(gml_input_gamepad_connected(vm,device)?joydll_name():"");
    case GML_EXTERNAL_INPUT_JOY_AXES:    return vreal(4);
    case GML_EXTERNAL_INPUT_JOY_AXIS:
      return vreal(joy_axis_value(vm,device,(int)N(args,count,1)));
    case GML_EXTERNAL_INPUT_JOY_BUTTONS: return vreal(16);
    case GML_EXTERNAL_INPUT_JOY_BUTTON: {
      int code=joy_button_code((int)N(args,count,1));
      return vreal(code?gml_input_gamepad(vm,device,code,0):0);
    }
    case GML_EXTERNAL_INPUT_JOY_HATS:    return vreal(1);
    case GML_EXTERNAL_INPUT_JOY_HAT:     return vreal(joydll_hat_degrees(vm,device));
    /* No trackball is reachable through the host input contract, so the count is zero and the
     * deltas stay at rest rather than inventing motion content would integrate. */
    case GML_EXTERNAL_INPUT_JOY_BALLS:
    case GML_EXTERNAL_INPUT_JOY_BALL:    return vreal(0);
    default: break;
  }
  if(operation==GML_EXTERNAL_INPUT_RUMBLE){
    double low=N(args,count,1)/65535.0,high=N(args,count,2)/65535.0;
    gml_input_gamepad_set_vibration(vm,device,low,high);
    return vreal(1);
  }
  if(operation==GML_EXTERNAL_INPUT_CONTROLLER_STATE)
    return vreal(gml_input_gamepad_connected(vm,device));
  if(operation==GML_EXTERNAL_INPUT_BUTTON_STATE)
    return vreal(gml_input_gamepad_connected(vm,device)?external_input_button_mask(vm,device):0);
  if(operation==GML_EXTERNAL_INPUT_CHECK_BUTTON){
    int requested=(int)N(args,count,1);
    return vreal(gml_input_gamepad_connected(vm,device) &&
                 (external_input_button_mask(vm,device)&requested)!=0);
  }
  if(operation==GML_EXTERNAL_INPUT_LEFT_TRIGGER ||
     operation==GML_EXTERNAL_INPUT_RIGHT_TRIGGER){
    int button=operation==GML_EXTERNAL_INPUT_LEFT_TRIGGER?32775:32776;
    return vreal(gml_input_gamepad(vm,device,button,0)?255:0);
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
  /* Window-space coordinates address the presented frame directly. */
  if(!strcmp(nm,"window_mouse_set")){ gml_input_mouse_set(vm,N(a,n,0),N(a,n,1)); return vreal(0); }
  /* The common SDL joydll extension uses zero-based devices, axes and buttons. Translate its
   * polling surface to the same normalized gamepad snapshot as GameMaker's joystick builtins.
   * Trackballs are not represented by the host input contract and hats use the first d-pad. */
  if(!strcmp(nm,"joy_init")||!strcmp(nm,"joy_update")||!strcmp(nm,"joy_close"))
    return vreal(1);
  if(!strcmp(nm,"joy_count")) return vreal(joydll_count(vm));
  if(!strcmp(nm,"joy_find")) return vreal(joydll_find(vm));
  if(!strcmp(nm,"joy_name"))
    return vstr(gml_input_gamepad_connected(vm,joy_device(N(a,n,0)))?joydll_name():"");
  if(!strcmp(nm,"joy_axes")) return vreal(4);
  if(!strcmp(nm,"joy_axis"))
    return vreal(joy_axis_value(vm,joy_device(N(a,n,0)),(int)N(a,n,1)));
  if(!strcmp(nm,"joy_buttons")) return vreal(16);
  if(!strcmp(nm,"joy_button")){
    int device=joy_device(N(a,n,0));
    int code=joy_button_code((int)N(a,n,1));
    return vreal(code?gml_input_gamepad(vm,device,code,0):0);
  }
  if(!strcmp(nm,"joy_hats")) return vreal(1);
  if(!strcmp(nm,"joy_hat")) return vreal(joydll_hat_degrees(vm,joy_device(N(a,n,0))));
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
    int dev=joystick_device(N(a,n,0));
    return vreal((b>=1 && b<=16) ? gml_input_gamepad(vm,dev,32768+b,0) : 0); }
  if(!strcmp(nm,"joystick_xpos")){ int dev=joystick_device(N(a,n,0));
    return vreal(gml_input_gamepad(vm,dev,32784,0) - gml_input_gamepad(vm,dev,32783,0)); }
  if(!strcmp(nm,"joystick_ypos")){ int dev=joystick_device(N(a,n,0));
    return vreal(gml_input_gamepad(vm,dev,32782,0) - gml_input_gamepad(vm,dev,32781,0)); }
  if(!strcmp(nm,"joystick_direction")){
    int dev=joystick_device(N(a,n,0));
    int x=gml_input_gamepad(vm,dev,32784,0) - gml_input_gamepad(vm,dev,32783,0);
    int y=gml_input_gamepad(vm,dev,32782,0) - gml_input_gamepad(vm,dev,32781,0);
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
