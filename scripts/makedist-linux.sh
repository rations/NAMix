#!/usr/bin/env bash
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cmake -B "$REPO/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -S "$REPO"
cmake --build "$REPO/build" --parallel "$(nproc)"

VERSION="$(grep -oP '(?<=^project\(smtg-namix$)|(?<=^\s{4}VERSION )\S+' "$REPO/CMakeLists.txt" | head -1)"
if [ -z "$VERSION" ]; then
  echo "could not read the project version from CMakeLists.txt" >&2
  exit 1
fi
ARCH="$(uname -m)"
STAGEDIR="$(mktemp -d)"
PKGDIR="$STAGEDIR/NAMix-${VERSION}"
mkdir -p "$PKGDIR"
trap 'rm -rf "$STAGEDIR"' EXIT

# VST3 bundle — strip the shared library inside the bundle.
BUNDLE="$REPO/build/VST3/Release/NAMix.vst3"
if [ ! -d "$BUNDLE" ]; then
  echo "VST3 bundle not found at $BUNDLE" >&2
  exit 1
fi
cp -r "$BUNDLE" "$PKGDIR/"
find "$PKGDIR/NAMix.vst3" -name "*.so" -exec strip --strip-unneeded {} \;

# LV2 bundle. A shipped component like the standalone below, so a build that
# skipped it (LV2 development files absent) must not silently produce a half
# release.
LV2BUNDLE="$REPO/build/lv2/NAMix.lv2"
if [ ! -d "$LV2BUNDLE" ]; then
  echo "NAMix.lv2 was not built - install the LV2 development files (lv2-dev)" >&2
  echo "and re-run, or the release would ship the VST3 only." >&2
  exit 1
fi
cp -r "$LV2BUNDLE" "$PKGDIR/"
PKGLV2="$PKGDIR/NAMix.lv2"
strip --strip-unneeded "$PKGLV2/NAMix.so" "$PKGLV2/NAMix_ui.so"

# What a host will actually see, checked on the STAGED copy rather than on the
# build tree, because stripping is the last thing that touches these binaries.
#
# Only strong global text symbols (nm's "T"): an ELF shared object legitimately
# keeps a few WEAK C++ template instantiations visible whatever -fvisibility and
# --exclude-libs do, so an "exactly these" rule over every symbol would fail on
# an innocent std::vector. scripts/lv2-gate.sh explains what each of these is
# for and checks the same things before install.
lv2_exports_exactly() {
  local elf="$1" want="$2" got
  got="$(nm -D --defined-only "$elf" | awk '$2 == "T" { print $3 }' | sort | tr '\n' ' ')"
  got="$(echo $got)"
  if [ "$got" != "$want" ]; then
    echo "$(basename "$elf") exports [$got] rather than exactly [$want]." >&2
    echo "Two plug-ins in one host process that export the same symbol can be merged" >&2
    echo "through STB_GNU_UNIQUE, and this archive ships the VST3 and the LV2 of the" >&2
    echo "same plug-in." >&2
    exit 1
  fi
}
lv2_exports_exactly "$PKGLV2/NAMix.so"    lv2_descriptor
lv2_exports_exactly "$PKGLV2/NAMix_ui.so" lv2ui_descriptor

for SO in "$PKGLV2/NAMix.so" "$PKGLV2/NAMix_ui.so"; do
  if [ "$(nm -D "$SO" | grep -c ' u ' || true)" -ne 0 ]; then
    echo "$(basename "$SO") exports an STB_GNU_UNIQUE symbol." >&2
    exit 1
  fi
done

# The DSP half draws nothing, and nothing in this archive may link JACK except
# the standalone.
if ldd "$PKGLV2/NAMix.so" | grep -qE 'libX11|libcairo|libjack'; then
  echo "NAMix.so links X11, cairo or JACK; the DSP half needs none of them." >&2
  exit 1
fi

for F in manifest.ttl NAMix.ttl fonts/Michroma-Regular.ttf fonts/Roboto-Regular.ttf \
         img/Background.png img/KnobBackground.png img/File.svg; do
  if [ ! -e "$PKGLV2/$F" ]; then
    echo "the staged NAMix.lv2 is missing $F" >&2
    exit 1
  fi
done

# Standalone binary. This is a shipped component, so a build that skipped it
# (JACK development files absent) must not silently produce a half release.
STANDALONE="$REPO/build/namix-standalone"
if [ ! -f "$STANDALONE" ]; then
  echo "namix-standalone was not built - install the JACK development files" >&2
  echo "(libjack-jackd2-dev) and re-run, or the release would ship the plug-ins only." >&2
  exit 1
fi
cp "$STANDALONE" "$PKGDIR/"
strip --strip-unneeded "$PKGDIR/namix-standalone"

# Licence and attribution.
cp "$REPO/NOTICE" "$REPO/LICENSE" "$REPO/README.md" "$PKGDIR/"

cat > "$PKGDIR/INSTALL.txt" <<EOF
NAMix ${VERSION} - Neural Amp Modeler for Linux (VST3 + LV2 + JACK standalone)

The VST3 and the LV2 are the same plug-in twice - the same DSP, the same panel
and the same state format - so a project saved with one opens with the other.
Install whichever your host prefers, or both.

VST3 plug-in
------------
Install for the current user:

    mkdir -p ~/.vst3
    cp -r NAMix.vst3 ~/.vst3/

or system-wide, for every user:

    sudo cp -r NAMix.vst3 /usr/lib/vst3/

Then rescan plug-ins in your DAW. The bundle carries its own icons and fonts,
so nothing else needs installing.

LV2 plug-in
-----------
Install for the current user:

    mkdir -p ~/.lv2
    cp -r NAMix.lv2 ~/.lv2/

or system-wide, for every user:

    sudo cp -r NAMix.lv2 /usr/lib/lv2/

The bundle carries its own icons and fonts here too.

If your host keeps showing an older version, look for a copy that is shadowing
this one - a bundle under /usr/lib/lv2 or /usr/local/lib/lv2 takes precedence
over ~/.lv2 in most hosts:

    ls -d /usr/lib/lv2/NAMix.lv2 /usr/local/lib/lv2/NAMix.lv2 2>/dev/null

Standalone
----------
./namix-standalone is a JACK application. Start a JACK server first (qjackctl,
or e.g. "jackd -R -d alsa -r 48000 -p 256"), then run:

    ./namix-standalone

It is a host, not a second copy of the plug-in: it loads NAMix.vst3 and looks
for it in \$NAMIX_VST3, then beside this binary, then ~/.vst3, then
/usr/local/lib/vst3 and /usr/lib/vst3. Running it from this directory works as
extracted. You can also pass the bundle explicitly:

    ./namix-standalone /path/to/NAMix.vst3

It registers the ports NAMix:in, NAMix:out_l and NAMix:out_r and connects them
to the first physical capture and playback ports it finds.

Requirements
------------
The VST3 and LV2 plug-ins need only cairo, freetype2, fontconfig and libX11,
which a desktop Linux install already has. Neither links JACK.

The standalone additionally needs the JACK client library (libjack.so.0) and a
running JACK server. On Debian/Devuan/Ubuntu:

    sudo apt install jackd2          # pulls in libjack-jackd2-0

On a PipeWire desktop, "pipewire-jack" provides the same library and server.
Either way you almost certainly have this already if you run JACK at all --
libjack.so.0 ships with the server, not separately.

Nothing here needs a -dev package; those are only for building from source.

Licence
-------
MIT. See LICENSE, and NOTICE for third-party attribution.
EOF

mkdir -p "$REPO/dist"
TARBALL="$REPO/dist/NAMix-${VERSION}-linux-${ARCH}.tar.gz"
tar -czf "$TARBALL" -C "$STAGEDIR" "NAMix-${VERSION}"

echo "Packaged: $TARBALL"
echo ""
echo "Contents:"
tar -tzf "$TARBALL"
