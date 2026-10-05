# ----------------------------------------------------------------------------
# fswcli_core: everything that does NOT depend on the cli library
# (catalog, encoder, protocol, transport, service). Populated from M2 onward.
# ----------------------------------------------------------------------------
# add_library(fswcli_core STATIC ...)

# ----------------------------------------------------------------------------
# fswcli: the executable. Only ui/ and app/ may include <cli/...>.
# ----------------------------------------------------------------------------
add_executable(fswcli
    app/main.cpp
    app/options.cpp
    ui/menu_builder.cpp)

target_include_directories(fswcli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fswcli PRIVATE cli::cli fswcli_warnings)
target_compile_definitions(fswcli PRIVATE
    FSWCLI_VERSION="${PROJECT_VERSION}")
