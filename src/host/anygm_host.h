/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_HOST_H
#define ANYGM_HOST_H

#include "anygm.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *anygm_host_development_setting(const AnygmHostServices *host,const char *name);
uint32_t anygm_host_capability(const AnygmHostServices *host,uint32_t capability);
uint64_t anygm_host_monotonic_time_ns(const AnygmHostServices *host);
void anygm_host_logf(const AnygmHostServices *host,AnygmLogLevel level,const char *format,...);
int anygm_calendar_from_unix_seconds(int64_t seconds,AnygmCalendarTime *calendar);

#ifdef __cplusplus
}
#endif

#endif
