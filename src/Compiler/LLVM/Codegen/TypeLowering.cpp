#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "eco.h"

#include "AST/ASTBundle.h"
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

static llvm::Intrinsic::IndependentIntrinsics get_intrinsic_for_string(const std::string &name)
{
    if (name == "llvm.sin") {
        return llvm::Intrinsic::sin;
    } else if (name == "llvm.cos") {
        return llvm::Intrinsic::cos;
    } else if (name == "llvm.exp") {
        return llvm::Intrinsic::exp;
    } else if (name == "llvm.log") {
        return llvm::Intrinsic::log;
    } else if (name == "llvm.sqrt") {
        return llvm::Intrinsic::sqrt;
    } else if (name == "llvm.pow") {
        return llvm::Intrinsic::pow;
    }

    return llvm::Intrinsic::not_intrinsic;
}

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

    // handle intrinsic functions
    if (node->intrinsic.has_value()) {
        llvm::Function *intrinsic_llvm_func = llvm::Intrinsic::getDeclaration(cmp_unit.llvm_module.get(), get_intrinsic_for_string(node->intrinsic.value()), arg_types);
        cmp_unit.function_table.push_function(func_name, node, intrinsic_llvm_func);
        return intrinsic_llvm_func;
    }

    llvm::FunctionType *requested_type = llvm::FunctionType::get(get_llvm_type(func_type, cmp_unit), arg_types, false);

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
        build_class_box(cmp_unit.structure_table->get_structure(struct_id), type_name, cmp_unit);
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
        build_class_box(cmp_unit.structure_table->get_structure(struct_id), type_name, cmp_unit);
    }

    return llvm_struct_type;
}

void TypeLowering::build_class_box(
    Structure &structure, const std::string &type_name, const Compiler::LLVM::CmpUnit &cmp_unit)
{
    assert(structure.llvm_struct != nullptr);

    if (structure.llvm_box != nullptr) {
        return;
    }

    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = llvm::PointerType::get(*_ctx.llvm_context, 0);

    // the field order is the contract Codegen/ClassLayout.h states: strong count first so retain and
    // release need no offset, then the identity pointer, then the payload
    std::vector<llvm::Type *> box_members(3);
    box_members[ClassBox::strong_index] = i64;
    box_members[ClassBox::typeinfo_index] = opaque_ptr;
    box_members[ClassBox::payload_index] = structure.llvm_struct;

    structure.llvm_box = llvm::StructType::create(*_ctx.llvm_context, box_members, type_name + ".box");

    // one byte whose *address* is the class's identity. linkonce_odr so every unit may define it and
    // the linker keeps one - which is what makes the address comparable across modules without a
    // numbering scheme anybody has to keep stable
    const std::string typeinfo_name = type_name + ".typeinfo";
    structure.typeinfo = cmp_unit.llvm_module->getGlobalVariable(typeinfo_name, true);

    if (structure.typeinfo == nullptr) {
        llvm::Type *i8 = llvm::Type::getInt8Ty(*_ctx.llvm_context);
        structure.typeinfo = new llvm::GlobalVariable(
            *cmp_unit.llvm_module,
            i8,
            /*isConstant=*/true,
            llvm::GlobalValue::LinkOnceODRLinkage,
            llvm::ConstantInt::get(i8, 0),
            typeinfo_name);
    }
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
        build_class_box(structure, type->name.value_or("anon"), cmp_unit);
    }

    return ClassLayout{ structure.llvm_struct, structure.llvm_box, structure.typeinfo };
}

void TypeLowering::build_function_maps(const AST::Bundle &bundle)
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

            if (!cmp_unit->function_table.get_function_id(decl)) {
                create_llvm_func_decl(decl, *cmp_unit);
            }
        }
    }
}

void TypeLowering::build_struct_maps(const AST::Bundle &bundle)
{
    // for now we do dump implementation which just copies all
    // struct types between all compilation units, this obviosly
    // should in the future only happen if a compilation unit actually
    // references the structure
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

    // an address is passed along as the address it is. reinterpreting one as pointing at a
    // different type is free under opaque pointers, and narrowing a nullable pointer to a
    // borrow is an assertion rather than a conversion - the trap for that is emitted by the
    // cast itself, not here
    if (target.is_pointer() || source.is_pointer()) {
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
