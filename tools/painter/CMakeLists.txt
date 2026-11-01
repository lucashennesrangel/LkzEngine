# painter [projeto.json] [modelo.glb] [--d3d12 | --vulkan]
lkz_add_game(painter
    SOURCES   main.cpp Document.cpp OrbitCamera.cpp Palette.cpp Project.cpp
    LIBRARIES lkz::asset lkz::renderer_raylib
)
if(TARGET lkz::renderer_nvrhi)
    target_link_libraries(painter PRIVATE lkz::renderer_nvrhi)
    target_compile_definitions(painter PRIVATE LKZ_WITH_NVRHI=1)
endif()

# A interface ao lado do executável, numa pasta própria (o executável pode morar junto com
# o de um jogo, que tem a sua pasta assets).
add_custom_command(TARGET painter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/ui $<TARGET_FILE_DIR:painter>/painter-ui)
