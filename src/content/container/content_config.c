/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "content_config.h"
#include "content_transform_source.h"
#include "content_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

typedef struct {
  int transforms;
  size_t begin,end;
  char key[112];
} ConfigSection;

struct AnygmContentConfig {
  char *text;
  size_t count;
  int has_input;
  ConfigSection sections[ANYGM_CONFIG_MAX_SECTIONS];
};

static int config_error(char *error,size_t size,const char *message){
  if(error && size) snprintf(error,size,"configuration: %s",message);
  return 0;
}

static int section_name(const char *name,size_t length,ConfigSection *section){
  memset(section,0,sizeof *section);
  /* Per-payload selectors were removed: identifying every input for global
   * configuration adds complete-file hashing to ordinary content startup. */
  if(length>=7 && !memcmp(name,"sha256:",7)) return -1;
  if((length>=4 && !memcmp(name,"zip:",4)) ||
     (length>=10 && !memcmp(name,"sample-v1:",10))){
    const char *dot=memchr(name,'.',length);
    if(!dot || (size_t)(dot-name)>=sizeof section->key) return -1;
    size_t prefix=!memcmp(name,"zip:",4)?4u:10u;
    size_t at=prefix;
    uint64_t size=0;
    if(at>=length || name[at]<'1' || name[at]>'9') return -1;
    while(at<length && name[at]>='0' && name[at]<='9'){
      unsigned digit=(unsigned)(name[at++]-'0');
      if(size>(UINT64_MAX-digit)/10u) return -1;
      size=size*10u+digit;
    }
    if(at>=length || name[at++]!=':') return -1;
    size_t digits=prefix==4?8u:64u;
    if((size_t)(dot-name)!=at+digits) return -1;
    for(size_t i=0;i<at+digits;i++){
      unsigned char c=(unsigned char)name[i];
      if(i>=at && !((c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F')))
        return -1;
      section->key[i]=(char)(c>='A' && c<='F'?c+('a'-'A'):c);
    }
    length-=(size_t)(dot+1-name); name=dot+1;
  }
  if(length==10 && !memcmp(name,"transforms",10)){ section->transforms=1; return 1; }
  if(length==9 && !memcmp(name,"pipelines",9)){ section->transforms=2; return 1; }
  if(length==7 && !memcmp(name,"patches",7)){ section->transforms=3; return 1; }
  if(length==9 && !memcmp(name,"overrides",9)) return 1;
  return section->key[0]?-1:0;
}

static char *transform_document(const AnygmContentConfig *config,const ConfigSection *section){
  size_t size=section->end-section->begin;
  char *text=malloc(size+14u);
  if(!text) return NULL;
  const char *header=section->transforms==3?"[patches]\n":
    section->transforms==2?"[pipelines]\n":"[transforms]\n";
  size_t prefix=strlen(header);
  memcpy(text,header,prefix);
  memcpy(text+prefix,config->text+section->begin,size);
  text[size+prefix]=0;
  return text;
}

static int override_text(const AnygmContentConfig *config,const ConfigSection *section,
                          char *out,size_t capacity,char *error,size_t error_size){
  size_t used=0,at=section->begin;
  if(!out || !capacity) return config_error(error,error_size,"missing override output");
  out[0]=0;
  while(at<section->end){
    size_t begin=at;
    while(at<section->end && config->text[at]!='\r' && config->text[at]!='\n') at++;
    size_t end=at;
    if(at<section->end && config->text[at]=='\r') at++;
    if(at<section->end && config->text[at]=='\n') at++;
    while(begin<end && (config->text[begin]==' ' || config->text[begin]=='\t')) begin++;
    while(end>begin && (config->text[end-1]==' ' || config->text[end-1]=='\t')) end--;
    if(begin==end || config->text[begin]=='#' || config->text[begin]==';') continue;
    size_t length=end-begin;
    if(length+2u>capacity-used) return config_error(error,error_size,"override section is too large");
    memcpy(out+used,config->text+begin,length); used+=length;
    out[used++]='\n'; out[used]=0;
  }
  return 1;
}

AnygmContentConfig *anygm_content_config_parse(const void *text,size_t size,
                                             char *error,size_t error_size){
  if(error && error_size) error[0]=0;
  if((size && !text) || size>ANYGM_TRANSFORM_MAX_CONFIG_BYTES ||
     (size && memchr(text,0,size))){
    config_error(error,error_size,"invalid or oversized INI"); return NULL;
  }
  AnygmContentConfig *config=calloc(1,sizeof *config);
  if(!config) return NULL;
  config->text=malloc(size+1u);
  if(!config->text){ free(config); return NULL; }
  if(size) memcpy(config->text,text,size);
  config->text[size]=0;
  size_t at=size>=3 && !memcmp(config->text,"\xef\xbb\xbf",3)?3u:0u;
  ConfigSection *current=NULL;
  while(at<size){
    size_t begin=at;
    while(at<size && config->text[at]!='\r' && config->text[at]!='\n') at++;
    size_t end=at;
    if(at<size && config->text[at]=='\r') at++;
    if(at<size && config->text[at]=='\n') at++;
    while(begin<end && (config->text[begin]==' ' || config->text[begin]=='\t')) begin++;
    while(end>begin && (config->text[end-1]==' ' || config->text[end-1]=='\t')) end--;
    if(begin==end || config->text[begin]=='#' || config->text[begin]==';') continue;
    if(config->text[begin]=='['){
      if(current) current->end=begin;
      current=NULL;
      if(config->text[end-1]!=']') goto invalid;
      ConfigSection section;
      int known=section_name(config->text+begin+1,end-begin-2,&section);
      if(known<0) goto invalid;
      if(!known) continue;
      if(config->count==ANYGM_CONFIG_MAX_SECTIONS) goto invalid;
      for(size_t i=0;i<config->count;i++){
        const ConfigSection *old=&config->sections[i];
        if(old->transforms==section.transforms && !strcmp(old->key,section.key)) goto invalid;
      }
      current=&config->sections[config->count++];
      *current=section; current->begin=at; current->end=size;
    } else if(current && current->transforms==1){
      /* Consume the complete function: section-like
       * source comments must never select overrides or another transform set. */
      const char *equal=memchr(config->text+begin,'=',end-begin);
      if(!equal) goto invalid;
      size_t source=(size_t)(equal+1-config->text),consumed=0,code_size=0,parameter_size=0;
      uint8_t *code=NULL,*parameters=NULL;
      int ok=anygm_content_transform_compile(config->text+source,size-source,&consumed,
        &code,&code_size,&parameters,&parameter_size,error,error_size);
      free(code); free(parameters);
      if(!ok) goto invalid;
      at=source+consumed;
      while(at<size && (config->text[at]==' ' || config->text[at]=='\t')) at++;
      if(at<size && config->text[at]!='\r' && config->text[at]!='\n') goto invalid;
    }
  }
  for(size_t i=0;i<config->count;i++){
    ConfigSection *section=&config->sections[i];
    if(section->transforms){
      char *document=transform_document(config,section);
      /* Functions, pipelines and patches share one namespace in each selection scope.
       * Validate the sections together so their ordering cannot hide a duplicate. */
      for(size_t prior=0;document && prior<i;prior++){
        ConfigSection *other=&config->sections[prior];
        if(!other->transforms || other->transforms==section->transforms ||
           strcmp(other->key,section->key)) continue;
        char *preceding=transform_document(config,other);
        size_t length=strlen(document),prefix=preceding?strlen(preceding):0;
        char *combined=preceding?malloc(prefix+length+2u):NULL;
        if(combined){
          memcpy(combined,preceding,prefix); combined[prefix]='\n';
          memcpy(combined+prefix+1u,document,length+1u);
        }
        free(preceding); free(document); document=combined;
      }
      AnygmContentTransforms *validation=anygm_content_transforms_create();
      int ok=document && validation && anygm_content_transforms_parse(validation,document,
        strlen(document),error,error_size);
      /* A payload can be identified only after its container has been routed.
       * Pre-routing source adapters therefore belong in defaults or anchors. */
      if(ok && section->key[0] && anygm_content_source_configured(validation))
        ok=config_error(error,error_size,"selected sections cannot declare input adapters");
      if(ok && !section->key[0] && anygm_content_source_configured(validation)) config->has_input=1;
      free(document); anygm_content_transforms_destroy(validation);
      if(!ok) goto invalid;
    } else {
      char overrides[ANYGM_CONFIG_OVERRIDE_BYTES];
      if(!override_text(config,section,overrides,sizeof overrides,error,error_size)) goto invalid;
    }
  }
  return config;
invalid:
  if(!error || !error_size || !error[0]) config_error(error,error_size,"malformed or duplicate section");
  anygm_content_config_destroy(config);
  return NULL;
}

void anygm_content_config_destroy(AnygmContentConfig *config){
  if(config){ free(config->text); free(config); }
}

int anygm_content_config_has_input(const AnygmContentConfig *config){
  return config && config->has_input;
}

int anygm_content_config_apply(const AnygmContentConfig *config,
                               AnygmContentTransforms *transforms,unsigned priority,
                               char *overrides,size_t capacity,char *error,size_t error_size){
  return anygm_content_config_select(config,"",transforms,priority,overrides,capacity,error,error_size);
}

int anygm_content_config_candidate(const AnygmContentConfig *config,const char *kind,uint64_t size){
  char prefix[64];
  snprintf(prefix,sizeof prefix,"%s:%" PRIu64 ":",kind,size);
  if(!size) snprintf(prefix,sizeof prefix,"%s:",kind);
  for(size_t i=0;config && i<config->count;i++)
    if(!strncmp(config->sections[i].key,prefix,strlen(prefix))) return 1;
  return 0;
}

int anygm_content_config_matches(const AnygmContentConfig *config,const char *key){
  for(size_t i=0;config && i<config->count;i++)
    if(!strcmp(config->sections[i].key,key)) return 1;
  return 0;
}

int anygm_content_config_select(const AnygmContentConfig *config,const char *key,
                               AnygmContentTransforms *transforms,unsigned priority,
                               char *overrides,size_t capacity,char *error,size_t error_size){
  if(overrides && capacity) overrides[0]=0;
  if(!config) return 1;
  AnygmContentTransforms *staged=anygm_content_transforms_create();
  char selected_overrides[ANYGM_CONFIG_OVERRIDE_BYTES]={0};
  int valid=staged && anygm_content_transforms_copy(staged,transforms);
  for(size_t i=0;valid && i<config->count;i++){
    const ConfigSection *section=&config->sections[i];
    if(strcmp(section->key,key)) continue;
    if(section->transforms){
      char *document=transform_document(config,section);
      valid=document && anygm_content_transforms_parse_layer(staged,document,strlen(document),
        priority,error,error_size);
      free(document);
    } else valid=override_text(config,section,selected_overrides,sizeof selected_overrides,error,error_size);
  }
  size_t length=strlen(selected_overrides);
  if(valid && (!overrides || length>=capacity))
    valid=config_error(error,error_size,"override output is too small");
  if(valid) valid=anygm_content_transforms_copy(transforms,staged);
  if(valid) memcpy(overrides,selected_overrides,length+1u);
  anygm_content_transforms_destroy(staged);
  return valid;
}
