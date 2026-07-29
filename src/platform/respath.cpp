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

std::string resolve()
{
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
const std::string &resourceDir()
{
    static const std::string dir = resolve();
    return dir;
}

} // namespace NAMix
