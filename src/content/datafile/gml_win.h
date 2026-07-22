/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_win.h - loader + normalized bytecode interface for a GameMaker: Studio data.win.
 * The loader validates supported versioned layouts and exposes one normalized interface. */
#ifndef GML_WIN_H
#define GML_WIN_H
#include <stdint.h>
#include <stddef.h>

struct AnygmCompatibilityProfile;
struct AnygmHostServices;

/* Hard parser limits for one normalized content image. These are deliberately
 * independent of frontend transport and compatibility generation. */
#define GML_WIN_MAX_FILE_BYTES             ((size_t)1073741824u)
#define GML_WIN_MAX_CHUNKS                 40u
#define GML_WIN_MAX_STRINGS                8388608u
#define GML_WIN_MAX_STRING_BYTES           16777216u
#define GML_WIN_MAX_CODE_ENTRIES           1048576u
#define GML_WIN_MAX_REFERENCES             16777216u
#define GML_WIN_MAX_ROOMS                  1048576u
#define GML_WIN_MAX_ROOM_ORDER             1048576u
#define GML_WIN_MAX_CLASSIC_INFO_BYTES     8388608u

/* ---- normalized opcodes (bc14 old opcodes and bc15+ split opcodes decode into these) ---- */
enum {
  OP_CONV=0x07, OP_MUL=0x08, OP_DIV=0x09, OP_REM=0x0A, OP_MOD=0x0B, OP_ADD=0x0C,
  OP_SUB=0x0D, OP_AND=0x0E, OP_OR=0x0F, OP_XOR=0x10, OP_NEG=0x11, OP_NOT=0x12,
  OP_SHL=0x13, OP_SHR=0x14, OP_CMP=0x15, OP_POP=0x45, OP_DUP=0x86, OP_RET=0x9C,
  OP_EXIT=0x9D, OP_POPZ=0x9E, OP_B=0xB6, OP_BT=0xB7, OP_BF=0xB8, OP_PUSHENV=0xBA,
  OP_POPENV=0xBB, OP_PUSH=0xC0, OP_CALL=0xD9, OP_CALLV=0x99, OP_BREAK=0xFF
};
/* DataType */
enum { DT_DOUBLE=0, DT_FLOAT=1, DT_INT32=2, DT_INT64=3, DT_BOOL=4, DT_VAR=5,
       DT_STRING=6, DT_INT16=0x0F };
/* InstanceType */
enum { IT_SELF=-1, IT_OTHER=-2, IT_ALL=-3, IT_NOONE=-4, IT_GLOBAL=-5, IT_BUILTIN=-6,
       IT_LOCAL=-7, IT_STACK=-9, IT_ARG=-15, IT_STATIC=-16 };
/* ComparisonType */
enum { CMP_LT=1, CMP_LTE=2, CMP_EQ=3, CMP_NEQ=4, CMP_GTE=5, CMP_GT=6 };

typedef struct {
  uint8_t  kind;       /* new opcode */
  uint8_t  oldkind;    /* raw byte from file */
  uint8_t  type1, type2;
  uint8_t  cmp;        /* comparison kind (OP_CMP only) */
  int16_t  inst;       /* instance type (low16) for var/pop refs and pushvar */
  int32_t  jump;       /* word offset for goto: target_bytes = ia + jump*4 */
  uint16_t argc;       /* call argument count */
  /* immediate operand */
  double   dval;
  int32_t  ival;
  int64_t  lval;
  int16_t  sval;       /* int16 immediate */
  uint32_t strindex;   /* push.s string index */
  uint32_t refaddr;    /* absolute file addr of the reference word (var/func site) */
  uint8_t  reftype;    /* VariableType: Array=0x00, StackTop=0x80, Normal=0xA0 */
  uint8_t  size;       /* total bytes consumed */
  const char *refname; /* VM cache: gml_ref_name(refaddr), when applicable */
  uint32_t refhash;    /* VM cache: strhash(refname), when applicable */
  int32_t  funcval_ci; /* VM cache: GMS2.3 push.i32 function-value code index, -1 if none */
  int16_t  builtin_id; /* VM cache: hot builtin dispatch id; 0=unresolved, -1=generic path */
} GmlInsn;

typedef struct { char name[8]; uint32_t off, size; } GmlChunk;
typedef struct {
  const char *name;
  uint32_t start, length;
  /* Lazy decoded-bytecode cache. The VM keeps byte offsets for trace/refaddr semantics and
   * branch_index maps branch targets to decoded instruction indices, with n_insn meaning exit. */
  GmlInsn *insn;
  uint32_t *insn_pc;
  int32_t *branch_index;
  uint32_t n_insn;
  uint8_t cache_bad;
  /* VM-only micro-specialization for tiny wrapper scripts. Not serialized. */
  uint8_t micro_kind;
  const char *micro_name;
  uint32_t micro_hash;
} GmlCode;

typedef struct {
  const char *name; uint32_t width, height, speed;
  int persistent;
  uint32_t bgcolor;  /* 0xAARRGGBB-ish; GM stores 0xAABBGGRR, alpha forced 0xFF */
  int draw_bg;
  int creation_code;  /* CODE index for the room's creation code, -1 if none */
  uint32_t flags;     /* serialized room flags; bit 0 enables the legacy view system */
  int view_enabled;
  uint32_t bg_ptr, view_ptr, obj_ptr, tile_ptr;
} GmlRoom;

typedef struct GmlWin {
  /* data storage: owns=0 borrowed memory, owns=1 malloc, owns=2 host-owned read-only mapping. */
  uint8_t *data; size_t size; int owns;
  void *mapping_handle;
  GmlChunk chunks[GML_WIN_MAX_CHUNKS]; int n_chunks;
  /* strings, in STRG order */
  char   **strs;  uint32_t *str_charoff; int n_strs;
  /* content-hash index over strs (lazy; for O(1) intern lookups) */
  int32_t *str_hix; uint32_t str_hix_cap;
  /* code entries */
  GmlCode *code;  int n_code;
  /* content-hash index over code names (lazy; for O(1) exact code lookup) */
  int32_t *code_hix; uint32_t code_hix_cap;
  /* reference name map: addr -> name (sorted by addr) */
  uint32_t *ref_addr; const char **ref_name; int n_refs;
  /* address-hash index over ref_addr/ref_name (lazy; for O(1) ref lookup) */
  int32_t *ref_hix; uint32_t ref_hix_cap;
  /* header */
  uint8_t bytecode; uint32_t gameid; int classic_version;
  int classic_scaling, classic_interpolate, classic_swap_creation_events;
  uint32_t classic_outside_color;
  const uint8_t *classic_game_information;
  size_t classic_game_information_size;
  double game_speed;                     /* GEN8 global cadence when present (GMS2 exports) */
  uint32_t disp_w, disp_h;           /* default window / native render size */
  uint32_t *room_order; int n_room_order;
  char content_dir[512];             /* directory containing the loaded data.win */
  char save_dir[512];                /* per-content writable sandbox supplied by the frontend */
  /* Keep newly parsed metadata after the historical save-path tail: external savestates and
   * helper binaries built against an older header retain all pre-existing field offsets. */
  uint64_t option_flags;             /* OPTN flags when the package uses the flagged layout */
  int classic_executable_layout;     /* CLSC tail: embedded rooms retain encoded chain order */
  const struct AnygmCompatibilityProfile *compatibility; /* immutable runtime policy, engine-owned */
  const struct AnygmHostServices *host; /* borrowed immutable service table, engine-owned */
} GmlWin;

int          gml_win_load_host(GmlWin *w,const struct AnygmHostServices *host,const char *path);
int          gml_win_from_mem(GmlWin *w, uint8_t *data, size_t size, int take_ownership);
void         gml_win_free(GmlWin *w);
const GmlChunk *gml_chunk(const GmlWin *w, const char *name);
const char  *gml_str_by_index(const GmlWin *w, uint32_t idx);
const char  *gml_str_by_ptr(const GmlWin *w, uint32_t fileoff);
const char  *gml_win_intern_lookup(GmlWin *w, const char *s);   /* O(1) STRG content lookup, NULL if absent */
const char  *gml_ref_name(const GmlWin *w, uint32_t addr);
int          gml_room_count(const GmlWin *w);
int          gml_room_get(const GmlWin *w, int room_index, GmlRoom *out);

/* The low-level decoder requires enough readable operand bytes for the encoded
 * instruction. Content-derived callers use the bounded entry point. */
int          gml_decode_bc(const uint8_t *d, uint32_t ia, uint8_t bytecode, GmlInsn *out);
int          gml_decode_bc_bounded(const uint8_t *d, size_t size, uint32_t ia,
                                   uint8_t bytecode, GmlInsn *out);
int          gml_decode(const uint8_t *d, uint32_t ia, GmlInsn *out);
const char  *gml_op_mnemonic(uint8_t newkind);

#endif
