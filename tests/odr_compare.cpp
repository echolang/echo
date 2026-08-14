#include <catch2/catch_test_macros.hpp>

#include <Compiler/LLVM/OdrComparison.h>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

#include <memory>
#include <string>
#include <vector>

// **the instrument the whole per-module object cache rests on.** Two `linkonce_odr` copies of one symbol
// must be identical, so nothing in such a body may read ambient compiler state - and what enforces that is
// `verify_odr_consistency`, which asks `Compiler::LLVM::first_odr_difference` of every duplicated
// definition in a bundle.
//
// asked here of hand-written IR rather than of a compiled program, because the divergences it exists to
// catch are ones no Echo source can currently produce: they are what a *future* change to codegen would
// produce, and a test that can only fire by reintroducing a bug is a test nobody runs.
//
// the two modules deliberately share one `LLVMContext`, which is the shape the compiler is in - one
// context, one `CmpUnit` per module - and is what makes a named struct in the second module come out as
// `%Payload.1`.

namespace
{

// the two modules of a bundle, alive together
struct Pair
{
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> left;
    std::unique_ptr<llvm::Module> right;

    llvm::Function *left_fn(const std::string &name) { return left->getFunction(name); }
    llvm::Function *right_fn(const std::string &name) { return right->getFunction(name); }
};

// parses two units and fails the test - rather than crashing in the comparator - if either does not
std::unique_ptr<Pair> parse_pair(const std::string &left_ir, const std::string &right_ir)
{
    auto pair = std::make_unique<Pair>();

    llvm::SMDiagnostic left_error;
    llvm::SMDiagnostic right_error;

    pair->left = llvm::parseAssemblyString(left_ir, left_error, pair->context);
    pair->right = llvm::parseAssemblyString(right_ir, right_error, pair->context);

    REQUIRE(pair->left != nullptr);
    REQUIRE(pair->right != nullptr);

    return pair;
}

bool agree(Pair &pair, const std::string &name)
{
    llvm::Function *left = pair.left_fn(name);
    llvm::Function *right = pair.right_fn(name);

    REQUIRE(left != nullptr);
    REQUIRE(right != nullptr);

    return !Compiler::LLVM::first_odr_difference(*left, *right).has_value();
}

};

TEST_CASE("two identical definitions agree", "[odr]")
{
    const std::string body = R"(
        define linkonce_odr i32 @sum(i32 %a, i32 %b) {
        entry:
          %r = add nsw i32 %a, %b
          ret i32 %r
        }
    )";

    auto pair = parse_pair(body, body);

    REQUIRE(agree(*pair, "sum"));
}

// the reason this could not be `llvm::FunctionComparator`: it numbers a referenced global per distinct
// pointer, so two modules' `@helper` are two numbers and this case would be reported as a divergence
TEST_CASE("a call to the same symbol in another module agrees", "[odr]")
{
    const std::string body = R"(
        declare i32 @helper(i32)
        define linkonce_odr i32 @wrap(i32 %a) {
        entry:
          %r = call i32 @helper(i32 %a)
          ret i32 %r
        }
    )";

    auto pair = parse_pair(body, body);

    REQUIRE(agree(*pair, "wrap"));
}

// the uniquing suffix: every unit mints its own StructType for one Echo type and they share a context, so
// the second one created is renamed. That is a fact about lowering order and never about the definition
TEST_CASE("a named struct renamed by the context still agrees", "[odr]")
{
    auto pair = parse_pair(
        R"(
            %Payload = type { i64, ptr }
            define linkonce_odr i64 @first_slot(ptr %p) {
            entry:
              %slot = getelementptr inbounds %Payload, ptr %p, i32 0, i32 0
              %v = load i64, ptr %slot, align 8
              ret i64 %v
            }
        )",
        R"(
            %Payload = type { i64, ptr }
            define linkonce_odr i64 @first_slot(ptr %p) {
            entry:
              %slot = getelementptr inbounds %Payload, ptr %p, i32 0, i32 0
              %v = load i64, ptr %slot, align 8
              ret i64 %v
            }
        )");

    // the premise of the case: the two units really did mint two types, and the second one is renamed
    const std::vector<llvm::StructType *> left_types = pair->left->getIdentifiedStructTypes();
    const std::vector<llvm::StructType *> right_types = pair->right->getIdentifiedStructTypes();

    REQUIRE(left_types.size() == 1);
    REQUIRE(right_types.size() == 1);
    REQUIRE(left_types[0] != right_types[0]);
    REQUIRE(left_types[0]->getName() == "Payload");
    REQUIRE(right_types[0]->getName() != "Payload");

    REQUIRE(agree(*pair, "first_slot"));
}

TEST_CASE("a struct of a different shape under one name is a divergence", "[odr]")
{
    auto pair = parse_pair(
        R"(
            %Payload = type { i64, ptr }
            define linkonce_odr i64 @first_slot(ptr %p) {
            entry:
              %slot = getelementptr inbounds %Payload, ptr %p, i32 0, i32 0
              %v = load i64, ptr %slot, align 8
              ret i64 %v
            }
        )",
        R"(
            %Payload = type { i32, ptr }
            define linkonce_odr i64 @first_slot(ptr %p) {
            entry:
              %slot = getelementptr inbounds %Payload, ptr %p, i32 0, i32 0
              %v = load i64, ptr %slot, align 8
              ret i64 %v
            }
        )");

    REQUIRE_FALSE(agree(*pair, "first_slot"));
}

TEST_CASE("a call to a different symbol is a divergence", "[odr]")
{
    auto pair = parse_pair(
        R"(
            declare i32 @a(i32)
            define linkonce_odr i32 @wrap(i32 %x) {
            entry:
              %r = call i32 @a(i32 %x)
              ret i32 %r
            }
        )",
        R"(
            declare i32 @b(i32)
            define linkonce_odr i32 @wrap(i32 %x) {
            entry:
              %r = call i32 @b(i32 %x)
              ret i32 %r
            }
        )");

    REQUIRE_FALSE(agree(*pair, "wrap"));
}

// **the divergence this check was written for.** An abort message is a private string built from whichever
// file the compiler happened to be walking, and a private global is *named* by a per-module slot - so the
// two bodies are indistinguishable by anything but the constant's content
TEST_CASE("two different private constants are a divergence", "[odr]")
{
    auto pair = parse_pair(
        R"(
            @0 = private unnamed_addr constant [7 x i8] c"lib.eco"
            declare void @report(ptr)
            define linkonce_odr void @fail() {
            entry:
              call void @report(ptr @0)
              ret void
            }
        )",
        R"(
            @0 = private unnamed_addr constant [7 x i8] c"app.eco"
            declare void @report(ptr)
            define linkonce_odr void @fail() {
            entry:
              call void @report(ptr @0)
              ret void
            }
        )");

    REQUIRE_FALSE(agree(*pair, "fail"));
}

TEST_CASE("one private constant reached from two modules agrees", "[odr]")
{
    const std::string body = R"(
        @0 = private unnamed_addr constant [6 x i8] c"same!!"
        declare void @report(ptr)
        define linkonce_odr void @fail() {
        entry:
          call void @report(ptr @0)
          ret void
        }
    )";

    auto pair = parse_pair(body, body);

    REQUIRE(agree(*pair, "fail"));
}

TEST_CASE("a different constant operand is a divergence", "[odr]")
{
    auto pair = parse_pair(
        R"(
            define linkonce_odr i32 @answer() {
            entry:
              ret i32 41
            }
        )",
        R"(
            define linkonce_odr i32 @answer() {
            entry:
              ret i32 42
            }
        )");

    REQUIRE_FALSE(agree(*pair, "answer"));
}

// an `!tbaa` leaf is uniqued in the shared context, so two units carrying the same access family carry the
// same node - and two carrying different ones do not
TEST_CASE("a different access family is a divergence", "[odr]")
{
    auto pair = parse_pair(
        R"(
            define linkonce_odr i64 @read(ptr %p) {
            entry:
              %v = load i64, ptr %p, align 8, !tbaa !2
              ret i64 %v
            }
            !0 = !{!"eco.byte"}
            !1 = !{!"eco.i64", !0, i64 0}
            !2 = !{!1, !1, i64 0}
        )",
        R"(
            define linkonce_odr i64 @read(ptr %p) {
            entry:
              %v = load i64, ptr %p, align 8, !tbaa !2
              ret i64 %v
            }
            !0 = !{!"eco.byte"}
            !1 = !{!"eco.f64", !0, i64 0}
            !2 = !{!1, !1, i64 0}
        )");

    REQUIRE_FALSE(agree(*pair, "read"));
}

TEST_CASE("metadata on one side only is a divergence", "[odr]")
{
    auto pair = parse_pair(
        R"(
            define linkonce_odr i64 @read(ptr %p) {
            entry:
              %v = load i64, ptr %p, align 8, !tbaa !2
              ret i64 %v
            }
            !0 = !{!"eco.byte"}
            !1 = !{!"eco.i64", !0, i64 0}
            !2 = !{!1, !1, i64 0}
        )",
        R"(
            define linkonce_odr i64 @read(ptr %p) {
            entry:
              %v = load i64, ptr %p, align 8
              ret i64 %v
            }
        )");

    REQUIRE_FALSE(agree(*pair, "read"));
}

// **the compile unit is a fact about the module, not about the body.** Every scope chain ends at one and it
// names the module's first file, which is supposed to differ - so two units' descriptions of one body agree
// although the units beneath them do not
TEST_CASE("two debug locations differing only in their compile unit agree", "[odr]")
{
    auto pair = parse_pair(
        R"(
            define linkonce_odr i32 @traced(i32 %x) !dbg !4 {
            entry:
              %r = add i32 %x, 1, !dbg !7
              ret i32 %r, !dbg !7
            }
            !llvm.module.flags = !{!0}
            !llvm.dbg.cu = !{!1}
            !0 = !{i32 2, !"Debug Info Version", i32 3}
            !1 = distinct !DICompileUnit(language: DW_LANG_C99, file: !2, emissionKind: FullDebug)
            !2 = !DIFile(filename: "lib.eco", directory: "/w")
            !3 = !DISubroutineType(types: !{})
            !4 = distinct !DISubprogram(name: "traced", file: !5, line: 3, type: !3, scopeLine: 3, spFlags: DISPFlagDefinition, unit: !1)
            !5 = !DIFile(filename: "shared.eco", directory: "/w")
            !7 = !DILocation(line: 4, column: 0, scope: !4)
        )",
        R"(
            define linkonce_odr i32 @traced(i32 %x) !dbg !4 {
            entry:
              %r = add i32 %x, 1, !dbg !7
              ret i32 %r, !dbg !7
            }
            !llvm.module.flags = !{!0}
            !llvm.dbg.cu = !{!1}
            !0 = !{i32 2, !"Debug Info Version", i32 3}
            !1 = distinct !DICompileUnit(language: DW_LANG_C99, file: !2, emissionKind: FullDebug)
            !2 = !DIFile(filename: "app.eco", directory: "/w")
            !3 = !DISubroutineType(types: !{})
            !4 = distinct !DISubprogram(name: "traced", file: !5, line: 3, type: !3, scopeLine: 3, spFlags: DISPFlagDefinition, unit: !1)
            !5 = !DIFile(filename: "shared.eco", directory: "/w")
            !7 = !DILocation(line: 4, column: 0, scope: !4)
        )");

    REQUIRE(agree(*pair, "traced"));
}

// and the line is not: a `!dbg` taken from the ambient walk rather than from the declaration is two
// descriptions of one symbol, of which the linker keeps an arbitrary one
TEST_CASE("two debug locations differing in their line are a divergence", "[odr]")
{
    auto pair = parse_pair(
        R"(
            define linkonce_odr i32 @traced(i32 %x) !dbg !4 {
            entry:
              %r = add i32 %x, 1, !dbg !7
              ret i32 %r, !dbg !7
            }
            !llvm.module.flags = !{!0}
            !llvm.dbg.cu = !{!1}
            !0 = !{i32 2, !"Debug Info Version", i32 3}
            !1 = distinct !DICompileUnit(language: DW_LANG_C99, file: !2, emissionKind: FullDebug)
            !2 = !DIFile(filename: "lib.eco", directory: "/w")
            !3 = !DISubroutineType(types: !{})
            !4 = distinct !DISubprogram(name: "traced", file: !2, line: 3, type: !3, scopeLine: 3, spFlags: DISPFlagDefinition, unit: !1)
            !7 = !DILocation(line: 4, column: 0, scope: !4)
        )",
        R"(
            define linkonce_odr i32 @traced(i32 %x) !dbg !4 {
            entry:
              %r = add i32 %x, 1, !dbg !7
              ret i32 %r, !dbg !7
            }
            !llvm.module.flags = !{!0}
            !llvm.dbg.cu = !{!1}
            !0 = !{i32 2, !"Debug Info Version", i32 3}
            !1 = distinct !DICompileUnit(language: DW_LANG_C99, file: !2, emissionKind: FullDebug)
            !2 = !DIFile(filename: "lib.eco", directory: "/w")
            !3 = !DISubroutineType(types: !{})
            !4 = distinct !DISubprogram(name: "traced", file: !2, line: 3, type: !3, scopeLine: 3, spFlags: DISPFlagDefinition, unit: !1)
            !7 = !DILocation(line: 9, column: 0, scope: !4)
        )");

    REQUIRE_FALSE(agree(*pair, "traced"));
}

// a subprogram *definition* is distinct, so identity answers nothing about it and the fields it keeps
// outside its operand list have to be asked for by name
TEST_CASE("a subprogram declared on a different line is a divergence", "[odr]")
{
    auto pair = parse_pair(
        R"(
            define linkonce_odr void @traced() !dbg !4 {
            entry:
              ret void
            }
            !llvm.module.flags = !{!0}
            !llvm.dbg.cu = !{!1}
            !0 = !{i32 2, !"Debug Info Version", i32 3}
            !1 = distinct !DICompileUnit(language: DW_LANG_C99, file: !2, emissionKind: FullDebug)
            !2 = !DIFile(filename: "shared.eco", directory: "/w")
            !3 = !DISubroutineType(types: !{})
            !4 = distinct !DISubprogram(name: "traced", file: !2, line: 3, type: !3, scopeLine: 3, spFlags: DISPFlagDefinition, unit: !1)
        )",
        R"(
            define linkonce_odr void @traced() !dbg !4 {
            entry:
              ret void
            }
            !llvm.module.flags = !{!0}
            !llvm.dbg.cu = !{!1}
            !0 = !{i32 2, !"Debug Info Version", i32 3}
            !1 = distinct !DICompileUnit(language: DW_LANG_C99, file: !2, emissionKind: FullDebug)
            !2 = !DIFile(filename: "shared.eco", directory: "/w")
            !3 = !DISubroutineType(types: !{})
            !4 = distinct !DISubprogram(name: "traced", file: !2, line: 11, type: !3, scopeLine: 11, spFlags: DISPFlagDefinition, unit: !1)
        )");

    REQUIRE_FALSE(agree(*pair, "traced"));
}

// **a `#dbg_declare` is not an instruction any more.** Since LLVM 19 a variable's description hangs off the
// instruction it precedes rather than being a call in the block, so an instruction walk alone cannot see one
TEST_CASE("a variable described under a different name is a divergence", "[odr]")
{
    auto pair = parse_pair(
        R"(
            define linkonce_odr void @traced() !dbg !4 {
            entry:
              %slot = alloca i32, align 4
                #dbg_declare(ptr %slot, !6, !DIExpression(), !7)
              ret void
            }
            !llvm.module.flags = !{!0}
            !llvm.dbg.cu = !{!1}
            !0 = !{i32 2, !"Debug Info Version", i32 3}
            !1 = distinct !DICompileUnit(language: DW_LANG_C99, file: !2, emissionKind: FullDebug)
            !2 = !DIFile(filename: "shared.eco", directory: "/w")
            !3 = !DISubroutineType(types: !{})
            !4 = distinct !DISubprogram(name: "traced", file: !2, line: 3, type: !3, scopeLine: 3, spFlags: DISPFlagDefinition, unit: !1)
            !5 = !DIBasicType(name: "int32", size: 32, encoding: DW_ATE_signed)
            !6 = !DILocalVariable(name: "count", scope: !4, file: !2, line: 4, type: !5)
            !7 = !DILocation(line: 4, column: 0, scope: !4)
        )",
        R"(
            define linkonce_odr void @traced() !dbg !4 {
            entry:
              %slot = alloca i32, align 4
                #dbg_declare(ptr %slot, !6, !DIExpression(), !7)
              ret void
            }
            !llvm.module.flags = !{!0}
            !llvm.dbg.cu = !{!1}
            !0 = !{i32 2, !"Debug Info Version", i32 3}
            !1 = distinct !DICompileUnit(language: DW_LANG_C99, file: !2, emissionKind: FullDebug)
            !2 = !DIFile(filename: "shared.eco", directory: "/w")
            !3 = !DISubroutineType(types: !{})
            !4 = distinct !DISubprogram(name: "traced", file: !2, line: 3, type: !3, scopeLine: 3, spFlags: DISPFlagDefinition, unit: !1)
            !5 = !DIBasicType(name: "int32", size: 32, encoding: DW_ATE_signed)
            !6 = !DILocalVariable(name: "total", scope: !4, file: !2, line: 4, type: !5)
            !7 = !DILocation(line: 4, column: 0, scope: !4)
        )");

    REQUIRE_FALSE(agree(*pair, "traced"));
}

TEST_CASE("a differing function attribute is a divergence", "[odr]")
{
    auto pair = parse_pair(
        R"(
            define linkonce_odr void @stop() noreturn {
            entry:
              unreachable
            }
        )",
        R"(
            define linkonce_odr void @stop() {
            entry:
              unreachable
            }
        )");

    REQUIRE_FALSE(agree(*pair, "stop"));
}

TEST_CASE("a differing instruction flag is a divergence", "[odr]")
{
    auto pair = parse_pair(
        R"(
            define linkonce_odr i32 @sum(i32 %a, i32 %b) {
            entry:
              %r = add nsw i32 %a, %b
              ret i32 %r
            }
        )",
        R"(
            define linkonce_odr i32 @sum(i32 %a, i32 %b) {
            entry:
              %r = add i32 %a, %b
              ret i32 %r
            }
        )");

    REQUIRE_FALSE(agree(*pair, "sum"));
}

// the two operands are the same *values*; what differs is which one each use reaches, which a comparison
// by position catches and a comparison by shape alone would not
TEST_CASE("two operands exchanged is a divergence", "[odr]")
{
    auto pair = parse_pair(
        R"(
            define linkonce_odr i32 @div(i32 %a, i32 %b) {
            entry:
              %r = sdiv i32 %a, %b
              ret i32 %r
            }
        )",
        R"(
            define linkonce_odr i32 @div(i32 %a, i32 %b) {
            entry:
              %r = sdiv i32 %b, %a
              ret i32 %r
            }
        )");

    REQUIRE_FALSE(agree(*pair, "div"));
}

TEST_CASE("a difference names where it is", "[odr]")
{
    auto pair = parse_pair(
        R"(
            define linkonce_odr i32 @answer() {
            entry:
              br label %tail
            tail:
              ret i32 41
            }
        )",
        R"(
            define linkonce_odr i32 @answer() {
            entry:
              br label %tail
            tail:
              ret i32 42
            }
        )");

    const auto difference =
        Compiler::LLVM::first_odr_difference(*pair->left_fn("answer"), *pair->right_fn("answer"));

    REQUIRE(difference.has_value());
    REQUIRE(difference->what == "block 1, instruction 0: operand");
    REQUIRE(difference->left != nullptr);
    REQUIRE(difference->right != nullptr);
}
