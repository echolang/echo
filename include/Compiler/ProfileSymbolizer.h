#ifndef PROFILESYMBOLIZER_H
#define PROFILESYMBOLIZER_H

#pragma once

#include "Compiler/ProfileFile.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Compiler
{
    struct ProfileBinaryIdentity
    {
        std::filesystem::path path;
        std::array<uint8_t, 16> uuid {};
        uint64_t anchor_file = 0;
        bool has_uuid = false;
        bool has_anchor = false;
    };

    // address -> function, file:line, inlined frames. DWARF on the dSYM companion
    // (or the executable), covering symbols from the executable for a name DWARF
    // did not carry. Darwin slide is `anchor_runtime - file address of __eco_prof_anchor`
    class ProfileSymbolizer
    {
    public:
        static bool read_identity(
            const std::filesystem::path &binary,
            ProfileBinaryIdentity &out,
            std::string &out_error
        );

        static bool symbolize(
            ProfileFile &file,
            const std::filesystem::path &binary,
            std::string &out_error
        );
    };
};

#endif
