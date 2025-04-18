#include "Compiler/LLVM/Codegen/ExprCodegen.h"
#include "Compiler/LLVM/Codegen/IfaceValue.h"

#include "AST/ASTConformance.h"
#include "Compiler/LLVM/Codegen/ClassLayout.h"
#include "Compiler/LLVM/Codegen/ClassCodegen.h"
#include "Compiler/LLVM/Codegen/AbortCodegen.h"
#include "eco.h"
#include "Compiler/LLVM/Codegen/LValueCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/VarDeclNode.h"
#include "AST/VarRefNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/VarNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/AssignNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ExprNode.h"
#include "AST/TypeCastNode.h"
#include "AST/ReturnNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ASTBuiltin.h"
#include "AST/IfStatementNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/NullNode.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>

#include <fmt/core.h>

#include <cassert>
#include <stdexcept>
#include <vector>

namespace Compiler::LLVM
{

// how `echo` prints one value: the printf conversion, and the primitive the value has to be
// widened to before it is handed to a variadic call. both halves sit in one row because C's
// default argument promotions are part of the conversion rather than a step after it - printf
// reads exactly what the format says, so `%d` against a raw i8 reads four bytes where one was
// passed, and `%f` against a raw float reads a double. only the AArch64 backend performs those
// promotions on our behalf, which is why passing the value raw looked correct on arm64 macOS and
// printed the untruncated int (300 for an int8 holding 44) and 0.000000 for a float32 on x86-64
//
// the promoted type is chosen to agree with the format exactly - unsigned widens to uint32 under
// `%u` rather than to the int32 C's integer promotion would strictly give - so the two cannot
// drift apart. a table rather than an if-cascade so that adding a primitive is one row and cannot
// silently reuse a neighbouring width: `usize`/`isize` print as their concrete 64 bit width, which
// is what ECO_TARGET_POINTER_SIZE says on every target wired up today
struct EchoConversion
{
    // null for a type echo has no conversion for
    const char *format;
    AST::ValueTypePrimitive promoted;
};

static EchoConversion printf_conversion_for(const AST::ValueType &type)
{
    constexpr EchoConversion unsupported{nullptr, AST::ValueTypePrimitive::t_void};

    if (!type.is_primitive()) {
        return unsupported;
    }

    switch (type.get_primitive_type()) {
        case AST::ValueTypePrimitive::t_int8:
        case AST::ValueTypePrimitive::t_int16:
        case AST::ValueTypePrimitive::t_int32:
        case AST::ValueTypePrimitive::t_bool:
            return {"%d\n", AST::ValueTypePrimitive::t_int32};

        case AST::ValueTypePrimitive::t_int64:
        case AST::ValueTypePrimitive::t_isize:
            return {"%lld\n", AST::ValueTypePrimitive::t_int64};

        case AST::ValueTypePrimitive::t_uint8:
        case AST::ValueTypePrimitive::t_uint16:
        case AST::ValueTypePrimitive::t_uint32:
            return {"%u\n", AST::ValueTypePrimitive::t_uint32};

        case AST::ValueTypePrimitive::t_uint64:
        case AST::ValueTypePrimitive::t_usize:
            return {"%llu\n", AST::ValueTypePrimitive::t_uint64};

        case AST::ValueTypePrimitive::t_float32:
        case AST::ValueTypePrimitive::t_float64:
            return {"%f\n", AST::ValueTypePrimitive::t_float64};

        default:
            return unsupported;
    }
}
void ExprCodegen::gen_type_cast(AST::TypeCastNode &node)
{
    // visit the expression
    node.expr->accept(*_ctx.visitor);

    auto value = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    // asked once each rather than five times: result_type() builds its answer, and for a pointer
    // it heap-allocates the pointee
    const AST::ValueType from = node.expr->result_type();
    const AST::ValueType to = node.result_type();

    // narrowing a nullable pointer to a borrow asserts the thing a borrow promises. under
    // opaque pointers the reinterpretation itself is free, so the check is all there is to emit
    if (to.is_pointer() && !to.is_nullable() && from.is_pointer() && from.is_nullable()) {
        gen_null_assert(value);
    }

    // the conversion table lives on TypeLowering, shared with every declaration, assignment
    // and member write, so all of them agree on signedness
    _ctx.value_stack.push(_ctx.types->coerce_value(value, from, to, *_ctx.current_cmp_unit));
}

void ExprCodegen::gen_var_ref(AST::VarRefNode &node)
{
    // gen_lvalue, not gen_place: any auto-deref this read needs is already an explicit
    // DerefExprNode above it, put there by the pointer adjustment pass. so a bare pointer
    // variable here means the pointer itself was asked for - which is what `$p:$` compiles to
    _ctx.value_stack.push(_ctx.lvalues->gen_load(node, node.is_var() ? node.get_var().decl().name().c_str() : "load"));
}

void ExprCodegen::gen_literal_float(AST::LiteralFloatExprNode &node)
{
    if (node.get_effective_primitive_type() == AST::ValueTypePrimitive::t_float64) {
        _ctx.value_stack.push(llvm::ConstantFP::get(*_ctx.llvm_context, llvm::APFloat(node.double_value())));
    } else {
        _ctx.value_stack.push(llvm::ConstantFP::get(*_ctx.llvm_context, llvm::APFloat(node.float_value())));
    }
}

void ExprCodegen::gen_literal_int(AST::LiteralIntExprNode &node)
{
    auto type = node.result_type().get_primitive_type();
    auto value = node.uint64_value();

    auto int_size = AST::get_integer_size(type);

    // push an integer constant on the stack
    _ctx.value_stack.push(llvm::ConstantInt::get(*_ctx.llvm_context, llvm::APInt(int_size.size * 8, value, int_size.is_signed)));
}

void ExprCodegen::gen_literal_bool(AST::LiteralBoolExprNode &node)
{
    if (node.get_bool_value()) {
        _ctx.value_stack.push(llvm::ConstantInt::getTrue(*_ctx.llvm_context));
    } else {
        _ctx.value_stack.push(llvm::ConstantInt::getFalse(*_ctx.llvm_context));
    }
}

void ExprCodegen::gen_literal_string(AST::LiteralStringExprNode &node)
{
    const std::string &bytes = node.get_string_value();

    // the bytes themselves, always: a private NUL-terminated global. the NUL costs one byte and buys
    // `->c_str()` for free on a literal, which is the whole C-interop story for one
    llvm::Value *byte_pointer = _ctx.builder->CreateGlobalStringPtr(bytes, "str");

    const AST::ValueType type = node.result_type();

    // no stdlib, no `string` type - the literal stays the bare address it has always been. this is the
    // path that compiles `stdlib/core/string.eco` itself. asks for a *layout*, since what follows
    // GEPs the window out of it
    if (!type.has_property_layout()) {
        _ctx.push(byte_pointer);
        return;
    }

    // resolved once by compile_bundle, which is also where a malformed stdlib string is reported
    const AST::CoreStringLayout &layout = _ctx.core_string_layout();

    auto *string_struct = llvm::cast<llvm::StructType>(_ctx.types->get_llvm_type(type, *_ctx.current_cmp_unit));
    auto *view_struct = llvm::cast<llvm::StructType>(string_struct->getElementType(layout.window_index));

    // **the whole point of this function.** a literal is a *constant* of the string type, not a call to
    // one of its constructors: the bytes are already in the binary, the size is known here, and the
    // owner is null - so nothing is allocated and nothing is retained. a null owner is also what makes
    // the retain and release the ownership pass wraps this value in no-ops, since both are null-guarded
    std::vector<llvm::Constant *> view_fields(2);
    view_fields[layout.bytes_index] = llvm::cast<llvm::Constant>(byte_pointer);
    view_fields[layout.size_index] =
        llvm::ConstantInt::get(view_struct->getElementType(layout.size_index), bytes.size());

    std::vector<llvm::Constant *> string_fields(2);
    string_fields[layout.window_index] = llvm::ConstantStruct::get(view_struct, view_fields);
    string_fields[layout.owner_index] =
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(string_struct->getElementType(layout.owner_index)));

    _ctx.push(llvm::ConstantStruct::get(string_struct, string_fields));
}

void ExprCodegen::gen_binary_expr(AST::BinaryExprNode &node)
{
    node.lhs->accept(*_ctx.visitor);
    node.rhs->accept(*_ctx.visitor);

    // raw, not collapsed to the pointee: after the adjustment pass a pointer here means the
    // address really was asked for, through `:$`, and the pointer arm below needs to see it
    auto lhsret = node.lhs->result_type();
    auto rhsret = node.rhs->result_type();

    auto right = _ctx.value_stack.top();
    _ctx.value_stack.pop();
    auto left = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    // two class handles, or a handle against null. the only operators a class answers, and the type
    // checker has already rejected the rest - so this is a plain address comparison over two opaque
    // pointers, ahead of the pointer arm because a class type is not a t_pointer
    if (lhsret.is_class() || rhsret.is_class()) {
        // which operators those are is Operator::is_identity_comparison, the same predicate the type
        // checker rejects the rest with - so the two passes read the rule off one function rather than
        // each enumerating it
        if (!node.op_node->op->is_identity_comparison()) {
            throw _ctx.error(fmt::format(
                "unsupported binary operator '{}' for operands '{}' and '{}' {}",
                node.op_node->token_literal.value(), lhsret.get_type_desciption(),
                rhsret.get_type_desciption(), _ctx.function_context()));
        }

        _ctx.value_stack.push(node.op_node->op->type == Token::Type::t_logical_eq
            ? _ctx.builder->CreateICmpEQ(left, right)
            : _ctx.builder->CreateICmpNE(left, right));
        return;
    }

    // everything reached through `:$` operates on the address itself: comparisons ask about
    // identity, and arithmetic is scaled by the pointee's size, never by bytes
    if (lhsret.is_pointer() || rhsret.is_pointer()) {
        const bool both_pointers = lhsret.is_pointer() && rhsret.is_pointer();

        switch (node.op_node->op->type) {
            case Token::Type::t_logical_eq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpEQ(left, right));
                return;
            case Token::Type::t_logical_neq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpNE(left, right));
                return;
            case Token::Type::t_open_angle:
                _ctx.value_stack.push(_ctx.builder->CreateICmpULT(left, right));
                return;
            case Token::Type::t_close_angle:
                _ctx.value_stack.push(_ctx.builder->CreateICmpUGT(left, right));
                return;
            case Token::Type::t_logical_leq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpULE(left, right));
                return;
            case Token::Type::t_logical_geq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpUGE(left, right));
                return;

            case Token::Type::t_op_add:
            case Token::Type::t_op_sub:
            {
                if (both_pointers) {
                    if (node.op_node->op->type != Token::Type::t_op_sub) {
                        throw _ctx.error(fmt::format("two addresses cannot be added {}",
                            _ctx.function_context()));
                    }

                    // the distance between two addresses, counted in elements
                    _ctx.value_stack.push(_ctx.builder->CreatePtrDiff(
                        _ctx.types->get_llvm_type(AST::value_type_of(lhsret), *_ctx.current_cmp_unit),
                        left, right));
                    return;
                }

                // GEP over the pointee type does the scaling, so the offset counts elements
                llvm::Value *offset = right;
                if (node.op_node->op->type == Token::Type::t_op_sub) {
                    offset = _ctx.builder->CreateNeg(offset);
                }

                _ctx.value_stack.push(_ctx.builder->CreateGEP(
                    _ctx.types->get_llvm_type(AST::value_type_of(lhsret), *_ctx.current_cmp_unit),
                    left, { offset }, "ptroff"));
                return;
            }

            default:
                throw _ctx.error(fmt::format("unsupported binary operator '{}' for operands '{}' and '{}' {}",
                    node.op_node->token_literal.value(), lhsret.get_type_desciption(),
                    rhsret.get_type_desciption(), _ctx.function_context()));
        }
    }

    if (lhsret.is_integer_type() && rhsret.is_integer_type()) {
        switch (node.op_node->op->type) {
            case Token::Type::t_op_add:
                _ctx.value_stack.push(_ctx.builder->CreateAdd(left, right));
                break;
            case Token::Type::t_op_sub:
                _ctx.value_stack.push(_ctx.builder->CreateSub(left, right));
                break;
            case Token::Type::t_op_mul:
                _ctx.value_stack.push(_ctx.builder->CreateMul(left, right));
                break;
            case Token::Type::t_op_div:
                _ctx.value_stack.push(_ctx.builder->CreateSDiv(left, right));
                break;
            case Token::Type::t_op_mod:
                _ctx.value_stack.push(_ctx.builder->CreateSRem(left, right));
                break;
            case Token::Type::t_op_pow:
                {
                    // im kinda just copying the behavior of C with clang here
                    // cast all values to double and then call the pow intrinsic
                    // cast the result back to the original type
                    std::vector<llvm::Type *> arg_type;
                    arg_type.push_back(llvm::Type::getDoubleTy(*_ctx.llvm_context));
                    arg_type.push_back(llvm::Type::getDoubleTy(*_ctx.llvm_context));

                    llvm::Function *fun = llvm::Intrinsic::getDeclaration(_ctx.current_module(), llvm::Intrinsic::pow, arg_type);
                    std::vector<llvm::Value *> args;
                    args.push_back(_ctx.builder->CreateSIToFP(left, llvm::Type::getDoubleTy(*_ctx.llvm_context)));
                    args.push_back(_ctx.builder->CreateSIToFP(right, llvm::Type::getDoubleTy(*_ctx.llvm_context)));

                    llvm::Value *result = _ctx.builder->CreateCall(fun, args);
                    _ctx.value_stack.push(_ctx.builder->CreateFPToSI(result, llvm::Type::getInt32Ty(*_ctx.llvm_context)));
                }
                break;
            case Token::Type::t_logical_eq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpEQ(left, right));
                break;
            case Token::Type::t_logical_neq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpNE(left, right));
                break;
            case Token::Type::t_close_angle:
                _ctx.value_stack.push(_ctx.builder->CreateICmpSGT(left, right));
                break;
            case Token::Type::t_open_angle:
                _ctx.value_stack.push(_ctx.builder->CreateICmpSLT(left, right));
                break;
            case Token::Type::t_logical_geq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpSGE(left, right));
                break;
            case Token::Type::t_logical_leq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpSLE(left, right));
                break;
            default:
                throw _ctx.error(fmt::format("unsupported binary operator '{}' for operands '{}' and '{}' {}",
                    node.op_node->token_literal.value(), lhsret.get_type_desciption(),
                    rhsret.get_type_desciption(), _ctx.function_context()));
        }
    }
    else if (lhsret.is_boolean_type() && rhsret.is_boolean_type()) {
        switch (node.op_node->op->type) {
            case Token::Type::t_logical_and:
                _ctx.value_stack.push(_ctx.builder->CreateAnd(left, right));
                break;
            case Token::Type::t_logical_or:
                _ctx.value_stack.push(_ctx.builder->CreateOr(left, right));
                break;
            default:
                throw _ctx.error(fmt::format("unsupported binary operator '{}' for operands '{}' and '{}' {}",
                    node.op_node->token_literal.value(), lhsret.get_type_desciption(),
                    rhsret.get_type_desciption(), _ctx.function_context()));
        }
    }
    else if (lhsret.is_floating_type() || rhsret.is_floating_type()) {
        // promote both sides to a common floating type (double if any operand is double)
        bool use_double = lhsret.is_primitive_of_type(AST::ValueTypePrimitive::t_float64) ||
                          rhsret.is_primitive_of_type(AST::ValueTypePrimitive::t_float64);

        auto promote_to_fp = [&](llvm::Value *value, const AST::ValueType &vt) {
            if (vt.is_floating_type()) {
                if (use_double && vt.is_primitive_of_type(AST::ValueTypePrimitive::t_float32)) {
                    return _ctx.builder->CreateFPExt(value, llvm::Type::getDoubleTy(*_ctx.llvm_context));
                }
                return value;
            }

            // integer/boolean -> float/double
            if (use_double) {
                return _ctx.builder->CreateSIToFP(value, llvm::Type::getDoubleTy(*_ctx.llvm_context));
            }
            return _ctx.builder->CreateSIToFP(value, llvm::Type::getFloatTy(*_ctx.llvm_context));
        };

        left = promote_to_fp(left, lhsret);
        right = promote_to_fp(right, rhsret);

        switch (node.op_node->op->type) {
            case Token::Type::t_op_add:
                _ctx.value_stack.push(_ctx.builder->CreateFAdd(left, right));
                break;
            case Token::Type::t_op_sub:
                _ctx.value_stack.push(_ctx.builder->CreateFSub(left, right));
                break;
            case Token::Type::t_op_mul:
                _ctx.value_stack.push(_ctx.builder->CreateFMul(left, right));
                break;
            case Token::Type::t_op_div:
                _ctx.value_stack.push(_ctx.builder->CreateFDiv(left, right));
                break;
            case Token::Type::t_op_mod:
                _ctx.value_stack.push(_ctx.builder->CreateFRem(left, right));
                break;
            case Token::Type::t_logical_eq:
                _ctx.value_stack.push(_ctx.builder->CreateFCmpOEQ(left, right));
                break;
            case Token::Type::t_logical_neq:
                _ctx.value_stack.push(_ctx.builder->CreateFCmpONE(left, right));
                break;
            case Token::Type::t_close_angle:
                _ctx.value_stack.push(_ctx.builder->CreateFCmpOGT(left, right));
                break;
            case Token::Type::t_open_angle:
                _ctx.value_stack.push(_ctx.builder->CreateFCmpOLT(left, right));
                break;
            case Token::Type::t_logical_geq:
                _ctx.value_stack.push(_ctx.builder->CreateFCmpOGE(left, right));
                break;
            case Token::Type::t_logical_leq:
                _ctx.value_stack.push(_ctx.builder->CreateFCmpOLE(left, right));
                break;


            default:
                throw _ctx.error(fmt::format("unsupported binary operator '{}' for operands '{}' and '{}' {}",
                    node.op_node->token_literal.value(), lhsret.get_type_desciption(),
                    rhsret.get_type_desciption(), _ctx.function_context()));
        }
    }
    else {
        throw std::runtime_error("Unsupported binary operator");
    }
}

void ExprCodegen::gen_unary_expr(AST::UnaryExprNode &node)
{
    node.expr->accept(*_ctx.visitor);

    auto value = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    auto type = node.expr->result_type();

    switch (node.token_operator.type()) {
        case Token::Type::t_op_sub:
            if (type.is_floating_type()) {
                _ctx.value_stack.push(_ctx.builder->CreateFNeg(value));
            }
            else if (type.is_integer_type()) {
                _ctx.value_stack.push(_ctx.builder->CreateNeg(value));
            }
            else {
                throw _ctx.error(fmt::format("unary '-' is not supported for operand type '{}' {}",
                    type.get_type_desciption(), _ctx.function_context()));
            }
            break;
        default:
            throw _ctx.error(fmt::format("unsupported unary operator '{}' {}",
                node.token_operator.value(), _ctx.function_context()));
    }
}

void ExprCodegen::gen_function_call(AST::FunctionCallExprNode &node)
{
    // a compiler builtin is answered here rather than called: it has no llvm::Function, so this
    // has to come before the function-table lookup further down
    if (node.decl && node.decl->is_builtin()) {
        gen_builtin_call(node);
        return;
    }

    // the other bodyless kind: lowered from its own token into a printf, with no symbol to look up -
    // AST::is_print_call owns which nodes those are
    if (AST::is_print_call(node)) {

        for (auto &arg : node.arguments) {
            arg->accept(*_ctx.visitor);

            auto arg_value = _ctx.value_stack.top();
            _ctx.value_stack.pop();

            // the argument's own type, with no peeling. the adjustment pass already inserted the
            // auto-deref for a pointer read, so a value-position read has the pointee's type by
            // the time it gets here - reaching for value_type_of() as well peeled a second time
            // and printed a genuine address (a call returning ptr<T>, say) as though it were the
            // int at that address. anything still a pointer here really is an address, and echo
            // has no format for one
            auto result_type = arg->result_type();

            // **the one non-primitive `echo` knows.** a string prints as a length-counted write rather
            // than through printf: its bytes are not NUL-terminated in general - a substring shares its
            // owner's buffer and simply stops early - and `%s` would run off the end of one
            if (_ctx.core_types().is_string_like(result_type)) {
                gen_echo_string(arg_value, result_type);
                continue;
            }

            const EchoConversion conversion = printf_conversion_for(result_type);
            if (conversion.format == nullptr) {
                throw _ctx.error(fmt::format(
                    "Unsupported argument type '{}' for 'echo' {}",
                    result_type.get_type_desciption(), _ctx.function_context()));
            }

            // echo is a destination like any other, so the widening goes through the one
            // conversion table rather than being hand rolled here: it takes the extend from the
            // *source's* signedness, so int8 sign extends where uint8 zero extends, and it hands
            // an already-wide value straight back - int32/int64/float64 emit no extra IR at all
            arg_value = _ctx.types->coerce_value(
                arg_value, result_type, AST::ValueType(conversion.promoted), *_ctx.current_cmp_unit);

            std::vector<llvm::Value *> ArgsV = {
                _ctx.builder->CreateGlobalStringPtr(conversion.format),
                arg_value
            };

            _ctx.builder->CreateCall(_ctx.current_module()->getFunction("printf"), ArgsV);
        }
    }

    // a requirement of an interface has no symbol of its own, so the callee comes out of the receiver's
    // vtable. checked before the lookup below, which would otherwise fail to find a function that
    // deliberately does not exist
    else if (node.decl != nullptr && node.decl->is_interface_requirement()) {
        gen_virtual_call(node);
    }

    else {
        llvm::Function *func = find_llvm_function(node.decl);

        if (!func) {
            throw _ctx.error(fmt::format(
                "No generated function found for call to '{}' {}",
                node.decl ? node.decl->func_name() : node.token_function_name.value(),
                _ctx.function_context()));
        }

        // evaluate every argument uniformly. arguments bound to pointer parameters were
        // already rewritten into AddrOfExprNode by the coercion pass (FuncCallParser /
        // Monomorphizer), so accepting them leaves the variable's address on the stack,
        // so no per-argument kind sniffing is needed here
        std::vector<llvm::Value *> args;
        for (auto &arg : node.arguments) {
            arg->accept(*_ctx.visitor);
            args.push_back(_ctx.value_stack.top());
            _ctx.value_stack.pop();
        }

        llvm::Value *ret = _ctx.builder->CreateCall(func, args);

        // a void call produces no value. pushing one anyway left a void-typed entry that no
        // parent ever pops, so a `foo();` statement quietly grew the stack
        if (!ret->getType()->isVoidTy()) {
            _ctx.value_stack.push(ret);
        }
    }
}

void ExprCodegen::gen_virtual_call(AST::FunctionCallExprNode &node)
{
    llvm::Type *opaque_ptr = llvm::PointerType::get(*_ctx.llvm_context, 0);
    auto *iface_type = _ctx.types->iface_llvm_type();

    if (node.arguments.empty() || node.arguments[0] == nullptr) {
        throw _ctx.error(fmt::format(
            "'{}' is dispatched through a receiver it does not have {}",
            node.decl->func_name(), _ctx.function_context()));
    }

    // which slot the requirement occupies, asked of the one walk that decides it - so the entry read here
    // and the entry the vtable was built with cannot disagree
    const std::optional<size_t> slot =
        AST::interface_method_slot(node.decl->owner_type, node.decl);

    if (!slot.has_value()) {
        throw _ctx.error(fmt::format(
            "'{}' has no vtable slot on '{}' {}",
            node.decl->func_name(),
            node.decl->owner_type != nullptr ? node.decl->owner_type->namespaced_name() : "<none>",
            _ctx.function_context()));
    }

    // the erased receiver, addressed by the parser exactly as any other receiver is - `&$d`
    node.arguments[0]->accept(*_ctx.visitor);
    llvm::Value *iface_ptr = _ctx.pop();

    // **the address of the object field is the `$this` the concrete method already expects.** a class
    // method's receiver is `Circle&`, the address of a slot holding a handle, and field 0 of the fat
    // pointer is such a slot - so the erasure costs no shim
    llvm::Value *self = _ctx.builder->CreateStructGEP(
        iface_type, iface_ptr, IfaceValue::object_index, "iface.self");

    llvm::Value *vtable = _ctx.builder->CreateLoad(
        opaque_ptr,
        _ctx.builder->CreateStructGEP(iface_type, iface_ptr, IfaceValue::vtable_index, "iface.vtable_ptr"),
        "iface.vtable");

    llvm::Value *callee = _ctx.builder->CreateLoad(
        opaque_ptr,
        _ctx.builder->CreateConstGEP1_64(
            opaque_ptr, vtable, IfaceValue::first_method_slot + slot.value(), "iface.slot"),
        "iface.callee");

    std::vector<llvm::Value *> args;
    args.reserve(node.decl->args.size());
    args.push_back(self);

    for (size_t i = 1; i < node.arguments.size(); i++) {
        node.arguments[i]->accept(*_ctx.visitor);
        args.push_back(_ctx.pop());
    }

    // the signature comes off the *requirement*, which is the whole reason this needed no new node: it
    // spells the return type and every parameter, and the implementor's own signature equals it below the
    // receiver by construction - AST::interface_implementations compared them
    //
    // through get_llvm_function_type rather than assembled here, because the leading opaque pointer this
    // needs is exactly the one it already puts first: a callable's environment and a method's receiver
    // occupy one slot by design, which is what lets an erased receiver need no shim. built by hand, the
    // callee's type here and the type create_llvm_func_decl gave the real symbol were two answers that
    // agree only by grace of opaque pointers. callable_type() strips the receiver, implicit_arg_count()'s
    // own job, and get_llvm_function_type puts it back
    llvm::FunctionType *fn_type =
        _ctx.types->get_llvm_function_type(node.decl->callable_type().signature(), *_ctx.current_cmp_unit);

    llvm::Value *ret = _ctx.builder->CreateCall(fn_type, callee, args);

    // a void call produces no value, exactly as at a direct one
    if (!ret->getType()->isVoidTy()) {
        _ctx.value_stack.push(ret);
    }
}

llvm::Function *ExprCodegen::find_llvm_function(const AST::FunctionDeclNode *decl)
{
    auto funcid = _ctx.current_cmp_unit->function_table.get_function_id(decl);
    llvm::Function *func = _ctx.current_cmp_unit->function_table.get_llvm_function(funcid);

    if (func) {
        return func;
    }

    // look for the function in the other modules
    for (auto &cmp_unit : _ctx.cmp_units) {
        if (cmp_unit.get() == _ctx.current_cmp_unit) {
            continue;
        }

        funcid = cmp_unit->function_table.get_function_id(decl);
        func = cmp_unit->function_table.get_llvm_function(funcid);

        if (func) {
            return func;
        }
    }

    return nullptr;
}

void ExprCodegen::gen_closure_expr(AST::ClosureExprNode &node)
{
    if (node.decl == nullptr) {
        throw _ctx.error(fmt::format("A closure literal reached codegen with no body {}", _ctx.function_context()));
    }

    // the body is an ordinary declaration, emitted from the file root like any other, so its
    // llvm::Function is already in the table by the time any expression is lowered
    llvm::Function *func = find_llvm_function(node.decl);

    if (!func) {
        throw _ctx.error(fmt::format(
            "No generated function found for the closure '{}' {}",
            node.decl->decorated_func_name(), _ctx.function_context()));
    }

    llvm::Value *environment = nullptr;

    if (node.environment_type != nullptr) {
        const AST::ValueType env_type = AST::ValueType::make_class(node.environment_type);

        // the same block a class value gets, because the environment *is* a class: strong count 1, so the
        // callable this expression produces holds the one reference, and every copy of it adds another
        environment = _ctx.classes->gen_class_box_alloc(env_type);

        const ClassLayout layout =
            _ctx.types->get_or_create_class_layout(node.environment_type, *_ctx.current_cmp_unit);

        llvm::Value *payload_ptr = _ctx.builder->CreateStructGEP(
            layout.box, environment, ClassBox::payload_index, "env_payload");

        // one store per capture, in property order - `captured_values` is built in the same order the
        // properties were appended, which is what lets this walk by index
        for (size_t i = 0; i < node.captured_values.size(); i++) {
            AST::ExprNode *value = node.captured_values[i];

            value->accept(*_ctx.visitor);
            llvm::Value *captured = _ctx.value_stack.top();
            _ctx.value_stack.pop();

            llvm::Value *slot = _ctx.builder->CreateStructGEP(
                layout.payload, payload_ptr, static_cast<unsigned>(i), "env_slot");

            _ctx.builder->CreateStore(
                _ctx.types->coerce_value(
                    captured,
                    value->result_type(),
                    node.environment_type->get_property_type(i),
                    *_ctx.current_cmp_unit),
                slot);
        }
    }
    else {
        // a non-capturing closure allocates nothing. that is the whole reason the callable is a *fat*
        // pointer rather than a handle to an environment that would always have to exist
        environment = llvm::ConstantPointerNull::get(llvm::PointerType::get(*_ctx.llvm_context, 0));
    }

    llvm::StructType *callable_type = _ctx.types->callable_llvm_type();

    llvm::Value *value = llvm::UndefValue::get(callable_type);
    value = _ctx.builder->CreateInsertValue(value, func, 0, "closure.fn");
    value = _ctx.builder->CreateInsertValue(value, environment, 1, "closure.env");

    _ctx.value_stack.push(value);
}

void ExprCodegen::gen_indirect_call(AST::IndirectCallExprNode &node)
{
    const AST::ValueType callee_type = node.callee_type();

    if (!callee_type.is_callable()) {
        throw _ctx.error(fmt::format(
            "An indirect call reached codegen over a '{}', which is not callable {}",
            callee_type.get_type_desciption(), _ctx.function_context()));
    }

    node.callee->accept(*_ctx.visitor);
    llvm::Value *callable = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    llvm::Value *fn = _ctx.builder->CreateExtractValue(callable, 0, "call.fn");
    llvm::Value *env = _ctx.builder->CreateExtractValue(callable, 1, "call.env");

    // the environment leads, always - see TypeLowering::get_llvm_function_type. one shape for both kinds
    // of target is what lets this site call either without asking which it has
    std::vector<llvm::Value *> args;
    args.push_back(env);

    const auto &signature = callee_type.signature();

    for (size_t i = 0; i < node.arguments.size(); i++) {
        AST::ExprNode *arg = node.arguments[i];

        arg->accept(*_ctx.visitor);
        llvm::Value *value = _ctx.value_stack.top();
        _ctx.value_stack.pop();

        // through the one conversion table, like every other destination. a direct call has its
        // arguments coerced by AST::CallResolver, which walks the callee's *declaration* - an indirect
        // call has no declaration, so its parameter types come off the signature and the fit happens here
        if (i < signature.parameter_types.size()) {
            value = _ctx.types->coerce_value(
                value, arg->result_type(), signature.parameter_types[i], *_ctx.current_cmp_unit);
        }

        args.push_back(value);
    }

    llvm::FunctionType *fn_type =
        _ctx.types->get_llvm_function_type(signature, *_ctx.current_cmp_unit);

    llvm::Value *ret = _ctx.builder->CreateCall(fn_type, fn, args);

    if (!ret->getType()->isVoidTy()) {
        _ctx.value_stack.push(ret);
    }
}

void ExprCodegen::gen_builtin_call(AST::FunctionCallExprNode &node)
{
    // dispatched on the kind first, because the two families answer different questions and share
    // nothing. `size_of`/`align_of` are generic, take no arguments and fold to a constant; `die`
    // and `assert` are concrete, take arguments, and push no value at all
    const AST::BuiltinKind kind = AST::builtin_kind_for(node.decl->builtin.value());

    switch (kind) {
        case AST::BuiltinKind::t_size_of:
        case AST::BuiltinKind::t_align_of:
            // the kind is handed down rather than looked up again: this switch has already made
            // the routing decision, so the callee's contract is two kinds, not four
            gen_type_query_builtin(node, kind);
            return;

        case AST::BuiltinKind::t_die:
            gen_die_builtin(node);
            return;

        case AST::BuiltinKind::t_assert:
            gen_assert_builtin(node);
            return;

        case AST::BuiltinKind::t_ref_count:
            gen_ref_count_builtin(node);
            return;
    }
}

void ExprCodegen::gen_echo_string(llvm::Value *value, const AST::ValueType &type)
{
    const AST::CoreStringLayout &layout = _ctx.core_string_layout();

    // both string types arrive here as a *value*, so the window is reached by extraction rather than a
    // GEP - there is no address to walk. an owning `string` is one level further out than its window
    llvm::Value *window = _ctx.core_types().is_string(type)
        ? _ctx.builder->CreateExtractValue(value, { static_cast<unsigned>(layout.window_index) }, "window")
        : value;

    llvm::Value *bytes = _ctx.builder->CreateExtractValue(
        window, { static_cast<unsigned>(layout.bytes_index) }, "bytes");
    llvm::Value *size = _ctx.builder->CreateExtractValue(
        window, { static_cast<unsigned>(layout.size_index) }, "size");

    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);

    // stdout is buffered and this write is not, so flushing first keeps a string in order with the
    // printf every other `echo` emits. AbortCodegen flushes for the same reason before its own write
    _ctx.builder->CreateCall(
        _ctx.libc_callee("fflush", i32, { _ctx.opaque_ptr_type() }),
        { llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(_ctx.opaque_ptr_type())) });

    _ctx.builder->CreateCall(
        _ctx.libc_callee("write", i64, { i32, _ctx.opaque_ptr_type(), i64 }),
        { llvm::ConstantInt::get(i32, 1), bytes, size });

    // the trailing newline every other `echo` conversion carries, so one statement behaves one way
    // whatever it is handed. writing it separately rather than copying the bytes to append it: the
    // bytes may be a shared substring, and there is nowhere to put a longer copy
    _ctx.builder->CreateCall(
        _ctx.libc_callee("write", i64, { i32, _ctx.opaque_ptr_type(), i64 }),
        { llvm::ConstantInt::get(i32, 1), _ctx.builder->CreateGlobalStringPtr("\n", "echo.nl"),
          llvm::ConstantInt::get(i64, 1) });
}

void ExprCodegen::gen_ref_count_builtin(AST::FunctionCallExprNode &node)
{
    // the shape is TypeChecker::check_ref_count_argument's, not this arm's: a non-class argument is
    // the user's mistake and gets a located diagnostic there rather than an internal compiler error
    // here. what is left is the invariant a settled call already carries
    if (node.arguments.size() != 1 || node.arguments[0] == nullptr) {
        throw _ctx.error(fmt::format("'ref_count' takes exactly one argument {}", _ctx.function_context()));
    }

    const AST::ValueType argument_type = node.arguments[0]->result_type();
    const AST::ValueType handle_type = AST::value_type_of(argument_type);

    node.arguments[0]->accept(*_ctx.visitor);
    llvm::Value *handle = _ctx.pop();

    // read *through* the borrow. the parameter is `T&`, so what arrives is the address of the slot
    // holding the handle rather than the handle - one load short. the pointer adjuster inserts no deref
    // here because the argument sits in a pointer position, so this load is owed at exactly this site
    if (argument_type.is_pointer()) {
        handle = _ctx.builder->CreateLoad(_ctx.opaque_ptr_type(), handle, "handle");
    }

    // the count itself belongs to the class subsystem, beside retain and release - this arm only routes
    // to it and fits the result to whatever the declaration promised, so the stdlib is free to say
    // `usize` where the block holds an i64
    llvm::Value *count = _ctx.classes->gen_strong_count(handle, handle_type);

    _ctx.push(_ctx.types->coerce_value(
        count, AST::ValueType(AST::ValueTypePrimitive::t_uint64), node.decl->get_return_type(),
        *_ctx.current_cmp_unit));
}

void ExprCodegen::gen_die_builtin(AST::FunctionCallExprNode &node)
{
    _ctx.abort->gen_abort(
        "fatal error", _ctx.abort->detail_of(node), _ctx.abort->location_of(node));
}

void ExprCodegen::gen_assert_builtin(AST::FunctionCallExprNode &node)
{
    // a release build emits nothing, and never visits the condition - which is what makes the
    // elision balanced: any retain or release the ownership pass wrapped the condition in is a
    // child of this node and disappears with it
    if (!_ctx.options.assertions_enabled()) {
        return;
    }

    if (node.arguments.empty() || node.arguments[0] == nullptr) {
        throw _ctx.error(fmt::format("'assert' has no condition {}", _ctx.function_context()));
    }

    node.arguments[0]->accept(*_ctx.visitor);
    llvm::Value *condition = _ctx.pop();

    // stop on the *false* path, so the branch reads the way the source does
    _ctx.abort->gen_abort_if(
        _ctx.builder->CreateNot(condition, "assert.failed"),
        "assertion failed", _ctx.abort->detail_of(node), _ctx.abort->location_of(node));
}

void ExprCodegen::gen_type_query_builtin(AST::FunctionCallExprNode &node, AST::BuiltinKind kind)
{
    const AST::FunctionDeclNode *decl = node.decl;

    // the builtin asks about a type, and that type is the instance's single type argument. an
    // un-instantiated template reaching here means the monomorphizer never resolved the call,
    // which is a compiler bug rather than a source error
    if (decl->instantiation_args.size() != 1) {
        throw _ctx.error(fmt::format(
            "Builtin '{}' expects exactly one type argument, got {} {}",
            decl->builtin.value(), decl->instantiation_args.size(), _ctx.function_context()));
    }

    const AST::ValueType &subject = decl->instantiation_args[0];
    llvm::Type *llvm_subject = _ctx.types->get_llvm_type(subject, *_ctx.current_cmp_unit);

    // the result is whatever the declaration promised - usize in the stdlib - so the constant
    // lands with the type the caller's arithmetic already expects
    llvm::Type *result_type = _ctx.types->get_llvm_type(decl->get_return_type(), *_ctx.current_cmp_unit);

    // getTypeAllocSize, not getTypeStoreSize: it includes tail padding, so it is the stride between
    // array elements. that is exactly what `alloc<T>(count)` and `$p:$[n]` mean, and using the store
    // size would under-allocate for any padded struct
    const uint64_t value = kind == AST::BuiltinKind::t_size_of
        ? _ctx.layout().getTypeAllocSize(llvm_subject)
        : _ctx.layout().getABITypeAlign(llvm_subject).value();

    _ctx.value_stack.push(llvm::ConstantInt::get(result_type, value));
}

void ExprCodegen::gen_addr_of(AST::AddrOfExprNode &node)
{
    // `&E` is the address of E's slot, with no transparency peeling - gen_lvalue, not
    // gen_place. so `&$buf` on a `ptr<uint8>` yields the address of $buf itself
    _ctx.value_stack.push(_ctx.lvalues->gen_lvalue(*node.operand).address);
}

void ExprCodegen::gen_null_assert(llvm::Value *address)
{
    // a property of the *program being compiled*, not of how echoc was built. this used to be an
    // `#if` over the host compiler's NDEBUG, which meant book/concept/pointers_and_refs_v2.md's
    // "in release builds it is unchecked" described nothing
    if (!_ctx.options.assertions_enabled()) {
        return;
    }

    llvm::Value *is_null = _ctx.builder->CreateICmpEQ(
        address,
        llvm::ConstantPointerNull::get(llvm::PointerType::get(*_ctx.llvm_context, 0)),
        "isnull");

    // through the same runtime *and the same message shape* `die` and `assert` use, so this finally
    // says what went wrong instead of raising SIGILL. a cast node carries no token, so its location
    // is the enclosing function rather than a line - see todo/C6
    _ctx.abort->gen_abort_if(is_null,
        "fatal error", "null pointer cast to a reference",
        fmt::format("{}, {}", _ctx.current_file_name(), _ctx.function_context()));
}

void ExprCodegen::gen_index(AST::IndexExprNode &node)
{
    _ctx.value_stack.push(_ctx.lvalues->gen_load(node, "elem"));
}

void ExprCodegen::gen_deref(AST::DerefExprNode &node)
{
    // gen_lvalue on the deref node itself resolves to the pointee's storage; loading it is
    // the read. keeping the address computation in LValueCodegen is what lets a deref appear
    // on the left of an assignment as readily as on the right
    _ctx.value_stack.push(_ctx.lvalues->gen_load(node, "deref"));
}

void ExprCodegen::gen_null(AST::NullNode &node)
{
    // every pointer is the same opaque `ptr` under llvm, so one null constant serves them all
    _ctx.value_stack.push(llvm::ConstantPointerNull::get(
        llvm::PointerType::get(*_ctx.llvm_context, 0)));
}

// **nothing reaches this, and nothing should.** an `OperatorNode` carries a symbol's identity and
// precedence for the parser; it is not a value. `gen_binary_expr` reads its operator without visiting
// it, a *declared* operator is lowered as an ordinary FunctionCallExprNode rather than as a node kind
// of its own, and `visitOperator` on the recursive visitor is a no-op
//
// so this is a throw rather than the empty body it used to be. every other expression visitor leaves
// exactly one value on `value_stack`, and a silent no-op here desynced it - a defect nothing in the
// IR could point at afterwards (todo/X1)
void ExprCodegen::gen_operator(AST::OperatorNode &node)
{
    throw Compiler::InternalCompilerException(fmt::format(
        "an operator node reached codegen: '{}'. an operator is not a value - a declared one is "
        "lowered as a call, and a built-in one is read by gen_binary_expr without being visited",
        node.token_literal.value()));
}
};
