#ifndef CONDITIONALFILTER_H
#define CONDITIONALFILTER_H

#pragma once

#include "Compiler/TargetFacts.h"
#include "Token.h"

#include <string>
#include <vector>

namespace Parser
{
    // conditional compilation, between lexing and pass 1. drops regions whose condition does
    // not hold (`#[if:]` / `#[elif:]` / `#[else]` / `#[end]`) and, when `facts.tests` is false,
    // `test <name> { ... }` with the attributes above it. a token filter so an excluded region
    // may name a symbol this platform has not got; a leftover `#[group:]` would attach in silence

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

        // is `name` one of the four? asked so the guard that catches `#[fi: ...]` does not also
        // report a directive this filter has already dealt with. directives never reach
        // AST::attribute_admission
        bool is_directive(const std::string &name);
    };

    // keep tokens from `from` that the conditions admit. a range, because files share one
    // TokenCollection. a malformed directive is an error, not a silent drop
    bool filter_conditional_tokens(
        TokenCollection &tokens,
        size_t from,
        const Compiler::TargetFacts &facts,
        std::string &out_error);
};

#endif
