#ifndef LSPTRANSPORT_H
#define LSPTRANSPORT_H

#pragma once

#include <istream>
#include <ostream>
#include <string>

namespace Compiler
{
    namespace Lsp
    {
        // Content-Length framing over a pair of streams. stream-parameterized so a stringstream
        // is a test, the same shape tests/progress.cpp uses
        class Transport
        {
        public:

            Transport(std::istream &in, std::ostream &out);

            enum class FrameKind
            {
                t_message,
                t_eof,
                t_invalid
            };

            struct Frame
            {
                FrameKind kind = FrameKind::t_eof;
                std::string body;
            };

            // the next framed body. t_eof ends the server; t_invalid is this frame only
            Frame read_message();

            void write_message(const std::string &body);

            // is there a byte the next read_message would take, without blocking longer than
            // `timeout_ms`. POSIX polls stdin; Windows waits on the stdin handle; a stringstream
            // answers from in_avail and does not wait
            bool input_pending(int timeout_ms);

        private:

            std::istream &_in;
            std::ostream &_out;
        };
    };
};

#endif
