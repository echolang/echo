#ifndef ASTOPS_H
#define ASTOPS_H

#pragma once

#include "Token.h"

#include <vector>
#include <unordered_map>
#include <memory>
#include <array>

namespace AST
{
    enum class OpAssociativity
    {
        left,
        right,
        none
    };

    struct OpPrecedence
    {
        OpAssociativity assoc;
        int sequence;
    };

    struct Operator
    {
        static OpPrecedence get_precedence_for_token(const Token::Type &type);
        static TokenList shunting_yard(TokenSlice &token_slice);

        const Token::Type type;
        const OpPrecedence precedence;

        Operator(const Token::Type type, const OpPrecedence precedence) :
            type(type),
            precedence(precedence)
        {
        }

        // the operators that ask a question about two operands rather than combining them
        // the distinction matters for pointers: comparing an address against a non-address is
        // a type error, while `$p:$ + 1` mixing an address and an int is ordinary offsetting
        inline bool is_comparison() const {
            return is_identity_comparison()
                || type == Token::Type::t_open_angle
                || type == Token::Type::t_close_angle
                || type == Token::Type::t_logical_leq
                || type == Token::Type::t_logical_geq;
        }

        // the two comparisons that ask "is this the same thing", rather than ordering it. the only
        // operators a class handle answers: two handles compare as addresses, and a handle compares
        // against null. every ordering operator on a class stays a type error
        inline bool is_identity_comparison() const {
            return type == Token::Type::t_logical_eq
                || type == Token::Type::t_logical_neq;
        }

        virtual ~Operator() = default;
    };

    struct PredefinedTokenOperator : public Operator
    {
        PredefinedTokenOperator(const Token::Type &type) : 
            Operator(type, get_precedence_for_token(type))
        {
        }
    };

    struct CustomOperator : public Operator
    {
        const std::string name;
        CustomOperator(const std::string &name, const OpPrecedence precedence) : 
            Operator(Token::Type::t_op_custom, precedence),
            name(name)
        {
        }
    };

    class OperatorRegistry
    {
    public:
        OperatorRegistry();
        ~OperatorRegistry() = default;

        void register_predefined_token_op(const Token::Type &type);
        void register_custom_op(const std::string &name, int precedence, OpAssociativity assoc);
        
        const Operator *get_operator(const TokenReference &token) const;
        const Operator *get_operator(const std::string &symbol) const;

        inline const std::vector<CustomOperator *> &get_custom_operators() const {
            return _custom_operators;
        }

    private:
        std::vector<std::unique_ptr<Operator>> _operators;
        std::vector<CustomOperator *> _custom_operators;
        std::unordered_map<std::string, Operator *> _operator_symbol_map;
        std::array<Operator *, static_cast<size_t>(Token::Type::t_unknown)> _predefined_operator_map;
    };
};

#endif