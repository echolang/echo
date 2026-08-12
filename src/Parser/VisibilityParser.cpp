#include "Parser/VisibilityParser.h"

#include "Parser/FuncDeclParser.h"
#include "Parser/OperatorDeclParser.h"
#include "Parser/TestDeclParser.h"
#include "Parser/TypeDeclParser.h"
#include "Parser/TypeParser.h"

#include "AST/ASTIssue.h"
#include "AST/TypeDeclNode.h"

#include <fmt/core.h>

const char *const Parser::k_operator_visibility_refusal =
    "An operator's symbol is global wherever it is declared - one entry in one table that every module "
    "reads - so there is no scope for it to be narrowed to.";

const char *const Parser::k_test_visibility_refusal =
    "A test is in no overload set and under no name any program can write, so there is nobody for a "
    "visibility modifier to be describing.";

Parser::VisibilityPrefix Parser::parse_visibility_prefix(
    Parser::Payload &payload,
    Parser::VisibilityPosition position
)
{
    auto &cursor = payload.cursor;

    // the level this position means when nothing is written. **a top-level declaration belongs to its own
    // module and a member to whatever its type does** - the same absence read two ways, which is why the
    // position is an argument rather than something a caller applies afterwards
    const AST::Visibility fallback = position == VisibilityPosition::t_member
        ? AST::member_visibility(std::nullopt)
        : AST::declaration_visibility(std::nullopt);

    VisibilityPrefix prefix;
    prefix.value = fallback;

    if (cursor.is_done()) {
        return prefix;
    }

    const std::optional<AST::Visibility> written = AST::visibility_of_token(cursor.current().type());

    if (!written.has_value()) {
        return prefix;
    }

    prefix.token.emplace(cursor.current());
    cursor.skip();

    // a modifier written inside a body. **consumed before it is refused**, and that is the whole reason
    // this is one function: the declaration pass and the body pass both walk a block, so a modifier one of
    // them skipped and the other did not would leave the two walks on different tokens - and the trap that
    // makes is silent, since both walks then still reach *a* declaration
    if (position == VisibilityPosition::t_block) {
        refuse_visibility_prefix(
            payload,
            prefix,
            "A declaration inside a body is already reachable from that block and from nowhere else, so "
            "there is nothing for a modifier to narrow.");

        return VisibilityPrefix { std::nullopt, fallback };
    }

    if (position == VisibilityPosition::t_member) {
        // `internal` is the module axis, and a member does not sit on one: what another module may reach is
        // the *type*, and a member of a reachable type is reachable with it
        if (written == AST::Visibility::t_module) {
            refuse_visibility_prefix(
                payload,
                prefix,
                "A member has no module of its own - it is reachable exactly where the type that owns it "
                "is. Write 'internal' on the type instead, or 'private' here to keep the member inside "
                "its own type.");

            return VisibilityPrefix { std::nullopt, fallback };
        }

        prefix.value = AST::member_visibility(written);
        return prefix;
    }

    prefix.value = AST::declaration_visibility(written);
    return prefix;
}

Parser::VisibilityPrefix Parser::consume_declaration_visibility(
    Parser::Payload &payload,
    const std::optional<TokenReference> &block_token
)
{
    const VisibilityPrefix prefix = parse_visibility_prefix(
        payload,
        block_token.has_value() ? VisibilityPosition::t_block : VisibilityPosition::t_declaration);

    if (!prefix.was_written()) {
        return prefix;
    }

    if (starts_operatordecl(payload.cursor)) {
        refuse_visibility_prefix(payload, prefix, k_operator_visibility_refusal);
    }
    else if (starts_testdecl(payload.cursor)) {
        refuse_visibility_prefix(payload, prefix, k_test_visibility_refusal);
    }
    else if (!starts_funcdecl(payload.cursor) && !starts_typedecl(payload.cursor)
        && !starts_constdecl(payload) && !payload.cursor.is_type(Token::Type::t_extern)) {
        refuse_visibility_prefix(
            payload,
            prefix,
            "A visibility modifier says who may name a declaration, and this is not one.");
    }

    return prefix;
}

bool Parser::refuse_visibility_prefix(
    Parser::Payload &payload,
    const Parser::VisibilityPrefix &prefix,
    std::string_view reason
)
{
    if (!prefix.was_written()) {
        return false;
    }

    const TokenReference &at = prefix.token.value();

    payload.collector.collect_issue<AST::Issue::GenericError>(
        payload.context.code_ref(at),
        fmt::format("'{}' cannot be written here. {}", at.value(), reason));

    return true;
}

void Parser::refuse_invisible_type(
    Parser::Payload &payload,
    const AST::TypeDeclNode &decl,
    const TokenReference &at
)
{
    const AST::DeclarationOrigin from = AST::origin_at(payload.context);
    const AST::ComplexType &layout = decl.complex_type();

    // asked before the sentence is worded, which is what the AST::visible_from / AST::visibility_refusal
    // split is for: this runs at every type name in the program, and every one of them would otherwise
    // pay for a name copy and a format to test the answer for emptiness
    if (AST::visible_from(layout.visibility, layout.declared_in, from)) {
        return;
    }

    payload.collector.collect_issue<AST::Issue::InaccessibleDeclaration>(
        payload.context.code_ref(at),
        AST::visibility_refusal(layout.visibility, layout.declared_in, from, decl.type_name()),
        layout.declared_in,
        decl.name_token);
}
