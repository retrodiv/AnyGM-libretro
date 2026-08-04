/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_bytecode_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *dup_range(const char *s, size_t n){
  char *out=(char*)malloc(n+1);
  if(out){ memcpy(out,s,n); out[n]=0; }
  return out;
}

/* GM6-GM8 strings treat a backslash as an ordinary character and accept
 * `begin`/`end` as block delimiters.  The shared compiler uses escaped strings
 * and brace-delimited blocks, so prepare those classic forms at its boundary.
 * Keeping this transformation here lets the project importer retain the source
 * exactly as authored while every token and delimiter scanner sees one
 * consistent representation. */
static char *compiler_source_text(const GmlcProject *project, const char *source){
  if(!source) return NULL;
  if(!project || project->classic_version<=0) return gmlc_strdup(source);
  size_t length=strlen(source);
  if(length>(SIZE_MAX-1)/2) return NULL;
  char *out=(char*)malloc(length*2+1);
  if(!out) return NULL;
  enum { SOURCE_CODE, SOURCE_STRING, SOURCE_LINE_COMMENT, SOURCE_BLOCK_COMMENT } state=SOURCE_CODE;
  char quote=0;
  size_t write=0;
  for(size_t read=0;read<length;read++){
    char ch=source[read];
    if(state==SOURCE_CODE){
      const char *block_word=NULL;
      size_t block_length=0;
      if(word_match_at(source,read,"begin")){
        block_word="begin";
        block_length=5;
      } else if(word_match_at(source,read,"end")){
        block_word="end";
        block_length=3;
      }
      if(block_word){
        out[write++]=block_word[0]=='b'?'{':'}';
        for(size_t index=1;index<block_length;index++) out[write++]=' ';
        read+=block_length-1;
        continue;
      }
      if(ch=='/' && read+1<length && source[read+1]=='/'){
        out[write++]=ch;
        out[write++]=source[++read];
        state=SOURCE_LINE_COMMENT;
        continue;
      }
      if(ch=='/' && read+1<length && source[read+1]=='*'){
        out[write++]=ch;
        out[write++]=source[++read];
        state=SOURCE_BLOCK_COMMENT;
        continue;
      }
      if(ch=='"' || ch=='\''){
        quote=ch;
        state=SOURCE_STRING;
      }
      out[write++]=ch;
      continue;
    }
    if(state==SOURCE_STRING){
      if(ch=='\\') out[write++]='\\';
      out[write++]=ch;
      if(ch==quote) state=SOURCE_CODE;
      continue;
    }
    out[write++]=ch;
    if(state==SOURCE_LINE_COMMENT){
      if(ch=='\n' || ch=='\r') state=SOURCE_CODE;
    } else if(ch=='*' && read+1<length && source[read+1]=='/'){
      out[write++]=source[++read];
      state=SOURCE_CODE;
    }
  }
  out[write]=0;
  return out;
}

char *compiler_read_source(const GmlcProject *project, const char *path){
  char *source=gmlc_project_read_source(project,path);
  if(!source) return NULL;
  char *prepared=compiler_source_text(project,source);
  free(source);
  return prepared;
}

static void lx_skip(Lexer *l){
  for(;;){
    while(isspace((unsigned char)l->src[l->pos])) l->pos++;
    if(l->src[l->pos]=='/' && l->src[l->pos+1]=='/'){ while(l->src[l->pos] && l->src[l->pos]!='\n') l->pos++; continue; }
    if(l->src[l->pos]=='/' && l->src[l->pos+1]=='*'){ l->pos+=2; while(l->src[l->pos] && !(l->src[l->pos]=='*'&&l->src[l->pos+1]=='/')) l->pos++; if(l->src[l->pos]) l->pos+=2; continue; }
    if(l->src[l->pos]=='#'){
      size_t bol=l->pos;
      while(bol>0 && l->src[bol-1]!='\n' && l->src[bol-1]!='\r') bol--;
      int line_directive=1;
      for(size_t p=bol;p<l->pos;p++) if(!isspace((unsigned char)l->src[p])){ line_directive=0; break; }
      if(line_directive){ while(l->src[l->pos] && l->src[l->pos]!='\n') l->pos++; continue; }
    }
    break;
  }
}

void lx_next(Lexer *l){
  lx_skip(l);
  memset(&l->tok,0,sizeof(l->tok));
  l->tok.start=l->pos;
  const char *s=l->src+l->pos;
  if(!*s){ l->tok.kind=TOK_EOF; l->tok.end=l->pos; return; }
  if(isalpha((unsigned char)*s) || *s=='_'){
    size_t n=0;
    while(isalnum((unsigned char)s[n]) || s[n]=='_') n++;
    if(n>=sizeof(l->tok.text)) n=sizeof(l->tok.text)-1;
    memcpy(l->tok.text,s,n); l->tok.text[n]=0;
    l->tok.kind=TOK_ID; l->pos+=(size_t)n; l->tok.end=l->pos; return;
  }
  if(isdigit((unsigned char)*s) || (*s=='.' && isdigit((unsigned char)s[1]))){
    /* The classic lexer accepts an extra dot in a numeric token (old action
     * editors can produce values such as `.5.25`).  Keep the final dot as the
     * decimal separator and ignore earlier ones. */
    size_t raw=0, dots=0, last_dot=0;
    while(isdigit((unsigned char)s[raw]) ||
          (s[raw]=='.' && isdigit((unsigned char)s[raw+1]))){
      if(s[raw]=='.'){ dots++; last_dot=raw; }
      raw++;
    }
    if(dots>1){
      char normalized[128]; size_t nn=0;
      for(size_t i=0;i<raw && nn+1<sizeof(normalized);i++)
        if(s[i]!='.' || i==last_dot) normalized[nn++]=s[i];
      normalized[nn]=0;
      l->tok.num=strtod(normalized,NULL);
      size_t n=raw<sizeof(l->tok.text)?raw:sizeof(l->tok.text)-1;
      memcpy(l->tok.text,s,n); l->tok.text[n]=0;
      l->tok.kind=TOK_NUM; l->pos+=raw; l->tok.end=l->pos; return;
    }
    char *end=NULL;
    l->tok.num=strtod(s,&end);
    size_t n=(size_t)(end-s);
    if(n>=sizeof(l->tok.text)) n=sizeof(l->tok.text)-1;
    memcpy(l->tok.text,s,n); l->tok.text[n]=0;
    l->tok.kind=TOK_NUM; l->pos+=(size_t)(end-s); l->tok.end=l->pos; return;
  }
  if((*s=='@' && s[1]=='"') || *s=='"' || *s=='\''){
    int verbatim=(*s=='@');
    char quote=verbatim?'"':*s;
    l->pos += verbatim ? 2 : 1;
    size_t n=0;
    while(l->src[l->pos] && l->src[l->pos]!=quote){
      char ch=l->src[l->pos++];
      if(!verbatim && ch=='\\' && l->src[l->pos]){
        char e=l->src[l->pos++];
        if(e=='n') ch='\n'; else if(e=='t') ch='\t'; else ch=e;
      }
      if(n+1<sizeof(l->tok.text)) l->tok.text[n++]=ch;
    }
    if(l->src[l->pos]==quote) l->pos++;
    l->tok.text[n]=0; l->tok.kind=TOK_STR; l->tok.end=l->pos; return;
  }
  static const char *ops[]={"==","!=","<>","<=",">=","&&","||","<<",">>","+=","-=","*=","/=","%=","++","--",NULL};
  for(int i=0;ops[i];i++){
    size_t n=strlen(ops[i]);
    if(!strncmp(s,ops[i],n)){ snprintf(l->tok.text,sizeof(l->tok.text),"%s",!strcmp(ops[i],"<>")?"!=":ops[i]); l->tok.kind=TOK_SYM; l->pos+=n; l->tok.end=l->pos; return; }
  }
  l->tok.kind=TOK_SYM; l->tok.text[0]=*s; l->tok.text[1]=0; l->pos++;
  l->tok.end=l->pos;
}

char *lx_string_value(const Lexer *l){
  if(!l || !l->src || l->tok.kind!=TOK_STR || l->tok.end<l->tok.start) return NULL;
  size_t position=l->tok.start;
  int verbatim=l->src[position]=='@' && l->src[position+1]=='"';
  char quote=verbatim?'"':l->src[position];
  position+=verbatim?2:1;
  size_t capacity=l->tok.end-position+1;
  char *value=(char*)malloc(capacity);
  if(!value) return NULL;
  size_t length=0;
  while(position<l->tok.end && l->src[position] &&
        l->src[position]!=quote){
    char ch=l->src[position++];
    if(!verbatim && ch=='\\' && position<l->tok.end && l->src[position]){
      char escaped=l->src[position++];
      if(escaped=='n') ch='\n';
      else if(escaped=='t') ch='\t';
      else ch=escaped;
    }
    value[length++]=ch;
  }
  value[length]=0;
  return value;
}

int word_match_at(const char *src, size_t pos, const char *w){
  size_t n=strlen(w);
  if(strncmp(src+pos,w,n)) return 0;
  if(pos>0 && (isalnum((unsigned char)src[pos-1]) || src[pos-1]=='_')) return 0;
  if(isalnum((unsigned char)src[pos+n]) || src[pos+n]=='_') return 0;
  return 1;
}

void trim_span(const char *src, Span *s){
  while(s->start<s->end && isspace((unsigned char)src[s->start])) s->start++;
  while(s->end>s->start && isspace((unsigned char)src[s->end-1])) s->end--;
}

int span_empty(const char *src, Span s){
  size_t p=s.start;
  while(p<s.end){
    if(isspace((unsigned char)src[p])){ p++; continue; }
    if(p+1<s.end && src[p]=='/' && src[p+1]=='/'){
      p+=2;
      while(p<s.end && src[p]!='\n') p++;
      continue;
    }
    if(p+1<s.end && src[p]=='/' && src[p+1]=='*'){
      p+=2;
      while(p+1<s.end && !(src[p]=='*' && src[p+1]=='/')) p++;
      if(p+1<s.end) p+=2;
      continue;
    }
    return 0;
  }
  return 1;
}

size_t skip_ws_comments_at(const char *src, size_t pos){
  for(;;){
    while(isspace((unsigned char)src[pos])) pos++;
    if(src[pos]=='/' && src[pos+1]=='/'){
      pos+=2;
      while(src[pos] && src[pos]!='\n') pos++;
      continue;
    }
    if(src[pos]=='/' && src[pos+1]=='*'){
      pos+=2;
      while(src[pos] && !(src[pos]=='*' && src[pos+1]=='/')) pos++;
      if(src[pos]) pos+=2;
      continue;
    }
    break;
  }
  return pos;
}

int scan_matching_delim(const char *src, size_t open_pos, char open_ch, char close_ch, size_t *out_close){
  int depth=0;
  for(size_t pos=open_pos; src[pos]; pos++){
    char ch=src[pos];
    if(ch=='"' || ch=='\''){
      char quote=ch;
      pos++;
      while(src[pos]){
        if(src[pos]=='\\' && src[pos+1]){ pos++; continue; }
        if(src[pos]==quote) break;
        pos++;
      }
      continue;
    }
    if(ch=='/' && src[pos+1]=='/'){
      pos+=2;
      while(src[pos] && src[pos]!='\n') pos++;
      if(!src[pos]) break;
      continue;
    }
    if(ch=='/' && src[pos+1]=='*'){
      pos+=2;
      while(src[pos] && !(src[pos]=='*' && src[pos+1]=='/')) pos++;
      if(!src[pos]) break;
      pos++;
      continue;
    }
    if(ch==open_ch){ depth++; continue; }
    if(ch==close_ch){
      depth--;
      if(depth==0){ *out_close=pos; return 1; }
    }
  }
  return 0;
}

int function_shape_at(const char *src, size_t pos){
  if(!word_match_at(src,pos,"function")) return 0;
  pos=skip_ws_comments_at(src,pos+8);
  if(src[pos]=='(') return 1;
  if(isalpha((unsigned char)src[pos]) || src[pos]=='_'){
    pos++;
    while(isalnum((unsigned char)src[pos]) || src[pos]=='_') pos++;
    pos=skip_ws_comments_at(src,pos);
    return src[pos]=='(';
  }
  return 0;
}
