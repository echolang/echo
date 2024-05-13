#include "Compiler/LLVM/LLVMCompiler.h"

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ExecutionEngine/GenericValue.h>

#include "AST/VarDeclNode.h"

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

void LLVMCompiler::visitVarDecl(AST::VarDeclNode &node)
{
    auto varname = node.name();
    llvm::Type* type = nullptr;

    switch (node.type_n->type.get_primitive_type()) {
        case AST::ValueTypePrimitive::t_float32:
            type = llvm::Type::getFloatTy(*llvm_context);
            break;
        case AST::ValueTypePrimitive::t_float64:
            type = llvm::Type::getDoubleTy(*llvm_context);
            break;
        case AST::ValueTypePrimitive::t_int8:
            type = llvm::Type::getInt8Ty(*llvm_context);
            break;
        case AST::ValueTypePrimitive::t_int16:
            type = llvm::Type::getInt16Ty(*llvm_context);
            break;
        case AST::ValueTypePrimitive::t_int32:
            type = llvm::Type::getInt32Ty(*llvm_context);
            break;
        case AST::ValueTypePrimitive::t_int64:
            type = llvm::Type::getInt64Ty(*llvm_context);
            break;
        case AST::ValueTypePrimitive::t_uint8:
            type = llvm::Type::getInt8Ty(*llvm_context);
            break;
        case AST::ValueTypePrimitive::t_uint16:
            type = llvm::Type::getInt16Ty(*llvm_context);
            break;
        case AST::ValueTypePrimitive::t_uint32:
            type = llvm::Type::getInt32Ty(*llvm_context);
            break;
        case AST::ValueTypePrimitive::t_uint64:
            type = llvm::Type::getInt64Ty(*llvm_context);
            break;
        case AST::ValueTypePrimitive::t_bool:
            type = llvm::Type::getInt1Ty(*llvm_context);
            break;
        default:
            throw std::runtime_error("Unsupported variable type");
    }

    // alloc the variable on the stack
    llvm::AllocaInst* alloca = llvm_builder->CreateAlloca(type, nullptr, varname);

    if (node.init_expr) {
        node.init_expr->accept(*this);
        llvm::Value* init_value = value_stack.top();
        llvm_builder->CreateStore(init_value, alloca);
        value_stack.pop();

        llvm::FunctionCallee CalleeF = llvm_module->getOrInsertFunction("printf",
            llvm::FunctionType::get(llvm::IntegerType::getInt32Ty(*llvm_context), llvm::PointerType::get(llvm::Type::getInt8Ty(*llvm_context), 0), true /* this is var arg func type*/) 
        );

        // print the value
        std::vector<llvm::Value *> ArgsV;
        ArgsV.push_back(llvm_builder->CreateGlobalStringPtr("%f\n"));
        llvm::Value* printed_value = llvm_builder->CreateFPExt(init_value, llvm::Type::getDoubleTy(*llvm_context), "toDouble");
        ArgsV.push_back(printed_value);

        llvm_builder->CreateCall(CalleeF, ArgsV);
    }
}

void LLVMCompiler::visitLiteralFloatExpr(AST::LiteralFloatExprNode &node)
{
    if (node.is_double_precision()) {
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

void LLVMCompiler::printIR(bool toFile) {
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
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

    auto TargetTriple = llvm::sys::getDefaultTargetTriple();

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
