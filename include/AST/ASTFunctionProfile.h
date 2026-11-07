#ifndef ASTFUNCTIONPROFILE_H
#define ASTFUNCTIONPROFILE_H

#pragma once

namespace AST
{
    class FunctionDeclNode;

    // does this function get an exact-calls frame. pure, so ODR-safe
    bool profile_instruments(const FunctionDeclNode *decl, bool include_stdlib);

    // coverage plants counters here. the profile gate, minus test bodies
    bool coverage_instruments(const FunctionDeclNode *decl, bool include_stdlib);
};

#endif
