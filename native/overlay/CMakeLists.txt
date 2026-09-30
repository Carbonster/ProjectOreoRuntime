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

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
set(VULKAN_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Headers/include")

add_subdirectory(third_party/minhook)

add_library(oreo_overlay SHARED
    src/dllmain.cpp
    src/ui_document.cpp
    src/version.rc
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(oreo_overlay PRIVATE
    src
    ${VULKAN_INCLUDE}
    third_party/minhook/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_compile_definitions(oreo_overlay PRIVATE
    VK_NO_PROTOTYPES
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
    # The gamepad reaches ImGui from the Runtime's own controller helper, which reads Xbox and
    # PlayStation pads alike. The backend's XInput reading would only see the former, twice.
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
)

target_compile_options(oreo_overlay PRIVATE /utf-8)
set_source_files_properties(src/dllmain.cpp PROPERTIES COMPILE_OPTIONS "/W4")

target_link_libraries(oreo_overlay PRIVATE minhook)
set_target_properties(oreo_overlay PROPERTIES OUTPUT_NAME "oreo_overlay")

# The document reader is the only part of the Runtime that parses input it did not write, and
# the pipe is reachable by any process running as this user. It gets its own test program:
# a crash in there is a crash inside somebody's game.
#
# ImGui is compiled in rather than linked because the model uses its colour and vector types;
# nothing here creates a context or draws.
add_executable(ui_document_test
    tests/ui_document_test.cpp
    src/ui_document.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)
target_include_directories(ui_document_test PRIVATE src ${IMGUI_DIR})
target_compile_definitions(ui_document_test PRIVATE IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
target_compile_options(ui_document_test PRIVATE /utf-8 /W4)
