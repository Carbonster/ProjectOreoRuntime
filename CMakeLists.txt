cmake_minimum_required(VERSION 3.20)
project(project_oreo_runtime LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Project Oreo Runtime currently supports Windows with MSVC only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "A 64-bit build is required. Configure with -A x64.")
endif()

set(OREO_RUNTIME_OUTPUT_DIR "${CMAKE_BINARY_DIR}/runtime/$<CONFIG>")

add_subdirectory(native/overlay)
add_subdirectory(native/controller_support)

set_target_properties(oreo_overlay oreo_controller_support PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OREO_RUNTIME_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${OREO_RUNTIME_OUTPUT_DIR}"
)
