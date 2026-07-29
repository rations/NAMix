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

# Standalone binary. This is a shipped component, so a build that skipped it
# (JACK development files absent) must not silently produce a half release.
STANDALONE="$REPO/build/namix-standalone"
if [ ! -f "$STANDALONE" ]; then
  echo "namix-standalone was not built - install the JACK development files" >&2
  echo "(libjack-jackd2-dev) and re-run, or the release would ship VST3 only." >&2
  exit 1
fi
cp "$STANDALONE" "$PKGDIR/"
strip --strip-unneeded "$PKGDIR/namix-standalone"

# Licence and attribution.
cp "$REPO/NOTICE" "$REPO/LICENSE" "$REPO/README.md" "$PKGDIR/"

cat > "$PKGDIR/INSTALL.txt" <<EOF
NAMix ${VERSION} - Neural Amp Modeler for Linux (VST3 + JACK standalone)

VST3 plug-in
------------
Install for the current user:

    mkdir -p ~/.vst3
    cp -r NAMix.vst3 ~/.vst3/

or system-wide, for every user:

    sudo cp -r NAMix.vst3 /usr/lib/vst3/

Then rescan plug-ins in your DAW. The bundle carries its own icons and fonts,
so nothing else needs installing.

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
The VST3 plug-in needs only cairo, freetype2, fontconfig and libX11, which a
desktop Linux install already has. It does NOT link JACK.

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
