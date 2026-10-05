#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "subprocess.h"

// entry-unit partitions: per-file objects plus `__shared`, keyed on a structural
// IR hash. subprocess tests because a one-file corpus cannot catch a cross-file
// link hole. Windows does not partition

namespace fs = std::filesystem;

#if !defined(_WIN32)
namespace
{

using EchoTests::ProcessResult;
using EchoTests::quoted;
using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("entry_partitions", name)
    {};
};

std::vector<std::string> partition_object_names(const fs::path &scratch)
{
    std::vector<std::string> names;
    std::error_code ec;

    if (!fs::is_directory(scratch, ec)) {
        return names;
    }

    for (const auto &entry : fs::directory_iterator(scratch, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() > 4 && name.compare(0, 2, "p.") == 0 && entry.path().extension() == ".o") {
            names.push_back(name);
        }
    }

    std::sort(names.begin(), names.end());
    return names;
}

};

TEST_CASE("an entry unit's partition objects reuse across a one-file edit", "[cache][partition]")
{
    // editing left.eco must miss that file's partition and hit the others, including
    // after a new unused function appears in it
    ScopedProject project("entry_partitions");

    write_file(project.root() / "module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    write_file(project.root() / "src" / "left.eco",
        "function left() : int32\n"
        "{\n"
        "    return 20;\n"
        "}\n");

    write_file(project.root() / "src" / "right.eco",
        "function right() : int32\n"
        "{\n"
        "    return 22;\n"
        "}\n");

    write_file(project.root() / "src" / "main.eco",
        "echo left() + right();\n");

    const fs::path cache = project.root() / "cache";
    const std::string args = "build -o out --build-dir " + quoted(cache);

    const ProcessResult first = project.echoc(args);
    INFO(first.output);
    REQUIRE(first.exit_code == 0);

    const ProcessResult ran = EchoTests::run_binary(project.root() / "out");
    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("42") != std::string::npos);

    const std::vector<std::string> before = partition_object_names(cache / "app" / "scratch");
    REQUIRE(before.size() >= 2);

    write_file(project.root() / "src" / "left.eco",
        "function left() : int32\n"
        "{\n"
        "    return 21;\n"
        "}\n"
        "\n"
        "function unused() : int32\n"
        "{\n"
        "    return 0;\n"
        "}\n");

    const ProcessResult second = project.echoc(args);
    INFO(second.output);
    REQUIRE(second.exit_code == 0);

    const ProcessResult ran_again = EchoTests::run_binary(project.root() / "out");
    REQUIRE(ran_again.exit_code == 0);
    REQUIRE(ran_again.output.find("43") != std::string::npos);

    const std::vector<std::string> after = partition_object_names(cache / "app" / "scratch");
    REQUIRE(after.size() >= 2);

    bool reused = false;
    for (const std::string &name : before) {
        if (std::find(after.begin(), after.end(), name) != after.end()) {
            reused = true;
            break;
        }
    }

    REQUIRE(reused);

    bool replaced = false;
    for (const std::string &name : after) {
        if (std::find(before.begin(), before.end(), name) == before.end()) {
            replaced = true;
            break;
        }
    }

    REQUIRE(replaced);
}

TEST_CASE("an entry unit's partition keys are stable across identical rebuilds", "[cache][partition]")
{
    ScopedProject project("entry_partition_stable");

    write_file(project.root() / "module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    write_file(project.root() / "src" / "left.eco",
        "function left() : int32\n"
        "{\n"
        "    array<int32> $a;\n"
        "    $a[] = 20;\n"
        "    $a[] = 1;\n"
        "    return $a[0] + $a[1];\n"
        "}\n");

    write_file(project.root() / "src" / "right.eco",
        "function right() : string\n"
        "{\n"
        "    return \"ok\";\n"
        "}\n");

    write_file(project.root() / "src" / "main.eco",
        "echo left();\n"
        "echo right();\n");

    const fs::path cache = project.root() / "cache";
    const std::string args = "build -o out --build-dir " + quoted(cache);

    const ProcessResult first = project.echoc(args);
    INFO(first.output);
    REQUIRE(first.exit_code == 0);

    const std::vector<std::string> before = partition_object_names(cache / "app" / "scratch");
    REQUIRE(before.size() >= 2);

    const ProcessResult second = project.echoc(args);
    INFO(second.output);
    REQUIRE(second.exit_code == 0);

    REQUIRE(partition_object_names(cache / "app" / "scratch") == before);
}

TEST_CASE("a large this-module copy constructor links across entry partitions", "[cache][partition]")
{
    // a large this-module copy constructor lives in the type's file. isolated GlobalDCE
    // used to drop it when the only calls sat in another file. two files, or it never splits
    ScopedProject project("entry_partition_enum_copy");

    write_file(project.root() / "module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    write_file(project.root() / "src" / "error.eco",
        "enum ModelError\n"
        "{\n"
        "    case truncated;\n"
        "    case v(uint32 $v);\n"
        "    case k(rc<string> $kind);\n"
        "    case w(uint32 $v);\n"
        "    case b(rc<string> $field, uint64 $value);\n"
        "}\n");

    write_file(project.root() / "src" / "main.eco",
        "class Decoder\n"
        "{\n"
        "    function a() : result<uint8, ModelError>\n"
        "    {\n"
        "        return .error(.k(rc<string>('scale')));\n"
        "    }\n"
        "\n"
        "    function b() : result<uint32, ModelError>\n"
        "    {\n"
        "        uint8 $v = guard $this->a() else ($e) {\n"
        "            return .error($e);\n"
        "        }\n"
        "        return .ok($v as uint32);\n"
        "    }\n"
        "}\n"
        "\n"
        "$r = Decoder()->b();\n"
        "match ($r->failure()) {\n"
        "    ModelError::k($kind) => { echo $kind->value; },\n"
        "    else => { echo 'other'; },\n"
        "}\n");

    const fs::path cache = project.root() / "cache";
    const ProcessResult built = project.echoc("build -o out --build-dir " + quoted(cache));
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    REQUIRE(partition_object_names(cache / "app" / "scratch").size() >= 2);

    const ProcessResult native = EchoTests::run_binary(project.root() / "out");
    REQUIRE(native.exit_code == 0);
    REQUIRE(native.output.find("scale") != std::string::npos);
}
#endif
