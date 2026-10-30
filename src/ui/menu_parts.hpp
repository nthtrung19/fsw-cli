#pragma once

// The menus buildRootMenu (menu_builder.cpp) puts together, one file each:
// menu_tlm.cpp, menu_file.cpp, menu_verify.cpp. Internal to ui/.

#include "service/file_transfer_service.hpp"
#include "ui/fsw_command.hpp"
#include "ui/menu_builder.hpp"
#include "ui/tlm_context.hpp"
#include "verify/verify_service.hpp"

#include <cstddef>
#include <memory>

namespace cli { class Menu; }

namespace mcs::ui {

class ScriptRunner;

namespace menus {

std::unique_ptr<cli::Menu> buildTlmMenu(TlmContext& tlm, std::size_t& sessionErrors);

std::unique_ptr<cli::Menu> buildFileMenu(FileTransferService& files, std::size_t& sessionErrors, FileMenuMode mode);

// Batch sessions only: the checks block while they wait.
void insertVerifyCommands(cli::Menu& root, VerifyService& verify);

std::unique_ptr<FswCommand> buildRunCommand(ScriptRunner& runner, std::size_t& sessionErrors);

} // namespace menus

} // namespace mcs::ui
