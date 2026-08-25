#ifndef LSPSERVER_H
#define LSPSERVER_H

#pragma once

#include "Compiler/DriverOptions.h"

#include <istream>
#include <memory>
#include <ostream>

namespace Compiler
{
    namespace Lsp
    {
        // JSON-RPC over stdio. queries stay on this thread. a rebuild runs on a
        // worker so hover is not stuck behind compiling the project
        class Server
        {
        public:

            Server(std::istream &in, std::ostream &out, const DriverOptions &driver);
            ~Server();

            Server(const Server &) = delete;
            Server &operator=(const Server &) = delete;

            int run();

        private:

            struct Impl;
            std::unique_ptr<Impl> _impl;
        };
    };
};

#endif
