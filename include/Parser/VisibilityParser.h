#ifndef VISIBILITYPARSER_H
#define VISIBILITYPARSER_H

#pragma once

#include "AST/ASTVisibility.h"
#include "Parser/ParserPayload.h"

#include <optional>
#include <string_view>

namespace AST
{
    class TypeDeclNode;
};

namespace Parser
{
    // where a visibility modifier was read, which decides what it is allowed to say. a `private` means the
    // *file* on a top-level declaration and the *owner type* on a member, and nothing but the position
    // tells the two apart - so the position is asked once, here, rather than re-derived by everything that
    // later reads the flag
    enum class VisibilityPosition
    {
        // file or namespace scope. all three keywords are legal, and nothing written means the module -
        // see AST::declaration_visibility
        t_declaration,

        // a struct, class or interface body. `private` narrows to Visibility::t_owner, `public` is the
        // level a member has anyway, and `internal` is refused: a member has no module axis, being
        // reachable exactly where the type that owns it is
        t_member,

        // inside a `{ }` block - a function body, or a lexical block at file scope. no modifier is legal:
        // a declaration there is already reachable from that block and nowhere else
        t_block,
    };

    // a visibility modifier as it was written, or nothing.
    //
    // the **token** is kept and not only the level, for the two things a consumer needs it for: refusing the
    // modifier at the word that is wrong rather than at the declaration behind it, and answering "was one
    // written at all" - a different question from `value`, since `internal` and nothing are the same level
    // but not the same source
    struct VisibilityPrefix
    {
        std::optional<TokenReference> token;
        AST::Visibility value = AST::Visibility::t_public;

        bool was_written() const {
            return token.has_value();
        }
    };

    // **reads a leading `public`, `internal` or `private`, or nothing at all.**
    //
    // consumed at the head of a dispatch loop and *before* the loop asks what kind of declaration follows,
    // which is the load-bearing part of this whole feature: `public const int32 $x;` is a property,
    // `public const MAX = 5;` is a constant and `public const function f()` is a const method, and the
    // three are told apart by the existing partition of starts_const_if / starts_constdecl /
    // starts_vardecl / starts_funcdecl - all four of which scan from the head of the statement. Teaching
    // each of them to skip a modifier would be four copies of one piece of arithmetic, and the first one
    // to disagree with the others silently reclassifies a declaration
    //
    // refuses what the position does not allow and hands back that position's default, so a refused
    // modifier costs one diagnostic and the declaration behind it still parses
    VisibilityPrefix parse_visibility_prefix(Payload &payload, VisibilityPosition position);

    // **the same thing at the head of a top-level dispatch loop, read *and* gated.** the declaration pass
    // and the body pass both walk a file root and both walk a block, and the two have to consume the same
    // tokens and report the same refusals - a diagnostic that appears or not depending on which pass
    // reached the statement first is what a second copy of this would be.
    //
    // the gate is a whitelist rather than a refusal in each arm that cannot take a modifier, the arms that
    // *can* being three: a visibility modifier says who may name a declaration, and an operator is the one
    // shape that is a declaration and still takes none - so it says why. `block_token` is the position:
    // inside a `{ }` no modifier is legal at all, which parse_visibility_prefix answers for itself
    VisibilityPrefix consume_declaration_visibility(
        Payload &payload,
        const std::optional<TokenReference> &block_token
    );

    // refuses a prefix that reached something with no visibility axis at all - an operator, an interface
    // requirement, a nested type, a statement that is not a declaration.
    //
    // separate from the reader above because these are decided *after* the dispatch: a loop does not know
    // it is looking at an operator until starts_operatordecl says so, and by then the modifier is gone. the
    // caller supplies the whole `reason` sentence rather than a shape name, because each of these is
    // refused for a different reason and a shape name alone reads as an arbitrary rule
    //
    // false when there was nothing to refuse, so a caller can read it as "was this rejected"
    bool refuse_visibility_prefix(Payload &payload, const VisibilityPrefix &prefix, std::string_view reason);

    // **an operator takes no modifier, wherever it is written**, and the sentence is shared by the three
    // dispatch loops rather than spelled at each: an operator declaration reaches a struct body, file scope
    // and a namespace, and a reason worded three ways is three rules as far as a reader is concerned
    extern const char *const k_operator_visibility_refusal;

    // **and a test takes none either**, for a stronger version of the same reason. Spelled beside the
    // operator's because the two are the shapes the whitelist above has to name by hand, and a reader
    // comparing them should find them in one place
    extern const char *const k_test_visibility_refusal;

    // **may the file being parsed name this type?** asked at every site that turns a type *name* into a
    // declaration, which is three: Parser::parse_value_type's own lookup, the head of an `Owner::Nested`
    // chain, and a generic constraint atom.
    //
    // asked here, while parsing, rather than in a later pass, because a type name is resolved as it is read
    // and nothing downstream keeps the token it was written at. That works because a type's visibility is
    // settled a pass earlier, in Parser::parse_type_names - see ComplexType::visibility.
    //
    // **it reports and the caller carries on with the type it found.** An unresolved unqualified type name
    // is silently `unknown` in this parser, so refusing to hand the declaration back would trade one exact
    // diagnostic for a cascade of wrong layouts underneath it
    void refuse_invisible_type(Payload &payload, const AST::TypeDeclNode &decl, const TokenReference &at);
};

#endif
