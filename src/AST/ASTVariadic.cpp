#include "AST/ASTVariadic.h"

#include "AST/ASTArrayLiteral.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"

#include <fmt/core.h>

bool AST::is_variadic_args(const AST::ValueType &type, const AST::CoreTypes &core)
{
    // **the kind before the binding**, which is the cheap half of the question: the marker is a struct,
    // so nothing else can be it, and this is asked once per argument in two per-argument loops. what
    // follows is two map probes and a ValueType copy, and neither belongs on every integer literal
    if (!type.is_struct() || !core.has(AST::CoreTypeKind::t_variadic_args)) {
        return false;
    }

    // `const` and the borrow are peeled before the compare, so every spelling of the marker is
    // recognised as the marker - a `const variadic_args&` is refused *as* one, in the position that
    // refuses it, rather than sliding through as an ordinary struct
    return AST::ValueType::make_mutable(type) == core.type(AST::CoreTypeKind::t_variadic_args);
}

AST::ArrayLiteralExprNode *AST::variadic_pack_of(AST::ExprNode *expr)
{
    AST::ArrayLiteralExprNode *literal = array_literal_of(expr);

    return literal != nullptr && literal->is_variadic_pack ? literal : nullptr;
}

bool AST::has_variadic_tail(const AST::FunctionDeclNode &decl, const AST::CoreTypes &core)
{
    if (decl.args.empty()) {
        return false;
    }

    const AST::VarDeclNode *last = decl.args.back();

    return last != nullptr && last->has_type() && is_variadic_args(last->type(), core);
}

std::optional<std::string> AST::variadic_args_refusal(
    const AST::FunctionDeclNode &decl,
    const AST::CoreTypes &core
)
{
    // a constructor is named after its type and "returns" it, so the marker's own declaration would
    // otherwise refuse itself. it has no parameters to check either
    if (decl.is_constructor() || decl.is_destructor()) {
        return std::nullopt;
    }

    // **the spelling is read inside each refusal, not before them.** this is asked of every declaration
    // in the program, and CoreTypes::spelling builds its answer out of the namespace path - so hoisting
    // it is a string built and thrown away for every function that is not doing anything wrong
    if (decl.return_type != nullptr && is_variadic_args(decl.return_type->type, core)) {
        return fmt::format(
            "'{}' cannot be returned - it is not a value, it is how a declaration says its last "
            "parameter is a C variadic tail.",
            core.spelling(AST::CoreTypeKind::t_variadic_args));
    }

    for (size_t index = 0; index < decl.args.size(); index++) {
        const AST::VarDeclNode *arg = decl.args[index];

        if (arg == nullptr || !arg->has_type() || !is_variadic_args(arg->type(), core)) {
            continue;
        }

        // an extern is the only thing there is a C convention to bridge *to*. an Echo function has
        // a signature this compiler emits, and there is nothing on the other side of it asking for
        // one - so the answer names what the author probably wanted instead
        if (!decl.is_extern()) {
            return fmt::format(
                "'{}' is only allowed on an 'extern' declaration - it describes C's calling "
                "convention, and an Echo function has no variable argument list. Take an "
                "'array<T>' or a 'slice<T>' instead.",
                core.spelling(AST::CoreTypeKind::t_variadic_args));
        }

        if (index + 1 != decl.args.size()) {
            return fmt::format(
                "'{}' must be the last parameter - C's variadic tail is what follows the arguments "
                "it can name, so nothing can follow it.",
                core.spelling(AST::CoreTypeKind::t_variadic_args));
        }

        // C requires at least one named parameter before the ellipsis, `va_start` needing something
        // to start *from*. so does this, for the same reason and with a better error
        if (index == 0) {
            return fmt::format(
                "'{}' needs at least one parameter before it - C has no way to find the start of a "
                "variadic tail that follows nothing.",
                core.spelling(AST::CoreTypeKind::t_variadic_args));
        }
    }

    return std::nullopt;
}

AST::ValueType AST::variadic_promotion_of(const AST::ValueType &type)
{
    if (!type.is_primitive()) {
        return type;
    }

    switch (type.get_primitive_type()) {
        case AST::ValueTypePrimitive::t_float32:
            return AST::ValueType(AST::ValueTypePrimitive::t_float64);

        case AST::ValueTypePrimitive::t_bool:
        case AST::ValueTypePrimitive::t_int8:
        case AST::ValueTypePrimitive::t_int16:
            return AST::ValueType(AST::ValueTypePrimitive::t_int32);

        case AST::ValueTypePrimitive::t_uint8:
        case AST::ValueTypePrimitive::t_uint16:
            return AST::ValueType(AST::ValueTypePrimitive::t_uint32);

        default:
            return type;
    }
}

std::optional<std::string> AST::variadic_argument_refusal(const AST::ValueType &type)
{
    if (type.is_void()) {
        return std::string(
            "a variadic argument has to be a value, and 'void' is not one.");
    }

    if (type.is_primitive() || type.is_pointer()) {
        return std::nullopt;
    }

    if (type.is_interface() || type.is_callable()) {
        return fmt::format(
            "'{}' has no C spelling - it is two words the other side has no declaration for.",
            type.get_type_desciption());
    }

    if (type.is_class() || type.is_weak()) {
        return fmt::format(
            "'{}' is a reference counted handle, and passing one across a C variadic boundary hands "
            "out its address with nothing holding a reference. Pass what C wants instead.",
            type.get_type_desciption());
    }

    return fmt::format(
        "'{}' is a struct, and how C's variadic convention unpacks one is platform specific. Pass "
        "the fields it wants, or a pointer to it.",
        type.get_type_desciption());
}
