#include <catch2/catch_test_macros.hpp>

#include <Compiler/LLVM/Codegen/Partition.h>
#include <Compiler/LLVM/Codegen/PartitionHash.h>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
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

std::unique_ptr<llvm::Module> parse_ir(llvm::LLVMContext &context, const std::string &ir)
{
    llvm::SMDiagnostic error;
    std::unique_ptr<llvm::Module> module = llvm::parseAssemblyString(ir, error, context);
    REQUIRE(module != nullptr);
    return module;
}

std::string hash_reachable(llvm::Module &module)
{
    llvm::Function *function = module.getFunction("f");
    REQUIRE(function != nullptr);

    const std::unordered_set<const llvm::GlobalValue *> needed =
        Compiler::LLVM::reachable_from({ function });

    REQUIRE(needed.count(function) == 1);
    REQUIRE(needed.count(module.getNamedValue("a")) == 1);

    return Compiler::LLVM::partition_hex(
        module, needed, [](const llvm::GlobalValue *) { return true; }, Compiler::LLVM::PartitionEnv{});
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

std::string cyclic(const std::string &a_points_at)
{
    return R"(
        @a = global ptr @)" + a_points_at + R"(
        @b = global ptr @a

        define void @f() {
        entry:
          %p = load ptr, ptr @a
          ret void
        }
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

TEST_CASE("a partition hash survives globals that point at each other", "[cache][partition]")
{
    // a GlobalVariable is itself a constant whose operand is its initializer. walking
    // operands recursively never ends when two globals point at each other; the worklist
    // already visits each initializer once
    llvm::LLVMContext context;

    auto ab_module = parse_ir(context, cyclic("b"));
    auto aa_module = parse_ir(context, cyclic("a"));

    REQUIRE(Compiler::LLVM::reachable_from({ ab_module->getFunction("f") }).count(ab_module->getNamedValue("b")) == 1);

    const std::string ab = hash_reachable(*ab_module);
    const std::string aa = hash_reachable(*aa_module);

    REQUIRE_FALSE(ab.empty());
    REQUIRE(ab == hash_reachable(*ab_module));
    REQUIRE(ab != aa);
}
