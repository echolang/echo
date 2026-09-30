#include "Compiler/LLVM/Codegen/PartitionHash.h"

#include "Compiler/ModuleCache.h"
#include "eco.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{

struct StableHash
{
    uint64_t digest = Compiler::k_fnv_offset_basis;
    llvm::DenseMap<const llvm::Type *, unsigned> type_ids;
    llvm::DenseSet<const llvm::Constant *> const_seen;

    void fold_bytes(const void *data, size_t length)
    {
        digest = Compiler::fnv1a64(data, length, digest);
    }

    void fold_u64(uint64_t value)
    {
        fold_bytes(&value, sizeof(value));
    }

    void fold_str(llvm::StringRef text)
    {
        fold_u64(text.size());
        fold_bytes(text.data(), text.size());
    }

    void fold_type(const llvm::Type *type)
    {
        if (type == nullptr) {
            fold_u64(0);
            return;
        }

        auto found = type_ids.find(type);
        if (found != type_ids.end()) {
            fold_u64(1);
            fold_u64(found->second);
            return;
        }

        const unsigned id = static_cast<unsigned>(type_ids.size()) + 1;
        type_ids[type] = id;
        fold_u64(2);
        fold_u64(id);
        fold_u64(static_cast<uint64_t>(type->getTypeID()));

        if (type->isIntegerTy()) {
            fold_u64(type->getIntegerBitWidth());
            return;
        }

        if (auto *pointer = llvm::dyn_cast<llvm::PointerType>(type)) {
            fold_u64(pointer->getAddressSpace());
            return;
        }

        if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
            fold_u64(array->getNumElements());
            fold_type(array->getElementType());
            return;
        }

        if (auto *vector = llvm::dyn_cast<llvm::FixedVectorType>(type)) {
            fold_u64(vector->getNumElements());
            fold_type(vector->getElementType());
            return;
        }

        if (auto *function = llvm::dyn_cast<llvm::FunctionType>(type)) {
            fold_u64(function->isVarArg() ? 1 : 0);
            fold_type(function->getReturnType());
            fold_u64(function->getNumParams());

            for (llvm::Type *param : function->params()) {
                fold_type(param);
            }

            return;
        }

        if (auto *structure = llvm::dyn_cast<llvm::StructType>(type)) {
            if (structure->isOpaque()) {
                fold_u64(1);
                fold_str(structure->getName());
                return;
            }

            fold_u64(structure->isPacked() ? 1 : 0);
            fold_u64(structure->getNumElements());

            for (llvm::Type *element : structure->elements()) {
                fold_type(element);
            }

            return;
        }
    }

    void fold_constant(const llvm::Constant *constant)
    {
        if (constant == nullptr) {
            fold_u64(0);
            return;
        }

        if (!const_seen.insert(constant).second) {
            fold_u64(1);
            return;
        }

        fold_u64(2);
        fold_u64(constant->getValueID());
        fold_type(constant->getType());

        if (auto *global = llvm::dyn_cast<llvm::GlobalValue>(constant)) {
            fold_str(global->getName());
            return;
        }

        if (auto *integer = llvm::dyn_cast<llvm::ConstantInt>(constant)) {
            const llvm::APInt &value = integer->getValue();
            fold_u64(value.getBitWidth());
            const unsigned words = value.getNumWords();
            fold_bytes(value.getRawData(), words * sizeof(uint64_t));
            return;
        }

        if (auto *floating = llvm::dyn_cast<llvm::ConstantFP>(constant)) {
            const llvm::APInt bits = floating->getValueAPF().bitcastToAPInt();
            fold_u64(bits.getBitWidth());
            fold_bytes(bits.getRawData(), bits.getNumWords() * sizeof(uint64_t));
            return;
        }

        if (auto *seq = llvm::dyn_cast<llvm::ConstantDataSequential>(constant)) {
            llvm::StringRef raw = seq->getRawDataValues();
            fold_u64(raw.size());
            fold_bytes(raw.data(), raw.size());
            return;
        }

        if (auto *as = llvm::dyn_cast<llvm::ConstantAggregateZero>(constant)) {
            (void)as;
            return;
        }

        if (llvm::isa<llvm::UndefValue>(constant) || llvm::isa<llvm::PoisonValue>(constant)
            || llvm::isa<llvm::ConstantPointerNull>(constant)
            || llvm::isa<llvm::ConstantTokenNone>(constant)) {
            return;
        }

        if (auto *expr = llvm::dyn_cast<llvm::ConstantExpr>(constant)) {
            fold_u64(expr->getOpcode());

            if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(expr)) {
                fold_type(gep->getSourceElementType());
                fold_u64(gep->isInBounds() ? 1 : 0);
            }
        }

        for (const llvm::Use &use : constant->operands()) {
            if (auto *inner = llvm::dyn_cast<llvm::Constant>(use.get())) {
                fold_constant(inner);
            }
        }
    }

    void fold_md(const llvm::Metadata *metadata, llvm::DenseSet<const llvm::Metadata *> &seen)
    {
        if (metadata == nullptr) {
            fold_u64(0);
            return;
        }

        if (!seen.insert(metadata).second) {
            fold_u64(1);
            return;
        }

        if (auto *text = llvm::dyn_cast<llvm::MDString>(metadata)) {
            fold_u64(2);
            fold_str(text->getString());
            return;
        }

        if (auto *value = llvm::dyn_cast<llvm::ValueAsMetadata>(metadata)) {
            fold_u64(3);

            if (auto *constant = llvm::dyn_cast<llvm::Constant>(value->getValue())) {
                fold_constant(constant);
            }

            return;
        }

        if (auto *node = llvm::dyn_cast<llvm::MDNode>(metadata)) {
            fold_u64(4);
            fold_u64(node->getNumOperands());

            for (const llvm::MDOperand &operand : node->operands()) {
                fold_md(operand.get(), seen);
            }
        }
    }

    void fold_attrs(const llvm::AttributeSet &attrs)
    {
        fold_u64(attrs.getNumAttributes());

        for (const llvm::Attribute &attr : attrs) {
            if (attr.isEnumAttribute()) {
                fold_u64(1);
                fold_u64(static_cast<uint64_t>(attr.getKindAsEnum()));
            }
            else if (attr.isIntAttribute()) {
                fold_u64(2);
                fold_u64(static_cast<uint64_t>(attr.getKindAsEnum()));
                fold_u64(attr.getValueAsInt());
            }
            else if (attr.isTypeAttribute()) {
                fold_u64(3);
                fold_u64(static_cast<uint64_t>(attr.getKindAsEnum()));
                fold_type(attr.getValueAsType());
            }
            else if (attr.isStringAttribute()) {
                fold_u64(4);
                fold_str(attr.getKindAsString());
                fold_str(attr.getValueAsString());
            }
            else {
                fold_u64(5);
            }
        }
    }

    void fold_value(
        const llvm::Value *value,
        llvm::DenseMap<const llvm::Value *, unsigned> &locals
    )
    {
        if (value == nullptr) {
            fold_u64(0);
            return;
        }

        if (auto *constant = llvm::dyn_cast<llvm::Constant>(value)) {
            fold_u64(1);
            fold_constant(constant);
            return;
        }

        if (auto *as = llvm::dyn_cast<llvm::InlineAsm>(value)) {
            fold_u64(2);
            fold_str(as->getAsmString());
            fold_str(as->getConstraintString());
            return;
        }

        fold_u64(3);
        auto found = locals.find(value);
        if (found == locals.end()) {
            const unsigned id = static_cast<unsigned>(locals.size()) + 1;
            locals[value] = id;
            fold_u64(id);
        }
        else {
            fold_u64(found->second);
        }
    }

    void fold_function(const llvm::Function &function, bool define)
    {
        fold_str(function.getName());
        fold_u64(define ? 1 : 0);
        fold_type(function.getFunctionType());
        fold_u64(static_cast<uint64_t>(function.getCallingConv()));
        fold_attrs(function.getAttributes().getFnAttrs());
        fold_attrs(function.getAttributes().getRetAttrs());

        for (unsigned i = 0; i < function.arg_size(); i++) {
            fold_attrs(function.getAttributes().getParamAttrs(i));
        }

        if (!define || function.isDeclaration()) {
            return;
        }

        fold_u64(static_cast<uint64_t>(function.getLinkage()));
        fold_u64(static_cast<uint64_t>(function.getVisibility()));
        fold_u64(static_cast<uint64_t>(function.getUnnamedAddr()));

        if (function.hasPersonalityFn()) {
            if (auto *personality = llvm::dyn_cast<llvm::GlobalValue>(function.getPersonalityFn())) {
                fold_str(personality->getName());
            }
        }

        llvm::DenseMap<const llvm::Value *, unsigned> locals;

        for (const llvm::Argument &arg : function.args()) {
            locals[&arg] = static_cast<unsigned>(locals.size()) + 1;
        }

        for (const llvm::BasicBlock &block : function) {
            locals[&block] = static_cast<unsigned>(locals.size()) + 1;
        }

        for (const llvm::BasicBlock &block : function) {
            fold_u64(4);

            for (const llvm::Instruction &instruction : block) {
                fold_u64(instruction.getOpcode());
                fold_u64(instruction.getRawSubclassOptionalData());
                fold_type(instruction.getType());

                if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                    fold_u64(load->getAlign().value());
                    fold_u64(static_cast<uint64_t>(load->getOrdering()));
                }
                else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                    fold_u64(store->getAlign().value());
                    fold_u64(static_cast<uint64_t>(store->getOrdering()));
                }
                else if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
                    fold_type(alloca->getAllocatedType());
                    fold_u64(alloca->getAlign().value());
                }
                else if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
                    fold_type(gep->getSourceElementType());
                }
                else if (auto *cmp = llvm::dyn_cast<llvm::CmpInst>(&instruction)) {
                    fold_u64(static_cast<uint64_t>(cmp->getPredicate()));
                }
                else if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                    fold_u64(static_cast<uint64_t>(call->getCallingConv()));

                    if (const llvm::Function *callee = call->getCalledFunction()) {
                        fold_str(callee->getName());
                    }
                }
                else if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
                    for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
                        fold_value(phi->getIncomingBlock(i), locals);
                    }
                }
                else if (auto *insert = llvm::dyn_cast<llvm::InsertValueInst>(&instruction)) {
                    for (unsigned idx : insert->indices()) {
                        fold_u64(idx);
                    }
                }
                else if (auto *extract = llvm::dyn_cast<llvm::ExtractValueInst>(&instruction)) {
                    for (unsigned idx : extract->indices()) {
                        fold_u64(idx);
                    }
                }
                else if (auto *shuffle = llvm::dyn_cast<llvm::ShuffleVectorInst>(&instruction)) {
                    for (int mask : shuffle->getShuffleMask()) {
                        fold_u64(static_cast<uint64_t>(static_cast<int64_t>(mask)));
                    }
                }
                else if (auto *atomic = llvm::dyn_cast<llvm::AtomicRMWInst>(&instruction)) {
                    fold_u64(static_cast<uint64_t>(atomic->getOperation()));
                    fold_u64(static_cast<uint64_t>(atomic->getOrdering()));
                }
                else if (auto *cmpxchg = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&instruction)) {
                    fold_u64(static_cast<uint64_t>(cmpxchg->getSuccessOrdering()));
                    fold_u64(static_cast<uint64_t>(cmpxchg->getFailureOrdering()));
                }

                fold_u64(instruction.getNumOperands());

                for (const llvm::Value *operand : instruction.operand_values()) {
                    fold_value(operand, locals);
                }

                llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> attached;
                instruction.getAllMetadata(attached);
                std::sort(attached.begin(), attached.end(), [](const auto &left, const auto &right) {
                    return left.first < right.first;
                });

                fold_u64(attached.size());

                for (const auto &item : attached) {
                    fold_u64(item.first);
                    llvm::DenseSet<const llvm::Metadata *> seen;
                    fold_md(item.second, seen);
                }
            }
        }
    }

    void fold_global(const llvm::GlobalVariable &global, bool define)
    {
        fold_str(global.getName());
        fold_u64(define ? 1 : 0);
        fold_type(global.getValueType());
        fold_u64(global.isConstant() ? 1 : 0);
        fold_u64(static_cast<uint64_t>(global.getLinkage()));
        fold_u64(global.getAlignment());

        if (define && global.hasInitializer()) {
            fold_constant(global.getInitializer());
        }
    }

    void fold_alias(const llvm::GlobalAlias &alias, bool define)
    {
        fold_str(alias.getName());
        fold_u64(define ? 1 : 0);
        fold_type(alias.getValueType());

        if (define && alias.getAliasee()) {
            fold_constant(alias.getAliasee());
        }
    }
};

};

std::string Compiler::LLVM::partition_hex(
    llvm::Module &module,
    const std::unordered_set<const llvm::GlobalValue *> &needed,
    const std::function<bool(const llvm::GlobalValue *)> &define,
    const Compiler::LLVM::PartitionEnv &env
)
{
    StableHash hash;

    std::vector<const llvm::Function *> functions;
    std::vector<const llvm::GlobalVariable *> globals;
    std::vector<const llvm::GlobalAlias *> aliases;

    for (const llvm::Function &function : module) {
        if (needed.find(&function) != needed.end()) {
            functions.push_back(&function);
        }
    }

    for (const llvm::GlobalVariable &global : module.globals()) {
        if (needed.find(&global) != needed.end()) {
            globals.push_back(&global);
        }
    }

    for (const llvm::GlobalAlias &alias : module.aliases()) {
        if (needed.find(&alias) != needed.end()) {
            aliases.push_back(&alias);
        }
    }

    auto by_name = [](const llvm::GlobalValue *left, const llvm::GlobalValue *right) {
        return left->getName() < right->getName();
    };

    std::sort(functions.begin(), functions.end(), by_name);
    std::sort(globals.begin(), globals.end(), by_name);
    std::sort(aliases.begin(), aliases.end(), by_name);

    hash.fold_u64(functions.size());

    for (const llvm::Function *function : functions) {
        hash.fold_function(*function, define(function));
    }

    hash.fold_u64(globals.size());

    for (const llvm::GlobalVariable *global : globals) {
        hash.fold_global(*global, define(global));
    }

    hash.fold_u64(aliases.size());

    for (const llvm::GlobalAlias *alias : aliases) {
        hash.fold_alias(*alias, define(alias));
    }

    hash.digest = Compiler::fnv1a64(env.triple, hash.digest);
    hash.digest = Compiler::fnv1a64(env.cpu, hash.digest);
    hash.digest = Compiler::fnv1a64(env.features, hash.digest);
    hash.digest = Compiler::fnv1a64(
        env.no_optimize ? std::string("noopt") : std::string("opt"), hash.digest);
    hash.digest = Compiler::fnv1a64(
        env.targeting_windows ? std::string("coff") : std::string("notcoff"), hash.digest);
    hash.digest = Compiler::fnv1a64(std::string(ECO_MODULE_CACHE_VERSION), hash.digest);
    hash.digest = Compiler::fnv1a64(std::string(LLVM_VERSION_STRING), hash.digest);

    return Compiler::to_hex(hash.digest);
}
