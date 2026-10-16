#pragma once

// Text for file transfers: the `file list` / `file status` output and the
// F1 console's TRANSFERS strip and header badge. Pure functions (no UI
// library), so they are unit-tested directly.

#include "cfdp/transfer.hpp"
#include "service/file_transfer_service.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace mcs::ui {

// "FILES ↑1 ↓1", "FILES ↓1 · 1 waiting"; "" when nothing is active or waiting.
std::string transferBadge(const FileTransferService::Counts& counts);

// Keeps both ends: "/cf/log/…/hk_0001.dat". Counts UTF-8 code points.
std::string shortenMiddle(const std::string& text, std::size_t width);

// `cells` characters of █ and ░.
std::string progressBar(double percent, int cells);

// "test.bin → /cf/test.bin" (uplink) or "/cf/a.bin → downlink/cf/a.bin" (downlink).
std::string transferFiles(const cfdp::TransferStatus& s);

// One row of the F1 TRANSFERS strip, split in fields so the UI can colour them.
struct StripRow {
    bool uplink = true;
    bool waiting = false;
    bool suspended = false;
    std::string arrow;     // "↑" or "↓"
    std::string kind;      // "put", "get", "in" (not requested)
    std::string id;        // "" when compact or waiting
    std::string files;     // shortened to fit
    std::string cls;       // "c1", "c2"
    std::string bar;       // 20 cells, "" when waiting
    std::string percent;   // " 76%"
    std::string rate;      // "48.2 KB/s"
    std::string eta;       // "5 s", "" when compact or unknown
    std::string note;      // waiting/suspended text
};

// `width`: the room for the whole row; `compact` (narrow terminal) drops the
// ID and the ETA.
StripRow formatStripRow(const cfdp::TransferStatus& s, int width, bool compact);

// The row as plain text, in the same order (tests, plain sessions).
std::string toText(const StripRow& row);

// `file list`: ACTIVE then RECENT sections.
std::vector<std::string> listLines(const std::vector<cfdp::TransferStatus>& transfers, std::size_t maxRecent = 20);

// `file status <id>`.
std::vector<std::string> statusLines(const cfdp::TransferStatus& s);

} // namespace mcs::ui
