#ifndef LSPSESSION_H
#define LSPSESSION_H

#pragma once

#include "AST/ASTBundle.h"
#include "AST/ASTDiagnostic.h"
#include "AST/ASTFile.h"
#include "Compiler/CompilerOptions.h"
#include "Compiler/DriverOptions.h"
#include "Compiler/ParsePipeline.h"
#include "Compiler/Lsp/LspSnapshot.h"
#include "Compiler/TargetFacts.h"
#include "Parser/ManifestParser.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Compiler
{
    namespace Lsp
    {
        struct Document
        {
            std::string content;
            int version = 0;
        };

        struct RebuildReport
        {
            int parse_ms = 0;
            int semantic_ms = 0;
            int index_ms = 0;
            int total_ms = 0;
            size_t modules = 0;
            size_t files = 0;
            bool failed = false;
            std::string reason;
        };

        // overlay, project resolution, rebuild. one rebuild owner: a worker compiles a
        // captured generation; apply ignores a result the overlay has outrun. queries
        // answer from the last good Snapshot through LspQuery.h
        class Session
        {
        public:

            explicit Session(const DriverOptions &driver);
            ~Session();

            Session(const Session &) = delete;
            Session &operator=(const Session &) = delete;

            void set_workspace_root(const std::filesystem::path &root);

            void did_open(const std::filesystem::path &path, int version, std::string content);
            void did_change(const std::filesystem::path &path, int version, std::string content);
            void did_close(const std::filesystem::path &path);

            bool dirty() const {
                return _dirty;
            }

            const Snapshot *snapshot() const {
                return _snapshot.get();
            }

            bool has_snapshot() const {
                return _snapshot != nullptr;
            }

            // wait for a running compile, then compile again if still dirty, until a
            // snapshot matches the current overlay. the idle path uses start_rebuild
            RebuildReport rebuild();

            void start_rebuild(const std::string &reason);
            bool rebuild_running() const;
            std::optional<RebuildReport> take_finished_rebuild();

            std::vector<std::string> indexed_paths() const;

            const std::optional<FrontEndFailure> &parse_failure() const {
                return _parse_failure;
            }

            std::vector<AST::Diagnostic> diagnostics() const;
            std::optional<int> overlay_version(const std::filesystem::path &path) const;
            const AST::File *file_of(const std::filesystem::path &path) const;

        private:

            struct CompileInputs
            {
                uint64_t generation = 0;
                std::string reason;
                std::map<std::filesystem::path, Document> overlay;
                Compiler::TargetFacts facts;
                std::vector<Parser::ModuleManifest> manifests;
                std::vector<std::filesystem::path> roots;
                bool loose_mode = true;
                Parser::ActiveTargets active_targets;
                bool with_stdlib = true;
                CompilerOptions options;
            };

            struct RebuildProduct
            {
                uint64_t generation = 0;
                std::unique_ptr<Snapshot> snapshot;
                std::optional<FrontEndFailure> failure;
                RebuildReport report;
            };

            const DriverOptions &_driver;
            std::filesystem::path _workspace_root;
            bool _dirty = true;
            bool _resolve_needed = true;
            uint64_t _generation = 0;

            std::map<std::filesystem::path, Document> _overlay;

            Compiler::TargetFacts _facts;
            bool _facts_ok = false;

            std::vector<Parser::ModuleManifest> _manifests;
            std::vector<std::filesystem::path> _roots;
            bool _loose_mode = true;

            std::unique_ptr<Snapshot> _snapshot;
            std::optional<FrontEndFailure> _parse_failure;

            std::thread _worker;
            std::atomic<bool> _running{ false };
            std::mutex _finished_mu;
            std::optional<RebuildProduct> _finished;

            void mark_dirty();
            void set_document(const std::filesystem::path &path, int version, std::string content);
            std::optional<std::string> overlay_content_in(
                const std::map<std::filesystem::path, Document> &overlay,
                const std::filesystem::path &path
            ) const;
            void resolve_project();
            Parser::ActiveTargets workspace_targets() const;
            std::vector<std::filesystem::path> loose_sources_from(const CompileInputs &inputs) const;
            std::vector<const Parser::ModuleManifest *> compiled_manifests(
                const std::vector<Parser::ModuleManifest> &manifests
            ) const;
            bool launch(const std::string &reason);
            void wait_worker();
            CompileInputs capture_inputs(const std::string &reason) const;
            RebuildProduct produce_snapshot(const CompileInputs &inputs) const;
            bool apply_product(RebuildProduct product);
        };
    };
};

#endif
