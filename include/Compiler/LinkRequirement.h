#ifndef LINKREQUIREMENT_H
#define LINKREQUIREMENT_H

#pragma once

#include "Compiler/TargetFacts.h"

#include <fmt/core.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Compiler
{
    // **the sole answer to "what does this build need linked".**
    //
    // one native library, framework, search directory or prebuilt object, as a module's manifest declared
    // it - `#[link: "lib:GL"]` - or as the command line did. Shaped like Compiler::TargetFacts: settled
    // facts with a closed vocabulary, resolved once and read afterwards.
    //
    // **typed rather than a flag string**, and that is not tidiness. `echoc run` has to make the same
    // libraries resolvable inside the JIT, which means dlopen'ing them - and a `-lGL` recovered from a flag
    // string means re-parsing linker syntax, while `-framework OpenGL` is two argv words meaning one thing
    // with no dlopen spelling at all. The vocabulary below is what makes both paths answerable from one
    // declaration; a passthrough would make `run` unimplementable rather than merely inelegant
    enum class LinkScheme
    {
        // `lib:GL` - a library by name, resolved by the linker's usual rules
        t_library,

        // `framework:OpenGL` - a Darwin framework. Refused anywhere else, see parse_link_requirement
        t_framework,

        // `search:vendor/lib` - a directory to look in, absolute once parsed
        t_search,

        // `object:vendor/glad.o` - a prebuilt object, absolute once parsed. The one scheme with no runtime
        // spelling, because there is nothing for the JIT to open
        t_object,
    };

    struct LinkRequirement
    {
        LinkScheme scheme = LinkScheme::t_library;

        // a library or framework name, or an absolute path for the two path schemes
        std::string value;

        // the module whose manifest declared this, empty for one that came off the command line. Carried
        // so a failed link can name who asked for it - today a missing library is raw `ld` output naming
        // nothing, in a build that may have a dozen manifests in it
        std::string declared_by;

        // **identity is the scheme and the value, never the declarer.** two modules asking for the same
        // library is one requirement, and which of them is credited for it is not part of the question
        bool operator==(const LinkRequirement &other) const {
            return scheme == other.scheme && value == other.value;
        }
    };

    // the "expected one of" a refused value is answered with, built from the same table that resolves one -
    // so a scheme added there cannot be missing from the message that rejects its neighbours. The rule
    // TargetFacts' axis vocabularies already live by
    std::string link_scheme_list();

    // **the `<scheme>:<value>` grammar itself, once**, for the two attributes that share it.
    //
    // split on the *first* colon so a value may contain one - `search:C:\libs` is a search path and not a
    // scheme called `search:C` - then the table, then the empty-value guard. `noun` is what the three
    // refusals call the thing ("link scheme", "C build scheme"); everything else about them is identical,
    // and a second hand-written copy is a rule that gets fixed in one attribute and not the other
    template <class Scheme>
    bool split_scheme(
        const std::string &spelled,
        const std::vector<std::pair<std::string, Scheme>> &table,
        const std::string &noun,
        const std::string &noun_list,
        Scheme &out_scheme,
        std::string &out_value,
        std::string &out_error
    )
    {
        const size_t separator = spelled.find(':');

        if (separator == std::string::npos) {
            out_error = fmt::format(
                "'{}' does not name a {} - write '<scheme>:<value>', where scheme is one of: {}.",
                spelled, noun, noun_list);
            return false;
        }

        const std::string scheme_name = spelled.substr(0, separator);

        for (const auto &[candidate, scheme] : table) {
            if (candidate != scheme_name) {
                continue;
            }

            out_value = spelled.substr(separator + 1);

            if (out_value.empty()) {
                out_error = fmt::format("'{}' names the '{}' scheme and no value.", spelled, scheme_name);
                return false;
            }

            out_scheme = scheme;
            return true;
        }

        out_error = fmt::format(
            "'{}' is not a {}, expected one of: {}.", scheme_name, noun, noun_list);
        return false;
    }

    // the "expected one of" for any such table, joined the one way - see split_scheme
    template <class Scheme>
    std::string scheme_list_of(const std::vector<std::pair<std::string, Scheme>> &table)
    {
        std::string list;

        for (const auto &[spelled, scheme] : table) {
            (void)scheme;
            list += list.empty() ? spelled : ", " + spelled;
        }

        return list;
    }

    // `<scheme>:<value>`, the way a manifest wrote it. **the one thing a message may quote back**: a
    // diagnostic naming the bare value sends the reader looking for `glfw` in a file that says `lib:glfw`,
    // and a path scheme's settled absolute value is not in the manifest at all
    std::string link_requirement_spelling(const LinkRequirement &requirement);

    // `<scheme>:<value>` - one requirement, or false with a located-shaped sentence in `out_error`.
    //
    // **the scheme is mandatory.** Accepting a bare `GL` as a library would be a second rule, and under it
    // a typo'd `framwork:OpenGL` reads as a library of that name and surfaces much later as a linker error
    // about a library nobody wrote. Split on the *first* colon, so `search:C:\libs` still means what it
    // looks like.
    //
    // `base` is what a relative path resolves against - a manifest's own directory, or the working
    // directory for a command-line one. `facts` is what refuses a framework off Darwin, and it is the
    // invocation's rather than the host's so that `--target-os linux` refuses one on a Mac
    bool parse_link_requirement(
        const std::string &spelled,
        const std::filesystem::path &base,
        const TargetFacts &facts,
        const std::string &declared_by,
        LinkRequirement &out,
        std::string &out_error
    );

    // **the sole renderer of a link line**, filling two vectors rather than appending words to one: an
    // `object:` belongs with the object files and the rest belongs after them, and a caller that had to
    // remember which is which is a caller that gets it wrong on one of the two paths.
    //
    // both spellings render through this - the `ld` fast path and the `clang` fallback beside it - because
    // the fallback is the one exercised least and therefore the one a change misses. `-L`, `-l` and
    // `-framework` are spelled the same to both, so there is one rendering and not two flavours
    void partition_link_requirements(
        const std::vector<LinkRequirement> &requirements,
        std::vector<std::filesystem::path> &out_objects,
        std::vector<std::string> &out_words
    );

    // the file the JIT has to open for this requirement's symbols to resolve, or nullopt with a sentence in
    // `out_refusal`.
    //
    // **three answers and not two.** A library becomes `lib<name>.<ext>`, tried against each `search:`
    // directory before the bare name; a framework becomes the binary inside the bundle; and an `object:`
    // has no runtime spelling at all - which is a refusal naming the module that asked, never a silence
    // that leaves MCJIT to report an unresolved symbol.
    //
    // asks TargetFacts::host() for the extension rather than the invocation's facts, and that is the one
    // place the two legitimately differ: `run` executes on this machine, so what `--target-os` says a
    // condition sees has nothing to do with what dlopen can open
    std::optional<std::filesystem::path> runtime_library_of(
        const LinkRequirement &requirement,
        const std::vector<LinkRequirement> &all,
        std::string &out_refusal
    );

    // appends what `into` does not already hold, comparing on scheme and value. First occurrence wins, so
    // the order a caller merges in is the priority it is expressing
    void merge_link_requirements(const std::vector<LinkRequirement> &incoming, std::vector<LinkRequirement> &into);
};

#endif
