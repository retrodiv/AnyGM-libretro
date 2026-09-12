/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Options are declared once, in the categorized form, and the flat declaration older hosts
 * understand is derived from that same table. Two hand-maintained lists drift, and the one that
 * drifts is always the one the host in front of the player happens to read. */
static const struct retro_core_option_v2_category g_categories[]={
  {"video","Video","Frame geometry, rendering and content shaders."},
  {"input","Input","Controllers and pointer."},
  {"development","Development","Tools for exercising content."},
  {NULL,NULL,NULL}
};

/* The array holds a fixed number of entries, of which one closes the list and one offers the
 * whole game. What is left is how many rooms a single stretch can name. */
#define ROOM_CHOICE_LIMIT (RETRO_NUM_CORE_OPTION_VALUES_MAX-2)

static struct retro_core_option_v2_definition g_definitions[]={
  {"anygm_alpha_cull","Transparency culling",NULL,
   "Skips pixels too faint to see. None draws every one; the rest trade faint detail for speed.",
   NULL,"video",
   {{"None",NULL},{"Light",NULL},{"Medium",NULL},{"High",NULL},{"Maximum",NULL},{NULL,NULL}},
   "High"},
  {"anygm_render_game_resolution","Render at game resolution",NULL,
   "Delivers the game's own raster and lets this program scale it. Monitor width and height use "
   "Game Base on this path. Off renders at the effective presentation window: the monitor "
   "dimensions supply that extent when set, while Game Base follows the game's request.",
   NULL,"video",
   {{"On",NULL},{"Off",NULL},{NULL,NULL}},
   "On"},
  {"anygm_adjust_crt_tv","Adjust for 4:3 CRT TV",NULL,
   "With Render at game resolution on, fits images wider than 364 pixels or taller than 244 "
   "pixels into 640x480 using sharp bilinear filtering. Preserves the image's aspect ratio "
   "with centered black bars. Limits are checked after Aspect Ratio force is applied. "
   "Smaller images keep their resolution. The frontend controls the TV video mode.",
   NULL,"video",
   {{"Off",NULL},{"On",NULL},{NULL,NULL}},
   "Off"},
  {"anygm_width_resolution","Monitor width",NULL,
   "With Render at game resolution off, reports this virtual monitor width to the game and "
   "supplies the effective presentation-window and framebuffer width. Game Base follows the "
   "game's request.",
   NULL,"video",
   {{"Game Base",NULL},{"64",NULL},{"128",NULL},{"144",NULL},{"160",NULL},{"176",NULL},
    {"192",NULL},{"200",NULL},{"224",NULL},{"240",NULL},{"256",NULL},{"288",NULL},{"300",NULL},
    {"320",NULL},{"352",NULL},{"360",NULL},{"384",NULL},{"400",NULL},{"416",NULL},{"448",NULL},
    {"480",NULL},{"512",NULL},{"560",NULL},{"576",NULL},{"600",NULL},{"640",NULL},{"704",NULL},
    {"720",NULL},{"768",NULL},{"800",NULL},{"832",NULL},{"854",NULL},{"896",NULL},{"960",NULL},
    {"1024",NULL},{"1080",NULL},{"1152",NULL},{"1200",NULL},{"1280",NULL},{"1360",NULL},
    {"1366",NULL},{"1400",NULL},{"1440",NULL},{"1536",NULL},{"1600",NULL},{"1680",NULL},
    {"1728",NULL},{"1792",NULL},{"1920",NULL},{"2048",NULL},{"2160",NULL},{"2304",NULL},
    {"2400",NULL},{"2560",NULL},{"2880",NULL},{"3200",NULL},{"3440",NULL},{"3840",NULL},
    {NULL,NULL}},
   "Game Base"},
  {"anygm_height_resolution","Monitor height",NULL,
   "With Render at game resolution off, reports this virtual monitor height to the game and "
   "supplies the effective presentation-window and framebuffer height. Game Base follows the "
   "game's request.",
   NULL,"video",
   {{"Game Base",NULL},{"64",NULL},{"128",NULL},{"144",NULL},{"160",NULL},{"176",NULL},
    {"180",NULL},{"192",NULL},{"200",NULL},{"216",NULL},{"224",NULL},{"240",NULL},{"256",NULL},
    {"270",NULL},{"288",NULL},{"300",NULL},{"320",NULL},{"350",NULL},{"360",NULL},{"384",NULL},
    {"400",NULL},{"432",NULL},{"448",NULL},{"450",NULL},{"480",NULL},{"512",NULL},{"540",NULL},
    {"576",NULL},{"600",NULL},{"640",NULL},{"720",NULL},{"768",NULL},{"800",NULL},{"864",NULL},
    {"900",NULL},{"960",NULL},{"1024",NULL},{"1050",NULL},{"1080",NULL},{"1152",NULL},
    {"1200",NULL},{"1280",NULL},{"1350",NULL},{"1440",NULL},{"1536",NULL},{"1600",NULL},
    {"1800",NULL},{"1920",NULL},{"2160",NULL},{NULL,NULL}},
   "Game Base"},
  {"anygm_aspect_ratio_force","Aspect Ratio force (Experimental)",NULL,
   "With Render at game resolution on, overrides the shape of the game's own raster before this "
   "program hands it over. None keeps the shape the game draws. Off that path the monitor "
   "dimensions already state the shape, so this neither acts nor is offered.",
   NULL,"video",
   {{"None",NULL},{"4:3",NULL},{"16:9",NULL},{"16:10",NULL},{"21:9",NULL},{NULL,NULL}},
   "None"},
  {"anygm_gamepad","RetroPad behavior",NULL,
   "Game gamepad lets a game with joystick or gamepad support read the RetroPad itself, the way it "
   "read a joystick originally, and falls back to keyboard emulation for a game with no pad "
   "support. Keyboard emulation always presents the RetroPad as the game's keyboard controls: both "
   "classic action rows (ZXCV and ASDF), the arrows, Enter, Space and Shift.",
   NULL,"input",
   {{"Game gamepad",NULL},{"Keyboard emulation",NULL},{NULL,NULL}},
   "Game gamepad"},
  {"anygm_mouse","Mouse input",NULL,
   "How pointer movement reaches the game. Auto follows what the content expects.",
   NULL,"input",
   {{"Auto",NULL},{"Absolute (pointer)",NULL},{"Relative (delta)",NULL},{NULL,NULL}},
   "Auto"},
  {"anygm_content_shader_readback","Game shaders (GLSL)",NULL,
   "Controls the game's GLSL programs that the software renderer does not already recognize. "
#if ANYGM_HARDWARE_RENDER
   "Performance runs them on OpenGL, uses the previous frame's result for draws inside a frame, "
   "and draws them plain for the rest of the session if they exceed the frame budget. Exact waits "
   "for the current result and can be much slower. Terminal GLSL presentations are exact in both "
   "modes. "
#endif
   "OFF (but report available) does not run them, but shader_is_compiled() reports valid "
   "sampling programs available, so unsupported effects draw plain. OFF does not run them and "
   "reports only software-recognized families available, allowing the game to choose its fallback. "
   "Procedural programs that sample no picture remain unavailable without a device. Recognized "
   "families keep running on the processor in every mode. "
#if ANYGM_HARDWARE_RENDER
   "A context is requested only when the "
   "loaded content contains a candidate program. Requires a compatible OpenGL or OpenGL ES frontend "
   "video driver. "
#endif
   "Frontend shaders are unaffected. Changes take effect after closing and reopening "
   "the content.",
   NULL,"video",
   {
#if ANYGM_HARDWARE_RENDER
    {"Performance",NULL},{"Exact",NULL},
#endif
    /* Stored values remain stable while the labels follow the frontend's canonical boolean style. */
    {"Off (but report available)","OFF (but report available)"},{"Off","OFF"},{NULL,NULL}},
   "Off (but report available)"},
#if ANYGM_HARDWARE_RENDER
  {"anygm_hybrid_gpu","Hybrid GPU rendering (Experimental)",NULL,
   "Chooses whether eligible final scaling and composition passes are executed on a graphics "
   "device instead of the processor. None keeps them on the processor. OpenGL can avoid building "
   "the final output frame on the processor. Passes that cannot be reproduced exactly on the "
   "graphics device continue using the software renderer. Requires a frontend running a compatible "
   "OpenGL or OpenGL ES video driver. Changes take effect after closing and reopening the content.",
   NULL,"video",
   {{"None",NULL},{"OpenGL","OpenGL / OpenGL ES"},{NULL,NULL}},
   "None"},
#endif
  {"anygm_content_overrides","Content override directives",NULL,
   "Applies override directives from system defaults, .anygm anchors, and matching payload "
   "SHA-256 sections (frozen variables, aspect patches, a development menu). Off ignores this "
   "channel without changing host cheats. Save states remember whether directives "
   "were active and only load under the same setting.",
   NULL,"development",
   {{"On",NULL},{"Off",NULL},{NULL,NULL}},
   "On"},
  /* These are the exact locale strings exposed by the runtime: lowercase language and uppercase
   * region. Auto follows the frontend language and its paired region. Locale environment variables
   * expose the same values for content without the locale builtins. */
  {"anygm_language","Language",NULL,
   "The language the game is told it is running in, through os_get_language() and the locale "
   "environment variables. Auto follows the frontend's language setting. Takes effect on "
   "restart.",
   NULL,"development",
   {{"Auto",NULL},{"en",NULL},{"es",NULL},{"fr",NULL},{"de",NULL},{"it",NULL},{"pt",NULL},
    {"nl",NULL},{"pl",NULL},{"ru",NULL},{"uk",NULL},{"cs",NULL},{"sv",NULL},{"fi",NULL},
    {"no",NULL},{"hu",NULL},{"el",NULL},{"tr",NULL},{"ca",NULL},{"ar",NULL},{"ja",NULL},
    {"ko",NULL},{"zh",NULL},{"vi",NULL},{"th",NULL},{NULL,NULL}},
   "Auto"},
  {"anygm_region","Region",NULL,
   "The region the game is told it is running in, through os_get_region() and the locale "
   "environment variables. Auto pairs it with the language. Takes effect on restart.",
   NULL,"development",
   {{"Auto",NULL},{"US",NULL},{"GB",NULL},{"ES",NULL},{"FR",NULL},{"DE",NULL},{"IT",NULL},
    {"BR",NULL},{"PT",NULL},{"NL",NULL},{"PL",NULL},{"RU",NULL},{"UA",NULL},{"CZ",NULL},
    {"SE",NULL},{"FI",NULL},{"NO",NULL},{"HU",NULL},{"GR",NULL},{"TR",NULL},{"SA",NULL},
    {"JP",NULL},{"KR",NULL},{"CN",NULL},{"TW",NULL},{"VN",NULL},{"TH",NULL},{NULL,NULL}},
   "Auto"},
  /* Both start-room entries are filled in once content is loaded and its rooms are known. */
  {"anygm_start_room_page","Start room range",NULL,
   "Which stretch of rooms the chooser below offers. Only games with more rooms than one list "
   "can hold need this.",
   NULL,"development",
   {{"0",NULL},{NULL,NULL}},
   "0"},
  {"anygm_start_room","Start room",NULL,
   "Boots straight into the chosen room instead of starting the game. Takes effect on restart.",
   NULL,"development",
   {{"Full game",NULL},{NULL,NULL}},
   "Full game"},
  {"anygm_clear_local_data","Clear local data on load",NULL,
   "Deletes everything this game has written before it loads, so the next run behaves like the "
   "first on this machine.",
   NULL,"development",
   {{"Off",NULL},{"On",NULL},{NULL,NULL}},
   "Off"},
  {NULL,NULL,NULL,NULL,NULL,NULL,{{NULL,NULL}},NULL}
};

static const struct retro_core_options_v2 g_options_v2={
  (struct retro_core_option_v2_category *)g_categories,
  g_definitions
};

/* Every room the loaded content declares, whatever the declaration can hold. */
static char **g_room_choice_text;
static size_t g_room_choice_count;

/* Categories arrived with the second revision of the option interface. Older hosts are given the
 * flat declaration instead, which carries no grouping and no help text but every value. */
static unsigned g_options_version;
static struct retro_variable *g_flat_variables;
static char **g_flat_storage;
static size_t g_flat_count;
static int g_published_ranges=-1;
static int g_published_logical_raster=-1;

static int update_option_visibility(void);

static void invalidate_published_visibility(void){
  g_published_ranges=-1;
  g_published_logical_raster=-1;
}

static void free_flat_variables(void){
  if(g_flat_storage){
    for(size_t i=0;i<g_flat_count;i++) free(g_flat_storage[i]);
    free(g_flat_storage);
    g_flat_storage=NULL;
  }
  free(g_flat_variables);
  g_flat_variables=NULL;
  g_flat_count=0;
}

/* The flat form takes the default from whichever value is listed first, so the default leads and
 * the remaining values follow in their declared order. */
static char *flat_value_text(const struct retro_core_option_v2_definition *definition){
  /* The room chooser is the one list a fixed-size declaration cannot hold, and this form has no
   * such ceiling, so it names every room rather than the stretch the other form was given. */
  int every_room=!strcmp(definition->key,"anygm_start_room") && g_room_choice_count>0;
  size_t capacity=strlen(definition->desc)+16;
  if(every_room){
    capacity+=strlen("Full game")+1;
    for(size_t i=0;i<g_room_choice_count;i++) capacity+=strlen(g_room_choice_text[i])+1;
  }
  else for(size_t i=0;definition->values[i].value;i++)
    capacity+=strlen(definition->values[i].value)+1;
  char *text=malloc(capacity);
  if(!text) return NULL;
  int written=snprintf(text,capacity,"%s; %s",definition->desc,
                       definition->default_value?definition->default_value:
                       definition->values[0].value);
  if(written<0){ free(text); return NULL; }
  size_t used=(size_t)written;
  if(every_room){
    for(size_t i=0;i<g_room_choice_count;i++){
      int added=snprintf(text+used,capacity-used,"|%s",g_room_choice_text[i]);
      if(added<0) break;
      used+=(size_t)added;
    }
    return text;
  }
  for(size_t i=0;definition->values[i].value;i++){
    const char *value=definition->values[i].value;
    if(definition->default_value && !strcmp(value,definition->default_value)) continue;
    int added=snprintf(text+used,capacity-used,"|%s",value);
    if(added<0) break;
    used+=(size_t)added;
  }
  return text;
}

static void publish_flat_variables(void){
  free_flat_variables();
  size_t count=0;
  while(g_definitions[count].key) count++;
  g_flat_variables=calloc(count+1,sizeof *g_flat_variables);
  g_flat_storage=calloc(count?count:1,sizeof *g_flat_storage);
  if(!g_flat_variables || !g_flat_storage){ free_flat_variables(); return; }
  size_t published=0;
  for(size_t i=0;i<count;i++){
    /* The flat form has no ceiling on values, so every room is named in one list and the stretch
     * selector that exists to work around that ceiling has nothing to select. */
    if(!strcmp(g_definitions[i].key,"anygm_start_room_page")) continue;
    g_flat_storage[published]=flat_value_text(&g_definitions[i]);
    g_flat_variables[published].key=g_definitions[i].key;
    g_flat_variables[published].value=g_flat_storage[published];
    published++;
  }
  g_flat_count=published;
  if(g_libretro.environment)
    g_libretro.environment(RETRO_ENVIRONMENT_SET_VARIABLES,g_flat_variables);
}

static struct retro_core_option_definition *g_v1_definitions;

static void free_v1_definitions(void){
  free(g_v1_definitions);
  g_v1_definitions=NULL;
}

/* The version-1 declaration is the version-2 one without categories: same keys, same descriptions,
 * same value lists and defaults. A version-1 frontend given the flat form instead loses every
 * per-option description, which is the whole difference between a settings screen a player can
 * read and a list of names. */
static void publish_v1_options(void){
  free_v1_definitions();
  size_t count=0;
  while(g_definitions[count].key) count++;
  g_v1_definitions=calloc(count+1,sizeof *g_v1_definitions);
  if(!g_v1_definitions){ publish_flat_variables(); return; }
  for(size_t i=0;i<count;i++){
    g_v1_definitions[i].key=g_definitions[i].key;
    g_v1_definitions[i].desc=g_definitions[i].desc;
    g_v1_definitions[i].info=g_definitions[i].info;
    g_v1_definitions[i].default_value=g_definitions[i].default_value;
    memcpy(g_v1_definitions[i].values,g_definitions[i].values,
           sizeof g_v1_definitions[i].values);
  }
  g_libretro.environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS,(void *)g_v1_definitions);
}

static void publish_options(void){
  if(!g_libretro.environment) return;
  if(g_options_version>=2){
    g_libretro.environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2,
                           (void *)&g_options_v2);
  }
  else if(g_options_version==1) publish_v1_options();
  else publish_flat_variables();
  /* Replacing a declaration gives the frontend a fresh default-visible copy. The values may be
   * unchanged, but every display hint belongs to the replaced copy and has to be sent again. */
  invalidate_published_visibility();
}

static bool options_update_display(void);

void libretro_options_register(void){
  if(!g_libretro.environment) return;
  g_options_version=0;
  if(!g_libretro.environment(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION,&g_options_version))
    g_options_version=0;
  publish_options();
  struct retro_core_options_update_display_callback update={options_update_display};
  g_libretro.environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK,&update);
  update_option_visibility();
}

static const char *option_value(const char *key){
  struct retro_variable variable={key,NULL};
  if(!g_libretro.environment ||
     !g_libretro.environment(RETRO_ENVIRONMENT_GET_VARIABLE,&variable)) return NULL;
  return variable.value;
}

/* Locale resolution happens beside the frontend language query. The returned string belongs to
 * the frontend and only until the next query, so callers copy values they retain. */
const char *libretro_options_value(const char *key){ return option_value(key); }

#if ANYGM_HARDWARE_RENDER
static int hybrid_gpu_selected(void){
  const char *value=option_value("anygm_hybrid_gpu");
  return value && !strcmp(value,"OpenGL");
}
#endif

static uint32_t option_on(const char *key,uint32_t fallback){
  const char *value=option_value(key);
  if(!value) return fallback;
  return !strcmp(value,"On")?1u:0u;
}


static uint32_t option_resolution(const char *key){
  const char *value=option_value(key);
  if(!value || !strcmp(value,"Game Base")) return 0;
  long parsed=strtol(value,NULL,10);
  return parsed>0?(uint32_t)parsed:0;
}

/* ---- the room chooser -------------------------------------------------------------------
 * The choices only exist once content is loaded, so what the entry point declares can only be
 * the whole game; a host asking which room to enter has nothing to show until they are published
 * again. Each choice carries its index first, which is what the reader converts back, and the
 * content's own name after it, because an index alone names nothing.
 *
 * A categorized declaration holds a fixed number of values, which no game is obliged to fit
 * inside. Those are offered a stretch at a time, with a second option selecting the stretch;
 * the flat declaration has no such ceiling and receives every room at once. */
static struct retro_core_option_v2_definition *definition_for(const char *key){
  for(size_t i=0;g_definitions[i].key;i++)
    if(!strcmp(g_definitions[i].key,key)) return &g_definitions[i];
  return NULL;
}

static char **g_page_choice_text;
static size_t g_page_choice_count;
static uint32_t g_room_count;
static uint32_t g_room_page;

static void free_text_block(char ***block,size_t *count){
  if(*block) for(size_t i=0;i<*count;i++) free((*block)[i]);
  free(*block);
  *block=NULL;
  *count=0;
}

static size_t room_page_span(void){
  return g_options_version>=2?(size_t)ROOM_CHOICE_LIMIT:(size_t)g_room_count;
}

static size_t room_page_total(void){
  size_t span=room_page_span();
  if(!span || !g_room_count) return 1;
  return ((size_t)g_room_count+span-1)/span;
}

static uint32_t room_page_from_value(const char *value){
  if(!value) return 0;
  char *end=NULL;
  unsigned long low=strtoul(value,&end,10);
  size_t span=room_page_span();
  if(end==value || !span) return 0;
  return (uint32_t)(low/span);
}

static void fill_values(struct retro_core_option_v2_definition *definition,
                       char **text,size_t count,const char *first){
  size_t slot=0;
  if(first && slot+1<RETRO_NUM_CORE_OPTION_VALUES_MAX){
    definition->values[slot].value=first;
    definition->values[slot].label=NULL;
    slot++;
  }
  for(size_t i=0;i<count && slot+1<RETRO_NUM_CORE_OPTION_VALUES_MAX;i++,slot++){
    definition->values[slot].value=text[i];
    definition->values[slot].label=NULL;
  }
  definition->values[slot].value=NULL;
  definition->values[slot].label=NULL;
}

void libretro_options_publish_rooms(void){
  if(!g_libretro.environment || !g_libretro.engine) return;
  struct retro_core_option_v2_definition *rooms=definition_for("anygm_start_room");
  struct retro_core_option_v2_definition *pages=definition_for("anygm_start_room_page");
  if(!rooms || !pages) return;
  g_room_count=0;
  if(anygm_get_room_count(g_libretro.engine,&g_room_count)!=ANYGM_OK) g_room_count=0;

  size_t span=room_page_span();
  size_t total=room_page_total();
  /* The stretch is read here rather than left to the settings pass: content is loaded after that
   * pass has already run, so on the first publication there is nothing for it to have read. */
  const char *selected=option_value("anygm_start_room_page");
  if(selected) g_room_page=room_page_from_value(selected);
  if(g_room_page>=total) g_room_page=0;
  size_t first=(size_t)g_room_page*span;
  size_t last=first+span;
  if(last>g_room_count) last=g_room_count;

  free_text_block(&g_room_choice_text,&g_room_choice_count);
  if(last>first){
    g_room_choice_text=calloc(last-first,sizeof *g_room_choice_text);
    if(g_room_choice_text){
      for(size_t index=first;index<last;index++){
        char name[128];
        if(anygm_get_room_name(g_libretro.engine,(uint32_t)index,name,sizeof name)!=ANYGM_OK)
          snprintf(name,sizeof name,"room");
        /* A separator inside a value would split it into two choices the reader cannot convert. */
        for(char *cursor=name;*cursor;cursor++) if(*cursor=='|') *cursor='/';
        char choice[160];
        snprintf(choice,sizeof choice,"%u: %s",(unsigned)index,name[0]?name:"room");
        char *stored=malloc(strlen(choice)+1);
        if(!stored) break;
        memcpy(stored,choice,strlen(choice)+1);
        g_room_choice_text[g_room_choice_count++]=stored;
      }
    }
  }
  fill_values(rooms,g_room_choice_text,g_room_choice_count,"Full game");

  free_text_block(&g_page_choice_text,&g_page_choice_count);
  if(total>1){
    g_page_choice_text=calloc(total,sizeof *g_page_choice_text);
    if(g_page_choice_text){
      for(size_t page=0;page<total;page++){
        size_t low=page*span, high=low+span-1;
        if(high>=g_room_count) high=g_room_count?g_room_count-1:0;
        char choice[64];
        snprintf(choice,sizeof choice,"%lu-%lu",(unsigned long)low,(unsigned long)high);
        char *stored=malloc(strlen(choice)+1);
        if(!stored) break;
        memcpy(stored,choice,strlen(choice)+1);
        g_page_choice_text[g_page_choice_count++]=stored;
      }
    }
  }
  fill_values(pages,g_page_choice_text,g_page_choice_count,NULL);
  if(!g_page_choice_count){
    /* A single stretch needs no selector, but the declaration still needs one legal value. */
    pages->values[0].value="0";
    pages->values[0].label=NULL;
    pages->values[1].value=NULL;
    pages->values[1].label=NULL;
  }
  pages->default_value=pages->values[0].value;

  /* Rooms beyond the published list are reachable through the range selector. Without one, they
   * are not, and a short list must not read as a short game. */
  if(g_room_choice_count<(size_t)g_room_count && g_page_choice_count<=1)
    libretro_log(RETRO_LOG_WARN,"Start room lists %lu of %u rooms and offers no range to reach "
                 "the rest\n",(unsigned long)g_room_choice_count,(unsigned)g_room_count);
  publish_options();
  update_option_visibility();
}

/* Options that cannot act are hidden rather than left to be tried: the range selector when every
 * room already fits in one list, and the two halves of the raster choice. Rendering at game resolution
 * delivers the game's own raster, so the forced shape has one to reshape and the monitor
 * dimensions have nothing to size; rendering at the presentation window is the reverse.
 */
static int update_option_visibility(void){
  if(!g_libretro.environment) return 0;
  static const char *const monitor_dimensions[]={
    "anygm_width_resolution","anygm_height_resolution",NULL
  };
  int ranges=g_page_choice_count>1?1:0;
  int logical_raster=option_on("anygm_render_game_resolution",1)?1:0;
  if(ranges==g_published_ranges && logical_raster==g_published_logical_raster)
    return 0;
  struct retro_core_option_display display;
  display.key="anygm_start_room_page";
  display.visible=ranges?true:false;
  g_libretro.environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY,&display);
  display.visible=logical_raster?false:true;
  for(size_t i=0;monitor_dimensions[i];i++){
    display.key=monitor_dimensions[i];
    g_libretro.environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY,&display);
  }
  display.key="anygm_aspect_ratio_force";
  display.visible=logical_raster?true:false;
  g_libretro.environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY,&display);
  display.key="anygm_adjust_crt_tv";
  g_libretro.environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY,&display);
  g_published_ranges=ranges;
  g_published_logical_raster=logical_raster;
  return 1;
}

/* Hosts ask through this while their menu is open, which is the only moment the answer can reach
 * the player who just changed the setting it depends on. */
static bool options_update_display(void){
  return update_option_visibility()?true:false;
}

/* Put the two room selectors back to the single value each was declared with. The strings are
 * literals in the declaration table, so the restored entries own nothing. */
static void restore_room_declarations(void){
  struct retro_core_option_v2_definition *rooms=definition_for("anygm_start_room");
  struct retro_core_option_v2_definition *pages=definition_for("anygm_start_room_page");
  if(rooms){
    rooms->values[0].value="Full game";
    rooms->values[0].label=NULL;
    rooms->values[1].value=NULL;
    rooms->values[1].label=NULL;
  }
  if(pages){
    pages->values[0].value="0";
    pages->values[0].label=NULL;
    pages->values[1].value=NULL;
    pages->values[1].label=NULL;
  }
}

/* The room names and the flat declaration are held for as long as a host may read them, which is
 * until the core is torn down. Content loaded again replaces them; a core shut down releases
 * them, so a host that opens one game after another does not accumulate the names of all of them.
 */
void libretro_options_release(void){
  /* The two room selectors point their value lists at the heap blocks freed below. The
   * declaration table itself is static and outlives this call: a host that keeps the library
   * resident across deinit and init - which STATIC_LINKING makes the normal case, and which any
   * frontend that does not close the library does too - republishes that same table on the next
   * init. Restoring the declared single value is what keeps the republished table from carrying
   * pointers into memory this function has already returned. */
  restore_room_declarations();
  free_v1_definitions();
  free_text_block(&g_room_choice_text,&g_room_choice_count);
  free_text_block(&g_page_choice_text,&g_page_choice_count);
  free_flat_variables();
  g_room_count=0;
  g_room_page=0;
  invalidate_published_visibility();
}

void libretro_options_apply(bool all_fields){
  if(!g_libretro.engine) return;
  AnygmConfig *config=&g_libretro.config;
  config->struct_size=sizeof *config;
  uint32_t present_logical_raster=option_on("anygm_render_game_resolution",1);
  /* A logical-raster frame has no monitor-sized drawing space. The frontend retains the hidden
   * selections, so reading them again when this path is turned off restores them without a
   * compatibility setting or a second source of truth. */
  config->monitor_width=present_logical_raster?0u:
                        option_resolution("anygm_width_resolution");
  config->monitor_height=present_logical_raster?0u:
                         option_resolution("anygm_height_resolution");
  /* The forced shape reshapes the game's own raster, which only the logical-raster path delivers.
   * Off that path the monitor dimensions already state the shape, so a selection made earlier must
   * not keep acting from a setting the player can no longer see. It is read again rather than
   * cleared, so returning to this path restores it exactly as the monitor dimensions are restored. */
  const char *value=present_logical_raster?option_value("anygm_aspect_ratio_force"):NULL;
  config->aspect_mode=value&&!strcmp(value,"4:3")?1u:
                      value&&!strcmp(value,"16:9")?2u:
                      value&&!strcmp(value,"21:9")?3u:
                      value&&!strcmp(value,"16:10")?4u:0u;
  value=option_value("anygm_mouse");
  config->mouse_mode=value&&strstr(value,"Absolute")?1u:
                     value&&strstr(value,"Relative")?2u:0u;
  /* Neither of these is offered to a player any more. God mode needs an object named through a
   * development setting before it does anything, and that is where it now lives; the room-skip
   * button belonged with it. Both are pinned off so a value stored under the old keys cannot
   * survive as a setting nothing in the menu explains. */
  config->god_mode=0u;
  config->room_skip_button=0u;
  /* Both graphics policies are frozen from content preparation through unload. The menu may
   * retain a newly selected value while a session is running, but it becomes effective only on
   * the next load, when a context can be negotiated without splitting the frame pipeline. */
  if(!g_libretro.loaded && !g_libretro.prepared){
    const char *shader=option_value("anygm_content_shader_readback");
#if ANYGM_HARDWARE_RENDER
    g_libretro.hybrid_gpu_selected=hybrid_gpu_selected()?true:false;
    g_libretro.content_glsl_selected=
      shader && (!strcmp(shader,"Performance") || !strcmp(shader,"Exact"));
    config->content_shader_readback=shader && !strcmp(shader,"Performance")
      ?ANYGM_SHADER_READBACK_BUDGETED:
      shader && !strcmp(shader,"Exact")
        ?ANYGM_SHADER_READBACK_ALWAYS:ANYGM_SHADER_READBACK_NEVER;
    config->report_all_shaders_compiled=shader && !strcmp(shader,"Off")?0u:1u;
#else
    g_libretro.hybrid_gpu_selected=false;
    g_libretro.content_glsl_selected=false;
    config->content_shader_readback=ANYGM_SHADER_READBACK_NEVER;
    config->report_all_shaders_compiled=shader && !strcmp(shader,"Off")?0u:1u;
#endif
    /* These are load-time intents. Content inspection and context negotiation finalize them before
     * the first authored event runs. */
    config->content_shader_device_expected=g_libretro.content_glsl_selected?1u:0u;
    config->hybrid_gpu_presentation=g_libretro.hybrid_gpu_selected?1u:0u;
  }
  config->content_overrides=option_on("anygm_content_overrides",1);
  /* The default option resolves from content before any menu is available. */
  /* The public option offers two states. An unconditional pad state remains available
   * internally; automatic mode instead follows the reference scan. */
  { const char *behavior=option_value("anygm_gamepad");
    config->gamepad_connected=
      (behavior && !strcmp(behavior,"Keyboard emulation")) ? 0u : ANYGM_GAMEPAD_AUTO; }
  /* The names describe how much is dropped, and the thresholds rise with them. An unrecognised
   * name resolves to the shipped amount rather than to none, so a value left behind by a host
   * cannot quietly land on the slowest setting. */
  value=option_value("anygm_alpha_cull");
  config->fast_alpha_cull=!value?24u:
                          !strcmp(value,"None")?0u:
                          !strcmp(value,"Light")?1u:
                          !strcmp(value,"Medium")?4u:
                          !strcmp(value,"Maximum")?32u:24u;
  value=option_value("anygm_start_room");
  config->start_room=value&&strcmp(value,"Full game")?(int32_t)strtol(value,NULL,10):-1;
  /* Rendering at the effective presentation window costs a full software upscale when it is
     larger than the view. Rasterizing at the view is the default; the window-matching path stays
     available for content and host-monitor layouts that depend on it. */
  config->present_logical_raster=present_logical_raster;
  config->adjust_crt_tv=present_logical_raster && option_on("anygm_adjust_crt_tv",0);
  config->clear_local_data=option_on("anygm_clear_local_data",0);

  /* Choosing another stretch of rooms changes which rooms the chooser holds, not any setting the
   * runtime reads, so the list is rebuilt before the player opens it again. */
  value=option_value("anygm_start_room_page");
  if(value && g_page_choice_count>1 && room_page_from_value(value)!=g_room_page)
    libretro_options_publish_rooms();
  update_option_visibility();

  AnygmConfigDelta delta;
  memset(&delta,0,sizeof delta);
  delta.struct_size=sizeof delta;
  delta.values=*config;
  delta.fields=all_fields?UINT64_MAX:
      ANYGM_CONFIG_MONITOR_WIDTH|ANYGM_CONFIG_MONITOR_HEIGHT|ANYGM_CONFIG_ASPECT_MODE|
      ANYGM_CONFIG_MOUSE_MODE|ANYGM_CONFIG_ROOM_SKIP_BUTTON|ANYGM_CONFIG_GOD_MODE|
      ANYGM_CONFIG_REPORT_ALL_SHADERS_COMPILED|
      ANYGM_CONFIG_GAMEPAD_CONNECTED|
      ANYGM_CONFIG_FAST_ALPHA_CULL|ANYGM_CONFIG_START_ROOM|
      ANYGM_CONFIG_PRESENT_LOGICAL_RASTER|ANYGM_CONFIG_ADJUST_CRT_TV|ANYGM_CONFIG_CLEAR_LOCAL_DATA|
      ANYGM_CONFIG_CONTENT_OVERRIDES|ANYGM_CONFIG_CONTENT_SHADER_READBACK|
      ANYGM_CONFIG_CONTENT_SHADER_DEVICE_EXPECTED|ANYGM_CONFIG_HYBRID_GPU_PRESENTATION;
  if(g_libretro.loaded || g_libretro.prepared)
    delta.fields&=~(ANYGM_CONFIG_REPORT_ALL_SHADERS_COMPILED|
                    ANYGM_CONFIG_CONTENT_SHADER_READBACK|
                    ANYGM_CONFIG_CONTENT_SHADER_DEVICE_EXPECTED|
                    ANYGM_CONFIG_HYBRID_GPU_PRESENTATION);
  anygm_set_config(g_libretro.engine,&delta);
}

void libretro_options_finalize_graphics(bool device_available){
  if(!g_libretro.engine || !g_libretro.prepared || g_libretro.loaded) return;
  AnygmConfig *config=&g_libretro.config;
  bool content_device=device_available && g_libretro.content_glsl_selected &&
                      g_libretro.content_glsl_candidate;
  config->content_shader_device_expected=content_device?1u:0u;
  config->hybrid_gpu_presentation=
    device_available && g_libretro.hybrid_gpu_selected?1u:0u;
  /* A requested GLSL device that was refused has the documented report-available software
   * fallback. A separately selected strict Off remains strict when only Hybrid GPU was refused. */
  if(g_libretro.content_glsl_selected && g_libretro.content_glsl_candidate && !device_available){
    config->content_shader_readback=ANYGM_SHADER_READBACK_NEVER;
    config->report_all_shaders_compiled=1u;
  }
  AnygmConfigDelta delta;
  memset(&delta,0,sizeof delta);
  delta.struct_size=sizeof delta;
  delta.values=*config;
  delta.fields=ANYGM_CONFIG_REPORT_ALL_SHADERS_COMPILED|
               ANYGM_CONFIG_CONTENT_SHADER_READBACK|
               ANYGM_CONFIG_CONTENT_SHADER_DEVICE_EXPECTED|
               ANYGM_CONFIG_HYBRID_GPU_PRESENTATION;
  anygm_set_config(g_libretro.engine,&delta);
}
