#include "Compiler/TargetFacts.h"

#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cassert>
#include <utility>

namespace Compiler
{

namespace
{
    // the vocabularies, spelled once. a condition's value is checked against these, an override on the
    // command line is checked against these, and the "expected one of" in both messages is built from
    // these - so a platform added here becomes sayable everywhere at once
    const std::vector<std::string> k_operating_systems = { "darwin", "linux", "windows", "ios", "android" };
    const std::vector<std::string> k_architectures = { "arm64", "x86_64" };
    const std::vector<std::string> k_families = { "darwin", "linux", "windows" };

    // which family an os belongs to. ios is Darwin the kernel (UIKit rather than AppKit); android is
    // Linux the kernel (Bionic rather than glibc). a name added to k_operating_systems without a row
    // here has an empty family, so `family == darwin` is false rather than silently matching
    const std::pair<const char *, const char *> k_os_family[] = {
        { "darwin", "darwin" },
        { "ios", "darwin" },
        { "linux", "linux" },
        { "android", "linux" },
        { "windows", "windows" },
    };

    constexpr const char *k_axis_os = "os";
    constexpr const char *k_axis_arch = "arch";
    constexpr const char *k_axis_family = "family";

    // the one flag the compiler sets itself. it reads like any other flag to a condition, and is reserved
    // against `--define` for the reason an axis is: writing it would be believing something false
    constexpr const char *k_flag_tests = "tests";

    struct Axis
    {
        const char *name;
        const std::vector<std::string> *vocabulary;
        std::string (*current)(const TargetFacts &);
    };

    // **one table, and a fourth axis is a row.** is_axis, axis_names, axis_equals and the "expected
    // one of" sentence all read this. a ternary on the axis name is the bug this type already exists
    // to prevent
    const Axis k_axes[] = {
        { k_axis_os, &k_operating_systems, [](const TargetFacts &facts) {
            return facts.operating_system;
        } },
        { k_axis_arch, &k_architectures, [](const TargetFacts &facts) {
            return facts.architecture;
        } },
        { k_axis_family, &k_families, [](const TargetFacts &facts) {
            return facts.family();
        } },
    };

    const Axis *axis_named(const std::string &name)
    {
        for (const Axis &axis : k_axes) {
            if (name == axis.name) {
                return &axis;
            }
        }

        return nullptr;
    }

    bool vocabulary_contains(const std::vector<std::string> &vocabulary, const std::string &name)
    {
        return std::find(vocabulary.begin(), vocabulary.end(), name) != vocabulary.end();
    }

    // LLVM's triple vocabulary is not ours. first match is the whole of the order: Android's OS
    // field is Linux (environment Android), so isAndroid must precede isOSLinux, the way isiOS
    // precedes isOSDarwin. isiOS includes tvOS - that is the policy until tvos is its own name,
    // not an accident of LLVM's predicate. watchOS, DriverKit and XROS fall through to darwin
    // until those names exist
    std::string operating_system_of(const llvm::Triple &triple)
    {
        struct Mapping
        {
            bool (llvm::Triple::*pred)() const;
            const char *os;
        };

        const Mapping mappings[] = {
            { &llvm::Triple::isAndroid, "android" },
            { &llvm::Triple::isiOS, "ios" },
            { &llvm::Triple::isOSDarwin, "darwin" },
            { &llvm::Triple::isOSLinux, "linux" },
            { &llvm::Triple::isOSWindows, "windows" },
        };

        for (const Mapping &mapping : mappings) {
            if ((triple.*mapping.pred)()) {
                return mapping.os;
            }
        }

        return "";
    }

    std::string architecture_of(const llvm::Triple &triple)
    {
        switch (triple.getArch()) {
            case llvm::Triple::aarch64:
            case llvm::Triple::aarch64_be:
                return "arm64";

            case llvm::Triple::x86_64:
                return "x86_64";

            default:
                return "";
        }
    }
};

const std::vector<std::string> &TargetFacts::known_operating_systems()
{
    return k_operating_systems;
}

const std::vector<std::string> &TargetFacts::known_architectures()
{
    return k_architectures;
}

const std::vector<std::string> &TargetFacts::known_families()
{
    return k_families;
}

bool TargetFacts::is_known_operating_system(const std::string &name)
{
    return vocabulary_contains(k_operating_systems, name);
}

bool TargetFacts::is_known_architecture(const std::string &name)
{
    return vocabulary_contains(k_architectures, name);
}

bool TargetFacts::is_known_family(const std::string &name)
{
    return vocabulary_contains(k_families, name);
}

bool TargetFacts::is_axis(const std::string &name)
{
    return axis_named(name) != nullptr;
}

std::string TargetFacts::axis_names()
{
    std::string list;

    for (const Axis &axis : k_axes) {
        list += list.empty() ? axis.name : std::string(", ") + axis.name;
    }

    return list;
}

std::string TargetFacts::family() const
{
    for (const auto &[os, family] : k_os_family) {
        if (operating_system == os) {
            return family;
        }
    }

    return "";
}

TargetFacts TargetFacts::for_module(
    const TargetFacts &base,
    const std::string &module_name,
    const std::set<std::string> &modules_with_tests
)
{
    TargetFacts facts = base;
    facts.tests = modules_with_tests.find(module_name) != modules_with_tests.end();

    return facts;
}

bool TargetFacts::axis_equals(
    const std::string &axis,
    const std::string &value,
    bool &out_match,
    std::string &out_error
) const
{
    const Axis *spec = axis_named(axis);
    assert(spec != nullptr && "axis_equals called with a name is_axis rejected");

    if (!vocabulary_contains(*spec->vocabulary, value)) {
        out_error = fmt::format("unknown {} '{}', expected one of: {}", axis, value,
            fmt::join(*spec->vocabulary, ", "));
        return false;
    }

    out_match = value == spec->current(*this);
    return true;
}

TargetFacts TargetFacts::from_triple(const std::string &spelled)
{
    const llvm::Triple triple(spelled);

    TargetFacts facts;
    facts.operating_system = operating_system_of(triple);
    facts.architecture = architecture_of(triple);

    return facts;
}

bool TargetFacts::resolve(
    const std::string &os_override,
    const std::string &arch_override,
    const std::vector<std::string> &defines,
    TargetFacts &out_facts,
    std::string &out_error
)
{
    out_facts = from_triple(llvm::sys::getDefaultTargetTriple());

    // **an unrecognised host is not an error.** It leaves the axis empty, so `os == darwin` is simply
    // false and an `#[else]` arm is what such a platform gets - which is the right outcome for a target
    // nobody has taught this compiler about yet. Refusing to compile at all would be worse, and a
    // *condition* naming an unknown value is still caught, because that is a typo rather than a platform
    if (!os_override.empty()) {
        if (!is_known_operating_system(os_override)) {
            out_error = fmt::format(
                "unknown --target-os '{}', expected one of: {}",
                os_override, fmt::join(k_operating_systems, ", "));
            return false;
        }

        out_facts.operating_system = os_override;
    }

    if (!arch_override.empty()) {
        if (!is_known_architecture(arch_override)) {
            out_error = fmt::format(
                "unknown --target-arch '{}', expected one of: {}",
                arch_override, fmt::join(k_architectures, ", "));
            return false;
        }

        out_facts.architecture = arch_override;
    }

    for (const std::string &define : defines) {
        // a define carrying a value would be a substitution, and this compiler has `const` declarations
        // for naming a value. refused rather than accepted-and-ignored, so `--define N=1` cannot look
        // like it worked
        if (define.find('=') != std::string::npos) {
            out_error = fmt::format(
                "--define takes a bare name, not '{}' - a define is a flag a condition can test, "
                "and carries no value", define);
            return false;
        }

        if (define.empty()) {
            out_error = "--define needs a name";
            return false;
        }

        // reserved against the axis names, so `--define os` cannot shadow the thing `os == darwin` reads.
        // The condition grammar would never look a define up under those names, but a person who wrote it
        // believed something false and should be told
        if (is_axis(define)) {
            out_error = fmt::format(
                "'{}' is a condition axis, not a flag - write `#[if: {} == <value>]` rather than "
                "defining it", define, define);
            return false;
        }

        // reserved for the same reason, and for one more: what compiles a module's `test` blocks has to be
        // the thing that also runs them, or a build could carry tests nothing ever calls
        if (define == k_flag_tests) {
            out_error = fmt::format(
                "'{}' is set by `echoc test`, not by --define - a build that defined it would compile "
                "every test block and run none of them", define);
            return false;
        }

        out_facts.defines.insert(define);
    }

    return true;
}

TargetFacts TargetFacts::host()
{
    return from_triple(llvm::sys::getDefaultTargetTriple());
}

bool TargetFacts::has_define(const std::string &name) const
{
    // `tests` is answered from the field rather than from the set, so there is one place that knows whether
    // this module compiles its tests and `#[if: tests]` cannot disagree with the token filter beside it
    if (name == k_flag_tests) {
        return tests;
    }

    return defines.find(name) != defines.end();
}

std::string TargetFacts::cache_signature() const
{
    std::string result = "os=" + operating_system
        + ";arch=" + architecture
        + ";family=" + family() + ";";

    // stated either way rather than only when set: a module compiled with its tests holds function bodies a
    // module compiled without them does not, so the two cannot be allowed to share an object
    result += std::string("tests=") + (tests ? "1" : "0") + ";";

    // the set is ordered, so this is stable across two invocations that typed the flags differently
    for (const std::string &define : defines) {
        result += "define=" + define + ";";
    }

    return result;
}

std::string TargetFacts::shared_library_extension() const
{
    if (family() == "darwin") {
        return ".dylib";
    }

    if (family() == "windows") {
        return ".dll";
    }

    return ".so";
}

};
