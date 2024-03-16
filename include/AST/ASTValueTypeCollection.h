#ifndef ASTVALUETYPECOLLECTION_H
#define ASTVALUETYPECOLLECTION_H

#pragma once

#include <vector>
#include "AST/ASTValueType.h"

namespace AST
{   
    typedef size_t vt_handle_t;
    
    class ValueTypeCollection
    {
        std::vector<ValueType> value_types;

    public:

        ValueTypeCollection();
        ~ValueTypeCollection();

        vt_handle_t push_type(ValueType type);

    private:

    };
};

#endif