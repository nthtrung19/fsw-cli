#pragma once

#include "catalog/catalog.hpp"
#include "encode/payload_encoder.hpp"
#include "log/packet_log.hpp"
#include "pipeline/pipeline.hpp"

#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

namespace mcs {

// Executes operator commands end to end:
//   catalog lookup -> payload encoding -> (arm check) -> pipeline -> log -> status line.
//
// Every error is caught here and reported as one "error: ..." line; nothing
// propagates into the UI. Methods return true on success.
class CommandService {
public:
    CommandService(const CommandCatalog& catalog, PayloadEncoder encoder, Pipeline& pipeline,
                   PacketLog* log, std::string targetName);

    bool execute(const std::string& app, const std::string& command,
                 const std::vector<std::string>& args, std::ostream& out);

    // Sends pre-built packet bytes given as hex tokens (still through layers + transport).
    bool executeRaw(const std::vector<std::string>& hexTokens, std::ostream& out);

    // Critical commands are refused unless armed; arming covers the next
    // critical command only.
    void arm() { armed_ = true; }
    bool armed() const { return armed_; }

    void setVerbose(bool on) { verbose_ = on; }
    bool verbose() const { return verbose_; }

    std::size_t failures() const { return failures_; }
    std::size_t packetsSent() const { return sent_; }

    const CommandCatalog& catalog() const { return catalog_; }
    const Pipeline& pipeline() const { return pipeline_; }
    const std::string& targetName() const { return targetName_; }

private:
    void report(const std::string& commandLine, const Pipeline::Result& result, std::ostream& out);
    bool fail(const std::string& message, std::ostream& out);

    const CommandCatalog& catalog_;
    PayloadEncoder encoder_;
    Pipeline& pipeline_;
    PacketLog* log_;
    std::string targetName_;
    bool verbose_ = false;
    bool armed_ = false;
    std::size_t failures_ = 0;
    std::size_t sent_ = 0;
};

} // namespace mcs
