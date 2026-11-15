cmake_minimum_required(VERSION 3.26)

project(Buddy VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "Buddy targets Windows and builds with MSVC only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

add_compile_options(/W4 /permissive- /utf-8 /EHsc)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(wil CONFIG REQUIRED)
find_package(unofficial-webview2 CONFIG REQUIRED)

# --- core: protocol, themes, settings. Standard library only, no Windows headers. ---
add_library(buddy_core STATIC
  src/core/hotkey.cpp
  src/core/log.cpp
  src/core/placement.cpp
  src/core/protocol.cpp
  src/core/settings.cpp
  src/core/text.cpp
  src/core/theme.cpp
  src/core/url.cpp
)
target_include_directories(buddy_core PUBLIC src)
target_link_libraries(buddy_core PUBLIC nlohmann_json::nlohmann_json)

# --- providers: the chat provider interface and the OpenAI-compatible client. No Windows headers. ---
add_library(buddy_providers STATIC
  src/providers/anthropic.cpp
  src/providers/local_endpoint.cpp
  src/providers/openai_compat.cpp
  src/providers/sse_parser.cpp
)
target_link_libraries(buddy_providers PUBLIC buddy_core)

# --- platform: WinHTTP transport, Credential Manager, known folders. ---
add_library(buddy_platform STATIC
  src/platform/credential_store.cpp
  src/platform/paths.cpp
  src/platform/winhttp_transport.cpp
)
target_link_libraries(buddy_platform PUBLIC buddy_providers PRIVATE winhttp advapi32 shell32 ole32)

# --- app logic: the controller behind the UI bridge. No window code, so tests can drive it. ---
add_library(buddy_app_logic STATIC
  src/app/app_controller.cpp
)
target_link_libraries(buddy_app_logic PUBLIC buddy_providers)

# --- webhost: the WebView2 control and its lock-down. ---
add_library(buddy_webhost STATIC
  src/webhost/webview_host.cpp
)
target_include_directories(buddy_webhost PUBLIC src)
target_link_libraries(buddy_webhost PRIVATE unofficial::webview2::webview2 WIL::WIL)

# --- the application ---
add_executable(buddy WIN32
  src/app/main.cpp
  src/app/main_window.cpp
  src/app/panel_window.cpp
  src/app/win_util.cpp
  src/app/app.manifest
)
target_compile_definitions(buddy PRIVATE BUDDY_VERSION="${PROJECT_VERSION}")
target_link_libraries(buddy PRIVATE buddy_app_logic buddy_platform buddy_webhost dwmapi shcore shell32)

# The web UI and the themes ship as folders next to the exe (virtual host mapping serves a folder).
add_custom_command(TARGET buddy POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:buddy>/ui"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:buddy>/ui"
  COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:buddy>/themes"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/themes" "$<TARGET_FILE_DIR:buddy>/themes"
  COMMENT "Copying ui/ and themes/ next to buddy.exe"
  VERBATIM
)
# Re-run the copy when only web assets changed.
file(GLOB_RECURSE BUDDY_ASSET_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/ui/*" "${CMAKE_SOURCE_DIR}/themes/*")
set_property(TARGET buddy APPEND PROPERTY LINK_DEPENDS ${BUDDY_ASSET_FILES})

# --- tests ---
option(BUDDY_BUILD_TESTS "Build the unit tests" ON)
if(BUDDY_BUILD_TESTS)
  find_package(Catch2 3 CONFIG REQUIRED)
  find_package(httplib CONFIG REQUIRED)
  enable_testing()

  add_executable(buddy_tests
    tests/fake_transport.h
    tests/test_app_controller.cpp
    tests/test_anthropic.cpp
    tests/test_credential_store.cpp
    tests/test_hotkey.cpp
    tests/test_local_endpoint.cpp
    tests/test_openai_compat.cpp
    tests/test_placement.cpp
    tests/test_protocol.cpp
    tests/test_settings.cpp
    tests/test_sse_parser.cpp
    tests/test_theme.cpp
    tests/test_url.cpp
    tests/test_winhttp_transport.cpp
  )
  target_compile_definitions(buddy_tests PRIVATE BUDDY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  target_link_libraries(buddy_tests PRIVATE
    buddy_app_logic buddy_platform
    Catch2::Catch2WithMain httplib::httplib ws2_32)

  include(Catch)
  catch_discover_tests(buddy_tests)

  # A stand-alone OpenAI-compatible mock server for trying the app by hand without an API key.
  add_executable(buddy_mock_server tests/mock_server/main.cpp)
  target_link_libraries(buddy_mock_server PRIVATE httplib::httplib nlohmann_json::nlohmann_json ws2_32)
endif()

# --- spikes: throw-away experiments, kept for the record. Off by default. ---
option(BUDDY_BUILD_SPIKES "Build the experiments under spikes/" OFF)
if(BUDDY_BUILD_SPIKES)
  # M1.5: transparent, rounded, click-through WebView2 through DirectComposition visual hosting.
  add_executable(buddy_spike_transparent WIN32 spikes/transparent_webview/main.cpp)
  target_link_libraries(buddy_spike_transparent PRIVATE unofficial::webview2::webview2 WIL::WIL dcomp dwmapi)
endif()
