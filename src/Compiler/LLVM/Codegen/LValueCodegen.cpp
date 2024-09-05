#include "Compiler/LLVM/Codegen/LValueCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/ExprNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/ASTPlaceExpr.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

#include <fmt/core.h>

namespace Compiler::LLVM
{

LValue LValueCodegen::gen_lvalue(AST::ExprNode &expr)
{
    // dispatch on the node tag rather than rtti, matching the has_type<T>() idiom used
    // everywhere else in the tree
    switch (expr.get_node_type())
    {
        case AST::NodeType::n_varref:
        {
            auto &var_ref = static_cast<AST::VarRefNode &>(expr);
            if (!var_ref.is_var()) {
                throw _ctx.error("Unknown VarRef target type");
            }

            auto &var_node = var_ref.get_var();
            auto it = _ctx.var_map.find(&var_node.decl());
            if (it == _ctx.var_map.end()) {
                throw _ctx.error(fmt::format(
                    "Variable '{}' has no allocation in scope {}",
                    var_node.decl().name(), _ctx.function_context()));
            }

            // the alloca is the slot; the declared type is what the slot holds
            return LValue{ it->second, var_node.decl().type_node()->type };
        }

        case AST::NodeType::n_member_access:
            return gen_member_lvalue(expr);

        case AST::NodeType::n_expr_index:
        {
            // GEP over the pointee type scales the offset by the element size, so `+ 1` on a
            // ptr<int32> advances four bytes rather than one
            auto &index_expr = static_cast<AST::IndexExprNode &>(expr);

            llvm::Value *base_address = gen_address_value(*index_expr.base);

            index_expr.index->accept(*_ctx.visitor);
            llvm::Value *offset = _ctx.value_stack.top();
            _ctx.value_stack.pop();

            AST::ValueType element_type = AST::value_type_of(index_expr.base->result_type());

            llvm::Value *address = _ctx.builder->CreateGEP(
                _ctx.types->get_llvm_type(element_type, *_ctx.current_cmp_unit),
                base_address,
                { offset },
                "elem");

            return LValue{ address, element_type };
        }

        case AST::NodeType::n_expr_deref:
        {
            // a deref addresses what its operand points at, which is exactly one auto-deref
            // applied to the operand's own place
            auto &deref = static_cast<AST::DerefExprNode &>(expr);
            return gen_place(*deref.operand);
        }

        default:
            throw _ctx.error(fmt::format(
                "Expression is not addressable {}", _ctx.function_context()));
    }
}

llvm::Value *LValueCodegen::gen_load(const LValue &place, const char *name)
{
    return _ctx.builder->CreateLoad(
        _ctx.types->get_llvm_type(place.storage_type, *_ctx.current_cmp_unit),
        place.address,
        name);
}

llvm::Value *LValueCodegen::gen_load(AST::ExprNode &expr, const char *name)
{
    return gen_load(gen_lvalue(expr), name);
}

LValue LValueCodegen::deref_once(const LValue &place)
{
    if (!place.storage_type.is_pointer()) {
        return place;
    }

    // exactly one level: load the address out of the slot, and the result addresses the
    // pointee. `ptr<ptr<uint8>>` still lands on a `ptr<uint8>`, never on the uint8
    return LValue{ gen_load(place, "deref"), AST::value_type_of(place.storage_type) };
}

LValue LValueCodegen::gen_place(AST::ExprNode &expr)
{
    return deref_once(gen_lvalue(expr));
}

LValue LValueCodegen::gen_member_lvalue(AST::ExprNode &expr)
{
    auto &node = static_cast<AST::MemberAccessNode &>(expr);

    auto *base = node.get_base_node().node();
    if (base == nullptr || !AST::make_ref(base).is_expression_node()) {
        throw _ctx.error(fmt::format(
            "Unsupported base for member access '{}' {}",
            node.get_member_name().value(), _ctx.function_context()));
    }

    // gen_place, not gen_lvalue: `->` reaches through a pointer base, so a `ptr<Point>`
    // addresses the Point it points at. a value base addresses itself.
    //
    // then keep going: the member lives on the struct however many addresses deep the base is,
    // so a `ptr<ptr<Point>>` loads twice. this is the one place that peels more than one level
    // - every other read is the single auto-deref the adjustment pass already made explicit
    LValue base_place = gen_place(static_cast<AST::ExprNode &>(*base));
    while (base_place.storage_type.is_pointer()) {
        base_place = deref_once(base_place);
    }

    if (!base_place.storage_type.is_struct() || !base_place.storage_type.get_complex_type()) {
        throw _ctx.error(fmt::format(
            "Cannot access member '{}' of '{}' {}",
            node.get_member_name().value(),
            base_place.storage_type.get_type_desciption(),
            _ctx.function_context()));
    }

    auto *complex = base_place.storage_type.get_complex_type();
    const auto &member_name = node.get_member_name().value();

    // one resolution for both the GEP index and the resulting storage type
    const AST::ComplexType::Property *member = complex->find_property(member_name);
    if (member == nullptr) {
        throw _ctx.error(fmt::format(
            "Member '{}' not found in struct '{}' {}",
            member_name, complex->name.value_or("<anonymous>"), _ctx.function_context()));
    }

    auto struct_id = _ctx.current_cmp_unit->structure_table->get_structure_id(complex);
    if (struct_id == 0) {
        // a generic instantiation is lowered lazily on first use
        _ctx.types->get_llvm_type(base_place.storage_type, *_ctx.current_cmp_unit);
        struct_id = _ctx.current_cmp_unit->structure_table->get_structure_id(complex);
    }

    if (struct_id == 0) {
        throw _ctx.error(fmt::format(
            "Struct '{}' is not declared in this compilation unit {}",
            complex->name.value_or("<anonymous>"), _ctx.function_context()));
    }

    auto &structure = _ctx.current_cmp_unit->structure_table->get_structure(struct_id);

    std::vector<llvm::Value *> indices = {
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), 0),
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), member->index)
    };

    llvm::Value *address = _ctx.builder->CreateGEP(
        structure.llvm_struct, base_place.address, indices, member_name + "_ptr");

    return LValue{ address, member->type };
}

llvm::Value *LValueCodegen::gen_address_value(AST::ExprNode &expr)
{
    if (!expr.result_type().is_pointer()) {
        throw _ctx.error(fmt::format(
            "Expected a pointer expression, got '{}' {}",
            expr.result_type().get_type_desciption(), _ctx.function_context()));
    }

    // a place holding a pointer: load the slot to get the address it holds, with no deref.
    // anything else already evaluates to an address, so just let it push its value
    if (AST::is_place_expression(expr)) {
        return gen_load(expr, "addr");
    }

    expr.accept(*_ctx.visitor);
    llvm::Value *address = _ctx.value_stack.top();
    _ctx.value_stack.pop();
    return address;
}

}
