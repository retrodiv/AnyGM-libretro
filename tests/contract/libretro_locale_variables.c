/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Locale values exposed through environment variables.
 *
 * Legacy content without locale builtins uses these variables. They answer from the resolved
 * locale so the Language setting reaches that content too. A selected language overrides the
 * hosting process; on Auto, a host value takes precedence and the resolved locale fills its
 * absence.
 *
 * Every other name is the process environment, memoised, and must stay that way. */
#include "libretro_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LibretroAdapter g_libretro;

/* The currently selected language, or NULL before the setting is touched. */
static const char *chosen_language;
const char *libretro_options_value(const char *key){
  return key && !strcmp(key,"anygm_language")?chosen_language:NULL;
}

/* The rest of the adapter is out of frame here; only the settings answer is under test. */
void libretro_vfs_services_init(AnygmHostServices *services){ (void)services; }
void libretro_log(enum retro_log_level level,const char *format,...){
  (void)level; (void)format;
}

static AnygmHostServices services;
static int failures;

static const char *answer(const char *name){
  return services.development_setting?services.development_setting(services.userdata,name):NULL;
}

static void expect(const char *what,const char *got,const char *want){
  int ok=want?(got && !strcmp(got,want)):(got==NULL);
  if(!ok){
    printf("FAIL %s: got %s, expected %s\n",what,got?got:"(none)",want?want:"(none)");
    failures++;
  }
}

/* Each case starts from an adapter that has resolved a locale and remembers nothing. */
static void begin(const char *language,const char *region,const char *chosen){
  memset(&g_libretro,0,sizeof g_libretro);
  snprintf(g_libretro.language,sizeof g_libretro.language,"%s",language);
  snprintf(g_libretro.region,sizeof g_libretro.region,"%s",region);
  chosen_language=chosen;
  libretro_host_services_init(&services);
}

int main(void){
  unsetenv("LANG");
  unsetenv("LANGUAGE");
  unsetenv("LC_ALL");

  /* Auto, and a host that says nothing: the resolved locale is the only answer there is. */
  begin("es","ES","Auto");
  expect("auto without a host LANG",answer("LANG"),"es_ES");
  expect("auto without a host LC_ALL",answer("LC_ALL"),"es_ES");
  expect("auto without a host LANGUAGE",answer("LANGUAGE"),"es");

  /* Auto, and a host that does say something: the desktop keeps its own locale. */
  setenv("LANG","fr_FR.UTF-8",1);
  begin("es","ES",NULL);
  expect("auto behind a host LANG",answer("LANG"),"fr_FR.UTF-8");
  begin("es","ES","Auto");
  expect("auto behind a host LANG, set to Auto",answer("LANG"),"fr_FR.UTF-8");

  /* A language the player chose outranks the process it happens to run in. */
  begin("de","DE","de");
  expect("a chosen language over a host LANG",answer("LANG"),"de_DE");
  expect("a chosen language over a host LANGUAGE",answer("LANGUAGE"),"de");

  /* A region the frontend never named still spells a locale a game can parse. */
  begin("it","","it");
  expect("a chosen language without a region",answer("LANG"),"it_US");

  /* An adapter with no locale at all falls back to the host rather than inventing one. */
  begin("","","es");
  expect("no resolved locale",answer("LANG"),"fr_FR.UTF-8");
  unsetenv("LANG");
  begin("","","es");
  expect("no resolved locale and no host LANG",answer("LANG"),NULL);

  /* Every other name is still the environment, and answering twice answers the same. */
  setenv("ANYGM_TEST_SETTING","visible",1);
  begin("es","ES","es");
  expect("an unrelated setting",answer("ANYGM_TEST_SETTING"),"visible");
  expect("an unrelated setting, asked twice",answer("ANYGM_TEST_SETTING"),"visible");
  expect("a setting nobody set",answer("ANYGM_TEST_MISSING"),NULL);

  /* A locale answered once is not remembered, so a setting changed mid-session is seen. The
   * adapter re-resolves the locale when the option changes; the answer follows that, not the
   * first reply. */
  begin("es","ES","Auto");
  expect("auto first",answer("LANG"),"es_ES");
  chosen_language="pt";
  snprintf(g_libretro.language,sizeof g_libretro.language,"pt");
  snprintf(g_libretro.region,sizeof g_libretro.region,"PT");
  expect("chosen after auto",answer("LANG"),"pt_PT");

  if(failures){
    printf("libretro locale variables: %d failure(s)\n",failures);
    return 1;
  }
  printf("libretro locale variables: ok\n");
  return 0;
}
