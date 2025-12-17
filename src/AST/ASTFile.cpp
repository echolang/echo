#include "AST/ASTFile.h"

#include <fmt/core.h>

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

void AST::File::set_content(const char *content, size_t length)
{
    set_content(
        std::string(static_cast<const char*>(content), static_cast<std::string::size_type>(length))
    );
}

void AST::File::read_from_disk()
{
    std::string error;

    // the caller did not ask to be told, so an unreadable file stays an empty one - see the overload
    // below for why that is worth an answer at all
    read_from_disk(error);
}

bool AST::File::read_from_disk(std::string &out_error)
{
    // load the file into a string
    // we probably should use a stream in the future
    auto istrm = std::ifstream(_path);

    // **an unreadable file used to parse as an empty one, silently.** The stream was never checked, so a
    // path that is not there - or is there and cannot be opened - produced a file with no declarations,
    // and the module simply lost everything that was supposed to be in it. Nothing downstream can tell
    // that from a genuinely empty file.
    //
    // it was survivable while every path came from a command line the user had just typed, because the
    // driver checked existence first. It stops being survivable once a *file list* comes from a manifest:
    // a pattern that matched a file which has since been deleted is an ordinary occurrence then, and a
    // build that quietly drops a source is a build that links against nothing
    if (!istrm) {
        out_error = fmt::format("{}: could not be opened for reading.", _path.string());
        return false;
    }

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

    return true;
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
