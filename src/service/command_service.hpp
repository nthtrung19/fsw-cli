#pragma once

#include "catalog/catalog.hpp"
#include "encode/payload_encoder.hpp"
#include "log/packet_log.hpp"
#include "pipeline/pipeline.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
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

    // What execute() / executeRaw() check, without sending or printing
    // anything but errors (the pass over a script before it runs). Arming is
    // tracked as execute() does. With `checkValues` false (the arguments come
    // from script variables not known yet) only the command and the number of
    // arguments are checked.
    bool check(const std::string& app, const std::string& command,
               const std::vector<std::string>& args, bool checkValues, std::ostream& out);
    bool checkRaw(const std::vector<std::string>& hexTokens, bool checkValues, std::ostream& out);

    // Critical commands are refused unless armed; arming covers the next
    // critical command only.
    void arm() { armed_ = true; }
    bool armed() const { return armed_; }

    void setVerbose(bool on) { verbose_ = on; }
    bool verbose() const { return verbose_; }

    std::size_t failures() const { return failures_; }
    std::size_t packetsSent() const { return sent_; }
    // When the last packet was sent (system clock, as telemetry receive times).
    std::optional<std::chrono::system_clock::time_point> lastSentAt() const { return lastSentAt_; }

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
    std::optional<std::chrono::system_clock::time_point> lastSentAt_;
};

} // namespace mcs
