#!/bin/sh
# Import the Neural Amp Modeler's MIT-licensed art into the NAMix bundle
# resource set. Dev-host only (needs ImageMagick); the committed files under
# ../resources/img are what ship — no end user ever runs this.
#
# Two kinds of asset, handled differently:
#
#   * The ten ICONS are copied as the original .svg files, verbatim. They are
#     rasterised at runtime, at exactly the size the layout asks for, by the
#     same NanoSVG rasteriser the original plugin renders them with — so the
#     glyph shapes match by construction and stay resolution-independent.
#
#   * The seven RASTER LAYERS carry the panel's visual identity (textured
#     backdrop, line overlay, knob face, file-row / meter / input-level
#     backgrounds, switch handle) and have no vector source. They are imported
#     preferring the @2x master for crispness, and converted to PNG32 because
#     Cairo reads PNG only (Background upstream is a .jpg).
#
# Everything else — knob pointers and arcs, meter fills, switch state, text —
# is drawn in code, exactly as the original does.
set -eu
cd "$(dirname "$0")"
. ./geometry.sh

OUT=../resources/img
mkdir -p "$OUT"

# ImageMagick 7 renamed `convert` to `magick`; accept either.
if command -v magick >/dev/null 2>&1; then
    IM="magick"
elif command -v convert >/dev/null 2>&1; then
    IM="convert"
else
    echo "ImageMagick not found (install imagemagick)" >&2
    exit 1
fi

[ -d "$NAM_UPSTREAM" ] || {
    echo "Upstream art not found at: $NAM_UPSTREAM" >&2
    echo "Set NAM_UPSTREAM=<path to NeuralAmpModeler/resources/img>" >&2
    exit 1
}

src() { # src <basename> -> preferred source path (@2x if present, else 1x)
    if [ -f "$NAM_UPSTREAM/$1@2x.png" ]; then
        echo "$NAM_UPSTREAM/$1@2x.png"
    elif [ -f "$NAM_UPSTREAM/$1@2x.jpg" ]; then
        echo "$NAM_UPSTREAM/$1@2x.jpg"
    elif [ -f "$NAM_UPSTREAM/$1.png" ]; then
        echo "$NAM_UPSTREAM/$1.png"
    else
        echo "$NAM_UPSTREAM/$1.jpg"
    fi
}

raster() { # raster <upstream-basename>
    s=$(src "$1")
    [ -f "$s" ] || { echo "MISSING: $1 (looked near $NAM_UPSTREAM)" >&2; exit 1; }
    "$IM" "$s" -strip PNG32:"$OUT/$1.png"
    echo "  $1.png  <-  $(basename "$s")"
}

icon() { # icon <upstream-basename> : copy the original SVG verbatim
    s="$NAM_UPSTREAM/$1.svg"
    [ -f "$s" ] || { echo "MISSING SVG: $1" >&2; exit 1; }
    cp -- "$s" "$OUT/$1.svg"
    echo "  $1.svg  <-  $1.svg (verbatim)"
}

echo "Importing NAM art from: $NAM_UPSTREAM"

echo "Raster layers (@2x preferred, -> PNG32):"
raster Background
raster Lines
raster KnobBackground
raster FileBackground
raster MeterBackground
raster SlideSwitchHandle
raster InputLevelBackground

echo "Icons (original SVG, rasterised at runtime):"
icon ArrowLeft
icon ArrowRight
icon Cross
icon File
icon Gear
icon Globe
icon IRIconOff
icon IRIconOn
icon ModelIcon
icon SlimmableIcon

echo "Done -> $OUT"
