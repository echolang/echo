#include "Compiler/LLVM/Codegen/LValueCodegen.h"

#include "AST/StaticPropertyExprNode.h"
#include "Compiler/LLVM/Codegen/StaticStorageCodegen.h"
#include <llvm/IR/Metadata.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Instructions.h>
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
    switch (expr.get_node_type()) {
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

        // **storage the type owns, with its initializer already run.** the address is a global
        // rather than a frame slot, and the one thing this arm does beyond handing it over is emit
        // the call that seats the value - once, here, so a read and a write do not emit two
        //
        // **typed provenance**: the address never left the compiler's accounting, so an access
        // through it carries its type's tbaa node exactly as a local's does
        case AST::NodeType::n_expr_static_property:
        {
            auto &static_property = static_cast<AST::StaticPropertyExprNode &>(expr);

            return LValue{
                _ctx.statics->gen_address(static_property),
                static_property.result_type(),
                Provenance::t_typed
            };
        }

        case AST::NodeType::n_member_access:
            return gen_member_lvalue(expr);

        case AST::NodeType::n_expr_index:
        {
            auto &index_expr = static_cast<AST::IndexExprNode &>(expr);

            // **a container's element, and it needs nothing of its own.** the element contract
            // returns a borrow, so the call's value *is* the address this function exists to
            // produce - the same `{address, what is stored there}` pair the GEP below builds.
            // that one equivalence is why reading, writing, `&` and `->` all work at once: they
            // already route through here, and none of them knows which arm answered
            if (index_expr.element_call != nullptr) {
                index_expr.element_call->accept(*_ctx.visitor);
                llvm::Value *address = _ctx.value_stack.top();
                _ctx.value_stack.pop();

                // **typed.** the container handed back a borrow of one of its own elements, and
                // what that element *is* is the element type - the only way to make that false is a
                // reinterpretation, which now needs `unsafe`. this is the arm that matters: `$a[$i]`
                // on an `array<int32>` is where a user program touches an element at all
                return LValue{ address, index_expr.result_type(), Provenance::t_typed };
            }

            // GEP over the pointee type scales the offset by the element size, so `+ 1` on a
            // ptr<int32> advances four bytes rather than one
            llvm::Value *base_address = gen_address_value(*index_expr.base);

            index_expr.indices[0]->accept(*_ctx.visitor);
            llvm::Value *offset = _ctx.value_stack.top();
            _ctx.value_stack.pop();

            AST::ValueType element_type = AST::value_type_of(index_expr.base->result_type());

            llvm::Value *address = _ctx.builder->CreateGEP(
                _ctx.types->get_llvm_type(element_type, *_ctx.current_cmp_unit),
                base_address,
                { offset },
                "elem");

            // **raw**, and this is the arm that keeps the whole scheme honest. `$p:$[3]` walks an
            // address the compiler is not accounting for - inside `mem::copy`, inside a container's
            // own body, through anything an FFI call handed back - so nothing about the storage's
            // type is knowable and nothing is claimed
            return LValue{ address, element_type, Provenance::t_raw };
        }

        case AST::NodeType::n_expr_deref:
        {
            // a deref addresses what its operand points at, which is exactly one auto-deref
            // applied to the operand's own place
            auto &deref = static_cast<AST::DerefExprNode &>(expr);
            return gen_place(*deref.operand);
        }

        case AST::NodeType::n_expr_chain_base:
        {
            // the slot a `?->` spilled its unwrapped base into, before running the continuation this
            // marker sits inside. so a method call in a chain gets an ordinary receiver address, and a
            // write through a chain has somewhere to write - both without the chain node knowing anything
            // about member access
            auto &chain_base = static_cast<AST::ChainBaseNode &>(expr);
            if (_ctx.chain_base_slots.empty()) {
                throw _ctx.error(fmt::format(
                    "a chain base marker was addressed outside a '?->' chain {}", _ctx.function_context()));
            }

            return LValue{ _ctx.chain_base_slots.back(), chain_base.type };
        }

        default:
            throw _ctx.error(fmt::format(
                "Expression is not addressable {}", _ctx.function_context()));
    }
}

llvm::Value *LValueCodegen::gen_load(const LValue &place, const char *name)
{
    llvm::LoadInst *load = _ctx.builder->CreateLoad(
        _ctx.types->get_llvm_type(place.storage_type, *_ctx.current_cmp_unit),
        place.address,
        name);

    tag_access(load, place);

    return load;
}

llvm::StoreInst *LValueCodegen::gen_store(const LValue &place, llvm::Value *value)
{
    llvm::StoreInst *store = _ctx.builder->CreateStore(value, place.address);

    tag_access(store, place);

    return store;
}

void LValueCodegen::tag_access(llvm::Instruction *access, const LValue &place)
{
    // **a raw place is left untagged, and that is the answer rather than a gap.** an instruction with
    // no `!tbaa` may alias anything, which is exactly what an address that came through a `ptr<T>`
    // deserves in a language whose reinterpretations are only a promise
    if (place.provenance != Provenance::t_typed || _ctx.tbaa == nullptr
        || _ctx.options.no_tbaa) {
        return;
    }

    if (llvm::MDNode *tag = _ctx.tbaa->scalar_tag(place.storage_type)) {
        access->setMetadata(llvm::LLVMContext::MD_tbaa, tag);
    }
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

    // **a nullable `ptr<T>` loses the provenance; a borrow keeps it.**
    //
    // the difference between the two is exactly one bit - whether null is allowed - and it happens to
    // be the bit that separates the two ways an address is obtained. a borrow comes from `&place` or
    // from a checked narrowing of one, so what it points at is what it says. a `ptr<T>` is what a
    // reinterpretation produces and what an FFI call hands back, and a program that writes `unsafe`
    // is a program where the pointee type is a claim rather than a fact
    const Provenance through = place.storage_type.is_nullable() ? Provenance::t_raw : place.provenance;

    // exactly one level: load the address out of the slot, and the result addresses the
    // pointee. `ptr<ptr<uint8>>` still lands on a `ptr<uint8>`, never on the uint8
    return LValue{ gen_load(place, "deref"), AST::value_type_of(place.storage_type), through };
}

LValue LValueCodegen::gen_place(AST::ExprNode &expr)
{
    // **a pointer with no slot of its own.** a call returning `T&` hands the address back as its
    // value - there is no storage holding it, which is why gen_lvalue has no arm for it and why
    // `$o->get()->x` and `$o->get()->m()` both died on "Expression is not addressable", an internal
    // exception with no location. that value *is* the place, the same equivalence the
    // element arm above rests on: the borrow a contract returns is the address
    //
    // the guard is the exact complement of gen_lvalue's switch - a place is what it has arms for -
    // so this can only answer where that used to throw. nothing that compiles today changes
    //
    // and it belongs here rather than in that switch, one level in rather than two: gen_lvalue would
    // have to invent a slot to hand back, and deref_once would then peel a level the expression never
    // had. for a `ptr<ptr<T>>` result that is not cosmetic - `gen_lvalue(Deref(E))` is this function,
    // and the parser spells one deref *per pointer level* into a receiver, so one peel too many points
    // `$this` at whatever the pointee happens to hold
    //
    // AST::is_place_expression stays as it is: a call is still not a place, so `&$o->get()` is still
    // refused in the parser. reading *through* the address a call returned is a different question
    if (!AST::is_place_expression(expr) && expr.result_type().is_pointer()) {
        // a *borrow* a call returned is typed for the element arm's reason; a nullable `ptr<T>` it
        // returned is not, and `mem::alloc` is exactly that
        const Provenance from_call =
            expr.result_type().is_nullable() ? Provenance::t_raw : Provenance::t_typed;

        return LValue{ gen_address_value(expr), AST::value_type_of(expr.result_type()), from_call };
    }

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
    // addresses the Point it points at. a value base addresses itself
    //
    // then keep going: the member lives on the struct however many addresses deep the base is,
    // so a `ptr<ptr<Point>>` loads twice. this is the one place that peels more than one level
    // - every other read is the single auto-deref the adjustment pass already made explicit
    LValue base_place = gen_place(static_cast<AST::ExprNode &>(*base));
    while (base_place.storage_type.is_pointer()) {
        base_place = deref_once(base_place);
    }

    if (!base_place.storage_type.has_property_layout() || !base_place.storage_type.get_complex_type()) {
        throw _ctx.error(fmt::format(
            "Cannot access member '{}' of '{}' {}",
            node.get_member_name().value(),
            base_place.storage_type.get_type_desciption(),
            _ctx.function_context()));
    }

    auto *complex = base_place.storage_type.get_complex_type();

    // one more hop for a class: the peel loop above landed on the *slot* holding the handle, so load
    // it and step into the block's payload. only the address moves - the property table below is the
    // one on `complex` either way, and the GEP is over the payload struct type, which is the layout a
    // struct with the same body would have had. that is the whole reason the block wraps a payload
    // rather than prefixing the properties with header fields, which would have shifted every index
    if (base_place.storage_type.is_class()) {
        const ClassLayout layout = _ctx.types->get_or_create_class_layout(complex, *_ctx.current_cmp_unit);

        llvm::Value *handle = gen_load(base_place, "obj");

        base_place.address = _ctx.builder->CreateStructGEP(
            layout.box, handle, ClassBox::payload_index, "payload");
    }

    const auto &member_name = node.get_member_name().value();

    // one resolution for both the GEP index and the resulting storage type
    const AST::ComplexType::Property *member = complex->find_property(member_name);
    if (member == nullptr) {
        throw _ctx.error(fmt::format(
            "Member '{}' not found in type '{}' {}",
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

    // **inherited from the base, never assumed.** a field of a local is typed; the same field
    // reached through a `ptr<Point>` that a reinterpretation produced is not, and the peel loop above
    // is where that was decided. a field is only ever as knowable as the thing holding it
    return LValue{ address, member->type, base_place.provenance };
}

llvm::Value *LValueCodegen::gen_address_value(AST::ExprNode &expr)
{
    if (!expr.result_type().is_pointer()) {
        throw _ctx.error(fmt::format(
            "Expected a pointer expression, got '{}' {}",
            expr.result_type().get_type_desciption(), _ctx.function_context()));
    }

    // a place holding a pointer: load the slot to get the address it holds, with no deref
    // anything else already evaluates to an address, so just let it push its value
    if (AST::is_place_expression(expr)) {
        return gen_load(expr, "addr");
    }

    expr.accept(*_ctx.visitor);
    llvm::Value *address = _ctx.value_stack.top();
    _ctx.value_stack.pop();
    return address;
}

};
