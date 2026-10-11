#ifndef FNV_H
#define FNV_H

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Compiler
{
    // fnv-1a, 64 bit. this answers "are these the same inputs as last time",
    // where the adversary is an edited source file. the module cache, the C
    // object cache, and AST::abi_fingerprint all fold with this one function
    constexpr uint64_t k_fnv_offset_basis = 1469598103934665603ULL;
    constexpr uint64_t k_fnv_prime = 1099511628211ULL;

    inline uint64_t fnv1a64(const void *data, size_t length, uint64_t seed)
    {
        const auto *bytes = static_cast<const unsigned char *>(data);
        uint64_t hash = seed;

        for (size_t i = 0; i < length; i++) {
            hash ^= bytes[i];
            hash *= k_fnv_prime;
        }

        return hash;
    }

    inline uint64_t fnv1a64(const std::string &text, uint64_t seed)
    {
        return fnv1a64(text.data(), text.size(), seed);
    }
};

#endif
