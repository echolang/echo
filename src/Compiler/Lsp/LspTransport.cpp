#include "Compiler/Lsp/LspTransport.h"

#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <poll.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

Compiler::Lsp::Transport::Transport(std::istream &in, std::ostream &out) :
    _in(in), _out(out)
{
}

Compiler::Lsp::Transport::Frame Compiler::Lsp::Transport::read_message()
{
    std::size_t content_length = 0;
    bool saw_length = false;
    bool invalid_length = false;
    std::string line;

    while (true) {
        if (!std::getline(_in, line)) {
            return Frame{ FrameKind::t_eof, {} };
        }

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            break;
        }

        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        std::string name = line.substr(0, colon);
        for (char &ch : name) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        if (name != "content-length") {
            continue;
        }

        std::string value = line.substr(colon + 1);
        const auto first = value.find_first_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }

        try {
            content_length = static_cast<std::size_t>(std::stoul(value.substr(first)));
            saw_length = true;
        }
        catch (const std::invalid_argument &) {
            invalid_length = true;
        }
        catch (const std::out_of_range &) {
            invalid_length = true;
        }
    }

    if (invalid_length) {
        return Frame{ FrameKind::t_invalid, {} };
    }

    if (!saw_length) {
        if (!_in) {
            return Frame{ FrameKind::t_eof, {} };
        }

        return Frame{ FrameKind::t_invalid, {} };
    }

    std::string body(content_length, '\0');
    _in.read(body.data(), static_cast<std::streamsize>(content_length));

    if (static_cast<std::size_t>(_in.gcount()) != content_length) {
        return !_in ? Frame{ FrameKind::t_eof, {} } : Frame{ FrameKind::t_invalid, {} };
    }

    return Frame{ FrameKind::t_message, std::move(body) };
}

void Compiler::Lsp::Transport::write_message(const std::string &body)
{
    _out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    _out.flush();
}

bool Compiler::Lsp::Transport::input_pending(int timeout_ms)
{
    if (_in.rdbuf() != nullptr && _in.rdbuf()->in_avail() > 0) {
        return true;
    }

    if (&_in != &std::cin) {
        return false;
    }

#if defined(__unix__) || defined(__APPLE__)
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;

    const int rc = ::poll(&pfd, 1, timeout_ms);
    return rc > 0 && (pfd.revents & POLLIN) != 0;
#elif defined(_WIN32)
    HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        return false;
    }

    const DWORD wait = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
    return WaitForSingleObject(handle, wait) == WAIT_OBJECT_0;
#else
    (void)timeout_ms;
    return false;
#endif
}
