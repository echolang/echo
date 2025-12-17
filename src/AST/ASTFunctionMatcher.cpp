#include "AST/ASTFunctionMatcher.h"

#include "AST/FunctionDeclNode.h"

#include <fmt/core.h>

#include <algorithm>

namespace
{
    // a candidate that survived filtering, with the per-argument fits it survived on
    struct Viable
    {
        const AST::FunctionCandidate *candidate;
        std::vector<AST::ArgumentFit> fits;
    };

    // "is `a` at least as good as `b` on every argument, and better on at least one" - the
    // Pareto comparison. arguments that told us nothing are skipped on both sides, so they can
    // neither break a tie nor create one
    bool strictly_better(const Viable &a, const Viable &b)
    {
        bool better_somewhere = false;

        for (size_t i = 0; i < a.fits.size(); i++) {
            if (a.fits[i] == AST::ArgumentFit::t_undetermined || b.fits[i] == AST::ArgumentFit::t_undetermined) {
                continue;
            }

            // the enum is ordered best to worst, so a smaller value is a better fit
            if (a.fits[i] > b.fits[i]) {
                return false;
            }

            if (a.fits[i] < b.fits[i]) {
                better_somewhere = true;
            }
        }

        return better_somewhere;
    }
}

AST::FunctionMatch AST::match_function(
    const std::vector<AST::FunctionCandidate> &candidates,
    const std::vector<AST::ValueType> &argument_types,
    const std::vector<AST::ExprNode *> &arguments
)
{
    FunctionMatch result;

    // every "this one wins" exit goes through here, so a resolved match is filled in one way only
    const auto resolved = [&result](FunctionDeclNode *decl) {
        result.decl = decl;
        result.outcome = FunctionMatch::Outcome::t_resolved;
        return result;
    };

    if (candidates.empty()) {
        result.outcome = FunctionMatch::Outcome::t_no_candidates;
        return result;
    }

    // 1. arity
    std::vector<const FunctionCandidate *> by_arity;
    for (const auto &candidate : candidates) {
        if (candidate.parameter_types.size() == argument_types.size()) {
            by_arity.push_back(&candidate);
        }
    }

    if (by_arity.empty()) {
        result.outcome = FunctionMatch::Outcome::t_no_viable;
        for (const auto &candidate : candidates) {
            result.tied.push_back(candidate.decl);
        }
        return result;
    }

    // 2. the single-candidate short circuit. deliberately before any type check: with one
    // candidate there is nothing to choose between, and letting a type mismatch fall through to
    // the type checker keeps its located, argument-numbered diagnostic as the thing the user sees
    if (by_arity.size() == 1) {
        return resolved(by_arity.front()->decl);
    }

    // 3. score, dropping anything an argument cannot reach at all
    std::vector<Viable> viable;
    for (const auto *candidate : by_arity) {
        Viable scored { candidate, {} };
        bool fits = true;

        for (size_t i = 0; i < argument_types.size(); i++) {
            auto *expr = i < arguments.size() ? arguments[i] : nullptr;
            const auto fit = argument_fit(argument_types[i], expr, candidate->parameter_types[i]);

            if (fit == ArgumentFit::t_none) {
                fits = false;
                break;
            }

            scored.fits.push_back(fit);
        }

        if (fits) {
            viable.push_back(std::move(scored));
        }
    }

    if (viable.empty()) {
        result.outcome = FunctionMatch::Outcome::t_no_viable;
        for (const auto *candidate : by_arity) {
            result.tied.push_back(candidate->decl);
        }
        return result;
    }

    // 4. Pareto: keep the candidates nothing beats
    //
    // this comes *before* the generic tiebreak below, and the order matters. how well each
    // argument fits is a statement about the call; "concrete or template" is a statement about
    // the declaration, and only gets a say once the call itself cannot tell them apart
    // reversed, `pick<T>(T)` and `pick(int32)` called with a float64 would resolve to the
    // concrete one and silently narrow, when the template matched the argument exactly
    std::vector<const Viable *> best;
    for (const auto &v : viable) {
        const bool beaten = std::any_of(viable.begin(), viable.end(), [&](const Viable &other) {
            return &other != &v && strictly_better(other, v);
        });

        if (!beaten) {
            best.push_back(&v);
        }
    }

    if (best.size() == 1) {
        return resolved(best.front()->candidate->decl);
    }

    // 5. a concrete overload settles a tie against a template. a template matches a whole family
    // of calls, so when it fits exactly as well as a function written for this one, the specific
    // one is what the author meant. narrowing only when something survives it, so a tie between
    // templates alone stays a tie rather than emptying the set
    std::vector<const Viable *> concrete;
    for (const auto *v : best) {
        if (!v->candidate->is_generic) {
            concrete.push_back(v);
        }
    }

    if (!concrete.empty()) {
        best = std::move(concrete);

        if (best.size() == 1) {
            return resolved(best.front()->candidate->decl);
        }
    }

    for (const auto *v : best) {
        result.tied.push_back(v->candidate->decl);
    }

    // 6. an undetermined argument is the difference between "the program is wrong" and "we
    // cannot tell yet". if any survivor was scored on an argument that had no type, a later
    // pass may still separate them, so this is not reported as an error here
    const bool waiting_on_a_type = std::any_of(best.begin(), best.end(), [](const Viable *v) {
        return std::any_of(v->fits.begin(), v->fits.end(), [](ArgumentFit fit) {
            return fit == ArgumentFit::t_undetermined;
        });
    });

    result.outcome = waiting_on_a_type
        ? FunctionMatch::Outcome::t_undecidable
        : FunctionMatch::Outcome::t_ambiguous;

    return result;
}

std::string AST::describe_operands(const std::vector<AST::ValueType> &operand_types)
{
    if (operand_types.empty()) {
        return "these operands";
    }

    std::string buffer;

    for (size_t i = 0; i < operand_types.size(); i++) {
        if (i > 0) {
            buffer += i + 1 == operand_types.size() ? " and " : ", ";
        }

        buffer += fmt::format("a '{}'", operand_types[i].get_type_desciption());
    }

    return buffer;
}

std::string AST::describe_candidates(const std::vector<AST::FunctionDeclNode *> &candidates)
{
    std::string buffer;

    for (const auto *candidate : candidates) {
        if (candidate == nullptr) {
            continue;
        }

        buffer += "\n  " + candidate->signature_description();
    }

    return buffer;
}
