#ifndef CBUILD_H
#define CBUILD_H

#pragma once

#include "Compiler/CompilerOptions.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Compiler
{
    // **the sole answer to "what C does this module bring with it".**
    //
    // an Echo binding to a real native library usually needs a shim - glad's loader, a `static inline`
    // header wrapper, a Cocoa entry point - and there is no way to write one in Echo, because a shim exists
    // precisely to say something C says and Echo does not.
    //
    // **the boundary is one sentence and it is hard: a C build contributes objects and nothing else.**
    // Never an include path, never a macro, never anything that reaches Echo's front end. Echo does not
    // read C headers and is not going to; an `extern { }` block is still written by hand and still the
    // author's to get right, which book/concept/cbinding.md already says. Everything below is a build
    // step that happens to run before the link, and it is deliberately not a build system: there is no
    // dependency between two modules' C, no library target, and no install step
    enum class CcScheme
    {
        // `sources:c/*.c` - a pattern, expanded by the one expander a `#[sources:]` uses
        t_sources,

        // `include:c/include` - `-I`, absolute once parsed
        t_include,

        // `define:GLAD_GL_IMPLEMENTATION` - `-D`
        t_define,

        // `flag:-Wno-unused` - the escape hatch, passed through verbatim. Unlike a link requirement this
        // one is safe to leave untyped: it reaches exactly one known tool and is never re-read for a
        // second purpose, which is the whole reason a link flag could not be a string
        t_flag,
    };

    // one module's C build, resolved. `sources` is expanded and sorted, `includes` absolute
    struct CBuildSpec
    {
        std::string module_name;

        // **the patterns as written**, still unexpanded. Expanding one is Parser::expand_source_pattern's
        // question, and it is answered a step later than the rest of this spec is filled - so the deferral
        // is a field here rather than an out-parameter threaded back through the manifest reader
        std::vector<std::string> source_patterns;

        std::vector<std::filesystem::path> sources;
        std::vector<std::filesystem::path> includes;
        std::vector<std::string> defines;
        std::vector<std::string> flags;

        // no sources means no build - an `include:` on its own is a manifest that declares a search path
        // for nothing, which read_module_manifest refuses rather than silently honouring
        bool empty() const {
            return sources.empty();
        }
    };

    // the "expected one of" a refused `#[cc:]` value is answered with, off the same table that resolves one
    std::string cc_scheme_list();

    // `<scheme>:<value>` - the scheme and its settled value, or false with a sentence.
    //
    // `include:` comes back absolute and checked; `sources:` comes back as the **pattern as written**,
    // because expanding it is Parser::expand_source_pattern's question and answering it a second time here
    // is how the command line and a manifest would come to mean different things by one `*`
    bool parse_cc_requirement(
        const std::string &spelled,
        const std::filesystem::path &base,
        CcScheme &out_scheme,
        std::string &out_value,
        std::string &out_error
    );

    // the same thing, put where it goes. **which member of a spec a scheme fills is this subsystem's
    // question**, not the manifest reader's: a scheme added above then touches one file rather than two in
    // two layers, and the reader is left with the one-line shape `#[link:]` already has
    bool apply_cc_requirement(
        const std::string &spelled,
        const std::filesystem::path &base,
        CBuildSpec &spec,
        std::string &out_error
    );

    // what one module's C build produced. `objects` go into the link, or into the loadable library
    // build_c_shared_library makes of them for the JIT
    struct CBuildResult
    {
        std::vector<std::filesystem::path> objects;

        // every object's content key, folded. **not derivable from the object names**: an object's path
        // carries only the settings half of its key, so a header edit rewrites an object in place and
        // leaves every filename alone. Anything downstream that caches on top of these objects has to key
        // on this instead, which is exactly what build_c_shared_library does
        uint64_t content_digest = 0;
    };

    // compiles what changed and reuses what did not, one clang invocation per source.
    //
    // **`-fPIC` unconditionally**, so one object serves both the executable and the loadable library the
    // JIT needs. The alternative is two objects per source under two keys, which doubles a cache to save
    // an indirection nothing has measured.
    //
    // **headers are tracked through clang's own depfile.** `-MD -MF` writes what the translation unit
    // actually included, and the *next* key folds those files' bytes in - so a first build always runs and
    // from then on editing a header moves the key. Without it, editing `glad.h` is a stale object with
    // nothing anywhere saying so, which is the module cache's one silent failure mode reproduced in a
    // second store.
    //
    // `cache_dir` empty means there is nowhere to keep anything and everything goes to `scratch_dir`: an
    // unwritable store is a slower build and never a failed one, the rule the Echo object cache already
    // follows. `out_explain` collects a line per source for `--explain-cache`
    bool build_c_sources(
        const CBuildSpec &spec,
        const CompilerOptions &options,
        const std::filesystem::path &cache_dir,
        const std::filesystem::path &scratch_dir,
        std::vector<std::string> &out_explain,
        CBuildResult &out,
        std::string &out_error
    );

    // links a module's C objects into something dlopen can take, for `echoc run`.
    //
    // `link_words` is the module's own link line as Compiler::partition_link_requirements rendered it: a
    // shim calling into GLFW has to resolve those symbols when it is *loaded*, not when the executable
    // that never gets built would have linked
    bool build_c_shared_library(
        const CBuildSpec &spec,
        const CBuildResult &compiled,
        const std::vector<std::string> &link_words,
        const std::filesystem::path &cache_dir,
        const std::filesystem::path &scratch_dir,
        std::filesystem::path &out_library,
        std::string &out_error
    );
};

#endif
