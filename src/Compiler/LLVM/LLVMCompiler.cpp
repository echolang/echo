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
}

void LLVMCompiler::visitLiteralBoolExpr(AST::LiteralBoolExprNode &node)
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
    llvm::ModulePassManager modulePM = passBuilder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);

    
    modulePM.run(*llvm_module, moduleAM);
}
