#ifndef ASTMONOMORPHIZER_H
#define ASTMONOMORPHIZER_H

#pragma once

#include "AST/ASTBundle.h"
#include "AST/ASTValueType.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AST
{
    class FunctionDeclNode;
    class FunctionCallExprNode;

    // resolves every generic use in the bundle into concrete instances before codegen.
    // a generic function call is instantiated by cloning its template with concrete type
    // arguments (explicit ones win, otherwise they are inferred from the call arguments),
    // and the call site is rewritten to point at the concrete instance. it runs after
    // parsing and before compilation; problems are reported as issues on the collector and
    // never thrown, so main() keeps gating on has_critical_issues() as before.
    class Monomorphizer
    {
    public:
        explicit Monomorphizer(Bundle &bundle);

        void run();

        // a human-readable dump of what monomorphization produced: every concrete instance created,
        // every call site rewired to an instance, and every struct instantiation interned. reuses
        // the same type descriptions as --print-ast so the two can be cross-checked. drives
        // --print-instances; call after run().
        std::string debug_dump_instances() const;

    private:
        Bundle &_bundle;
        Collector &_collector;

        // every function declaration mapped to the module that owns it, so instances are
        // cloned into the template's module (keeping copied token references valid).
        std::unordered_map<const FunctionDeclNode *, Module *> _decl_module;

        // interned function instances, keyed by (template, concrete args).
        std::unordered_map<std::string, FunctionDeclNode *> _func_instances;

        // call sites already handled, so re-scans skip them.
        std::unordered_set<const FunctionCallExprNode *> _processed;

        size_t _instance_count = 0;

        // set once the instance cap fires, so the runaway-instantiation diagnostic is reported a
        // single time rather than for every subsequent over-cap request.
        bool _instance_cap_reported = false;

        // when true (env ECO_TRACE_MONO set) the pass prints its per-round resolution decisions to
        // stdout, replacing the add/remove-fprintf loop the retrospective flagged as its own cost.
        bool _trace = false;

        // returns the concrete type arguments for a generic call, or nullopt when the call
        // cannot be resolved yet (it sits in an un-instantiated template body) or is invalid
        // (in which case an issue is recorded and `is_error` is set).
        std::optional<std::vector<ValueType>> determine_type_args(FunctionCallExprNode *call, Module &mod, bool &is_error);

        FunctionDeclNode *get_or_create_function_instance(FunctionDeclNode *tmpl, const std::vector<ValueType> &args);

        void insert_argument_casts(FunctionCallExprNode *call, FunctionDeclNode *instance, Module &mod);

        // matches a parameter type against a concrete argument type, capturing the type
        // parameters it mentions (handles bare T and nested applications like Box<T>).
        void unify(const ValueType &param, const ValueType &arg, std::vector<ValueType> &out, std::vector<bool> &resolved);

        CodeRef code_ref_for(Module &mod, const TokenReference &token);
    };
}

#endif
