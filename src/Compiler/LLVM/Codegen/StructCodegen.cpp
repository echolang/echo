#include "Compiler/LLVM/Codegen/StructCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

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

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <fmt/core.h>

#include <cassert>
#include <stdexcept>
#include <vector>

namespace Compiler::LLVM
{
void StructCodegen::gen_struct_decl(AST::StructDeclNode &node)
{
    // a generic struct template has type-parameter-typed properties and no concrete layout;
    // only its instantiations are lowered (lazily, in get_llvm_type).
    if (node.is_generic()) {
        return;
    }

    if (!node.name_token.has_value()) {
        assert(false);
        throw _ctx.error("Anonymous struct declarations are not yet supported.");
    }

    auto struct_name = node.struct_name();
    
    // Check if this struct is already defined in the structure table
    if (_ctx.current_cmp_unit->structure_table->get_structure_id(&node) != 0) {
        // Already defined, skip
        return;
    }

    // Create an opaque struct type first and register it immediately
    llvm::StructType *llvm_struct_type = llvm::StructType::create(*_ctx.llvm_context, struct_name);
    _ctx.current_cmp_unit->structure_table->push_structure(&node, llvm_struct_type);

    // Now collect member types for LLVM struct (other structs should be resolvable now)
    std::vector<llvm::Type *> member_types;
    for (const auto &prop : node.properties()) {
        llvm::Type *llvm_type = _ctx.types->get_llvm_type(prop->type_node()->type, *_ctx.current_cmp_unit);
        if (!llvm_type) {
            assert(false);
            throw _ctx.error(fmt::format(
                "Unknown type for field '{}' in struct '{}'.", 
                prop->name(), struct_name
            ));
        }
        member_types.push_back(llvm_type);
    }

    // Set the body of the struct type
    llvm_struct_type->setBody(member_types);
}

void StructCodegen::gen_member_access(AST::MemberAccessNode &node)
{
    llvm::Value *base_ptr = nullptr;
    
    // Handle different base node types for member access
    if (node.get_base_node().has_type<AST::VarRefNode>()) {
        auto &var_ref = node.get_base_node().get<AST::VarRefNode>();
        
        if (var_ref.is_var()) {
            // Get the variable node and visit it to get the pointer, not the loaded value
            auto &var_node = var_ref.get_var();
            var_node.accept(*_ctx.visitor);
            base_ptr = _ctx.value_stack.top();
            _ctx.value_stack.pop();
        } else {
            // For other VarRef types, use normal visit but expect a pointer
            node.get_base_node().node()->accept(*_ctx.visitor);
            base_ptr = _ctx.value_stack.top();
            _ctx.value_stack.pop();
        }
    } else if (node.get_base_node().has_type<AST::MemberAccessNode>()) {
        // For chained member access, we need to get a pointer to the intermediate struct
        auto &base_member_access = node.get_base_node().get<AST::MemberAccessNode>();
        
        // Get the base for the first member access
        if (base_member_access.get_base_node().has_type<AST::VarRefNode>()) {
            auto &base_var_ref = base_member_access.get_base_node().get<AST::VarRefNode>();
            if (base_var_ref.is_var()) {
                auto &base_var_node = base_var_ref.get_var();
                base_var_node.accept(*_ctx.visitor);
                
                // If the base variable is a pointer, we need to load it for GEP operations
                if (base_var_node.decl().type_node()->type.is_pointer()) {
                    llvm::Value *var_alloca = _ctx.value_stack.top();
                    _ctx.value_stack.pop();
                    
                    llvm::Type *pointer_type = _ctx.types->get_llvm_type(base_var_node.decl().type_node()->type, *_ctx.current_cmp_unit);
                    llvm::Value *loaded_pointer = _ctx.builder->CreateLoad(pointer_type, var_alloca, base_var_node.decl().name() + "_loaded");
                    _ctx.value_stack.push(loaded_pointer);
                }
            } else {
                base_member_access.get_base_node().node()->accept(*_ctx.visitor);
            }
        } else {
            base_member_access.get_base_node().node()->accept(*_ctx.visitor);
        }
        
        if (_ctx.value_stack.empty()) {
            throw _ctx.error("No base value on stack for chained member access");
        }
        
        llvm::Value *intermediate_base = _ctx.value_stack.top();
        _ctx.value_stack.pop();
        
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
                throw _ctx.error(fmt::format("Intermediate member '{}' not found in struct", intermediate_member_name));
            }
            
            // Get the struct type from the structure table
            auto struct_id = _ctx.current_cmp_unit->structure_table->get_structure_id(base_result_type.get_complex_type());
            if (struct_id == 0) {
                throw _ctx.error("Intermediate struct not found in structure table");
            }
            
            auto &structure = _ctx.current_cmp_unit->structure_table->get_structure(struct_id);
            
            // Create GEP instruction to access the intermediate member
            std::vector<llvm::Value *> indices = {
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), 0),
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), intermediate_member_index)
            };
            
            base_ptr = _ctx.builder->CreateGEP(
                structure.llvm_struct, intermediate_base, indices, intermediate_member_name + "_ptr");
        } else {
            throw _ctx.error("Invalid intermediate type for chained member access");
        }
    } else {
        // For other base node types, use normal visit
        node.get_base_node().node()->accept(*_ctx.visitor);
        base_ptr = _ctx.value_stack.top();
        _ctx.value_stack.pop();
    }
    
    if (!base_ptr) {
        throw _ctx.error("No base value on stack for member access");
    }
    
    // Get the type information for the final member access
    auto result_type = node.result_type();
    if (result_type.is_void()) {
        throw _ctx.error("Cannot access member of void type");
    }
    
    // Get the base type (either from the variable or from the intermediate member access)
    AST::ValueType base_type;
    if (node.get_base_node().has_type<AST::VarRefNode>()) {
        base_type = node.get_base_node().get<AST::VarRefNode>().result_type();
    } else if (node.get_base_node().has_type<AST::MemberAccessNode>()) {
        base_type = node.get_base_node().get<AST::MemberAccessNode>().result_type();
    } else {
        throw _ctx.error("Unsupported base type for member access");
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
            throw _ctx.error(fmt::format("Member '{}' not found in struct", member_name));
        }
        
        // Get the struct type from the structure table
        auto struct_id = _ctx.current_cmp_unit->structure_table->get_structure_id(base_type.get_complex_type());
        if (struct_id == 0) {
            throw _ctx.error("Struct not found in structure table");
        }
        
        auto &structure = _ctx.current_cmp_unit->structure_table->get_structure(struct_id);
        
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
            llvm::Type *pointer_type = _ctx.types->get_llvm_type(var_node.decl().type_node()->type, *_ctx.current_cmp_unit);
            struct_ptr = _ctx.builder->CreateLoad(pointer_type, base_ptr, root_var_name + "_deref");
        }
        
        // Create GEP instruction to access the final member
        std::vector<llvm::Value *> indices = {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), 0), // struct pointer
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), member_index)  // member index
        };
        
        llvm::Value *member_ptr = _ctx.builder->CreateGEP(
            structure.llvm_struct, struct_ptr, indices, member_name + "_ptr");
        
        // Load the value from the member
        llvm::Value *member_value = _ctx.builder->CreateLoad(
            _ctx.types->get_llvm_type(result_type, *_ctx.current_cmp_unit), member_ptr, member_name);
        
        _ctx.value_stack.push(member_value);
        return;
    }
    
    throw _ctx.error("Unsupported member access pattern");
}

void StructCodegen::gen_var(AST::VarNode &node)
{
    // Get the LLVM value for this variable (should be an alloca instruction)
    auto it = _ctx.var_map.find(&node.decl());
    if (it == _ctx.var_map.end()) {
        throw _ctx.error(fmt::format(
            "Variable '{}' not found in variable map", node.decl().name()));
    }
    
    // Push the alloca instruction (variable pointer) onto the stack
    _ctx.value_stack.push(it->second);
}

void StructCodegen::gen_var_member(AST::VarMemberNode &node)
{
    // Get the struct declaration and member property
    auto struct_decl = node.struct_decl();
    if (!struct_decl) {
        throw _ctx.error("Cannot find struct declaration for member access");
    }
    
    auto &property = node.property();
    
    // Get the base variable reference
    node.get_ref().accept(*_ctx.visitor);
    if (_ctx.value_stack.empty()) {
        throw _ctx.error("No base variable on stack for member access");
    }
    
    llvm::Value *base_ptr = _ctx.value_stack.top();
    _ctx.value_stack.pop();
    
    // Get the struct type from the structure table
    auto struct_id = _ctx.current_cmp_unit->structure_table->get_structure_id(struct_decl);
    if (struct_id == 0) {
        throw _ctx.error("Struct not found in structure table");
    }
    
    auto &structure = _ctx.current_cmp_unit->structure_table->get_structure(struct_id);
    
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
        throw _ctx.error(fmt::format("Member '{}' not found in struct", property.name));
    }
    
    // Create GEP instruction to access the member
    std::vector<llvm::Value *> indices = {
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), 0), // struct pointer
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), member_index)  // member index
    };
    
    llvm::Value *member_ptr = _ctx.builder->CreateGEP(
        structure.llvm_struct, base_ptr, indices, property.name + "_ptr");
    
    // Push the member pointer onto the stack
    _ctx.value_stack.push(member_ptr);
}

void StructCodegen::gen_member_mut(AST::MemberMutNode &node)
{
    // Visit the value expression first to get the new value
    node.value_expr->accept(*_ctx.visitor);
    
    if (_ctx.value_stack.empty()) {
        throw _ctx.error("No value on stack for member mutation");
    }
    
    llvm::Value *new_value = _ctx.value_stack.top();
    _ctx.value_stack.pop();
    
    // Get the member access node and generate the pointer to the member
    auto &member_access = *node.member_access;
    
    llvm::Value *base_ptr = nullptr;
    
    // Handle different base node types for member access
    if (member_access.get_base_node().has_type<AST::VarRefNode>()) {
        auto &var_ref = member_access.get_base_node().get<AST::VarRefNode>();
        
        if (var_ref.is_var()) {
            // Get the variable node and visit it to get the pointer, not the loaded value
            auto &var_node = var_ref.get_var();
            var_node.accept(*_ctx.visitor);
            base_ptr = _ctx.value_stack.top();
            _ctx.value_stack.pop();
        } else {
            // For other VarRef types, use normal visit but expect a pointer
            member_access.get_base_node().node()->accept(*_ctx.visitor);
            base_ptr = _ctx.value_stack.top();
            _ctx.value_stack.pop();
        }
    } else if (member_access.get_base_node().has_type<AST::MemberAccessNode>()) {
        // For chained member access, we need to get a pointer to the intermediate struct
        auto &base_member_access = member_access.get_base_node().get<AST::MemberAccessNode>();
        
        // Get the base for the first member access
        if (base_member_access.get_base_node().has_type<AST::VarRefNode>()) {
            auto &base_var_ref = base_member_access.get_base_node().get<AST::VarRefNode>();
            if (base_var_ref.is_var()) {
                auto &base_var_node = base_var_ref.get_var();
                base_var_node.accept(*_ctx.visitor);
                
                // If the base variable is a pointer, we need to load it for GEP operations
                if (base_var_node.decl().type_node()->type.is_pointer()) {
                    llvm::Value *var_alloca = _ctx.value_stack.top();
                    _ctx.value_stack.pop();
                    
                    llvm::Type *pointer_type = _ctx.types->get_llvm_type(base_var_node.decl().type_node()->type, *_ctx.current_cmp_unit);
                    llvm::Value *loaded_pointer = _ctx.builder->CreateLoad(pointer_type, var_alloca, base_var_node.decl().name() + "_loaded");
                    _ctx.value_stack.push(loaded_pointer);
                }
            } else {
                base_member_access.get_base_node().node()->accept(*_ctx.visitor);
            }
        } else {
            base_member_access.get_base_node().node()->accept(*_ctx.visitor);
        }
        
        if (_ctx.value_stack.empty()) {
            throw _ctx.error("No base value on stack for chained member access");
        }
        
        llvm::Value *intermediate_base = _ctx.value_stack.top();
        _ctx.value_stack.pop();
        
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
                throw _ctx.error(fmt::format("Intermediate member '{}' not found in struct", intermediate_member_name));
            }
            
            // Get the struct type from the structure table
            auto struct_id = _ctx.current_cmp_unit->structure_table->get_structure_id(base_result_type.get_complex_type());
            if (struct_id == 0) {
                throw _ctx.error("Intermediate struct not found in structure table");
            }
            
            auto &structure = _ctx.current_cmp_unit->structure_table->get_structure(struct_id);
            
            // Create GEP instruction to access the intermediate member
            std::vector<llvm::Value *> indices = {
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), 0),
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), intermediate_member_index)
            };
            
            base_ptr = _ctx.builder->CreateGEP(
                structure.llvm_struct, intermediate_base, indices, intermediate_member_name + "_ptr");
        } else {
            throw _ctx.error("Invalid intermediate type for chained member access");
        }
    } else {
        // For other base node types, use normal visit
        member_access.get_base_node().node()->accept(*_ctx.visitor);
        base_ptr = _ctx.value_stack.top();
        _ctx.value_stack.pop();
    }
    
    if (!base_ptr) {
        throw _ctx.error("No base value on stack for member mutation");
    }
    
    // Get the base type (either from the variable or from the intermediate member access)
    AST::ValueType base_type;
    if (member_access.get_base_node().has_type<AST::VarRefNode>()) {
        base_type = member_access.get_base_node().get<AST::VarRefNode>().result_type();
    } else if (member_access.get_base_node().has_type<AST::MemberAccessNode>()) {
        base_type = member_access.get_base_node().get<AST::MemberAccessNode>().result_type();
    } else {
        throw _ctx.error("Unsupported base type for member mutation");
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
            throw _ctx.error(fmt::format("Member '{}' not found in struct", member_name));
        }
        
        // Get the struct type from the structure table
        auto struct_id = _ctx.current_cmp_unit->structure_table->get_structure_id(base_type.get_complex_type());
        if (struct_id == 0) {
            throw _ctx.error("Struct not found in structure table");
        }
        
        auto &structure = _ctx.current_cmp_unit->structure_table->get_structure(struct_id);
        
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
            llvm::Type *pointer_type = _ctx.types->get_llvm_type(var_node.decl().type_node()->type, *_ctx.current_cmp_unit);
            struct_ptr = _ctx.builder->CreateLoad(pointer_type, base_ptr, root_var_name + "_deref");
        }
        
        // Create GEP instruction to access the final member
        std::vector<llvm::Value *> indices = {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), 0), // struct pointer
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), member_index)  // member index
        };
        
        llvm::Value *member_ptr = _ctx.builder->CreateGEP(
            structure.llvm_struct, struct_ptr, indices, member_name + "_ptr");
        
        // Get the member type for potential type conversion
        auto result_type = member_access.result_type();
        llvm::Type *member_llvm_type = _ctx.types->get_llvm_type(result_type, *_ctx.current_cmp_unit);
        
        // Cast the new value to the member's type if necessary
        if (member_llvm_type->isFloatTy() && new_value->getType()->isDoubleTy()) {
            new_value = _ctx.builder->CreateFPTrunc(new_value, member_llvm_type);
        } else if (member_llvm_type->isDoubleTy() && new_value->getType()->isFloatTy()) {
            new_value = _ctx.builder->CreateFPExt(new_value, member_llvm_type);
        } else if (member_llvm_type->isIntegerTy() && new_value->getType()->isFloatingPointTy()) {
            new_value = _ctx.builder->CreateFPToSI(new_value, member_llvm_type);
        } else if (member_llvm_type->isFloatingPointTy() && new_value->getType()->isIntegerTy()) {
            new_value = _ctx.builder->CreateSIToFP(new_value, member_llvm_type);
        } else if (member_llvm_type->isIntegerTy() && new_value->getType()->isIntegerTy() && 
                   member_llvm_type->getIntegerBitWidth() != new_value->getType()->getIntegerBitWidth()) {
            if (member_llvm_type->getIntegerBitWidth() > new_value->getType()->getIntegerBitWidth()) {
                new_value = _ctx.builder->CreateSExt(new_value, member_llvm_type);
            } else {
                new_value = _ctx.builder->CreateTrunc(new_value, member_llvm_type);
            }
        }
        
        // Store the new value in the member
        _ctx.builder->CreateStore(new_value, member_ptr);
        return;
    }
    
    throw _ctx.error("Unsupported member mutation pattern");
}
}
