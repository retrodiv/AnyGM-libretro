/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm_host.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *anygm_host_development_setting(const AnygmHostServices *host,const char *name){
  if(!host || !host->development_setting || !name || !name[0]) return NULL;
  return host->development_setting(host->userdata,name);
}

uint32_t anygm_host_capability(const AnygmHostServices *host,uint32_t capability){
  return host&&host->capability?host->capability(host->userdata,capability):0;
}

uint64_t anygm_host_monotonic_time_ns(const AnygmHostServices *host){
  return host&&host->monotonic_time_ns?host->monotonic_time_ns(host->userdata):0;
}

void anygm_host_logf(const AnygmHostServices *host,AnygmLogLevel level,const char *format,...){
  if(!host || !host->log || !format) return;
  char message[2048];
  va_list args;
  va_start(args,format);
  vsnprintf(message,sizeof message,format,args);
  va_end(args);
  host->log(host->userdata,level,message);
}

int anygm_calendar_from_unix_seconds(int64_t seconds,AnygmCalendarTime *calendar){
  if(!calendar) return 0;
  int64_t unix_day=seconds/86400;
  int64_t day_seconds=seconds%86400;
  if(day_seconds<0){
    day_seconds+=86400;
    unix_day--;
  }
  int64_t z=unix_day+719468;
  int64_t era=(z>=0?z:z-146096)/146097;
  unsigned day_of_era=(unsigned)(z-era*146097);
  unsigned year_of_era=(day_of_era-day_of_era/1460+day_of_era/36524-
                        day_of_era/146096)/365;
  int64_t year=(int64_t)year_of_era+era*400;
  unsigned day_of_year=day_of_era-(365*year_of_era+year_of_era/4-year_of_era/100);
  unsigned month_phase=(5*day_of_year+2)/153;
  unsigned day=day_of_year-(153*month_phase+2)/5+1;
  int month=(int)month_phase+(month_phase<10?3:-9);
  year+=month<=2;
  if(year<INT32_MIN||year>INT32_MAX) return 0;
  int weekday=(int)((unix_day+4)%7);
  if(weekday<0) weekday+=7;
  memset(calendar,0,sizeof *calendar);
  calendar->struct_size=sizeof *calendar;
  calendar->year=(int32_t)year;
  calendar->month=month;
  calendar->day=(int32_t)day;
  calendar->weekday=weekday;
  calendar->hour=(int32_t)(day_seconds/3600);
  calendar->minute=(int32_t)((day_seconds/60)%60);
  calendar->second=(int32_t)(day_seconds%60);
  return 1;
}
