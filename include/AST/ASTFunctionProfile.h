#ifndef ASTFUNCTIONPROFILE_H
#define ASTFUNCTIONPROFILE_H

#pragma once

namespace AST
{
    class FunctionDeclNode;

    // does this function get an exact-calls frame. pure, so ODR-safe
    bool profile_instruments(const FunctionDeclNode *decl, bool include_stdlib);
};

#endif
