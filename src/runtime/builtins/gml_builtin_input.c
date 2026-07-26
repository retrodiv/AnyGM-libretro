/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Keyboard, gamepad, pointer, joystick, and display-coordinate adaptation. */
#include "gml_builtin_internal.h"
#include "gml_render.h"

#include <string.h>

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
