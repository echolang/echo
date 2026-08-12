#include "Compiler/LLVM/LLVMCompiler.h"

#include "Compiler/PhaseTimings.h"

#include "AST/ASTFunctionEmission.h"
#include "AST/FunctionDeclNode.h"
#include "AST/LoopControlNode.h"
#include "AST/ForeachNode.h"
#include "AST/ConstIfNode.h"
#include "AST/ConstExprNode.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
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
      _abort(_ctx), _memory(_ctx), _process(_ctx), _debug_print(_ctx), _debug_info(_ctx),
      _backend(_ctx)
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
    _ctx.memory = &_memory;
    _ctx.process = &_process;
    _ctx.debug_print = &_debug_print;
    _ctx.debug_info = &_debug_info;
}

LLVMCompiler::~LLVMCompiler()
{
}

void LLVMCompiler::set_entry(const std::string &module_name, const std::filesystem::path &entry_file)
{
    _ctx.entry_module_name = module_name;
    _ctx.entry_file = entry_file;
}

void LLVMCompiler::compile_bundle(const AST::Bundle &bundle, const std::set<std::string> &cached_modules)
{
    _ctx.llvm_context = std::make_unique<llvm::LLVMContext>();

    // beside the context and not per unit: the leaves have to be the *same* MDNode across units, or
    // LLVM's pointer comparison of two equal-looking trees answers "unrelated"
    _ctx.tbaa = std::make_unique<Compiler::LLVM::TbaaTree>(*_ctx.llvm_context);
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

    // `int main(int argc, char **argv, char **envp)` - the three-argument form, always, whether or not
    // this program reads any of them. It is POSIX on both platforms we target and a documented CRT
    // extension on Windows, and it is the *only* way the arguments and the environment reach Echo:
    // `environ` is a data symbol an extern block cannot bind, `argv` is not a symbol at all, and the
    // language has no globals to cache either in
    //
    // unconditional rather than widened only for programs that ask, which was considered and is worse:
    // it would trade three stores of registers already in hand for a whole-program AST query, an entry
    // point whose signature varies per program, and a silent failure - a call the query missed leaves
    // ProcessCodegen's globals reading the zero they were initialized with
    llvm::Type *opaque_ptr = _ctx.opaque_ptr_type();
    llvm::FunctionType *funcType = llvm::FunctionType::get(
        _ctx.builder->getInt32Ty(), { _ctx.builder->getInt32Ty(), opaque_ptr, opaque_ptr }, false);
    llvm::Function *function = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, ECO_ENTRY_SYMBOL_NAME, main_cmp_unit->llvm_module.get());
    function->getArg(0)->setName("argc");
    function->getArg(1)->setName("argv");
    function->getArg(2)->setName("envp");
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", function);
    _ctx.set_insert_point(entry);

    _ctx.current_cmp_unit = main_cmp_unit;

    // the entry point is built here with a bare Function::Create rather than through gen_function_decl,
    // so it needs its own subprogram - and its own prologue location, because gen_capture below emits
    // stores and gen_report emits calls, and a call inside a subprogram-carrying function with no !dbg
    // is a verifier error rather than a missing line
    _debug_info.begin_entry_point(function);
    _debug_info.set_function_scope_location();

    // before the file-root walk below, because a module-scope `env::arg(1)` is one of the statements it
    // emits and would otherwise read a global nothing had filled in yet
    _process.gen_capture(function);

    {
    Compiler::ScopedPhase entry_phase("entry point");

    // **the narrowing below needs a failure mode**, and this is it: an `entry_file` that matches no file
    // of the entry module would otherwise leave `main` holding the prologue and a `ret 0`, and the build
    // would succeed. Counted rather than asked ahead of the walk, so the one answer to "is this file the
    // program" stays CodegenContext::file_is_entry's
    size_t entry_files_emitted = 0;

    // visit all nodes in the main module
    for (auto &file : main_cmp_unit->ast_module->files()) {
        // a file whose top level stopped the program - a `die` at module scope - leaves the block
        // terminated, and everything after it, including the next file, is unreachable. the same
        // question StmtCodegen::gen_scope asks after every statement, asked across files because
        // this is the one function body not emitted through gen_function_decl
        if (_ctx.block_is_terminated()) {
            break;
        }

        // **a target names the one file that is the program.** Every other file of the entry module is
        // shared with the module's other targets, so it contributes what the declaration walk above
        // already emitted from it and nothing more - which is exactly what a *non-entry module's* files
        // get, rather than a rule of its own. With no target the answer is yes for all of them
        if (!_ctx.file_is_entry(file)) {
            continue;
        }

        _ctx.current_file = &file;
        entry_files_emitted += 1;

        // **`main`'s body is the concatenation of every file root of the entry module**, so a location
        // from the second file would otherwise sit inside a subprogram whose file is the first - which
        // is not describable with a plain scope and fails the verifier. This is the shape that does
        // describe it, and the same one a C compiler emits for an #included body
        _debug_info.push_file_scope(&file);

        file.root->accept(*this);

        _debug_info.pop_file_scope();
    }

    if (entry_files_emitted == 0 && !_ctx.entry_file.empty()) {
        throw Compiler::InternalCompilerException(fmt::format(
            "the entry file '{}' is not one of module '{}'s files, so the program has no body",
            _ctx.entry_file.string(), _ctx.entry_module_name), nullptr);
    }

    // terminate the function, unless the program already stopped itself
    //
    // the `[memory]` section goes inside this guard rather than before it, and the terminated case
    // printing nothing is the point: a `die` at module scope already ran the abort runtime's exit(1),
    // so there is no post-teardown moment left to report on. this *is* that moment on the other path -
    // the file-root walk above emitted every module-scope release, which `-ar` shows as the last
    // statements inside the root scope
    if (!_ctx.block_is_terminated()) {
        // the epilogue is the function's own, not the last file-root statement's - and gen_report emits
        // `printf` calls, which must carry a location like every other call here
        _debug_info.set_function_scope_location();

        _memory.gen_report();
        _ctx.builder->CreateRet(_ctx.builder->getInt32(0));
    }

    _debug_info.end_function();
    }

    {
        // the bodies no module owns, into each unit that named one. After `main`'s epilogue, because the
        // file roots above are what discover most of them, and before the verifier, because until this
        // runs those units hold `declare`s nothing defines
        Compiler::ScopedPhase phase("drain");
        drain_pending_definitions();
    }

    // **after the drain and before the ODR check.** The drain is the last thing that can add a body to
    // any unit, and verify_odr_consistency compares final metadata - everything downstream of here (the
    // merge, both pipelines, the object writer) is then strictly later, which is why there is one moment
    // rather than an ordering rule per consumer. A builder left unfinalized leaves temporary MDNodes
    // behind and the verifier rejects the module with a message that does not name the cause
    _debug_info.finalize_all();

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

            // `#0` an attribute group reference, `@4` an unnamed global's slot, `!42` a metadata node.
            // All three are positions in a per-module numbering, so the digits are dropped and the sigil
            // kept - which leaves a body that says "some private constant here" and "some debug location
            // here", and is why both are compared separately below. A *named* global is untouched:
            // `@__eco_abort` starts with a letter, and so does every metadata *kind*, `!dbg` and `!tbaa`
            if ((text[i] == '#' || text[i] == '@' || text[i] == '!') && digits_follow) {
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

    // **the compile unit is a fact about the module, not about the body.** Every scope chain ends at one -
    // a DILocation names a subprogram, which names its unit, which names the module's first file - so a
    // printed tree drags in something that is *supposed* to differ between two units and would make every
    // ODR-shared body compare unequal. The types beneath it are deduplicated by the linker on their
    // `identifier:`, exactly as C++ does across translation units.
    //
    // printTree indents by depth, so dropping the line and everything below it is the subtree
    std::string strip_compile_unit(const std::string &text)
    {
        std::string out;
        out.reserve(text.size());

        size_t cut_at_indent = std::string::npos;

        for (size_t i = 0; i < text.size();) {
            const size_t end = text.find('\n', i);
            const size_t stop = end == std::string::npos ? text.size() : end;
            const std::string_view line(text.data() + i, stop - i);

            const size_t indent = line.find_first_not_of(' ');
            const size_t depth = indent == std::string_view::npos ? 0 : indent;

            if (cut_at_indent != std::string::npos) {
                if (depth > cut_at_indent) {
                    i = stop + 1;
                    continue;
                }

                cut_at_indent = std::string::npos;
            }

            if (line.find("!DICompileUnit(") != std::string_view::npos) {
                cut_at_indent = depth;
                i = stop + 1;
                continue;
            }

            out += line;
            out += '\n';

            if (end == std::string::npos) {
                break;
            }

            i = end + 1;
        }

        return out;
    }

    // **the other half of what the body text cannot see**, and the exact mirror of the function above.
    //
    // a metadata reference renders as `!42`, a per-module slot, so `strip_module_local_numbering` drops
    // the digits for `@0`'s reason - and once they are gone two bodies carrying *different* metadata
    // render identically. Which is precisely the divergence this check exists for: a t_odr_shared body
    // is emitted into every unit that references it, and a `!dbg` derived from the ambient walk rather
    // than from the declaration is two descriptions of one symbol, of which the linker keeps an
    // arbitrary one.
    //
    // **every kind, not just `!dbg`.** `!tbaa` is the other one a body carries today, and singling out
    // debug info would leave exactly the hole this function was written to close - one that only opens
    // the day some access family stops being a pure function of the declaration. getAllMetadata is
    // ordered by kind id, so the walk is deterministic without sorting.
    //
    // **in instruction order, not a set.** A private constant is a thing a body *reaches*, and the order
    // says nothing; metadata describes each instruction, so the sequence is the content.
    std::string referenced_metadata(llvm::Function *fn)
    {
        std::string out;
        llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> attached;

        // **one render per distinct node, not per attachment.** MDNodes are uniqued and heavily shared -
        // one `!tbaa` leaf hangs off nearly every load and store in a body, and a `!dbg` drags its whole
        // reachable type graph along - so rendering at the attachment made this quadratic in the size of
        // the metadata graph for a check whose distinct content is linear
        std::unordered_map<const llvm::MDNode *, std::string> rendered_nodes;

        for (llvm::BasicBlock &block : *fn) {
            for (llvm::Instruction &inst : block) {
                attached.clear();
                inst.getAllMetadata(attached);

                // an instruction carrying none is a fact about this body as much as one carrying some -
                // the prologue and the emitted runtime deliberately have no position, and a copy that
                // grew one is what this is watching for
                if (attached.empty()) {
                    out += "-\n";
                    continue;
                }

                for (const auto &[kind, node] : attached) {
                    auto known = rendered_nodes.find(node);

                    if (known == rendered_nodes.end()) {
                        std::string rendered;
                        llvm::raw_string_ostream stream(rendered);

                        // the whole tree, so a DILocation brings its scope, its subprogram and its file
                        // with it rather than one more slot number. run through the same normalizer as
                        // the body above, because a tree prints its own shared subnodes by slot too -
                        // what survives is the content: the file names, the lines, the type names.
                        //
                        // **with the module, which is not optional.** Handed no module, printTree
                        // numbers the nodes it reaches by *address* - `<0x13bf09128>` - and an address
                        // differs between two units by construction, so every ODR-shared body compared
                        // unequal the moment it carried any metadata at all
                        node->printTree(stream, fn->getParent());
                        stream.flush();

                        std::string normalized =
                            strip_module_local_numbering(strip_compile_unit(rendered));

                        known = rendered_nodes.emplace(node, std::move(normalized)).first;
                    }

                    out += fmt::format("{}: {}\n", kind, known->second);
                }
            }
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
        std::string first_metadata;
        llvm::AttributeList first_attributes;

        for (Compiler::LLVM::CmpUnit *unit : units) {
            llvm::Function *fn = unit->llvm_module->getFunction(name);

            std::string rendered;
            llvm::raw_string_ostream stream(rendered);
            fn->print(stream);
            stream.flush();

            const std::string body = strip_module_local_numbering(rendered);
            const std::string constants = referenced_private_constants(fn);
            const std::string metadata = referenced_metadata(fn);

            if (!have_first) {
                have_first = true;
                first_body = body;
                first_constants = constants;
                first_metadata = metadata;
                first_attributes = fn->getAttributes();
                continue;
            }

            if (body != first_body || constants != first_constants || metadata != first_metadata
                || fn->getAttributes() != first_attributes) {
                throw Compiler::InternalCompilerException(fmt::format(
                    "'{}' is defined with linkonce_odr linkage in more than one module and the "
                    "definitions differ, so the linker would keep an arbitrary one. A generated "
                    "definition must be a pure function of the declaration and its substitution - "
                    "something in this body read ambient compiler state instead.\n\n{}{}{}\n--- vs ---\n\n{}{}{}",
                    name, first_body, first_constants, first_metadata, body, constants, metadata
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
void LLVMCompiler::visit_for_statement(AST::ForStatementNode &node) { _stmt.gen_for_statement(node); }
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

// the same bargain for the other lowered-away literal shape: AST::InterpolationLowering replaces every
// interpolation with the `str::from` calls and the concatenation it stands for, so there is no such
// thing to emit here and answering with the chunks alone would silently print the holes away
void LLVMCompiler::visit_string_interpolation(AST::StringInterpolationExprNode &node)
{
    throw _ctx.error("a string interpolation survived the monomorphizer's fixpoint - it should have "
        "been lowered into a concatenation " + _ctx.function_context());
}

// **both unreachable in practice, and written for that reason.** AST::PointerAdjuster throws for either
// survivor first, and run_semantic_passes gates on has_critical_issues() only *after* it - so these are
// the second line of a defence whose first line already fired. a plausible answer here would be worse
// than a throw: emitting the `if` as a runtime branch is exactly the silence `const if` exists to end
void LLVMCompiler::visit_const_if(AST::ConstIfNode &node)
{
    throw _ctx.error("a 'const if' survived the monomorphizer's fixpoint - its condition should have "
        "selected an arm, or the statement should have been discarded after a refusal "
        + _ctx.function_context());
}

void LLVMCompiler::visit_const_expr(AST::ConstExprNode &node)
{
    throw _ctx.error("a 'const(...)' survived the monomorphizer's fixpoint - it should have become the "
        "literal it folded to, or been refused " + _ctx.function_context());
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
void LLVMCompiler::visit_const_ref(AST::ConstRefExprNode &node)
{
    throw _ctx.error("a constant reference survived AST::ConstantExpander");
}
void LLVMCompiler::visit_const_decl(AST::ConstDeclNode &node)
{
    // a compile-time constant has no storage and no symbol - it was copied into each of its use sites, and
    // there is nothing left to emit. reaching here means one was added to a scope's children, which is the
    // one thing its declaration is documented never to be
    throw _ctx.error("a constant declaration reached codegen - it belongs to no scope");
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
    std::vector<std::filesystem::path> &out_objects
)
{
    Compiler::ScopedPhase phase("emit objects");

    for (auto &cmp_unit : _ctx.cmp_units) {
        if (!cmp_unit->llvm_module) {
            continue;
        }

        // **the baseline pipeline, per unit, before the object is written.** this is the path an ordinary
        // `echoc build` takes, and until now it ran no IR pass at all - see Backend::optimize_unit. It is
        // deliberately here and not inside emit_object: the whole-program path also emits an object, and
        // that module has already been through Backend::optimize
        if (!_ctx.options.no_optimize) {
            _backend.optimize_unit(*cmp_unit);
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
    const std::string &executable_name,
    const std::vector<std::filesystem::path> &objects,
    const std::vector<Compiler::LinkRequirement> &link
)
{
    Compiler::ScopedPhase phase("link");
    return _backend.link_executable(executable_name, objects, link);
}

void LLVMCompiler::optimize() { _backend.optimize(); }
void LLVMCompiler::printIR(bool toFile) { _backend.print_ir(toFile); }
void LLVMCompiler::print_unit_ir() { _backend.print_unit_ir(); }
int LLVMCompiler::run_code(const std::vector<std::string> &arguments, const char *const *environment)
{
    return _backend.run_code(arguments, environment);
}
const std::string &LLVMCompiler::prune_report() const { return _backend.prune_report(); }
bool LLVMCompiler::make_exec(
    const std::string &executable_name,
    const std::filesystem::path &object_path,
    const std::vector<Compiler::LinkRequirement> &link
)
{
    return _backend.make_exec(executable_name, object_path, link);
}
