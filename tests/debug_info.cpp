#include <catch2/catch_test_macros.hpp>

#include "subprocess.h"

#include <filesystem>
#include <fstream>
#include <string>

// what `-g` promises beyond "some metadata appeared", which is all an IR golden can see.
//
// three claims, and each of them fails in a way the corpus cannot catch:
//
//  - **the debug-info verifier passes over every unit.** LLVMCompiler::compile_bundle verifies only the
//    main module, so a malformed scope chain in a library unit reaches the object writer unchallenged.
//    DebugInfoCodegen::finalize_all runs the verifier per unit under a debug build, and this is what
//    makes that observable from outside.
//  - **an ODR-shared body's debug info is a pure function of its declaration.** A t_odr_shared body is
//    emitted into every unit that references it, so anything in it taken from the ambient unit - the
//    file being lowered, whether *this* module owns a token - makes two descriptions of one symbol.
//    verify_odr_consistency compares them and throws; a multi-module `-g` build is the only thing that
//    exercises it.
//  - **`-g` changes the cached object**, so it has to change the cache key. Without that entry a debug
//    session is served a stripped artifact for every cached module and every breakpoint in it silently
//    fails to resolve.
//
// subprocess tests rather than corpus goldens for module_cache.cpp's reasons: two of them need more than
// one `echoc` invocation, and a cache key folds in the LLVM version and the host triple so no digest is
// comparable across machines.

namespace fs = std::filesystem;

namespace
{

// the shared process primitives - see subprocess.h
using EchoTests::ProcessResult;
using EchoTests::quoted;
using EchoTests::run_capturing;
using EchoTests::write_file;

// the scratch project, under this suite's own directory
class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("debug_info", name)
    {};
};

};

TEST_CASE("a -g build of a program using generics, classes and closures verifies", "[debuginfo]")
{
    // every shape whose debug info is built somewhere other than the obvious place: a class is a pointer
    // to a heap box whose payload fields are hoisted to their box offsets, a generic instantiation takes
    // its template's file through function_file_map, a closure is a declaration nested in an expression
    // that no file-root walk reaches, and a `foreach` mints a cursor no source file spells
    ScopedProject project("verifies");

    write_file(project.root() / "app.eco", R"(
class Counter
{
    int32 $count;
}

struct Box<T>
{
    T $item;

    function get() : T { return $this->item; }
}

Counter $c = Counter(0);
Box<int32> $b = Box<int32>(41);
array<int32> $xs = [1, 2, 3];

foreach ($xs as $x) {
    $c->count = $c->count + $x;
}

int32 $boxed = $b->get();

echo $c->count + $boxed;
)");

    // --track-allocations because the corpus does, and because it puts the emitted allocation runtime -
    // which carries no subprogram and must therefore carry no locations - into the build
    const ProcessResult result = project.echoc("build -g --track-allocations -o app app.eco");

    INFO(result.output);
    REQUIRE(result.exit_code == 0);

    // the per-unit verifier and the ODR check both report by throwing, which reaches stderr as an
    // uncaught exception rather than as a diagnostic - so this is what a failure of either looks like
    REQUIRE(result.output.find("verification failed") == std::string::npos);
    REQUIRE(result.output.find("definitions differ") == std::string::npos);

    const ProcessResult run = run_capturing(quoted(project.root() / "app") + " 2>&1");

    INFO(run.output);
    REQUIRE(run.output.find("47") != std::string::npos);
}

TEST_CASE("an ODR-shared body carries the same debug info in every unit", "[debuginfo]")
{
    // **the strongest single check in the feature.** A generic declared in a library and instantiated in
    // the application is emitted into both units, and each mints its own DISubprogram, DILocations and
    // DITypes for it. verify_odr_consistency renders both and refuses a difference - so this passing is
    // the claim that nothing in that body was read from the ambient unit.
    //
    // it is what caught the emitted runtime inheriting a statement's location, a struct's DIType naming
    // whichever module was being lowered, and `$this` counting as artificial in one object and not the
    // next
    ScopedProject project("odr");

    write_file(project.root() / "lib" / "module.eco", R"(
#[module: "lib"]
#[sources: "*.eco"]
)");

    write_file(project.root() / "lib" / "pair.eco", R"(
public struct Pair<T>
{
    T $a;
    T $b;

    function sum() : T { return $this->a + $this->b; }
}
)");

    write_file(project.root() / "app" / "module.eco", R"(
#[module: "main"]
#[sources: "*.eco"]
#[depends: "../lib"]
)");

    write_file(project.root() / "app" / "main.eco", R"(
Pair<int32> $p = Pair<int32>(20, 22);
int32 $total = $p->sum();

echo $total;
)");

    const ProcessResult result = project.echoc(
        "build -g --track-allocations -m " + quoted(project.root() / "lib") + " --build-dir "
            + quoted(project.root() / "cache") + " -o app main.eco",
        project.root() / "app");

    INFO(result.output);
    REQUIRE(result.output.find("definitions differ") == std::string::npos);
    REQUIRE(result.output.find("verification failed") == std::string::npos);
    REQUIRE(result.exit_code == 0);
}

TEST_CASE("-g is part of the module cache key", "[debuginfo]")
{
    // an object carrying DWARF is not the object beside it that does not, so serving one for the other is
    // the unsound-cache case rather than a missed optimization - and it fails silently, because a
    // stripped object is a perfectly valid one and only the breakpoints go missing
    ScopedProject project("cache_key");

    write_file(project.root() / "lib" / "module.eco", R"(
#[module: "lib"]
#[sources: "*.eco"]
)");

    write_file(project.root() / "lib" / "answer.eco", R"(
public function answer() : int32 { return 42; }
)");

    write_file(project.root() / "app" / "module.eco", R"(
#[module: "main"]
#[sources: "*.eco"]
#[depends: "../lib"]
)");

    write_file(project.root() / "app" / "main.eco", "echo answer();\n");

    const std::string common = " --track-allocations --explain cache -m " + quoted(project.root() / "lib")
        + " --build-dir " + quoted(project.root() / "cache") + " -o app main.eco";

    const ProcessResult without_g = project.echoc("build" + common, project.root() / "app");
    const ProcessResult with_g = project.echoc("build -g" + common, project.root() / "app");

    INFO(without_g.output);
    INFO(with_g.output);

    REQUIRE(without_g.exit_code == 0);
    REQUIRE(with_g.exit_code == 0);

    // the digests themselves are host-specific, so what is asserted is that they differ - and that the
    // `-g` build could not have been served the object the first one stored
    REQUIRE(without_g.output.find("[cache]") != std::string::npos);
    REQUIRE(with_g.output.find("[cache]") != std::string::npos);
    REQUIRE(without_g.output != with_g.output);
}

TEST_CASE("a synthesized teardown is described at the type it belongs to", "[debuginfo]")
{
    // **the file and the line have to come from one place, and nothing else checks that they did.** A
    // `$deinit` is built out of virtual tokens that inherit the type's file, so the token names both
    // the file and the line. The two agree because OwnershipPass writes the declaration into the file
    // that declares the type: while it used the module's first file instead, a stdlib type's teardown
    // claimed `arr.eco` at a line belonging to `string.eco`, and a debugger asked to step into it
    // opened the wrong file or none.
    //
    // Two files in the module is what makes it visible, and the corpus cannot: it has no way to read
    // DWARF, and a `CHECK` over rendered IR cannot follow `file: !13` to the DIFile it names. So this
    // parses the metadata reference and resolves it
    ScopedProject project("teardown_site");

    write_file(project.root() / "lib" / "module.eco", R"(
#[module: "lib"]
#[sources: "*.eco"]
)");

    // the first file by name, and deliberately not where the owning type lives
    write_file(project.root() / "lib" / "a_first.eco", R"(
public function unrelated() : int32 { return 1; }
)");

    write_file(project.root() / "lib" / "z_journal.eco", R"(
public struct Journal
{
    private array<int32> $entries;

    constructor()
    {
        $this->entries = array<int32>();
    }

    function add(int32 $v) : void { $this->entries->push($v); }

    const function count() : usize { return $this->entries->count(); }
}
)");

    write_file(project.root() / "app" / "main.eco", R"(
Journal $j = Journal();
$j->add(7);
echo $j->count() + unrelated();
)");

    const ProcessResult result = project.echoc(
        "build -g --track-allocations -p ir -m " + quoted(project.root() / "lib") + " -o app main.eco",
        project.root() / "app");

    INFO(result.output);
    REQUIRE(result.exit_code == 0);

    // the subprogram for `Journal`'s teardown, and the metadata node it names as its file
    const size_t at = result.output.find("linkageName: \"_M7Journal_$deinit");
    REQUIRE(at != std::string::npos);

    const size_t line_start = result.output.rfind("!DISubprogram", at);
    const size_t line_end = result.output.find('\n', at);
    REQUIRE(line_start != std::string::npos);

    const std::string subprogram = result.output.substr(line_start, line_end - line_start);

    INFO(subprogram);

    const size_t file_at = subprogram.find("file: !");
    REQUIRE(file_at != std::string::npos);

    const size_t id_start = file_at + std::string("file: ").size();
    const size_t id_end = subprogram.find(',', id_start);
    const std::string file_id = subprogram.substr(id_start, id_end - id_start);

    // and it resolves to the file that declares the type, not to the module's first one
    const size_t file_node = result.output.find(file_id + " = !DIFile(");
    REQUIRE(file_node != std::string::npos);

    const std::string file_line = result.output.substr(file_node, result.output.find('\n', file_node) - file_node);

    INFO(file_line);
    REQUIRE(file_line.find("z_journal.eco") != std::string::npos);

    // the scope is that same node, so a DILocation inside the body cannot name a third file
    REQUIRE(subprogram.find("scope: " + file_id + ",") != std::string::npos);
}
