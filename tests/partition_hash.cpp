#include <catch2/catch_test_macros.hpp>

#include <Compiler/LLVM/Codegen/PartitionHash.h>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

#include <memory>
#include <string>
#include <unordered_set>

namespace
{

std::string hash_of(llvm::LLVMContext &context, const std::string &ir)
{
    llvm::SMDiagnostic error;
    std::unique_ptr<llvm::Module> module = llvm::parseAssemblyString(ir, error, context);
    REQUIRE(module != nullptr);

    std::unordered_set<const llvm::GlobalValue *> needed;
    for (const llvm::Function &function : *module) {
        needed.insert(&function);
    }

    return Compiler::LLVM::partition_hex(
        *module, needed, [](const llvm::GlobalValue *) { return true; }, Compiler::LLVM::PartitionEnv{});
}

std::string stores(const std::string &last)
{
    return R"(
        define void @f(ptr %p) {
        entry:
          store i32 7, ptr %p
          store i32 9, ptr %p
          store i32 )" + last + R"(, ptr %p
          ret void
        }
    )";
}

std::string attached(const std::string &last)
{
    return R"(
        define void @f() {
        entry:
          ret void, !eco.test !0
        }

        !0 = !{!"a", !"b", !")" + last + R"("}
    )";
}

};

TEST_CASE("a partition hash is stable for identical IR", "[cache][partition]")
{
    llvm::LLVMContext context;

    REQUIRE(hash_of(context, stores("7")) == hash_of(context, stores("7")));
    REQUIRE(hash_of(context, attached("a")) == hash_of(context, attached("a")));
}

TEST_CASE("a partition hash tells apart which constant is repeated", "[cache][partition]")
{
    llvm::LLVMContext context;

    REQUIRE(hash_of(context, stores("7")) != hash_of(context, stores("9")));
}

TEST_CASE("a partition hash tells apart which metadata is repeated", "[cache][partition]")
{
    llvm::LLVMContext context;

    REQUIRE(hash_of(context, attached("a")) != hash_of(context, attached("b")));
}
