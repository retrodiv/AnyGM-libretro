/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_internal.h"
#include "gml_render_backend.h"

#include <math.h>
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
    "uniform vec3 u_tint; void main(){ vec4 sampled = "
    "texture2D(gm_BaseTexture, v_vTexcoord); if(sampled.a < 0.25){ "
    "sampled.a = 0.0; } gl_FragColor = vec4(u_tint.rgb, sampled.a); }",
    "uniform vec3 u_partial; void main(){ vec4 sampled = "
    "texture2D(gm_BaseTexture, v_vTexcoord); if(sampled.a < 0.25){ "
    "sampled.a = 0.0; } sampled.r *= 0.5; "
    "gl_FragColor = vec4(u_partial.rgb, sampled.a); }",
    "const vec3 toneA = vec3(256.0/256.0,256.0/256.0,256.0/256.0);"
    "const vec3 toneB = vec3(256.0/256.0,0.0/256.0,0.0/256.0);"
    "const vec3 toneC = vec3(0.0/256.0,0.0/256.0,0.0/256.0);"
    "const vec3 toneD = vec3(0.0/256.0,0.0/256.0,256.0/256.0);"
    "void main(){ float classifierUniform=1.0; gl_FragColor=vec4(classifierUniform); }",
    "const vec3 toneA = vec3(256.0/256.0,256.0/256.0,256.0/256.0);"
    "const vec3 toneB = vec3(256.0/256.0,0.0/256.0,0.0/256.0);"
    "const vec3 toneC = vec3(0.0/256.0,0.0/256.0,0.0/256.0);"
    "void main(){ float classifierUniform=1.0; gl_FragColor=vec4(classifierUniform); }",
    "uniform vec3 u_blur_colour; void main(){ mediump vec4 sum=vec4(0.0,0.0,0.0,0.0);"
    "vec2 delta=vec2(0.01,0.0);"
    "sum+=texture2D(gm_BaseTexture,uv-1.0*delta)*0.25;"
    "sum+=texture2D(gm_BaseTexture,uv)*0.5;"
    "sum+=texture2D(gm_BaseTexture,uv+1.0*delta)*0.25;"
    "delta=vec2(0.0,0.02);"
    "sum+=texture2D(gm_BaseTexture,uv-1.0*delta)*0.1*sum;"
    "sum+=texture2D(gm_BaseTexture,uv+1.0*delta)*0.1*sum;"
    "gl_FragColor=vec4(u_blur_colour.rgb,sum.a);}",
    "uniform vec3 u_near_blur; void main(){ vec4 sum=vec4(0.0,0.0,0.0,0.0);"
    "vec2 delta=vec2(0.01,0.0);"
    "sum+=texture2D(gm_BaseTexture,uv-1.0*delta)*0.25;"
    "sum+=texture2D(gm_BaseTexture,uv)*0.5;"
    "sum+=texture2D(gm_BaseTexture,uv+1.0*delta)*0.25;"
    "delta=vec2(0.0,0.02);"
    "sum+=texture2D(gm_BaseTexture,uv-1.0*delta)*0.1*sum;"
    "sum+=texture2D(gm_BaseTexture,uv+1.0*delta)*0.1*sum;"
    "sum.rgb*=0.5;gl_FragColor=vec4(u_near_blur.rgb,sum.a);}",
    "float quantize(float v,float r){return floor(v*r)/r;}"
    "float wrap_value(float v,float d){return mod(mod(v,d)+d,d);}"
    "float near_quantize(float v,float r){return floor(v*r)/r;}"
    "float near_wrap(float v,float d){return mod(mod(v,d)+d,d);}"
  };
  enum { SHADER_COUNT = 13, DATA_SIZE = 16384 };
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
  write_u32(data, 4 + 12 * 4, DATA_SIZE - 8);

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
    struct GmlShaderPal *mask = &render.shader_pal[2];
    const struct GmlShaderPal *mask_near_match = &render.shader_pal[3];
    const struct GmlShaderPal *palette = &render.shader_pal[4];
    const struct GmlShaderPal *near_match = &render.shader_pal[5];
    struct GmlShaderPal *blur = &render.shader_pal[6];
    const struct GmlShaderPal *blur_near_match = &render.shader_pal[7];
    const struct GmlShaderPal *binary_hsv = &render.shader_pal[8];
    const struct GmlShaderPal *binary_hsv_near_match = &render.shader_pal[9];
    struct GmlShaderPal *noise_jumble = &render.shader_pal[10];
    const struct GmlShaderPal *noise_jumble_near_match = &render.shader_pal[11];
    const struct GmlShaderPal *bounded = &render.shader_pal[12];
    expect(alpha->alpha_discard && alpha->alpha_discard_inclusive &&
           alpha->alpha_discard_cutoff == 0.25f,
           "alpha-discard structure was not recognized exactly");
    expect(gray->grayscale && gray->grayscale_alpha == 1.0f &&
           gray->grayscale_weight[0] == 0.2f &&
           gray->grayscale_weight[1] == 0.7f &&
           gray->grayscale_weight[2] == 0.1f,
           "grayscale structure or constants were not preserved");
    expect(mask->solid_alpha_mask && !mask->solid_alpha_mask_inclusive &&
           mask->solid_alpha_mask_cutoff == 0.25f &&
           !strcmp(mask->solid_alpha_mask_uniform, "u_tint") &&
           gml_render_shader_is_compiled(&render, 2),
           "constant-colour alpha-mask graph was not recognized exactly");
    int mask_uniform =
      gml_render_shader_uniform_handle(&render, 2, "u_tint");
    const double mask_values[4] = {0.25, 0.5, 0.75, 1.0};
    gml_render_shader_uniform_set(&render, mask_uniform, mask_values);
    expect(mask_uniform == 2 * 64 + 51 &&
           mask->solid_alpha_mask_colour[0] == 0.25f &&
           mask->solid_alpha_mask_colour[1] == 0.5f &&
           mask->solid_alpha_mask_colour[2] == 0.75f,
           "constant-colour alpha-mask uniform did not retain its values");
    expect(blur->solid_blur_alpha &&
           blur->solid_blur_alpha_x_count == 3 &&
           blur->solid_blur_alpha_y_count == 2 &&
           blur->solid_blur_alpha_step_x == 0.01f &&
           blur->solid_blur_alpha_step_y == 0.02f &&
           !strcmp(blur->solid_blur_alpha_uniform, "u_blur_colour") &&
           gml_render_shader_is_compiled(&render, 6),
           "constant-colour alpha-convolution graph was not recognized exactly");
    int blur_uniform =
      gml_render_shader_uniform_handle(&render, 6, "u_blur_colour");
    const double blur_values[4] = {0.125, 0.375, 0.625, 1.0};
    gml_render_shader_uniform_set(&render, blur_uniform, blur_values);
    expect(blur_uniform == 6 * 64 + 52 &&
           blur->solid_blur_alpha_rgb == 0x0020609fu,
           "constant-colour alpha-convolution uniform did not retain its values");
    expect(palette->has && palette->L[0] == 255 && palette->L[1] == 255 &&
           palette->L[2] == 255 && palette->M[0] == 255 &&
           palette->M[1] == 0 && palette->M[2] == 0 &&
           palette->D[0] == 0 && palette->D[1] == 0 &&
           palette->D[2] == 0 && palette->S[0] == 0 &&
           palette->S[1] == 0 && palette->S[2] == 255,
           "palette structure or constants were not preserved");
    expect(binary_hsv->hsv_scan && binary_hsv->hsv_scan_binary_palette &&
           fabsf(binary_hsv->hsv_scan_uv_scale - 0.875f) < 0.000001f &&
           fabsf(binary_hsv->hsv_scan_binary_threshold - 0.375f) < 0.000001f &&
           fabsf(binary_hsv->hsv_scan_binary_high[0] - 0.2f) < 0.000001f &&
           fabsf(binary_hsv->hsv_scan_binary_low[2] - 0.1f) < 0.000001f &&
           fabsf(binary_hsv->hsv_scan_row_frequency - 381.0f) < 0.001f &&
           gml_render_shader_is_compiled(&render, 8),
           "binary-palette HSV graph or constants were not preserved");
    expect(!mask_near_match->solid_alpha_mask &&
           !blur_near_match->solid_blur_alpha &&
           !binary_hsv_near_match->hsv_scan &&
           !noise_jumble_near_match->noise_jumble &&
           !near_match->has && !bounded->has &&
           !near_match->alpha_discard && !bounded->alpha_discard,
           "partial or out-of-bounds shader record was accepted");
    expect(noise_jumble->noise_jumble &&
           !strcmp(noise_jumble->noise_jumble_uniform[GML_NOISE_JUMBLE_INTENSITY],
                   "u_strength") &&
           !strcmp(noise_jumble->noise_jumble_uniform[GML_NOISE_JUMBLE_RESOLUTION],
                   "u_extent") &&
           !strcmp(noise_jumble->noise_jumble_uniform[GML_NOISE_JUMBLE_SHAKINESS],
                   "u_wobble") &&
           gml_render_shader_is_compiled(&render,10),
           "noise/jumble operation graph or semantic controls were not preserved");
    int extent_uniform=gml_render_shader_uniform_handle(&render,10,"u_extent");
    int noise_uniform=gml_render_shader_uniform_handle(&render,10,"u_noise");
    const double extent_values[4]={320.0,180.0,0.0,0.0};
    const double noise_values[4]={0.625,0.0,0.0,0.0};
    gml_render_shader_uniform_set(&render,extent_uniform,extent_values);
    gml_render_shader_uniform_set(&render,noise_uniform,noise_values);
    expect(extent_uniform==10*64+20+GML_NOISE_JUMBLE_RESOLUTION &&
           noise_uniform==10*64+20+GML_NOISE_JUMBLE_NOISE_LEVEL &&
           noise_jumble->noise_jumble_value[GML_NOISE_JUMBLE_RESOLUTION][0]==320.0f &&
           noise_jumble->noise_jumble_value[GML_NOISE_JUMBLE_RESOLUTION][1]==180.0f &&
           noise_jumble->noise_jumble_value[GML_NOISE_JUMBLE_NOISE_LEVEL][0]==0.625f,
           "noise/jumble uniform handles did not retain scalar and vector values");
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

static void check_zero_reference_alpha_test_pixels(void) {
  static uint8_t rgba[] = {
    240, 16, 32, 0,
    8, 224, 48, 64,
    24, 72, 240, 128,
    192, 96, 32, 255,
    16, 48, 80, 255,
    208, 160, 112, 128,
    64, 192, 224, 64,
    255, 255, 255, 0
  };
  int frame_index = 0;
  uint32_t baseline[8], zero_reference[8], threshold[8];
  GmlSprite sprite;
  GmlTpag tpag;
  GmlAtlas atlas;
  GmlRender render;

  memset(&sprite, 0, sizeof(sprite));
  memset(&tpag, 0, sizeof(tpag));
  memset(&atlas, 0, sizeof(atlas));
  memset(&render, 0, sizeof(render));
  sprite.w = 4;
  sprite.h = 2;
  sprite.n_frames = 1;
  sprite.frame = &frame_index;
  tpag.sw = tpag.bw = 4;
  tpag.sh = tpag.bh = 2;
  tpag.atlas = 0;
  atlas.w = 4;
  atlas.h = 2;
  atlas.px = rgba;
  render.spr = &sprite;
  render.n_spr = 1;
  render.tpag = &tpag;
  render.n_tpag = 1;
  render.atlas = &atlas;
  render.n_atlas = 1;
  render.fbw = render.base_fbw = 4;
  render.fbh = render.base_fbh = 2;
  render.alpha = 1.0;
  render.alphablend = 1;
  render.blendmode = 0;
  render.blend_equation = render.blend_equation_alpha = 1;
  render.color_write_mask = 0x0f;
  render.target_id = -1;
  render.active_shader = -1;
  render.lut_pal_sprite = -1;

  for (size_t index = 0; index < 8; index++)
    baseline[index] = zero_reference[index] = threshold[index] = 0xff183858u;
  render.fb = render.base_fb = baseline;
  gml_draw_sprite(&render, 0, 0, 0, 0);
  render.fb = render.base_fb = zero_reference;
  render.alpha_test_enable = 1;
  render.alpha_test_ref = 0;
  gml_draw_sprite(&render, 0, 0, 0, 0);
  expect(!memcmp(baseline, zero_reference, sizeof(baseline)),
         "zero-reference alpha test changed atlas sprite pixels");

  render.fb = render.base_fb = threshold;
  render.alpha_test_ref = 128;
  gml_draw_sprite(&render, 0, 0, 0, 0);
  expect(threshold[0] == 0xff183858u && threshold[1] == 0xff183858u &&
         threshold[2] == 0xff183858u && threshold[3] == 0xffc06020u,
         "nonzero alpha-test threshold bypassed the mapped sprite path");

  for (size_t index = 0; index < 8; index++)
    baseline[index] = zero_reference[index] = 0xff183858u;
  sprite.runtime_rgba = rgba;
  render.fb = render.base_fb = baseline;
  render.alpha_test_enable = 0;
  render.alpha_test_ref = 0;
  gml_draw_sprite(&render, 0, 0, 0, 0);
  render.fb = render.base_fb = zero_reference;
  render.alpha_test_enable = 1;
  gml_draw_sprite(&render, 0, 0, 0, 0);
  expect(!memcmp(baseline, zero_reference, sizeof(baseline)),
         "zero-reference alpha test changed runtime sprite pixels");
}

static void check_solid_alpha_mask_pixels(void) {
  static uint8_t rgba[] = {
    240, 16, 32, 63,
    8, 224, 48, 255
  };
  int frame_index = 0;
  uint32_t pixels[] = {0xff102030u, 0xff102030u};
  GmlSprite sprite;
  GmlTpag tpag;
  GmlAtlas atlas;
  struct GmlShaderPal mask;
  GmlRender render;

  memset(&sprite, 0, sizeof(sprite));
  memset(&tpag, 0, sizeof(tpag));
  memset(&atlas, 0, sizeof(atlas));
  memset(&mask, 0, sizeof(mask));
  memset(&render, 0, sizeof(render));
  sprite.w = 2;
  sprite.h = 1;
  sprite.n_frames = 1;
  sprite.frame = &frame_index;
  tpag.sw = tpag.bw = 2;
  tpag.sh = tpag.bh = 1;
  tpag.atlas = 0;
  atlas.w = 2;
  atlas.h = 1;
  atlas.px = rgba;
  mask.solid_alpha_mask = 1;
  mask.solid_alpha_mask_cutoff = 0.25f;
  mask.solid_alpha_mask_cutoff_step = 64;
  mask.solid_alpha_mask_colour[0] = 0.25f;
  mask.solid_alpha_mask_colour[1] = 0.5f;
  mask.solid_alpha_mask_colour[2] = 0.75f;
  mask.solid_alpha_mask_rgb = 0x004080bfu;
  render.fb = render.base_fb = pixels;
  render.fbw = render.base_fbw = 2;
  render.fbh = render.base_fbh = 1;
  render.spr = &sprite;
  render.n_spr = 1;
  render.tpag = &tpag;
  render.n_tpag = 1;
  render.atlas = &atlas;
  render.n_atlas = 1;
  render.shader_pal = &mask;
  render.n_shader_pal = 1;
  render.active_shader = 0;
  render.alpha = 1.0;
  render.alphablend = 1;
  render.color_write_mask = 0x0f;
  render.target_id = -1;

  gml_draw_sprite(&render, 0, 0, 0, 0);
  expect(pixels[0] == 0xff102030u && pixels[1] == 0xff4080bfu,
         "constant-colour alpha-mask pixel kernel diverged from its parsed graph");
}

static void check_solid_blur_alpha_pixels(void) {
  uint8_t rgba[3 * 3 * 4];
  int frame_index = 0;
  uint32_t pixels[3 * 3];
  GmlSprite sprite;
  GmlTpag tpag;
  GmlAtlas atlas;
  struct GmlShaderPal blur;
  GmlRender render;

  memset(rgba, 0, sizeof(rgba));
  rgba[(1 * 3 + 1) * 4 + 3] = 255;
  for (int index = 0; index < 9; index++) pixels[index] = 0xff000000u;
  memset(&sprite, 0, sizeof(sprite));
  memset(&tpag, 0, sizeof(tpag));
  memset(&atlas, 0, sizeof(atlas));
  memset(&blur, 0, sizeof(blur));
  memset(&render, 0, sizeof(render));
  sprite.w = sprite.h = 3;
  sprite.n_frames = 1;
  sprite.frame = &frame_index;
  tpag.sw = tpag.sh = tpag.bw = tpag.bh = 3;
  tpag.atlas = 0;
  tpag.solid_blur_alpha_shader = -1;
  atlas.w = atlas.h = 3;
  atlas.px = rgba;
  blur.solid_blur_alpha = 1;
  blur.solid_blur_alpha_rgb = 0x00643219u;
  blur.solid_blur_alpha_step_x = 1.0f / 3.0f;
  blur.solid_blur_alpha_step_y = 1.0f / 3.0f;
  blur.solid_blur_alpha_x_count = 3;
  blur.solid_blur_alpha_x_offset[0] = -1;
  blur.solid_blur_alpha_x_offset[1] = 0;
  blur.solid_blur_alpha_x_offset[2] = 1;
  blur.solid_blur_alpha_x_weight[0] = 0.25f;
  blur.solid_blur_alpha_x_weight[1] = 0.5f;
  blur.solid_blur_alpha_x_weight[2] = 0.25f;
  blur.solid_blur_alpha_y_count = 2;
  blur.solid_blur_alpha_y_offset[0] = -1;
  blur.solid_blur_alpha_y_offset[1] = 1;
  blur.solid_blur_alpha_y_weight[0] = 0.1f;
  blur.solid_blur_alpha_y_weight[1] = 0.1f;
  render.fb = render.base_fb = pixels;
  render.fbw = render.base_fbw = 3;
  render.fbh = render.base_fbh = 3;
  render.spr = &sprite;
  render.n_spr = 1;
  render.tpag = &tpag;
  render.n_tpag = 1;
  render.atlas = &atlas;
  render.n_atlas = 1;
  render.shader_pal = &blur;
  render.n_shader_pal = 1;
  render.active_shader = 0;
  render.alpha = 1.0;
  render.alphablend = 1;
  render.color_write_mask = 0x0f;
  render.target_id = -1;

  gml_draw_sprite(&render, 0, 0, 0, 0);
  expect(tpag.solid_blur_alpha_cache &&
           tpag.solid_blur_alpha_cache[1] == 0 &&
           tpag.solid_blur_alpha_cache[3] == 0x40000000u &&
           tpag.solid_blur_alpha_cache[4] == 0x80000000u &&
           tpag.solid_blur_alpha_cache[5] == 0x40000000u &&
           tpag.solid_blur_alpha_cache[7] == 0,
         "constant-colour alpha-convolution cache diverged from its parsed graph");
  expect((pixels[4] & 0x00ffffffu) != 0,
         "constant-colour alpha-convolution pixels were not composited");
  free(tpag.solid_blur_alpha_cache);
}

/* Fully synthetic skeletal record: identifiers, pixels and animation are authored for this test. */
static void encode_skeleton_text(uint8_t *destination,const char *text){
  uint32_t key=42;
  for(size_t index=0;text[index];index++){
    destination[index]=(uint8_t)((uint8_t)text[index]+(uint8_t)key);
    key*=key+1;
  }
}

static void check_skeleton_asset_and_pose(void) {
  static const char json[] =
    "{\"bones\":[{\"name\":\"root\"},{\"name\":\"joint\",\"parent\":\"root\","
    "\"x\":2,\"y\":3}],\"slots\":[{\"name\":\"panel\",\"bone\":\"joint\","
    "\"attachment\":\"tile\"}],\"skins\":{\"default\":{\"panel\":{"
    "\"tile\":{\"width\":2,\"height\":2},"
    "\"tile2\":{\"width\":2,\"height\":2}}}},\"animations\":{\"idle\":{"
    "\"bones\":{\"joint\":{\"rotate\":[{\"angle\":0},{\"time\":1,"
    "\"angle\":90}]}},\"slots\":{\"panel\":{\"attachment\":[{\"time\":0.75,"
    "\"name\":\"tile2\"}]}}}}}";
  static const char atlas_text[] =
    "neutral.png\n"
    "size: 4, 2\n"
    "format: RGBA8888\n"
    "filter: Linear,Linear\n"
    "repeat: none\n"
    "tile\n"
    "  bounds: 0, 0, 2, 2\n"
    "  offsets: 0, 0, 2, 2\n"
    "tile2\n"
    "  bounds: 2, 0, 2, 2\n"
    "  offsets: 0, 0, 2, 2\n";
  enum { RECORD = 0, LIST = 84, HEADER = 92, TEXT = 112 };
  uint8_t data[2048];
  uint32_t texture_pointer=0x10203040u;
  GmlWin content;
  GmlRender render;
  GmlSprite sprite;
  GmlTpag tpag;
  GmlAtlas atlas;
  GmlSpineDrawItem items[4];

  memset(data,0,sizeof(data));
  memset(&content,0,sizeof(content));
  memset(&render,0,sizeof(render));
  memset(&sprite,0,sizeof(sprite));
  memset(&tpag,0,sizeof(tpag));
  memset(&atlas,0,sizeof(atlas));
  write_u32(data,LIST,1);
  write_u32(data,LIST+4,texture_pointer);
  write_u32(data,HEADER,3);
  write_u32(data,HEADER+4,1);
  write_u32(data,HEADER+8,(uint32_t)strlen(json));
  write_u32(data,HEADER+12,(uint32_t)strlen(atlas_text));
  write_u32(data,HEADER+16,1);
  encode_skeleton_text(data+TEXT,json);
  encode_skeleton_text(data+TEXT+strlen(json),atlas_text);
  content.data=data;
  content.size=TEXT+strlen(json)+strlen(atlas_text);
  render.win=&content;
  render.tpag=&tpag;
  render.tpag_ptr=&texture_pointer;
  render.n_tpag=1;
  render.atlas=&atlas;
  render.n_atlas=1;
  tpag.atlas=0;
  atlas.w=4;
  atlas.h=2;

  expect(gml_render_parse_spine(&render,&sprite,RECORD,HEADER),
         "bounded skeletal asset was not decoded");
  expect(sprite.spine && sprite.spine->region_count==2 &&
           gml_render_sprite_is_skeleton(&render,0)==0,
         "skeletal asset metadata was not retained");
  render.spr=&sprite;
  render.n_spr=1;
  expect(gml_render_sprite_is_skeleton(&render,0),
         "skeletal sprite existence was not exposed");

  GmlRenderSkeletonState state;
  memset(&state,0,sizeof(state));
  state.animation="idle";
  state.time=0.5;
  gml_render_skeleton_state_set(&render,&state);
  int count=gml_render_spine_build_items(
    &render,&sprite,10,10,1,1,0,items,4);
  expect(count==1 &&
           fabs(items[0].x[0]-12.0)<0.001 &&
           fabs(items[0].y[0]-8.414213562)<0.001 &&
           fabs(items[0].x[1]-13.414213562)<0.001 &&
           fabs(items[0].y[1]-7.0)<0.001,
         "skeletal bone rotation did not transform the attachment quad");
  expect(items[0].u[0]==0.0 && items[0].u[1]==0.5,
         "skeletal setup attachment selected the wrong atlas region");

  state.time=0.8;
  gml_render_skeleton_state_set(&render,&state);
  count=gml_render_spine_build_items(
    &render,&sprite,10,10,1,1,0,items,4);
  expect(count==1 && items[0].u[0]==0.5 && items[0].u[1]==1.0,
         "skeletal attachment timeline did not select its keyed region");
  gml_render_skeleton_state_set(&render,NULL);
  gml_render_free_spine(sprite.spine);
}

static void initialize_primitive_render(
    GmlRender *render,uint32_t *pixels,int width,int height){
  memset(render,0,sizeof(*render));
  render->fb=render->base_fb=pixels;
  render->fbw=render->base_fbw=width;
  render->fbh=render->base_fbh=height;
  render->alpha=1.0;
  render->alphablend=1;
  render->blend_equation=1;
  render->blend_equation_alpha=1;
  render->color_write_mask=15;
  render->target_id=-1;
}

static void check_optimized_primitives(void) {
  enum { PRIMITIVE_WIDTH=8, PRIMITIVE_HEIGHT=8 };
  uint32_t pixels[PRIMITIVE_WIDTH*PRIMITIVE_HEIGHT];
  GmlRender render;

  for(size_t index=0;index<sizeof(pixels)/sizeof(pixels[0]);index++)
    pixels[index]=UINT32_C(0xFF102030);
  initialize_primitive_render(
    &render,pixels,PRIMITIVE_WIDTH,PRIMITIVE_HEIGHT);
  render.blendmode=1;
  gml_render_primitive_rectangle(
    &render,0,0,7,0,UINT32_C(0x00102040),0);
  for(int x=0;x<PRIMITIVE_WIDTH;x++)
    expect(pixels[x]==UINT32_C(0xFF504040),
           "additive rectangle span did not preserve saturating channels");

  for(size_t index=0;index<sizeof(pixels)/sizeof(pixels[0]);index++)
    pixels[index]=UINT32_C(0xFF010203);
  initialize_primitive_render(
    &render,pixels,PRIMITIVE_WIDTH,PRIMITIVE_HEIGHT);
  const uint32_t corners[4]={
    UINT32_C(0x000000ff),UINT32_C(0x00ff0000),
    UINT32_C(0x0000ff00),UINT32_C(0x00ffffff)
  };
  gml_render_primitive_rectangle_color(
    &render,1,1,5,4,corners[0],corners[1],corners[2],corners[3],0);
  uint32_t converted[4];
  for(int corner=0;corner<4;corner++)
    converted[corner]=gml_render_backend_color_to_xrgb(corners[corner]);
  for(int y=1;y<=4;y++){
    uint32_t left=gml_render_backend_lerp_xrgb(
      converted[0],converted[3],y-1,3);
    uint32_t right=gml_render_backend_lerp_xrgb(
      converted[1],converted[2],y-1,3);
    for(int x=1;x<=5;x++)
      expect(pixels[y*PRIMITIVE_WIDTH+x]==
               gml_render_backend_lerp_xrgb(left,right,x-1,4),
             "opaque rectangle gradient diverged from fixed-point interpolation");
  }

  for(size_t index=0;index<sizeof(pixels)/sizeof(pixels[0]);index++)
    pixels[index]=UINT32_C(0xFF010203);
  initialize_primitive_render(
    &render,pixels,PRIMITIVE_WIDTH,PRIMITIVE_HEIGHT);
  gml_render_primitive_triangle_alpha(
    &render,1,1,6,2,2,6,UINT32_C(0x004080c0),1.0,0);
  uint32_t triangle_color=
    gml_render_backend_color_to_xrgb(UINT32_C(0x004080c0));
  double denominator=(2.0-6.0)*(1.0-2.0)+(2.0-6.0)*(1.0-6.0);
  for(int y=0;y<PRIMITIVE_HEIGHT;y++){
    for(int x=0;x<PRIMITIVE_WIDTH;x++){
      double first=((2.0-6.0)*(x-2.0)+(2.0-6.0)*(y-6.0))/
                   denominator;
      double second=((6.0-1.0)*(x-2.0)+(1.0-2.0)*(y-6.0))/
                    denominator;
      double third=1.0-first-second;
      uint32_t expected=
        first>=0.0&&second>=0.0&&third>=0.0
          ? triangle_color : UINT32_C(0xFF010203);
      expect(pixels[y*PRIMITIVE_WIDTH+x]==expected,
             "opaque triangle span diverged from barycentric coverage");
    }
  }
}

static uint32_t fast8_solid_step(uint32_t destination,uint32_t source,uint32_t alpha){
  uint32_t inverse=256u-alpha;
  uint32_t red=(((source>>16)&255u)*alpha+
                ((destination>>16)&255u)*inverse)>>8;
  uint32_t green=(((source>>8)&255u)*alpha+
                  ((destination>>8)&255u)*inverse)>>8;
  uint32_t blue=((source&255u)*alpha+
                 (destination&255u)*inverse)>>8;
  return UINT32_C(0xff000000)|(red<<16)|(green<<8)|blue;
}

static void check_batched_solid_mask_order(void) {
  enum { SOURCE_SIDE=128, TARGET_SIDE=640 };
  uint32_t *pixels=malloc((size_t)TARGET_SIDE*TARGET_SIDE*sizeof(*pixels));
  GmlRender render;
  memset(&render,0,sizeof(render));
  render.spr=calloc(1,sizeof(*render.spr));
  render.tpag=calloc(1,sizeof(*render.tpag));
  render.atlas=calloc(1,sizeof(*render.atlas));
  render.shader_pal=calloc(1,sizeof(*render.shader_pal));
  expect(pixels && render.spr && render.tpag && render.atlas && render.shader_pal,
         "batched-mask fixture allocation failed");
  if(!pixels || !render.spr || !render.tpag || !render.atlas || !render.shader_pal){
    free(pixels);
    free(render.spr);
    free(render.tpag);
    free(render.atlas);
    free(render.shader_pal);
    return;
  }
  render.atlas[0].px=calloc(
    (size_t)SOURCE_SIDE*SOURCE_SIDE,4);
  render.spr[0].frame=malloc(sizeof(*render.spr[0].frame));
  expect(render.atlas[0].px && render.spr[0].frame,
         "batched-mask source allocation failed");
  if(!render.atlas[0].px || !render.spr[0].frame){
    gml_render_free(&render);
    free(pixels);
    return;
  }

  for(size_t index=0;index<(size_t)TARGET_SIDE*TARGET_SIDE;index++)
    pixels[index]=UINT32_C(0xff102030);
  for(int y=0;y<SOURCE_SIDE;y++){
    for(int x=0;x<SOURCE_SIDE;x++){
      uint8_t *sample=render.atlas[0].px+
        ((size_t)y*SOURCE_SIDE+x)*4u;
      sample[3]=(uint8_t)(x==0?0:x==1?64:x==2?128:255);
    }
  }
  render.fb=render.base_fb=pixels;
  render.fbw=render.base_fbw=TARGET_SIDE;
  render.fbh=render.base_fbh=TARGET_SIDE;
  render.alpha=1.0;
  render.alphablend=1;
  render.color_write_mask=15;
  render.target_id=-1;
  render.n_spr=render.n_tpag=render.n_atlas=render.n_shader_pal=1;
  render.active_shader=0;
  render.spr[0].w=render.spr[0].h=SOURCE_SIDE;
  render.spr[0].n_frames=1;
  render.spr[0].frame[0]=0;
  render.tpag[0].sw=render.tpag[0].bw=SOURCE_SIDE;
  render.tpag[0].sh=render.tpag[0].bh=SOURCE_SIDE;
  render.tpag[0].atlas=0;
  render.atlas[0].w=render.atlas[0].h=SOURCE_SIDE;
  render.shader_pal[0].solid_alpha_mask=1;

  static const uint32_t colours[]={
    UINT32_C(0x00ff0000),UINT32_C(0x0000ff00),UINT32_C(0x000000ff)
  };
  for(size_t index=0;index<sizeof(colours)/sizeof(colours[0]);index++){
    render.shader_pal[0].solid_alpha_mask_rgb=colours[index];
    gml_draw_sprite_ext(&render,0,0,0,0,5,5,0,UINT32_C(0x00ffffff),1.0);
  }
  expect(render.rotated_batch_count==3,
         "large axis-aligned masks were not retained in draw order");
  gml_render_flush_rotated_batch(&render);

  uint32_t expected=UINT32_C(0xff102030);
  for(size_t index=0;index<sizeof(colours)/sizeof(colours[0]);index++)
    expected=fast8_solid_step(expected,colours[index],64);
  expect(pixels[2*TARGET_SIDE+4]==UINT32_C(0xff102030),
         "zero-alpha SIMD lane overwrote its destination");
  expect(pixels[2*TARGET_SIDE+5]==expected,
         "batched partial-alpha masks changed composition order");
  expect(pixels[2*TARGET_SIDE+20]==UINT32_C(0xff0000ff),
         "batched opaque mask did not retain the final source colour");

  gml_render_free(&render);
  free(pixels);
}

static void check_maximum_preset_sprite(void) {
  uint8_t rgba[4]={64,128,192,128};
  uint32_t target=UINT32_C(0xff204080);
  int frame_index=0;
  GmlSprite sprite;
  GmlTpag tpag;
  GmlAtlas atlas;
  GmlRender render;
  memset(&sprite,0,sizeof sprite);
  memset(&tpag,0,sizeof tpag);
  memset(&atlas,0,sizeof atlas);
  memset(&render,0,sizeof render);
  sprite.w=sprite.h=1;
  sprite.n_frames=1;
  sprite.frame=&frame_index;
  tpag.sw=tpag.sh=tpag.bw=tpag.bh=1;
  tpag.atlas=0;
  atlas.w=atlas.h=1;
  atlas.px=rgba;
  atlas.decode_attempted=1;
  render.spr=&sprite;
  render.tpag=&tpag;
  render.atlas=&atlas;
  render.n_spr=render.n_tpag=render.n_atlas=1;
  render.fb=render.base_fb=&target;
  render.fbw=render.fbh=render.base_fbw=render.base_fbh=1;
  render.target_id=-1;
  render.alpha=1.0;
  render.alphablend=1;
  render.blendmode=4;
  render.blend_equation=render.blend_equation_alpha=1;
  render.color_write_mask=0x0F;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  gml_draw_sprite_ext(&render,0,0,0.0,0.0,1.0,1.0,0.0,0xFFFFFFu,0.5);
  expect(target==UINT32_C(0xff284050),
         "maximum preset sprite factors changed");
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
  check_binary_hsv_pixels();
  check_palette_alpha_threshold();
  check_zero_reference_alpha_test_pixels();
  check_solid_alpha_mask_pixels();
  check_solid_blur_alpha_pixels();
  check_skeleton_asset_and_pose();
  check_optimized_primitives();
  check_batched_solid_mask_order();
  check_maximum_preset_sprite();
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
