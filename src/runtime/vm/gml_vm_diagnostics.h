/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_GML_VM_DIAGNOSTICS_H
#define ANYGM_GML_VM_DIAGNOSTICS_H

#include "gml_vm.h"

#include <stdint.h>

void gml_vm_diagnostics_opcode(GmlVM *vm, const char *code_name,
                               uint32_t offset, const char *opcode,
                               int stack_depth, double stack_top);
void gml_vm_diagnostics_array_growth_init(GmlVM *vm);
void gml_vm_diagnostics_callv_stack(GmlVM *vm,int argc,int sp_in,int sp_out);
void gml_vm_diagnostics_opcode_after(GmlVM *vm, const char *code_name,
                                    uint32_t offset, int stack_depth);
int gml_vm_diagnostics_opcode_enabled(GmlVM *vm, const char *code_name);
void gml_vm_diagnostics_event(GmlVM *vm, const GmlInstance *instance,
                              const char *event_name, int code_index);
void gml_vm_diagnostics_collision(GmlVM *vm,
                                  const GmlInstance *first,
                                  const GmlInstance *second,
                                  int code_index, int hit);
void gml_vm_diagnostics_variable_scope(GmlVM *vm, int scope,
                                       const char *name, int array_index,
                                       GmlVal value);
void gml_vm_diagnostics_variable_instance(GmlVM *vm,
                                          const GmlInstance *instance,
                                          const char *name, int array_index,
                                          GmlVal value);
void gml_vm_diagnostics_destroy(GmlVM *vm);

#endif
