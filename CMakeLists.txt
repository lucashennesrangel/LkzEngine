cmake_minimum_required(VERSION 3.28)
project(LkzEngine VERSION 0.1.0 LANGUAGES CXX)

# A engine funciona de dois jeitos:
# - aberta sozinha (top level): compila as bibliotecas e os samples;
# - consumida por um jogo (FetchContent/add_subdirectory): só as bibliotecas.
# Nada aqui pode depender de um jogo específico.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # sem módulos C++20 por enquanto: evita o passo de scan no build

option(LKZ_ENABLE_PROFILING "Instrumentação com Tracy (TRACY_ENABLE)" ON)
option(LKZ_BUILD_SAMPLES "Compila os exemplos em samples/" ${PROJECT_IS_TOP_LEVEL})
option(LKZ_BUILD_RENDERER_RAYLIB "Backend gráfico lkz::renderer_raylib (baixa a raylib)" ON)
option(LKZ_BUILD_RENDERER_NVRHI "Backend gráfico lkz::renderer_nvrhi: DirectX 12 / Vulkan (baixa NVRHI, SDL3, DXC)" ON)
option(LKZ_BUILD_PHYSICS "Física lkz::physics (baixa o Jolt Physics)" ON)
option(LKZ_BUILD_TESTS "Compila os testes em tests/ (ctest)" ${PROJECT_IS_TOP_LEVEL})
# Ferramentas (Pintor de modelos...): um jogo que as usa liga isto antes de incluir a engine.
option(LKZ_BUILD_TOOLS "Compila as ferramentas em tools/" ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    # Executáveis e DLLs na mesma pasta, independente do gerador.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/Dependencies.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/LkzEngine.cmake)

add_subdirectory(engine/core)
add_subdirectory(engine/asset) # glTF, imagens, UV, bake (sem render)
add_subdirectory(engine/backends)
if(LKZ_BUILD_PHYSICS)
    add_subdirectory(engine/physics)
endif()

if(LKZ_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

if(LKZ_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(LKZ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
