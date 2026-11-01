# lkz::asset: ler, gravar e preparar arte fora do render (sem raylib, sem GPU). glTF (cgltf),
# imagens (stb), e o que o Pintor de modelos usa: desembrulhar UV (xatlas), assar texturas,
# otimizar malhas (meshoptimizer). Qualquer ferramenta ou backend linka isto.
include(FetchContent)

# Clone local em third_party/<nome> tem prioridade sobre o download (ver third_party/README.md).
foreach(dependency cgltf stb xatlas meshoptimizer nlohmann_json)
    string(TOUPPER ${dependency} upper)
    if(NOT FETCHCONTENT_SOURCE_DIR_${upper} AND EXISTS "${LkzEngine_SOURCE_DIR}/third_party/${dependency}")
        set(FETCHCONTENT_SOURCE_DIR_${upper} "${LkzEngine_SOURCE_DIR}/third_party/${dependency}")
        message(STATUS "lkz: usando ${dependency} local em third_party/${dependency}")
    endif()
endforeach()

# cgltf (MIT): só o header (SOURCE_SUBDIR inexistente: o CMake dele traria os testes).
FetchContent_Declare(
    cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.15
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  lkz-sem-cmake
)
# stb (domínio público): stb_image e stb_image_write. Sem versões: commit fixo.
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20 # 2026-08-02
)
# xatlas (MIT): cartas de UV, parametrização e empacotamento. Sem versões: commit fixo.
FetchContent_Declare(
    xatlas
    GIT_REPOSITORY https://github.com/jpcy/xatlas.git
    GIT_TAG        f700c7790aaa030e794b52ba7791a05c085faf0c # 2022-07-25
    SOURCE_SUBDIR  lkz-sem-cmake
)
# meshoptimizer (MIT): soldar vértices, ordem de cache, simplificação (o backend raylib usa
# a mesma declaração para os LODs).
FetchContent_Declare(
    meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG        v0.22
    GIT_SHALLOW    TRUE
    SYSTEM
)
# nlohmann/json (MIT): arquivos de projeto, paleta e catálogo das ferramentas e dos jogos.
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(cgltf stb xatlas meshoptimizer nlohmann_json)

add_library(lkz_asset STATIC
    src/Gltf.cpp
    src/Image.cpp
    src/Raycast.cpp
    src/Unwrap.cpp
    src/Bake.cpp
    "${xatlas_SOURCE_DIR}/source/xatlas/xatlas.cpp"
)
add_library(lkz::asset ALIAS lkz_asset)
# O xatlas é código de terceiros: compila sem os avisos da engine.
set_source_files_properties("${xatlas_SOURCE_DIR}/source/xatlas/xatlas.cpp" PROPERTIES COMPILE_OPTIONS "-w")

target_include_directories(lkz_asset
    PUBLIC  include
    PRIVATE src "${cgltf_SOURCE_DIR}" "${stb_SOURCE_DIR}" "${xatlas_SOURCE_DIR}/source/xatlas"
)
# A json é pública: quem usa lkz::asset (Pintor, jogos) lê os seus .json com a mesma versão.
target_link_libraries(lkz_asset PUBLIC lkz::core nlohmann_json::nlohmann_json PRIVATE meshoptimizer)
lkz_target_defaults(lkz_asset)
