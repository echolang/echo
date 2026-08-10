#ifndef SETTLEDPATH_H
#define SETTLEDPATH_H

#pragma once

#include <filesystem>
#include <string>

namespace Compiler
{
    // **the sole answer to "what does a written path settle to".**
    //
    // canonical where the filesystem can answer, absolute where it cannot - a path naming something that
    // does not exist yet is still a path, and refusing to settle one would make every "is this a directory"
    // check below report the wrong reason. Canonical rather than merely absolute because two manifests may
    // reach one directory by different routes, and a requirement that compares unequal to itself defeats
    // every dedup downstream
    std::filesystem::path canonical_or_absolute(const std::filesystem::path &path);

    // the same rule for a path written *relative to something* - a manifest's own directory for anything it
    // declares, the working directory for anything the command line did. An absolute spelling ignores the
    // base, which is what lets a manifest name a system location without knowing where it sits itself
    std::filesystem::path settled_path(const std::filesystem::path &base, const std::string &spelled);
};

#endif
