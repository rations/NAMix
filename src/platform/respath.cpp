// Resource-directory resolution. See respath.h.

#include "respath.h"

#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace NAMix
{

namespace
{

bool isDir(const std::string &path)
{
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string parentOf(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
        return std::string();
    return path.substr(0, slash);
}

// Address of a symbol in THIS shared object, for dladdr to resolve back to a
// file name. Taking the address of a local function is enough.
void marker()
{
}

// <bundle>/Contents/x86_64-linux/NAMix.so -> <bundle>/Contents/Resources
std::string fromModulePath()
{
    Dl_info info;
    if (dladdr(reinterpret_cast<void *>(&marker), &info) == 0 || !info.dli_fname)
        return std::string();

    const std::string archDir = parentOf(info.dli_fname); // Contents/x86_64-linux
    const std::string contents = parentOf(archDir);       // Contents
    if (contents.empty())
        return std::string();

    const std::string res = contents + "/Resources";
    return isDir(res) ? res : std::string();
}

// For the standalone: <dir of executable>/resources, and one level up, so it
// works both from a build tree and from an installed prefix.
std::string fromExecutablePath()
{
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return std::string();
    buf[n] = '\0';

    const std::string binDir = parentOf(buf);
    if (binDir.empty())
        return std::string();

    const std::string candidates[] = {binDir + "/resources", parentOf(binDir) + "/resources"};
    for (const std::string &c : candidates)
        if (isDir(c))
            return c;
    return std::string();
}

// Set by setResourceDirOverride(), which is how a bundle format that is not a
// VST3 bundle names its own resources. Read once, by resolve(), and never
// written after that read — see the header.
std::string gOverride;
bool gResolved = false;

std::string resolve()
{
    gResolved = true;
    if (!gOverride.empty()) {
        if (isDir(gOverride))
            return gOverride;
        fprintf(stderr, "NAMix: the resource directory %s does not exist (ignored)\n",
                gOverride.c_str());
    }

    if (const char *env = std::getenv("NAMIX_RESOURCE_DIR")) {
        if (isDir(env))
            return std::string(env);
        fprintf(stderr, "NAMix: NAMIX_RESOURCE_DIR=%s is not a directory (ignored)\n", env);
    }

    std::string dir = fromModulePath();
    if (!dir.empty())
        return dir;

    dir = fromExecutablePath();
    if (!dir.empty())
        return dir;

    fprintf(stderr, "NAMix: could not locate the resource directory; "
                    "art and fonts will fall back\n");
    return std::string();
}

} // namespace

//------------------------------------------------------------------------
void setResourceDirOverride(const std::string &dir)
{
    if (gResolved) {
        // Not fatal, and deliberately not silent: the answer is already cached
        // and this call cannot change it, so a caller that reached here has an
        // ordering problem rather than a missing directory, and that is worth
        // being told once.
        fprintf(stderr,
                "NAMix: setResourceDirOverride(%s) came after the resource directory "
                "was already resolved; ignored\n",
                dir.c_str());
        return;
    }
    gOverride = dir;
}

//------------------------------------------------------------------------
const std::string &resourceDir()
{
    static const std::string dir = resolve();
    return dir;
}

} // namespace NAMix
