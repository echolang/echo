#include "Compiler/LLVM/Codegen/CoverageCodegen.h"

#include "AST/ASTFile.h"
#include "AST/ASTFunctionProfile.h"
#include "AST/ASTMangler.h"
#include "AST/ASTModule.h"
#include "AST/ASTSourceToken.h"
#include "AST/FunctionDeclNode.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <eco_coverage.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include <fmt/core.h>

#include <cstdint>
#include <string>

namespace Compiler::LLVM
{

namespace
{
    constexpr uint32_t k_skip_site = 0xffffffffu;

    std::string module_name_of(const AST::FunctionDeclNode *decl)
    {
        if (decl == nullptr || decl->declared_in.module == nullptr) {
            return {};
        }

        return decl->declared_in.module->name;
    }

    std::string module_dir_of(const AST::FunctionDeclNode *decl)
    {
        if (decl == nullptr || decl->declared_in.module == nullptr) {
            return {};
        }

        return decl->declared_in.module->directory.generic_string();
    }

    bool has_source_token(const AST::Node &node)
    {
        const TokenReference *token = AST::source_token_of(node);
        return token != nullptr && token->is_valid() && !token->is_minted();
    }
};

bool CoverageCodegen::active() const
{
    return _ctx.options.coverage();
}

bool CoverageCodegen::should_instrument(const AST::FunctionDeclNode *decl) const
{
    return active() && AST::coverage_instruments(decl, _ctx.options.coverage_stdlib());
}

CoverageCodegen::FunctionState *CoverageCodegen::state_for(const AST::FunctionDeclNode *decl)
{
    if (decl == nullptr) {
        return nullptr;
    }

    const std::string mangled = AST::mangle_function_name(decl);
    if (mangled.empty()) {
        return nullptr;
    }

    FunctionState &state = _functions[mangled];
    if (state.mangled.empty()) {
        state.mangled = mangled;
        state.module = module_name_of(decl);
        state.dir = module_dir_of(decl);
        state.function = decl->func_name();
    }

    return &state;
}

std::string CoverageCodegen::site_key(
    FunctionState &state,
    Compiler::CoverageSiteKind kind,
    std::string_view edge,
    const AST::Node &node
) const
{
    if (kind == Compiler::CoverageSiteKind::t_fn) {
        return "F";
    }

    const AST::SourceSite at = AST::source_site_of(node);
    if (kind == Compiler::CoverageSiteKind::t_line) {
        return fmt::format("L:{}:{}", at.file, at.line);
    }

    const uint32_t seq = state.branch_seq++;
    return fmt::format("B:{}:{}:{}:{}:{}", at.file, at.line, at.column, edge, seq);
}

llvm::StructType *CoverageCodegen::reg_type()
{
    if (auto *existing = llvm::StructType::getTypeByName(*_ctx.llvm_context, "eco.cov.reg")) {
        return existing;
    }

    llvm::Type *ptr = _ctx.opaque_ptr_type();
    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    return llvm::StructType::create(
        *_ctx.llvm_context, { ptr, ptr, i32, i32, ptr, ptr, ptr }, "eco.cov.reg");
}

llvm::GlobalVariable *CoverageCodegen::ensure_base(FunctionState &state)
{
    llvm::Module *module = _ctx.current_module();
    const std::string name = fmt::format(
        "__eco_cov_base_{}", Compiler::coverage_module_stem(state.mangled));
    if (auto *existing = module->getGlobalVariable(name, true)) {
        return existing;
    }

    llvm::PointerType *ptr = llvm::cast<llvm::PointerType>(_ctx.opaque_ptr_type());
    auto *global = new llvm::GlobalVariable(
        *module,
        ptr,
        false,
        llvm::GlobalValue::LinkOnceODRLinkage,
        llvm::ConstantPointerNull::get(ptr),
        name);
    global->setAlignment(_ctx.layout().getABITypeAlign(ptr));
    return global;
}

uint32_t CoverageCodegen::add_site(
    FunctionState &state,
    Compiler::CoverageSiteKind kind,
    std::string_view edge,
    const AST::Node &node
)
{
    const std::string key = site_key(state, kind, edge, node);
    auto found = state.site_ids.find(key);
    if (found != state.site_ids.end()) {
        return found->second;
    }

    if (state.sealed) {
        return k_skip_site;
    }

    const AST::SourceSite at = AST::source_site_of(node);

    Compiler::CoverageSite site;
    site.id = static_cast<uint32_t>(state.sites.size());
    site.module = state.module;
    site.dir = state.dir;
    site.file = at.file;
    site.line = at.line;
    site.column = at.column;
    site.kind = kind;
    site.edge = std::string(edge);
    site.function = state.function;

    state.site_ids.emplace(key, site.id);
    state.sites.push_back(std::move(site));
    return state.sites.back().id;
}

void CoverageCodegen::increment(FunctionState &state, uint32_t id)
{
    if (id == k_skip_site) {
        return;
    }

    llvm::IRBuilder<> *builder = _ctx.builder.get();
    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    llvm::Value *base = builder->CreateLoad(
        _ctx.opaque_ptr_type(), ensure_base(state), "eco.cov.base");

    llvm::Value *slot = builder->CreateGEP(
        i32,
        base,
        llvm::ConstantInt::get(i32, id),
        "eco.cov.slot");

    llvm::LoadInst *loaded = builder->CreateLoad(i32, slot, "eco.cov.n");
    loaded->setVolatile(true);

    llvm::Value *one = llvm::ConstantInt::get(i32, 1);
    llvm::Value *sum = builder->CreateAdd(loaded, one, "eco.cov.sum");
    llvm::Value *maxed = builder->CreateICmpEQ(
        loaded, llvm::ConstantInt::get(i32, 0xffffffffu), "eco.cov.sat");
    llvm::Value *next = builder->CreateSelect(maxed, loaded, sum, "eco.cov.next");

    llvm::StoreInst *stored = builder->CreateStore(next, slot);
    stored->setVolatile(true);
}

void CoverageCodegen::plant(
    Compiler::CoverageSiteKind kind,
    std::string_view edge,
    const AST::Node &node
)
{
    if (!should_instrument(_ctx.current_function)) {
        return;
    }

    FunctionState *state = state_for(_ctx.current_function);
    if (state == nullptr) {
        return;
    }

    increment(*state, add_site(*state, kind, edge, node));
}

void CoverageCodegen::instrument_function(llvm::Function &fn, const AST::FunctionDeclNode &decl)
{
    if (!should_instrument(&decl)) {
        return;
    }

    FunctionState *state = state_for(&decl);
    if (state == nullptr) {
        return;
    }

    if (!state->sites.empty()) {
        state->sealed = true;
    }
    state->branch_seq = 0;

    // the builder is at the end of the prologue. rewinding to firstNonPHI would plant
    // t_fn in front of the argument stores and the body's ret would land between them
    increment(*state, add_site(*state, Compiler::CoverageSiteKind::t_fn, "", decl));
}

void CoverageCodegen::instrument_statement(AST::Node &node)
{
    if (!has_source_token(node)) {
        return;
    }

    plant(Compiler::CoverageSiteKind::t_line, "", node);
}

void CoverageCodegen::instrument_branch(AST::Node &node, std::string_view edge)
{
    plant(Compiler::CoverageSiteKind::t_branch, edge, node);
}

llvm::BasicBlock *CoverageCodegen::false_edge(
    llvm::BasicBlock *merge,
    AST::Node &node,
    std::string_view edge
)
{
    if (!should_instrument(_ctx.current_function) || merge == nullptr) {
        return merge;
    }

    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();
    auto *skip = llvm::BasicBlock::Create(*_ctx.llvm_context, "cov.skip", function);
    llvm::IRBuilderBase::InsertPointGuard guard(*_ctx.builder);
    _ctx.set_insert_point(skip);
    instrument_branch(node, edge);
    _ctx.builder->CreateBr(merge);
    return skip;
}

void CoverageCodegen::emit_start()
{
    if (!active()) {
        return;
    }

    llvm::Module *module = _ctx.current_module();
    llvm::FunctionCallee start = module->getOrInsertFunction(
        "__eco_cov_start",
        llvm::FunctionType::get(llvm::Type::getVoidTy(*_ctx.llvm_context), false));
    _ctx.builder->CreateCall(start);
}

void CoverageCodegen::emit_function_reg(llvm::Module &owner, FunctionState &state)
{
    const std::string stem = Compiler::coverage_module_stem(state.mangled);
    const std::string base_name = fmt::format("__eco_cov_base_{}", stem);
    if (owner.getGlobalVariable(base_name, true) == nullptr) {
        return;
    }

    llvm::LLVMContext &context = *_ctx.llvm_context;
    llvm::Type *i32 = llvm::Type::getInt32Ty(context);
    llvm::StructType *reg = reg_type();
    llvm::PointerType *ptr = llvm::cast<llvm::PointerType>(_ctx.opaque_ptr_type());
    llvm::Type *void_ty = llvm::Type::getVoidTy(context);
    const uint32_t n = static_cast<uint32_t>(state.sites.size());
    llvm::ArrayType *counts_ty = llvm::ArrayType::get(i32, n);

    const std::string counts_name = fmt::format("__eco_cov_counts_{}", stem);
    llvm::GlobalVariable *counts = owner.getGlobalVariable(counts_name, true);
    if (counts == nullptr) {
        counts = new llvm::GlobalVariable(
            owner,
            counts_ty,
            false,
            llvm::GlobalValue::LinkOnceODRLinkage,
            llvm::ConstantAggregateZero::get(counts_ty),
            counts_name);
        counts->setAlignment(_ctx.layout().getABITypeAlign(counts_ty));
    }

    llvm::GlobalVariable *base = owner.getGlobalVariable(base_name, true);

    const std::string reg_name = fmt::format("__eco_cov_reg_{}", stem);
    if (owner.getGlobalVariable(reg_name, true) == nullptr) {
        llvm::Constant *name_init = llvm::ConstantDataArray::getString(context, state.module);
        llvm::GlobalVariable *name_gv = new llvm::GlobalVariable(
            owner,
            name_init->getType(),
            true,
            llvm::GlobalValue::PrivateLinkage,
            name_init,
            fmt::format("__eco_cov_mod_{}", stem));

        llvm::Constant *id_init = llvm::ConstantDataArray::getString(context, state.mangled);
        llvm::GlobalVariable *id_gv = new llvm::GlobalVariable(
            owner,
            id_init->getType(),
            true,
            llvm::GlobalValue::PrivateLinkage,
            id_init,
            fmt::format("__eco_cov_id_{}", stem));

        llvm::Constant *reg_init = llvm::ConstantStruct::get(reg, {
            counts,
            base,
            llvm::ConstantInt::get(i32, n),
            llvm::ConstantInt::get(i32, 0),
            name_gv,
            id_gv,
            llvm::ConstantPointerNull::get(ptr)
        });

        new llvm::GlobalVariable(
            owner,
            reg,
            false,
            llvm::GlobalValue::LinkOnceODRLinkage,
            reg_init,
            reg_name);
    }

    const std::string ctor_name = fmt::format("__eco_cov_init_{}", stem);
    if (owner.getFunction(ctor_name) != nullptr) {
        return;
    }

    llvm::Function *ctor = llvm::Function::Create(
        llvm::FunctionType::get(void_ty, false),
        llvm::GlobalValue::LinkOnceODRLinkage,
        ctor_name,
        &owner);

    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", ctor));
    llvm::FunctionCallee register_fn = owner.getOrInsertFunction(
        "__eco_cov_register",
        llvm::FunctionType::get(void_ty, { ptr }, false));
    builder.CreateCall(register_fn, { owner.getGlobalVariable(reg_name, true) });
    builder.CreateRetVoid();
    llvm::appendToGlobalCtors(owner, ctor, 65535);
}

void CoverageCodegen::finish_units()
{
    if (!active()) {
        return;
    }

    CmpUnit *saved = _ctx.current_cmp_unit;
    for (auto &unit : _ctx.cmp_units) {
        if (unit == nullptr || unit->llvm_module == nullptr) {
            continue;
        }

        _ctx.current_cmp_unit = unit.get();
        for (auto &[_, state] : _functions) {
            emit_function_reg(*unit->llvm_module, state);
        }
    }
    _ctx.current_cmp_unit = saved;
}

Compiler::CoverageModuleSites CoverageCodegen::module_sites() const
{
    Compiler::CoverageModuleSites out;
    for (const auto &[_, state] : _functions) {
        if (state.module.empty()) {
            continue;
        }

        std::vector<Compiler::CoverageSite> &packed = out[state.module];
        for (Compiler::CoverageSite site : state.sites) {
            site.id = static_cast<uint32_t>(packed.size());
            packed.push_back(std::move(site));
        }
    }

    return out;
}

};
