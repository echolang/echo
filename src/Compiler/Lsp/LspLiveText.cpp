#include "Compiler/Lsp/LspLiveText.h"

#include "AST/ASTFile.h"
#include "Compiler/Lsp/LspSession.h"

#include <algorithm>

Compiler::Lsp::LiveText::LiveText(std::string text) :
    _text(std::move(text))
{
    _line_offsets.push_back(0);
    for (size_t i = 0; i < _text.size(); i++) {
        if (_text[i] == '\n') {
            _line_offsets.push_back(i + 1);
        }
    }
}

std::string_view Compiler::Lsp::LiveText::line(uint32_t echo_line) const
{
    if (echo_line == 0 || echo_line > _line_offsets.size()) {
        return {};
    }

    const size_t start = _line_offsets[echo_line - 1];
    const size_t end = echo_line < _line_offsets.size()
        ? _line_offsets[echo_line] - 1
        : _text.size();

    return std::string_view(_text).substr(start, end - start);
}

size_t Compiler::Lsp::LiveText::offset_of(AST::Location location) const
{
    if (location.line == 0) {
        return 0;
    }

    if (location.line > _line_offsets.size()) {
        return _text.size();
    }

    const size_t start = _line_offsets[location.line - 1];
    const size_t column = location.column == 0 ? 0 : location.column - 1;
    return std::min(start + std::min(column, line(location.line).size()), _text.size());
}

AST::Location Compiler::Lsp::LiveText::location_of(size_t offset) const
{
    offset = std::min(offset, _text.size());
    auto it = std::upper_bound(_line_offsets.begin(), _line_offsets.end(), offset);
    const size_t line = static_cast<size_t>(it - _line_offsets.begin());
    return AST::Location{
        static_cast<uint32_t>(line),
        static_cast<uint32_t>(offset - _line_offsets[line - 1] + 1) };
}

AST::Location Compiler::Lsp::live_location_of(
    const LiveText &live,
    Position position,
    bool utf8_encoding
)
{
    AST::Location location;
    location.line = position.line + 1;

    const uint32_t byte_column = utf8_encoding
        ? position.character
        : byte_column_of_utf16(live.line(location.line), position.character);
    location.column = byte_column + 1;
    return location;
}

Compiler::Lsp::Position Compiler::Lsp::live_position_of(
    const LiveText &live,
    AST::Location location,
    bool utf8_encoding
)
{
    Position position;
    position.line = location.line == 0 ? 0 : location.line - 1;

    const uint32_t byte_column = location.column == 0 ? 0 : location.column - 1;
    position.character = utf8_encoding
        ? byte_column
        : utf16_column_of(live.line(location.line), byte_column);
    return position;
}

Compiler::Lsp::LineMap Compiler::Lsp::LineMap::build(const AST::File &compiled, const LiveText &live)
{
    LineMap map;
    map._live_lines = static_cast<uint32_t>(live.line_count());

    // a file with no content has nothing to line up against; every live line is new
    if (!compiled.content.has_value()) {
        return map;
    }

    if (compiled.content.value() == live.text()) {
        return identity();
    }

    map._snapshot_lines = static_cast<uint32_t>(compiled.line_count());
    const uint32_t shorter = std::min(map._live_lines, map._snapshot_lines);

    while (map._prefix < shorter
        && live.line(map._prefix + 1) == compiled.get_content_of_line(map._prefix + 1)) {
        map._prefix++;
    }

    while (map._suffix < shorter - map._prefix
        && live.line(map._live_lines - map._suffix)
            == compiled.get_content_of_line(map._snapshot_lines - map._suffix)) {
        map._suffix++;
    }

    return map;
}

Compiler::Lsp::LineMap Compiler::Lsp::LineMap::identity()
{
    LineMap map;
    map._identity = true;
    return map;
}

std::optional<uint32_t> Compiler::Lsp::LineMap::to_snapshot(uint32_t live_line) const
{
    if (_identity) {
        return live_line;
    }

    if (live_line == 0) {
        return std::nullopt;
    }

    if (live_line <= _prefix) {
        return live_line;
    }

    if (live_line > _live_lines - _suffix && live_line <= _live_lines) {
        return live_line - _live_lines + _snapshot_lines;
    }

    return std::nullopt;
}

std::optional<uint32_t> Compiler::Lsp::LineMap::to_live(uint32_t snapshot_line) const
{
    if (_identity) {
        return snapshot_line;
    }

    if (snapshot_line == 0) {
        return std::nullopt;
    }

    if (snapshot_line <= _prefix) {
        return snapshot_line;
    }

    if (snapshot_line > _snapshot_lines - _suffix && snapshot_line <= _snapshot_lines) {
        return snapshot_line - _snapshot_lines + _live_lines;
    }

    return std::nullopt;
}

std::optional<AST::Location> Compiler::Lsp::LineMap::location_to_live(AST::Location snapshot_location) const
{
    const std::optional<uint32_t> line = to_live(snapshot_location.line);
    if (!line.has_value()) {
        return std::nullopt;
    }

    return AST::Location{ line.value(), snapshot_location.column };
}

std::optional<AST::Location> Compiler::Lsp::LineMap::location_to_snapshot(AST::Location live_location) const
{
    const std::optional<uint32_t> line = to_snapshot(live_location.line);
    if (!line.has_value()) {
        return std::nullopt;
    }

    return AST::Location{ line.value(), live_location.column };
}

Compiler::Lsp::LiveViews::LiveViews(const Session &session) :
    _session(session)
{}

const Compiler::Lsp::LiveViews::View &Compiler::Lsp::LiveViews::view_of(const AST::File &file)
{
    auto found = _views.find(&file);
    if (found != _views.end()) {
        return found->second;
    }

    View view;
    if (const Document *open = _session.document(file.get_path())) {
        view.live.emplace(open->content);
        view.map = LineMap::build(file, view.live.value());
    }

    return _views.emplace(&file, std::move(view)).first->second;
}

std::optional<AST::Location> Compiler::Lsp::snapshot_location(
    LiveViews &views,
    const AST::File &file,
    Position position,
    bool utf8
)
{
    const LiveViews::View &view = views.view_of(file);
    if (!view.live.has_value()) {
        return echo_location_of(file, position, utf8);
    }

    return view.map.location_to_snapshot(live_location_of(view.live.value(), position, utf8));
}
