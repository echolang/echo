#ifndef LSPCOMPLETIONCONTEXT_H
#define LSPCOMPLETIONCONTEXT_H

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Compiler
{
    namespace Lsp
    {
        // what the cursor is in the middle of, read off the editor's text.
        //
        // `$p->` is still being typed, so recovery skips it and the snapshot has no node for it.
        // this half lexes the live text (the real lexer throws on unterminated input) and says
        // which question is being asked. LspCompletion answers it from the snapshot.
        // pure: text in, context out

        enum class LexKind
        {
            t_identifier,
            t_variable,
            t_number,
            t_string,
            t_punct
        };

        struct LexToken
        {
            LexKind kind = LexKind::t_punct;
            std::string text;
            size_t offset = 0;
        };

        // a variable as the text declares it. `offset` is the `$`, which is also where the snapshot
        // records the declaration's token: the key from a lexical declaration to its type
        struct LexicalVariable
        {
            std::string name;
            size_t offset = 0;

            // `Point` of `Point $p = ...`, empty for `$p = ...`
            std::string written_type;

            // what follows the `=`, up to the end of the statement. read for a type when the snapshot
            // has none, because the line was typed after the last compile
            std::vector<LexToken> initializer;
            bool is_parameter = false;
        };

        enum class FrameKind
        {
            t_block,

            // a named function, a method or a test. lookup of `$` stops at this frame
            t_function,

            // a closure literal. it captures what is outside, so lookup goes on through it
            t_closure,

            // a struct, class, enum or interface body
            t_type
        };

        // one open `{` at the cursor
        struct LexicalFrame
        {
            FrameKind kind = FrameKind::t_block;
            size_t brace_offset = 0;
            std::vector<LexicalVariable> variables;

            // t_type: the type's name. t_function: the function's name, empty for a constructor
            std::string name;
            size_t name_offset = 0;

            // t_function: true when the method was written `static`
            bool is_static = false;
        };

        enum class CompletionKind
        {
            // in a comment, in a string, or past a token that starts no list
            t_none,

            // `$na|`
            t_variable,

            // `$p->na|`, `$p?->na|`
            t_member,

            // `Type::na|`, `ns::na|`, `Type::$na|`
            t_static,

            // `use std::|`, `use a::{x, |`
            t_use_path,

            // `.ki|` where a destination names the type: `return .ok`, `Unit $u = .meter`
            t_shorthand,

            // `#[in|`
            t_attribute,

            // a bare name: a keyword, a function, a type, a namespace
            t_identifier
        };

        // where a bare name is being written, which keywords make sense there
        enum class NamePosition
        {
            t_file_root,
            t_type_body,
            t_statement,
            t_expression,
            t_after_expression,
            t_type
        };

        // one link of a `$a->b()->c` chain, root first
        struct ReceiverSegment
        {
            enum class Kind
            {
                // `$a`, the root
                t_variable,

                // `->b`
                t_property,

                // `->b(...)`
                t_method,

                // `[...]`
                t_index,

                // `f(...)` or `Point(...)`, the root
                t_call,

                // `a::f(...)` or `Type::make(...)`, the root
                t_static_call,

                // `Unit::meter`, the root
                t_static_value
            };

            Kind kind = Kind::t_variable;
            std::string name;
            std::vector<std::string> path;
        };

        struct CompletionContext
        {
            CompletionKind kind = CompletionKind::t_none;
            NamePosition position = NamePosition::t_expression;

            // what is typed of the name so far, `$` included for a variable
            std::string prefix;

            // where that name starts and where the identifier under the cursor ends. a completion
            // replaces from start to the cursor, or to the end when the editor offers to replace
            size_t replace_start = 0;
            size_t replace_end = 0;

            // t_static / t_use_path: the segments before the last `::`
            std::vector<std::string> path;

            // t_member: the receiver, root first
            std::vector<ReceiverSegment> receiver;
            bool optional_chain = false;

            // t_shorthand: the declaration's written type, or the place the value is assigned to,
            // compared with, or defaulted after (`??`), or the enclosing function's return. a call
            // argument leaves this empty: its destination is the parameter, which the text has no
            // name for
            std::string destination_type;
            std::vector<ReceiverSegment> destination_chain;
            bool destination_is_return = false;

            // the open braces at the cursor, outermost (the file) first
            std::vector<LexicalFrame> frames;
        };

        CompletionContext analyze_completion_context(std::string_view text, size_t cursor);

        // the variables a `$` at the cursor can name, innermost first. a function body stops the
        // walk; a closure keeps going. `$this` lives on this_frame, the enclosing type's
        std::vector<LexicalVariable> visible_variables(const CompletionContext &context);

        // the type body whose method the cursor is in, when that method has a `$this`. null otherwise
        const LexicalFrame *this_frame(const CompletionContext &context);

        // the type body the cursor is anywhere inside, static members included. null at file scope
        const LexicalFrame *enclosing_type_frame(const CompletionContext &context);

        // the named function the cursor is in, for a `return .x` destination. null at file scope
        const LexicalFrame *enclosing_function_frame(const CompletionContext &context);

        // a `$a->b()->c` chain read backwards from the end of `tokens`
        std::vector<ReceiverSegment> receiver_chain_of(const std::vector<LexToken> &tokens);

        // the lexer itself, exposed for the tests. `cursor_in_code` is false when `end` lands inside
        // a comment or a string (outside an interpolation hole)
        std::vector<LexToken> lex_echo(std::string_view text, size_t end, bool &cursor_in_code);
    };
};

#endif
