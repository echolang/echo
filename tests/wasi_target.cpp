#include <catch2/catch_test_macros.hpp>

#include <Compiler/CodegenTarget.h>
#include <Compiler/HostTool.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
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
    REQUIRE(ir.output.find("wasm-export-name") != std::string::npos);
    REQUIRE(ir.output.find("\"render\"") != std::string::npos);
    REQUIRE(ir.output.find("wasm-import-module") != std::string::npos);
    REQUIRE(ir.output.find("\"env\"") != std::string::npos);

    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    REQUIRE(ir.exit_code == 0);

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
    REQUIRE(ir.output.find("__eco_static_once") != std::string::npos);
    REQUIRE(ir.output.find("pthread_self") == std::string::npos);
    REQUIRE(ir.output.find("cmpxchg") == std::string::npos);
    REQUIRE(ir.output.find("sched_yield") == std::string::npos);
    if (!Compiler::wasi_sysroot().empty()) {
        REQUIRE(ir.exit_code == 0);
    }
}

TEST_CASE("wasi allocation counter is a non-atomic add", "[target][wasi]")
{
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
    REQUIRE(ir.output.find("atomicrmw") == std::string::npos);
    if (!Compiler::wasi_sysroot().empty()) {
        REQUIRE(ir.exit_code == 0);
    }
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
    REQUIRE(std::find(argv.begin(), argv.end(), "-fuse-ld=lld") == argv.end());

    bool fused_wasm_ld = false;
    for (const std::string &arg : argv) {
        if (arg.rfind("-fuse-ld=", 0) == 0 && arg.find("wasm-ld") != std::string::npos) {
            fused_wasm_ld = true;
        }
    }
    REQUIRE(fused_wasm_ld);
    REQUIRE_FALSE(Compiler::wasi_wasm_ld().empty());
}

TEST_CASE("an extern block import attribute applies to every function", "[target][wasi]")
{
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
    REQUIRE(ir.output.find("wasm-import-module") != std::string::npos);
    REQUIRE(ir.output.find("\"env\"") != std::string::npos);
    REQUIRE(ir.output.find("wasm-import-name") != std::string::npos);
    REQUIRE(ir.output.find("\"host_a\"") != std::string::npos);
    REQUIRE(ir.output.find("\"host_b\"") != std::string::npos);
    if (!Compiler::wasi_sysroot().empty()) {
        REQUIRE(ir.exit_code == 0);
    }
}

ProcessResult run_wasmtime(
    const std::filesystem::path &wasm,
    const std::filesystem::path &workdir,
    const std::vector<std::string> &extra = {})
{
    std::vector<std::string> argv = { "wasmtime" };
    argv.insert(argv.end(), extra.begin(), extra.end());
    argv.push_back(wasm.string());
    return EchoTests::run_process(argv, EchoTests::k_default_timeout_ms, workdir);
}

void require_wasmtime(const ProcessResult &ran)
{
    if (ran.exit_code != 127) {
        return;
    }

    if (std::getenv("GITHUB_ACTIONS") != nullptr) {
        FAIL("wasmtime not found on CI");
    }

    SKIP("wasmtime not found");
}

bool define_line_has(
    const std::string &ir,
    const std::string &name,
    const std::string &attr)
{
    std::istringstream in(ir);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("define ") != std::string::npos
            && line.find("@" + name) != std::string::npos
            && line.find(attr) != std::string::npos) {
            return true;
        }
    }

    return false;
}

TEST_CASE("exported narrow integers carry signext and zeroext", "[target][wasi]")
{
    ScopedProject project("c_abi_ext");
    write_file(
        project.root() / "lib.eco",
        "#[export]\n"
        "function flag(int8 $n) : bool\n"
        "{\n"
        "    return $n != 0;\n"
        "}\n");

    const ProcessResult ir = project.echoc(
        "build --no-stdlib --target-os wasi --target-arch wasm32 -p ir -o lib.wasm lib.eco");
    INFO(ir.output);
    REQUIRE(define_line_has(ir.output, "flag", "signext"));
    REQUIRE(define_line_has(ir.output, "flag", "zeroext"));
}

TEST_CASE("a C function pointer call and callback carry signext", "[target][wasi]")
{
    ScopedProject project("c_abi_indirect");
    write_file(
        project.root() / "lib.eco",
        "function cb(int8 $n) : bool\n"
        "{\n"
        "    return $n != 0;\n"
        "}\n"
        "\n"
        "#[export]\n"
        "function run(extern function<bool(int8)> $f) : bool\n"
        "{\n"
        "    return $f(1);\n"
        "}\n"
        "\n"
        "#[export]\n"
        "function callback() : extern function<bool(int8)>\n"
        "{\n"
        "    return &cb;\n"
        "}\n");

    const ProcessResult ir = project.echoc(
        "build --no-stdlib --target-os wasi --target-arch wasm32 -p ir -o lib.wasm lib.eco");
    INFO(ir.output);
    REQUIRE(define_line_has(ir.output, "__eco_c_adapt.", "signext"));
    REQUIRE(define_line_has(ir.output, "__eco_c_adapt.", "zeroext"));
    const bool call_extends = ir.output.find("call zeroext i1 %") != std::string::npos;
    REQUIRE(call_extends);
}

TEST_CASE("a cross-module C callback is a unit-local adapter", "[target][wasi]")
{
    ScopedProject project("c_abi_cross_module");
    write_file(
        project.root() / "lib/module.eco",
        "#[module: \"cback\"]\n"
        "#[sources: \"*.eco\"]\n");
    write_file(
        project.root() / "lib/cb.eco",
        "public function cb(int8 $n) : bool\n"
        "{\n"
        "    return $n != 0;\n"
        "}\n"
        "\n"
        "#[inline]\n"
        "public function flag(int8 $n) : bool\n"
        "{\n"
        "    return $n != 0;\n"
        "}\n");
    write_file(
        project.root() / "app.eco",
        "#[export]\n"
        "function callback() : extern function<bool(int8)>\n"
        "{\n"
        "    return &cback::cb;\n"
        "}\n"
        "\n"
        "#[export]\n"
        "function inline_cb() : extern function<bool(int8)>\n"
        "{\n"
        "    return &cback::flag;\n"
        "}\n");

    const ProcessResult ir = project.echoc(
        "build --no-stdlib --target-os wasi --target-arch wasm32 -p ir -o app.wasm "
        "-m " + EchoTests::quoted(project.root() / "lib") + " app.eco");
    INFO(ir.output);
    REQUIRE(ir.exit_code == 0);
    REQUIRE(ir.output.find("ODR") == std::string::npos);
    REQUIRE(define_line_has(ir.output, "__eco_c_adapt.", "zeroext"));
    REQUIRE(define_line_has(ir.output, "__eco_c_adapt.", "signext"));
}

TEST_CASE("wasmtime runs a wasi command that prints 1", "[target][wasi]")
{
    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    ScopedProject project("wasmtime_echo");
    write_file(project.root() / "hello.eco", "echo 1;\n");

    const ProcessResult built = project.echoc(
        "build --no-stdlib --target-os wasi -o hello.wasm hello.eco");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const ProcessResult ran = run_wasmtime(project.root() / "hello.wasm", project.root());
    INFO(ran.output);
    require_wasmtime(ran);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(EchoTests::output_has_line(ran.output, "1"));
}

TEST_CASE("wasmtime runs stdlib io, time and pid on wasi", "[target][wasi]")
{
    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    ScopedProject project("wasmtime_stdlib");
    write_file(
        project.root() / "hello.eco",
        "use std::io;\n"
        "use std::time;\n"
        "use std::env;\n"
        "\n"
        "echo 'pid';\n"
        "echo std::env::pid();\n"
        "match (io::open('nope.txt', io::filemode::read)) {\n"
        "    .ok($f) => { echo 'missing'; echo 0; }\n"
        "    .error($e) => { echo 'missing'; echo $e->missing(); }\n"
        "}\n"
        "match (io::create('listed.txt')) {\n"
        "    .ok($f) => { match ($f->write('ok')) { .ok($n) => {} .error($e) => {} } }\n"
        "    .error($e) => {}\n"
        "}\n"
        "match (io::mkdir('sub')) { .ok($ok) => {} .error($e) => {} }\n"
        "io::dir $listing = guard io::opendir('.') else {\n"
        "    echo 'file';\n"
        "    echo 0;\n"
        "    echo 'dir';\n"
        "    echo 0;\n"
        "    return 1;\n"
        "};\n"
        "bool $file = false;\n"
        "bool $dir = false;\n"
        "foreach ($listing as $e) {\n"
        "    if ($e->name() == 'listed.txt') {\n"
        "        $file = $e->is_file();\n"
        "    }\n"
        "    if ($e->name() == 'sub') {\n"
        "        $dir = $e->is_directory();\n"
        "    }\n"
        "}\n"
        "echo 'file';\n"
        "echo $file;\n"
        "echo 'dir';\n"
        "echo $dir;\n"
        "time::instant $t = time::instant::now();\n"
        "echo 'nanos';\n"
        "echo $t->elapsed()->as_nanos() >= 0;\n");

    const ProcessResult built = project.echoc("build --target-os wasi -o hello.wasm hello.eco");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const ProcessResult ran = run_wasmtime(
        project.root() / "hello.wasm",
        project.root(),
        { "--dir=." });
    INFO(ran.output);
    require_wasmtime(ran);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("pid") != std::string::npos);
    REQUIRE(ran.output.find("missing") != std::string::npos);
    REQUIRE(ran.output.find("file") != std::string::npos);
    REQUIRE(ran.output.find("dir") != std::string::npos);
    REQUIRE(std::filesystem::is_regular_file(project.root() / "listed.txt"));

    auto after = [&](const char *label) -> std::string {
        const std::string needle = std::string(label) + "\n";
        const auto pos = ran.output.find(needle);
        if (pos == std::string::npos) {
            return {};
        }
        const auto start = pos + needle.size();
        const auto end = ran.output.find('\n', start);
        return ran.output.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };

    REQUIRE(after("pid") == "1");
    REQUIRE(after("missing") == "1");
    REQUIRE(after("file") == "1");
    REQUIRE(after("dir") == "1");
    REQUIRE(after("nanos") == "1");
}

TEST_CASE("wasmtime die writes the message and exits 1", "[target][wasi]")
{
    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    ScopedProject project("wasmtime_die");
    write_file(project.root() / "die.eco", "die('x');\n");

    const ProcessResult built = project.echoc(
        "build --target-os wasi -o die.wasm die.eco");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const ProcessResult ran = run_wasmtime(project.root() / "die.wasm", project.root());
    INFO(ran.output);
    require_wasmtime(ran);
    REQUIRE(ran.exit_code == 1);
    REQUIRE(ran.output.find("fatal error: x") != std::string::npos);
}

TEST_CASE("MAX_ISIZE is the 32-bit maximum on wasm32", "[target][wasi]")
{
    ScopedProject project("wasm_isize");
    write_file(
        project.root() / "hello.eco",
        "echo std::math::MAX_ISIZE;\n");

    const ProcessResult ir = project.echoc(
        "build --target-os wasi -p ir -o hello.wasm hello.eco");
    INFO(ir.output);
    REQUIRE(ir.output.find("cannot") == std::string::npos);
    REQUIRE(ir.output.find("2147483647") != std::string::npos);
}

TEST_CASE("a reactor with the stdlib echoes an int from an export", "[target][wasi]")
{
    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    ScopedProject project("reactor_echo");
    write_file(
        project.root() / "module.eco",
        "#[module: \"reactor\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"echoer\" }]\n");
    write_file(
        project.root() / "src/lib.eco",
        "#[export]\n"
        "public function echo_one() : void\n"
        "{\n"
        "    echo 1;\n"
        "}\n");

    const ProcessResult built = project.echoc("build --target-os wasi");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const std::filesystem::path wasm = project.root() / "ecobuild" / "echoer.wasm";
    REQUIRE(std::filesystem::is_regular_file(wasm));

    const ProcessResult ran = run_wasmtime(
        wasm, project.root(), { "--invoke", "echo_one" });
    INFO(ran.output);
    require_wasmtime(ran);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(EchoTests::output_has_line(ran.output, "1"));
}

TEST_CASE("wasmtime runs a host import through --preload", "[target][wasi]")
{
    if (Compiler::wasi_sysroot().empty()) {
        SKIP("WASI SDK not found");
    }

    ScopedProject project("import_preload");
    write_file(
        project.root() / "module.eco",
        "#[module: \"reactor\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"calls_host\" }]\n");
    write_file(
        project.root() / "src/lib.eco",
        "#[wasm: import \"env\"]\n"
        "extern {\n"
        "    function host_log(ptr<const uint8> $text, usize $len) : void;\n"
        "}\n"
        "\n"
        "#[export]\n"
        "public function render() : int32\n"
        "{\n"
        "    host_log(null, 0);\n"
        "    return 7;\n"
        "}\n");
    write_file(
        project.root() / "env.wat",
        "(module\n"
        "  (func (export \"host_log\") (param i32 i32))\n"
        ")\n");

    const ProcessResult built = project.echoc(
        "build --no-stdlib --target-os wasi --target-arch wasm32");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const std::filesystem::path wasm = project.root() / "ecobuild" / "calls_host.wasm";
    REQUIRE(std::filesystem::is_regular_file(wasm));

    const ProcessResult ran = run_wasmtime(
        wasm,
        project.root(),
        { "--preload", "env=" + (project.root() / "env.wat").string(), "--invoke", "render" });
    INFO(ran.output);
    require_wasmtime(ran);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(EchoTests::output_has_line(ran.output, "7"));
}
