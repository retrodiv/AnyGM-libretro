/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "content_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int source_error(char *error,size_t size,const char *message){
  if(error && size) snprintf(error,size,"input preparation: %s",message);
  return 0;
}

static uint64_t source_u64(const uint8_t *bytes){
  uint64_t value=0;
  for(unsigned i=0;i<8u;i++) value|=(uint64_t)bytes[i]<<(i*8u);
  return value;
}

int anygm_content_source_configured(const AnygmContentTransforms *transforms){
  return anygm_content_transforms_has(transforms,"input") ||
    anygm_content_transforms_has(transforms,"input.probe") ||
    anygm_content_transforms_has(transforms,"input.final");
}

static int bytes_changed(const void *before,size_t before_size,const void *after,size_t after_size){
  return before_size!=after_size || (before_size && memcmp(before,after,before_size)!=0);
}

static int prepare_selected_source(const AnygmContentTransforms *transforms,
                                  const void *data,size_t size,
                                  AnygmContentSourceValidator validate,void *context,
                                  AnygmContentSourceScope *scope,int *scoped_selection,
                                  uint8_t **output,size_t *output_size,
                                  char *error,size_t error_size){
  if(error && error_size) error[0]=0;
  if(output) *output=NULL;
  if(output_size) *output_size=0;
  if(!output || !output_size || (size && !data))
    return source_error(error,error_size,"invalid arguments");
  if(!anygm_content_transforms_has(transforms,"input") &&
     !anygm_content_transforms_has(transforms,"input.probe")) return 1;
  if(!anygm_content_transforms_has(transforms,"input.probe")){
    if(scope) *scoped_selection=anygm_content_transforms_has_priority(transforms,"input",scope->priority);
    return anygm_content_transform_run(transforms,"input",data,size,NULL,0,
      output,output_size,error,error_size);
  }
  int scoped_probe=scope &&
    anygm_content_transforms_has_priority(transforms,"input.probe",scope->priority);
  if(!validate) return source_error(error,error_size,"candidate selection requires a validator");
  uint8_t *table=NULL,*selected=NULL;
  size_t table_size=0,selected_size=0;
  if(!anygm_content_transform_run(transforms,"input.probe",data,size,NULL,0,
       &table,&table_size,error,error_size)) return 0;
  const size_t stride=ANYGM_SOURCE_CANDIDATE_BYTES;
  if(table_size%stride || table_size/stride>ANYGM_SOURCE_MAX_CANDIDATES){
    free(table);
    return source_error(error,error_size,"malformed or oversized candidate table");
  }
  size_t count=table_size/stride;
  uint64_t previous=0;
  for(size_t i=0;i<count;i++){
    const uint8_t *record=table+i*stride;
    uint64_t offset=source_u64(record),length=source_u64(record+8u);
    if(offset>size || length>size-offset || (i && offset<previous)){
      free(table);
      return source_error(error,error_size,"invalid candidate range or ordering");
    }
    size_t end=16u;
    while(end<stride && record[end]) end++;
    if(end==stride){
      free(table); return source_error(error,error_size,"unterminated candidate operation");
    }
    for(size_t pad=end;pad<stride;pad++) if(record[pad]){
      free(table); return source_error(error,error_size,"nonzero candidate operation padding");
    }
    for(size_t earlier=0;earlier<i;earlier++) if(!memcmp(record,table+earlier*stride,stride)){
      free(table); return source_error(error,error_size,"duplicate candidate record");
    }
    const char *operation=record[16]?(const char*)record+16u:"input";
    if(!anygm_content_transform_validate(transforms,operation,error,error_size)){
      free(table); return 0;
    }
    previous=offset;
  }
  char last_error[256]={0};
  for(size_t i=0;i<count;i++){
    const uint8_t *record=table+i*stride;
    size_t offset=(size_t)source_u64(record),length=(size_t)source_u64(record+8u);
    const char *operation=record[16]?(const char*)record+16u:"input";
    uint8_t *candidate=NULL; size_t candidate_size=0;
    const uint8_t *start=data?(const uint8_t*)data+offset:NULL;
    char detail[256]={0};
    if(!anygm_content_transform_run(transforms,operation,start,length,NULL,0,
         &candidate,&candidate_size,detail,sizeof detail)){
      snprintf(last_error,sizeof last_error,"%s",detail);
      continue;
    }
    if(count==1u && !offset && length==size && candidate_size==size &&
       (!size || !memcmp(candidate,data,size))){
      free(candidate); free(table); return 1;
    }
    int valid=validate(context,candidate,candidate_size,detail,sizeof detail);
    if(valid<0){
      free(candidate); free(selected); free(table);
      return source_error(error,error_size,detail[0]?detail:"candidate validation failed");
    }
    if(valid){
      if(selected){
        free(candidate); free(selected); free(table);
        return source_error(error,error_size,"ambiguous normalized candidates");
      }
      selected=candidate; selected_size=candidate_size;
      if(scope) *scoped_selection=scoped_probe ||
        anygm_content_transforms_has_priority(transforms,operation,scope->priority);
    } else {
      free(candidate);
      if(detail[0]) snprintf(last_error,sizeof last_error,"%s",detail);
    }
  }
  free(table);
  if(count && !selected)
    return source_error(error,error_size,last_error[0]?last_error:"no structurally valid candidate");
  *output=selected; *output_size=selected_size;
  return 1;
}

int anygm_content_source_prepare_scoped(const AnygmContentTransforms *transforms,
    const void *data,size_t size,AnygmContentSourceValidator validate,void *context,
    AnygmContentSourceScope *scope,uint8_t **output,size_t *output_size,
    char *error,size_t error_size){
  if(scope) scope->changed_outside_scope=0;
  if(output) *output=NULL;
  if(output_size) *output_size=0;
  if(!output || !output_size || (size && !data))
    return source_error(error,error_size,"invalid arguments");
  uint8_t *selected=NULL;
  size_t selected_size=0;
  int scoped_selection=0;
  if(!prepare_selected_source(transforms,data,size,validate,context,scope,&scoped_selection,
       &selected,&selected_size,error,error_size)) return 0;
  int changed_outside_scope=scope && selected && !scoped_selection &&
    bytes_changed(data,size,selected,selected_size);
  if(!anygm_content_transforms_has(transforms,"input.final")){
    if(scope) scope->changed_outside_scope=changed_outside_scope;
    *output=selected; *output_size=selected_size; return 1;
  }
  /* A probe's identity/zero-record decline selects ordinary input; it must not
   * bypass an explicitly configured final pipeline. Candidate enumeration and
   * ambiguity checks are unchanged and finish before this single final pass. */
  const void *final_input=selected?selected:data;
  size_t final_size=selected?selected_size:size;
  int ok=anygm_content_transform_run(transforms,"input.final",final_input,final_size,
                                    NULL,0,output,output_size,error,error_size);
  if(ok && scope) scope->changed_outside_scope=changed_outside_scope ||
    (!anygm_content_transforms_has_priority(transforms,"input.final",scope->priority) &&
     bytes_changed(final_input,final_size,*output,*output_size));
  if(ok && selected && !bytes_changed(selected,selected_size,*output,*output_size)){
    free(*output);
    *output=selected;
  } else free(selected);
  return ok;
}

int anygm_content_source_prepare(const AnygmContentTransforms *transforms,
    const void *data,size_t size,AnygmContentSourceValidator validate,void *context,
    uint8_t **output,size_t *output_size,char *error,size_t error_size){
  return anygm_content_source_prepare_scoped(transforms,data,size,validate,context,NULL,
                                              output,output_size,error,error_size);
}
