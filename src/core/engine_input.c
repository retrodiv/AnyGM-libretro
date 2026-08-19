/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Normalized keyboard, pad, pointer, edge, rumble, and VM input services. */
#include "engine_internal.h"
#include "anygm_host.h"

static int core_opt_mouse_mode(AnygmEngine *engine);
static int core_opt_gamepad_connected(AnygmEngine *engine);

/* ---- normalized input snapshot (held this frame plus prior-frame edges) ---- */
static int anygm_key_for_vk(int vk);
static int event_key_state_for_vk(AnygmEngine *engine,int vk, int prev);
static int dbg_key_enabled(AnygmEngine *engine){
  if(engine->diagnostics.key_enabled < 0) engine->diagnostics.key_enabled = anygm_host_development_setting(&engine->host,"GML_DBG_KEY") != NULL;
  return engine->diagnostics.key_enabled;
}
static void dbg_key_log(AnygmEngine *engine,int vk, int edge, int cur, int prev, int out){
  if(!out || !dbg_key_enabled(engine)) return;
  engine_logf(engine,ANYGM_LOG_DEBUG,"[key] f%ld vk=%d edge=%d cur=%d prev=%d -> %d\n",engine->vm.frame,vk,edge,cur,prev,out);
}
/* map a GM virtual-key code to a RetroPad button id (-1 = unmapped) */
static int vk_to_pad(AnygmEngine *engine,int vk){
  if(anygm_policy_uses_classic_runtime(&engine->win)){
    switch(vk){
      case 37: return ANYGM_PAD_LEFT;
      case 39: return ANYGM_PAD_RIGHT;
      case 38: return ANYGM_PAD_UP;
      case 40: return ANYGM_PAD_DOWN;
      /* Classic keyboard content commonly uses either ZXCV or ASDF as its action row. Map both
       * rows to the same face buttons; corresponding keys from both rows represent one action. */
      case 90: case 65: return ANYGM_PAD_FACE_BOTTOM;  /* Z or A */
      case 88: case 83: return ANYGM_PAD_FACE_RIGHT;   /* X or S */
      case 67: case 68: return ANYGM_PAD_FACE_LEFT;    /* C or D */
      case 86: case 70: return ANYGM_PAD_FACE_TOP;     /* V or F */
      case 13: case 77: return ANYGM_PAD_START; /* Enter / M */
      case 32: return ANYGM_PAD_SELECT;  /* Space */
      case 16: return ANYGM_PAD_LEFT_SHOULDER;   /* Shift */
      case 17: return ANYGM_PAD_RIGHT_SHOULDER;  /* Control */
      default: return -1;
    }
  }
  switch(vk){
    case 37: return ANYGM_PAD_LEFT;   /* vk_left  */
    case 39: return ANYGM_PAD_RIGHT;  /* vk_right */
    case 38: return ANYGM_PAD_UP;     /* vk_up    */
    case 40: return ANYGM_PAD_DOWN;   /* vk_down  */
    case 90: case 32: return ANYGM_PAD_FACE_BOTTOM;   /* Z / space = button1: attack/confirm */
    case 88: return ANYGM_PAD_FACE_LEFT;            /* X = button2: jump                 */
    case 67: return ANYGM_PAD_FACE_RIGHT;            /* C keyboard alias on RetroPad A    */
    case 13: return ANYGM_PAD_START;        /* enter = start      */
    case 77: return ANYGM_PAD_START;        /* M (key_start arcade)*/
    case 16: return ANYGM_PAD_SELECT;       /* shift = coin/select */
    default: return -1;
  }
}
/* The gamepad option selects ownership. Off makes RetroPad a keyboard compatibility layer; On
 * exposes an independent pad and leaves keyboard state to the frontend's key source — but only
 * for content that can hear a pad at all. Content that never references a joystick_* or
 * gamepad_* builtin has no other way to receive input from a pad-only frontend, so for it the
 * RetroPad remains the keyboard whatever the option reports. */
static int engine_pad_reserved_for_pad_api(AnygmEngine *engine){
  return core_opt_gamepad_connected(engine) &&
         gml_win_references_pad_input(&engine->win);
}
static int engine_input_key(void *userdata,int vk, int edge){
  AnygmEngine *engine=userdata;
  if(vk == 1 || vk == 0){   /* vk_anykey (1) / vk_nokey (0): aggregate over every input */
    int any = 0, anyp = 0;
    if(!engine_pad_reserved_for_pad_api(engine)){
      for(int i = 0; i < NPAD; i++){
        any |= engine->pad_current[i];
        anyp |= engine->pad_previous[i];
      }
    }
    for(int i = 2; i < NKEY; i++){
      any |= engine->key_current[i] | engine->hardware_key_current[i] | engine->event_vk_current[i];
      anyp |= engine->key_previous[i] | engine->hardware_key_previous[i] | engine->event_vk_previous[i];
    }  /* skip 0/1 (the sentinels) */
    for(int i = 1; i < ANYGM_KEY_LAST; i++){
      any |= engine->event_key_current[i];
      anyp |= engine->event_key_previous[i];
    }
    int cur  = (vk == 1) ? any  : !any;    /* anykey = something down; nokey = nothing down */
    int prev = (vk == 1) ? anyp : !anyp;
    int out = edge==1 ? (cur && !prev) : edge==2 ? (!cur && prev) : cur;
    dbg_key_log(engine,vk, edge, cur, prev, out);
    return out;
  }
  /* Held state is the union of input sources, but edges are evaluated per source
   * and then joined. A held simulated key cannot suppress a new physical-key
   * transition of the same virtual key. */
  int sim_c=0, sim_p=0, hw_c=0, hw_p=0, evk_c=0, evk_p=0, pad_c=0, pad_p=0;
  if(vk >= 0 && vk < NKEY){
    sim_c=engine->key_current[vk];      sim_p=engine->key_previous[vk];
    hw_c=engine->hardware_key_current[vk]; hw_p=engine->hardware_key_previous[vk];
    evk_c=engine->event_vk_current[vk]; evk_p=engine->event_vk_previous[vk];
  }
  int key_c = event_key_state_for_vk(engine,vk, 0);
  int key_p = event_key_state_for_vk(engine,vk, 1);
  if(!engine_pad_reserved_for_pad_api(engine)){
    int b = vk_to_pad(engine,vk);
    if(b >= 0){ pad_c=engine->pad_current[b]; pad_p=engine->pad_previous[b]; }
  }
  /* A simulated press cancelled by its own frame's release is not readable in that frame: the
   * carry hands it to the following one, where the key events run. Reading it in both frames
   * would answer one synthesized press twice. */
  int sim_edge = vk>=0 && vk<NKEY ?
      (engine->key_press_raised[vk] && !engine->key_press_carry[vk]) : 0;
  int cur = sim_c|hw_c|evk_c|key_c|pad_c;
  int prev = sim_p|hw_p|evk_p|key_p|pad_p;
  int out;
  if(edge==1)
    out = (sim_c&&!sim_p)||sim_edge||
          (hw_c&&!hw_p)||(evk_c&&!evk_p)||(key_c&&!key_p)||(pad_c&&!pad_p);
  else if(edge==2)
    out = (!sim_c&&sim_p)||(!hw_c&&hw_p)||(!evk_c&&evk_p)||(!key_c&&key_p)||(!pad_c&&pad_p);
  else
    out = cur;
  dbg_key_log(engine,vk, edge, cur, prev, out);
  return out;
}
static void engine_input_key_clear(void *userdata,int vk){
  AnygmEngine *engine=userdata;
  if(vk >= 0 && vk < NKEY){ engine->key_current[vk]=0; engine->key_previous[vk]=0; }
}
static void engine_input_key_press(void *userdata,int vk){
  AnygmEngine *engine=userdata;
  if(vk >= 0 && vk < NKEY){
    engine->key_current[vk]=1; engine->key_previous[vk]=0;
    engine->key_press_raised[vk]=1;
    engine->key_press_step[vk]=1;
    engine->key_release_defer[vk]=0;
  }
}
static void engine_input_key_release(void *userdata,int vk){
  AnygmEngine *engine=userdata;
  if(vk >= 0 && vk < NKEY){
    if(engine->key_press_raised[vk]) engine->key_press_carry[vk]=1;
    /* A release paired with a press in the running step is deferred. The key
     * remains held through the following frame, including Begin Step; repeated
     * paired presses sustain it. A release after an earlier-step press remains
     * immediate. This distinguishes per-step hold from a single press/release. */
    if(engine->key_press_step[vk]){ engine->key_release_defer[vk]=1; return; }
    engine->key_current[vk]=0; engine->key_previous[vk]=1;
  }
}
static int anygm_key_for_vk(int vk){
  if(vk >= 'A' && vk <= 'Z') return ANYGM_KEY_a + (vk - 'A');
  if(vk >= '0' && vk <= '9') return vk;
  switch(vk){
    case 8: return ANYGM_KEY_BACKSPACE;
    case 9: return ANYGM_KEY_TAB;
    case 13: return ANYGM_KEY_RETURN;
    case 16: return ANYGM_KEY_LSHIFT;   /* handled specially with RSHIFT too */
    case 17: return ANYGM_KEY_LCTRL;    /* handled specially with RCTRL too */
    case 18: return ANYGM_KEY_LALT;     /* handled specially with RALT too */
    case 27: return ANYGM_KEY_ESCAPE;
    case 32: return ANYGM_KEY_SPACE;
    case 33: return ANYGM_KEY_PAGEUP;
    case 34: return ANYGM_KEY_PAGEDOWN;
    case 35: return ANYGM_KEY_END;
    case 36: return ANYGM_KEY_HOME;
    case 37: return ANYGM_KEY_LEFT;
    case 38: return ANYGM_KEY_UP;
    case 39: return ANYGM_KEY_RIGHT;
    case 40: return ANYGM_KEY_DOWN;
    case 45: return ANYGM_KEY_INSERT;
    case 46: return ANYGM_KEY_DELETE;
    default:
      if(vk >= 112 && vk <= 123) return ANYGM_KEY_F1 + (vk - 112);
      if(vk >= 32 && vk <= 126) return vk;
      return -1;
  }
}
static int event_key_state_for_vk(AnygmEngine *engine,int vk, int prev){
  const uint8_t *keys = prev ? engine->event_key_previous : engine->event_key_current;
  int rk=anygm_key_for_vk(vk);
  int down = 0;
  if(rk >= 0 && rk < ANYGM_KEY_LAST) down |= keys[rk];
  if(vk == 16) down |= keys[ANYGM_KEY_RSHIFT] | keys[ANYGM_KEY_LSHIFT];
  else if(vk == 17) down |= keys[ANYGM_KEY_RCTRL] | keys[ANYGM_KEY_LCTRL];
  else if(vk == 18) down |= keys[ANYGM_KEY_RALT] | keys[ANYGM_KEY_LALT];
  return down;
}
void engine_input_poll_keyboard(AnygmEngine *engine){
  /* The frame that raised a press has ended. One cancelled by its own frame's release is carried
   * into this frame's event phase; every other trace of it goes. */
  memset(engine->key_press_raised,0,sizeof engine->key_press_raised);
  memcpy(engine->key_press_raised,engine->key_press_carry,sizeof engine->key_press_raised);
  memset(engine->key_press_carry,0,sizeof engine->key_press_carry);
  /* A release the pressing step deferred comes due one whole frame later, so the key is down for
   * every phase of the frame after the press — Begin Step included — and a bridge pressing once a
   * frame never lets it go. */
  for(int vk=0;vk<NKEY;vk++){
    if(!engine->key_release_defer[vk]) continue;
    if(engine->key_release_defer[vk]==1){ engine->key_release_defer[vk]=2; continue; }
    engine->key_current[vk]=0;
    engine->key_release_defer[vk]=0;
  }
  memset(engine->key_press_step,0,sizeof engine->key_press_step);
  memcpy(engine->hardware_key_previous, engine->hardware_key_current, sizeof(engine->hardware_key_current));
  memcpy(engine->event_key_previous,engine->event_key_current,sizeof engine->event_key_current);
  memset(engine->hardware_key_current, 0, sizeof(engine->hardware_key_current));
  memcpy(engine->event_key_current,engine->input.keys,sizeof engine->event_key_current);
  memset(engine->event_vk_current,0,sizeof engine->event_vk_current);
  static const unsigned char vk_poll[] = {
    8,9,13,16,17,18,27,32,33,34,35,36,37,38,39,40,45,46,
    '0','1','2','3','4','5','6','7','8','9',
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    112,113,114,115,116,117,118,119,120,121,122,123
  };
  for(size_t i=0; i<sizeof(vk_poll)/sizeof(vk_poll[0]); i++){
    int vk = vk_poll[i];
    int rk=anygm_key_for_vk(vk);
    if(rk < 0) continue;
    int down = engine->input.keys[rk] != 0;
    if(vk == 16) down |= engine->input.keys[ANYGM_KEY_RSHIFT] != 0;
    else if(vk == 17) down |= engine->input.keys[ANYGM_KEY_RCTRL] != 0;
    else if(vk == 18) down |= engine->input.keys[ANYGM_KEY_RALT] != 0;
    engine->hardware_key_current[vk] = down ? 1 : 0;
  }
  /* keyboard_key_press latches a simulated key until keyboard_key_release or a falling edge
   * from the corresponding physical key. A gamepad-to-keyboard bridge can release it explicitly
   * when the pad direction changes. */
  for(int vk=0;vk<NKEY;vk++)
    if(engine->key_current[vk] && engine->hardware_key_previous[vk] &&
       !engine->hardware_key_current[vk])
      engine->key_current[vk]=0;
}
/* GM gamepad button constant (gp_face1=32769 …) -> RetroPad button id (-1 = unmapped) */
static int gp_to_pad(AnygmEngine *engine,int gp){
  /* Modern function-value profiles encode button enums as 1..16. Earlier
   * profiles also accept 0..15 raw indexes; the XInput-compatible slot maps
   * these indexes to digital-button bit positions. Keep both interpretations
   * tied to the resolved profile. */
  if(anygm_policy_has_modern_function_values(&engine->win)) {
    if(gp >= 1 && gp <= 16) gp += 32768;
  } else if(gp >= 0 && gp < 16) {
    static const int xinput_bit_to_gp[16]={
      32781,32782,32783,32784,   /* 0..3   dpad up, down, left, right */
      32778,32777,32779,32780,   /* 4..7   start, back, left thumb, right thumb */
      32773,32774,-1,-1,         /* 8..11  shoulders, guide, reserved */
      32769,32770,32771,32772    /* 12..15 A, B, X, Y */
    };
    gp=xinput_bit_to_gp[gp];
    if(gp<0) return -1;
  }
  switch(gp){
    case 32769: return ANYGM_PAD_FACE_BOTTOM;       /* gp_face1 */
    case 32770: return ANYGM_PAD_FACE_RIGHT;       /* gp_face2 */
    case 32771: return ANYGM_PAD_FACE_LEFT;       /* gp_face3 */
    case 32772: return ANYGM_PAD_FACE_TOP;       /* gp_face4 */
    case 32773: return ANYGM_PAD_LEFT_SHOULDER;       /* gp_shoulderl */
    case 32774: return ANYGM_PAD_RIGHT_SHOULDER;       /* gp_shoulderr */
    case 32775: return ANYGM_PAD_LEFT_TRIGGER;      /* gp_shoulderlb */
    case 32776: return ANYGM_PAD_RIGHT_TRIGGER;      /* gp_shoulderrb */
    case 32777: return ANYGM_PAD_SELECT;  /* gp_select */
    case 32778: return ANYGM_PAD_START;   /* gp_start */
    case 32779: return ANYGM_PAD_LEFT_STICK;      /* gp_stickl */
    case 32780: return ANYGM_PAD_RIGHT_STICK;      /* gp_stickr */
    case 32781: return ANYGM_PAD_UP;      /* gp_padu */
    case 32782: return ANYGM_PAD_DOWN;    /* gp_padd */
    case 32783: return ANYGM_PAD_LEFT;    /* gp_padl */
    case 32784: return ANYGM_PAD_RIGHT;   /* gp_padr */
    default: return -1;
  }
}
static int engine_input_gamepad(void *userdata,int button, int edge){
  AnygmEngine *engine=userdata;
  int b = gp_to_pad(engine,button); if(b < 0) return 0;
  int cur = engine->pad_current[b], prev = engine->pad_previous[b];
  if(anygm_host_development_setting(&engine->host,"GML_DBG_GP_STATE")){
    engine_logf(engine,ANYGM_LOG_DEBUG,"[gpstate] f%ld button=%d pad=%d edge=%d cur=%d prev=%d\n",
            engine->vm.frame,button,b,edge,cur,prev);
  }
  switch(edge){ case 1: return cur && !prev; case 2: return !cur && prev; default: return cur; }
}
static int gp_axis_slot(int axis){
  switch(axis){
    case 0: case 32785: return 0;  /* gp_axislh */
    case 1: case 32786: return 1;  /* gp_axislv */
    case 2: case 32787: return 2;  /* gp_axisrh */
    case 3: case 32788: return 3;  /* gp_axisrv */
    default: return -1;
  }
}
static double engine_input_gamepad_axis(void *userdata,int device, int axis){
  AnygmEngine *engine=userdata;
  int slot = gp_axis_slot(axis);
  if(device != 0 || slot < 0) return 0.0;
  return engine->axis_current[slot];
}

/* ---- mouse/pointer: absolute pointer plus relative mouse deltas and buttons. Position is kept in
 * presented-frame pixels; gml_input_mouse maps it to room, GUI, and window coordinate spaces for
 * the GM mouse builtins. ---- */
/* Previous frame's application-surface placement inside the published framebuffer. Pointer
 * coordinates arrive in framebuffer pixels, whereas runtime mouse coordinates live first in the
 * application surface and then in the active room view. */
void engine_input_poll_mouse(AnygmEngine *engine){
  memcpy(engine->mouse_button_previous, engine->mouse_button_current, sizeof(engine->mouse_button_current));
  engine->mouse_wheel=engine->input.wheel_delta;
  int px=engine->input.pointer_x,py=engine->input.pointer_y;
  int pp=engine->input.pointer_pressed?1:0;
  int dx=engine->input.mouse_delta_x,dy=engine->input.mouse_delta_y;
  unsigned ow = engine->output_width ? engine->output_width : engine->width, oh = engine->output_height ? engine->output_height : engine->height;
  if(engine->host_canvas_active && engine->host_canvas_width>0 && engine->host_canvas_height>0){
    if(px>=0)
      px=(int)lround((px-engine->host_canvas_x)*(double)ow/engine->host_canvas_width);
    if(py>=0)
      py=(int)lround((py-engine->host_canvas_y)*(double)oh/engine->host_canvas_height);
    dx=(int)lround(dx*(double)ow/engine->host_canvas_width);
    dy=(int)lround(dy*(double)oh/engine->host_canvas_height);
  }
  int mmode = core_opt_mouse_mode(engine);              /* 0 auto, 1 absolute, 2 relative */
  int pointer_signal=(px>=0 && py>=0) || pp;
  if(pointer_signal) engine->pointer_active = 1;
  int use_pointer = mmode != 2 && (mmode == 1 || engine->pointer_active);
  if(use_pointer && px>=0 && py>=0){
    engine->mouse_pixel_x=px;
    engine->mouse_pixel_y=py;
  } else if(mmode != 1 && (dx || dy)){            /* RELATIVE — accumulate deltas from last position */
    if(engine->mouse_pixel_x < 0){ engine->mouse_pixel_x = ow / 2.0; engine->mouse_pixel_y = oh / 2.0; }
    engine->mouse_pixel_x += dx; engine->mouse_pixel_y += dy;
  }
  if(engine->mouse_pixel_x >= 0){                             /* clamp to the frame once we have any position */
    if(engine->mouse_pixel_x > ow) engine->mouse_pixel_x = ow;
    if(engine->mouse_pixel_x < 0) engine->mouse_pixel_x = 0;
    if(engine->mouse_pixel_y > oh) engine->mouse_pixel_y = oh;
    if(engine->mouse_pixel_y < 0) engine->mouse_pixel_y = 0;
  }
  engine->mouse_button_current[0]=(engine->input.mouse_buttons[0]!=0) || pp;
  engine->mouse_button_current[1]=engine->input.mouse_buttons[1]!=0;
  engine->mouse_button_current[2]=engine->input.mouse_buttons[2]!=0;
}
/* Fill GM-space mouse state. Spaces: room (view transform), GUI (display_set_gui_size space),
 * window (presented-frame px). held/pressed/released are bitmasks: bit0=left,1=right,2=middle. */
static void engine_input_mouse(void *userdata,double *rx, double *ry, double *gx, double *gy, double *wx, double *wy,
                     int *held, int *pressed, int *released, int *wheel){
  AnygmEngine *engine=userdata;
  double sx = engine->mouse_pixel_x < 0 ? 0 : engine->mouse_pixel_x, sy = engine->mouse_pixel_y < 0 ? 0 : engine->mouse_pixel_y;
  unsigned ow = engine->output_width ? engine->output_width : (engine->width ? engine->width : 1), oh = engine->output_height ? engine->output_height : (engine->height ? engine->height : 1);
  double appx=sx, appy=sy;
  if(engine->present_mouse_valid && engine->present_mouse_width>0 && engine->present_mouse_height>0 &&
     engine->present_mouse_source_width>0 && engine->present_mouse_source_height>0){
    appx=(sx-engine->present_mouse_x)*(double)engine->present_mouse_source_width/engine->present_mouse_width;
    appy=(sy-engine->present_mouse_y)*(double)engine->present_mouse_source_height/engine->present_mouse_height;
  }
  if(wx) *wx = sx;
  if(wy) *wy = sy;
  if(gx || gy){
    double gxx, gyy;
    if(engine->canvas_mode){ gxx = sx - engine->gui_offset_x; gyy = sy - engine->gui_offset_y; }   /* canvas is 1:1 GUI space */
    else if(engine->aspect_force_active){
      int full = aspect_compositor_fullwidth_gen(engine);
      gxx = sx - (full ? 0 : engine->gui_offset_x);
      gyy = sy - (full ? 0 : engine->gui_offset_y);
    }
    else if(engine->vm.gui_maximise_active){
      int window_w=engine->vm.window_w>0?engine->vm.window_w:(int)(engine->win.disp_w?engine->win.disp_w:ow);
      int window_h=engine->vm.window_h>0?engine->vm.window_h:(int)(engine->win.disp_h?engine->win.disp_h:oh);
      double xscale=engine->vm.gui_maximise_xscale>0.0?engine->vm.gui_maximise_xscale:1.0;
      double yscale=engine->vm.gui_maximise_yscale>0.0?engine->vm.gui_maximise_yscale:1.0;
      gxx=(sx*window_w/(double)ow-engine->vm.gui_maximise_xoffset)/xscale;
      gyy=(sy*window_h/(double)oh-engine->vm.gui_maximise_yoffset)/yscale;
    } else { gxx = sx * (engine->gui_space_width > 0 ? engine->gui_space_width : (int)ow) / (double)ow;
             gyy = sy * (engine->gui_space_height > 0 ? engine->gui_space_height : (int)oh) / (double)oh; }
    if(gx) *gx = gxx;
    if(gy) *gy = gyy;
  }
  if(rx || ry){
    double vx = gml_global_arr(&engine->vm, "view_xview", 0), vy = gml_global_arr(&engine->vm, "view_yview", 0);
    double wv = gml_global_arr(&engine->vm, "view_wview", 0), hv = gml_global_arr(&engine->vm, "view_hview", 0);
    double sx2 = appx, sy2 = appy;
    double roomx, roomy;
    if(engine->aspect_force_active){
      double camx, camy;
      aspect_forced_camera(engine,vx, vy, &camx, &camy);
      roomx = camx + sx2 * (double)engine->width /
              (double)(engine->present_mouse_source_width>0?engine->present_mouse_source_width:(int)(ow?ow:1));
      roomy = camy + sy2 * (double)engine->height /
              (double)(engine->present_mouse_source_height>0?engine->present_mouse_source_height:(int)(oh?oh:1));
    } else if(wv > 0 && hv > 0){
      double xp=gml_global_arr(&engine->vm,"view_xport",0), yp=gml_global_arr(&engine->vm,"view_yport",0);
      double wp=gml_global_arr(&engine->vm,"view_wport",0), hp=gml_global_arr(&engine->vm,"view_hport",0);
      if(wp<=0) wp=engine->present_mouse_source_width>0?engine->present_mouse_source_width:(int)(ow?ow:1);
      if(hp<=0) hp=engine->present_mouse_source_height>0?engine->present_mouse_source_height:(int)(oh?oh:1);
      roomx = vx + (sx2-xp) * wv / wp;
      roomy = vy + (sy2-yp) * hv / hp;
    } else { roomx = sx2; roomy = sy2; }
    if(anygm_host_development_setting(&engine->host,"GML_LOG_MOUSE_COORDS")){
      if(engine->diagnostics.mouse_frame!=engine->vm.frame){
        engine->diagnostics.mouse_frame=engine->vm.frame;
        engine_logf(engine,ANYGM_LOG_DEBUG,"[mouse-coords] f%ld frame=%.1f,%.1f app=%.1f,%.1f room=%.1f,%.1f present=%d,%d %dx%d src=%dx%d\n",
          engine->vm.frame,sx,sy,appx,appy,roomx,roomy,
          engine->present_mouse_x,engine->present_mouse_y,engine->present_mouse_width,engine->present_mouse_height,
          engine->present_mouse_source_width,engine->present_mouse_source_height);
      }
    }
    if(rx) *rx = roomx;
    if(ry) *ry = roomy;
  }
  int h = (engine->mouse_button_current[0] ? 1 : 0) | (engine->mouse_button_current[1] ? 2 : 0) | (engine->mouse_button_current[2] ? 4 : 0);
  int p = ((engine->mouse_button_current[0] && !engine->mouse_button_previous[0]) ? 1 : 0) | ((engine->mouse_button_current[1] && !engine->mouse_button_previous[1]) ? 2 : 0) |
          ((engine->mouse_button_current[2] && !engine->mouse_button_previous[2]) ? 4 : 0);
  int r = ((!engine->mouse_button_current[0] && engine->mouse_button_previous[0]) ? 1 : 0) | ((!engine->mouse_button_current[1] && engine->mouse_button_previous[1]) ? 2 : 0) |
          ((!engine->mouse_button_current[2] && engine->mouse_button_previous[2]) ? 4 : 0);
  if(held) *held = h;
  if(pressed) *pressed = p;
  if(released) *released = r;
  if(wheel) *wheel = engine->mouse_wheel;
}
static void engine_input_mouse_set(void *userdata,double x, double y){
  AnygmEngine *engine=userdata;
  unsigned ow=engine->output_width?engine->output_width:(engine->width?engine->width:1), oh=engine->output_height?engine->output_height:(engine->height?engine->height:1);
  engine->mouse_pixel_x=x<0?0:(x>(double)ow?ow:x);
  engine->mouse_pixel_y=y<0?0:(y>(double)oh?oh:y);
}


static int core_opt_mouse_mode(AnygmEngine *engine) {
  return engine->config.mouse_mode<=2u?(int)engine->config.mouse_mode:0;
}


static int core_opt_gamepad_connected(AnygmEngine *engine) {
  if(engine->config.gamepad_connected==ANYGM_GAMEPAD_AUTO){
    /* Apply the same structural input-reference rule to every generation. Content
     * without a pad-reading reference retains keyboard emulation. */
    return gml_win_references_pad_input(&engine->win);
  }
  return engine->config.gamepad_connected?1:0;
}


static int engine_input_gamepad_connected(void *userdata,int device) {
  AnygmEngine *engine=userdata;
  return device == 0 && core_opt_gamepad_connected(engine);
}
static int engine_input_gamepad_device_count(void *userdata) {
  (void)userdata;
  return (int)ANYGM_MAX_GAMEPADS;
}
static uint16_t rumble_strength(double v) {
  if (!isfinite(v) || v <= 0.0) return 0;
  if (v >= 1.0) return 0xffffu;
  return (uint16_t)(v * 65535.0 + 0.5);
}
static void engine_input_gamepad_set_vibration(void *userdata,int device, double low, double high) {
  AnygmEngine *engine=userdata;
  if(!engine->host.rumble || device<0) return;
  engine->host.rumble(engine->host.userdata,(uint32_t)device,rumble_strength(low),rumble_strength(high));
}

void engine_input_bind(AnygmEngine *engine){
  engine->vm.input.userdata=engine;
  engine->vm.input.key=engine_input_key;
  engine->vm.input.key_clear=engine_input_key_clear;
  engine->vm.input.key_press=engine_input_key_press;
  engine->vm.input.key_release=engine_input_key_release;
  engine->vm.input.gamepad=engine_input_gamepad;
  engine->vm.input.gamepad_connected=engine_input_gamepad_connected;
  engine->vm.input.gamepad_device_count=engine_input_gamepad_device_count;
  engine->vm.input.gamepad_axis=engine_input_gamepad_axis;
  engine->vm.input.gamepad_vibration=engine_input_gamepad_set_vibration;
  engine->vm.input.mouse=engine_input_mouse;
  engine->vm.input.mouse_set=engine_input_mouse_set;
}
