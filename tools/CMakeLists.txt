# Ferramentas da engine (não são jogos): cada uma lê um arquivo de projeto do jogo que a usa.
if(TARGET lkz::renderer_raylib)
    add_subdirectory(painter) # Pintor de modelos: pinta um glTF em 3D e exporta o .glb
endif()
