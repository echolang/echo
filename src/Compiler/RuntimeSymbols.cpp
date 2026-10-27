#include "Compiler/RuntimeSymbols.h"

#include <cassert>
#include <cstddef>
#include <vector>

namespace
{
    // libc names echoc does not declare. lowering still emits calls to them
    const char *const k_reserved_libc_names[] = { "memcpy", "memset", "memmove" };

    const char *const k_wasm_crt_names[] = {
        "_start",
        "_initialize",
        "memory",
        "__heap_base",
        "__data_end",
        "__stack_pointer",
        "__indirect_function_table",
        "__wasm_call_ctors",
    };

    // runtime names, then the libc ones. built from runtime_symbol_name
    const std::vector<const char *> &reserved_names()
    {
        static const std::vector<const char *> names = [] {
            std::vector<const char *> out;
            for (size_t i = 0; i < static_cast<size_t>(Compiler::RuntimeSymbol::t_count); ++i) {
                out.push_back(Compiler::runtime_symbol_name(static_cast<Compiler::RuntimeSymbol>(i)));
            }
            out.insert(out.end(), std::begin(k_reserved_libc_names), std::end(k_reserved_libc_names));
            return out;
        }();

        return names;
    }
};

const char *Compiler::runtime_symbol_name(RuntimeSymbol symbol)
{
    // switch with no tail so a missing spelling is a compile warning
    switch (symbol) {
        case RuntimeSymbol::t_malloc: return "malloc";
        case RuntimeSymbol::t_free: return "free";
        case RuntimeSymbol::t_realloc: return "realloc";
        case RuntimeSymbol::t_printf: return "printf";
        case RuntimeSymbol::t_fflush: return "fflush";
        case RuntimeSymbol::t_setvbuf: return "setvbuf";
        case RuntimeSymbol::t_write: return "write";
        case RuntimeSymbol::t_win_write: return "_write";
        case RuntimeSymbol::t_exit: return "exit";
        case RuntimeSymbol::t_atexit: return "atexit";
        case RuntimeSymbol::t_getenv: return "getenv";
        case RuntimeSymbol::t_strcmp: return "strcmp";
        case RuntimeSymbol::t_pthread_self: return "pthread_self";
        case RuntimeSymbol::t_sched_yield: return "sched_yield";
        case RuntimeSymbol::t_get_current_thread_id: return "GetCurrentThreadId";
        case RuntimeSymbol::t_switch_to_thread: return "SwitchToThread";
        case RuntimeSymbol::t_acrt_iob_func: return "__acrt_iob_func";
        case RuntimeSymbol::t_wasilibc_get_environ: return "__wasilibc_get_environ";
        case RuntimeSymbol::t_ns_get_environ: return "_NSGetEnviron";
        case RuntimeSymbol::t_environ: return "environ";
        case RuntimeSymbol::t_stdout: return "stdout";
        case RuntimeSymbol::t_strtoull: return "strtoull";
        case RuntimeSymbol::t_arc4random_buf: return "arc4random_buf";
        case RuntimeSymbol::t_getentropy: return "getentropy";
        case RuntimeSymbol::t_rand_s: return "rand_s";
        case RuntimeSymbol::t_get_module_handle_a: return "GetModuleHandleA";
        case RuntimeSymbol::t_get_proc_address: return "GetProcAddress";
        case RuntimeSymbol::t_dlopen: return "dlopen";
        case RuntimeSymbol::t_dlsym: return "dlsym";
        case RuntimeSymbol::t_setmode: return "_setmode";
        case RuntimeSymbol::t_signal: return "signal";
        case RuntimeSymbol::t_darwin_errno: return "__error";
        case RuntimeSymbol::t_errno_location: return "__errno_location";
        case RuntimeSymbol::t_ucrt_errno: return "_errno";
        case RuntimeSymbol::t_count: break;
    }

    assert(false && "t_count is a length, not a symbol");
    return "";
}

std::span<const char *const> Compiler::compiler_runtime_names()
{
    return { reserved_names().data(), static_cast<size_t>(RuntimeSymbol::t_count) };
}

std::span<const char *const> Compiler::reserved_c_export_names()
{
    return { reserved_names().data(), reserved_names().size() };
}

std::span<const char *const> Compiler::wasm_crt_names()
{
    return k_wasm_crt_names;
}
