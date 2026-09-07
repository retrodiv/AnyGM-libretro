/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "content_config.h"
#include "content_transform_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int transforms,targeted;
  uint8_t digest[32];
  size_t begin,end;
} ConfigSection;

struct AnygmContentConfig {
  char *text;
  size_t count;
  ConfigSection sections[ANYGM_CONFIG_MAX_SECTIONS];
};

static int config_error(char *error,size_t size,const char *message){
  if(error && size) snprintf(error,size,"configuration: %s",message);
  return 0;
}

static int hex_digit(unsigned char c){
  if(c>='0' && c<='9') return c-'0';
  if(c>='a' && c<='f') return c-'a'+10;
  if(c>='A' && c<='F') return c-'A'+10;
  return -1;
}

static int section_name(const char *name,size_t length,ConfigSection *section){
  memset(section,0,sizeof *section);
  if(length>=7 && !memcmp(name,"sha256:",7)){
    if(length<72 || name[71]!='.') return -1;
    section->targeted=1;
    for(size_t i=0;i<32;i++){
      int high=hex_digit((unsigned char)name[7+i*2]);
      int low=hex_digit((unsigned char)name[8+i*2]);
      if(high<0 || low<0) return -1;
      section->digest[i]=(uint8_t)((high<<4)|low);
    }
    name+=72; length-=72;
  }
  if(length==10 && !memcmp(name,"transforms",10)){ section->transforms=1; return 1; }
  if(length==9 && !memcmp(name,"overrides",9)) return 1;
  return section->targeted?-1:0;
}

static char *transform_document(const AnygmContentConfig *config,const ConfigSection *section){
  size_t size=section->end-section->begin;
  char *text=malloc(size+14u);
  if(!text) return NULL;
  memcpy(text,"[transforms]\n",13);
  memcpy(text+13,config->text+section->begin,size);
  text[size+13]=0;
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
        if(old->transforms==section.transforms && old->targeted==section.targeted &&
           !memcmp(old->digest,section.digest,32)) goto invalid;
      }
      current=&config->sections[config->count++];
      *current=section; current->begin=at; current->end=size;
    } else if(current && current->transforms){
      /* Consume the complete function even in an unmatched section: section-like
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
      AnygmContentTransforms *validation=anygm_content_transforms_create();
      int ok=document && validation && anygm_content_transforms_parse(validation,document,
        strlen(document),error,error_size);
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

int anygm_content_config_apply(const AnygmContentConfig *config,const uint8_t *digest,
                               AnygmContentTransforms *transforms,unsigned priority,
                               char *overrides,size_t capacity,char *error,size_t error_size){
  if(overrides && capacity) overrides[0]=0;
  if(!config) return 1;
  AnygmContentTransforms *staged=anygm_content_transforms_create();
  char selected_overrides[ANYGM_CONFIG_OVERRIDE_BYTES]={0};
  int valid=staged && anygm_content_transforms_copy(staged,transforms);
  for(size_t i=0;valid && i<config->count;i++){
    const ConfigSection *section=&config->sections[i];
    if(section->targeted!=(digest!=NULL) || (digest && memcmp(section->digest,digest,32))) continue;
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
