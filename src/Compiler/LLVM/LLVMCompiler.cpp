#include "Compiler/LLVM/LLVMCompiler.h"

#include "AST/FunctionDeclNode.h"
#include "AST/LoopControlNode.h"
#include "AST/ForeachNode.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/raw_ostream.h>

#include <fmt/core.h>

LLVMCompiler::LLVMCompiler(Compiler::CompilerOptions options)
    : _types(_ctx), _lvalues(_ctx), _expr(_ctx), _stmt(_ctx), _struct(_ctx), _classes(_ctx),
      _abort(_ctx), _debug_print(_ctx), _backend(_ctx)
{
    // what the invocation asked for, before any subsystem can read it
    _ctx.options = options;

    // wire the shared context back to the single visitor (for accept-recursion) and to the
    // type-lowering subsystem (reachable from every other subsystem)
    _ctx.visitor = this;
    _ctx.types = &_types;
    _ctx.lvalues = &_lvalues;
    _ctx.classes = &_classes;
    _ctx.abort = &_abort;
    _ctx.debug_print = &_debug_print;
}

LLVMCompiler::~LLVMCompiler()
{
}

void LLVMCompiler::compile_bundle(const AST::Bundle &bundle)
{
    _ctx.llvm_context = std::make_unique<llvm::LLVMContext>();
    _ctx.builder = std::make_unique<llvm::IRBuilder<>>(*_ctx.llvm_context);

    // which declared type is `string`, published before anything is lowered: gen_literal_string builds a
    // constant of it, so it has to be answerable from the first expression onward
    _ctx.core_types_ptr = &bundle.collector.core_types;

    // the registry an interface widening reads, published beside the core types and for the same
    // reason - see CodegenContext. a widening resolves its vtable from the concrete class, which means
    // asking which declaration answers each requirement
    //
    // the bundle arrives const and TypeRegistry's interning is a mutating operation, so this casts it
    // away. that is sound rather than convenient: the type checker resolved every one of these
    // conformances before codegen ran, so each substitution the vtable walk performs is a lookup that
    // hits the cache. it interns nothing new, which is exactly the property that has to hold after
    // monomorphization anyway
    _ctx.type_registry_ptr = const_cast<AST::TypeRegistry *>(&bundle.collector.type_registry);

    // and *where its fields sit*, resolved here, once. absence of a binding is not an error - a program
    // compiled without the stdlib has no string - but a bound type of the wrong shape is, and reporting
    // it here rather than at whichever literal happened to be lowered first is what makes the message
    // about the stdlib declaration it is actually about
    if (_ctx.core_types().has(AST::CoreTypeKind::t_string)
        || _ctx.core_types().has(AST::CoreTypeKind::t_string_view)) {
        std::string layout_error;
        _ctx.string_layout = AST::resolve_core_string_layout(_ctx.core_types(), layout_error);

        if (!_ctx.string_layout.has_value()) {
            throw _ctx.error(fmt::format("the declared core string types are unusable: {}", layout_error));
        }
    }

    // resolve the host target first: create_cmp_units stamps its data layout onto every module,
    // and a compile-time size_of<T>() reads it
    _backend.init_target();

    // initialize the compilation units
    _types.create_cmp_units(bundle);

    // build the struct maps
    _types.build_struct_maps();

    // build the function maps
    _types.build_function_maps();

    // always declare printf @TODO make this a bit more dynamic..
    //
    // eagerly into every unit rather than lazily on first use, unlike the other emitted-runtime
    // symbols - `echo` fetches it back by name at the call site rather than through a getter
    for (auto &cmp_unit : _ctx.cmp_units) {
        _ctx.current_cmp_unit = cmp_unit.get();
        _ctx.libc_callee("printf",
            llvm::Type::getInt32Ty(*_ctx.llvm_context),
            { _ctx.opaque_ptr_type() },
            /*variadic=*/true);
    }

    // fetch all function declarations inside of the module
    for (auto &cmpu : _ctx.cmp_units) {
        _ctx.current_cmp_unit = cmpu.get();

        for (auto &file : _ctx.current_cmp_unit->ast_module->files()) {
            _ctx.current_file = &file;

            for (auto &node : file.root->children) {
                if (node.has_type<AST::FunctionDeclNode>()) {
                    auto func_decl = node.get<AST::FunctionDeclNode>();
                    func_decl.accept(*this);
                }
            }
        }
    }

    // search for the main module
    Compiler::LLVM::CmpUnit *main_cmp_unit = _ctx.main_cmp_unit();   
    if (!main_cmp_unit) {
        throw Compiler::InternalCompilerException("No main module found in the bundle", nullptr);
    }

    llvm::FunctionType *funcType = llvm::FunctionType::get(_ctx.builder->getInt32Ty(), false);
    llvm::Function *function = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, "main", main_cmp_unit->llvm_module.get());
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", function);
    _ctx.builder->SetInsertPoint(entry);

    _ctx.current_cmp_unit = main_cmp_unit;

    // visit all nodes in the main module
    for (auto &file : main_cmp_unit->ast_module->files()) {
        // a file whose top level stopped the program - a `die` at module scope - leaves the block
        // terminated, and everything after it, including the next file, is unreachable. the same
        // question StmtCodegen::gen_scope asks after every statement, asked across files because
        // this is the one function body not emitted through gen_function_decl
        if (_ctx.block_is_terminated()) {
            break;
        }

        _ctx.current_file = &file;
        file.root->accept(*this);
    }

    // terminate the function, unless the program already stopped itself
    if (!_ctx.block_is_terminated()) {
        _ctx.builder->CreateRet(_ctx.builder->getInt32(0));
    }

    // verify the main module before linking
    std::string error_str;
    llvm::raw_string_ostream error_stream(error_str);
    if (llvm::verifyModule(*main_cmp_unit->llvm_module, &error_stream)) {
        throw Compiler::InternalCompilerException(fmt::format(
            "LLVM IR verification failed for main module:\n{}", error_str
        ));
    }

    // link all modules together into the main module
    auto linker = llvm::Linker(*main_cmp_unit->llvm_module);

    for (auto &cmpu : _ctx.cmp_units) {

        // skip the main module
        if (cmpu.get() == main_cmp_unit) {
            continue;
        }

        if (linker.linkInModule(std::move(cmpu->llvm_module))) {
            throw Compiler::InternalCompilerException(fmt::format(
                "Failed to link module '{}'.\n{}", 
                cmpu->ast_module->name,
                _ctx.llvm_err_str()
            ));
        }
        cmpu->llvm_module.reset();
    }

    // optimize the module
    // optimize();
}

// -- visitor facade -----------------------------------------------------------
// each visit forwards to the subsystem that owns the node kind; the structural no-ops below emit
// nothing on purpose

void LLVMCompiler::visitScope(AST::ScopeNode &node) { _stmt.gen_scope(node); }
void LLVMCompiler::visitVarDecl(AST::VarDeclNode &node) { _stmt.gen_var_decl(node); }
void LLVMCompiler::visitFunctionDecl(AST::FunctionDeclNode &node) { _stmt.gen_function_decl(node); }
void LLVMCompiler::visitReturn(AST::ReturnNode &node) { _stmt.gen_return(node); }
void LLVMCompiler::visitIfStatement(AST::IfStatementNode &node) { _stmt.gen_if_statement(node); }
void LLVMCompiler::visitWhileStatement(AST::WhileStatementNode &node) { _stmt.gen_while_statement(node); }
void LLVMCompiler::visit_loop_control(AST::LoopControlNode &node) { _stmt.gen_loop_control(node); }

// a `foreach` is lowered away inside the monomorphizer's fixpoint, into the iterator declaration and the
// `while` a hand-written loop would have been. one reaching here means AST::ForeachLowering neither
// lowered nor discarded it, which is AST::PointerValueNode's contract: a marker a pass was supposed to
// erase is a compiler bug, and answering with something plausible would hide it
void LLVMCompiler::visit_foreach(AST::ForeachNode &node)
{
    throw _ctx.error("a 'foreach' survived the monomorphizer's fixpoint - it should have been lowered "
        "into an iterator and a while " + _ctx.function_context());
}
void LLVMCompiler::visit_assign(AST::AssignNode &node) { _stmt.gen_assign(node); }

void LLVMCompiler::visitTypeCast(AST::TypeCastNode &node) { _expr.gen_type_cast(node); }
void LLVMCompiler::visitVarRef(AST::VarRefNode &node) { _expr.gen_var_ref(node); }
void LLVMCompiler::visitLiteralFloatExpr(AST::LiteralFloatExprNode &node) { _expr.gen_literal_float(node); }
void LLVMCompiler::visitLiteralIntExpr(AST::LiteralIntExprNode &node) { _expr.gen_literal_int(node); }
void LLVMCompiler::visitLiteralBoolExpr(AST::LiteralBoolExprNode &node) { _expr.gen_literal_bool(node); }
void LLVMCompiler::visitLiteralStringExpr(AST::LiteralStringExprNode &node) { _expr.gen_literal_string(node); }
void LLVMCompiler::visitFunctionCallExpr(AST::FunctionCallExprNode &node) { _expr.gen_function_call(node); }
void LLVMCompiler::visit_addr_of_expr(AST::AddrOfExprNode &node) { _expr.gen_addr_of(node); }
void LLVMCompiler::visit_deref_expr(AST::DerefExprNode &node) { _expr.gen_deref(node); }
void LLVMCompiler::visit_index_expr(AST::IndexExprNode &node) { _expr.gen_index(node); }

// an array literal is a *statement-level* construct that AST::OperatorRewriter expands into a
// constructor call plus one append per element, so nothing reaches codegen with one still in the
// tree. it throws for AST::PointerValueNode's reason: a marker a pass is supposed to have erased is
// a compiler bug, and answering with something plausible would hide it
void LLVMCompiler::visit_array_literal_expr(AST::ArrayLiteralExprNode &node)
{
    throw _ctx.error("an array literal survived to codegen - it should have been expanded into a "
        "constructor and appends " + _ctx.function_context());
}

// a peel marker is erased by the pointer adjustment pass; one surviving to codegen means the
// pass missed a position, which would otherwise silently emit the wrong number of loads
void LLVMCompiler::visit_pointer_value(AST::PointerValueNode &node)
{
    throw _ctx.error("':$' survived the pointer adjustment pass");
}
// same contract as the peel marker above: AST::OwnershipPass erases every `mv` once it has read
// it. one reaching codegen would mean a move was never resolved, and the copy it was meant to
// replace is still there
void LLVMCompiler::visit_move_expr(AST::MoveExprNode &node)
{
    throw _ctx.error("'mv' survived the ownership pass");
}
void LLVMCompiler::visit_class_alloc_expr(AST::ClassAllocExprNode &node) { _classes.gen_class_alloc(node); }
void LLVMCompiler::visit_retain_expr(AST::RetainExprNode &node) { _classes.gen_retain_expr(node); }
void LLVMCompiler::visit_strong_expr(AST::StrongExprNode &node) { _expr.gen_strong_expr(node); }
void LLVMCompiler::visit_guard(AST::GuardNode &node) { _stmt.gen_guard(node); }
void LLVMCompiler::visit_null_coalesce(AST::NullCoalesceExprNode &node) { _expr.gen_null_coalesce(node); }
void LLVMCompiler::visit_optional_chain(AST::OptionalChainExprNode &node) { _expr.gen_optional_chain(node); }
void LLVMCompiler::visit_chain_base(AST::ChainBaseNode &node) { _expr.gen_chain_base(node); }
void LLVMCompiler::visit_closure_expr(AST::ClosureExprNode &node) { _expr.gen_closure_expr(node); }
void LLVMCompiler::visit_indirect_call_expr(AST::IndirectCallExprNode &node) { _expr.gen_indirect_call(node); }
void LLVMCompiler::visit_instanceof_expr(AST::InstanceOfExprNode &node) { _classes.gen_instanceof(node); }
void LLVMCompiler::visit_temporary_bind(AST::TemporaryBindExprNode &node) { _expr.gen_temporary_bind(node); }
void LLVMCompiler::visit_release(AST::ReleaseNode &node) { _classes.gen_release_stmt(node); }
void LLVMCompiler::visitBinaryExpr(AST::BinaryExprNode &node) { _expr.gen_binary_expr(node); }
void LLVMCompiler::visitUnaryExpr(AST::UnaryExprNode &node) { _expr.gen_unary_expr(node); }
void LLVMCompiler::visitNull(AST::NullNode &node) { _expr.gen_null(node); }
void LLVMCompiler::visitOperator(AST::OperatorNode &node) { _expr.gen_operator(node); }

void LLVMCompiler::visit_type_decl(AST::TypeDeclNode &node) { _struct.gen_type_decl(node); }
void LLVMCompiler::visitMemberAccess(AST::MemberAccessNode &node) { _struct.gen_member_access(node); }
void LLVMCompiler::visitVar(AST::VarNode &node) { _struct.gen_var(node); }

// structural nodes with no codegen of their own
void LLVMCompiler::visitType(AST::TypeNode &node) {}
void LLVMCompiler::visitNamespaceDecl(AST::NamespaceDeclNode &node) {}
void LLVMCompiler::visitNamespace(AST::NamespaceNode &node) {}
void LLVMCompiler::visitAttribute(AST::AttributeNode &node) {}

// -- backend forwarders -------------------------------------------------------

void LLVMCompiler::optimize() { _backend.optimize(); }
void LLVMCompiler::printIR(bool toFile) { _backend.print_ir(toFile); }
void LLVMCompiler::run_code() { _backend.run_code(); }
bool LLVMCompiler::make_exec(std::string executable_name) { return _backend.make_exec(executable_name); }
