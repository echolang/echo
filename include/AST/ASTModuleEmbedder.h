#ifndef ASTMODULEEMBEDDER_H
#define ASTMODULEEMBEDDER_H

#pragma once

#include "AST/ASTModule.h"

namespace AST
{
    void write_embedded_module(AST::Module &module, const std::string &output_path);
}
#endif