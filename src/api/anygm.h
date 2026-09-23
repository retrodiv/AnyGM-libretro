/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_H
#define ANYGM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The one copy of this project's version. Everything else derives it: an adapter reports it as the
 * runtime library version, and every publishable metadata file declares the literal "Git" instead
 * of a number, so no copy can promise a value that has gone stale. CONTRIBUTING.md owns the single
 * increment step. */
#define ANYGM_VERSION "0.1.26"
#define ANYGM_API_VERSION 4u
#define ANYGM_HOST_SERVICES_VERSION 1u
#define ANYGM_STATE_SCHEMA 34u
#define ANYGM_MAX_GAMEPADS 4u
#define ANYGM_MAX_GAMEPAD_BUTTONS 16u
#define ANYGM_MAX_GAMEPAD_AXES 4u
/* gamepad_connected: beyond Off (0) and On (1), the default resolves per content. */
#define ANYGM_GAMEPAD_AUTO 2u
#define ANYGM_MAX_KEYS 512u
#define ANYGM_MAX_RUNTIME_OVERRIDES 256u
#define ANYGM_MAX_RUNTIME_OVERRIDE_EXPRESSION 127u

enum {
  ANYGM_PAD_FACE_BOTTOM=0,
  ANYGM_PAD_FACE_LEFT=1,
  ANYGM_PAD_SELECT=2,
  ANYGM_PAD_START=3,
  ANYGM_PAD_UP=4,
  ANYGM_PAD_DOWN=5,
  ANYGM_PAD_LEFT=6,
  ANYGM_PAD_RIGHT=7,
  ANYGM_PAD_FACE_RIGHT=8,
  ANYGM_PAD_FACE_TOP=9,
  ANYGM_PAD_LEFT_SHOULDER=10,
  ANYGM_PAD_RIGHT_SHOULDER=11,
  ANYGM_PAD_LEFT_TRIGGER=12,
  ANYGM_PAD_RIGHT_TRIGGER=13,
  ANYGM_PAD_LEFT_STICK=14,
  ANYGM_PAD_RIGHT_STICK=15
};

/* Key values use the stable, framework-neutral set consumed by AnyGM. */
enum {
  ANYGM_KEY_BACKSPACE=8,
  ANYGM_KEY_TAB=9,
  ANYGM_KEY_CLEAR=12,
  ANYGM_KEY_RETURN=13,
  ANYGM_KEY_PAUSE=19,
  ANYGM_KEY_ESCAPE=27,
  ANYGM_KEY_SPACE=32,
  ANYGM_KEY_a='a',
  ANYGM_KEY_c='c',
  ANYGM_KEY_x='x',
  ANYGM_KEY_z='z',
  ANYGM_KEY_DELETE=127,
  ANYGM_KEY_KP0=256,
  ANYGM_KEY_KP1=257,
  ANYGM_KEY_KP2=258,
  ANYGM_KEY_KP3=259,
  ANYGM_KEY_KP4=260,
  ANYGM_KEY_KP5=261,
  ANYGM_KEY_KP6=262,
  ANYGM_KEY_KP7=263,
  ANYGM_KEY_KP8=264,
  ANYGM_KEY_KP9=265,
  ANYGM_KEY_KP_PERIOD=266,
  ANYGM_KEY_KP_DIVIDE=267,
  ANYGM_KEY_KP_MULTIPLY=268,
  ANYGM_KEY_KP_MINUS=269,
  ANYGM_KEY_KP_PLUS=270,
  ANYGM_KEY_KP_ENTER=271,
  ANYGM_KEY_KP_EQUALS=272,
  ANYGM_KEY_UP=273,
  ANYGM_KEY_DOWN=274,
  ANYGM_KEY_RIGHT=275,
  ANYGM_KEY_LEFT=276,
  ANYGM_KEY_INSERT=277,
  ANYGM_KEY_HOME=278,
  ANYGM_KEY_END=279,
  ANYGM_KEY_PAGEUP=280,
  ANYGM_KEY_PAGEDOWN=281,
  ANYGM_KEY_F1=282,
  ANYGM_KEY_F2=283,
  ANYGM_KEY_F3=284,
  ANYGM_KEY_F4=285,
  ANYGM_KEY_F5=286,
  ANYGM_KEY_F6=287,
  ANYGM_KEY_F7=288,
  ANYGM_KEY_F8=289,
  ANYGM_KEY_F9=290,
  ANYGM_KEY_F10=291,
  ANYGM_KEY_F11=292,
  ANYGM_KEY_F12=293,
  ANYGM_KEY_NUMLOCK=300,
  ANYGM_KEY_CAPSLOCK=301,
  ANYGM_KEY_SCROLLOCK=302,
  ANYGM_KEY_RIGHT_SHIFT=303,
  ANYGM_KEY_LEFT_SHIFT=304,
  ANYGM_KEY_RIGHT_CTRL=305,
  ANYGM_KEY_LEFT_CTRL=306,
  ANYGM_KEY_RIGHT_ALT=307,
  ANYGM_KEY_LEFT_ALT=308,
  ANYGM_KEY_LAST=512
};

#define ANYGM_KEY_LSHIFT ANYGM_KEY_LEFT_SHIFT
#define ANYGM_KEY_RSHIFT ANYGM_KEY_RIGHT_SHIFT
#define ANYGM_KEY_LCTRL ANYGM_KEY_LEFT_CTRL
#define ANYGM_KEY_RCTRL ANYGM_KEY_RIGHT_CTRL
#define ANYGM_KEY_LALT ANYGM_KEY_LEFT_ALT
#define ANYGM_KEY_RALT ANYGM_KEY_RIGHT_ALT

typedef struct AnygmEngine AnygmEngine;

typedef int32_t AnygmResult;
enum {
  ANYGM_OK=0,
  ANYGM_ERROR_INVALID_ARGUMENT=-1,
  ANYGM_ERROR_INCOMPATIBLE_ABI=-2,
  ANYGM_ERROR_INVALID_STATE=-3,
  ANYGM_ERROR_OUT_OF_MEMORY=-4,
  ANYGM_ERROR_IO=-5,
  ANYGM_ERROR_UNSUPPORTED=-6,
  ANYGM_ERROR_INVALID_CONTENT=-7,
  ANYGM_ERROR_CORRUPT_DATA=-8,
  ANYGM_ERROR_STATE_MISMATCH=-9,
  ANYGM_ERROR_INTERNAL=-10,
  ANYGM_RESULT_END=1
};

typedef uint32_t AnygmLogLevel;
enum {
  ANYGM_LOG_DEBUG=0,
  ANYGM_LOG_INFO=1,
  ANYGM_LOG_WARN=2,
  ANYGM_LOG_ERROR=3
};

typedef uint32_t AnygmFileMode;
enum {
  ANYGM_FILE_READ=1u<<0,
  ANYGM_FILE_WRITE=1u<<1,
  ANYGM_FILE_CREATE=1u<<2,
  ANYGM_FILE_TRUNCATE=1u<<3
};

typedef uint32_t AnygmSeekOrigin;
enum {
  ANYGM_SEEK_START=0,
  ANYGM_SEEK_CURRENT=1,
  ANYGM_SEEK_END=2
};

typedef struct AnygmFileInfo {
  uint32_t struct_size;
  uint32_t flags;
  uint64_t size;
} AnygmFileInfo;

typedef struct AnygmDirectoryEntry {
  uint32_t struct_size;
  uint32_t flags;
  char name[512];
} AnygmDirectoryEntry;

typedef struct AnygmWallTime {
  uint32_t struct_size;
  uint32_t flags;
  int64_t unix_seconds;
  int32_t utc_offset_minutes;
  int32_t reserved;
} AnygmWallTime;

typedef struct AnygmCalendarTime {
  uint32_t struct_size;
  int32_t year;
  int32_t month;
  int32_t day;
  int32_t weekday;
  int32_t hour;
  int32_t minute;
  int32_t second;
} AnygmCalendarTime;

enum {
  ANYGM_WALL_TIME_OFFSET_VALID=1u<<0
};

enum {
  ANYGM_FILE_INFO_EXISTS=1u<<0,
  ANYGM_FILE_INFO_DIRECTORY=1u<<1,
  ANYGM_FILE_INFO_REGULAR=1u<<2
};

typedef void (*AnygmLogFn)(void *userdata,AnygmLogLevel level,const char *message);
typedef uint64_t (*AnygmMonotonicTimeFn)(void *userdata);
typedef AnygmResult (*AnygmWallTimeFn)(void *userdata,AnygmWallTime *time);
typedef uint64_t (*AnygmRandomSeedFn)(void *userdata);
typedef void *(*AnygmFileOpenFn)(void *userdata,const char *path,AnygmFileMode mode);
typedef size_t (*AnygmFileReadFn)(void *userdata,void *file,void *data,size_t size);
typedef size_t (*AnygmFileWriteFn)(void *userdata,void *file,const void *data,size_t size);
typedef int64_t (*AnygmFileSeekFn)(void *userdata,void *file,int64_t offset,AnygmSeekOrigin origin);
typedef AnygmResult (*AnygmFileFlushFn)(void *userdata,void *file);
typedef void (*AnygmFileCloseFn)(void *userdata,void *file);
/* Large immutable files may be borrowed directly from a host mapping. The returned handle is
 * opaque and remains valid until file_unmap receives it with the original data and size. */
typedef void *(*AnygmFileMapFn)(void *userdata,const char *path,
                                const void **data,size_t *size);
typedef void (*AnygmFileUnmapFn)(void *userdata,void *mapping,
                                 const void *data,size_t size);
typedef AnygmResult (*AnygmFileStatFn)(void *userdata,const char *path,AnygmFileInfo *info);
typedef AnygmResult (*AnygmPathFn)(void *userdata,const char *path);
typedef AnygmResult (*AnygmRenameFn)(void *userdata,const char *from,const char *to);
typedef void *(*AnygmDirectoryOpenFn)(void *userdata,const char *path);
typedef AnygmResult (*AnygmDirectoryReadFn)(void *userdata,void *directory,
                                            AnygmDirectoryEntry *entry);
typedef void (*AnygmDirectoryCloseFn)(void *userdata,void *directory);
typedef AnygmResult (*AnygmLocaleFn)(void *userdata,char *language,size_t language_size,
                                     char *region,size_t region_size,char *tag,size_t tag_size);
typedef AnygmResult (*AnygmDateTimeFormatFn)(void *userdata,
                                             const AnygmCalendarTime *calendar,
                                             uint32_t style,char *text,size_t text_size);
typedef void (*AnygmRumbleFn)(void *userdata,uint32_t port,uint16_t strong,uint16_t weak);
typedef uint32_t (*AnygmHostCapabilityFn)(void *userdata,uint32_t capability);
/* Development settings are optional, host-owned strings. A returned pointer must remain valid
 * until the next setting query on the same host. Production hosts may leave this callback NULL. */
typedef const char *(*AnygmDevelopmentSettingFn)(void *userdata,const char *name);
/* The returned path belongs to the host VFS namespace. The callback may use family, style and the
 * optional filename hint; returning unsupported lets the runtime use its embedded fallback. */
typedef AnygmResult (*AnygmFontResolveFn)(void *userdata,const char *family,
                                          const char *filename_hint,uint32_t style,
                                          char *path,size_t path_size);
/* Optional host-native RTF rendering. The destination is XRGB8888 with the supplied byte pitch.
 * Returning unsupported selects the deterministic embedded renderer. */
typedef AnygmResult (*AnygmRichTextRenderFn)(void *userdata,const void *rtf,size_t rtf_size,
                                             uint32_t background_xrgb,void *pixels,
                                             uint32_t width,uint32_t height,size_t pitch);

enum {
  ANYGM_HOST_CAPABILITY_NETWORK_CONNECTED=1u
};

enum {
  ANYGM_DATE_FORMAT_DATE=1u,
  ANYGM_DATE_FORMAT_TIME=2u,
  ANYGM_DATE_FORMAT_DATE_TIME=3u
};

enum {
  ANYGM_FONT_STYLE_BOLD=1u<<0,
  ANYGM_FONT_STYLE_ITALIC=1u<<1,
  ANYGM_FONT_STYLE_MONOSPACE=1u<<2,
  ANYGM_FONT_STYLE_SERIF=1u<<3
};

typedef struct AnygmHostServices {
  uint32_t struct_size;
  uint32_t abi_version;
  void *userdata;
  AnygmLogFn log;
  AnygmMonotonicTimeFn monotonic_time_ns;
  AnygmWallTimeFn wall_time;
  AnygmRandomSeedFn random_seed;
  AnygmFileOpenFn file_open;
  AnygmFileReadFn file_read;
  AnygmFileWriteFn file_write;
  AnygmFileSeekFn file_seek;
  AnygmFileFlushFn file_flush;
  AnygmFileCloseFn file_close;
  AnygmFileMapFn file_map;
  AnygmFileUnmapFn file_unmap;
  AnygmFileStatFn file_stat;
  AnygmPathFn directory_create;
  AnygmRenameFn path_rename;
  AnygmPathFn path_remove;
  AnygmDirectoryOpenFn directory_open;
  AnygmDirectoryReadFn directory_read;
  AnygmDirectoryCloseFn directory_close;
  AnygmLocaleFn locale;
  AnygmDateTimeFormatFn date_time_format;
  AnygmRumbleFn rumble;
  AnygmHostCapabilityFn capability;
  AnygmDevelopmentSettingFn development_setting;
  AnygmFontResolveFn font_resolve;
  AnygmRichTextRenderFn rich_text_render;
} AnygmHostServices;

typedef uint32_t AnygmContentKind;
enum {
  ANYGM_CONTENT_PATH=1,
  ANYGM_CONTENT_MEMORY=2
};

typedef struct AnygmContentSource {
  uint32_t struct_size;
  AnygmContentKind kind;
  const char *path;
  const void *data;
  size_t size;
  const char *cache_directory;
  const char *save_directory;
  const char *system_directory;
} AnygmContentSource;

/* Path sources are opened during anygm_load. Memory sources are borrowed and must remain valid
 * until anygm_unload or anygm_destroy; the optional path is an identity and directory hint.
 * cache_directory, save_directory and system_directory are roots in the host VFS.
 * system_directory optionally supplies AnyGM.ini for content transforms and overrides,
 * including SHA-256 sections selected by the original path or memory payload bytes.
 * When a save root is present,
 * writable content files live below save_directory/AnyGM/<sanitized-label>-<path-hash>. */

/* Locale strings are what os_get_language and its relatives expose: lowercase language ("en"),
 * uppercase region ("US"), and a fuller tag ("en-US"). Content loaded without a configuration
 * takes the locale service's answer and asks again on every anygm_reset; an explicit configuration
 * pins the values for the lifetime of the load. */
typedef struct AnygmLoadConfig {
  uint32_t struct_size;
  uint32_t flags;
  const char *language;
  const char *region;
  const char *language_tag;
} AnygmLoadConfig;

/* Facts discovered while preparing content, before any extension, room or instance event runs.
 * A host may use them to negotiate facilities whose lifetime has to cover the whole session. */
typedef struct AnygmContentInfo {
  uint32_t struct_size;
  uint32_t flags;
} AnygmContentInfo;

enum {
  /* The payload contains at least one valid GLSL program that the software renderer does not
   * recognize. This says the program may be used, not that later game code will select it. */
  ANYGM_CONTENT_GLSL_DEVICE_CANDIDATE=1u<<0
};

typedef struct AnygmConfig {
  uint32_t struct_size;
  /* Optional virtual-monitor dimensions reported to content. Zero selects the runtime fallback.
   * These values do not resize the content's window or the framebuffer delivered to the host. */
  uint32_t monitor_width;
  uint32_t monitor_height;
  /* Optional forced shape for the delivered raster. Zero keeps the shape the content draws. It
   * reshapes the logical raster, so it belongs with present_logical_raster and excludes the
   * virtual-monitor dimensions above: a host that sizes a presentation window has already stated
   * the shape, and the two together describe no single frame. */
  uint32_t aspect_mode;
  uint32_t mouse_mode;
  uint32_t room_skip_button;
  uint32_t god_mode;
  /* 0 routes RetroPad input through keyboard mappings; 1 reports a connected pad
   * unconditionally. AUTO reports a pad only when the content references input
   * functions or events that read one. The public option offers 0 and AUTO. */
  uint32_t gamepad_connected;
  uint32_t fast_alpha_cull;
  uint32_t fast_forward;
  int32_t start_room;
  /* Rasterize at the logical view extent and let the host scale, instead of rasterizing at the
   * window extent the content requested. A window larger than the view carries no extra detail
   * for a software renderer: the pixels are a nearest upscale the host performs anyway. */
  uint32_t present_logical_raster;
  /* With the logical raster selected, fit a completed image wider than 364 or taller than 244
   * into 640x480 using sharp bilinear and black margins. Test the extent after aspect forcing.
   * This host presentation policy is neither simulation state nor state identity. */
  uint32_t adjust_crt_tv;
  /* Delete this content's generated local data (saves, extracted archive cache) and reload. */
  uint32_t clear_local_data;
  /* How shader_is_compiled answers for content shaders. Non-zero reports valid sampling shaders
   * in the payload available, while an unsupported effect draws plain. Zero reports only
   * software-recognized families as available, so authored fallback paths can be selected.
   * Procedural shaders that sample no picture remain unavailable without a device under either
   * policy. The host default is non-zero. */
  uint32_t report_all_shaders_compiled;
  /* Apply the override directives the loaded content carried in its anchor file. Enabled by
   * default; disabling ignores this channel without changing host cheats. */
  uint32_t content_overrides;
  /* How far to go running the content's own shaders on a graphics device for draws that are not
   * the frame's last operation. Such a draw is executed off-screen and read back, and the read
   * back stalls the device, so the cost is the device's rather than the picture's:
   * ANYGM_SHADER_READBACK_BUDGETED measures the first frames and stops when they cost more than
   * a frame can afford, ANYGM_SHADER_READBACK_ALWAYS keeps going however slow, and
   * ANYGM_SHADER_READBACK_NEVER draws them unshaded as a build without a device does. */
  uint32_t content_shader_readback;
  /* Whether content GLSL may use the host graphics device. Set this only after content inspection
   * and context negotiation; shader support queries made by the first game events then see the
   * final session policy. */
  uint32_t content_shader_device_expected;
  /* Whether eligible final scaling and composition passes may run on the host graphics device.
   * This is independent of content_shader_device_expected: either policy may use a context alone,
   * and both share it when enabled together. */
  uint32_t hybrid_gpu_presentation;
} AnygmConfig;

/* Zero is the default a host that never sets the field gets, so it is the measured one rather than
 * the one that turns the feature off. */
enum {
  ANYGM_SHADER_READBACK_BUDGETED=0,
  ANYGM_SHADER_READBACK_ALWAYS=1,
  ANYGM_SHADER_READBACK_NEVER=2
};

typedef struct AnygmConfigDelta {
  uint32_t struct_size;
  uint64_t fields;
  AnygmConfig values;
} AnygmConfigDelta;

enum {
  ANYGM_CONFIG_MONITOR_WIDTH=1ull<<0,
  ANYGM_CONFIG_MONITOR_HEIGHT=1ull<<1,
  ANYGM_CONFIG_ASPECT_MODE=1ull<<2,
  ANYGM_CONFIG_MOUSE_MODE=1ull<<3,
  ANYGM_CONFIG_ROOM_SKIP_BUTTON=1ull<<4,
  ANYGM_CONFIG_GOD_MODE=1ull<<5,
  ANYGM_CONFIG_GAMEPAD_CONNECTED=1ull<<12,
  ANYGM_CONFIG_FAST_ALPHA_CULL=1ull<<13,
  ANYGM_CONFIG_FAST_FORWARD=1ull<<14,
  ANYGM_CONFIG_START_ROOM=1ull<<15,
  ANYGM_CONFIG_PRESENT_LOGICAL_RASTER=1ull<<16,
  ANYGM_CONFIG_CLEAR_LOCAL_DATA=1ull<<17,
  ANYGM_CONFIG_REPORT_ALL_SHADERS_COMPILED=1ull<<18,
  ANYGM_CONFIG_CONTENT_OVERRIDES=1ull<<19,
  ANYGM_CONFIG_CONTENT_SHADER_READBACK=1ull<<20,
  ANYGM_CONFIG_CONTENT_SHADER_DEVICE_EXPECTED=1ull<<21,
  ANYGM_CONFIG_HYBRID_GPU_PRESENTATION=1ull<<22,
  ANYGM_CONFIG_ADJUST_CRT_TV=1ull<<23
};

typedef struct AnygmInputFrame {
  uint32_t struct_size;
  uint32_t connected_gamepads;
  uint8_t gamepad_buttons[ANYGM_MAX_GAMEPADS][ANYGM_MAX_GAMEPAD_BUTTONS];
  float gamepad_axes[ANYGM_MAX_GAMEPADS][ANYGM_MAX_GAMEPAD_AXES];
  uint8_t keys[ANYGM_MAX_KEYS];
  int32_t pointer_x;
  int32_t pointer_y;
  int32_t mouse_delta_x;
  int32_t mouse_delta_y;
  int32_t wheel_delta;
  uint8_t mouse_buttons[3];
  uint8_t pointer_pressed;
} AnygmInputFrame;

typedef uint32_t AnygmPixelFormat;
enum {
  ANYGM_PIXEL_XRGB8888=1
};

typedef struct AnygmFrameOutput {
  uint32_t struct_size;
  const void *pixels;
  uint32_t width;
  uint32_t height;
  size_t pitch;
  AnygmPixelFormat pixel_format;
  const int16_t *audio;
  size_t audio_frames;
  uint32_t audio_rate;
  uint32_t flags;
} AnygmFrameOutput;

enum {
  ANYGM_FRAME_GEOMETRY_CHANGED=1u<<0,
  ANYGM_FRAME_TIMING_CHANGED=1u<<1,
  ANYGM_FRAME_SHUTDOWN_REQUESTED=1u<<2,
  /* The frame was rendered into the host graphics target supplied through
   * anygm_graphics_context_reset rather than into a CPU buffer. `pixels` may then be null and
   * `width`/`height` still describe the rendered area, so a host that announced a hardware target
   * presents that target instead of the pixel view. Without this flag a CPU frame is always
   * available, and the two are never both authoritative for the same frame. */
  ANYGM_FRAME_HARDWARE_TARGET=1u<<3
};

/* Optional host graphics target.
 *
 * A host that owns a graphics context may lend it to the engine. The engine renders eligible final
 * passes into that context's current framebuffer and reports it through
 * ANYGM_FRAME_HARDWARE_TARGET; everything else keeps the software renderer and the CPU frame. No
 * graphics API type appears here: the host resolves entry points itself through the callback it
 * supplies, and identifies the target by an opaque handle whose meaning belongs to the chosen API.
 * The context and its host-selected execution policies are transport choices rather than emulated
 * state: neither enters the state configuration fingerprint or any serialized section. */
typedef uint32_t AnygmGraphicsApi;
enum {
  ANYGM_GRAPHICS_OPENGL_CORE=1,
  ANYGM_GRAPHICS_OPENGLES3=2
};

typedef void (*AnygmGraphicsProc)(void);
typedef AnygmGraphicsProc (*AnygmGraphicsGetProcAddressFn)(void *userdata,const char *name);
typedef uintptr_t (*AnygmGraphicsGetFramebufferFn)(void *userdata);

typedef struct AnygmGraphicsContext {
  uint32_t struct_size;
  AnygmGraphicsApi api;
  uint32_t version_major;
  uint32_t version_minor;
  void *userdata;
  AnygmGraphicsGetProcAddressFn get_proc_address;
  /* Queried once per rendered frame. A host is free to hand over a different target each time. */
  AnygmGraphicsGetFramebufferFn get_current_framebuffer;
} AnygmGraphicsContext;

/* Adopt a host graphics context, or adopt a recreated one. Valid before or after content is
 * loaded, and valid repeatedly without an intervening destroy: a reset with no destroy before it
 * means the previous context is already gone, so previously created objects are forgotten rather
 * than deleted against the new one. Returns ANYGM_ERROR_UNSUPPORTED when the context cannot be
 * used, and the engine remains fully usable in software either way. */
AnygmResult anygm_graphics_context_reset(AnygmEngine *engine,const AnygmGraphicsContext *context);

/* Release everything the engine created in the host context. Objects are deleted only when
 * `context_is_current` says the context is still current on the calling thread; otherwise the
 * handles are forgotten and only CPU-side records are released. Destroying an engine without a
 * current context is safe and issues no graphics call. */
void anygm_graphics_context_destroy(AnygmEngine *engine,uint32_t context_is_current);

typedef struct AnygmAvInfo {
  uint32_t struct_size;
  uint32_t base_width;
  uint32_t base_height;
  uint32_t max_width;
  uint32_t max_height;
  double aspect_ratio;
  double frames_per_second;
  uint32_t audio_rate;
} AnygmAvInfo;

uint32_t anygm_api_version(void);
AnygmResult anygm_create(const AnygmHostServices *services,AnygmEngine **engine);
void anygm_destroy(AnygmEngine *engine);
/* Prepare and validate content without running extension, room or instance events. The renderer's
 * shader catalogue is available in `info`, allowing a host to request a session-long graphics
 * context only when the payload contains a candidate. Call anygm_load_start after the context
 * decision is final. A prepared load may be abandoned with anygm_unload or anygm_destroy. */
AnygmResult anygm_load_prepare(AnygmEngine *engine,const AnygmContentSource *source,
                               const AnygmLoadConfig *config,AnygmContentInfo *info);
AnygmResult anygm_load_start(AnygmEngine *engine);
/* One-shot form for hosts that need no negotiation between preparation and startup. */
AnygmResult anygm_load(AnygmEngine *engine,const AnygmContentSource *source,
                       const AnygmLoadConfig *config);
void anygm_unload(AnygmEngine *engine);
AnygmResult anygm_reset(AnygmEngine *engine);
AnygmResult anygm_get_av_info(const AnygmEngine *engine,AnygmAvInfo *info);
/* Rooms of the loaded content, in the order the content declares them. A host presenting a
 * room chooser has no other way to name what it is offering, and the index it collects is the
 * one ANYGM_CONFIG_START_ROOM takes. Both calls require loaded content.
 *
 * anygm_get_room_name writes a NUL-terminated name, truncating to capacity. */
AnygmResult anygm_get_room_count(const AnygmEngine *engine,uint32_t *count);
AnygmResult anygm_get_room_name(const AnygmEngine *engine,uint32_t index,
                                char *name,size_t capacity);
AnygmResult anygm_run_frame(AnygmEngine *engine,const AnygmInputFrame *input,
                            AnygmFrameOutput *output);
AnygmResult anygm_set_config(AnygmEngine *engine,const AnygmConfigDelta *delta);
/* Runtime expressions are frontend input, not anchor declarations. `set|object|variable|value`
 * immediately writes every matching live instance when enabled; it is not repeated, restored on
 * disable, or serialized as active policy. */
AnygmResult anygm_set_runtime_override(AnygmEngine *engine,uint32_t slot,uint32_t enabled,
                                       const char *expression);
size_t anygm_state_size(AnygmEngine *engine);
/* The exact size of the same canonical state without its optional completed-frame section. A host
 * that resumes simulation from frequent in-memory snapshots can provision this smaller form. It
 * loads through anygm_state_load and the next run continues from the restored post-frame state. */
size_t anygm_state_resume_size(AnygmEngine *engine);
/* The capacity advised for frame-free states: at least the largest size remembered from an earlier
 * session, raised by mutable render allocations whose dimensions are already known at load. It is
 * a bounded advisory hint, just like anygm_state_capacity_hint; the current exact requirement is
 * always anygm_state_resume_size. */
size_t anygm_state_resume_capacity_hint(const AnygmEngine *engine);
enum {
  ANYGM_STATE_CAPACITY_DYNAMIC_SURFACES = 1u << 0,
  ANYGM_STATE_CAPACITY_DYNAMIC_CONTAINERS = 1u << 1
};
/* Content references which make cold render or container allocations an incomplete growth estimate. These are
 * conservative risk flags (including dynamic source execution or payload replacement), not a
 * bound on arbitrary future language allocations. A host choosing
 * a smaller fixed reserve must account for surfaces and containers created after the first frame. */
uint32_t anygm_state_capacity_flags(const AnygmEngine *engine);
/* The capacity the engine advises a host to provision for complete states: at least the completed
 * frame's ceiling under the current output geometry, raised by the largest serialized size this
 * content is known to have reached in earlier sessions and remembered as a disposable cache entry
 * in the content's writable namespace. A hint is advisory: the exact size of the current complete
 * state is always anygm_state_size. */
size_t anygm_state_capacity_hint(const AnygmEngine *engine);
AnygmResult anygm_state_save(AnygmEngine *engine,void *data,size_t capacity,size_t *written);
/* Writes the canonical state without its optional completed frame. This is the explicit form of
 * the fallback anygm_state_save already uses when the supplied capacity can hold every required
 * section but not that frame. */
AnygmResult anygm_state_save_for_resume(AnygmEngine *engine,void *data,size_t capacity,
                                        size_t *written);
AnygmResult anygm_state_load(AnygmEngine *engine,const void *data,size_t size);
size_t anygm_get_last_error(const AnygmEngine *engine,char *message,size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
