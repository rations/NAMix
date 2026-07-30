# Master geometry and palette for the NAMix editor art. Sourced by
# import_assets.sh; the same numbers are mirrored as constants in
# src/namgeometry.h (keep the two in sync). All values are FINAL (1x) pixels —
# the canvas is drawn 1:1.
#
# Layout mirrors the original Neural Amp Modeler amp-head panel (600x400):
# a row of six knobs under the title, two toggle switches, a model row and an
# IR row near the bottom, and input/output level meters on the outer edges.
# The raster layers are the plugin's own MIT-licensed art; the ten icons ship
# as the original SVGs and are rasterised at runtime. Knob pointers/arcs, meter
# fills and switch handles are drawn in code, exactly as the original does.

# Editor canvas.
WIN_W=600
WIN_H=400

# Upstream art source (MIT, Steven Atkinson). Override with NAM_UPSTREAM=... .
: "${NAM_UPSTREAM:=/home/human/third_party/NeuralAmpModelerPlugin/NeuralAmpModeler/resources/img}"

# Palette (from the original Colors.h).
BG_COLOR="#1D1A1F"    # raisin-black backdrop
AZURE="#5085E8"       # theme accent (knob pointer, meter fill, switch-on)
TEXT_COLOR="#F2F2F2"  # near-white labels
DIM_COLOR="#A2B2BF"   # cadet blue (empty / disabled text)
PEAK_COLOR="#FF3B30"  # meter peak marker
