add_library(lkz_core STATIC
    src/Core/Log.cpp
    src/Scene/Entity.cpp
    src/Scene/SceneNode.cpp
    src/Scene/Camera.cpp
    src/Systems/PhysicsSystem.cpp
    src/Systems/RenderSystem.cpp
    src/Terrain/Drape.cpp
    src/Terrain/Heightmap.cpp
    src/Terrain/TerrainLod.cpp
    src/World.cpp
)
add_library(lkz::core ALIAS lkz_core)

target_include_directories(lkz_core PUBLIC include)
target_compile_features(lkz_core PUBLIC cxx_std_20)
target_link_libraries(lkz_core
    PUBLIC
        EnTT::EnTT
        glm::glm
        spdlog::spdlog
        Tracy::TracyClient
        Threads::Threads
)
lkz_target_defaults(lkz_core)
