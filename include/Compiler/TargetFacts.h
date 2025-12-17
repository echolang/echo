#ifndef TARGETFACTS_H
#define TARGETFACTS_H

#pragma once

#include <set>
#include <string>
#include <vector>

namespace Compiler
{
    // **the sole answer to "what can a condition see".**
    //
    // what `#[if: os == darwin]` is evaluated against, and the whole of it: an operating system, an
    // architecture, and the flags the invocation declared. Nothing else is visible to a condition and
    // nothing else may become visible without passing through here - a second source of truth about what
    // platform this is would let two files disagree about it inside one compile.
    //
    // deliberately **not** on CodegenContext, where `target_triple` lives. That one is published by
    // Backend::init_target(), which runs inside compile_bundle - long after the three parse passes, and
    // therefore long after the filter has already had to decide what to keep. These read
    // llvm::sys::getDefaultTargetTriple() directly, which is free to call at any point and which
    // Compiler::compute_module_keys already calls for the same reason.
    //
    // an Echo `const` can never reach here, and that is architectural rather than an omission: the filter
    // runs before pass 1, so no Echo declaration exists yet to read.
    struct TargetFacts
    {
        // the closed vocabularies. an unknown *value* in a condition is a located error rather than a
        // silent false, which is the whole reason these are enumerated here instead of being whatever
        // string the triple happened to contain - `#[if: os == darwn]` has to be caught, or a platform
        // block vanishes without a word. The rule the manifest reader already lives by
        static const std::vector<std::string> &known_operating_systems();
        static const std::vector<std::string> &known_architectures();

        static bool is_known_operating_system(const std::string &name);
        static bool is_known_architecture(const std::string &name);

        // **the axes a condition may name**, and the whole of what routing on one costs a caller. closed,
        // so `#[if: cpu == arm64]` is caught rather than read as a flag called `cpu` that nobody defined -
        // which would be false, and silently so
        static bool is_axis(const std::string &name);
        static std::string axis_names();

        // `<axis> == <value>`, answered against these facts. false with a sentence in `out_error` when the
        // value is outside that axis's vocabulary - `#[if: os == darwn]` is a typo that would otherwise be
        // a silently vanishing block, and there is nothing else in the compiler that would ever notice.
        //
        // here rather than in Parser::ConditionalFilter, which owned it as three parallel ternaries on the
        // axis name plus a fourth spelling of the "expected one of" list: *what an axis is* is this type's
        // question, and a third one added there would have had to find all four
        bool axis_equals(
            const std::string &axis, const std::string &value, bool &out_match, std::string &out_error) const;

        // resolves the host's facts, then applies the overrides. an empty override means "keep the host's".
        //
        // false with a sentence in `out_error` when an override names something outside the vocabulary -
        // reported here rather than at the condition, because the mistake is on the command line and a
        // diagnostic pointing at a source file would be blaming the wrong text
        static bool resolve(
            const std::string &os_override,
            const std::string &arch_override,
            const std::vector<std::string> &defines,
            TargetFacts &out_facts,
            std::string &out_error
        );

        // the host's facts and nothing else, for a caller that legitimately has no command line to read -
        // the unit tests. One spelling of `resolve("", "", {})`, which cannot fail: every way it reports an
        // error is an override outside a vocabulary, and there are no overrides here.
        //
        // **not a default anywhere.** A parser that quietly resolved these when nobody configured it is what
        // let the manifest reader evaluate `#[if: os == ...]` against the wrong platform for as long as it
        // did: the facts were always present, so nothing was ever unset enough to notice
        static TargetFacts host();

        std::string operating_system;
        std::string architecture;

        // sorted, because this is folded into the module cache key and a key that depends on the order
        // two flags were typed in is a key that misses for no reason
        std::set<std::string> defines;

        // is `name` a flag this invocation declared? **false rather than an error when it is not** - a
        // condition has to be able to ask about a flag nobody set, or `#[if: TRACE]` could only ever be
        // written by someone who had already passed `--define TRACE`
        bool has_define(const std::string &name) const;

        // `os=darwin;arch=arm64;define=A;define=B;` - everything a condition can see, in one string, for
        // the module cache to fold. Here rather than in ModuleCache because *what the facts are* is this
        // type's question: a fact added without a line here would silently share a cache key with a build
        // that did not have it, which is the one cache failure with no diagnostic
        std::string cache_signature() const;
    };
};

#endif
