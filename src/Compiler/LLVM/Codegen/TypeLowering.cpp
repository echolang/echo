#include "Compiler/LLVM/Codegen/TypeLowering.h"

#include "AST/ASTVariadic.h"
#include "Compiler/LLVM/Codegen/IfaceValue.h"
#include "Compiler/LLVM/Codegen/ClassCodegen.h"
#include "Compiler/LLVM/Codegen/IntrinsicResolution.h"
#include "Compiler/LLVM/Codegen/ReturnAbi.h"
#include "Compiler/LLVM/Codegen/DebugInfoCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "eco.h"

#include "AST/ASTAccess.h"
#include "AST/ASTBundle.h"
#include "AST/ASTConformance.h"
#include "AST/ASTFunctionEmission.h"
#include "AST/ASTMangler.h"
#include "AST/ASTNullability.h"
#include "AST/ASTSourceToken.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>

#include <fmt/core.h>

#include <cassert>
#include <string>
#include <vector>

namespace Compiler::LLVM
{
void TypeLowering::create_cmp_units(
    const AST::Bundle &bundle,
    const std::set<std::string> &cached_modules
)
{
    for (auto &module : bundle.modules) {
        // served from the cache: no unit, so nothing below ever declares or defines anything for it
        if (cached_modules.count(module->name) > 0) {
            continue;
        }

        // check if the module is already in the map which is not allowed
        if (_ctx.cmp_unit_map.find(module->name) != _ctx.cmp_unit_map.end()) {
            throw Compiler::InternalCompilerException(fmt::format(
                "A module named '{}' already exists, all module names of a bundle must be unique.",
                module->name
            ));
        }

        // create a new cmp unit for the module
        _ctx.cmp_units.emplace_back(std::make_unique<Compiler::LLVM::CmpUnit>());
        auto &cmp_unit = _ctx.cmp_units.back();
        cmp_unit->ast_module = module.get();
        cmp_unit->llvm_module = std::make_unique<llvm::Module>(module->name, *_ctx.llvm_context);

        // every module carries the host layout and triple from the moment it exists, so any
        // question about type sizes during codegen gets the real target's answer, the optimizer
        // has a layout to reason with, and the JIT and object paths cannot disagree
        cmp_unit->llvm_module->setDataLayout(_ctx.layout());
        cmp_unit->llvm_module->setTargetTriple(_ctx.target_triple);

        // and its compile unit and debug module flags, for the same reason and at the same moment: all
        // three are things a module carries from the instant it exists rather than acquires later. a
        // no-op with `-g` off
        _ctx.debug_info->create_unit(*cmp_unit);

        _ctx.cmp_unit_map[module->name] = cmp_unit.get();
    }

    // where every declaration was written, recorded once now that the file roots are final - every pass
    // that appends one has run. See CodegenContext::function_file_map for why a body needs this rather
    // than the ambient current_file
    _ctx.function_file_map.clear();

    // **all three ways a function is declared, or the map answers for none of them.** a free function is a
    // file-root child; a method is a child of a type declaration that is; and an instantiation is neither -
    // the monomorphizer appends it to the module, so it has no file of its own and takes its template's.
    //
    // recording only the first left the other two on the ambient fallback, and an instantiation is exactly
    // the case the map exists for: it is `t_odr_shared`, so its `assert` message is baked into a definition
    // two units may both emit. It read right for as long as the generic type happened to be declared in the
    // stdlib file the walk reached first, which is luck rather than a rule, and adding one file ahead of it
    // alphabetically was enough to make every `array<T>` bounds message name the wrong source file

    // where each declared type was written. Two readers, and neither could derive it: the third sweep
    // below reaches declarations that have no file root above them and no template to read through and
    // carry only an owner, and DebugInfoCodegen needs a type's description to be the same in every unit
    // that mentions it. Built here because a ComplexType has no back-pointer to its TypeDeclNode
    _ctx.type_site_map.clear();

    // every module, so a token can be resolved to its file regardless of which unit is being lowered -
    // see CodegenContext::token_modules. Recorded here beside the maps below, and over *every* module
    // including the cached ones, because a cached module still owns the tokens a live one's
    // instantiations point at
    _ctx.token_modules.clear();

    for (auto &module : bundle.modules) {
        _ctx.token_modules.push_back(module.get());
    }

    for (auto &module : bundle.modules) {
        for (auto &file : module->files()) {
            if (file.root == nullptr) {
                continue;
            }

            for (auto &child : file.root->children) {
                if (child.has_type<AST::FunctionDeclNode>()) {
                    _ctx.function_file_map[child.get_ptr<AST::FunctionDeclNode>()] = &file;
                }
                else if (child.has_type<AST::TypeDeclNode>()) {
                    AST::TypeDeclNode *type_decl = child.get_ptr<AST::TypeDeclNode>();
                    const TokenReference *at = AST::source_token_of(*type_decl);

                    _ctx.type_site_map[&type_decl->complex_type()] = CodegenContext::TypeSite{
                        &file, at != nullptr ? at->line() : 0 };

                    for (AST::FunctionDeclNode *method : type_decl->methods()) {
                        _ctx.function_file_map[method] = &file;
                    }
                }
            }
        }
    }

    // the instantiations, in a second sweep: a template must already be in the map before an instance can
    // read through it, and a template is not guaranteed to be walked before the module holding its instances
    for (auto &module : bundle.modules) {
        for (AST::FunctionDeclNode *decl : module->nodes.of_type<AST::FunctionDeclNode>()) {
            if (decl->template_ref == nullptr) {
                continue;
            }

            auto found = _ctx.function_file_map.find(decl->template_ref);

            if (found != _ctx.function_file_map.end()) {
                _ctx.function_file_map[decl] = found->second;
            }
        }
    }

    // **and the declarations no walk above reaches at all.** Two shapes, both t_odr_shared, so a miss
    // here is not a cosmetically wrong file name in an abort message - it is two units emitting
    // different bytes for one symbol, which verify_odr_consistency now throws on:
    //
    //   - a **closure**, which is nested inside an expression rather than being a file-root child. It
    //     was written in a file though, and its `declaration_token` is the real `function` keyword, so
    //     the token answers directly.
    //   - a **synthesized deinit, copy constructor or field-wise constructor**, appended to the module
    //     by AST::OwnershipPass or the struct parser with no template_ref and only a virtual name
    //     token. Nothing about it was written anywhere, so the honest answer is the file its *owner*
    //     was declared in - which is also where a person reading a backtrace expects to land.
    //
    // asking the token first and the owner second is the order that matters: a hand-written method has
    // both and the token is the more specific of the two
    for (auto &module : bundle.modules) {
        for (AST::FunctionDeclNode *decl : module->nodes.of_type<AST::FunctionDeclNode>()) {
            if (_ctx.function_file_map.find(decl) != _ctx.function_file_map.end()) {
                continue;
            }

            // a token belongs to exactly one module's collection, and a clone appended to another
            // module still carries its template's - so this asks the bundle rather than this module
            if (const TokenReference *token = AST::source_token_of(*decl)) {
                if (AST::File *written_in = _ctx.file_of_token(*token)) {
                    _ctx.function_file_map[decl] = written_in;
                    continue;
                }
            }

            if (decl->owner_type == nullptr) {
                continue;
            }

            // through template_or_self, or an instantiation's synthesized deinit finds no declaration
            // node at all - only the template it was derived from ever had one
            if (auto owner_site = _ctx.site_of(decl->owner_type)) {
                _ctx.function_file_map[decl] = owner_site->file;
            }
        }
    }

    // ensure none of the modules are null
    for (auto &cmp_unit : _ctx.cmp_units) {
        if (cmp_unit->llvm_module == nullptr) {
            throw Compiler::InternalCompilerException(fmt::format(
                "Compiler failed to create a module for '{}', error: {}",
                cmp_unit->ast_module->name,
                _ctx.llvm_err_str()
            ));
        }
    }
}

ReturnAbi TypeLowering::return_abi_of(
    const AST::FunctionDeclNode *node,
    Compiler::LLVM::CmpUnit &cmp_unit
)
{
    if (node->extern_symbol.has_value()) {
        return ReturnAbi{};
    }

    return return_abi_for(get_llvm_type(node->get_return_type(), cmp_unit), _ctx.layout());
}

void TypeLowering::apply_function_attributes(
    const AST::FunctionDeclNode *node,
    llvm::Function *func,
    Compiler::LLVM::CmpUnit &cmp_unit,
    const ReturnAbi &abi
)
{
    // a *hint*, which is exactly what `#[inline]` is: FunctionDeclNode::is_inline is documented as "not a
    // promise the optimizer has to keep", so `inlinehint` fits that wording where `alwaysinline` would
    // contradict it. the attribute is what makes the placement `#[inline]` already buys worth something -
    // the body was being copied into every referencing unit and then judged on cost alone
    if (node->is_inline) {
        func->addFnAttr(llvm::Attribute::InlineHint);
    }

    // **the `sret` attribute is what makes the hidden argument mean something to the optimizer**, and
    // Compiler::LLVM::indirect_return_attributes is the one place that spells it - every call site applies
    // the same builder, because a function and a call to it disagreeing about this is a miscompile rather
    // than a missed optimization
    //
    // written here rather than in get_function_type so the whole per-argument attribute story stays in one
    // function, and taken as a parameter rather than re-derived: the caller minting this Function asked
    // TypeLowering::return_abi_of for the signature it built, and one answer is what keeps the two from
    // being able to differ at all
    const size_t abi_offset = abi.is_indirect() ? 1 : 0;

    if (abi.is_indirect() && func->arg_size() > 0) {
        func->addParamAttrs(0, indirect_return_attributes(*_ctx.llvm_context, abi, _ctx.layout()));
    }

    for (size_t i = 0; i < node->args.size(); i++) {
        const AST::VarDeclNode *arg = node->args[i];

        // **every index below is shifted past the hidden `sret` argument**, which is why abi_offset is
        // computed once above rather than at each of the three uses: the declaration's parameter i is the
        // function's argument i + 1 whenever the answer comes back through storage
        const size_t at = i + abi_offset;

        if (arg == nullptr || !arg->has_type() || at >= func->arg_size()) {
            continue;
        }

        const AST::ValueType type = arg->type();

        // **`readonly` is the one aliasing-adjacent attribute this function may write today, and it is
        // written only for a parameter whose author *declared* `read`.**
        //
        // it says the function writes nothing through this argument or anything based on it, and that
        // is a claim AST::AccessPass now checks in both directions it can be broken: a write through
        // the parameter's own region, and a call handing that region somewhere that does not promise
        // the same. before those two checks existed the claim was simply false - `const` is a
        // per-level flag, so `$src->storage->data:$[0] = 999` through a `const array<T>&` writes the
        // caller's elements.
        //
        // an *inferred* read - a bare `const T&` - deliberately gets nothing. the checks are scoped to
        // the declared form precisely so that the promise is opt-in, so inferring the attribute here
        // would be asserting the half of the rule nobody opted into.
        //
        // **and no `noalias`, from here or from anywhere.** a call-site conflict check does not
        // license one: a callee can reach the same storage through a class handle it holds or a
        // pointer it stored, neither of which appears at the call.
        if (AST::declared_access_effect(*arg) == AST::AccessEffect::t_read && type.is_pointer()) {
            func->getArg(static_cast<unsigned>(at))->addAttr(llvm::Attribute::ReadOnly);
        }

        // **`t_pointer` only, and deliberately not a class handle.** a class also lowers to a bare `ptr`,
        // but it points at a payload inside a heap block whose header sits *before* it - so a size taken
        // from the payload type is not the size that is dereferenceable from that address. the borrow is
        // the case where the type system's answer and the address agree
        if (!type.is_pointer()) {
            continue;
        }

        // the whole of what distinguishes a borrow `T&` from a `ptr<T>` is this one bit, and it is the
        // reason for the attributes below. a `ptr<T>` may legitimately be null and gets nothing
        if (type.is_nullable()) {
            continue;
        }

        llvm::Argument *param = func->getArg(static_cast<unsigned>(at));

        // **the bargain, stated because it is a real one.** a borrow is non-nullable by the type system,
        // and the one path that can produce a null one is a `ptr<T>` narrowing whose check a release build
        // drops - so in a program that is already undefined, this lets the optimizer act on it. That is
        // the same trade C++ makes for a reference, and it is the trade this language already took when
        // it made the narrowing check a debug-only one
        param->addAttr(llvm::Attribute::NonNull);

        const AST::ValueType pointee = AST::value_type_of(type);

        // **`dereferenceable` and `align` want a size, and a size wants the pointee lowered - which this
        // must not do.** get_llvm_type answers a bare `ptr` for every pointer level precisely so that a
        // pointer never drags its pointee's layout into a unit that has not declared it, and asking it
        // here for the pointee anyway minted a *second* `%"array<int32>"` in the module. so only a
        // primitive gets the two size-dependent attributes: those lower to LLVM's own interned integer and
        // float types and create nothing at module level, which is what makes them safe to ask about.
        //
        // a struct or class borrow keeps `nonnull` alone. that is the attribute that carries the type
        // system's actual claim; the other two only let a load be speculated
        if (!pointee.is_primitive() || pointee.is_void()) {
            continue;
        }

        llvm::Type *lowered = get_llvm_type(pointee, cmp_unit);

        if (lowered == nullptr || !lowered->isSized()) {
            continue;
        }

        param->addAttrs(llvm::AttrBuilder(*_ctx.llvm_context)
            .addDereferenceableAttr(_ctx.layout().getTypeAllocSize(lowered))
            .addAlignmentAttr(_ctx.layout().getABITypeAlign(lowered)));
    }
}

llvm::Function *TypeLowering::create_llvm_func_decl(const AST::FunctionDeclNode *node, Compiler::LLVM::CmpUnit &cmp_unit)
{
    auto func_name = AST::mangle_function_name(node);
    auto func_type = node->get_return_type();

    // **the C variadic tail is a parameter here and no parameter at all in LLVM.** a declaration
    // ending in `#[core: variadic_args]` lowers to a function type over the parameters *before* it,
    // marked variadic - which is what makes the call ABI-correct: a variadic parameter is passed by a
    // different convention from a named one on several targets, and `isVarArg` is how the backend is
    // told which arguments are which
    const bool is_c_variadic = AST::has_variadic_tail(*node, _ctx.core_types());
    const size_t fixed_count = is_c_variadic ? node->args.size() - 1 : node->args.size();

    // function arguments
    // @TODO support complex types
    std::vector<llvm::Type *> arg_types;
    for (size_t i = 0; i < fixed_count; i++) {
        // one lowering path for every parameter shape: get_llvm_type already handles structs,
        // primitives and pointers. the old split called get_primitive_type() on anything
        // non-struct, which for a pointer would answer t_void and then assert inside LLVM
        arg_types.push_back(get_llvm_type(node->args[i]->type_node()->type, cmp_unit));
    }

    // **an aggregate too big for registers comes back through a hidden first argument.**
    // Compiler::LLVM::return_abi_for is the one owner of that decision and every other site asks it -
    // the prologue that names the arguments, the `return` that fills the slot, and each call site.
    //
    // the `extern` exemption is TypeLowering::return_abi_of's, which is where every site asks it
    llvm::Type *lowered_return = get_llvm_type(func_type, cmp_unit);

    const ReturnAbi abi = return_abi_of(node, cmp_unit);

    if (abi.is_indirect()) {
        lowered_return = llvm::Type::getVoidTy(*_ctx.llvm_context);
        arg_types.insert(arg_types.begin(), llvm::PointerType::getUnqual(*_ctx.llvm_context));
    }

    llvm::FunctionType *requested_type =
        llvm::FunctionType::get(lowered_return, arg_types, is_c_variadic);

    // handle intrinsic functions
    //
    // the whole signature goes to the resolver, not just the arguments: which positions of an
    // intrinsic are overloaded is the resolver's question to answer, and the return type is one of
    // the positions it reads. AST::resolve_intrinsic is the one owner of both halves
    if (node->intrinsic.has_value()) {
        std::string failure;
        llvm::Function *intrinsic_llvm_func = declare_intrinsic(cmp_unit.llvm_module.get(), node->intrinsic.value(), requested_type, failure);

        if (!intrinsic_llvm_func) {
            // func_name is the mangled symbol, which is not what a user wrote - say the declared
            // name instead
            throw _ctx.error(fmt::format(
                "Function '{}' declares #[intrinsic: \"{}\"], but {}",
                node->func_name(), node->intrinsic.value(), failure
            ));
        }

        cmp_unit.function_table.push_function(func_name, node, intrinsic_llvm_func);
        return intrinsic_llvm_func;
    }

    // an extern declaration binds to a symbol that may already exist in this module - declared by
    // another extern in a different file, or by the compiler itself. getOrInsertFunction is what
    // unifies them instead of colliding
    //
    // it hands back the *existing* global whenever the name is taken, though, and under opaque
    // pointers a signature mismatch is invisible in the IR - so two declarations of `malloc` with
    // different argument types would silently share one call sequence. compare the types and say
    // so instead
    if (node->extern_symbol.has_value()) {
        auto callee = cmp_unit.llvm_module->getOrInsertFunction(func_name, requested_type);
        auto *extern_llvm_func = llvm::dyn_cast<llvm::Function>(callee.getCallee());

        if (!extern_llvm_func) {
            throw _ctx.error(fmt::format(
                "Extern symbol '{}' is already declared in module '{}' as something other than a function.",
                func_name, cmp_unit.ast_module->name
            ));
        }

        if (extern_llvm_func->getFunctionType() != requested_type) {
            throw _ctx.error(fmt::format(
                "Extern symbol '{}' is already declared in module '{}' with a different signature. "
                "Every declaration of a C symbol must agree on its argument and return types.",
                func_name, cmp_unit.ast_module->name
            ));
        }

        cmp_unit.function_table.push_function(func_name, node, extern_llvm_func);
        return extern_llvm_func;
    }

    // two declarations mangling to one name means the mangler lost information, and the failure
    // is otherwise silent: push_function overwrites by name, gen_function_decl looks the function
    // up by name, and Function::Create quietly renames the loser to "<name>.1" - so two bodies
    // end up in one function. fail loudly instead
    if (auto existing_id = cmp_unit.function_table.get_function_id(func_name); existing_id != 0) {
        const auto *existing = cmp_unit.function_table.get_function(existing_id).ast_funcdecl;
        if (existing != node) {
            throw _ctx.error(fmt::format(
                "Symbol '{}' is already declared in module '{}' (by '{}', now again by '{}'). "
                "This is a name mangling defect, not a source error.",
                func_name,
                cmp_unit.ast_module->name,
                existing ? existing->namespaced_func_name() : "<unknown>",
                node->namespaced_func_name()
            ));
        }
    }

    // **external here even for an ODR-shared definition**, and weakened later, in
    // StmtCodegen::gen_function_decl. LLVM's verifier rejects a bodyless linkonce_odr function, and this
    // is also the path a unit takes to *reference* one it does not define - so the linkage a symbol ends
    // up with is decided by whether a body was emitted into this unit, not by what it is
    llvm::Function *llvm_func = llvm::Function::Create(requested_type, llvm::Function::ExternalLinkage, func_name, cmp_unit.llvm_module.get());

    // the declaration is where the attributes go, not the definition: a unit that only *references* this
    // symbol has to make the same promises about it, or the caller side of a call cannot use them
    apply_function_attributes(node, llvm_func, cmp_unit, abi);

    // store in the function map
    cmp_unit.function_table.push_function(func_name, node, llvm_func);

    // and if nobody owns this definition, this unit now owes a body for it - see
    // CmpUnit::pending_definitions. Queued here rather than at the four call sites that reach this
    // function lazily, because this is the one place all of them pass through, so transitivity is free:
    // draining one body runs the same path again for whatever that body names
    if (AST::function_emission_kind(node) == AST::FunctionEmission::t_odr_shared
        && cmp_unit.definition_queued.insert(node).second) {
        cmp_unit.pending_definitions.push_back(node);
    }

    return llvm_func;
}

llvm::StructType *TypeLowering::create_llvm_struct_decl(const AST::TypeDeclNode *node, Compiler::LLVM::CmpUnit &cmp_unit)
{
    if (!node->name_token.has_value()) {
        assert(false);
        throw _ctx.error("Anonymous struct declarations are not yet supported.");
    }

    // idempotent, because more than one thing lowers a declaration: build_struct_maps walks every
    // TypeDeclNode a unit holds up front, and the codegen visitor reaches the same node again when it
    // walks the file root
    if (auto existing = cmp_unit.structure_table->get_structure_id(node); existing != 0) {
        return cmp_unit.structure_table->get_structure(existing).llvm_struct;
    }

    auto type_name = node->type_name();

    // make the prop types
    std::vector<llvm::Type *> member_types;
    for (const auto &prop : node->properties()) {
        llvm::Type *llvm_type = get_llvm_type(prop->type_node()->type, cmp_unit);
        if (!llvm_type) {
            assert(false);
            throw _ctx.error(fmt::format(
                "Unknown type for field '{}' in struct '{}'.",
                prop->name(), type_name
            ));
        }
        member_types.push_back(llvm_type);
    }

    // define the llvm struct type
    llvm::StructType *llvm_struct_type = llvm::StructType::create(*_ctx.llvm_context, member_types, type_name);

    // store the struct in the struct table
    auto struct_id = cmp_unit.structure_table->push_structure(node, llvm_struct_type);

    // a class needs the block around that payload before anything can allocate one. built eagerly
    // here, alongside the payload, so the unit that declares the class always has it
    if (node->is_class()) {
        build_class_box(cmp_unit.structure_table->get_structure(struct_id), node->complex_type(), cmp_unit);
    }

    return llvm_struct_type;
}

llvm::StructType *TypeLowering::create_llvm_struct_for_instance(const AST::ComplexType *type, const Compiler::LLVM::CmpUnit &cmp_unit)
{
    std::string type_name = type->name.value_or("anon");

    // create the struct opaque first and register it, so a self-referential instantiation
    // (a property that mentions the same instantiation) resolves to this in-progress type
    llvm::StructType *llvm_struct_type = llvm::StructType::create(*_ctx.llvm_context, type_name);
    auto struct_id = cmp_unit.structure_table->push_structure(type, llvm_struct_type);

    std::vector<llvm::Type *> member_types;
    for (size_t i = 0; i < type->property_count(); i++) {
        member_types.push_back(get_llvm_type(type->get_property_type(i), cmp_unit));
    }
    llvm_struct_type->setBody(member_types);

    // the block, once the payload is complete - it is a member of the block, so it has to be
    if (type->is_class_kind()) {
        build_class_box(cmp_unit.structure_table->get_structure(struct_id), *type, cmp_unit);
    }

    return llvm_struct_type;
}

void TypeLowering::build_class_box(
    Structure &structure,
    const AST::ComplexType &type,
    const Compiler::LLVM::CmpUnit &cmp_unit
)
{
    assert(structure.llvm_struct != nullptr);

    if (structure.llvm_box != nullptr) {
        return;
    }

    const std::string type_name = type.name.value_or("anon");

    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = llvm::PointerType::get(*_ctx.llvm_context, 0);

    // the field order is the contract Codegen/ClassLayout.h states: the two counts, then the identity
    // pointer, then the payload. written by index rather than pushed, so the contract is the only place
    // the order is decided - and it must stay in step with class_header_llvm_type() below, which is how
    // an environment and an erased operand reach the same words with no layout to GEP through
    std::vector<llvm::Type *> box_members(4);
    box_members[ClassBox::strong_index] = i64;
    box_members[ClassBox::weak_index] = i64;
    box_members[ClassBox::typeinfo_index] = opaque_ptr;
    box_members[ClassBox::payload_index] = structure.llvm_struct;

    structure.llvm_box = llvm::StructType::create(*_ctx.llvm_context, box_members, type_name + ".box");

    // the descriptor whose *address* is the class's identity. linkonce_odr so every unit may define it
    // and the linker keeps one - which is what makes the address comparable across modules without a
    // numbering scheme anybody has to keep stable
    //
    // named from the **mangled token**, never the written or displayed name, for exactly the reason
    // the release thunk beside it is: linkonce_odr folds by symbol name, so `a::Foo` and `b::Foo`
    // sharing one spelling here made them one identity, and instanceof then answered true across two
    // unrelated classes. ComplexType::mangled_token() is the existing answer to "which type is this",
    // including the namespace path, a nested owner and an instantiation's arguments
    structure.typeinfo = get_or_create_odr_constant(
        type.mangled_token() + ".typeinfo",
        [&] {
            // `{ i64 count, ptr conformances }` - see Codegen/ClassLayout.h. it used to be a bare `i8 0`,
            // and the *address* is still all that identity needs; the body is what answers the second
            // question a class can be asked, which interfaces it conforms to
            llvm::Constant *conformances = build_conformance_table(type, cmp_unit);
            auto *info_type = typeinfo_llvm_type();

            std::vector<llvm::Constant *> info_values(2);
            info_values[ClassTypeInfo::conformance_count_index] =
                llvm::ConstantInt::get(i64, type.conformances().size());
            info_values[ClassTypeInfo::conformances_index] = conformances != nullptr
                ? conformances
                : llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(opaque_ptr));

            return llvm::ConstantStruct::get(info_type, info_values);
        },
        cmp_unit);
}

llvm::GlobalVariable *TypeLowering::get_or_create_interface_identity(
    const AST::ComplexType &interface,
    const Compiler::LLVM::CmpUnit &cmp_unit
)
{
    // one byte, and only its address is ever read - the same shape a class's typeinfo had before it
    // grew a body, and for the same reason: an interface has nothing to say about itself at runtime
    // beyond being itself
    return get_or_create_odr_constant(
        interface.mangled_token() + ".itype",
        [&] { return llvm::ConstantInt::get(llvm::Type::getInt8Ty(*_ctx.llvm_context), 0); },
        cmp_unit);
}

llvm::Constant *TypeLowering::build_conformance_table(
    const AST::ComplexType &type,
    const Compiler::LLVM::CmpUnit &cmp_unit
)
{
    const auto &conformances = type.conformances();

    if (conformances.empty()) {
        return nullptr;
    }

    llvm::Type *opaque_ptr = llvm::PointerType::get(*_ctx.llvm_context, 0);

    std::vector<llvm::Constant *> identities;
    identities.reserve(conformances.size());

    for (const AST::ValueType &conformance : conformances) {
        // only valid interface types are ever published on a ComplexType - parse_typedecl refuses
        // anything else at the declaration - so this asserts rather than filters. filtering made the
        // table shorter than the count the descriptor beside it carries, which are two numbers derived
        // from one list in two places: a scan bounded by the longer one reads past the array
        assert(conformance.is_interface() && "a non-interface reached ComplexType::conformances()");

        identities.push_back(
            get_or_create_interface_identity(*conformance.get_complex_type(), cmp_unit));
    }

    auto *table_type = llvm::ArrayType::get(opaque_ptr, identities.size());

    // linkonce_odr like the typeinfo that points at it, and named off the same mangled token: two units
    // that both lower this class emit identical tables and the linker keeps one
    return get_or_create_odr_constant(
        type.mangled_token() + ".conformances",
        [&] { return llvm::ConstantArray::get(table_type, identities); },
        cmp_unit);
}

llvm::GlobalVariable *TypeLowering::get_or_create_odr_constant(
    const std::string &name,
    const std::function<llvm::Constant *()> &build,
    const Compiler::LLVM::CmpUnit &cmp_unit
)
{
    if (auto *existing = cmp_unit.llvm_module->getGlobalVariable(name, true)) {
        return existing;
    }

    llvm::Constant *initializer = build();

    if (initializer == nullptr) {
        return nullptr;
    }

    return new llvm::GlobalVariable(
        *cmp_unit.llvm_module,
        initializer->getType(),
        /*isConstant=*/true,
        llvm::GlobalValue::LinkOnceODRLinkage,
        initializer,
        name);
}

Compiler::LLVM::ClassLayout TypeLowering::get_or_create_class_layout(
    const AST::ComplexType *type,
    const Compiler::LLVM::CmpUnit &cmp_unit
)
{
    if (type == nullptr) {
        throw _ctx.error(fmt::format("Cannot resolve the layout of an anonymous class {}", _ctx.function_context()));
    }

    auto struct_id = cmp_unit.structure_table->get_structure_id(type);

    // the same lazy path get_llvm_type takes for a struct: a generic instantiation has no
    // TypeDeclNode, and a class declared in another module was never registered here
    if (struct_id == 0) {
        create_llvm_struct_for_instance(type, cmp_unit);
        struct_id = cmp_unit.structure_table->get_structure_id(type);
    }

    if (struct_id == 0) {
        throw _ctx.error(fmt::format(
            "Class '{}' is not declared in compilation unit '{}' ({})",
            type->name.value_or("<anonymous>"),
            cmp_unit.ast_module ? cmp_unit.ast_module->name : "<unknown>",
            _ctx.function_context()));
    }

    Structure &structure = cmp_unit.structure_table->get_structure(struct_id);

    // a struct registered before the declaration was known to be a class cannot happen - the kind is
    // settled in the type-name pass - but the box may still be missing if the payload was lowered
    // through the struct path, so build it rather than assuming
    if (structure.llvm_box == nullptr) {
        build_class_box(structure, *type, cmp_unit);
    }

    return ClassLayout{ structure.llvm_struct, structure.llvm_box, structure.typeinfo };
}

// two loops with two different scopes, and the difference is the whole design. the first is
// *declaration* scoped: a unit emits a body for every function its own module declares, and
// StmtCodegen::gen_function_decl looks that body's function up by mangled name in the current unit,
// so the symbol has to exist there before any body is emitted. the second is *reference* scoped: a
// call to a function another module defines needs a `declare` in this unit and nothing more, so only
// callees a unit actually names are copied in
void TypeLowering::build_function_maps()
{
    for (auto &cmp_unit : _ctx.cmp_units) {
        // first build all functions actually declared in the module
        for (auto fncdecl : cmp_unit->ast_module->nodes.of_type<AST::FunctionDeclNode>()) {
            // exactly the declarations this unit will emit a body for: the symbol has to exist before
            // gen_function_decl looks one up by mangled name. Everything left out is left out because
            // there is no body of ours to define - a builtin, an interface requirement or a template has
            // no symbol at all, and an extern or an intrinsic has one somebody else supplies. Each of
            // those is still declared by the reference-scoped loop below wherever it is actually named,
            // which is what keeps a program that touches no math from paying for every row of
            // stdlib/std/math/intrinsics.eco: resolving one is a signature match against LLVM's whole
            // intrinsic table
            const AST::FunctionEmission kind = AST::function_emission_kind(fncdecl);

            if (!AST::emission_has_body(kind)) {
                continue;
            }

            // an ODR-shared definition is deliberately *not* claimed here. It has no owning module, so
            // "the unit whose arena holds the declaration node" is the wrong home: an instantiation is
            // cloned into the *template's* module, which for `mem::alloc<Padded>` means the stdlib unit
            // would define a body over a struct only the application declares. It is emitted into every
            // unit that references it instead, by the drain
            if (kind == AST::FunctionEmission::t_odr_shared) {
                continue;
            }

            create_llvm_func_decl(fncdecl, *cmp_unit);
        }
    }

    // then go through all function calls inside each module
    // to decide which declarations to link in
    for (auto &cmp_unit : _ctx.cmp_units) {
        _ctx.current_cmp_unit = cmp_unit.get();

        for (auto fnccall : cmp_unit->ast_module->nodes.of_type<AST::FunctionCallExprNode>()) {
            // if there is no matching llvm function for the call inside of the module
            // we copy the declaration from another module
            auto decl = fnccall->decl;

            // a call this unit names needs this unit to name the symbol, and nothing more. The one shape
            // that gets no declaration is the one that has no symbol: a builtin call folds to a constant,
            // an interface requirement dispatches through the receiver's vtable, and a template has no
            // concrete signature - none of the three is a name the linker will ever be asked for
            if (!AST::emission_needs_declaration(AST::function_emission_kind(decl))) {
                continue;
            }

            if (!cmp_unit->function_table.get_function_id(decl)) {
                create_llvm_func_decl(decl, *cmp_unit);
            }
        }
    }
}

// each unit registers the declarations *its own* module holds, because those are the layouts it
// emits bodies against - nothing is copied between units here. the two shapes this leaves out both
// arrive on demand instead, in get_llvm_type: a generic instantiation, which has no TypeDeclNode at
// all, and a struct another module declared but this unit's instantiation of a template mentions.
// see the comment on that path for why keying the lazy registration on the ComplexType is what makes
// the two routes agree
void TypeLowering::build_struct_maps()
{
    for (auto &cmp_unit : _ctx.cmp_units) {
        for (auto &struct_decl : cmp_unit->ast_module->nodes.of_type<AST::TypeDeclNode>()) {
            // a generic struct template has type-parameter-typed properties and no concrete
            // layout; only its instantiations (Box<int>) are lowered, lazily in get_llvm_type.
            if (struct_decl->is_generic()) {
                continue;
            }
            create_llvm_struct_decl(struct_decl, *cmp_unit);
        }
    }
}

llvm::StructType *TypeLowering::callable_llvm_type()
{
    auto *ptr_type = llvm::PointerType::get(*_ctx.llvm_context, 0);

    // named rather than literal so the IR reads, and looked up before creating so every unit and every
    // signature share one type - `getTypeByName` is what makes the second ask return the first answer
    if (auto *existing = llvm::StructType::getTypeByName(*_ctx.llvm_context, "eco.callable")) {
        return existing;
    }

    return llvm::StructType::create(*_ctx.llvm_context, { ptr_type, ptr_type }, "eco.callable");
}

llvm::StructType *TypeLowering::iface_llvm_type()
{
    auto *ptr_type = llvm::PointerType::get(*_ctx.llvm_context, 0);

    // named and looked up before creating, exactly as eco.callable is: one type shared by every unit and
    // every interface, so two spellings of one erased value are one llvm::Type
    if (auto *existing = llvm::StructType::getTypeByName(*_ctx.llvm_context, "eco.iface")) {
        return existing;
    }

    return llvm::StructType::create(*_ctx.llvm_context, { ptr_type, ptr_type }, "eco.iface");
}

llvm::Value *TypeLowering::gen_has_value(llvm::Value *value, const AST::ValueType &type)
{
    // the tag, for a `T?` whose `T` had no null value to donate. `is_wrapped_optional()` is the one
    // spelling of that question - see ValueType::has_null_representation
    if (type.is_wrapped_optional()) {
        return _ctx.builder->CreateExtractValue(value, { AST::k_optional_has_index }, "opt.has");
    }

    // and otherwise the value *is* an address, so being present is being non-null. that covers a
    // `ptr<T>`, a class handle and a weak handle with one instruction and no unwrapping at all - which is
    // what makes `Foo?` cost exactly what `Foo` costs
    return _ctx.builder->CreateIsNotNull(value, "has_value");
}

llvm::Value *TypeLowering::gen_unwrapped(llvm::Value *value, const AST::ValueType &type)
{
    if (type.is_wrapped_optional()) {
        return _ctx.builder->CreateExtractValue(value, { AST::k_optional_value_index }, "opt.val");
    }

    // an address-like nullable is its own payload: `Foo?` and `Foo` are the same machine value, and the
    // difference between them was only ever what the type system would let you do with it
    return value;
}

llvm::Value *TypeLowering::gen_absent(
    const AST::ValueType &type,
    const Compiler::LLVM::CmpUnit &cmp_unit
)
{
    return llvm::Constant::getNullValue(get_llvm_type(type, cmp_unit));
}

llvm::StructType *TypeLowering::optional_llvm_type(
    const AST::ValueType &type,
    const Compiler::LLVM::CmpUnit &cmp_unit
)
{
    assert(type.is_wrapped_optional() && "optional_llvm_type over a type whose null value is its own");

    // **the unit's structure table is the cache, exactly as it is for every other struct.** the pair is an
    // ordinary layout now - AST::ComplexType::is_optional - so a member access reaches `__value` through
    // gen_member_lvalue, which resolves a GEP index out of that table. a *second* cache beside it was
    // worse than none: it is one per compiler while the table is one per unit, so the second unit to
    // mention a `string?` took the early return and never registered the layout, which is the
    // "is not declared in this compilation unit" throw from a synthesized teardown
    const AST::ComplexType *layout = type.get_complex_type();

    if (auto id = cmp_unit.structure_table->get_structure_id(layout); id != 0) {
        return cmp_unit.structure_table->get_structure(id).llvm_struct;
    }

    // the mangled name of the *payload*, so `int32?` and `float64?` are two shapes and two names.
    //
    // **and minted per unit, exactly like every other struct.** This used to look the name up in the
    // context first, so two units shared one `eco.optional.*` - which the `linkonce_odr` deinit and copy
    // constructor were held to need, `verify_odr_consistency` having compared the two copies as *text*
    // and a renamed `%eco.optional.X.1` reading as a divergence. That check compares layouts now
    // (Compiler::LLVM::first_odr_difference), so the sharing bought nothing and cost the one thing a
    // shared type cannot survive: the layout was built in whichever unit reached it first and holds
    // *that* unit's `%string`, while the values the next unit inserts into it are its own `%string.1`.
    //
    // `eco.callable`, `eco.iface` and `eco.classheader` keep their by-name lookup, and the difference is
    // the whole rule: their members are context-primitives, so they have no payload to split
    const AST::ValueType payload = type.optional_payload();
    const std::string name = "eco.optional." + payload.get_mangled_name();

    // opaque first and registered before the payload is lowered, exactly as create_llvm_struct_for_instance
    // does and for its reason: a payload that mentions this same optional resolves to the in-progress type
    llvm::StructType *minted = llvm::StructType::create(*_ctx.llvm_context, name);
    cmp_unit.structure_table->push_structure(layout, minted);

    std::vector<llvm::Type *> members(2);
    members[AST::k_optional_has_index] = get_llvm_type(layout->get_property_type(AST::k_optional_has_index), cmp_unit);
    members[AST::k_optional_value_index] = get_llvm_type(payload, cmp_unit);

    // by slot index rather than in written order, the rule every shape in Codegen/ClassLayout.h follows
    minted->setBody(members);

    return minted;
}

llvm::StructType *TypeLowering::class_header_llvm_type()
{
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Type *ptr_type = llvm::PointerType::get(*_ctx.llvm_context, 0);

    if (auto *existing = llvm::StructType::getTypeByName(*_ctx.llvm_context, "eco.classheader")) {
        return existing;
    }

    // the box's header with the payload cut off, by slot index for build_class_box's reason - these two
    // are the shape's only two writers and a GEP through this one has to land on the same word
    std::vector<llvm::Type *> members(ClassBox::payload_index);
    members[ClassBox::strong_index] = i64;
    members[ClassBox::weak_index] = i64;
    members[ClassBox::typeinfo_index] = ptr_type;

    return llvm::StructType::create(*_ctx.llvm_context, members, "eco.classheader");
}

llvm::StructType *TypeLowering::typeinfo_llvm_type()
{
    std::vector<llvm::Type *> members(2);
    members[ClassTypeInfo::conformance_count_index] = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    members[ClassTypeInfo::conformances_index] = llvm::PointerType::get(*_ctx.llvm_context, 0);

    // by slot index rather than in written order, so the names in Codegen/ClassLayout.h are what decides
    // the layout - the one place the descriptor's shape is spelled, for the writer and the scan both
    return llvm::StructType::get(*_ctx.llvm_context, members);
}

llvm::Constant *TypeLowering::get_or_create_vtable(
    const AST::ValueType &class_type,
    const AST::ValueType &interface,
    const Compiler::LLVM::CmpUnit &cmp_unit
)
{
    if (!class_type.has_complex_type() || !interface.is_interface()) {
        return nullptr;
    }

    const AST::ComplexType *implementor = class_type.get_complex_type();
    const AST::ComplexType *iface = interface.get_complex_type();

    // both mangled tokens, so `a::Circle` conforming to `b::Drawable` cannot share a table with any other
    // pairing - the same identity rule the typeinfo global follows
    //
    // the walk below only runs on a miss, which is what the callback shape buys: a widening asks for its
    // table every time it is lowered, and resolving which declaration answers each requirement is the
    // expensive half
    return get_or_create_odr_constant(
        implementor->mangled_token() + "." + iface->mangled_token() + ".vtable",
        [&]() -> llvm::Constant * {
            // which declaration answers each requirement, in slot order. asked of the one walk that
            // decides it, so the table and every dispatch site agree about which entry a method is
            const std::vector<AST::FunctionDeclNode *> implementations =
                AST::interface_implementations(implementor, interface, _ctx.type_registry());

            if (implementations.empty()) {
                return nullptr;
            }

            llvm::Type *opaque_ptr = llvm::PointerType::get(*_ctx.llvm_context, 0);

            std::vector<llvm::Constant *> slots;
            slots.reserve(implementations.size() + IfaceValue::first_method_slot);

            // slot 0 is the implementor's release thunk - see Codegen/IfaceValue.h. an erased value owns
            // a reference and its release site knows only the interface, so this is the one way to reach
            // the concrete teardown. filled here, at the widening, which is late enough that the layout
            // the thunk is built from exists - the typeinfo descriptor is built during type lowering and
            // could not have held it
            slots.push_back(_ctx.classes->get_or_create_release_thunk(class_type));

            // the table is written into, which every lowering path here already does through a const
            // CmpUnit - the constness is about the *unit* not being replaced, not its tables being frozen
            auto &unit = const_cast<Compiler::LLVM::CmpUnit &>(cmp_unit);

            for (AST::FunctionDeclNode *impl : implementations) {
                // an operator requirement has no slot - such an interface is refused as a storable type
                // before anything asks for its table, so reaching here means that refusal is missing
                if (impl == nullptr) {
                    return nullptr;
                }

                auto function_id = unit.function_table.get_function_id(impl);

                // the implementation may not have been declared into *this* unit yet: the class can be
                // declared in another module, exactly as a class layout can. declaring it is the same
                // idempotent call build_function_maps makes
                if (function_id == 0) {
                    create_llvm_func_decl(impl, unit);
                    function_id = unit.function_table.get_function_id(impl);
                }

                if (function_id == 0) {
                    return nullptr;
                }

                slots.push_back(unit.function_table.get_llvm_function(function_id));
            }

            auto *table_type = llvm::ArrayType::get(opaque_ptr, slots.size());
            return llvm::ConstantArray::get(table_type, slots);
        },
        cmp_unit);
}

llvm::FunctionType *TypeLowering::get_llvm_function_type(
    const AST::CallableSignature &signature,
    const Compiler::LLVM::CmpUnit &cmp_unit,
    FunctionCallingShape shape
)
{
    std::vector<llvm::Type *> param_types;
    param_types.reserve(signature.parameter_types.size() + 1);

    // the environment first, under Echo's shape - see the header. a non-capturing target ignores it.
    // a C function pointer has no environment, and saying so at the call site is the whole of
    // FunctionCallingShape
    if (shape == FunctionCallingShape::t_echo) {
        param_types.push_back(llvm::PointerType::get(*_ctx.llvm_context, 0));
    }

    for (const auto &param : signature.parameter_types) {
        param_types.push_back(get_llvm_type(param, cmp_unit));
    }

    // **the same return ABI a declaration gets, or the two shapes of one call disagree.** this builds the
    // type a *callable value* and an *interface requirement* are called through, and the body on the other
    // end of both is emitted by StmtCodegen::gen_function_decl from a declaration - so if only one of the
    // two asks Compiler::LLVM::return_abi_for, a closure returning a big struct is called with the
    // arguments one slot out of place and reads its receiver as its answer
    llvm::Type *lowered_return = get_llvm_type(signature.return_type, cmp_unit);

    // **t_c is always-direct.** Echo's sret convention is not C's, and claiming it for a C
    // function pointer is a silent ABI miss the moment a struct slips through
    // c_function_signature_refusal. return_abi_of is the declaration half of the same
    // exemption; this is the callable / indirect-call half
    const ReturnAbi abi = shape == FunctionCallingShape::t_c
        ? ReturnAbi{}
        : return_abi_for(lowered_return, _ctx.layout());

    if (abi.is_indirect()) {
        lowered_return = llvm::Type::getVoidTy(*_ctx.llvm_context);
        param_types.insert(param_types.begin(), llvm::PointerType::getUnqual(*_ctx.llvm_context));
    }

    return llvm::FunctionType::get(lowered_return, param_types, false);
}

llvm::Type *TypeLowering::get_llvm_type(const AST::ValueType &type, const Compiler::LLVM::CmpUnit &cmp_unit)
{
    llvm::Type *base_type = nullptr;

    // **`T?` first, and only when `T` has no null value of its own.** over a pointer, a class or a weak
    // the flag changes nothing about the machine type - a null address already means absent - so those
    // fall through to their own arms below and `Foo?` costs exactly what `Foo` costs. over anything else
    // there is no spare representation to donate, so the value is tagged
    //
    // asked through ValueType::is_wrapped_optional() rather than spelled out here, because coerce_value
    // and the null comparison branch on the same question and three answers would be three shapes
    if (type.is_wrapped_optional()) {
        return optional_llvm_type(type, cmp_unit);
    }

    // every pointer level is the same opaque `ptr` under LLVM's opaque pointer model, so the
    // pointee is never lowered. that also sidesteps lowering a pointer to a struct that has
    // not been declared in this compilation unit yet
    if (type.is_pointer()) {
        return llvm::PointerType::get(*_ctx.llvm_context, 0);
    }

    // a class-typed *value* is the handle - one machine address into the heap block, never the block
    // itself. so this answers before the struct arm, and deliberately does not lower the layout: that
    // is get_or_create_class_layout's job, asked for by the few places that need it. lowering it here
    // instead would mean every `ptr<Foo>` and every borrow parameter dragged the whole layout in
    if (type.is_class()) {
        return llvm::PointerType::get(*_ctx.llvm_context, 0);
    }

    // and a weak handle is the very same address - the block it names, differently typed. the whole of
    // what makes it weak is which count it moved and that reading it needs an upgrade, both of which are
    // settled long before lowering, so there is nothing left for the machine type to carry
    if (type.is_weak()) {
        return llvm::PointerType::get(*_ctx.llvm_context, 0);
    }

    // a callable is a *fat* pointer, `{ ptr fn, ptr env }`. two words rather than one because a
    // non-capturing callable - which includes every plain function used as a value - must not have to
    // allocate an environment just to be callable; its env slot is simply null. and the env has to be
    // there at all because a capturing closure's storage cannot live in the function pointer
    //
    // structural, so this type is built rather than looked up: two spellings of one signature are one
    // Echo type and must be one llvm::Type, which an anonymous StructType gives for free
    if (type.is_callable()) {
        return callable_llvm_type();
    }

    // a C function pointer is one opaque word - C's shape, no environment. a kind rather than a
    // flag on the callable so this cannot silently share the two-word lowering above
    if (type.is_c_function()) {
        return llvm::PointerType::get(*_ctx.llvm_context, 0);
    }

    // an interface *value* is a fat pointer too, `{ ptr object, ptr vtable }`, and for the callable's
    // reason: the object alone cannot answer which method to call without a scan at every call site,
    // so the vtable is resolved once at the **widening**, where the concrete class is still known
    //
    // field 0 holds the class handle, which is what makes a receiver free: a class method's `$this` is
    // `Circle&` - the address of a slot holding a handle - and `&iface.object` is exactly that shape
    if (type.is_interface()) {
        return iface_llvm_type();
    }

    if (type.is_primitive()) {
        base_type = get_llvm_type(type.get_primitive_type());
    }
    // an enum shares the arm because it shares the shape: a layout of ordinary properties, `__tag`
    // first, reached through the same structure table and lowered by the same two entry points. the
    // discriminant and the payload slots are not codegen's invention, which is what keeps this from
    // being a second layout minter beside ClassBox
    else if (type.is_struct() || type.is_enum()) {
        auto *complex = type.get_complex_type();
        auto struct_id = cmp_unit.structure_table->get_structure_id(complex);

        // lower the layout into this unit on demand. build_struct_maps only registers the structs
        // each module declares itself, so two cases arrive here undeclared:
        //
        //  - a generic instantiation (Box<int>), which has no TypeDeclNode at all;
        //  - a struct declared in *another* module. `mem::alloc<Padded>(2)` is instantiated into
        //    the stdlib module, because that is where the `alloc<T>` template lives, but `Padded`
        //    is declared in main - so lowering that instance needs a layout the stdlib unit has
        //    never seen. allocating a user struct on the heap is a headline use of mem::, so this
        //    is the common case rather than a corner
        //
        // both are keyed on the ComplexType, so identity survives even though each unit ends up
        // with its own llvm::StructType for the same Echo type
        if (!struct_id && complex) {
            create_llvm_struct_for_instance(complex, cmp_unit);
            struct_id = cmp_unit.structure_table->get_structure_id(complex);
        }

        if (!struct_id) {
            throw _ctx.error(fmt::format(
                "Struct '{}' is not declared in compilation unit '{}' ({})",
                complex ? complex->name.value_or("<anonymous>") : "<null>",
                cmp_unit.ast_module ? cmp_unit.ast_module->name : "<unknown>",
                _ctx.function_context()));
        }

        base_type = cmp_unit.structure_table->get_structure(struct_id).llvm_struct;
    }
    else if (type.is_type_param()) {
        // a resolved instance never carries a type parameter; reaching here is a compiler bug
        // (a template escaped monomorphization) rather than a user error. name the parameter and
        // where it was declared, so the report points at the generic that failed to instantiate
        // instead of leaving the reader to guess which type carried it
        const AST::TypeParamDecl *param = type.get_type_param();

        std::string declared_at;
        if (param->name_token.has_value()) {
            declared_at = fmt::format(" declared at {}:{}",
                param->name_token.value().line(), param->name_token.value().char_offset());
        }

        throw _ctx.error(fmt::format(
            "Cannot lower unresolved generic type parameter '{}'{} {}: generics must be "
            "instantiated with concrete types before compilation",
            param->describe(), declared_at, _ctx.function_context()));
    }
    else {
        throw _ctx.error(fmt::format(
            "Unsupported type '{}' {}", type.get_type_desciption(), _ctx.function_context()));
    }

    return base_type;
}

llvm::Type *TypeLowering::get_llvm_type(const AST::ValueTypePrimitive type)
{
    switch (type) {
        case AST::ValueTypePrimitive::t_void:
            return llvm::Type::getVoidTy(*_ctx.llvm_context);
        case AST::ValueTypePrimitive::t_float32:
            return llvm::Type::getFloatTy(*_ctx.llvm_context);
        case AST::ValueTypePrimitive::t_float64:
            return llvm::Type::getDoubleTy(*_ctx.llvm_context);
        case AST::ValueTypePrimitive::t_int8:
            return llvm::Type::getInt8Ty(*_ctx.llvm_context);
        case AST::ValueTypePrimitive::t_int16:
            return llvm::Type::getInt16Ty(*_ctx.llvm_context);
        case AST::ValueTypePrimitive::t_int32:
            return llvm::Type::getInt32Ty(*_ctx.llvm_context);
        case AST::ValueTypePrimitive::t_int64:
            return llvm::Type::getInt64Ty(*_ctx.llvm_context);
        case AST::ValueTypePrimitive::t_uint8:
            return llvm::Type::getInt8Ty(*_ctx.llvm_context);
        case AST::ValueTypePrimitive::t_uint16:
            return llvm::Type::getInt16Ty(*_ctx.llvm_context);
        case AST::ValueTypePrimitive::t_uint32:
            return llvm::Type::getInt32Ty(*_ctx.llvm_context);
        case AST::ValueTypePrimitive::t_uint64:
            return llvm::Type::getInt64Ty(*_ctx.llvm_context);
        // pointer-width integers. the width comes from the same constant AST::get_primitive_size
        // answers from, so the ast-level size and the lowered llvm type cannot disagree
        case AST::ValueTypePrimitive::t_usize:
        case AST::ValueTypePrimitive::t_isize:
            return llvm::Type::getIntNTy(*_ctx.llvm_context, ECO_TARGET_POINTER_SIZE * 8);
        case AST::ValueTypePrimitive::t_bool:
            return llvm::Type::getInt1Ty(*_ctx.llvm_context);
        default:
            throw _ctx.error(fmt::format(
                "Unsupported primitive type '{}' {}",
                AST::get_primitive_name(type), _ctx.function_context()));
    }
}

llvm::Value *TypeLowering::coerce_value(llvm::Value *value, const AST::ValueType &from, const AST::ValueType &to, const CmpUnit &cmp_unit)
{
    const AST::ValueType &source = from;
    const AST::ValueType &target = to;

    if (source == target) {
        return value;
    }

    // **the interface widening.** a class handle becomes `{ object, vtable }` - the object is the handle
    // unchanged and the vtable is resolved here, from the concrete class, which is the whole reason the
    // conversion lives at this end rather than at the call site
    //
    // it is here, in the one conversion table, so that every destination gets it from one change:
    // an initializer, an assignment, a member write, an argument and a return all route through
    // coerce_value. ahead of the pointer arm below because a class handle *is* a pointer under the hood
    // and that arm would pass it straight through as one
    if (target.is_interface() && source.is_class()) {
        llvm::Constant *vtable = get_or_create_vtable(source, target, cmp_unit);

        // every shape that has no table - a non-conforming class, an unmet requirement, an operator
        // requirement, a generic implementor - is refused with a located diagnostic before codegen, so
        // this is a compiler bug rather than a program error
        if (vtable == nullptr) {
            throw _ctx.error(fmt::format(
                "no vtable for '{}' as '{}' {}",
                source.get_type_desciption(), target.get_type_desciption(), _ctx.function_context()));
        }

        llvm::Value *erased = llvm::UndefValue::get(iface_llvm_type());
        erased = _ctx.builder->CreateInsertValue(erased, value, { IfaceValue::object_index }, "iface.obj");
        erased = _ctx.builder->CreateInsertValue(erased, vtable, { IfaceValue::vtable_index }, "iface.vt");

        return erased;
    }

    // **wrapping into a `T?`, and unwrapping back out.** only ever reached for the tagged shape: over an
    // address-like `T` the flag is invisible at the machine level, so the identity fast path at the top of
    // this function already returned, and the arms below pass the value through as they always did
    //
    // the *unwrap* direction is not an implicit conversion - is_implicitly_convertible refuses it, and
    // deliberately - so it arrives here only from a site that has already proven the value is there:
    // `guard`, `??`, `?->`. this is the store, not the check
    // asked once each: every arm below is about one side or the other being the tagged shape, and the
    // question is a tag read through a ComplexType rather than a flag test
    const bool target_is_tagged = target.is_wrapped_optional();
    const bool source_is_tagged = source.is_wrapped_optional();

    if (target_is_tagged || source_is_tagged) {
        // **an undetermined source is never wrapped as present.** it means a `null` that was never bound
        // to its destination, and wrapping one produces `{ i1 true, <garbage> }` - a value that claims to
        // be there and is not, which is the single worst thing this code could emit. a throw rather than a
        // guess: every path that legitimately reaches here knows its source type, so this firing is a
        // compiler bug and wants to say so rather than to be quietly absorbed
        if (target_is_tagged && AST::is_undetermined_type(source)) {
            throw _ctx.error(fmt::format(
                "an untyped value reached a '{}' destination - a null here was never bound to its type {}",
                target.get_type_desciption(), _ctx.function_context()));
        }

        // asked of AST::arrival_wraps_optional, the same question AST::argument_fit ranked this arrival by
        // and AST::CallResolver minted the cast from - this is the half that emits the wrap
        if (AST::arrival_wraps_optional(source, target)) {
            // `T` -> `T?`: present, carrying the value. the payload is coerced first, so widening
            // `int32 -> int64?` is one conversion and one wrap rather than a shape mismatch
            //
            // the payload is built inside the arm that wants it: the two arms are mutually exclusive, and a
            // ValueType is not a free thing to materialise twice for one of them to be thrown away
            const AST::ValueType target_payload = AST::ValueType::make_non_nullable(target);

            llvm::Value *payload = coerce_value(value, source, target_payload, cmp_unit);
            llvm::Value *wrapped = llvm::UndefValue::get(optional_llvm_type(target, cmp_unit));

            wrapped = _ctx.builder->CreateInsertValue(
                wrapped,
                llvm::ConstantInt::getTrue(*_ctx.llvm_context),
                { AST::k_optional_has_index },
                "opt.has");

            return _ctx.builder->CreateInsertValue(
                wrapped, payload, { AST::k_optional_value_index }, "opt.val");
        }

        if (source_is_tagged && !target.is_nullable()) {
            // `T?` -> `T`: read the payload out. the tag is not tested here - whoever asked for this
            // narrowing tested it, and that is the whole reason the narrowing is not implicit
            llvm::Value *payload = _ctx.builder->CreateExtractValue(
                value, { AST::k_optional_value_index }, "opt.val");

            return coerce_value(payload, AST::ValueType::make_non_nullable(source), target, cmp_unit);
        }

        // both sides nullable and not identical - `int32? -> int64?`. **passed through unconverted**: the
        // payloads would have to be unwrapped, converted and rewrapped under the tag they arrived with,
        // and that is not written yet. left as it is rather than guessed at, since every arrival that
        // reaches codegen with two different wrapped payloads is a shape this file cannot yet lower
        return value;
    }

    // an address is passed along as the address it is. reinterpreting one as pointing at a
    // different type is free under opaque pointers, and narrowing a nullable pointer to a
    // borrow is an assertion rather than a conversion - the trap for that is emitted by the
    // cast itself, not here
    if (target.is_pointer() || source.is_pointer()) {
        return value;
    }

    // a weak handle is an address too, and the two operations that produce one - gen_weak_of and
    // gen_strong_upgrade - already hand back exactly the value their destination wants. so there is
    // nothing to convert here, and importantly nothing to convert *silently*: a weak arriving where a
    // class is wanted was refused by is_implicitly_convertible long before this
    if (target.is_weak() || source.is_weak()) {
        return value;
    }

    if (!target.is_primitive()) {
        return value;
    }

    llvm::Type *target_llvm = get_llvm_type(target, cmp_unit);
    if (value->getType() == target_llvm) {
        return value;
    }

    // BinaryExprNode::result_type() answers void whenever its operands differ, so `from` is
    // frequently undeterminable at a decl/assign site. fall back to what the value actually
    // is, and let the target supply the signedness
    bool source_known = source.is_primitive() && !source.is_void();
    bool source_is_float = source_known ? source.is_floating_type() : value->getType()->isFloatingPointTy();
    bool source_is_int = source_known ? source.is_integer_type() : value->getType()->isIntegerTy();
    bool source_is_bool = source_known && source.is_boolean_type();
    bool source_signed = source_known ? source.is_signed_integer() : true;

    if (target.is_floating_type()) {
        if (source_is_int && !source_is_bool) {
            return source_signed
                ? _ctx.builder->CreateSIToFP(value, target_llvm)
                : _ctx.builder->CreateUIToFP(value, target_llvm);
        }
        if (source_is_bool) {
            return _ctx.builder->CreateUIToFP(value, target_llvm);
        }
        if (source_is_float) {
            return value->getType()->getPrimitiveSizeInBits() < target_llvm->getPrimitiveSizeInBits()
                ? _ctx.builder->CreateFPExt(value, target_llvm)
                : _ctx.builder->CreateFPTrunc(value, target_llvm);
        }
    }

    else if (target.is_integer_type()) {
        if (source_is_float) {
            return target.is_signed_integer()
                ? _ctx.builder->CreateFPToSI(value, target_llvm)
                : _ctx.builder->CreateFPToUI(value, target_llvm);
        }
        // a bool is one bit that means 0 or 1, so it always widens *unsigned*. it needs its own
        // row because is_integer_type() deliberately excludes bool, so the cast below never saw
        // it - and if it had, source_signed would have sign extended `true` into -1. reached by
        // `int32 $x = $b;` for a bool $b, and by a bool literal in a generic body whose T only
        // becomes an integer at the instance. `int32 $x = true;` does *not* reach it: the bool
        // literal parser converts that one at the destination, so both sides are already int32
        if (source_is_bool || value->getType()->isIntegerTy(1)) {
            return _ctx.builder->CreateIntCast(value, target_llvm, false);
        }

        if (source_is_int) {
            // CreateIntCast picks extend or truncate from the widths, and takes the *source's*
            // signedness for the extend - which is what makes uint8 -> uint32 a zero extend
            // where the old hand rolled cascades always sign extended
            return _ctx.builder->CreateIntCast(value, target_llvm, source_signed);
        }
    }

    else if (target.is_boolean_type()) {
        if (source_is_float) {
            return _ctx.builder->CreateFCmpONE(value, llvm::ConstantFP::get(value->getType(), 0.0));
        }
        if (source_is_int) {
            return _ctx.builder->CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0));
        }
    }

    throw _ctx.error(fmt::format("unsupported type cast from '{}' to '{}' {}",
        from.get_type_desciption(), to.get_type_desciption(), _ctx.function_context()));
}
};
