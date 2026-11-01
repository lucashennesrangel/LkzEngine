# Backend gráfico com raylib (OpenGL 3.3 + GLFW embutido): janela e desenho
# instanciado de primitivas, malhas do jogo e modelos 3D. A raylib fica escondida
# no .cpp; quem linka lkz::renderer_raylib só enxerga os headers <lkz/...>.
include(FetchContent)

# Clone local em third_party/<nome> tem prioridade sobre o download (ver third_party/README.md).
if(NOT FETCHCONTENT_SOURCE_DIR_RAYLIB AND EXISTS "${LkzEngine_SOURCE_DIR}/third_party/raylib/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_RAYLIB "${LkzEngine_SOURCE_DIR}/third_party/raylib")
    message(STATUS "lkz: usando raylib local em third_party/raylib")
endif()
foreach(dependency meshoptimizer)
    string(TOUPPER ${dependency} upper)
    if(NOT FETCHCONTENT_SOURCE_DIR_${upper} AND EXISTS "${LkzEngine_SOURCE_DIR}/third_party/${dependency}/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_${upper} "${LkzEngine_SOURCE_DIR}/third_party/${dependency}")
        message(STATUS "lkz: usando ${dependency} local em third_party/${dependency}")
    endif()
endforeach()

FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        6.2
    GIT_SHALLOW    TRUE
    SYSTEM
)
# meshoptimizer (MIT): simplificação de malhas para os níveis de detalhe (LOD).
FetchContent_Declare(
    meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG        v0.22
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(raylib meshoptimizer)

# Modelos 3D (glTF) costumam trazer as imagens em JPEG, que a raylib deixa desligado
# por padrão. O config.h dela só define o valor se ninguém definiu antes.
target_compile_definitions(raylib PRIVATE SUPPORT_FILEFORMAT_JPG=1)

add_library(lkz_renderer_raylib STATIC
    src/RaylibRenderBackend.cpp
    src/Impostor.cpp
    src/MeshSimplify.cpp
    src/RlglUiRenderer.cpp
    src/TerrainRenderer.cpp
)
add_library(lkz::renderer_raylib ALIAS lkz_renderer_raylib)

target_include_directories(lkz_renderer_raylib PUBLIC include)
target_link_libraries(lkz_renderer_raylib
    PUBLIC  lkz::core
    PRIVATE raylib meshoptimizer lkz_ui_rmlui lkz::asset
)
lkz_target_defaults(lkz_renderer_raylib)
