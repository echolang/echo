#include "Compiler/LLVM/LLVMCompiler.h"

#include "AST/FunctionDeclNode.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/raw_ostream.h>

#include <fmt/core.h>

LLVMCompiler::LLVMCompiler()
    : _types(_ctx), _lvalues(_ctx), _expr(_ctx), _stmt(_ctx), _struct(_ctx), _backend(_ctx)
{
    // wire the shared context back to the single visitor (for accept-recursion) and to the
    // type-lowering subsystem (reachable from every other subsystem).
    _ctx.visitor = this;
    _ctx.types = &_types;
    _ctx.lvalues = &_lvalues;
}

LLVMCompiler::~LLVMCompiler()
{
}

void LLVMCompiler::compile_bundle(const AST::Bundle &bundle)
{
    _ctx.llvm_context = std::make_unique<llvm::LLVMContext>();
    _ctx.builder = std::make_unique<llvm::IRBuilder<>>(*_ctx.llvm_context);

    // initialize the compilation units
    _types.create_cmp_units(bundle);

    // build the struct maps
    _types.build_struct_maps(bundle);

    // build the function maps
    _types.build_function_maps(bundle);

    // always declare printf @TODO make this a bit more dynamic..
    for (auto &cmp_unit : _ctx.cmp_units) {
        cmp_unit->llvm_module->getOrInsertFunction("printf",
            llvm::FunctionType::get(llvm::IntegerType::getInt32Ty(*_ctx.llvm_context), llvm::PointerType::get(llvm::Type::getInt8Ty(*_ctx.llvm_context), 0), true) 
        );
    }

    // fetch and build all structs in the module
    // for (auto &cmpu : _ctx.cmp_units) {
    //     _ctx.current_cmp_unit = cmpu.get();

    //     for (auto &file : _ctx.current_cmp_unit->ast_module->files()) {
    //         _ctx.current_file = &file;

    //         for (auto &node : file.root->children) {
    //             if (node.has_type<AST::StructDeclNode>()) {
    //                 auto struct_decl = node.get<AST::StructDeclNode>();
    //                 struct_decl.accept(*this);
    //             }
    //         }
    //     }
    // }

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
        _ctx.current_file = &file;
        file.root->accept(*this);
    }

    // terminate the function
    _ctx.builder->CreateRet(_ctx.builder->getInt32(0));

    // Verify the main module before linking
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
// nothing on purpose.

void LLVMCompiler::visitScope(AST::ScopeNode &node) { _stmt.gen_scope(node); }
void LLVMCompiler::visitVarDecl(AST::VarDeclNode &node) { _stmt.gen_var_decl(node); }
void LLVMCompiler::visitFunctionDecl(AST::FunctionDeclNode &node) { _stmt.gen_function_decl(node); }
void LLVMCompiler::visitReturn(AST::ReturnNode &node) { _stmt.gen_return(node); }
void LLVMCompiler::visitIfStatement(AST::IfStatementNode &node) { _stmt.gen_if_statement(node); }
void LLVMCompiler::visitWhileStatement(AST::WhileStatementNode &node) { _stmt.gen_while_statement(node); }
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

// a peel marker is erased by the pointer adjustment pass; one surviving to codegen means the
// pass missed a position, which would otherwise silently emit the wrong number of loads
void LLVMCompiler::visit_pointer_value(AST::PointerValueNode &node) {
    throw _ctx.error("':$' survived the pointer adjustment pass");
}
void LLVMCompiler::visitBinaryExpr(AST::BinaryExprNode &node) { _expr.gen_binary_expr(node); }
void LLVMCompiler::visitUnaryExpr(AST::UnaryExprNode &node) { _expr.gen_unary_expr(node); }
void LLVMCompiler::visitNull(AST::NullNode &node) { _expr.gen_null(node); }
void LLVMCompiler::visitOperator(AST::OperatorNode &node) { _expr.gen_operator(node); }

void LLVMCompiler::visitStructDecl(AST::StructDeclNode &node) { _struct.gen_struct_decl(node); }
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
void LLVMCompiler::make_exec(std::string executable_name) { _backend.make_exec(executable_name); }
