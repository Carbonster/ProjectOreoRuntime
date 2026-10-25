cmake_minimum_required(VERSION 3.20)
project(oreo_overlay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The overlay currently supports Windows with MSVC only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "A 64-bit build is required. Configure with -A x64.")
endif()

set(VULKAN_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Headers/include")

add_subdirectory(third_party/minhook)
include(third_party/rmlui.cmake)

# Everything that reads what arrives over the pipe, testable without a game.
set(WIRE_SOURCES
    src/input/active_device.cpp
    src/input/pad_input.cpp
    src/ipc/json_scalar.cpp
    src/ipc/protocol.cpp
    src/ui/file_access.cpp
    src/ui/rml_source.cpp
    src/ui/ui_commands.cpp
)

# Mods' documents in RmlUi, testable without a GPU.
set(DOCUMENT_SOURCES
    src/ipc/outbox.cpp
    src/ui/documents.cpp
    src/ui/element_events.cpp
    src/ui/polygon_element.cpp
    src/ui/rml_system.cpp
    src/ui/visor_filter.cpp
)

set(OVERLAY_SOURCES
    ${WIRE_SOURCES}
    ${DOCUMENT_SOURCES}
    src/dllmain.cpp
    src/diagnostics.cpp
    src/game/game_image.cpp
    src/game/game_input.cpp
    src/game/game_window.cpp
    src/input/rml_input.cpp
    src/input/window_input.cpp
    src/ipc/pipe_server.cpp
    src/render/gpu_commands.cpp
    src/render/gpu_context.cpp
    src/render/gpu_memory.cpp
    src/render/gpu_timer.cpp
    src/render/overlay_renderer.cpp
    src/render/rml/filters.cpp
    src/render/rml/frame_arena.cpp
    src/render/rml/gradients.cpp
    src/render/rml/graveyard.cpp
    src/render/rml/image_decoder.cpp
    src/render/rml/pipelines.cpp
    src/render/rml/recorder.cpp
    src/render/rml/rml_renderer.cpp
    src/render/rml/targets.cpp
    src/render/rml/textures.cpp
    src/render/surface_query.cpp
    src/render/vk_functions.cpp
    src/render/vk_hooks.cpp
    src/ui/ui_frame.cpp
    src/ui/ui_runtime.cpp
)

add_library(oreo_overlay SHARED ${OVERLAY_SOURCES} src/version.rc)
target_include_directories(oreo_overlay PRIVATE src ${VULKAN_INCLUDE} third_party/minhook/include)
target_compile_definitions(oreo_overlay PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
target_compile_options(oreo_overlay PRIVATE /utf-8 /W4)
target_link_libraries(oreo_overlay PRIVATE minhook rmlui_core windowscodecs ole32)
set_target_properties(oreo_overlay PROPERTIES OUTPUT_NAME "oreo_overlay")

# The pipe is reachable by any process running as this user, and a crash in what reads it is a
# crash inside somebody's game: that half gets a test program of its own.
add_executable(overlay_tests tests/wire_test.cpp ${WIRE_SOURCES})
target_include_directories(overlay_tests PRIVATE src)
target_compile_options(overlay_tests PRIVATE /utf-8 /W4)
target_link_libraries(overlay_tests PRIVATE rmlui_core)

add_executable(documents_test tests/documents_test.cpp ${WIRE_SOURCES} ${DOCUMENT_SOURCES})
target_include_directories(documents_test PRIVATE src tests)
target_compile_definitions(documents_test PRIVATE
    OREO_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../src/oreo_runtime/assets"
    OREO_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../examples")
target_compile_options(documents_test PRIVATE /utf-8 /W4)
target_link_libraries(documents_test PRIVATE rmlui_core)

# A modder's tool: what RmlUi says about a document, without starting the game.
add_executable(rml_check tools/rml_check.cpp ${WIRE_SOURCES} ${DOCUMENT_SOURCES})
target_include_directories(rml_check PRIVATE src tests)
target_compile_options(rml_check PRIVATE /utf-8 /W4)
target_link_libraries(rml_check PRIVATE rmlui_core)
