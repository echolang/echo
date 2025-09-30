#ifndef MODULECACHE_H
#define MODULECACHE_H

#pragma once

#include "Compiler/CompilerOptions.h"
#include "Parser/ManifestParser.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Compiler
{
    // FNV-1a, 64 bit. Not cryptographic and does not need to be: this answers "are these the same inputs as
    // last time", where the adversary is an edited source file rather than a person
    uint64_t fnv1a64(const void *data, size_t length, uint64_t seed);
    uint64_t fnv1a64(const std::string &text, uint64_t seed);

    // what a module's compiled object is a function of. Two builds that agree on this may share an artifact;
    // two that do not, must not.
    //
    // **the inputs are kept, not just the digest.** A cache miss is otherwise unexplainable - "something
    // changed" is not an answer a person can act on - and `--explain-cache` reads them back to name the file
    struct ModuleCacheKey
    {
        std::string hex;
        std::vector<std::pair<std::filesystem::path, uint64_t>> inputs;
    };

    // a key per module, by module name. `manifests` must be in dependency order, which is what
    // Parser::resolve_module_graph returns.
    //
    // a dependency contributes its whole *key* rather than its sources, which is what makes invalidation
    // transitive for free: a key already folds in its own dependencies, so editing a leaf reaches everything
    // above it without a second walk.
    //
    // conservative on purpose. With no visibility modifiers a module's entire source is its interface, so
    // any edit to a dependency rebuilds its dependents - even one to a function body nothing else can name
    bool compute_module_keys(
        const std::vector<Parser::ModuleManifest> &manifests,
        const CompilerOptions &options,
        bool optimize,
        std::map<std::string, ModuleCacheKey> &out_keys,
        std::string &out_error);

    // where a module's artifacts live. `cache_dir_override` empty means the default, `<manifest dir>/.echo`,
    // which keeps a module's cache beside the code it belongs to - deleting a checkout deletes its cache and
    // no two projects can be confused for one another
    std::filesystem::path module_cache_dir(
        const Parser::ModuleManifest &manifest, const std::filesystem::path &cache_dir_override);

    // the object for a key. The key is *in the filename* rather than only in a sidecar, so two build modes
    // coexist instead of overwriting each other
    std::filesystem::path module_object_path(
        const Parser::ModuleManifest &manifest,
        const ModuleCacheKey &key,
        const std::filesystem::path &cache_dir_override);

    // can artifacts actually be written here? Creates the directory as a side effect, since a store that does
    // not exist yet and one that cannot be created are the same answer to the caller.
    //
    // **a cache that cannot be written must not fail a build.** It is an optimization, so the only correct
    // response to an unwritable store is to compile the module and not keep the result
    bool cache_dir_is_writable(const std::filesystem::path &directory);

    // the record of what the last build of this module was made of, for explaining a miss. One per module
    // rather than one per key, because the interesting comparison is against whatever was there before
    std::filesystem::path module_inputs_path(
        const Parser::ModuleManifest &manifest, const std::filesystem::path &cache_dir_override);

    // writes the sidecar beside a freshly emitted object
    bool write_inputs_record(const std::filesystem::path &path, const ModuleCacheKey &key);

    // the first input whose hash differs from the recorded one, as a sentence, or "" when the record is
    // missing or agrees. Best-effort: this is a diagnostic, and being unable to explain a miss is not itself
    // an error
    std::string explain_miss(const std::filesystem::path &inputs_path, const ModuleCacheKey &key);
};

#endif
