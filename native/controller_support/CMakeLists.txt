cmake_minimum_required(VERSION 3.20)
project(oreo_controller_support LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Controller support currently supports Windows with MSVC only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "A 64-bit build is required. Configure with -A x64.")
endif()

set(SDL2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2" CACHE PATH
    "Path to the SDL2 development package")
set(SDL2_INCLUDE "${SDL2_ROOT}/include")
set(SDL2_LIB "${SDL2_ROOT}/lib/x64/SDL2.lib")
set(SDL2_DLL "${SDL2_ROOT}/lib/x64/SDL2.dll")

if(NOT EXISTS "${SDL2_INCLUDE}/SDL.h" OR
   NOT EXISTS "${SDL2_LIB}" OR
   NOT EXISTS "${SDL2_DLL}")
    message(FATAL_ERROR "SDL2 headers, import library, or runtime DLL are missing under ${SDL2_ROOT}.")
endif()

add_executable(oreo_controller_support src/main.cpp src/version.rc)
target_include_directories(oreo_controller_support PRIVATE ${SDL2_INCLUDE})
target_link_libraries(oreo_controller_support PRIVATE ${SDL2_LIB})
target_link_options(oreo_controller_support PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
set_source_files_properties(src/main.cpp PROPERTIES COMPILE_OPTIONS "/W4")

add_custom_command(TARGET oreo_controller_support POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SDL2_DLL}" "$<TARGET_FILE_DIR:oreo_controller_support>"
)
