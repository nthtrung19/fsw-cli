# ----------------------------------------------------------------------------
# mcs_core: all logic that does NOT depend on the cli library.
# Dependency rules (see docs/DESIGN.md §3.2):
#   core      <- nothing
#   catalog, encode, protocol, transport, registry, pipeline, log <- core
#   config    <- the above + nlohmann/json (the ONLY JSON-aware module)
#   service   <- catalog, encode, pipeline, log
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

    registry/plugin_registry.cpp
    registry/builtin_plugins.cpp

    pipeline/pipeline.cpp

    config/json_util.cpp
    config/catalog_loader.cpp
    config/target_config.cpp
    config/config_paths.cpp
    config/pipeline_factory.cpp

    log/packet_log.cpp

    service/command_service.cpp)

target_include_directories(mcs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mcs_core
    PUBLIC  mcs_warnings
    PRIVATE nlohmann_json::nlohmann_json)

# ----------------------------------------------------------------------------
# mcs: the executable. Only ui/ and app/ may include <cli/...>.
# ----------------------------------------------------------------------------
add_executable(mcs
    ui/fsw_command.cpp
    ui/menu_builder.cpp
    app/options.cpp
    app/main.cpp)

target_link_libraries(mcs PRIVATE mcs_core cli::cli mcs_warnings)
target_compile_definitions(mcs PRIVATE mcs_VERSION="${PROJECT_VERSION}")
