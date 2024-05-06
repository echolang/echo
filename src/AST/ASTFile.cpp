#include "AST/ASTFile.h"

#include <iostream>
#include <fstream>
#include <sstream>

void AST::File::set_content(const std::string &content)
{
    this->content = content;
    _line_offsets.clear();
    _line_offsets.push_back(0);
    for (size_t i = 0; i < content.size(); i++) {
        if (content[i] == '\n') {
            _line_offsets.push_back(i + 1);
        }
    }
}

void AST::File::read_from_disk() 
{
    // load the file into a string
    // we probably should use a stream in the future
    auto istrm = std::ifstream(_path);
    auto stream = std::stringstream();

    // if the first line is just "<?php" or "<?eco" we skip it
    // this is a TEMPORARY hack so my dump text editor will do syntax highlighting
    // without having to create a syntax highlighting extension..
    stream << istrm.rdbuf();
    auto str = stream.str();
    if (str.substr(0, 5) == "<?php" || str.substr(0, 5) == "<?eco") {
        stream.str(str.substr(5));
    }
    // end of hack

    // update the content
    set_content(stream.str());
}

std::string AST::File::debug_description() const
{
    return root->node_description();
}

std::string AST::File::get_content_of_line(uint32_t line) const
{
    if (!content.has_value()) {
        return "";
    }

    // our line numbers start at 1
    // but our offset array starts at 0
    if (line != 0) {
        line--;
    } else {
        return "";
    }

    if (line >= _line_offsets.size()) {
        return "";
    }

    size_t start = _line_offsets[line];
    size_t end = content.value().size();
    if (line + 1 < _line_offsets.size()) {
        end = _line_offsets[line + 1] - 1;
    }

    return content.value().substr(start, end - start);
}