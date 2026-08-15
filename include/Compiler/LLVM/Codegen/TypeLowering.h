#ifndef TYPELOWERING_H
#define TYPELOWERING_H

#pragma once

#include "AST/ASTValueType.h"
#include "Compiler/LLVM/CompilationUnit.h"
#include "Compiler/LLVM/Codegen/ClassLayout.h"
#include "Compiler/LLVM/Codegen/ReturnAbi.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

#include <functional>
#include <set>
#include <string>
#include <unordered_map>

namespace AST
{
    class Bundle;
    class FunctionDeclNode;
    class TypeDeclNode;
    class ComplexType;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // lowers echo types & declarations to their llvm equivalents: the primitive/struct type
    // mapping, the per-module function & struct declaration prepass, and lazy instantiation of a
    // generic struct instance on first use
    class TypeLowering
    {
    public:
        TypeLowering(CodegenContext &ctx) : _ctx(ctx) {};

        // one unit per module, **except the ones named in `cached_modules`**.
        //
        // A module whose compiled object comes from the cache gets no unit at all, which is the whole of the
        // gate: every later step - build_struct_maps, both loops of build_function_maps, the body walk, the
        // drain - iterates `_ctx.cmp_units`, so it is skipped by construction rather than by a flag each of
        // them has to remember to ask about. References to its symbols still resolve, because a caller
        // declares what it needs into its *own* unit.
        //
        // `function_file_map` is deliberately still built over every module in the bundle, cached or not: an
        // ODR-shared body from a cached module's template can be emitted into a fresh unit, and it has to
        // carry the source position it was written at
        void create_cmp_units(
            const AST::Bundle &bundle, const std::set<std::string> &cached_modules = {});

        // both walk `_ctx.cmp_units`, which create_cmp_units has already built out of the bundle -
        // so neither takes the bundle again, and neither is a whole-program pass despite running
        // once for the program
        void build_function_maps();
        void build_struct_maps();

        llvm::Function *create_llvm_func_decl(const AST::FunctionDeclNode *node, Compiler::LLVM::CmpUnit &cmp_unit);

        // **what the type system knows and the IR did not say.** every declaration went to LLVM with no
        // attribute of any kind, so the optimizer had to re-derive from raw IR what `ValueType` had
        // already decided - most of all that a borrow is never null, which is one bit on the type
        // (`t_nullable`) and was the difference between a load it may forward and one it must repeat.
        //
        // here rather than in the AST layer because it needs a `DataLayout` and a lowered `llvm::Type` to
        // say *how much* is dereferenceable, and neither exists before codegen. the AST supplies the
        // facts, this supplies the spelling - one owner, called from the one place that mints a Function
        void apply_function_attributes(
            const AST::FunctionDeclNode *node, llvm::Function *func, Compiler::LLVM::CmpUnit &cmp_unit,
            const ReturnAbi &abi);

        // **how *this declaration* hands its answer back**, which is the size rule
        // Compiler::LLVM::return_abi_for owns plus the one thing it cannot see: whether the declaration
        // uses Echo's convention at all.
        //
        // an `extern` is exempt, and that exemption is the load-bearing half - the rule below is Echo's
        // own, chosen because Echo emits both halves of every call it makes, and claiming it for a
        // foreign symbol replaces one wrong answer with a different wrong answer. it lives here rather
        // than at each site because ReturnAbi.h says four places must agree byte for byte or the
        // arguments shift one register in silence, and a rule that must not drift may not be written
        // four times
        ReturnAbi return_abi_of(
            const AST::FunctionDeclNode *node, Compiler::LLVM::CmpUnit &cmp_unit);
        llvm::StructType *create_llvm_struct_decl(const AST::TypeDeclNode *node, Compiler::LLVM::CmpUnit &cmp_unit);

        // lowers a generic struct instantiation (an interned ComplexType with concrete property
        // types) to an llvm struct on first use, registering it in the compilation unit
        llvm::StructType *create_llvm_struct_for_instance(const AST::ComplexType *type, const Compiler::LLVM::CmpUnit &cmp_unit);

        // the heap block, payload and identity global of a class, lowered into this unit on first
        // use. separate from get_llvm_type because that answers what a class *value* is - an opaque
        // handle - and so cannot be the thing that lowers the layout: everything that needs the
        // layout (sizing an allocation, reaching the payload, touching the count, instanceof) asks
        // here instead. keyed on the ComplexType, so a generic instantiation and a class used from
        // another module both resolve
        ClassLayout get_or_create_class_layout(const AST::ComplexType *type, const Compiler::LLVM::CmpUnit &cmp_unit);

        // the `linkonce_odr` byte whose *address* is an interface's identity, minted on first use in this
        // unit. the interface counterpart of a class's typeinfo global and named the same way - from
        // mangled_token(), so two same-named interfaces in different namespaces are two identities
        //
        // public for the reason the layout above is: the two readers are the conformance table built here
        // and the `instanceof` scan in ClassCodegen, and both need the same global
        llvm::GlobalVariable *get_or_create_interface_identity(
            const AST::ComplexType &interface,
            const Compiler::LLVM::CmpUnit &cmp_unit
        );

        llvm::Type *get_llvm_type(const AST::ValueType &type, const Compiler::LLVM::CmpUnit &cmp_unit);
        llvm::Type *get_llvm_type(const AST::ValueTypePrimitive type);

        // the fat pointer every `function<R(P...)>` value is: `{ ptr fn, ptr env }`. one shape for every
        // signature, because the *signature* only decides how the target is called, not how the value is
        // stored - which is what lets a callable be assigned, passed and returned without knowing what
        // it points at. an anonymous StructType, so two units agree structurally with no registration
        llvm::StructType *callable_llvm_type();

        // the fat pointer every interface value is: `{ ptr object, ptr vtable }`. one shape for every
        // interface, for the reason a callable has one shape for every signature - the *interface* only
        // decides how the target is called, not how the value is stored
        //
        // **field 0 holds the class handle**, which is what makes an erased receiver free: a class
        // method's `$this` is `Circle&`, the address of a slot holding a handle, and the address of
        // field 0 is exactly that. see Codegen/IfaceValue.h for the slot names
        llvm::StructType *iface_llvm_type();

        // **is this nullable value present?** an i1, and the one primitive `== null`, `??`, `?->` and
        // `guard` all branch on - so the two shapes a `T?` can have are told apart in exactly one place
        //
        //  - address-like (`ptr<T>`, a class handle, a weak handle): a null pointer comparison
        //  - wrapped (`{ i1 __has, T }`): the tag
        //
        // here rather than in ExprCodegen because three subsystems need it - the expression arm for
        // `== null` and `??`, the statement arm for `guard` - and this is the file that already owns what
        // shape a ValueType has, right beside coerce_value and the wrapper it mints below
        llvm::Value *gen_has_value(llvm::Value *value, const AST::ValueType &type);

        // the payload of a nullable that is known present - the counterpart of gen_has_value, and only
        // ever emitted on a path that one has guarded. an address-like nullable *is* its payload, so this
        // is the identity there; a wrapped one gives up its value field
        llvm::Value *gen_unwrapped(llvm::Value *value, const AST::ValueType &type);

        // **the empty value of a nullable** - the third of the set, and here for the reason the two above
        // are: `null` itself and a `?->` that short-circuited both have to produce one, and they produced
        // two different values before this (an all-zero wrapper against a wrapper with an undef payload).
        // both are correct, because nothing reads a payload gen_has_value said was not there - which is
        // exactly why the difference could sit there unnoticed
        //
        // `Constant::getNullValue` covers both shapes at once: a null pointer for an address-like nullable,
        // and an all-zero `{ i1 __has, T }` for a wrapped one, the two agreeing because
        // AST::k_optional_has_index is field 0 and false is zero
        llvm::Value *gen_absent(const AST::ValueType &type, const Compiler::LLVM::CmpUnit &cmp_unit);

        // the shape a `T?` takes when `T` has no spare null value of its own: `{ i1 __has, T }`. asked only
        // for a type AST::ValueType::is_wrapped_optional() answers true for - a nullable primitive, struct,
        // interface or callable. over a pointer, a class handle or a weak handle there is no wrapper at
        // all and `T?` lowers to `T`, because a null address already *is* the empty case
        //
        // named `eco.optional.<mangled>` and looked up by name first, exactly as class_header_llvm_type is,
        // so every unit that mentions `int32?` agrees on one llvm::Type. the mangled name is what keeps
        // `int32?` and `float64?` apart - a literal StructType would work too, but the name is what makes
        // the IR readable when a wrapped optional turns up in a dump
        llvm::StructType *optional_llvm_type(const AST::ValueType &type, const Compiler::LLVM::CmpUnit &cmp_unit);

        // the **header** every class block starts with, `{ i64 strong, ptr typeinfo }`, with no payload.
        // for a handle whose concrete class is not statically known - the object inside an erased value -
        // which is the one thing a full ClassLayout cannot be built for
        //
        // sound because the header is identical in every box by construction (see Codegen/ClassLayout.h):
        // the payload is *wrapped*, never prefixed, so the strong count and the typeinfo word sit at the
        // same two offsets for every class. it is the same reasoning gen_strong_inc already relies on when
        // it is handed a null layout and reads the count as the block's first word
        llvm::StructType *class_header_llvm_type();

        // the descriptor a block's typeinfo word points at, `{ i64 count, ptr conformances }` - see
        // Codegen/ClassLayout.h for the slot names. one answer for the writer that mints it and the
        // `instanceof` scan that GEPs through it, so a third slot cannot be added to one and not the
        // other; the two spelled it out independently and nothing would have caught the drift
        //
        // a literal StructType rather than a named one, like eco.callable is not: it is structural, and
        // llvm uniques an identical literal to one type across every unit
        llvm::StructType *typeinfo_llvm_type();

        // the `[N x ptr]` of a class's implementations of an interface's requirements, in slot order -
        // `@Circle.8Drawable.vtable`. resolved at the **widening**, where the concrete class is still
        // statically known, which is what makes a dispatch one load rather than a table scan
        //
        // null when the class does not conform, when any requirement is unanswered, or when the interface
        // declares an operator requirement (which has no slot). every one of those is refused with a
        // located diagnostic before codegen, so a null here is a compiler bug rather than a program error
        llvm::Constant *get_or_create_vtable(
            const AST::ValueType &class_type,
            const AST::ValueType &interface,
            const Compiler::LLVM::CmpUnit &cmp_unit
        );

        // the calling shape get_llvm_function_type prepends an environment for, or does not. one
        // argument rather than a second function, so the unconditional env prepend keeps one owner
        // and both callers say which they meant
        enum class FunctionCallingShape
        {
            t_echo,
            t_c,
        };

        // the llvm::FunctionType a callable's `fn` slot points at. the environment is parameter 0
        // under t_echo, always, exactly the way a method's `$this` is - a capturing closure has
        // nowhere else to read its captures from, and a uniform shape is what lets one indirect
        // call site invoke either kind. t_c is C's one-word shape: no environment
        llvm::FunctionType *get_llvm_function_type(
            const AST::CallableSignature &signature,
            const Compiler::LLVM::CmpUnit &cmp_unit,
            FunctionCallingShape shape
        );

        // converts `value` from one echo type to another, emitting the widening, narrowing or
        // int/float conversion the pair calls for. returns the value unchanged when no
        // conversion is needed, and throws when the pair has no meaning
        //
        // keyed on ValueType rather than llvm::Type because signedness does not survive
        // lowering: i8 -> i32 is a sign extend for int8 and a zero extend for uint8, and the
        // llvm types are identical either way
        //
        // `from` may be void or unknown - BinaryExprNode::result_type() answers void whenever
        // its operands differ - in which case the value's own llvm type stands in for it and
        // `to` supplies the signedness
        llvm::Value *coerce_value(llvm::Value *value, const AST::ValueType &from, const AST::ValueType &to, const Compiler::LLVM::CmpUnit &cmp_unit);

    private:
        // wraps an already-lowered payload in its heap block and mints the class's typeinfo global
        // idempotent - a second call over a structure that already has a box does nothing
        //
        // takes the ComplexType rather than a name, because the two names it needs are different
        // questions: the block's llvm name is cosmetic (llvm uniques it anyway), while the typeinfo
        // global's name *is* the class's runtime identity and has to be the mangled token. every
        // caller had a ComplexType to hand and passed a display string, which is how the two came
        // apart - `Foo` in two namespaces, and an instantiation whose name is the string `Box<int32>`
        void build_class_box(Structure &structure, const AST::ComplexType &type, const Compiler::LLVM::CmpUnit &cmp_unit);

        // the `[N x ptr]` of interface identities a class conforms to, or null when it conforms to none.
        // what a typeinfo's `conformances` slot points at, and what `instanceof <interface>` scans
        llvm::Constant *build_conformance_table(
            const AST::ComplexType &type,
            const Compiler::LLVM::CmpUnit &cmp_unit
        );

        // the one way this file mints a runtime structure: a `linkonce_odr constant` looked up by name
        // first, so a unit that already emitted it reuses the definition rather than colliding with it.
        // every global the interface machinery emits follows the same policy - a typeinfo, an interface
        // identity, a conformance table and a vtable - and the policy is what makes an *address* a
        // stable identity across modules with no numbering scheme to keep in sync. spelled out four
        // times, one copy forgetting `isConstant` or the name pre-check is a silent duplicate symbol
        //
        // the initializer is built through a callback rather than passed, so a miss is what pays for it:
        // a vtable's costs a whole conformance walk, and the hit is the common case at a widening.
        // `build` may answer null, which declines to emit and answers null - the vtable's own refusals
        llvm::GlobalVariable *get_or_create_odr_constant(
            const std::string &name,
            const std::function<llvm::Constant *()> &build,
            const Compiler::LLVM::CmpUnit &cmp_unit
        );

        CodegenContext &_ctx;
    };
};

#endif
