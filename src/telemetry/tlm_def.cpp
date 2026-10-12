#include "telemetry/tlm_def.hpp"

#include <algorithm>

namespace mcs {

std::size_t TlmPacketDef::payloadSize() const
{
    std::size_t end = 0;
    for (const auto& f : fields) {
        end = std::max(end, f.offset + f.size);
    }
    return end;
}

const TlmFieldDef* TlmPacketDef::findField(const std::string& fieldName) const
{
    for (const auto& f : fields) {
        if (f.visible() && f.name == fieldName) {
            return &f;
        }
    }
    return nullptr;
}

} // namespace mcs
