# renderer_nvrhi

Backend gráfico moderno da engine: [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) (NVIDIA, MIT) sobre **DirectX 12** ou **Vulkan 1.3**, com janela e entrada pela [SDL3](https://libsdl.org) (zlib). O alvo é `lkz::renderer_nvrhi`, e o header público é `<lkz/backends/NvrhiRenderBackend.hpp>`. Ele recebe o mesmo `lkz::RenderConfig` do backend raylib, então trocar de backend é trocar uma linha:

```cpp
lkz::RenderConfig config;          // o mesmo do RaylibRenderBackend (RaylibConfig é outro nome dele)
config.meshes = {/* ... */};
auto backend = std::make_unique<lkz::NvrhiRenderBackend>(std::move(config), lkz::GraphicsApi::Vulkan);
// ou lkz::GraphicsApi::D3D12; o terceiro argumento (true) liga a validação
```

`lkz::parseGraphicsApi("d3d12" | "vulkan")` converte o nome dado na linha de comando. O sample `window` aceita `--d3d12` e `--vulkan` (mais `--debug`).

## O que já desenha

- **Malhas:** primitivas (caixa, esfera, cilindro, plano), malhas do jogo (`World::uploadMesh`) e modelos glTF (cgltf). No glTF, cada primitiva do arquivo vira uma parte, com a imagem e a cor base do material. Um draw call instanciado por parte, com cor e transformação de UV por cópia.
- **Imagens:** PNG e JPEG (stb_image), com mipmaps e filtro anisotrópico. Repetição em grade comum ou em hex-tiling (`TextureTiling::Hex`), com o mesmo shader do backend raylib, portado para HLSL.
- **Luz:** a mesma do backend raylib (direcional + ambiente, sem correção de gama), para a cena sair igual nos dois.
- **Lotes de cópias** (`World::setInstanceBatch`): buffer fixo na GPU, em células do plano XZ. Por frame, só as células dentro da câmera são desenhadas, e células seguidas no buffer viram um único draw call.
- **Terreno CDLOD** (`World::setTerrain` / `updateTerrain`): a mesma seleção da engine (`lkz::TerrainLod`) e o mesmo shader. A mudança de relevo do editor sobe para a GPU só no retângulo mudado. F3 colore os níveis.
- **Interface** (`World::showUi`): a mesma RmlUi do backend raylib, pelo módulo comum `ui_rmlui`; este backend só faz o desenho. F8 abre o depurador da RmlUi.
- **Profundidade invertida** (reversed-Z, 32 bits): boa precisão de perto e de longe, o que importa num mapa de 18 km com ruas 30 cm acima do chão.

## Ainda não tem

LOD e impostores (desenha sempre o original), texto sobre a cena (`showStats` e `World::drawOverlay`) e MSAA. Para esses, o backend raylib continua sendo o completo.

## Como é montado

- **Device** (`src/DeviceD3D12.cpp`, `src/DeviceVulkan.cpp`):
  - cria o device, a fila e a swapchain da janela da SDL, e põe o device do NVRHI por cima;
  - no Vulkan, as funções vêm do driver na hora, sem precisar do Vulkan SDK;
  - no D3D12, sem vsync, usa tearing quando o sistema permite;
  - janela minimizada fica sem swapchain até voltar.
- **Shaders** (`shaders/*.hlsl`): o DXC (pacote oficial v1.9.2609, baixado pelo CMake) compila cada ponto de entrada para DXIL e SPIR-V (`cmake/CompileShader.cmake`). Os binários ficam embutidos na biblioteca, sem arquivos ao lado do executável. No SPIR-V, os registradores seguem o deslocamento padrão do NVRHI (t 0, s 128, b 256, u 384).
- **Frames em voo:** no máximo 2, controlados por event queries. Todas as listas de comandos são não imediatas, porque a interface envia geometria nova no meio do frame.
- **Validação:** com `debug`, liga as camadas do NVRHI, mais a do D3D12 ou do Vulkan quando instaladas (Ferramentas Gráficas do Windows ou Vulkan SDK). Os erros vão para o log.

## Dependências

| Dependência | Versão | Para quê |
|---|---|---|
| NVRHI | commit `10fbb57` (main de 2026-09-23; o projeto não publica versões) | a camada sobre D3D12 e Vulkan |
| Vulkan-Headers, DirectX-Headers | as que o NVRHI fixa | headers das APIs |
| SDL3 | `release-3.4.16` | janela, teclado e mouse |
| DXC | v1.9.2609 (zip oficial, com hash conferido) | HLSL → DXIL e SPIR-V, no build |
| stb_image | commit `2c980bb` | PNG e JPEG |
| cgltf | v1.15 | glTF |

Com MinGW (o toolchain da engine), o backend D3D12 do NVRHI precisa de dois ajustes, aplicados no download por `cmake/patches/nvrhi-mingw.cmake`. Rodar o script de novo não muda nada.
- **Métodos COM que devolvem struct** (`GetDesc`, `GetCPUDescriptorHandleForHeapStart`...): fora do MSVC eles têm a forma com ponteiro de saída, e as chamadas são reescritas nessa forma.
- **`__uuidof` das interfaces:** passa a vir do `dxguids.h` do DirectX-Headers (`cmake/patches/nvrhi-mingw/`).

A raylib embute as próprias cópias do stb_image e do cgltf. Para os dois backends caberem no mesmo executável, a cópia deste backend fica com escopo de arquivo (stb) ou ganha um prefixo (cgltf, `src/CgltfPrefix.hpp`).
