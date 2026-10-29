# renderer_bgfx (a fazer)

Trilha A do plano: bgfx via [bgfx.cmake](https://github.com/bkaradzic/bgfx.cmake), que abstrai Vulkan, DX12, Metal e OpenGL.

Para implementar:

1. `CMakeLists.txt` criando `lkz_renderer_bgfx` (alias `lkz::renderer_bgfx`), puxando bgfx.cmake via FetchContent.
2. Uma classe `BgfxRenderBackend : lkz::RenderBackend` (veja `engine/core/include/lkz/Render/RenderBackend.hpp`).
3. Descomentar `add_subdirectory(renderer_bgfx)` em `engine/backends/CMakeLists.txt`.

Atenção à thread: no Windows, a janela e o loop de mensagens ficam na thread que criou a janela (a principal). Com bgfx, a thread principal chama `bgfx::renderFrame()` antes de `bgfx::init()`, e a thread de render do `World` passa a ser a "API thread" do bgfx. A projeção deve respeitar `bgfx::getCaps()->homogeneousDepth`.
