#include "AST/ASTFunctionEmission.h"
#include "AST/FunctionDeclNode.h"

// the order of the arms is the whole content of this function, because the properties overlap: a
// `#[builtin:]` and a `#[intrinsic:]` may both be generic, and the monomorphizer instantiates them like
// anything else, so an instance carries `is_instantiated()` *and* whichever attribute its template had.
// Asking "was this generated?" first would answer t_odr_shared for `size_of<int32>` and emit a body for
// a function that has no symbol at all.
AST::FunctionEmission AST::function_emission_kind(const AST::FunctionDeclNode *decl)
{
    // a null decl is an unresolved call. It has no symbol by definition, and answering anything else
    // would turn a resolution failure into a link failure
    if (decl == nullptr) {
        return FunctionEmission::t_no_symbol;
    }

    if (decl->is_builtin() || decl->is_interface_requirement()) {
        return FunctionEmission::t_no_symbol;
    }

    // a template, as opposed to an instance of one: `is_generic()` is exactly a non-empty
    // `type_parameters`, and a clone clears it, so the two cannot be confused
    if (decl->is_generic()) {
        return FunctionEmission::t_no_symbol;
    }

    if (decl->is_extern()) {
        return FunctionEmission::t_extern_symbol;
    }

    if (decl->intrinsic.has_value()) {
        return FunctionEmission::t_intrinsic;
    }

    // the two ways a definition gets here without anybody writing it: the monomorphizer cloned it from a
    // template, or the parser / the ownership pass built it from a type's shape.
    //
    // `#[inline]` joins them by *asking* for the same treatment. It reaches this arm rather than an earlier
    // one because everything above has no body for us to copy anywhere - which is also why the stdlib
    // marking every intrinsic `#[inline]` is harmless rather than a contradiction
    if (decl->is_instantiated() || decl->is_implicitly_generated || decl->is_inline) {
        return FunctionEmission::t_odr_shared;
    }

    return FunctionEmission::t_module_local;
}

bool AST::emission_needs_declaration(AST::FunctionEmission kind)
{
    return kind != FunctionEmission::t_no_symbol;
}

bool AST::declaration_owes_a_body(const AST::FunctionDeclNode *decl)
{
    // a null decl is an unresolved call, and the resolution failure is the diagnostic there
    if (decl == nullptr) {
        return false;
    }

    // the same first arm function_emission_kind takes, and for the same reason: a builtin is
    // answered at its call sites and a requirement is answered by its implementors
    if (decl->is_builtin() || decl->is_interface_requirement()) {
        return false;
    }

    if (decl->is_extern() || decl->intrinsic.has_value()) {
        return false;
    }

    // no arm for `is_generic()`, deliberately - a template owes a body just like anything else
    return true;
}

bool AST::emission_has_body(AST::FunctionEmission kind)
{
    switch (kind) {
        // no body of ours. An extern and an intrinsic are still *declared*, which is a different
        // question - see emission_needs_declaration
        case FunctionEmission::t_no_symbol:
        case FunctionEmission::t_extern_symbol:
        case FunctionEmission::t_intrinsic:
            return false;

        case FunctionEmission::t_module_local:
        case FunctionEmission::t_odr_shared:
            return true;
    }

    return false;
}
