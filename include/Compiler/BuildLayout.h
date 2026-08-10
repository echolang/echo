#ifndef BUILDLAYOUT_H
#define BUILDLAYOUT_H

#pragma once

#include "Parser/ManifestParser.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace Compiler
{
    // the directory a manifest gets when it does not name one.
    //
    // **not a dot-directory**, deliberately: a build's output is something its author is meant to see, find
    // and be able to delete, and the previous `.echo` was invisible in a listing and in most file browsers
    constexpr std::string_view k_default_build_directory = "ecobuild";

    // how far a directory has to prove itself before anything is written into it.
    //
    // **only a directory a person named can be pointed at a source tree.** The default beside a manifest
    // and `~/.cache/echo/<name>` are this compiler's own choices, so they are adopted whatever is in them -
    // including artifacts an older echoc left behind. A `--build-dir` or a `#[build_dir:]` is a path
    // somebody typed, and one that is not provably ours is refused rather than written into, because the
    // typo that names a source directory is the one that `echoc clean` would later delete
    enum class BuildDirTrust
    {
        t_ours,
        t_asked_for
    };

    // three answers, and the middle one is not a failure. **an unwritable store must never fail a build**:
    // a cache is an optimization, so the only correct response is to compile and not keep the result. The
    // third is the opposite - it means the author pointed the build at something of theirs, and carrying on
    // would write into it
    enum class BuildDirState
    {
        t_ready,
        t_unusable,
        t_foreign
    };

    // creates `directory` and leaves the marker that says echoc made it.
    //
    // **the marker write is the write probe.** Creating a directory is not the same as being allowed to
    // write in it - it may already have existed with no permission to add to it - so something has to
    // actually be written, and a store that cannot take the marker cannot take an object either. It is also
    // the proof `remove_build_directory` requires, so one act answers both questions and there is no probe
    // to keep in step with a marker
    BuildDirState prepare_build_directory(const std::filesystem::path &directory, BuildDirTrust trust);

    // does this manifest ship with the compiler rather than with the program being built.
    //
    // read here to keep the standard library's artifacts out of the toolchain tree, and by `echoc clean` to
    // leave them alone: that store is the machine's rather than this project's, shared by every project on
    // it, and the most expensive thing in a build to produce again
    bool is_compiler_supplied_module(const Parser::ModuleManifest &manifest);

    // what a `#[build_dir: "..."]` names, or false with a sentence in `out_reason`.
    //
    // **here rather than in the manifest reader**, which is what the `link` and `cc` arms beside it already
    // do: where an artifact may go is this file's question, and a manifest reader that answers it is a
    // second answer for the next path-shaped key to copy rather than call
    bool resolve_declared_build_dir(
        const std::string &written,
        const std::filesystem::path &module_dir,
        std::filesystem::path &out_directory,
        std::string &out_reason
    );

    // did echoc leave its marker in this directory
    bool is_echo_build_directory(const std::filesystem::path &directory);

    // a directory that has to prove itself and has not: asked for by name, already holding something, and
    // carrying no marker. **one predicate, two readers** - the check that refuses a whole build up front and
    // the one that refuses the individual write - so the two cannot come to different conclusions about the
    // same directory
    bool is_foreign_build_directory(const std::filesystem::path &directory, BuildDirTrust trust);

    enum class BuildDirRemoval
    {
        t_removed,
        t_absent,
        t_refused
    };

    // **the only recursive removal in the compiler.** The trust rule is applied here rather than by the
    // caller, so there is no spelling of "remove this" that skips it: a directory the compiler chose the
    // name of is removed on that basis, and one somebody typed has to carry the marker first. `out_reason`
    // is a sentence on a refusal and empty otherwise.
    //
    // the split matters for a store that predates markers - `~/.cache/echo/<name>` full of objects and no
    // CACHEDIR.TAG is exactly what an upgrade leaves behind, and refusing to clean it would be a `clean`
    // that does not work until after the next build
    BuildDirRemoval remove_build_directory(
        const std::filesystem::path &directory, BuildDirTrust trust, std::string &out_reason);

    // where this invocation's build artifacts go.
    //
    // **one owner for "where does a build artifact go".** That question used to have four answers in three
    // files - a cache directory beside each manifest, a `cc` join in the driver, and two string concatenations
    // against the output path - so a build scattered objects nothing collected and nothing could remove.
    //
    // Resolved once, in run_front_end, and carried on FrontEnd: a second resolution is a second answer, and
    // the two can disagree about a directory one of them has already written into
    class BuildLayout
    {
    public:
        // `flag_directory` is --build-dir as written, empty for none. `entry` is the manifest of the module
        // being built, null when the program is a loose .eco file with no project around it, in which case
        // artifacts nothing owns go to `fallback_scratch`
        static BuildLayout resolve(const std::filesystem::path &flag_directory, const Parser::ModuleManifest *entry, const std::filesystem::path &fallback_scratch);

        // where this module's own artifacts live - its object, its inputs record, its C build
        std::filesystem::path module_dir(const Parser::ModuleManifest &manifest) const;
        std::filesystem::path module_cc_dir(const Parser::ModuleManifest &manifest) const;

        // the objects nothing may keep: the entry module's, the merged whole-program one, and a C build
        // whose own store could not be written
        std::filesystem::path scratch_dir() const {
            return _scratch;
        }

        // where a C object goes when its module's own store could not be written. The `cc` join lives
        // here beside module_cc_dir's: a caller spelling it is the fourth answer this class replaced
        std::filesystem::path scratch_cc_dir() const;

        std::filesystem::path scratch_object(const std::string &module_name) const;

        // removes the scratch directory, but **only when it is the temporary one**.
        //
        // A project's scratch lives inside its build directory, where `echoc clean` reaches it and where a
        // debugger can still find the objects a Mach-O binary's debug map points at. The fallback is a
        // per-process directory under the system's temporary path that no later command knows the name of,
        // so this process is the only thing that can ever remove it - and one left per invocation is a leak
        void discard_temporary_scratch() const;

        // prepares a directory this layout named. **the trust rule is applied here and nowhere else**, so a
        // caller never has to know whether the path it was handed came from a person or from the default
        BuildDirState prepare_module_dir(const Parser::ModuleManifest &manifest) const;
        BuildDirState prepare_scratch_dir() const;

        // the same rule on the way out - see remove_build_directory
        BuildDirRemoval remove_module_dir(
            const Parser::ModuleManifest &manifest, std::string &out_reason) const;

        // the first module whose build directory is somebody else's, as a sentence, or empty when every one
        // of them is this compiler's to write in.
        //
        // **a query with no side effects**, asked once before anything is built: a whole-program build
        // creates no directory at all, so preparing them all to find out would leave a trail of empty ones
        // for a build that keeps nothing. It is also the only place that turns the refusal into a sentence,
        // which is why it is asked of the whole graph rather than of whichever module is reached first
        std::string first_foreign_directory(const std::vector<Parser::ModuleManifest> &manifests) const;

    private:
        // whether a module's directory was asked for by name, which is what decides how far it has to prove
        // itself. Shares module_dir's arm order deliberately - two walks of it would be two answers
        BuildDirTrust trust_of(const Parser::ModuleManifest &manifest) const;

        std::filesystem::path _flag_directory;

        // the entry module's own directory, and how far it has to prove itself. Empty when the program is a
        // loose .eco file. Kept rather than re-derived because prepare_scratch_dir is the only thing that
        // ever creates it - see BuildLayout::resolve
        std::filesystem::path _entry_dir;
        BuildDirTrust _entry_trust = BuildDirTrust::t_ours;

        std::filesystem::path _scratch;
        bool _scratch_is_temporary = false;
    };
};

#endif
