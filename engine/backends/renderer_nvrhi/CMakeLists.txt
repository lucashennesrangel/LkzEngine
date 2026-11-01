# Backend gráfico moderno: NVRHI (NVIDIA, MIT) sobre DirectX 12 ou Vulkan 1.3, com janela
# e entrada pela SDL3 (zlib) e shaders em HLSL compilados pelo DXC (DXIL para o D3D12,
# SPIR-V para o Vulkan), embutidos na biblioteca. Quem linka lkz::renderer_nvrhi só
# enxerga os headers <lkz/...>.
include(FetchContent)

# Clone local em third_party/<nome> tem prioridade sobre o download (ver third_party/README.md).
foreach(dependency nvrhi sdl3 stb)
    string(TOUPPER ${dependency} upper)
    if(NOT FETCHCONTENT_SOURCE_DIR_${upper} AND EXISTS "${LkzEngine_SOURCE_DIR}/third_party/${dependency}")
        set(FETCHCONTENT_SOURCE_DIR_${upper} "${LkzEngine_SOURCE_DIR}/third_party/${dependency}")
        message(STATUS "lkz: usando ${dependency} local em third_party/${dependency}")
    endif()
endforeach()

# NVRHI não publica versões: o commit fica fixo. Com MinGW, o backend D3D12 precisa de um
# ajuste nas chamadas que devolvem struct (cmake/patches/nvrhi-mingw.cmake).
set(lkz_nvrhi_patch)
if(MINGW)
    set(lkz_nvrhi_patch PATCH_COMMAND "${CMAKE_COMMAND}" -DNVRHI_DIR=<SOURCE_DIR>
        -P "${LkzEngine_SOURCE_DIR}/cmake/patches/nvrhi-mingw.cmake")
endif()
FetchContent_Declare(
    nvrhi
    GIT_REPOSITORY https://github.com/NVIDIA-RTX/NVRHI.git
    GIT_TAG        10fbb572f3071f29ca1754dd6fbc72e9993fb23d # main de 2026-09-23
    ${lkz_nvrhi_patch}
    SYSTEM
)
FetchContent_Declare(
    sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.16
    GIT_SHALLOW    TRUE
    SYSTEM
)
# stb_image (domínio público): PNG e JPEG das texturas. Também sem versões: commit fixo.
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20 # 2026-08-02
)

set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
foreach(option SDL_RENDER SDL_GPU SDL_CAMERA)
    set(${option} OFF CACHE BOOL "" FORCE) # a SDL só faz janela e entrada aqui
endforeach()
FetchContent_MakeAvailable(nvrhi sdl3 stb)
if(MINGW AND TARGET nvrhi_d3d12)
    target_include_directories(nvrhi_d3d12 PRIVATE "${LkzEngine_SOURCE_DIR}/cmake/patches/nvrhi-mingw")
endif()

# DXC (compilador de HLSL da Microsoft, licença LLVM/MIT): o pacote oficial, com o
# dxil.dll que assina os shaders para o D3D12.
if(CMAKE_HOST_WIN32)
    FetchContent_Declare(
        dxc
        URL      https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip
        URL_HASH SHA256=ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(dxc)
    set(LKZ_DXC "${dxc_SOURCE_DIR}/bin/x64/dxc.exe")
else()
    find_program(LKZ_DXC dxc REQUIRED)
endif()

# Cada shader vira um header com o binário DXIL e o SPIR-V (cmake/CompileShader.cmake).
set(lkz_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(lkz_shader_headers)
function(lkz_nvrhi_shader name source entry profile)
    set(header "${lkz_shader_dir}/${name}.h")
    add_custom_command(
        OUTPUT  "${header}"
        COMMAND "${CMAKE_COMMAND}" -DDXC=${LKZ_DXC} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}
                -DENTRY=${entry} -DPROFILE=${profile} -DNAME=${name} -DOUTPUT=${header}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileShader.cmake"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileShader.cmake"
        COMMENT "HLSL ${source}:${entry} (${profile}) -> DXIL + SPIR-V"
        VERBATIM
    )
    set(lkz_shader_headers ${lkz_shader_headers} "${header}" PARENT_SCOPE)
endfunction()
lkz_nvrhi_shader(mesh_vs Mesh.hlsl mainVS vs_6_0)
lkz_nvrhi_shader(mesh_ps Mesh.hlsl mainPS ps_6_0)
lkz_nvrhi_shader(terrain_vs Terrain.hlsl mainVS vs_6_0)
lkz_nvrhi_shader(terrain_ps Terrain.hlsl mainPS ps_6_0)
lkz_nvrhi_shader(ui_vs Ui.hlsl mainVS vs_6_0)
lkz_nvrhi_shader(ui_ps Ui.hlsl mainPS ps_6_0)

add_library(lkz_renderer_nvrhi STATIC
    src/NvrhiRenderBackend.cpp
    src/GraphicsDevice.cpp
    src/DeviceD3D12.cpp
    src/DeviceVulkan.cpp
    src/VulkanDispatch.cpp
    src/Primitives.cpp
    src/TerrainPass.cpp
    src/NvrhiUiRenderer.cpp
    src/Images.cpp
    src/Models.cpp
    ${lkz_shader_headers}
)
add_library(lkz::renderer_nvrhi ALIAS lkz_renderer_nvrhi)

target_include_directories(lkz_renderer_nvrhi
    PUBLIC  include
    PRIVATE "${lkz_shader_dir}" "${stb_SOURCE_DIR}"
)
if(MINGW)
    target_include_directories(lkz_renderer_nvrhi PRIVATE "${LkzEngine_SOURCE_DIR}/cmake/patches/nvrhi-mingw")
endif()
target_link_libraries(lkz_renderer_nvrhi
    PUBLIC  lkz::core
    PRIVATE lkz::asset lkz_ui_rmlui nvrhi nvrhi_d3d12 nvrhi_vk Vulkan::Headers Microsoft::DirectX-Headers SDL3::SDL3-static
            d3d12 dxgi dxguid
)
lkz_target_defaults(lkz_renderer_nvrhi)
