#include "Compiler/LLVM/LLVMCompiler.h"

#include "Compiler/PhaseTimings.h"

#include "AST/ASTFunctionEmission.h"
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

#include <cctype>
#include <set>

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

void LLVMCompiler::set_entry_module(const std::string &module_name)
{
    _ctx.entry_module_name = module_name;
}

void LLVMCompiler::compile_bundle(const AST::Bundle &bundle, const std::set<std::string> &cached_modules)
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

    {
        Compiler::ScopedPhase phase("declare");

        // initialize the compilation units
        _types.create_cmp_units(bundle, cached_modules);

        // build the struct maps
        _types.build_struct_maps();

        // build the function maps
        _types.build_function_maps();
    }

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

    {
    Compiler::ScopedPhase bodies_phase("bodies");

    // fetch all function declarations inside of the module
    for (auto &cmpu : _ctx.cmp_units) {
        _ctx.current_cmp_unit = cmpu.get();

        for (auto &file : _ctx.current_cmp_unit->ast_module->files()) {
            _ctx.current_file = &file;

            for (auto &node : file.root->children) {
                if (!node.has_type<AST::FunctionDeclNode>()) {
                    continue;
                }

                auto func_decl = node.get<AST::FunctionDeclNode>();

                // an ODR-shared definition is not emitted from the unit that holds its declaration
                // node - it has no owning module, and build_function_maps deliberately gave it no
                // symbol here. The drain below emits it into each unit that references it instead
                if (AST::function_emission_kind(&func_decl) == AST::FunctionEmission::t_odr_shared) {
                    continue;
                }

                func_decl.accept(*this);
            }
        }
    }
    }

    // search for the main module
    Compiler::LLVM::CmpUnit *main_cmp_unit = _ctx.main_cmp_unit();   
    if (!main_cmp_unit) {
        throw Compiler::InternalCompilerException(fmt::format(
            "no entry module '{}' in the bundle", _ctx.entry_module_name), nullptr);
    }

    llvm::FunctionType *funcType = llvm::FunctionType::get(_ctx.builder->getInt32Ty(), false);
    llvm::Function *function = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, ECO_ENTRY_SYMBOL_NAME, main_cmp_unit->llvm_module.get());
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", function);
    _ctx.builder->SetInsertPoint(entry);

    _ctx.current_cmp_unit = main_cmp_unit;

    {
    Compiler::ScopedPhase entry_phase("entry point");

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
    }

    {
        // the bodies no module owns, into each unit that named one. After `main`'s epilogue, because the
        // file roots above are what discover most of them, and before the verifier, because until this
        // runs those units hold `declare`s nothing defines
        Compiler::ScopedPhase phase("drain");
        drain_pending_definitions();
    }

    {
        // before the merge, because afterwards there is only one copy left to look at
        Compiler::ScopedPhase phase("odr check");
        verify_odr_consistency();
    }

    // verify the main module before linking
    std::string error_str;
    llvm::raw_string_ostream error_stream(error_str);
    if (llvm::verifyModule(*main_cmp_unit->llvm_module, &error_stream)) {
        throw Compiler::InternalCompilerException(fmt::format(
            "LLVM IR verification failed for main module:\n{}", error_str
        ));
    }

}

// folds every unit into the main module, leaving one llvm::Module for the whole program.
//
// **separate from compile_bundle, because not every output wants it.** A merged module is what the paths
// that can only look at one need - the O3 pipeline, the IR dump, the JIT - and it is exactly what a
// per-module object cache cannot have, since after this runs the per-unit modules are gone. So the caller
// decides, and `build` without `-O` does not call it at all
void LLVMCompiler::link_into_main()
{
    Compiler::ScopedPhase merge_phase("merge");

    Compiler::LLVM::CmpUnit *main_cmp_unit = _ctx.main_cmp_unit();
    if (!main_cmp_unit) {
        throw Compiler::InternalCompilerException("No main module found in the bundle", nullptr);
    }

    // idempotent: `run` merges and then hands the module to the JIT, which moves it out. Asking twice is a
    // caller mistake rather than a state to repair, but answering it with a null deref is not useful
    if (!main_cmp_unit->llvm_module) {
        throw Compiler::InternalCompilerException(
            "the main module has already been consumed - link_into_main ran twice", nullptr);
    }

    // link all modules together into the main module
    auto linker = llvm::Linker(*main_cmp_unit->llvm_module);

    for (auto &cmpu : _ctx.cmp_units) {

        // skip the main module
        if (cmpu.get() == main_cmp_unit) {
            continue;
        }

        // a unit whose module is already gone: nothing to fold in
        if (!cmpu->llvm_module) {
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
}

void LLVMCompiler::drain_pending_definitions()
{
    // a backstop, not a budget: every round has to emit at least one body it had not emitted before, and
    // `definition_queued` never lets a declaration be queued twice per unit, so the queues are strictly
    // shrinking. A runaway means a body is somehow queueing work that never gets marked, and spinning
    // forever would be a far worse way to find that out than saying so
    constexpr size_t k_max_rounds = 4096;

    Compiler::LLVM::CmpUnit *entry_cmp_unit = _ctx.current_cmp_unit;
    AST::File *entry_file = _ctx.current_file;

    for (size_t round = 0; round < k_max_rounds; round++) {
        bool emitted_any = false;

        for (auto &cmp_unit : _ctx.cmp_units) {
            // taken by value: emitting these bodies appends to `pending_definitions`, and the appended
            // ones are picked up by the next round rather than by a vector being resized mid-iteration
            std::vector<const AST::FunctionDeclNode *> owed;
            owed.swap(cmp_unit->pending_definitions);

            if (owed.empty()) {
                continue;
            }

            emitted_any = true;
            _ctx.current_cmp_unit = cmp_unit.get();

            for (const AST::FunctionDeclNode *decl : owed) {
                // gen_function_decl sets current_file from the declaration itself, so a body's own source
                // position does not depend on this walk - which it must not, since two units emit the
                // same bytes. See CodegenContext::function_file_map
                _stmt.gen_function_decl(const_cast<AST::FunctionDeclNode &>(*decl));
            }
        }

        if (!emitted_any) {
            _ctx.current_cmp_unit = entry_cmp_unit;
            _ctx.current_file = entry_file;
            return;
        }
    }

    throw Compiler::InternalCompilerException(
        "draining the ODR-shared definitions did not converge - a body being emitted is queueing work "
        "that is never marked as queued", nullptr);
}

#ifndef NDEBUG
namespace
{

    // three things in a rendered body are numbered *per module* rather than being properties of the
    // definition, so two identical bodies in two units can still render differently:
    //
    //  - attribute group slots, `#0` / `#1`, numbered in the order a module first needed each group;
    //  - the disambiguating suffix LLVM appends to a named type or a local, `%N3str3bufE.box.4`. Every unit
    //    builds its own StructType for the same Echo type and they all share one LLVMContext, so the
    //    second one to be created gets renamed. IRMover unifies them structurally at link time, and under
    //    separate object files the name is gone entirely - only the layout survives;
    //  - nothing else, and that is the point of doing this by text at all: an instruction sequence, a
    //    called symbol or an integer constant that differs *is* a real divergence and does show up.
    //
    // both are stripped rather than compared, and what would otherwise be lost with them is compared
    // exactly instead - the AttributeList at the comparison, which is uniqued in the shared context.
    //
    // free functions rather than lambdas inside the check: none of them touches the compiler, and as
    // lambdas the text normalizer - the part most likely to need a fourth rule - could not be reached
    // from anywhere else.
    // `array<int32>.2` -> `array<int32>`: the uniquing suffix, off the end of a name
    std::string strip_uniquing_suffix(const std::string &name)
    {
        size_t end = name.size();
        while (end > 0 && std::isdigit(static_cast<unsigned char>(name[end - 1]))) {
            end--;
        }

        const bool is_suffix = end > 1 && end < name.size() && name[end - 1] == '.';

        return is_suffix ? name.substr(0, end - 1) : name;
    }

    std::string strip_module_local_numbering(const std::string &text)
    {
        std::string out;
        out.reserve(text.size());

        for (size_t i = 0; i < text.size(); i++) {
            const bool digits_follow =
                i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]));

            // `#0` an attribute group reference, `@4` an unnamed global's slot. Both are positions in a
            // per-module numbering, so the digits are dropped and the sigil kept - which leaves a body
            // that says "some private constant here" and is why the constants themselves are compared
            // separately below. A *named* global is untouched: `@__eco_abort` starts with a letter
            if ((text[i] == '#' || text[i] == '@') && digits_follow) {
                while (i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
                    i++;
                }
                continue;
            }

            // `%"array<int32>.2"` - a quoted name, which is how LLVM renders one holding characters an
            // identifier cannot. Taken whole rather than by the per-character rule below, because the
            // character before the suffix is then whatever the Echo type's name ended with - `>` for an
            // instantiation - and no plausible character rule covers that without covering too much
            if ((text[i] == '%' || text[i] == '@') && i + 1 < text.size() && text[i + 1] == '"') {
                const size_t close = text.find('"', i + 2);

                if (close != std::string::npos) {
                    out.push_back(text[i]);
                    out.push_back('"');
                    out += strip_uniquing_suffix(text.substr(i + 2, close - (i + 2)));
                    out.push_back('"');
                    i = close;
                    continue;
                }
            }

            // `.4` at the end of a `%`-identifier - a uniquing suffix, dropped while the name it belongs
            // to is kept. Only inside an identifier, so a struct field index or a plain number is safe
            if (text[i] == '.' && digits_follow && !out.empty()
                && (std::isalnum(static_cast<unsigned char>(out.back())) || out.back() == '_'
                    || out.back() == '.')) {
                size_t lookahead = i + 1;
                while (lookahead < text.size() && std::isdigit(static_cast<unsigned char>(text[lookahead]))) {
                    lookahead++;
                }

                // a suffix ends the identifier; digits followed by more name are part of the name
                const bool ends_identifier = lookahead >= text.size()
                    || (!std::isalnum(static_cast<unsigned char>(text[lookahead])) && text[lookahead] != '_'
                        && text[lookahead] != '.');

                if (ends_identifier) {
                    i = lookahead - 1;
                    continue;
                }
            }

            out.push_back(text[i]);
        }

        return out;
    }

    // **what the body text cannot see.** A private global renders as its slot number, `@0`, so two bodies
    // referencing two *different* private constants render identically - and that is exactly the shape of
    // the divergence this check exists for, since an abort message is a private string built from the file
    // name. So the initializer of every private constant a body reaches is folded into the comparison.
    std::string referenced_private_constants(llvm::Function *fn)
    {
        std::string out;
        std::set<std::string> seen;

        for (llvm::BasicBlock &block : *fn) {
            for (llvm::Instruction &inst : block) {
                for (llvm::Value *operand : inst.operands()) {
                    // through the constant expression a GEP'd string literal arrives as
                    llvm::SmallVector<llvm::Value *, 4> roots{ operand };
                    if (auto *expr = llvm::dyn_cast<llvm::ConstantExpr>(operand)) {
                        roots.assign(expr->op_begin(), expr->op_end());
                    }

                    for (llvm::Value *root : roots) {
                        auto *global = llvm::dyn_cast<llvm::GlobalVariable>(root);
                        if (global == nullptr || !global->hasLocalLinkage() || !global->hasInitializer()) {
                            continue;
                        }

                        std::string rendered;
                        llvm::raw_string_ostream stream(rendered);
                        global->getInitializer()->print(stream);
                        stream.flush();

                        seen.insert(rendered);
                    }
                }
            }
        }

        for (const std::string &entry : seen) {
            out += entry;
            out += "\n";
        }

        return out;
    }

};
#endif

void LLVMCompiler::verify_odr_consistency()
{
#ifndef NDEBUG
    // a single unit cannot define anything twice, and the merge leaves exactly one - so the whole-program
    // path pays nothing for this
    if (_ctx.cmp_units.size() < 2) {
        return;
    }

    // names first, bodies only where there is something to compare. Rendering every definition in the
    // bundle to a string would cost more than the rest of codegen, and the overwhelmingly common answer
    // is that no symbol is defined twice at all
    std::unordered_map<std::string, std::vector<Compiler::LLVM::CmpUnit *>> definers;

    for (auto &cmp_unit : _ctx.cmp_units) {
        if (cmp_unit->llvm_module == nullptr) {
            continue;
        }

        for (llvm::Function &fn : *cmp_unit->llvm_module) {
            if (fn.isDeclaration() || !fn.hasLinkOnceODRLinkage()) {
                continue;
            }

            definers[fn.getName().str()].push_back(cmp_unit.get());
        }
    }

    for (const auto &[name, units] : definers) {
        if (units.size() < 2) {
            continue;
        }

        bool have_first = false;
        std::string first_body;
        std::string first_constants;
        llvm::AttributeList first_attributes;

        for (Compiler::LLVM::CmpUnit *unit : units) {
            llvm::Function *fn = unit->llvm_module->getFunction(name);

            std::string rendered;
            llvm::raw_string_ostream stream(rendered);
            fn->print(stream);
            stream.flush();

            const std::string body = strip_module_local_numbering(rendered);
            const std::string constants = referenced_private_constants(fn);

            if (!have_first) {
                have_first = true;
                first_body = body;
                first_constants = constants;
                first_attributes = fn->getAttributes();
                continue;
            }

            if (body != first_body || constants != first_constants
                || fn->getAttributes() != first_attributes) {
                throw Compiler::InternalCompilerException(fmt::format(
                    "'{}' is defined with linkonce_odr linkage in more than one module and the "
                    "definitions differ, so the linker would keep an arbitrary one. A generated "
                    "definition must be a pure function of the declaration and its substitution - "
                    "something in this body read ambient compiler state instead.\n\n{}{}\n--- vs ---\n\n{}{}",
                    name, first_body, first_constants, body, constants
                ), nullptr);
            }
        }
    }
#endif
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

// deliberately not "for each unit": a unit whose module was already consumed - by a merge, or because a
// cache supplied its object - has nothing to emit, and asking is how you find out
bool LLVMCompiler::emit_objects(
    const std::function<std::filesystem::path(const std::string &)> &object_for,
    std::vector<std::filesystem::path> &out_objects)
{
    Compiler::ScopedPhase phase("emit objects");

    for (auto &cmp_unit : _ctx.cmp_units) {
        if (!cmp_unit->llvm_module) {
            continue;
        }

        const std::filesystem::path object_path = object_for(cmp_unit->ast_module->name);

        if (!_backend.emit_object(*cmp_unit, object_path)) {
            return false;
        }

        out_objects.push_back(object_path);
    }

    return true;
}

bool LLVMCompiler::link_executable(
    const std::string &executable_name, const std::vector<std::filesystem::path> &objects)
{
    Compiler::ScopedPhase phase("link");
    return _backend.link_executable(executable_name, objects);
}

void LLVMCompiler::optimize() { _backend.optimize(); }
void LLVMCompiler::prune_to_entry() { _backend.prune_to_entry(); }
void LLVMCompiler::printIR(bool toFile) { _backend.print_ir(toFile); }
void LLVMCompiler::run_code() { _backend.run_code(); }
bool LLVMCompiler::make_exec(std::string executable_name) { return _backend.make_exec(executable_name); }
