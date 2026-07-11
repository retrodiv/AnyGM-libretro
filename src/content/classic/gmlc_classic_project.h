/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#ifndef GMLC_CLASSIC_PROJECT_H
#define GMLC_CLASSIC_PROJECT_H

#include "gmlc_project.h"
#include <stddef.h>

int gmlc_classic_project_load(GmlcProject *project, const char *project_path,
                              const char *cache_dir, char *err, size_t errcap);

#endif
