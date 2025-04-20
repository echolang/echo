#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/Codegen/IfaceValue.h"
#include "Compiler/LLVM/Codegen/ClassCodegen.h"
#include "Compiler/LLVM/Codegen/IntrinsicResolution.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "eco.h"

#include "AST/ASTBundle.h"
#include "AST/ASTConformance.h"
#include "AST/ASTMangler.h"
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
void TypeLowering::create_cmp_units(const AST::Bundle &bundle)
{
    for (auto &module : bundle.modules) {
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

        _ctx.cmp_unit_map[module->name] = cmp_unit.get();
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

llvm::Function *TypeLowering::create_llvm_func_decl(const AST::FunctionDeclNode *node, Compiler::LLVM::CmpUnit &cmp_unit)
{
    auto func_name = AST::mangle_function_name(node);
    auto func_type = node->get_return_type();

    // function arguments
    // @TODO support complex types
    std::vector<llvm::Type *> arg_types;
    for (auto &arg : node->args) {
        // one lowering path for every parameter shape: get_llvm_type already handles structs,
        // primitives and pointers. the old split called get_primitive_type() on anything
        // non-struct, which for a pointer would answer t_void and then assert inside LLVM
        arg_types.push_back(get_llvm_type(arg->type_node()->type, cmp_unit));
    }

    llvm::FunctionType *requested_type = llvm::FunctionType::get(get_llvm_type(func_type, cmp_unit), arg_types, false);

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

    llvm::Function *llvm_func = llvm::Function::Create(requested_type, llvm::Function::ExternalLinkage, func_name, cmp_unit.llvm_module.get());

    // store in the function map
    cmp_unit.function_table.push_function(func_name, node, llvm_func);

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
    Structure &structure, const AST::ComplexType &type, const Compiler::LLVM::CmpUnit &cmp_unit)
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
    const AST::ComplexType &interface, const Compiler::LLVM::CmpUnit &cmp_unit)
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
    const AST::ComplexType &type, const Compiler::LLVM::CmpUnit &cmp_unit)
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
    const Compiler::LLVM::CmpUnit &cmp_unit)
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
    const AST::ComplexType *type, const Compiler::LLVM::CmpUnit &cmp_unit)
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
            // skip generic function templates during function map building
            if (fncdecl->is_generic()) {
                continue;
            }
            // a builtin is answered at the call site and has no symbol, so declaring one would
            // emit a `declare` nobody defines and the call would fail to resolve at link time
            if (fncdecl->is_builtin()) {
                continue;
            }
            // an interface requirement is a signature with no implementation - the implementors have
            // the bodies, under their own symbols. declaring one would emit a `declare` nobody defines
            if (fncdecl->is_interface_requirement()) {
                continue;
            }
            // an intrinsic has no body to emit either, so this loop's reason for existing - the symbol
            // must exist before gen_function_decl looks a body up - does not apply to one. the second,
            // reference-scoped loop declares the ones a unit actually calls, which is what keeps a
            // program that touches no math from paying for every row in stdlib/math/intrinsics.eco:
            // each is an IIT-table signature match against LLVM's whole intrinsic list
            if (fncdecl->intrinsic.has_value()) {
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
            if (!decl) {
                continue;
            }

            // skip generic function templates
            if (decl->is_generic()) {
                continue;
            }

            // as above: a builtin call folds to a constant, there is nothing to link
            if (decl->is_builtin()) {
                continue;
            }

            // a call whose declaration is an interface requirement is dispatched through the receiver's
            // vtable rather than to a symbol, so there is nothing to link here either
            if (decl->is_interface_requirement()) {
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
        for(auto &struct_decl : cmp_unit->ast_module->nodes.of_type<AST::TypeDeclNode>()) {
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
        return _ctx.builder->CreateExtractValue(value, { OptionalBox::has_index }, "opt.has");
    }

    // and otherwise the value *is* an address, so being present is being non-null. that covers a
    // `ptr<T>`, a class handle and a weak handle with one instruction and no unwrapping at all - which is
    // what makes `Foo?` cost exactly what `Foo` costs
    return _ctx.builder->CreateIsNotNull(value, "has_value");
}

llvm::Value *TypeLowering::gen_unwrapped(llvm::Value *value, const AST::ValueType &type)
{
    if (type.is_wrapped_optional()) {
        return _ctx.builder->CreateExtractValue(value, { OptionalBox::value_index }, "opt.val");
    }

    // an address-like nullable is its own payload: `Foo?` and `Foo` are the same machine value, and the
    // difference between them was only ever what the type system would let you do with it
    return value;
}

llvm::Value *TypeLowering::gen_absent(
    const AST::ValueType &type, const Compiler::LLVM::CmpUnit &cmp_unit)
{
    return llvm::Constant::getNullValue(get_llvm_type(type, cmp_unit));
}

llvm::StructType *TypeLowering::optional_llvm_type(
    const AST::ValueType &type, const Compiler::LLVM::CmpUnit &cmp_unit)
{
    assert(type.is_wrapped_optional() && "optional_llvm_type over a type whose null value is its own");

    if (auto cached = _optional_types.find(type); cached != _optional_types.end()) {
        return cached->second;
    }

    // the mangled name of the *payload*, so `int32?` and `float64?` are two shapes and two names, and so
    // the same `int32?` reached from two units is one llvm::Type. name-first lookup for the reason
    // class_header_llvm_type uses one
    const AST::ValueType payload = AST::ValueType::make_non_nullable(type);
    const std::string name = "eco.optional." + payload.get_mangled_name();

    if (auto *existing = llvm::StructType::getTypeByName(*_ctx.llvm_context, name)) {
        _optional_types[type] = existing;
        return existing;
    }

    std::vector<llvm::Type *> members(2);
    members[OptionalBox::has_index] = llvm::Type::getInt1Ty(*_ctx.llvm_context);
    members[OptionalBox::value_index] = get_llvm_type(payload, cmp_unit);

    // by slot index rather than in written order, the rule every shape in Codegen/ClassLayout.h follows
    llvm::StructType *minted = llvm::StructType::create(*_ctx.llvm_context, members, name);
    _optional_types[type] = minted;

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
    const Compiler::LLVM::CmpUnit &cmp_unit)
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
    const AST::CallableSignature &signature, const Compiler::LLVM::CmpUnit &cmp_unit)
{
    std::vector<llvm::Type *> param_types;
    param_types.reserve(signature.parameter_types.size() + 1);

    // the environment first, always - see the header. a non-capturing target ignores it
    param_types.push_back(llvm::PointerType::get(*_ctx.llvm_context, 0));

    for (const auto &param : signature.parameter_types) {
        param_types.push_back(get_llvm_type(param, cmp_unit));
    }

    return llvm::FunctionType::get(get_llvm_type(signature.return_type, cmp_unit), param_types, false);
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
    else if (type.is_struct()) {
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
    if (target.is_wrapped_optional() || source.is_wrapped_optional()) {
        const AST::ValueType source_payload = AST::ValueType::make_non_nullable(source);
        const AST::ValueType target_payload = AST::ValueType::make_non_nullable(target);

        // **an undetermined source is never wrapped as present.** it means a `null` that was never bound
        // to its destination, and wrapping one produces `{ i1 true, <garbage> }` - a value that claims to
        // be there and is not, which is the single worst thing this code could emit. a throw rather than a
        // guess: every path that legitimately reaches here knows its source type, so this firing is a
        // compiler bug and wants to say so rather than to be quietly absorbed
        if (target.is_wrapped_optional() && AST::is_undetermined_type(source)) {
            throw _ctx.error(fmt::format(
                "an untyped value reached a '{}' destination - a null here was never bound to its type {}",
                target.get_type_desciption(), _ctx.function_context()));
        }

        if (target.is_wrapped_optional() && !source.is_nullable()) {
            // `T` -> `T?`: present, carrying the value. the payload is coerced first, so widening
            // `int32 -> int64?` is one conversion and one wrap rather than a shape mismatch
            llvm::Value *payload = coerce_value(value, source, target_payload, cmp_unit);
            llvm::Value *wrapped = llvm::UndefValue::get(optional_llvm_type(target, cmp_unit));

            wrapped = _ctx.builder->CreateInsertValue(
                wrapped,
                llvm::ConstantInt::getTrue(*_ctx.llvm_context),
                { OptionalBox::has_index },
                "opt.has");

            return _ctx.builder->CreateInsertValue(
                wrapped, payload, { OptionalBox::value_index }, "opt.val");
        }

        if (source.is_wrapped_optional() && !target.is_nullable()) {
            // `T?` -> `T`: read the payload out. the tag is not tested here - whoever asked for this
            // narrowing tested it, and that is the whole reason the narrowing is not implicit
            llvm::Value *payload = _ctx.builder->CreateExtractValue(
                value, { OptionalBox::value_index }, "opt.val");

            return coerce_value(payload, source_payload, target, cmp_unit);
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
