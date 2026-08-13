/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_tracker.c — portable ProTracker MOD and FastTracker II XM playback. See gml_tracker.h.
 *
 * Everything here is integer arithmetic on purpose: the render has to be byte-identical across
 * platforms, so frequencies come from a baked 2^(i/768) table rather than libm, and mixing steps
 * in 32.32 fixed point. Note-to-period uses the continuous formula the table makes exact instead
 * of ProTracker's hand-rounded period tables; the two differ by at most one period unit, which is
 * stated here rather than hidden. Zxx pattern callbacks do not exist in either format — that
 * effect is an Impulse Tracker MIDI macro — so the jbfmod layer above reports none.
 */
#include "gml_tracker.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum {
  TR_MAX_CHANNELS = 32,
  TR_MAX_INSTRUMENTS = 128,
  TR_MAX_SAMPLES = 16,
  TR_MAX_PATTERNS = 256,
  TR_RENDER_CAP_SECONDS = 1200,
};

/* 2^(i/768) in 16.16, i = 0..767. Baked with 40-digit decimal arithmetic so every build of every
 * platform holds the same constants; one octave is 768 steps, one semitone 64, one XM finetune 0.5. */
static const uint32_t pow2_768[768] = {
  65536u,65595u,65654u,65714u,65773u,65832u,65892u,65951u,
  66011u,66071u,66130u,66190u,66250u,66309u,66369u,66429u,
  66489u,66549u,66609u,66670u,66730u,66790u,66850u,66911u,
  66971u,67032u,67092u,67153u,67213u,67274u,67335u,67395u,
  67456u,67517u,67578u,67639u,67700u,67761u,67823u,67884u,
  67945u,68007u,68068u,68129u,68191u,68252u,68314u,68376u,
  68438u,68499u,68561u,68623u,68685u,68747u,68809u,68871u,
  68933u,68996u,69058u,69120u,69183u,69245u,69308u,69370u,
  69433u,69496u,69558u,69621u,69684u,69747u,69810u,69873u,
  69936u,69999u,70062u,70126u,70189u,70252u,70316u,70379u,
  70443u,70507u,70570u,70634u,70698u,70762u,70825u,70889u,
  70953u,71017u,71082u,71146u,71210u,71274u,71339u,71403u,
  71468u,71532u,71597u,71661u,71726u,71791u,71856u,71920u,
  71985u,72050u,72115u,72181u,72246u,72311u,72376u,72442u,
  72507u,72573u,72638u,72704u,72769u,72835u,72901u,72967u,
  73032u,73098u,73164u,73230u,73297u,73363u,73429u,73495u,
  73562u,73628u,73695u,73761u,73828u,73894u,73961u,74028u,
  74095u,74162u,74229u,74296u,74363u,74430u,74497u,74564u,
  74632u,74699u,74766u,74834u,74902u,74969u,75037u,75105u,
  75172u,75240u,75308u,75376u,75444u,75512u,75581u,75649u,
  75717u,75786u,75854u,75922u,75991u,76060u,76128u,76197u,
  76266u,76335u,76404u,76473u,76542u,76611u,76680u,76749u,
  76819u,76888u,76957u,77027u,77096u,77166u,77236u,77305u,
  77375u,77445u,77515u,77585u,77655u,77725u,77795u,77866u,
  77936u,78006u,78077u,78147u,78218u,78288u,78359u,78430u,
  78501u,78572u,78642u,78713u,78785u,78856u,78927u,78998u,
  79069u,79141u,79212u,79284u,79355u,79427u,79499u,79571u,
  79642u,79714u,79786u,79858u,79930u,80003u,80075u,80147u,
  80220u,80292u,80365u,80437u,80510u,80582u,80655u,80728u,
  80801u,80874u,80947u,81020u,81093u,81166u,81240u,81313u,
  81386u,81460u,81533u,81607u,81681u,81754u,81828u,81902u,
  81976u,82050u,82124u,82198u,82273u,82347u,82421u,82496u,
  82570u,82645u,82719u,82794u,82869u,82944u,83019u,83093u,
  83169u,83244u,83319u,83394u,83469u,83545u,83620u,83696u,
  83771u,83847u,83923u,83998u,84074u,84150u,84226u,84302u,
  84378u,84454u,84531u,84607u,84683u,84760u,84836u,84913u,
  84990u,85066u,85143u,85220u,85297u,85374u,85451u,85528u,
  85606u,85683u,85760u,85838u,85915u,85993u,86070u,86148u,
  86226u,86304u,86382u,86460u,86538u,86616u,86694u,86772u,
  86851u,86929u,87008u,87086u,87165u,87244u,87322u,87401u,
  87480u,87559u,87638u,87717u,87796u,87876u,87955u,88034u,
  88114u,88194u,88273u,88353u,88433u,88513u,88592u,88672u,
  88752u,88833u,88913u,88993u,89073u,89154u,89234u,89315u,
  89396u,89476u,89557u,89638u,89719u,89800u,89881u,89962u,
  90043u,90125u,90206u,90288u,90369u,90451u,90532u,90614u,
  90696u,90778u,90860u,90942u,91024u,91106u,91188u,91271u,
  91353u,91436u,91518u,91601u,91684u,91766u,91849u,91932u,
  92015u,92098u,92181u,92265u,92348u,92431u,92515u,92598u,
  92682u,92766u,92849u,92933u,93017u,93101u,93185u,93269u,
  93354u,93438u,93522u,93607u,93691u,93776u,93860u,93945u,
  94030u,94115u,94200u,94285u,94370u,94455u,94541u,94626u,
  94711u,94797u,94882u,94968u,95054u,95140u,95226u,95312u,
  95398u,95484u,95570u,95656u,95743u,95829u,95916u,96002u,
  96089u,96176u,96263u,96350u,96436u,96524u,96611u,96698u,
  96785u,96873u,96960u,97048u,97135u,97223u,97311u,97399u,
  97487u,97575u,97663u,97751u,97839u,97928u,98016u,98104u,
  98193u,98282u,98370u,98459u,98548u,98637u,98726u,98815u,
  98905u,98994u,99083u,99173u,99262u,99352u,99442u,99531u,
  99621u,99711u,99801u,99891u,99982u,100072u,100162u,100253u,
  100343u,100434u,100524u,100615u,100706u,100797u,100888u,100979u,
  101070u,101162u,101253u,101344u,101436u,101527u,101619u,101711u,
  101803u,101895u,101987u,102079u,102171u,102263u,102356u,102448u,
  102540u,102633u,102726u,102818u,102911u,103004u,103097u,103190u,
  103283u,103377u,103470u,103564u,103657u,103751u,103844u,103938u,
  104032u,104126u,104220u,104314u,104408u,104502u,104597u,104691u,
  104786u,104880u,104975u,105070u,105165u,105260u,105355u,105450u,
  105545u,105640u,105736u,105831u,105927u,106022u,106118u,106214u,
  106310u,106406u,106502u,106598u,106694u,106791u,106887u,106984u,
  107080u,107177u,107274u,107371u,107468u,107565u,107662u,107759u,
  107856u,107954u,108051u,108149u,108246u,108344u,108442u,108540u,
  108638u,108736u,108834u,108932u,109031u,109129u,109228u,109326u,
  109425u,109524u,109623u,109722u,109821u,109920u,110019u,110119u,
  110218u,110317u,110417u,110517u,110617u,110716u,110816u,110917u,
  111017u,111117u,111217u,111318u,111418u,111519u,111619u,111720u,
  111821u,111922u,112023u,112124u,112226u,112327u,112428u,112530u,
  112631u,112733u,112835u,112937u,113039u,113141u,113243u,113345u,
  113448u,113550u,113653u,113755u,113858u,113961u,114064u,114167u,
  114270u,114373u,114476u,114580u,114683u,114787u,114890u,114994u,
  115098u,115202u,115306u,115410u,115514u,115618u,115723u,115827u,
  115932u,116036u,116141u,116246u,116351u,116456u,116561u,116667u,
  116772u,116877u,116983u,117088u,117194u,117300u,117406u,117512u,
  117618u,117724u,117831u,117937u,118043u,118150u,118257u,118363u,
  118470u,118577u,118684u,118792u,118899u,119006u,119114u,119221u,
  119329u,119437u,119544u,119652u,119760u,119869u,119977u,120085u,
  120194u,120302u,120411u,120519u,120628u,120737u,120846u,120955u,
  121065u,121174u,121283u,121393u,121502u,121612u,121722u,121832u,
  121942u,122052u,122162u,122272u,122383u,122493u,122604u,122715u,
  122825u,122936u,123047u,123158u,123270u,123381u,123492u,123604u,
  123715u,123827u,123939u,124051u,124163u,124275u,124387u,124500u,
  124612u,124725u,124837u,124950u,125063u,125176u,125289u,125402u,
  125515u,125628u,125742u,125855u,125969u,126083u,126197u,126310u,
  126425u,126539u,126653u,126767u,126882u,126996u,127111u,127226u,
  127341u,127456u,127571u,127686u,127801u,127917u,128032u,128148u,
  128263u,128379u,128495u,128611u,128727u,128844u,128960u,129076u,
  129193u,129310u,129426u,129543u,129660u,129777u,129894u,130012u,
  130129u,130247u,130364u,130482u,130600u,130718u,130836u,130954u,
};


typedef struct {
  int16_t *pcm;               /* mono S16 */
  uint32_t length, loop_start, loop_length;
  int loop_type;              /* 0 none, 1 forward, 2 ping-pong */
  int volume;                 /* 0..64 */
  int finetune;               /* XM: -128..127 (halves of a 768th step); MOD: -8..7 eighths of a semitone */
  int relative_note;          /* XM only */
  int panning;                /* 0..255 */
} TrSample;

typedef struct {
  char name[23];
  int num_samples;
  TrSample sample[TR_MAX_SAMPLES];
  uint8_t sample_map[96];
  /* XM envelopes: points are (tick, value 0..64). */
  uint16_t vol_env[24]; int vol_points, vol_sustain, vol_loop_start, vol_loop_end, vol_flags;
  uint16_t pan_env[24]; int pan_points, pan_sustain, pan_loop_start, pan_loop_end, pan_flags;
  int fadeout;
  int vib_type, vib_sweep, vib_depth, vib_rate;
} TrInstrument;

typedef struct { uint8_t note, instrument, volcol, effect, param; } TrCell;
typedef struct { int rows; TrCell *cells; } TrPattern;

struct GmlTrackerModule {
  char name[24];
  int is_xm, linear_freq;
  int channels, num_orders, restart_pos, num_patterns, num_instruments, total_samples;
  int default_speed, default_bpm;
  uint8_t orders[256];
  TrPattern pattern[TR_MAX_PATTERNS];
  TrInstrument instrument[TR_MAX_INSTRUMENTS];
  GmlTrackerRowMark *timeline; uint32_t timeline_count, timeline_cap;
  GmlTrackerNoteEvent *events; uint32_t event_count, event_cap;
};

/* ---------------------------------------------------------------- fixed-point helpers */

/* 2^(x/768) in 16.16 for a possibly negative x. */
static uint64_t pow2_fp(int32_t x){
  int32_t oct = (x >= 0) ? x / 768 : -((-x + 767) / 768);
  int32_t idx = x - oct * 768;              /* 0..767 */
  uint64_t v = pow2_768[idx];
  if(oct >= 0) return v << oct;
  return v >> (-oct);
}

/* XM linear-mode frequency (Hz, 16.16) from a period; period 7680 - note*64 - finetune/2. */
static uint64_t xm_linear_freq(int period){
  return (8363ull * pow2_fp(4608 - period));           /* 16.16 */
}

/* Amiga-style frequency for MOD periods: PAL 3546895 / period. 16.16. */
static uint64_t mod_freq(int period){
  if(period < 1) period = 1;
  return (3546895ull << 16) / (uint32_t)period;
}

/* XM amiga-mode frequency: 8363 * 1712 / period. 16.16. */
static uint64_t xm_amiga_freq(int period){
  if(period < 1) period = 1;
  return (14317456ull << 16) / (uint32_t)period;
}

/* XM amiga-mode period from linear semitone offset (in 768ths). */
static int xm_amiga_period(int32_t x768){
  return (int)((1712ull << 32) / pow2_fp(x768) >> 16);
}

/* ---------------------------------------------------------------- parsing: MOD */

static uint16_t be16(const uint8_t *p){ return (uint16_t)((p[0] << 8) | p[1]); }
static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p){ return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24)); }

static int mod_channels_from_tag(const uint8_t *tag){
  if(!memcmp(tag, "M.K.", 4) || !memcmp(tag, "M!K!", 4) || !memcmp(tag, "FLT4", 4)) return 4;
  if(tag[1] == 'C' && tag[2] == 'H' && tag[3] == 'N' && tag[0] >= '2' && tag[0] <= '9')
    return tag[0] - '0';
  if(tag[2] == 'C' && tag[3] == 'H' && tag[0] >= '1' && tag[0] <= '3' && tag[1] >= '0' && tag[1] <= '9')
    return (tag[0]-'0')*10 + (tag[1]-'0');
  return 0;
}

/* Nearest MOD note for a raw pattern period; ProTracker files store periods, the engine keys on
 * notes. 856 is note 1; three octaves down to 113, extended periods above and below accepted. */
static int mod_note_from_period(int period){
  if(period <= 0) return 0;
  int best = 1, best_diff = 1 << 30;
  for(int note = 1; note <= 96; note++){
    int p = (int)((856ull << 32) / pow2_fp((note - 1) * 64) >> 16);
    int diff = p > period ? p - period : period - p;
    if(diff < best_diff){ best_diff = diff; best = note; }
  }
  return best;
}

static GmlTrackerModule *load_mod(const uint8_t *d, size_t n, char *err, size_t errcap){
  if(n < 1084) return NULL;
  int channels = mod_channels_from_tag(d + 1080);
  if(!channels) return NULL;
  if(channels > TR_MAX_CHANNELS){ snprintf(err, errcap, "MOD with %d channels", channels); return NULL; }
  GmlTrackerModule *m = (GmlTrackerModule*)calloc(1, sizeof *m);
  if(!m) return NULL;
  memcpy(m->name, d, 20); m->name[20] = 0;
  m->channels = channels;
  m->is_xm = 0; m->linear_freq = 0;
  m->num_instruments = 31; m->total_samples = 0;
  m->default_speed = 6; m->default_bpm = 125;
  size_t off = 20;
  uint32_t sample_bytes[32] = {0};
  for(int i = 1; i <= 31; i++){
    TrInstrument *ins = &m->instrument[i - 1];
    memcpy(ins->name, d + off, 22); ins->name[22] = 0;
    TrSample *s = &ins->sample[0];
    sample_bytes[i] = (uint32_t)be16(d + off + 22) * 2u;
    int ft = d[off + 24] & 0x0F; if(ft > 7) ft -= 16;
    s->finetune = ft;
    s->volume = d[off + 25] > 64 ? 64 : d[off + 25];
    s->loop_start = (uint32_t)be16(d + off + 26) * 2u;
    s->loop_length = (uint32_t)be16(d + off + 28) * 2u;
    s->loop_type = s->loop_length > 2 ? 1 : 0;
    s->panning = 128;
    ins->num_samples = sample_bytes[i] ? 1 : 0;
    if(ins->num_samples) m->total_samples++;
    off += 30;
  }
  m->num_orders = d[950]; if(m->num_orders > 128) m->num_orders = 128;
  m->restart_pos = d[951] < m->num_orders ? d[951] : 0;
  memcpy(m->orders, d + 952, 128);
  int max_pat = 0;
  for(int i = 0; i < 128; i++) if(m->orders[i] > max_pat) max_pat = m->orders[i];
  m->num_patterns = max_pat + 1;
  off = 1084;
  size_t pat_size = (size_t)64 * channels * 4;
  for(int p = 0; p < m->num_patterns; p++){
    if(off + pat_size > n){ gml_tracker_free(m); return NULL; }
    TrPattern *pat = &m->pattern[p];
    pat->rows = 64;
    pat->cells = (TrCell*)calloc((size_t)64 * channels, sizeof(TrCell));
    if(!pat->cells){ gml_tracker_free(m); return NULL; }
    for(int r = 0; r < 64; r++)
      for(int c = 0; c < channels; c++){
        const uint8_t *q = d + off + ((size_t)r * channels + c) * 4;
        TrCell *cell = &pat->cells[r * channels + c];
        int period = ((q[0] & 0x0F) << 8) | q[1];
        cell->note = period ? (uint8_t)mod_note_from_period(period) : 0;
        cell->instrument = (uint8_t)((q[0] & 0xF0) | (q[2] >> 4));
        cell->effect = q[2] & 0x0F;
        cell->param = q[3];
      }
    off += pat_size;
  }
  for(int i = 1; i <= 31; i++){
    TrSample *s = &m->instrument[i - 1].sample[0];
    uint32_t bytes = sample_bytes[i];
    if(!bytes) continue;
    if(off + bytes > n) bytes = (uint32_t)(n - off);
    s->length = bytes;
    s->pcm = (int16_t*)malloc(sizeof(int16_t) * (bytes ? bytes : 1));
    if(!s->pcm){ gml_tracker_free(m); return NULL; }
    for(uint32_t k = 0; k < bytes; k++) s->pcm[k] = (int16_t)((int8_t)d[off + k] << 8);
    if(s->loop_start >= s->length){ s->loop_type = 0; s->loop_start = 0; }
    if(s->loop_type && s->loop_start + s->loop_length > s->length)
      s->loop_length = s->length - s->loop_start;
    off += bytes;
  }
  return m;
}

/* ---------------------------------------------------------------- parsing: XM */

static GmlTrackerModule *load_xm(const uint8_t *d, size_t n, char *err, size_t errcap){
  if(n < 60 + 276 || memcmp(d, "Extended Module: ", 17)) return NULL;
  GmlTrackerModule *m = (GmlTrackerModule*)calloc(1, sizeof *m);
  if(!m) return NULL;
  memcpy(m->name, d + 17, 20); m->name[20] = 0;
  m->is_xm = 1;
  uint32_t hsize = le32(d + 60);
  const uint8_t *h = d + 64;
  m->num_orders = le16(h + 0);
  m->restart_pos = le16(h + 2);
  m->channels = le16(h + 4);
  m->num_patterns = le16(h + 6);
  m->num_instruments = le16(h + 8);
  m->linear_freq = le16(h + 10) & 1;
  m->default_speed = le16(h + 12); if(!m->default_speed) m->default_speed = 6;
  m->default_bpm = le16(h + 14); if(m->default_bpm < 32) m->default_bpm = 125;
  if(m->channels < 1 || m->channels > TR_MAX_CHANNELS || m->num_patterns > TR_MAX_PATTERNS ||
     m->num_instruments > TR_MAX_INSTRUMENTS || m->num_orders > 256){
    snprintf(err, errcap, "XM header out of range"); free(m); return NULL;
  }
  memcpy(m->orders, h + 16, m->num_orders > 256 ? 256 : m->num_orders);
  if(m->restart_pos >= m->num_orders) m->restart_pos = 0;
  size_t off = 60 + hsize;
  for(int p = 0; p < m->num_patterns; p++){
    if(off + 9 > n){ gml_tracker_free(m); return NULL; }
    uint32_t phead = le32(d + off);
    int rows = le16(d + off + 5);
    uint32_t packed = le16(d + off + 7);
    if(rows < 1 || rows > 256){ gml_tracker_free(m); return NULL; }
    TrPattern *pat = &m->pattern[p];
    pat->rows = rows;
    pat->cells = (TrCell*)calloc((size_t)rows * m->channels, sizeof(TrCell));
    if(!pat->cells){ gml_tracker_free(m); return NULL; }
    const uint8_t *q = d + off + phead, *qe = q + packed;
    if((size_t)(qe - d) > n){ gml_tracker_free(m); return NULL; }
    for(int r = 0; r < rows && q < qe; r++)
      for(int c = 0; c < m->channels && q < qe; c++){
        TrCell *cell = &pat->cells[r * m->channels + c];
        uint8_t head = *q++;
        if(head & 0x80){
          if(head & 1) cell->note = *q++;
          if(head & 2) cell->instrument = *q++;
          if(head & 4) cell->volcol = *q++;
          if(head & 8) cell->effect = *q++;
          if(head & 16) cell->param = *q++;
        } else {
          cell->note = head;
          cell->instrument = *q++;
          cell->volcol = *q++;
          cell->effect = *q++;
          cell->param = *q++;
        }
      }
    off += phead + packed;
  }
  for(int i = 0; i < m->num_instruments; i++){
    if(off + 4 > n){ gml_tracker_free(m); return NULL; }
    TrInstrument *ins = &m->instrument[i];
    uint32_t isize = le32(d + off);
    if(off + isize > n || isize < 29){ gml_tracker_free(m); return NULL; }
    memcpy(ins->name, d + off + 4, 22); ins->name[22] = 0;
    int nsamples = le16(d + off + 27);
    ins->num_samples = nsamples > TR_MAX_SAMPLES ? TR_MAX_SAMPLES : nsamples;
    size_t sh_off = off + isize;
    if(nsamples > 0 && isize >= 243){
      const uint8_t *x = d + off + 33;
      memcpy(ins->sample_map, x, 96);
      for(int k = 0; k < 24; k++) ins->vol_env[k] = le16(x + 96 + k * 2);
      for(int k = 0; k < 24; k++) ins->pan_env[k] = le16(x + 144 + k * 2);
      ins->vol_points = x[192] > 12 ? 12 : x[192];
      ins->pan_points = x[193] > 12 ? 12 : x[193];
      ins->vol_sustain = x[194]; ins->vol_loop_start = x[195]; ins->vol_loop_end = x[196];
      ins->pan_sustain = x[197]; ins->pan_loop_start = x[198]; ins->pan_loop_end = x[199];
      ins->vol_flags = x[200]; ins->pan_flags = x[201];
      ins->vib_type = x[202]; ins->vib_sweep = x[203];
      ins->vib_depth = x[204]; ins->vib_rate = x[205];
      ins->fadeout = le16(x + 206);
    }
    m->total_samples += ins->num_samples;
    /* sample headers, then all sample data */
    size_t data_off = sh_off + (size_t)nsamples * 40;
    for(int s = 0; s < nsamples; s++){
      if(sh_off + 40 > n){ gml_tracker_free(m); return NULL; }
      const uint8_t *y = d + sh_off;
      uint32_t bytes = le32(y);
      uint32_t loop_start = le32(y + 4), loop_len = le32(y + 8);
      int volume = y[12] > 64 ? 64 : y[12];
      int finetune = (int8_t)y[13];
      int type = y[14];
      int panning = y[15];
      int relnote = (int8_t)y[16];
      int is16 = (type & 0x10) != 0;
      if(s < TR_MAX_SAMPLES){
        TrSample *sm = &ins->sample[s];
        sm->volume = volume; sm->finetune = finetune; sm->relative_note = relnote;
        sm->panning = panning;
        sm->loop_type = type & 3;
        uint32_t frames = is16 ? bytes / 2 : bytes;
        sm->length = frames;
        sm->loop_start = is16 ? loop_start / 2 : loop_start;
        sm->loop_length = is16 ? loop_len / 2 : loop_len;
        if(sm->loop_length == 0) sm->loop_type = 0;
        if(sm->loop_start >= frames){ sm->loop_type = 0; sm->loop_start = 0; }
        if(sm->loop_type && sm->loop_start + sm->loop_length > frames)
          sm->loop_length = frames - sm->loop_start;
        if(data_off + bytes > n){ gml_tracker_free(m); return NULL; }
        sm->pcm = (int16_t*)malloc(sizeof(int16_t) * (frames ? frames : 1));
        if(!sm->pcm){ gml_tracker_free(m); return NULL; }
        /* XM sample data is stored as deltas. */
        if(is16){
          int16_t acc = 0;
          for(uint32_t k = 0; k < frames; k++){
            acc = (int16_t)(acc + (int16_t)le16(d + data_off + k * 2));
            sm->pcm[k] = acc;
          }
        } else {
          int8_t acc = 0;
          for(uint32_t k = 0; k < frames; k++){
            acc = (int8_t)(acc + (int8_t)d[data_off + k]);
            sm->pcm[k] = (int16_t)(acc << 8);
          }
        }
      }
      data_off += bytes;
      sh_off += 40;
    }
    off = data_off;
  }
  return m;
}

/* ---------------------------------------------------------------- shared load/free */

GmlTrackerModule *gml_tracker_load(const uint8_t *data, size_t size, char *err, size_t errcap){
  char scratch[1] = {0};
  if(!err || !errcap){ err = scratch; errcap = 1; }
  err[0] = 0;
  if(!data || size < 64){ snprintf(err, errcap, "not a module"); return NULL; }
  GmlTrackerModule *m = load_xm(data, size, err, errcap);
  if(m) return m;
  if(err[0]) return NULL;
  m = load_mod(data, size, err, errcap);
  if(m) return m;
  if(!err[0]) snprintf(err, errcap, "neither an XM nor a MOD image");
  return NULL;
}

void gml_tracker_free(GmlTrackerModule *m){
  if(!m) return;
  for(int p = 0; p < TR_MAX_PATTERNS; p++) free(m->pattern[p].cells);
  for(int i = 0; i < TR_MAX_INSTRUMENTS; i++)
    for(int s = 0; s < TR_MAX_SAMPLES; s++) free(m->instrument[i].sample[s].pcm);
  free(m->timeline);
  free(m->events);
  free(m);
}

const char *gml_tracker_name(const GmlTrackerModule *m){ return m ? m->name : ""; }
const char *gml_tracker_type(const GmlTrackerModule *m){ return m && m->is_xm ? "XM" : "MOD"; }
int gml_tracker_num_channels(const GmlTrackerModule *m){ return m ? m->channels : 0; }
int gml_tracker_num_orders(const GmlTrackerModule *m){ return m ? m->num_orders : 0; }
int gml_tracker_num_patterns(const GmlTrackerModule *m){ return m ? m->num_patterns : 0; }
int gml_tracker_num_instruments(const GmlTrackerModule *m){ return m ? m->num_instruments : 0; }
int gml_tracker_num_samples(const GmlTrackerModule *m){ return m ? m->total_samples : 0; }
const GmlTrackerRowMark *gml_tracker_timeline(const GmlTrackerModule *m, uint32_t *count){
  if(count) *count = m ? m->timeline_count : 0;
  return m ? m->timeline : NULL;
}
const GmlTrackerNoteEvent *gml_tracker_events(const GmlTrackerModule *m, uint32_t *count){
  if(count) *count = m ? m->event_count : 0;
  return m ? m->events : NULL;
}
int gml_tracker_pattern_rows(const GmlTrackerModule *m, int pattern){
  if(!m || pattern < 0 || pattern >= m->num_patterns) return 0;
  return m->pattern[pattern].rows;
}

/* ---------------------------------------------------------------- sequencer */

typedef struct {
  int instrument, note;              /* last trigger */
  const TrSample *sample;
  int64_t pos;                       /* 32.32 sample position */
  int dir;                           /* ping-pong direction */
  int active, keyoff;
  int period, target_period, porta_speed, glissando;
  int volume;                        /* 0..64 */
  int panning;                       /* 0..255 */
  int fade;                          /* 65536 down to 0 after keyoff */
  int vol_env_tick, pan_env_tick;
  int vib_pos, vib_speed, vib_depth, vib_wave;
  int trem_pos, trem_speed, trem_depth, trem_wave;
  int autovib_tick;
  int vib_period_offset, arp_offset, trem_vol_offset;
  int loop_row, loop_count;          /* E6x */
  int retrig_tick, retrig_param;
  int tremor_tick;
  int mem_porta_up, mem_porta_down, mem_fine_up, mem_fine_down, mem_xfine_up, mem_xfine_down;
  int mem_volslide, mem_fine_vup, mem_fine_vdown, mem_gvol_slide, mem_pan_slide, mem_offset;
  int mem_vibrato, mem_tremolo, mem_tremor, mem_retrig;
  int volcol_vib_speed;
} TrChannel;

typedef struct {
  const GmlTrackerModule *m;
  GmlTrackerModule *recording;       /* same module, mutable, for timeline/event appends */
  uint32_t current_frame;
  int rate, pan_separation;
  int order, row, tick, speed, bpm;
  int pattern_delay;
  int jump_order, jump_row, do_jump;
  int global_volume;                 /* 0..64 */
  int ended;
  TrChannel ch[TR_MAX_CHANNELS];
  /* accumulate exact tick lengths: frames_per_tick = rate*5 / (bpm*2) with carried remainder */
  uint32_t tick_rem;
} TrPlayer;

static const int8_t tr_sine[64] = {
  0,12,25,37,49,60,71,81,90,98,106,112,118,122,125,127,
  127,127,125,122,118,112,106,98,90,81,71,60,49,37,25,12,
  0,-12,-25,-37,-49,-60,-71,-81,-90,-98,-106,-112,-118,-122,-125,-127,
  -127,-127,-125,-122,-118,-112,-106,-98,-90,-81,-71,-60,-49,-37,-25,-12,
};

static int wave_value(int wave, int pos){
  pos &= 63;
  switch(wave & 3){
    case 0: return tr_sine[pos];
    case 1: return 127 - (pos * 255) / 63;          /* ramp down */
    case 2: return pos < 32 ? 127 : -127;           /* square */
    default: return tr_sine[pos];
  }
}

static int channel_note_period(const TrPlayer *pl, const TrChannel *c, int note){
  const TrSample *s = c->sample;
  if(!s) return 0;
  if(pl->m->is_xm){
    int real = note + s->relative_note;
    if(real < 1) real = 1;
    if(real > 118) real = 118;
    if(pl->m->linear_freq)
      return 7680 - real * 64 - s->finetune / 2;
    return xm_amiga_period((real - 1) * 64 + s->finetune / 2);
  }
  return (int)((856ull << 32) / pow2_fp((note - 1) * 64 + s->finetune * 8) >> 16);
}

static uint64_t channel_frequency(const TrPlayer *pl, int period){
  if(pl->m->is_xm)
    return pl->m->linear_freq ? xm_linear_freq(period) : xm_amiga_freq(period);
  return mod_freq(period);
}

static void envelope_value(const uint16_t *env, int points, int tick, int *out){
  if(points <= 0) return;
  if(tick >= env[(points - 1) * 2]){ *out = env[(points - 1) * 2 + 1]; return; }
  for(int i = 0; i < points - 1; i++){
    int t0 = env[i * 2], v0 = env[i * 2 + 1];
    int t1 = env[(i + 1) * 2], v1 = env[(i + 1) * 2 + 1];
    if(tick >= t0 && tick < t1){
      if(t1 == t0){ *out = v0; return; }
      *out = v0 + (v1 - v0) * (tick - t0) / (t1 - t0);
      return;
    }
  }
  *out = env[(points - 1) * 2 + 1];
}

static void trigger_note(TrPlayer *pl, TrChannel *c, int note, int keep_position){
  const GmlTrackerModule *m = pl->m;
  const TrInstrument *ins = NULL;
  if(c->instrument >= 1 && c->instrument <= m->num_instruments)
    ins = &m->instrument[c->instrument - 1];
  if(!ins || !ins->num_samples){ c->active = 0; return; }
  int si = 0;
  if(m->is_xm){
    int idx = note - 1; if(idx < 0) idx = 0; if(idx > 95) idx = 95;
    si = ins->sample_map[idx];
    if(si >= ins->num_samples) si = 0;
  }
  c->sample = &ins->sample[si];
  if(!c->sample->pcm || !c->sample->length){ c->active = 0; return; }
  c->note = note;
  c->period = channel_note_period(pl, c, note);
  if(!keep_position){ c->pos = 0; c->dir = 1; }
  c->active = 1; c->keyoff = 0;
  if(pl->recording && c->instrument){
    GmlTrackerModule *rm = pl->recording;
    if(rm->event_count == rm->event_cap){
      uint32_t cap = rm->event_cap ? rm->event_cap * 2 : 1024;
      GmlTrackerNoteEvent *grown =
        (GmlTrackerNoteEvent*)realloc(rm->events, cap * sizeof *grown);
      if(grown){ rm->events = grown; rm->event_cap = cap; }
    }
    if(rm->event_count < rm->event_cap){
      rm->events[rm->event_count].frame = pl->current_frame;
      rm->events[rm->event_count].instrument = (uint16_t)c->instrument;
      rm->event_count++;
    }
  }
  c->fade = 65536;
  c->vol_env_tick = 0; c->pan_env_tick = 0; c->autovib_tick = 0;
  if((c->vib_wave & 4) == 0) c->vib_pos = 0;
  if((c->trem_wave & 4) == 0) c->trem_pos = 0;
}

static void note_on(TrPlayer *pl, TrChannel *c, const TrCell *cell){
  const GmlTrackerModule *m = pl->m;
  int instrument = cell->instrument;
  int note = cell->note;
  int is_keyoff = m->is_xm && note == 97;
  int tone_porta = cell->effect == 3 || cell->effect == 5 ||
                   (m->is_xm && (cell->volcol & 0xF0) == 0xF0);
  if(instrument){
    c->instrument = instrument;
    /* An instrument by itself resets volume and envelopes at the current position. */
    if(c->instrument >= 1 && c->instrument <= m->num_instruments){
      const TrInstrument *ins = &m->instrument[c->instrument - 1];
      int si = 0;
      if(m->is_xm && c->note >= 1){
        int idx = c->note - 1; if(idx > 95) idx = 95;
        si = ins->sample_map[idx];
        if(si >= ins->num_samples) si = 0;
      }
      if(ins->num_samples){
        c->volume = ins->sample[si].volume;
        c->panning = m->is_xm ? ins->sample[si].panning : c->panning;
      }
      c->fade = 65536; c->vol_env_tick = 0; c->pan_env_tick = 0; c->keyoff = 0;
    }
  }
  if(is_keyoff){ c->keyoff = 1; return; }
  if(note >= 1 && note <= 96){
    if(tone_porta && c->active){
      c->target_period = channel_note_period(pl, c, note);
      c->note = note;
    } else {
      trigger_note(pl, c, note, 0);
    }
  }
}

static void volume_column_tick0(TrPlayer *pl, TrChannel *c, int v){
  (void)pl;
  if(v >= 0x10 && v <= 0x50){ c->volume = v - 0x10; if(c->volume > 64) c->volume = 64; }
  else if((v & 0xF0) == 0x80) { c->volume -= v & 15; if(c->volume < 0) c->volume = 0; }
  else if((v & 0xF0) == 0x90) { c->volume += v & 15; if(c->volume > 64) c->volume = 64; }
  else if((v & 0xF0) == 0xA0) { if(v & 15) c->volcol_vib_speed = v & 15; }
  else if((v & 0xF0) == 0xC0) { c->panning = (v & 15) * 17; }
  else if((v & 0xF0) == 0xF0) { if(v & 15) c->porta_speed = (v & 15) * 16; }
}

static void volume_column_tick(TrPlayer *pl, TrChannel *c, int v){
  (void)pl;
  if((v & 0xF0) == 0x60){ c->volume -= v & 15; if(c->volume < 0) c->volume = 0; }
  else if((v & 0xF0) == 0x70){ c->volume += v & 15; if(c->volume > 64) c->volume = 64; }
  else if((v & 0xF0) == 0xB0){
    if(v & 15) c->vib_depth = v & 15;
    c->vib_pos += c->volcol_vib_speed;
    c->vib_period_offset = (wave_value(c->vib_wave, c->vib_pos) * c->vib_depth) / 32;
  }
  else if((v & 0xF0) == 0xD0){ c->panning -= v & 15; if(c->panning < 0) c->panning = 0; }
  else if((v & 0xF0) == 0xE0){ c->panning += v & 15; if(c->panning > 255) c->panning = 255; }
  else if((v & 0xF0) == 0xF0){ /* tone porta handled with the effect path */
    int speed = c->porta_speed;
    if(c->target_period){
      if(c->period < c->target_period){ c->period += speed; if(c->period > c->target_period) c->period = c->target_period; }
      else if(c->period > c->target_period){ c->period -= speed; if(c->period < c->target_period) c->period = c->target_period; }
    }
  }
}

static void do_volslide(TrChannel *c, int param){
  int up = param >> 4, down = param & 15;
  if(up) c->volume += up; else c->volume -= down;
  if(c->volume < 0) c->volume = 0;
  if(c->volume > 64) c->volume = 64;
}

static void do_tone_porta(TrChannel *c){
  if(!c->target_period) return;
  if(c->period < c->target_period){
    c->period += c->porta_speed;
    if(c->period > c->target_period) c->period = c->target_period;
  } else if(c->period > c->target_period){
    c->period -= c->porta_speed;
    if(c->period < c->target_period) c->period = c->target_period;
  }
}

static void row_effects_tick0(TrPlayer *pl, TrChannel *c, const TrCell *cell){
  const GmlTrackerModule *m = pl->m;
  int e = cell->effect, p = cell->param;
  switch(e){
    case 0x0: c->arp_offset = 0; break;
    case 0x1: if(p) c->mem_porta_up = p; break;
    case 0x2: if(p) c->mem_porta_down = p; break;
    case 0x3: if(p) c->porta_speed = m->is_xm ? p * 4 : p; break;
    case 0x4:
      if(p >> 4) c->vib_speed = p >> 4;
      if(p & 15) c->vib_depth = p & 15;
      break;
    case 0x5: if(p) c->mem_volslide = p; break;
    case 0x6: if(p) c->mem_volslide = p; break;
    case 0x7:
      if(p >> 4) c->trem_speed = p >> 4;
      if(p & 15) c->trem_depth = p & 15;
      break;
    case 0x8: c->panning = p; break;
    case 0x9: {
      int off = p ? p : c->mem_offset; if(p) c->mem_offset = p;
      if(cell->note >= 1 && cell->note <= 96 && c->sample){
        uint32_t target = (uint32_t)off << 8;
        c->pos = target < c->sample->length ? ((int64_t)target << 32) : ((int64_t)(c->sample->length ? c->sample->length - 1 : 0) << 32);
      }
      break;
    }
    case 0xA: if(p) c->mem_volslide = p; break;
    case 0xB: pl->do_jump = 1; pl->jump_order = p; pl->jump_row = 0; break;
    case 0xC: c->volume = p > 64 ? 64 : p; break;
    case 0xD: pl->do_jump = 1; pl->jump_order = pl->order + 1;
              pl->jump_row = (p >> 4) * 10 + (p & 15); break;
    case 0xE:
      switch(p >> 4){
        case 0x1: { int v = p & 15 ? p & 15 : c->mem_fine_up; c->mem_fine_up = v;
                    c->period -= v * (m->is_xm ? 4 : 1); if(c->period < 1) c->period = 1; break; }
        case 0x2: { int v = p & 15 ? p & 15 : c->mem_fine_down; c->mem_fine_down = v;
                    c->period += v * (m->is_xm ? 4 : 1); break; }
        case 0x3: c->glissando = p & 15; break;
        case 0x4: c->vib_wave = p & 15; break;
        case 0x5:
          /* set finetune: applies to the playing sample's period from the fresh note */
          if(c->sample && cell->note >= 1 && cell->note <= 96){
            if(m->is_xm){
              int ft = ((p & 15) << 4) - 128;
              int real = cell->note + c->sample->relative_note;
              if(real < 1) real = 1;
              if(real > 118) real = 118;
              c->period = m->linear_freq ? 7680 - real * 64 - ft / 2
                                         : xm_amiga_period((real - 1) * 64 + ft / 2);
            } else {
              int ft = p & 15; if(ft > 7) ft -= 16;
              c->period = (int)((856ull << 32) / pow2_fp((cell->note - 1) * 64 + ft * 8) >> 16);
            }
          }
          break;
        case 0x6:
          if((p & 15) == 0) c->loop_row = pl->row;
          else {
            if(c->loop_count == 0) c->loop_count = p & 15;
            else c->loop_count--;
            if(c->loop_count){ pl->do_jump = 1; pl->jump_order = pl->order; pl->jump_row = c->loop_row; }
          }
          break;
        case 0x7: c->trem_wave = p & 15; break;
        case 0x9: c->retrig_tick = 0; c->retrig_param = p & 15; break;
        case 0xA: { int v = p & 15 ? p & 15 : c->mem_fine_vup; c->mem_fine_vup = v;
                    c->volume += v; if(c->volume > 64) c->volume = 64; break; }
        case 0xB: { int v = p & 15 ? p & 15 : c->mem_fine_vdown; c->mem_fine_vdown = v;
                    c->volume -= v; if(c->volume < 0) c->volume = 0; break; }
        case 0xC: /* handled on its tick */ break;
        case 0xD: /* note delay handled by caller */ break;
        case 0xE: pl->pattern_delay = p & 15; break;
        default: break;
      }
      break;
    case 0xF:
      if(p == 0) break;
      if(p < 0x20) pl->speed = p;
      else pl->bpm = p;
      break;
    case 0x10: pl->global_volume = p > 64 ? 64 : p; break;                    /* Gxx */
    case 0x11: if(p) c->mem_gvol_slide = p; break;                            /* Hxx */
    case 0x14: if(m->is_xm) c->keyoff = 1; break;                             /* Kxx (tick 0 for K00) */
    case 0x15: c->vol_env_tick = p; c->pan_env_tick = p; break;               /* Lxx */
    case 0x19: if(p) c->mem_pan_slide = p; break;                             /* Pxx */
    case 0x1B: if(p) c->mem_retrig = p; c->retrig_tick = 0; break;            /* Rxx */
    case 0x1D: if(p) c->mem_tremor = p; break;                                /* Txx */
    case 0x21:                                                                 /* Xxx */
      if((p >> 4) == 1){ int v = p & 15 ? p & 15 : c->mem_xfine_up; c->mem_xfine_up = v;
                         c->period -= v; if(c->period < 1) c->period = 1; }
      else if((p >> 4) == 2){ int v = p & 15 ? p & 15 : c->mem_xfine_down; c->mem_xfine_down = v;
                              c->period += v; }
      break;
    default: break;
  }
}

static void row_effects_tick(TrPlayer *pl, TrChannel *c, const TrCell *cell){
  const GmlTrackerModule *m = pl->m;
  int e = cell->effect, p = cell->param;
  switch(e){
    case 0x0:
      if(p){
        int step = pl->tick % 3;
        c->arp_offset = step == 0 ? 0 : step == 1 ? (p >> 4) : (p & 15);
      }
      break;
    case 0x1: c->period -= c->mem_porta_up * (m->is_xm ? 4 : 1); if(c->period < 1) c->period = 1; break;
    case 0x2: c->period += c->mem_porta_down * (m->is_xm ? 4 : 1); break;
    case 0x3: do_tone_porta(c); break;
    case 0x4:
      c->vib_pos += c->vib_speed;
      c->vib_period_offset = (wave_value(c->vib_wave, c->vib_pos) * c->vib_depth) / 32;
      break;
    case 0x5: do_tone_porta(c); do_volslide(c, c->mem_volslide); break;
    case 0x6:
      c->vib_pos += c->vib_speed;
      c->vib_period_offset = (wave_value(c->vib_wave, c->vib_pos) * c->vib_depth) / 32;
      do_volslide(c, c->mem_volslide);
      break;
    case 0x7:
      c->trem_pos += c->trem_speed;
      c->trem_vol_offset = (wave_value(c->trem_wave, c->trem_pos) * c->trem_depth) / 64;
      break;
    case 0xA: do_volslide(c, c->mem_volslide); break;
    case 0xE:
      switch(p >> 4){
        case 0x9: {
          int interval = c->retrig_param;
          if(interval && pl->tick % interval == 0 && c->sample){ c->pos = 0; c->dir = 1; }
          break;
        }
        case 0xC: if(pl->tick == (p & 15)) c->volume = 0; break;
        case 0xD: if(pl->tick == (p & 15)) note_on(pl, c, cell); break;
        default: break;
      }
      break;
    case 0x11: { int up = c->mem_gvol_slide >> 4, down = c->mem_gvol_slide & 15;   /* Hxx */
                 if(up) pl->global_volume += up; else pl->global_volume -= down;
                 if(pl->global_volume < 0) pl->global_volume = 0;
                 if(pl->global_volume > 64) pl->global_volume = 64;
                 break; }
    case 0x14: if(pl->tick == p) c->keyoff = 1; break;                              /* Kxx */
    case 0x19: { int left = c->mem_pan_slide >> 4, right = c->mem_pan_slide & 15;   /* Pxx */
                 c->panning += right - left;
                 if(c->panning < 0) c->panning = 0;
                 if(c->panning > 255) c->panning = 255;
                 break; }
    case 0x1B: {                                                                    /* Rxx */
      int interval = c->mem_retrig & 15, vol = c->mem_retrig >> 4;
      if(interval && pl->tick % interval == 0){
        if(c->sample){ c->pos = 0; c->dir = 1; }
        static const int8_t slide[16] = {0,-1,-2,-4,-8,-16,0,0,0,1,2,4,8,16,0,0};
        static const int8_t scale[16] = {1,1,1,1,1,1,2,1,1,1,1,1,1,1,3,2};
        static const int8_t divv[16]  = {1,1,1,1,1,1,3,2,1,1,1,1,1,1,2,1};
        c->volume = c->volume * scale[vol] / divv[vol] + slide[vol];
        if(c->volume < 0) c->volume = 0;
        if(c->volume > 64) c->volume = 64;
      }
      break;
    }
    case 0x1D: {                                                                    /* Txx */
      int on = (c->mem_tremor >> 4) + 1, off = (c->mem_tremor & 15) + 1;
      c->tremor_tick = (c->tremor_tick + 1) % (on + off);
      break;
    }
    default: break;
  }
}

static void advance_envelopes(TrPlayer *pl, TrChannel *c){
  const GmlTrackerModule *m = pl->m;
  if(!m->is_xm || c->instrument < 1 || c->instrument > m->num_instruments) return;
  const TrInstrument *ins = &m->instrument[c->instrument - 1];
  if(ins->vol_flags & 1){
    int sustain_tick = ins->vol_points ? ins->vol_env[ins->vol_sustain * 2] : 0;
    if(!(c->keyoff) && (ins->vol_flags & 2) && c->vol_env_tick >= sustain_tick){
      c->vol_env_tick = sustain_tick;
    } else {
      c->vol_env_tick++;
      if(ins->vol_flags & 4){
        int end_tick = ins->vol_env[ins->vol_loop_end * 2];
        if(c->vol_env_tick >= end_tick) c->vol_env_tick = ins->vol_env[ins->vol_loop_start * 2];
      }
    }
  }
  if(ins->pan_flags & 1){
    int sustain_tick = ins->pan_points ? ins->pan_env[ins->pan_sustain * 2] : 0;
    if(!(c->keyoff) && (ins->pan_flags & 2) && c->pan_env_tick >= sustain_tick){
      c->pan_env_tick = sustain_tick;
    } else {
      c->pan_env_tick++;
      if(ins->pan_flags & 4){
        int end_tick = ins->pan_env[ins->pan_loop_end * 2];
        if(c->pan_env_tick >= end_tick) c->pan_env_tick = ins->pan_env[ins->pan_loop_start * 2];
      }
    }
  }
  if(c->keyoff){
    c->fade -= ins->fadeout * 2;
    if(c->fade < 0) c->fade = 0;
  }
  /* auto-vibrato */
  if(ins->vib_depth){
    int depth = ins->vib_depth;
    if(ins->vib_sweep && c->autovib_tick < ins->vib_sweep)
      depth = depth * c->autovib_tick / ins->vib_sweep;
    c->autovib_tick++;
    int pos = (c->autovib_tick * ins->vib_rate) / 4;
    c->vib_period_offset += (wave_value(ins->vib_type, pos) * depth) / 128;
  }
}

/* Mix one tick's worth of frames for every channel. */
static void mix_tick(TrPlayer *pl, int16_t *out, uint32_t frames){
  const GmlTrackerModule *m = pl->m;
  for(int i = 0; i < m->channels; i++){
    TrChannel *c = &pl->ch[i];
    if(!c->active || !c->sample || !c->sample->pcm) continue;
    const TrSample *s = c->sample;
    int period = c->period + c->vib_period_offset;
    if(c->arp_offset && c->period){
      if(m->is_xm && m->linear_freq) period = c->period - c->arp_offset * 64;
      else {
        /* arpeggio in period space: divide by 2^(n/12) */
        period = (int)(((uint64_t)c->period << 16) / pow2_fp(c->arp_offset * 64));
      }
      period += c->vib_period_offset;
    }
    if(period < 1) period = 1;
    uint64_t freq = channel_frequency(pl, period);              /* 16.16 Hz */
    uint64_t step = (freq << 16) / (uint32_t)pl->rate;          /* 32.32 per frame */
    int vol = c->volume + c->trem_vol_offset;
    if(vol < 0) vol = 0;
    if(vol > 64) vol = 64;
    /* Txx tremor gates the volume */
    if(c->mem_tremor){
      int on = (c->mem_tremor >> 4) + 1;
      if(c->tremor_tick >= on) vol = 0;
    }
    int env_vol = 64, env_pan = 32;
    if(m->is_xm && c->instrument >= 1 && c->instrument <= m->num_instruments){
      const TrInstrument *ins = &m->instrument[c->instrument - 1];
      if(ins->vol_flags & 1) envelope_value(ins->vol_env, ins->vol_points, c->vol_env_tick, &env_vol);
      else if(c->keyoff) env_vol = 0;
      if(ins->pan_flags & 1) envelope_value(ins->pan_env, ins->pan_points, c->pan_env_tick, &env_pan);
    }
    int pan = c->panning;
    if(m->is_xm){
      int center_distance = pan >= 128 ? pan - 128 : 128 - pan;
      pan = pan + (env_pan - 32) * (128 - center_distance) / 32;
    }
    if(pan < 0) pan = 0;
    if(pan > 255) pan = 255;
    /* jbfmod's pan separation: 0 collapses to mono, 128 leaves authored panning alone. */
    pan = 128 + ((pan - 128) * pl->pan_separation) / 128;
    /* final gain in 0..(64*64*64*65536) → precompute a 16.16 multiplier */
    int64_t gain = (int64_t)vol * env_vol * pl->global_volume;  /* ≤ 64^3 = 262144 */
    gain = gain * c->fade / 65536;                              /* fadeout */
    /* left/right 8-bit factors from pan */
    int rgain = pan, lgain = 255 - pan;
    for(uint32_t f = 0; f < frames; f++){
      int64_t pos = c->pos >> 32;
      if(pos >= (int64_t)s->length){
        if(s->loop_type == 1 && s->loop_length){
          c->pos -= (int64_t)s->loop_length << 32;
          pos = c->pos >> 32;
          if(pos < 0){ c->active = 0; break; }
        } else if(s->loop_type == 2 && s->loop_length){
          c->dir = -c->dir;
          c->pos = ((int64_t)(s->length ? s->length - 1 : 0) << 32);
          pos = c->pos >> 32;
        } else { c->active = 0; break; }
      }
      if(pos < 0){ c->active = 0; break; }
      /* ping-pong lower bound */
      if(c->dir < 0 && pos <= (int64_t)s->loop_start){
        c->dir = 1;
        c->pos = (int64_t)s->loop_start << 32;
        pos = s->loop_start;
      }
      uint32_t p0 = (uint32_t)pos;
      uint32_t frac = (uint32_t)((c->pos >> 16) & 0xFFFF);
      int32_t a = s->pcm[p0];
      int32_t b = p0 + 1 < s->length ? s->pcm[p0 + 1]
                : (s->loop_type == 1 && s->loop_length ? s->pcm[s->loop_start] : a);
      int32_t smp = a + (int32_t)(((int64_t)(b - a) * frac) >> 16);
      /* scale: smp * gain / 262144 / 65536 * (l/255) — keep it in int64 */
      int64_t scaled = (int64_t)smp * gain / (262144);           /* ≈ full-scale S16 */
      int32_t l = (int32_t)(scaled * lgain / 255);
      int32_t r = (int32_t)(scaled * rgain / 255);
      int32_t L = out[f * 2] + l, R = out[f * 2 + 1] + r;
      if(L > 32767) L = 32767;
      if(L < -32768) L = -32768;
      if(R > 32767) R = 32767;
      if(R < -32768) R = -32768;
      out[f * 2] = (int16_t)L; out[f * 2 + 1] = (int16_t)R;
      if(c->dir >= 0) c->pos += step; else c->pos -= step;
    }
  }
}

static void mark_row(TrPlayer *pl, GmlTrackerModule *m, uint32_t frame){
  if(m->timeline_count == m->timeline_cap){
    uint32_t cap = m->timeline_cap ? m->timeline_cap * 2 : 1024;
    GmlTrackerRowMark *grown = (GmlTrackerRowMark*)realloc(m->timeline, cap * sizeof *grown);
    if(!grown) return;
    m->timeline = grown; m->timeline_cap = cap;
  }
  GmlTrackerRowMark *mk = &m->timeline[m->timeline_count++];
  mk->frame = frame;
  mk->order = (uint16_t)pl->order;
  mk->pattern = (uint16_t)m->orders[pl->order];
  mk->row = (uint16_t)pl->row;
  mk->speed = (uint8_t)pl->speed;
  mk->bpm = (uint8_t)(pl->bpm > 255 ? 255 : pl->bpm);
}

int gml_tracker_render(GmlTrackerModule *m, int rate, int pan_separation,
                       int16_t **pcm, uint32_t *frames, uint32_t *loop_frame){
  if(!m || !pcm || !frames || rate < 8000) return 0;
  if(pan_separation < 0) pan_separation = 0;
  if(pan_separation > 128) pan_separation = 128;
  free(m->timeline); m->timeline = NULL; m->timeline_count = m->timeline_cap = 0;
  free(m->events); m->events = NULL; m->event_count = m->event_cap = 0;

  TrPlayer pl; memset(&pl, 0, sizeof pl);
  pl.m = m; pl.recording = m; pl.rate = rate; pl.pan_separation = pan_separation;
  pl.speed = m->default_speed; pl.bpm = m->default_bpm;
  pl.global_volume = 64;
  for(int i = 0; i < m->channels; i++){
    pl.ch[i].panning = m->is_xm ? 128 : ((i & 3) == 1 || (i & 3) == 2 ? 192 : 64);
    pl.ch[i].fade = 65536;
    pl.ch[i].dir = 1;
  }

  uint32_t cap_frames = (uint32_t)rate * TR_RENDER_CAP_SECONDS;
  uint32_t out_cap = rate * 60;
  int16_t *out = (int16_t*)calloc((size_t)out_cap * 2, sizeof(int16_t));
  if(!out) return 0;
  uint32_t total = 0;
  uint32_t loop_at = 0;

  /* visited row-starts for loop detection; E6x pattern loops legitimately revisit rows, so the
   * bitmap is only consulted while no channel is inside one. */
  uint8_t *visited = (uint8_t*)calloc((size_t)m->num_orders * 256 / 8 + 1, 1);
  if(!visited){ free(out); return 0; }
  int done = 0;

  while(!done && total < cap_frames){
    /* row start */
    int in_pattern_loop = 0;
    for(int i = 0; i < m->channels; i++) if(pl.ch[i].loop_count) in_pattern_loop = 1;
    if(!in_pattern_loop){
      uint32_t bit = (uint32_t)pl.order * 256 + pl.row;
      if(visited[bit / 8] & (1 << (bit % 8))){
        /* the song came back to a row it already played: that first pass is the loop point */
        for(uint32_t k = 0; k < m->timeline_count; k++)
          if(m->timeline[k].order == pl.order && m->timeline[k].row == pl.row){
            loop_at = m->timeline[k].frame; break;
          }
        break;
      }
      visited[bit / 8] |= (uint8_t)(1 << (bit % 8));
    }
    mark_row(&pl, m, total);

    const TrPattern *pat = &m->pattern[m->orders[pl.order]];
    pl.do_jump = 0; pl.pattern_delay = 0;
    int row_ticks;
    for(int i = 0; i < m->channels; i++){
      const TrCell *cell = &pat->cells[pl.row * m->channels + i];
      TrChannel *c = &pl.ch[i];
      c->vib_period_offset = 0; c->arp_offset = 0; c->trem_vol_offset = 0;
      int delayed = cell->effect == 0xE && (cell->param >> 4) == 0xD && (cell->param & 15) != 0;
      if(!delayed) note_on(&pl, c, cell);
      if(m->is_xm && cell->volcol) volume_column_tick0(&pl, c, cell->volcol);
      row_effects_tick0(&pl, c, cell);
    }
    row_ticks = pl.speed * (1 + pl.pattern_delay);
    for(pl.tick = 0; pl.tick < row_ticks; pl.tick++){
      if(pl.tick > 0){
        for(int i = 0; i < m->channels; i++){
          const TrCell *cell = &pat->cells[pl.row * m->channels + i];
          TrChannel *c = &pl.ch[i];
          c->trem_vol_offset = 0;
          if(m->is_xm && cell->volcol) volume_column_tick(&pl, c, cell->volcol);
          row_effects_tick(&pl, c, cell);
        }
      }
      for(int i = 0; i < m->channels; i++) advance_envelopes(&pl, &pl.ch[i]);
      /* exact tick length: rate * 2.5 / bpm frames, remainder carried */
      uint32_t num = (uint32_t)pl.rate * 5 + pl.tick_rem;
      uint32_t den = (uint32_t)pl.bpm * 2;
      uint32_t tick_frames = num / den;
      pl.tick_rem = num % den;
      while(total + tick_frames > out_cap){
        uint32_t grown_cap = out_cap * 2;
        int16_t *grown = (int16_t*)realloc(out, (size_t)grown_cap * 2 * sizeof(int16_t));
        if(!grown){ free(out); free(visited); return 0; }
        memset(grown + (size_t)out_cap * 2, 0, (size_t)(grown_cap - out_cap) * 2 * sizeof(int16_t));
        out = grown; out_cap = grown_cap;
      }
      mix_tick(&pl, out + (size_t)total * 2, tick_frames);
      total += tick_frames;
      pl.current_frame = total;
      if(total >= cap_frames) break;
    }
    /* advance position */
    if(pl.do_jump){
      if(pl.jump_order >= m->num_orders){ done = 1; }
      else if(pl.jump_order < pl.order ||
              (pl.jump_order == pl.order && pl.jump_row < pl.row)){
        /* a backwards jump: land there, and let the visited bitmap catch the revisit */
        pl.order = pl.jump_order; pl.row = pl.jump_row;
      } else {
        pl.order = pl.jump_order; pl.row = pl.jump_row;
      }
      if(!done && pl.row >= m->pattern[m->orders[pl.order]].rows) pl.row = 0;
    } else {
      pl.row++;
      if(pl.row >= pat->rows){
        pl.row = 0;
        pl.order++;
        if(pl.order >= m->num_orders){
          /* natural end: loop back to the restart position */
          pl.order = m->restart_pos;
          if(pl.order >= m->num_orders){ done = 1; }
        }
      }
    }
  }
  free(visited);
  *pcm = out;
  *frames = total;
  if(loop_frame) *loop_frame = loop_at;
  return total > 0;
}

/* ---------------------------------------------------------------- spectrum */

/* cos(2*pi*k/1024) in Q30 for k = 0..511, baked so a spectrum query is pure integer work. */
static const int32_t cos_1024_q30[512] = {
  1073741824,1073721611,1073660973,1073559913,1073418433,1073236540,1073014240,1072751542,
  1072448455,1072104991,1071721163,1071296985,1070832474,1070327646,1069782521,1069197120,
  1068571464,1067905576,1067199483,1066453210,1065666786,1064840240,1063973603,1063066909,
  1062120190,1061133483,1060106826,1059040255,1057933813,1056787540,1055601479,1054375676,
  1053110176,1051805027,1050460278,1049075980,1047652185,1046188946,1044686319,1043144360,
  1041563127,1039942680,1038283080,1036584389,1034846671,1033069992,1031254418,1029400018,
  1027506862,1025575020,1023604567,1021595575,1019548121,1017462281,1015338134,1013175761,
  1010975242,1008736660,1006460100,1004145648,1001793390,999403415,996975812,994510675,
  992008094,989468165,986890984,984276646,981625251,978936898,976211688,973449725,
  970651112,967815955,964944360,962036435,959092290,956112036,953095785,950043650,
  946955747,943832191,940673101,937478595,934248793,930983817,927683790,924348837,
  920979082,917574653,914135678,910662286,907154608,903612776,900036924,896427186,
  892783698,889106597,885396022,881652112,877875009,874064853,870221790,866345964,
  862437520,858496606,854523370,850517961,846480531,842411232,838310216,834177638,
  830013654,825818421,821592095,817334838,813046808,808728167,804379079,799999706,
  795590213,791150767,786681534,782182683,777654384,773096806,768510122,763894504,
  759250125,754577161,749875788,745146182,740388522,735602987,730789757,725949013,
  721080937,716185713,711263525,706314559,701339000,696337036,691308855,686254647,
  681174602,676068911,670937767,665781362,660599890,655393548,650162530,644907034,
  639627258,634323400,628995660,623644239,618269338,612871159,607449906,602005783,
  596538995,591049748,585538248,580004702,574449320,568872310,563273883,557654248,
  552013618,546352205,540670223,534967884,529245404,523502998,517740883,511959275,
  506158392,500338453,494499676,488642281,482766489,476872522,470960600,465030947,
  459083786,453119340,447137835,441139496,435124548,429093217,423045732,416982319,
  410903207,404808624,398698801,392573967,386434353,380280190,374111709,367929144,
  361732726,355522689,349299266,343062693,336813204,330551034,324276419,317989595,
  311690799,305380268,299058239,292724951,286380643,280025552,273659918,267283981,
  260897982,254502159,248096755,241682010,235258165,228825464,222384147,215934457,
  209476638,203010932,196537583,190056834,183568930,177074115,170572633,164064728,
  157550647,151030634,144504935,137973796,131437462,124896179,118350194,111799753,
  105245103,98686491,92124163,85558366,78989349,72417357,65842639,59265442,
  52686014,46104602,39521455,32936819,26350943,19764076,13176464,6588356,
  0,-6588356,-13176464,-19764076,-26350943,-32936819,-39521455,-46104602,
  -52686014,-59265442,-65842639,-72417357,-78989349,-85558366,-92124163,-98686491,
  -105245103,-111799753,-118350194,-124896179,-131437462,-137973796,-144504935,-151030634,
  -157550647,-164064728,-170572633,-177074115,-183568930,-190056834,-196537583,-203010932,
  -209476638,-215934457,-222384147,-228825464,-235258165,-241682010,-248096755,-254502159,
  -260897982,-267283981,-273659918,-280025552,-286380643,-292724951,-299058239,-305380268,
  -311690799,-317989595,-324276419,-330551034,-336813204,-343062693,-349299266,-355522689,
  -361732726,-367929144,-374111709,-380280190,-386434353,-392573967,-398698801,-404808624,
  -410903207,-416982319,-423045732,-429093217,-435124548,-441139496,-447137835,-453119340,
  -459083786,-465030947,-470960600,-476872522,-482766489,-488642281,-494499676,-500338453,
  -506158392,-511959275,-517740883,-523502998,-529245404,-534967884,-540670223,-546352205,
  -552013618,-557654248,-563273883,-568872310,-574449320,-580004702,-585538248,-591049748,
  -596538995,-602005783,-607449906,-612871159,-618269338,-623644239,-628995660,-634323400,
  -639627258,-644907034,-650162530,-655393548,-660599890,-665781362,-670937767,-676068911,
  -681174602,-686254647,-691308855,-696337036,-701339000,-706314559,-711263525,-716185713,
  -721080937,-725949013,-730789757,-735602987,-740388522,-745146182,-749875788,-754577161,
  -759250125,-763894504,-768510122,-773096806,-777654384,-782182683,-786681534,-791150767,
  -795590213,-799999706,-804379079,-808728167,-813046808,-817334838,-821592095,-825818421,
  -830013654,-834177638,-838310216,-842411232,-846480531,-850517961,-854523370,-858496606,
  -862437520,-866345964,-870221790,-874064853,-877875009,-881652112,-885396022,-889106597,
  -892783698,-896427186,-900036924,-903612776,-907154608,-910662286,-914135678,-917574653,
  -920979082,-924348837,-927683790,-930983817,-934248793,-937478595,-940673101,-943832191,
  -946955747,-950043650,-953095785,-956112036,-959092290,-962036435,-964944360,-967815955,
  -970651112,-973449725,-976211688,-978936898,-981625251,-984276646,-986890984,-989468165,
  -992008094,-994510675,-996975812,-999403415,-1001793390,-1004145648,-1006460100,-1008736660,
  -1010975242,-1013175761,-1015338134,-1017462281,-1019548121,-1021595575,-1023604567,-1025575020,
  -1027506862,-1029400018,-1031254418,-1033069992,-1034846671,-1036584389,-1038283080,-1039942680,
  -1041563127,-1043144360,-1044686319,-1046188946,-1047652185,-1049075980,-1050460278,-1051805027,
  -1053110176,-1054375676,-1055601479,-1056787540,-1057933813,-1059040255,-1060106826,-1061133483,
  -1062120190,-1063066909,-1063973603,-1064840240,-1065666786,-1066453210,-1067199483,-1067905576,
  -1068571464,-1069197120,-1069782521,-1070327646,-1070832474,-1071296985,-1071721163,-1072104991,
  -1072448455,-1072751542,-1073014240,-1073236540,-1073418433,-1073559913,-1073660973,-1073721611,
};

static int32_t cos_q30(int index){
  index &= 1023;
  if(index < 512) return cos_1024_q30[index];
  return cos_1024_q30[index - 512] == 0 ? 0 : -cos_1024_q30[index - 512];
}

uint32_t gml_tracker_spectrum(const int16_t *stereo, uint32_t frames,
                              uint32_t at_frame, int band, int bands){
  enum { WINDOW = 1024 };
  if(!stereo || !frames || bands <= 0 || band < 0 || band >= bands) return 0;
  if(at_frame > frames) at_frame = frames;
  uint32_t start = at_frame > WINDOW ? at_frame - WINDOW : 0;
  uint32_t n = at_frame - start;
  if(n < 8) return 0;
  /* Goertzel bin: the band maps onto a 1024-point spectrum's bin. */
  int bin = (int)((int64_t)band * 512 / bands);
  int64_t coeff = 2 * (int64_t)cos_q30(bin);        /* Q30, doubled */
  int64_t s1 = 0, s2 = 0;
  for(uint32_t i = 0; i < n; i++){
    const int16_t *f = stereo + (size_t)(start + i) * 2;
    int32_t x = ((int32_t)f[0] + f[1]) >> 3;         /* mono fold with headroom */
    int64_t s0 = (int64_t)x + ((coeff * s1) >> 30) - s2;
    s2 = s1; s1 = s0;
  }
  /* |X|^2 = s1^2 + s2^2 - coeff*s1*s2 (coeff already doubled above halves back out) */
  int64_t cross = ((coeff * s1) >> 30) * s2;
  int64_t power = s1 * s1 + s2 * s2 - cross;
  if(power < 0) power = 0;
  /* normalize: full-scale bin magnitude is about n/2 * 4096 after the >>3 fold */
  uint64_t magnitude = 0;
  { uint64_t v = (uint64_t)power, r = 0, bit = (uint64_t)1 << 62;
    while(bit > v) bit >>= 2;
    while(bit){ if(v >= r + bit){ v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    magnitude = r; }
  uint64_t full = ((uint64_t)n * 4096u) / 2u;
  if(!full) return 0;
  uint64_t out = (magnitude << 16) / full;
  return out > 65536u ? 65536u : (uint32_t)out;
}
