#include "Compiler/LLVM/Codegen/ExprCodegen.h"

#include "AST/MatchExprNode.h"

#include "AST/StaticPropertyExprNode.h"

#include "AST/ASTArrayLiteral.h"
#include "AST/ASTSourceToken.h"
#include "AST/ASTVariadic.h"
#include "Compiler/LLVM/Codegen/IfaceValue.h"

#include "AST/ASTControlFlow.h"
#include "Compiler/LLVM/Codegen/ReturnAbi.h"
#include "AST/ASTNullability.h"
#include "AST/ASTConstFold.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTDestruction.h"
#include "AST/ASTConformance.h"
#include "Compiler/LLVM/Codegen/ClassLayout.h"
#include "Compiler/LLVM/Codegen/ClassCodegen.h"
#include "Compiler/LLVM/Codegen/MemoryCodegen.h"
#include "Compiler/LLVM/Codegen/ProcessCodegen.h"
#include "Compiler/LLVM/Codegen/DebugPrintCodegen.h"
#include "Compiler/LLVM/Codegen/AbortCodegen.h"
#include "Compiler/LLVM/Codegen/IntrinsicResolution.h"
#include "eco.h"
#include "Compiler/LLVM/Codegen/LValueCodegen.h"
#include "Compiler/LLVM/Codegen/PrintfConversion.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/Codegen/DebugInfoCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/VarDeclNode.h"
#include "AST/VarRefNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/TemporaryBindExprNode.h"
#include "AST/VarNode.h"
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
    //
    // which casts those are is AST::narrowing_promotes_raw_storage's question, not this arm's: the
    // same conjunction decides whether the author is asked to write `unsafe`, and a copy here lets a
    // cast emit the assert and hand back a trusted borrow that nobody was asked to promise
    if (AST::narrowing_promotes_raw_storage(from, to)) {
        gen_null_assert(value, AST::location_of_expression(node.expr));
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

void ExprCodegen::gen_static_property(AST::StaticPropertyExprNode &node)
{
    _ctx.value_stack.push(_ctx.lvalues->gen_load(node, node.token_name.value().c_str()));
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
    // **every refusal below is a compiler bug rather than the user's mistake**, and says so. the arms in
    // this function and the arms of AST::binary_has_builtin_meaning are mirrors: that predicate's `true`
    // *is* the claim that there is an arm here, and a false answer sends the use site looking for a
    // declared `operator` and, failing that, to a located diagnostic in AST::TypeChecker. so a pair
    // reaching one of these throws got past a gate that should have refused it
    const auto unlowered = [&](const AST::ValueType &lhs, const AST::ValueType &rhs) {
        return _ctx.error(fmt::format(
            "compiler bug: operator '{}' has no lowering for operands '{}' and '{}', but "
            "AST::binary_has_builtin_meaning accepted them {}",
            node.op_node->token_literal.value(), lhs.get_type_desciption(),
            rhs.get_type_desciption(), _ctx.function_context()));
    };

    // **`&&` and `||` evaluate the right side only when the left has not already decided.** both
    // sides always running was a CreateAnd / CreateOr, and `false && die("no")` died. the branch
    // lives in its own helper so this function does not create blocks - dprint is the only builtin
    // that does, and it has its own subsystem for that reason
    if (node.op_node != nullptr && node.op_node->op != nullptr) {
        const Token::Type op = node.op_node->op->type;

        if (op == Token::Type::t_logical_and || op == Token::Type::t_logical_or) {
            gen_logical_short_circuit(node);
            return;
        }
    }

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

    // **a presence test against a written `null`** - over a wrapped `T?`, whose tag it reads, or over a
    // weak handle, which lowers to an opaque address and so is present exactly when it is non-null. One
    // arm and not two, because it is one question and `TypeLowering::gen_has_value` is the one primitive
    // that answers it for either shape: two blocks computing the same `CreateNot` off the same predicate
    // were two spellings of "is this reference absent" held in step by inspection.
    //
    // ahead of every arm below because the operand's *kind* is still whatever it was: an `int32?` would
    // otherwise fall into the numeric arm and compare a `{ i1, i32 }` aggregate as a number, and a weak is
    // its own kind rather than a `t_class`.
    //
    // `!$w` at gen_unary_expr's `!` arm reads gen_has_value and inverts it exactly this way, down to the
    // value name, so those two emit the same instruction rather than agreeing by inspection either
    if (lhsret.is_wrapped_optional() || rhsret.is_wrapped_optional() || lhsret.is_weak() || rhsret.is_weak()) {
        // through AST::is_written_null rather than the raw tag: an implicit cast reconciling the null
        // with the optional's type hides it, and this arm and AST::binary_has_builtin_meaning - which
        // decided this comparison *has* a built-in lowering at all - have to agree about which operand
        // was the null
        const bool lhs_is_null = AST::is_written_null(node.lhs);
        const bool rhs_is_null = AST::is_written_null(node.rhs);

        if ((lhs_is_null || rhs_is_null) && node.op_node->op->is_identity_comparison()) {
            const AST::ValueType &present_of = lhs_is_null ? rhsret : lhsret;
            llvm::Value *present = _ctx.types->gen_has_value(lhs_is_null ? right : left, present_of);

            // `$x == null` is *absent*, so the tag is inverted - and `!=` is the tag as it stands
            _ctx.value_stack.push(node.op_node->op->type == Token::Type::t_logical_eq
                ? _ctx.builder->CreateNot(present, "is_null")
                : present);
            return;
        }

        // **the two shapes part company here.** an optional falls through: only against a written `null`,
        // never against another optional - `$a == $b` over two `int32?`s is a question about the values,
        // and answering it here would silently compare presence instead, so it stays unhandled and the
        // type checker's ordinary rules report it. a weak has no arm below it at all, so anything else
        // over one got past a gate that should have refused it
        if (lhsret.is_weak() || rhsret.is_weak()) {
            throw unlowered(lhsret, rhsret);
        }
    }

    // two class handles, or a handle against null. the only operators a class answers, and the type
    // checker has already rejected the rest - so this is a plain address comparison over two opaque
    // pointers, ahead of the pointer arm because a class type is not a t_pointer
    if (lhsret.is_class() || rhsret.is_class()) {
        // which operators those are is Operator::is_identity_comparison, the same predicate the type
        // checker rejects the rest with - so the two passes read the rule off one function rather than
        // each enumerating it
        if (!node.op_node->op->is_identity_comparison()) {
            throw unlowered(lhsret, rhsret);
        }

        _ctx.value_stack.push(node.op_node->op->type == Token::Type::t_logical_eq
            ? _ctx.builder->CreateICmpEQ(left, right)
            : _ctx.builder->CreateICmpNE(left, right));
        return;
    }

    // **two payload-free enums, compared by their discriminant** - which is what an enum's identity is.
    // AST::binary_has_builtin_meaning is what refused every other operator and every enum that carries a
    // payload, so what reaches here is exactly `==` and `!=` over two of one type.
    //
    // the operands arrive as whole aggregates - `{ i8 }` for a plain enum - so the tag is *extracted*
    // rather than loaded: the values are already in registers by this point, and reaching for the place
    // again would be a second answer to where a discriminant lives
    if (lhsret.is_enum() || rhsret.is_enum()) {
        if (!node.op_node->op->is_identity_comparison()) {
            throw unlowered(lhsret, rhsret);
        }

        llvm::Value *left_tag = _ctx.builder->CreateExtractValue(left, {AST::k_enum_tag_index}, "lhs.tag");
        llvm::Value *right_tag = _ctx.builder->CreateExtractValue(right, {AST::k_enum_tag_index}, "rhs.tag");

        _ctx.value_stack.push(node.op_node->op->type == Token::Type::t_logical_eq
            ? _ctx.builder->CreateICmpEQ(left_tag, right_tag)
            : _ctx.builder->CreateICmpNE(left_tag, right_tag));
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
                        throw unlowered(lhsret, rhsret);
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
                throw unlowered(lhsret, rhsret);
        }
    }

    if (lhsret.is_integer_type() && rhsret.is_integer_type()) {
        // **the operation's own signedness, asked of the one owner.** `/ % ** < > <= >=` all mean
        // something different over an unsigned operand, and this arm used to emit the signed spelling
        // for every one of them - so `uint64 $big = 18446744073709551615; $big > 1` answered *false*.
        //
        // read off the *reconciled* type rather than either side, because AST::const_fold folds these
        // same operators at that type: taking the lhs alone, or "either side is unsigned", would answer
        // `int64 < uint32` differently from the folder and a `const if` would take the other arm.
        //
        // by the time codegen runs the two are the same type anyway - a mismatched integer pair makes
        // BinaryExprNode::result_type() void, which is what Parser::parse_binary_expr and
        // OperatorRewriter::widen_binary_operands each insert a cast for - so this reads the answer an
        // earlier pass already wrote down, rather than choosing a winner here.
        //
        // **the operator is part of the question**, and `>>` is why: a shift is performed at its left
        // operand's type whatever the count is written as, so `int32 -16 >> uint32 2` is `ashr`. read
        // without the operator this reconciled to `uint32`, took `lshr`, and answered 1073741820
        const AST::ValueType op_type =
            AST::binary_operation_type(node.op_node->op, lhsret, rhsret);
        const bool is_unsigned = op_type.is_unsigned_integer();

        // the count, in the operand's type - see the two shift arms below. routed through
        // TypeLowering::coerce_value rather than a CreateIntCast here, because that is the one owner of
        // "this value, at that type" and it is the half that knows a narrowing count is a truncation
        const auto shift_count = [&]() {
            return _ctx.types->coerce_value(
                right, AST::value_type_of(rhsret), op_type, *_ctx.current_cmp_unit);
        };

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
                _ctx.value_stack.push(is_unsigned
                    ? _ctx.builder->CreateUDiv(left, right)
                    : _ctx.builder->CreateSDiv(left, right));
                break;
            case Token::Type::t_op_mod:
                _ctx.value_stack.push(is_unsigned
                    ? _ctx.builder->CreateURem(left, right)
                    : _ctx.builder->CreateSRem(left, right));
                break;
            case Token::Type::t_op_pow:
                {
                    // im kinda just copying the behavior of C with clang here
                    // cast all values to double and then call the pow intrinsic
                    // cast the result back to the original type
                    //
                    // routed through the one resolver rather than reaching for the intrinsic
                    // directly. this site used to hand getDeclaration a two-element overload vector
                    // for `llvm.pow`, which has one overload type - the same bug TypeLowering had,
                    // and the reason both now ask the same question in one place
                    llvm::Type *double_type = llvm::Type::getDoubleTy(*_ctx.llvm_context);
                    llvm::FunctionType *pow_type = llvm::FunctionType::get(double_type, {double_type, double_type}, false);

                    std::string failure;
                    llvm::Function *fun = declare_intrinsic(_ctx.current_module(), "llvm.pow", pow_type, failure);

                    if (!fun) {
                        throw _ctx.error(fmt::format("Cannot lower the '**' operator: {}", failure));
                    }

                    // both conversions read the operation's signedness, and the result goes back to the
                    // operation's *own* type. this used to widen signed unconditionally and truncate to
                    // `i32` whatever the operands were, so `int64 $x = 2 ** 40;` lost the top bits
                    std::vector<llvm::Value *> args;
                    args.push_back(is_unsigned
                        ? _ctx.builder->CreateUIToFP(left, double_type)
                        : _ctx.builder->CreateSIToFP(left, double_type));
                    args.push_back(is_unsigned
                        ? _ctx.builder->CreateUIToFP(right, double_type)
                        : _ctx.builder->CreateSIToFP(right, double_type));

                    llvm::Value *result = _ctx.builder->CreateCall(fun, args);
                    llvm::Type *int_type = _ctx.types->get_llvm_type(op_type, *_ctx.current_cmp_unit);

                    _ctx.value_stack.push(is_unsigned
                        ? _ctx.builder->CreateFPToUI(result, int_type)
                        : _ctx.builder->CreateFPToSI(result, int_type));
                }
                break;
            // **the bitwise five.** four of them are sign-agnostic and lower to one instruction each.
            //
            // `&` is the one with a grammar note attached: `&$x` lexes as an address-of and `& $x` as
            // this operator, because LexerFunction::ReferenceFrom wins only when the `&` is glued to what
            // follows it. So `$h & $mask` is this and `$h &$mask` is an address-of - a real wart, left
            // alone deliberately, since unpicking it would change what `&$a[$i]` means everywhere
            case Token::Type::t_and:
                _ctx.value_stack.push(_ctx.builder->CreateAnd(left, right));
                break;
            case Token::Type::t_or:
                _ctx.value_stack.push(_ctx.builder->CreateOr(left, right));
                break;
            case Token::Type::t_xor:
                _ctx.value_stack.push(_ctx.builder->CreateXor(left, right));
                break;
            // **the two shifts, and the only arms in this function that convert an operand themselves.**
            // every other pair arrives already reconciled, because AST::common_numeric_type widened one
            // side at parse time - but a count is not an operand and nothing widened it
            // (AST::binary_reconciles_operands), while `shl`/`lshr`/`ashr` still require one LLVM type
            // for both. So the count is brought to the *operand's* type here, which is the direction
            // that leaves the operation where the user wrote it: `uint8 200 << int64 40` is a `uint8`
            // shift by 40, refused by AST::shift_count_refusal, rather than an int64 one that answers
            case Token::Type::t_op_shl:
                _ctx.value_stack.push(_ctx.builder->CreateShl(left, shift_count()));
                break;
            // **the one bitwise operator that is not sign-agnostic**, and it reads the same `is_unsigned`
            // `/ % **` and the comparisons above already read rather than asking a second time: a right
            // shift over a signed operand keeps the sign bit (`ashr`) and over an unsigned one brings in
            // zeroes (`lshr`). `op_type` is the *left* operand's for a shift, so the count's declared
            // type cannot pick the instruction - which it did, and `int32 -16 >> uint32 2` answered
            // 1073741820 where the same shift by an `int32 2` answered -4
            case Token::Type::t_op_shr:
                _ctx.value_stack.push(is_unsigned
                    ? _ctx.builder->CreateLShr(left, shift_count())
                    : _ctx.builder->CreateAShr(left, shift_count()));
                break;
            case Token::Type::t_logical_eq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpEQ(left, right));
                break;
            case Token::Type::t_logical_neq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpNE(left, right));
                break;
            // `==` and `!=` above need no arm of their own: they are sign-agnostic at equal width, which
            // is why the four below are the whole of the exposure
            case Token::Type::t_close_angle:
                _ctx.value_stack.push(is_unsigned
                    ? _ctx.builder->CreateICmpUGT(left, right)
                    : _ctx.builder->CreateICmpSGT(left, right));
                break;
            case Token::Type::t_open_angle:
                _ctx.value_stack.push(is_unsigned
                    ? _ctx.builder->CreateICmpULT(left, right)
                    : _ctx.builder->CreateICmpSLT(left, right));
                break;
            case Token::Type::t_logical_geq:
                _ctx.value_stack.push(is_unsigned
                    ? _ctx.builder->CreateICmpUGE(left, right)
                    : _ctx.builder->CreateICmpSGE(left, right));
                break;
            case Token::Type::t_logical_leq:
                _ctx.value_stack.push(is_unsigned
                    ? _ctx.builder->CreateICmpULE(left, right)
                    : _ctx.builder->CreateICmpSLE(left, right));
                break;
            default:
                throw unlowered(lhsret, rhsret);
        }
    }
    else if (lhsret.is_boolean_type() && rhsret.is_boolean_type()) {
        switch (node.op_node->op->type) {
            // **whether two answers agree**, over the i1 they already are. the four *ordering*
            // comparisons are deliberately absent: a bool is a yes/no rather than a small number, and
            // `$a < $b` on two of them is a precedence mistake far more often than it is a question -
            // refused the same way ordering two class handles is
            case Token::Type::t_logical_eq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpEQ(left, right));
                break;
            case Token::Type::t_logical_neq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpNE(left, right));
                break;
            default:
                throw unlowered(lhsret, rhsret);
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
                throw unlowered(lhsret, rhsret);
        }
    }
    else {
        throw unlowered(lhsret, rhsret);
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

        // **`!` over a bool is the negation; over anything that may be absent it is the presence
        // test, inverted.** the second arm emits nothing of its own - TypeLowering::gen_has_value is
        // the one owner of the wrapped-optional / free-over-an-address split, and the `== null` arm
        // of gen_binary_expr reads it and inverts it exactly this way. so `!$maybe` and
        // `$maybe == null` are the same instruction by construction rather than by agreement
        case Token::Type::t_exclamation:
            if (type.is_boolean_type()) {
                _ctx.value_stack.push(_ctx.builder->CreateNot(value, "not"));
            }
            else if (AST::destination_admits_null(type)) {
                _ctx.value_stack.push(_ctx.builder->CreateNot(
                    _ctx.types->gen_has_value(value, type), "is_null"));
            }
            else {
                throw _ctx.error(fmt::format("unary '!' is not supported for operand type '{}' {}",
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

            const PrintfConversion conversion = printf_conversion_for(result_type);
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

            // the newline is appended here rather than carried in the table: the table is shared with
            // `dprint`, which ends a line once per *value* and not once per leaf it prints
            std::vector<llvm::Value *> ArgsV = {
                _ctx.builder->CreateGlobalStringPtr(std::string(conversion.format) + "\n"),
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
            // **a variadic pack is not one argument, it is the tail.** the list written at the call
            // site becomes the arguments after the fixed ones, in order. every element already
            // carries the C promotion AST::CallResolver applied, so there is nothing to decide here -
            // which is the point: the promotion table is written once, in AST::variadic_promotion_of
            if (auto *pack = AST::variadic_pack_of(arg)) {
                for (auto *element : pack->elements) {
                    element->accept(*_ctx.visitor);
                    args.push_back(_ctx.pop());
                }

                continue;
            }

            arg->accept(*_ctx.visitor);
            args.push_back(_ctx.value_stack.top());
            _ctx.value_stack.pop();
        }

        emit_call(func, args, _ctx.types->return_abi_of(node.decl, *_ctx.current_cmp_unit));
    }
}

void ExprCodegen::emit_call(
    llvm::FunctionCallee callee,
    std::vector<llvm::Value *> &args,
    const ReturnAbi &abi
)
{
    // **an aggregate too big for registers comes back through storage this call site provides.**
    // `abi` is the same answer the signature asked, so a caller and its callee cannot disagree about
    // where the answer is. the slot is an ordinary entry alloca, which is what lets SROA promote it
    // into scalars once the callee is inlined
    //
    // **the slot's type comes from this unit's lowering of the ABI, never from the `sret` attribute
    // on the callee.** reading it off the attribute is the same answer right up until the modules are
    // merged: the JIT and `--optimize whole` both link every unit into one and `llvm::Linker` brings
    // each unit's own named struct types along, so the attribute can name the *other* unit's `%string`
    // while this unit allocates and reads its own

    if (abi.is_indirect()) {
        llvm::Value *slot = _ctx.entry_alloca(abi.indirect_type, "call.sret");

        args.insert(args.begin(), slot);

        auto *call = _ctx.builder->CreateCall(callee, args);

        // **the attribute goes on the call too, and forgetting it is a miscompile rather than a missed
        // optimization** - see the note on Compiler::LLVM::indirect_return_attributes. it decides which
        // register the hidden pointer travels in, and LLVM's fallback to the callee's own attributes stops
        // working the moment a merge leaves this unit's `%string` and the callee's as two types
        call->setAttributes(call->getAttributes().addParamAttributes(
            *_ctx.llvm_context, 0,
            indirect_return_attributes(*_ctx.llvm_context, abi, _ctx.layout())));

        // the call answers `void`, so the value this expression produces is what the callee wrote
        _ctx.value_stack.push(
            _ctx.builder->CreateLoad(abi.indirect_type, slot, "call.result"));

        return;
    }

    auto *call = _ctx.builder->CreateCall(callee, args);

    // a void call produces no value. pushing one anyway left a void-typed entry that no
    // parent ever pops, so a `foo();` statement quietly grew the stack
    if (!call->getType()->isVoidTy()) {
        _ctx.value_stack.push(call);
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
        _ctx.types->get_llvm_function_type(
            node.decl->callable_type().signature(),
            *_ctx.current_cmp_unit,
            TypeLowering::FunctionCallingShape::t_echo);

    emit_call({ fn_type, callee }, args, _ctx.types->return_abi_of(node.decl, *_ctx.current_cmp_unit));
}

// the callee symbol *in the current unit*, declared on demand if this unit has not named it yet.
//
// it used to fall back to searching the other units' tables, which is wrong in a way nothing catches:
// an llvm::Function belongs to exactly one llvm::Module, and compile_bundle moves every non-main
// module into main and then resets it. a call planted against a foreign unit's Function therefore
// references an object that is about to be destroyed, and LLVM's verifier does not check the module
// membership of a referenced global - so the whole thing is silent. the path was unreachable only
// because build_function_maps' reference-scoped loop happens to declare every callee of every call
// node a module owns, which stops being true as soon as a definition is emitted into a unit other
// than the one that owns its declaration.
//
// declaring into the current unit is what that loop already does, so this is the same answer reached
// on demand rather than up front, and a `declare` is all a cross-module callee ever needs.
llvm::Function *ExprCodegen::find_llvm_function(const AST::FunctionDeclNode *decl)
{
    // a null decl is an unresolved call, which the caller reports by name - there is nothing to
    // declare, and minting a symbol for it would turn a resolution failure into a link failure
    if (decl == nullptr) {
        return nullptr;
    }

    auto funcid = _ctx.current_cmp_unit->function_table.get_function_id(decl);

    if (llvm::Function *func = _ctx.current_cmp_unit->function_table.get_llvm_function(funcid)) {
        return func;
    }

    return _ctx.types->create_llvm_func_decl(decl, *_ctx.current_cmp_unit);
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

    if (!callee_type.has_signature()) {
        throw _ctx.error(fmt::format(
            "An indirect call reached codegen over a '{}', which is not callable {}",
            callee_type.get_type_desciption(), _ctx.function_context()));
    }

    node.callee->accept(*_ctx.visitor);
    llvm::Value *callee_value = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    const bool c_function = callee_type.is_c_function();

    // a C function pointer is one word and has no environment. a callable is two, and the
    // environment leads - see TypeLowering::get_llvm_function_type. one lowering, the branch
    // is only which word(s) we load
    llvm::Value *fn = c_function
        ? callee_value
        : _ctx.builder->CreateExtractValue(callee_value, 0, "call.fn");

    std::vector<llvm::Value *> args;

    if (!c_function) {
        args.push_back(_ctx.builder->CreateExtractValue(callee_value, 1, "call.env"));
    }

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

    llvm::FunctionType *fn_type = _ctx.types->get_llvm_function_type(
        signature,
        *_ctx.current_cmp_unit,
        c_function
            ? TypeLowering::FunctionCallingShape::t_c
            : TypeLowering::FunctionCallingShape::t_echo);

    // the same exemption get_llvm_function_type applies: a C function pointer is never sret
    const ReturnAbi abi = c_function
        ? ReturnAbi{}
        : return_abi_for(
            _ctx.types->get_llvm_type(signature.return_type, *_ctx.current_cmp_unit),
            _ctx.layout());

    emit_call({ fn_type, fn }, args, abi);
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
        case AST::BuiltinKind::t_is_trivially_copyable:
        case AST::BuiltinKind::t_needs_destruction:
            // the kind is handed down rather than looked up again: this switch has already made
            // the routing decision, so the callee's contract is four kinds, not all of them
            gen_type_query_builtin(node, kind);
            return;

        case AST::BuiltinKind::t_take:
            gen_take_builtin(node);
            return;

        case AST::BuiltinKind::t_init:
            gen_init_builtin(node);
            return;

        case AST::BuiltinKind::t_die:
            gen_die_builtin(node);
            return;

        case AST::BuiltinKind::t_unwrap_abort:
            _ctx.abort->gen_abort(
                "fatal error", "unwrapped an absent value", node.token_function_name);
            return;

        case AST::BuiltinKind::t_assert:
            gen_assert_builtin(node);
            return;

        case AST::BuiltinKind::t_crash_set_hook:
            if (node.arguments.empty() || node.arguments[0] == nullptr) {
                throw _ctx.error(fmt::format("'set_hook' has no hook {}", _ctx.function_context()));
            }
            node.arguments[0]->accept(*_ctx.visitor);
            _ctx.push(_ctx.abort->swap_hook(_ctx.pop()));
            return;

        case AST::BuiltinKind::t_crash_take_hook:
            _ctx.push(_ctx.abort->take_hook());
            return;

        case AST::BuiltinKind::t_crash_default_hook:
            if (node.arguments.empty() || node.arguments[0] == nullptr) {
                throw _ctx.error(fmt::format(
                    "'default_hook' has no info {}", _ctx.function_context()));
            }
            node.arguments[0]->accept(*_ctx.visitor);
            _ctx.abort->gen_default_hook(_ctx.pop());
            return;

        case AST::BuiltinKind::t_ref_count:
        case AST::BuiltinKind::t_weak_count:
            // the kind is handed down for gen_type_query_builtin's reason: the two read two words of one
            // header and differ in nothing else, so the routing decision is not made twice
            gen_ref_count_builtin(node, kind);
            return;

        case AST::BuiltinKind::t_dprint:
            gen_dprint_builtin(node);
            return;

        case AST::BuiltinKind::t_alloc_bytes:
        case AST::BuiltinKind::t_realloc_bytes:
        case AST::BuiltinKind::t_free_bytes:
            // one arm for all three: they evaluate their arguments and route to the allocation seam,
            // and differ in nothing but arity - which the kind already says
            gen_raw_memory_builtin(node, kind);
            return;

        case AST::BuiltinKind::t_live_allocations:
            gen_live_allocations_builtin(node);
            return;

        case AST::BuiltinKind::t_process_argc:
        case AST::BuiltinKind::t_process_argv:
        case AST::BuiltinKind::t_process_envp:
            // one arm for all three, for the reason the raw-memory trio has one: they read three
            // globals through one subsystem and differ in nothing the kind does not already say
            gen_process_query_builtin(node, kind);
            return;

        case AST::BuiltinKind::t_exit:
            gen_exit_builtin(node);
            return;
    }
}

void ExprCodegen::gen_dprint_builtin(AST::FunctionCallExprNode &node)
{
    // the subject type, exactly as gen_type_query_builtin reads it: the monomorphizer bound `T` from the
    // argument, and it is the *declared* type of what is being printed rather than anything the value
    // itself could be asked at runtime
    if (node.decl->instantiation_args.size() != 1) {
        throw _ctx.error(fmt::format(
            "'dprint' expects exactly one type argument, got {} {}",
            node.decl->instantiation_args.size(), _ctx.function_context()));
    }

    const AST::ValueType subject = node.decl->instantiation_args[0];

    // **no load, unlike gen_ref_count_builtin.** that arm wants the handle *out of* the slot; this one
    // wants the slot itself, because a struct is walked by GEP. what arrives is already the address:
    // the parameter is a borrow, so AST::CallResolver wrapped the argument in an AddrOfExprNode
    node.arguments[0]->accept(*_ctx.visitor);
    llvm::Value *address = _ctx.pop();

    _ctx.debug_print->gen_dprint(LValue{address, subject});

    // and nothing pushed: dprint returns void, exactly as `die` does, which is what makes it a statement
    // rather than an expression
}

void ExprCodegen::gen_echo_string(llvm::Value *value, const AST::ValueType &type)
{
    // both string types arrive here as a *value*, so the window is reached by extraction rather than a
    // GEP - there is no address to walk
    const auto [bytes, size] = _ctx.gen_string_window(value, type, "");

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

void ExprCodegen::gen_ref_count_builtin(AST::FunctionCallExprNode &node, AST::BuiltinKind kind)
{
    const bool weak = kind == AST::BuiltinKind::t_weak_count;
    const char *name = weak ? "weak_count" : "ref_count";

    // the shape is TypeChecker::check_ref_count_argument's, not this arm's: a non-class argument is
    // the user's mistake and gets a located diagnostic there rather than an internal compiler error
    // here. what is left is the invariant a settled call already carries
    if (node.arguments.size() != 1 || node.arguments[0] == nullptr) {
        throw _ctx.error(fmt::format("'{}' takes exactly one argument {}", name, _ctx.function_context()));
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
    // `usize` where the block holds an i64. which word, by name from ClassBox rather than by a 0 or a 1
    llvm::Value *count = _ctx.classes->gen_count(
        handle,
        handle_type,
        weak ? ClassBox::weak_index : ClassBox::strong_index);

    _ctx.push(_ctx.types->coerce_value(
        count, AST::ValueType(AST::ValueTypePrimitive::t_uint64), node.decl->get_return_type(),
        *_ctx.current_cmp_unit));
}

void ExprCodegen::gen_die_builtin(AST::FunctionCallExprNode &node)
{
    const auto index = AST::builtin_message_index(AST::BuiltinKind::t_die);

    if (!index.has_value() || node.arguments.size() <= *index || node.arguments[*index] == nullptr) {
        _ctx.abort->gen_abort("fatal error", "", node.token_function_name);
        return;
    }

    AST::ExprNode *message = node.arguments[*index];

    // the same question TypeChecker asks. `folded.empty()` is not it: `die("")` is a literal
    if (const auto literal = AST::literal_string_value(message)) {
        _ctx.abort->gen_abort("fatal error", *literal, node.token_function_name);
        return;
    }

    // a runtime string: fold the location around the bytes the program already holds. the
    // argument is a `string` by the declaration, so the window is the same two words `echo`
    // reads
    message->accept(*_ctx.visitor);
    llvm::Value *value = _ctx.pop();
    const auto [bytes, size] = _ctx.gen_string_window(value, message->result_type(), "");

    _ctx.abort->gen_abort_dynamic("fatal error", bytes, size, node.token_function_name);
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
        "assertion failed", _ctx.abort->detail_of(node), node.token_function_name);
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

    // the result is whatever the declaration promised - usize for the two sizes, bool for the two
    // predicates - so the constant lands with the type the caller already expects, and an i1 needs no
    // arm of its own here
    llvm::Type *result_type = _ctx.types->get_llvm_type(decl->get_return_type(), *_ctx.current_cmp_unit);

    // no tail: the four kinds this is routed for are the four answered here, and a fifth added to the
    // dispatch above without one is a compile error rather than a constant zero nothing would notice
    uint64_t value = 0;

    switch (kind) {
        // getTypeAllocSize, not getTypeStoreSize: it includes tail padding, so it is the stride between
        // array elements. that is exactly what `alloc<T>(count)` and `$p:$[n]` mean, and using the store
        // size would under-allocate for any padded struct
        case AST::BuiltinKind::t_size_of:
            value = _ctx.layout().getTypeAllocSize(_ctx.types->get_llvm_type(subject, *_ctx.current_cmp_unit));
            break;

        case AST::BuiltinKind::t_align_of:
            value = _ctx.layout().getABITypeAlign(
                _ctx.types->get_llvm_type(subject, *_ctx.current_cmp_unit)).value();
            break;

        // **the taxonomy answering for itself, through its one owner.** these two read no layout at all -
        // they are AST facts - and they used to be folded here *as well as* in AST::const_fold, which a
        // `const if` needs one pass earlier. two spellings of one fact, held in step by nothing, is the
        // recurring bug in this codebase, so this arm asks rather than answering.
        //
        // the arity guard above stays and is still the load-bearing part: it is what guarantees `subject`
        // is concrete. the two callers' answers to "is `T` bound yet" are deliberately different - a
        // fixpoint round gets `t_pending`, because it has more rounds, and this throws, because an
        // un-instantiated template reaching codegen is a compiler bug rather than a source error
        case AST::BuiltinKind::t_is_trivially_copyable:
        case AST::BuiltinKind::t_needs_destruction: {
            const AST::ConstFoldResult folded = AST::const_fold(&node);

            if (folded.result != AST::ConstFoldResult::Result::t_folded) {
                throw _ctx.error(fmt::format(
                    "Builtin '{}' did not fold for a settled call: {} {}",
                    decl->builtin.value(),
                    folded.refusal.empty() ? "its type argument is not bound" : folded.refusal,
                    _ctx.function_context()));
            }

            value = folded.bits;
            break;
        }

        case AST::BuiltinKind::t_die:
        case AST::BuiltinKind::t_unwrap_abort:
        case AST::BuiltinKind::t_crash_set_hook:
        case AST::BuiltinKind::t_crash_take_hook:
        case AST::BuiltinKind::t_crash_default_hook:
        case AST::BuiltinKind::t_assert:
        case AST::BuiltinKind::t_take:
        case AST::BuiltinKind::t_init:
        case AST::BuiltinKind::t_ref_count:
        case AST::BuiltinKind::t_weak_count:
        case AST::BuiltinKind::t_dprint:
        case AST::BuiltinKind::t_alloc_bytes:
        case AST::BuiltinKind::t_realloc_bytes:
        case AST::BuiltinKind::t_free_bytes:
        case AST::BuiltinKind::t_live_allocations:
        case AST::BuiltinKind::t_process_argc:
        case AST::BuiltinKind::t_process_argv:
        case AST::BuiltinKind::t_process_envp:
        case AST::BuiltinKind::t_exit:
            throw _ctx.error(fmt::format(
                "Builtin '{}' is not a type query {}", decl->builtin.value(), _ctx.function_context()));
    }

    _ctx.value_stack.push(llvm::ConstantInt::get(result_type, value));
}

LValue ExprCodegen::gen_raw_place(AST::FunctionCallExprNode &node, const char *name, size_t arity)
{
    // the shape is TypeChecker::check_raw_storage_argument's, not this one's - what is left here is the
    // invariant a settled call already carries
    if (node.arguments.size() != arity) {
        throw _ctx.error(fmt::format(
            "'{}' takes exactly {} argument(s) {}", name, arity, _ctx.function_context()));
    }

    for (const auto &argument : node.arguments) {
        if (argument == nullptr) {
            throw _ctx.error(fmt::format("'{}' has a null argument {}", name, _ctx.function_context()));
        }
    }

    const AST::ValueType place_type = node.arguments[0]->result_type();

    if (!place_type.is_pointer()) {
        throw _ctx.error(fmt::format(
            "'{}' expects the address of a place, got '{}' {}",
            name, place_type.get_type_desciption(), _ctx.function_context()));
    }

    node.arguments[0]->accept(*_ctx.visitor);

    // the *pointee*, which is what both callers want: the address names a slot holding a `T`, and
    // `place_type` is the borrow that reached it
    return LValue{ _ctx.pop(), AST::value_type_of(place_type) };
}

void ExprCodegen::gen_take_builtin(AST::FunctionCallExprNode &node)
{
    const LValue place = gen_raw_place(node, "take", 1);

    // **the whole lowering: one load through the borrow.** the parameter is `T&`, so what arrives is the
    // address of the slot rather than what is in it, and the pointer adjuster inserts no deref because
    // the argument sits in a pointer position - so this read is owed at exactly this site, the same way
    // gen_ref_count_builtin's is
    //
    // gen_load and not that arm's CreateLoad: a count is always a handle and can name its own llvm type,
    // and `T` here is any type at all
    //
    // nothing is written back. that *is* the move - the slot keeps its bits and stops being an owner,
    // which is a claim about the source that only its manager can make and is why this sits in `mem::`
    _ctx.value_stack.push(_ctx.lvalues->gen_load(place, "take"));
}

void ExprCodegen::gen_init_builtin(AST::FunctionCallExprNode &node)
{
    const LValue place = gen_raw_place(node, "init", 2);

    node.arguments[1]->accept(*_ctx.visitor);
    llvm::Value *value = _ctx.pop();

    // **the whole lowering: one store through the borrow.** the mirror of `take`'s one load, and correct
    // for the same reason read from the other end - the parameter is `T&`, so what arrives is the address
    // of the slot rather than what is in it.
    //
    // **nothing is released first.** that is the point of the builtin rather than an omission: an
    // ordinary `=` into this place would have AST::OwnershipPass end whatever the destination held, and
    // over a slot straight out of `mem::alloc` that is a destructor over whatever bytes the allocator
    // handed back. the claim that there is nothing there is the caller's to make, which is why this sits
    // in `mem::` beside `take` and `free`
    //
    // and nothing is *retained* either: the value arrived by value, so the caller's copy already
    // happened and this hands that owner over rather than duplicating it
    _ctx.builder->CreateStore(
        _ctx.types->coerce_value(
            value, node.arguments[1]->result_type(), place.storage_type, *_ctx.current_cmp_unit),
        place.address);
}

void ExprCodegen::gen_raw_memory_builtin(AST::FunctionCallExprNode &node, AST::BuiltinKind kind)
{
    // no argument check: all three carry an ordinary declared signature in mem.eco, so AST::CallResolver
    // already refused every call whose arity or types did not fit - the same reason the two builtins below
    // do not check either

    // a byte count, as the seam wants it. the declaration says `usize`, and AST::CallResolver already
    // coerced the argument to it - so this is the one conversion left, and it is between two spellings of
    // the same 64-bit integer rather than between two types
    const AST::ValueType size_type = AST::ValueType(AST::ValueTypePrimitive::t_uint64);

    // and a block address. **nullable**, because that is the whole of what these three promise: `alloc`
    // hands back null when the allocator could not, `free` accepts null, and `realloc` takes and returns
    // it - which is why the raw allocator is spelled `ptr<uint8>` in the stdlib and not `uint8&`
    const AST::ValueType block_type = AST::ValueType::make_pointer(
        AST::ValueType(AST::ValueTypePrimitive::t_uint8), /*nullable=*/true);

    std::vector<llvm::Value *> args;

    for (size_t i = 0; i < node.arguments.size(); i++) {
        node.arguments[i]->accept(*_ctx.visitor);

        // by position rather than by kind: the block, where there is one, is always first
        const AST::ValueType &wanted = (i == 0 && kind != AST::BuiltinKind::t_alloc_bytes)
            ? block_type
            : size_type;

        args.push_back(_ctx.types->coerce_value(
            _ctx.pop(), node.arguments[i]->result_type(), wanted, *_ctx.current_cmp_unit));
    }

    // `free` returns void, so it pushes nothing - the one of the three that is a statement
    if (kind == AST::BuiltinKind::t_free_bytes) {
        _ctx.memory->gen_free(args[0]);
        return;
    }

    llvm::Value *block = kind == AST::BuiltinKind::t_alloc_bytes
        ? _ctx.memory->gen_alloc(args[0], "bytes")
        : _ctx.memory->gen_realloc(args[0], args[1], "bytes");

    _ctx.value_stack.push(_ctx.types->coerce_value(
        block, block_type, node.decl->get_return_type(), *_ctx.current_cmp_unit));
}

void ExprCodegen::gen_live_allocations_builtin(AST::FunctionCallExprNode &node)
{
    // no argument check: the declaration takes none, so AST::CallResolver already refused every call
    // that passed one. no availability check either - AST::TypeChecker refuses this builtin without
    // --track-allocations, at the call site, where it can name a line
    _ctx.value_stack.push(_ctx.types->coerce_value(
        _ctx.memory->gen_live_count("live"),
        AST::ValueType(AST::ValueTypePrimitive::t_uint64),
        node.decl->get_return_type(), *_ctx.current_cmp_unit));
}

void ExprCodegen::gen_process_query_builtin(AST::FunctionCallExprNode &node, AST::BuiltinKind kind)
{
    // no argument check, for gen_live_allocations_builtin's reason: all three declarations take none,
    // so AST::CallResolver already refused every call that passed one
    if (kind == AST::BuiltinKind::t_process_argc) {
        _ctx.value_stack.push(_ctx.types->coerce_value(
            _ctx.process->gen_argc("argc"),
            AST::ValueType(AST::ValueTypePrimitive::t_uint64),
            node.decl->get_return_type(), *_ctx.current_cmp_unit));
        return;
    }

    // `ptr<ptr<uint8>>`, and **nullable at both levels** - which is not a formality. The outer word can
    // genuinely be null: `envp` is absent on a platform that does not pass it, and both blocks are null
    // in a unit whose `main` never ran the capture. The inner one is how each block says it has ended,
    // since both are NUL-terminated rather than counted
    const AST::ValueType block_type = AST::ValueType::make_pointer(
        AST::ValueType::make_pointer(
            AST::ValueType(AST::ValueTypePrimitive::t_uint8), /*nullable=*/true),
        /*nullable=*/true);

    llvm::Value *block = kind == AST::BuiltinKind::t_process_argv
        ? _ctx.process->gen_argv("argv")
        : _ctx.process->gen_envp("envp");

    _ctx.value_stack.push(_ctx.types->coerce_value(
        block, block_type, node.decl->get_return_type(), *_ctx.current_cmp_unit));
}

void ExprCodegen::gen_exit_builtin(AST::FunctionCallExprNode &node)
{
    // no argument check, for gen_raw_memory_builtin's reason: the declaration takes exactly one code, so
    // AST::CallResolver already refused every call that did not
    node.arguments[0]->accept(*_ctx.visitor);

    // C's exit takes an `int`, which is `int32` here. the declaration already says so and
    // AST::CallResolver already coerced to it, so this is the last spelling difference rather than a
    // conversion
    llvm::Value *code = _ctx.types->coerce_value(
        _ctx.pop(), node.arguments[0]->result_type(),
        AST::ValueType(AST::ValueTypePrimitive::t_int32), *_ctx.current_cmp_unit);

    // pushes nothing and terminates the block, like `die` - the two ways a program stops share one
    // owner, so the `unreachable` and the NoReturn on the symbol are decided in one place
    _ctx.abort->gen_exit(code);
}

void ExprCodegen::gen_addr_of(AST::AddrOfExprNode &node)
{
    // the weak reading reads the *handle out of* the slot rather than taking the slot's address, which is
    // one load further and the opposite direction from the arm below. the two are not variations on one
    // lowering: `&$obj` on a class hands back the block the handle names, having taken a weak reference
    // to it, and the slot the handle happened to be sitting in is not part of the answer
    if (node.denotes_weak_reference()) {
        const AST::ValueType class_type = node.operand->result_type();

        LValue place = _ctx.lvalues->gen_lvalue(*node.operand);
        llvm::Value *handle = _ctx.lvalues->gen_load(place, "obj");

        _ctx.value_stack.push(_ctx.classes->gen_weak_of(handle, class_type));
        return;
    }

    // `&E` is the address of E's slot, with no transparency peeling - gen_lvalue, not
    // gen_place. so `&$buf` on a `ptr<uint8>` yields the address of $buf itself
    _ctx.value_stack.push(_ctx.lvalues->gen_lvalue(*node.operand).address);
}

void ExprCodegen::gen_function_ref(AST::FunctionRefExprNode &node)
{
    if (node.decl == nullptr) {
        throw _ctx.error(fmt::format(
            "an unresolved function reference reached codegen {}",
            _ctx.function_context()));
    }

    llvm::Function *fn = find_llvm_function(node.decl);

    if (fn == nullptr) {
        throw _ctx.error(fmt::format(
            "function reference '&{}' has no symbol {}",
            node.token_name.value(), _ctx.function_context()));
    }

    _ctx.value_stack.push(fn);
}

void ExprCodegen::gen_strong_expr(AST::StrongExprNode &node)
{
    const AST::ValueType operand_type = node.operand->result_type();

    // the shape is TypeChecker's, not this arm's - a non-weak operand is the user's mistake and gets a
    // located diagnostic there rather than an internal compiler error here
    if (!operand_type.is_weak()) {
        throw _ctx.error(fmt::format(
            "'strong' reached codegen with a '{}' operand {}",
            operand_type.get_type_desciption(), _ctx.function_context()));
    }

    node.operand->accept(*_ctx.visitor);

    _ctx.value_stack.push(
        _ctx.classes->gen_strong_upgrade(_ctx.pop(), operand_type.weak_target()));
}

void ExprCodegen::gen_null_assert(llvm::Value *address, const TokenReference &at)
{
    // a property of the *program being compiled*, not of how echoc was built. this used to be an
    // `#if` over the host compiler's NDEBUG, which meant the language rule
    // "in release builds it is unchecked" described nothing
    if (!_ctx.options.assertions_enabled()) {
        return;
    }

    llvm::Value *is_null = _ctx.builder->CreateICmpEQ(
        address,
        llvm::ConstantPointerNull::get(llvm::PointerType::get(*_ctx.llvm_context, 0)),
        "isnull");

    // through the same runtime *and the same message shape* `die` and `assert` use. the location
    // is the operand's token - a cast node carries none of its own
    _ctx.abort->gen_abort_if(is_null,
        "fatal error", "null pointer cast to a reference", at);
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

void ExprCodegen::gen_temporary_bind(AST::TemporaryBindExprNode &node)
{
    // the slot comes from gen_var_decl, the same one a named local gets. a temporary hangs off this node
    // rather than off a scope, so StmtCodegen::gen_scope's sweep never sees it - here the alloca still
    // happens where the declaration is *visited*, and this loop running before the body is what puts it
    // ahead of every read. the same reason TemporaryBindExprNode::clone clones the temporaries first
    for (AST::VarDeclNode *temp : node.temporaries) {
        temp->accept(*_ctx.visitor);
    }

    const size_t depth_before = _ctx.value_stack.size();

    node.body->accept(*_ctx.visitor);

    // the body stopped the program - a `die` reached inside it. the teardown would be instructions
    // after a terminator, which the verifier rejects, and nothing would run on that path anyway: an
    // abort is exit(1), it does not unwind. the same question gen_scope asks between two statements
    if (_ctx.block_is_terminated()) {
        return;
    }

    // **a void body pushed nothing**, and one is reachable: `$o->get()->m();` as a statement binds a
    // temporary for the receiver and the call answers with nothing at all. asked of the stack rather
    // than of result_type() because gen_function_call is what decides it - a void call deliberately
    // pushes no value - and the two must not be able to disagree. popping unconditionally read an
    // empty stack, which is not a diagnostic but a crash
    llvm::Value *value = _ctx.value_stack.size() > depth_before ? _ctx.pop() : nullptr;

    // the value out of the way *before* the drops rather than after: they are void calls and releases,
    // so they push nothing, and popping first keeps that a fact rather than a hope. it is also what
    // makes the ordering the node promises real - the body has read what it wanted out of the
    // temporary, and only then is the temporary destroyed
    for (auto &drop : node.teardown) {
        drop.node()->accept(*_ctx.visitor);
    }

    assert(_ctx.value_stack.size() == depth_before && "a temporary's drop leaked a value onto the stack");

    if (value != nullptr) {
        _ctx.value_stack.push(value);
    }
}

void ExprCodegen::gen_logical_short_circuit(AST::BinaryExprNode &node)
{
    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();

    const bool is_and = node.op_node->op->type == Token::Type::t_logical_and;
    const char *rhs_name = is_and ? "and.rhs" : "or.rhs";
    const char *done_name = is_and ? "and.done" : "or.done";
    const char *phi_name = is_and ? "and" : "or";

    node.lhs->accept(*_ctx.visitor);
    llvm::Value *left = _ctx.pop();

    auto *rhs_block = llvm::BasicBlock::Create(*_ctx.llvm_context, rhs_name, function);
    auto *done_block = llvm::BasicBlock::Create(*_ctx.llvm_context, done_name, function);

    // `&&` takes the right side only when the left is true. `||` takes it only when the left is false.
    // the skipped path carries the left value itself, which is the last operand that ran
    if (is_and) {
        _ctx.builder->CreateCondBr(left, rhs_block, done_block);
    }
    else {
        _ctx.builder->CreateCondBr(left, done_block, rhs_block);
    }

    llvm::BasicBlock *lhs_end = _ctx.builder->GetInsertBlock();

    _ctx.set_insert_point(rhs_block);
    node.rhs->accept(*_ctx.visitor);
    llvm::Value *right = _ctx.pop();
    llvm::BasicBlock *rhs_end = _ctx.builder->GetInsertBlock();
    _ctx.builder->CreateBr(done_block);

    _ctx.set_insert_point(done_block);
    llvm::PHINode *phi = _ctx.builder->CreatePHI(left->getType(), 2, phi_name);
    phi->addIncoming(left, lhs_end);
    phi->addIncoming(right, rhs_end);
    _ctx.value_stack.push(phi);
}

void ExprCodegen::gen_null_coalesce(AST::NullCoalesceExprNode &node)
{
    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();

    const AST::ValueType lhs_type = node.lhs->result_type();
    const AST::ValueType result = node.result_type();

    node.lhs->accept(*_ctx.visitor);
    llvm::Value *left = _ctx.pop();

    auto *present_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "coalesce.present", function);
    auto *absent_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "coalesce.absent", function);
    auto *done_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "coalesce.done", function);

    _ctx.builder->CreateCondBr(
        _ctx.types->gen_has_value(left, lhs_type), present_block, absent_block);

    // the present path unwraps and fits the result type. that second step matters when the two sides
    // differ - `lookup($k) ?? 0` over an `int32?` and an untyped literal, or a nullable result the right
    // side made non-nullable
    //
    // **present_value is the copy AST::OwnershipPass built over the payload place**, when a place
    // left side still owns what extractvalue would alias. evaluated here and not before the branch,
    // so a copy constructor's work does not run on the absent path. null is the common case: a
    // computed left side, or a payload that copies as bytes, and unwrap is the whole of it
    _ctx.set_insert_point(present_block);

    llvm::Value *unwrapped = nullptr;
    AST::ValueType present_type = AST::unwrapped_type_of(lhs_type);

    if (node.present_value != nullptr) {
        node.present_value->accept(*_ctx.visitor);
        unwrapped = _ctx.pop();
        present_type = node.present_value->result_type();
    }
    else {
        unwrapped = _ctx.types->gen_unwrapped(left, lhs_type);
    }

    llvm::Value *present = _ctx.types->coerce_value(
        unwrapped,
        present_type,
        result,
        *_ctx.current_cmp_unit);
    llvm::BasicBlock *present_end = _ctx.builder->GetInsertBlock();
    _ctx.builder->CreateBr(done_block);

    // **the right side is evaluated here and nowhere else**, which is the reason this is a branch rather
    // than a select: it may be a call, and calling it on the path where the left was there would be a
    // side effect the program did not ask for
    _ctx.set_insert_point(absent_block);
    node.rhs->accept(*_ctx.visitor);
    llvm::Value *right = _ctx.types->coerce_value(
        _ctx.pop(), node.rhs->result_type(), result, *_ctx.current_cmp_unit);
    llvm::BasicBlock *absent_end = _ctx.builder->GetInsertBlock();
    _ctx.builder->CreateBr(done_block);

    // the blocks the phi takes its incoming values from are the ones the builder *ended* in, not the ones
    // it started in: either arm may have branched internally - a call with its own control flow, a nested
    // `??` - and a phi naming the wrong predecessor is an llvm verifier failure with no source location
    _ctx.set_insert_point(done_block);
    llvm::PHINode *phi = _ctx.builder->CreatePHI(
        _ctx.types->get_llvm_type(result, *_ctx.current_cmp_unit), 2, "coalesce");
    phi->addIncoming(present, present_end);
    phi->addIncoming(right, absent_end);

    _ctx.value_stack.push(phi);
}

void ExprCodegen::gen_chain_base(AST::ChainBaseNode &node)
{
    if (_ctx.chain_base_slots.empty()) {
        throw _ctx.error(fmt::format(
            "a chain base marker was lowered outside a '?->' chain {}", _ctx.function_context()));
    }

    // the nearest enclosing chain's, which is the top: the marker is built by the parser inside exactly
    // one chain's continuation, and a nested chain pushes and pops around its own
    //
    // a *load*, because this is the value position - reaching the slot itself is gen_lvalue's arm, which
    // is what a method receiver and a write through the chain go through
    _ctx.value_stack.push(_ctx.builder->CreateLoad(
        _ctx.types->get_llvm_type(node.type, *_ctx.current_cmp_unit),
        _ctx.chain_base_slots.back(),
        "chain.base"));
}

void ExprCodegen::gen_optional_chain(AST::OptionalChainExprNode &node)
{
    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();

    const AST::ValueType base_type = node.base->result_type();
    const AST::ValueType result = node.result_type();

    // **evaluated once, before the branch.** the continuation reaches it through the marker rather than by
    // re-evaluating, which is what makes `$cache->find($k)?->name` call `find` exactly once
    node.base->accept(*_ctx.visitor);
    llvm::Value *base = _ctx.pop();

    auto *reach_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "chain.reach", function);
    auto *absent_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "chain.absent", function);
    auto *done_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "chain.done", function);

    _ctx.builder->CreateCondBr(
        _ctx.types->gen_has_value(base, base_type), reach_block, absent_block);

    _ctx.set_insert_point(reach_block);

    // spilled to a slot rather than kept as a value: the continuation may call a method, and a receiver
    // is an address. two instructions to keep one receiver convention, the same trade the class release
    // thunk makes when it spills a handle for a deinit
    llvm::Value *unwrapped = _ctx.types->gen_unwrapped(base, base_type);

    // **the slot is seated in the entry block, the store is not.** an alloca here would be an alloca per
    // *evaluation*, so a `?->` in a loop body would grow the stack once per turn - and `run` defaults to
    // --debug, where nothing folds it away. CodegenContext::entry_alloca is the one owner of that rule,
    // shared with every local and every parameter, so no slot in the language is seated any other way
    llvm::Value *slot = _ctx.entry_alloca(unwrapped->getType(), "chain.slot");
    _ctx.builder->CreateStore(unwrapped, slot);

    _ctx.chain_base_slots.push_back(slot);
    node.continuation->accept(*_ctx.visitor);

    const AST::ValueType reached_type = node.continuation->result_type();
    const bool has_value = !reached_type.is_void();

    llvm::Value *reached = has_value
        ? _ctx.types->coerce_value(_ctx.pop(), reached_type, result, *_ctx.current_cmp_unit)
        : nullptr;

    _ctx.chain_base_slots.pop_back();

    llvm::BasicBlock *reach_end = _ctx.builder->GetInsertBlock();
    _ctx.builder->CreateBr(done_block);

    // the absent arm runs **nothing at all** - that is the whole difference from `??`, which evaluates a
    // replacement. it only supplies the result type's empty value, and for a void chain not even that
    _ctx.set_insert_point(absent_block);

    // lowered once and used by both the empty value and the phi below
    llvm::Type *llvm_result = has_value
        ? _ctx.types->get_llvm_type(result, *_ctx.current_cmp_unit)
        : nullptr;

    llvm::Value *absent =
        has_value ? _ctx.types->gen_absent(result, *_ctx.current_cmp_unit) : nullptr;

    llvm::BasicBlock *absent_end = _ctx.builder->GetInsertBlock();
    _ctx.builder->CreateBr(done_block);

    _ctx.set_insert_point(done_block);

    // a void chain is a statement and pushes nothing, exactly as a void call does - so gen_scope's
    // stack-depth assertion stays true rather than being special-cased for this node
    if (!has_value) {
        return;
    }

    llvm::PHINode *phi = _ctx.builder->CreatePHI(llvm_result, 2, "chain");
    phi->addIncoming(reached, reach_end);
    phi->addIncoming(absent, absent_end);

    _ctx.value_stack.push(phi);
}

void ExprCodegen::gen_null(AST::NullNode &node)
{
    const AST::ValueType bound = node.result_type();

    // **a wrapped `T?` has no null address to be.** `int32?` is `{ i1 __has, i32 }`, so its empty value is
    // the tag cleared and the payload left undef - written out here rather than left to coerce_value,
    // because there is no source value to convert *from*: `null` is the absence itself
    //
    // this is the one place the destination has to be known, which is what NullNode::bound_type is for.
    // an unbound null still answers with the pointer constant below, and that is not a fallback so much as
    // the shape every *other* nullable actually has
    if (bound.is_wrapped_optional()) {
        _ctx.value_stack.push(_ctx.types->gen_absent(bound, *_ctx.current_cmp_unit));
        return;
    }

    // every pointer is the same opaque `ptr` under llvm, so one null constant serves them all - and a
    // class handle and a weak handle are addresses too, which is exactly what has_null_representation()
    // says about them
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
// IR could point at afterwards
void ExprCodegen::gen_operator(AST::OperatorNode &node)
{
    throw Compiler::InternalCompilerException(fmt::format(
        "an operator node reached codegen: '{}'. an operator is not a value - a declared one is "
        "lowered as a call, and a built-in one is read by gen_binary_expr without being visited",
        node.token_literal.value()));
}

void ExprCodegen::gen_match(AST::MatchExprNode &node)
{
    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();

    const AST::ValueType result = node.result_type();
    const bool has_value = !result.is_void();

    // **the subject is emitted as the declaration it is**, slot and initializer, through the same
    // gen_var_decl every local goes through - so it is evaluated exactly once, before the branch, and
    // the arms read its payload off storage rather than re-running whatever produced it
    node.subject->accept(*_ctx.visitor);

    // **the subject is a borrow whenever it named storage**, which AST::MatchResolution decided and this
    // reads back off the declaration's type rather than re-deciding. peeled through the one rule, so the
    // layout below is the enum's however the node reached it
    const AST::ValueType subject_decl_type = node.subject->type();
    const AST::ValueType subject_type = AST::value_type_of(subject_decl_type);
    const AST::ComplexType *ct = subject_type.get_complex_type();

    auto slot = _ctx.var_map.find(node.subject);

    if (slot == _ctx.var_map.end()) {
        throw _ctx.error(fmt::format(
            "the subject of a 'match' has no allocation in scope {}", _ctx.function_context()));
    }

    // the address the payload and the tag are read off. for a borrowed subject the slot holds an address,
    // so it is loaded through once here - the single auto-deref every read of a borrow performs, done once
    // for the whole node rather than per arm
    llvm::Value *subject_address = slot->second;

    if (subject_decl_type.is_pointer()) {
        subject_address = _ctx.lvalues->gen_load(
            LValue {
                slot->second,
                subject_decl_type,
                Provenance::t_typed,
            },
            "match.subject");
    }

    // the discriminant, read as the ordinary property it is - the same GEP any member read emits, which
    // is the whole dividend of `__tag` being in the layout rather than a shape codegen invents
    llvm::Value *tag_address = _ctx.builder->CreateStructGEP(
        _ctx.types->get_llvm_type(subject_type, *_ctx.current_cmp_unit),
        subject_address,
        AST::k_enum_tag_index,
        "match.tag_ptr");

    // **and read through the seam, not with a CreateLoad of its own.** LValueCodegen::gen_load is where
    // the `!tbaa` tag is attached, and an untagged load is the conservative answer said by omission - so
    // reading the tag by hand would make the one access this node emits alias everything, while every
    // ordinary member read beside it stays described. the place is `t_typed`: the address is the
    // subject's own slot and has been nowhere near a `ptr<T>`
    llvm::Value *tag = _ctx.lvalues->gen_load(
        LValue { tag_address, ct->get_property_type(AST::k_enum_tag_index), Provenance::t_typed },
        "match.tag");

    auto *done_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "match.done", function);

    // **the default block is the `else` arm when there is one, and unreachable otherwise.** unreachable
    // is not a shortcut: AST::MatchResolution refuses a match that does not cover every case, so a tag
    // outside the set is a value the program cannot hold - and telling LLVM so is what lets the switch
    // fold away when the subject's case is known
    llvm::BasicBlock *default_block = nullptr;

    std::vector<llvm::BasicBlock *> arm_blocks;
    arm_blocks.reserve(node.arms.size());

    for (const AST::MatchExprNode::Arm &arm : node.arms) {
        const bool is_else = arm.is_else();

        arm_blocks.push_back(llvm::BasicBlock::Create(
            *_ctx.llvm_context, is_else ? "match.else" : "match.arm", function));

        if (is_else) {
            default_block = arm_blocks.back();
        }
    }

    const bool has_else = default_block != nullptr;

    if (!has_else) {
        default_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "match.unreachable", function);
    }

    llvm::SwitchInst *branch = _ctx.builder->CreateSwitch(
        tag, default_block, static_cast<unsigned>(node.arms.size()));

    for (size_t i = 0; i < node.arms.size(); i++) {
        const AST::MatchExprNode::Arm &arm = node.arms[i];

        if (arm.is_else()) {
            continue;  // the default block, which takes no case of its own
        }

        const AST::ComplexType::EnumCase &entry = ct->enum_cases()[arm.case_ordinal.value()];

        branch->addCase(
            llvm::ConstantInt::get(
                llvm::cast<llvm::IntegerType>(tag->getType()),
                static_cast<uint64_t>(entry.discriminant),
                /*IsSigned=*/true),
            arm_blocks[i]);
    }

    // the value each arm produced and the block it ended in, which is what the phi is built from. the
    // ended-in block is the load-bearing half - an arm holding a branch of its own leaves the builder
    // somewhere other than where it started, and a phi naming the start block names a predecessor that
    // does not reach it
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;

    for (size_t i = 0; i < node.arms.size(); i++) {
        const AST::MatchExprNode::Arm &arm = node.arms[i];

        _ctx.set_insert_point(arm_blocks[i]);

        // the arm's scope first - its bindings are the scope's own leading declarations, so this is
        // what seats and initializes them, and a block arm's statements follow in the same walk
        if (arm.scope != nullptr) {
            arm.scope->accept(*_ctx.visitor);
        }

        if (arm.value != nullptr && !_ctx.block_is_terminated()) {
            // **an arm whose value never comes back is emitted and then not read.** `die('...')` leaves
            // no value on the stack and terminates its own block with `unreachable`, so popping one would
            // read a stack that is empty and coerce whatever was under it. AST::expression_never_returns
            // is the same owner AST::MatchResolution asked when it let this arm out of the unification -
            // it contributed no type there and it contributes no incoming value here, which are the two
            // halves of one fact
            //
            // emitted **unconditionally** rather than under `has_value`: when every arm dies the match's
            // own type is `void`, and a guard on that would drop the only statement those arms had.
            // one axis and three answers, so one chain rather than a nest testing it twice
            if (AST::expression_never_returns(*arm.value)) {
                arm.value->accept(*_ctx.visitor);
            }
            // **a place-yielding match phis addresses, so the arm is addressed rather than read.** the
            // one difference between the two shapes, and it is one line: gen_place is what every other
            // read-through goes through, so the payload's `!tbaa` tag, its provenance and its GEP are the
            // same ones a `$e->__c0_v` written by hand would get. reading the value here instead and
            // taking its address after would be a copy, which is what this shape exists to avoid
            //
            // **gen_place and not gen_lvalue**, and the difference is the whole bug it replaced: a binding
            // is a `T&`, so its *own* slot holds the address rather than the payload. gen_lvalue hands
            // back that slot, and the phi then joined pointers-to-borrows - which read back as whatever
            // the address happened to be, an integer in the low billions instead of the payload
            else if (node.arm_yields_address(arm)) {
                LValue place = _ctx.lvalues->gen_place(*arm.value);

                incoming.emplace_back(place.address, _ctx.builder->GetInsertBlock());
            }
            else {
                arm.value->accept(*_ctx.visitor);

                if (has_value) {
                    llvm::Value *value = _ctx.types->coerce_value(
                        _ctx.pop(), arm.value->result_type(), result, *_ctx.current_cmp_unit);

                    incoming.emplace_back(value, _ctx.builder->GetInsertBlock());
                }
            }
        }

        // an arm whose body already left - every path returned, or it ended in `die` - owes no branch
        // to the join and contributes no incoming value. the same test gen_scope makes before it seats
        // a slot, and for the same reason: a block that already ends cannot take a second terminator
        if (!_ctx.block_is_terminated()) {
            _ctx.builder->CreateBr(done_block);
        }
    }

    if (!has_else) {
        _ctx.set_insert_point(default_block);
        _ctx.builder->CreateUnreachable();
    }

    _ctx.set_insert_point(done_block);

    // a void match is a statement and pushes nothing, exactly as a void call does - so gen_scope's
    // stack-depth assertion stays true rather than being special-cased for this node
    if (!has_value || incoming.empty()) {
        return;
    }

    llvm::PHINode *phi = _ctx.builder->CreatePHI(
        _ctx.types->get_llvm_type(result, *_ctx.current_cmp_unit),
        static_cast<unsigned>(incoming.size()),
        "match");

    for (const auto &[value, block] : incoming) {
        phi->addIncoming(value, block);
    }

    _ctx.value_stack.push(phi);
}
};
