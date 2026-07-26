/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_TEST_RUNNER_H
#define ANYGM_TEST_RUNNER_H

#include <stddef.h>

typedef int (*AnygmTestFunction)(void);

typedef struct {
  const char *name;
  AnygmTestFunction function;
} AnygmTestCase;

typedef struct {
  const char *name;
  const AnygmTestCase *cases;
  size_t case_count;
} AnygmTestGroup;

typedef struct {
  int passed;
  int failed;
  size_t matched;
} AnygmTestResult;

int anygm_test_run_groups(const AnygmTestGroup *groups, size_t group_count,
                          const char *filter, AnygmTestResult *result);

#endif
