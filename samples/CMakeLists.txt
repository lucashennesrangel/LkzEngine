# Samples exercitam a engine isoladamente (testes de fumaça, benchmarks, demos de API).
# Não são jogos: jogo de verdade fica num repositório próprio que consome a engine.
add_subdirectory(sandbox)
add_subdirectory(uv_bake) # lkz::asset: UV único + bake de um glTF, sem janela

if(TARGET lkz::renderer_raylib)
    add_subdirectory(window)
    add_subdirectory(gltf_roundtrip) # lkz::asset: lê, regrava e compara um glTF
    add_subdirectory(texture_paint) # World::uploadTexture / updateTexture: pincel em tempo real
endif()

if(TARGET lkz::renderer_raylib AND TARGET lkz::physics)
    add_subdirectory(physics)
endif()
