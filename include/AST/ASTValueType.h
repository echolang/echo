#ifndef ASTVALUETYPE_H
#define ASTVALUETYPE_H

#pragma once

#include <type_traits>
#include <string>
#include <vector>
#include <map>

namespace AST
{   
    enum class ValueTypeKind {
        t_primitive,
        t_class,
        t_struct,
        t_unknown
    };

    enum class ValueTypePrimitive {
        t_complex,
        t_int8,
        t_int16,
        t_int32,
        t_int64,
        t_uint8,
        t_uint16,
        t_uint32,
        t_uint64,
        t_float32,
        t_float64,
        t_bool,
        t_void,
    };

    class ValueType {

        ValueTypeKind kind;
        ValueTypePrimitive primitive;

        std::optional<std::string> name;
        std::map<std::string, ValueType> properties;

    public:

        ValueType() = default;
        ValueType(ValueTypePrimitive primitive) : kind(ValueTypeKind::t_primitive), primitive(primitive) {}

        static ValueType void_type() {
            return ValueType(ValueTypePrimitive::t_void);
        }

        std::string get_type_signature() const {
            return "ValueType";
        }

        bool is_primitive() const {
            return kind == ValueTypeKind::t_primitive;
        }

        bool is_named() const {
            return name.has_value();
        }

    };
};

#endif