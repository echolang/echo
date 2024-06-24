#include "Compiler/LLVM/LLVMCompiler.h"

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Utils.h>

#include "AST/VarDeclNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/ExprNode.h"
#include "AST/TypeCastNode.h"
#include "AST/FunctionDeclNode.h"

#include <iostream>

LLVMCompiler::LLVMCompiler()
{

}

LLVMCompiler::~LLVMCompiler()
{
}

void LLVMCompiler::compile_bundle(const AST::Bundle &bundle)
{
    llvm_context = std::make_unique<llvm::LLVMContext>();
    llvm_module = std::make_unique<llvm::Module>("echo_module", *llvm_context);
    llvm_builder = std::make_unique<llvm::IRBuilder<>>(*llvm_context);

    if (!llvm_module) {
        llvm::errs() << "Failed to create module.\n";
    }

    llvm::FunctionCallee CalleeF = llvm_module->getOrInsertFunction("printf",
        llvm::FunctionType::get(llvm::IntegerType::getInt32Ty(*llvm_context), llvm::PointerType::get(llvm::Type::getInt8Ty(*llvm_context), 0), true /* this is var arg func type*/) 
    );

    llvm::FunctionType *funcType = llvm::FunctionType::get(llvm_builder->getVoidTy(), false);
    llvm::Function *function = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, "main", llvm_module.get());
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(*llvm_context, "entry", function);
    llvm_builder->SetInsertPoint(entry);

    for (auto &module : bundle.modules) {
        for (auto &file : module->files()) {
            file.root->accept(*this);
        }
    }

    // terminate the function
    llvm_builder->CreateRetVoid();

    // optimize the module
    // optimize();
}

void LLVMCompiler::visitScope(AST::ScopeNode &node)
{
    for (auto &child : node.children) {
        child.node()->accept(*this);
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
    auto new_type = node.result_type().get_primitive_type();
    auto old_type = node.expr->result_type().get_primitive_type();

    auto new_llvm_type = get_llvm_type(new_type);

    auto value = value_stack.top();
    value_stack.pop();


    // @TODO make this pretty, i just wanted to try this out quickly

    if (old_type == new_type) {
        value_stack.push(value);
    } else if (old_type == AST::ValueTypePrimitive::t_float32 && new_type == AST::ValueTypePrimitive::t_float64) {
        value_stack.push(llvm_builder->CreateFPExt(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_float64 && new_type == AST::ValueTypePrimitive::t_float32) {
        value_stack.push(llvm_builder->CreateFPTrunc(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int8 && new_type == AST::ValueTypePrimitive::t_int16) {
        value_stack.push(llvm_builder->CreateSExt(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int16 && new_type == AST::ValueTypePrimitive::t_int8) {
        value_stack.push(llvm_builder->CreateTrunc(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int32 && new_type == AST::ValueTypePrimitive::t_int8) {
        value_stack.push(llvm_builder->CreateTrunc(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int32 && new_type == AST::ValueTypePrimitive::t_int16) {
        value_stack.push(llvm_builder->CreateTrunc(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int64 && new_type == AST::ValueTypePrimitive::t_int8) {
        value_stack.push(llvm_builder->CreateTrunc(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int64 && new_type == AST::ValueTypePrimitive::t_int16) {
        value_stack.push(llvm_builder->CreateTrunc(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int64 && new_type == AST::ValueTypePrimitive::t_int32) {
        value_stack.push(llvm_builder->CreateTrunc(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint8 && new_type == AST::ValueTypePrimitive::t_uint16) {
        value_stack.push(llvm_builder->CreateZExt(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint16 && new_type == AST::ValueTypePrimitive::t_uint8) {
        value_stack.push(llvm_builder->CreateTrunc(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint32 && new_type == AST::ValueTypePrimitive::t_uint8) {
        value_stack.push(llvm_builder->CreateTrunc(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint32 && new_type == AST::ValueTypePrimitive::t_uint16) {
        value_stack.push(llvm_builder->CreateTrunc(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint64 && new_type == AST::ValueTypePrimitive::t_uint8) {
        value_stack.push(llvm_builder->CreateTrunc(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint64 && new_type == AST::ValueTypePrimitive::t_uint16) {
        value_stack.push(llvm_builder->CreateTrunc(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint64 && new_type == AST::ValueTypePrimitive::t_uint32) {
        value_stack.push(llvm_builder->CreateTrunc(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int8 && new_type == AST::ValueTypePrimitive::t_uint8) {
        value_stack.push(llvm_builder->CreateIntCast(value, new_llvm_type, false));
    } else if (old_type == AST::ValueTypePrimitive::t_int16 && new_type == AST::ValueTypePrimitive::t_uint16) {
        value_stack.push(llvm_builder->CreateIntCast(value, new_llvm_type, false));
    } else if (old_type == AST::ValueTypePrimitive::t_int32 && new_type == AST::ValueTypePrimitive::t_uint32) {
        value_stack.push(llvm_builder->CreateIntCast(value, new_llvm_type, false));
    } else if (old_type == AST::ValueTypePrimitive::t_int64 && new_type == AST::ValueTypePrimitive::t_uint64) {
        value_stack.push(llvm_builder->CreateIntCast(value, new_llvm_type, false));
    } else if (old_type == AST::ValueTypePrimitive::t_uint8 && new_type == AST::ValueTypePrimitive::t_int8) {
        value_stack.push(llvm_builder->CreateIntCast(value, new_llvm_type, true));
    } else if (old_type == AST::ValueTypePrimitive::t_uint16 && new_type == AST::ValueTypePrimitive::t_int16) {
        value_stack.push(llvm_builder->CreateIntCast(value, new_llvm_type, true));
    } else if (old_type == AST::ValueTypePrimitive::t_uint32 && new_type == AST::ValueTypePrimitive::t_int32) {
        value_stack.push(llvm_builder->CreateIntCast(value, new_llvm_type, true));
    } else if (old_type == AST::ValueTypePrimitive::t_uint64 && new_type == AST::ValueTypePrimitive::t_int64) {
        value_stack.push(llvm_builder->CreateIntCast(value, new_llvm_type, true));
    } else if (old_type == AST::ValueTypePrimitive::t_int8 && new_type == AST::ValueTypePrimitive::t_float32) {
        value_stack.push(llvm_builder->CreateSIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int16 && new_type == AST::ValueTypePrimitive::t_float32) {
        value_stack.push(llvm_builder->CreateSIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int32 && new_type == AST::ValueTypePrimitive::t_float32) {
        value_stack.push(llvm_builder->CreateSIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int64 && new_type == AST::ValueTypePrimitive::t_float32) {
        value_stack.push(llvm_builder->CreateSIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int8 && new_type == AST::ValueTypePrimitive::t_float64) {
        value_stack.push(llvm_builder->CreateSIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int16 && new_type == AST::ValueTypePrimitive::t_float64) {
        value_stack.push(llvm_builder->CreateSIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int32 && new_type == AST::ValueTypePrimitive::t_float64) {
        value_stack.push(llvm_builder->CreateSIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_int64 && new_type == AST::ValueTypePrimitive::t_float64) {
        value_stack.push(llvm_builder->CreateSIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint8 && new_type == AST::ValueTypePrimitive::t_float32) {
        value_stack.push(llvm_builder->CreateUIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint16 && new_type == AST::ValueTypePrimitive::t_float32) {
        value_stack.push(llvm_builder->CreateUIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint32 && new_type == AST::ValueTypePrimitive::t_float32) {
        value_stack.push(llvm_builder->CreateUIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint64 && new_type == AST::ValueTypePrimitive::t_float32) {
        value_stack.push(llvm_builder->CreateUIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint8 && new_type == AST::ValueTypePrimitive::t_float64) {
        value_stack.push(llvm_builder->CreateUIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint16 && new_type == AST::ValueTypePrimitive::t_float64) {
        value_stack.push(llvm_builder->CreateUIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint32 && new_type == AST::ValueTypePrimitive::t_float64) {
        value_stack.push(llvm_builder->CreateUIToFP(value, new_llvm_type));
    } else if (old_type == AST::ValueTypePrimitive::t_uint64 && new_type == AST::ValueTypePrimitive::t_float64) {
        value_stack.push(llvm_builder->CreateUIToFP(value, new_llvm_type));
    } else {
        throw std::runtime_error("Unsupported type cast");
    }
}

llvm::Type *LLVMCompiler::get_llvm_type(AST::ValueTypePrimitive type)
{
    switch (type) {
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
    llvm::Type* type = get_llvm_type(node.type_node()->type.get_primitive_type());

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
}

void LLVMCompiler::visitBinaryExpr(AST::BinaryExprNode &node)
{
    node.lhs->accept(*this);
    node.rhs->accept(*this);

    auto lhsret =  node.lhs->result_type();
    auto rhsret =  node.rhs->result_type();

    auto right = value_stack.top();
    value_stack.pop();
    auto left = value_stack.top();
    value_stack.pop();

    if (lhsret.is_integer() && rhsret.is_integer()) 
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
            default:
                throw std::runtime_error("Unsupported binary operator");
        }
    }
    else 
    {
        // identify if the left or right value is a float and of what size
        bool left_is_float = lhsret.is_primitive_of_type(AST::ValueTypePrimitive::t_float32);
        bool right_is_float = rhsret.is_primitive_of_type(AST::ValueTypePrimitive::t_float32);
        bool left_is_double = lhsret.is_primitive_of_type(AST::ValueTypePrimitive::t_float64);
        bool right_is_double = rhsret.is_primitive_of_type(AST::ValueTypePrimitive::t_float64);

        if (left_is_float && right_is_double) {
            left = llvm_builder->CreateFPExt(left, llvm::Type::getDoubleTy(*llvm_context));
        } else if (left_is_double && right_is_float) {
            right = llvm_builder->CreateFPExt(right, llvm::Type::getDoubleTy(*llvm_context));
        } else if (left_is_float && (!right_is_float && !right_is_double)) {
            right = llvm_builder->CreateSIToFP(right, llvm::Type::getFloatTy(*llvm_context));
        } else if (left_is_double && (!right_is_float && !right_is_double)) {
            right = llvm_builder->CreateSIToFP(right, llvm::Type::getDoubleTy(*llvm_context));
        } else if ((!left_is_float && !left_is_double) && right_is_float) {
            left = llvm_builder->CreateSIToFP(left, llvm::Type::getFloatTy(*llvm_context));
        } else if ((!left_is_float && !left_is_double) && right_is_double) {
            left = llvm_builder->CreateSIToFP(left, llvm::Type::getDoubleTy(*llvm_context));
        } else {
            // throw std::runtime_error("Unsupported binary operator");
        }

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
            default:
                throw std::runtime_error("Unsupported binary operator");
        }
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

            // printf each argument value
            std::vector<llvm::Value *> ArgsV;

            if (arg_value->getType()->isFloatTy()) {
                ArgsV.push_back(llvm_builder->CreateGlobalStringPtr("%f\n"));
                llvm::Value* printed_value = llvm_builder->CreateFPExt(arg_value, llvm::Type::getDoubleTy(*llvm_context), "toDouble");
                ArgsV.push_back(printed_value);
            } else if (arg_value->getType()->isDoubleTy()) {
                ArgsV.push_back(llvm_builder->CreateGlobalStringPtr("%f\n"));
                ArgsV.push_back(arg_value);
            } else if (arg_value->getType()->isIntegerTy()) {
                ArgsV.push_back(llvm_builder->CreateGlobalStringPtr("%d\n"));
                ArgsV.push_back(arg_value);
            } else if (arg_value->getType()->isIntegerTy(1)) {
                ArgsV.push_back(llvm_builder->CreateGlobalStringPtr("%d\n"));
                ArgsV.push_back(arg_value);
            } else if (arg_value->getType()->isPointerTy()) {
                ArgsV.push_back(llvm_builder->CreateGlobalStringPtr("%s\n"));
                ArgsV.push_back(arg_value);
            } else {
                throw std::runtime_error("Unsupported argument type for 'echo'");
            }

            llvm_builder->CreateCall(llvm_module->getFunction("printf"), ArgsV);
        }
    }

    else 
    {
        llvm::Function *func = llvm_module->getFunction(node.token_function_name.value());
        if (!func) {
            throw std::runtime_error("Function not found");
        }

        std::vector<llvm::Value *> args;
        for (auto &arg : node.arguments) {
            arg->accept(*this);
            args.push_back(value_stack.top());
            value_stack.pop();
        }

        llvm::Value *ret = llvm_builder->CreateCall(func, args);
        value_stack.push(ret);
    
    }
}

void LLVMCompiler::visitVarRefExpr(AST::VarRefExprNode &node)
{
    auto var_ref = node.var_ref;
    llvm::AllocaInst *var = var_map[var_ref->decl];

    llvm::Type *type = var->getAllocatedType();
    llvm::Value* varval = llvm_builder->CreateLoad(type, var, var->getName());

    value_stack.push(varval);
}

void LLVMCompiler::visitNull(AST::NullNode &node)
{
}

void LLVMCompiler::visitOperator(AST::OperatorNode &node)
{
}

void LLVMCompiler::visitFunctionDecl(AST::FunctionDeclNode &node)
{
    AST::TypeNode *return_type = node.return_type;
    assert(return_type && "Function return type is not set");
    llvm::Type *llvm_return_type = get_llvm_type(return_type->type.get_primitive_type());

    std::vector<llvm::Type *> arg_types;
    for (auto &arg : node.args) {
        arg_types.push_back(get_llvm_type(arg->type_node()->type.get_primitive_type()));
    }

    llvm::FunctionType *func_type = llvm::FunctionType::get(llvm_return_type, arg_types, false);
    llvm::Function *func = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage, node.func_name(), llvm_module.get());

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(*llvm_context, "entry", func);
    llvm_builder->SetInsertPoint(entry);

    // create the arguments
    for (auto &arg : func->args()) {
        arg.setName(node.args[arg.getArgNo()]->name());
        llvm::AllocaInst *alloca = llvm_builder->CreateAlloca(arg.getType(), nullptr, arg.getName());
        llvm_builder->CreateStore(&arg, alloca);
        var_map[node.args[arg.getArgNo()]] = alloca;
    }

    // visit the function body
    node.body->accept(*this);

    // terminate the function
    llvm_builder->CreateRetVoid();
}

void LLVMCompiler::visitReturn(AST::ReturnNode &node)
{
}
void LLVMCompiler::printIR(bool toFile)
{
    if (toFile) {
        std::error_code EC;
        llvm::raw_fd_ostream outFile("output.ll", EC);
        if (EC) {
            llvm::errs() << "Could not open file: " << EC.message() << '\n';
            return;
        }
        llvm_module->print(outFile, nullptr);
        outFile.close();
    } else {
        llvm_module->print(llvm::outs(), nullptr);
    }
}

void LLVMCompiler::run_code() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    std::string errorStr;
    const llvm::TargetOptions opts;
    llvm::ExecutionEngine *EE = llvm::EngineBuilder(std::move(llvm_module))
                                .setErrorStr(&errorStr)
                                .setEngineKind(llvm::EngineKind::JIT)
                                .setTargetOptions(opts)
                                .create();

    if (!EE) {
        llvm::errs() << "Failed to create ExecutionEngine: " << errorStr << '\n';
        return;
    }

    EE->finalizeObject();

    auto *func = EE->FindFunctionNamed("main");
    if (!func) {
        llvm::errs() << "Function 'main' not found in module.\n";
        return;
    }

    std::vector<llvm::GenericValue> noargs;
    llvm::GenericValue gv = EE->runFunction(func, noargs);

    llvm::outs() << "Function 'main' executed.\n";

    delete EE;
    llvm::llvm_shutdown();
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

    llvm_module->setDataLayout(TargetMachine->createDataLayout());
    llvm_module->setTargetTriple(TargetTriple);

    auto Filename = "output.o";
    std::error_code EC;
    llvm::raw_fd_ostream dest(Filename, EC, llvm::sys::fs::OF_None);

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

    pass.run(*llvm_module);
    dest.flush();
}

void LLVMCompiler::optimize() {
    if (!llvm_module) {
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
    // llvm::ModulePassManager modulePM = passBuilder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    llvm::ModulePassManager modulePM = passBuilder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O0);

    
    modulePM.run(*llvm_module, moduleAM);
}
