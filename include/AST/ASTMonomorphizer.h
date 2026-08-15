#ifndef ASTMONOMORPHIZER_H
#define ASTMONOMORPHIZER_H

#pragma once

#include "AST/ASTBundle.h"
#include "AST/ASTInstantiation.h"
#include "AST/ASTOperatorRewriter.h"
#include "AST/ASTForeachLowering.h"
#include "AST/ASTGuardLowering.h"
#include "AST/ASTMatchResolution.h"
#include "AST/ASTInterpolationLowering.h"
#include "AST/ASTConstFolding.h"
#include "AST/ASTOwnership.h"
#include "AST/ASTValueType.h"

#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AST
{
    class FunctionDeclNode;
    class FunctionCallExprNode;

    // resolves every generic use in the bundle into concrete instances before codegen
    // a generic function call is instantiated by cloning its template with concrete type
    // arguments (explicit ones win, otherwise they are inferred from the call arguments),
    // and the call site is rewritten to point at the concrete instance. it runs after
    // parsing and before compilation; problems are reported as issues on the collector and
    // never thrown, so main() keeps gating on has_critical_issues() as before
    class Monomorphizer
    {
    public:
        explicit Monomorphizer(Bundle &bundle);

        // the fixpoint, which also drives AST::OwnershipPass - see the comment on the member below
        void run();

        // a human-readable dump of what monomorphization produced: every concrete instance created,
        // every call site rewired to an instance, and every struct instantiation interned. reuses
        // the same type descriptions as --print-ast so the two can be cross-checked. drives
        // --print-instances; call after run().
        std::string debug_dump_instances() const;

    private:
        Bundle &_bundle;
        Collector &_collector;

        // single-ownership resolution: drop insertion, `mv` and the moved-state analysis. driven from
        // *inside* this fixpoint rather than as a pass of its own, because the two feed each other -
        // whether a `T $x` local owns anything is only answerable after substitution, and the drop
        // the ownership pass then inserts for a `Box<int32>` local is a new generic call site that
        // this fixpoint has to instantiate. it lives in its own translation unit; only the call is
        // here
        OwnershipPass _ownership;

        // `const if` and `const(...)`, decided and then removed. driven from inside here because what a
        // condition asks about is a type this fixpoint decides, and it must run before anything else in
        // the round touches an arm that is about to disappear - see its header, and the ordering comment
        // at its call site in run()
        ConstFolding _const_folding;

        // operand syntax whose meaning depends on a type this fixpoint is still deciding: a bracket,
        // and an operator over a bare type parameter. driven from inside here for the ownership
        // pass's exact reason, and see its header for the ordering the round depends on
        OperatorRewriter _operators;

        // a `guard` over a type of the author's own, rewritten into the hoisted subject and the two
        // protocol calls it stands for. driven from inside here because minting `has_value()` on a
        // generic subject *creates a generic call site*, which is the ownership pass's exact reason -
        // see its header for the ordering the round depends on. a `T?` never reaches it
        GuardLowering _guards;
        MatchResolution _matches;

        // `foreach`, rewritten into the iterator declaration and the `while` it stands for. driven from
        // inside here because the element type is only knowable after resolution, and it must run before
        // the ownership pass reaches the body - see its header for the ordering the round depends on
        ForeachLowering _foreach;

        // an interpolated string literal, rewritten into the `str::from` calls and the concatenation
        // it stands for. driven from inside here because a call it mints may name a user's own
        // overload, and because every one of them returns an owning `string` the ownership pass has
        // to see before it walks the body - see its header
        InterpolationLowering _interpolation;

        // every function declaration mapped to the module that owns it, so instances are
        // cloned into the template's module (keeping copied token references valid)
        std::unordered_map<const FunctionDeclNode *, Module *> _decl_module;

        // interned function instances, keyed structurally by (template, concrete args) - the same
        // identity TypeRegistry uses for struct instantiations. a rendered-string key would inherit
        // whatever get_mangled_name() happens to lose (e.g. every anonymous complex type mangles
        // alike), silently handing two distinct instantiations the same instance
        typedef std::tuple<const FunctionDeclNode *, std::vector<ValueType>> InstanceKey;

        struct InstanceKeyHash
        {
            size_t operator()(const InstanceKey &key) const {
                size_t h1 = std::hash<const FunctionDeclNode *>{}(std::get<0>(key));
                size_t h2 = 0;
                for (const auto &vt : std::get<1>(key)) {
                    h2 ^= std::hash<AST::ValueType>{}(vt) + 0x9e3779b9 + (h2 << 6) + (h2 >> 2);
                }
                return h1 ^ (h2 << 1);
            }
        };

        std::unordered_map<InstanceKey, FunctionDeclNode *, InstanceKeyHash> _func_instances;

        // generic call sites this pass is done with: instantiated, or reported as unresolvable. only
        // *this* pass's business - whether a call is finished overall is AST::CallSettlement on the
        // node, which is one answer shared by the parser, this pass and the finalizing sweep
        std::unordered_set<const FunctionCallExprNode *> _processed;

        size_t _instance_count = 0;

        // set once the instance cap fires, so the runaway-instantiation diagnostic is reported a
        // single time rather than for every subsequent over-cap request
        bool _instance_cap_reported = false;

        // when true (env ECO_TRACE_MONO set) the pass prints its per-round resolution decisions to
        // stdout, replacing the add/remove-fprintf loop the retrospective flagged as its own cost
        bool _trace = false;

        // returns the concrete type arguments for a generic call, or nullopt when the call
        // cannot be resolved yet (it sits in an un-instantiated template body) or is invalid
        // (in which case an issue is recorded and `is_error` is set)
        //
        // the inference itself is AST::can_instantiate's, shared with the overload resolution that
        // scores a template candidate; this adds only the diagnostics, which is the one thing a
        // candidate-scoring caller must not do
        std::optional<std::vector<ValueType>> determine_type_args(FunctionCallExprNode *call, Module &mod, bool &is_error);

        FunctionDeclNode *get_or_create_function_instance(FunctionDeclNode *tmpl, const std::vector<ValueType> &args);

        // every call in the bundle, snapshotted before anything is instantiated - cloning appends to
        // the very collections this walks
        std::vector<std::pair<FunctionCallExprNode *, Module *>> snapshot_calls();

        // one round's steps, in the order they have to run in - see the comments on each
        bool instantiate_generic_calls(size_t round);
        bool rederive_stale_variable_types();
        bool rederive_stale_capture_types();
        bool settle_calls();

        // after the fixpoint has stopped: report every call that never resolved, and give the ones
        // that resolved but never got concrete arguments the coercion they would have got anyway, so
        // the passes below this one see the tree shape they expect
        void finalize_calls();

        // the wording finalize_calls owes a call whose name resolved to no candidates at all.
        // AST::CallResolver deliberately does not report that state - "no such function" and "no such
        // member" are different errors at different tokens - so the last caller words it
        void report_unknown_name(FunctionCallExprNode &call, const CodeRef &at);

        CodeRef code_ref_for(Module &mod, const TokenReference &token);
    };
};

#endif
