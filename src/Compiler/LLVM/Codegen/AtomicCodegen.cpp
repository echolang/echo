#include "Compiler/LLVM/Codegen/AtomicCodegen.h"

#include "Compiler/LLVM/Codegen/LValueCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ASTPlaceExpr.h"

#include <llvm/IR/Instructions.h>

#include <fmt/core.h>

namespace Compiler::LLVM
{
    LValue AtomicCodegen::slot_of(
        AST::FunctionCallExprNode &node,
        const char *name,
        size_t arity
    )
    {
        if (node.arguments.size() != arity) {
            throw _ctx.error(fmt::format(
                "'{}' takes exactly {} argument(s) {}", name, arity, _ctx.function_context()));
        }

        for (const auto &argument : node.arguments) {
            if (argument == nullptr) {
                throw _ctx.error(fmt::format(
                    "'{}' has a null argument {}", name, _ctx.function_context()));
            }
        }

        const AST::ValueType place_type = node.arguments[0]->result_type();

        if (!place_type.is_pointer()) {
            throw _ctx.error(fmt::format(
                "'{}' expects the address of a place, got '{}' {}",
                name, place_type.get_type_desciption(), _ctx.function_context()));
        }

        node.arguments[0]->accept(*_ctx.visitor);

        return LValue{ _ctx.pop(), AST::value_type_of(place_type) };
    }

    void AtomicCodegen::gen_atomic_builtin(
        AST::FunctionCallExprNode &node,
        AST::BuiltinKind kind
    )
    {
        // sequentially consistent, always. an ordering is a claim about two accesses and
        // nothing in the language can check it, so the surface does not take one
        const llvm::AtomicOrdering order = llvm::AtomicOrdering::SequentiallyConsistent;

        if (kind == AST::BuiltinKind::t_atomic_fence) {
            _ctx.builder->CreateFence(order);
            return;
        }

        if (node.decl->instantiation_args.size() != 1) {
            throw _ctx.error(fmt::format(
                "Builtin '{}' expects exactly one type argument, got {} {}",
                node.decl->builtin.value(), node.decl->instantiation_args.size(),
                _ctx.function_context()));
        }

        const AST::ValueType subject = node.decl->instantiation_args[0];

        // LLVM cannot emit an atomic i1, so a bool rides as i8 and is narrowed back. the
        // rest of T is already a word AST::atomic_operand_refusal admitted
        const bool as_bool = subject.is_boolean_type();
        llvm::Type *access_type = as_bool
            ? llvm::Type::getInt8Ty(*_ctx.llvm_context)
            : _ctx.types->get_llvm_type(subject, *_ctx.current_cmp_unit);
        const llvm::Align align = _ctx.layout().getABITypeAlign(access_type);

        const char *name = "atomic";
        size_t arity = 1;

        switch (kind) {
            case AST::BuiltinKind::t_atomic_load:
                name = "atomic.load";
                arity = 1;
                break;
            case AST::BuiltinKind::t_atomic_store:
                name = "atomic.store";
                arity = 2;
                break;
            case AST::BuiltinKind::t_atomic_add:
                name = "atomic.add";
                arity = 2;
                break;
            case AST::BuiltinKind::t_atomic_sub:
                name = "atomic.sub";
                arity = 2;
                break;
            case AST::BuiltinKind::t_atomic_exchange:
                name = "atomic.xchg";
                arity = 2;
                break;
            case AST::BuiltinKind::t_atomic_compare_exchange:
                name = "atomic.cas";
                arity = 3;
                break;
            default:
                throw _ctx.error(fmt::format(
                    "Builtin '{}' is not an atomic verb {}",
                    node.decl->builtin.value(), _ctx.function_context()));
        }

        const LValue place = slot_of(node, name, arity);

        auto widen = [&](llvm::Value *value) -> llvm::Value * {
            if (!as_bool) {
                return value;
            }

            return _ctx.builder->CreateZExt(
                value, llvm::Type::getInt8Ty(*_ctx.llvm_context), "atomic.bool");
        };

        auto narrow = [&](llvm::Value *value, const char *result_name) -> llvm::Value * {
            if (!as_bool) {
                return value;
            }

            return _ctx.builder->CreateICmpNE(
                value,
                llvm::ConstantInt::get(llvm::Type::getInt8Ty(*_ctx.llvm_context), 0),
                result_name);
        };

        auto eval_value = [&](size_t index) -> llvm::Value * {
            node.arguments[index]->accept(*_ctx.visitor);
            return _ctx.types->coerce_value(
                _ctx.pop(),
                node.arguments[index]->result_type(),
                subject,
                *_ctx.current_cmp_unit);
        };

        switch (kind) {
            case AST::BuiltinKind::t_atomic_load: {
                llvm::LoadInst *load =
                    _ctx.builder->CreateLoad(access_type, place.address, "atomic.load");
                load->setAtomic(order);
                load->setAlignment(align);
                _ctx.push(narrow(load, "atomic.load.bool"));
                return;
            }

            case AST::BuiltinKind::t_atomic_store: {
                llvm::StoreInst *store = _ctx.builder->CreateStore(
                    widen(eval_value(1)), place.address);
                store->setAtomic(order);
                store->setAlignment(align);
                return;
            }

            case AST::BuiltinKind::t_atomic_add:
            case AST::BuiltinKind::t_atomic_sub:
            case AST::BuiltinKind::t_atomic_exchange: {
                const auto op = kind == AST::BuiltinKind::t_atomic_add
                    ? llvm::AtomicRMWInst::Add
                    : kind == AST::BuiltinKind::t_atomic_sub
                        ? llvm::AtomicRMWInst::Sub
                        : llvm::AtomicRMWInst::Xchg;
                llvm::Value *prev = _ctx.builder->CreateAtomicRMW(
                    op, place.address, widen(eval_value(1)), align, order);
                prev->setName(name);
                _ctx.push(narrow(prev, "atomic.prev.bool"));
                return;
            }

            case AST::BuiltinKind::t_atomic_compare_exchange: {
                llvm::AtomicCmpXchgInst *cas = _ctx.builder->CreateAtomicCmpXchg(
                    place.address,
                    widen(eval_value(1)),
                    widen(eval_value(2)),
                    align,
                    order,
                    order);
                cas->setName("atomic.cas");
                _ctx.push(_ctx.builder->CreateExtractValue(cas, 1, "atomic.cas.ok"));
                return;
            }

            default:
                throw _ctx.error(fmt::format(
                    "Builtin '{}' is not an atomic verb {}",
                    node.decl->builtin.value(), _ctx.function_context()));
        }
    }
};
