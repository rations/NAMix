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

} // namespace NAMix
