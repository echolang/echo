#ifndef RUNTIMESYMBOLS_H
#define RUNTIMESYMBOLS_H

#pragma once

#include <span>
#include <string_view>

namespace Compiler
{
    // C symbols echoc itself declares into every unit. libc_callee takes an
    // enumerator, so a name added at a call site without a row here is a
    // compile error rather than a symbol a program can export on top of.
    //
    // `__eco_*` is a prefix, not a row: those are the runtime echoc emits.
    // `_start` / `_initialize` are wasm crt, asked through wasm_crt_names
    enum class RuntimeSymbol
    {
        t_malloc,
        t_free,
        t_realloc,
        t_printf,
        t_fflush,
        t_setvbuf,
        t_write,
        t_win_write,
        t_exit,
        t_getenv,
        t_strcmp,
        t_pthread_self,
        t_sched_yield,
        t_get_current_thread_id,
        t_switch_to_thread,
        t_acrt_iob_func,
        t_count
    };

    // the C spelling libc_callee inserts. order matches the enumerators
    const char *runtime_symbol_name(RuntimeSymbol symbol);

    // the table libc_callee walks. first `t_count` names of reserved_c_export_names
    std::span<const char *const> compiler_runtime_names();

    // names `#[export]` may not claim: the runtime table, then memcpy/memset/memmove
    // (libc, not a RuntimeSymbol, because echoc does not declare them). `main` is
    // CodegenTarget::reserved_entry_names, even in a lib
    std::span<const char *const> reserved_c_export_names();

    inline constexpr std::string_view k_compiler_runtime_prefix = "__eco_";

    // wasm-ld's reactor/command crt, plus the layout symbols wasm-ld
    // plants. empty of meaning off a wasm row
    std::span<const char *const> wasm_crt_names();
};

#endif
