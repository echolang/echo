#include "Compiler/LLVM/Codegen/DebugPrintCodegen.h"

#include "Compiler/LLVM/Codegen/ClassLayout.h"
#include "Compiler/LLVM/CompilationUnit.h"
#include "Compiler/LLVM/Codegen/PrintfConversion.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/Codegen/DebugInfoCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/ASTNullability.h"
#include "AST/ASTCoreTypes.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

#include <fmt/core.h>

#include <cassert>

namespace Compiler::LLVM
{

namespace
{
    // pushed and popped around one recursion level, so an early return through the `<cycle>` or
    // `<max depth>` arm cannot leave an entry behind - which would then cut a *sibling* that shares the
    // type, and the output would depend on the order properties happen to be declared in
    struct PathGuard
    {
        std::vector<const AST::ComplexType *> &path;

        PathGuard(std::vector<const AST::ComplexType *> &path, const AST::ComplexType *entry) : path(path)
        {
            path.push_back(entry);
        }

        ~PathGuard() { path.pop_back(); }
    };
}

void DebugPrintCodegen::gen_dprint(const LValue &value)
{
    // being called after a `die` would be a caller bug, not a shape this handles: every block this
    // creates assumes there is a live one to branch from
    assert(!_ctx.block_is_terminated() && "dprint reached a terminated block");
    assert(_pending_format.empty() && _pending_args.empty() && "dprint left a buffer behind");

    _path.clear();

    render(value, "", 0);
    text("\n");
    flush();
}

void DebugPrintCodegen::render(
    const LValue &place,
    std::string_view label,
    size_t depth,
    const AST::ValueType *display_type
)
{
    const AST::ValueType &type = place.storage_type;

    // built once rather than in each arm: get_type_desciption walks the type and allocates
    const std::string type_name = (display_type != nullptr ? *display_type : type).get_type_desciption();

    // **the order is load-bearing.** a nullable class has to reach the class arm and not the optional
    // one, and a bound string has to be recognised before the struct it actually is

    // defensive: the type checker refuses a void argument with a location first, so this is only
    // reachable through a property whose type never settled
    if (type.is_void()) {
        text(fmt::format("[void] {}<void>", label));
        return;
    }

    if (type.is_wrapped_optional()) {
        render_optional(place, type, type_name, label, depth);
        return;
    }

    // the `has_value()` guard is the `--no-stdlib` cope, and it is not theoretical: `string.eco` is
    // itself compiled without a binding. unbound, a string renders as the plain struct it is
    if (_ctx.string_layout.has_value() && _ctx.core_types().is_string_like(type)) {
        text(fmt::format("[{}] {}", type_name, label));
        render_string(place, type);
        return;
    }

    // never followed. after PointerAdjuster an address really is an address, and what is behind it may
    // be anything at all - unmapped, freed, or the middle of something
    if (type.is_pointer()) {
        text(fmt::format("[{}] {}", type_name, label));
        address(_ctx.lvalues->gen_load(place, "dprint.ptr"));
        return;
    }

    // **not upgraded**, deliberately: ClassCodegen's upgrade moves the strong count, and a printer must
    // not change what it is observing. an empty weak prints its null address for free
    if (type.is_weak()) {
        text(fmt::format("[{}] {}weak(", type_name, label));
        address(_ctx.lvalues->gen_load(place, "dprint.weak"));
        text(")");
        return;
    }

    if (type.is_callable()) {
        llvm::Value *callable = _ctx.lvalues->gen_load(place, "dprint.fn");

        text(fmt::format("[{}] {}fn=", type_name, label));
        address(_ctx.builder->CreateExtractValue(callable, 0, "dprint.fn.code"));
        text(" env=");
        address(_ctx.builder->CreateExtractValue(callable, 1, "dprint.fn.env"));
        return;
    }

    if (type.is_c_function()) {
        text(fmt::format("[{}] {}fn=", type_name, label));
        address(_ctx.lvalues->gen_load(place, "dprint.cfn"));
        return;
    }

    // the concrete type is a word in the heap block at runtime, and there is no name stored beside it to
    // print. no address either, so the output stays deterministic and a golden can assert on it
    if (type.is_interface()) {
        text(fmt::format("[{}] {}<interface>", type_name, label));
        return;
    }

    if (type.is_class()) {
        render_class(place, type, type_name, label, depth);
        return;
    }

    if (type.is_enum()) {
        render_enum(place, type, type_name, label, depth);
        return;
    }

    if (type.has_property_layout()) {
        const AST::ComplexType *complex = type.get_complex_type();
        if (complex != nullptr) {
            render_properties(place, *complex, type_name, label, depth);
            return;
        }
    }

    if (type.is_primitive()) {
        text(fmt::format("[{}] {}", type_name, label));
        render_primitive(_ctx.lvalues->gen_load(place, "dprint.val"), type);
        return;
    }

    // a type parameter that reached codegen escaped monomorphization, which is a compiler bug and not
    // something a program can be written to produce
    throw _ctx.error(fmt::format(
        "'dprint' has no rendering for '{}' {}",
        type.get_type_desciption(), _ctx.function_context()));
}

const char *DebugPrintCodegen::cut_reason(const AST::ComplexType &complex, size_t depth) const
{
    // checked *before* the push, so a type is cut on its second occurrence and not its first
    for (const AST::ComplexType *seen : _path) {
        if (seen == &complex) {
            return "<cycle>";
        }
    }

    return depth >= max_depth ? "<max depth>" : nullptr;
}

void DebugPrintCodegen::render_properties(
    const LValue &aggregate,
    const AST::ComplexType &complex,
    std::string_view type_name,
    std::string_view label,
    size_t depth
)
{
    if (const char *cut = cut_reason(complex, depth)) {
        text(fmt::format("[{}] {}{}", type_name, label, cut));
        return;
    }

    const size_t count = complex.property_count();

    if (count == 0) {
        text(fmt::format("[{}] {}{{}}", type_name, label));
        return;
    }

    PathGuard guard(_path, &complex);

    const Structure &structure = _ctx.lvalues->structure_of(&complex, aggregate.storage_type);

    text(fmt::format("[{}] {}{{\n", type_name, label));

    for (size_t i = 0; i < count; i++) {
        render_property(aggregate, structure, complex, i, depth + 1);
    }

    text(fmt::format("{}}}", indent_for(depth)));
}

void DebugPrintCodegen::render_property(
    const LValue &aggregate,
    const Structure &structure,
    const AST::ComplexType &complex,
    size_t index,
    size_t depth
)
{
    const AST::ComplexType::Property &property = complex.get_property(index);
    const std::string slot_name = fmt::format("dprint.{}", property.name);

    LValue slot = _ctx.lvalues->property_place(
        structure, aggregate, property.index, property.type, slot_name.c_str());

    text(indent_for(depth));
    render(slot, fmt::format("${} = ", property.name), depth);
    text("\n");
}

void DebugPrintCodegen::render_enum(
    const LValue &place,
    const AST::ValueType &type,
    std::string_view type_name,
    std::string_view label,
    size_t depth
)
{
    const AST::ComplexType *complex = type.get_complex_type();
    if (complex == nullptr) {
        text(fmt::format("[{}] {}<opaque>", type_name, label));
        return;
    }

    if (const char *cut = cut_reason(*complex, depth)) {
        text(fmt::format("[{}] {}{}", type_name, label, cut));
        return;
    }

    (void)_ctx.types->get_llvm_type(type, *_ctx.current_cmp_unit);

    const Structure &structure = _ctx.lvalues->structure_of(complex, type);
    PathGuard guard(_path, complex);

    text(fmt::format("[{}] {}{{\n", type_name, label));
    render_property(place, structure, *complex, AST::k_enum_tag_index, depth + 1);

    bool any_payload = false;
    for (const AST::ComplexType::EnumCase &entry : complex->enum_cases()) {
        if (entry.has_payload()) {
            any_payload = true;
            break;
        }
    }

    if (!any_payload) {
        text(fmt::format("{}}}", indent_for(depth)));
        return;
    }

    const AST::ValueType tag_type = complex->get_property_type(AST::k_enum_tag_index);
    llvm::Value *tag = _ctx.lvalues->gen_load(
        _ctx.lvalues->property_place(
            structure, place, AST::k_enum_tag_index, tag_type, "dprint.enum.tag"),
        "dprint.enum.tag");

    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();
    llvm::BasicBlock *join = llvm::BasicBlock::Create(*_ctx.llvm_context, "dprint.enum.join", function);

    llvm::Type *tag_ty = _ctx.types->get_llvm_type(tag_type, *_ctx.current_cmp_unit);

    for (const AST::ComplexType::EnumCase &entry : complex->enum_cases()) {
        if (!entry.has_payload()) {
            continue;
        }

        llvm::BasicBlock *arm = llvm::BasicBlock::Create(*_ctx.llvm_context, "dprint.enum.arm", function);
        llvm::BasicBlock *next = llvm::BasicBlock::Create(*_ctx.llvm_context, "dprint.enum.next", function);

        flush();
        llvm::Value *want = llvm::ConstantInt::get(
            tag_ty, static_cast<uint64_t>(entry.discriminant), /*isSigned=*/true);
        _ctx.builder->CreateCondBr(
            _ctx.builder->CreateICmpEQ(tag, want, "dprint.enum.hit"), arm, next);

        _ctx.set_insert_point(arm);
        _pending_block = nullptr;

        for (size_t i = 0; i < entry.payload_field_count; i++) {
            render_property(
                place, structure, *complex, entry.first_payload_property + i, depth + 1);
        }

        close_arm(join);

        _ctx.set_insert_point(next);
        _pending_block = nullptr;
    }

    close_arm(join);
    _ctx.set_insert_point(join);
    _pending_block = nullptr;

    text(fmt::format("{}}}", indent_for(depth)));
}

void DebugPrintCodegen::render_class(
    const LValue &place,
    const AST::ValueType &type,
    std::string_view type_name,
    std::string_view label,
    size_t depth
)
{
    AST::ComplexType *complex = type.get_complex_type();

    // the cycle and depth cuts are answered before the branch is opened: a handle we are not going to
    // expand needs no null test, and emitting one would leave two empty arms in the IR. the cuts
    // themselves are render_properties', so the two arms cannot disagree about when to stop descending
    if (complex == nullptr) {
        text(fmt::format("[{}] {}<opaque>", type_name, label));
        return;
    }

    if (const char *cut = cut_reason(*complex, depth)) {
        text(fmt::format("[{}] {}{}", type_name, label, cut));
        return;
    }

    const ClassLayout layout = _ctx.types->get_or_create_class_layout(complex, *_ctx.current_cmp_unit);

    // **no retain.** the payload is read in place through the handle, so ref_count($x) reads the same on
    // either side of a dprint($x) - which the corpus pins
    llvm::Value *handle = _ctx.lvalues->gen_load(place, "dprint.obj");
    llvm::Value *is_null = _ctx.builder->CreateIsNull(handle, "dprint.isnull");

    llvm::BasicBlock *value_block = nullptr;
    llvm::BasicBlock *join = open_branch(is_null, "dprint.null", value_block);

    // the null arm
    text(fmt::format("[{}] {}null", type_name, label));
    close_arm(join);

    // the payload arm
    _ctx.set_insert_point(value_block);
    _pending_block = nullptr;

    llvm::Value *payload = _ctx.builder->CreateStructGEP(
        layout.box, handle, ClassBox::payload_index, "dprint.payload");

    LValue payload_place { payload, type, place.provenance };
    render_properties(payload_place, *complex, type_name, label, depth);
    close_arm(join);

    _ctx.set_insert_point(join);
    _pending_block = nullptr;
}

void DebugPrintCodegen::render_optional(
    const LValue &place,
    const AST::ValueType &type,
    std::string_view type_name,
    std::string_view label,
    size_t depth
)
{
    llvm::Type *box = _ctx.types->get_llvm_type(type, *_ctx.current_cmp_unit);

    // **through TypeLowering::gen_has_value like the other four askers**, not a hand-rolled tag load.
    // having an address rather than a value is a reason to load the wrapper first, not a reason to
    // re-derive how the two shapes of a `T?` are told apart
    llvm::Value *has = _ctx.types->gen_has_value(
        _ctx.builder->CreateLoad(box, place.address, "dprint.opt"), type);

    llvm::BasicBlock *some_block = nullptr;
    llvm::BasicBlock *join = open_branch(_ctx.builder->CreateNot(has, "dprint.absent"), "dprint.none", some_block);

    // the absent arm. the type is still printed, because "which optional was empty" is most of what the
    // reader is here for
    text(fmt::format("[{}] {}null", type_name, label));
    close_arm(join);

    // the present arm reads the payload as a plain `T` - the tag is stripped and the value GEP'd out -
    // but prints under the optional's own name, or nothing in the output would distinguish an `int32?`
    // holding 12 from an `int32` holding 12
    _ctx.set_insert_point(some_block);
    _pending_block = nullptr;

    llvm::Value *value_address = _ctx.builder->CreateStructGEP(
        box, place.address, AST::k_optional_value_index, "dprint.some.ptr");

    render(LValue{value_address, AST::unwrapped_type_of(type)}, label, depth, &type);
    close_arm(join);

    _ctx.set_insert_point(join);
    _pending_block = nullptr;
}

void DebugPrintCodegen::render_string(const LValue &place, const AST::ValueType &type)
{
    const auto [bytes, size] = _ctx.gen_string_window(
        _ctx.lvalues->gen_load(place, "dprint.str"), type, "dprint.str.");

    // **`%.*s` is why this can share the surrounding printf.** C requires `%s` with a precision to read
    // at most that many bytes from an array that need not be NUL-terminated, which is exactly the shape
    // an Echo string has - a substring shares its owner's buffer and simply stops early. so unlike
    // `echo`, which pays fflush + write(2) per string, a string here is two more varargs in the frame
    //
    // one deviation worth knowing: `%.*s` also stops at an embedded NUL, where write(2) would not. the
    // right trade for a debug printer whose value is one call per frame
    //
    // the precision argument is an `int`, so the usize is narrowed through the one conversion table
    llvm::Value *precision = _ctx.types->coerce_value(
        size, AST::ValueType(AST::ValueTypePrimitive::t_usize),
        AST::ValueType(AST::ValueTypePrimitive::t_int32), *_ctx.current_cmp_unit);

    text("\"");
    _pending_args.push_back(precision);
    _pending_args.push_back(bytes);
    _pending_format += "%.*s";
    text("\"");
}

void DebugPrintCodegen::render_primitive(llvm::Value *value, const AST::ValueType &type)
{
    // **a bool selects between two literal pointers rather than switching the format.** the format has to
    // stay one compile-time constant for the whole frame, or the buffer could not merge across it
    if (type.is_boolean_type()) {
        llvm::Value *yes = _ctx.builder->CreateGlobalStringPtr("true", "dprint.true");
        llvm::Value *no = _ctx.builder->CreateGlobalStringPtr("false", "dprint.false");

        arg("%s", _ctx.builder->CreateSelect(value, yes, no, "dprint.bool"));
        return;
    }

    PrintfConversion conversion = printf_conversion_for(type);

    if (conversion.format == nullptr) {
        throw _ctx.error(fmt::format(
            "'dprint' has no conversion for '{}' {}",
            type.get_type_desciption(), _ctx.function_context()));
    }

    // the two rows `dprint` overrides. the *promotion* is shared - both floats reach printf as a double
    // either way - and only the presentation differs, which is this renderer's to choose
    //
    // `%g` at 6 significant digits is exactly FLT_DIG, so 42.69 prints as `42.69` where echo's `%f`
    // gives `42.690000`. `%.15g` is DBL_DIG, the most digits always exact, so 0.1 prints as `0.1`;
    // `%.17g` would round-trip but renders 42.69 as `42.689999999999998`, which no reader wants
    if (type.is_primitive_of_type(AST::ValueTypePrimitive::t_float32)) {
        conversion.format = "%g";
    } else if (type.is_primitive_of_type(AST::ValueTypePrimitive::t_float64)) {
        conversion.format = "%.15g";
    }

    // through the one conversion table, which takes the extend from the source's signedness and hands an
    // already-wide value straight back
    arg(conversion.format, _ctx.types->coerce_value(
        value, type, AST::ValueType(conversion.promoted), *_ctx.current_cmp_unit));
}

void DebugPrintCodegen::text(std::string_view literal)
{
    if (_pending_block == nullptr) {
        _pending_block = _ctx.builder->GetInsertBlock();
    }

    assert(_pending_block == _ctx.builder->GetInsertBlock()
        && "a dprint buffer crossed a block boundary - its text would land in the wrong arm");

    for (char c : literal) {
        if (c == '%') {
            _pending_format += '%';
        }
        _pending_format += c;
    }
}

void DebugPrintCodegen::arg(std::string_view conversion, llvm::Value *value)
{
    if (_pending_block == nullptr) {
        _pending_block = _ctx.builder->GetInsertBlock();
    }

    assert(_pending_block == _ctx.builder->GetInsertBlock()
        && "a dprint buffer crossed a block boundary - its argument would land in the wrong call");

    _pending_format += conversion;
    _pending_args.push_back(value);

    if (_pending_args.size() >= max_pending_args) {
        flush();
    }
}

void DebugPrintCodegen::address(llvm::Value *pointer)
{
    // `%llx` and an i64 rather than `%p` and the pointer: the width is then the compiler's to state
    // rather than the varargs ABI's to guess, and every host this targets has a 64-bit address
    arg("0x%llx", _ctx.builder->CreatePtrToInt(
        pointer, llvm::Type::getInt64Ty(*_ctx.llvm_context), "dprint.addr"));
}

void DebugPrintCodegen::flush()
{
    if (_pending_format.empty() && _pending_args.empty()) {
        return;
    }

    std::vector<llvm::Value *> args;
    args.reserve(_pending_args.size() + 1);
    args.push_back(_ctx.builder->CreateGlobalStringPtr(_pending_format, "dprint.fmt"));
    args.insert(args.end(), _pending_args.begin(), _pending_args.end());

    _ctx.builder->CreateCall(_ctx.current_module()->getFunction("printf"), args);

    _pending_format.clear();
    _pending_args.clear();
    _pending_block = nullptr;
}

llvm::BasicBlock *DebugPrintCodegen::open_branch(
    llvm::Value *condition,
    const char *label,
    llvm::BasicBlock *&on_false_out
)
{
    // whatever is buffered belongs to the block we are still in, and a printf cannot straddle a branch
    flush();

    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();

    llvm::BasicBlock *on_true = llvm::BasicBlock::Create(*_ctx.llvm_context, label, function);
    on_false_out = llvm::BasicBlock::Create(*_ctx.llvm_context, "dprint.value", function);
    llvm::BasicBlock *join = llvm::BasicBlock::Create(*_ctx.llvm_context, "dprint.join", function);

    _ctx.builder->CreateCondBr(condition, on_true, on_false_out);

    _ctx.set_insert_point(on_true);
    _pending_block = nullptr;

    return join;
}

void DebugPrintCodegen::close_arm(llvm::BasicBlock *join)
{
    flush();

    // **re-read the current block rather than remembering the one the arm started in**: a nested class
    // property opens its own branch, so an arm often ends somewhere other than where it began
    //
    // nothing inside a dprint frame can terminate a block today - there is no `die`, no `return`, and no
    // call that does not return - but the guard is the contract every emitter that can follow a
    // terminator owes, and without it the day one does the failure is a verifier error far from the cause
    if (!_ctx.block_is_terminated()) {
        _ctx.builder->CreateBr(join);
    }
}

std::string_view DebugPrintCodegen::indent_for(size_t depth)
{
    static const std::string spaces(max_depth * 2 + 2, ' ');

    const size_t width = std::min(depth * 2, spaces.size());
    return std::string_view(spaces).substr(0, width);
}

};
