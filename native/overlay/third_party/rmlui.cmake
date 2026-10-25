# RmlUi's core with its SVG plugin, the part of FreeType its font engine needs, and lunasvg on
# plutovg for the SVG, as static libraries built with our settings rather than through their own
# build systems: no samples, no backends, no Lua, nothing fetched or generated at configure time.

set(FREETYPE_DIR "${CMAKE_CURRENT_LIST_DIR}/freetype")
set(RMLUI_DIR "${CMAKE_CURRENT_LIST_DIR}/RmlUi")

# TrueType and OpenType only; the list is in freetype_config/freetype_modules.h.
add_library(freetype_core STATIC
    "${FREETYPE_DIR}/src/base/ftsystem.c"
    "${FREETYPE_DIR}/src/base/ftinit.c"
    "${FREETYPE_DIR}/src/base/ftdebug.c"
    "${FREETYPE_DIR}/src/base/ftbase.c"
    "${FREETYPE_DIR}/src/base/ftbitmap.c"
    "${FREETYPE_DIR}/src/base/ftglyph.c"
    "${FREETYPE_DIR}/src/base/ftmm.c"
    "${FREETYPE_DIR}/src/autofit/autofit.c"
    "${FREETYPE_DIR}/src/truetype/truetype.c"
    "${FREETYPE_DIR}/src/cff/cff.c"
    "${FREETYPE_DIR}/src/psaux/psaux.c"
    "${FREETYPE_DIR}/src/psnames/psnames.c"
    "${FREETYPE_DIR}/src/pshinter/pshinter.c"
    "${FREETYPE_DIR}/src/sfnt/sfnt.c"
    "${FREETYPE_DIR}/src/smooth/smooth.c"
    "${FREETYPE_DIR}/src/gzip/ftgzip.c"
)
target_include_directories(freetype_core
    PUBLIC "${FREETYPE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/freetype_config"
)
target_compile_definitions(freetype_core PRIVATE
    "FT_CONFIG_MODULES_H=<freetype_modules.h>" FT2_BUILD_LIBRARY _CRT_SECURE_NO_WARNINGS
)
target_compile_options(freetype_core PRIVATE /W0)

# SVG drawing for the <svg> element. Neither scans the system for fonts: an SVG's text is not
# what the overlay draws SVG for.
set(PLUTOVG_DIR "${CMAKE_CURRENT_LIST_DIR}/plutovg")
file(GLOB PLUTOVG_SOURCES CONFIGURE_DEPENDS "${PLUTOVG_DIR}/source/*.c")
add_library(plutovg_core STATIC ${PLUTOVG_SOURCES})
target_include_directories(plutovg_core PUBLIC "${PLUTOVG_DIR}/include")
target_compile_definitions(plutovg_core
    PUBLIC PLUTOVG_BUILD_STATIC
    PRIVATE PLUTOVG_BUILD PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD _CRT_SECURE_NO_WARNINGS
)
target_compile_options(plutovg_core PRIVATE /W0)

set(LUNASVG_DIR "${CMAKE_CURRENT_LIST_DIR}/lunasvg")
file(GLOB LUNASVG_SOURCES CONFIGURE_DEPENDS "${LUNASVG_DIR}/source/*.cpp")
add_library(lunasvg_core STATIC ${LUNASVG_SOURCES})
target_include_directories(lunasvg_core PUBLIC "${LUNASVG_DIR}/include")
target_compile_definitions(lunasvg_core
    PUBLIC LUNASVG_BUILD_STATIC
    PRIVATE LUNASVG_BUILD LUNASVG_DISABLE_LOAD_SYSTEM_FONTS _CRT_SECURE_NO_WARNINGS
)
target_compile_options(lunasvg_core PRIVATE /W0 /utf-8)
target_link_libraries(lunasvg_core PRIVATE plutovg_core)

file(GLOB RMLUI_SOURCES CONFIGURE_DEPENDS
    "${RMLUI_DIR}/Source/Core/*.cpp"
    "${RMLUI_DIR}/Source/Core/Elements/*.cpp"
    "${RMLUI_DIR}/Source/Core/Layout/*.cpp"
    "${RMLUI_DIR}/Source/Core/FontEngineDefault/*.cpp"
    "${RMLUI_DIR}/Source/SVG/*.cpp"
)

add_library(rmlui_core STATIC ${RMLUI_SOURCES})
target_include_directories(rmlui_core PUBLIC "${RMLUI_DIR}/Include")
target_compile_definitions(rmlui_core
    PUBLIC RMLUI_STATIC_LIB RMLUI_FONT_ENGINE_FREETYPE RMLUI_SVG_PLUGIN
    PRIVATE RMLUI_VERSION="6.3" _CRT_SECURE_NO_WARNINGS
)
target_compile_options(rmlui_core PRIVATE /utf-8 /MP /permissive-)
target_precompile_headers(rmlui_core PRIVATE "${RMLUI_DIR}/Source/Core/precompiled.h")
target_link_libraries(rmlui_core PRIVATE freetype_core lunasvg_core)
