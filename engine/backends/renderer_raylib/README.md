# renderer_raylib

Backend com janela sobre a [raylib](https://github.com/raysan5/raylib) 6.2 (OpenGL 3.3, GLFW embutido). Alvo `lkz::renderer_raylib`, header `<lkz/backends/RaylibRenderBackend.hpp>`. A raylib fica só no `.cpp`: quem usa o backend não inclui `<raylib.h>`.

```cpp
lkz::RaylibConfig config;
config.title = "Meu jogo";
config.meshes = {
    // id de RenderComponent::mesh -> primitiva, tamanho (caixa envolvente), deslocamento
    {1, {lkz::PrimitiveShape::Cylinder, {0.5f, 1.8f, 0.5f}, {0.0f, 0.9f, 0.0f}}}, // apoiado no chão
    {2, {lkz::PrimitiveShape::Box, {1.0f, 0.2f, 1.0f}}},                          // placa (escala vem do nó)
    {3, {lkz::PrimitiveShape::Plane, glm::vec3{1.0f}, glm::vec3{0.0f}, "assets/chao.png"}}, // chão com imagem
    {4, {lkz::PrimitiveShape::Plane, glm::vec3{1.0f}, glm::vec3{0.0f}, "assets/grama.png",
         lkz::TextureTiling::Hex}},                                                    // repete sem grade visível
};
lkz::World world{std::make_unique<lkz::RaylibRenderBackend>(std::move(config))};
world.run(); // até fechar a janela ou apertar ESC
```

## Como desenha

- **Geometria:** cada id configurado vira uma primitiva (`Box`, `Sphere`, `Cylinder`, `Plane`) gerada uma vez em `initialize()`, com tamanho e deslocamento já aplicados aos vértices. Um id sem entrada vira um cubo unitário (com um aviso no log, uma vez só).
- **Malha do jogo:** com `PrimitiveShape::Custom`, a geometria vem de `world.uploadMesh(id, lkz::MeshData{...})` (posições, normais, UVs, índices; `<lkz/Render/MeshData.hpp>`). Pode ser trocada a qualquer momento (ex.: a cada edição de um editor). A cópia chega ao backend antes do próximo frame. Id sem entrada no config também aceita malha, só que sem imagem. Os triângulos são expandidos para lista sem índices, então não há limite de 65k vértices. O sample `window` envia um hexágono.
- **Modelos 3D:** com `PrimitiveShape::Model`, a geometria vem do arquivo em `PrimitiveMesh::model` (glTF `.glb`/`.gltf`, OBJ, IQM...), carregado em `initialize()`. Ela fica nas unidades do arquivo (glTF: metros, Y para cima), com as transformações dos nós já aplicadas; `size` multiplica cada eixo (1 = tamanho original) e `offset` desloca. Cada malha do arquivo vira uma parte com a imagem e a cor base do seu material (imagens embutidas em PNG ou JPEG, com mipmaps e filtro anisotrópico), e cada parte é um draw call instanciado: mil árvores iguais custam o mesmo número de draw calls que uma. `PrimitiveMesh::texture` preenchida substitui as imagens do arquivo. Use `doubleSided = true` em folhagem e placas finas: os dois lados aparecem, e a luz vale para o lado visto. Animações e materiais além da cor base (normal map, metal, transparência) ainda não são usados. O sample `window` carrega `assets/house.glb` (gerado por `samples/window/make_house_glb.py`).
- **Imagem:** `PrimitiveMesh::texture` é o caminho de uma imagem (PNG, JPG...), carregada em `initialize()` com mipmaps e filtro anisotrópico e multiplicada pela cor da instância. No `Plane`, o topo da imagem fica em -Z e a esquerda em -X. O caminho é usado como veio: quem usa o backend decide onde ficam os arquivos (o sample `window` monta o caminho a partir do executável). Meshes com o mesmo caminho compartilham a textura. Se a imagem não carregar, há um aviso no log e a mesh é desenhada só com a cor.
- **Repetição da imagem por objeto:** `RenderComponent::uvTransform` (uv * xy + zw), enviado por instância. Ex.: um `Plane` esticado para 10 × 80 com `{1, 8, 0, 0}` repete a imagem 8 vezes no comprimento em vez de esticá-la. A imagem repete (wrap) nas bordas.
- **Repetição sem grade (hex-tiling):** com `PrimitiveMesh::tiling = lkz::TextureTiling::Hex`, a imagem não se repete em grade. A superfície vira uma grade de hexágonos (cerca de 0,29 altura da imagem cada), e cada um lê a imagem com deslocamento e rotação aleatórios, misturada com os vizinhos nas bordas. Serve para texturas orgânicas que repetem muitas vezes (grama, terra, cascalho, pedra). A imagem precisa emendar sem costura nas bordas; não precisa ser quadrada (a grade e as rotações seguem a proporção dela). Ajustes em `PrimitiveMesh::hex`: `rotation` (0 = só desloca, 1 = qualquer ângulo; use 0 em imagens com direção, como tábuas) e `contrast` (0.5 = padrão; mais = transição mais nítida). Custa 3 leituras da imagem por pixel, só nas meshes que usam. A técnica é a de Mikkelsen, ["Practical Real-Time Hex-Tiling"](https://jcgt.org/published/0011/03/05/) (JCGT 2022), versão para cor, portada do [demo oficial](https://github.com/mmikk/hextile-demo) (MIT), com o hash inteiro pcg2d no lugar do `sin()`. No sample `window`, a placa à direita usa hex-tiling.
- **Texto sobre a cena:** `world.setOverlayText(...)` (linhas separadas por `\n`) aparece no canto, sobre uma faixa escura, abaixo do FPS. Serve para status e ajuda de ferramentas. Acentos funcionam na fonte padrão.
- **Instancing:** um draw call por parte e por nível de detalhe. A matriz de modelo e a cor (`RenderComponent::color`) vão por instância num único buffer dinâmico, e o shader aplica uma luz direcional (`RaylibConfig::lightDirection`).
- **Projeção:** perspectiva com o `fovY`/`near`/`far` da `lkz::Camera` ativa e o aspect ratio atual da janela (ela é redimensionável).
- **HUD:** FPS, objetos, draw calls e triângulos desenhados no canto (`showStats`).

## Muitas cópias (florestas, cidades)

O modelo é carregado uma vez; cada cópia é só a matriz dela (padrão Flyweight + instancing da GPU). Em cima disso, quatro técnicas, todas opcionais e combináveis:

| Técnica | Como ligar | O que faz |
|---|---|---|
| Descarte pela câmera | sempre ligado | a esfera envolvente de cada cópia (ou célula de lote) é testada contra os 6 planos da câmera; o que está fora não vai para a GPU |
| Níveis de detalhe (LOD) | `PrimitiveMesh::lods` | versões mais leves, escolhidas pelo tamanho na tela. Geradas na abertura pela [meshoptimizer](https://github.com/zeux/meshoptimizer) (`triangleRatio`; primeiro preservando a forma, e se não chegar perto da meta, a simplificação "sloppy", boa para folhagem) ou lidas de arquivos próprios (`model`) |
| Impostor | `PrimitiveMesh::impostor` | de longe, cada cópia vira um quadrado com fotos dela de N x N direções do hemisfério de cima (impostor octaédrico, Ryan Brucks), assadas na abertura: cor e normal, com a luz aplicada na hora e mistura das 4 vistas vizinhas (sem pulos ao girar a câmera). 2 triângulos por cópia |
| Lotes parados | `world.setInstanceBatch(id, lkz::InstanceBatch{...})` | cópias sem nó nem entidade: as matrizes ficam num buffer da GPU, enviado uma vez, ordenado por células (`cellSize`, 64 m). Por frame, só se escolhe o que desenhar: descarte e nível por célula, e células vizinhas com o mesmo nível saem num draw call só |

E mais `PrimitiveMesh::cullScreenSize`: abaixo desse tamanho na tela, a cópia some.

**Tamanho na tela** é quanto da altura da tela o diâmetro da esfera envolvente ocupa (1 = a tela toda). Não depende da resolução. Um objeto de raio r a d metros, com campo de visão vertical fov, ocupa r / (d · tan(fov/2)). Os limiares de `lods` (em qualquer ordem; são ordenados) e do impostor dizem a partir de que tamanho cada versão entra.

```cpp
lkz::PrimitiveMesh tree{lkz::PrimitiveShape::Model};
tree.model = "assets/arvore.glb";
tree.doubleSided = true;
tree.lods = {{0.25f, 0.3f}, {0.1f, 0.08f}}; // < 25% da tela: 30% dos triângulos; < 10%: 8%
tree.impostor = {true, 0.05f, 8, 128};       // < 5%: impostor, 8 x 8 vistas de 128 px
tree.cullScreenSize = 0.003f;                // < 0,3%: não desenha
config.meshes[meshArvore] = tree;
...
lkz::InstanceBatch forest{meshArvore};
forest.transforms = {...}; // uma matriz por árvore
world.setInstanceBatch(1, std::move(forest)); // de novo a cada mudança; vazio remove
```

**Para ajustar:** a tecla `RaylibConfig::lodDebugKey` (F3 por padrão) colore as cópias pelo nível: original, azul (LOD 1), amarelo (LOD 2), laranja (3 ou mais) e magenta (impostor). O canto da tela mostra draw calls e triângulos do frame. O log da abertura lista os triângulos de cada LOD gerado.

**Números** (RTX 4060, i5-10400F, árvore de 7.170 triângulos, sem vsync): sem nada disso, 10 mil árvores davam 44 FPS (72 milhões de triângulos por frame). Com lote, LOD (30% e 8%), impostor e descarte: 100 mil árvores a ~168 FPS, igual à cena sem elas, com 8 a 50 draw calls e até ~4 milhões de triângulos por frame conforme a câmera.

**Limites:** lods e impostor não valem para `PrimitiveShape::Custom` (a malha muda a qualquer momento). O impostor vê o objeto de cima e dos lados, não de baixo. O nível por célula é escolhido pela distância até o ponto mais próximo da célula (conservador: células grandes ficam com mais detalhe que o necessário). Descarte e escolha de nível acontecem na CPU (OpenGL 3.3; o descarte na GPU com draw indireto pede 4.3).

## Terreno

`world.setTerrain(lkz::TerrainDesc{heightmap, "imagem.png"})` desenha o relevo no estilo CDLOD (Strugar, 2009): a mesma grade de 32 × 32 quadrados, instanciada em cada pedaço que o `lkz::TerrainLod` escolhe para a câmera (2 draw calls: pedaços inteiros e quartos). A altura sai de uma textura de 32 bits com o heightmap inteiro, lida no vertex shader com interpolação bilinear; a normal sai das alturas vizinhas, e cada vértice se transforma no do nível seguinte perto do fim da faixa (morph), sem rachaduras entre níveis. A imagem cobre o terreno inteiro (topo em -Z, esquerda em -X) com a mesma luz das malhas: um terreno plano sai igual a um `Plane` com a mesma imagem. `world.updateTerrain(patch)` atualiza só o retângulo mudado (textura e alturas dos nós). F3 colore os níveis. O sample `physics` mostra um relevo de 2 × 2 km.

## Interface (RmlUi)

`world.showUi(lkz::UiDocument{...})` mostra uma tela por cima da cena, escrita em RML/RCSS, a "quase-HTML/CSS" da [RmlUi](https://github.com/mikke89/RmlUi) (MIT; fontes pela FreeType). Dá para prototipar no navegador e passar quase sem mudança.

```cpp
lkz::UiDocument ui;
ui.path = "assets/ui/tela.rml";
ui.fonts = {"assets/ui/fonts/Barlow-Regular.ttf"};
ui.values = {{"pontos", 0}, {"pausado", false}}; // todos os nomes que a tela usa
world.showUi(std::move(ui));
...
world.setUiValue("pontos", 120);                 // a qualquer momento, da lógica
for (const auto& event : world.input().uiEvents) // cliques: action('pausar') no RML
    if (event.action == "pausar") ...
```

- **Modelo de dados "ui"** (`<body data-model="ui">`): `{{pontos}}`, `data-if="pausado"`, `data-class-ativo="aba == 'mapa'"`, `data-style-left="x + 'dp'"`, e `data-event-click="action('nome', 'valor')"` (até dois textos), que chega em `InputState::uiEvents`.
- **Valores da engine** no mesmo modelo: `lkzFps`, `lkzDrawCalls`, `lkzTriangles` (milhões), `lkzObjects` e `lkzLodColors`. Ações começando com `lkz.` são da engine: `action('lkz.lodColors')` liga as cores de LOD.
- **Mouse:** o fundo do documento é transparente para o mouse. Sobre um painel ou botão, `InputState::pointerOverUi` fica true, e o clique, o arrasto e a roda que começam ali não chegam ao jogo. O teclado não vai para a interface (os atalhos ficam com o jogo).
- **Tamanho:** use `dp` no RCSS; 1dp = altura da janela / `UiDocument::designHeight` (720), então uma tela desenhada para 1280 × 720 vale em qualquer resolução.
- **Depurar:** `RaylibConfig::uiDebuggerKey` (F8) abre o depurador visual da RmlUi (elementos, estilos, eventos, log). Um quadrado amarelo no canto avisa que há erros no log da interface.
- **Cuidados do RCSS:** sem CSS grid (use flexbox); texto solto dentro de um contêiner flex não aparece, então ponha o texto num `<span>`; cores com alfa em `#rrggbbaa`; desenho básico (sem degradê, sombra ou filtro). O sample `window` tem uma tela de exemplo em `assets/ui/demo.rml`.

## Threads

Tudo acontece na thread de render do `World`: a janela é criada em `initialize()` e os eventos são atendidos em `endFrame()` (`EndDrawing`). No Windows isso funciona porque a fila de mensagens é da thread que criou a janela. Fechar a janela ou apertar ESC faz `closeRequested()` retornar true, e o `World` encerra o loop. Depois de cada frame, `pollInput()` copia teclado e mouse para o `World`, e a lógica lê com `world.input()` (`lkz::InputState`, em `<lkz/Input/Input.hpp>`): `down()` é o estado atual, `pressed()`/`released()` são eventos acumulados até o próximo passo fixo (nenhum clique se perde). Com `RaylibConfig::escapeClosesWindow = false`, o ESC vira uma tecla comum e quem decide fechar é o jogo (`world.requestStop()`).

## Desempenho

Com cerca de 21 mil objetos, o FPS é limitado pela thread lógica, não pela GPU: em Debug (`-O0`) fica perto de 20 FPS, e em RelWithDebInfo chega ao vsync (60). Para jogar ou medir, use um build otimizado.

## Ainda não tem

Animação de modelos, normal maps, recorte por transparência (folhas em plano com alfa), transparência ordenada, sombras, descarte por oclusão (o que está atrás de outra coisa), transição suave entre níveis de detalhe (hoje troca de uma vez) e descarte na GPU (draw indireto, OpenGL 4.3).
