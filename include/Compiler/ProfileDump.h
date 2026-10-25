#ifndef PROFILEDUMP_H
#define PROFILEDUMP_H

#pragma once

#include "Compiler/ProfileFile.h"

#include <string>

namespace Compiler
{
    // ECODUMP bytes into a builder. `ProfileFile::open` is the magic branch
    bool parse_profile_dump(
        const unsigned char *data,
        size_t size,
        ProfileFileBuilder &out,
        std::string &out_error
    );
};

#endif
