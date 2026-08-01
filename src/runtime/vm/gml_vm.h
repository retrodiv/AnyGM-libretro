/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_vm.h — normalized bytecode interpreter + instance/object/event model. */
#ifndef GML_VM_H
#define GML_VM_H
#include "anygm.h"
#include "gml_value.h"
#include "gml_win.h"
#include "gml_software3d.h"

struct GmlClassicDispatchCache;

/* ---- instance ---- */
#define GML_ALARMS 12
#define GML_ROOM_CAMERA_COUNT 8
#define GML_CAMERA_LIMIT 64
typedef struct {
  int     active;      /* slot in use */
  int     marked;      /* pending destroy */
  int     deactivated; /* instance_deactivate_*: exists but skipped by step/draw/collision/queries */
  int     room_owner;
  unsigned char room_dormant, room_was_deactivated, room_placed;
  uint32_t id;         /* instance id */
  uint64_t creation_seq; /* stable insertion order even when a dead pool slot is recycled */
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
  int draw_layer_element_order; /* authored position inside a ROOM instance layer, or -1 */
  unsigned char method_bound;/* transient cache for bound-method structs; rebuilt from vars when absent */
  int method_fci;
  GmlVal method_self;
  GmlVarMap vars;      /* custom instance variables */
} GmlInstance;

/* ---- path (parsed from PATH) ---- */
typedef struct { double x, y, sp, clen; } GmlPathPt;   /* sp = point speed factor, clen = cumulative length */
typedef struct { GmlPathPt *pts; int n; int kind, closed, precision; double len; } GmlPath;

/* ---- timelines (parsed from native package and compiler-authored TMLN records) ---- */
typedef struct { int step, code; } GmlTimelineMoment;
typedef struct {
  const char *name;
  char *owned_name;                 /* non-NULL only for timeline_add() assets */
  GmlTimelineMoment *moments;
  int n, last_step;
  unsigned generation;             /* invalidates playback snapshots after a runtime clear */
} GmlTimeline;

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
typedef struct {
  int obj;
  long frame, generation;
  int *slots;
  int count, capacity;
} GmlCollisionCandidateCache;
/* runtime layers (GMS2 layer_create / layer_tile_create — the compat scripts GMS emits for
 * upgraded GM8 projects route tile_add/tile_delete through these, so terrain painted at
 * runtime lives here, not in the ROOM chunk). Cleared on room enter like GM tiles. */
typedef struct { int id, used, visible, touched, order, script_begin, script_end; double depth, x, y, hs, vs; char name[32]; } GmlRtLayer;
/* GMS2 tile layer (room layer type 4): a grid of tileset cells. Games read it for tile-based collision
 * (tilemap_get + tilemap_get_cell_*_at_pixel). tiles points INTO the immutable room data (no copy). */
typedef struct { int id, used, visible, order; double x, y, depth; int tileset, tw, th, cols, rows; const unsigned char *tiles, *base_tiles; unsigned char *owned_tiles, *decoded_tiles; char name[32]; } GmlTileMap;
typedef struct { int id, used, layer, type;         /* type: 7=tile, 3=sprite, 1=background (layerelementtype_*) */
  int sprite; double x, y; int sx, sy, w, h;        /* sx/sy/w/h = source region (tiles) */
  double xs, ys, alpha; int visible; uint32_t blend;
  int htiled, vtiled, stretch;                      /* background-element extras */
  char name[64];                                    /* room-authored sprite asset name */
  double image_index, image_speed, image_angle; } GmlRtElem;
/* Per-instance input bridge. Runtime code receives a complete VM context and
 * never reaches process-global host callbacks. Hosts may leave callbacks
 * unset; the neutral defaults report no input and no connected devices. */
typedef struct {
  void *userdata;
  int (*key)(void *userdata,int key,int edge);
  void (*key_clear)(void *userdata,int key);
  void (*key_press)(void *userdata,int key);
  void (*key_release)(void *userdata,int key);
  int (*gamepad)(void *userdata,int button,int edge);
  int (*gamepad_connected)(void *userdata,int device);
  int (*gamepad_device_count)(void *userdata);
  double (*gamepad_axis)(void *userdata,int device,int axis);
  void (*gamepad_vibration)(void *userdata,int device,double low,double high);
  void (*mouse)(void *userdata,double *room_x,double *room_y,double *gui_x,double *gui_y,
                double *window_x,double *window_y,int *held,int *pressed,int *released,int *wheel);
  void (*mouse_set)(void *userdata,double x,double y);
} GmlInputServices;

typedef struct {
  int object;
  char suffix[24];
  double milliseconds;
  long runs;
} GmlVmEventProfileEntry;

typedef struct {
  int hot_builtin_profile;
  int watchdog_warnings;
  int pc_initialized;
  char pc_name[128];
  long pc_offset;
  int pc_max_logs;
  int pc_log_count;
  int function_value_debug;
  int push_reference_debug;
  int event_filter_initialized;
  char event_filter[128];
  int event_time_enabled;
  GmlVmEventProfileEntry event_profile[256];
  int event_profile_count;
  long event_profile_last_frame;
  long event_profile_frames;
  int instance_pool_warned;
  int audio_room_warm_disabled;
  int audio_room_warm_debug;
  int god_initialized;
  int god_enabled;
  int god_alarm;
  int god_value;
  char god_object[128];
  int follow_log_count;
  int follow_target_log_count;
  int camera_follow_log_count;
  int instance_draw_dumped;
  int collision_candidate_debug;
  long collision_candidate_last_frame;
  int collision_line_setting_initialized;
  char collision_line_setting[16];
  int collision_event_time_enabled;
  int rng_call_logging;
  int big_array_log_count;
  int state_variable_debug;
  int state_variable_load_debug;
  size_t state_profile_last_total;
  int state_time_log_count;
} GmlVmDiagnostics;

typedef struct GmlVM {
  GmlWin   *win;
  const AnygmHostServices *host;
  GmlInputServices input;
  struct GmlParticleState *particles;
  struct GmlBuiltinState *builtins;
  GmlSoftware3D *software3d;
  GmlVmDiagnostics diagnostics;
  GmlVarMap globals;
  /* Function-static storage. Each CODE entry owns one persistent scope and the
   * initialization latch driven by the isstaticok/setstatic bytecode pair. */
  GmlVarMap *code_static; unsigned char *code_static_init; int code_static_count;
  GmlVarSlot **state_sort_slots; int state_sort_slots_capacity;
  GmlObject *objects; int n_objects;
  int **obj_desc; int *obj_desc_n;   /* lazy per-object descendant lists; reset after hierarchy changes */
  int *obj_alive;    /* live instances per object INCLUDING descendants (family counts; runtime-only) */
  int *obj_head;     /* per exact object type: first live instance slot (-1 none; runtime-only) */
  int *inst_next, *inst_prev;   /* doubly-linked per-type instance lists over pool slots */
  long obj_list_gen; /* bumped on any create/destroy/change — invalidates per-frame candidate caches */
  GmlPath *paths; int n_paths;
  GmlTimeline *timelines; int n_timelines, cap_timelines;
  GmlColEvent *col_events; int n_col_events;
  GmlColPairCache *col_pair_cache; int col_pair_cache_cap;
  GmlEventCache *event_cache; int event_cache_cap;
  GmlCollisionCandidateCache collision_candidate[48];
  uint64_t *collision_candidate_bits; int collision_candidate_bits_words;
  GmlInstance *inst; int inst_cap, inst_count;
  int *event_ord; int event_ord_cap; /* reusable classic per-object event-order scratch */
  struct GmlClassicDispatchCache *classic_dispatch;
  int classic_dispatch_empty;
  int      step_alloc_base;   /* frame-start pool extent; zero outside a normal step/while entering a room */
  int     *step_free; int step_free_n, step_free_pos, step_free_cap;
  uint32_t step_first_id;     /* Studio fixed snapshot: ids at/above this were created during this step */
  int      step_active;       /* lets alloc reuse only holes that were already free at frame start */
  int      execution_depth;   /* per-VM recursion guard for nested script calls */
  uint32_t *special_var_hash; /* per-VM acceleration table for builtin-variable lookup */
  uint64_t special_var_bloom;
  uint32_t next_id;
  uint64_t next_creation_seq;
  long     frame;             /* simulation frame owned by this VM */
  long     time_sample_frame; /* frame used as the deterministic base for timer builtins */
  double   time_sample_cpu_ms;
  uint64_t fallback_monotonic_ns;
  int      room_index;        /* current room (ROOM index) */
  int      pending_room;      /* -1 none, else target ROOM index (play-order resolved) */
  unsigned char *room_stored; int room_state_count;
  int      game_end;
  int      classic_info_active; /* modal classic game-information page */
  int      started;           /* Game Start fired */
  int      gs_roots_run;      /* GMS2.3 GlobalScript root entries executed (once per session) */
  /* collision-candidate grid (built lazily once per frame over all instances; queries take grid
   * candidates + the touched-since-build overlay; see gml_colgrid_* in
   * gml_builtin_collision.c). Runtime only — never serialized; invalidated on room enter / state
   * load. */
  long     cg_built_frame;    /* frame of the current build, -1 = invalid */
  int      cg_gen;            /* build generation (touch stamps) */
  int      cg_vgen;           /* per-query visit generation (dedupe stamps) */
  int      cg_cw, cg_ch;      /* grid cells */
  double   cg_cell, cg_ox, cg_oy;
  int     *cg_off; int cg_off_cap; /* CSR offsets (cw*ch+1) */
  int     *cg_items; int cg_items_cap;
  int     *cg_overlay; int cg_overlay_n, cg_overlay_cap;
  int     *cg_candidate; int cg_candidate_cap;
  uint64_t *cg_candidate_bits; int cg_candidate_bits_words;
  double   last_key;          /* GM keyboard_lastkey: last vk pressed */
  /* Physical virtual-key -> logical virtual-key remapping. Values are -1 (disabled)
   * or a Windows VK code; the default mapping is the identity. */
  int16_t  key_map[256];
  double   window_fullscreen;  /* GM window_get/set_fullscreen: menu state */
  double   window_x, window_y; /* logical window position for window_get/set_position */
  int      window_cursor;      /* GM window_get/set_cursor logical cursor id (-1 hidden) */
  /* Frontend locale. os_get_language/region use the ISO components while platform
   * extensions that expose a desired language use the BCP-47-style tag. */
  char     os_language[8], os_region[8], language_tag[16];
  char     working_directory[560], program_directory[560];
  int      action_relative;   /* D&D action_set_relative flag for following action_* calls */
  double   math_epsilon;      /* real-comparison tolerance (math_set/get_epsilon) */
  double   potential_max_rotation, potential_rotate_step, potential_check_distance;
  int      potential_rotate_on_spot;
  int      god_mode;          /* host core option; requires GML_GOD_OBJ to name a target family */
  /* execution context */
  GmlInstance *cur_self, *cur_other;
  int      cur_code_index;   /* current CODE entry, so IT_STATIC resolves across nested calls */
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
  int room_rec_stride;   /* room instance record size (36/40/48), detected lazily */
  /* presentation: runtime window size (window_set_size), GUI canvas (display_set_gui_size), and
   * the optional screen-relative GUI transform (display_set_gui_maximise).  A zero maximise
   * scale means automatic fit; inactive restores the application-surface-relative default. */
  int window_w, window_h, gui_w, gui_h;
  int gui_maximise_active;
  double gui_maximise_xscale, gui_maximise_yscale;
  double gui_maximise_xoffset, gui_maximise_yoffset;
  long room_enter_frame;      /* simulation frame at room entry (layer scroll phase) */
  int layer_data_off;         /* GMS2 layer type-data offset with optional effect fields; 0=undetected */
  /* keyboard events (Keyboard_N held / KeyPress_N / KeyRelease_N): unique suffixes present in
   * the game's CODE names, fired each step against the key state (event-driven input games). */
  struct { char suffix[24]; int vk, kind; } key_events[64];
  int n_key_events;
  /* runtime layer/element pools (layer_* builtins); compact arrays, freed slots reused */
  GmlRtLayer *rtl; int n_rtl, cap_rtl;
  GmlRtElem  *rte; int n_rte, cap_rte;
  int rt_next_id;
  /* Runtime room-view edits take effect on re-entry. This covers viewport-only and full-record
   * APIs. Lazily allocate n_rooms*8 entries; .set marks a port override and .full the full view. */
  struct GmlViewOvr {
    unsigned char set, full, vis, room_enabled_set, room_enabled;
    int x, y, w, h;
    int view_x, view_y, view_w, view_h;
    int hborder, vborder, hspeed, vspeed, object;
  } *view_ovr;
  int n_view_ovr;   /* allocated entries (rooms*8), 0 = table absent */
  /* instance Mouse_<n> events present in CODE (parse_mouse_events); dispatched per step */
  struct GmlMouseEvent { int sub; char suffix[20]; } mouse_events[32];
  int n_mouse_events;
  int draw_events_off;   /* draw_enable_drawevent(false): skip all instance drawing */
  void (*draw_event_hook)(struct GmlVM *vm, GmlInstance *in, const char *suffix, int begin, void *user);
  void *draw_event_hook_user;  /* host-only transient hook around individual draw events */
  int *draw_ord; int draw_ord_cap;  /* scratch order buffer for draw passes (runtime-only) */
  void    *render;   /* GmlRender* (set by the engine coordinator) for draw_* builtins */
  void    *audio;    /* GmlAudio*  (set by the engine coordinator) for audio_* builtins */
  struct GmlDrawScratch *draw_scratch;
  unsigned char *audio_room_warm_scan; int audio_room_warm_scan_n;
  /* GMS2.3 struct pool: standalone GmlInstances (malloc'd individually so the growable pointer
   * array can realloc without dangling them). Field access resolves struct ids here via vm_inst_from_ref. */
  GmlInstance **structs; int n_structs, cap_structs; unsigned next_struct_id;
  unsigned char *struct_gen;                      /* per-slot generation (id = BASE|gen<<20|slot); a stale id to a
                                                   * reused slot fails the gen check → NULL, not a wrong struct. */
  int *struct_free, n_struct_free, cap_struct_free;  /* GC free-list: reclaimed struct slots for reuse (bounded pool) */
  long structs_last_gc_frame;
  GmlTileMap *tilemaps; int n_tilemaps, cap_tilemaps, next_tilemap_id;  /* per-room GMS2 tile layers (collision) */
} GmlVM;
GmlTileMap *gml_tilemap_find(GmlVM *vm, int id);
int gml_tilemap_set_cell(GmlTileMap *tm, int cx, int cy, uint32_t datum);
void gml_tilemap_effective(GmlVM *vm, const GmlTileMap *tm,
                           double *x, double *y, double *depth, int *visible);
int gml_room_layer_data_off(GmlVM *vm);
uint32_t gml_room_layer_type_off(GmlVM *vm, uint32_t lp);  /* per-layer type-data offset with effect fields */
void gml_struct_gc(GmlVM *vm);
void gml_path_eval_public(GmlVM *vm, int pi, double t, double *ox, double *oy);
double gml_legacy_view_follow_axis(double current, double target, double extent,
                                   double border, double speed);
int gml_room_instance_precreate_code(GmlVM *vm, uint32_t instance_record);
GmlRtLayer *gml_rt_layer_find(GmlVM *vm, int id);
GmlRtLayer *gml_rt_layer_find_by_name(GmlVM *vm, const char *nm);
GmlRtLayer *gml_rt_layer_new(GmlVM *vm);
GmlRtElem  *gml_rt_elem_find(GmlVM *vm, int id);
GmlRtElem  *gml_rt_elem_new(GmlVM *vm);

int     gml_vm_init(GmlVM *vm, GmlWin *win,const AnygmHostServices *host);
void    gml_vm_free(GmlVM *vm);
int     gml_keyboard_check(GmlVM *vm, int vk, int edge); /* remapped keyboard_check* semantics */
int     gml_keyboard_get_map(GmlVM *vm, int source);
void    gml_keyboard_set_map(GmlVM *vm, int source, int destination);
void    gml_keyboard_unset_map(GmlVM *vm);
void    gml_keyboard_clear(GmlVM *vm, int logical_vk);
int     gml_input_key(GmlVM *vm,int key,int edge);
void    gml_input_key_clear(GmlVM *vm,int key);
void    gml_input_key_press(GmlVM *vm,int key);
void    gml_input_key_release(GmlVM *vm,int key);
int     gml_input_gamepad(GmlVM *vm,int button,int edge);
int     gml_input_gamepad_connected(GmlVM *vm,int device);
int     gml_input_gamepad_device_count(GmlVM *vm);
double  gml_input_gamepad_axis(GmlVM *vm,int device,int axis);
void    gml_input_gamepad_set_vibration(GmlVM *vm,int device,double low,double high);
uint64_t gml_host_monotonic_time_ns(GmlVM *vm);
AnygmResult gml_host_wall_time(GmlVM *vm,AnygmWallTime *time);
uint64_t gml_host_random_seed(GmlVM *vm);
double  gml_vm_get_timer_us(GmlVM *vm);
void    gml_input_mouse(GmlVM *vm,double *room_x,double *room_y,double *gui_x,double *gui_y,
                        double *window_x,double *window_y,int *held,int *pressed,int *released,int *wheel);
void    gml_input_mouse_set(GmlVM *vm,double x,double y);
void    gml_rng_seed(GmlVM *vm, uint32_t seed);   /* WELL512 seeding (MSVC LCG expand) */
double  gml_rng_value(GmlVM *vm);                 /* next()/2^32 -> [0,1) */
uint64_t gml_rng_select(GmlVM *vm, uint64_t count);
uint64_t gml_rng_integer(GmlVM *vm, uint64_t inclusive_max);
int     gml_real_compare(double lhs, double rhs, int cmp, int classic);
int     gml_real_compare_epsilon(double lhs, double rhs, int cmp, double epsilon);
GmlVal  gml_vm_run_code(GmlVM *vm, int code_index, GmlInstance *self, GmlInstance *other,
                        GmlVal *args, int n_args);
/* Invoke a function value or bound method using the same receiver rules as OP_CALLV.
 * Builtins with callback arguments use this instead of discarding a method's bound self. */
GmlVal  gml_vm_call_callable(GmlVM *vm, GmlVal callable, GmlVal *args, int n_args);
GmlVal  gml_vm_call_member_callable(GmlVM *vm, GmlVal receiver,
                                    GmlVal callable, GmlVal *args,
                                    int n_args);
void    gml_time_sources_tick(GmlVM *vm);          /* between Begin Step and normal Step */
int     gml_code_index_by_name(GmlWin *win, const char *name);  /* exact */
int     gml_code_index_find(GmlWin *win, const char *substr);   /* first containing */

/* instances / events / rooms */
GmlInstance *gml_instance_create(GmlVM *vm, double x, double y, int obj);  /* runs Create */
GmlInstance *gml_instance_create_depth(GmlVM *vm, double x, double y, int obj, int have_depth, double depth);  /* sets depth BEFORE Create */
GmlInstance *gml_instance_create_layer(GmlVM *vm, double x, double y, int obj, int layer_id);  /* applies layer depth/order BEFORE Create */
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
GmlTileMap  *gml_tilemap_by_layer(GmlVM *vm, GmlVal layer);
int          gml_event_inherited(GmlVM *vm);   /* run the current event on the parent object */
double       gml_inst_var_get(GmlVM *vm, GmlInstance *in, const char *name); /* read a builtin or custom var */
int          gml_inst_var_exists(GmlVM *vm, GmlVal ref, const char *name);
GmlVal       gml_inst_var_get_val(GmlVM *vm, GmlVal ref, const char *name, int *ok);
int          gml_inst_var_set_val(GmlVM *vm, GmlVal ref, const char *name, GmlVal v);
GmlVal       gml_ds_map_find_value_direct(GmlVM *vm, int id, GmlVal keyv, int has_key);
GmlVal       gml_ds_map_find_first_direct(GmlVM *vm, int id);
GmlVal       gml_ds_map_find_next_direct(GmlVM *vm, int id, GmlVal keyv, int has_key);
int          gml_ds_map_size_direct(GmlVM *vm, int id);
void         gml_gamepad_set_axis_deadzone_direct(GmlVM *vm, int device, double dz);
void         gml_instance_destroy(GmlVM *vm, GmlInstance *in);
int          gml_instance_number(GmlVM *vm, int target); /* object index, special scope, or real instance id */
int          gml_object_is(GmlVM *vm, int obj, int target);  /* obj == target or descends from it */
int          gml_object_set_parent(GmlVM *vm, int obj, int parent); /* cycle-safe runtime hierarchy mutation */
int          gml_object_index_by_name(GmlVM *vm, const char *name);  /* -1 if not found */
GmlInstance *gml_find_instance(GmlVM *vm, int obj);          /* first active instance of obj (or child) */
int          gml_run_event(GmlVM *vm, GmlInstance *in, const char *suffix); /* e.g. "Create_0" */
int          gml_timeline_add(GmlVM *vm);                    /* append an empty runtime timeline */
void         gml_timeline_clear(GmlVM *vm, int timeline);    /* remove every moment, retaining the asset */
/* Collision-candidate grid hooks (gml_builtin_collision.c): touch = a bbox input
 * (x/y/scale/angle/sprite/mask) of `in` was written after the current build;
 * invalidate = drop the build. The cached mode policy remains in gml_builtin.c. */
void         gml_colgrid_touch(GmlVM *vm, GmlInstance *in);
void         gml_colgrid_invalidate(GmlVM *vm);
int          gml_colgrid_mode(GmlVM *vm);   /* 0 linear, 1 grid, 2 grid plus verification */
void         gml_obj_alive_adjust(GmlVM *vm, int obj, int delta);   /* family live-count maintenance */
void         gml_obj_alive_recount(GmlVM *vm);                       /* rebuild from the pool (state load) */
int          gml_colgrid_collect(GmlVM *vm, double l, double t, double r, double b, int **out);
void         gml_fire_gamepad_connected(GmlVM *vm);
void gml_fire_async_saveload(GmlVM *vm);  /* drain queued Other_72 (async save/load) events */ /* dispatch GM gamepad-discovered async event */
void gml_fire_async_http(GmlVM *vm);      /* drain queued Other_62 (async HTTP, always-fail offline) events */
void         gml_room_enter(GmlVM *vm, int room_index);      /* instantiate + Create events */
int          gml_vm_room_get(GmlVM *vm, int room_index, GmlRoom *out);
int          gml_vm_room_set_dimension(GmlVM *vm, int room_index, int height, double value);
void         gml_vm_goto_room_order(GmlVM *vm, int order_index);
void         gml_vm_warm_audio_for_room(GmlVM *vm, int room_index);
/* Apply one generic cheat line ("room=N", "name=V", "name[i]=V"). Returns 1 if it is a
 * sticky global write the caller should re-apply each frame (freeze), 0 for a one-shot/no-op. */
int          gml_cheat_apply(GmlVM *vm, const char *code);
void         gml_vm_step(GmlVM *vm);                         /* one frame of the game loop */
void         gml_vm_draw(GmlVM *vm);                         /* draw phase (needs vm->render) */
void         gml_vm_post_draw(GmlVM *vm);                    /* classic animation phase */
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

/* read a global scalar / global array element by name (0 if absent). Names are matched by
 * content (not interned ptr) so the host can query e.g. "view_xview". */
double  gml_global_num(GmlVM *vm, const char *name);
double  gml_global_arr(GmlVM *vm, const char *name, int idx);
double  gml_room_speed(GmlVM *vm);
uint32_t gml_vm_room_background_argb(GmlVM *vm);
void    gml_set_global_arr(GmlVM *vm, const char *name, int idx, double val);
void    gml_set_global_scalar(GmlVM *vm, const char *name, double val);           /* write V_REAL, not array */
int     gml_set_inst_var_all(GmlVM *vm, const char *objname, const char *var, double val); /* freeze inst var; returns count */
/* generic pause-menu injection helpers (host menu editor) */
int     gml_inst_get_num(const GmlInstance *in, const char *var);
int     gml_inst_array_count(const GmlInstance *in, const char *var);
void    gml_inst_array_set_str(GmlInstance *in, const char *var, int is2d, int row, int col, const char *str);
void    gml_inst_array_set_num(GmlInstance *in, const char *var, int is2d, int row, int col, double num);
int     gml_room_index_by_name(GmlWin *win, const char *name);

/* save-state payload for the VM runtime only. Static data parsed from data.win is not included. */
size_t  gml_vm_state_size(GmlVM *vm);
int     gml_vm_draw_pass_active(GmlVM *vm, const char *suffix);
void    gml_vm_draw_pass(GmlVM *vm, const char *suffix); /* Draw_72-77 and Draw_65/66 stage events */
int     gml_vm_state_save(GmlVM *vm, void *data, size_t len, size_t *written);
int     gml_vm_state_load(GmlVM *vm, const void *data, size_t len, size_t *used);

/* Software-3D state is serialized with the VM so rewind/load cannot change
 * the active projection, culling, lighting, or light definitions mid-frame. */
GmlSoftware3D *gml_vm_software3d_ensure(GmlVM *vm);
void    gml_vm_software3d_reset(GmlVM *vm);
void    gml_vm_software3d_state_get(GmlVM *vm,int flags[GML_SOFTWARE3D_STATE_FLAG_COUNT],
                         double values[GML_SOFTWARE3D_STATE_VALUE_COUNT],
                         uint32_t colors[GML_SOFTWARE3D_STATE_COLOR_COUNT]);
void    gml_vm_software3d_state_set(GmlVM *vm,const int flags[GML_SOFTWARE3D_STATE_FLAG_COUNT],
                         const double values[GML_SOFTWARE3D_STATE_VALUE_COUNT],
                         const uint32_t colors[GML_SOFTWARE3D_STATE_COLOR_COUNT]);

#endif
