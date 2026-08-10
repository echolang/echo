#include "Compiler/CBuild.h"

#include "eco.h"

#include "Compiler/HostTool.h"
#include "Compiler/LinkRequirement.h"
#include "Compiler/ModuleCache.h"
#include "Compiler/SettledPath.h"
#include "Compiler/TargetFacts.h"

#include <fmt/core.h>

#include <fstream>
#include <set>

namespace
{

const std::vector<std::pair<std::string, Compiler::CcScheme>> &cc_scheme_table()
{
    static const std::vector<std::pair<std::string, Compiler::CcScheme>> table = {
        { "sources", Compiler::CcScheme::t_sources },
        { "include", Compiler::CcScheme::t_include },
        { "define", Compiler::CcScheme::t_define },
        { "flag", Compiler::CcScheme::t_flag },
    };

    return table;
}

// the prerequisites out of a make-style depfile, which is what `clang -MD` writes:
//
//     glad.o: c/glad.c c/include/glad/gl.h \
//       /usr/include/stdint.h
//
// the target and its colon are dropped, a `\` before a newline is a continuation, and a `\` before a space
// is a literal space in a path. Answers an empty list for a depfile that is not there, which is the first
// build and is not an error - see build_c_sources
std::vector<std::filesystem::path> read_depfile(const std::filesystem::path &path)
{
    const std::optional<std::string> bytes = Compiler::read_whole_file(path);

    if (!bytes.has_value()) {
        return {};
    }

    const std::string &text = bytes.value();

    const size_t colon = text.find(':');
    if (colon == std::string::npos) {
        return {};
    }

    std::vector<std::filesystem::path> prerequisites;
    std::string current;

    const auto flush = [&] {
        if (!current.empty()) {
            prerequisites.push_back(current);
            current.clear();
        }
    };

    for (size_t i = colon + 1; i < text.size(); i++) {
        const char c = text[i];

        if (c == '\\' && i + 1 < text.size()) {
            const char next = text[i + 1];

            // a continuation: the backslash and the newline both vanish and the list carries on
            if (next == '\n' || next == '\r') {
                flush();
                i++;
                continue;
            }

            // an escaped space, which is how a path with a space in it survives this format
            if (next == ' ') {
                current += ' ';
                i++;
                continue;
            }
        }

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            flush();
            continue;
        }

        current += c;
    }

    flush();

    return prerequisites;
}

// **the half of the key that reads no file contents**, and therefore the half that can name a path: this
// compiler, this target, the build mode, and the arguments every translation unit in the spec shares. It
// goes in each object's *filename*, which is what lets two build modes coexist in one store the way the
// Echo object cache's key does.
//
// **folded once for the whole spec**, because none of it varies per source - only the source's own path
// does, which c_settings_digest adds. Per source it cost a subtarget resolution each, and resolving one
// builds a whole llvm::MCSubtargetInfo to ask whether a CPU name is valid
bool c_spec_digest(
    const Compiler::CBuildSpec &spec,
    const Compiler::CompilerOptions &options,
    uint64_t &out_digest,
    std::string &out_error)
{
    uint64_t digest = Compiler::fnv1a64(std::string(ECO_C_BUILD_VERSION), Compiler::k_fnv_offset_basis);

    // the triple and the CPU inside it, shared with the Echo object cache - see fold_target_environment
    if (!Compiler::fold_target_environment(options, digest, digest, out_error)) {
        return false;
    }

    digest = Compiler::fnv1a64(
        options.no_optimize ? std::string("noopt") : std::string("opt"), digest);
    digest = Compiler::fnv1a64(
        options.emitting_debug_info() ? std::string("g") : std::string("nog"), digest);

    for (const std::filesystem::path &include : spec.includes) {
        digest = Compiler::fnv1a64(include.string(), digest);
    }

    for (const std::string &define : spec.defines) {
        digest = Compiler::fnv1a64(define, digest);
    }

    for (const std::string &flag : spec.flags) {
        digest = Compiler::fnv1a64(flag, digest);
    }

    out_digest = digest;

    return true;
}

// a stable, key-independent name for one source's artifacts. **the absolute path is in it**, because two
// modules - or two directories inside one - may hold a `util.c` and a store keyed on the stem alone would
// hand one of them the other's object
std::string artifact_stem(uint64_t module_digest, const std::filesystem::path &source)
{
    return fmt::format(
        "{}-{}", source.stem().string(), Compiler::to_hex(Compiler::fnv1a64(source.string(), module_digest)));
}

// and the other half: the source's bytes and every header the last build recorded it reaching.
//
// **this one cannot be in a filename**, and that is the whole shape of this cache. The depfile that names
// the headers is written *by* the compile, so a key holding them cannot be known before one has run - if
// the object's path carried it, a first build would store under a key no second build ever computes and
// every source would compile exactly twice. So the path carries the settings and a sidecar carries this,
// written after the compile from the depfile that compile just produced
// `source_digest` is the source's own bytes already folded into the settings half, which the caller holds
// because it reads the source exactly once for however many times this is asked
std::string c_content_key(
    const std::filesystem::path &source, uint64_t source_digest, const std::filesystem::path &depfile)
{
    uint64_t digest = source_digest;

    // **sorted, and the source itself dropped.** clang lists the source among its own prerequisites and
    // lists the rest in include order, which is an order a harmless edit can change - a key that moved for
    // that would be a rebuild for nothing
    std::set<std::filesystem::path> headers;

    for (const std::filesystem::path &prerequisite : read_depfile(depfile)) {
        std::error_code ec;
        const std::filesystem::path resolved = std::filesystem::weakly_canonical(prerequisite, ec);

        if (ec || resolved == source) {
            continue;
        }

        headers.insert(resolved);
    }

    for (const std::filesystem::path &header : headers) {
        digest = Compiler::fnv1a64(header.string(), digest);

        // **a header that has since been deleted still moves the key**, because its absence folds in as a
        // different byte than its contents did. Skipping it silently would make removing an include a
        // no-op for the cache
        const std::optional<std::string> header_bytes = Compiler::read_whole_file(header);

        digest = header_bytes.has_value()
            ? Compiler::fnv1a64(header_bytes.value(), digest)
            : Compiler::fnv1a64(std::string("<gone>"), digest);
    }

    return Compiler::to_hex(digest);
}

// the arguments every C compile shares, after the output and input
void append_common_arguments(
    const Compiler::CBuildSpec &spec, const Compiler::CompilerOptions &options,
    std::vector<std::string> &argv)
{
    for (const std::filesystem::path &include : spec.includes) {
        argv.push_back("-I" + include.string());
    }

    for (const std::string &define : spec.defines) {
        argv.push_back("-D" + define);
    }

    // **before the module's own flags, so an author can override one.** A `flag:-O0` on a debug build is a
    // reasonable thing to write and would be pointless if it came first
    argv.push_back(options.no_optimize ? "-O0" : "-O2");

    if (options.emitting_debug_info()) {
        argv.push_back("-g");
    }

    for (const std::string &flag : spec.flags) {
        argv.push_back(flag);
    }
}

};

std::string Compiler::cc_scheme_list()
{
    return scheme_list_of(cc_scheme_table());
}

bool Compiler::parse_cc_requirement(
    const std::string &spelled,
    const std::filesystem::path &base,
    CcScheme &out_scheme,
    std::string &out_value,
    std::string &out_error
)
{
    std::string value;

    if (!split_scheme(
            spelled, cc_scheme_table(), "C build scheme", cc_scheme_list(), out_scheme, value, out_error)) {
        return false;
    }

    switch (out_scheme) {
    case CcScheme::t_sources:
        // **as written.** expanding a pattern has one owner and it is Parser::expand_source_pattern; a
        // second expander here is how `*` would come to mean one thing in `#[sources:]` and another here
        out_value = value;
        return true;

    case CcScheme::t_include: {
        const std::filesystem::path directory = settled_path(base, value);

        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec)) {
            out_error = fmt::format(
                "the include path '{}' resolves to '{}', which is not a directory.",
                value, directory.string());
            return false;
        }

        out_value = directory.string();
        return true;
    }

    case CcScheme::t_define:
    case CcScheme::t_flag:
        out_value = value;
        return true;
    }

    return false;
}

bool Compiler::apply_cc_requirement(
    const std::string &spelled,
    const std::filesystem::path &base,
    CBuildSpec &spec,
    std::string &out_error
)
{
    CcScheme scheme = CcScheme::t_sources;
    std::string settled;

    if (!parse_cc_requirement(spelled, base, scheme, settled, out_error)) {
        return false;
    }

    switch (scheme) {
    case CcScheme::t_sources:
        spec.source_patterns.push_back(settled);
        break;

    case CcScheme::t_include:
        spec.includes.push_back(settled);
        break;

    case CcScheme::t_define:
        spec.defines.push_back(settled);
        break;

    case CcScheme::t_flag:
        spec.flags.push_back(settled);
        break;
    }

    return true;
}

bool Compiler::build_c_sources(
    const CBuildSpec &spec,
    const CompilerOptions &options,
    const std::filesystem::path &cache_dir,
    const std::filesystem::path &scratch_dir,
    std::vector<std::string> &out_explain,
    CBuildResult &out,
    std::string &out_error
)
{
    if (spec.empty()) {
        return true;
    }

    // **an unwritable store is a slower build and never a failed one**, the rule plan_module_artifacts
    // already follows for Echo objects. cache_dir_is_writable creates the directory as it probes it, so
    // only the scratch fallback is left to create here
    std::error_code ec;
    const bool keeping = !cache_dir.empty() && cache_dir_is_writable(cache_dir);
    const std::filesystem::path directory = keeping ? cache_dir : scratch_dir;

    if (!keeping) {
        std::filesystem::create_directories(directory, ec);

        if (ec) {
            out_error = fmt::format("{}: cannot be created for the C build.", directory.string());
            return false;
        }
    }

    // everything the whole spec shares, folded once rather than once per source
    uint64_t settings = 0;

    if (!c_spec_digest(spec, options, settings, out_error)) {
        return false;
    }

    const uint64_t module_digest = fnv1a64(spec.module_name, Compiler::k_fnv_offset_basis);

    out.content_digest = module_digest;

    for (const std::filesystem::path &source : spec.sources) {
        const uint64_t source_settings = fnv1a64(source.string(), settings);
        const std::string stem =
            fmt::format("{}.{}", artifact_stem(module_digest, source), to_hex(source_settings));

        const std::filesystem::path object = directory / (stem + ".o");
        const std::filesystem::path depfile = directory / (stem + ".d");
        const std::filesystem::path sidecar = directory / (stem + ".key");

        // **read once**, and folded into the settings half here so the key can be recomputed against the
        // depfile this build is about to write without touching the file again
        const std::optional<std::string> source_bytes = read_whole_file(source);

        if (!source_bytes.has_value()) {
            out_error = fmt::format("{}: cannot be read.", source.string());
            return false;
        }

        const uint64_t source_digest = fnv1a64(source_bytes.value(), source_settings);
        const std::string key = c_content_key(source, source_digest, depfile);

        out.content_digest = fnv1a64(key, out.content_digest);

        // a hit is the object being there *and* the sidecar agreeing about what it was built from. Either
        // one alone is not enough: an object with no sidecar is one an interrupted build left behind
        if (keeping
            && std::filesystem::is_regular_file(object, ec)
            && read_whole_file(sidecar).value_or(std::string()) == key) {
            out_explain.push_back(fmt::format("  {}  {}  hit", source.filename().string(), key));
            out.objects.push_back(object);
            continue;
        }

        out_explain.push_back(fmt::format("  {}  {}  miss", source.filename().string(), key));

        std::vector<std::string> argv = {
            "clang",
            "-c",

            // one object serves the executable and the loadable library both - see the header
            "-fPIC",
            "-o", object.string(),
            source.string(),

            // what lets the key above see this translation unit's headers, from the next build onward
            "-MD",
            "-MF", depfile.string(),
        };

        append_common_arguments(spec, options, argv);

        if (!run_tool(argv)) {
            out_error = fmt::format(
                "compiling '{}' for module '{}' failed.", source.string(), spec.module_name);
            return false;
        }

        // **recomputed against the depfile this compile just wrote**, which is the whole point of the
        // sidecar: the key stored is the one the *next* build will compute, so a fresh store settles after
        // one build rather than compiling every source twice. The source's own bytes are the ones already
        // folded into source_digest - the compile did not change them
        if (keeping) {
            std::ofstream(sidecar, std::ios::binary) << c_content_key(source, source_digest, depfile);
        }

        out.objects.push_back(object);
    }

    return true;
}

bool Compiler::build_c_shared_library(
    const CBuildSpec &spec,
    const CBuildResult &compiled,
    const std::vector<std::string> &link_words,
    const std::filesystem::path &cache_dir,
    const std::filesystem::path &scratch_dir,
    std::filesystem::path &out_library,
    std::string &out_error
)
{
    const std::vector<std::filesystem::path> &objects = compiled.objects;

    if (objects.empty()) {
        out_error = fmt::format("module '{}' has no C objects to load.", spec.module_name);
        return false;
    }

    std::error_code ec;
    const bool keeping = !cache_dir.empty() && cache_dir_is_writable(cache_dir);
    const std::filesystem::path directory = keeping ? cache_dir : scratch_dir;

    if (!keeping) {
        std::filesystem::create_directories(directory, ec);
    }

    // **keyed on the objects' content digest, never on their names.** An object's path carries only the
    // settings half of its key, so a header edit rewrites the object in place and every filename stays
    // exactly as it was - a library keyed on those would be served stale for as long as nobody touched a
    // .c file, which is the one case this whole depfile arrangement exists to catch
    uint64_t digest = fnv1a64(spec.module_name, Compiler::k_fnv_offset_basis);
    digest = fnv1a64(&compiled.content_digest, sizeof(compiled.content_digest), digest);

    for (const std::string &word : link_words) {
        digest = fnv1a64(word, digest);
    }

    // the host's, because this is a file this machine's dlopen has to open - see runtime_library_of
    out_library = directory / fmt::format(
        "lib{}.{}{}", spec.module_name, to_hex(digest), TargetFacts::host().shared_library_extension());

    if (keeping && std::filesystem::is_regular_file(out_library, ec)) {
        return true;
    }

    // through the clang driver rather than the system linker: a shared library's platform flags are
    // considerably fussier than an executable's, and this path is not the one Backend::link_executable's
    // fast path exists to speed up - it runs once per module per change, not once per build
    std::vector<std::string> argv = { "clang", "-shared", "-o", out_library.string() };

    for (const std::filesystem::path &object : objects) {
        argv.push_back(object.string());
    }

    argv.insert(argv.end(), link_words.begin(), link_words.end());

    if (!run_tool(argv)) {
        out_error = fmt::format(
            "linking the C sources of module '{}' into a loadable library failed.", spec.module_name);
        return false;
    }

    return true;
}
