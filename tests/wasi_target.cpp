#include <catch2/catch_test_macros.hpp>

#include <Compiler/CodegenTarget.h>
#include <Compiler/HostTool.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "subprocess.h"

// WASI is a real cross on every host. the row and the vocabularies are
// always tested; linking needs a sysroot, so the emit case skips when
// none is on this machine

namespace
{

using EchoTests::ProcessResult;
using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("wasi_target", name)
    {};
};

uint32_t read_uleb(const std::vector<uint8_t> &bytes, size_t &i)
{
    uint32_t result = 0;
    int shift = 0;

    while (i < bytes.size()) {
        const uint8_t byte = bytes[i++];
        result |= uint32_t(byte & 0x7f) << shift;

        if ((byte & 0x80) == 0) {
            break;
        }

        shift += 7;
    }

    return result;
}

std::string read_name(const std::vector<uint8_t> &bytes, size_t &i)
{
    const uint32_t len = read_uleb(bytes, i);

    if (i + len > bytes.size()) {
        return {};
    }

    std::string name(reinterpret_cast<const char *>(bytes.data() + i), len);
    i += len;
    return name;
}

struct WasmSurface
{
    std::vector<std::string> exports;
    std::vector<std::pair<std::string, std::string>> imports;
};

WasmSurface parse_wasm_surface(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());

    REQUIRE(bytes.size() >= 8);
    REQUIRE(bytes[0] == 0x00);
    REQUIRE(bytes[1] == 'a');
    REQUIRE(bytes[2] == 's');
    REQUIRE(bytes[3] == 'm');

    WasmSurface surface;
    size_t i = 8;

    while (i < bytes.size()) {
        const uint8_t id = bytes[i++];
        const uint32_t size = read_uleb(bytes, i);
        const size_t end = i + size;

        if (end > bytes.size()) {
            break;
        }

        if (id == 2) {
            const uint32_t count = read_uleb(bytes, i);

            for (uint32_t n = 0; n < count && i < end; n++) {
                const std::string module = read_name(bytes, i);
                const std::string name = read_name(bytes, i);
                surface.imports.push_back({ module, name });

                if (i >= end) {
                    break;
                }

                const uint8_t kind = bytes[i++];

                if (kind == 0) {
                    (void)read_uleb(bytes, i);
                }
                else if (kind == 1 || kind == 2) {
                    const uint32_t flags = read_uleb(bytes, i);
                    (void)read_uleb(bytes, i);

                    if (flags & 1) {
                        (void)read_uleb(bytes, i);
                    }
                }
                else if (kind == 3) {
                    (void)read_uleb(bytes, i);
                    if (i < end) {
                        i += 1;
                    }
                }
            }
        }
        else if (id == 7) {
            const uint32_t count = read_uleb(bytes, i);

            for (uint32_t n = 0; n < count && i < end; n++) {
                surface.exports.push_back(read_name(bytes, i));

                if (i >= end) {
                    break;
                }

                i += 1;
                (void)read_uleb(bytes, i);
            }
        }

        i = end;
    }

    return surface;
}

bool has_name(const std::vector<std::string> &names, const std::string &wanted)
{
    return std::find(names.begin(), names.end(), wanted) != names.end();
}

bool has_import(
    const std::vector<std::pair<std::string, std::string>> &imports,
    const std::string &module,
    const std::string &name
)
{
    for (const auto &entry : imports) {
        if (entry.first == module && entry.second == name) {
            return true;
        }
    }

    return false;
}

};

TEST_CASE("echoc build --target-os wasi emits a wasm module", "[target][wasi]")
{
    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    ScopedProject project("hello");
    write_file(project.root() / "hello.eco", "echo 1;\n");

    const ProcessResult result = project.echoc(
        "build --no-stdlib --target-os wasi --target-arch wasm32 -p ir -o hello.wasm hello.eco");

    REQUIRE(result.exit_code == 0);
    REQUIRE(std::filesystem::is_regular_file(project.root() / "hello.wasm"));
    REQUIRE(result.output.find("__main_argc_argv") != std::string::npos);
}

TEST_CASE("echoc build --target-os wasi with stdlib emits a wasm module", "[target][wasi]")
{
    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    ScopedProject project("stdlib");
    write_file(project.root() / "hello.eco", "echo 1;\n");

    const ProcessResult result = project.echoc(
        "build --target-os wasi -o hello.wasm hello.eco");

    REQUIRE(result.exit_code == 0);
    REQUIRE(std::filesystem::is_regular_file(project.root() / "hello.wasm"));
}

TEST_CASE("echoc build --target-os wasi allocates a class box", "[target][wasi]")
{
    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    ScopedProject project("class_box");
    write_file(
        project.root() / "hello.eco",
        "class Point\n"
        "{\n"
        "    int32 $x;\n"
        "    constructor(int32 $x) { $this->x = $x; }\n"
        "}\n"
        "Point $p = Point(1);\n"
        "echo $p->x;\n");

    const ProcessResult result = project.echoc(
        "build --target-os wasi -o hello.wasm hello.eco");

    REQUIRE(result.exit_code == 0);
    REQUIRE(std::filesystem::is_regular_file(project.root() / "hello.wasm"));
}

TEST_CASE("a lib target is a wasi reactor with exports and host imports", "[target][wasi]")
{
    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    ScopedProject project("reactor");
    write_file(
        project.root() / "module.eco",
        "#[module: \"reactor\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"mandelbrot\" }]\n");
    write_file(
        project.root() / "src/lib.eco",
        "#[wasm: export \"memory\"]\n"
        "#[wasm: import \"env\"]\n"
        "extern {\n"
        "    function host_log(ptr<const uint8> $text, usize $len) : void;\n"
        "}\n"
        "\n"
        "#[export]\n"
        "public function render() : int32\n"
        "{\n"
        "    host_log(null, 0);\n"
        "    return 1;\n"
        "}\n");

    const ProcessResult ir = project.echoc(
        "build --no-stdlib --target-os wasi --target-arch wasm32 -p ir");
    INFO(ir.output);
    REQUIRE(ir.exit_code == 0);
    REQUIRE(ir.output.find("wasm-export-name") != std::string::npos);
    REQUIRE(ir.output.find("\"render\"") != std::string::npos);
    REQUIRE(ir.output.find("wasm-import-module") != std::string::npos);
    REQUIRE(ir.output.find("\"env\"") != std::string::npos);

    const ProcessResult built = project.echoc(
        "build --no-stdlib --target-os wasi --target-arch wasm32");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const std::filesystem::path wasm = project.root() / "ecobuild" / "mandelbrot.wasm";
    REQUIRE(std::filesystem::is_regular_file(wasm));

    const WasmSurface surface = parse_wasm_surface(wasm);
    REQUIRE(has_name(surface.exports, "render"));
    REQUIRE(has_name(surface.exports, "_initialize"));
    REQUIRE(has_name(surface.exports, "memory"));
    REQUIRE_FALSE(has_name(surface.exports, "_start"));
    REQUIRE(has_import(surface.imports, "env", "host_log"));
}

TEST_CASE("a suffix-less -o on wasi becomes .wasm", "[target][wasi]")
{
    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    ScopedProject project("suffix");
    write_file(project.root() / "hello.eco", "echo 1;\n");

    const ProcessResult result = project.echoc(
        "build --no-stdlib --target-os wasi --target-arch wasm32 -o hello hello.eco");
    INFO(result.output);
    REQUIRE(result.exit_code == 0);
    REQUIRE(std::filesystem::is_regular_file(project.root() / "hello.wasm"));
    REQUIRE_FALSE(std::filesystem::is_regular_file(project.root() / "hello.exe"));
}

TEST_CASE("wasi static init is a non-atomic once", "[target][wasi]")
{
    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    ScopedProject project("static_once");
    write_file(
        project.root() / "hello.eco",
        "struct Owns\n"
        "{\n"
        "    int32 $n;\n"
        "    destructor() {}\n"
        "}\n"
        "struct Holder\n"
        "{\n"
        "    static Owns $x = Owns(3);\n"
        "}\n"
        "echo Holder::$x->n;\n");

    const ProcessResult ir = project.echoc(
        "build --no-stdlib --target-os wasi --target-arch wasm32 -p ir -o hello.wasm hello.eco");
    INFO(ir.output);
    REQUIRE(ir.exit_code == 0);
    REQUIRE(ir.output.find("__eco_static_once") != std::string::npos);
    REQUIRE(ir.output.find("pthread_self") == std::string::npos);
    REQUIRE(ir.output.find("cmpxchg") == std::string::npos);
    REQUIRE(ir.output.find("sched_yield") == std::string::npos);
}

TEST_CASE("wasi allocation counter is a non-atomic add", "[target][wasi]")
{
    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    ScopedProject project("alloc_once");
    write_file(
        project.root() / "hello.eco",
        "class Point\n"
        "{\n"
        "    int32 $x;\n"
        "    constructor(int32 $x) { $this->x = $x; }\n"
        "}\n"
        "Point $p = Point(1);\n"
        "echo $p->x;\n");

    const ProcessResult ir = project.echoc(
        "build --no-stdlib --track-allocations --target-os wasi --target-arch wasm32 "
        "-p ir -o hello.wasm hello.eco");
    INFO(ir.output);
    REQUIRE(ir.exit_code == 0);
    REQUIRE(ir.output.find("atomicrmw") == std::string::npos);
}

TEST_CASE("wasm export memory does not attach to the next function", "[target][wasi]")
{
    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    ScopedProject project("memory_prefix");
    write_file(
        project.root() / "module.eco",
        "#[module: \"memprefix\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"plot\" }]\n");
    write_file(
        project.root() / "src/lib.eco",
        "#[wasm: export \"memory\"]\n"
        "#[export]\n"
        "public function render() : int32\n"
        "{\n"
        "    return 1;\n"
        "}\n");

    const ProcessResult built = project.echoc(
        "build --no-stdlib --target-os wasi --target-arch wasm32");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);
    REQUIRE(built.output.find("object export is not a function") == std::string::npos);

    const std::filesystem::path wasm = project.root() / "ecobuild" / "plot.wasm";
    REQUIRE(std::filesystem::is_regular_file(wasm));
    const WasmSurface surface = parse_wasm_surface(wasm);
    REQUIRE(has_name(surface.exports, "render"));
    REQUIRE(has_name(surface.exports, "memory"));
}

TEST_CASE("codegen link args pass --export-memory when asked", "[target][wasi]")
{
    Compiler::CodegenTarget target;
    target.triple = Compiler::k_wasi_triple;
    target.wasm = true;
    target.pointer_bytes = 4;
    std::vector<std::string> argv = { "clang" };
    std::string error;

    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    REQUIRE(Compiler::append_codegen_link_args(argv, target, true, error));
    REQUIRE(std::find(argv.begin(), argv.end(), "-Wl,--export-memory") != argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "-mexec-model=reactor") == argv.end());

    argv = { "clang" };
    target.exec_model = Compiler::ExecModel::t_library;
    REQUIRE(Compiler::append_codegen_link_args(argv, target, false, error));
    REQUIRE(std::find(argv.begin(), argv.end(), "-Wl,--export-memory") == argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "-mexec-model=reactor") != argv.end());
}

TEST_CASE("codegen target args for wasi come from append_wasi_target_args", "[target][wasi]")
{
    Compiler::CodegenTarget target;
    target.triple = Compiler::k_wasi_triple;
    target.wasm = true;
    target.pointer_bytes = 4;
    std::vector<std::string> argv = { "clang" };
    std::string error;

    if (Compiler::wasi_sysroot().empty()) {
        REQUIRE_FALSE(Compiler::append_codegen_target_args(argv, target, error));
        REQUIRE(error.find("WASI SDK") != std::string::npos);
        return;
    }

    REQUIRE(Compiler::append_codegen_target_args(argv, target, error));
    REQUIRE(error.empty());
    REQUIRE(std::find(argv.begin(), argv.end(), "-target") != argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), target.triple) != argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "--sysroot") != argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "-isysroot") == argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "-fms-runtime-lib=static") == argv.end());
}

TEST_CASE("an extern block import attribute applies to every function", "[target][wasi]")
{
    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    ScopedProject project("block_import");
    write_file(project.root() / "host.eco",
        "#[wasm: export \"memory\"]\n"
        "#[wasm: import \"env\"]\n"
        "extern {\n"
        "    function host_a() : void;\n"
        "    function host_b() : void;\n"
        "}\n"
        "\n"
        "#[export]\n"
        "function skip() : int32\n"
        "{\n"
        "    host_a();\n"
        "    host_b();\n"
        "    return 1;\n"
        "}\n");

    const ProcessResult ir = project.echoc(
        "build --no-stdlib --target-os wasi --target-arch wasm32 -p ir -o host.wasm host.eco");
    INFO(ir.output);
    REQUIRE(ir.exit_code == 0);
    REQUIRE(ir.output.find("wasm-import-module") != std::string::npos);
    REQUIRE(ir.output.find("\"env\"") != std::string::npos);
    REQUIRE(ir.output.find("wasm-import-name") != std::string::npos);
    REQUIRE(ir.output.find("\"host_a\"") != std::string::npos);
    REQUIRE(ir.output.find("\"host_b\"") != std::string::npos);
}
