#ifndef COVERAGECODEGEN_H
#define COVERAGECODEGEN_H

#pragma once

#include "Compiler/CoverageDriver.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace llvm
{
    class BasicBlock;
    class Function;
    class GlobalVariable;
    class Module;
    class StructType;
};

namespace AST
{
    class FunctionDeclNode;
    class Node;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // the one owner of which IR site becomes a coverage counter. call sites are
    // unconditional; each method returns immediately when coverage is off, so a
    // test compile without the flag emits the bytes it emits today
    class CoverageCodegen
    {
    public:
        CoverageCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        void emit_start();
        void finish_units();

        void instrument_function(llvm::Function &fn, const AST::FunctionDeclNode &decl);
        void instrument_statement(AST::Node &node);
        void instrument_branch(AST::Node &node, std::string_view edge);

        // merge when coverage is off or this body is skipped; a skip block the caller
        // condbrs to otherwise, so gen_if / gen_loop never name coverage themselves
        llvm::BasicBlock *false_edge(
            llvm::BasicBlock *merge,
            AST::Node &node,
            std::string_view edge
        );

        Compiler::CoverageModuleSites module_sites() const;

    private:
        struct FunctionState
        {
            std::string mangled;
            std::string module;
            std::string dir;
            std::string function;
            std::vector<Compiler::CoverageSite> sites;
            std::unordered_map<std::string, uint32_t> site_ids;
            uint32_t branch_seq = 0;
            bool sealed = false;
        };

        CodegenContext &_ctx;
        std::map<std::string, FunctionState> _functions;

        bool active() const;
        bool should_instrument(const AST::FunctionDeclNode *decl) const;
        FunctionState *state_for(const AST::FunctionDeclNode *decl);
        llvm::StructType *reg_type();
        llvm::GlobalVariable *ensure_base(FunctionState &state);
        uint32_t add_site(
            FunctionState &state,
            Compiler::CoverageSiteKind kind,
            std::string_view edge,
            const AST::Node &node
        );
        void plant(
            Compiler::CoverageSiteKind kind,
            std::string_view edge,
            const AST::Node &node
        );
        void increment(FunctionState &state, uint32_t id);
        void emit_function_reg(llvm::Module &owner, FunctionState &state);
        std::string site_key(
            FunctionState &state,
            Compiler::CoverageSiteKind kind,
            std::string_view edge,
            const AST::Node &node
        ) const;
    };
};

#endif
