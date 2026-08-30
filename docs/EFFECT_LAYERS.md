<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Room-layer effects: what they do, and how this runtime draws them

Studio room records can attach named effects and property lists to a layer. This runtime reads
those names and properties and applies the image operations documented below. These descriptions
specify this implementation, not an exact-output claim about another renderer. The stated ranges
describe the values handled here. A property this runtime does not use is said so.

## Common mechanics

- The layer is drawn into an isolated capture target (transparent black, premultiplied), the
  effect runs over that capture, and the result is composited back with source-over. "Affects
  below" captures what is already on the target instead, so the effect reads the scene beneath.
- Time is the frame clock in seconds. Camera position is the room camera of the current view.
- "The sampler" is the texture the property list names. Two effects read it as a noise field
  (clouds, underwater); boxes reads it as a colour palette; the others do not read theirs.

## Tint (`_filter_tintfilter`)

`g_TintCol`: colour with alpha. Every channel of the capture, alpha included, is multiplied by
the tint. That is the whole effect.

## Colourise (`_filter_colourise`)

`g_TintCol`: colour; `g_Intensity`: 0..1. A duotone: the luminance of each pixel selects a point
on a black -> tint -> white ramp, where the tint sits at its own luminance, and the result is mixed
with the original by the intensity. Luminance is the Rec. 601 weighting.

## RGB noise (`_filter_rgbnoise`)

`g_Intensity`: 0..1; `g_Animation`: any real, a phase that changes the pattern; `g_Colour`: colour
that scales the noise. Every pixel receives an independent random colour whose three channels are
uniform in 0..1 and scaled by the colour, mixed into the pixel by the intensity and the pixel's
own coverage. The pattern is a function of the pixel position and the animation phase only, so
it is stable while the phase is stable and changes completely when it moves. The noise texture the
property list names is not read: a hash of the position and phase produces the values.

## Clouds (`_filter_clouds`)

`g_CloudScale`: pixels per noise unit; `g_CloudVelocity`: drift in pixels per second;
`g_CloudTurbulence`: how much the field churns over time; `g_CloudLevel`: coverage threshold in
0..1; `g_CloudWaves`: amount of a sinusoidal warp along x; `g_CloudShape`: x/y stretch;
`g_CloudDensity`: steepness of the edge between sky and cloud; `g_CloudFade`: softness of the lit
edge; `g_CloudColour1`: lit colour; `g_CloudColour2`: shaded colour; `g_CloudShadeOffset`: where
the shaded sample is taken relative to the lit one; `g_CloudShadeFade`: softness of the shade edge.

Two octaves of value noise from the sampler, scaled, stretched, drifted by the velocity and
churned by the turbulence, warped along y by a sine of x, give a field in 0..1. Coverage is a
smooth step from the level upward, as steep as the density and as soft as the fade. A second
field read at the shade offset gives the shaded fraction the same way with the shade fade, and the
cloud colour mixes from lit to shaded by it. The cloud is painted over the capture with that
coverage, limited to where the capture has pixels.

## Glow (`_effect_glow`)

`g_GlowRadius`: blur radius in pixels; `g_GlowQuality`: 1..16 blur passes; `g_GlowIntensity`:
0..; `g_GlowGamma`: brightness curve; `g_GlowAlpha`: 0..1 alpha of the layer itself.

The layer is composited normally with its alpha. Then a halo is built from the inside out: one
blur per quality step, each reaching further toward the radius than the last. A blur averages a
small source away, so each step is lifted by the square root of the ratio between the source's
peak and the blurred peak: a narrow reach keeps the core bright and a wide reach leaves a dimmer
skirt, which is the falloff of a halo, and the gamma raises the result so the skirt falls off
harder or softer. Each step carries its share of the intensity and is added
to the scene, clipped at white.

## Underwater (`_filter_underwater`)

`g_Distort1Speed`, `g_Distort2Speed`: scroll speeds of two noise reads; `g_Distort1Scale`,
`g_Distort2Scale`: their x/y scales; `g_Distort1Amount`, `g_Distort2Amount`: their displacement in
pixels; `g_ChromaSpreadAmount`: how much further the red and green channels are displaced;
`g_CamOffsetScale`: how much the camera position scrolls the noise; `g_GlintCol`: colour added
where the displacement is strongest; `g_TintCol`: multiplied colour; `g_AddCol`: added colour.

Two reads of the sampler, each at its own scale and scrolled by its own speed and by the camera,
give two displacement vectors centred on zero; their weighted sum is the displacement in pixels.
Each channel samples the capture at the displaced position, red furthest, green halfway, blue at
the base displacement, which spreads the colours the way a moving water surface does. Where the
displacement is near its maximum a glint colour is added, then the tint multiplies and the add
colour is added, both weighted by the pixel's coverage.

## Zoom blur (`_filter_zoom_blur`)

`g_ZoomBlurCenter`: x/y in 0..1 of the target; `g_ZoomBlurIntensity`: how far along the ray
toward the centre the samples reach, as a fraction of the distance; `g_ZoomBlurFocusRadius`:
pixels around the centre that stay sharp.

Each pixel averages a fixed number of samples taken along the segment from itself toward the
centre. The segment's length grows with the intensity and with the distance from the centre, and
is zero inside the focus radius, so the picture is sharp at the centre and streaks outward. The
noise texture the property list names is not read.

## Large blur (`_filter_large_blur`)

`g_Radius`: blur radius in pixels. A separable blur of that radius, applied as three box passes,
which is a close approximation of a Gaussian, in premultiplied space so edges do not darken. The
noise texture the property list names is not read.

## Boxes (`_filter_boxes`)

`g_BoxesScale`: cell size in pixels; `g_BoxesSize`: min/max box size as a fraction of the cell;
`g_BoxesDisplacement`: how far a box wanders from its cell centre, as a fraction of the cell;
`g_BoxesSpeed`: how fast it wanders; `g_BoxesAngle`: base rotation in degrees;
`g_BoxesRotation`: min/max spin in degrees per second; `g_BoxesRoundness`: 0 square .. 1 disc;
`g_BoxesColourSpeed`: how fast the palette scrolls; `g_BoxesColours`: number of palette entries
across the sampler; `g_BoxesSharpness`: edge sharpness.

The target is divided into cells. Each cell owns one box whose size, spin, wander phase and
palette entry are drawn from a hash of the cell, so they are stable for that cell and different
from its neighbours. The box orbits its cell centre by the displacement at the speed, spins at
its rate around the base angle, and is drawn as a rounded rectangle whose corner radius follows
the roundness and whose edge width follows the sharpness. Its colour is the palette entry read
from the sampler at a position that scrolls with the colour speed. Each pixel considers its own
cell and the eight around it, so boxes may cross cell borders, and paints the boxes over the
capture where the capture has pixels.
