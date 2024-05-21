#ifndef ASTVALUETYPE_H
#define ASTVALUETYPE_H

#pragma once

#include <type_traits>
#include <string>
#include <vector>
#include <map>
#include <optional>

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

    struct IntegerSize {
        uint8_t size;
        bool is_signed;

        IntegerSize(uint8_t size, bool is_signed) : size(size), is_signed(is_signed) {}

        int64_t get_max_negative_value() const {
            return is_signed ? -(1 << (size * 8 - 1)) : 0;
        }

        uint64_t get_max_positive_value() const {
            return is_signed ? (1 << (size * 8 - 1)) - 1 : (1 << (size * 8)) - 1;
        }
    };

    std::string get_primitive_name(ValueTypePrimitive primitive);
    uint8_t get_primitive_size(ValueTypePrimitive primitive);
    IntegerSize get_integer_size(ValueTypePrimitive primitive);

    class ValueType {

        ValueTypeKind kind;
        ValueTypePrimitive primitive;

        std::optional<std::string> name;
        std::map<std::string, ValueType> properties;

        ValueType(ValueTypeKind kind, ValueTypePrimitive primitive) : kind(kind), primitive(primitive) {}

    public:

        static ValueType make_void() {
            return ValueType(ValueTypePrimitive::t_void);
        }

        static ValueType make_unknown() {
            return ValueType(ValueTypeKind::t_unknown, ValueTypePrimitive::t_void);
        }

        static ValueType void_type() {
            return ValueType(ValueTypePrimitive::t_void);
        }


        ValueType() = default;
        ValueType(ValueTypePrimitive primitive) : kind(ValueTypeKind::t_primitive), primitive(primitive) {}

        template<ValueTypeKind K, typename = std::enable_if_t<K == ValueTypeKind::t_class || K == ValueTypeKind::t_struct>>
        ValueType(ValueTypeKind kind, std::vector<ValueType> properties) : kind(kind) {
            for (auto& prop : properties) {
                this->properties[prop.name.value()] = prop;
            }
        }

        bool is_primitive() const {
            return kind == ValueTypeKind::t_primitive;
        }

        bool is_primitive_of_type(ValueTypePrimitive primitive) const {
            return is_primitive() && this->primitive == primitive;
        }

        bool is_numeric_type() const {
            if (!is_primitive()) {
                return false;
            }

            switch (this->primitive)
            {
            case ValueTypePrimitive::t_int8:
            case ValueTypePrimitive::t_int16:
            case ValueTypePrimitive::t_int32:
            case ValueTypePrimitive::t_int64:
            case ValueTypePrimitive::t_uint8:
            case ValueTypePrimitive::t_uint16:
            case ValueTypePrimitive::t_uint32:
            case ValueTypePrimitive::t_uint64:
            case ValueTypePrimitive::t_float32:
            case ValueTypePrimitive::t_float64:
                return true;
            
            default:
                return false;
            }
        }

        bool is_floating_type() const {
            if (!is_primitive()) {
                return false;
            }

            switch (this->primitive)
            {
            case ValueTypePrimitive::t_float32:
            case ValueTypePrimitive::t_float64:
                return true;
            
            default:
                return false;
            }
        }

        bool is_signed_integer() const {
            if (!is_primitive()) {
                return false;
            }

            switch (this->primitive)
            {
            case ValueTypePrimitive::t_int8:
            case ValueTypePrimitive::t_int16:
            case ValueTypePrimitive::t_int32:
            case ValueTypePrimitive::t_int64:
                return true;
            
            default:
                return false;
            }
        }

        bool is_unsigned_integer() const {
            if (!is_primitive()) {
                return false;
            }

            switch (this->primitive)
            {
            case ValueTypePrimitive::t_uint8:
            case ValueTypePrimitive::t_uint16:
            case ValueTypePrimitive::t_uint32:
            case ValueTypePrimitive::t_uint64:
                return true;
            
            default:
                return false;
            }
        }

        bool is_integer() const {
            return is_signed_integer() || is_unsigned_integer();
        }

        bool will_fit_into(ValueType other) const;
        
        inline ValueTypePrimitive get_primitive_type() const {
            return primitive;
        }

        bool is_named() const {
            return name.has_value();
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
            return signature;
        }

        std::string get_type_desciption() const {
            if (is_named()) {
                return name.value();
            }

            return get_type_match_signature();
        }

    };
};

#endif