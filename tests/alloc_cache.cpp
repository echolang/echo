#include <catch2/catch_test_macros.hpp>

#include <Compiler/AllocRuntime.h>
#include <Compiler/CodegenTarget.h>
#include <Compiler/CompilerOptions.h>
#include <Compiler/LinkRequirement.h>
#include <Compiler/ProfileRuntime.h>
#include <eco_cache.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

static bool is_16_aligned(void *p)
{
    return (reinterpret_cast<uintptr_t>(p) & 15u) == 0;
}

TEST_CASE("alloc returns a 16-aligned block", "[alloc]")
{
    __eco_cache_flush();

    void *p = __eco_cache_alloc(1);
    REQUIRE(p != nullptr);
    REQUIRE(is_16_aligned(p));

    __eco_cache_free(p);
    __eco_cache_flush();
}

TEST_CASE("two live allocs of one class are distinct", "[alloc]")
{
    __eco_cache_flush();

    void *first = __eco_cache_alloc(32);
    void *second = __eco_cache_alloc(32);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(first != second);

    __eco_cache_free(first);
    __eco_cache_free(second);
    __eco_cache_flush();
}

TEST_CASE("a free then a same-class alloc succeeds", "[alloc]")
{
    __eco_cache_flush();

    void *first = __eco_cache_alloc(32);
    REQUIRE(first != nullptr);
    __eco_cache_free(first);

    void *again = __eco_cache_alloc(32);
    REQUIRE(again != nullptr);
    REQUIRE(is_16_aligned(again));
    REQUIRE(again == first);

    __eco_cache_free(again);
    __eco_cache_flush();
}

TEST_CASE("a 32-byte free does not satisfy a 64-byte alloc", "[alloc]")
{
    __eco_cache_flush();

    void *small = __eco_cache_alloc(32);
    REQUIRE(small != nullptr);
    __eco_cache_free(small);

    void *big = __eco_cache_alloc(64);
    REQUIRE(big != nullptr);
    REQUIRE(big != small);

    __eco_cache_free(big);
    __eco_cache_flush();
}

TEST_CASE("realloc within a class keeps the pointer", "[alloc]")
{
    __eco_cache_flush();

    void *p = __eco_cache_alloc(20);
    REQUIRE(p != nullptr);
    std::memset(p, 0xab, 20);

    void *grown = __eco_cache_realloc(p, 30);
    REQUIRE(grown == p);
    REQUIRE(static_cast<unsigned char *>(grown)[0] == 0xab);
    REQUIRE(static_cast<unsigned char *>(grown)[19] == 0xab);

    __eco_cache_free(grown);
    __eco_cache_flush();
}

TEST_CASE("realloc across a class copies and frees", "[alloc]")
{
    __eco_cache_flush();

    void *p = __eco_cache_alloc(32);
    REQUIRE(p != nullptr);
    std::memset(p, 0xcd, 32);

    void *grown = __eco_cache_realloc(p, 100);
    REQUIRE(grown != nullptr);
    REQUIRE(grown != p);
    REQUIRE(static_cast<unsigned char *>(grown)[0] == 0xcd);
    REQUIRE(static_cast<unsigned char *>(grown)[31] == 0xcd);

    __eco_cache_free(grown);
    __eco_cache_flush();
}

TEST_CASE("a 3000-byte alloc is small", "[alloc]")
{
    __eco_cache_flush();

    void *p = __eco_cache_alloc(3000);
    REQUIRE(p != nullptr);
    REQUIRE(is_16_aligned(p));
    std::memset(p, 0x11, 3000);

    void *grown = __eco_cache_realloc(p, 3050);
    REQUIRE(grown == p);

    __eco_cache_free(grown);
    __eco_cache_flush();
}

TEST_CASE("a 40000-byte alloc is large", "[alloc]")
{
    __eco_cache_flush();

    void *p = __eco_cache_alloc(40000);
    REQUIRE(p != nullptr);
    std::memset(p, 0x22, 40000);

    void *grown = __eco_cache_realloc(p, 80000);
    REQUIRE(grown != nullptr);
    REQUIRE(static_cast<unsigned char *>(grown)[0] == 0x22);
    REQUIRE(static_cast<unsigned char *>(grown)[39999] == 0x22);

    __eco_cache_free(grown);
    __eco_cache_flush();
}

TEST_CASE("realloc of a large block preserves bytes", "[alloc]")
{
    __eco_cache_flush();

    constexpr size_t k_before = 40000;
    constexpr size_t k_after = 80000;

    void *p = __eco_cache_alloc(k_before);
    REQUIRE(p != nullptr);
    std::memset(p, 0xef, k_before);

    void *grown = __eco_cache_realloc(p, k_after);
    REQUIRE(grown != nullptr);
    REQUIRE(static_cast<unsigned char *>(grown)[0] == 0xef);
    REQUIRE(static_cast<unsigned char *>(grown)[k_before - 1] == 0xef);

    __eco_cache_free(grown);
    __eco_cache_flush();
}

TEST_CASE("realloc out of a class into a large block preserves bytes", "[alloc]")
{
    __eco_cache_flush();

    void *p = __eco_cache_alloc(8000);
    REQUIRE(p != nullptr);
    std::memset(p, 0x12, 8000);

    void *grown = __eco_cache_realloc(p, 20000);
    REQUIRE(grown != nullptr);
    REQUIRE(grown != p);
    REQUIRE(static_cast<unsigned char *>(grown)[0] == 0x12);
    REQUIRE(static_cast<unsigned char *>(grown)[7999] == 0x12);

    __eco_cache_free(grown);
    __eco_cache_flush();
}

TEST_CASE("a 20000-byte alloc is large", "[alloc]")
{
    __eco_cache_flush();

    void *p = __eco_cache_alloc(20000);
    REQUIRE(p != nullptr);
    std::memset(p, 0x33, 20000);

    void *grown = __eco_cache_realloc(p, 40000);
    REQUIRE(grown != nullptr);
    REQUIRE(static_cast<unsigned char *>(grown)[0] == 0x33);
    REQUIRE(static_cast<unsigned char *>(grown)[19999] == 0x33);

    __eco_cache_free(grown);
    __eco_cache_flush();
}

TEST_CASE("the last small class keeps the pointer", "[alloc]")
{
    __eco_cache_flush();

    void *p = __eco_cache_alloc(14000);
    REQUIRE(p != nullptr);
    REQUIRE(is_16_aligned(p));

    void *grown = __eco_cache_realloc(p, 14336);
    REQUIRE(grown == p);

    __eco_cache_free(grown);
    __eco_cache_flush();
}

TEST_CASE("null free is a no-op", "[alloc]")
{
    __eco_cache_flush();
    __eco_cache_free(nullptr);
}

TEST_CASE("realloc of null allocates", "[alloc]")
{
    __eco_cache_flush();

    void *p = __eco_cache_realloc(nullptr, 32);
    REQUIRE(p != nullptr);
    __eco_cache_free(p);
    __eco_cache_flush();
}

TEST_CASE("a free on another thread is reusable", "[alloc]")
{
    __eco_cache_flush();

    void *p = __eco_cache_alloc(48);
    REQUIRE(p != nullptr);

    std::thread worker([p]() {
        __eco_cache_free(p);
    });
    worker.join();

    void *again = __eco_cache_alloc(48);
    REQUIRE(again != nullptr);
    REQUIRE(is_16_aligned(again));

    __eco_cache_free(again);
    __eco_cache_flush();
}

TEST_CASE("a burst of frees still allocates", "[alloc]")
{
    __eco_cache_flush();

    std::vector<void *> live;
    live.reserve(10000);
    for (int i = 0; i < 10000; i++) {
        void *p = __eco_cache_alloc(48);
        REQUIRE(p != nullptr);
        live.push_back(p);
    }
    for (void *p : live) {
        __eco_cache_free(p);
    }

    void *again = __eco_cache_alloc(48);
    REQUIRE(again != nullptr);
    __eco_cache_free(again);
    __eco_cache_flush();
}

TEST_CASE("many live pages still allocate and free", "[alloc]")
{
    __eco_cache_flush();

    std::vector<void *> live;
    live.reserve(600);
    for (int i = 0; i < 600; i++) {
        void *p = __eco_cache_alloc(8000);
        REQUIRE(p != nullptr);
        live.push_back(p);
    }
    for (void *p : live) {
        __eco_cache_free(p);
    }

    void *again = __eco_cache_alloc(8000);
    REQUIRE(again != nullptr);
    __eco_cache_free(again);
    __eco_cache_flush();
}

TEST_CASE("a libc pointer is not a small slot", "[alloc]")
{
    __eco_cache_flush();

    void *foreign = std::malloc(64);
    REQUIRE(foreign != nullptr);

    __eco_cache_free(foreign);
    REQUIRE(__eco_cache_realloc(foreign, 128) == nullptr);

    std::free(foreign);
    __eco_cache_flush();
}

TEST_CASE("compiler-built runtime objects keep link reuse", "[alloc]")
{
    const Compiler::LinkRequirement profile = Compiler::profile_runtime_link("eco_profile.o");
    REQUIRE(profile.compiler_object);
    REQUIRE(profile.scheme == Compiler::LinkScheme::t_object);

    const Compiler::LinkRequirement cache = Compiler::alloc_runtime_link("eco_cache.o");
    REQUIRE(cache.compiler_object);
    REQUIRE(cache.scheme == Compiler::LinkScheme::t_object);
}

TEST_CASE("page heap is native-only", "[alloc]")
{
    Compiler::CompilerOptions options;
    options.alloc_cache = true;
    REQUIRE(options.using_alloc_cache());
    REQUIRE(options.using_page_heap());

    options.codegen = Compiler::CodegenTarget::wasi();
    REQUIRE(options.using_alloc_cache());
    REQUIRE_FALSE(options.using_page_heap());
}

TEST_CASE("two threads burst and the main thread still allocates", "[alloc]")
{
    __eco_cache_flush();

    auto burst = []() {
        std::vector<void *> live;
        live.reserve(1000);
        for (int i = 0; i < 1000; i++) {
            void *p = __eco_cache_alloc(32);
            REQUIRE(p != nullptr);
            live.push_back(p);
        }
        for (void *p : live) {
            __eco_cache_free(p);
        }
    };

    std::thread a(burst);
    std::thread b(burst);
    a.join();
    b.join();

    void *p = __eco_cache_alloc(32);
    REQUIRE(p != nullptr);
    __eco_cache_free(p);
    __eco_cache_flush();
}
