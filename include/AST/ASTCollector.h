#ifndef ASTCOLLECTOR_H
#define ASTCOLLECTOR_H

#pragma once

#include "AST/ASTValueTypeCollection.h"

namespace AST
{  
    class Collector
    {
    public:
        ValueTypeCollection value_types;

        Collector();
        ~Collector();
    };
};

#endif