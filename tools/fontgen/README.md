<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Regenerating the bundled fallback glyph coverage

`src/generated/gml_default_font_data.h` is raster coverage for the face the classic runtime falls
back to when content's own TrueType face is unavailable. It is generated, and this is the
generator, so the table can be rebuilt from its input instead of being data whose origin has to
be taken on trust.

```sh
x86_64-w64-mingw32-gcc -O2 -o gen_default_font.exe gen_default_font.c -lgdi32 -luser32
./gen_default_font.exe LiberationSans-Regular.ttf "Liberation Sans" \
    default_font_advances.txt ../../src/generated/gml_default_font_data.h
python3 ../../tests/architecture/check_provenance.py generate
```

**Windows only, by construction.** The coverage is GDI output; no other rasterizer reproduces it,
which is why the production build consumes the immutable header and never runs this. It is not
part of `make`.

The layout parameters are fixed in the source: the face is loaded privately from the named file
with `AddFontResourceEx`, the selected TrueType bytes must match the input exactly, the height is
12 point at 96 dpi, and the quality is `NONANTIALIASED_QUALITY` — every pixel is 0 or 255, so no
font-smoothing setting can reach the result. A code point the face has no glyph for is drawn as
the face's own `.notdef` rather than as whatever the machine's font linking would substitute.

**The advances are not font data.** `default_font_advances.txt` holds one advance per code point,
first-party compatibility widths rather than values read from the typeface.
The generator takes them from this file so a re-rasterization cannot silently reflow text.

The source release and its OFL terms are recorded in `docs/PROVENANCE.md` and
`LICENSES/font-ofl-1.1.txt`. Exact input and output verification values belong in
external release evidence, not in this repository.

## The Studio table

`src/generated/gml_studio_default_font_data.h` is the same kind of table for the second-generation
Studio runtime's default font, and `gen_studio_font.c` is its generator. It rasterizes through the
vendored stb_truetype rather than GDI, so it builds and runs on any host:

```sh
cc -O2 -I ../../src/third_party/stb -o gen_studio_font gen_studio_font.c -lm
./gen_studio_font RobotoMono-Medium.ttf ../../src/generated/gml_studio_default_font_data.h
python3 ../../tests/architecture/check_provenance.py generate
```

Every parameter is fixed in the source: the advance is 9 pixels, which fixes the scale for a
monospaced face; the line box is 20 rows with the baseline on row 16; coverage is antialiased.
The input release and its OFL terms are recorded in `docs/PROVENANCE.md`.

## The classic Game Information table

The checked-in coverage has declared Liberation Sans provenance.
`gen_classic_info_font.c` takes the four font styles and a family name
as explicit inputs, checks the selected font bytes, and writes the
coverage header using Windows GDI ClearType. The generator carries
fixed layout parameters and emits MIT/OFL notices. The input fonts
are not bundled, and this repository alone does not prove that the
checked-in table was reproduced byte-for-byte. See
`docs/PROVENANCE.md` for the remaining verification boundary.
