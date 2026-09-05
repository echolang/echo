#ifndef LLVMCOMPILER_H
#define LLVMCOMPILER_H

#pragma once

#include "eco.h"
#include "AST/ASTBundle.h"
#include "AST/ASTVisitor.h"

#include "Compiler/CompilerException.h"
#include "Compiler/CompilerOptions.h"
#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/Codegen/AbortCodegen.h"
#include "Compiler/LLVM/Codegen/AtomicCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/Codegen/LValueCodegen.h"
#include "Compiler/LLVM/Codegen/ExprCodegen.h"
#include "Compiler/LLVM/Codegen/StmtCodegen.h"
#include "Compiler/LLVM/Codegen/TypeDeclCodegen.h"
#include "Compiler/LLVM/Codegen/ClassCodegen.h"
#include "Compiler/LLVM/Codegen/MemoryCodegen.h"
#include "Compiler/LLVM/Codegen/StaticStorageCodegen.h"
#include "Compiler/LLVM/Codegen/ProcessCodegen.h"
#include "Compiler/LLVM/Codegen/DebugPrintCodegen.h"
#include "Compiler/LLVM/Codegen/ErasureCodegen.h"
#include "Compiler/LLVM/Codegen/DebugInfoCodegen.h"
#include "Compiler/LLVM/Codegen/Backend.h"

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

// the compiler facade and the sole AST::Visitor. it owns the shared CodegenContext and the codegen
// subsystems, orchestrates the compile of a bundle, and forwards each visit to the subsystem that
// owns that node kind. the actual lowering logic lives in the Codegen/ subsystems, not here
class LLVMCompiler : public AST::Visitor
{
public:
    LLVMCompiler(Compiler::CompilerOptions options);
    ~LLVMCompiler();

    // names the program: the module whose file-scope statements become the entry point, and - when a
    // `#[target:]` named one - the single file of it that they come from. Must be set before
    // compile_bundle; defaults to ECO_MAIN_MODULE_NAME and every file root of it
    void set_entry(const std::string &module_name, const std::filesystem::path &entry_file = {});

    // **this compile is for a test run, so no file root becomes the program at all.** Beside set_entry
    // because it is the other half of the same question and both have to be answered before
    // compile_bundle - see CodegenContext::test_mode
    void set_test_mode(bool test_mode);

    // `main` itself looks up `ECO_INTERNAL_RUN_TEST` and calls that test. only a linked runner
    // needs this; the JIT path calls each test by address and must not emit the ladder
    void set_native_test_runner(bool enabled);

    // the tests this compile will run, as mangled symbols. must be set before compile_bundle
    // so a native runner's `main` can dispatch on them and so DCE cannot drop a test nothing
    // in `main` appeared to reach
    void set_test_symbols(std::vector<std::string> symbols);

    // `cached_modules` names the modules whose compiled object is being reused, so no code is generated for
    // them at all - see TypeLowering::create_cmp_units for why that is the only place it has to be said
    void compile_bundle(const AST::Bundle &bundle, const std::set<std::string> &cached_modules = {});

    // every ODR-shared symbol defined in more than one unit must be defined *identically*, or the linker
    // keeps an arbitrary one and the program silently gets the wrong body. That is the obligation
    // AST::FunctionEmission::t_odr_shared takes on, and it is the one property of this design nothing
    // else would notice being broken: there is no diagnostic, no crash and no wrong answer at compile
    // time, only a program that behaves differently depending on which unit the linker preferred.
    //
    // so it is checked rather than assumed, in a debug compiler, right before the units are merged. It
    // costs a string render per duplicated definition and nothing at all when there are none, which is
    // every single-module program
    void verify_odr_consistency();

    // emits a body into every unit that owes one for an ODR-shared definition, until no unit owes any.
    //
    // the loop is what makes it total: emitting a body runs the same lazy declaration paths a source body
    // does, so `array<Padded>::reserve` naming `mem::realloc<Padded>` appends to the queue being drained.
    // An up-front closure could not compute this set - a cloned instance's call nodes live in the
    // template's module, not the referencing one, and an interface vtable names an implementation after
    // build_function_maps has already finished
    void drain_pending_definitions();

    // **`main`'s body: the entry module's file roots, concatenated.** Which of them is the program is
    // CodegenContext::file_is_entry's question, asked per file rather than ahead of the walk.
    //
    // **a test run has no program**, so it returns having emitted nothing rather than walking and
    // narrowing to nothing: `main` keeps the prologue its caller emitted - a test may read
    // `std::env::args()` - and ends there. The tests themselves are ordinary functions the declaration
    // walk already emitted. A native runner's `main` then dispatches; the JIT path calls each
    // symbol from TestRunner
    void emit_entry_file_roots(Compiler::LLVM::CmpUnit &main_cmp_unit);
    void emit_test_dispatch();

    void visitScope(AST::ScopeNode &node);
    void visitType(AST::TypeNode &node);
    void visitTypeCast(AST::TypeCastNode &node);
    void visitVarDecl(AST::VarDeclNode &node);
    void visit_const_decl(AST::ConstDeclNode &node);
    void visitVarRef(AST::VarRefNode &node);
    void visitLiteralFloatExpr(AST::LiteralFloatExprNode &node);
    void visitLiteralIntExpr(AST::LiteralIntExprNode &node);
    void visitLiteralBoolExpr(AST::LiteralBoolExprNode &node);
    void visitLiteralStringExpr(AST::LiteralStringExprNode &node);
    void visitFunctionCallExpr(AST::FunctionCallExprNode &node);
    void visit_addr_of_expr(AST::AddrOfExprNode &node);
    void visit_deref_expr(AST::DerefExprNode &node);
    void visit_pointer_value(AST::PointerValueNode &node);
    void visit_move_expr(AST::MoveExprNode &node);
    void visit_const_ref(AST::ConstRefExprNode &node);
    void visit_generic_value(AST::GenericValueExprNode &node);
    void visit_class_alloc_expr(AST::ClassAllocExprNode &node);
    void visit_retain_expr(AST::RetainExprNode &node);
    void visit_strong_expr(AST::StrongExprNode &node);
    void visit_guard(AST::GuardNode &node);
    void visit_null_coalesce(AST::NullCoalesceExprNode &node);
    void visit_optional_chain(AST::OptionalChainExprNode &node);
    void visit_chain_base(AST::ChainBaseNode &node);
    void visit_closure_expr(AST::ClosureExprNode &node);
    void visit_indirect_call_expr(AST::IndirectCallExprNode &node);
    void visit_function_ref_expr(AST::FunctionRefExprNode &node);
    void visit_instanceof_expr(AST::InstanceOfExprNode &node);
    void visit_temporary_bind(AST::TemporaryBindExprNode &node);
    void visit_match(AST::MatchExprNode &node);
    void visit_release(AST::ReleaseNode &node);
    void visit_index_expr(AST::IndexExprNode &node);
    void visit_array_literal_expr(AST::ArrayLiteralExprNode &node);
    void visitBinaryExpr(AST::BinaryExprNode &node);
    void visitUnaryExpr(AST::UnaryExprNode &node);
    void visitNull(AST::NullNode &node);
    void visitOperator(AST::OperatorNode &node);
    void visitFunctionDecl(AST::FunctionDeclNode &node);
    void visitReturn(AST::ReturnNode &node);
    void visitIfStatement(AST::IfStatementNode &node);
    void visit_const_if(AST::ConstIfNode &node);
    void visit_const_expr(AST::ConstExprNode &node);
    void visitWhileStatement(AST::WhileStatementNode &node);
    void visit_for_statement(AST::ForStatementNode &node);
    void visit_loop_control(AST::LoopControlNode &node);
    void visit_foreach(AST::ForeachNode &node);
    void visit_string_interpolation(AST::StringInterpolationExprNode &node);
    void visit_static_property(AST::StaticPropertyExprNode &node);
    void visit_assign(AST::AssignNode &node);
    void visitNamespaceDecl(AST::NamespaceDeclNode &node);
    void visit_use_decl(AST::UseDeclNode &node);
    void visitNamespace(AST::NamespaceNode &node);
    void visitAttribute(AST::AttributeNode &node);
    void visit_type_decl(AST::TypeDeclNode &node);
    void visitMemberAccess(AST::MemberAccessNode &node);
    void visitVar(AST::VarNode &node);

    // folds every compilation unit into the main module. Must run before any output that can only look at
    // one module - optimize(), printIR(), run_code() - and must *not* run when per-unit objects are wanted,
    // because it consumes them
    void link_into_main();

    void optimize();

    void printIR(bool toFile);
    void print_unit_ir();

    // **the JIT, with the seam between preparing it and calling into it left open** - what a test run
    // needs, since it calls a definition of its own once per test instead of running a program.
    //
    // preparing drops everything the roots cannot reach, so the module that runs is smaller than the one
    // printIR prints. `arguments` and `environment` are the *program's*, and run_main returns what it
    // returned - see Backend::prepare_execution and Backend::run_main, which own all of those decisions
    bool prepare_execution();
    int run_main(const std::vector<std::string> &arguments, const char *const *environment);
    uint64_t function_address(const std::string &mangled) const;

    // the symbols the prune keeps besides the entry point - a test run's tests. Must be set before
    // prepare_execution, which is when the prune happens
    void set_jit_roots(std::vector<std::string> roots);

    // `__eco_static_once` names pthread_self / sched_yield. the compiler emitted the
    // symbols, so it owes the link requirement - a `--no-stdlib` static must still link
    bool needs_pthread() const {
        return _ctx.needs_pthread;
    }

    // what that prune dropped, for `--explain-prune`. Empty on any path that has not run one
    const std::string &prune_report() const;

    // one object per unit that still has a module, into `object_for(unit name)`. The objects are appended to
    // `out_objects` in unit order, so the link command is deterministic
    bool emit_objects(
        const std::function<std::filesystem::path(const std::string &)> &object_for,
        std::vector<std::filesystem::path> &out_objects);

    // links what emit_objects produced, plus anything a cache supplied
    bool link_executable(
        const std::string &executable_name,
        const std::vector<std::filesystem::path> &objects,
        const std::vector<Compiler::LinkRequirement> &link
    );

private:
    Compiler::LLVM::CodegenContext _ctx;

    Compiler::LLVM::TypeLowering _types;
    Compiler::LLVM::LValueCodegen _lvalues;
    Compiler::LLVM::ExprCodegen _expr;
    Compiler::LLVM::StmtCodegen _stmt;
    Compiler::LLVM::TypeDeclCodegen _struct;
    Compiler::LLVM::ClassCodegen _classes;
    Compiler::LLVM::AbortCodegen _abort;
    Compiler::LLVM::AtomicCodegen _atomics;
    Compiler::LLVM::MemoryCodegen _memory;
    Compiler::LLVM::StaticStorageCodegen _statics;
    Compiler::LLVM::ProcessCodegen _process;
    Compiler::LLVM::DebugPrintCodegen _debug_print;
    Compiler::LLVM::ErasureCodegen _erasure;
    Compiler::LLVM::DebugInfoCodegen _debug_info;
    Compiler::LLVM::Backend _backend;
};

#endif
