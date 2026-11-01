# Testes da engine (ctest --preset ... ou ctest --test-dir build/<preset>).
add_executable(terrain_test terrain_test.cpp)
target_link_libraries(terrain_test PRIVATE lkz::core)
lkz_target_defaults(terrain_test)
add_test(NAME terrain COMMAND terrain_test)

add_executable(asset_test asset_test.cpp)
target_link_libraries(asset_test PRIVATE lkz::asset)
lkz_target_defaults(asset_test)
add_test(NAME asset COMMAND asset_test)

if(TARGET lkz::physics)
    add_executable(physics_test physics_test.cpp)
    target_link_libraries(physics_test PRIVATE lkz::physics)
    lkz_target_defaults(physics_test)
    add_test(NAME physics COMMAND physics_test)
endif()

# O documento do Pintor de modelos (tools/painter), sem janela.
if(TARGET painter)
    add_executable(painter_test painter_test.cpp ${LkzEngine_SOURCE_DIR}/tools/painter/Document.cpp
                            ${LkzEngine_SOURCE_DIR}/tools/painter/Palette.cpp)
    target_include_directories(painter_test PRIVATE ${LkzEngine_SOURCE_DIR}/tools/painter)
    target_link_libraries(painter_test PRIVATE lkz::asset)
    lkz_target_defaults(painter_test)
    add_test(NAME painter COMMAND painter_test)
endif()
