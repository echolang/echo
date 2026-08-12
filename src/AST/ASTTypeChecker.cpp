#include "AST/ASTTypeChecker.h"


#include "AST/ASTConstFold.h"
#include "AST/ASTOperatorSemantics.h"

#include "AST/ASTModule.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/AssignNode.h"
#include "AST/VarRefNode.h"
#include "AST/ExprNode.h"
#include "AST/TypeCastNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/GuardNode.h"
#include "AST/ReturnNode.h"
#include "AST/ASTArgumentFit.h"
#include "AST/ASTBuiltin.h"
#include "AST/ASTConformance.h"
#include "AST/ASTConstness.h"
#include "AST/ASTDestruction.h"
#include "AST/ASTFunctionEmission.h"
#include "AST/ASTNullability.h"
#include "AST/ASTAccess.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/LiteralValueNode.h"

#include <fmt/core.h>

namespace AST
{

// **does this destination admit a `null`?** the question and the answer, in one place.
//
// `check_destination_fits` exempts a null value on purpose - null answers to its own rules - and those
// rules then got spelled out again at every arrival site that cared. Three of the five were missing the
// callable case, which reached codegen as a null aggregate, and that either crashed the compiler or
// produced a value that faults when called.
//
// Answers with the reason rather than a bool, so each site can frame it for the destination it is
static const char *null_rejection_reason(const ValueType &to)
{
    // **one question now: does this destination admit absence at all?** it used to be a list of the kinds
    // that happened to reject null, which meant every kind not on the list quietly accepted it - and a
    // class was the big one. `Foo $x = null;` compiled, so a class handle was nullable whether its author
    // wanted it or not, and nothing could be relied on to hold an object
    //
    // a weak reference is admitted without carrying the flag: an empty weak is an ordinary value, and
    // `weak<T>` has no non-empty spelling to contrast with the way `ptr<T>` has `T&`
    //
    // asked of AST::destination_admits_null, which is the same call the four binding sites make. this is
    // the reject half of that rule and they are the accept half - spelled apart they drift, and a drift
    // here is either a null AST::CallResolver bound being reported as an error or one it refused reaching
    // codegen with no diagnostic at all
    if (destination_admits_null(to)) {
        return nullptr;
    }

    // a borrow is the type that promises it is never null, so seeding one with null defeats the only
    // guarantee it carries (book/concept/pointers_and_refs_v2.md, "Two pointer types"). it keeps its own
    // message because it has its own *other* spelling - `ptr<T>`, not `T&?`
    if (to.is_pointer()) {
        return "declare it as a nullable pointer instead";
    }

    // a callable's *environment* slot is nullable - that is how a non-capturing closure is represented -
    // but its function slot is not. so a `function<...>` still rejects null, and now has somewhere to send
    // the author who wanted one: `function<...>?` is a tagged pair with a real empty value
    if (to.is_callable()) {
        return "a callable has no empty value - write 'function<...>?' if it may be absent";
    }

    // and everything else, which before this could not be written at all. the message names the spelling
    // rather than only refusing, because the destination is almost always one `?` away from being right
    return "add '?' to its type if it may be absent";
}

// the destinations with no conversion to fall back on, so they have to be satisfied exactly. A
// pointer's conversions are directional - `T&` widens to `ptr<T>`, never the reverse - and a struct
// or a class has none at all.
//
// Primitive-to-primitive is deliberately *not* in here. Fitting an int32 literal into a float64 slot
// is TypeLowering::coerce_value's job, which is why this is not simply `!is_implicitly_convertible`.
//
// The struct half is what catches `Foo $x = 42;`. The parser used to reject that while typing the
// literal, but a hint that cannot type a literal is now ignored there, and coerce_value passes a
// non-primitive destination straight through
static bool demands_exact_conversion(const ValueType &type)
{
    // a callable joins the list for the same reason a pointer is on it: there is no conversion between
    // two signatures. leaving it off would let a cast be silently accepted between two callables that
    // agree on nothing, and the only thing that catches a wrong `fn` slot afterwards is a crash
    // and a weak, for the third time the same reason: there is no conversion into or out of one, so a cast
    // that claimed otherwise would be reinterpreting a handle whose object may already be gone
    return type.is_pointer() || type.has_complex_type() || type.is_callable() || type.is_weak();
}

// names the storage an assignment target denotes, so a const diagnostic can say what the user
// wrote rather than only what its type is. empty for the shapes that have no name of their own
static std::string place_description(const ExprNode &expr)
{
    switch (expr.get_node_type()) {
        case NodeType::n_varref:
        {
            auto &ref = static_cast<const VarRefNode &>(expr);
            return ref.is_var() ? ref.get_var().decl().name_full() : "";
        }

        case NodeType::n_member_access:
            return static_cast<const MemberAccessNode &>(expr).get_member_name().value();

        default:
            return "";
    }
}

// **can this argument reach this parameter?** one rule, AST::argument_fit.
//
// It is also what the overload matcher ranks with and what the implicit borrow is decided by, so a
// call this pass accepts is a call resolution could have chosen, and vice versa. This used to be a
// fourth hand-written copy of the same case analysis, and it disagreed with argument_fit about the
// borrow arm, which additionally requires the argument to be a place.
//
// Numeric conversions are inserted as casts by AST::CallResolver, so a t_conversion answer is a legal
// argument here and only t_none is a real error. An undeterminable type answers t_undetermined, which
// is how "no information yet" stays out of this pass's diagnostics.
//
// `expr` is the argument as written, or null when only its type is available. Passing it admits
// t_borrow - the parameter is a borrow, this is a place, so an address would be taken. That is right
// at a call site and wrong for a cast, because a cast is not an address-of, and the two callers
// differ on exactly that
static bool arg_assignable_to(const ValueType &arg, const ExprNode *expr, const ValueType &param)
{
    return argument_fit(arg, expr, param) != ArgumentFit::t_none;
}

// the same rule asked about an implicit cast rather than an argument, named so the intent is not a
// null pointer the reader has to interpret: no expression is offered, so the borrow arm is declined,
// because a cast is not an address-of
static bool implicit_conversion_is_legal(const ValueType &from, const ValueType &to)
{
    return arg_assignable_to(from, nullptr, to);
}

TypeChecker::TypeChecker(Bundle &bundle, Compiler::CompilerOptions options) :
    _bundle(bundle),
    _collector(bundle.collector),
    _options(options)
{
}

CodeRef TypeChecker::code_ref_for(const TokenReference &token)
{
    return CodeRef{_current_module, _current_file, token.make_slice()};
}

DeclarationOrigin TypeChecker::current_origin() const
{
    // **a generic instantiation's body is asked from nowhere, deliberately.**
    //
    // a template body is source written in one module and *compiled into* whichever module instantiates it,
    // so "which module is asking" has no single answer - and the answer that looks obvious, the template's
    // own, is the one that breaks the language's own extension points. `map<K, V>` calls `hash::of($key)`
    // and `operator ==`, and for a user's key type those live in the *user's* module: judging that call
    // against `stdlib` refuses a program whose author did nothing wrong and cannot see the reason.
    //
    // an unknown origin reaches everywhere, which is the rule AST::visible_from already states for a site
    // the walk could not place - so this is that rule and not a second one
    if (_current_function != nullptr && _current_function->is_instantiated()) {
        return DeclarationOrigin {};
    }

    // otherwise the enclosing declaration's own stamp. off the declaration rather than off the walk, so an
    // ordinary body is judged against the file it was written in whatever reached it
    if (_current_function != nullptr && _current_function->declared_in.is_known()) {
        return _current_function->declared_in;
    }

    // and at file scope there is no declaration to ask - where the walk is exactly right, a file root being
    // reachable only from the file it is
    return DeclarationOrigin { _current_module, _current_file };
}

const ComplexType *TypeChecker::enclosing_type() const
{
    return _current_function != nullptr ? enclosing_type_of(*_current_function) : nullptr;
}

void TypeChecker::run()
{
    for (auto &module_ptr : _bundle.modules) {
        _current_module = module_ptr.get();
        for (auto &file : module_ptr->files()) {
            _current_file = &file;
            if (file.root) {
                file.root->accept(*this);
            }
        }
    }
}

void TypeChecker::visitFunctionDecl(FunctionDeclNode &node)
{
    // **ahead of the generic early-return below, deliberately** - the same reason check_conformances
    // sits ahead of visit_type_decl's. a template with no body is one of the shapes this reports, and
    // it has no instance for the return to hand the check to: nothing instantiates a body that is not
    // there
    check_has_implementation(node);

    // a generic template's body legitimately mentions its type parameters; it is only
    // meaningful once cloned into a concrete instance, which is checked separately
    if (node.is_generic()) {
        return;
    }

    FunctionDeclNode *prev = _current_function;
    _current_function = &node;

    // a body is its own region of source, so it starts outside every `unsafe` block - see the field's
    // comment. without this, a function declared inside one would carry the promise into code that
    // shows no sign of it
    const size_t prev_unsafe = _unsafe_depth;
    _unsafe_depth = 0;

    RecursiveVisitor::visitFunctionDecl(node);

    _unsafe_depth = prev_unsafe;
    _current_function = prev;
}

void TypeChecker::visitScope(ScopeNode &node)
{
    _unsafe_depth += node.is_unsafe ? 1 : 0;

    RecursiveVisitor::visitScope(node);

    _unsafe_depth -= node.is_unsafe ? 1 : 0;
}

// **a private property is reachable from inside its own type and nowhere else.**
//
// the modifier is what turns an invariant from something a library keeps into something the compiler
// knows. `mem::buffer<T>` says exactly one value names its allocation; without this,
// `$b->data:$ = $a->data;` builds a second owner by hand, in ordinary safe code, and the claim every
// aliasing conclusion rests on is a convention. see notes/aliasing.md
//
// reported here rather than at the layout, because privacy is about the *site* and the layout has no
// idea where it is being read from
void TypeChecker::check_private_member(
    MemberAccessNode &node,
    const ComplexType &complex,
    const ComplexType::Property &property
)
{
    if (!property.is_private) {
        return;
    }

    if (can_reach_private_member(enclosing_type(), &complex)) {
        return;
    }

    _collector.collect_issue<Issue::PrivateMember>(
        code_ref_for(node.get_member_name()),
        property.name,
        complex.namespaced_name());
}

// **asked of a settled call, on purpose.** an invisible declaration is not filtered out of the overload
// set: it competes like any other candidate and the *chosen* one is then refused by name, so a program can
// never quietly bind a different overload than the one written. what the author reads is the declaration
// they meant and why it is out of reach, not "no such function"
void TypeChecker::check_call_visibility(FunctionCallExprNode &node)
{
    FunctionDeclNode *decl = node.decl;

    if (decl == nullptr) {
        return;
    }

    // **a constructor call is the one place a type is named without being spelled in type position.**
    // `Hidden(1)` builds a value and mentions no type at all as far as Parser::parse_value_type is
    // concerned, so the three type-name sites never see it - and without this arm the whole modifier is
    // sidesteppable by leaving the declared type off the local
    //
    // asked of the *layout* rather than of a TypeDeclNode, which an instantiation has not got, and through
    // AST::enclosing_type_of, `owner_type` being null on a constructor
    if (decl->is_constructor()) {
        if (const ComplexType *built = enclosing_type_of(*decl)) {
            const ComplexType *declared = built->template_or_self();
            const DeclarationOrigin from = current_origin();

            // asked before the sentence is worded - the AST::visible_from / AST::visibility_refusal split
            // exists so that the common answer costs no name and no format, and every constructor call in
            // the program reaches this
            if (!visible_from(declared->visibility, declared->declared_in, from)) {
                // a constructor's `name_token` *is* its struct's name token - one of the two things
                // FunctionDeclNode keeps a separate `declaration_token` for - so the label lands on the
                // type declaration, which is where the modifier was written
                _collector.collect_issue<Issue::InaccessibleDeclaration>(
                    code_ref_for(node.token_function_name),
                    visibility_refusal(
                        declared->visibility, declared->declared_in, from, built->namespaced_name()),
                    declared->declared_in,
                    decl->name_token);

                return;
            }
        }
    }

    if (decl->visibility == Visibility::t_public) {
        return;
    }

    // the *owner* axis: a `private` method or constructor. asked through AST::enclosing_type_of on both
    // sides rather than off `owner_type`, which is null on a constructor - the one shape that has to reach
    // its own type's privates and the reason that function exists at all
    if (decl->visibility == Visibility::t_owner) {
        const ComplexType *owner = enclosing_type_of(*decl);

        if (can_reach_private_member(enclosing_type(), owner)) {
            return;
        }

        _collector.collect_issue<Issue::PrivateMethod>(
            code_ref_for(node.token_function_name),
            decl->signature_description(),
            owner != nullptr ? owner->namespaced_name() : std::string("<unknown type>"));

        return;
    }

    // and the file and module axes, which are about where the two declarations were written and not about
    // types at all - so the rule is AST::visible_from and the wording comes off it
    const DeclarationOrigin from = current_origin();

    // the answer first and the sentence only if it is no - see the split's comment in ASTVisibility.h.
    // `signature_description()` walks the namespace chain and renders every parameter type, and this runs
    // at every call to every declaration that did not say `public`
    if (visible_from(decl->visibility, decl->declared_in, from)) {
        return;
    }

    _collector.collect_issue<Issue::InaccessibleDeclaration>(
        code_ref_for(node.token_function_name),
        visibility_refusal(decl->visibility, decl->declared_in, from, decl->signature_description()),
        decl->declared_in,
        std::optional<TokenReference>(decl->declaration_site_token()));
}

void TypeChecker::check_has_implementation(FunctionDeclNode &node)
{
    if (node.body != nullptr || !declaration_owes_a_body(&node)) {
        return;
    }

    // an anonymous declaration has no token to report against, and a closure that reached here with
    // no body is a compiler bug rather than a source omission - ExprCodegen says so already
    if (node.is_anonymous()) {
        return;
    }

    // a constructor or a destructor is not this check's to report: the struct parser reports one at
    // the tail of its body pass, where it knows which members that walk actually reached. Two
    // diagnostics for one declaration reads worse than the narrower owner does
    if (node.is_constructor() || node.is_destructor()) {
        return;
    }

    // an instance of a bodyless template inherits the omission rather than committing it - the clone
    // has nothing to copy, and the template it came from is reported above. one per call site
    // otherwise, all of them pointing at the one declaration nobody wrote a body for
    if (node.is_instantiated()) {
        return;
    }

    _collector.collect_issue<Issue::GenericError>(
        code_ref_for(node.declaration_site_token()),
        fmt::format("'{}' was declared but never given a body - write one, or say where its "
                    "implementation comes from with '#[intrinsic: ...]', '#[builtin: ...]' or an "
                    "'extern' block.",
            node.signature_description()));
}

void TypeChecker::visitReturn(ReturnNode &node)
{
    // a return at file scope has no signature to answer to, and a synthesized return (the one
    // the struct parser builds for a constructor) has no token to report against
    if (_current_function != nullptr && node.expr != nullptr && node.token_return.has_value()) {
        const ValueType declared = _current_function->get_return_type();
        const ValueType actual = node.expr->result_type();

        check_destination_fits(Destination::t_return, declared, *node.expr, node.token_return.value());

        // and the same for a returned null, which check_destination_fits also waves through
        if (is_written_null(node.expr)) {
            if (const char *reason = null_rejection_reason(declared)) {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(node.token_return.value()),
                    fmt::format("cannot return null as '{}' - {}", declared.get_type_desciption(), reason));
            }
        }

        // the storage a local names is gone before the caller can read it, so handing back its
        // address is always wrong (book/concept/pointers_and_refs_v2.md, "Lifetimes")
        // a parameter is the caller's storage and outlives the call, so it is the legal case
        if (declared.is_pointer() && actual.is_pointer()) {
            VarDeclNode *root = place_root_of(node.expr);
            if (root != nullptr) {
                bool is_parameter = false;
                for (auto *arg : _current_function->args) {
                    if (arg == root) {
                        is_parameter = true;
                        break;
                    }
                }

                if (!is_parameter) {
                    _collector.collect_issue<Issue::GenericError>(
                        code_ref_for(node.token_return.value()),
                        fmt::format("cannot return the address of local '{}' - its storage ends with the call",
                            root->name_full()));
                }
            }
        }
    }

    RecursiveVisitor::visitReturn(node);
}

void TypeChecker::visit_type_decl(TypeDeclNode &node)
{
    // **ahead of the generic early-return below, deliberately.** a conformance is checked on the
    // *template*: `struct Bag<E> : contract::iterable<E>`'s requirements mention the very `E` that Bag's own
    // methods mention, and both sides are the same TypeParamDecl *, so the exact comparison works
    // without any instantiation - and the check is then done once rather than per instance. checking it
    // behind the return would mean a generic implementor was never checked at all, since an
    // instantiation has no TypeDeclNode for this visitor to reach
    check_conformances(node);

    // a generic struct template's property types legitimately mention its type parameters (the T
    // in `struct Box<T> { T $value; }`); it is only meaningful once instantiated with concrete
    // types. concrete/non-generic struct declarations are still checked
    if (node.is_generic()) {
        return;
    }
    RecursiveVisitor::visit_type_decl(node);
}

void TypeChecker::check_conformances(TypeDeclNode &node)
{
    const AST::ComplexType &ct = node.complex_type();

    for (const ValueType &interface : ct.conformances()) {
        // **the binding is reported first, and that ordering matters.** a failed solve leaves the
        // associated type unsubstituted, so first_unmet_requirement's wanted_signature would render
        // `iterate() : Iter` - naming a type the implementor's author never wrote and cannot act on,
        // which is exactly what that signature was made substituted to avoid
        const AST::ConformanceBinding binding =
            AST::conformance_bindings(&ct, interface, _collector.type_registry);

        if (binding.failure != AST::ConformanceBinding::Failure::t_none) {
            std::string detail;

            if (binding.failure == AST::ConformanceBinding::Failure::t_ambiguous) {
                detail = fmt::format(
                    "two of its members disagree about what '{}' is - one says '{}', another '{}'",
                    binding.associated->name,
                    binding.first.get_type_desciption(),
                    binding.second.get_type_desciption());
            }
            else {
                detail = fmt::format(
                    "'{}' would be '{}', which does not satisfy '{}'",
                    binding.associated->name,
                    binding.first.get_type_desciption(),
                    binding.constraint_spelling);
            }

            _collector.collect_issue<Issue::UnmetInterfaceRequirement>(
                code_ref_for(node.declaration_site_token()),
                fmt::format("'{}' says it conforms to '{}', but {}.",
                    node.type_name(), interface.get_type_desciption(), detail));

            continue;
        }

        auto unmet = AST::first_unmet_requirement(
            &ct, interface, _collector.type_registry, &_collector.functions);

        if (!unmet.has_value()) {
            continue;
        }

        // located on the *implementor's* name, not on the requirement: the type is what has to change,
        // and the interface may well be in another file the author does not own
        //
        // an operator is worded apart from a method because it is not a member of anything: what is
        // missing is a *file scope* declaration over this type, not something the type failed to declare
        std::string detail;

        if (unmet->requirement->is_operator()) {
            detail = fmt::format(
                "no '{}' over '{}' is declared at file scope",
                unmet->requirement->func_name(), node.type_name());
        }
        else if (unmet->found_signature.empty()) {
            detail = fmt::format("it declares no '{}'", unmet->requirement->func_name());
        }
        else {
            detail = fmt::format("the closest it declares is '{}'", unmet->found_signature);
        }

        _collector.collect_issue<Issue::UnmetInterfaceRequirement>(
            code_ref_for(node.declaration_site_token()),
            fmt::format(
                "'{}' says it conforms to '{}' but does not satisfy '{}' - {}.",
                node.type_name(),
                interface.get_type_desciption(),
                unmet->wanted_signature,
                detail));
    }
}

void TypeChecker::visitMemberAccess(MemberAccessNode &node)
{
    // the node answers this itself now. it used to be a second copy of the switch in
    // MemberAccessNode::result_type(), and the two drifted exactly as such pairs do: neither knew
    // an index base, so a typo'd member behind `$items:$[0]->` went unreported
    ValueType base_type = node.base_target_type();

    // does the name denote a property of the **tagged optional itself** - `__has` or `__value` - rather
    // than one the reader hoped was reachable through the absence? read by the nullable arm below.
    //
    // a *flag* nullable is deliberately not this: a `Node?` is one address, and the properties reached
    // through it are the payload's own, so `$maybe->tag` must still be refused
    bool names_own_property = false;

    if (base_type.has_complex_type()) {
        ComplexType *complex = base_type.get_complex_type();
        if (complex != nullptr) {
            // one lookup for both questions - whether the name denotes a property at all, and
            // whether the one it denotes is reachable from here
            const std::string member = node.get_member_name().value();
            const ComplexType::Property *property = complex->find_property(member);

            if (property != nullptr) {
                names_own_property = base_type.is_wrapped_optional();
                check_private_member(node, *complex, *property);
            }
            // **an unknown member of a tagged optional is not an unknown member.** the two it has are the
            // compiler's own, so anything else the author named is a member of the *payload* - and what
            // they need to hear is that the value may not be there, which the nullable arm below says.
            // reporting both would name the member as the mistake when the absence is
            else if (!base_type.is_wrapped_optional()) {
                _collector.collect_issue<Issue::UnknownMember>(
                    code_ref_for(node.get_member_name()),
                    member,
                    complex->name.value_or("<anonymous>"));
            }
        }
    }

    // **and a base that has no properties at all**, which is a different question from the unknown
    // member above, and one that was answered nowhere. `$p->x` over a `ptr<int32>` reached codegen and
    // died on gen_member_lvalue's contextless "Cannot access member 'x' of 'int32'". A borrow-returning
    // call is one more spelling of the same shape, so it is worth a location either way.
    //
    // is_undetermined_type is what makes this safe to ask here. It is the one spelling of "no
    // information", so a type parameter or an unresolved call passes through rather than earning a
    // second diagnostic - such a call's result_type() is void, on top of the UnknownFunction already
    // reported.
    //
    // An interface is excluded because the check above already covers it: it has a complex type and no
    // properties, so a `->x` on one is already an UnknownMember
    // **one chain, so a base earns exactly one message.** the three arms below are three different
    // things that can be wrong with `E->x`, ordered by how specific the advice is.
    //
    // They were an `if` and a separate `if`/`else if` until a weak base collected two of them at once,
    // which reads as two problems where there is one.
    //
    // A weak base comes first, because "has no members" is true of it but unhelpful. The object it
    // names does have the member. What is missing is the upgrade that proves the object is still there
    if (base_type.is_weak()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.get_member_name()),
            fmt::format(
                "'{}' cannot be read through - it does not keep its object alive, so the object may "
                "already be gone. upgrade it first with 'strong(...)', or reach through it with '?->'",
                base_type.get_type_desciption()));
    }
    // **`->` through something that may not be there.** the member exists; what is not guaranteed is the
    // value holding it, so the message names the three ways through rather than claiming the member is
    // wrong. this is the check that makes `Foo?` worth having: without it a nullable would read exactly
    // like a `Foo` right up until the one execution where it was absent
    //
    // **a pointer is excluded**, and that is deliberate rather than an oversight. `ptr<T>` carries this
    // same flag, but `->` through one is the language's established auto-deref and
    // book/concept/pointers_and_refs_v2.md documents null-checking the *address* instead. changing that
    // is a separate decision about pointers, not part of introducing `T?`
    //
    // **a property the optional itself declares is not a reach *through* it.** a tagged `T?` is a layout
    // with two of them, `__has` and `__value`, and the teardown, the copy and a guard's payload read that
    // AST::OwnershipPass writes all reach `__value` as the field it is. What this rule protects is the
    // *payload's* members, and those are not properties of the optional - `$maybe->x` still lands here,
    // because the optional has no `x`.
    //
    // it does mean a program that names `__value` itself is read as the compiler's own access and admitted -
    // the two are not `private`, and cannot be until a guard's payload copy is minted inside a body the
    // type owns. see AST::TypeRegistry::get_or_create_optional, where trying it is written down
    else if (base_type.is_nullable() && !base_type.is_pointer() && !names_own_property) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.get_member_name()),
            fmt::format(
                "'{}' may not be there, so '->' cannot reach through it - use '?->' to skip when it is "
                "absent, '??' to supply a replacement, or 'guard' to bind it once and read it plainly",
                base_type.get_type_desciption()));
    }
    else if (!is_undetermined_type(base_type) && !base_type.has_property_layout()
        && !base_type.is_interface()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.get_member_name()),
            fmt::format(
                "'{}' has no members - only a struct or a class has properties to reach with '->'",
                base_type.get_type_desciption()));
    }

    // **a base with no storage is no longer this pass's question**. it used to be reported
    // here - "has no storage to read a member from" - because there was no answer to give: a member is
    // reached from an address, and a value nobody stored has none. both halves have one now, and both
    // live where the answer is rather than where the shape is visible:
    //
    //  - a *borrow*-returning call already **is** the address, so it is simply lowered (A13a, the arm in
    //    LValueCodegen::gen_place)
    //  - a *value*-returning call is given storage by AST::OwnershipPass, which is the only pass that can
    //    both create the temporary and say who destroys it (A13b) - and the three positions that cannot
    //    be made safe, an address, a borrow and a write, are refused *there*, each naming what would
    //    have gone wrong. keeping a vaguer copy here reported every one of them twice
    RecursiveVisitor::visitMemberAccess(node);
}

void TypeChecker::visit_instanceof_expr(InstanceOfExprNode &node)
{
    const ValueType operand_type = node.operand->result_type();

    // "structs do not have any runtime meta data ... you can also not perform any runtime reflection
    // checks on them" (CONCEPT.md). the question is not merely false for a struct, it is unanswerable:
    // there is no block and no identity word, so the value never carried an answer. a *class* operand
    // against a struct type is a different matter and folds to false, which is why only the left side
    // is checked here
    // an **interface** operand carries one too: it holds a class handle, and that block's typeinfo word is
    // the same answer a class-typed operand's is. so an erased value can be asked both which interface it
    // answers and which concrete class is inside it - the downcast test. a struct is still the refusal,
    // and still for CONCEPT.md's reason
    if (!operand_type.is_class() && !operand_type.is_interface()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.token_instanceof),
            fmt::format(
                "'instanceof' needs a class or an interface on the left - a '{}' carries no runtime type "
                "to check",
                operand_type.get_type_desciption()));
    }

    // a declared type of any kind. a class is the exact-identity question, an interface the conformance
    // one, and a struct folds to false - has_complex_type() is already the predicate for "a declared
    // type", so admitting interfaces needed nothing but this wording
    if (!node.queried_type.has_complex_type()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.token_instanceof),
            fmt::format(
                "'instanceof' needs a struct, a class or an interface on the right, not '{}'",
                node.queried_type.get_type_desciption()));
    }

    RecursiveVisitor::visit_instanceof_expr(node);
}

void TypeChecker::visit_strong_expr(StrongExprNode &node)
{
    const ValueType operand_type = node.operand->result_type();

    // reported here rather than in the parser for the reason every type question in this compiler is: at
    // parse time the operand may be a bare type parameter, an unsettled call, or an element access whose
    // contract a later monomorphizer round attaches. by the time this pass runs every type is answered
    //
    // is_undetermined_type still passes through, because a call that never resolved has an
    // UnknownFunction already and does not need a second diagnostic on top of it
    if (!operand_type.is_weak() && !is_undetermined_type(operand_type)) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.token),
            fmt::format(
                "'strong' needs a weak reference, not '{}' - there is nothing to upgrade, and a value "
                "that owns its object is already as strong as it gets",
                operand_type.get_type_desciption()));
    }

    RecursiveVisitor::visit_strong_expr(node);
}

// **the three nullability forms, asked a second time.** both halves of what makes them sound were
// decided in the parser and nowhere else, and inside a template the operand is a bare `T` that
// AST::is_certainly_present correctly answers "later" for - so a form over a `T` that substituted to a
// non-nullable was never checked at all, and a guard that could never fail compiled silently (B27).
//
// the parser keeps its check: it fires for non-generic code, where the diagnostic is best located and
// where waiting for this pass would be a worse message. what is shared is the *wording*, through
// AST::certainly_present_refusal - two askers per form is exactly how three strings become six
void TypeChecker::check_optional_operand(
    OptionalForm form,
    const ExprNode *operand,
    const TokenReference &at
)
{
    if (operand == nullptr) {
        return;
    }

    // is_undetermined_type passes through inside the refusal itself, for the reason it does everywhere
    // else in this file: a call that never resolved already has its own issue
    const std::string refusal = certainly_present_refusal(form, operand->result_type());

    if (!refusal.empty()) {
        _collector.collect_issue<Issue::GenericError>(code_ref_for(at), refusal);
    }
}

void TypeChecker::visit_guard(GuardNode &node)
{
    // the initializer is the tested value, and it is the declaration's own - a guard has no separate
    // condition edge
    if (node.decl != nullptr) {
        check_optional_operand(OptionalForm::t_guard, node.decl->init_expr, node.token);
    }

    RecursiveVisitor::visit_guard(node);
}

void TypeChecker::visit_null_coalesce(NullCoalesceExprNode &node)
{
    check_optional_operand(OptionalForm::t_null_coalesce, node.lhs, node.token);

    RecursiveVisitor::visit_null_coalesce(node);
}

void TypeChecker::visit_optional_chain(OptionalChainExprNode &node)
{
    check_optional_operand(OptionalForm::t_optional_chain, node.base, node.token);

    RecursiveVisitor::visit_optional_chain(node);
}

// **an address of something that has no address.**
//
// By the time this pass runs, every legitimate borrow of a value with no storage has been reseated
// onto a temporary's varref by AST::OwnershipPass. So an operand that is still neither a place nor
// pointer-typed means two gates disagreed: AST::argument_fit ranked t_borrow_temporary where the
// ownership pass declined to mint a slot.
//
// This is a *guard rail*, not a user diagnostic. Nothing a program can be written to say reaches it.
// It exists because the alternative failure is the worst kind this compiler has: ExprCodegen::
// gen_addr_of hands the operand to gen_lvalue, which throws "Expression is not addressable" with no
// source location at all, far from whichever rule was wrong. One visitor arm turns every future
// divergence between those two gates into a located error.
//
// is_undetermined_type passes through for the reason it does everywhere else: an unsettled call
// already has its own issue and does not need a second one stacked on top.
//
// **and so does a program that has already failed.** AST::OwnershipPass *refuses* some requests
// rather than binding them - a write through a temporary's element is the shape that reaches here -
// and a refusal deliberately leaves the tree exactly as it was written, so the unbacked `&` survives
// to this arm.
//
// The two gates did not disagree there. One of them declined, out loud, with a located reason the
// author can act on. Stacking "this is a compiler bug" on top of that would bury the real message,
// and run_semantic_passes fails the compile on the first one either way.
//
// The cost is that the rail is blind in a program that is already broken - which is the one program
// where it was never the interesting diagnostic
void TypeChecker::visit_addr_of_expr(AddrOfExprNode &node)
{
    // **the same predicate AST::OwnershipPass mints slots from, not a second spelling of it.** a guard
    // rail that re-derives the rule it polices stops policing it the moment either copy grows an arm,
    // which is precisely the divergence this arm exists to catch
    if (borrow_operand_needs_storage(*node.operand) && !_collector.has_critical_issues()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(location_of_expression(node.operand)),
            fmt::format(
                "'{}' has no storage to take the address of, and nothing gave it any. This is a compiler "
                "bug - AST::argument_fit and AST::OwnershipPass disagree about this operand",
                node.operand->result_type().get_type_desciption()));
    }

    // **every implicit promotion arrives here**, which is why there are two call sites and not six.
    // the explicit `T&($p:$)` is a cast; `&$p:$[0]`, the borrow a call argument gets, a receiver's
    // auto-borrow and a `return &...` are all this node, minted by the parser or by CallResolver
    //
    // `mem::init` and `mem::take` are exempt, and it is not a convenience: they are the two seams that
    // deliberately name storage nothing is accounting for, and a promotion promises the storage holds
    // a valid `T` - which for the slot `mem::init` is about to fill is *false at the moment it would
    // be made*. AST::is_unaccounted_storage is the same boundary said from the other side
    //
    // asked of AST::builtin_owns_raw_storage and not of `is_builtin()`: `dprint`, `mem::ref_count` and
    // `mem::weak_count` are builtins taking an ordinary `T&`, and exempting those let `dprint($p:$[0])`
    // mint a trusted borrow out of a raw address with nothing asked of the author
    const bool callee_owns_raw_storage = _context_callee != nullptr
        && _context_callee->is_builtin()
        && builtin_owns_raw_storage(builtin_kind_for(_context_callee->builtin.value()));

    if (!callee_owns_raw_storage) {
        // **the call's token when there is one.** a *synthesized* borrow - a receiver's auto-borrow,
        // the one CallResolver wraps an argument in - has no token of its own, and its operand's
        // varref carries the *declaration's*, so a diagnostic pointed there lands on a line that has
        // nothing to do with the promotion. `$p->bump()` reported at `ptr<Counter> $p = &$c;`
        const TokenReference &at =
            _context_token != nullptr ? *_context_token : location_of_expression(node.operand);

        check_unsafe_promotion(node.result_type(), node.operand, at);
    }

    RecursiveVisitor::visit_addr_of_expr(node);
}

// `ref_count<T>(T& $handle)` infers T from anything, so overload resolution admits `ref_count($aStruct)`
// and nothing below it says no. reported here for the reason check_abort_message's rule is: the only
// other reader is ExprCodegen, which throws an *internal compiler error* with no source location, and
// that is not the user's mistake to read
//
// `weak_count` has the same signature and the same hole, so it is checked here rather than beside itself:
// the two read two words of one header and there is one thing wrong you can do to either
void TypeChecker::check_ref_count_argument(FunctionCallExprNode &node)
{
    if (!node.decl->is_builtin()) {
        return;
    }

    const BuiltinKind kind = builtin_kind_for(node.decl->builtin.value());

    if (kind != BuiltinKind::t_ref_count && kind != BuiltinKind::t_weak_count) {
        return;
    }

    const char *name = kind == BuiltinKind::t_weak_count ? "weak_count" : "ref_count";

    if (node.arguments.empty() || node.arguments[0] == nullptr) {
        return;
    }

    // the parameter is `T&`, so the argument arrives as the address of the slot holding the handle -
    // one load short of the handle itself, which is what codegen reads through. that indirection is the
    // point: taking the handle by value would retain it and answer one too high every time
    const ValueType argument_type = node.arguments[0]->result_type();
    const ValueType handle_type = value_type_of(argument_type);

    // an argument still generic is not this pass's to judge - the monomorphizer reports an
    // uninstantiated call itself, and a bare type parameter here means nothing was decided yet
    if (is_undetermined_type(handle_type) || handle_type.is_type_param()) {
        return;
    }

    if (!handle_type.is_class()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.token_function_name),
            fmt::format("'{}' needs a class handle, not '{}' - only a class carries a "
                "reference count", name, handle_type.get_type_desciption()));
    }
}

// `dprint<T>(T& $value)` renders whatever it is handed, so there is exactly one thing wrong you can do to
// it - and it is reachable. `argument_fit` treats void as t_undetermined, which is *neutral* rather than a
// mismatch, so `dprint(some_void_call())` resolves with T = void and reaches codegen, where the failure is
// an internal compiler error with no source location. that is check_ref_count_argument's reason, verbatim
//
// nothing else is refused. a still-generic T returns early exactly as its neighbour above does, because
// the monomorphizer already reports an uninstantiated call and a second diagnostic on top of it is noise;
// and every other type has a rendering, which is the whole point of the builtin
void TypeChecker::check_dprint_argument(FunctionCallExprNode &node)
{
    if (!node.decl->is_builtin() || builtin_kind_for(node.decl->builtin.value()) != BuiltinKind::t_dprint) {
        return;
    }

    if (node.arguments.empty() || node.arguments[0] == nullptr) {
        return;
    }

    // the parameter is `T&`, so what arrives is the address of the slot - one level out from the value
    // being printed, exactly as ref_count's argument is
    const ValueType printed_type = value_type_of(node.arguments[0]->result_type());

    if (printed_type.is_void()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.token_function_name),
            "'dprint' has nothing to print - this expression produces no value");
    }
}

// `mem::take<T>(T& $place)` hands the value at a place over and writes nothing back, so the storage it
// read from is no longer an owner - and *nothing in the compiler knows that*. That is deliberate. The
// only thing that can say so is whatever manages the storage, which for `array<T>` is the array's `len`.
//
// So the rule is about **which storage the compiler is already accounting for**, and it is narrow on
// purpose. A local gets a scope-exit drop, a temporary gets one from the frame it was bound to, and a
// property gets one from its owner's teardown. `take` tells none of them otherwise, so emptying any of
// them destroys the value twice, silently and far from here.
//
// What is left is storage reached *through a pointer* - `$p:$[$i]`, `$p:$` - which is exactly the
// storage nothing walks, because a pointer is not an owner. That is the same line `mem::free` sits on,
// which is why both live in `mem::`.
//
// Reported at this pass rather than in codegen for check_ref_count_argument's reason: the call has a
// token to point at, where ExprCodegen's failure is an internal compiler error naming only the
// enclosing function
// `mem::init<T>(T& $place, T $value)` is the mirror and is judged by the same rule, so the two share one
// check rather than one predicate spelled twice. Only the sentence differs, because the two ways of
// getting it wrong are opposites: taking from accounted storage destroys the value twice, initializing it
// leaks what was already there
void TypeChecker::check_raw_storage_argument(FunctionCallExprNode &node)
{
    if (!node.decl->is_builtin()) {
        return;
    }

    const BuiltinKind kind = builtin_kind_for(node.decl->builtin.value());

    if (!builtin_owns_raw_storage(kind)) {
        return;
    }

    if (node.arguments.empty() || node.arguments[0] == nullptr) {
        return;
    }

    // the parameter is `T&`, so AST::CallResolver wrapped the place in the `&` this looks through - one
    // level out from the value itself, exactly as ref_count's and dprint's arguments are
    auto *address = node.arguments[0];

    if (address->get_node_type() != NodeType::n_expr_addrof) {
        return;
    }

    if (AST::is_unaccounted_storage(*static_cast<AddrOfExprNode *>(address)->operand)) {
        return;
    }

    // a still-generic body is not this pass's to judge: the monomorphizer reports an uninstantiated call
    // itself, and an unsettled operand here means nothing was decided yet. the same early out
    // check_ref_count_argument takes, for the same reason
    const ValueType place = value_type_of(node.arguments[0]->result_type());

    if (is_undetermined_type(place) || place.is_type_param()) {
        return;
    }

    if (kind == BuiltinKind::t_take) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.token_function_name),
            "'mem::take' can only empty storage reached through a pointer, such as an element of a buffer "
            "you allocated. This source is a variable, a property or a temporary, and the scope or the "
            "value holding it already owes it a teardown - so taking it here would destroy it twice. "
            "Write 'mv' to hand a variable over.");

        return;
    }

    _collector.collect_issue<Issue::GenericError>(
        code_ref_for(node.token_function_name),
        "'mem::init' can only fill storage reached through a pointer, such as an element of a buffer you "
        "allocated. This destination is a variable, a property or a temporary, and something already owes "
        "whatever it holds a teardown - so initializing it here would leak that value. Write an ordinary "
        "'=', which ends the old value before storing the new one.");
}

// **the only builtin that can be unavailable**, and the only reason this pass reads the compiler options
// at all. `mem::live_allocations()` reads a counter the allocation seam maintains, and the seam only
// maintains one when --track-allocations asked it to - so without the flag the load would answer 0.
//
// which is the one wrong answer that cannot be told apart from the right one: a person adds
// `assert(mem::live_allocations() == 0)` to prove a program is balanced, and gets a passing assertion that
// proves nothing. A refusal here is not pedantry, it is the difference between a leak check and a
// decoration
//
// here rather than in ExprCodegen, where the builtin is lowered, because codegen's only failure is
// InternalCompilerException: it names the enclosing function and no line. This pass has the call's own
// token, so the message can point at the call and name the flag that fixes it
void TypeChecker::check_allocation_tracking(FunctionCallExprNode &node)
{
    if (!node.decl->is_builtin()
        || builtin_kind_for(node.decl->builtin.value()) != BuiltinKind::t_live_allocations) {
        return;
    }

    if (_options.tracking_allocations()) {
        return;
    }

    _collector.collect_issue<Issue::GenericError>(
        code_ref_for(node.token_function_name),
        "'live_allocations' has nothing to read without allocation tracking - compile with "
        "'--track-allocations' (or '--explain-memory', which implies it)");
}

void TypeChecker::check_abort_message(FunctionCallExprNode &node)
{
    if (!node.decl->is_builtin()) {
        return;
    }

    // which argument the message is comes from AST::builtin_message_index, shared with the
    // ExprCodegen site that folds it - spelled here as well, the two could check one argument and
    // fold another, and a message that is not a literal folds to *nothing* rather than to an error
    const auto index = builtin_message_index(builtin_kind_for(node.decl->builtin.value()));

    if (!index.has_value() || node.arguments.size() <= *index) {
        return;
    }

    ExprNode *message = node.arguments[*index];

    // an argument whose *type* is already wrong has been reported - directly by the per-argument
    // walk above, or by visitTypeCast when the resolver wrapped it to make it fit. saying it is
    // also not a literal is two diagnostics for one mistake, and the shape is the less useful one.
    //
    // asked of the expression the user *wrote*, through the same strip is_written_null uses: a
    // legal cast around a perfectly good literal must not read as "not a literal"
    const ExprNode *written = strip_implicit_casts(message);
    if (written == nullptr) {
        return;
    }

    if (*index < node.decl->args.size() && node.decl->args[*index]->has_type()
        && !arg_assignable_to(written->result_type(), message, node.decl->args[*index]->type())) {
        return;
    }

    // the message is folded into a constant at the call site, together with the source location -
    // that is what makes these builtins rather than library functions, and it is why the text has
    // to be readable at compile time. lifts when there is a `string` type to hand one at runtime
    if (!literal_string_value(message).has_value()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.token_function_name),
            fmt::format("the message of '{}' must be a string literal - it is folded into the "
                "binary along with the source location, so it has to be known at compile time",
                node.decl->func_name()));
    }
}

// a `const` value may only be handed to a method that promised to read it. the promise is the
// receiver's own type - `const Foo&` at args[0] - so the *fit* already refuses this, and what is
// left is which of the two diagnostics the reader gets: the conversion's, which names a pointee they
// never wrote, or this one, which names the method and the modifier that would fix it
//
// AST::CallResolver declines to wrap such a receiver in a cast for exactly this reason, so nothing
// else has already spoken by the time this runs
bool TypeChecker::check_receiver_const(FunctionCallExprNode &node)
{
    // the node's own halves only - what makes a *declaration* refusable is const_receiver_refusal's,
    // and re-derived here it would be a third site that knows what a method is
    if (node.decl == nullptr || node.arguments.empty() || node.arguments[0] == nullptr) {
        return false;
    }

    const std::string refusal =
        const_receiver_refusal(*node.decl, node.arguments[0]->result_type());

    if (refusal.empty()) {
        return false;
    }

    _collector.collect_issue<Issue::ConstViolation>(
        code_ref_for(node.token_function_name), refusal);

    return true;
}

void TypeChecker::check_call_argument(
    ExprNode *argument,
    const ValueType &param_type,
    size_t arg_number,
    const FunctionDeclNode *callee,
    const std::string &indirect_name,
    const TokenReference &at
)
{
    const bool callee_is_operator = callee != nullptr && callee->is_operator();

    // **built where a refusal is written and not before.** an operator's spelling is recovered from its
    // decorated name, so asking every argument of every operator call in the program for it allocates
    // twice over for a string that only a diagnostic reads
    const auto callee_name = [&]() -> std::string {
        if (callee == nullptr) {
            return indirect_name;
        }

        return callee_is_operator ? callee->operator_spelling() : callee->func_name();
    };

    // the declaration site already refuses to seed a non-nullable parameter with null - the call site
    // has to refuse too, or the promise only holds for locals. this was a segfault the moment the
    // callee read through it
    if (is_written_null(argument)) {
        if (const char *reason = null_rejection_reason(param_type)) {
            // an operator says it in its own words, for visitTypeCast's reason: every operator shares the
            // root namespace, so naming the losing candidate's parameter type here tells the author about
            // a type no file of theirs mentions. `$p == null` on a struct would otherwise be answered with
            // a sentence about 'const string&'
            //
            // silent while the operand rule has already answered: visitFunctionCallExpr wrote the
            // sentence that says what to do ("it is always there, write 'P?'"), and this one would be
            // a second, vaguer report of the same mistake
            if (callee_is_operator) {
                if (!_context_operands_refused) {
                    _collector.collect_issue<Issue::GenericError>(
                        code_ref_for(at), null_operand_refusal(callee_name()));
                }
            }
            else {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(at),
                    fmt::format("argument {} of '{}' is '{}', which cannot be null - {}",
                        arg_number, callee_name(), param_type.get_type_desciption(), reason));
            }
        }
        return;
    }

    // a mismatched argument that the parser/monomorphizer could not reconcile with an implicit cast is
    // caught here directly (e.g. two distinct struct types). one that *was* wrapped in an implicit cast
    // is validated in visitTypeCast instead, where the illegal conversion actually lives
    //
    // the argument as written is passed, so this scores it exactly as the matcher did
    const ValueType arg_type = argument->result_type();

    // an interface parameter is an arrival site like any other, so the storable question is asked here
    // too - and ahead of the fit below for the same reason: a struct argument *conforms*, and the vaguer
    // "does not accept" would say nothing about why storing it is what fails
    if (check_interface_erasure(param_type, *argument, at)) {
        return;
    }

    if (!arg_assignable_to(arg_type, argument, param_type)) {
        _collector.collect_issue<Issue::ArgumentTypeMismatch>(
            code_ref_for(at),
            fmt::format(
                "Argument {} of '{}' expects type '{}' but got '{}'",
                arg_number,
                callee_name(),
                param_type.get_type_desciption(),
                arg_type.get_type_desciption()));
    }
}

void TypeChecker::visitFunctionCallExpr(FunctionCallExprNode &node)
{
    // **what is wrong with an operator's operands, worked out once for the whole call.** the two
    // readers below - the null refusal in check_call_argument and the conversion refusal in
    // visitTypeCast - each see one argument, and both of these rules are about the pair. computed
    // here and only *read* where a refusal is already being written, so a call that resolves cleanly
    // can never be refused by it: a user-declared `operator (ptr<int32> $a) == (int32 $b)` is a
    // perfectly good declaration that the address rule would otherwise reject at every use
    const bool prev_operands_refused = _context_operands_refused;
    _context_operands_refused = false;

    if (node.decl != nullptr && node.decl->is_operator() && node.arguments.size() == 2
        && node.arguments[0] != nullptr && node.arguments[1] != nullptr) {
        // **the operands as the author wrote them**, under the casts the resolver put there to reach
        // the candidate it chose. without the strip the message names the *parameter's* type, which is
        // the whole thing this is here to stop saying
        const auto refusal = binary_operand_refusal(
            _collector.operators.get_operator(node.decl->operator_spelling()),
            adjusted_operand(strip_implicit_casts(node.arguments[0])),
            adjusted_operand(strip_implicit_casts(node.arguments[1])));

        // reported **here**, once, rather than by the two readers below: they see one argument each, so
        // a refusal about the pair would be written twice - once by the null rule and once by the
        // conversion. they stay silent while the flag is set, and their own wordings are the fallback
        if (refusal.has_value()) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(node.token_function_name), *refusal);

            _context_operands_refused = true;
        }
    }

    // **outside the generic gate below**, unlike its two neighbours, because the one thing it catches is
    // precisely a call the monomorphizer could not instantiate: `dprint(some_void_call())` binds T to
    // void, which is a type there is no slot to allocate, so `decl` is still the template when this pass
    // runs. inside the gate it would never fire and the failure would stay a location-less codegen throw
    if (node.decl != nullptr) {
        check_dprint_argument(node);

        // outside the gate too, and for a plainer reason than its neighbour's: the question is whether
        // the *builtin* may be called at all, which does not depend on a single argument being resolved
        check_allocation_tracking(node);

        // and outside it for the plainest reason of the three: who may call a declaration is a property of
        // the declaration, so it is the same answer for a template and for every instance of it - the
        // monomorphizer having copied the flag along with everything else
        check_call_visibility(node);
    }

    // generic templates are resolved to concrete instances by the monomorphizer; only a
    // resolved, non-generic callee has stable parameter types to check against
    if (node.decl && !node.decl->is_generic()) {
        const auto &params = node.decl->args;

        // the receiver's const-ness first, and taking argument 0 out of the loop when it reported -
        // the generic "expects type 'Box&' but got 'const Box&'" is true and says nothing about the
        // one edit that fixes it
        const bool receiver_reported = check_receiver_const(node);

        // every argument is checked, the receiver included - `$p->m()` on a null pointer is exactly
        // the case the borrow guard below exists for - but the *number* a diagnostic reports is the
        // one the reader can count to, which is what user_arg_number answers
        if (node.arguments.size() == params.size()) {
            for (size_t i = 0; i < params.size(); i++) {
                if (!node.arguments[i] || !params[i]->has_type()) {
                    continue;
                }

                if (i == 0 && receiver_reported) {
                    continue;
                }

                check_call_argument(
                    node.arguments[i],
                    params[i]->type(),
                    node.decl->user_arg_number(i),
                    node.decl,
                    std::string(),
                    node.token_function_name);
            }
        }

        check_abort_message(node);
        check_ref_count_argument(node);
        check_raw_storage_argument(node);
    }

    // echo is a decl-less builtin, and its codegen has a printf conversion for every primitive and
    // nothing else. reported here so each gap is a located diagnostic instead of the uncaught codegen
    // throw it used to be. AST::is_print_call owns the recognition, including the "has a declaration,
    // so the ordinary argument checks above apply instead" half of it
    //
    // two shapes are worth naming. an *address*, because after the adjustment pass a pointer here
    // really is an address rather than a not-yet-dereferenced read, so printing one is almost always
    // a missing read. and a *named type*, struct or class, for which there is no rendering to pick at
    // all, and giving them one is still open
    if (is_print_call(node)) {
        for (auto *arg : node.arguments) {
            if (arg == nullptr) {
                continue;
            }

            // **a tagged optional over a primitive prints its payload**, whether or not it is there - the
            // peel `echo` has always performed. asked of AST::echo_printed_type_of, which is also what
            // Compiler::LLVM::printf_conversion_for asks: this arm decides what is *accepted* and that
            // table what is *emitted*, so a second spelling of the condition is a program accepted here
            // and thrown at by the other half
            const ValueType type = echo_printed_type_of(arg->result_type());

            // the one complex type `echo` prints, so it is admitted ahead of the blanket refusal below.
            // ExprCodegen::gen_echo_string is the other half of this rule and the two have to agree, or
            // a program is either rejected for something that works or lowered by a path that throws
            if (_collector.core_types.is_string_like(type)) {
                continue;
            }

            if (type.is_pointer()) {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(node.token_function_name),
                    fmt::format("cannot echo an address of type '{}' - echo prints values",
                        type.get_type_desciption()));
            }
            else if (type.has_complex_type()) {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(node.token_function_name),
                    fmt::format("'echo' has no way to print a '{}' - print its members instead",
                        type.get_type_desciption()));
            }
            else if (type.is_callable()) {
                // reported here for the reason the two above are: ExprCodegen has no printf conversion
                // for it and throws an *internal compiler error*, which is not the user's mistake to read
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(node.token_function_name),
                    fmt::format("'echo' has no way to print a '{}' - call it and print the result",
                        type.get_type_desciption()));
            }
        }
    }

    // walk arguments with this call's name token as the location context, so an illegal implicit
    // cast inserted around an argument is reported at the call site
    const TokenReference *prev = _context_token;
    const FunctionDeclNode *prev_callee = _context_callee;

    _context_token = &node.token_function_name;

    // the callee travels with the token, so visitTypeCast can tell an operator's argument from an
    // ordinary one - see _context_callee. null is legitimate: an unresolved call is a normal
    // intermediate state, and there is nothing to name in the operator's words if nothing was chosen
    _context_callee = node.decl;

    RecursiveVisitor::visitFunctionCallExpr(node);

    _context_token = prev;
    _context_callee = prev_callee;
    _context_operands_refused = prev_operands_refused;
}

void TypeChecker::visit_indirect_call_expr(IndirectCallExprNode &node)
{
    const ValueType callee_type = node.callee_type();

    // the callee's *signature* is the parameter list here - there is no declaration to walk. the shape
    // ("this is not callable") and the arity are the parser's, reported where the call was written; what
    // is left is whether each argument reaches its parameter, which is the same question a direct call
    // asks and the same one answer
    if (callee_type.is_callable()) {
        const auto &signature = callee_type.signature();

        if (node.arguments.size() == signature.parameter_types.size()) {
            // the callee is named by its *type* - there is no declaration to take a name from. built
            // once: a callable's description recurses through its return and every parameter
            const std::string callee_name = callee_type.get_type_desciption();

            for (size_t i = 0; i < node.arguments.size(); i++) {
                if (node.arguments[i] == nullptr) {
                    continue;
                }

                // an indirect call has no implicit parameter, so the position a reader counts to is the
                // index
                check_call_argument(
                    node.arguments[i],
                    signature.parameter_types[i],
                    i + 1,
                    // no declaration: an indirect call is through a callable value, which is never an
                    // operator and has no name of its own beyond the one the call site wrote
                    nullptr,
                    callee_name,
                    node.token);
            }
        }
    }

    RecursiveVisitor::visit_indirect_call_expr(node);
}

void TypeChecker::visit_closure_expr(ClosureExprNode &node)
{
    // capture is by value, and a copy of an owning value is a whole taxonomy - a retain, a copy
    // constructor, or nothing that exists at all. the environment's teardown is uniform precisely
    // because it holds no owner: one `__eco_release_env` thunk and no deinit, so an owner admitted here
    // is a leak rather than a wrong destructor
    //
    // here rather than at the capture site in the parser, where the read is written: the captured
    // variable's type is not final until the monomorphizer has settled the call it was inferred from,
    // so `$b = Box<int32>(5)` was still a `Box<T>` when the parser saw it - and a bare type parameter
    // owns nothing, which is how an owning capture used to pass unnoticed
    if (node.environment_type != nullptr) {
        for (size_t i = 0; i < node.environment_type->property_count(); i++) {
            const ComplexType::Property &property = node.environment_type->get_property(i);

            if (!needs_destruction(property.type)) {
                continue;
            }

            // the property name *is* the variable's name - it is what the body's `$__env->name` read
            // resolves through - so the diagnostic can name the capture without a second list to keep
            // in step. located at the literal, which is where the copy would be made
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(node.token),
                fmt::format(
                    "'{}' is a '{}', which owns a resource. Capturing an owning value is not supported "
                    "yet - pass it as a parameter instead.",
                    property.name, property.type.get_type_desciption()));
        }
    }

    RecursiveVisitor::visit_closure_expr(node);
}

void TypeChecker::visitTypeCast(TypeCastNode &node)
{
    // the parser/monomorphizer inserts implicit casts to reconcile types; if such a cast is not a
    // legal conversion (e.g. a struct where a primitive is expected) it would otherwise surface as
    // a context-free "Unsupported type cast" deep in codegen. report it here, located
    if (node.is_implcit && node.expr && _context_token) {
        ValueType from = node.expr->result_type();

        // an interface target is asked the *storable* question first. an implicit cast to one is how a
        // widening reaches an argument position, so this is the arrival site for a call - and a
        // conforming struct would otherwise be reported as "cannot implicitly convert", which is true and
        // says nothing about the reason
        if (check_interface_erasure(node.cast_to, *node.expr, *_context_token)) {
            RecursiveVisitor::visitTypeCast(node);
            return;
        }

        if (!implicit_conversion_is_legal(from, node.cast_to)) {
            // **an operator says it in its own words.** every `operator` declaration in the program
            // shares the root namespace, so a program carries every operator's overload set whether it
            // uses the types or not. Naming the losing candidate's parameter type here would tell the
            // author about a type no file of theirs mentions - one `operator ==` for `string` in the
            // standard library otherwise degrades `==` for every struct in every program to "cannot
            // convert 'P' to 'const string&'".
            //
            // The operand types are what the author actually wrote, so they are what the message
            // carries. "no overload of it accepts" rather than "is not supported", because a
            // declaration does exist and saying otherwise would be a lie the author cannot act on.
            //
            // And silent where AST::binary_operand_refusal already answered about the *pair* of
            // operands. visitFunctionCallExpr reported that one, it carries the advice, and this would
            // be the same mistake said again more vaguely. This stays the wording for the refusals that
            // really are about one argument not fitting a parameter
            if (_context_callee != nullptr && _context_callee->is_operator()) {
                if (!_context_operands_refused) {
                    _collector.collect_issue<Issue::InvalidTypeConversion>(
                        code_ref_for(*_context_token),
                        fmt::format(
                            "no overload of operator '{}' accepts a '{}' here - declare one for it, or "
                            "convert the operand first.",
                            _context_callee->operator_spelling(), from.get_type_desciption()));
                }
            }
            else {
                _collector.collect_issue<Issue::InvalidTypeConversion>(
                    code_ref_for(*_context_token),
                    fmt::format("cannot implicitly convert '{}' to '{}'",
                        from.get_type_desciption(), node.cast_to.get_type_desciption()));
            }
        }
    }

    // an *implicit* cast never promotes: nothing the compiler inserts turns raw storage into a
    // trusted borrow on its own, and reporting against one would blame the author for a node they
    // did not write. the implicit borrow at an argument position is an AddrOf, and it is checked there
    if (!node.is_implcit && node.expr != nullptr
        && narrowing_promotes_raw_storage(node.expr->result_type(), node.cast_to)) {
        report_unsafe_promotion(node.cast_to, location_of_expression(node.expr));
    }

    RecursiveVisitor::visitTypeCast(node);
}

// **forming a trusted borrow out of raw storage is the operation `unsafe` marks.**
//
// Deliberately *not* pointer casting. `ptr<uint32>(&$f)` only computes another raw address: reads and
// writes through a `ptr<T>` carry no type tag, so the optimizer stays conservative around all of them
// and nothing has been promised.
//
// What licenses a promise is the step that turns a raw address into a `T&`. From there the type is the
// contract, every later access carries that family, and it keeps carrying it through however many
// function boundaries the borrow travels.
//
// So this fires on every way a borrow can be minted, not only the explicit cast - the address of a raw
// element, the implicit borrow a call argument gets, a receiver's auto-borrow, a `return &...`. All of
// them arrive here as one of the two nodes below, which is why there are two call sites and not six.
//
// A `#[builtin:]` callee is exempt. `mem::init` and `mem::take` are the two seams that deliberately
// name storage the compiler is not accounting for, and requiring them to manufacture an ordinary
// readable `T&` over uninitialized bytes would be asking for a promise that is *false* at the moment
// it is made
void TypeChecker::check_unsafe_promotion(
    const ValueType &to,
    ExprNode *operand,
    const TokenReference &at
)
{
    if (operand == nullptr || !borrow_promotes_raw_storage(to, operand)) {
        return;
    }

    report_unsafe_promotion(to, at);
}

// **the `unsafe` gate and the issue, said once.** the two refusal predicates are different questions -
// one is about a place a borrow is taken of, the other about a value conversion - but what happens
// once either answers yes is the same, and a depth rule that grows a condition in one of two copies
// is a rule the other silently keeps the old version of
void TypeChecker::report_unsafe_promotion(const ValueType &to, const TokenReference &at)
{
    if (_unsafe_depth > 0) {
        return;
    }

    _collector.collect_issue<Issue::UnsafePromotion>(
        code_ref_for(at), to.get_type_desciption());
}

void TypeChecker::visitBinaryExpr(BinaryExprNode &node)
{
    // codegen (gen_binary_expr) lowers operators only over numeric and a narrow bool set; it
    // supports no operator on struct/class operands, where it would otherwise fall through to a
    // context-free codegen throw. flag exactly that unambiguous case here, located at the operator
    // undeterminable operands (unknown/void/type-param) are left to other diagnostics, and the rarer
    // per-branch primitive gaps (e.g. `%` on two bools) are left to the enriched codegen throw
    // rather than re-encoding codegen's full operator matrix and risking false positives
    if (node.lhs && node.rhs && node.op_node) {
        // **the operands as every rule below wants them**, read once: the type a value-position read
        // yields, plus whether the user wrote `null` there. `adjusted_operand` rather than
        // `parse_time_operand` because this pass runs after PointerAdjuster, where every deref is
        // already a node and result_type() is the truth - the asymmetry those two named constructors
        // exist for. asking result_type() again per rule also re-walks the operand subtree each time
        const OperandFacts lhs_facts = adjusted_operand(node.lhs);
        const OperandFacts rhs_facts = adjusted_operand(node.rhs);

        const ValueType &lhs = lhs_facts.type;
        const ValueType &rhs = rhs_facts.type;

        // **what is wrong with the operands**, asked of AST::binary_operand_refusal rather than
        // decided here. the same two rules have to be reachable from an operator *call*, which is what
        // a use site becomes as soon as anybody in the program declares an infix form of the symbol -
        // see that function
        if (auto refusal = binary_operand_refusal(node.op_node->op, lhs_facts, rhs_facts)) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(node.op_node->token_literal), *refusal);
        }

        // **a shift count the compiler can see, checked here as well as in the folder.** at or above the
        // operand's width the emitted `shl`/`lshr`/`ashr` is poison, and the value that reaches it is
        // poison whether or not anybody asked for the expression to be folded - so `const(1 << 32)` being
        // a located error while the plain `1 << 32` beside it compiled to a number nobody chose was one
        // fact with one diagnostic and one silence. AST::shift_count_refusal is the shared sentence.
        //
        // asked of AST::const_fold rather than of a literal, so `1 << (16 * 2)` is caught too, and only
        // when it answers: a count that is not a constant is a runtime value this cannot speak about, and
        // a refused one is somebody else's diagnostic. The fixpoint has converged by the time this pass
        // runs, so `t_pending` here is already "no round will answer it"
        if (node.op_node->op != nullptr && node.op_node->op->is_shift()) {
            const ConstFoldResult count = const_fold(node.rhs);

            if (count.is_folded()) {
                if (auto refusal = shift_count_refusal(lhs, count.bits)) {
                    _collector.collect_issue<Issue::GenericError>(
                        code_ref_for(node.op_node->token_literal), *refusal);
                }
            }
        }

        // **the one predicate**, AST::binary_has_builtin_meaning, which the parser reads to decide
        // whether to look for a declared `operator` and this reads to report that none was found. it
        // used to be spelled out here, and the parser asking the same question its own way is exactly
        // how the two would come to different answers - one of them silently
        //
        // an undeterminable operand needs no guard here: has_complex_type() is false for unknown, void
        // and a bare type parameter, so the predicate already answers "there is a meaning" for them and
        // leaves the diagnostic to whichever pass actually knows what went wrong
        if (!binary_has_builtin_meaning(node.op_node->op, lhs_facts, rhs_facts)) {

            // **a declared operator that did not fire** is a different thing to say, and the only
            // place it can be said. the parser decides from the operand types it can see, so inside a
            // generic body it saw `T`, took the built-in path, and this node is the substituted clone -
            // a use site that looks like it should have worked
            const bool declared_but_unreached = node.op_node->op->has_fixity(OpFixity::t_infix);

            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(node.op_node->token_literal),
                declared_but_unreached
                    ? fmt::format(
                        "operator '{}' is declared for '{}' and '{}', but an operator applied to a "
                        "type parameter is not resolved yet - the operand types are only known after "
                        "substitution. Write a named function and call that instead.",
                        node.op_node->op->spelling,
                        lhs.get_type_desciption(),
                        rhs.get_type_desciption())
                    : binary_unsupported_operands(node.op_node->op, lhs_facts, rhs_facts));
        }
    }

    RecursiveVisitor::visitBinaryExpr(node);
}

// there was no unary arm here at all, so `-$point` fell through to a context-free codegen throw with
// no location and nothing for the user to act on. the same predicate answers it, and the same
// "an operand that says nothing is somebody else's diagnostic" rule applies
void TypeChecker::visitUnaryExpr(UnaryExprNode &node)
{
    if (node.expr != nullptr) {
        const Operator *op = _collector.operators.get_operator(node.token_operator);
        const OperandFacts operand = adjusted_operand(node.expr);

        if (!unary_has_builtin_meaning(op, operand)) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(node.token_operator),
                fmt::format("operator '{}' is not supported on an operand of type '{}'",
                    node.token_operator.value(),
                    operand.type.get_type_desciption()));
        }
    }

    RecursiveVisitor::visitUnaryExpr(node);
}

// `const` is a promise about the storage an assignment reaches, and after the adjustment pass the
// target's shape says which level that is: a deref means the write goes *through* a pointer, so the
// pointee's const decides it, while any other place names the slot itself. the parser cannot make
// this call - writing through and re-seating are the same token sequence until the adjuster has
// inserted the deref (book/concept/pointers_and_refs_v2.md, "Const")
void TypeChecker::check_const_target(AssignNode &node)
{
    ExprNode &target = *node.target;

    if (target.get_node_type() == NodeType::n_expr_deref) {
        const ValueType pointer_type = static_cast<DerefExprNode &>(target).operand->result_type();

        if (pointer_type.is_pointer() && pointer_type.pointee().is_const()) {
            _collector.collect_issue<Issue::ConstViolation>(
                code_ref_for(node.token_assign),
                fmt::format("cannot write through '{}' - its pointee is const",
                    pointer_type.get_type_desciption()));
        }

        return;
    }

    // an initialization is the one write a const *slot* legitimately gets, and the one re-seat a const
    // *pointer* legitimately gets - both of which are decided below. what it is never entitled to is
    // the write-*through* above: a `ptr<const T>` property means the pointee is not this constructor's
    // to write, however fresh the slot holding the pointer is. so the exemption starts here rather
    // than at the call site, which used to skip this function whole
    //
    // the flag is the whole question, and this exemption is why it has to stay that way: anything folded in
    // here becomes a way to launder a `const` away. a container that seats an element on demand declares an
    // element-*write* operator, which is a call and not an assignment - so a `const` container is refused
    // where the write is decided, in AST::OperatorRewriter::resolve_index_write, and nothing reaches here
    if (node.is_initialization) {
        return;
    }

    if (!is_place_expression(target)) {
        return;
    }

    const ValueType storage = target.result_type();
    if (!storage.is_const()) {
        return;
    }

    // a const *pointer* still permits the write-through above - what it forbids is re-seating, and
    // `$p:$` is the only spelling that reaches the slot, so arriving here with a pointer means that
    if (storage.is_pointer()) {
        _collector.collect_issue<Issue::ConstViolation>(
            code_ref_for(node.token_assign),
            fmt::format("cannot re-seat '{}' - the pointer is const, only its pointee may be written",
                storage.get_type_desciption()));
        return;
    }

    const std::string name = place_description(target);
    _collector.collect_issue<Issue::ConstViolation>(
        code_ref_for(node.token_assign),
        name.empty()
            ? fmt::format("cannot assign to const storage of type '{}'", storage.get_type_desciption())
            : fmt::format("cannot assign to '{}' - it is declared const", name));
}

// the shapes that conform but cannot be *stored* - a struct, a generic instantiation, an interface with an
// operator requirement. answered by AST::interface_erasure_refusal, the one owner of the question, and
// reported here at every arrival site: codegen's widening has no table to fall back on, so an unreported
// one is an internal error rather than a diagnostic
//
// true when it reported, so a caller stops rather than adding a second, vaguer message about the same
// value. that is why this is checked *before* argument_fit: `Square` conforms perfectly well, and
// "cannot implicitly convert" would say nothing about why storing it is the problem
bool TypeChecker::check_interface_erasure(const ValueType &to, const ExprNode &value, const TokenReference &at)
{
    if (!to.is_interface()) {
        return false;
    }

    const std::string refusal = AST::interface_erasure_refusal(value.result_type(), to);

    if (refusal.empty()) {
        return false;
    }

    _collector.collect_issue<Issue::InvalidTypeConversion>(code_ref_for(at), refusal);
    return true;
}

void TypeChecker::check_destination_fits(Destination dest, const ValueType &to, const ExprNode &value, const TokenReference &at)
{
    const ValueType from = value.result_type();

    // scoped to the destinations that have no conversion to fall back on: that is the surface where
    // a mismatch is a real error rather than a widening. `T&` widens to `ptr<T>` freely while the
    // narrowing back asserts non-nullness and needs the explicit cast
    // (book/concept/pointers_and_refs_v2.md, "Two pointer types"), and a struct slot takes nothing
    // but that struct. null answers to its own rules, and an undeterminable type to other diagnostics
    if (to.is_void() || from.is_void()
        || is_written_null(&value)
        || (!demands_exact_conversion(to) && !demands_exact_conversion(from))
        || is_implicitly_convertible(from, to)) {
        return;
    }

    // **an interface destination takes a class that conforms.** asked through AST::argument_fit, which is
    // already the one answer to "does this value reach that type" and where the widening's rank lives -
    // so a declaration, an assignment and a return accept exactly what an argument position does
    //
    // asked of the *payload* when the destination is a tagged optional and the value is not itself one:
    // `Drawable? $d = Circle(4)` is two widenings, and an optional accepts whatever its payload accepts.
    // one peel here rather than an arm per question, and through AST::arrival_wraps_optional so it is the
    // same peel AST::argument_fit and TypeLowering::coerce_value make
    const ValueType destination = arrival_wraps_optional(from, to) ? to.optional_payload() : to;

    if (destination.is_interface()) {
        if (check_interface_erasure(destination, value, at)) {
            return;
        }

        if (arg_assignable_to(from, &value, destination)) {
            return;
        }
    }

    // the hints are properties of the type pair rather than of the destination, so they are
    // phrased once here
    std::string hint;
    if (!to.is_pointer() && from.is_pointer() && dest == Destination::t_assignment) {
        hint = " - to change where a pointer points, assign to ':$'";
    }
    // only when the value is an address too: the cast the hint asks for narrows a nullable
    // pointer to a borrow, and there is nothing to narrow if the value is not one
    else if (from.is_pointer() && to.is_pointer() && !to.is_nullable()) {
        hint = " - write the cast explicitly to assert it is not null";
    }

    std::string message;
    switch (dest) {
        case Destination::t_declaration:
            message = fmt::format("cannot implicitly convert '{}' to '{}'{}",
                from.get_type_desciption(), to.get_type_desciption(), hint);
            break;

        case Destination::t_assignment:
            message = fmt::format("cannot assign '{}' to '{}'{}",
                from.get_type_desciption(), to.get_type_desciption(), hint);
            break;

        case Destination::t_return:
            message = fmt::format("cannot return '{}' from a function declared '{}'{}",
                from.get_type_desciption(), to.get_type_desciption(), hint);
            break;
    }

    _collector.collect_issue<Issue::InvalidTypeConversion>(code_ref_for(at), message);
}

void TypeChecker::visit_assign(AssignNode &node)
{
    if (node.target != nullptr) {
        check_const_target(node);
    }

    // the value has to fit the storage the target names, checked wherever a conversion cannot be
    // synthesized for it (demands_exact_conversion)
    //
    // this is what rejects `$p = &$b`: after the adjustment pass the target is a deref of $p,
    // so the storage is an int32 while the value is an int32& - assigning an address into the
    // pointee's slot. re-seating is spelled `$p:$ = &$b`, whose target *is* the slot
    // (book/concept/pointers_and_refs_v2.md, "Binding, writing, and re-seating")
    if (node.target && node.value_expr) {
        const ValueType target_type = node.target->result_type();

        // check_destination_fits waves a null value through, so the rule for one is asked here
        if (is_written_null(node.value_expr)) {
            if (const char *reason = null_rejection_reason(target_type)) {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(node.token_assign),
                    fmt::format("cannot assign null to '{}' - {}",
                        target_type.get_type_desciption(), reason));
            }
        }

        check_destination_fits(Destination::t_assignment, target_type, *node.value_expr, node.token_assign);
    }

    RecursiveVisitor::visit_assign(node);
}

void TypeChecker::visitVarDecl(VarDeclNode &node)
{
    if (node.has_type() && contains_type_param(node.type())) {
        _collector.collect_issue<Issue::UnresolvedTypeParameter>(
            code_ref_for(node.token_varname),
            fmt::format(
                "The type of variable '{}' could not be resolved to a concrete type "
                "(unresolved generic type parameter)",
                node.name()));
    }

    // a guard's binding is deliberately one level less nullable than its initializer - the statement
    // around it is what makes that sound, by only reaching the declaration on the path where the value
    // was there. so the ordinary fit rule is skipped rather than relaxed: relaxing it would let *every*
    // declaration drop a `?`, which is exactly what this whole phase exists to prevent
    //
    // what still gets checked is the payload, in Parser::parse_guard: a declared type that does not match
    // what is inside the nullable is refused there, where the two are both in hand
    if (node.init_expr && node.has_type() && !node.binds_unwrapped) {
        check_destination_fits(Destination::t_declaration, node.type(), *node.init_expr, node.token_varname);
    }

    if (node.has_type() && is_written_null(node.init_expr)) {
        if (const char *reason = null_rejection_reason(node.type())) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(node.token_varname),
                fmt::format("'{}' cannot be null - {}", node.type().get_type_desciption(), reason));
        }
    }

    // locate any implicit cast in the initializer at the declared variable
    const TokenReference *prev = _context_token;
    _context_token = &node.token_varname;
    RecursiveVisitor::visitVarDecl(node);
    _context_token = prev;
}

};  // namespace AST
