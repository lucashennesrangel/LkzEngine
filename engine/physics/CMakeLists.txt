# Física (lkz::physics): Jolt Physics (MIT) atrás de uma API da engine. Quem linka só vê
# <lkz/Physics/...>; os headers do Jolt ficam no .cpp.
include(FetchContent)

if(NOT FETCHCONTENT_SOURCE_DIR_JOLTPHYSICS AND EXISTS "${LkzEngine_SOURCE_DIR}/third_party/JoltPhysics/Build/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_JOLTPHYSICS "${LkzEngine_SOURCE_DIR}/third_party/JoltPhysics")
    message(STATUS "lkz: usando JoltPhysics local em third_party/JoltPhysics")
endif()

FetchContent_Declare(
    JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG        v5.5.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  Build
    SYSTEM
)
# O CMake do Jolt mexe em opções globais por padrão: aqui ele só compila a biblioteca.
foreach(option OVERRIDE_CXX_FLAGS INTERPROCEDURAL_OPTIMIZATION ENABLE_ALL_WARNINGS ENABLE_INSTALL
               FLOATING_POINT_EXCEPTIONS_ENABLED PROFILER_IN_DEBUG_AND_RELEASE USE_ASSERTS
               GENERATE_DEBUG_SYMBOLS)
    set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_MakeAvailable(JoltPhysics)

add_library(lkz_physics STATIC
    src/PhysicsWorld.cpp
)
add_library(lkz::physics ALIAS lkz_physics)

target_include_directories(lkz_physics PUBLIC include)
target_link_libraries(lkz_physics
    PUBLIC  lkz::core
    PRIVATE Jolt
)
lkz_target_defaults(lkz_physics)
