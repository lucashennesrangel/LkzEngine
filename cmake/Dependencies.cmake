# Dependências do núcleo, baixadas e compiladas no configure via FetchContent.
include(FetchContent)

# Clone local em third_party/<nome> tem prioridade sobre o download (trabalho offline
# ou testes com um fork). Também dá para apontar FETCHCONTENT_SOURCE_DIR_<NOME> na mão.
foreach(dep IN ITEMS entt glm spdlog tracy)
    string(TOUPPER ${dep} dep_upper)
    if(NOT FETCHCONTENT_SOURCE_DIR_${dep_upper} AND EXISTS "${LkzEngine_SOURCE_DIR}/third_party/${dep}/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_${dep_upper} "${LkzEngine_SOURCE_DIR}/third_party/${dep}")
        message(STATUS "lkz: usando ${dep} local em third_party/${dep}")
    endif()
endforeach()

# --- EnTT (exige C++20 e CMake >= 3.28) ---
FetchContent_Declare(
    entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG        v4.0.0
    GIT_SHALLOW    TRUE
    SYSTEM
)

# --- GLM (header-only) ---
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.3
    GIT_SHALLOW    TRUE
    SYSTEM
)

# --- spdlog ---
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE
    SYSTEM
)

# --- Tracy ---
# O cliente precisa ter a MESMA versão do profiler (GUI) usado para capturar.
# TRACY_ON_DEMAND: só coleta dados com o profiler conectado, então rodar o jogo
# sem o profiler aberto não acumula memória.
set(TRACY_ENABLE ${LKZ_ENABLE_PROFILING} CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "")
FetchContent_Declare(
    tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.13.1
    GIT_SHALLOW    TRUE
    SYSTEM
)

FetchContent_MakeAvailable(entt glm spdlog tracy)

find_package(Threads REQUIRED)
# Alvos importados por find_package só são visíveis neste diretório; o jogo que
# linka lkz::core precisa enxergar Threads::Threads também.
set_target_properties(Threads::Threads PROPERTIES IMPORTED_GLOBAL TRUE)
