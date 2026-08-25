#include <catch2/catch_test_macros.hpp>

#include <Compiler/Lsp/LspTransport.h>

#include <chrono>
#include <sstream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef ECHOC_BINARY
#define ECHOC_BINARY "echoc"
#endif

#if defined(__unix__) || defined(__APPLE__)

namespace
{
    std::string frame(const std::string &body)
    {
        return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }

    bool well_formed_frames(const std::string &stdout_text)
    {
        std::istringstream in(stdout_text);
        std::ostringstream unused;
        Compiler::Lsp::Transport reader(in, unused);

        size_t count = 0;
        while (true) {
            const auto frame = reader.read_message();
            if (frame.kind == Compiler::Lsp::Transport::FrameKind::t_eof) {
                break;
            }

            if (frame.kind != Compiler::Lsp::Transport::FrameKind::t_message
                || frame.body.find("jsonrpc") == std::string::npos) {
                return false;
            }

            count++;
        }

        return count > 0;
    }

    std::vector<std::string> diagnostic_payloads(const std::string &stdout_text)
    {
        std::istringstream in(stdout_text);
        std::ostringstream unused;
        Compiler::Lsp::Transport reader(in, unused);
        std::vector<std::string> out;

        while (true) {
            const auto next = reader.read_message();
            if (next.kind == Compiler::Lsp::Transport::FrameKind::t_eof) {
                break;
            }

            if (next.kind == Compiler::Lsp::Transport::FrameKind::t_message
                && next.body.find("textDocument/publishDiagnostics") != std::string::npos) {
                out.push_back(next.body);
            }
        }

        return out;
    }

    bool read_more(int fd, std::string &captured, int timeout_ms)
    {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        const int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc <= 0) {
            return false;
        }

        char buffer[4096];
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n <= 0) {
            return false;
        }

        captured.append(buffer, static_cast<size_t>(n));
        return true;
    }

    bool wait_until(int fd, std::string &captured, int timeout_ms, bool (*ok)(const std::string &))
    {
        const auto started = std::chrono::steady_clock::now();
        while (!ok(captured)) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            const int left = timeout_ms - static_cast<int>(elapsed);
            if (left <= 0) {
                return false;
            }

            if (!read_more(fd, captured, left)) {
                return ok(captured);
            }
        }

        return true;
    }
};

TEST_CASE("echoc lsp speaks framed JSON-RPC on stdout and nothing else", "[lsp]")
{
    int to_child[2];
    int from_child[2];
    REQUIRE(pipe(to_child) == 0);
    REQUIRE(pipe(from_child) == 0);

    const pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        execl(ECHOC_BINARY, "echoc", "lsp", "--no-stdlib", static_cast<char *>(nullptr));
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);

    const std::string initialize = frame(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    const std::string initialized = frame(
        "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
    const std::string shutdown = frame(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\",\"params\":null}");
    const std::string exit_msg = frame(
        "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":null}");

    const std::string script = initialize + initialized + shutdown + exit_msg;
    REQUIRE(write(to_child[1], script.data(), script.size()) == static_cast<ssize_t>(script.size()));
    close(to_child[1]);

    std::string captured;
    char buffer[4096];
    while (true) {
        const ssize_t n = read(from_child[0], buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }
        captured.append(buffer, static_cast<size_t>(n));
    }
    close(from_child[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
    REQUIRE(well_formed_frames(captured));
    REQUIRE(captured.find("Content-Length:") == 0);
    REQUIRE(captured.find("\"hoverProvider\":true") != std::string::npos);
    REQUIRE(captured.find("\"referencesProvider\":true") != std::string::npos);
    REQUIRE(captured.find("\"workspaceSymbolProvider\":true") != std::string::npos);
    REQUIRE(captured.find("signatureHelpProvider") != std::string::npos);
}

TEST_CASE("didOpen of a broken file publishes diagnostics, a fix clears them", "[lsp]")
{
    int to_child[2];
    int from_child[2];
    REQUIRE(pipe(to_child) == 0);
    REQUIRE(pipe(from_child) == 0);

    const pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        execl(ECHOC_BINARY, "echoc", "lsp", "--no-stdlib", static_cast<char *>(nullptr));
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);

    const std::string uri = "file:///tmp/lsp-e2e-session.eco";
    const std::string broken
        = "function main() : void { echo $missing; }";
    const std::string fixed
        = "function main() : void { echo 1; }";

    auto escape = [](const std::string &text) {
        std::string out;
        for (char ch : text) {
            if (ch == '"' || ch == '\\') {
                out.push_back('\\');
            }
            out.push_back(ch);
        }
        return out;
    };

    auto send = [&](const std::string &body) {
        const std::string framed = frame(body);
        REQUIRE(write(to_child[1], framed.data(), framed.size()) == static_cast<ssize_t>(framed.size()));
    };

    send("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"capabilities\":{}}}");
    send("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
    send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{"
        "\"textDocument\":{\"uri\":\"" + uri + "\",\"languageId\":\"echo\",\"version\":1,\"text\":\""
        + escape(broken) + "\"}}}");
    send("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/hover\",\"params\":{"
        "\"textDocument\":{\"uri\":\"" + uri + "\"},\"position\":{\"line\":0,\"character\":10}}}");

    std::string captured;
    REQUIRE(wait_until(from_child[0], captured, 10000, [](const std::string &text) {
        const auto payloads = diagnostic_payloads(text);
        return !payloads.empty() && payloads.back().find("$missing") != std::string::npos;
    }));

    send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{"
        "\"textDocument\":{\"uri\":\"" + uri + "\",\"version\":2},"
        "\"contentChanges\":[{\"text\":\"" + escape(fixed) + "\"}]}}");

    REQUIRE(wait_until(from_child[0], captured, 10000, [](const std::string &text) {
        const auto payloads = diagnostic_payloads(text);
        return !payloads.empty() && payloads.back().find("$missing") == std::string::npos;
    }));

    send("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"shutdown\",\"params\":null}");
    send("{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":null}");
    close(to_child[1]);

    while (read_more(from_child[0], captured, 1000)) {
    }
    close(from_child[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
    REQUIRE(well_formed_frames(captured));

    const auto payloads = diagnostic_payloads(captured);
    REQUIRE_FALSE(payloads.empty());
    REQUIRE(payloads.front().find("$missing") != std::string::npos);
    REQUIRE(payloads.back().find("$missing") == std::string::npos);
}

#endif
