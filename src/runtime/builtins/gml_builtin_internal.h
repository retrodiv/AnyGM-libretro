/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_BUILTIN_INTERNAL_H
#define GML_BUILTIN_INTERNAL_H

#include "gml_builtin.h"
#include "gml_render.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
/* file_find_first/next state (GM exposes one active find). Wildcard match is hand-rolled:
 * mingw has no fnmatch.h and GM masks only use * and ?. Case-insensitive like Windows. */
#define GML_FF_MAX 512
#define GML_GP_MAX_DEV 16
#define GML_INI_MAX 16384
#define GML_DS_MAP_MAX 256
#define GML_DS_LIST_MAX 256
#define GML_DS_GRID_MAX 32
#define GML_TIME_SOURCE_MAX 256
#define GML_TIME_SOURCE_ID_BASE UINT32_C(0x48000000)
#define GML_MAX_EMITTERS 32
#define GML_PHYS_FIXTURE_MAX 128
#define GML_PHYS_JOINT_MAX 256
#define GML_PHYS_FIXTURE_POINTS 16

typedef struct {
  char *key;
  GmlVal key_val, val;
  unsigned char child_kind;
} GmlDSMapEntry;
typedef struct {
  int live;
  uint32_t id;
  GmlDSMapEntry *entry;
  int len, cap;
  int *hidx;
  int hcap, hdirty, last_lookup;
} GmlDSMap;
typedef struct {
  int live;
  uint32_t id;
  GmlVal *item;
  unsigned char *child_kind;
  int len, cap;
} GmlDSList;
typedef struct {
  int live;
  uint32_t id;
  GmlVal *cell;
  int w, h;
} GmlDSGrid;
typedef struct {
  int live, parent, units, state;
  int repetitions, reps_remaining, reps_completed, expiry_type;
  uint32_t id;
  double period, remaining;
  GmlVal callback, args;
} GmlTimeSource;
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
typedef struct {
  char *section, *key, *sval;
  double val;
  int is_str;
} GmlBuiltinIniEntry;
typedef struct {
  unsigned char *data;
  int size, cap, pos, live;
} GmlBuiltinBuffer;
typedef struct {
  char name[40];
  double value;
} GmlFallbackAudioParameter;
typedef struct {
  int used, playing, paused;
  GmlFallbackAudioParameter param[16];
  int nparam;
} GmlFallbackAudioEvent;
typedef struct {
  int live;
  double left, top;
  int hc, vc, cw, ch;
  uint8_t *cell;
} GmlMpGrid;
/* Ten high-score places, ordered by score. */
#define GML_HIGHSCORE_PLACES 10
typedef struct { char name[64]; double score; int used; } GmlBuiltinHighscore;

struct GmlBuiltinState {
  GmlVM *vm;
  GmlBuiltinHighscore highscore[GML_HIGHSCORE_PLACES];
  GmlBuiltinIniEntry ini_kv[GML_INI_MAX];
  int ini_n, ini_open;
  char ini_path[256];
  void *bin_file[16];
  GmlBuiltinBuffer buffer[16];
  int next_buffer_id;
  int async_sl_q[16], async_sl_status[16], n_async_sl, async_seq;
  int async_group_active, async_group_id;
  int async_group_status, async_group_count;
  int async_http_q[16], n_async_http;
  GmlDSMap ds_map[GML_DS_MAP_MAX];
  GmlDSList ds_list[GML_DS_LIST_MAX];
  GmlDSGrid ds_grid[GML_DS_GRID_MAX];
  int next_ds_id;
  int ds_map_last_slot;
  int ds_list_compat_repair;
  GmlTimeSource time_source[GML_TIME_SOURCE_MAX];
  uint32_t next_time_source_id;
  int time_source_game_state;
  unsigned char emitter_live[GML_MAX_EMITTERS];
  double emitter_gain[GML_MAX_EMITTERS];
  double emitter_x[GML_MAX_EMITTERS];
  double emitter_y[GML_MAX_EMITTERS];
  double emitter_z[GML_MAX_EMITTERS];
  double emitter_ref[GML_MAX_EMITTERS];
  double emitter_max[GML_MAX_EMITTERS];
  double emitter_factor[GML_MAX_EMITTERS];
  double listener_x, listener_y, listener_z;
  double listener_forward_x, listener_forward_y, listener_forward_z;
  double listener_up_x, listener_up_y, listener_up_z;
  int audio_falloff_model;
  GmlPhysicsFixture phys_fixture[GML_PHYS_FIXTURE_MAX];
  GmlPhysicsJoint phys_joint[GML_PHYS_JOINT_MAX];
  uint32_t phys_next_id;
  double phys_gravity_x, phys_gravity_y, phys_update_speed;
  int phys_update_iterations, phys_paused, phys_debug_draw;
  char *file_find_name[GML_FF_MAX];
  int file_find_count;
  int file_find_index;
  double gamepad_deadzone[GML_GP_MAX_DEV];
  unsigned char gamepad_deadzone_set[GML_GP_MAX_DEV];
  char string_ring[8][64];
  unsigned string_ring_index;
  GmlFallbackAudioEvent fallback_audio_event[512];
  GmlFallbackAudioParameter fallback_audio_global_parameter[16];
  int fallback_audio_global_parameter_count;
  GmlMpGrid mp_grid[8];
  int log_ds;
  int log_collision;
  int log_tile_collision;
  int gamepad_debug;
  int collision_grid_mode;
  int log_stub;
  int unknown_builtin_logged;
  int random_seed_initialized;
  long fixed_random_seed;
  long skeleton_log_count;
  int json_log_count;
  int d3_draw_log_count;
  long d3_wall_limit_frame;
  int d3_wall_count;
  long shader_set_log_count;
  long shader_compiled_log_count;
  int shader_texture_log_count;
  int gpu_log_count;
  char collision_filter[128];
};

struct GmlRender;

/*
 * Private ordered-dispatch boundary. Family stages return a result when they
 * recognize a name and otherwise delegate to the next declaration in source
 * order. Exact-name registry work must preserve that visible precedence.
 */
/* Additional registry-owner shared operations. */
void builtin_set_blendmode_ext(GmlVM *vm,GmlRender *R,int src,int dst);
void builtin_set_blendmode(GmlRender *R, int bm);
void draw_legacy_sprite_shadow(GmlVM *vm,double vertical_extra,double alpha);
GmlSoftware3D *graphics_state_for_vm(GmlVM *vm);

/* Registry-owner crossing operations. */
void gp_deadzone_set(GmlVM *vm, int dev, double dz);
double gp_axis_value_filtered(GmlVM *vm, int dev, int ax);
int gp_debug_on(GmlVM *vm);
GmlVal builtin_call_impl(GmlVM *vm, const char *nm, GmlVal *a, int n);

GmlVal gml_builtin_try_animation(GmlVM *vm, const char *name,
                                 GmlVal *args, int count);
GmlVal gml_builtin_try_physics(GmlVM *vm, const char *name,
                               GmlVal *args, int count);
GmlVal gml_builtin_try_platform_extensions(GmlVM *vm, const char *name,
                                           GmlVal *args, int count);
GmlVal gml_builtin_try_platform_noops(GmlVM *vm, const char *name,
                                      GmlVal *args, int count);
GmlVal gml_builtin_try_scripts_fallback(GmlVM *vm, const char *name,
                                        GmlVal *args, int count);
GmlVal gml_builtin_try_actions(GmlVM *vm, const char *name,
                               GmlVal *args, int count);
GmlVal gml_builtin_try_instances(GmlVM *vm, const char *name,
                                 GmlVal *args, int count);
GmlVal gml_builtin_try_instances_rooms(GmlVM *vm, const char *name,
                                       GmlVal *args, int count);
GmlVal gml_builtin_try_values_language(GmlVM *vm, const char *name,
                                       GmlVal *args, int count);
GmlVal gml_builtin_try_instances_destroy(GmlVM *vm, const char *name,
                                         GmlVal *args, int count);
GmlVal gml_builtin_try_actions_legacy(GmlVM *vm, const char *name,
                                      GmlVal *args, int count);
GmlVal gml_builtin_try_instances_queries(GmlVM *vm, const char *name,
                                         GmlVal *args, int count);
GmlVal gml_builtin_try_input(GmlVM *vm, const char *name,
                             GmlVal *args, int count);
GmlVal gml_builtin_try_io(GmlVM *vm, const char *name,
                          GmlVal *args, int count);
GmlVal gml_builtin_try_instances_scripts(GmlVM *vm, const char *name,
                                         GmlVal *args, int count);
GmlVal gml_builtin_try_audio(GmlVM *vm, const char *name,
                             GmlVal *args, int count);
int builtin_external_audio_define(const char *library,const char *symbol);
GmlVal builtin_external_audio_call(GmlVM *vm,int handle,
                                   GmlVal *args,int count,int *handled);
GmlVal gml_builtin_try_instances_paths(GmlVM *vm, const char *name,
                                       GmlVal *args, int count);
GmlVal gml_builtin_try_layers_early(GmlVM *vm, const char *name,
                                    GmlVal *args, int count);
GmlVal gml_builtin_try_platform(GmlVM *vm, const char *name,
                                GmlVal *args, int count);
GmlVal gml_builtin_try_ds(GmlVM *vm, const char *name,
                          GmlVal *args, int count);
GmlVal gml_builtin_try_json(GmlVM *vm, const char *name,
                            GmlVal *args, int count);
GmlVal gml_builtin_try_layers(GmlVM *vm, const char *name,
                              GmlVal *args, int count);
GmlVal gml_builtin_try_platform_tail(GmlVM *vm, const char *name,
                                     GmlVal *args, int count);
GmlVal gml_builtin_try_collision(GmlVM *vm, const char *name,
                                 GmlVal *args, int count);
GmlVal gml_builtin_try_values_math(GmlVM *vm, const char *name,
                                   GmlVal *args, int count);
GmlVal gml_builtin_try_collision_planning(GmlVM *vm, const char *name,
                                          GmlVal *args, int count);
GmlVal gml_builtin_try_values_strings(GmlVM *vm, const char *name,
                                      GmlVal *args, int count);
GmlVal gml_builtin_continue_values_strings(GmlVM *vm, const char *name,
                                           GmlVal *args, int count);
GmlVal gml_builtin_try_io_ini(GmlVM *vm, const char *name,
                              GmlVal *args, int count);
GmlVal gml_builtin_try_particles(GmlVM *vm, const char *name,
                                 GmlVal *args, int count);
GmlVal gml_builtin_try_draw(GmlVM *vm, const char *name,
                            GmlVal *args, int count);
GmlVal gml_builtin_try_values_variables(GmlVM *vm, const char *name,
                                        GmlVal *args, int count);
GmlVal gml_builtin_try_draw_3d(GmlVM *vm, const char *name,
                               GmlVal *args, int count);

static inline GmlBuiltinState *builtin_state_ensure(GmlVM *vm){
  return gml_builtin_state_ensure(vm);
}

const char *builtin_setting(const GmlVM *vm, const char *name);
void file_find_reset(GmlBuiltinState *state);
void builtin_io_files_close(GmlBuiltinState *state);
void builtin_state_ini_reset(GmlBuiltinState *state);
GmlRenderDrawState builtin_draw_state(const GmlRender *render);
GmlRenderTargetMetrics builtin_target_metrics(const GmlRender *render);
GmlRenderPresentationMetrics
builtin_presentation_metrics(const GmlRender *render);
void builtin_set_draw_color(GmlRender *render, uint32_t color);
void builtin_set_alpha_blend(GmlRender *render, int enabled);
void builtin_set_interpolation(GmlRender *render, int enabled);
int presentation_size(const GmlVM *vm, const GmlRender *render, int height);
int log_ds_on(GmlVM *vm);
GmlDSMap *ds_map_slot(GmlVM *vm, int id);
GmlDSList *ds_list_slot(GmlVM *vm, int id);
int ds_map_create_id(GmlVM *vm);
int ds_map_put(GmlVM *vm, int id, GmlVal key, GmlVal value, int overwrite);
void ds_map_mark_child(GmlVM *vm, int id, GmlVal key, int kind);
void ds_map_destroy_id(GmlVM *vm, int id);
int ds_list_create_id(GmlVM *vm);
void ds_list_push_kind(GmlDSList *list, GmlVal value, int kind);
void ds_list_destroy_id(GmlVM *vm, int id);
GmlVal ds_map_lookup_s(GmlVM *vm, int id, const char *key, int *found);
void gml_ds_list_clear_direct(GmlVM *vm, int id);
GmlVal gml_ds_map_find_previous_direct(GmlVM *vm, int id,
                                       GmlVal key, int has_key);
int gml_ds_map_exists_direct(GmlVM *vm, int id, GmlVal key, int has_key);
int gml_ds_map_empty_direct(GmlVM *vm, int id);
GmlVal gml_ds_map_find_last_direct(GmlVM *vm, int id);
GmlVal gml_ds_list_find_value_direct(GmlVM *vm, int id, int position);
int gml_ds_list_size_direct(GmlVM *vm, int id);

typedef struct {
  GmlVal key;
  GmlVal value;
  int nested_kind;
} GmlBuiltinMapItemView;
typedef struct {
  GmlVal value;
  int nested_kind;
} GmlBuiltinListItemView;
int gml_ds_map_view_count(GmlVM *vm, int id);
int gml_ds_map_item_view(GmlVM *vm, int id, int index,
                         GmlBuiltinMapItemView *item);
int gml_ds_list_view_count(GmlVM *vm, int id);
int gml_ds_list_item_view(GmlVM *vm, int id, int index,
                          GmlBuiltinListItemView *item);
int gml_ds_list_append_direct(GmlVM *vm, int id, GmlVal value,
                              int nested_kind);

/*
 * Private streaming JSON boundary used by DS text serialization. Callers may
 * zero-initialize the value, but must otherwise use these operations rather
 * than inspecting or modifying its storage.
 */
typedef struct {
  char *text;
  size_t length;
  size_t capacity;
} GmlBuiltinJsonWriter;
int gml_builtin_json_writer_putc(GmlBuiltinJsonWriter *writer, char value);
int gml_builtin_json_writer_puts(GmlBuiltinJsonWriter *writer,
                                 const char *value);
int gml_builtin_json_writer_put_value(GmlVM *vm,
                                      GmlBuiltinJsonWriter *writer,
                                      GmlVal value, int depth,
                                      int allowed_ds_kind);
void gml_builtin_json_writer_discard(GmlBuiltinJsonWriter *writer);
char *gml_builtin_json_writer_take(GmlBuiltinJsonWriter *writer,
                                   const char *fallback);
GmlVal gml_builtin_json_decode_ds(GmlVM *vm, const char *text);
void buffer_write_raw(GmlVM *vm, int buffer_index,
                      const void *bytes, int byte_count);
int vm_buffer_slot(GmlVM *vm, int id);
int vm_file_slot(GmlVM *vm, int id);
void vm_file_ungetc(GmlVM *vm, int slot, int value);
GmlVal builtin_ini_open_file(GmlVM *vm, GmlVal *args, int count);
GmlVal builtin_file_text_open_read(GmlVM *vm, GmlVal *args, int count);
GmlVal builtin_file_text_read_string(GmlVM *vm, GmlVal *args, int count);
GmlVal builtin_file_text_readln(GmlVM *vm, GmlVal *args, int count);
int builtin_layer_exact(GmlVM *vm, const char *name, GmlVal *args,
                        int count, GmlVal *result);
GmlVal builtin_skeleton(GmlVM *vm, const char *name,
                        GmlVal *args, int count);
int builtin_log_tile_collision(GmlVM *vm);
int log_col_on(GmlVM *vm);
int log_col_match(GmlVM *vm, const char *object_name);
const char *gm_string_format(GmlVal value, char buffer[64]);
char *dup_n(const char *source, int length);
GmlVal md5_hex_val(const uint8_t *bytes, size_t length);
GmlVal sha1_hex_val(const uint8_t *bytes, size_t length);
unsigned char *base64_decode_alloc(const char *text, int *output_length);
char *base64_encode_alloc(const unsigned char *bytes, int length);
const char *S(GmlVM *vm, GmlVal *args, int count, int index);
GmlVal array4(double a, double b, double c, double d);
GmlVal arr_newv(int count);
GmlVal arr8(double a0, double a1, double a2, double a3,
            double a4, double a5, double a6, double a7);
int array_equals_recursive(GmlVM *vm, GmlVal left, GmlVal right);
GmlVal var_store_clone(GmlVal value);
double builtin_math_sqrt(GmlVM *vm, double value);
void builtin_math_set_epsilon(GmlVM *vm, double epsilon);
double gm_sign(double x);
GmlVal gml_array_create_ext(GmlVM *vm, GmlVal *args, int count);
GmlVal gml_array_map_ext(GmlVM *vm, GmlVal *args, int count);
GmlVal gml_array_foreach(GmlVM *vm, GmlVal *args, int count);
int gml_array_search_index(GmlVal *args, int count);
GmlVal gml_array_shuffle_copy(GmlVM *vm, GmlVal source,
                              GmlVal *args, int count);
void gml_array_sort(GmlVal arr, int ascending);
int gml_val_sort_compare(GmlVal a, GmlVal b);
void gml_val_sort_reverse(GmlVal *value, int count);
int ds_val_equal(GmlVal a, GmlVal b);
GmlVal builtin_http_request_stub(GmlVM *vm);
char *resolve_read_path(GmlVM *vm, const char *path);
GmlTimeSource *time_source_find(GmlVM *vm, int id);
int order_pos(GmlVM *vm, int room_index);
int script_code_of(GmlVM *vm, int script_id);
int script_ref_code_of(GmlVM *vm, GmlVal value);
GmlRtLayer *rt_layer_resolve(GmlVM *vm, GmlVal *args, int count);
void rt_layer_touch(GmlVM *vm, GmlRtLayer *layer);
GmlRtElem *rt_sprite_for_layer_name(GmlVM *vm, int layer_id,
                                    const char *name);
GmlRtElem *rt_background_for_layer(GmlVM *vm, GmlRtLayer *layer,
                                   int create_from_room);
GmlDSList *ds_list_slot_repair(GmlVM *vm, int id);
void ds_list_push(GmlDSList *list, GmlVal value);
double gm_round(double value);
void motion_from_components(GmlInstance *instance);
void motion_from_speed_direction(GmlVM *vm, GmlInstance *instance);
int target_matches_instance(GmlVM *vm, GmlInstance *self,
                            GmlInstance *candidate, int target);
int inst_bbox(GmlVM *vm, GmlInstance *instance, double x, double y,
              double *left, double *top, double *right, double *bottom);
double point_to_instance_distance(GmlVM *vm, GmlInstance *instance,
                                  double x, double y);
double distance_to_target(GmlVM *vm, GmlInstance *self, int target);
int collision_instance_list_at(GmlVM *vm, double x, double y, int object,
                               GmlDSList *list, int ordered);
GmlInstance *collision_instance_at(GmlVM *vm, double x, double y, int object,
                                   int solid_only);
int collision_at(GmlVM *vm, double x, double y, int object, int solid_only);
void classic_move_bounce(GmlVM *vm, GmlInstance *instance, int all,
                         int advanced);
int potential_step_move(GmlVM *vm, GmlInstance *instance, double target_x,
                        double target_y, double amount, int target,
                        int solid_only);
int instance_region_hit(GmlVM *vm, GmlInstance *instance, double x, double y,
                        double width, double height);
void snap_contact_axis(GmlVM *vm, GmlInstance *instance, double dx, double dy);
int resolve_landing_overlap(GmlVM *vm, GmlInstance *instance,
                            int max_distance);
GmlInstance *instance_at_point(GmlVM *vm, double x, double y, int object);
GmlInstance *collision_line_query(GmlVM *vm, double x1, double y1, double x2,
                                  double y2, int object, int precise,
                                  int not_self);
int collision_line_list_query(GmlVM *vm, double x1, double y1, double x2,
                              double y2, int object, int precise, int not_self,
                              GmlDSList *list, int ordered);
GmlInstance *collision_shape(GmlVM *vm, int kind, double *parameters,
                             int object, int precise, int not_self);
int collision_shape_list_query(GmlVM *vm, int kind, double *parameters,
                               GmlVal target, int precise, int not_self,
                               GmlDSList *list, int ordered);
int builtin_input_kbgp(GmlVM *vm, const char *name, GmlVal *args, int count,
                       GmlVal *result);
int mouse_btn_check(GmlVM *vm, int button, int edge);
double classic_display_mouse_coord(const GmlVM *vm,
                                   const struct GmlRender *render,
                                   double value, int height, int to_window);
void legacy_move_rpg(GmlVM *vm, GmlVal *args, int count);
void legacy_direction_rpg(GmlVM *vm, GmlVal *args, int count);
void legacy_friction_platform(GmlVM *vm, GmlVal *args, int count);
void legacy_destroy_self(GmlVM *vm, int count);

/* Draw-owner crossing operations. */
void builtin_set_draw_alpha(GmlRender *render,double alpha);
void builtin_set_draw_font(GmlVM *vm,GmlRender *render,int font);
void builtin_set_draw_halign(GmlRender *render,int alignment);
void builtin_set_draw_valign(GmlRender *render,int alignment);
int display_size(const GmlVM *vm, const GmlRender *r, int height);
double gml_shader_get_uniform(GmlRender *R, int sh, const char *un);
void gml_shader_set_uniform_f(GmlRender *R, int h, GmlVal *a, int n);
int gml_draw_subimg(GmlVM *vm, double raw);
int texture_info(GmlRender *R, int tex, double *uw, double *uh, double *tw, double *th);
char *resolve_write_path(GmlVM *vm, const char *p);
int vm_file_open(GmlVM *vm, const char *path, const char *mode);
void vm_file_close(GmlVM *vm,int slot);
int vm_file_getc(GmlVM *vm,int slot);
int vm_file_writef(GmlVM *vm,int slot,const char *format,...);
int vm_file_read_real(GmlVM *vm,int slot,double *value);
static inline GmlSoftware3D *graphics_state_for_render(GmlRender *render){
  return gml_render_software3d(render);
}
double draw_gui_x(GmlRender *render,double value);
double draw_gui_y(GmlRender *render,double value);
double draw_gui_w(GmlRender *render,double value);
double draw_gui_h(GmlRender *render,double value);
GmlVal builtin_fmod(GmlVM *vm, const char *nm, GmlVal *a, int n);

#define GML_GRAPHICS (graphics_state_for_render(R))

/* Header-local hot helpers. */
static inline uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o]|(uint32_t)d[o+1]<<8|(uint32_t)d[o+2]<<16|(uint32_t)d[o+3]<<24;
}
static inline double N(GmlVal *a, int n, int i){ return (i<n)? (a[i].t==V_REAL?a[i].d:(a[i].s?atof(a[i].s):0)) : 0; }

/* Runtime values reach 32-bit arguments as doubles, and content routinely passes negative ones —
 * -1 is the ordinary "no tint" colour. Converting a negative double straight to an unsigned type is
 * undefined, and the two architectures this runtime targets disagree in the worst possible way: one
 * wraps to all-ones while the other saturates to zero, turning an untinted draw black. Wrap
 * explicitly so every host obtains the required modular result. */
static inline uint32_t U32(double value){
  if(!isfinite(value)) return 0;
  double truncated=trunc(value);
  double wrapped=fmod(truncated,4294967296.0);
  if(wrapped<0.0) wrapped+=4294967296.0;
  return (uint32_t)wrapped;
}
static inline uint32_t NU32(GmlVal *a, int n, int i){ return U32(N(a,n,i)); }


#endif
