/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_CONTENT_TRANSFORM_H
#define ANYGM_CONTENT_TRANSFORM_H

#include <stddef.h>
#include <stdint.h>

#define ANYGM_TRANSFORM_MAX_CONFIG_BYTES 524288u
#define ANYGM_TRANSFORM_MAX_PROGRAM_BYTES 12288u
#define ANYGM_TRANSFORM_MAX_PARAMETER_BYTES 4096u
#define ANYGM_TRANSFORM_MAX_PROGRAMS 16u
#define ANYGM_TRANSFORM_MAX_PIPELINE_STEPS 16u
#define ANYGM_TRANSFORM_SCRATCH_BYTES 65536u
#define ANYGM_TRANSFORM_MAX_INPUT_BYTES UINT64_C(1073741824)
#define ANYGM_TRANSFORM_MAX_STEPS (UINT64_C(1000000)+ANYGM_TRANSFORM_MAX_INPUT_BYTES*128u)
#define ANYGM_TRANSFORM_INSTRUCTION_BYTES 12u

/* The internal instruction representation is twelve bytes: opcode, destination, left, right,
 * followed by an unsigned little-endian 64-bit immediate. See CONTENT_TRANSFORMS.md.
 * None of these instructions performs a format-specific operation. */
enum {
  ANYGM_TRANSFORM_RETURN=0, ANYGM_TRANSFORM_REJECT=1,
  ANYGM_TRANSFORM_CONSTANT=2, ANYGM_TRANSFORM_MOVE=3,
  ANYGM_TRANSFORM_ADD=4, ANYGM_TRANSFORM_SUBTRACT=5,
  ANYGM_TRANSFORM_MULTIPLY=6, ANYGM_TRANSFORM_DIVIDE=7,
  ANYGM_TRANSFORM_REMAINDER=8, ANYGM_TRANSFORM_AND=9,
  ANYGM_TRANSFORM_OR=10, ANYGM_TRANSFORM_XOR=11,
  ANYGM_TRANSFORM_SHIFT_LEFT=12, ANYGM_TRANSFORM_SHIFT_RIGHT=13,
  ANYGM_TRANSFORM_EQUAL=14, ANYGM_TRANSFORM_LESS=15,
  ANYGM_TRANSFORM_LOAD8=16, ANYGM_TRANSFORM_LOAD32=17,
  ANYGM_TRANSFORM_LOAD64=18, ANYGM_TRANSFORM_STORE8=19,
  ANYGM_TRANSFORM_STORE32=20, ANYGM_TRANSFORM_STORE64=21,
  ANYGM_TRANSFORM_JUMP=22, ANYGM_TRANSFORM_JUMP_ZERO=23,
  ANYGM_TRANSFORM_JUMP_NONZERO=24
};

typedef struct AnygmContentTransforms AnygmContentTransforms;

/* Empty sets contain no implicit programs. Parsing [transforms] is transactional:
 * a malformed declaration leaves the existing set unchanged. One declaration is
 * name=buffer function_name() { ... } in a bounded C-like language. Declarations
 * replace whole entries, never mix parameters from different configuration layers. */
AnygmContentTransforms *anygm_content_transforms_create(void);
void anygm_content_transforms_destroy(AnygmContentTransforms *set);
/* Transactionally copy the complete bounded program set. */
int anygm_content_transforms_copy(AnygmContentTransforms *target,const AnygmContentTransforms *source);
int anygm_content_transforms_parse(AnygmContentTransforms *set,const void *text,size_t size,
                                   char *error,size_t error_size);
/* Higher-priority declarations survive later lower-priority layers. Equal priority
 * replaces an entry. Priority affects selection only, not the resulting identity. */
int anygm_content_transforms_parse_layer(AnygmContentTransforms *set,const void *text,size_t size,
                                         unsigned priority,char *error,size_t error_size);
int anygm_content_transforms_has(const AnygmContentTransforms *set,const char *name);
/* Validate a complete expansion before executing any leaf. Pipeline references
 * may be forward-declared or supplied by another configuration layer. */
int anygm_content_transform_validate(const AnygmContentTransforms *set,const char *name,
                                      char *error,size_t error_size);
void anygm_content_transforms_hash(const AnygmContentTransforms *set,uint8_t digest[32]);
int anygm_content_transform_run(const AnygmContentTransforms *set,const char *name,
                                 const void *input,size_t input_size,
                                 uint8_t **output,size_t *output_size,
                                 char *error,size_t error_size);

/* Execute one program with no host capabilities. The optional lower step limit is
 * useful to callers with tighter scheduling requirements; zero selects the default.
 * Output is independently owned, and input/parameters are never modified. On every
 * failure output is NULL and output_size is zero. */
int anygm_content_transform_execute(const void *program,size_t program_size,
                                     const void *parameters,size_t parameter_size,
                                     const void *input,size_t input_size,uint64_t step_limit,
                                     uint8_t **output,size_t *output_size,
                                     char *error,size_t error_size);

#endif
