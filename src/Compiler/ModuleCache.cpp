#include "Compiler/ModuleCache.h"

#include "Compiler/TargetSubtarget.h"
#include "eco.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/TargetParser/Host.h>

#include <fmt/core.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>

namespace
{

constexpr uint64_t k_fnv_offset_basis = 1469598103934665603ULL;
constexpr uint64_t k_fnv_prime = 1099511628211ULL;

std::string to_hex(uint64_t value)
{
    return fmt::format("{:016x}", value);
}

// the whole file, or nullopt. **the content, never the timestamp**: a manifest's file list is a glob, so a
// checkout, a branch switch or a `touch` all move mtimes without changing what is compiled - and the reverse
// matters more, since two files can share an mtime and differ
std::optional<std::string> read_whole_file(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }

    // sized up front and read straight into the string. Streaming through an ostringstream instead copied
    // the bytes a second time before the hash ever saw them, once per source per build
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return std::nullopt;
    }

    std::string bytes(static_cast<size_t>(size), '\0');

    in.seekg(0);
    in.read(bytes.data(), size);

    if (in.bad()) {
        return std::nullopt;
    }

    bytes.resize(static_cast<size_t>(in.gcount()));

    return bytes;
}

};

uint64_t Compiler::fnv1a64(const void *data, size_t length, uint64_t seed)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    uint64_t hash = seed;

    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= k_fnv_prime;
    }

    return hash;
}

uint64_t Compiler::fnv1a64(const std::string &text, uint64_t seed)
{
    return fnv1a64(text.data(), text.size(), seed);
}

bool Compiler::compute_module_keys(
    const std::vector<Parser::ModuleManifest> &manifests,
    const CompilerOptions &options,
    const TargetFacts &facts,
    bool optimize,
    std::map<std::string, ModuleCacheKey> &out_keys,
    std::string &out_error)
{
    out_keys.clear();

    // everything about *this compiler* and *this target* that changes what an object contains. Fold once and
    // reuse: it is the same for every module in the build
    //
    // the build mode is here because assertions_enabled() decides whether a body carries its asserts at all,
    // and `-O` because a cached release object must never be folded against a fresh debug one. `-O` bypasses
    // the cache today, so it cannot collide - but that is the caller's behaviour, not this key's, and a key
    // that depends on a caller staying the way it is has the wrong inputs
    //
    // allocation tracking is a third one, and it is *not* covered by the build mode: it decides whether a
    // body's allocations go through the counting seam or straight to the allocator, which is a difference in
    // every module that allocates. --explain-memory is deliberately absent - it only changes the entry
    // module, which is never cached, and it already implies this one
    uint64_t environment = k_fnv_offset_basis;
    environment = fnv1a64(std::string(ECO_MODULE_CACHE_VERSION), environment);
    environment = fnv1a64(std::string(LLVM_VERSION_STRING), environment);
    environment = fnv1a64(llvm::sys::getDefaultTargetTriple(), environment);

    // **and which CPU inside that triple**, which the triple does not say. Two builds differing only in
    // `--target-cpu` produce different instructions in every module that has a loop, so without this
    // one of them is served the other's object - the unsound-cache case rather than the ineffective one.
    // The *resolved* pair and not the request, because the platform baseline changing is the same
    // difference wearing no flag at all
    {
        Subtarget subtarget;
        std::string subtarget_error;

        if (!resolve_subtarget(
                llvm::sys::getDefaultTargetTriple(), options.target_cpu, options.target_features,
                subtarget, subtarget_error)) {
            out_error = subtarget_error;
            return false;
        }

        environment = fnv1a64(subtarget_signature(subtarget), environment);
    }

    environment = fnv1a64(options.assertions_enabled() ? std::string("debug") : std::string("release"), environment);
    environment = fnv1a64(optimize ? std::string("O") : std::string("noO"), environment);

    // **and whether the per-unit pipeline ran**, which is a different question from the one above: `-O`
    // decides what is compiled *together*, `--no-optimize` decides whether the object this key names went
    // through Backend::optimize_unit at all. Two builds differing only in that flag produce different
    // objects, so without this one of them would be served the other's
    environment = fnv1a64(
        options.no_optimize ? std::string("noopt") : std::string("opt"), environment);
    environment = fnv1a64(
        options.tracking_allocations() ? std::string("track") : std::string("notrack"), environment);

    // **and whether the object carries DWARF**, which none of the three above covers. Every module that
    // declares a function emits different bytes with `-g`, so without this entry a debug session is
    // served a stripped artifact for every cached module and its breakpoints simply never resolve -
    // with nothing anywhere saying why, since a stripped object is a perfectly valid one
    environment = fnv1a64(
        options.emitting_debug_info() ? std::string("g") : std::string("nog"), environment);

    // **what the conditional filter saw**, which decides which declarations a module even has. The triple
    // above is not enough on its own: `--target-os` and `--define` change the answer without changing the
    // host, so two builds differing only in a define would otherwise share one object. The signature is
    // TargetFacts' own, so a fact added there reaches this key without an edit here
    environment = fnv1a64(facts.cache_signature(), environment);

    // by canonical manifest path, because that is what `depends` holds
    std::map<std::filesystem::path, uint64_t> digest_by_path;

    // the digest of every module already keyed, in order - see the fold below
    uint64_t preceding = k_fnv_offset_basis;

    for (const Parser::ModuleManifest &manifest : manifests) {
        ModuleCacheKey key;
        uint64_t hash = environment;

        hash = fnv1a64(manifest.name, hash);
        hash = fnv1a64(manifest.version, hash);

        // the manifest's own bytes: its source list, its dependencies and its comments are all part of what
        // the module is. Hashing the expanded file list instead would miss a pattern that stopped matching
        const std::optional<std::string> manifest_bytes = read_whole_file(manifest.path);
        if (!manifest_bytes.has_value()) {
            out_error = fmt::format("{}: cannot be read to compute a cache key.", manifest.path.string());
            return false;
        }

        const uint64_t manifest_digest = fnv1a64(manifest_bytes.value(), k_fnv_offset_basis);
        key.inputs.emplace_back(manifest.path, manifest_digest);
        hash = fnv1a64(&manifest_digest, sizeof(manifest_digest), hash);

        // `sources` is already sorted by the manifest reader, so the order here is stable
        for (const std::filesystem::path &source : manifest.sources) {
            const std::optional<std::string> bytes = read_whole_file(source);
            if (!bytes.has_value()) {
                out_error = fmt::format("{}: cannot be read to compute a cache key.", source.string());
                return false;
            }

            const uint64_t digest = fnv1a64(bytes.value(), k_fnv_offset_basis);
            key.inputs.emplace_back(source, digest);

            // the path as well as the content: renaming a file changes the module even when no byte of it did
            hash = fnv1a64(source.filename().string(), hash);
            hash = fnv1a64(&digest, sizeof(digest), hash);
        }

        // and every dependency's key. The topological order guarantees they are already computed - if one is
        // missing the graph was not ordered, which is a resolver bug rather than a user error
        for (const std::filesystem::path &dependency : manifest.depends) {
            auto found = digest_by_path.find(dependency);
            if (found == digest_by_path.end()) {
                out_error = fmt::format(
                    "internal: the manifest '{}' was keyed before its dependency '{}'.",
                    manifest.path.string(), dependency.string());
                return false;
            }

            hash = fnv1a64(&found->second, sizeof(found->second), hash);
        }

        // **and every module parsed before this one, declared dependency or not.**
        //
        // `depends` is the ordering the author asked for; it is not the extent of what this module can see.
        // AST::Collector is shared across the whole bundle, so a module can name anything an *earlier*
        // module declared - which is why the standard library needs no `#[depends:]` to be usable. The same
        // reach means an earlier module can change what this one compiles to without appearing in its
        // dependency list: one more overload in a set this module calls into resolves a call differently.
        //
        // so the key folds in the running digest of everything before it. Conservative - adding an unrelated
        // library ahead of this one invalidates it - but the alternative is a stale object that links
        // silently, and the conservative direction is the only safe one to be wrong in
        hash = fnv1a64(&preceding, sizeof(preceding), hash);

        key.hex = to_hex(hash);

        digest_by_path.emplace(manifest.path, hash);
        preceding = fnv1a64(&hash, sizeof(hash), preceding);
        out_keys.emplace(manifest.name, std::move(key));
    }

    return true;
}

namespace
{

// `$XDG_CACHE_HOME/echo`, else `$HOME/.cache/echo`, else empty when neither is set
std::filesystem::path user_cache_root()
{
    if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "echo";
    }

    if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".cache" / "echo";
    }

    return {};
}

// this manifest ships with the compiler rather than with the program being built
bool is_compiler_supplied(const Parser::ModuleManifest &manifest)
{
    const std::filesystem::path stdlib_root(STDLIB_SOURCE_DIR);

    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(manifest.path, stdlib_root, ec);

    return !ec && !relative.empty() && relative.native().rfind("..", 0) != 0;
}

};

std::filesystem::path Compiler::module_cache_dir(
    const Parser::ModuleManifest &manifest, const std::filesystem::path &cache_dir_override)
{
    if (!cache_dir_override.empty()) {
        // one directory for every module, so the module name has to be in the artifact path rather than only
        // in the filename - two modules could otherwise agree on a key only by agreeing on everything
        return cache_dir_override / manifest.name;
    }

    // **the standard library does not cache into the compiler's own source tree.**
    //
    // `.echo` beside the manifest is right for a library that belongs to the program being built: the cache
    // travels with the code, and deleting a checkout deletes it. The standard library's manifest lives wherever
    // this compiler was built from, which is nobody's project - writing there means every user's build
    // scribbles in the toolchain, and against an installed compiler that directory is read-only, so the build
    // would fail rather than merely not cache.
    if (is_compiler_supplied(manifest)) {
        const std::filesystem::path user_root = user_cache_root();

        if (!user_root.empty()) {
            return user_root / manifest.name;
        }
    }

    return manifest.directory / ".echo";
}

bool Compiler::cache_dir_is_writable(const std::filesystem::path &directory)
{
    std::error_code ec;

    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return false;
    }

    // creating the directory is not the same as being allowed to write in it - it may already have existed with
    // no permission to add to it. So actually write something, which is the only answer that cannot be wrong
    const std::filesystem::path probe = directory / ".probe";
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
    }

    std::filesystem::remove(probe, ec);

    return true;
}

std::filesystem::path Compiler::module_object_path(
    const Parser::ModuleManifest &manifest,
    const ModuleCacheKey &key,
    const std::filesystem::path &cache_dir_override)
{
    return module_cache_dir(manifest, cache_dir_override) / fmt::format("{}-{}.o", manifest.name, key.hex);
}

std::filesystem::path Compiler::module_inputs_path(
    const Parser::ModuleManifest &manifest, const std::filesystem::path &cache_dir_override)
{
    return module_cache_dir(manifest, cache_dir_override) / fmt::format("{}.inputs", manifest.name);
}

bool Compiler::write_inputs_record(const std::filesystem::path &path, const ModuleCacheKey &key)
{
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }

    out << key.hex << "\n";
    for (const auto &[input, digest] : key.inputs) {
        out << to_hex(digest) << " " << input.string() << "\n";
    }

    return out.good();
}

std::string Compiler::explain_miss(const std::filesystem::path &inputs_path, const ModuleCacheKey &key)
{
    std::ifstream in(inputs_path, std::ios::binary);
    if (!in) {
        return "";
    }

    std::string recorded_hex;
    if (!std::getline(in, recorded_hex)) {
        return "";
    }

    std::map<std::string, std::string> recorded;
    std::string line;
    while (std::getline(in, line)) {
        const size_t space = line.find(' ');
        if (space == std::string::npos) {
            continue;
        }
        recorded.emplace(line.substr(space + 1), line.substr(0, space));
    }

    for (const auto &[input, digest] : key.inputs) {
        auto found = recorded.find(input.string());

        if (found == recorded.end()) {
            return fmt::format("'{}' is new", input.filename().string());
        }

        if (found->second != to_hex(digest)) {
            return fmt::format("'{}' changed", input.filename().string());
        }
    }

    // every input this build has agrees with the record, so the difference is either an input the record has
    // and this build does not - a deleted file - or something outside the file list entirely
    for (const auto &[recorded_path, digest] : recorded) {
        const bool still_present = std::any_of(key.inputs.begin(), key.inputs.end(),
            [&recorded_path](const std::pair<std::filesystem::path, uint64_t> &input) {
                return input.first.string() == recorded_path;
            });

        if (!still_present) {
            return fmt::format("'{}' is gone", std::filesystem::path(recorded_path).filename().string());
        }
    }

    return "the compiler, target or build mode changed";
}
