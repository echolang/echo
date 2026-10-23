#ifndef ARTIFACTSEAL_H
#define ARTIFACTSEAL_H

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Compiler
{
    // **is this artifact one this user's echoc wrote.**
    // a cache name is computable from the sources, so a shipped object would be linked as-is.
    // sealed on write: keyed BLAKE3 of name, context and bytes, under a key in user_cache_root().
    // no usable key: a per-process one, so nothing earlier is a hit

    // the seal of `artifact` under `context`, as hex, or nothing when the file cannot be read
    std::optional<std::string> artifact_seal(const std::filesystem::path &artifact, std::string_view context = {});

    // writes `<artifact>.seal`. false when it could not: the artifact is then not reusable
    bool seal_artifact(const std::filesystem::path &artifact, std::string_view context = {});

    // does `<artifact>.seal` vouch for the artifact as it is now. **memoized per process**
    bool artifact_is_sealed(const std::filesystem::path &artifact, std::string_view context = {});

    // where an artifact's seal lives
    std::filesystem::path seal_path_of(const std::filesystem::path &artifact);
};

#endif
