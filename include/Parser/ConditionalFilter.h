#ifndef CONDITIONALFILTER_H
#define CONDITIONALFILTER_H

#pragma once

#include "Compiler/TargetFacts.h"
#include "Token.h"

#include <string>
#include <vector>

namespace Parser
{
    // **the sole owner of conditional compilation**, and the whole of it.
    //
    // runs between lexing and pass 1, on a file's tokens, and drops the regions whose condition does not
    // hold:
    //
    //     #[if: os == darwin]  ...  #[elif: os == linux]  ...  #[else]  ...  #[end]
    //
    // A *token* filter rather than a rule in the parsers, and that is what makes the feature cheap. None
    // of the three passes is touched. Pass 1 registers type names and does not read attributes at all.
    // Passes 2 and 3 would each have to drop a declaration identically or `claim_declaration_site`
    // breaks. And an `extern` block accepts nothing but `function` and has no attribute plumbing.
    //
    // A filter needs none of that, and it works uniformly at file level, inside a struct body, inside a
    // function body, around an `extern` block and around a `namespace` statement.
    //
    // it is also the only mechanism that lets an excluded region name things that **do not exist here** -
    // `_NSGetExecutablePath` on Linux, `GetModuleFileNameW` on Darwin. Nothing parses it, so nothing has
    // to resolve it. The price is C's price: a typo inside a region for another platform is invisible on
    // this one, which is what `--target-os` exists to let a test go looking for.
    //
    // locations survive, because tokens are dropped rather than text - each token already carries its own
    // line, so a diagnostic after a filtered region still names the line the author sees in the file.

    // the directive names, spelled once.
    //
    // syntactically these are `#[...]` attributes, so the lexer needs no arm for them - but two of the
    // four are *keywords* rather than identifiers (`if` is `t_if`, `else` is `t_else`), which is why
    // Parser::parse_attribute could never have read them anyway: it wants an identifier for the name. They
    // never reach it regardless, because this filter consumes them
    namespace ConditionalDirective
    {
        constexpr const char *k_if = "if";
        constexpr const char *k_elif = "elif";
        constexpr const char *k_else = "else";
        constexpr const char *k_end = "end";

        // is `name` one of the four? asked by AST::is_known_attribute, so the guard that catches
        // `#[fi: ...]` does not also report a directive this filter has already dealt with
        bool is_directive(const std::string &name);
    };

    // rewrites the tokens from `from` onwards, keeping only what the conditions admit.
    //
    // A range rather than a whole collection, because **every file in a module shares one
    // TokenCollection** and a `TokenizedFile` is a slice into it.
    //
    // `from` is always the index the file being lexed started at, so the range is the tail, and erasing
    // inside it cannot move an earlier file's slice. It is measured *after* this runs - see
    // AST::Module::tokenize, which is the one place that ordering is owned.
    //
    // false with a `line N: ...` sentence in `out_error` on any of: an unknown axis, an unknown value for
    // a known axis, a malformed condition, `#[elif]` or `#[else]` after an `#[else]`, an `#[elif]` /
    // `#[else]` / `#[end]` with no open `#[if]`, or an `#[if]` left unterminated at end of file.
    //
    // **every one of those is an error rather than a no-op**, for the reason the manifest reader refuses
    // an unknown attribute: a region that silently vanishes is the one failure mode this feature could
    // have that nothing else in the compiler would catch.
    bool filter_conditional_tokens(
        TokenCollection &tokens,
        size_t from,
        const Compiler::TargetFacts &facts,
        std::string &out_error);
};

#endif
