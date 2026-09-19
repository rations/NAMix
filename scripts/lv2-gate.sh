#!/usr/bin/env bash
# lv2-gate.sh — the LV2 bundle's gate: what a host sees, and whether it runs.
#
# The LV2 build is the SAME processor, controller and editor the VST3 bundle carries, wrapped so an
# LV2 host can drive them (src/lv2/namixlv2.h says why it is a wrapper and not a second plug-in).
# So the DSP needs no second proof — the VST3 validator is a statement about code this build
# shares. What DOES need proving is everything between the host and that code, and all of it fails
# silently:
#
#   * the bundle's Turtle has to parse, and its manifest has to OFFER the UI. A UI declared only in
#     the plug-in's own file is invisible to most hosts, and nothing about the build says so.
#   * every output port the editor reads has to carry a ui:portNotification. Without them both
#     meters are permanently dead while the plug-in loads, plays and otherwise works perfectly.
#   * the two shared objects have to export their one entry point and nothing else. Two plug-ins in
#     one host process that both export the same symbol can be merged through STB_GNU_UNIQUE,
#     leaving one running the other's code — and this tree ships the VST3 and the LV2 of the same
#     plug-in, so a host with both installed is exactly that path.
#   * the whole editor-to-DSP round trip has to join up: a model load goes in as an atom, the
#     message thread reads the file, and the capability report comes back out of the notify port.
#     Every one of those seams is new in this format.
#
# Three parts, in the order a failure is cheapest to read:
#
#   1. shape           the exports, measured with nm, the linkage, and the bundle's contents.
#   2. namix_lv2check  what lilv — the reference discovery library, and what real hosts use — sees
#                      in the INSTALLED bundle, plus loading it, running it, and round-tripping its
#                      state. With --model it also loads a real capture and asserts the amp makes a
#                      finite, non-silent sound with its meters moving.
#   3. sord_validate   the RDF schema check, when the tool is available. It is not packaged on
#                      Debian/Devuan; build it from the lv2 source tree's sord with meson and put
#                      it on PATH, or point $NAMIX_SORD_VALIDATE at it.
#
# It needs no JACK server and no DAW. It installs the bundle to ~/.lv2 first, because a gate that
# read the BUILD directory would not catch a bundle that is wrong once installed — and because a
# stale copy under /usr/lib/lv2 shadows ~/.lv2 and would have you debugging the wrong binary.
#
# Usage:
#   scripts/lv2-gate.sh                                  # everything that needs no model
#   scripts/lv2-gate.sh --model ~/captures/amp.nam       # ... plus the end-to-end load and sound
#   scripts/lv2-gate.sh --no-install                     # gate whatever is already in ~/.lv2

set -u

repo=$(cd "$(dirname "$0")/.." && pwd)
build="${NAMIX_BUILD_DIR:-$repo/build}"
bundle="$build/lv2/NAMix.lv2"
installed="$HOME/.lv2/NAMix.lv2"

model="${NAMIX_TEST_MODEL:-}"
ir="${NAMIX_TEST_IR:-}"
do_install=1

while [ "$#" -gt 0 ]; do
    case "$1" in
        --model) model="$2"; shift 2 ;;
        --ir)    ir="$2";    shift 2 ;;
        --no-install) do_install=0; shift ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "lv2-gate: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

failures=0
pass() { printf '  ok   %s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; failures=$((failures + 1)); }
note() { printf '  --   %s\n' "$*"; }
section() { printf '\n%s\n' "$*"; }

if [ ! -d "$bundle" ]; then
    echo "lv2-gate: $bundle does not exist." >&2
    echo "Build it first: cmake --build $build   (needs the LV2 development files, lv2-dev)" >&2
    exit 1
fi

#---------------------------------------------------------------------------
section "1. shape - what the two binaries expose, and what the bundle contains"

# Only strong global text symbols (nm's "T"). An ELF shared object legitimately keeps a handful of
# WEAK C++ template instantiations visible whatever -fvisibility=hidden and --exclude-libs,ALL do,
# so an "exactly these" rule over every symbol fails on an innocent std::vector.
exports_exactly() {
    local elf="$1" want="$2" got
    got=$(nm -D --defined-only "$elf" | awk '$2 == "T" { print $3 }' | sort | tr '\n' ' ')
    got=$(echo $got)
    if [ "$got" = "$want" ]; then
        pass "$(basename "$elf") exports exactly [$want]"
    else
        fail "$(basename "$elf") exports [$got] rather than exactly [$want]"
    fi
}

exports_exactly "$bundle/NAMix.so"    lv2_descriptor
exports_exactly "$bundle/NAMix_ui.so" lv2ui_descriptor

for so in NAMix.so NAMix_ui.so; do
    n=$(nm -D "$bundle/$so" | grep -c ' u ' || true)
    if [ "$n" -eq 0 ]; then
        pass "$so exports no STB_GNU_UNIQUE symbol"
    else
        fail "$so exports $n STB_GNU_UNIQUE symbol(s); a host with the VST3 loaded too can merge them"
    fi
done

# The DSP half draws nothing. If it links X11 or cairo, something that belongs in the editor has
# been pulled into it.
if ldd "$bundle/NAMix.so" | grep -qE 'libX11|libcairo'; then
    fail "NAMix.so links X11 or cairo; the DSP half draws nothing and should need neither"
else
    pass "NAMix.so links neither X11 nor cairo"
fi
if ldd "$bundle/NAMix.so" | grep -q 'libjack'; then
    fail "NAMix.so links JACK; nothing in the plug-in may"
else
    pass "NAMix.so does not link JACK"
fi

missing=0
for f in manifest.ttl NAMix.ttl NAMix.so NAMix_ui.so \
         fonts/Michroma-Regular.ttf fonts/Roboto-Regular.ttf \
         img/Background.png img/KnobBackground.png img/File.svg img/ModelIcon.svg; do
    [ -e "$bundle/$f" ] || { fail "the bundle is missing $f"; missing=1; }
done
[ "$missing" -eq 0 ] && pass "the bundle carries its Turtle, both binaries, both fonts and its art"

#---------------------------------------------------------------------------
section "2. namix_lv2check - the INSTALLED bundle, through lilv"

if [ "$do_install" -eq 1 ]; then
    mkdir -p "$HOME/.lv2"
    rm -rf "$installed"
    cp -r "$bundle" "$installed" || { echo "lv2-gate: could not install to $installed" >&2; exit 1; }
    note "installed to $installed"
fi
if [ ! -d "$installed" ]; then
    fail "$installed does not exist and --no-install was given"
else
    for shadow in /usr/local/lib/lv2/NAMix.lv2 /usr/lib/lv2/NAMix.lv2; do
        [ -e "$shadow" ] && note "WARNING: $shadow exists and will shadow ~/.lv2 in most hosts"
    done

    check_bin="$build/namix_lv2check"
    if [ ! -x "$check_bin" ]; then
        fail "namix_lv2check was not built - install the lilv development files (liblilv-dev)"
    else
        args=("$installed")
        [ -n "$model" ] && args+=(--model "$model")
        [ -n "$ir" ] && args+=(--ir "$ir")
        [ -z "$model" ] && note "no --model given; the load path and the sound are not exercised"
        if "$check_bin" "${args[@]}"; then
            pass "namix_lv2check"
        else
            fail "namix_lv2check reported problems (above)"
        fi
    fi
fi

#---------------------------------------------------------------------------
section "3. sord_validate - the RDF schema check"

sordv="${NAMIX_SORD_VALIDATE:-$(command -v sord_validate 2>/dev/null || true)}"
if [ -z "$sordv" ]; then
    note "sord_validate is not on PATH; skipped. It is not packaged on Debian/Devuan - build it"
    note "from the lv2 source tree's sord with meson, or set \$NAMIX_SORD_VALIDATE."
else
    specs=$(ls -d /usr/lib/lv2/*.lv2 2>/dev/null | tr '\n' ' ')
    if "$sordv" $specs "$installed"/*.ttl; then
        pass "sord_validate"
    else
        fail "sord_validate reported schema problems (above)"
    fi
fi

#---------------------------------------------------------------------------
printf '\n'
if [ "$failures" -eq 0 ]; then
    echo "lv2-gate: everything checked passed."
    exit 0
fi
echo "lv2-gate: $failures check(s) failed."
exit 1
