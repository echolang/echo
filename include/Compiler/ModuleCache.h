#ifndef MODULECACHE_H
#define MODULECACHE_H

#pragma once

#include "Compiler/BuildLayout.h"
#include "Compiler/CompilerOptions.h"
#include "Compiler/Fnv.h"
#include "Compiler/TargetFacts.h"
#include "Parser/ManifestParser.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Compiler
{
    // **which machine an object is being compiled for**, folded into `seed`.
    //
    // the triple and the CPU inside it, which the triple does not say. Shared by the two artifact stores
    // rather than spelled in each, because *that* question has one answer even though the stores are
    // separate and keyed on unrelated things besides: a target axis added here reaches both, where two
    // hand-written folds drift into one store serving an object built for another subtarget - the unsound
    // case rather than the merely ineffective one.
    //
    // false with a sentence in `out_error` when the requested CPU or features are not this target's, which
    // is a refusal both callers owe a person rather than a difference either may quietly ignore
    // **one in/out digest, not a seed and an answer.** Both callers fold it into a running digest, and a
    // pair of parameters both spelled with the same variable is a signature saying they might differ
    bool fold_target_environment(
        const CompilerOptions &options, uint64_t &digest, std::string &out_error);

    // what a module's compiled object is a function of. Two builds that agree on this may share an artifact;
    // two that do not, must not.
    //
    // **the inputs are kept, not just the digest.** A cache miss is otherwise unexplainable - "something
    // changed" is not an answer a person can act on - and `--explain-cache` reads them back to name the file
    struct ModuleCacheKey
    {
        std::string hex;
        // this module's source+config digest *before* embed file contents. the sidecar is named
        // with it, so two facts (debug/release, `--define`, tests) keep separate lists
        std::string embed_identity;
        std::vector<std::pair<std::filesystem::path, uint64_t>> inputs;
    };

    // a key per module, by name. `manifests` in dependency order. a dependency contributes its key,
    // so a leaf edit invalidates everything above it. `modules_with_tests` is per module (only the
    // roots this invocation pointed at). `active_targets` is read through module_contribution_for
    bool compute_module_keys(
        const std::vector<const Parser::ModuleManifest *> &manifests,
        const CompilerOptions &options,
        const TargetFacts &facts,
        const std::set<std::string> &modules_with_tests,
        const Parser::ActiveTargets &active_targets,
        bool optimize,
        const BuildLayout &layout,
        std::map<std::string, ModuleCacheKey> &out_keys,
        std::string &out_error);

    // the object for a key. **only the filename is this store's** - which directory it goes in is
    // Compiler::BuildLayout's one question - because the key is what invents the name and nothing else
    // knows how to spell one. The key is *in the filename* rather than only in a sidecar, so two build
    // modes coexist instead of overwriting each other
    std::filesystem::path module_object_path(
        const Parser::ModuleManifest &manifest, const ModuleCacheKey &key, const BuildLayout &layout);

    // an object this user's echoc wrote and sealed, and nothing else. see Compiler::artifact_is_sealed
    bool cached_object_is_hit(const std::filesystem::path &object);

    // what this link was: the argv plus each object's path, once it is on disk.
    // filenames already carry the content hex (module cache key, partition hash).
    // a missing object is a miss
    std::optional<std::string> link_inputs_digest(
        const std::vector<std::string> &command,
        const std::vector<std::filesystem::path> &objects);

    // the record of what the last build of this module was made of, for explaining a miss. One per module
    // rather than one per key, because the interesting comparison is against whatever was there before
    std::filesystem::path module_inputs_path(
        const Parser::ModuleManifest &manifest, const BuildLayout &layout);

    // writes the sidecar beside a freshly emitted object
    bool write_inputs_record(const std::filesystem::path &path, const ModuleCacheKey &key);

    // files a previous compile of this module, under this source+config identity, embedded.
    // missing is "none", which is what keeps every key that never embedded byte for byte
    // what it was. **named with `embed_identity`**, not the module name alone: a later compile
    // that embedded nothing must not truncate the list another configuration still needs
    std::filesystem::path module_embed_inputs_path(
        const std::filesystem::path &module_dir,
        const std::string &module_name,
        const std::string &embed_identity);

    bool write_embed_inputs_record(
        const std::filesystem::path &path,
        const std::vector<std::filesystem::path> &files);

    // the first input whose hash differs from the recorded one, as a sentence, or "" when the record is
    // missing or agrees. Best-effort: this is a diagnostic, and being unable to explain a miss is not itself
    // an error
    std::string explain_miss(const std::filesystem::path &inputs_path, const ModuleCacheKey &key);
};

#endif
