lkz_add_game(texture_paint
    SOURCES   main.cpp
    LIBRARIES lkz::renderer_raylib
)
# `texture_paint --d3d12` ou `--vulkan` pinta no backend NVRHI.
if(TARGET lkz::renderer_nvrhi)
    target_link_libraries(texture_paint PRIVATE lkz::renderer_nvrhi)
    target_compile_definitions(texture_paint PRIVATE LKZ_WITH_NVRHI=1)
endif()
