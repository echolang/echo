#include "AST/ASTValueType.h"

#include "eco.h"

#include "AST/ASTNamespace.h"
#include "AST/ASTTypeParam.h"
#include "External/infint.h"

#include <cassert>

std::string AST::get_primitive_name(ValueTypePrimitive primitive)
{
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
        case ValueTypePrimitive::t_usize: return "usize";
        case ValueTypePrimitive::t_isize: return "isize";
        case ValueTypePrimitive::t_float32: return "float32";
        case ValueTypePrimitive::t_float64: return "float64";
        case ValueTypePrimitive::t_bool: return "bool";
        case ValueTypePrimitive::t_void: return "void";

        default: return "";
    };
}

uint8_t AST::get_primitive_size(ValueTypePrimitive primitive)
{
    switch (primitive) {
        case ValueTypePrimitive::t_int8: return 1;
        case ValueTypePrimitive::t_int16: return 2;
        case ValueTypePrimitive::t_int32: return 4;
        case ValueTypePrimitive::t_int64: return 8;
        case ValueTypePrimitive::t_uint8: return 1;
        case ValueTypePrimitive::t_uint16: return 2;
        case ValueTypePrimitive::t_uint32: return 4;
        case ValueTypePrimitive::t_uint64: return 8;
        // pointer-width, from the one constant that knows the target
        case ValueTypePrimitive::t_usize: return ECO_TARGET_POINTER_SIZE;
        case ValueTypePrimitive::t_isize: return ECO_TARGET_POINTER_SIZE;
        case ValueTypePrimitive::t_float32: return 4;
        case ValueTypePrimitive::t_float64: return 8;
        case ValueTypePrimitive::t_bool: return 1;

        default: return 0;
    };
}

// the character a primitive contributes to a mangled symbol name. every value must be distinct:
// this reaches the LLVM symbol table through decorated_func_name, so two primitives sharing a
// char means two functions sharing a symbol
//
// deliberately exhaustive with no `default`. the default used to answer 'u' - the same char as
// t_complex - so a newly added primitive silently collided instead of failing the build
// 'y' and 'z' are chosen because the surrounding grammar already reserves a lot: 'T' prefixes a
// type parameter, 'C'/'M' mark const/mutable, 'L'/'R' mark leaf/ref, 'N'/'B' mark
// nullable/borrow, 'G' marks a type argument, and decorated_func_name uses 'Z' as its separator
char AST::get_primitive_id_char(ValueTypePrimitive primitive)
{
    switch (primitive) {
        case ValueTypePrimitive::t_complex: return 'u';
        case ValueTypePrimitive::t_int8: return 'c';
        case ValueTypePrimitive::t_int16: return 's';
        case ValueTypePrimitive::t_int32: return 'i';
        case ValueTypePrimitive::t_int64: return 'l';
        case ValueTypePrimitive::t_uint8: return 'C';
        case ValueTypePrimitive::t_uint16: return 'S';
        case ValueTypePrimitive::t_uint32: return 'I';
        case ValueTypePrimitive::t_uint64: return 'L';
        case ValueTypePrimitive::t_usize: return 'y';
        case ValueTypePrimitive::t_isize: return 'z';
        case ValueTypePrimitive::t_float32: return 'f';
        case ValueTypePrimitive::t_float64: return 'd';
        case ValueTypePrimitive::t_bool: return 'b';
        case ValueTypePrimitive::t_void: return 'v';
    };

    assert(false && "unhandled primitive in get_primitive_id_char - every primitive needs a distinct mangling char");
    return 'u';
}

AST::IntegerSize AST::get_integer_size(ValueTypePrimitive primitive)
{
    const uint8_t size = get_primitive_size(primitive);

    switch (primitive) {
        case ValueTypePrimitive::t_int8: return AST::IntegerSize(size, true);
        case ValueTypePrimitive::t_int16: return AST::IntegerSize(size, true);
        case ValueTypePrimitive::t_int32: return AST::IntegerSize(size, true);
        case ValueTypePrimitive::t_int64: return AST::IntegerSize(size, true);
        case ValueTypePrimitive::t_uint8: return AST::IntegerSize(size, false);
        case ValueTypePrimitive::t_uint16: return AST::IntegerSize(size, false);
        case ValueTypePrimitive::t_uint32: return AST::IntegerSize(size, false);
        case ValueTypePrimitive::t_uint64: return AST::IntegerSize(size, false);
        case ValueTypePrimitive::t_usize: return AST::IntegerSize(size, false);
        case ValueTypePrimitive::t_isize: return AST::IntegerSize(size, true);

        default:
            assert(false && "Invalid integer type");
            return AST::IntegerSize(0, false);
    };
}

// the one factory. the kind comes off the ComplexType, so a t_class ValueType can only ever name a
// layout that was declared as a class - which is what lets everything downstream trust the tag on
// either the type or its layout and get the same answer
AST::ValueType AST::ValueType::make_complex(ComplexType *complex_type, const std::vector<ValueType> &args, TypeRegistry *registry)
{
    if (!args.empty()) {
        assert(complex_type->is_generic());
        if (registry) {
            complex_type = registry->get_or_create_instantiation(complex_type, args);
        }
    }

    assert(complex_type != nullptr);

    // a switch rather than a chain of ternaries, so a ComplexTypeKind added without an arm here is a
    // compile error instead of quietly becoming a struct
    ValueTypeKind value_kind = ValueTypeKind::t_struct;
    switch (complex_type->kind) {
    case ComplexTypeKind::t_struct:
        value_kind = ValueTypeKind::t_struct;
        break;
    case ComplexTypeKind::t_class:
        value_kind = ValueTypeKind::t_class;
        break;
    case ComplexTypeKind::t_interface:
        value_kind = ValueTypeKind::t_interface;
        break;
    }

    return ValueType(value_kind, complex_type);
}

AST::ValueType AST::ValueType::make_struct(ComplexType *complex_type, const std::vector<ValueType> &args, TypeRegistry *registry)
{
    ValueType type = make_complex(complex_type, args, registry);
    assert(type.is_struct() && "make_struct over a class layout - use make_complex");
    return type;
}

AST::ValueType AST::ValueType::make_class(ComplexType *complex_type, const std::vector<ValueType> &args, TypeRegistry *registry)
{
    ValueType type = make_complex(complex_type, args, registry);
    assert(type.is_class() && "make_class over a struct layout - use make_complex");
    return type;
}

bool AST::ValueType::will_fit_into(ValueType other) const
{
    if (!(is_primitive() && other.is_primitive())) {
        return false;
    }

    // for floating types we can just check if the size is smaller
    if (is_floating_type() && other.is_floating_type()) {
        return get_primitive_size(primitive) <= get_primitive_size(other.primitive);
    }

    // for integers we need to check if the size is smaller and if the sign is compatible
    else if (is_numeric_type() && other.is_numeric_type()) {
        if (is_signed_integer() && !other.is_signed_integer()) {
            return false;
        }

        return get_primitive_size(primitive) <= get_primitive_size(other.primitive);
    }

    // bool will fit into all numeric types
    else if (primitive == ValueTypePrimitive::t_bool) {
        return other.is_numeric_type();
    }

    return false;
}

bool AST::ValueType::is_same_size(ValueType other) const
{
    if (!(is_primitive() && other.is_primitive())) {
        return false;
    }

    return get_primitive_size(primitive) == get_primitive_size(other.primitive);
}

std::string AST::ComplexType::namespaced_name() const
{
    std::string type_name = name.value_or("[anonymous]");

    // a nested type is reached through its owner and never through a namespace path, so the owner is
    // what a reader has to be given to find it - it already carries the namespace prefix itself
    if (owner_type != nullptr) {
        return owner_type->namespaced_name() + ECO_NAMESPACE_SEPARATOR + type_name;
    }

    if (ast_namespace) {
        std::string ns = ast_namespace->full_name();
        if (!ns.empty()) {
            return ns + ECO_NAMESPACE_SEPARATOR + type_name;
        }
    }

    return type_name;
}

// length prefixed token, so concatenated parts can never be read back ambiguously
// ("a" + "Foo" and "aFoo" both become distinct tokens)
static std::string mangle_length_prefixed(const std::string &part)
{
    return std::to_string(part.size()) + part;
}

std::string AST::ComplexType::mangled_token() const
{
    // an instantiation's own name is the display string ("Box<int32>"), so the token is built
    // from the template plus the recursively mangled arguments instead
    if (is_instantiated() && template_ref) {
        std::string token = template_ref->mangled_token() + "I";
        for (const auto &arg : instantiation_args) {
            token += arg.get_mangled_name();
        }
        return token + "E";
    }

    if (!name.has_value()) {
        return "0";
    }

    // a nested type's owner is part of its symbol identity, or two structs' `view`s would mangle
    // identically and their methods would collide on one llvm::Function. the owner's token already
    // carries its namespace, and nesting an N...E inside one stays self-delimiting
    if (owner_type != nullptr) {
        return "N" + owner_type->mangled_token() + mangle_length_prefixed(name.value()) + "E";
    }

    // the mangling segments, for the same reason mangle_function_name takes them: this token is a
    // symbol identity, and a lexical namespace's display name is shared by every block of one
    // function. the display path for a ComplexType is namespaced_name(), just above
    std::vector<std::string> segments =
        ast_namespace ? ast_namespace->mangling_segments() : std::vector<std::string>{};

    // unqualified types stay a plain token, the `N...E` wrapper only appears when there
    // actually is a namespace path to separate from the name
    if (segments.empty()) {
        return mangle_length_prefixed(name.value());
    }

    std::string token = "N";
    for (const auto &segment : segments) {
        token += mangle_length_prefixed(segment);
    }
    token += mangle_length_prefixed(name.value());

    return token + "E";
}

std::string AST::ValueType::get_mangled_name() const
{
    std::string mangled_name = "";

    // const or mutable
    if (is_const()) {
        mangled_name += "C"; // const
    } else {
        mangled_name += "M"; // mutable
    }

    // a pointer level recurses; anything else is a leaf. keeping the historical R/L slot and
    // giving R a payload means every pointer-free signature mangles byte for byte as before,
    // so only functions that actually take a pointer get a new symbol
    //
    //   <type> ::= [C|M] [Q] ( 'L' <leaf> | 'R' [N|B] <type> | 'W' <type> )
    //
    // N is a nullable ptr<T>, B a borrow T&. self delimiting and prefix free
    //
    // `Q` marks a nullable **non-pointer** level, `T?`. it is emitted here rather than folded into the
    // R slot because a pointer level already spells the same bit with N/B, and reusing that would make
    // `ptr<T>` and `T?` collide. only levels that can now carry the flag and could not before get a new
    // character, so **no existing symbol changes** - the same property the R slot was introduced with
    if (is_nullable() && !is_pointer()) {
        mangled_name += "Q";
    }

    if (is_pointer()) {
        mangled_name += "R";
        mangled_name += is_nullable() ? "N" : "B";
        return mangled_name + pointee().get_mangled_name();
    }

    // its own recursive slot beside R rather than a third letter under it: a weak is not a pointer level,
    // so `weak<Foo>` and `Foo&` have to mangle differently or two overloads taking them would collide
    if (is_weak()) {
        return mangled_name + "W" + weak_target().get_mangled_name();
    }

    mangled_name += "L"; // lvalue

    // primitive type
    if (is_primitive()) {
        mangled_name += "P"; // primitive type
        mangled_name += get_primitive_id_char(primitive);
    } else if (is_type_param()) {
        // the ordinal, not the declaration's address: this feeds decorated_func_name and so the
        // LLVM symbol table, which has to stay reproducible across runs. only a template mangles
        // a type parameter at all, and a template is never emitted
        mangled_name += "T"; // type parameter
        mangled_name += std::to_string(_type_param->ordinal);
    } else if (has_complex_type()) {
        mangled_name += "C"; // complex type
        mangled_name += get_complex_type()->mangled_token();
    } else if (is_callable()) {
        // `F` <return> <param>... `E`, self delimiting the way mangled_token's `I...E` is: each nested
        // type mangles itself and the terminator says where the parameter list ends. without a distinct
        // form here every callable shared the `UA` unknown token, so `function<void()>` and
        // `function<int32(int32)>` produced one symbol
        mangled_name += "F";
        mangled_name += _signature->return_type.get_mangled_name();
        for (const auto &param : _signature->parameter_types) {
            mangled_name += param.get_mangled_name();
        }
        mangled_name += "E";
    } else {
        assert(kind == ValueTypeKind::t_unknown && "a ValueType kind with no mangling would share the unknown token");
        mangled_name += "U"; // unknown type
        mangled_name += "A";
    }

    return mangled_name;
}

std::string AST::ValueType::get_type_desciption() const
{
    std::string prefix = is_const() ? "const " : "";

    // `T?`, rendered on the level that carries the flag. a pointer level is excluded because it has its
    // own two spellings for the same bit - `ptr<T>` is nullable and `T&` is not - so appending a `?` there
    // would render `ptr<int32>?`
    //
    // it has to be rendered *somewhere*: while it was not, a diagnostic about a nullable arriving where a
    // non-nullable was wanted read "cannot implicitly convert 'Node' to 'Node'", which names the one thing
    // the reader can already see and hides the only thing that differs
    const std::string suffix = (is_nullable() && !is_pointer()) ? "?" : "";

    // recursive rather than a prefix/suffix accumulator: an accumulator cannot render
    // `const ptr<const int32>`, where two different levels are each const
    if (is_pointer()) {
        if (is_nullable()) {
            return prefix + "ptr<" + pointee().get_type_desciption() + ">";
        }

        // a borrow spells its pointee's const outward - `const int32&` is the read only
        // borrow of the doc's "Const" section. a const borrow *level* has no spelling of
        // its own, so parenthesise it rather than colliding with the pointee-const form
        if (is_const()) {
            return "const (" + pointee().get_type_desciption() + "&)";
        }
        return pointee().get_type_desciption() + "&";
    }

    // recursive for the same reason, and lowercase for the same reason `ptr` is: it is a type
    // constructor the compiler owns, not a library type the user could have written
    if (is_weak()) {
        return prefix + "weak<" + weak_target().get_type_desciption() + ">" + suffix;
    }

    if (is_primitive()) {
        return prefix + get_primitive_name(primitive) + suffix;
    }

    // the name the user wrote, unqualified: this feeds the interned name of every generic
    // application, so qualifying it here would render Box<int> as Box<Box::T> in the template.
    // TypeParamDecl::describe() is the qualified form, for diagnostics
    if (is_type_param()) {
        return prefix + _type_param->name + suffix;
    }

    if (is_callable()) {
        std::string buffer = prefix + "function<" + _signature->return_type.get_type_desciption() + "(";

        for (size_t i = 0; i < _signature->parameter_types.size(); i++) {
            buffer += (i > 0 ? ", " : "") + _signature->parameter_types[i].get_type_desciption();
        }

        return buffer + ")>" + suffix;
    }

    if (has_complex_type()) {
        ComplexType *ct = get_complex_type();
        if (!ct->name.has_value()) {
            return prefix + "[unknown]" + suffix;
        }

        // if this is an instantiated generic type, the name already includes template args
        // from the TypeRegistry's args_description method. the namespace is prepended here so
        // two same-named types from different namespaces are distinguishable in diagnostics
        return prefix + ct->namespaced_name() + suffix;
    }

    // handle unknown or other types
    return prefix + "[unknown]" + suffix;
}

AST::ComplexType *AST::TypeRegistry::create_anonymous_type(
    const std::string &name,
    AST::ComplexTypeKind kind,
    AST::Namespace *ns
)
{
    auto owned = std::make_unique<ComplexType>();
    ComplexType *type = owned.get();
    _owned.push_back(std::move(owned));

    type->name = name;
    type->kind = kind;
    type->ast_namespace = ns;

    // deliberately *not* entered into `_instantiations`: it instantiates no template, so there is no key
    // it could be interned under, and nothing will ever ask for it by (template, args) again. the caller
    // holds the only handle, which is exactly the ownership a closure's environment wants
    return type;
}

AST::ComplexType *AST::TypeRegistry::get_or_create_instantiation(ComplexType *tmpl, const std::vector<ValueType> &args)
{
    assert(tmpl->is_generic() && tmpl->type_parameters.size() == args.size());

    auto key = std::make_tuple(tmpl, args);
    if (auto it = _instantiations.find(key); it != _instantiations.end()) {
        ComplexType *inst = it->second;
        // refresh if the template gained properties or conformances after this instance was interned -
        // this happens when an application (e.g. a return type) is parsed during the symbol pass,
        // before the struct body populates the template
        const bool stale = inst->property_count() < tmpl->property_count()
            || inst->conformances().size() < tmpl->conformances().size();

        if (inst != tmpl && stale) {
            derive_instantiation(inst, tmpl, args);
        }
        return inst;
    }

    auto owned = std::make_unique<ComplexType>();
    ComplexType *instantiated = owned.get();
    _owned.push_back(std::move(owned));

    if (tmpl->name) {
        instantiated->name = tmpl->name.value() + "<" + args_description(args) + ">";
    }
    instantiated->ast_namespace = tmpl->ast_namespace;
    // the storage class is the template's: `Box<int32>` is a class exactly when `Box` is one. without
    // this an instantiation would answer t_struct and lower as a stack aggregate
    instantiated->kind = tmpl->kind;

    // and so is uniqueness: `buffer<int32>` owns an allocation exactly when `buffer` says it does.
    // without this every instantiation of a unique template would be copyable, which is silent - the
    // copy is a byte copy, so two values end up naming one allocation and both free it
    instantiated->is_unique = tmpl->is_unique;

    // and so is visibility: `Hidden<int32>` is as reachable as `Hidden` is, and where it was written is
    // the template's file. without this a private generic type would be private as a template and public
    // for every instance of it - which is the shape a use site actually names
    instantiated->visibility = tmpl->visibility;
    instantiated->declared_in = tmpl->declared_in;

    instantiated->template_ref = tmpl;
    instantiated->instantiation_args = args;

    // insert into the cache BEFORE substituting properties, so a self-referential generic
    // (e.g. a property of type ptr<Self<T>>) resolves back to this in-progress instance
    // instead of recursing forever
    _instantiations[key] = instantiated;

    derive_instantiation(instantiated, tmpl, args);

    return instantiated;
}

void AST::TypeRegistry::derive_instantiation(
    ComplexType *inst,
    ComplexType *tmpl,
    const std::vector<ValueType> &args
)
{
    // a self-referential argument comes back through get_or_create_instantiation, finds this instance
    // in the cache mid-build and reads it as stale. without this the pair would recurse until the stack
    // ran out; with it the inner call hands back the in-progress instance and the outer one finishes
    if (!_deriving.insert(inst).second) {
        return;
    }

    // one substitution for everything derived below: the arguments arrive positionally, matching the
    // template's declared parameter order, and positional() is where that arity is checked
    TypeSubstitution subst = TypeSubstitution::positional(tmpl->type_parameters, args);

    inst->_properties.clear();
    inst->_property_map.clear();
    for (const auto &prop : tmpl->_properties) {
        // **privacy travels with the property**, for `kind`'s and `is_unique`'s reason: an
        // instantiation has properties and no declaration nodes, so this line is the only thing that
        // can answer for `mem::buffer<int32>` what `mem::buffer<T>` declared. without it privacy held
        // for a template and evaporated for every instance - which is to say, for every value anyone
        // actually builds
        inst->add_property(prop.name, substitute_type(prop.type, subst, *this), prop.is_private);
    }

    // the conformances, at the same moment and through the same substitution as the properties, because
    // they go stale together: `struct Bag<E> : contract::iterable<E>` conforms to `contract::iterable<int32>`
    // once E is bound. done here rather than redirected through template_or_self at read time, which is what
    // lets AST::conforms_to stay a pure membership test - it has no TypeRegistry to re-intern
    // `contract::iterable<int32>` with, and neither has TypeParamDecl::allows(), its caller
    inst->_conformances.clear();
    for (const auto &conformance : tmpl->_conformances) {
        inst->_conformances.push_back(substitute_type(conformance, subst, *this));
    }

    _deriving.erase(inst);
}

std::string AST::TypeRegistry::args_description(const std::vector<ValueType> &args) const
{
    std::string desc;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) desc += ",";
        desc += args[i].get_type_desciption();
    }
    return desc;
}

void AST::ComplexType::add_type_parameter(TypeParamDecl *param)
{
    assert(param);
    assert(param->ordinal == type_parameters.size());
    param->set_owner(this);
    type_parameters.push_back(param);
}

void AST::ComplexType::add_associated_type(TypeParamDecl *decl)
{
    assert(decl);
    assert(find_associated_type(decl->name) == nullptr
        && "an associated type is declared twice - the parser must reuse it across passes");

    decl->is_associated = true;
    decl->set_owner(this);

    // the ordinal continues past the type parameters rather than restarting. hygiene rather than
    // necessity - a requirement is never emitted, so it is never mangled - but get_mangled_name renders
    // a t_generic as `T<ordinal>`, and a collision would make `interface I<V> { type V2 : ...; }`'s two
    // declarations print alike in a `--print-*` dump
    decl->ordinal = type_parameters.size() + _associated_types.size();

    _associated_types.push_back(decl);
}

AST::TypeParamDecl *AST::ComplexType::find_associated_type(const std::string &name) const
{
    for (TypeParamDecl *decl : _associated_types) {
        if (decl->name == name) {
            return decl;
        }
    }

    return nullptr;
}

bool AST::ComplexType::declares_type_param(const ValueType &type) const
{
    assert(type.is_type_param());

    // pointer membership, which is a stronger check than the bounds test it replaces: a parameter
    // of some *other* generic no longer passes just because the ordinals happen to line up
    const TypeParamDecl *param = type.get_type_param();
    for (const auto *declared : type_parameters) {
        if (declared == param) {
            return true;
        }
    }
    return false;
}

AST::ValueType AST::value_type_of(const ValueType &type)
{
    return type.is_pointer() ? type.pointee() : type;
}

AST::ValueType AST::target_type_of(const ValueType &type)
{
    ValueType target = type;
    while (target.is_pointer()) {
        target = target.pointee();
    }
    return target;
}

bool AST::is_implicitly_convertible(const ValueType &from, const ValueType &to)
{
    ValueType bare_from = ValueType::make_mutable(from);
    ValueType bare_to = ValueType::make_mutable(to);

    if (bare_from == bare_to) {
        return true;
    }

    // **a value widens into `T?`, and a `T?` never narrows back.** one direction only, and it is the same
    // asymmetry `T&` -> `ptr<T>` already has one arm down: widening discards a guarantee, which is always
    // safe, while narrowing *asserts* one and needs somewhere to put the failure
    //
    // so `Foo? $x = $foo;` and `f($foo)` against a `Foo?` parameter are free, and `Foo $y = $maybe;` is a
    // located error naming the three ways through - `guard`, `??`, `?->`. that refusal is the entire
    // safety story of book/concept/nullability.md: a dead weak upgrades to `Foo?`, and there is no step from
    // to a `Foo` anybody can read
    //
    // pointer levels are excluded and keep their own arm below: on one of them this same flag is the
    // `ptr<T>` / `T&` distinction, which already has a rule and a narrowing that emits a runtime trap
    if (!bare_from.is_pointer() && !bare_to.is_pointer()
        && bare_to.is_nullable() && !bare_from.is_nullable()) {
        return ValueType::make_non_nullable(bare_to) == bare_from;
    }

    if (bare_from.is_pointer() && bare_to.is_pointer()) {
        // the pointee has to be the same type, but the target may add const to it: a `const
        // int32&` only promises to read, so every `int32&` satisfies it. going the other way
        // would launder the promise away, so it stays an error. this is what makes the doc's
        // recommended read-only parameter form usable at all (L229)
        const ValueType from_pointee = bare_from.pointee();
        const ValueType to_pointee = bare_to.pointee();

        const bool pointee_compatible =
            ValueType::make_mutable(from_pointee) == ValueType::make_mutable(to_pointee)
            && (to_pointee.is_const() || !from_pointee.is_const());

        if (!pointee_compatible) {
            return false;
        }

        // a borrow widens to a nullable pointer over the same pointee: `T&` is a `ptr<T>` that
        // happens to be known non-null, so the conversion only discards a guarantee. the
        // reverse asserts non-nullness and needs the explicit cast
        return bare_from.is_nullable() == bare_to.is_nullable() || bare_to.is_nullable();
    }

    // **a weak converts to nothing and nothing converts to a weak.** it reaches here only through the
    // equality above, which is what refuses `Foo $x = $w` and `weak<Foo> $w = $obj` alike. taking a weak
    // reference is a written operation - `&$obj`, `weak($obj)` - and reading one is `strong($w)`, because
    // both move a count and an implicit conversion that moved a count would be invisible at the site
    // that pays for it
    if (bare_from.is_weak() || bare_to.is_weak()) {
        return false;
    }

    // note there is deliberately no "a pointer converts to its pointee" rule. the auto-deref
    // that makes a pointer usable where its pointee is expected is a *read*, and the pointer
    // adjustment pass writes it into the tree as an explicit deref node - so by the time
    // anything asks this question, a value-position pointer read already has the pointee type
    // allowing it here would instead accept `$p = &$b`, quietly storing an address into the
    // pointee's slot where the doc requires an error (L87)
    return false;
}

bool AST::can_type_a_literal(const ValueType &type)
{
    return type.is_primitive() && !type.is_void();
}

bool AST::contains_type_param(const ValueType &type)
{
    return contains_type_param(type, nullptr);
}

bool AST::contains_type_param(const ValueType &type, const TypeParamDecl *param)
{
    if (type.is_type_param()) {
        // a null `param` is the coarse question - any parameter at all - so the two share one walk
        // rather than one being a copy of the other with a comparison added
        return param == nullptr || type.get_type_param() == param;
    }

    if (type.is_pointer()) {
        return contains_type_param(type.pointee(), param);
    }

    // recursive for the pointer's reason: a `weak<T>` is as unresolved as a `ptr<T>`, and answering
    // false here would have the monomorphizer stop chasing it and TypeLowering throw on the T far away
    if (type.is_weak()) {
        return contains_type_param(type.weak_target(), param);
    }

    // structurally, like a pointer: `function<void(T)>` is as unresolved as `ptr<T>` is. answering
    // false here would make the monomorphizer stop chasing it and TypeLowering throw on the T far away
    if (type.is_callable()) {
        if (contains_type_param(type.signature().return_type, param)) {
            return true;
        }

        for (const auto &parameter_type : type.signature().parameter_types) {
            if (contains_type_param(parameter_type, param)) {
                return true;
            }
        }

        return false;
    }

    // a generic application is unresolved if any of its arguments still is
    if (type.has_complex_type()) {
        ComplexType *ct = type.get_complex_type();
        if (ct && ct->is_instantiated()) {
            for (const auto &arg : ct->instantiation_args) {
                if (contains_type_param(arg, param)) {
                    return true;
                }
            }
        }
    }

    return false;
}

AST::ValueType AST::substitute_type(const ValueType &type, const TypeSubstitution &subst, TypeRegistry &registry)
{
    // a type-parameter reference resolves to its bound type, carrying the reference's flags
    // an *unbound* parameter is returned unchanged rather than asserting: that is what makes a
    // partial substitution well defined, so a generic member of a generic owner can have the
    // owner's parameters resolved while its own stay generic. the arity check that used to live
    // here now sits in TypeSubstitution::positional, which knows what full coverage means
    // a pointer substitutes through its pointee and is rebuilt, so `ptr<T>` with T := ptr<int>
    // now yields ptr<ptr<int>> instead of collapsing onto a single idempotent flag
    if (type.is_pointer()) {
        ValueType inner = substitute_type(type.pointee(), subst, registry);
        ValueType result = ValueType::make_pointer(inner, type.is_nullable());
        return type.is_const() ? ValueType::make_const(result) : result;
    }

    // and a weak substitutes through its target, so `weak<T>` inside a template becomes `weak<Node>` in
    // the instance. an *unbound* target is rebuilt unchanged rather than skipped, for the reason the
    // pointer arm is rebuilt: partial substitution has to stay well defined
    if (type.is_weak()) {
        ValueType inner = substitute_type(type.weak_target(), subst, registry);

        // a target that substituted to something uncounted is not this function's to report - the
        // constraint that admits only a class belongs at the declaration, and rebuilding here would
        // trip make_weak's assert on a program that merely has a diagnostic waiting for it
        if (!inner.is_class() && !inner.is_type_param()) {
            return type;
        }

        ValueType result = ValueType::make_weak(inner);
        return type.is_const() ? ValueType::make_const(result) : result;
    }

    // and structurally through a signature, for the same reason - returning it unchanged would leave
    // `function<void(T)>` generic forever inside an instantiated body
    if (type.is_callable()) {
        // a concrete signature substitutes to itself, and rebuilding one mints a fresh
        // shared_ptr that then compares structurally rather than by identity. asked here rather
        // than at the top: the arms above are cheap to redo, a signature is not
        if (!contains_type_param(type)) {
            return type;
        }

        std::vector<ValueType> params;
        params.reserve(type.signature().parameter_types.size());

        for (const auto &param : type.signature().parameter_types) {
            params.push_back(substitute_type(param, subst, registry));
        }

        ValueType result = ValueType::make_callable(
            substitute_type(type.signature().return_type, subst, registry), std::move(params));

        return type.is_const() ? ValueType::make_const(result) : result;
    }

    if (type.is_type_param()) {
        const ValueType *bound = subst.lookup(type.get_type_param());
        if (!bound) {
            return type;
        }

        // **every flag on the reference carries over**, because a flag describes the level it sits on and
        // this level is being replaced by what it named. `const T` with T := Node is a `const Node`, and
        // `T?` is a `Node?` - both are properties the *use* declared, not the bound type's to have
        //
        // it used to carry only const, from when nullability could sit nowhere but a pointer level. once
        // `T?` became writable that omission made a generic silently disagree with itself: the declared
        // type of `T? $s` substituted to `Node` while its initializer substituted to `Node?`, so a body
        // that compiled as a template failed at every instantiation, naming a conversion neither half of
        // the program had asked for
        //
        // OR'd rather than re-derived, so a bound type that is *already* nullable stays so - `T?` with
        // T := Node? is a Node?, which is what make_nullable being idempotent means
        ValueType result = *bound;
        if (type.is_const()) {
            result = ValueType::make_const(result);
        }
        if (type.is_nullable() && !result.is_pointer()) {
            // a pointer level spells this bit with its own two forms, `ptr<T>` and `T&`, so setting it on
            // one would silently turn a borrow into a nullable pointer - a promise laundered away rather
            // than a flag copied. `T?` with T := int32& is a case the grammar has no spelling for anyway
            result = ValueType::make_nullable(result);
        }

        return result;
    }

    // a generic application: recursively substitute its arguments, then re-intern
    if (type.has_complex_type()) {
        ComplexType *ct = type.get_complex_type();
        if (ct && ct->is_instantiated()) {
            std::vector<ValueType> resolved_args;
            resolved_args.reserve(ct->instantiation_args.size());
            for (const auto &arg : ct->instantiation_args) {
                resolved_args.push_back(substitute_type(arg, subst, registry));
            }
            ComplexType *inst = registry.get_or_create_instantiation(ct->template_ref, resolved_args);
            ValueType result = ValueType::make_complex(inst);
            return type.is_const() ? ValueType::make_const(result) : result;
        }
    }

    // primitives and already-concrete types are unchanged
    return type;
}
// structural, and recursive through every part: this is what makes two independently written
// `function<void(int32)>`s one type, which is the whole point of the callable kind being structural
bool AST::CallableSignature::operator==(const AST::CallableSignature &other) const
{
    if (parameter_types.size() != other.parameter_types.size()) {
        return false;
    }

    if (!(return_type == other.return_type)) {
        return false;
    }

    for (size_t i = 0; i < parameter_types.size(); i++) {
        if (!(parameter_types[i] == other.parameter_types[i])) {
            return false;
        }
    }

    return true;
}
