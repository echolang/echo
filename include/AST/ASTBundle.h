#ifndef ASTBUNDLE_H
#define ASTBUNDLE_H

#pragma once

#include "ASTModule.h"
#include "ASTCollector.h"

namespace AST
{  
    class Bundle
    {
    public:
        ModuleCollection modules;
        Collector collector;

        Bundle() {};
        ~Bundle() {};

    private:

    };
};

#endif