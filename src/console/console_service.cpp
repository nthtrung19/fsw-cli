#include "console/console_service.hpp"

namespace mcs {

ConsoleService::ConsoleService(std::unique_ptr<IReceiver> receiver, ConsoleStore& store)
    : store_(store),
      thread_(std::move(receiver), [this](const Bytes& datagram) { store_.ingest(datagram); },
              [this] { store_.flushIdle(); }, "console service")
{
}

} // namespace mcs
