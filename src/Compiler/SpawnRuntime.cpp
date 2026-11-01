#include "Compiler/SpawnRuntime.h"

#include "Compiler/CBuild.h"
#include "Compiler/CodegenTarget.h"
#include "Compiler/LinkRequirement.h"

#include "spawn_runtime_embedded.h"

#include "eco_spawn.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/DynamicLibrary.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    const Compiler::EmbeddedCFile k_files[] = {
        { "eco_spawn.h", k_spawn_runtime_header },
        { "eco_spawn.c", k_spawn_runtime_c },
    };
};

std::string Compiler::spawn_runtime_stamp()
{
    return embedded_c_stamp(k_files, sizeof(k_files) / sizeof(k_files[0]));
}

void Compiler::touch_spawn_runtime()
{
    llvm::sys::DynamicLibrary::AddSymbol(
        "__eco_spawn_with_chdir",
        reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&__eco_spawn_with_chdir)));
}

Compiler::LinkRequirement Compiler::spawn_runtime_link(const std::filesystem::path &object)
{
    return compiler_object_link(object, "runtime");
}

bool Compiler::spawn_runtime_objects(
    const CompilerOptions &options,
    const std::filesystem::path &spawn_dir,
    const std::filesystem::path &scratch_dir,
    std::vector<LinkRequirement> &out_objects,
    std::string &out_error
)
{
    out_objects.clear();

    CBuildSpec spec;
    spec.module_name = "eco_spawn";
    spec.flags = { "-O2" };

    std::vector<std::filesystem::path> objects;
    if (!compile_embedded_c(
            "the spawn helper",
            spec,
            k_files,
            sizeof(k_files) / sizeof(k_files[0]),
            options,
            spawn_dir,
            scratch_dir,
            objects,
            out_error)) {
        return false;
    }

    for (const std::filesystem::path &object : objects) {
        out_objects.push_back(spawn_runtime_link(object));
    }

    return true;
}

bool Compiler::spawn_link_runtime(
    const CompilerOptions &options,
    const std::filesystem::path &spawn_dir,
    const std::filesystem::path &scratch_dir,
    std::vector<LinkRequirement> &objects,
    std::string &out_error
)
{
    if (options.codegen.os() != Compiler::TargetOs::t_linux || options.codegen.is_wasm()) {
        return true;
    }

    std::vector<LinkRequirement> runtime;
    if (!spawn_runtime_objects(options, spawn_dir, scratch_dir, runtime, out_error)) {
        return false;
    }

    objects.insert(objects.end(), runtime.begin(), runtime.end());
    return true;
}
