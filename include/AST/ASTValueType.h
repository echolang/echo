#ifndef ASTVALUETYPE_H
#define ASTVALUETYPE_H

#pragma once

#include <type_traits>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <cstdint>
#include <cassert>

namespace AST
{   
    enum class ValueTypeKind {
        t_primitive,
        t_class,
        t_struct,
        t_unknown
    };

    enum class ValueTypeFlags {
        t_const = 1 << 0,
        t_pointer = 1 << 1,
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
            if (!is_signed) return 0;
            return -(1LL << (size * 8 - 1));
        }

        uint64_t get_max_positive_value() const {
            if (is_signed)
                return (1ULL << (size * 8 - 1)) - 1;
            else
                return (1ULL << (size * 8)) - 1;
        }
    };

    std::string get_primitive_name(ValueTypePrimitive primitive);
    uint8_t get_primitive_size(ValueTypePrimitive primitive);
    char get_primitive_id_char(ValueTypePrimitive primitive);
    IntegerSize get_integer_size(ValueTypePrimitive primitive);

    class ValueType 
    {
        ValueTypeKind kind;
        ValueTypePrimitive primitive;
        uint8_t type_flags = 0;

        // @todo move this to the ComplexType class
        std::optional<std::string> name;
        std::map<std::string, ValueType> properties;

        // ComplexType *complex_type

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

        static ValueType make_const(ValueType type) {
            type.set_const(true);
            return type;
        }

        static ValueType make_pointer(ValueType type) {
            type.set_pointer(true);
            return type;
        }

        ValueType() = default;
        ValueType(ValueTypePrimitive primitive) : kind(ValueTypeKind::t_primitive), primitive(primitive) {}

        template<ValueTypeKind K, typename = std::enable_if_t<K == ValueTypeKind::t_class || K == ValueTypeKind::t_struct>>
        ValueType(ValueTypeKind kind, std::vector<ValueType> properties) : kind(kind) {
            for (auto& prop : properties) {
                this->properties[prop.name.value()] = prop;
            }
        }

        bool is_const() const {
            return type_flags & static_cast<uint8_t>(ValueTypeFlags::t_const);
        }

        bool is_pointer() const {
            return type_flags & static_cast<uint8_t>(ValueTypeFlags::t_pointer);
        }

        void set_const(bool is_const) {
            if (is_const) {
                type_flags |= static_cast<uint8_t>(ValueTypeFlags::t_const);
            } else {
                type_flags &= ~static_cast<uint8_t>(ValueTypeFlags::t_const);
            }
        }

        void set_pointer(bool is_pointer) {
            if (is_pointer) {
                type_flags |= static_cast<uint8_t>(ValueTypeFlags::t_pointer);
            } else {
                type_flags &= ~static_cast<uint8_t>(ValueTypeFlags::t_pointer);
            }
        }

        bool is_primitive() const {
            return kind == ValueTypeKind::t_primitive;
        }

        bool is_primitive_of_type(ValueTypePrimitive primitive) const {
            return is_primitive() && this->primitive == primitive;
        }

        bool is_void() const {
            return is_primitive_of_type(ValueTypePrimitive::t_void);
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

        bool is_integer_type() const {
            return is_signed_integer() || is_unsigned_integer();
        }

        bool is_boolean_type() const {
            return is_primitive() && primitive == ValueTypePrimitive::t_bool;
        }

        bool will_fit_into(ValueType other) const;

        bool is_same_size(ValueType other) const;
        
        inline ValueTypePrimitive get_primitive_type() const {
            return primitive;
        }

        bool is_named() const {
            return name.has_value();
        }

        // compare two types
        bool operator==(const ValueType& other) const {
            if (is_primitive() && other.is_primitive()) {
                return primitive == other.primitive;
            }

            assert(false && "Not implemented");
        }

        std::string get_mangled_name() const;

        std::string get_type_desciption() const {
            std::string prefix = is_const() ? "const " : "";
            std::string pointer = is_pointer() ? "*" : "";

            if (is_named()) {
                return prefix + name.value() + pointer;
            }

            return prefix + get_primitive_name(primitive) + pointer;
        }

    };
};

#endif