// mcs_fake_fsw: a stand-in for the FSW's DS app, for end-to-end tests of
// script checks ("expect", "expect-event", "save", ...) without cFS.
//
//   receives on UDP (like CI)   DS commands on 0x194B: noop (cc 0) and reset (cc 1)
//                               are accepted, any other code is rejected
//   sends on UDP (like TO)      DS housekeeping (0x0948) every --hk-period ms and
//                               right after each command, and EVS long events (0x0808):
//                               noop id 31 INFO, reset id 33 DEBUG, bad code id 21 ERROR
//
// Packets are laid out from config/telemetry/ds.json and evs.json, as the FSW
// sends them (little-endian).
//
//   mcs_fake_fsw --config-dir DIR [--port 1234] [--tlm-host 127.0.0.1] [--tlm-port 5011]
//                [--hk-period 200] [--lifetime SECONDS]

#include "config/tlm_catalog_loader.hpp"
#include "telemetry/tlm_catalog.hpp"
#include "transport/udp_receiver.hpp"
#include "transport/udp_transport.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>

using namespace mcs;

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int)
{
    g_stop = true;
}

// cFE 6.7 telemetry packet: primary header, 6-byte time (0 here), payload.
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

// Writes `value` (little-endian, field size) or `text` (NUL-padded) into field `name` of `payload`.
void put(Bytes& payload, const TlmPacketDef& def, const std::string& name, std::uint64_t value)
{
    const TlmFieldDef* f = def.findField(name);
    for (std::size_t i = 0; f != nullptr && i < f->size; ++i) {
        payload[f->offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

void putText(Bytes& payload, const TlmPacketDef& def, const std::string& name, const std::string& text)
{
    const TlmFieldDef* f = def.findField(name);
    if (f != nullptr) {
        std::memcpy(&payload[f->offset], text.data(), std::min(text.size(), f->size - 1));
    }
}

} // namespace

int main(int argc, char* argv[])
{
    std::string configDir;
    std::uint16_t port = 1234;
    std::string tlmHost = "127.0.0.1";
    std::uint16_t tlmPort = 5011;
    int hkPeriodMs = 200;
    int lifetime = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string opt = argv[i];
        const std::string val = argv[i + 1];
        if (opt == "--config-dir") {
            configDir = val;
        } else if (opt == "--port") {
            port = static_cast<std::uint16_t>(std::stoi(val));
        } else if (opt == "--tlm-host") {
            tlmHost = val;
        } else if (opt == "--tlm-port") {
            tlmPort = static_cast<std::uint16_t>(std::stoi(val));
        } else if (opt == "--hk-period") {
            hkPeriodMs = std::stoi(val);
        } else if (opt == "--lifetime") {
            lifetime = std::stoi(val);
        } else {
            std::cerr << "mcs_fake_fsw: unknown option " << opt << '\n';
            return 2;
        }
    }
    if (configDir.empty()) {
        std::cerr << "mcs_fake_fsw: --config-dir is required\n";
        return 2;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    try {
        TelemetryCatalog catalog;
        for (const char* file : {"/telemetry/ds.json", "/telemetry/evs.json"}) {
            for (auto& p : loadTlmFile(configDir + file)) {
                catalog.add(std::move(p));
            }
        }
        const TlmPacketDef* hkDef = catalog.find(0x0948);
        const TlmPacketDef* eventDef = catalog.find(0x0808);
        if (hkDef == nullptr || eventDef == nullptr) {
            std::cerr << "mcs_fake_fsw: ds.hk (0x0948) or evs.long_event (0x0808) is not defined\n";
            return 1;
        }

        UdpReceiver commands("127.0.0.1", port);
        UdpTransport tlm(tlmHost, tlmPort);
        std::uint16_t hkSeq = 0;
        std::uint16_t eventSeq = 0;
        std::uint8_t accepted = 0;
        std::uint8_t rejected = 0;

        const auto sendHk = [&] {
            Bytes payload(hkDef->payloadSize(), 0);
            put(payload, *hkDef, "cmd_accepted_counter", accepted);
            put(payload, *hkDef, "cmd_rejected_counter", rejected);
            tlm.send(telemetry(0x0948, hkSeq++, payload));
        };
        const auto sendEvent = [&](std::uint16_t id, std::uint16_t type, const std::string& message) {
            Bytes payload(eventDef->payloadSize(), 0);
            putText(payload, *eventDef, "app_name", "DS");
            put(payload, *eventDef, "event_id", id);
            put(payload, *eventDef, "event_type", type);
            putText(payload, *eventDef, "message", message);
            tlm.send(telemetry(0x0808, eventSeq++, payload));
        };

        std::cerr << "fake_fsw: DS stand-in on udp://127.0.0.1:" << port << ", telemetry to " << tlm.describe()
                  << '\n';
        const auto start = std::chrono::steady_clock::now();
        auto nextHk = start;
        while (!g_stop && (lifetime == 0 || std::chrono::steady_clock::now() < start + std::chrono::seconds(lifetime))) {
            if (auto packet = commands.receive(std::chrono::milliseconds(10))) {
                const Bytes& p = *packet;
                if (p.size() >= 8 && ((p[0] << 8) | p[1]) == 0x194B) {   // DS_CMD_MID
                    const int cc = p[7] & 0x7F;
                    if (cc == 0) {
                        ++accepted;
                        sendEvent(31, 2, "NOOP command, Version 2.5.2.0");
                    } else if (cc == 1) {
                        accepted = 0;
                        rejected = 0;
                        sendEvent(33, 1, "Reset counters command");
                    } else {
                        ++rejected;
                        sendEvent(21, 3, "Invalid command code: MID = 0x194B, CC = " + std::to_string(cc));
                    }
                    sendHk();
                }
            }
            if (std::chrono::steady_clock::now() >= nextHk) {
                sendHk();
                nextHk += std::chrono::milliseconds(hkPeriodMs);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "mcs_fake_fsw: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
