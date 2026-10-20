#include "telemetry/tlm_catalog.hpp"

#include "core/errors.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <set>

namespace mcs {

namespace {

void checkField(const TlmFieldDef& f, const std::string& ctx)
{
    if (f.type == FieldType::Padding) {
        if (f.size == 0) {
            throw ConfigError(ctx + ": padding needs a size > 0");
        }
        return;
    }
    if (!isIdentifier(f.name)) {
        throw ConfigError(ctx + ": field name '" + f.name
                          + "' must be lowercase letters, digits and '_', starting with a letter");
    }
    const std::size_t natural = naturalSize(f.type);
    if (natural != 0 && f.size != natural) {
        throw ConfigError(ctx + ": field '" + f.name + "' is " + toString(f.type) + " ("
                          + std::to_string(natural) + " bytes), not " + std::to_string(f.size));
    }
    if (f.type == FieldType::String && f.size == 0) {
        throw ConfigError(ctx + ": string field '" + f.name + "' needs a size > 0");
    }
    if (!f.enumValues.empty() && !isInteger(f.type)) {
        throw ConfigError(ctx + ": field '" + f.name + "': 'enum' is only allowed on integer fields");
    }
    if (f.hex && !isInteger(f.type)) {
        throw ConfigError(ctx + ": field '" + f.name + "': 'hex' is only allowed on integer fields");
    }
}

} // namespace

void TelemetryCatalog::add(TlmPacketDef packet)
{
    const std::string ctx = "telemetry packet '" + packet.app + "." + packet.name + "'";
    if (!isIdentifier(packet.app)) {
        throw ConfigError(ctx + ": app name must be lowercase letters, digits and '_'");
    }
    if (!isIdentifier(packet.name)) {
        throw ConfigError(ctx + ": packet name must be lowercase letters, digits and '_'");
    }
    if (const auto it = byMid_.find(packet.mid); it != byMid_.end()) {
        throw ConfigError(ctx + ": MID " + toHexString(packet.mid, 4) + " is already used by '"
                          + packets_[it->second].fullName() + "'");
    }
    for (const auto& p : packets_) {
        if (p.app == packet.app && p.name == packet.name) {
            throw ConfigError(ctx + ": defined twice");
        }
    }

    std::set<std::string> names;
    std::size_t offset = 0;
    for (auto& f : packet.fields) {
        checkField(f, ctx);
        if (f.visible() && !names.insert(f.name).second) {
            throw ConfigError(ctx + ": field '" + f.name + "' is defined twice");
        }
        f.offset = offset;
        offset += f.size;
    }
    if (kTlmHeaderSize + offset > 0xFFFF + 7) {
        throw ConfigError(ctx + ": payload of " + std::to_string(offset)
                          + " bytes does not fit a CCSDS packet");
    }

    if (packet.isEvent) {
        for (const char* required : {"app_name", "event_id", "event_type"}) {
            if (packet.findField(required) == nullptr) {
                throw ConfigError(ctx + ": event packets need a field '" + required + "'");
            }
        }
    }

    byMid_.emplace(packet.mid, packets_.size());
    packets_.push_back(std::move(packet));
}

const TlmPacketDef* TelemetryCatalog::find(std::uint16_t mid) const
{
    const auto it = byMid_.find(mid);
    return it == byMid_.end() ? nullptr : &packets_[it->second];
}

const TlmPacketDef* TelemetryCatalog::findByName(std::string_view fullName) const
{
    for (const auto& p : packets_) {
        if (iequals(p.fullName(), fullName)) {
            return &p;
        }
    }
    return nullptr;
}

std::vector<std::string> TelemetryCatalog::apps() const
{
    std::vector<std::string> out;
    for (const auto& p : packets_) {
        if (std::find(out.begin(), out.end(), p.app) == out.end()) {
            out.push_back(p.app);
        }
    }
    return out;
}

} // namespace mcs
