/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GMLC_PACKAGE_H
#define GMLC_PACKAGE_H

#include "gmlc_project.h"
#include <stddef.h>

int gmlc_package_write_structural(const GmlcProject *p, const char *out_path, char *err, size_t errcap);

#endif
