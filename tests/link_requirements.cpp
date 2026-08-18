#include <catch2/catch_test_macros.hpp>

#include <AST/ASTAttributeReader.h>
#include <AST/AttributeNode.h>
#include <Compiler/LinkRequirement.h>
#include <Compiler/TargetFacts.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

#include "subprocess.h"
#include "helpers.h"

// what a `#[link: ...]` value means, and what it becomes on a link line and inside the JIT.
//
// the scheme vocabulary is closed for the reason TargetFacts' axis vocabularies are: a value outside it has
// to be a located error, because a link requirement that quietly does nothing surfaces much later as a
// linker naming a symbol - which says nothing at all about the declaration that was never applied.

namespace fs = std::filesystem;

namespace
{

using EchoTests::write_file;

// the facts a manifest is read against, without a command line to resolve them from
Compiler::TargetFacts facts_for(const std::string &operating_system)
{
    Compiler::TargetFacts facts = Compiler::TargetFacts::host();
    facts.operating_system = operating_system;
    return facts;
}

Compiler::LinkRequirement parsed(
    const std::string &spelled, const fs::path &base, const std::string &os = "darwin")
{
    Compiler::LinkRequirement requirement;
    std::string error;

    REQUIRE(Compiler::parse_link_requirement(spelled, base, facts_for(os), "lib", requirement, error));
    REQUIRE(error.empty());

    return requirement;
}

std::string refusal_of(
    const std::string &spelled, const fs::path &base, const std::string &os = "darwin")
{
    Compiler::LinkRequirement requirement;
    std::string error;

    REQUIRE_FALSE(Compiler::parse_link_requirement(spelled, base, facts_for(os), "lib", requirement, error));

    return error;
}

// `#[link: <spelled>]` through the real parser, so a record payload is the same value a manifest holds
std::vector<Compiler::LinkRequirement> from_attribute(
    const std::string &spelled, const std::string &os = "darwin")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[link: " + spelled + "]\nfunction marked() : void {}\n");

    AST::AttributeNode *node = nullptr;

    for (auto &module : bundle->modules) {
        for (AST::AttributeNode *attribute : module->nodes.of_type<AST::AttributeNode>()) {
            if (attribute->attribute_id.value() == "link") {
                node = attribute;
            }
        }
    }

    REQUIRE(node != nullptr);
    REQUIRE(node->value.has_value());

    AST::AttributeReader reader("link");
    std::vector<Compiler::LinkRequirement> requirements;

    REQUIRE(Compiler::parse_link_attribute(
        node->value.value(), fs::current_path(), facts_for(os), "lib", reader, requirements));
    REQUIRE_FALSE(reader.has_refusals());

    return requirements;
}

std::string attribute_refusal(const std::string &spelled, const std::string &os = "darwin")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[link: " + spelled + "]\nfunction marked() : void {}\n");

    AST::AttributeNode *node = nullptr;

    for (auto &module : bundle->modules) {
        for (AST::AttributeNode *attribute : module->nodes.of_type<AST::AttributeNode>()) {
            if (attribute->attribute_id.value() == "link") {
                node = attribute;
            }
        }
    }

    REQUIRE(node != nullptr);
    REQUIRE(node->value.has_value());

    AST::AttributeReader reader("link");
    std::vector<Compiler::LinkRequirement> requirements;
    Compiler::parse_link_attribute(
        node->value.value(), fs::current_path(), facts_for(os), "lib", reader, requirements);

    REQUIRE(reader.has_refusals());
    return reader.refusals().front().message;
}

// four bytes that `is_loadable_shared_object` accepts on every host. the checker reads magic, it
// does not load the file, so ELF is a fine fixture on Darwin
void write_fake_elf(const fs::path &path)
{
    const unsigned char magic[] = { 0x7f, 'E', 'L', 'F' };
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(magic), sizeof(magic));
}

void write_linker_script(const fs::path &path)
{
    write_file(path, "/* GNU ld script */\nGROUP ( libc.so.6 )\n");
}

// a scratch directory this suite can point a `search:` or an `object:` at
class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("link_requirements", name)
    {};
};

};

TEST_CASE("a library names itself and nothing else", "[link]")
{
    const Compiler::LinkRequirement requirement = parsed("lib:GL", fs::current_path());

    REQUIRE(requirement.scheme == Compiler::LinkScheme::t_library);
    REQUIRE(requirement.value == "GL");
    REQUIRE(requirement.declared_by == "lib");
}

TEST_CASE("a requirement spells itself the way it was written", "[link]")
{
    ScopedProject project("spelling");

    fs::create_directories(project.root() / "vendor");

    // what a diagnostic quotes back. The settled value of a path scheme is absolute and is not the text in
    // the file, so the scheme has to come back with it or the reader is sent looking for the wrong string.
    //
    // **which of the two spellings is `declared_by`**, which already records the medium: `parsed` here
    // credits a module, so these read as a manifest wrote them
    REQUIRE(Compiler::link_requirement_spelling(parsed("lib:GL", project.root())) == "lib \"GL\"");
    REQUIRE(Compiler::link_requirement_spelling(parsed("framework:OpenGL", project.root()))
        == "framework \"OpenGL\"");
    REQUIRE(Compiler::link_requirement_spelling(parsed("search:vendor", project.root()))
        .rfind("search \"/", 0) == 0);

    // a command-line one is credited to nobody, and comes back in the syntax it was typed in - quoting
    // it as an attribute would send its reader looking for a manifest line that does not exist
    Compiler::LinkRequirement from_argv;
    std::string error;

    REQUIRE(Compiler::parse_link_requirement(
        "lib:GL", project.root(), facts_for("darwin"), "", from_argv, error));
    REQUIRE(Compiler::link_requirement_spelling(from_argv) == "lib:GL");

    // a record that is not the defaults quotes the record, so a blame line names a line the file holds
    const std::vector<Compiler::LinkRequirement> process =
        from_attribute("lib { name: \"pthread\", runtime: process }");
    REQUIRE(Compiler::link_requirement_spelling(process.front())
        == "lib { name: \"pthread\", runtime: process }");
}

TEST_CASE("the scheme is mandatory", "[link]")
{
    // a bare `GL` is not a library. Accepting one would be a second rule, and under it a typo'd scheme
    // reads as a library of that name and fails at link time about something nobody wrote
    const std::string refusal = refusal_of("GL", fs::current_path());

    REQUIRE(refusal.find("does not name a link scheme") != std::string::npos);
    REQUIRE(refusal.find("lib, framework, search, object") != std::string::npos);
}

TEST_CASE("a misspelled scheme is refused rather than read as a name", "[link]")
{
    const std::string refusal = refusal_of("framwork:OpenGL", fs::current_path());

    REQUIRE(refusal.find("'framwork' is not a link scheme") != std::string::npos);
}

TEST_CASE("a scheme with no value is refused", "[link]")
{
    REQUIRE(refusal_of("lib:", fs::current_path()).find("and no value") != std::string::npos);
}

TEST_CASE("a framework is refused off darwin, and says how to gate it", "[link]")
{
    const std::string refusal = refusal_of("framework:OpenGL", fs::current_path(), "linux");

    REQUIRE(refusal.find("Darwin framework") != std::string::npos);
    REQUIRE(refusal.find("#[if: os == darwin]") != std::string::npos);
}

TEST_CASE("a path resolves against the base, never the working directory", "[link]")
{
    ScopedProject project("relative_search");

    fs::create_directories(project.root() / "vendor" / "lib");

    // spelled relative to the "manifest", which is what makes a library work when it is depended on from
    // somewhere else entirely
    const Compiler::LinkRequirement requirement = parsed("search:vendor/lib", project.root());

    REQUIRE(requirement.scheme == Compiler::LinkScheme::t_search);
    REQUIRE(fs::path(requirement.value).is_absolute());
    REQUIRE(fs::equivalent(requirement.value, project.root() / "vendor" / "lib"));
}

TEST_CASE("only the first colon separates, so a value may hold one", "[link]")
{
    ScopedProject project("colon_in_value");

    // the case a scheme prefix has to survive: a Windows drive letter, and any path with a colon in it
    const fs::path odd = project.root() / "a:b";
    fs::create_directories(odd);

    const Compiler::LinkRequirement requirement = parsed("search:" + odd.string(), project.root());

    REQUIRE(fs::equivalent(requirement.value, odd));
}

TEST_CASE("a search path that is not a directory is refused", "[link]")
{
    ScopedProject project("missing_search");

    REQUIRE(refusal_of("search:nowhere", project.root()).find("is not a directory") != std::string::npos);
}

TEST_CASE("an object that is not a file is refused", "[link]")
{
    ScopedProject project("missing_object");

    REQUIRE(refusal_of("object:nothing.o", project.root()).find("is not a file") != std::string::npos);
}

TEST_CASE("objects and flag words land in different vectors", "[link]")
{
    ScopedProject project("partition");

    fs::create_directories(project.root() / "vendor");
    write_file(project.root() / "vendor" / "glad.o", "not really an object");

    const std::vector<Compiler::LinkRequirement> requirements = {
        parsed("lib:glfw3", project.root()),
        parsed("object:vendor/glad.o", project.root()),
        parsed("framework:OpenGL", project.root()),
        parsed("search:vendor", project.root()),
    };

    std::vector<fs::path> objects;
    std::vector<std::string> words;
    Compiler::partition_link_requirements(requirements, objects, words);

    REQUIRE(objects.size() == 1);
    REQUIRE(objects.front().filename() == "glad.o");

    // **the search path comes before the libraries that need it**: a `-l` resolves against the `-L`s seen
    // so far, so one written after them is one the linker never consults
    REQUIRE(words.size() == 4);
    REQUIRE(words[0].rfind("-L", 0) == 0);
    REQUIRE(words[1] == "-lglfw3");

    // a framework is two words meaning one thing, which is the shape a flag string could not have carried
    REQUIRE(words[2] == "-framework");
    REQUIRE(words[3] == "OpenGL");
}

TEST_CASE("merging keeps the first of a duplicate and preserves order", "[link]")
{
    std::vector<Compiler::LinkRequirement> into;

    Compiler::LinkRequirement first = parsed("lib:GL", fs::current_path());
    first.declared_by = "renderer";

    Compiler::LinkRequirement again = parsed("lib:GL", fs::current_path());
    again.declared_by = "window";

    std::string error;

    REQUIRE(Compiler::merge_link_requirements({ first, parsed("lib:m", fs::current_path()) }, into, error));
    REQUIRE(Compiler::merge_link_requirements({ again }, into, error));

    REQUIRE(into.size() == 2);
    REQUIRE(into[0].value == "GL");
    REQUIRE(into[1].value == "m");

    // identity is the scheme and the value, so the second module asking changes nothing - including who is
    // credited, which the first declaration keeps
    REQUIRE(into[0].declared_by == "renderer");
}

TEST_CASE("a search path is what the JIT tries before the bare name", "[link][jit]")
{
    ScopedProject project("runtime_search");

    const fs::path directory = project.root() / "vendor";
    fs::create_directories(directory);

    // a real DSO magic, not a placeholder string: a linker script in this slot used to win and then
    // fail at dlopen, which is the Ubuntu `libpthread.so` failure
    const std::string extension = Compiler::TargetFacts::host().shared_library_extension();
    write_fake_elf(directory / ("libfake" + extension));

    const std::vector<Compiler::LinkRequirement> requirements = {
        parsed("search:vendor", project.root()),
        parsed("lib:fake", project.root()),
    };

    std::string refusal;
    const auto resolved = Compiler::runtime_library_of(requirements[1], requirements, refusal);

    REQUIRE(resolved.has_value());
    REQUIRE(fs::equivalent(resolved.value(), directory / ("libfake" + extension)));
    REQUIRE(refusal.empty());
}

TEST_CASE("a linker script is not a loadable library", "[link][jit]")
{
    ScopedProject project("linker_script");

    const fs::path directory = project.root() / "vendor";
    const std::string extension = Compiler::TargetFacts::host().shared_library_extension();

    write_linker_script(directory / ("libfake" + extension));
    write_fake_elf(directory / ("libfake" + extension + ".1"));

    const std::vector<Compiler::LinkRequirement> requirements = {
        parsed("search:vendor", project.root()),
        parsed("lib:fake", project.root()),
    };

    std::string refusal;
    const auto resolved = Compiler::runtime_library_of(requirements[1], requirements, refusal);

    // the unversioned name is the *linker* name. the SONAME next to it is what dlopen can open
    REQUIRE(resolved.has_value());
    REQUIRE(fs::equivalent(resolved.value(), directory / ("libfake" + extension + ".1")));
    REQUIRE(refusal.empty());
}

TEST_CASE("a library nothing declares a path for falls back to the bare name", "[link][jit]")
{
    // a name that cannot sit in the host lib directories, so the resolver has to hand the loader
    // the constructed filename rather than a path it found
    const std::vector<Compiler::LinkRequirement> requirements = {
        parsed("lib:eco-no-such-library-anywhere", fs::current_path()),
    };

    std::string refusal;
    const auto resolved = Compiler::runtime_library_of(requirements.front(), requirements, refusal);

    REQUIRE(resolved.has_value());
    REQUIRE(resolved.value().filename().string().rfind("libeco-no-such-library-anywhere.", 0) == 0);
    REQUIRE_FALSE(resolved.value().has_parent_path());
}

TEST_CASE("runtime process is nothing for the JIT to open", "[link][jit]")
{
    const std::vector<Compiler::LinkRequirement> requirements =
        from_attribute("lib { name: \"pthread\", runtime: process }");

    REQUIRE(requirements.size() == 1);
    REQUIRE(requirements.front().runtime == Compiler::LinkRuntime::t_process);
    REQUIRE(requirements.front().value == "pthread");

    std::string refusal;
    const auto resolved = Compiler::runtime_library_of(requirements.front(), requirements, refusal);

    REQUIRE_FALSE(resolved.has_value());
    REQUIRE(refusal.empty());
}

TEST_CASE("a static library refuses the JIT like an object", "[link][jit]")
{
    const std::vector<Compiler::LinkRequirement> requirements =
        from_attribute("lib { name: \"foo\", linkage: static }");

    REQUIRE(requirements.front().linkage == Compiler::LinkLinkage::t_static);

    std::string refusal;
    const auto resolved = Compiler::runtime_library_of(requirements.front(), requirements, refusal);

    REQUIRE_FALSE(resolved.has_value());
    REQUIRE(refusal.find("echoc build") != std::string::npos);
}

TEST_CASE("a lib record names the library and the defaults stay load and dynamic", "[link]")
{
    const std::vector<Compiler::LinkRequirement> requirements = from_attribute("lib { name: \"GL\" }");

    REQUIRE(requirements.size() == 1);
    REQUIRE(requirements.front().value == "GL");
    REQUIRE(requirements.front().linkage == Compiler::LinkLinkage::t_dynamic);
    REQUIRE(requirements.front().runtime == Compiler::LinkRuntime::t_load);
    REQUIRE_FALSE(requirements.front().file.has_value());
}

TEST_CASE("a lib record refuses an unknown field", "[link]")
{
    REQUIRE(attribute_refusal("lib { name: \"GL\", flavour: extra }").find("flavour") != std::string::npos);
}

TEST_CASE("an explicit file is the basename the JIT looks for", "[link][jit]")
{
    ScopedProject project("explicit_file");

    const fs::path directory = project.root() / "vendor";
    write_fake_elf(directory / "libssl.so.3");

    std::vector<Compiler::LinkRequirement> requirements = from_attribute(
        "lib { name: \"ssl\", file: \"libssl.so.3\" }");
    requirements.insert(requirements.begin(), parsed("search:vendor", project.root()));

    std::string refusal;
    const auto resolved = Compiler::runtime_library_of(requirements.back(), requirements, refusal);

    REQUIRE(resolved.has_value());
    REQUIRE(fs::equivalent(resolved.value(), directory / "libssl.so.3"));
}

TEST_CASE("several SONAMEs refuse rather than pick one", "[link][jit]")
{
    ScopedProject project("ambiguous_soname");

    const fs::path directory = project.root() / "vendor";
    const std::string extension = Compiler::TargetFacts::host().shared_library_extension();

    write_linker_script(directory / ("libfake" + extension));
    write_fake_elf(directory / ("libfake" + extension + ".1"));
    write_fake_elf(directory / ("libfake" + extension + ".3"));

    const std::vector<Compiler::LinkRequirement> requirements = {
        parsed("search:vendor", project.root()),
        parsed("lib:fake", project.root()),
    };

    std::string refusal;
    const auto resolved = Compiler::runtime_library_of(requirements[1], requirements, refusal);

    REQUIRE_FALSE(resolved.has_value());
    REQUIRE(refusal.find("several loadable files") != std::string::npos);
    REQUIRE(refusal.find("file:") != std::string::npos);
}

TEST_CASE("a merge refuses a second resolution of the same library", "[link]")
{
    std::vector<Compiler::LinkRequirement> into;
    std::string error;

    REQUIRE(Compiler::merge_link_requirements(from_attribute("lib \"ssl\""), into, error));

    REQUIRE_FALSE(Compiler::merge_link_requirements(
        from_attribute("lib { name: \"ssl\", file: \"libssl.so.3\" }"), into, error));
    REQUIRE(error.find("cannot be linked two ways") != std::string::npos);
    REQUIRE(error.find("libssl.so.3") != std::string::npos);
}

TEST_CASE("a static archive in a search path is seated as an object", "[link]")
{
    ScopedProject project("static_archive");

    write_file(project.root() / "vendor" / "libfoo.a", "not really an archive");

    std::vector<Compiler::LinkRequirement> requirements =
        from_attribute("lib { name: \"foo\", linkage: static }");
    requirements.insert(requirements.begin(), parsed("search:vendor", project.root()));

    std::vector<fs::path> objects;
    std::vector<std::string> words;
    Compiler::partition_link_requirements(requirements, objects, words);

    REQUIRE(objects.size() == 1);
    REQUIRE(objects.front().filename() == "libfoo.a");
    REQUIRE(std::find(words.begin(), words.end(), "-lfoo") == words.end());
}

TEST_CASE("an object refuses the JIT with a sentence rather than a silence", "[link][jit]")
{
    ScopedProject project("runtime_object");

    write_file(project.root() / "glad.o", "not really an object");

    const std::vector<Compiler::LinkRequirement> requirements = { parsed("object:glad.o", project.root()) };

    std::string refusal;
    const auto resolved = Compiler::runtime_library_of(requirements.front(), requirements, refusal);

    // three answers and not two: dropping this one leaves MCJIT naming an unresolved symbol, which says
    // nothing about the declaration that could never have been applied
    REQUIRE_FALSE(resolved.has_value());
    REQUIRE(refusal.find("echoc build") != std::string::npos);
}

TEST_CASE("a library that will not open refuses the run rather than hanging", "[link][jit][store]")
{
    // **a subprocess test rather than a corpus golden**, for the reason tests/module_cache.cpp gives: the
    // message quotes the platform's own dlerror, which names the dyld cache on macOS and something else
    // entirely on Linux. What is portable is the refusal, the exit status and who is blamed.
    //
    // this is the case that used to hang: a note was printed and the run carried on, and MCJIT's answer to
    // a program whose externals cannot resolve is to sit there with the note scrolled off the top
    ScopedProject project("unloadable");

    write_file(project.root() / "module.eco",
        "#[module: \"unloadable\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    write_file(project.root() / "src" / "main.eco", "echo 1;\n");

    const EchoTests::ProcessResult result =
        project.echoc("run --link lib:eco-no-such-library-anywhere");

    REQUIRE(result.exit_code == 1);
    REQUIRE(result.output.find("Cannot Run This Program") != std::string::npos);

    // the *spelling the command line used*, not the file name it resolved to - a reader told `glfw` goes
    // looking for it in a manifest that says `lib:glfw`
    REQUIRE(result.output.find("lib:eco-no-such-library-anywhere") != std::string::npos);
    REQUIRE(result.output.find("the command line") != std::string::npos);

    // and the way out, in the message rather than in a book somebody has to know exists
    REQUIRE(result.output.find("--link search:") != std::string::npos);

    // nothing ran: the program's own output would be here if the refusal had come too late
    REQUIRE(result.output.find("1\n") == std::string::npos);
}

TEST_CASE("pthread does not refuse a run when the file is a linker name", "[link][jit][store]")
{
    ScopedProject project("pthread_resident");

    write_file(project.root() / "module.eco",
        "#[module: \"pthread_resident\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    write_file(project.root() / "src" / "main.eco", "echo 1;\n");

    // stdlib declares `runtime: process` for pthread. `--link lib:pthread` is a *load* and would
    // disagree; a bare `run` is the pin that the program starts
    const EchoTests::ProcessResult result = project.echoc("run");

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.output.find("1\n") != std::string::npos);
    REQUIRE(result.output.find("Cannot Run This Program") == std::string::npos);
}

TEST_CASE("a search path is nothing for the JIT to open", "[link][jit]")
{
    ScopedProject project("runtime_search_alone");

    fs::create_directories(project.root() / "vendor");

    const std::vector<Compiler::LinkRequirement> requirements = { parsed("search:vendor", project.root()) };

    std::string refusal;
    const auto resolved = Compiler::runtime_library_of(requirements.front(), requirements, refusal);

    // not an answer and not a refusal: a search path is where another requirement's answer lives
    REQUIRE_FALSE(resolved.has_value());
    REQUIRE(refusal.empty());
}
