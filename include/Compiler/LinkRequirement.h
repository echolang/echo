#ifndef LINKREQUIREMENT_H
#define LINKREQUIREMENT_H

#pragma once

#include "AST/ASTAttributeReader.h"
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

    // what the native linker should ask for. `dynamic` is "the linker's usual resolution"; `static`
    // seats an archive. A closed vocabulary on the record, not a second scheme - `lib` is still the
    // kind of thing, this is how it is linked
    enum class LinkLinkage
    {
        t_dynamic,
        t_static,
    };

    // what `echoc run` should do with a `lib`. `load` finds a DSO and opens it; `process` means the
    // symbols are already in this process (libc-folded pthread, libSystem). Declared, never guessed
    // from a compiler name list - the author knows, the resolver does not
    enum class LinkRuntime
    {
        t_load,
        t_process,
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

        LinkLinkage linkage = LinkLinkage::t_dynamic;
        LinkRuntime runtime = LinkRuntime::t_load;

        // exact loadable name when the author knows the SONAME (`libssl.so.3`). empty means "find
        // `lib<value>.<ext>` and its versioned neighbours"
        std::optional<std::string> file;

        // **identity is the scheme and the value, never the declarer or the resolution.** two modules
        // asking for the same library is one requirement, and which of them is credited for it is not
        // part of the question. First occurrence wins the linkage and the runtime
        bool operator==(const LinkRequirement &other) const {
            return scheme == other.scheme && value == other.value;
        }
    };

    // the "expected one of" a refused value is answered with, built from the same table that resolves one -
    // so a scheme added there cannot be missing from the message that rejects its neighbours. The rule
    // TargetFacts' axis vocabularies already live by
    std::string link_scheme_list();

    // **the `<scheme>:<value>` grammar itself, once** - now the *command line's* spelling, and only its.
    //
    // A manifest writes the scheme as a tag in the grammar (`#[link: framework "OpenGL"]`), read by
    // AST::AttributeReader::tag out of this same table. Two spellings, because the two media are not
    // alike: an argv word cannot carry a quoted payload without a shell quoting it first, and an Echo
    // file has a lexer. One vocabulary between them, which is what the shared table is for.
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

    // the way it was written. **the one thing a message may quote back**: a diagnostic naming the bare
    // value sends the reader looking for `glfw` in a file that says `lib "glfw"`, and a path scheme's
    // settled absolute value is not in the manifest at all.
    //
    // **which of the two spellings it renders is `declared_by`**, which already records the medium: a
    // manifest's requirement is credited to its module and reads `lib "glfw"`, a command line's is
    // credited to nobody and reads `lib:glfw`. Quoting one in the other's syntax is a message that
    // sends its reader looking for a line nothing contains.
    //
    // a `lib` whose linkage, runtime or file is not the default renders as the record the author
    // wrote - `lib { name: "pthread", runtime: process }` - so a blame line does not invent `lib
    // "pthread"`
    std::string link_requirement_spelling(const LinkRequirement &requirement);

    // one attribute's worth of requirements - `lib "GL"`, `lib { name: "pthread", runtime: process }`,
    // `framework "OpenGL"`, `lib ["GL", "GLU"]`, or an untagged list holding several of those.
    // **appends**, because one attribute may name more than one, and refuses through `reader` so the
    // manifest and a source file report it the same way.
    //
    // a `lib` payload may be a string or a **fixed record** `{ name:, linkage:, runtime:, file: }`.
    // Unknown keys are refused. The command line cannot carry a record; `--link lib:pthread` is the
    // defaults (`dynamic` + `load`)
    //
    // reads the same scheme table `parse_link_requirement` does, and settles a path exactly as it does:
    // what differs between the two is where the scheme and the value were read from, and nothing after
    bool parse_link_attribute(
        const AST::AttributeValue &value,
        const std::filesystem::path &base,
        const TargetFacts &facts,
        const std::string &declared_by,
        AST::AttributeReader &reader,
        std::vector<LinkRequirement> &out
    );

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
    // `-framework` are spelled the same to both, so there is one rendering and not two flavours.
    //
    // **a renderer, not a resolver.** `linkage: static` seats `lib<name>.a` from a declared `search:`
    // or emits `-l<name>`. It does not ask the host OS and it does not invent `-Bstatic`
    void partition_link_requirements(
        const std::vector<LinkRequirement> &requirements,
        std::vector<std::filesystem::path> &out_objects,
        std::vector<std::string> &out_words
    );

    // true when `file` is a real DSO the loader can open - ELF, Mach-O (including fat), or PE.
    // A GNU ld script is a regular file and is not one; handing it to dlopen is the Ubuntu
    // `libpthread.so` failure
    bool is_loadable_shared_object(const std::filesystem::path &file);

    // the file `dlopen` can open for this library name, or nullopt if nothing loadable sits in the
    // given directories or the host's usual lib directories.
    //
    // **search_dirs first, then the host.** A vendored copy wins over a system one. Inside a directory
    // the unversioned `lib<name>.<ext>` wins when it is a real DSO; otherwise *one* versioned neighbour
    // that passes `is_loadable_shared_object`. Two versioned neighbours is a refusal naming `file:`,
    // never a guess. `file`, when set, is the exact basename to look for and is not version-expanded
    std::optional<std::filesystem::path> find_loadable_library(
        const std::string &name,
        const std::vector<std::filesystem::path> &search_dirs,
        const std::optional<std::string> &file,
        std::string &out_refusal
    );

    // the file the JIT has to open for this requirement's symbols to resolve, or nullopt with a sentence in
    // `out_refusal`.
    //
    // **three answers and not two.** A `lib` with `runtime: process` is nothing to open; a `lib` with
    // `linkage: static` and an `object:` refuse (there is nothing to dlopen); a `lib` with `runtime:
    // load` is whatever `find_loadable_library` found, or the bare `lib<name>.<ext>` for the loader
    // (Darwin's dyld cache still answers that); a framework is the binary inside the bundle.
    //
    // asks TargetFacts::host() for the extension rather than the invocation's facts, and that is the one
    // place the two legitimately differ: `run` executes on this machine, so what `--target-os` says a
    // condition sees has nothing to do with what dlopen can open
    std::optional<std::filesystem::path> runtime_library_of(
        const LinkRequirement &requirement,
        const std::vector<LinkRequirement> &all,
        std::string &out_refusal
    );

    // appends what `into` does not already hold, comparing on scheme and value. First occurrence wins
    // when the two agree on linkage, runtime and file. A disagreement is a refusal in `out_error` -
    // those fields change what is linked and what `run` opens, and silently keeping the first is a
    // dropped declaration
    bool merge_link_requirements(
        const std::vector<LinkRequirement> &incoming,
        std::vector<LinkRequirement> &into,
        std::string &out_error
    );
};

#endif
