#include <catch2/catch_test_macros.hpp>

#include <Compiler/Lsp/LspTransport.h>

#include <iostream>
#include <sstream>
#include <string>

TEST_CASE("a framed message round-trips through a stringstream", "[lsp]")
{
    std::ostringstream out;
    Compiler::Lsp::Transport writer(std::cin, out);
    writer.write_message("{\"jsonrpc\":\"2.0\"}");

    const std::string framed = out.str();
    REQUIRE(framed.rfind("Content-Length:", 0) == 0);
    REQUIRE(framed.find("\r\n\r\n") != std::string::npos);
    REQUIRE(framed.substr(framed.find('{')) == "{\"jsonrpc\":\"2.0\"}");

    std::istringstream in(framed);
    std::ostringstream unused;
    Compiler::Lsp::Transport reader(in, unused);

    const auto frame = reader.read_message();
    REQUIRE(frame.kind == Compiler::Lsp::Transport::FrameKind::t_message);
    REQUIRE(frame.body == "{\"jsonrpc\":\"2.0\"}");
}

TEST_CASE("a header name is matched case-insensitively", "[lsp]")
{
    std::istringstream in("content-length: 2\r\n\r\n{}");
    std::ostringstream unused;
    Compiler::Lsp::Transport reader(in, unused);

    const auto frame = reader.read_message();
    REQUIRE(frame.kind == Compiler::Lsp::Transport::FrameKind::t_message);
    REQUIRE(frame.body == "{}");
}

TEST_CASE("EOF before a frame is a missing message, not a throw", "[lsp]")
{
    std::istringstream in("");
    std::ostringstream unused;
    Compiler::Lsp::Transport reader(in, unused);

    REQUIRE(reader.read_message().kind == Compiler::Lsp::Transport::FrameKind::t_eof);
}

TEST_CASE("a header block with no Content-Length is this frame only", "[lsp]")
{
    std::istringstream in(
        "Content-Type: application/vscode-jsonrpc\r\n\r\n"
        "Content-Length: 2\r\n\r\n{}");
    std::ostringstream unused;
    Compiler::Lsp::Transport reader(in, unused);

    REQUIRE(reader.read_message().kind == Compiler::Lsp::Transport::FrameKind::t_invalid);

    const auto frame = reader.read_message();
    REQUIRE(frame.kind == Compiler::Lsp::Transport::FrameKind::t_message);
    REQUIRE(frame.body == "{}");
}

TEST_CASE("a non-numeric Content-Length is this frame only", "[lsp]")
{
    std::istringstream in(
        "Content-Length: no\r\n\r\n"
        "Content-Length: 2\r\n\r\n{}");
    std::ostringstream unused;
    Compiler::Lsp::Transport reader(in, unused);

    REQUIRE(reader.read_message().kind == Compiler::Lsp::Transport::FrameKind::t_invalid);

    const auto frame = reader.read_message();
    REQUIRE(frame.kind == Compiler::Lsp::Transport::FrameKind::t_message);
    REQUIRE(frame.body == "{}");
}

TEST_CASE("input_pending is true when the stream already holds bytes", "[lsp]")
{
    std::istringstream in("Content-Length: 2\r\n\r\n{}");
    std::ostringstream unused;
    Compiler::Lsp::Transport reader(in, unused);

    REQUIRE(reader.input_pending(0));
}
