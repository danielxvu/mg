// bridge.cpp -- C++ implementation of the mg.io extern "C" bridge (task C3.5).
//
// A regular TU (not a module) that imports mg.io and exposes the plain-C API in
// bridge.h to the legacy core. Errors collapse to a single sentinel (-1): the
// caller (fisdir) only needs the dir / not-dir / error trichotomy.

#include "bridge.h"

import mg.io;

extern "C" int mg_io_isdir(const char *path)
{
    if (path == nullptr)
        return -1;
    auto st = mg::io::stat_file(path);
    if (!st)
        return -1;               // stat failed (ENOENT, EACCES, ...)
    return st->is_dir ? 1 : 0;
}
