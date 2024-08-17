#include "Compiler/LLVM/LLVMCompiler.h"
#include "Compiler/CompilerException.h"

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/Program.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Utils.h>
#include <llvm/Linker/Linker.h>

#include "AST/VarDeclNode.h"
#include "AST/VarRefNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/VarMemberNode.h"
#include "AST/VarNode.h"
#include "AST/StructNode.h"
#include "AST/MemberMutNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/ExprNode.h"
#include "AST/TypeCastNode.h"
#include "AST/ReturnNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/VarMutNode.h"
#include "AST/StructNode.h"

#include <fmt/core.h>

#include <iostream>

LLVMCompiler::LLVMCompiler()
{

}

LLVMCompiler::~LLVMCompiler()
{
}

Compiler::LLVM::CmpUnit *LLVMCompiler::get_main_cmpu()
{
    if (_cmp_unit_map.find(ECO_MAIN_MODULE_NAME) == _cmp_unit_map.end()) {
        return nullptr;
    }

    return _cmp_unit_map[ECO_MAIN_MODULE_NAME];
}

std::string LLVMCompiler::get_llvm_err_str()
{
    std::string error;
    llvm::raw_string_ostream error_stream(error);
    llvm::errs().write(error_stream.str().data(), error_stream.str().size());
    return error;
}

void LLVMCompiler::create_cmp_units(const AST::Bundle &bundle)
{
    for (auto &module : bundle.modules) 
    {
        // check if the module is already in the map which is not allowed
        if (_cmp_unit_map.find(module->name) != _cmp_unit_map.end()) {
            throw Compiler::InternalCompilerException(fmt::format(
                "A module named '{}' already exists, all module names of a bundle must be unique.",
                module->name
            ));
        }

        // create a new cmp unit for the module
        _cmp_units.emplace_back(std::make_unique<Compiler::LLVM::CmpUnit>());
        auto &cmp_unit = _cmp_units.back();
        cmp_unit->ast_module = module.get();
        cmp_unit->llvm_module = std::make_unique<llvm::Module>(module->name, *llvm_context);

        _cmp_unit_map[module->name] = cmp_unit.get();
    }

    // ensure none of the modules are null
    for (auto &cmp_unit : _cmp_units) {
        if (cmp_unit->llvm_module == nullptr) {
            throw Compiler::InternalCompilerException(fmt::format(
                "Compiler failed to create a module for '{}', error: {}",
                cmp_unit->ast_module->name,
                get_llvm_err_str()
            ));
        }
    }
}

llvm::Intrinsic::IndependentIntrinsics get_intrinsic_for_string(const std::string &name)
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

llvm::Function *LLVMCompiler::create_llvm_func_decl(const AST::FunctionDeclNode *node, Compiler::LLVM::CmpUnit &cmp_unit)
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

llvm::StructType *LLVMCompiler::create_llvm_struct_decl(const AST::StructDeclNode *node, Compiler::LLVM::CmpUnit &cmp_unit)
{
    if (!node->name_token.has_value()) {
        assert(false);
        throw make_internal_compiler_error("Anonymous struct declarations are not yet supported.");
    }

    auto struct_name = node->struct_name();
    // if (_current_cmp_unit->struct_table.is_defined(struct_name)) {
    //     assert(false);
    //     throw make_internal_compiler_error(fmt::format(
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
            throw make_internal_compiler_error(fmt::format(
                "Unknown type for field '{}' in struct '{}'.", 
                prop->name(), struct_name
            ));
        }
        member_types.push_back(llvm_type);
    }

    // define the llvm struct type
    llvm::StructType *llvm_struct_type = llvm::StructType::create(*llvm_context, member_types, struct_name);

    // store the struct in the struct table
    cmp_unit.structure_table->push_structure(node, llvm_struct_type);

    return llvm_struct_type;
}

llvm::StructType *LLVMCompiler::create_llvm_struct_for_instance(const AST::ComplexType *type, const Compiler::LLVM::CmpUnit &cmp_unit)
{
    std::string struct_name = type->name.value_or("anon");

    // create the struct opaque first and register it, so a self-referential instantiation
    // (a property that mentions the same instantiation) resolves to this in-progress type.
    llvm::StructType *llvm_struct_type = llvm::StructType::create(*llvm_context, struct_name);
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

void LLVMCompiler::build_function_maps(const AST::Bundle &bundle)
{
    for (auto &cmp_unit : _cmp_units) {
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
    for (auto &cmp_unit : _cmp_units) {
        _current_cmp_unit = cmp_unit.get();
        
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

void LLVMCompiler::build_struct_maps(const AST::Bundle &bundle)
{
    // for now we do dump implementation which just copies all 
    // struct types between all compilation units, this obviosly 
    // should in the future only happen if a compilation unit actually 
    // references the structure
    for (auto &cmp_unit : _cmp_units) {
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

Compiler::InternalCompilerException LLVMCompiler::make_internal_compiler_error(std::string message)
{
    return Compiler::InternalCompilerException(message, _current_file);
}

void LLVMCompiler::compile_bundle(const AST::Bundle &bundle)
{
    llvm_context = std::make_unique<llvm::LLVMContext>();
    llvm_builder = std::make_unique<llvm::IRBuilder<>>(*llvm_context);

    // initialize the compilation units
    create_cmp_units(bundle);

    // build the struct maps
    build_struct_maps(bundle);

    // build the function maps
    build_function_maps(bundle);

    // always declare printf @TODO make this a bit more dynamic..
    for (auto &cmp_unit : _cmp_units) {
        cmp_unit->llvm_module->getOrInsertFunction("printf",
            llvm::FunctionType::get(llvm::IntegerType::getInt32Ty(*llvm_context), llvm::PointerType::get(llvm::Type::getInt8Ty(*llvm_context), 0), true) 
        );
    }

    // fetch and build all structs in the module
    // for (auto &cmpu : _cmp_units) {
    //     _current_cmp_unit = cmpu.get();

    //     for (auto &file : _current_cmp_unit->ast_module->files()) {
    //         _current_file = &file;

    //         for (auto &node : file.root->children) {
    //             if (node.has_type<AST::StructDeclNode>()) {
    //                 auto struct_decl = node.get<AST::StructDeclNode>();
    //                 struct_decl.accept(*this);
    //             }
    //         }
    //     }
    // }

    // fetch all function declarations inside of the module
    for (auto &cmpu : _cmp_units) {
        _current_cmp_unit = cmpu.get();

        for (auto &file : _current_cmp_unit->ast_module->files()) {
            _current_file = &file;

            for (auto &node : file.root->children) {
                if (node.has_type<AST::FunctionDeclNode>()) {
                    auto func_decl = node.get<AST::FunctionDeclNode>();
                    func_decl.accept(*this);
                }
            }
        }
    }

    // search for the main module
    Compiler::LLVM::CmpUnit *main_cmp_unit = get_main_cmpu();   
    if (!main_cmp_unit) {
        throw Compiler::InternalCompilerException("No main module found in the bundle", nullptr);
    }

    llvm::FunctionType *funcType = llvm::FunctionType::get(llvm_builder->getInt32Ty(), false);
    llvm::Function *function = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, "main", main_cmp_unit->llvm_module.get());
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(*llvm_context, "entry", function);
    llvm_builder->SetInsertPoint(entry);

    _current_cmp_unit = main_cmp_unit;

    // visit all nodes in the main module
    for (auto &file : main_cmp_unit->ast_module->files()) {
        _current_file = &file;
        file.root->accept(*this);
    }

    // terminate the function
    llvm_builder->CreateRet(llvm_builder->getInt32(0));

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

    for (auto &cmpu : _cmp_units) {

        // skip the main module
        if (cmpu.get() == main_cmp_unit) {
            continue;
        }

        if (linker.linkInModule(std::move(cmpu->llvm_module))) {
            throw Compiler::InternalCompilerException(fmt::format(
                "Failed to link module '{}'.\n{}", 
                cmpu->ast_module->name,
                get_llvm_err_str()
            ));
        }
        cmpu->llvm_module.reset();
    }

    // optimize the module
    // optimize();
}

void LLVMCompiler::visitScope(AST::ScopeNode &node)
{
    for (auto &child : node.children) {

        // skip function declarations
        if (child.has_type<AST::FunctionDeclNode>()) {
            continue;
        }

        child.node()->accept(*this);

        // after any return statement we need to terminate the block
        if (child.has_type<AST::ReturnNode>()) {
            break;
        }
    }
}

void LLVMCompiler::visitType(AST::TypeNode &node)
{
}

void LLVMCompiler::visitTypeCast(AST::TypeCastNode &node)
{
    // visit the expression
    node.expr->accept(*this);

    // create a new value with the new type
    auto new_type = node.result_type();
    auto old_type = node.expr->result_type();

    auto new_llvm_type = get_llvm_type(new_type.get_primitive_type());

    auto value = value_stack.top();
    value_stack.pop();

    // if the types are identical we don't need to do anything
    if (old_type == new_type) {
        value_stack.push(value);
        return;
    }

    // convert the value to a floating point type
    if (new_type.is_floating_type()) {
        if (old_type.is_integer_type()) {
            if (old_type.is_signed_integer()) {
                value = llvm_builder->CreateSIToFP(value, new_llvm_type);
            } else {
                value = llvm_builder->CreateUIToFP(value, new_llvm_type);
            }
        }
        // cast to another floating point type simply requires an extension or truncation
        else if (old_type.is_floating_type()) {
            if (old_type.get_primitive_type() == AST::ValueTypePrimitive::t_float32) {
                value = llvm_builder->CreateFPExt(value, new_llvm_type);
            } else {
                value = llvm_builder->CreateFPTrunc(value, new_llvm_type);
            }
        }
        // cast to a boolean type
        else if (old_type.is_boolean_type()) {
            value = llvm_builder->CreateUIToFP(value, new_llvm_type);
        }
        else {
            throw std::runtime_error("Unsupported type cast");
        }
    }

    else if (new_type.is_integer_type()) {
        if (old_type.is_floating_type()) {
            if (new_type.is_signed_integer()) {
                value = llvm_builder->CreateFPToSI(value, new_llvm_type);
            } else {
                value = llvm_builder->CreateFPToUI(value, new_llvm_type);
            }
        }
        // cast to another integer type 
        else if (old_type.is_integer_type()) {
            // any int -> signed int
            if (new_type.is_signed_integer()) {
                // uint -> int
                if (old_type.is_same_size(new_type) && old_type.is_unsigned_integer()) {
                    value = llvm_builder->CreateIntCast(value, new_llvm_type, true);
                } 
                // int8 -> int32 (smaller -> larger)
                else if (old_type.will_fit_into(new_type)) {
                    value = llvm_builder->CreateSExt(value, new_llvm_type);
                } 
                // int32 -> int8 (larger -> smaller)
                else {
                    value = llvm_builder->CreateTrunc(value, new_llvm_type);
                }
            } 
            // any int -> unsigned int
            else {
                // int -> uint
                if (old_type.is_same_size(new_type) && old_type.is_signed_integer()) {
                    value = llvm_builder->CreateIntCast(value, new_llvm_type, false);
                } 
                // uint8 -> uint32 (smaller -> larger)
                else if (old_type.will_fit_into(new_type)) {
                    value = llvm_builder->CreateZExt(value, new_llvm_type);
                }
                // uint32 -> uint8 (larger -> smaller)
                else {
                    value = llvm_builder->CreateTrunc(value, new_llvm_type);
                }
            }
        }
        // cast to a boolean type
        else if (old_type.is_boolean_type()) {
            value = llvm_builder->CreateZExt(value, new_llvm_type);
        }
        else {
            throw std::runtime_error("Unsupported type cast");
        }
    }

    else if (new_type.is_boolean_type()) {
        if (old_type.is_integer_type()) {
            value = llvm_builder->CreateICmpNE(value, llvm::ConstantInt::get(*llvm_context, llvm::APInt(1, 0, false)));
        }
        else if (old_type.is_floating_type()) {
            value = llvm_builder->CreateFCmpONE(value, llvm::ConstantFP::get(*llvm_context, llvm::APFloat(0.0)));
        }
        else {
            throw std::runtime_error("Unsupported type cast");
        }
    }

    else {
        throw std::runtime_error("Unsupported type cast");
    }

    // push the new value on the stack
    value_stack.push(value);
}

llvm::Type *LLVMCompiler::get_llvm_type(const AST::ValueType &type, const Compiler::LLVM::CmpUnit &cmp_unit)
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
            throw std::runtime_error("Trying to get a non declared struct in compilation unit");
        }

        base_type = cmp_unit.structure_table->get_structure(struct_id).llvm_struct;
    }
    else if (type.is_type_param()) {
        // a resolved instance never carries a type parameter; reaching here is a compiler bug
        // (a template escaped monomorphization) rather than a user error.
        throw std::runtime_error("Cannot compile generic functions with unresolved type parameters. Generic functions must be instantiated with concrete types before compilation.");
    }
    else {
        throw std::runtime_error("Unsupported type");
    }

    // If the ValueType has the pointer flag set, wrap it in a pointer type
    if (type.is_pointer()) {
        base_type = llvm::PointerType::get(base_type, 0);
    }

    return base_type;
}

llvm::Type *LLVMCompiler::get_llvm_type(const AST::ValueTypePrimitive type)
{
    switch (type) {
        case AST::ValueTypePrimitive::t_void:
            return llvm::Type::getVoidTy(*llvm_context);
        case AST::ValueTypePrimitive::t_float32:
            return llvm::Type::getFloatTy(*llvm_context);
        case AST::ValueTypePrimitive::t_float64:
            return llvm::Type::getDoubleTy(*llvm_context);
        case AST::ValueTypePrimitive::t_int8:
            return llvm::Type::getInt8Ty(*llvm_context);
        case AST::ValueTypePrimitive::t_int16:
            return llvm::Type::getInt16Ty(*llvm_context);
        case AST::ValueTypePrimitive::t_int32:
            return llvm::Type::getInt32Ty(*llvm_context);
        case AST::ValueTypePrimitive::t_int64:
            return llvm::Type::getInt64Ty(*llvm_context);
        case AST::ValueTypePrimitive::t_uint8:
            return llvm::Type::getInt8Ty(*llvm_context);
        case AST::ValueTypePrimitive::t_uint16:
            return llvm::Type::getInt16Ty(*llvm_context);
        case AST::ValueTypePrimitive::t_uint32:
            return llvm::Type::getInt32Ty(*llvm_context);
        case AST::ValueTypePrimitive::t_uint64:
            return llvm::Type::getInt64Ty(*llvm_context);
        case AST::ValueTypePrimitive::t_bool:
            return llvm::Type::getInt1Ty(*llvm_context);
        default:
            throw std::runtime_error("Unsupported variable type");
    }
}

void LLVMCompiler::visitVarDecl(AST::VarDeclNode &node)
{
    auto varname = node.name();
    llvm::Type* type = get_llvm_type(node.type_node()->type, *_current_cmp_unit);

    // alloc the variable on the stack
    llvm::AllocaInst* alloca = llvm_builder->CreateAlloca(type, nullptr, varname);

    // store the variable in the map
    var_map[&node] = alloca;

    if (node.init_expr) {
        node.init_expr->accept(*this);

        // check that the visited node pushed a value on the stack
        assert(value_stack.size() > 0 && "No value on the stack");

        llvm::Value* init_value = value_stack.top();

        // if the type is a float but our init_value is a double we need to convert it
        if (type->isFloatTy() && init_value->getType()->isDoubleTy()) {
            init_value = llvm_builder->CreateFPTrunc(init_value, type);
        }
        else if (type->isDoubleTy() && init_value->getType()->isFloatTy()) {
            init_value = llvm_builder->CreateFPExt(init_value, type);
        }

        llvm_builder->CreateStore(init_value, alloca);
        value_stack.pop();
    }
}

void LLVMCompiler::visitVarRef(AST::VarRefNode &node)
{
    if (node.is_var()) {
        // Handle regular variable reference
        auto &var_node = node.get_var();
        
        // Get the LLVM value for this variable (should be an alloca instruction)
        auto it = var_map.find(&var_node.decl());
        if (it == var_map.end()) {
            throw make_internal_compiler_error(fmt::format(
                "Variable '{}' not found in variable map", var_node.decl().name()));
        }
        
        llvm::Value *var_ptr = it->second;
        
        // Check if this is a pointer variable using ValueType.is_pointer()
        if (var_node.decl().type_node()->type.is_pointer()) {
            // For pointer variables, load the pointer first, then load the value it points to
            llvm::Type *pointer_type = get_llvm_type(var_node.decl().type_node()->type, *_current_cmp_unit);
            llvm::Value *pointer_value = llvm_builder->CreateLoad(pointer_type, var_ptr, var_node.decl().name() + "_ptr");
            
            // Get the target type (what the pointer points to)
            AST::ValueType target_type = var_node.decl().type_node()->type;
            target_type.set_pointer(false); // Remove pointer flag to get target type
            llvm::Type *target_llvm_type = get_llvm_type(target_type, *_current_cmp_unit);
            
            // Dereference the pointer to get the actual value
            llvm::Value *dereferenced_value = llvm_builder->CreateLoad(target_llvm_type, pointer_value, var_node.decl().name());
            value_stack.push(dereferenced_value);
        } else {
            // For non-pointer variables, just load the value normally
            llvm::Type *var_type = get_llvm_type(var_node.decl().type_node()->type, *_current_cmp_unit);
            llvm::Value *loaded_value = llvm_builder->CreateLoad(var_type, var_ptr, var_node.decl().name());
            value_stack.push(loaded_value);
        }
    }
    else if (node.is_varmember()) {
        // Handle struct member reference
        auto &var_member_node = node.get_varmember();
        
        // Visit the var member node to get the pointer to the member
        var_member_node.accept(*this);
        
        if (value_stack.empty()) {
            throw make_internal_compiler_error("No member pointer on stack");
        }
        
        llvm::Value *member_ptr = value_stack.top();
        value_stack.pop();
        
        // Get the member type and load its value
        auto &property = var_member_node.property();
        llvm::Type *member_type = get_llvm_type(property.type, *_current_cmp_unit);
        
        llvm::Value *member_value = llvm_builder->CreateLoad(member_type, member_ptr, property.name);
        value_stack.push(member_value);
    }
    else {
        throw make_internal_compiler_error("Unknown VarRef target type");
    }
}

void LLVMCompiler::visitLiteralFloatExpr(AST::LiteralFloatExprNode &node)
{
    if (node.get_effective_primitive_type() == AST::ValueTypePrimitive::t_float64) {
        value_stack.push(llvm::ConstantFP::get(*llvm_context, llvm::APFloat(node.double_value())));
    } else {
        value_stack.push(llvm::ConstantFP::get(*llvm_context, llvm::APFloat(node.float_value())));
    }
}

void LLVMCompiler::visitLiteralIntExpr(AST::LiteralIntExprNode &node)
{
    auto type = node.result_type().get_primitive_type();
    auto value = node.uint64_value();

    auto int_size = AST::get_integer_size(type);

    // push an integer constant on the stack
    value_stack.push(llvm::ConstantInt::get(*llvm_context, llvm::APInt(int_size.size * 8, value, int_size.is_signed)));
}

void LLVMCompiler::visitLiteralBoolExpr(AST::LiteralBoolExprNode &node)
{
    if (node.get_bool_value()) {
        value_stack.push(llvm::ConstantInt::getTrue(*llvm_context));
    } else {
        value_stack.push(llvm::ConstantInt::getFalse(*llvm_context));
    }
}

void LLVMCompiler::visitLiteralStringExpr(AST::LiteralStringExprNode &node)
{
}

void LLVMCompiler::visitBinaryExpr(AST::BinaryExprNode &node)
{
    node.lhs->accept(*this);
    node.rhs->accept(*this);

    auto lhsret = node.lhs->result_type();
    auto rhsret = node.rhs->result_type();

    auto right = value_stack.top();
    value_stack.pop();
    auto left = value_stack.top();
    value_stack.pop();

    if (lhsret.is_integer_type() && rhsret.is_integer_type()) 
    {
        switch (node.op_node->op->type) {
            case Token::Type::t_op_add:
                value_stack.push(llvm_builder->CreateAdd(left, right));
                break;
            case Token::Type::t_op_sub:
                value_stack.push(llvm_builder->CreateSub(left, right));
                break;
            case Token::Type::t_op_mul:
                value_stack.push(llvm_builder->CreateMul(left, right));
                break;
            case Token::Type::t_op_div:
                value_stack.push(llvm_builder->CreateSDiv(left, right));
                break;
            case Token::Type::t_op_mod:
                value_stack.push(llvm_builder->CreateSRem(left, right));
                break;
            case Token::Type::t_op_pow:
                {
                    // im kinda just copying the behavior of C with clang here
                    // cast all values to double and then call the pow intrinsic
                    // cast the result back to the original type
                    std::vector<llvm::Type *> arg_type;
                    arg_type.push_back(llvm::Type::getDoubleTy(*llvm_context));
                    arg_type.push_back(llvm::Type::getDoubleTy(*llvm_context));

                    llvm::Function *fun = llvm::Intrinsic::getDeclaration(curr_llvm_module(), llvm::Intrinsic::pow, arg_type);
                    std::vector<llvm::Value *> args;
                    args.push_back(llvm_builder->CreateSIToFP(left, llvm::Type::getDoubleTy(*llvm_context)));
                    args.push_back(llvm_builder->CreateSIToFP(right, llvm::Type::getDoubleTy(*llvm_context)));

                    llvm::Value *result = llvm_builder->CreateCall(fun, args);
                    value_stack.push(llvm_builder->CreateFPToSI(result, llvm::Type::getInt32Ty(*llvm_context)));
                }
                break;
            case Token::Type::t_logical_eq:
                value_stack.push(llvm_builder->CreateICmpEQ(left, right));
                break;
            case Token::Type::t_logical_neq:
                value_stack.push(llvm_builder->CreateICmpNE(left, right));
                break;
            case Token::Type::t_close_angle:
                value_stack.push(llvm_builder->CreateICmpSGT(left, right));
                break;
            case Token::Type::t_open_angle:
                value_stack.push(llvm_builder->CreateICmpSLT(left, right));
                break;
            case Token::Type::t_logical_geq:
                value_stack.push(llvm_builder->CreateICmpSGE(left, right));
                break;
            case Token::Type::t_logical_leq:
                value_stack.push(llvm_builder->CreateICmpSLE(left, right));
                break;
            default:
                throw std::runtime_error("Unsupported binary operator");
        }
    }
    else if (lhsret.is_boolean_type() && rhsret.is_boolean_type()) 
    {
        switch (node.op_node->op->type) {
            case Token::Type::t_logical_and:
                value_stack.push(llvm_builder->CreateAnd(left, right));
                break;
            case Token::Type::t_logical_or:
                value_stack.push(llvm_builder->CreateOr(left, right));
                break;
            default:
                throw std::runtime_error("Unsupported binary operator");
        }
    }
    else if (lhsret.is_floating_type() || rhsret.is_floating_type())
    {
        // Promote both sides to a common floating type (double if any operand is double)
        bool use_double = lhsret.is_primitive_of_type(AST::ValueTypePrimitive::t_float64) ||
                          rhsret.is_primitive_of_type(AST::ValueTypePrimitive::t_float64);

        auto promote_to_fp = [&](llvm::Value *value, const AST::ValueType &vt) {
            if (vt.is_floating_type()) {
                if (use_double && vt.is_primitive_of_type(AST::ValueTypePrimitive::t_float32)) {
                    return llvm_builder->CreateFPExt(value, llvm::Type::getDoubleTy(*llvm_context));
                }
                return value;
            }

            // integer/boolean -> float/double
            if (use_double) {
                return llvm_builder->CreateSIToFP(value, llvm::Type::getDoubleTy(*llvm_context));
            }
            return llvm_builder->CreateSIToFP(value, llvm::Type::getFloatTy(*llvm_context));
        };

        left = promote_to_fp(left, lhsret);
        right = promote_to_fp(right, rhsret);

        switch (node.op_node->op->type) {
            case Token::Type::t_op_add:
                value_stack.push(llvm_builder->CreateFAdd(left, right));
                break;
            case Token::Type::t_op_sub:
                value_stack.push(llvm_builder->CreateFSub(left, right));
                break;
            case Token::Type::t_op_mul:
                value_stack.push(llvm_builder->CreateFMul(left, right));
                break;
            case Token::Type::t_op_div:
                value_stack.push(llvm_builder->CreateFDiv(left, right));
                break;
            case Token::Type::t_op_mod:
                value_stack.push(llvm_builder->CreateFRem(left, right));
                break;
            case Token::Type::t_logical_eq:
                value_stack.push(llvm_builder->CreateFCmpOEQ(left, right));
                break;
            case Token::Type::t_logical_neq:
                value_stack.push(llvm_builder->CreateFCmpONE(left, right));
                break;
            case Token::Type::t_close_angle:
                value_stack.push(llvm_builder->CreateFCmpOGT(left, right));
                break;
            case Token::Type::t_open_angle:
                value_stack.push(llvm_builder->CreateFCmpOLT(left, right));
                break;
            case Token::Type::t_logical_geq:
                value_stack.push(llvm_builder->CreateFCmpOGE(left, right));
                break;
            case Token::Type::t_logical_leq:
                value_stack.push(llvm_builder->CreateFCmpOLE(left, right));
                break;

            
            default:
                throw std::runtime_error("Unsupported binary operator");
        }
    }
    else {
        throw std::runtime_error("Unsupported binary operator");
    }
}

void LLVMCompiler::visitUnaryExpr(AST::UnaryExprNode &node)
{
}

void LLVMCompiler::visitFunctionCallExpr(AST::FunctionCallExprNode &node)
{
    if (node.token_function_name.value() == "echo") {

        for (auto &arg : node.arguments) {
            arg->accept(*this);

            auto arg_value = value_stack.top();
            value_stack.pop();

            auto result_type = arg->result_type();

            // printf each argument value
            std::vector<llvm::Value *> ArgsV;


            if (
                result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_int8) || 
                result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_int16) ||
                result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_int32)
            ) {
                ArgsV.push_back(llvm_builder->CreateGlobalStringPtr("%d\n"));
                ArgsV.push_back(arg_value);
            }
            else if (result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_int64)) {
                ArgsV.push_back(llvm_builder->CreateGlobalStringPtr("%lld\n"));
                ArgsV.push_back(arg_value);
            }
            else if (
                result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_uint8) || 
                result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_uint16) ||
                result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_uint32)
            ) {
                ArgsV.push_back(llvm_builder->CreateGlobalStringPtr("%u\n"));
                ArgsV.push_back(arg_value);
            }
            else if (result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_uint64)) {
                ArgsV.push_back(llvm_builder->CreateGlobalStringPtr("%llu\n"));
                ArgsV.push_back(arg_value);
            }
            else if (result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_float32)) {
                ArgsV.push_back(llvm_builder->CreateGlobalStringPtr("%f\n"));
                ArgsV.push_back(arg_value);
            }
            else if (result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_float64)) {
                ArgsV.push_back(llvm_builder->CreateGlobalStringPtr("%f\n"));
                ArgsV.push_back(arg_value);
            }
            else if (result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_bool)) {
                ArgsV.push_back(llvm_builder->CreateGlobalStringPtr("%d\n"));
                ArgsV.push_back(arg_value);
            }
            else {
                throw std::runtime_error("Unsupported argument type for 'echo'");
            }

            llvm_builder->CreateCall(curr_llvm_module()->getFunction("printf"), ArgsV);
        }
    }

    else 
    {
        // locate the function 
        auto funcid = _current_cmp_unit->function_table.get_function_id(node.decl);
        llvm::Function *func = _current_cmp_unit->function_table.get_llvm_function(funcid);

        // look for the function in the other modules
        if (!func) {
            for (auto &cmp_unit : _cmp_units) {
                if (cmp_unit.get() == _current_cmp_unit) {
                    continue;
                }

                auto funcid = cmp_unit->function_table.get_function_id(node.decl);
                func = cmp_unit->function_table.get_llvm_function(funcid);

                if (func) {
                    break;
                }
            }
        }

        if (!func) {
            throw std::runtime_error("Function not found");
        }

        std::vector<llvm::Value *> args;
        for (size_t i = 0; i < node.arguments.size(); ++i) {
            auto &arg = node.arguments[i];
            auto &param = node.decl->args[i];

            // Check if the parameter expects a pointer/reference
            if (param->type_node()->type.is_pointer()) {
                // For pointer parameters, we need to pass the address
                // Check if the argument is a variable reference that we can get the address of
                if (auto var_ref_node = dynamic_cast<AST::VarRefNode*>(arg)) {
                    if (var_ref_node->is_var()) {
                        // Get the pointer (alloca) to the variable from the variable map
                        auto &var_node = var_ref_node->get_var();
                        auto it = var_map.find(&var_node.decl());
                        if (it == var_map.end()) {
                            throw make_internal_compiler_error(fmt::format(
                                "Variable '{}' not found in variable map", var_node.decl().name()));
                        }
                        args.push_back(it->second);
                    } else {
                        // For other VarRef types (like member access), evaluate and get address
                        arg->accept(*this);
                        args.push_back(value_stack.top());
                        value_stack.pop();
                    }
                } else {
                    // For other expressions, evaluate them and then get address
                    // This might need more sophisticated handling
                    arg->accept(*this);
                    args.push_back(value_stack.top());
                    value_stack.pop();
                }
            } else {
                // For value parameters, evaluate normally
                arg->accept(*this);
                args.push_back(value_stack.top());
                value_stack.pop();
            }
        }

        llvm::Value *ret = llvm_builder->CreateCall(func, args);
        value_stack.push(ret);
    
    }
}

void LLVMCompiler::visitVarPtrExpr(AST::VarPtrExprNode &node)
{
    // Get a pointer to the variable referenced by var_ref
    // We need to handle different types of variable references
    
    if (node.var_ref->is_var()) {
        // Handle regular variable reference - get the pointer (alloca)
        auto &var_node = node.var_ref->get_var();
        
        // Get the LLVM alloca instruction for this variable
        auto it = var_map.find(&var_node.decl());
        if (it == var_map.end()) {
            throw make_internal_compiler_error(fmt::format(
                "Variable '{}' not found in variable map", var_node.decl().name()));
        }
        
        // Push the alloca instruction (pointer to the variable) onto the stack
        value_stack.push(it->second);
    }
    else if (node.var_ref->is_varmember()) {
        // Handle struct member reference - get pointer to the member
        auto &var_member_node = node.var_ref->get_varmember();
        
        // Visit the var member node to get the pointer to the member
        var_member_node.accept(*this);
        
        if (value_stack.empty()) {
            throw make_internal_compiler_error("No member pointer on stack");
        }
        
        // The member pointer is already on the stack, no need to load the value
        // since we want the pointer, not the value
    }
    else {
        throw make_internal_compiler_error("Unknown VarRef target type in VarPtrExpr");
    }
}

void LLVMCompiler::visitNull(AST::NullNode &node)
{
}

void LLVMCompiler::visitOperator(AST::OperatorNode &node)
{
}

void LLVMCompiler::visitFunctionDecl(AST::FunctionDeclNode &node)
{
    // Skip compilation of generic function templates
    if (node.is_generic()) {
        return;
    }
    
    // sanity checks 

    // 1. must have a body
    if (!node.body) {
        // if its an intrinsic function we can skip this
        if (node.intrinsic) {
            return;
        }
        
        // Skip instantiated generic functions that don't have bodies yet
        // This is a temporary measure while we implement proper body cloning
        if (!node.is_generic() && node.type_parameters.empty()) {
            // This is likely an instantiated generic function without a body
            // Skip compilation for now
            return;
        }

        assert(false);
        throw make_internal_compiler_error(fmt::format(
            "Function '{}' has no body associated with it.", 
            node.func_name()
        ));
    }

    // dump all function names in map
    auto funcid = _current_cmp_unit->function_table.get_function_id_by_name(AST::mangle_function_name(&node));
    auto func = _current_cmp_unit->function_table.get_llvm_function(funcid);

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(*llvm_context, "entry", func);
    llvm_builder->SetInsertPoint(entry);

    // create the arguments
    for (auto &arg : func->args()) {
        arg.setName(node.args[arg.getArgNo()]->name());
        llvm::AllocaInst *alloca = llvm_builder->CreateAlloca(arg.getType(), nullptr, arg.getName());
        llvm_builder->CreateStore(&arg, alloca);
        var_map[node.args[arg.getArgNo()]] = alloca;
    }

    // Auto-synthesized struct constructor only when there is no user-provided body
    bool is_struct_constructor = false;
    llvm::StructType *struct_type = nullptr;
    
    if (node.return_type && node.return_type->type.is_struct()) {
        struct_type = llvm::dyn_cast<llvm::StructType>(func->getReturnType());
        is_struct_constructor = (struct_type != nullptr && node.args.size() > 0 && node.body == nullptr);
    }

    if (is_struct_constructor) {
        // Generate struct constructor body
        // Allocate the struct on the stack
        llvm::AllocaInst *struct_alloca = llvm_builder->CreateAlloca(struct_type, nullptr, "result");
        
        // Initialize struct fields with the constructor arguments
        for (size_t i = 0; i < node.args.size(); ++i) {
            // Get the argument variable 
            auto arg_var = var_map[node.args[i]];
            llvm::Value *arg_value = llvm_builder->CreateLoad(arg_var->getAllocatedType(), arg_var);
            
            // Get pointer to the struct field
            std::vector<llvm::Value*> indices = {
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvm_context), 0),
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvm_context), i)
            };
            llvm::Value *field_ptr = llvm_builder->CreateGEP(struct_type, struct_alloca, indices);
            
            // Store the argument value in the field
            llvm_builder->CreateStore(arg_value, field_ptr);
        }
        
        // Load the struct and return it
        llvm::Value *struct_value = llvm_builder->CreateLoad(struct_type, struct_alloca);
        llvm_builder->CreateRet(struct_value);
    } else {
        // visit the function body for normal functions (including custom constructors)
        node.body->accept(*this);
        
        // Add a terminator if the block doesn't already have one
        if (!llvm_builder->GetInsertBlock()->getTerminator()) {
            // If the function returns void, add a void return
            if (func->getReturnType()->isVoidTy()) {
                llvm_builder->CreateRetVoid();
            } else {
                // For non-void functions without explicit return, this is an error
                // but we'll add a dummy return to keep LLVM happy
                llvm::Value *dummy_ret = llvm::UndefValue::get(func->getReturnType());
                llvm_builder->CreateRet(dummy_ret);
            }
        }
    }
}

void LLVMCompiler::visitReturn(AST::ReturnNode &node)
{
    // handle returns without an actual extression
    if (node.expr == nullptr) {
        llvm_builder->CreateRetVoid();
        return;
    }

    // Always evaluate the return expression during compilation
    // The stored result_type() may be void for generic expressions,
    // but during LLVM compilation the expression will be properly typed
    node.expr->accept(*this);    

    // Check if we actually got a value on the stack
    if (value_stack.empty()) {
        llvm_builder->CreateRetVoid();
        return;
    }

    llvm::Value *ret = value_stack.top();
    value_stack.pop();

    llvm_builder->CreateRet(ret);
}

void LLVMCompiler::visitIfStatement(AST::IfStatementNode &node)
{
    llvm::BasicBlock *if_block = llvm::BasicBlock::Create(*llvm_context, "if", llvm_builder->GetInsertBlock()->getParent());
    llvm::BasicBlock *merge_block = nullptr;

    // condition
    node.condition->accept(*this);
    llvm::Value *condition = value_stack.top();

    // if there is no else block we directly jump to the merge block
    if (!node.else_scope) {
        merge_block = llvm::BasicBlock::Create(*llvm_context, "merge", llvm_builder->GetInsertBlock()->getParent());
        llvm_builder->CreateCondBr(condition, if_block, merge_block);

        // if block
        llvm_builder->SetInsertPoint(if_block);
        node.if_scope->accept(*this);

        // if last instruction is not a terminator we need to add a branch to the merge block
        if (!llvm_builder->GetInsertBlock()->getTerminator()) {
            llvm_builder->CreateBr(merge_block);
        }

        // llvm_builder->CreateBr(merge_block);
    } else {
        llvm::BasicBlock *else_block = llvm::BasicBlock::Create(*llvm_context, "else", llvm_builder->GetInsertBlock()->getParent());
        
        llvm_builder->CreateCondBr(condition, if_block, else_block);

        // if block
        llvm_builder->SetInsertPoint(if_block);
        node.if_scope->accept(*this);
        // llvm_builder->CreateBr(merge_block);

        if (!llvm_builder->GetInsertBlock()->getTerminator()) {
            merge_block = llvm::BasicBlock::Create(*llvm_context, "merge", llvm_builder->GetInsertBlock()->getParent());
            llvm_builder->CreateBr(merge_block);
        }

        // else block
        llvm_builder->SetInsertPoint(else_block);
        node.else_scope->accept(*this);
        // llvm_builder->CreateBr(merge_block);

        if (!llvm_builder->GetInsertBlock()->getTerminator()) {
            if (!merge_block) {
                merge_block = llvm::BasicBlock::Create(*llvm_context, "merge", llvm_builder->GetInsertBlock()->getParent());
            }
            llvm_builder->CreateBr(merge_block);
        }
    }

    if (merge_block) {
        llvm_builder->SetInsertPoint(merge_block);
    }
}

void LLVMCompiler::visitWhileStatement(AST::WhileStatementNode &node)
{
    llvm::BasicBlock *loop_block = llvm::BasicBlock::Create(*llvm_context, "loop", llvm_builder->GetInsertBlock()->getParent());
    llvm::BasicBlock *body_block = llvm::BasicBlock::Create(*llvm_context, "body", llvm_builder->GetInsertBlock()->getParent());
    llvm::BasicBlock *merge_block = llvm::BasicBlock::Create(*llvm_context, "merge", llvm_builder->GetInsertBlock()->getParent());

    llvm_builder->CreateBr(loop_block);

    // loop block
    llvm_builder->SetInsertPoint(loop_block);
    node.condition->accept(*this);
    llvm::Value *condition = value_stack.top();
    value_stack.pop();

    llvm_builder->CreateCondBr(condition, body_block, merge_block);

    // body block
    llvm_builder->SetInsertPoint(body_block);
    node.loop_scope->accept(*this);
    llvm_builder->CreateBr(loop_block);

    // merge block
    llvm_builder->SetInsertPoint(merge_block);
}

void LLVMCompiler::visitVarMut(AST::VarMutNode &node)
{
    // Visit the value expression to get its LLVM IR value
    node.value_expr->accept(*this);
    
    // Get the value from the stack
    llvm::Value* new_value = value_stack.top();
    value_stack.pop();
    
    // Find the variable declaration
    if (node.var_decl == nullptr) {
        throw std::runtime_error("Variable declaration not found for mutation");
    }
    
    // Get the allocated variable from the map
    auto var_iter = var_map.find(node.var_decl);
    if (var_iter == var_map.end()) {
        throw std::runtime_error("Variable not found in the map");
    }
    
    llvm::AllocaInst* var = var_iter->second;
    llvm::Value* target = var;

    // Check if it's a pointer using ValueType.is_pointer()
    if (node.var_decl->type_node()->type.is_pointer()) {
        // For pointer variables, load the pointer first, then store through it
        target = llvm_builder->CreateLoad(var->getAllocatedType(), var);
        
        // Get the target type (what the pointer points to) for type casting
        AST::ValueType target_type = node.var_decl->type_node()->type;
        target_type.set_pointer(false); // Remove pointer flag to get target type
        llvm::Type *target_llvm_type = get_llvm_type(target_type, *_current_cmp_unit);
        
        // Cast the new value to the target type if necessary
        if (target_llvm_type->isFloatTy() && new_value->getType()->isDoubleTy()) {
            new_value = llvm_builder->CreateFPTrunc(new_value, target_llvm_type);
        } else if (target_llvm_type->isDoubleTy() && new_value->getType()->isFloatTy()) {
            new_value = llvm_builder->CreateFPExt(new_value, target_llvm_type);
        } else if (target_llvm_type->isIntegerTy() && new_value->getType()->isFloatingPointTy()) {
            new_value = llvm_builder->CreateFPToSI(new_value, target_llvm_type);
        } else if (target_llvm_type->isFloatingPointTy() && new_value->getType()->isIntegerTy()) {
            new_value = llvm_builder->CreateSIToFP(new_value, target_llvm_type);
        } else if (target_llvm_type->isIntegerTy() && new_value->getType()->isIntegerTy() && 
                   target_llvm_type->getIntegerBitWidth() != new_value->getType()->getIntegerBitWidth()) {
            if (target_llvm_type->getIntegerBitWidth() > new_value->getType()->getIntegerBitWidth()) {
                new_value = llvm_builder->CreateSExt(new_value, target_llvm_type);
            } else {
                new_value = llvm_builder->CreateTrunc(new_value, target_llvm_type);
            }
        }
    } else {
        // For non-pointer variables, cast to the variable type
        llvm::Type* var_type = var->getAllocatedType();
        
        if (var_type->isFloatTy() && new_value->getType()->isDoubleTy()) {
            new_value = llvm_builder->CreateFPTrunc(new_value, var_type);
        } else if (var_type->isDoubleTy() && new_value->getType()->isFloatTy()) {
            new_value = llvm_builder->CreateFPExt(new_value, var_type);
        } else if (var_type->isIntegerTy() && new_value->getType()->isFloatingPointTy()) {
            new_value = llvm_builder->CreateFPToSI(new_value, var_type);
        } else if (var_type->isFloatingPointTy() && new_value->getType()->isIntegerTy()) {
            new_value = llvm_builder->CreateSIToFP(new_value, var_type);
        } else if (var_type->isIntegerTy() && new_value->getType()->isIntegerTy() && 
                   var_type->getIntegerBitWidth() != new_value->getType()->getIntegerBitWidth()) {
            if (var_type->getIntegerBitWidth() > new_value->getType()->getIntegerBitWidth()) {
                new_value = llvm_builder->CreateSExt(new_value, var_type);
            } else {
                new_value = llvm_builder->CreateTrunc(new_value, var_type);
            }
        }
    }
    
    // Store the new value in the target
    llvm_builder->CreateStore(new_value, target);
}

void LLVMCompiler::visitNamespaceDecl(AST::NamespaceDeclNode &node)
{
}

void LLVMCompiler::visitNamespace(AST::NamespaceNode &node)
{
}

void LLVMCompiler::visitAttribute(AST::AttributeNode &node)
{
}

void LLVMCompiler::visitStructDecl(AST::StructDeclNode &node)
{
    // a generic struct template has type-parameter-typed properties and no concrete layout;
    // only its instantiations are lowered (lazily, in get_llvm_type).
    if (node.is_generic()) {
        return;
    }

    if (!node.name_token.has_value()) {
        assert(false);
        throw make_internal_compiler_error("Anonymous struct declarations are not yet supported.");
    }

    auto struct_name = node.struct_name();
    
    // Check if this struct is already defined in the structure table
    if (_current_cmp_unit->structure_table->get_structure_id(&node) != 0) {
        // Already defined, skip
        return;
    }

    // Create an opaque struct type first and register it immediately
    llvm::StructType *llvm_struct_type = llvm::StructType::create(*llvm_context, struct_name);
    _current_cmp_unit->structure_table->push_structure(&node, llvm_struct_type);

    // Now collect member types for LLVM struct (other structs should be resolvable now)
    std::vector<llvm::Type *> member_types;
    for (const auto &prop : node.properties()) {
        llvm::Type *llvm_type = get_llvm_type(prop->type_node()->type, *_current_cmp_unit);
        if (!llvm_type) {
            assert(false);
            throw make_internal_compiler_error(fmt::format(
                "Unknown type for field '{}' in struct '{}'.", 
                prop->name(), struct_name
            ));
        }
        member_types.push_back(llvm_type);
    }

    // Set the body of the struct type
    llvm_struct_type->setBody(member_types);
}

void LLVMCompiler::visitMemberAccess(AST::MemberAccessNode &node)
{
    llvm::Value *base_ptr = nullptr;
    
    // Handle different base node types for member access
    if (node.get_base_node().has_type<AST::VarRefNode>()) {
        auto &var_ref = node.get_base_node().get<AST::VarRefNode>();
        
        if (var_ref.is_var()) {
            // Get the variable node and visit it to get the pointer, not the loaded value
            auto &var_node = var_ref.get_var();
            var_node.accept(*this);
            base_ptr = value_stack.top();
            value_stack.pop();
        } else {
            // For other VarRef types, use normal visit but expect a pointer
            node.get_base_node().node()->accept(*this);
            base_ptr = value_stack.top();
            value_stack.pop();
        }
    } else if (node.get_base_node().has_type<AST::MemberAccessNode>()) {
        // For chained member access, we need to get a pointer to the intermediate struct
        auto &base_member_access = node.get_base_node().get<AST::MemberAccessNode>();
        
        // Get the base for the first member access
        if (base_member_access.get_base_node().has_type<AST::VarRefNode>()) {
            auto &base_var_ref = base_member_access.get_base_node().get<AST::VarRefNode>();
            if (base_var_ref.is_var()) {
                auto &base_var_node = base_var_ref.get_var();
                base_var_node.accept(*this);
                
                // If the base variable is a pointer, we need to load it for GEP operations
                if (base_var_node.decl().type_node()->type.is_pointer()) {
                    llvm::Value *var_alloca = value_stack.top();
                    value_stack.pop();
                    
                    llvm::Type *pointer_type = get_llvm_type(base_var_node.decl().type_node()->type, *_current_cmp_unit);
                    llvm::Value *loaded_pointer = llvm_builder->CreateLoad(pointer_type, var_alloca, base_var_node.decl().name() + "_loaded");
                    value_stack.push(loaded_pointer);
                }
            } else {
                base_member_access.get_base_node().node()->accept(*this);
            }
        } else {
            base_member_access.get_base_node().node()->accept(*this);
        }
        
        if (value_stack.empty()) {
            throw make_internal_compiler_error("No base value on stack for chained member access");
        }
        
        llvm::Value *intermediate_base = value_stack.top();
        value_stack.pop();
        
        // Get the intermediate member pointer (don't load the value)
        auto base_result_type = base_member_access.get_base_node().get<AST::VarRefNode>().result_type();
        if (base_result_type.is_struct() && base_result_type.get_complex_type()) {
            auto complex = base_result_type.get_complex_type();
            auto intermediate_member_name = base_member_access.get_member_name().value();
            
            // Find the intermediate member index
            size_t intermediate_member_index = 0;
            bool found = false;
            for (size_t i = 0; i < complex->property_count(); ++i) {
                auto prop = complex->get_property(i);
                if (prop.name == intermediate_member_name) {
                    intermediate_member_index = i;
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                throw make_internal_compiler_error(fmt::format("Intermediate member '{}' not found in struct", intermediate_member_name));
            }
            
            // Get the struct type from the structure table
            auto struct_id = _current_cmp_unit->structure_table->get_structure_id(base_result_type.get_complex_type());
            if (struct_id == 0) {
                throw make_internal_compiler_error("Intermediate struct not found in structure table");
            }
            
            auto &structure = _current_cmp_unit->structure_table->get_structure(struct_id);
            
            // Create GEP instruction to access the intermediate member
            std::vector<llvm::Value *> indices = {
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvm_context), 0),
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvm_context), intermediate_member_index)
            };
            
            base_ptr = llvm_builder->CreateGEP(
                structure.llvm_struct, intermediate_base, indices, intermediate_member_name + "_ptr");
        } else {
            throw make_internal_compiler_error("Invalid intermediate type for chained member access");
        }
    } else {
        // For other base node types, use normal visit
        node.get_base_node().node()->accept(*this);
        base_ptr = value_stack.top();
        value_stack.pop();
    }
    
    if (!base_ptr) {
        throw make_internal_compiler_error("No base value on stack for member access");
    }
    
    // Get the type information for the final member access
    auto result_type = node.result_type();
    if (result_type.is_void()) {
        throw make_internal_compiler_error("Cannot access member of void type");
    }
    
    // Get the base type (either from the variable or from the intermediate member access)
    AST::ValueType base_type;
    if (node.get_base_node().has_type<AST::VarRefNode>()) {
        base_type = node.get_base_node().get<AST::VarRefNode>().result_type();
    } else if (node.get_base_node().has_type<AST::MemberAccessNode>()) {
        base_type = node.get_base_node().get<AST::MemberAccessNode>().result_type();
    } else {
        throw make_internal_compiler_error("Unsupported base type for member access");
    }
    
    if (base_type.is_struct() && base_type.get_complex_type()) {
        auto complex = base_type.get_complex_type();
        auto member_name = node.get_member_name().value();
        
        // Find the member index
        size_t member_index = 0;
        bool found = false;
        for (size_t i = 0; i < complex->property_count(); ++i) {
            auto prop = complex->get_property(i);
            if (prop.name == member_name) {
                member_index = i;
                found = true;
                break;
            }
        }
        
        if (!found) {
            throw make_internal_compiler_error(fmt::format("Member '{}' not found in struct", member_name));
        }
        
        // Get the struct type from the structure table
        auto struct_id = _current_cmp_unit->structure_table->get_structure_id(base_type.get_complex_type());
        if (struct_id == 0) {
            throw make_internal_compiler_error("Struct not found in structure table");
        }
        
        auto &structure = _current_cmp_unit->structure_table->get_structure(struct_id);
        
        // Check if we need to dereference a pointer to get to the struct
        llvm::Value *struct_ptr = base_ptr;
        
        // For chained member access, we need to trace back to find the root variable
        // and check if it's a pointer that needs dereferencing
        bool needs_pointer_deref = false;
        std::string root_var_name;
        
        // Find the root variable by traversing the member access chain
        const AST::MemberAccessNode *current_access = &node;
        while (current_access) {
            if (current_access->get_base_node().has_type<AST::VarRefNode>()) {
                auto &var_ref = current_access->get_base_node().get<AST::VarRefNode>();
                if (var_ref.is_var()) {
                    auto &var_node = var_ref.get_var();
                    if (var_node.decl().type_node()->type.is_pointer()) {
                        needs_pointer_deref = true;
                        root_var_name = var_node.decl().name();
                    }
                }
                break; // Found the root variable
            } else if (current_access->get_base_node().has_type<AST::MemberAccessNode>()) {
                current_access = &current_access->get_base_node().get<AST::MemberAccessNode>();
            } else {
                break; // Unknown base type
            }
        }
        
        // If we found a pointer variable at the root, and this is the first member access in the chain,
        // we need to dereference it
        if (needs_pointer_deref && node.get_base_node().has_type<AST::VarRefNode>()) {
            // This is direct access to a pointer variable - dereference it
            auto &var_ref = node.get_base_node().get<AST::VarRefNode>();
            auto &var_node = var_ref.get_var();
            llvm::Type *pointer_type = get_llvm_type(var_node.decl().type_node()->type, *_current_cmp_unit);
            struct_ptr = llvm_builder->CreateLoad(pointer_type, base_ptr, root_var_name + "_deref");
        }
        
        // Create GEP instruction to access the final member
        std::vector<llvm::Value *> indices = {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvm_context), 0), // struct pointer
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvm_context), member_index)  // member index
        };
        
        llvm::Value *member_ptr = llvm_builder->CreateGEP(
            structure.llvm_struct, struct_ptr, indices, member_name + "_ptr");
        
        // Load the value from the member
        llvm::Value *member_value = llvm_builder->CreateLoad(
            get_llvm_type(result_type, *_current_cmp_unit), member_ptr, member_name);
        
        value_stack.push(member_value);
        return;
    }
    
    throw make_internal_compiler_error("Unsupported member access pattern");
}

void LLVMCompiler::visitVar(AST::VarNode &node)
{
    // Get the LLVM value for this variable (should be an alloca instruction)
    auto it = var_map.find(&node.decl());
    if (it == var_map.end()) {
        throw make_internal_compiler_error(fmt::format(
            "Variable '{}' not found in variable map", node.decl().name()));
    }
    
    // Push the alloca instruction (variable pointer) onto the stack
    value_stack.push(it->second);
}

void LLVMCompiler::visitVarMember(AST::VarMemberNode &node)
{
    // Get the struct declaration and member property
    auto struct_decl = node.struct_decl();
    if (!struct_decl) {
        throw make_internal_compiler_error("Cannot find struct declaration for member access");
    }
    
    auto &property = node.property();
    
    // Get the base variable reference
    node.get_ref().accept(*this);
    if (value_stack.empty()) {
        throw make_internal_compiler_error("No base variable on stack for member access");
    }
    
    llvm::Value *base_ptr = value_stack.top();
    value_stack.pop();
    
    // Get the struct type from the structure table
    auto struct_id = _current_cmp_unit->structure_table->get_structure_id(struct_decl);
    if (struct_id == 0) {
        throw make_internal_compiler_error("Struct not found in structure table");
    }
    
    auto &structure = _current_cmp_unit->structure_table->get_structure(struct_id);
    
    // Find the member index in the properties
    size_t member_index = 0;
    bool found = false;
    for (size_t i = 0; i < struct_decl->properties().size(); ++i) {
        if (struct_decl->properties()[i]->name() == property.name) {
            member_index = i;
            found = true;
            break;
        }
    }
    
    if (!found) {
        throw make_internal_compiler_error(fmt::format("Member '{}' not found in struct", property.name));
    }
    
    // Create GEP instruction to access the member
    std::vector<llvm::Value *> indices = {
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvm_context), 0), // struct pointer
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvm_context), member_index)  // member index
    };
    
    llvm::Value *member_ptr = llvm_builder->CreateGEP(
        structure.llvm_struct, base_ptr, indices, property.name + "_ptr");
    
    // Push the member pointer onto the stack
    value_stack.push(member_ptr);
}

void LLVMCompiler::visitMemberMut(AST::MemberMutNode &node)
{
    // Visit the value expression first to get the new value
    node.value_expr->accept(*this);
    
    if (value_stack.empty()) {
        throw make_internal_compiler_error("No value on stack for member mutation");
    }
    
    llvm::Value *new_value = value_stack.top();
    value_stack.pop();
    
    // Get the member access node and generate the pointer to the member
    auto &member_access = *node.member_access;
    
    llvm::Value *base_ptr = nullptr;
    
    // Handle different base node types for member access
    if (member_access.get_base_node().has_type<AST::VarRefNode>()) {
        auto &var_ref = member_access.get_base_node().get<AST::VarRefNode>();
        
        if (var_ref.is_var()) {
            // Get the variable node and visit it to get the pointer, not the loaded value
            auto &var_node = var_ref.get_var();
            var_node.accept(*this);
            base_ptr = value_stack.top();
            value_stack.pop();
        } else {
            // For other VarRef types, use normal visit but expect a pointer
            member_access.get_base_node().node()->accept(*this);
            base_ptr = value_stack.top();
            value_stack.pop();
        }
    } else if (member_access.get_base_node().has_type<AST::MemberAccessNode>()) {
        // For chained member access, we need to get a pointer to the intermediate struct
        auto &base_member_access = member_access.get_base_node().get<AST::MemberAccessNode>();
        
        // Get the base for the first member access
        if (base_member_access.get_base_node().has_type<AST::VarRefNode>()) {
            auto &base_var_ref = base_member_access.get_base_node().get<AST::VarRefNode>();
            if (base_var_ref.is_var()) {
                auto &base_var_node = base_var_ref.get_var();
                base_var_node.accept(*this);
                
                // If the base variable is a pointer, we need to load it for GEP operations
                if (base_var_node.decl().type_node()->type.is_pointer()) {
                    llvm::Value *var_alloca = value_stack.top();
                    value_stack.pop();
                    
                    llvm::Type *pointer_type = get_llvm_type(base_var_node.decl().type_node()->type, *_current_cmp_unit);
                    llvm::Value *loaded_pointer = llvm_builder->CreateLoad(pointer_type, var_alloca, base_var_node.decl().name() + "_loaded");
                    value_stack.push(loaded_pointer);
                }
            } else {
                base_member_access.get_base_node().node()->accept(*this);
            }
        } else {
            base_member_access.get_base_node().node()->accept(*this);
        }
        
        if (value_stack.empty()) {
            throw make_internal_compiler_error("No base value on stack for chained member access");
        }
        
        llvm::Value *intermediate_base = value_stack.top();
        value_stack.pop();
        
        // Get the intermediate member pointer (don't load the value)
        auto base_result_type = base_member_access.get_base_node().get<AST::VarRefNode>().result_type();
        if (base_result_type.is_struct() && base_result_type.get_complex_type()) {
            auto complex = base_result_type.get_complex_type();
            auto intermediate_member_name = base_member_access.get_member_name().value();
            
            // Find the intermediate member index
            size_t intermediate_member_index = 0;
            bool found = false;
            for (size_t i = 0; i < complex->property_count(); ++i) {
                auto prop = complex->get_property(i);
                if (prop.name == intermediate_member_name) {
                    intermediate_member_index = i;
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                throw make_internal_compiler_error(fmt::format("Intermediate member '{}' not found in struct", intermediate_member_name));
            }
            
            // Get the struct type from the structure table
            auto struct_id = _current_cmp_unit->structure_table->get_structure_id(base_result_type.get_complex_type());
            if (struct_id == 0) {
                throw make_internal_compiler_error("Intermediate struct not found in structure table");
            }
            
            auto &structure = _current_cmp_unit->structure_table->get_structure(struct_id);
            
            // Create GEP instruction to access the intermediate member
            std::vector<llvm::Value *> indices = {
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvm_context), 0),
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvm_context), intermediate_member_index)
            };
            
            base_ptr = llvm_builder->CreateGEP(
                structure.llvm_struct, intermediate_base, indices, intermediate_member_name + "_ptr");
        } else {
            throw make_internal_compiler_error("Invalid intermediate type for chained member access");
        }
    } else {
        // For other base node types, use normal visit
        member_access.get_base_node().node()->accept(*this);
        base_ptr = value_stack.top();
        value_stack.pop();
    }
    
    if (!base_ptr) {
        throw make_internal_compiler_error("No base value on stack for member mutation");
    }
    
    // Get the base type (either from the variable or from the intermediate member access)
    AST::ValueType base_type;
    if (member_access.get_base_node().has_type<AST::VarRefNode>()) {
        base_type = member_access.get_base_node().get<AST::VarRefNode>().result_type();
    } else if (member_access.get_base_node().has_type<AST::MemberAccessNode>()) {
        base_type = member_access.get_base_node().get<AST::MemberAccessNode>().result_type();
    } else {
        throw make_internal_compiler_error("Unsupported base type for member mutation");
    }
    
    if (base_type.is_struct() && base_type.get_complex_type()) {
        auto complex = base_type.get_complex_type();
        auto member_name = member_access.get_member_name().value();
        
        // Find the member index
        size_t member_index = 0;
        bool found = false;
        for (size_t i = 0; i < complex->property_count(); ++i) {
            auto prop = complex->get_property(i);
            if (prop.name == member_name) {
                member_index = i;
                found = true;
                break;
            }
        }
        
        if (!found) {
            throw make_internal_compiler_error(fmt::format("Member '{}' not found in struct", member_name));
        }
        
        // Get the struct type from the structure table
        auto struct_id = _current_cmp_unit->structure_table->get_structure_id(base_type.get_complex_type());
        if (struct_id == 0) {
            throw make_internal_compiler_error("Struct not found in structure table");
        }
        
        auto &structure = _current_cmp_unit->structure_table->get_structure(struct_id);
        
        // Check if we need to dereference a pointer to get to the struct
        llvm::Value *struct_ptr = base_ptr;
        
        // For chained member access, we need to trace back to find the root variable
        // and check if it's a pointer that needs dereferencing
        bool needs_pointer_deref = false;
        std::string root_var_name;
        
        // Find the root variable by traversing the member access chain
        const AST::MemberAccessNode *current_access = &member_access;
        while (current_access) {
            if (current_access->get_base_node().has_type<AST::VarRefNode>()) {
                auto &var_ref = current_access->get_base_node().get<AST::VarRefNode>();
                if (var_ref.is_var()) {
                    auto &var_node = var_ref.get_var();
                    if (var_node.decl().type_node()->type.is_pointer()) {
                        needs_pointer_deref = true;
                        root_var_name = var_node.decl().name();
                    }
                }
                break; // Found the root variable
            } else if (current_access->get_base_node().has_type<AST::MemberAccessNode>()) {
                current_access = &current_access->get_base_node().get<AST::MemberAccessNode>();
            } else {
                break; // Unknown base type
            }
        }
        
        // If we found a pointer variable at the root, and this is the first member access in the chain,
        // we need to dereference it
        if (needs_pointer_deref && member_access.get_base_node().has_type<AST::VarRefNode>()) {
            // This is direct access to a pointer variable - dereference it
            auto &var_ref = member_access.get_base_node().get<AST::VarRefNode>();
            auto &var_node = var_ref.get_var();
            llvm::Type *pointer_type = get_llvm_type(var_node.decl().type_node()->type, *_current_cmp_unit);
            struct_ptr = llvm_builder->CreateLoad(pointer_type, base_ptr, root_var_name + "_deref");
        }
        
        // Create GEP instruction to access the final member
        std::vector<llvm::Value *> indices = {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvm_context), 0), // struct pointer
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvm_context), member_index)  // member index
        };
        
        llvm::Value *member_ptr = llvm_builder->CreateGEP(
            structure.llvm_struct, struct_ptr, indices, member_name + "_ptr");
        
        // Get the member type for potential type conversion
        auto result_type = member_access.result_type();
        llvm::Type *member_llvm_type = get_llvm_type(result_type, *_current_cmp_unit);
        
        // Cast the new value to the member's type if necessary
        if (member_llvm_type->isFloatTy() && new_value->getType()->isDoubleTy()) {
            new_value = llvm_builder->CreateFPTrunc(new_value, member_llvm_type);
        } else if (member_llvm_type->isDoubleTy() && new_value->getType()->isFloatTy()) {
            new_value = llvm_builder->CreateFPExt(new_value, member_llvm_type);
        } else if (member_llvm_type->isIntegerTy() && new_value->getType()->isFloatingPointTy()) {
            new_value = llvm_builder->CreateFPToSI(new_value, member_llvm_type);
        } else if (member_llvm_type->isFloatingPointTy() && new_value->getType()->isIntegerTy()) {
            new_value = llvm_builder->CreateSIToFP(new_value, member_llvm_type);
        } else if (member_llvm_type->isIntegerTy() && new_value->getType()->isIntegerTy() && 
                   member_llvm_type->getIntegerBitWidth() != new_value->getType()->getIntegerBitWidth()) {
            if (member_llvm_type->getIntegerBitWidth() > new_value->getType()->getIntegerBitWidth()) {
                new_value = llvm_builder->CreateSExt(new_value, member_llvm_type);
            } else {
                new_value = llvm_builder->CreateTrunc(new_value, member_llvm_type);
            }
        }
        
        // Store the new value in the member
        llvm_builder->CreateStore(new_value, member_ptr);
        return;
    }
    
    throw make_internal_compiler_error("Unsupported member mutation pattern");
}

void LLVMCompiler::printIR(bool toFile)
{ 
    auto main = get_main_cmpu();
    main->llvm_module->print(llvm::outs(), nullptr);
}

void LLVMCompiler::run_code() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    auto main_cmp_unit = get_main_cmpu();
    if (!main_cmp_unit) {
        throw Compiler::InternalCompilerException("No main module found to run", nullptr);
    }

    std::string errorStr;
    const llvm::TargetOptions opts;
    llvm::ExecutionEngine *EE = llvm::EngineBuilder(std::move(get_main_cmpu()->llvm_module))
        .setErrorStr(&errorStr)
        .setEngineKind(llvm::EngineKind::JIT)
        .setTargetOptions(opts)
        .create();

    get_main_cmpu()->llvm_module = nullptr;

    if (!EE) {
        llvm::errs() << "Failed to create ExecutionEngine: " << errorStr << '\n';
        return;
    }

    // enable debugging

    EE->finalizeObject();

    auto *func = EE->FindFunctionNamed("main");
    if (!func) {
        llvm::errs() << "Function 'main' not found in module.\n";
        return;
    }

    std::vector<llvm::GenericValue> noargs;
    llvm::GenericValue gv = EE->runFunction(func, noargs);

    delete EE;
    // llvm::llvm_shutdown();
}

void LLVMCompiler::make_exec(std::string executable_name)
{
    // llvm::InitializeAllTargetInfos();
    // llvm::InitializeAllTargets();
    // llvm::InitializeAllTargetMCs();
    // llvm::InitializeAllAsmParsers();
    // llvm::InitializeAllAsmPrinters();
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();



    auto TargetTriple = llvm::sys::getDefaultTargetTriple();
    // auto TargetTriple = "aarch64-linux-gnu";

    std::string Error;
    auto Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);
    if (!Target) {
        llvm::errs() << Error;
        return;
    }

    auto CPU = "generic";
    auto Features = "";

    llvm::TargetOptions opt;
    auto TargetMachine = Target->createTargetMachine(TargetTriple, CPU, Features, opt, llvm::Reloc::PIC_);

    curr_llvm_module()->setDataLayout(TargetMachine->createDataLayout());
    curr_llvm_module()->setTargetTriple(TargetTriple);

    std::error_code EC;
    std::string objectFileName = executable_name + ".o";
    llvm::raw_fd_ostream dest(objectFileName, EC, llvm::sys::fs::OF_None);

    if (EC) {
        llvm::errs() << "Could not open file: " << EC.message();
        return;
    }

    llvm::legacy::PassManager pass;
    auto FileType = llvm::CodeGenFileType::ObjectFile;

    if (TargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
        llvm::errs() << "TargetMachine can't emit a file of this type";
        return;
    }

    pass.run(*curr_llvm_module());
    dest.flush();

    llvm::outs() << "Generated object file: " << objectFileName << "\n";

    std::string command = "clang -o " + executable_name + " " + objectFileName;
    int result = std::system(command.c_str());
    if (result != 0) {
        llvm::errs() << "Error: linking failed\n";
        return;
    }

    llvm::outs() << "Executable \"" << executable_name << "\" created successfully\n";
}

void LLVMCompiler::optimize() {
    if (!curr_llvm_module()) {
        llvm::errs() << "Module is not initialized.\n";
        return;
    }

    llvm::PassBuilder passBuilder;
    llvm::LoopAnalysisManager loopAM;
    llvm::FunctionAnalysisManager functionAM;
    llvm::CGSCCAnalysisManager cgsccAM;
    llvm::ModuleAnalysisManager moduleAM;
    
    passBuilder.registerModuleAnalyses(moduleAM);
    passBuilder.registerCGSCCAnalyses(cgsccAM);
    passBuilder.registerFunctionAnalyses(functionAM);
    passBuilder.registerLoopAnalyses(loopAM);
    passBuilder.crossRegisterProxies(loopAM, functionAM, cgsccAM, moduleAM);

    // make the pipeline
    llvm::ModulePassManager modulePM = passBuilder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    modulePM.addPass(llvm::ModuleInlinerPass(llvm::getInlineParams(3, 0), llvm::InliningAdvisorMode::Default,
                                  llvm::ThinOrFullLTOPhase::None));
    
    modulePM.run(*curr_llvm_module(), moduleAM);
}
