#include "Compiler/AllocRuntime.h"

#include "Compiler/CBuild.h"
#include "Compiler/LinkRequirement.h"

#include "alloc_runtime_embedded.h"

#include "eco_cache.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{
    const Compiler::EmbeddedCFile k_files[] = {
        { "eco_cache.h", k_alloc_runtime_header },
        { "eco_cache.c", k_alloc_runtime_c },
    };
};

std::string Compiler::alloc_runtime_stamp()
{
    return embedded_c_stamp(k_files, sizeof(k_files) / sizeof(k_files[0]));
}

void Compiler::touch_alloc_runtime()
{
    (void)&__eco_cache_alloc;
    (void)&__eco_cache_realloc;
    (void)&__eco_cache_free;
}

Compiler::LinkRequirement Compiler::alloc_runtime_link(const std::filesystem::path &object)
{
    return compiler_object_link(object, "runtime");
}

bool Compiler::alloc_runtime_objects(
    const CompilerOptions &options,
    const std::filesystem::path &alloc_dir,
    const std::filesystem::path &scratch_dir,
    std::vector<LinkRequirement> &out_objects,
    std::string &out_error
)
{
    out_objects.clear();

    CBuildSpec spec;
    spec.module_name = "eco_cache";
    spec.flags = { "-O2" };
    // a loadable lib with _Thread_local cannot unload on Darwin, which is
    // the plugin fini test. a mutex and a process list instead
    if (options.codegen.is_library()) {
        spec.flags.push_back("-DECO_CACHE_NO_TLS");
    }

    std::vector<std::filesystem::path> objects;
    if (!compile_embedded_c(
            "the allocation cache",
            spec,
            k_files,
            sizeof(k_files) / sizeof(k_files[0]),
            options,
            alloc_dir,
            scratch_dir,
            objects,
            out_error)) {
        return false;
    }

    for (const std::filesystem::path &object : objects) {
        out_objects.push_back(alloc_runtime_link(object));
    }

    return true;
}

bool Compiler::alloc_link_runtime(
    const CompilerOptions &options,
    const std::filesystem::path &alloc_dir,
    const std::filesystem::path &scratch_dir,
    std::vector<LinkRequirement> &objects,
    std::string &out_error
)
{
    if (!options.using_page_heap()) {
        return true;
    }

    std::vector<LinkRequirement> runtime;
    if (!alloc_runtime_objects(options, alloc_dir, scratch_dir, runtime, out_error)) {
        return false;
    }

    objects.insert(objects.end(), runtime.begin(), runtime.end());
    return true;
}
