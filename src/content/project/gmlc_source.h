/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GMLC_SOURCE_H
#define GMLC_SOURCE_H

#include "gmlc_project.h"

typedef struct {
  int files;
  int missing_files;
  int nonempty_files;
  int macros;
  int ifs, loops, switches, withs, returns, calls, arrays;
} GmlcSourceReport;

int gmlc_source_scan_project(const GmlcProject *p, GmlcSourceReport *out, char *err, size_t errcap);
void gmlc_source_log_report(const AnygmHostServices *host,const GmlcSourceReport *r);

#endif
