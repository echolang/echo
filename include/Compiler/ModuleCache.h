#ifndef MODULECACHE_H
#define MODULECACHE_H

#pragma once

#include "Compiler/BuildLayout.h"
#include "Compiler/CompilerOptions.h"
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
    // FNV-1a, 64 bit. Not cryptographic and does not need to be: this answers "are these the same inputs as
    // last time", where the adversary is an edited source file rather than a person
    uint64_t fnv1a64(const void *data, size_t length, uint64_t seed);
    uint64_t fnv1a64(const std::string &text, uint64_t seed);

    // what a fold starts from. Here rather than private to this file because the C object cache folds its
    // own inputs with the same function, and a second store starting from a hand-copied magic number is a
    // second place for the algorithm to be subtly not the same one
    constexpr uint64_t k_fnv_offset_basis = 1469598103934665603ULL;

    // a digest as it is written into a filename or a sidecar. One spelling, because a key compared against
    // one formatted a hair differently is a permanent miss that looks exactly like a working cache
    std::string to_hex(uint64_t value);

    // the whole file, or nullopt. **the content, never the timestamp**: a manifest's file list is a glob, so
    // a checkout, a branch switch or a `touch` all move mtimes without changing what is compiled - and the
    // reverse matters more, since two files can share an mtime and differ.
    //
    // shared with the C object cache, which hashes a source and every header it reached. Sized up front and
    // read straight into the string; the ostringstream spelling this replaced copied the bytes a second time
    // before the hash ever saw them, once per file per build
    std::optional<std::string> read_whole_file(const std::filesystem::path &path);

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
    //
    // `modules_with_tests` names the modules that compiled their `test` blocks, which is the one input here
    // that is genuinely **per module** rather than per build: an invocation compiles the tests of what it
    // pointed at and not of the libraries below it. Passed rather than derived because that decision belongs
    // to the driver, and folded through TargetFacts::cache_signature so there is still one answer to "what
    // could the conditional filter see"
    bool compute_module_keys(
        const std::vector<Parser::ModuleManifest> &manifests,
        const CompilerOptions &options,
        const TargetFacts &facts,
        const std::set<std::string> &modules_with_tests,
        bool optimize,
        std::map<std::string, ModuleCacheKey> &out_keys,
        std::string &out_error);

    // the object for a key. **only the filename is this store's** - which directory it goes in is
    // Compiler::BuildLayout's one question - because the key is what invents the name and nothing else
    // knows how to spell one. The key is *in the filename* rather than only in a sidecar, so two build
    // modes coexist instead of overwriting each other
    std::filesystem::path module_object_path(
        const Parser::ModuleManifest &manifest, const ModuleCacheKey &key, const BuildLayout &layout);

    // the record of what the last build of this module was made of, for explaining a miss. One per module
    // rather than one per key, because the interesting comparison is against whatever was there before
    std::filesystem::path module_inputs_path(
        const Parser::ModuleManifest &manifest, const BuildLayout &layout);

    // writes the sidecar beside a freshly emitted object
    bool write_inputs_record(const std::filesystem::path &path, const ModuleCacheKey &key);

    // the first input whose hash differs from the recorded one, as a sentence, or "" when the record is
    // missing or agrees. Best-effort: this is a diagnostic, and being unable to explain a miss is not itself
    // an error
    std::string explain_miss(const std::filesystem::path &inputs_path, const ModuleCacheKey &key);
};

#endif
