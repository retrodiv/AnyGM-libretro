/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "content_transform.h"
#include "content_transform_source.h"
#include "content_pipeline.h"
#include "gml_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TransformEntry {
  char name[64];
  uint8_t *program,*parameters;
  size_t program_size,parameter_size;
  unsigned priority;
  size_t step_count;
  char steps[ANYGM_TRANSFORM_MAX_PIPELINE_STEPS][64];
} TransformEntry;

struct AnygmContentTransforms {
  TransformEntry entries[ANYGM_TRANSFORM_MAX_PROGRAMS];
  size_t count;
};

static int fail(char *error,size_t size,const char *message){
  if(error && size) snprintf(error,size,"content transform: %s",message);
  return 0;
}

static uint64_t read_le(const uint8_t *p,unsigned width){
  uint64_t value=0;
  for(unsigned i=0;i<width;i++) value|=(uint64_t)p[i]<<(8u*i);
  return value;
}

static int valid_program(const uint8_t *code,size_t size){
  if(!code || !size || size>ANYGM_TRANSFORM_MAX_PROGRAM_BYTES ||
     size%ANYGM_TRANSFORM_INSTRUCTION_BYTES) return 0;
  size_t count=size/ANYGM_TRANSFORM_INSTRUCTION_BYTES;
  for(size_t i=0;i<count;i++){
    const uint8_t *p=code+i*ANYGM_TRANSFORM_INSTRUCTION_BYTES;
    unsigned op=p[0],d=p[1],a=p[2],b=p[3];
    uint64_t imm=read_le(p+4,8);
    if(op>ANYGM_TRANSFORM_JUMP_NONZERO || d>=32 || a>=32 || b>=32) return 0;
    if(op==ANYGM_TRANSFORM_CONSTANT){ if(a || b) return 0; }
    else if(op>=ANYGM_TRANSFORM_LOAD8 && op<=ANYGM_TRANSFORM_STORE64){
      if(b>=4 || (op>=ANYGM_TRANSFORM_STORE8 && b!=1 && b!=3)) return 0;
    } else if(op>=ANYGM_TRANSFORM_JUMP){
      if(imm>=count || d || b || (op==ANYGM_TRANSFORM_JUMP && a)) return 0;
    } else {
      if(imm) return 0;
      if(op==ANYGM_TRANSFORM_REJECT && (d || a || b)) return 0;
      if(op==ANYGM_TRANSFORM_RETURN && d) return 0;
      if(op==ANYGM_TRANSFORM_MOVE && b) return 0;
    }
  }
  return 1;
}

int anygm_content_transform_execute(const void *program,size_t program_size,
                                     const void *parameters,size_t parameter_size,
                                     const void *input,size_t input_size,uint64_t step_limit,
                                     uint8_t **output,size_t *output_size,
                                     char *error,size_t error_size){
  if(error && error_size) error[0]=0;
  if(output) *output=NULL;
  if(output_size) *output_size=0;
  if(!output || !output_size || (input_size && !input) ||
     (parameter_size && !parameters) || input_size>ANYGM_TRANSFORM_MAX_INPUT_BYTES ||
     parameter_size>ANYGM_TRANSFORM_MAX_PARAMETER_BYTES ||
     !valid_program((const uint8_t*)program,program_size))
    return fail(error,error_size,"invalid program or buffer");
  uint8_t *work=(uint8_t*)malloc(input_size?input_size:1u);
  uint8_t *scratch=(uint8_t*)calloc(ANYGM_TRANSFORM_SCRATCH_BYTES,1u);
  if(!work || !scratch){ free(work); free(scratch); return fail(error,error_size,"allocation failed"); }
  if(input_size) memcpy(work,input,input_size);
  const uint8_t *memory[4]={(const uint8_t*)input,work,(const uint8_t*)parameters,scratch};
  size_t lengths[4]={input_size,input_size,parameter_size,ANYGM_TRANSFORM_SCRATCH_BYTES};
  uint64_t registers[32]={0};
  registers[0]=input_size; registers[1]=parameter_size;
  uint64_t budget=UINT64_C(1000000)+(uint64_t)input_size*128u;
  if(budget>ANYGM_TRANSFORM_MAX_STEPS) budget=ANYGM_TRANSFORM_MAX_STEPS;
  if(step_limit && step_limit<budget) budget=step_limit;
  size_t pc=0,count=program_size/ANYGM_TRANSFORM_INSTRUCTION_BYTES;
  const char *reason="instruction budget exhausted";
  while(budget--){
    if(pc>=count){ reason="program ended without a result"; break; }
    const uint8_t *p=(const uint8_t*)program+pc++*ANYGM_TRANSFORM_INSTRUCTION_BYTES;
    unsigned op=p[0],d=p[1],a=p[2],b=p[3];
    uint64_t imm=read_le(p+4,8),left=registers[a],right=registers[b];
    switch(op){
      case ANYGM_TRANSFORM_RETURN:
        if(left>input_size || right>input_size-left){ reason="invalid result slice"; goto rejected; }
        if(right) memmove(work,work+(size_t)left,(size_t)right);
        free(scratch); *output=work; *output_size=(size_t)right; return 1;
      case ANYGM_TRANSFORM_REJECT: reason="program rejected input"; goto rejected;
      case ANYGM_TRANSFORM_CONSTANT: registers[d]=imm; break;
      case ANYGM_TRANSFORM_MOVE: registers[d]=left; break;
      case ANYGM_TRANSFORM_ADD: registers[d]=left+right; break;
      case ANYGM_TRANSFORM_SUBTRACT: registers[d]=left-right; break;
      case ANYGM_TRANSFORM_MULTIPLY: registers[d]=left*right; break;
      case ANYGM_TRANSFORM_DIVIDE:
      case ANYGM_TRANSFORM_REMAINDER:
        if(!right){ reason="division by zero"; goto rejected; }
        registers[d]=op==ANYGM_TRANSFORM_DIVIDE?left/right:left%right; break;
      case ANYGM_TRANSFORM_AND: registers[d]=left&right; break;
      case ANYGM_TRANSFORM_OR: registers[d]=left|right; break;
      case ANYGM_TRANSFORM_XOR: registers[d]=left^right; break;
      case ANYGM_TRANSFORM_SHIFT_LEFT:
      case ANYGM_TRANSFORM_SHIFT_RIGHT:
        if(right>=64){ reason="invalid shift"; goto rejected; }
        registers[d]=op==ANYGM_TRANSFORM_SHIFT_LEFT?left<<right:left>>right; break;
      case ANYGM_TRANSFORM_EQUAL: registers[d]=left==right; break;
      case ANYGM_TRANSFORM_LESS: registers[d]=left<right; break;
      case ANYGM_TRANSFORM_LOAD8:
      case ANYGM_TRANSFORM_LOAD32:
      case ANYGM_TRANSFORM_LOAD64:
      case ANYGM_TRANSFORM_STORE8:
      case ANYGM_TRANSFORM_STORE32:
      case ANYGM_TRANSFORM_STORE64: {
        unsigned width=(op-ANYGM_TRANSFORM_LOAD8)%3u;
        width=width==0?1u:width==1?4u:8u;
        uint64_t address=left+imm;
        if(address<left || address>lengths[b] || width>lengths[b]-address){
          reason="buffer access out of bounds"; goto rejected;
        }
        if(op<=ANYGM_TRANSFORM_LOAD64) registers[d]=read_le(memory[b]+(size_t)address,width);
        else {
          uint8_t *destination=b==1?work:scratch;
          for(unsigned i=0;i<width;i++) destination[(size_t)address+i]=(uint8_t)(registers[d]>>(8u*i));
        }
        break;
      }
      case ANYGM_TRANSFORM_JUMP: pc=(size_t)imm; break;
      case ANYGM_TRANSFORM_JUMP_ZERO: if(!left) pc=(size_t)imm; break;
      case ANYGM_TRANSFORM_JUMP_NONZERO: if(left) pc=(size_t)imm; break;
    }
  }
rejected:
  free(work); free(scratch); return fail(error,error_size,reason);
}

AnygmContentTransforms *anygm_content_transforms_create(void){
  return (AnygmContentTransforms*)calloc(1,sizeof(AnygmContentTransforms));
}

void anygm_content_transforms_destroy(AnygmContentTransforms *set){
  if(!set) return;
  for(size_t i=0;i<set->count;i++){
    free(set->entries[i].program); free(set->entries[i].parameters);
  }
  free(set);
}

static const TransformEntry *lookup(const AnygmContentTransforms *set,const char *name){
  if(set && name) for(size_t i=0;i<set->count;i++)
    if(!strcmp(set->entries[i].name,name)) return &set->entries[i];
  return NULL;
}

int anygm_content_transforms_copy(AnygmContentTransforms *target,const AnygmContentTransforms *source){
  if(!target || !source) return 0;
  AnygmContentTransforms *copy=anygm_content_transforms_create();
  if(!copy) return 0;
  for(size_t i=0;i<source->count;i++){
    const TransformEntry *from=&source->entries[i];
    TransformEntry *to=&copy->entries[copy->count++];
    *to=*from;
    to->program=malloc(from->program_size?from->program_size:1u);
    to->parameters=malloc(from->parameter_size?from->parameter_size:1u);
    if(!to->program || !to->parameters){ anygm_content_transforms_destroy(copy); return 0; }
    if(from->program_size) memcpy(to->program,from->program,from->program_size);
    if(from->parameter_size) memcpy(to->parameters,from->parameters,from->parameter_size);
  }
  AnygmContentTransforms old=*target;
  *target=*copy; *copy=old;
  anygm_content_transforms_destroy(copy);
  return 1;
}

int anygm_content_transforms_has(const AnygmContentTransforms *set,const char *name){
  return lookup(set,name)!=NULL;
}

int anygm_content_transforms_parse(AnygmContentTransforms *set,const void *text,size_t size,
                                   char *error,size_t error_size){
  return anygm_content_transforms_parse_layer(set,text,size,0,error,error_size);
}

static int pipeline_steps(TransformEntry *entry,const char *value,const char *end){
  while(value<end){
    while(value<end && (*value==' ' || *value=='\t')) value++;
    const char *begin=value;
    while(value<end && *value!='|') value++;
    const char *last=value;
    while(last>begin && (last[-1]==' ' || last[-1]=='\t')) last--;
    size_t length=(size_t)(last-begin);
    if(!length || length>=sizeof entry->steps[0] ||
       entry->step_count==ANYGM_TRANSFORM_MAX_PIPELINE_STEPS) return 0;
    for(const char *p=begin;p<last;p++)
      if(!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') ||
           *p=='.' || *p=='_' || *p=='-' || *p==':')) return 0;
    char *step=entry->steps[entry->step_count++];
    memcpy(step,begin,length);
    if(!strncmp(step,"builtin.",8) && !anygm_content_pipeline_builtin_valid(step)) return 0;
    if(value<end && ++value==end) return 0;
  }
  return entry->step_count!=0;
}

int anygm_content_transforms_parse_layer(AnygmContentTransforms *set,const void *text,size_t size,
                                         unsigned priority,char *error,size_t error_size){
  if(error && error_size) error[0]=0;
  if(!set || (size && !text) || size>ANYGM_TRANSFORM_MAX_CONFIG_BYTES ||
     (size && memchr(text,0,size))) return fail(error,error_size,"invalid configuration");
  AnygmContentTransforms staged={0};
  int active=0,seen_sections=0;
  const char *bytes=(const char*)text;
  size_t at=size>=3 && !memcmp(bytes,"\xef\xbb\xbf",3)?3u:0u;
  for(;at<size;){
    size_t start=at;
    while(at<size && bytes[at]!='\n' && bytes[at]!='\r') at++;
    size_t end=at;
    if(at<size && bytes[at]=='\r') at++;
    if(at<size && bytes[at]=='\n') at++;
    while(start<end && (bytes[start]==' ' || bytes[start]=='\t')) start++;
    while(end>start && (bytes[end-1]==' ' || bytes[end-1]=='\t' || bytes[end-1]=='\r')) end--;
    if(start==end || bytes[start]=='#' || bytes[start]==';') continue;
    if(bytes[start]=='['){
      if(bytes[end-1]!=']') goto invalid;
      active=end-start==12 && !memcmp(bytes+start,"[transforms]",12)?1:
        end-start==11 && !memcmp(bytes+start,"[pipelines]",11)?2:0;
      if(active && (seen_sections&(1<<active))) goto invalid;
      seen_sections|=1<<active;
      continue;
    }
    if(!active) continue;
    const char *equal=(const char*)memchr(bytes+start,'=',end-start);
    if(!equal || staged.count>=ANYGM_TRANSFORM_MAX_PROGRAMS) goto invalid;
    size_t name_size=(size_t)(equal-(bytes+start));
    while(name_size && (bytes[start+name_size-1]==' ' || bytes[start+name_size-1]=='\t')) name_size--;
    if(!name_size || name_size>=sizeof(staged.entries[0].name)) goto invalid;
    for(size_t i=0;i<name_size;i++){
      unsigned char c=(unsigned char)bytes[start+i];
      if(!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='.' || c=='_' || c=='-')) goto invalid;
    }
    TransformEntry *entry=&staged.entries[staged.count];
    memcpy(entry->name,bytes+start,name_size); entry->name[name_size]=0;
    if(!strncmp(entry->name,"builtin.",8)) goto invalid;
    if(lookup(&staged,entry->name)) goto invalid;
    staged.count++;
    entry->priority=priority;
    const char *value=equal+1,*last=bytes+end;
    while(value<last && (*value==' ' || *value=='\t')) value++;
    if(active==2){
      if(!pipeline_steps(entry,value,last)) goto invalid;
      continue;
    }
    size_t consumed=0; char detail[160]={0};
    if(!anygm_content_transform_compile(value,size-(size_t)(value-bytes),&consumed,
         &entry->program,&entry->program_size,&entry->parameters,&entry->parameter_size,
         detail,sizeof detail)){
      if(error && error_size) snprintf(error,error_size,"content transform '%s': %s",entry->name,detail);
      goto invalid;
    }
    if(!valid_program(entry->program,entry->program_size)) goto invalid;
    at=(size_t)(value-bytes)+consumed;
    while(at<size && (bytes[at]==' ' || bytes[at]=='\t')) at++;
    if(at<size && bytes[at]!='\r' && bytes[at]!='\n') goto invalid;
  }
  size_t additional=0;
  for(size_t i=0;i<staged.count;i++) if(!lookup(set,staged.entries[i].name)) additional++;
  if(additional>ANYGM_TRANSFORM_MAX_PROGRAMS-set->count) goto invalid;
  /* No operation below this point can fail, so a bad overlay cannot partly replace defaults. */
  for(size_t i=0;i<staged.count;i++){
    size_t position=0;
    while(position<set->count && strcmp(set->entries[position].name,staged.entries[i].name)) position++;
    if(position==set->count) set->count++;
    else if(set->entries[position].priority>priority){
      free(staged.entries[i].program); free(staged.entries[i].parameters);
      continue;
    } else { free(set->entries[position].program); free(set->entries[position].parameters); }
    set->entries[position]=staged.entries[i];
  }
  return 1;
invalid:
  for(size_t i=0;i<staged.count;i++){
    free(staged.entries[i].program); free(staged.entries[i].parameters);
  }
  if(error && error_size && error[0]) return 0;
  return fail(error,error_size,"malformed or oversized transform declaration");
}

void anygm_content_transforms_hash(const AnygmContentTransforms *set,uint8_t digest[32]){
  /* Fixed per-entry hashes sorted by name make identity independent of INI order. */
  const TransformEntry *ordered[ANYGM_TRANSFORM_MAX_PROGRAMS];
  uint8_t records[ANYGM_TRANSFORM_MAX_PROGRAMS][160];
  size_t count=set?set->count:0;
  memset(records,0,sizeof records);
  for(size_t i=0;i<count;i++){
    size_t at=i;
    while(at && strcmp(ordered[at-1]->name,set->entries[i].name)>0){
      ordered[at]=ordered[at-1]; at--;
    }
    ordered[at]=&set->entries[i];
  }
  for(size_t i=0;i<count;i++){
    const TransformEntry *entry=ordered[i];
    memcpy(records[i],entry->name,strlen(entry->name));
    gml_sha256(entry->program,entry->program_size,records[i]+64);
    gml_sha256(entry->parameters,entry->parameter_size,records[i]+96);
    if(entry->step_count) gml_sha256(entry->steps,
      entry->step_count*sizeof entry->steps[0],records[i]+128);
  }
  gml_sha256(records,count*sizeof records[0],digest);
}

typedef struct {
  const TransformEntry *program;
  const char *builtin;
} PipelineLeaf;

static int expand(const AnygmContentTransforms *set,const char *name,
                    PipelineLeaf leaves[ANYGM_TRANSFORM_MAX_PIPELINE_STEPS],
                    size_t *count,unsigned depth,char *error,size_t error_size){
  if(depth>ANYGM_TRANSFORM_MAX_PROGRAMS)
    return fail(error,error_size,"cyclic or excessively deep pipeline");
  const TransformEntry *entry=lookup(set,name);
  if(entry && entry->step_count){
    for(size_t i=0;i<entry->step_count;i++)
      if(!expand(set,entry->steps[i],leaves,count,depth+1u,error,error_size)) return 0;
    return 1;
  }
  if(!entry && !anygm_content_pipeline_builtin_valid(name)){
    if(error && error_size) snprintf(error,error_size,"content transform: missing user program or pipeline step '%s'",name?name:"");
    return 0;
  }
  if(*count==ANYGM_TRANSFORM_MAX_PIPELINE_STEPS)
    return fail(error,error_size,"expanded pipeline has too many steps");
  leaves[*count].program=entry;
  leaves[(*count)++].builtin=entry?NULL:name;
  return 1;
}

int anygm_content_transform_validate(const AnygmContentTransforms *set,const char *name,
                                      char *error,size_t error_size){
  if(error && error_size) error[0]=0;
  PipelineLeaf leaves[ANYGM_TRANSFORM_MAX_PIPELINE_STEPS];
  size_t count=0;
  return expand(set,name,leaves,&count,0,error,error_size);
}

int anygm_content_transform_run(const AnygmContentTransforms *set,const char *name,
                                 const void *input,size_t input_size,
                                 uint8_t **output,size_t *output_size,
                                 char *error,size_t error_size){
  if(error && error_size) error[0]=0;
  if(output) *output=NULL;
  if(output_size) *output_size=0;
  if(!output || !output_size || (input_size && !input) ||
     input_size>ANYGM_TRANSFORM_MAX_INPUT_BYTES)
    return fail(error,error_size,"invalid pipeline input");
  PipelineLeaf leaves[ANYGM_TRANSFORM_MAX_PIPELINE_STEPS];
  size_t count=0;
  if(!expand(set,name,leaves,&count,0,error,error_size)) return 0;
  uint8_t *owned=NULL;
  const void *current=input;
  size_t size=input_size;
  for(size_t i=0;i<count;i++){
    const TransformEntry *entry=leaves[i].program;
    uint8_t *next=NULL; size_t next_size=0;
    int ok=entry?anygm_content_transform_execute(entry->program,entry->program_size,
      entry->parameters,entry->parameter_size,current,size,0,&next,&next_size,error,error_size):
      anygm_content_pipeline_builtin_run(leaves[i].builtin,current,size,&next,&next_size);
    free(owned);
    if(!ok){
      if(!entry) fail(error,error_size,"built-in step rejected input or exceeded its output capacity");
      return 0;
    }
    owned=next; current=next; size=next_size;
  }
  *output=owned; *output_size=size;
  return 1;
}
