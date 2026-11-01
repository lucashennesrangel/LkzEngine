# Interface (World::showUi) sobre a RmlUi (MIT), comum aos backends com janela: documento
# RML/RCSS, modelo de dados "ui", eventos e mouse. Cada backend fornece só o desenho
# (ui_rmlui::UiRenderer). A FreeType (licença FTL) lê as fontes; sem zlib/png/harfbuzz
# do sistema, só .ttf/.otf.
include(FetchContent)

# Clone local em third_party/<nome> tem prioridade sobre o download (ver third_party/README.md).
foreach(dependency freetype rmlui)
    string(TOUPPER ${dependency} upper)
    if(NOT FETCHCONTENT_SOURCE_DIR_${upper} AND EXISTS "${LkzEngine_SOURCE_DIR}/third_party/${dependency}/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_${upper} "${LkzEngine_SOURCE_DIR}/third_party/${dependency}")
        message(STATUS "lkz: usando ${dependency} local em third_party/${dependency}")
    endif()
endforeach()

FetchContent_Declare(
    freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG        VER-2-13-3
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_Declare(
    rmlui
    GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
    GIT_TAG        6.2
    GIT_SHALLOW    TRUE
    SYSTEM
)
foreach(option FT_DISABLE_ZLIB FT_DISABLE_BZIP2 FT_DISABLE_PNG FT_DISABLE_HARFBUZZ FT_DISABLE_BROTLI)
    set(${option} ON CACHE BOOL "" FORCE)
endforeach()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(RMLUI_COMPILER_OPTIONS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(freetype)
# A FreeType põe um recurso de versão do Windows (.rc), que pede o compilador de recursos;
# numa biblioteca estática ele não tem uso.
get_target_property(freetype_sources freetype SOURCES)
list(FILTER freetype_sources EXCLUDE REGEX "\\.rc$")
set_target_properties(freetype PROPERTIES SOURCES "${freetype_sources}")
# A RmlUi procura a FreeType pelo alvo Freetype::Freetype; o build local da FreeType o
# fornece (ou fornecemos aqui).
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()
FetchContent_MakeAvailable(rmlui)

add_library(lkz_ui_rmlui STATIC src/UiLayer.cpp)
target_include_directories(lkz_ui_rmlui PUBLIC include)
target_link_libraries(lkz_ui_rmlui PUBLIC lkz::core RmlUi::Core RmlUi::Debugger)
lkz_target_defaults(lkz_ui_rmlui)
