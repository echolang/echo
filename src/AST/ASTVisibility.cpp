#include "AST/ASTVisibility.h"

#include "AST/ASTCollector.h"
#include "AST/ASTFile.h"
#include "AST/ASTModule.h"

#include <fmt/core.h>

namespace
{
    // how a file and a module are named in a diagnostic. the file's *filename* rather than its path,
    // because a path is what the renderer already draws above the excerpt and because a golden must not
    // depend on which directory the corpus was run from
    //
    // beside the one function that words a refusal rather than beside the record they read, there being
    // nothing else in the compiler that spells an origin into English
    std::string describe_origin_file(const AST::DeclarationOrigin &origin)
    {
        if (origin.file == nullptr) {
            return "<unknown file>";
        }

        return origin.file->get_path().filename().string();
    }

    std::string describe_origin_module(const AST::DeclarationOrigin &origin)
    {
        if (origin.module == nullptr) {
            return "<unknown module>";
        }

        return origin.module->name;
    }
};

std::optional<AST::Visibility> AST::visibility_of_token(Token::Type type)
{
    switch (type) {
        case Token::Type::t_public:
            return AST::Visibility::t_public;

        // the level a declaration has anyway. accepted rather than refused because a module's surface is
        // worth saying out loud - it is the one line of a library a consumer reads
        case Token::Type::t_internal:
            return AST::Visibility::t_module;

        // the *declaration* reading of `private`. a member narrows it to t_owner, which is
        // AST::member_visibility's line - the position is the only thing that tells the two apart
        case Token::Type::t_private:
            return AST::Visibility::t_file;

        default:
            return std::nullopt;
    }
}

AST::Visibility AST::declaration_visibility(const std::optional<AST::Visibility> &written)
{
    return written.value_or(AST::Visibility::t_module);
}

AST::Visibility AST::member_visibility(const std::optional<AST::Visibility> &written)
{
    if (written == AST::Visibility::t_file) {
        return AST::Visibility::t_owner;
    }

    // `internal` on a member is the module axis the top level already has: this module, and no
    // further. a public type may have internal fields; a second module that can name the type still
    // cannot name those members
    if (written == AST::Visibility::t_module) {
        return AST::Visibility::t_module;
    }

    // a member with nothing written, or `public` written out, carries no restriction of its own: what bounds
    // it is the visibility of the type that owns it, which is that type's answer to give
    return AST::Visibility::t_public;
}

bool AST::visible_from(
    AST::Visibility visibility,
    const AST::DeclarationOrigin &origin,
    const AST::DeclarationOrigin &from
)
{
    switch (visibility) {
        case AST::Visibility::t_public:
            return true;

        // **an unknown origin on either side reaches everywhere.** the declaration side is a compiler-minted
        // one, which never carries a modifier anyway; the asking side is a site the walk could not place -
        // and refusing there would refuse a program for a reason the program cannot see or fix
        case AST::Visibility::t_module:
            return !origin.is_known() || !from.is_known() || origin.same_module(from);

        case AST::Visibility::t_file:
            return !origin.is_known() || !from.is_known() || origin.same_file(from);

        // not this function's axis - see the header. answering "yes" rather than asserting keeps a caller
        // that has not split the two arms from silently refusing every private member in the program
        case AST::Visibility::t_owner:
            return true;
    }

    return true;
}

std::string AST::visibility_refusal(
    AST::Visibility visibility,
    const AST::DeclarationOrigin &origin,
    const AST::DeclarationOrigin &from,
    const std::string &what
)
{
    if (AST::visible_from(visibility, origin, from)) {
        return "";
    }

    // **the wording names the word that is missing, not one that is wrong**, because at this level nothing
    // was written: a declaration belongs to its own module unless its author said otherwise, so what the
    // reader is looking for is a `public` that is absent
    if (visibility == AST::Visibility::t_module) {
        return fmt::format(
            "'{}' is internal to the module '{}', so the module '{}' cannot name it. Write 'public' on the "
            "declaration to make it part of what '{}' offers.",
            what, describe_origin_module(origin), describe_origin_module(from),
            describe_origin_module(origin));
    }

    return fmt::format(
        "'{}' is private to '{}', so it can only be named in that file. Remove the 'private' to reach it "
        "from the rest of its module, or write 'public' to reach it from anywhere.",
        what, describe_origin_file(origin));
}

void AST::refuse_invisible_property(
    Collector &collector,
    const CodeRef &at,
    Visibility visibility,
    const DeclarationOrigin &origin,
    const DeclarationOrigin &from,
    const std::string &what,
    const std::optional<TokenReference> &declaration_token
)
{
    // not this function's axis. t_owner is AST::can_reach_private_member; t_public and t_file
    // are not a property's module-axis question
    if (visibility != Visibility::t_module) {
        return;
    }

    if (visible_from(visibility, origin, from)) {
        return;
    }

    collector.collect_issue<Issue::InaccessibleDeclaration>(
        at, visibility_refusal(visibility, origin, from, what), origin, declaration_token);
}
