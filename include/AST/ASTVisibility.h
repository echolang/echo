#ifndef ASTVISIBILITY_H
#define ASTVISIBILITY_H

#pragma once

#include "AST/ASTDeclarationOrigin.h"
#include "Token.h"

#include <optional>
#include <string>

namespace AST
{
    // **the scope a declaration is reachable from**, and the whole of what a visibility modifier says.
    //
    // three levels on a declaration and one ladder: the file it was written in, the module that owns that
    // file, or every module. **the middle one is the default**, and that is the design decision the rest of
    // this follows from - a module's interface is something its author states rather than everything they
    // happened to write, and `public` is the word that states it. `internal` is then the redundant spelling,
    // accepted because a declaration that means it should be able to say so.
    //
    // the fourth value is the *member* axis. it is in the same enum rather than a second one because one
    // field carries both and the parser is what knows which: `private` on a method is `t_owner` and
    // `private` on a free function is `t_file`, told apart by position and nowhere else. held apart, every
    // reader of the flag would have to re-ask "is this a member" to know what it meant
    enum class Visibility
    {
        // `public`. every module. also the level of a *member* that says nothing - a member is reachable
        // exactly where the type that owns it is, and it is the type that carries that answer
        //
        // **the default of the field on every carrier**, so anything the compiler mints for itself - a
        // synthesized deinit, a copy constructor, a field-wise constructor, an instantiation - is reachable
        // from wherever it is referenced without anyone having to remember to say so
        t_public,

        // nothing written on a declaration, or `internal` written out. the module that declared it
        t_module,

        // `private` on a top-level declaration. the file that declared it
        t_file,

        // `private` on a property or a method. the type that declared it
        t_owner,
    };

    // which level a visibility keyword names, before a position narrows it. nothing for a token that is not
    // one, so a caller can ask it of any token
    //
    // deliberately *before* the narrowing: `private` names the file here and the owner type on a member, and
    // the two are told apart by where it was written - which is a question about a position, not a token
    std::optional<Visibility> visibility_of_token(Token::Type type);

    // the level a **top-level declaration** carries. `t_module` when nothing was written, which is the whole
    // of the default
    Visibility declaration_visibility(const std::optional<Visibility> &written);

    // and the level a **member** carries. `t_public` when nothing was written - a member has no restriction
    // of its own and is reached exactly where its type is, which is the type's answer to give. `t_module`
    // cannot arrive here: the parser refuses `internal` on a member, a member having no module axis
    Visibility member_visibility(const std::optional<Visibility> &written);

    // **may a site written in `from` name a declaration of this visibility declared in `origin`?**
    //
    // `t_owner` is not answered here. That axis asks about *types* and not about files, so it is
    // AST::can_reach_private_member, which already existed for a private property and already knows to go
    // through `template_or_self()`. Answering it here as well would be the second answer this file exists
    // to prevent - and it would need the enclosing type, which a file and a module cannot supply
    bool visible_from(Visibility visibility, const DeclarationOrigin &origin, const DeclarationOrigin &from);

    // the same rule, worded, or empty when there is nothing to refuse - the split
    // AST::const_receiver_refused / AST::const_receiver_refusal takes, and for its reason: the resolver
    // asks the question while it still has candidates to consider, and formatting a sentence per member
    // call in the program to test it for emptiness is what that split avoids
    //
    // `what` is the thing being named, already spelled the way the diagnostic wants it - a signature for a
    // function, a bare name for a type - because this cannot know which of those it was handed
    std::string visibility_refusal(
        Visibility visibility,
        const DeclarationOrigin &origin,
        const DeclarationOrigin &from,
        const std::string &what
    );
};

#endif
