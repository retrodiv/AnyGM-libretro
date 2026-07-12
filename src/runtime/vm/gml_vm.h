/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_vm.h — normalized bytecode interpreter + instance/object/event model. */
#ifndef GML_VM_H
#define GML_VM_H
#include "gml_win.h"

/* ---- value ---- */
typedef enum { V_REAL=0, V_STR=1, V_ARR=2, V_UNDEF=3 } GmlValType;
typedef struct { GmlValType t; double d; const char *s; void *arr; } GmlVal;
typedef struct {
  GmlVal *data; int len, cap;
  int is_2d, height2d, row_cap;
  int *row_len;
  int escaped;   /* referenced beyond its creating scope (stored to a global/instance var,
                    a ds structure, or returned) — locals cleanup must not free it */
} GmlArr;
static inline GmlVal vreal(double d){ GmlVal v; v.t=V_REAL; v.d=d; v.s=0; v.arr=0; return v; }
static inline GmlVal vstr(const char *s){ GmlVal v; v.t=V_STR; v.d=0; v.s=s; v.arr=0; return v; }
/* owned = the string is a fresh malloc'd temporary (v.d!=0 marks ownership); the VM frees it when
 * consumed. Literals/references (data.win STRG, rodata, var pointers) use vstr() and are never freed. */
static inline GmlVal vstr_owned(char *s){ GmlVal v; v.t=V_STR; v.d=1; v.s=s; v.arr=0; return v; }
static inline GmlVal vundef(void){ GmlVal v; v.t=V_UNDEF; v.d=0; v.s=0; v.arr=0; return v; }

/* ---- variable map: open-addressing, key = interned name pointer ---- */
typedef struct { const char *key; uint32_t hash; GmlVal val; } GmlVarSlot;
typedef struct { GmlVarSlot *slots; int cap, len; } GmlVarMap;

/* ---- instance ---- */
#define GML_ALARMS 12
typedef struct {
  int     active;      /* slot in use */
  int     marked;      /* pending destroy */
  int     deactivated; /* instance_deactivate_*: exists but skipped by step/draw/collision/queries */
  int     room_owner;
  unsigned char room_dormant, room_was_deactivated;
  uint32_t id;         /* instance id */
  int     obj;         /* object index */
  /* builtin vars */
  double  x, y, xprevious, yprevious, xstart, ystart;
  double  sprite_index, mask_index, image_index, image_speed, image_xscale, image_yscale;
  double  image_angle, image_alpha, image_blend;
  double  depth, visible, solid, persistent;
  double  hspeed, vspeed, direction, speed, gravity, gravity_direction, friction;
  double  alarm[GML_ALARMS];
  /* path following (path_start): path_index=-1 when not on a path */
  double  path_index, path_position, path_positionprevious, path_speed, path_orientation, path_scale;
  double  path_endaction, path_xoff, path_yoff;   /* world anchor for transformed path coords */
  double  path_origin_x, path_origin_y;           /* local pivot: first point for relative paths */
  /* classic GameMaker timeline playback */
  double  timeline_index, timeline_position, timeline_speed, timeline_running, timeline_loop;
  unsigned char mouse_over;  /* transient hover flag for Mouse enter/leave (not serialized) */
  int cg_touch, cg_visit;    /* collision-grid stamps: touched-since-build / visited-this-query (not serialized) */
  int draw_layer_order;      /* transient GMS2 room layer order for equal-depth draw ties */
  unsigned char method_bound;/* transient cache for bound-method structs; rebuilt from vars when absent */
  int method_fci;
  GmlVal method_self;
  GmlVarMap vars;      /* custom instance variables */
} GmlInstance;

/* ---- path (parsed from PATH) ---- */
typedef struct { double x, y, sp, clen; } GmlPathPt;   /* sp = point speed factor, clen = cumulative length */
typedef struct { GmlPathPt *pts; int n; int kind, closed, precision; double len; } GmlPath;

/* ---- controlled classic timeline records (parsed from compiler-authored TMLN) ---- */
typedef struct { int step, code; } GmlTimelineMoment;
typedef struct { const char *name; GmlTimelineMoment *moments; int n, last_step; } GmlTimeline;

/* ---- object (parsed from OBJT) ---- */
typedef struct {
  const char *name;
  int sprite_index, mask_index, parent, depth, visible, solid, persistent;
  int physics_enabled, physics_kinematic;
  double physics_density, physics_area_px;
  int bevents;   /* classic boundary flags: room bits 0..1, outside-view 2..9, intersect-view 10..17 */
  int colself;   /* this object (or an ancestor) owns >=1 Collision_* handler — run_collisions outer filter */
  /* (collision-grid stamps live per-instance, see GmlInstance.cg_*) */
  /* event code: indexed [evtype][slot]; we keep a small dynamic list */
  struct { int evtype, subtype, code; } *events; int n_events;
} GmlObject;

/* ---- VM ---- */
typedef struct { int self_obj, target_obj, code; } GmlColEvent;  /* Collision_<target> handler */
typedef struct { int valid, self_obj, other_obj, handler_obj, target_obj, code; } GmlColPairCache;
typedef struct { int valid, start_obj, handler_obj, code; char suffix[32]; } GmlEventCache;
typedef struct { char *key; GmlVal key_val, val; } GmlDSMapEntry;
/* hidx: lazy open-addressing index over entry[] (built past ~48 entries, rebuilt when hdirty).
 * Runtime-only — never serialized; state load leaves it NULL and the first lookup rebuilds. */
typedef struct { int live; uint32_t id; GmlDSMapEntry *entry; int len, cap;
                 int *hidx; int hcap; int hdirty;
                 int last_lookup; } GmlDSMap;
typedef struct { int live; uint32_t id; GmlVal *item; int len, cap; } GmlDSList;
typedef struct { int live; uint32_t id; GmlVal *cell; int w, h; } GmlDSGrid;   /* row-major w*h cells */
#define GML_DS_MAP_MAX 256
#define GML_DS_LIST_MAX 256
#define GML_DS_GRID_MAX 32

/* runtime layers (GMS2 layer_create / layer_tile_create — the compat scripts GMS emits for
 * upgraded GM8 projects route tile_add/tile_delete through these, so terrain painted at
 * runtime lives here, not in the ROOM chunk). Cleared on room enter like GM tiles. */
typedef struct { int id, used, visible, touched, order, script_begin, script_end; double depth, x, y, hs, vs; char name[32]; } GmlRtLayer;
/* GMS2 tile layer (room layer type 4): a grid of tileset cells. Games read it for tile-based collision
 * (tilemap_get + tilemap_get_cell_*_at_pixel). tiles points INTO the immutable room data (no copy). */
typedef struct { int id, used, visible, order; double x, y, depth; int tileset, tw, th, cols, rows; const unsigned char *tiles, *base_tiles; unsigned char *owned_tiles; char name[32]; } GmlTileMap;
typedef struct { int id, used, layer, type;         /* type: 7=tile, 3=sprite, 1=background (layerelementtype_*) */
  int sprite; double x, y; int sx, sy, w, h;        /* sx/sy/w/h = source region (tiles) */
  double xs, ys, alpha; int visible; uint32_t blend;
  int htiled, vtiled, stretch;                      /* background-element extras */
  char name[64];                                    /* room-authored sprite asset name */
  double image_index, image_speed, image_angle; } GmlRtElem;
#define GML_PHYS_FIXTURE_MAX 128
#define GML_PHYS_JOINT_MAX 256
#define GML_PHYS_FIXTURE_POINTS 16
typedef struct {
  int live, shape, bound_inst, points;
  uint32_t id;
  double density, friction, restitution, lin_damp, ang_damp, awake;
  double radius, w, h, x1, y1, x2, y2;
  double px[GML_PHYS_FIXTURE_POINTS], py[GML_PHYS_FIXTURE_POINTS];
} GmlPhysicsFixture;
typedef struct {
  int live, type, value_count;
  uint32_t id;
  double a, b, x1, y1, x2, y2, params[24];
} GmlPhysicsJoint;

typedef struct GmlVM {
  GmlWin   *win;
  GmlVarMap globals;
  GmlObject *objects; int n_objects;
  int **obj_desc; int *obj_desc_n;   /* lazy per-object descendant lists (hierarchy is static) */
  int *obj_alive;    /* live instances per object INCLUDING descendants (family counts; runtime-only) */
  int *obj_head;     /* per exact object type: first live instance slot (-1 none; runtime-only) */
  int *inst_next, *inst_prev;   /* doubly-linked per-type instance lists over pool slots */
  long obj_list_gen; /* bumped on any create/destroy/change — invalidates per-frame candidate caches */
  GmlPath *paths; int n_paths;
  GmlTimeline *timelines; int n_timelines;
  GmlColEvent *col_events; int n_col_events;
  GmlColPairCache *col_pair_cache; int col_pair_cache_cap;
  GmlEventCache *event_cache; int event_cache_cap;
  GmlInstance *inst; int inst_cap, inst_count;
  int *event_ord; int event_ord_cap; /* reusable classic per-object event-order scratch */
  int      step_alloc_base;   /* while stepping, new instances must not reuse slots in the current frame snapshot */
  uint32_t next_id;
  int      room_index;        /* current room (ROOM index) */
  int      pending_room;      /* -1 none, else target ROOM index (play-order resolved) */
  unsigned char *room_stored; int room_state_count;
  int      game_end;
  int      started;           /* Game Start fired */
  int      gs_roots_run;      /* GMS2.3 GlobalScript root entries executed (once per session) */
  /* collision-candidate grid (built lazily once per frame over all instances; queries take grid
   * candidates + the touched-since-build overlay; see gml_colgrid_* in gml_builtin.c). Runtime
   * only — never serialized; invalidated on room enter / state load. */
  long     cg_built_frame;    /* g_vm_frame of the current build, -1 = invalid */
  int      cg_gen;            /* build generation (touch stamps) */
  int      cg_vgen;           /* per-query visit generation (dedupe stamps) */
  int      cg_cw, cg_ch;      /* grid cells */
  double   cg_cell, cg_ox, cg_oy;
  int     *cg_off;            /* CSR offsets (cw*ch+1) */
  int     *cg_items; int cg_items_cap;
  int     *cg_overlay; int cg_overlay_n, cg_overlay_cap;
  double   last_key;          /* GM keyboard_lastkey: last vk pressed */
  double   window_fullscreen;  /* GM window_get/set_fullscreen: menu state */
  double   window_x, window_y; /* logical window position for window_get/set_position */
  int      window_cursor;      /* GM window_get/set_cursor logical cursor id (-1 hidden) */
  int      action_relative;   /* D&D action_set_relative flag for following action_* calls */
  double   potential_max_rotation, potential_rotate_step, potential_check_distance;
  int      potential_rotate_on_spot;
  int      god_mode;          /* frontend core option; requires GML_GOD_OBJ to name a target family */
  /* execution context */
  GmlInstance *cur_self, *cur_other;
  int32_t call_script_ci;   /* side channel: generic dispatch reports "name resolved to script <ci>" for the caller's per-site cache */
  const char *cur_event; int cur_event_obj;   /* current event suffix + object level (for event_inherited) */
  int      event_type, event_number;          /* transient GM event_type/event_number builtins */
  GmlVal script_args[16]; int script_argc;     /* current script argumentN/argument_count */
  /* RNG: WELL512 (Lomont compact form), MSVC-LCG seed expansion,
   * default seed 0. random(x) = (next()/2^32)*x. See LICENSES/WELL512.txt. */
  uint32_t rng_well[16]; int rng_index;
  uint32_t rng_state;   /* last seed set (randomize/random_set_seed); random_get_seed reads it */
  uint32_t rng_classic_state; /* GM6-8 live linear-generator state */
  /* tile-layer runtime mutations (tile_layer_delete/depth/shift/hide/show), reset on room enter.
   * Pure no-op when n_tile_mut==0 → zero effect on the hot draw path for rooms that
   * never call tile_layer_*; only those that do pay the per-tile lookup. */
  struct { int depth, flags, has_remap, remap; double dx, dy; } tile_mut[64];
  int n_tile_mut;
  /* tile_layer_delete_at: per-tile deletion by position.
   * At draw time, tiles matching (depth, x, y) are dropped. */
  struct { int depth, x, y; } tile_del_at[64]; int n_tile_del_at;
  /* INI persistence: the currently-open .ini as a simple key-value map */
  struct { char *section, *key, *sval; double val; int is_str; } ini_kv[256]; int ini_n, ini_open;
  char ini_path[256];
  /* Small runtime I/O tables for GMS file_bin_* and buffer_* handles. These are transient
   * runtime handles, not serialized into libretro save-states. */
  void *bin_file[16];
  struct { unsigned char *data; int size, cap, pos, live; } buffer[16];
  /* deferred GM async Save/Load events (Other_72): request ids queued by buffer_*_async this
   * step, fired at end-of-step so the caller's `loadid = buffer_load_async(...)` assignment has
   * landed before the handler compares async_load[?"id"] against it. Drained every step. */
  int async_sl_q[16]; int async_sl_status[16]; int n_async_sl; int async_seq;
  int async_group_active, async_group_id, async_group_status, async_group_count;
  /* deferred GM Async HTTP events (Other_62): the libretro core has no network, so every
   * http_get/http_post/http_request returns a fresh id and fires a failed response next step
   * ({id, status:-1, http_status:0, result:""}). Games with online features (leaderboards)
   * take their offline/error path instead of waiting forever. */
  int async_http_q[16]; int n_async_http;
  int room_rec_stride;   /* room instance record size (36/40/48), detected lazily */
  /* presentation: runtime window size (window_set_size) and GUI canvas (display_set_gui_size).
   * 0 = unset -> window falls back to GEN8 disp, GUI falls back to the window. */
  int window_w, window_h, gui_w, gui_h;
  long room_enter_frame;      /* g_vm_frame at room entry (GMS2 layer scroll phase) */
  int layer_data_off;         /* GMS2 layer type-data offset (36, or 48 with effect fields); 0=undetected */
  int next_buffer_id;
  /* keyboard events (Keyboard_N held / KeyPress_N / KeyRelease_N): unique suffixes present in
   * the game's CODE names, fired each step against the key state (event-driven input games). */
  struct { char suffix[24]; int vk, kind; } key_events[64];
  int n_key_events;
  /* runtime layer/element pools (layer_* builtins); compact arrays, freed slots reused */
  GmlRtLayer *rtl; int n_rtl, cap_rtl;
  GmlRtElem  *rte; int n_rte, cap_rte;
  int rt_next_id;
  /* room_set_viewport overrides: like GMS, they modify the room's viewport config and take
   * effect on (re)entry. Lazily allocated [n_rooms*8]; .set marks an active override. */
  struct GmlViewOvr { unsigned char set, vis; int x, y, w, h; } *view_ovr;
  int n_view_ovr;   /* allocated entries (rooms*8), 0 = table absent */
  GmlDSMap ds_map[GML_DS_MAP_MAX];
  GmlDSList ds_list[GML_DS_LIST_MAX];
  GmlDSGrid ds_grid[GML_DS_GRID_MAX];
  int next_ds_id;
  int ds_map_last_slot;       /* transient slot-id cache for repeated DS-map ops */
  int ds_list_compat_repair;  /* old save-states did not serialize ds_list payloads */
#define GML_MAX_EMITTERS 32
  unsigned char emitter_live[GML_MAX_EMITTERS];   /* audio emitters = gain cells (ids 3000000+i) */
  double emitter_gain[GML_MAX_EMITTERS];
  /* instance Mouse_<n> events present in CODE (parse_mouse_events); dispatched per step */
  struct GmlMouseEvent { int sub; char suffix[20]; } mouse_events[32];
  int n_mouse_events;
  int draw_events_off;   /* draw_enable_drawevent(false): skip all instance drawing */
  void (*draw_event_hook)(struct GmlVM *vm, GmlInstance *in, const char *suffix, int begin, void *user);
  void *draw_event_hook_user;  /* frontend-only transient hook around individual draw events */
  int *draw_ord; int draw_ord_cap;  /* scratch order buffer for draw passes (runtime-only) */
  void    *render;   /* GmlRender* (set by the frontend) for draw_* builtins */
  void    *audio;    /* GmlAudio*  (set by the frontend) for audio_* builtins */
  unsigned char *audio_room_warm_scan; int audio_room_warm_scan_n;
  /* GMS2.3 struct pool: standalone GmlInstances (malloc'd individually so the growable pointer
   * array can realloc without dangling them). Field access resolves struct ids here via vm_inst_from_ref. */
  GmlInstance **structs; int n_structs, cap_structs; unsigned next_struct_id;
  unsigned char *struct_gen;                      /* per-slot generation (id = BASE|gen<<20|slot); a stale id to a
                                                   * reused slot fails the gen check → NULL, not a wrong struct. */
  int *struct_free, n_struct_free, cap_struct_free;  /* GC free-list: reclaimed struct slots for reuse (bounded pool) */
  long structs_last_gc_frame;
  GmlTileMap *tilemaps; int n_tilemaps, cap_tilemaps, next_tilemap_id;  /* per-room GMS2 tile layers (collision) */
  GmlPhysicsFixture phys_fixture[GML_PHYS_FIXTURE_MAX];
  GmlPhysicsJoint phys_joint[GML_PHYS_JOINT_MAX];
  uint32_t phys_next_id;
  double phys_gravity_x, phys_gravity_y, phys_update_speed;
  int phys_update_iterations, phys_paused, phys_debug_draw;
} GmlVM;
GmlTileMap *gml_tilemap_find(GmlVM *vm, int id);
int gml_tilemap_set_cell(GmlTileMap *tm, int cx, int cy, uint32_t datum);
void gml_tilemap_effective(GmlVM *vm, const GmlTileMap *tm,
                           double *x, double *y, double *depth, int *visible);
int gml_room_layer_data_off(GmlVM *vm);
uint32_t gml_room_layer_type_off(GmlVM *vm, uint32_t lp);  /* per-layer type-data offset with optional effect fields */
void gml_struct_gc(GmlVM *vm);

void gml_arr_mark_escaped(GmlVal v);   /* array stored beyond its scope: locals cleanup must not free it */
void gml_path_eval_public(GmlVM *vm, int pi, double t, double *ox, double *oy);
GmlRtLayer *gml_rt_layer_find(GmlVM *vm, int id);
GmlRtLayer *gml_rt_layer_find_by_name(GmlVM *vm, const char *nm);
GmlRtLayer *gml_rt_layer_new(GmlVM *vm);
GmlRtElem  *gml_rt_elem_find(GmlVM *vm, int id);
GmlRtElem  *gml_rt_elem_new(GmlVM *vm);

int     gml_vm_init(GmlVM *vm, GmlWin *win);
void    gml_vm_free(GmlVM *vm);
void    gml_rng_seed(GmlVM *vm, uint32_t seed);   /* WELL512 seeding (MSVC LCG expand) */
double  gml_rng_value(GmlVM *vm);                 /* next()/2^32 -> [0,1) */
GmlVal  gml_vm_run_code(GmlVM *vm, int code_index, GmlInstance *self, GmlInstance *other,
                        GmlVal *args, int n_args);
int     gml_code_index_by_name(GmlWin *win, const char *name);  /* exact */
int     gml_code_index_find(GmlWin *win, const char *substr);   /* first containing */

/* instances / events / rooms */
GmlInstance *gml_instance_create(GmlVM *vm, double x, double y, int obj);  /* runs Create */
GmlInstance *gml_instance_create_depth(GmlVM *vm, double x, double y, int obj, int have_depth, double depth);  /* sets depth BEFORE Create */
GmlInstance *gml_struct_new(GmlVM *vm);              /* GMS2.3: allocate a struct (standalone instance) */
GmlInstance *gml_struct_find(GmlVM *vm, unsigned id);
void         gml_instance_change(GmlVM *vm, GmlInstance *in, int obj, int perform_events);
void         gml_path_start(GmlVM *vm, GmlInstance *in, int path, double speed, double endaction, int absolute);
/* tile-layer runtime mutations (tile_layer_* builtins) on the current room's tiles */
void         gml_tile_layer_delete(GmlVM *vm, int depth);
void         gml_tile_layer_depth(GmlVM *vm, int depth, int newdepth);
void         gml_tile_layer_shift(GmlVM *vm, int depth, double dx, double dy);
void         gml_tile_layer_delete_at(GmlVM *vm, int depth, double x, double y);
void         gml_tile_layer_hide(GmlVM *vm, int depth, int hidden);
int          gml_event_inherited(GmlVM *vm);   /* run the current event on the parent object */
double       gml_inst_var_get(GmlVM *vm, GmlInstance *in, const char *name); /* read a builtin or custom var */
int          gml_inst_var_exists(GmlVM *vm, GmlVal ref, const char *name);
GmlVal       gml_inst_var_get_val(GmlVM *vm, GmlVal ref, const char *name, int *ok);
int          gml_inst_var_set_val(GmlVM *vm, GmlVal ref, const char *name, GmlVal v);
int          gml_val_array_length(GmlVal v);   /* array_length_1d: logical length of a V_ARR value, else 0 */
int          gml_val_array_height_2d(GmlVal v);
int          gml_val_array_length_2d(GmlVal v, int row);
/* GMS2.3 array-function forms (array_create/get/set/push/pop/resize/copy) — operate on V_ARR values */
GmlVal       gml_arr_store_clone(GmlVal v); /* own strings / mark-escape arrays before storing in an array */
GmlVal       gml_arr_new(int size, GmlVal fill);
void         gml_arr_set(GmlVal arr, int idx, GmlVal val);
GmlVal       gml_arr_get(GmlVal arr, int idx);
void         gml_arr_push(GmlVal arr, GmlVal val);
GmlVal       gml_arr_pop(GmlVal arr);
void         gml_arr_resize(GmlVal arr, int size);
void         gml_arr_copy(GmlVal dst, int di, GmlVal src, int si, int count);
GmlVal       gml_ds_map_find_value_direct(GmlVM *vm, int id, GmlVal keyv, int has_key);
GmlVal       gml_ds_map_find_first_direct(GmlVM *vm, int id);
GmlVal       gml_ds_map_find_next_direct(GmlVM *vm, int id, GmlVal keyv, int has_key);
int          gml_ds_map_size_direct(GmlVM *vm, int id);
void         gml_gamepad_set_axis_deadzone_direct(int device, double dz);
void         gml_instance_destroy(GmlVM *vm, GmlInstance *in);
int          gml_instance_number(GmlVM *vm, int target); /* object index, special scope, or real instance id */
int          gml_object_is(GmlVM *vm, int obj, int target);  /* obj == target or descends from it */
int          gml_object_index_by_name(GmlVM *vm, const char *name);  /* -1 if not found */
GmlInstance *gml_find_instance(GmlVM *vm, int obj);          /* first active instance of obj (or child) */
int          gml_run_event(GmlVM *vm, GmlInstance *in, const char *suffix); /* e.g. "Create_0" */
/* collision-candidate grid hooks (gml_builtin.c): touch = a bbox input (x/y/scale/angle/
 * sprite/mask) of `in` was written after the current build; invalidate = drop the build. */
void         gml_colgrid_touch(GmlInstance *in);
void         gml_colgrid_invalidate(GmlVM *vm);
int          gml_colgrid_mode(void);   /* 0 linear (GML_NO_COLGRID), 1 grid, 2 grid+verify */
void         gml_obj_alive_adjust(GmlVM *vm, int obj, int delta);   /* family live-count maintenance */
void         gml_obj_alive_recount(GmlVM *vm);                       /* rebuild from the pool (state load) */
int          gml_colgrid_collect(GmlVM *vm, double l, double t, double r, double b, int **out);
void         gml_fire_gamepad_connected(GmlVM *vm);
void gml_fire_async_saveload(GmlVM *vm);  /* drain queued Other_72 (async save/load) events */ /* dispatch GM gamepad-discovered async event */
void gml_fire_async_http(GmlVM *vm);      /* drain queued Other_62 (async HTTP, always-fail offline) events */
void         gml_room_enter(GmlVM *vm, int room_index);      /* instantiate + Create events */
void         gml_vm_goto_room_order(GmlVM *vm, int order_index);
void         gml_vm_warm_audio_for_room(GmlVM *vm, int room_index);
/* Apply one generic cheat line ("room=N", "name=V", "name[i]=V"). Returns 1 if it is a
 * sticky global write the caller should re-apply each frame (freeze), 0 for a one-shot/no-op. */
int          gml_cheat_apply(GmlVM *vm, const char *code);
void         gml_vm_step(GmlVM *vm);                         /* one frame of the game loop */
void         gml_vm_draw(GmlVM *vm);                         /* draw phase (needs vm->render) */
void         gml_vm_draw_gui(GmlVM *vm);                     /* Draw GUI (Draw_64) pass */

/* var map */
/* GMS2.3 function-value encoding: tag in the high bits + CODE-entry index in the low 24.
 * Shared by the VM (push/OP_CALLV) and builtins that accept script references (script_execute). */
#define GML_FUNCVAL_TAG 0x40000000
#define GML_IS_FUNCVAL(iv) (((uint32_t)(iv) & 0x7F000000u) == (uint32_t)GML_FUNCVAL_TAG)
/* GMS2.3 structs: a struct value is the id of a standalone GmlInstance kept in a SEPARATE pool
 * (never in the room instance array, so it is not stepped/drawn/counted). Ids start high enough to
 * never collide with real instance ids (~1e5..1e6) or funcvals (0x40xxxxxx). A bound method is a
 * struct carrying "__fn"/"__self" fields. Base bit pattern 0x50xxxxxx is not a funcval (0x40xxxxxx). */
#define GML_STRUCT_ID_BASE 0x50000000u
#define GML_IS_STRUCT_ID(v) ((v) >= (double)GML_STRUCT_ID_BASE && (v) < (double)(GML_STRUCT_ID_BASE+0x08000000u))

GmlVal *gml_varmap_get(GmlVarMap *m, const char *key);   /* NULL if absent */
GmlVal *gml_varmap_put(GmlVarMap *m, const char *key);   /* get-or-create slot */

/* read a global scalar / global array element by name (0 if absent). Names are matched by
 * content (not interned ptr) so the frontend can query e.g. "view_xview". */
double  gml_global_num(GmlVM *vm, const char *name);
double  gml_global_arr(GmlVM *vm, const char *name, int idx);
double  gml_room_speed(GmlVM *vm);
void    gml_set_global_arr(GmlVM *vm, const char *name, int idx, double val);

/* save-state payload for the VM runtime only. Static data parsed from data.win is not included. */
size_t  gml_vm_state_size(GmlVM *vm);
void    gml_vm_draw_pass(GmlVM *vm, const char *suffix);   /* Draw_72/73/74/75/65/66 stage events */
int     gml_vm_state_save(GmlVM *vm, void *data, size_t len, size_t *written);
int     gml_vm_state_load(GmlVM *vm, const void *data, size_t len, size_t *used);

/* Software D3 state is serialized with the VM so rewind/load cannot change the
 * active projection, culling, lighting, or light definitions mid-frame. */
#define GML_D3_STATE_FLAG_COUNT 26
#define GML_D3_STATE_VALUE_COUNT 584
#define GML_D3_STATE_COLOR_COUNT 10
void    gml_d3_reset(void);
void    gml_d3_state_get(int flags[GML_D3_STATE_FLAG_COUNT],
                         double values[GML_D3_STATE_VALUE_COUNT],
                         uint32_t colors[GML_D3_STATE_COLOR_COUNT]);
void    gml_d3_state_set(const int flags[GML_D3_STATE_FLAG_COUNT],
                         const double values[GML_D3_STATE_VALUE_COUNT],
                         const uint32_t colors[GML_D3_STATE_COLOR_COUNT]);
size_t  gml_d3_models_state_size(void);
int     gml_d3_models_state_save(void *data, size_t capacity);
int     gml_d3_models_state_load(const void *data, size_t size);

#endif
