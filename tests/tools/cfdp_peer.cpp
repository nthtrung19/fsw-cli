// mcs_cfdp_peer: a stand-in for CF on the PC build, for end-to-end tests of
// "file put/get" without cFS.
//
//   receives on UDP (like CI)   0x192D command packets  -> 8-byte header stripped -> CFDP engine (0.24)
//                               0x1923 cc 2 playback_file -> the engine sends the file
//   sends on UDP (like TO)      every PDU of the engine in a 0x092D telemetry packet
//
// It runs the same GSFC engine as CF and as mcs. Files live under --root:
// the FSW path "/cf/log/hk.dat" is <root>/cf/log/hk.dat.
//
//   mcs_cfdp_peer [--port 1234] [--tlm-host 127.0.0.1] [--tlm-port 5011] [--root DIR]
//                 [--drop N] [--rate PDUS_PER_SECOND] [--lifetime SECONDS]
//
// --drop N drops every Nth FileData PDU it receives (the first time for each
// offset), so class 2 must recover it with a NAK. --rate paces the FileData
// it sends (CF on the PC build: about 4 per second, 4000 bytes each).

#include "cfdp/gsfc_engine.hpp"
#include "cfdp/pdu_info.hpp"
#include "transport/udp_receiver.hpp"
#include "transport/udp_transport.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <string>

using namespace mcs;
using namespace mcs::cfdp;
namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int)
{
    g_stop = true;
}

std::string field(const Bytes& b, std::size_t at, std::size_t size)
{
    std::string s;
    for (std::size_t i = at; i < at + size && i < b.size() && b[i] != 0; ++i) {
        s += static_cast<char>(b[i]);
    }
    return s;
}

// cFE 6.7 telemetry packet: primary header, 6-byte time (little-endian on the PC build), payload.
Bytes telemetry(std::uint16_t mid, std::uint16_t sequence, const Bytes& payload)
{
    const std::size_t length = 12 + payload.size();
    const auto lengthField = static_cast<std::uint16_t>(length - 7);
    const auto seqField = static_cast<std::uint16_t>(0xC000 | (sequence & 0x3FFF));
    Bytes p{static_cast<std::uint8_t>(mid >> 8),         static_cast<std::uint8_t>(mid),
            static_cast<std::uint8_t>(seqField >> 8),    static_cast<std::uint8_t>(seqField),
            static_cast<std::uint8_t>(lengthField >> 8), static_cast<std::uint8_t>(lengthField),
            0, 0, 0, 0, 0, 0};
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}

} // namespace

int main(int argc, char* argv[])
{
    std::uint16_t port = 1234;
    std::string tlmHost = "127.0.0.1";
    std::uint16_t tlmPort = 5011;
    fs::path root = "fsw";
    unsigned drop = 0;
    unsigned rate = 2000;
    int lifetime = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string opt = argv[i];
        const std::string val = argv[i + 1];
        if (opt == "--port") {
            port = static_cast<std::uint16_t>(std::stoi(val));
        } else if (opt == "--tlm-host") {
            tlmHost = val;
        } else if (opt == "--tlm-port") {
            tlmPort = static_cast<std::uint16_t>(std::stoi(val));
        } else if (opt == "--root") {
            root = val;
        } else if (opt == "--drop") {
            drop = static_cast<unsigned>(std::stoi(val));
        } else if (opt == "--rate") {
            rate = static_cast<unsigned>(std::stoi(val));
        } else if (opt == "--lifetime") {
            lifetime = std::stoi(val);
        } else {
            std::cerr << "mcs_cfdp_peer: unknown option " << opt << '\n';
            return 2;
        }
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    try {
        UdpReceiver commands("127.0.0.1", port);
        UdpTransport tlm(tlmHost, tlmPort);
        std::uint16_t tlmSeq = 0;

        Config cfg;
        cfg.localEntity = "0.24";
        cfg.remoteEntity = "0.23";
        cfg.downlinkDir = root;   // received "/cf/x" lands at <root>/cf/x
        cfg.chunkSize = 4000;      // like CF's table
        cfg.pdusPerSecond = rate;
        cfg.ackTimeout = 1;
        cfg.nakTimeout = 1;
        cfg.ackLimit = 4;
        cfg.nakLimit = 6;
        cfg.inactivityTimeout = 10;
        GsfcEngine engine(
            cfg, [&](const Bytes& pdu, const PduInfo&) { tlm.send(telemetry(0x092D, tlmSeq++, pdu)); },
            [](EngineEvent e, const TransferStatus& s) {
                std::cerr << "peer: " << (e == EngineEvent::Started ? "started " : "finished ") << s.id.toString()
                          << ' ' << s.remote << (e == EngineEvent::Finished ? std::string(" ") + toString(s.state) : "")
                          << '\n';
            },
            [](int level, const std::string& text) {
                if (level >= 2) {
                    std::cerr << "peer: cfdp: " << text << '\n';
                }
            });

        unsigned seenFileData = 0;
        std::set<std::uint32_t> dropped;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(lifetime);
        std::cerr << "peer: CF stand-in 0.24 on udp://127.0.0.1:" << port << ", telemetry to " << tlm.describe()
                  << ", root " << root << '\n';
        while (!g_stop && (lifetime == 0 || std::chrono::steady_clock::now() < end)) {
            if (auto packet = commands.receive(std::chrono::milliseconds(10))) {
                const Bytes& p = *packet;
                if (p.size() >= 8) {
                    const auto mid = static_cast<std::uint16_t>((p[0] << 8) | p[1]);
                    if (mid == 0x192D) {   // CF_INCOMING_PDU_MID
                        const Bytes pdu(p.begin() + 8, p.end());
                        const auto info = inspectPdu(pdu);
                        bool skip = false;
                        if (drop != 0 && info && info->kind == PduKind::FileData && dropped.count(info->offset) == 0
                            && ++seenFileData % drop == 0) {
                            dropped.insert(info->offset);
                            skip = true;
                        }
                        if (!skip) {
                            engine.givePdu(pdu);
                        }
                    } else if (mid == 0x1923 && p[7] == 2 && p.size() >= 8 + 4 + 16 + 64 + 64) {
                        // CF_PlaybackFileCmd_t: class, channel, priority, preserve, peer[16], src[64], dst[64]
                        const int cls = p[8];
                        const std::string src = field(p, 28, 64);
                        const std::string dst = field(p, 92, 64);
                        std::string error;
                        const fs::path local = root / fs::path(src).relative_path();
                        if (!engine.put({local, dst, cls, src}, error)) {
                            std::cerr << "peer: playback_file " << src << " refused: " << error << '\n';
                        }
                    }
                }
            }
            engine.cycle();
        }
    } catch (const std::exception& e) {
        std::cerr << "mcs_cfdp_peer: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
