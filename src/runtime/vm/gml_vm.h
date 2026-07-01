/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_vm.h — bytecode-14 interpreter + instance/object/event model. */
#ifndef GML_VM_H
#define GML_VM_H
#include "gml_win.h"

/* ---- value ---- */
typedef enum { V_REAL=0, V_STR=1, V_ARR=2 } GmlValType;
typedef struct { GmlValType t; double d; const char *s; void *arr; } GmlVal;
static inline GmlVal vreal(double d){ GmlVal v; v.t=V_REAL; v.d=d; v.s=0; v.arr=0; return v; }
static inline GmlVal vstr(const char *s){ GmlVal v; v.t=V_STR; v.d=0; v.s=s; v.arr=0; return v; }

/* ---- variable map: open-addressing, key = interned name pointer ---- */
typedef struct { const char *key; GmlVal val; } GmlVarSlot;
typedef struct { GmlVarSlot *slots; int cap, len; } GmlVarMap;

/* ---- instance ---- */
#define GML_ALARMS 12
typedef struct {
  int     active;      /* slot in use */
  int     marked;      /* pending destroy */
  int     deactivated; /* instance_deactivate_*: exists but skipped by step/draw/collision/queries */
  uint32_t id;         /* instance id */
  int     obj;         /* object index */
  /* builtin vars */
  double  x, y, xprevious, yprevious, xstart, ystart;
  double  sprite_index, image_index, image_speed, image_xscale, image_yscale;
  double  image_angle, image_alpha, image_blend;
  double  depth, visible, solid, persistent;
  double  hspeed, vspeed, direction, speed, gravity, gravity_direction, friction;
  double  alarm[GML_ALARMS];
  /* path following (path_start): path_index=-1 when not on a path */
  double  path_index, path_position, path_positionprevious, path_speed, path_orientation, path_scale;
  double  path_endaction, path_xoff, path_yoff;   /* xoff/yoff: relative-mode anchor (absolute=0) */
  GmlVarMap vars;      /* custom instance variables */
} GmlInstance;

/* ---- path (parsed from PATH) ---- */
typedef struct { double x, y, clen; } GmlPathPt;   /* clen = cumulative length to this point */
typedef struct { GmlPathPt *pts; int n; int closed; double len; } GmlPath;

/* ---- object (parsed from OBJT) ---- */
typedef struct {
  const char *name;
  int sprite_index, parent, depth, visible, solid, persistent;
  int bevents;   /* boundary-event flags: 1=Outside Room, 2=Intersect Room, 4=Outside View0, 8=Intersect View0 */
  /* event code: indexed [evtype][slot]; we keep a small dynamic list */
  struct { int evtype, subtype, code; } *events; int n_events;
} GmlObject;

/* ---- VM ---- */
typedef struct { int self_obj, target_obj, code; } GmlColEvent;  /* Collision_<target> handler */

typedef struct GmlVM {
  GmlWin   *win;
  GmlVarMap globals;
  GmlObject *objects; int n_objects;
  GmlPath *paths; int n_paths;
  GmlColEvent *col_events; int n_col_events;
  GmlInstance *inst; int inst_cap, inst_count;
  uint32_t next_id;
  int      room_index;        /* current room (ROOM index) */
  int      pending_room;      /* -1 none, else target ROOM index (play-order resolved) */
  int      game_end;
  int      started;           /* Game Start fired */
  double   last_key;          /* GM keyboard_lastkey: last vk pressed */
  double   window_fullscreen;  /* GM window_get/set_fullscreen: menu state */
  /* execution context */
  GmlInstance *cur_self, *cur_other;
  const char *cur_event; int cur_event_obj;   /* current event suffix + object level (for event_inherited) */
  /* RNG: WELL512 (Lomont compact form), MSVC-LCG seeding, default seed 0.
   * random(x) = (next()/2^32)*x. See LICENSES/WELL512.txt for the recurrence source. */
  uint32_t rng_well[16]; int rng_index;
  uint32_t rng_state;   /* (legacy; unused by the WELL512 path) */
  /* tile-layer runtime mutations (tile_layer_delete/depth/shift), reset on room enter.
   * Pure no-op when n_tile_mut==0 → zero effect on the hot draw path for rooms that
   * never call tile_layer_*; only those that do pay the per-tile lookup. */
  struct { int depth, deleted, has_remap, remap; double dx, dy; } tile_mut[64];
  int n_tile_mut;
  /* tile_layer_delete_at: per-tile deletion by position.
   * At draw time, tiles matching (depth, x, y) are dropped. */
  struct { int depth, x, y; } tile_del_at[64]; int n_tile_del_at;
  /* INI persistence: the currently-open .ini as a simple key-value map */
  struct { char *section, *key; double val; } ini_kv[256]; int ini_n, ini_open;
  char ini_path[256];
  void    *render;   /* GmlRender* (set by the frontend) for draw_* builtins */
  void    *audio;    /* GmlAudio*  (set by the frontend) for audio_* builtins */
} GmlVM;

int     gml_vm_init(GmlVM *vm, GmlWin *win);
void    gml_vm_free(GmlVM *vm);
void    gml_rng_seed(GmlVM *vm, uint32_t seed);   /* GM WELL512 seeding (MSVC LCG expand) */
double  gml_rng_value(GmlVM *vm);                 /* GM next()/2^32 → [0,1) */
GmlVal  gml_vm_run_code(GmlVM *vm, int code_index, GmlInstance *self, GmlInstance *other,
                        GmlVal *args, int n_args);
int     gml_code_index_by_name(GmlWin *win, const char *name);  /* exact */
int     gml_code_index_find(GmlWin *win, const char *substr);   /* first containing */

/* instances / events / rooms */
GmlInstance *gml_instance_create(GmlVM *vm, double x, double y, int obj);  /* runs Create */
void         gml_path_start(GmlVM *vm, GmlInstance *in, int path, double speed, double endaction, int absolute);
/* tile-layer runtime mutations (tile_layer_* builtins) on the current room's tiles */
void         gml_tile_layer_delete(GmlVM *vm, int depth);
void         gml_tile_layer_depth(GmlVM *vm, int depth, int newdepth);
void         gml_tile_layer_shift(GmlVM *vm, int depth, double dx, double dy);
void         gml_tile_layer_delete_at(GmlVM *vm, int depth, double x, double y);
int          gml_event_inherited(GmlVM *vm);   /* run the current event on the parent object */
double       gml_inst_var_get(GmlVM *vm, GmlInstance *in, const char *name); /* read a builtin or custom var */
int          gml_val_array_length(GmlVal v);   /* array_length_1d: length of a V_ARR value, else 0 */
void         gml_instance_destroy(GmlVM *vm, GmlInstance *in);
int          gml_instance_number(GmlVM *vm, int obj);
int          gml_object_is(GmlVM *vm, int obj, int target);  /* obj == target or descends from it */
int          gml_object_index_by_name(GmlVM *vm, const char *name);  /* -1 if not found */
GmlInstance *gml_find_instance(GmlVM *vm, int obj);          /* first active instance of obj (or child) */
int          gml_run_event(GmlVM *vm, GmlInstance *in, const char *suffix); /* e.g. "Create_0" */
void         gml_room_enter(GmlVM *vm, int room_index);      /* instantiate + Create events */
void         gml_vm_goto_room_order(GmlVM *vm, int order_index);
void         gml_vm_step(GmlVM *vm);                         /* one frame of the game loop */
void         gml_vm_draw(GmlVM *vm);                         /* draw phase (needs vm->render) */
void         gml_vm_draw_gui(GmlVM *vm);                     /* Draw GUI (Draw_64) pass */

/* var map */
GmlVal *gml_varmap_get(GmlVarMap *m, const char *key);   /* NULL if absent */
GmlVal *gml_varmap_put(GmlVarMap *m, const char *key);   /* get-or-create slot */

/* read a global scalar / global array element by name (0 if absent). Names are matched by
 * content (not interned ptr) so the frontend can query e.g. "view_xview". */
double  gml_global_num(GmlVM *vm, const char *name);
double  gml_global_arr(GmlVM *vm, const char *name, int idx);

#endif
