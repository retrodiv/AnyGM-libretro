/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GMLC_ASSETS_H
#define GMLC_ASSETS_H

#include "gmlc_project.h"

int gmlc_assets_load(GmlcProject *p, char *err, size_t errcap);
const char *gmlc_res_kind_name(GmlcResKind k);

#endif
