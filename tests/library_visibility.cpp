#include <catch2/catch_test_macros.hpp>

#include <Compiler/LLVM/Codegen/LibraryVisibility.h>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <string>

// hide-except-export over hand-written IR: a local symbol beside the ones it hides

namespace
{
    const char *k_library_unit = R"(
@str = private unnamed_addr constant [3 x i8] c"hi\00"
@counter = global i32 0
@table = dllexport constant i32 1

define internal void @adapter() {
  ret void
}

define void @helper() {
  call void @adapter()
  ret void
}

define dllexport i32 @exported() {
  call void @helper()
  ret i32 0
}
)";
}

TEST_CASE("hide-except-export leaves a local symbol at default visibility", "[library_visibility]")
{
    llvm::LLVMContext context;
    llvm::SMDiagnostic error;
    std::unique_ptr<llvm::Module> module = llvm::parseAssemblyString(k_library_unit, error, context);
    REQUIRE(module != nullptr);

    Compiler::LLVM::hide_non_exported_symbols(*module);

    std::string problems;
    llvm::raw_string_ostream out(problems);
    INFO(problems);
    REQUIRE_FALSE(llvm::verifyModule(*module, &out));

    REQUIRE(module->getFunction("adapter")->hasDefaultVisibility());
    REQUIRE(module->getNamedGlobal("str")->hasDefaultVisibility());

    REQUIRE(module->getFunction("helper")->hasHiddenVisibility());
    REQUIRE(module->getNamedGlobal("counter")->hasHiddenVisibility());

    REQUIRE(module->getFunction("exported")->hasDefaultVisibility());
    REQUIRE(module->getNamedGlobal("table")->hasDefaultVisibility());
}
