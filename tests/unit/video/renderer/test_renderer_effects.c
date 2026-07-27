/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 8, HEIGHT = 8, PIXEL_COUNT = WIDTH * HEIGHT };

static int failures;

static void expect(int condition, const char *message) {
  if (condition) return;
  fprintf(stderr, "renderer effects: %s\n", message);
  failures++;
}

static uint64_t pixel_hash(const uint32_t *pixels, size_t count) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < count; i++) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      hash ^= (pixels[i] >> shift) & 0xffu;
      hash *= UINT64_C(1099511628211);
    }
  }
  return hash;
}

static void fill_pixels(uint32_t *background, uint32_t *layer) {
  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      unsigned i = (unsigned)(y * WIDTH + x);
      unsigned alpha = 72u + (i * 29u) % 184u;
      unsigned red = (unsigned)(x * 31 + y * 17 + 23) & 255u;
      unsigned green = (unsigned)(x * 11 + y * 37 + 41) & 255u;
      unsigned blue = (unsigned)(x * 43 + y * 7 + 13) & 255u;
      background[i] = 0xff000000u | (red << 16) | (green << 8) | blue;
      layer[i] = (alpha << 24) |
                 (((red * alpha + 127u) / 255u) << 16) |
                 (((green * alpha + 127u) / 255u) << 8) |
                 ((blue * alpha + 127u) / 255u);
    }
  }
}

static void fill_sampler(uint8_t *rgba) {
  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      size_t offset = (size_t)(y * 4 + x) * 4u;
      rgba[offset + 0] = (uint8_t)(x * 53 + y * 19 + 7);
      rgba[offset + 1] = (uint8_t)(x * 17 + y * 61 + 29);
      rgba[offset + 2] = (uint8_t)(x * 37 + y * 23 + 47);
      rgba[offset + 3] = 255;
    }
  }
}

static void release_effect_state(GmlRender *render) {
  free(render->layer_noise_rgb);
  free(render->layer_filter_src);
  free(render->layer_filter_work);
  free(render->layer_filter_aux);
  free(render->layer_blur_taps);
}

static uint64_t run_filter(const GmlLayerFilter *filter) {
  uint32_t background[PIXEL_COUNT], layer[PIXEL_COUNT];
  uint8_t sampler[4 * 4 * 4];
  GmlSprite sprite;
  GmlRender render;

  memset(&render, 0, sizeof(render));
  memset(&sprite, 0, sizeof(sprite));
  fill_pixels(background, layer);
  fill_sampler(sampler);

  sprite.w = 4;
  sprite.h = 4;
  sprite.n_frames = 1;
  sprite.runtime_rgba = sampler;
  render.spr = &sprite;
  render.n_spr = 1;
  render.fb = background;
  render.fbw = WIDTH;
  render.fbh = HEIGHT;
  render.base_fb = background;
  render.base_fbw = WIDTH;
  render.base_fbh = HEIGHT;
  render.interp = 1;
  render.target_id = -1;
  render.fb_opaque_known = 1;
  render.fb_all_opaque = 1;

  expect(gml_render_layer_filter_begin(&render, filter),
         "filter begin rejected a bounded synthetic target");
  expect(render.layer_filter_active && render.target_sp == 1,
         "filter begin did not establish isolated target ownership");
  memcpy(render.fb, layer, sizeof(layer));
  render.fb_opaque_known = 0;
  render.fb_all_opaque = 0;
  render.fb_all_transparent = 0;
  gml_render_layer_filter_end(&render, filter, 0.375);
  expect(!render.layer_filter_active && render.target_sp == 0 &&
         render.fb == background,
         "filter end did not restore the original target transactionally");

  uint64_t result = pixel_hash(background, PIXEL_COUNT);
  release_effect_state(&render);
  return result;
}

static uint64_t run_noise(void) {
  uint32_t pixels[PIXEL_COUNT], unused[PIXEL_COUNT];
  uint8_t sampler[4 * 4 * 4];
  uint32_t tpag_pointer = 0x10203040u;
  GmlTpag tpag;
  GmlAtlas atlas;
  GmlRender render;

  memset(&render, 0, sizeof(render));
  memset(&tpag, 0, sizeof(tpag));
  memset(&atlas, 0, sizeof(atlas));
  fill_pixels(pixels, unused);
  fill_sampler(sampler);
  tpag.sw = tpag.sh = 4;
  tpag.atlas = 0;
  atlas.w = atlas.h = 4;
  atlas.px = sampler;
  render.fb = pixels;
  render.fbw = WIDTH;
  render.fbh = HEIGHT;
  render.tpag = &tpag;
  render.tpag_ptr = &tpag_pointer;
  render.n_tpag = 1;
  render.atlas = &atlas;
  render.n_atlas = 1;
  gml_render_layer_rgb_noise(&render, tpag_pointer, 0.625, 0.375, 0x80c040u);

  uint64_t result = pixel_hash(pixels, PIXEL_COUNT);
  release_effect_state(&render);
  return result;
}

static uint64_t run_tint(void) {
  uint32_t pixels[PIXEL_COUNT], unused[PIXEL_COUNT];
  GmlRender render;
  memset(&render, 0, sizeof(render));
  fill_pixels(pixels, unused);
  render.fb = pixels;
  render.fbw = WIDTH;
  render.fbh = HEIGHT;
  render.fb_opaque_known = 1;
  render.fb_all_opaque = 1;
  gml_render_layer_tint(&render, 0xc08040a0u);
  return pixel_hash(pixels, PIXEL_COUNT);
}

static GmlLayerFilter filter_for(int kind) {
  GmlLayerFilter filter;
  memset(&filter, 0, sizeof(filter));
  filter.kind = kind;
  filter.sampler_sprite = 0;
  switch (kind) {
    case GML_LAYER_FILTER_TINT:
      filter.u.tint.colour = 0xb070c0e0u;
      break;
    case GML_LAYER_FILTER_CLOUDS:
      filter.u.clouds.scale = 9.0;
      filter.u.clouds.velocity[0] = 1.25;
      filter.u.clouds.velocity[1] = -0.75;
      filter.u.clouds.turbulence = 0.35;
      filter.u.clouds.level = 0.18;
      filter.u.clouds.waves = 0.12;
      filter.u.clouds.shape[0] = 0.8;
      filter.u.clouds.shape[1] = 1.1;
      filter.u.clouds.density = 1.3;
      filter.u.clouds.fade = 2.5;
      filter.u.clouds.shade_offset[0] = 0.3;
      filter.u.clouds.shade_offset[1] = -0.2;
      filter.u.clouds.shade_fade = 2.0;
      filter.u.clouds.light_colour = 0xe0b080ffu;
      filter.u.clouds.shade_colour = 0x305090ffu;
      break;
    case GML_LAYER_FILTER_GLOW:
      filter.u.glow.radius = 3.5;
      filter.u.glow.quality = 2.0;
      filter.u.glow.intensity = 0.6;
      filter.u.glow.gamma = 1.4;
      filter.u.glow.alpha = 0.7;
      break;
    case GML_LAYER_FILTER_UNDERWATER:
      filter.u.underwater.speed[0] = 0.7;
      filter.u.underwater.speed[1] = -0.4;
      filter.u.underwater.scale[0][0] = 0.08;
      filter.u.underwater.scale[0][1] = 0.11;
      filter.u.underwater.scale[1][0] = 0.07;
      filter.u.underwater.scale[1][1] = 0.09;
      filter.u.underwater.amount[0] = 1.2;
      filter.u.underwater.amount[1] = 0.8;
      filter.u.underwater.chroma = 0.35;
      filter.u.underwater.camera_scale = 0.4;
      filter.u.underwater.glint_colour = 0x80d0ffffu;
      filter.u.underwater.tint_colour = 0x90b0d0ffu;
      filter.u.underwater.add_colour = 0x102030ffu;
      break;
    case GML_LAYER_FILTER_ZOOM_BLUR:
      filter.u.zoom_blur.centre[0] = 0.45;
      filter.u.zoom_blur.centre[1] = 0.6;
      filter.u.zoom_blur.intensity = 0.35;
      filter.u.zoom_blur.focus_radius = 0.18;
      break;
    case GML_LAYER_FILTER_LARGE_BLUR:
      filter.u.large_blur.radius = 2.75;
      break;
    case GML_LAYER_FILTER_BOXES:
      filter.u.boxes.scale = 7.0;
      filter.u.boxes.size[0] = 0.8;
      filter.u.boxes.size[1] = 1.15;
      filter.u.boxes.displacement = 0.4;
      filter.u.boxes.speed = 0.3;
      filter.u.boxes.angle = 0.2;
      filter.u.boxes.rotation[0] = 0.6;
      filter.u.boxes.rotation[1] = -0.3;
      filter.u.boxes.roundness = 0.25;
      filter.u.boxes.colour_speed = 0.4;
      filter.u.boxes.colours = 3.0;
      filter.u.boxes.sharpness = 0.7;
      break;
    case GML_LAYER_FILTER_COLOURISE:
      filter.u.colourise.intensity = 0.65;
      filter.u.colourise.tint_colour = 0xff6040ffu;
      break;
    default:
      break;
  }
  return filter;
}

static void write_u32(uint8_t *bytes, size_t offset, uint32_t value) {
  bytes[offset + 0] = (uint8_t)value;
  bytes[offset + 1] = (uint8_t)(value >> 8);
  bytes[offset + 2] = (uint8_t)(value >> 16);
  bytes[offset + 3] = (uint8_t)(value >> 24);
}

static void check_shader_recognition(void) {
  static const char *const fragments[] = {
    "void main(){ vec4 sampled = texture2D(gm_BaseTexture, vec2(0.0)); "
    "if (sampled.a <= 0.25) discard; gl_FragColor = sampled; }",
    "const vec3 weights = vec3(0.2,0.7,0.1); void main(){ "
    "float luminance=dot(vec3(1.0),weights); "
    "gl_FragColor=vec4(luminance,luminance,luminance,1.0); }",
    "const vec3 toneA = vec3(256.0/256.0,256.0/256.0,256.0/256.0);"
    "const vec3 toneB = vec3(256.0/256.0,0.0/256.0,0.0/256.0);"
    "const vec3 toneC = vec3(0.0/256.0,0.0/256.0,0.0/256.0);"
    "const vec3 toneD = vec3(0.0/256.0,0.0/256.0,256.0/256.0);"
    "void main(){ float classifierUniform=1.0; gl_FragColor=vec4(classifierUniform); }",
    "const vec3 toneA = vec3(256.0/256.0,256.0/256.0,256.0/256.0);"
    "const vec3 toneB = vec3(256.0/256.0,0.0/256.0,0.0/256.0);"
    "const vec3 toneC = vec3(0.0/256.0,0.0/256.0,0.0/256.0);"
    "void main(){ float classifierUniform=1.0; gl_FragColor=vec4(classifierUniform); }"
  };
  enum { SHADER_COUNT = 5, DATA_SIZE = 4096 };
  uint8_t data[DATA_SIZE];
  GmlWin content;
  GmlRender render;
  size_t fragment_offset = 512;

  memset(data, 0, sizeof(data));
  memset(&content, 0, sizeof(content));
  write_u32(data, 0, SHADER_COUNT);
  for (size_t i = 0; i < sizeof(fragments) / sizeof(fragments[0]); i++) {
    size_t record_offset = 64 + i * 32;
    size_t length = strlen(fragments[i]);
    expect(fragment_offset + length + 1 < sizeof(data),
           "synthetic shader fixture exceeded its owned buffer");
    write_u32(data, 4 + i * 4, (uint32_t)record_offset);
    write_u32(data, record_offset + 16, (uint32_t)fragment_offset);
    memcpy(data + fragment_offset, fragments[i], length + 1);
    fragment_offset += length + 1;
  }
  write_u32(data, 4 + 4 * 4, DATA_SIZE - 8);

  content.data = data;
  content.size = sizeof(data);
  content.n_chunks = 1;
  memcpy(content.chunks[0].name, "SHDR", 4);
  content.chunks[0].off = 0;
  content.chunks[0].size = 4 + SHADER_COUNT * 4;

  expect(gml_render_init(&render, &content) == 0,
         "synthetic shader renderer initialization failed");
  expect(render.active_shader == -1 && render.n_shader_pal == SHADER_COUNT &&
         render.shader_pal,
         "shader table ownership or inactive default mismatch");
  if (render.shader_pal && render.n_shader_pal == SHADER_COUNT) {
    const struct GmlShaderPal *alpha = &render.shader_pal[0];
    const struct GmlShaderPal *gray = &render.shader_pal[1];
    const struct GmlShaderPal *palette = &render.shader_pal[2];
    const struct GmlShaderPal *near_match = &render.shader_pal[3];
    const struct GmlShaderPal *bounded = &render.shader_pal[4];
    expect(alpha->alpha_discard && alpha->alpha_discard_inclusive &&
           alpha->alpha_discard_cutoff == 0.25f,
           "alpha-discard structure was not recognized exactly");
    expect(gray->grayscale && gray->grayscale_alpha == 1.0f &&
           gray->grayscale_weight[0] == 0.2f &&
           gray->grayscale_weight[1] == 0.7f &&
           gray->grayscale_weight[2] == 0.1f,
           "grayscale structure or constants were not preserved");
    expect(palette->has && palette->L[0] == 255 && palette->L[1] == 255 &&
           palette->L[2] == 255 && palette->M[0] == 255 &&
           palette->M[1] == 0 && palette->M[2] == 0 &&
           palette->D[0] == 0 && palette->D[1] == 0 &&
           palette->D[2] == 0 && palette->S[0] == 0 &&
           palette->S[1] == 0 && palette->S[2] == 255,
           "palette structure or constants were not preserved");
    expect(!near_match->has && !bounded->has &&
           !near_match->alpha_discard && !bounded->alpha_discard,
           "partial or out-of-bounds shader record was accepted");
  }
  gml_render_free(&render);
}

static void check_palette_alpha_threshold(void) {
  static uint8_t rgba[] = {
    0, 128, 0, 0,
    255, 0, 0, 127,
    255, 0, 0, 128
  };
  int frame_index = 0;
  uint32_t pixels[] = {0xff102030u, 0xff102030u, 0xff102030u};
  GmlSprite sprite;
  GmlSprite stretched_sprite;
  GmlTpag tpag;
  GmlTpag stretched_tpag;
  GmlAtlas atlas;
  struct GmlShaderPal palette;
  GmlRender render;

  memset(&sprite, 0, sizeof(sprite));
  memset(&stretched_sprite, 0, sizeof(stretched_sprite));
  memset(&tpag, 0, sizeof(tpag));
  memset(&stretched_tpag, 0, sizeof(stretched_tpag));
  memset(&atlas, 0, sizeof(atlas));
  memset(&palette, 0, sizeof(palette));
  memset(&render, 0, sizeof(render));
  sprite.w = 3;
  sprite.h = 1;
  sprite.n_frames = 1;
  sprite.frame = &frame_index;
  tpag.sw = tpag.bw = 3;
  tpag.sh = tpag.bh = 1;
  tpag.atlas = 0;
  stretched_sprite.w = 1;
  stretched_sprite.h = 1;
  stretched_sprite.n_frames = 1;
  stretched_sprite.frame = &frame_index;
  stretched_tpag.sx = 2;
  stretched_tpag.sw = stretched_tpag.bw = 1;
  stretched_tpag.sh = stretched_tpag.bh = 1;
  stretched_tpag.atlas = 0;
  atlas.w = 3;
  atlas.h = 1;
  atlas.px = rgba;
  palette.has = 1;
  palette.L[0] = palette.L[1] = palette.L[2] = 240;
  palette.M[0] = 224;
  palette.M[1] = 32;
  palette.M[2] = 16;
  palette.D[0] = palette.D[1] = palette.D[2] = 8;
  palette.S[0] = 16;
  palette.S[1] = 64;
  palette.S[2] = 224;
  render.fb = render.base_fb = pixels;
  render.fbw = render.base_fbw = 3;
  render.fbh = render.base_fbh = 1;
  render.spr = &sprite;
  render.n_spr = 1;
  render.tpag = &tpag;
  render.n_tpag = 1;
  render.atlas = &atlas;
  render.n_atlas = 1;
  render.shader_pal = &palette;
  render.n_shader_pal = 1;
  render.active_shader = 0;
  render.alpha = 1.0;
  render.alphablend = 1;
  render.color_write_mask = 0x0f;
  render.target_id = -1;

  gml_draw_sprite(&render, 0, 0, 0, 0);
  expect(pixels[0] == 0xff102030u && pixels[1] == 0xff102030u &&
         pixels[2] == 0xffe02010u,
         "palette alpha threshold exposed residual RGB");

  pixels[0] = pixels[1] = pixels[2] = 0xff102030u;
  render.spr = &stretched_sprite;
  render.tpag = &stretched_tpag;
  gml_draw_sprite_stretched(&render, 0, 0, 0, 0, 3, 1, 0xffffffu, 1.0);
  expect(pixels[0] == 0xffe02010u && pixels[1] == 0xffe02010u &&
         pixels[2] == 0xffe02010u,
         "stretched sprite bypassed the active palette");
}

int main(void) {
  static const struct {
    const char *name;
    int kind;
    uint64_t expected;
  } cases[] = {
    {"tint-filter", GML_LAYER_FILTER_TINT, UINT64_C(0x46f70534d7271242)},
    {"clouds", GML_LAYER_FILTER_CLOUDS, UINT64_C(0xf85e05886d83ca7a)},
    {"glow", GML_LAYER_FILTER_GLOW, UINT64_C(0xb830b8b5c64b4f5c)},
    {"underwater", GML_LAYER_FILTER_UNDERWATER, UINT64_C(0x20f4198e8ea7106d)},
    {"zoom-blur", GML_LAYER_FILTER_ZOOM_BLUR, UINT64_C(0x2f9e97b3b530147c)},
    {"large-blur", GML_LAYER_FILTER_LARGE_BLUR, UINT64_C(0x34553f2943d685a6)},
    {"boxes", GML_LAYER_FILTER_BOXES, UINT64_C(0x0ac25df5129fcce4)},
    {"colourise", GML_LAYER_FILTER_COLOURISE, UINT64_C(0x6e103e4dfb900bc4)}
  };
  uint64_t noise = run_noise();
  uint64_t tint = run_tint();
  check_shader_recognition();
  check_palette_alpha_threshold();
  expect(noise == UINT64_C(0xe9d7942b9ca5361e), "rgb-noise");
  expect(tint == UINT64_C(0x9ded760f28a3f2a0), "direct-tint");
  printf("rgb-noise %016llx\n", (unsigned long long)noise);
  printf("direct-tint %016llx\n", (unsigned long long)tint);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    GmlLayerFilter filter = filter_for(cases[i].kind);
    uint64_t actual = run_filter(&filter);
    printf("%s %016llx\n", cases[i].name, (unsigned long long)actual);
    expect(actual == cases[i].expected, cases[i].name);
  }
  if (failures) return 1;
  puts("renderer effects tests: ok");
  return 0;
}
