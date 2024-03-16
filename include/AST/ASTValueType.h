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

    std::string get_primitive_name(ValueTypePrimitive primitive) {
        switch (primitive) {
            case ValueTypePrimitive::t_complex: return "complex";
            case ValueTypePrimitive::t_int8: return "int8";
            case ValueTypePrimitive::t_int16: return "int16";
            case ValueTypePrimitive::t_int32: return "int32";
            case ValueTypePrimitive::t_int64: return "int64";
            case ValueTypePrimitive::t_uint8: return "uint8";
            case ValueTypePrimitive::t_uint16: return "uint16";
            case ValueTypePrimitive::t_uint32: return "uint32";
            case ValueTypePrimitive::t_uint64: return "uint64";
            case ValueTypePrimitive::t_float32: return "float32";
            case ValueTypePrimitive::t_float64: return "float64";
            case ValueTypePrimitive::t_bool: return "bool";
            case ValueTypePrimitive::t_void: return "void";
        };
    }

    class ValueType {

        ValueTypeKind kind;
        ValueTypePrimitive primitive;

        std::optional<std::string> name;
        std::map<std::string, ValueType> properties;

    public:

        static ValueType make_void() {
            return ValueType(ValueTypePrimitive::t_void);
        }

        static ValueType make_unknown() {
            return ValueType(ValueTypeKind::t_unknown);
        }

        ValueType() = default;
        ValueType(ValueTypePrimitive primitive) : kind(ValueTypeKind::t_primitive), primitive(primitive) {}

        template<ValueTypeKind K, typename = std::enable_if_t<K == ValueTypeKind::t_class || K == ValueTypeKind::t_struct>>
        ValueType(ValueTypeKind kind, std::vector<ValueType> properties) : kind(kind) {
            for (auto& prop : properties) {
                this->properties[prop.name.value()] = prop;
            }
        }

        template<ValueTypeKind K, typename = std::enable_if_t<K == ValueTypeKind::t_unknown>>
        ValueType(ValueTypeKind kind) : kind(kind), primitive(ValueTypePrimitive::t_void) {}

        static ValueType void_type() {
            return ValueType(ValueTypePrimitive::t_void);
        }

        std::string get_type_match_signature() const {
            if (is_primitive()) {
                return get_primitive_name(primitive);
            }

            std::string signature = "{";
            for (auto it = properties.begin(); it != properties.end(); ++it) {
                const auto& [name, type] = *it;
                signature += type.get_type_match_signature();
                if (std::next(it) != properties.end()) {
                    signature += ", ";
                }
            }

            signature += "}";
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