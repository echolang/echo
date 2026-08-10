#ifndef ASTATTRIBUTEREADER_H
#define ASTATTRIBUTEREADER_H

#pragma once

#include "AST/ASTAttributeValue.h"

#include <fmt/core.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace AST
{
    // one refusal: a sentence, and the tokens it is about
    struct AttributeRefusal
    {
        std::string message;
        TokenSpan span;
    };

    // **how a consumer refuses an attribute value** - the single owner, replacing the one-string reader
    // and the hand-written node-type tests that had grown around each of its callers.
    //
    // It **accumulates** refusals rather than reporting them, because there are two channels and the
    // reader must not have to know which it is in: a source file pushes an AST::Issue::GenericError
    // through the collector, and a manifest still hand-formats `<file>:<line>: <what>` into its own
    // error string. Carrying a TokenSpan on every refusal is also what hands the manifest a span the
    // day it starts reporting like the rest of the compiler.
    //
    // **Two readings of a record, and the consumer says which.** The grammar produces one `{ ... }`;
    // what differs is who owns the key set. A *fixed* record has a closed vocabulary the consumer
    // knows - `git { url:, rev: }` - and anything else in it is refused by reject_unknown_fields,
    // which is the format's "understood or an error, never a no-op" rule applied per key. An *open*
    // record's keys are the payload - `define { GLAD_GL_IMPLEMENTATION: 1 }` - and the consumer walks
    // `fields` directly. Neither is a mode on the value, so nothing has to be annotated to say which
    // kind of record it is
    class AttributeReader
    {
    public:
        AttributeReader(const std::string &attribute_name) :
            _attribute_name(attribute_name)
        {};
        ~AttributeReader() {};

        // **anywhere one value is accepted, a list of them is accepted.** one rule here rather than a
        // decision per attribute: a value that is not a list reads as a list holding it, so no consumer
        // grows an arm for `#[sources: ["a", "b"]]` beside `#[sources: "a"]`
        //
        // an **untagged** list only, because a tagged one is a single tagged thing whose payload
        // happens to be a list - peeling `lib ["GL", "GLU"]` here would hand back two values with no
        // scheme between them
        static std::vector<const AttributeValue *> each(const AttributeValue &value);

        // the same fan-out one level in, after a tag has been read off - so `lib ["GL", "GLU"]` is one
        // scheme and two libraries. the tag is the caller's by then, which is what makes peeling safe
        static std::vector<const AttributeValue *> payload(const AttributeValue &value);

        std::optional<std::string> string(const AttributeValue &value);
        std::optional<std::string> name(const AttributeValue &value);
        std::optional<int64_t> integer(const AttributeValue &value);

        // free text, however it was spelled - a string, a bare name, or a number. what `define`'s
        // record values go through, where `{ WIDTH: 40 }`, `{ WIDTH: "40" }` and `{ WIDTH: OTHER }`
        // all mean the same word on a command line. a bool is refused, because C has no spelling for
        // one and `-DX=true` is a compile error in somebody else's file
        std::optional<std::string> word(const AttributeValue &value);

        // the tag against a closed vocabulary, shaped like Compiler::split_scheme so the two spellings
        // of a scheme - `lib "GL"` in a manifest, `lib:GL` on a command line - refuse in the same words
        template <class Scheme>
        bool tag(
            const AttributeValue &value,
            const std::vector<std::pair<std::string, Scheme>> &table,
            const std::string &noun,
            const std::string &noun_list,
            Scheme &out_scheme
        )
        {
            if (!value.has_tag()) {
                refuse(value.span, fmt::format(
                    "'{}' does not name a {} - write '<{}> <value>', where it is one of: {}.",
                    attribute_value_spelling(value), noun, noun, noun_list));
                return false;
            }

            for (const auto &[candidate, scheme] : table) {
                if (candidate != value.tag()) {
                    continue;
                }

                out_scheme = scheme;
                return true;
            }

            refuse(value.tag_span, fmt::format(
                "'{}' is not a {}, expected one of: {}.", value.tag(), noun, noun_list));
            return false;
        }

        // a fixed record: the value has to *be* a record, then each key is pulled by name
        bool record(const AttributeValue &value);
        const AttributeValue *field(const AttributeValue &value, const std::string &key);
        const AttributeValue *require_field(const AttributeValue &value, const std::string &key);

        // everything in the record that `known` does not name, refused at the key that is not known.
        // the rule that anything the format does not understand is an error, applied per key
        void reject_unknown_fields(const AttributeValue &value, const std::vector<std::string> &known);

        void refuse(const TokenSpan &span, const std::string &message);

        bool has_refusals() const {
            return !_refusals.empty();
        }

        const std::vector<AttributeRefusal> &refusals() const {
            return _refusals;
        }

        const std::string &attribute_name() const {
            return _attribute_name;
        }

    private:
        // the refusal every wrong-shape answer above shares - "the 'core' attribute wants a name, not
        // a string." written once, so a kind added to AttributeValueKind needs no second wording
        void wrong_shape(const AttributeValue &value, AttributeValueKind wanted);

        std::string _attribute_name;
        std::vector<AttributeRefusal> _refusals;
    };
};

#endif
