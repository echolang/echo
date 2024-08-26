#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/ASTBundle.h"
#include "AST/ASTMangler.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/StructNode.h"

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
    for (auto &module : bundle.modules)
    {
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
        auto &arg_type = arg->type_node()->type;

        llvm::Type *param_type = nullptr;
        if ((arg_type.is_struct() || arg_type.is_class()) && arg_type.get_complex_type()) {
            param_type = get_llvm_type(arg_type, cmp_unit);
        } else {
            param_type = get_llvm_type(arg->type_node()->type.get_primitive_type());
        }

        // If the parameter is a pointer/reference, wrap it in a pointer type
        if (arg_type.is_pointer()) {
            param_type = llvm::PointerType::get(param_type, 0);
        }

        arg_types.push_back(param_type);
    }

    // handle intrinsic functions
    if (node->intrinsic.has_value()) {
        llvm::Function *intrinsic_llvm_func = llvm::Intrinsic::getDeclaration(cmp_unit.llvm_module.get(), get_intrinsic_for_string(node->intrinsic.value()), arg_types);
        cmp_unit.function_table.push_function(func_name, node, intrinsic_llvm_func);
        return intrinsic_llvm_func;
    }

    llvm::FunctionType *llvm_fnc_type = llvm::FunctionType::get(get_llvm_type(func_type, cmp_unit), arg_types, false);
    llvm::Function *llvm_func = llvm::Function::Create(llvm_fnc_type, llvm::Function::ExternalLinkage, func_name, cmp_unit.llvm_module.get());

    // store in the function map
    cmp_unit.function_table.push_function(func_name, node, llvm_func);

    return llvm_func;
}

llvm::StructType *TypeLowering::create_llvm_struct_decl(const AST::StructDeclNode *node, Compiler::LLVM::CmpUnit &cmp_unit)
{
    if (!node->name_token.has_value()) {
        assert(false);
        throw _ctx.error("Anonymous struct declarations are not yet supported.");
    }

    auto struct_name = node->struct_name();
    // if (_ctx.current_cmp_unit->struct_table.is_defined(struct_name)) {
    //     assert(false);
    //     throw _ctx.error(fmt::format(
    //         "Struct '{}' is already defined.",
    //         struct_name
    //     ));
    // }

    // make the prop types
    std::vector<llvm::Type *> member_types;
    for (const auto &prop : node->properties()) {
        llvm::Type *llvm_type = get_llvm_type(prop->type_node()->type, cmp_unit);
        if (!llvm_type) {
            assert(false);
            throw _ctx.error(fmt::format(
                "Unknown type for field '{}' in struct '{}'.",
                prop->name(), struct_name
            ));
        }
        member_types.push_back(llvm_type);
    }

    // define the llvm struct type
    llvm::StructType *llvm_struct_type = llvm::StructType::create(*_ctx.llvm_context, member_types, struct_name);

    // store the struct in the struct table
    cmp_unit.structure_table->push_structure(node, llvm_struct_type);

    return llvm_struct_type;
}

llvm::StructType *TypeLowering::create_llvm_struct_for_instance(const AST::ComplexType *type, const Compiler::LLVM::CmpUnit &cmp_unit)
{
    std::string struct_name = type->name.value_or("anon");

    // create the struct opaque first and register it, so a self-referential instantiation
    // (a property that mentions the same instantiation) resolves to this in-progress type.
    llvm::StructType *llvm_struct_type = llvm::StructType::create(*_ctx.llvm_context, struct_name);
    cmp_unit.structure_table->push_structure(type, llvm_struct_type);

    for (size_t i = 0; i < type->property_count(); i++) {
    }
    std::vector<llvm::Type *> member_types;
    for (size_t i = 0; i < type->property_count(); i++) {
        member_types.push_back(get_llvm_type(type->get_property_type(i), cmp_unit));
    }
    llvm_struct_type->setBody(member_types);

    return llvm_struct_type;
}

void TypeLowering::build_function_maps(const AST::Bundle &bundle)
{
    for (auto &cmp_unit : _ctx.cmp_units) {
        // first build all functions actually declared in the module
        for (auto fncdecl : cmp_unit->ast_module->nodes.of_type<AST::FunctionDeclNode>()) {
            // Skip generic function templates during function map building
            if (fncdecl->is_generic()) {
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

            // Skip generic function templates
            if (decl->is_generic()) {
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
        for(auto &struct_decl : cmp_unit->ast_module->nodes.of_type<AST::StructDeclNode>()) {
            // a generic struct template has type-parameter-typed properties and no concrete
            // layout; only its instantiations (Box<int>) are lowered, lazily in get_llvm_type.
            if (struct_decl->is_generic()) {
                continue;
            }
            create_llvm_struct_decl(struct_decl, *cmp_unit);
        }
    }
}

llvm::Type *TypeLowering::get_llvm_type(const AST::ValueType &type, const Compiler::LLVM::CmpUnit &cmp_unit)
{
    llvm::Type* base_type = nullptr;

    if (type.is_primitive()) {
        base_type = get_llvm_type(type.get_primitive_type());
    }
    else if (type.is_struct()) {
        auto *complex = type.get_complex_type();
        auto struct_id = cmp_unit.structure_table->get_structure_id(complex);

        // a generic struct instantiation (Box<int>) has no StructDeclNode, so it is not built
        // during build_struct_maps; lower it lazily the first time it is needed here.
        if (!struct_id && complex && complex->is_instantiated()) {
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

    // If the ValueType has the pointer flag set, wrap it in a pointer type
    if (type.is_pointer()) {
        base_type = llvm::PointerType::get(base_type, 0);
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
        case AST::ValueTypePrimitive::t_bool:
            return llvm::Type::getInt1Ty(*_ctx.llvm_context);
        default:
            throw _ctx.error(fmt::format(
                "Unsupported primitive type '{}' {}",
                AST::get_primitive_name(type), _ctx.function_context()));
    }
}
}
