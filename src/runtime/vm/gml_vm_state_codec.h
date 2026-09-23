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

void gml_vm_state_write_raw(GmlVmStateWriter *writer,
                            const void *bytes, size_t size);
void gml_vm_state_write_u32(GmlVmStateWriter *writer, uint32_t value);
void gml_vm_state_write_i32(GmlVmStateWriter *writer, int value);
void gml_vm_state_write_real(GmlVmStateWriter *writer, double value);
void gml_vm_state_write_string(GmlVmStateWriter *writer, const char *value);
void gml_vm_state_write_value(GmlVmStateWriter *writer, GmlVal value);
/* A zero-initialized memo is owned by one immutable string in this VM's content lifetime.
 * The owner must clear it when replacing that string. These derived answers are not state. */
void gml_vm_state_write_owned_string(GmlVmStateWriter *writer, const char *value,
                                     uint64_t *encoding);
void gml_vm_state_write_owned_value(GmlVmStateWriter *writer, GmlVal value,
                                    uint64_t *encoding);

void gml_vm_state_read_raw(GmlVmStateReader *reader,
                           void *bytes, size_t size);
uint32_t gml_vm_state_read_u32(GmlVmStateReader *reader);
int gml_vm_state_read_i32(GmlVmStateReader *reader);
double gml_vm_state_read_real(GmlVmStateReader *reader);
char *gml_vm_state_read_string(GmlVmStateReader *reader);
GmlVal gml_vm_state_read_value(GmlVmStateReader *reader);

int gml_vm_state_reader_ok(const GmlVmStateReader *reader);
size_t gml_vm_state_reader_position(const GmlVmStateReader *reader);
void gml_vm_state_reader_fail(GmlVmStateReader *reader,
                              const char *message, uint32_t detail);
size_t gml_vm_state_measure_string(struct GmlVM *vm, const char *value);
size_t gml_vm_state_measure_value(struct GmlVM *vm, GmlVal value);

#endif
