/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/*
 * Opaque canonical-state cursor operations shared only by the VM payload
 * owner and the builtin resource-state owner.  Cursor representation, value
 * encoding, framing, and transactional restore remain owned by
 * gml_vm_state.c.
 */
#ifndef GML_VM_STATE_CODEC_H
#define GML_VM_STATE_CODEC_H

#include "gml_value.h"

#include <stddef.h>
#include <stdint.h>

typedef struct GmlVmStateWriter GmlVmStateWriter;
typedef struct GmlVmStateReader GmlVmStateReader;
struct GmlVM;

typedef struct {
  const char *key;
  GmlVal key_value, value;
  uint64_t *encoding;
  unsigned char **compressed;
  uint32_t *compressed_size;
  unsigned char *compressed_ready;
  int child_kind;
} GmlVmStateScalarMapEntry;

void gml_vm_state_write_raw(GmlVmStateWriter *writer,
                            const void *bytes, size_t size);
void gml_vm_state_write_u32(GmlVmStateWriter *writer, uint32_t value);
void gml_vm_state_write_i32(GmlVmStateWriter *writer, int value);
void gml_vm_state_write_real(GmlVmStateWriter *writer, double value);
void gml_vm_state_write_string(GmlVmStateWriter *writer, const char *value);
void gml_vm_state_write_value(GmlVmStateWriter *writer, GmlVal value);
/* A zero-initialized memo is owned by one immutable string in this VM's content lifetime. */
void gml_vm_state_write_owned_string(GmlVmStateWriter *writer, const char *value,
                                     uint64_t *encoding, uint64_t *hash,
                                     unsigned char *hash_valid,
                                     unsigned char **compressed,
                                     uint32_t *compressed_size,
                                     unsigned char *compressed_ready);
void gml_vm_state_write_owned_value(GmlVmStateWriter *writer, GmlVal value,
                                    uint64_t *encoding, uint64_t *hash,
                                    unsigned char *hash_valid,
                                    unsigned char **compressed,
                                    uint32_t *compressed_size,
                                    unsigned char *compressed_ready);
int gml_vm_state_build_scalar_map_blob(struct GmlVM *vm,
                                       const GmlVmStateScalarMapEntry *entries,
                                       uint32_t count, unsigned char **bytes,
                                       uint32_t *size);
int gml_vm_state_build_scalar_grid_blob(struct GmlVM *vm, const GmlVal *values,
                                        uint32_t count, unsigned char **bytes,
                                        uint32_t *size);
void gml_vm_state_read_raw(GmlVmStateReader *reader,
                           void *bytes, size_t size);
uint32_t gml_vm_state_read_u32(GmlVmStateReader *reader);
int gml_vm_state_read_i32(GmlVmStateReader *reader);
double gml_vm_state_read_real(GmlVmStateReader *reader);
char *gml_vm_state_read_string(GmlVmStateReader *reader);
GmlVal gml_vm_state_read_value(GmlVmStateReader *reader);
char *gml_vm_state_read_isolated_string(GmlVmStateReader *reader);
GmlVal gml_vm_state_read_isolated_value(GmlVmStateReader *reader);
/* One means a scalar blob was consumed, zero means the general marker was consumed, and minus one
 * means malformed input. A successful scalar read retains the exact wire bytes for the next save. */
int gml_vm_state_read_scalar_grid_blob(GmlVmStateReader *reader, GmlVal *values,
                                       uint32_t count, unsigned char **bytes,
                                       uint32_t *size);

int gml_vm_state_reader_ok(const GmlVmStateReader *reader);
size_t gml_vm_state_reader_position(const GmlVmStateReader *reader);
size_t gml_vm_state_reader_remaining(const GmlVmStateReader *reader);
int gml_vm_state_reader_copy_range(const GmlVmStateReader *reader,
                                   size_t start, size_t end,
                                   unsigned char **bytes, uint32_t *size);
void gml_vm_state_reader_fail(GmlVmStateReader *reader,
                              const char *message, uint32_t detail);
size_t gml_vm_state_measure_string(struct GmlVM *vm, const char *value);
size_t gml_vm_state_measure_value(struct GmlVM *vm, GmlVal value);

#endif
