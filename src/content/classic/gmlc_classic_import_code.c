/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_classic_import_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_imported_scripts(GmlcProject *project){
  for(int i = 0; i < project->n_scripts; ++i){
    free(project->scripts[i].id);
    free(project->scripts[i].name);
    free(project->scripts[i].source_path);
  }
  free(project->scripts);
  project->scripts = NULL;
  project->n_scripts = project->cap_scripts = 0;
  for(int i = 0; i < project->n_script_order; ++i) free(project->script_order_ids[i]);
  free(project->script_order_ids);
  project->script_order_ids = NULL;
  project->n_script_order = 0;
}

int gmlc_classic_import_scripts(const GmlcClassicManifest *classic,
                                GmlcProject *project, const char *cache_dir,
                                char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->scripts || project->n_scripts){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid script-import arguments");
    return 0;
  }
  uint32_t count = classic->inventory.resource_slots[GMLC_CLASSIC_SCRIPT];
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many script slots");
    return 0;
  }
  project->scripts = (GmlcScript*)calloc(count ? count : 1, sizeof(*project->scripts));
  project->script_order_ids = (char**)calloc(count ? count : 1, sizeof(*project->script_order_ids));
  if(!project->scripts || !project->script_order_ids){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating scripts");
    free_imported_scripts(project);
    return 0;
  }
  project->n_scripts = project->cap_scripts = (int)count;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_SCRIPT];
  for(uint32_t i = 0; i < count; ++i){
    if(!slots || !slots[i].exists) continue;
    char leaf[64];
    snprintf(leaf, sizeof(leaf), "classic_script_%06u.gml", i);
    const char *name = slots[i].name ? slots[i].name : "";
    const char *source = slots[i].source ? slots[i].source : "exit;\n";
    GmlcScript *script = &project->scripts[i];
    script->id = copy_string(name);
    script->name = copy_string(name);
    script->source_path = import_source_path(project,cache_dir,leaf,source,err,errcap);
    if(!script->id || !script->name || !script->source_path){
      if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: out of memory importing script %u", i);
      free_imported_scripts(project);
      return 0;
    }
    project->script_order_ids[project->n_script_order] = copy_string(name);
    if(!project->script_order_ids[project->n_script_order]){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory recording script order");
      free_imported_scripts(project);
      return 0;
    }
    ++project->n_script_order;
  }
  return 1;
}

static int text_reserve(ImportText *text, size_t extra){
  if(extra > SIZE_MAX - text->length - 1) return 0;
  size_t need = text->length + extra + 1;
  if(need <= text->capacity) return 1;
  size_t capacity = text->capacity ? text->capacity : 256;
  while(capacity < need){
    if(capacity > SIZE_MAX / 2){ capacity = need; break; }
    capacity *= 2;
  }
  char *data = (char*)realloc(text->data, capacity);
  if(!data) return 0;
  text->data = data;
  text->capacity = capacity;
  return 1;
}

static int text_append_n(ImportText *text, const char *value, size_t length){
  if(!text_reserve(text, length)) return 0;
  memcpy(text->data + text->length, value, length);
  text->length += length;
  text->data[text->length] = '\0';
  return 1;
}

int text_append(ImportText *text, const char *value){
  return text_append_n(text, value ? value : "", value ? strlen(value) : 0);
}

static int text_append_int(ImportText *text, int32_t value){
  char number[32];
  snprintf(number, sizeof(number), "%d", value);
  return text_append(text, number);
}

static int text_append_quoted(ImportText *text, const char *value){
  if(!text_append(text, "\"")) return 0;
  for(const unsigned char *p = (const unsigned char*)(value ? value : ""); *p; ++p){
    char escaped[2] = {(char)*p, '\0'};
    if(*p == '\\' || *p == '"'){
      if(!text_append(text, "\\")) return 0;
    } else if(*p == '\n'){
      if(!text_append(text, "\\n")) return 0;
      continue;
    } else if(*p == '\r'){
      if(!text_append(text, "\\r")) return 0;
      continue;
    }
    if(!text_append(text, escaped)) return 0;
  }
  return text_append(text, "\"");
}

/* An argument field holds one expression. A trailing statement separator or a following term
 * without an operator lies outside that expression and must not be spliced into the call. */
static int action_word_operator(const char *value, size_t start, size_t end){
  static const char *const words[]={"and","or","xor","not","div","mod","then"};
  for(size_t i=0;i<sizeof words/sizeof words[0];i++){
    size_t length=strlen(words[i]);
    if(start+length<=end && !strncmp(value+start,words[i],length) &&
       (start+length==end ||
        !(isalnum((unsigned char)value[start+length]) || value[start+length]=='_')))
      return 1;
  }
  return 0;
}
static size_t action_expression_length(const char *value){
  size_t length = value ? strlen(value) : 0;
  while(length && (isspace((unsigned char)value[length-1]) || value[length-1]==';')) length--;
  /* The field is read as one expression and nothing more. Where a complete term is followed by the
   * start of another with no operator between them the expression has ended, and the rest is text
   * the reader never reaches — a missing operator in an authored condition is exactly that. Keeping
   * it would splice unparsable text into the call and cost the whole event its code. */
  int depth=0;
  for(size_t at=0; at<length; at++){
    char ch=value[at];
    if(ch=='"' || ch=='\''){
      char quote=ch;
      for(at++; at<length && value[at]!=quote; at++)
        if(value[at]=='\\' && at+1<length) at++;
      continue;
    }
    if(ch=='(' || ch=='[' || ch=='{'){ depth++; continue; }
    if((ch==')' || ch==']' || ch=='}') && depth>0){ depth--; continue; }
    if(depth || !isspace((unsigned char)ch)) continue;
    size_t before=at;
    while(before && isspace((unsigned char)value[before-1])) before--;
    if(!before) continue;
    char last=value[before-1];
    if(!(isalnum((unsigned char)last) || last=='_' || last==')' || last==']' ||
         last=='"' || last=='\'')) continue;
    size_t word=before;
    while(word && (isalnum((unsigned char)value[word-1]) || value[word-1]=='_')) word--;
    if(word<before && action_word_operator(value,word,before)) continue;
    size_t after=at;
    while(after<length && isspace((unsigned char)value[after])) after++;
    if(after>=length) break;
    char next=value[after];
    if(!(isalpha((unsigned char)next) || next=='_' || isdigit((unsigned char)next))) continue;
    if(action_word_operator(value,after,length)) continue;
    return before;
  }
  return length;
}

static int emit_action_call(ImportText *text, const char *function_name,
                            char **arguments, uint32_t *argument_kinds,
                            uint32_t used_arguments, int append_relative, int relative){
  if(!function_name || !*function_name) return text_append(text, "/* empty action */");
  if(!text_append(text, function_name) || !text_append(text, "(")) return 0;
  for(uint32_t i = 0; i < used_arguments; ++i){
    if(i && !text_append(text, ",")) return 0;
    int quote = argument_kinds && argument_kinds[i] == 1;
    if(!quote && i == 0 && argument_kinds && argument_kinds[i] == 2 &&
       (!strcmp(function_name, "action_message") || !strcmp(function_name, "action_draw_text") ||
        !strcmp(function_name, "action_question"))){
      const char *value = arguments[i] ? arguments[i] : "";
      int plain_text = strchr(value, ':') != NULL;
      for(const unsigned char *p=(const unsigned char*)value; !plain_text && *p; ++p){
        if(isspace(*p)) plain_text=1;
        if(p!=(const unsigned char*)value && strchr("+-*/()[]\"'",*p)){ plain_text=0; break; }
      }
      quote=plain_text;
    }
    if(quote){
      if(!text_append_quoted(text, arguments[i])) return 0;
    } else {
      size_t length = action_expression_length(arguments[i]);
      if(!length){ if(!text_append(text, "0")) return 0; }
      else if(!text_append_n(text, arguments[i], length)) return 0;
    }
  }
  if(append_relative){
    if(used_arguments && !text_append(text,",")) return 0;
    if(!text_append(text,relative?"1":"0")) return 0;
  }
  return text_append(text, ")");
}

typedef enum {
  ACTION_NORMAL,
  ACTION_QUOTE,
  ACTION_LINE_COMMENT,
  ACTION_BLOCK_COMMENT
} ActionCodeState;

static ActionCodeState action_code_end_state(const char *code, char *open_quote){
  ActionCodeState state=ACTION_NORMAL;
  char quote='\0';
  for(size_t i=0; code && code[i]; ++i){
    char ch=code[i], next=code[i+1];
    if(state==ACTION_QUOTE){
      /* In GM6-GM8 a backslash is literal and does not hide the quote. */
      if(ch==quote) state=ACTION_NORMAL;
    } else if(state==ACTION_LINE_COMMENT){
      if(ch=='\n' || ch=='\r') state=ACTION_NORMAL;
    } else if(state==ACTION_BLOCK_COMMENT){
      if(ch=='*' && next=='/'){ state=ACTION_NORMAL; ++i; }
    } else if(ch=='"' || ch=='\''){
      state=ACTION_QUOTE; quote=ch;
    } else if(ch=='/' && next=='/'){
      state=ACTION_LINE_COMMENT; ++i;
    } else if(ch=='/' && next=='*'){
      state=ACTION_BLOCK_COMMENT; ++i;
    }
  }
  if(open_quote) *open_quote=state==ACTION_QUOTE?quote:'\0';
  return state;
}

static int emit_action_code_call(ImportText *text, const char *code,
                                 char **arguments, uint32_t *argument_kinds,
                                 uint32_t used_arguments){
  if(!text_append(text,"(function(){\n") || !text_append(text,code)) return 0;
  char open_quote='\0';
  ActionCodeState end_state=action_code_end_state(code,&open_quote);
  /* Close an unterminated string at the end of a Classic Execute Code action before
   * adding the structural wrapper used by the importer. */
  if(end_state==ACTION_QUOTE){
    char closing[2]={open_quote,'\0'};
    if(!text_append(text,closing)) return 0;
  }
  /* Each classic Execute Code action is compiled as a separate unit. An open
   * block comment therefore ends with that action; close it before appending
   * the synthetic function boundary used by the structural importer. */
  if(end_state==ACTION_BLOCK_COMMENT && !text_append(text,"\n*/")) return 0;
  if(!text_append(text,"\n})(")) return 0;
  for(uint32_t i=0;i<used_arguments;i++){
    if(i && !text_append(text,",")) return 0;
    if(argument_kinds && argument_kinds[i]==1){
      if(!text_append_quoted(text,arguments[i])) return 0;
    } else if(!text_append(text,arguments[i] && *arguments[i] ? arguments[i] : "0")) return 0;
  }
  return text_append(text,")");
}

/* The action list is structural rather than compiled source. Emit only block ends matched by an
 * open start, then close any starts still open when the list ends. */
int import_actions(ImportReader *r, ImportText *text){
  int block_depth = 0;
  uint32_t list_version, count;
  if(!import_u32(r, &list_version, "action-list version") || !import_u32(r, &count, "action count")) return 0;
  (void)list_version;
  for(uint32_t action_index = 0; action_index < count; ++action_index){
    uint32_t action_version = 0, library_id = 0, action_id = 0, kind = 0;
    uint32_t may_relative = 0, question = 0, applies = 0, type = 0;
    uint32_t used_arguments = 0, kind_count = 0, target = 0, relative = 0;
    uint32_t argument_count = 0, negate = 0;
    char *function_name = NULL, *code = NULL;
    uint32_t *argument_kinds = NULL;
    char **arguments = NULL;
    int ok = import_u32(r, &action_version, "action version") &&
      import_u32(r, &library_id, "action library") && import_u32(r, &action_id, "action id") &&
      import_u32(r, &kind, "action kind") && import_u32(r, &may_relative, "action relative capability") &&
      import_u32(r, &question, "action question flag") && import_u32(r, &applies, "action target flag") &&
      import_u32(r, &type, "action type") && import_copy_string(r, &function_name, "action function") &&
      import_copy_string(r, &code, "action code") && import_u32(r, &used_arguments, "used action arguments") &&
      import_u32(r, &kind_count, "action argument-kind count");
    if(!ok) goto action_done;
    if(kind_count > 1024 || used_arguments > kind_count){ ok = 0; goto action_done; }
    argument_kinds = (uint32_t*)calloc(kind_count ? kind_count : 1, sizeof(*argument_kinds));
    if(!argument_kinds){ ok = 0; goto action_done; }
    for(uint32_t i = 0; i < kind_count; ++i)
      if(!import_u32(r, &argument_kinds[i], "action argument kind")){ ok = 0; goto action_done; }
    if(!import_u32(r, &target, "action target") || !import_u32(r, &relative, "action relative flag") ||
       !import_u32(r, &argument_count, "action argument count") || argument_count > 1024){ ok = 0; goto action_done; }
    arguments = (char**)calloc(argument_count ? argument_count : 1, sizeof(*arguments));
    if(!arguments){ ok = 0; goto action_done; }
    for(uint32_t i = 0; i < argument_count; ++i)
      if(!import_copy_string(r, &arguments[i], "action argument")){ ok = 0; goto action_done; }
    if(!import_u32(r, &negate, "action negation flag")){ ok = 0; goto action_done; }
    (void)action_version; (void)library_id; (void)action_id; (void)may_relative;
    if(kind == 1){ ok = text_append(text, "{\n"); if(ok) block_depth++; }
    else if(kind == 2){
      if(block_depth > 0){ ok = text_append(text, "}\n"); if(ok) block_depth--; }
    }
    else if(kind == 3) ok = text_append(text, "else\n");
    else if(kind == 4) ok = text_append(text, "exit;\n");
    else if(kind == 5){
      ok = text_append(text, "repeat (") && text_append(text, argument_count && arguments[0][0] ? arguments[0] : "0") &&
           text_append(text, ")\n");
    } else if(kind == 6){
      const char *lhs = argument_count > 0 && arguments[0][0] ? arguments[0] : "__classic_variable";
      const char *rhs = argument_count > 1 && arguments[1][0] ? arguments[1] : "0";
      ok = text_append(text, lhs) && text_append(text, relative ? " += " : " = ") && text_append(text, rhs) && text_append(text, ";\n");
    } else {
      int wrapped_target = applies && !question && (int32_t)target != -1;
      if(wrapped_target){
        ok = text_append(text, "with (") && text_append_int(text, (int32_t)target) && text_append(text, ") {\n");
      }
      /* A relative action lowers to setup, execution and reset. Keep them one
       * statement so a preceding question or repeat owns all three. */
      if(ok && relative && !question) ok = text_append(text, "{\naction_set_relative(1);\n");
      if(ok && question) ok = text_append(text, "if (") && (!negate || text_append(text, "!"));
      if(ok){
        if(type == 2 || kind == 7){
          if(code && *code)
            ok = emit_action_code_call(text,code,arguments,argument_kinds,
              used_arguments < argument_count ? used_arguments : argument_count);
          else {
            const char *action_code = argument_count && arguments[0] && arguments[0][0] ?
              arguments[0] : "/* empty code action */";
            /* Execute Code is one D&D action, not the whole event. An
             * `exit` inside it ends that action before the dispatcher continues with later actions.
             * A small invoked function preserves that boundary after the action list is
             * normalized into one source file. */
            ok = emit_action_code_call(text,action_code,NULL,NULL,0);
          }
        }
        else ok = emit_action_call(text, function_name, arguments, argument_kinds,
                                   used_arguments < argument_count ? used_arguments : argument_count,
                                   question, relative!=0);
      }
      if(ok && question) ok = text_append(text, ")\n");
      else if(ok) ok = text_append(text, ";\n");
      if(ok && relative && !question) ok = text_append(text, "action_set_relative(0);\n}\n");
      if(ok && wrapped_target) ok = text_append(text, "}\n");
    }
action_done:
    for(uint32_t i = 0; arguments && i < argument_count; ++i) free(arguments[i]);
    free(arguments); free(argument_kinds); free(function_name); free(code);
    if(!ok){
      if(r->err && r->errcap && !r->err[0]) snprintf(r->err, r->errcap, "classic import: invalid action %u", action_index);
      return 0;
    }
  }
  while(block_depth > 0){
    if(!text_append(text, "}\n")) return 0;
    block_depth--;
  }
  return 1;
}
