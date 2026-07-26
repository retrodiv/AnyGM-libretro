/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm_test_runner.h"

#include <stdio.h>
#include <string.h>

int anygm_test_run_groups(const AnygmTestGroup *groups, size_t group_count,
                          const char *filter, AnygmTestResult *result){
  if(!groups || !result) return 0;
  memset(result,0,sizeof *result);
  for(size_t group_index=0;group_index<group_count;++group_index){
    const AnygmTestGroup *group=&groups[group_index];
    for(size_t case_index=0;case_index<group->case_count;++case_index){
      const AnygmTestCase *test_case=&group->cases[case_index];
      char full_name[256];
      int written=snprintf(full_name,sizeof full_name,"%s.%s",
                           group->name,test_case->name);
      if(written<0 || (size_t)written>=sizeof full_name){
        fprintf(stderr,"test case name is too long: %s / %s\n",
                group->name,test_case->name);
        ++result->failed;
        continue;
      }
      if(filter && !strstr(full_name,filter)) continue;
      ++result->matched;
      if(test_case->function()) ++result->passed;
      else {
        fprintf(stderr,"%s failed\n",full_name);
        ++result->failed;
      }
    }
  }
  if(filter && !result->matched){
    fprintf(stderr,"no test case matches filter: %s\n",filter);
    ++result->failed;
  }
  return result->failed==0;
}
