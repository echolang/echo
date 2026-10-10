#include "Compiler/RuntimeSymbols.h"

#include <cstddef>

namespace
{
    // enumerator order is the content: RuntimeSymbol::t_count is the length of
    // the prefix. memcpy/memset/memmove sit after it so TypeChecker can pass
    // one span to check_exports
    static const char *const k_reserved_c_export_names[] = {
        "malloc",
        "free",
        "realloc",
        "printf",
        "fflush",
        "setvbuf",
        "write",
        "_write",
        "exit",
        "getenv",
        "strcmp",
        "pthread_self",
        "sched_yield",
        "GetCurrentThreadId",
        "SwitchToThread",
        "__acrt_iob_func",
        "__wasilibc_get_environ",
        "_NSGetEnviron",
        "environ",
        "stdout",
        "memcpy",
        "memset",
        "memmove",
    };

    static constexpr size_t k_extra_reserved_count = 3;

    static_assert(
        sizeof(k_reserved_c_export_names) / sizeof(k_reserved_c_export_names[0])
            == static_cast<size_t>(Compiler::RuntimeSymbol::t_count) + k_extra_reserved_count,
        "RuntimeSymbol enumerators must be the prefix of k_reserved_c_export_names");

    static const char *const k_wasm_crt_names[] = {
        "_start",
        "_initialize",
        "memory",
        "__heap_base",
        "__data_end",
        "__stack_pointer",
        "__indirect_function_table",
        "__wasm_call_ctors",
    };
};

const char *Compiler::runtime_symbol_name(RuntimeSymbol symbol)
{
    return k_reserved_c_export_names[static_cast<size_t>(symbol)];
}

std::span<const char *const> Compiler::compiler_runtime_names()
{
    return { k_reserved_c_export_names, static_cast<size_t>(RuntimeSymbol::t_count) };
}

std::span<const char *const> Compiler::reserved_c_export_names()
{
    return k_reserved_c_export_names;
}

std::span<const char *const> Compiler::wasm_crt_names()
{
    return k_wasm_crt_names;
};
