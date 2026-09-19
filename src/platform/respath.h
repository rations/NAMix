// Locating the plug-in's own resources at runtime.
//
// A VST3 bundle keeps its art and fonts in
//   <name>.vst3/Contents/Resources/{img,fonts}
// and the loaded module is
//   <name>.vst3/Contents/x86_64-linux/<name>.so
// so the resource directory is two levels up from the shared object, plus
// "Resources". Unlike an application, a plug-in cannot ask for "its own
// executable" — the host's binary is not ours — so the .so path comes from
// dladdr() on a symbol we own.
//
// Resolution order:
//   0. a directory named outright through setResourceDirOverride(), below;
//   1. $NAMIX_RESOURCE_DIR, if set (development and packaging override);
//   2. the bundle layout above, derived via dladdr;
//   3. an executable-relative "resources" directory, which is what the
//      standalone build uses when run from a build tree.
//
// Returns an empty string if nothing resolves; callers treat that as "no art",
// which every load path already degrades gracefully for.

#pragma once

#include <string>

namespace NAMix
{

// Cached after the first call.
const std::string &resourceDir();

// Name the resource directory outright, ahead of every rule above.
//
// It exists for ONE caller: the LV2 build, whose bundle is not a VST3 bundle.
// The layout rule above walks two directories up from the loaded module and
// adds "Resources", which is where a .vst3 keeps its art; an LV2 bundle keeps
// its binaries and its art side by side in one directory and hands that
// directory to the UI as instantiate()'s bundle_path. There is nothing to
// derive there, so the answer is given rather than looked for.
//
// MUST be called before the first resourceDir(), because that answer is cached
// for the life of the process. The LV2 UI calls it in instantiate(), before the
// editor is created and therefore before anything loads a resource. A call that
// arrives late warns and changes nothing rather than silently disagreeing with
// art that is already loaded.
void setResourceDirOverride(const std::string &dir);

} // namespace NAMix
