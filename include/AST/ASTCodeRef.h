#ifndef ASTCODEREF_H
#define ASTCODEREF_H

#pragma once

#include "ASTModule.h"

#include <tuple>

namespace AST
{  
    struct CodeRef
    {
        const Module *module;
        const TokenizedFile *file;
        const TokenCollection::Slice token_slice;

        std::tuple<uint32_t, uint32_t> line_range() const
        {
            return std::make_tuple(token_slice.startt().line, token_slice.endt().line);
        }

        std::tuple<uint32_t, uint32_t> char_offset_range() const
        {
            return std::make_tuple(token_slice.startt().char_offset, token_slice.endt().char_offset);
        }

        bool is_single_line() const
        {
            return token_slice.startt().line == token_slice.endt().line;
        }

        bool is_single_token() const
        {
            return token_slice.startt().char_offset == token_slice.endt().char_offset;
        }

        const std::string get_referenced_code_excerpt() const
        {
            if (!file->file->content.has_value()) {
                return "[No content available]";
            }

            const auto &content = file->file->content.value();

            auto lines = line_range();

            std::string excerpt;
            excerpt += "Begin: '" + token_slice.start_ref().value() + "'\n";
            excerpt += "End: '" + token_slice.end_ref().value() + "'\n";
            excerpt += "Code excerpt:\n";

            for (uint32_t i = std::get<0>(lines) - 1; i <= std::get<1>(lines) + 1; i++) {
                excerpt += " [" + std::to_string(i) + "]> " + file->file->get_content_of_line(i) + "\n";
                if (i == std::get<0>(lines)) {
                    excerpt += "     ";
                    for (uint32_t j = 0; j < token_slice.startt().char_offset; j++) {
                        excerpt += " ";
                    }
                    excerpt += "^";
                    excerpt += "\n";
                }
            }

            return excerpt;
        }
    };
};

#endif