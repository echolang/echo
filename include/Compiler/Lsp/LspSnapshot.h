#ifndef LSPSNAPSHOT_H
#define LSPSNAPSHOT_H

#pragma once

#include "AST/ASTBundle.h"
#include "Compiler/Lsp/LspPositionIndex.h"

#include <memory>

namespace Compiler
{
    namespace Lsp
    {
        // the last good compile. Session publishes one; Query reads it. lives and dies
        // with the bundle's arenas, which the index holds Node* / File* into
        struct Snapshot
        {
            std::unique_ptr<AST::Bundle> bundle;
            PositionIndex index;
        };
    };
};

#endif
