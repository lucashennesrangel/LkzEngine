lkz_add_game(window
    SOURCES   main.cpp
    LIBRARIES lkz::renderer_raylib
)
# Com o backend NVRHI compilado, `window --d3d12` ou `window --vulkan` desenha a mesma cena nele.
if(TARGET lkz::renderer_nvrhi)
    target_link_libraries(window PRIVATE lkz::renderer_nvrhi)
    target_compile_definitions(window PRIVATE LKZ_WITH_NVRHI=1)
endif()

# Imagens do sample ao lado do executável.
add_custom_command(TARGET window POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:window>/assets)
