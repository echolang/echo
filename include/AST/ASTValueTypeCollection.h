#ifndef ASTVALUETYPECOLLECTION_H
#define ASTVALUETYPECOLLECTION_H

#pragma once

#include "AST/ASTValueType.h"

#include <vector>

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
