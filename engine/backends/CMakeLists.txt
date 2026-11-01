# Cada backend é uma biblioteca separada; o jogo escolhe quais linkar.
add_subdirectory(renderer_null)

# Interface (RmlUi), comum aos backends com janela.
if(LKZ_BUILD_RENDERER_RAYLIB OR LKZ_BUILD_RENDERER_NVRHI)
    add_subdirectory(ui_rmlui)
endif()

if(LKZ_BUILD_RENDERER_RAYLIB)
    add_subdirectory(renderer_raylib)
endif()

if(LKZ_BUILD_RENDERER_NVRHI)
    add_subdirectory(renderer_nvrhi)
endif()

# Próximos passos (ver README de cada pasta):
# add_subdirectory(renderer_bgfx)
# add_subdirectory(physics_jolt)
