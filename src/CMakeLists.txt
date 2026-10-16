# ----------------------------------------------------------------------------
# mcs_core: all logic that does NOT depend on the cli library.
# Dependency rules (see docs/DESIGN.md §3.2):
#   core      <- nothing
#   catalog, encode, protocol, transport, registry, pipeline, log <- core
#   config    <- the above + nlohmann/json (the ONLY JSON-aware module)
#   service   <- catalog, encode, pipeline, log, cfdp, telemetry (parser only)
#   telemetry <- core, catalog (field types), transport (IReceiver)
#   cfdp      <- core + the GSFC CFDP library (external/cfdp-gsfc), reached
#                only through cfdp/gsfc_bridge.c (C) and cfdp/gsfc_engine.cpp
# ----------------------------------------------------------------------------
add_library(mcs_core STATIC
    core/bytes.cpp
    core/byte_writer.cpp
    core/hex.cpp
    core/options.cpp
    core/text.cpp

    catalog/field_def.cpp
    catalog/command_def.cpp
    catalog/catalog.cpp
    catalog/arg_completion.cpp

    encode/payload_encoder.cpp

    protocol/ccsds_v1_format.cpp

    transport/udp_transport.cpp
    transport/dry_run_transport.cpp
    transport/udp_receiver.cpp

    registry/plugin_registry.cpp
    registry/builtin_plugins.cpp

    pipeline/pipeline.cpp

    config/json_util.cpp
    config/catalog_loader.cpp
    config/target_config.cpp
    config/config_paths.cpp
    config/pipeline_factory.cpp
    config/tlm_catalog_loader.cpp

    log/packet_log.cpp

    service/command_service.cpp
    service/file_transfer_service.cpp

    telemetry/tlm_def.cpp
    telemetry/tlm_catalog.cpp
    telemetry/ccsds_tlm_parser.cpp
    telemetry/tlm_decoder.cpp
    telemetry/tlm_store.cpp
    telemetry/telemetry_service.cpp

    cfdp/transfer.cpp
    cfdp/pdu_info.cpp
    cfdp/gsfc_engine.cpp
    cfdp/gsfc_bridge.c)

# The bridge is our C code: same strictness as the C++ code.
set_source_files_properties(cfdp/gsfc_bridge.c PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic")

find_package(Threads REQUIRED)

target_include_directories(mcs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mcs_core
    PUBLIC  mcs_warnings Threads::Threads
    PRIVATE nlohmann_json::nlohmann_json cfdp_gsfc)

# ----------------------------------------------------------------------------
# mcs: the executable. Only ui/ and app/ may include <cli/...> or <ftxui/...>.
# ----------------------------------------------------------------------------
add_executable(mcs
    ui/fsw_command.cpp
    ui/menu_builder.cpp
    ui/completion.cpp
    ui/help_format.cpp
    ui/transfer_format.cpp
    ui/tui_session.cpp
    app/options.cpp
    app/main.cpp)

target_link_libraries(mcs PRIVATE mcs_core cli::cli ftxui::component mcs_warnings)
target_compile_definitions(mcs PRIVATE mcs_VERSION="${PROJECT_VERSION}")
