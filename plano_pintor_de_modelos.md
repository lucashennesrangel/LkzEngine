# Pintor de modelos: plano de implementação

Ferramenta para importar um modelo 3D sem textura (ou com textura ruim), pintá-lo com cores e com os materiais do projeto, e exportar um `.glb` final, leve e com UV fixo, pronto para ser usado muitas vezes na cidade.

Escrito em 31/10/2026, a partir do código da Lkz Engine (raylib 6.2, meshoptimizer v0.22, cgltf v1.15 e stb no backend NVRHI) e do MafiaCity como estão hoje. Nada aqui foi compilado. Os nomes de API são propostas.

---

## 1. Objetivo e escopo

**O fluxo que o usuário quer:**

1. Modelar a forma no Blender (ou usar um modelo pronto) e importar no Pintor.
2. Pintar: cor livre, materiais da paleta (tijolo, reboco, vidro...), balde por face ou por parede.
3. Marcar o que é **material compartilhado**, o que é **pintura única** e o que é **só cor**.
4. Exportar: o Pintor gera o UV final, assa a pintura numa textura, limpa a malha e grava o `.glb`.
5. O modelo aparece no catálogo do jogo, sem mexer em código.

**Fica fora:**
- Modelagem (continua no Blender).
- Animação, rigging, mapa de normal e emissivo (a engine não lê os dois últimos hoje).
- Pintura em camadas complexa ao estilo do Substance (pode vir depois, ver a fase 8).

---

## 2. O que já existe e o que falta

### Já existe (verificado no código)

| Item | Onde |
|---|---|
| Malha do jogo em tempo real: `world.uploadMesh(id, MeshData)` (posições, normais, UVs, índices) | `lkz/World.hpp`, `lkz/Render/MeshData.hpp` |
| Atualização parcial com precedente: `world.updateTerrain(TerrainPatch)` manda só o retângulo que mudou | `lkz/World.hpp`, `RenderBackend::updateTerrain` |
| Modelos glTF com várias partes, cada uma com a imagem e a cor base do seu material | `RaylibRenderBackend.cpp`, `loadModel` |
| Mipmaps + filtro trilinear + anisotrópico 16x em toda textura (`sharpen`) | `RaylibRenderBackend.cpp` |
| Cache de texturas **por caminho** para `PrimitiveMesh::texture` | `RaylibRenderBackend.cpp` (`textures`) |
| JPEG ligado na raylib (`SUPPORT_FILEFORMAT_JPG=1`) | `renderer_raylib/CMakeLists.txt` |
| LOD automático (`MeshLod::triangleRatio`, meshoptimizer) e impostores | `RenderConfig.hpp` |
| Hex-tiling (Mikkelsen 2022) para materiais orgânicos | shader do backend raylib |
| cgltf (MIT, v1.15) e stb (domínio público) já baixados pela engine | `renderer_nvrhi/CMakeLists.txt` |
| Prefixo de símbolos do cgltf para não colidir com a cópia interna da raylib | `renderer_nvrhi/src/CgltfPrefix.hpp` |
| Atualização de retângulo de textura na GPU: `UpdateTextureRec` | raylib 6.2 (`raylib.h`) |
| Interface RmlUi, input, câmera, editor com modos | `ui_rmlui`, MafiaCity `EditorUi`, `RoadEditor::Mode` |

### Falta

| Item | Por quê |
|---|---|
| Leitor de glTF no lado da lógica (não no backend) | o pincel precisa da geometria na CPU para o raio |
| Gravador de `.glb` | não existe; a `cgltf_write` não monta o bloco binário nem as imagens, isso fica conosco |
| Textura editável em tempo real (`uploadTexture` / `updateTexture`) | hoje só há textura carregada de arquivo |
| Desembrulhar UV, mapa de texels, transferência e dilatação | base do bake |
| Reaproveitar a mesma imagem entre modelos diferentes | a raylib carrega as imagens de cada `.glb` separadamente; sem isso, "material compartilhado" não economiza memória |
| Catálogo de props lido de arquivo | hoje `propTypes` é fixo em `Props.hpp` e exige recompilar |

### Fatos que pesam no desenho

- **A raylib ignora `KHR_texture_transform` e o sampler do glTF.** Procurei no `rmodels.c` da 6.2 e não há referência a nenhum dos dois. Portanto a escala "metros por repetição" tem de ser **gravada no próprio UV**: valores acima de 1 se repetem, porque a textura usa repetição (wrap).
- **Draw calls:** os lotes de cópias (`setInstanceBatch`) desenham por **parte × célula × nível de detalhe**, não por cópia. Cada material a mais num modelo multiplica as chamadas de todos os lotes daquele modelo. (Isso corrige o que eu disse na conversa, que era "um draw call por cópia".)
- **Memória de vídeo** é o que limita, não o tamanho do arquivo. Cópias do mesmo modelo dividem a textura.

---

## 3. Arquitetura

### 3.1 Onde cada coisa mora (regra `engine-game-boundary`)

| Peça | Lugar | Motivo |
|---|---|---|
| Ler e gravar glTF, imagens, desembrulhar, assar, otimizar, raio | **Engine**, módulo novo `lkz::asset` | genérico, qualquer jogo usa |
| `uploadTexture` / `updateTexture` | **Engine**, `World` + `RenderBackend` (padrão no-op, como `updateTerrain`) | mecanismo genérico |
| Reaproveitar imagem por caminho entre modelos | **Engine**, backends raylib e NVRHI | genérico |
| O aplicativo Pintor (câmera, pincéis, interface) | ver a **decisão D1** abaixo | |
| Paleta de materiais, pastas, catálogo de props | **Jogo** (`MafiaCity/assets`, `Props.*`) | conteúdo do jogo |

**Decisão D1: onde fica o Pintor**
- **A (recomendada):** ferramenta genérica `Lkz-Engine/tools/painter`. Ela lê um arquivo de projeto do jogo (`MafiaCity/painter.json`) com a pasta da paleta, a pasta de saída e o arquivo de catálogo. A engine não fica sabendo de nenhum jogo, e um segundo jogo usa o Pintor sem mudar nada.
- **B:** um modo a mais no editor do MafiaCity. É mais rápido de ligar, mas mistura o editor da cidade com a criação de assets e vai contra a regra de não deixar código genérico preso a um jogo.

### 3.2 Módulo `lkz::asset` (engine)

Biblioteca estática nova em `engine/asset`, sem raylib (respeita a fronteira).

```
<lkz/Asset/ModelData.hpp>   ModelData { parts, materials, images }, ImageData, MaterialData
<lkz/Asset/Gltf.hpp>        loadGltf(path) -> ModelData;  saveGlb(path, ModelData, GlbOptions)
<lkz/Asset/Image.hpp>       loadImage / saveImage (PNG, JPEG), resize, dilate
<lkz/Asset/Raycast.hpp>     MeshRaycaster (BVH): intersect(ray) -> {part, triângulo, baricêntricas, distância}
<lkz/Asset/Unwrap.hpp>      unwrapUnique(...) via xatlas; packPlanarCharts(...) para prédios
<lkz/Asset/TexelMap.hpp>    para cada texel: parte, triângulo, baricêntricas, posição 3D, normal
<lkz/Asset/Bake.hpp>        transferTexture, bakeTriplanar, bakeSwatches
<lkz/Asset/Optimize.hpp>    weld, removeDegenerate, removeHidden, optimizeForGpu, splitFor16Bit
```

Esboço dos dados:

```cpp
namespace lkz::asset {
struct ImageData { int width = 0, height = 0; std::vector<std::uint8_t> rgba; std::string uri; };

enum class SurfaceTarget : std::uint8_t { Shared, Unique, Color };

struct MaterialData {
    std::string name;
    glm::vec4 baseColor{1.0f};
    int image = -1;                 // índice em ModelData::images (-1 = só cor)
    std::string shared;             // nome do material da paleta (Shared)
    float metersPerRepeat = 0.0f;   // Shared: tamanho de uma repetição
};

struct ModelPart { MeshData mesh; int material = 0; std::vector<SurfaceTarget> faceTarget; };
struct ModelData { std::vector<ModelPart> parts; std::vector<MaterialData> materials; std::vector<ImageData> images; };
}
```

### 3.3 Textura editável (engine)

```cpp
// World (thread lógica; entrega antes do próximo frame, como uploadMesh)
void uploadTexture(std::uint32_t textureId, asset::ImageData image);   // cria ou troca
void updateTexture(std::uint32_t textureId, TexturePatch patch);        // retângulo x, y, w, h + pixels
// PrimitiveMesh ganha: std::uint32_t runtimeTexture = 0;  // 0 = usa `texture`/modelo
```

- Raylib: `UpdateTextureRec` no retângulo. Os mipmaps são refeitos **ao fim de cada traço**, não a cada pincelada, porque `GenTextureMipmaps` refaz a imagem toda.
- NVRHI: a mesma API, com implementação própria, ou no-op na primeira versão (o Pintor roda no backend raylib).

### 3.4 Reaproveitar imagens compartilhadas (engine)

O `.glb` exportado aponta os materiais compartilhados para **imagens externas** (glTF permite `uri` relativa), por exemplo `../../textures/materials/tijolo-vermelho.png`, e guarda o nome do material em `extras`.

No backend raylib: depois do `LoadModel`, ler o mesmo arquivo com o cgltf para saber a `uri` de cada material, trocar a textura do material pela do cache (por caminho resolvido) e liberar a duplicata. O cache `textures` já existe.

Atenção: a raylib compila sua própria implementação do cgltf. A cópia da engine precisa do prefixo de símbolos, como em `CgltfPrefix.hpp` do NVRHI.

---

## 4. Como o Pintor funciona

### 4.1 Importar e checar o UV
- Lê o `.glb`/`.gltf` com `lkz::asset::loadGltf`.
- Constrói o **mapa de texels**: rasteriza cada triângulo no espaço do UV e guarda, para cada texel, o triângulo, as baricêntricas, a posição 3D e a normal.
- **Detecta UV sobreposto**: texels cobertos por mais de um triângulo. Se houver, avisa: "pintar aqui pinta N faces ao mesmo tempo. Gerar UV único?" (os modelos `_tex` atuais caem nesse caso, porque repetem células do atlas).

### 4.2 Pincel no espaço 3D (não no UV)
A ideia vem da pintura direta sobre a forma 3D (Hanrahan & Haeberli 1990):
1. Um raio do mouse acha o ponto atingido (BVH).
2. O BVH devolve os triângulos dentro da esfera do pincel; os retângulos de UV deles limitam os texels a testar.
3. Cada texel é pintado se a sua **posição 3D** estiver dentro do raio, com queda suave (dureza), e se a normal estiver virada para a câmera (não atravessa paredes finas).

Como a distância é medida em 3D, o traço atravessa as emendas de UV sem quebrar.

**Ferramentas:** cor, material (paleta), borracha (volta ao original), balde (por face, ilha de UV, plano ou parede), conta-gotas, espelhamento por eixo.

### 4.3 Pincel de material (o "jeito The Sims")
- A paleta é a pasta de materiais do jogo, com um `.json` ao lado de cada imagem (`metersPerRepeat`, `tiling: repeat|hex`, opcional `alignToWorldUp`).
- O pincel amostra o material por **projeção triplanar** usando a posição 3D e a normal do texel, na escala real em metros. Tijolo fica horizontal e no tamanho certo em qualquer parede.

### 4.4 Destino de cada face
| Destino | Uso | Resultado na exportação |
|---|---|---|
| **Compartilhado** | paredes lisas, telhado, calçada, vidro | UV em metros / `metersPerRepeat`, imagem externa da paleta |
| **Único** | letreiro, pichação, sujeira, fachada especial | entra no atlas único do modelo |
| **Só cor** | ferragem, detalhes pequenos | amostra de cor de 4 × 4 px dentro do atlas único (não cria material) |

- **Tingir** (feito, a pedido): um material da paleta multiplicado por uma cor. Uma imagem clara (tinta quase branca) dá paredes de qualquer cor com textura; no `.glb`, a mesma imagem com a cor no `baseColorFactor`, um material por combinação.
- **Seleção:** face, ilha, plano (faces coplanares), parede (coplanar + contígua), retângulo na tela.
- **Regra inicial automática:** superfícies planas acima de X m² viram Compartilhado e o resto vira Único. Você corrige depois.
- **Nomes vindos do Blender:** o prefixo da peça (`parede_`, `telhado_`, `unico_`, `cor_`) define o destino na importação.

### 4.5 Sobre "atlas compartilhado"
Um atlas genérico com várias texturas dentro **não repete** sem emenda: na repetição, as bordas puxam a vizinha e os mipmaps misturam regiões. Por isso:
- materiais que se repetem nos dois eixos ficam **cada um na sua imagem**;
- elementos que se repetem só num eixo (frisos, cornijas, rodapés, faixas de janela) podem ir numa **trim sheet**: faixas que ocupam a largura inteira da imagem, e por isso repetem em U sem emenda (Olsen 2015, *Sunset Overdrive*). É uma fase opcional (7b).

### 4.6 Desfazer e projeto
- Desfazer por **blocos de 64 × 64 px** copiados antes de cada traço (copy-on-write).
- O projeto editável fica em `source/models/<nome>/` (modelo original + camada de pintura em PNG + `pintura.json` com destinos e paleta usada), seguindo a regra assets/source do README. Só o `.glb` final vai para `assets/`.

### 4.7 Carimbo (decalques: janela, porta, letreiro...)

Para uma imagem de proporção fixa que **não** deve se repetir nem distorcer (porta, janela, letreiro) — ao contrário do material Compartilhado, que é feito para repetir:

1. Escolhe a imagem: da pasta de decalques (`assets/textures/decals/`, um `.json` ao lado com `larguraMetros`) ou importada na hora (ver 4.8).
2. Clica num ponto da superfície e arrasta: o clique ancora um canto, o arrasto mede a largura ao longo do eixo horizontal da face (a mesma projeção `surfaceMeters` usada no pincel de material, seção 4.3); a altura vem da proporção da própria imagem — nunca estica.
3. Enquanto arrasta, um contorno acompanha o mouse. Depois de soltar, dá para mover, redimensionar (mantendo a proporção) e girar em múltiplos de 90°, até confirmar (Enter ou clique fora) ou cancelar (Esc).
4. Confirmado, as faces do plano clicado são **cortadas no retângulo** (Sutherland–Hodgman nos eixos da face): os pedaços de fora ficam como estavam (Compartilhado, Único ou Cor, com o UV de antes), e os de dentro ganham o destino novo **Decal**, com o UV no retângulo da imagem.
5. Cada carimbada é independente. A mesma janela em 6 lugares são 6 carimbadas, mas dividem a mesma imagem. Desfazer devolve a malha de antes (o desfazer guarda um retrato da geometria).
6. **Mão** (adiantada da fase 8, a pedido): pega um carimbo já posto para mover, redimensionar, girar, copiar/colar e apagar (seção 4.7.1).

**Mudança em relação à primeira versão deste plano** (compor a imagem nos pixels da tela Única), decidida ao implementar:
- as paredes Compartilhadas não têm tela, e é nelas que vão as janelas;
- na tela Única, a janela teria a densidade do atlas (~18 px/m no prédio de tijolo) e ficaria borrada.

Com o corte:
- funciona em qualquer destino;
- a imagem aparece na resolução dela;
- na exportação, o material do decalque aponta para a imagem em `textures/decals/` como a paleta (`extras.lkz.target = "decal"`), e o cache por caminho do backend faz todos os carimbos usarem uma textura só.
- Decalque com transparência: cada carimbo ganha uma imagem composta sobre o que a face mostrava (embutida, PNG).

Custo: mais triângulos (uma janela numa parede de 2 triângulos dá ~20) e vértices em T nas bordas cortadas. A fase 6 (solda, limpeza) deve juntá-los.

#### 4.7.1 Mão: ajustar um carimbo já posto

Ferramenta **Mão** (tecla H), com o ponteiro do mouse mudando conforme a ação (`World::setMouseCursor`, genérico na engine: raylib e SDL3):
- passar o mouse sobre um carimbo o realça, com o ponteiro de mão;
- clicar pega o carimbo: ele **sai da parede** (o que ele cobria volta) e vira o mesmo retângulo pendente do Carimbo, já arrastando. Arrastar de dentro move (ponteiro de mover); um canto muda o tamanho (ponteiro diagonal; Shift solta a proporção); R gira; Enter ou clique fora confirma; Esc devolve como estava;
- **Ctrl+C** copia o carimbo pego ou o que está sob o mouse; **Ctrl+V** cola: o carimbo segue o mouse de parede em parede, e o clique o solta (pendente, para ajustar);
- **Delete** apaga o carimbo pego ou o que está sob o mouse.

Como funciona por baixo:
- cada carimbo é uma parte própria da malha (`Document::stampOf_`), e cada triângulo dela lembra o que cobria (`Origin`: a parte e o triângulo do arquivo de onde veio, o destino e o UV de lá, e, se lá também era um carimbo, a origem daquele);
- tirar (`lift`) devolve esses triângulos às partes de onde vieram, com o destino **atual** do resto daquela face: um balde na parede depois do carimbo vale também para o buraco;
- confirmar é `stamp` de novo, no mesmo passo de desfazer do `lift`. Confirmar sem ter mudado nada desfaz o `lift`, sem cortar de novo;
- a parede fica com os cortes do lugar antigo (triângulos a mais, mesma aparência); a fase 6 os junta.

Limites:
- ~~carimbos de um arquivo reaberto e os de antes do UV único são fixos~~ Resolvido (4.7.4);
- uma pincelada dada depois do carimbo não passa por baixo dele: ao tirá-lo, o buraco mostra a tela como estava.

Seleção (feito, a pedido): a Mão seleciona carimbos (Shift soma), e cor, profundidade e giro mudam os selecionados na parede, num passo só (`Document::replaceStamps`, ids estáveis por `stampId`). Arrastar move ou redimensiona, e soltar confirma. Colar põe um carimbo a cada clique, até o Esc.

#### 4.7.2 Cor e profundidade do carimbo (feito, a pedido)

**Cor** (`DecalPlacement::tintMode`, `tint`):
- **Multiplicar**: a imagem vezes a cor. Vai no `baseColorFactor` do material do decalque, e a imagem continua uma só, apontada por todos os modelos.
- **Só o branco**: a cor só onde o pixel não tem saturação. É a "máscara de tinta" dos jogos (por exemplo, o `$blendtintbybasealpha` da Source e as máscaras de cor do jogador), só que gerada da imagem: peso = 1 − smoothstep(0,08; 0,25; saturação HSV). Como o jogo só sabe multiplicar, o Pintor gera uma imagem por decalque e cor, embutida.
- Com transparência, a cor entra na composta de cada carimbo.

**Profundidade** (`DecalPlacement::depth`): é o "inset + extrude" dos modeladores, restrito ao retângulo do carimbo.
- Os pedaços de dentro já saem do corte com a borda exata do buraco. Eles andam `normal × depth`.
- Cada aresta deles que cai na borda do retângulo vira uma parede do vão, com o destino da parede de onde saiu.
- As paredes do vão ficam em partes próprias, com `StampRecord::owner` apontando para o carimbo, e saem com ele no `lift`. Os triângulos do decalque voltam ao plano (`StampRecord::offset`).
- Limites:
  - só retângulos de um carimbo (ainda não há "selecionar uma área e afundar" sem imagem);
  - a espessura da parede não é conferida;
  - na parede Única, o UV do vão é a borda esticada.

#### 4.7.4 Carimbos ajustáveis depois de reabrir (feito, a pedido)

A exportação (e o UV único) grava `extras.pintorCarimbos` no modelo: decalque, normal e plano da parede, retângulo, giro, cor e profundidade. Ao abrir, `Document::restoreStamps` faz o seguinte.

1. **Reconstrói cada carimbo da lista** a partir dos triângulos da malha:
   - os triângulos de decalque da mesma imagem, no plano da frente, com o centro no retângulo **e o UV do carimbo**. Sem o UV, carimbos iguais sobrepostos se confundiriam;
   - as paredes do vão: perpendiculares, entre a parede e a frente, na borda.
2. **Adota os grupos fora da lista** (um `.glb` de antes dela). Cada grupo ligado de triângulos de decalque, da mesma imagem e no mesmo plano, dá o mapa do plano para o UV. Desse mapa saem o retângulo e o giro.
   - O grupo só é aceito se o retângulo reproduzir o UV de todos os triângulos.
   - Grupos iguais sem aresta em comum viram um carimbo só.
   - A profundidade não dá para saber.
3. **Recria "o que fica embaixo"** a partir da face da parede em volta mais perto do retângulo: o destino dela e o UV dela estendido pelo plano (`SurfaceToUv`).

O plano gravado é o do maior triângulo de cada carimbo; o de uma lasca fina tem a normal imprecisa.

No prédio de tijolo do MafiaCity, exportado antes da lista, 27 carimbos voltaram ajustáveis e iguais depois de três exportações seguidas. Um grupo torto (25 triângulos) ficou fixo.

De passagem, um bug corrigido: uma parte que perdia todos os triângulos ficava com os índices vazios, que no glTF querem dizer "sem índices", e os triângulos antigos voltavam. Agora a malha é esvaziada (`setTriangles`).

#### 4.7.3 Esculpir (feito, a pedido)

Ferramenta **Esculpir** (X), seção "Esculpir" do README do Pintor:
- seleção por triângulo, parede, plano ou retângulo desenhado (`Document::cutRectangle`, o corte do carimbo sem imagem);
- **Puxar**: extrusão da região, com paredes nas arestas da borda, no acabamento da face vizinha de fora;
- **Mover**: os vértices soldados da seleção andam em todas as partes, então as vizinhas esticam;
- sessão `beginSculpt` / `updateSculpt` / `endSculpt`: cada atualização recomeça da malha preparada (prévia ao vivo), e confirmar é um passo de desfazer.

Antes de mover, `splitTJunctions` divide as arestas com vértices em T na região: um leque a partir do centro do triângulo, sem triângulos achatados. Fica de fora da fase 6 a solda e a limpeza do modelo inteiro.

Testes (`painter_test`): o corte do retângulo; afundar e saltar com as paredes viradas para o lado certo; sem frestas (uma grade de raios); desistir e confirmar sem andar; Mover esticando as vizinhas e o chão subindo com a parede; uma porta carimbada não criando paredes falsas.

### 4.8 Importar uma imagem nova durante a sessão

Vale tanto para a paleta (materiais compartilhados, seção 4.3) quanto para os decalques (4.7), sem precisar fechar o Pintor:

- Botão **Importar imagem...** nas duas telas (Tinta → Material da paleta, e Carimbo).
- Reaproveita o navegador de pastas que já existe para **Abrir** (seção 4.1), mas sem travar nas raízes do projeto: um botão "Procurar em outro lugar" deixa navegar o resto do disco, começando na última pasta usada.
- Ao escolher o arquivo, ele é **copiado** para `textures/materials/` ou `textures/decals/`, com o nome do arquivo original sanitizado (minúsculo, sem espaço nem acento); se o nome já existe, pergunta antes de substituir.
- Cria o `.json` com um valor padrão (1 m de repetição para a paleta; a proporção da própria imagem para o decalque) e o item já aparece selecionado na lista, sem reabrir o Pintor.

---

## 5. Pipeline de exportação

Na ordem:

1. **Separar por destino**: Compartilhado (um grupo por material), Único e Cor.
2. **UV dos compartilhados**: projeção planar por face, alinhada à vertical do mundo, em metros / `metersPerRepeat`. Não precisa de atlas.
3. **UV único**: para prédios e peças retas, **cartas planas** (faces coplanares contíguas) empacotadas com `xatlas::AddUvMesh` + `PackCharts`, sem distorção e com bordas retas. Para formas orgânicas (árvore, estátua), `xatlas::Generate` completo. A resolução sai da densidade de texel desejada (ver a seção 7).
4. **Assar**: para cada texel do atlas novo, achar o triângulo e as baricêntricas, voltar ao UV antigo e amostrar a pintura composta (base + camada). Amostras de cor entram em blocos de 4 × 4.
5. **Dilatar** as ilhas (padding) de 4 a 8 px a 1024, para os mipmaps não puxarem o fundo (é o que o Substance Painter faz por padrão).
6. **Limpar a malha** (meshoptimizer): soldar vértices iguais, tirar triângulos degenerados, tirar a face de baixo apoiada em y = 0, e opcionalmente tirar triângulos **nunca visíveis** (raios de fora do modelo vindos de várias direções; quem não é atingido sai). Depois, otimizar o cache de vértices e a ordem de leitura.
7. **Dividir** em primitivas de até 65 535 vértices (índices de 16 bits; a raylib converte índices de 32 bits).
8. **Imagens**: potência de 2; **JPEG** (qualidade ~90) se não houver transparência, PNG se houver (vegetação); sem mipmaps gravados (a engine gera). Sem mapa de normal nem emissivo.
9. **Gravar o `.glb`** com `extras.lkz` = { versão, origem, densidade de texel, destinos por material }. Base centrada, apoiada em y = 0, frente em +Z (padrão do projeto).
10. **Validar** relendo com `loadGltf`: triângulos, materiais, tamanhos de imagem, caixa envolvente, **memória de vídeo estimada** e tamanho do arquivo. Mostrar isso num relatório antes de gravar por cima.
11. **Registrar** no catálogo do jogo (seção 6).

---

## 6. Integração com o MafiaCity

- **Catálogo em arquivo**: `assets/models/catalogo.json` lido na inicialização e somado à lista fixa de `Props.hpp`, que vira apenas o padrão. O redirecionamento de chaves antigas (`predio_bege` → `predio_bege_tex`, já em `Props.cpp`) continua.
- Campos por entrada: chave, nome, arquivo, categoria, raio/área, LOD (`screenSize`, `triangleRatio`), impostor, `cullScreenSize`.
- **Miniaturas**: o Pintor salva a miniatura na hora de exportar, ou o `scripts/ui/make_thumbnails.py` continua sendo rodado.
- **Paleta**: `assets/textures/materials/` com os `.json` de escala.
- **Conversão dos modelos `_tex` atuais**: importar, gerar UV único, transferir e exportar com **uma textura**, em vez de 22 a 32. É o primeiro teste real do pipeline.

---

## 7. Orçamentos e números

### Memória de vídeo por textura (com mipmaps)

| Lado | RGBA8 | BC1 (futuro) | BC7 (futuro) |
|---|---|---|---|
| 256 | 0,33 MiB | 0,04 MiB | 0,08 MiB |
| 512 | 1,33 MiB | 0,17 MiB | 0,33 MiB |
| 1024 | 5,33 MiB | 0,67 MiB | 1,33 MiB |
| 2048 | 21,3 MiB | 2,67 MiB | 5,33 MiB |

Exemplo: 30 prédios distintos com textura única de 1024 ocupam ~160 MiB. Com materiais compartilhados nas paredes, o gasto fica perto do tamanho da paleta, mais texturas únicas pequenas para os detalhes.

### Densidade de texel (o limite do bake único)

`px/m ≈ √(lado² × ocupação / área da superfície)`

| Superfície | Textura | Densidade (75% de ocupação) |
|---|---|---|
| Prédio 20 × 20 × 40 m (~3 600 m²) | 1024 | ~15 px/m |
| o mesmo | 2048 | ~30 px/m |
| Loja de ~600 m² | 1024 | ~36 px/m |
| Prop de ~150 m² | 512 | ~36 px/m |

Por isso as **paredes grandes devem ser Compartilhadas**: o material repetido mantém a nitidez em qualquer tamanho. A densidade alvo deve ser medida no jogo: quantos pixels de tela 1 m ocupa no zoom mais próximo da câmera.

### Metas por tipo de modelo (ponto de partida)

| Tipo | Triângulos (LOD 0) | Materiais | Textura única |
|---|---|---|---|
| Prédio comum | até ~3 000 | 2 a 3 | 512 a 1024 |
| Prédio de destaque | até ~8 000 | 3 a 4 | 1024 a 2048 |
| Prop médio | até ~800 | 1 | 256 a 512 |
| Vegetação | até ~1 500 | 1 (PNG com alfa) | 256 a 512 |

---

## 8. Bibliotecas

| Biblioteca | Licença | Para quê | Situação |
|---|---|---|---|
| **cgltf** v1.15 | MIT | ler e gravar glTF (`cgltf_write.h`); o bloco binário e as imagens a gente monta | já na engine (NVRHI) |
| **stb_image / stb_image_write** | domínio público | ler e gravar PNG e JPEG | já na engine (NVRHI) |
| **meshoptimizer** | MIT | soldar vértices, cache, ordem de leitura, simplificação | já na engine (v0.22; a atual é v1.2, API estável desde a v1.0) |
| **xatlas** | MIT, C++11, sem dependências | UV único: cartas, parametrização e empacotamento; `AddUvMesh` + `PackCharts` para cartas próprias | nova |
| **tinybvh** v1.9 | MIT, C++14/17 | BVH para o raio do pincel e o teste de visibilidade (`Intersect`, `IsOccluded`) | nova |
| **RmlUi** | MIT | interface do Pintor | já na engine |
| **Basis Universal** v2.5 | Apache-2.0 | compressão de textura na GPU (KTX2 → BC1/BC7) | futura (fase 8); a raylib 6.2 tem KTX desligado por padrão e não verifiquei KTX2 |

**Descartadas:**
- **tinygltf**: duplicaria o cgltf e traz dependência de JSON.
- **fastgltf**: rápida, mas exige simdjson e seria redundante.
- **UVAtlas** (Microsoft): depende do DirectXMath e é voltada a D3D.
- **Embree**: pesada demais para um pincel.
- **Assimp**: enorme, e só glTF é necessário.
- **Ptex / Mesh Colors**: dispensam UV, mas nem glTF nem o shader da engine suportam.

O raio-triângulo pode começar com Möller–Trumbore em força bruta (modelos de poucos milhares de triângulos) e passar para o tinybvh quando ficar lento.

---

## 9. Fases

Tamanho: P = pequeno, M = médio, G = grande. Cada fase termina com algo que você compila e testa.

| Fase | Entrega | Lado | Tamanho | Pronto quando |
|---|---|---|---|---|
| **0. Preparação** | decisões D1–D4, `lkz::asset` vazio no CMake, xatlas e tinybvh no FetchContent, prefixo do cgltf | engine | P | engine e jogo (`engine-dev`) compilam |
| **1. Ler e gravar glTF** | `loadGltf`, `saveGlb`, `Image`; sample `samples/gltf_roundtrip`; teste no `ctest` | engine | M | um `_tex` atual relido e regravado abre igual no jogo |
| **2. Textura editável** | `uploadTexture`, `updateTexture`, `PrimitiveMesh::runtimeTexture` (raylib; NVRHI no-op) | engine | P–M | sample pinta um quadrado numa malha em tempo real |
| **3. Pintor mínimo** | app com câmera orbital, importar, mapa de texels, pincel de cor em 3D, desfazer, exportar com o UV original | engine/tools | G | pinta um modelo com UV bom e o `.glb` abre no jogo |
| **4. UV único e bake** | detectar sobreposição, cartas planas + xatlas, transferir, dilatar, escolher resolução pela densidade | engine | G | `predio_tijolo_reserv_tex` convertido para 1 textura, sem emendas visíveis |
| **5. Materiais e destinos** | paleta com `.json`, pincel triplanar, seleção e destinos, exportação Compartilhado/Único/Cor, imagens externas, cache por caminho no backend | engine + jogo | G | dois prédios com o mesmo tijolo usam a mesma imagem na GPU |
| **5b. Decalques e importação** | ferramenta Carimbo (corta o plano no retângulo e põe ali uma imagem de proporção fixa, sem distorcer, com mover/redimensionar/girar antes de confirmar; destino Decal no glTF), pasta `assets/textures/decals/`, importar arquivo de qualquer pasta para a paleta ou para os decalques sem reiniciar o Pintor | engine + jogo | M | uma janela e uma porta carimbadas em posições diferentes do mesmo prédio, sem distorção, e uma imagem nova importada e usada na mesma sessão |
| **6. Otimização e relatório** | solda, degenerados, vértices em T dos carimbos, face de baixo, invisíveis, cache, divisão em 16 bits, JPEG, relatório de memória | engine | M | relatório bate com o que a engine carrega |
| **7. Catálogo no jogo** | `catalogo.json` lido em `Props`, miniatura na exportação; (7b) trim sheets | jogo | M | prédio exportado aparece no editor sem recompilar |
| **8. Depois** | decalques reposicionáveis também depois de reabrir o arquivo, camadas, estêncil, AO assado na cor, compressão KTX2/Basis | engine | — | — |

Ao fim de cada fase na engine, rodar a checagem da skill de fronteira:

```bash
grep -rniE 'mafia|gangster|famil|distrito|district|MafiaCity' engine cmake CMakeLists.txt
grep -rnE '#include ["<]' engine | grep -vE '["<](lkz/|entt/|glm/|spdlog/|tracy/|[a-z_]+(\.h)?>|windows\.h)'
```

e seguir o fluxo de versão: tag nova na engine, depois `LKZ_ENGINE_TAG` no jogo.

---

## 10. Riscos

| Risco | Efeito | Saída |
|---|---|---|
| Colisão de símbolos do cgltf com a raylib | erro de link | prefixo, como no `CgltfPrefix.hpp` |
| Refazer mipmaps a cada pincelada | travadas ao pintar em 2048 | mipmaps só no fim do traço; durante o traço, filtro sem mip |
| xatlas cortando prédios em cartas tortas | emendas e desperdício | cartas planas próprias + `PackCharts` |
| Bake único com pouca resolução em paredes grandes | prédio borrado de perto | paredes como Compartilhado; densidade medida no jogo |
| Imagens externas quebram se a pasta mudar | modelo sem textura | caminho relativo + nome em `extras`; o Pintor avisa ao exportar |
| Muitos carimbos numa parede | triângulos a mais, vértices em T | fase 6: soldar e refazer a triangulação das faces coplanares |
| Thread de render separada | corrida ao atualizar textura | mesma fila de entrega do `uploadMesh` |
| Não consigo compilar daqui | erros só aparecem no seu PC | fases pequenas, `ctest` e samples para cada peça |

---

## 11. Decisões em aberto

- **D1.** Pintor como ferramenta da engine (A, recomendada) ou modo do editor do jogo (B).
- **D2.** Densidade de texel alvo: medir no jogo com a câmera mais próxima e fixar o padrão (sugestão inicial: 32 px/m).
- **D3.** Formato do catálogo: um `catalogo.json` único ou um `.json` ao lado de cada `.glb`.
- **D4.** Os modelos importados virão do Blender com UV bom, ou a regra é sempre gerar UV único na importação?
- **D5.** ~~Nome do arquivo ao importar~~: decidido, sanitizado automaticamente (minúsculas, sem acento, hífen); só pergunta se o nome já existe.
- **D6.** ~~Giro do Carimbo~~: decidido, múltiplos de 90° (tecla R).

---

## 12. Referências

**Pintura e mapeamento**
- Hanrahan, P.; Haeberli, P. *Direct WYSIWYG painting and texturing on 3D shapes*. SIGGRAPH 1990. <https://doi.org/10.1145/97879.97903>
- Lévy, B. et al. *Least Squares Conformal Maps for Automatic Texture Atlas Generation*. SIGGRAPH 2002. <https://hal.archives-ouvertes.fr/inria-00100754v1>
- Sander, P.; Snyder, J.; Gortler, S.; Hoppe, H. *Texture Mapping Progressive Meshes*. SIGGRAPH 2001. <https://www.hhoppe.com/proj/tmpm/>
- Sander, P. et al. *Signal-Specialized Parametrization*. 2002. <https://hhoppe.com/proj/ssp/>
- Burley, B.; Lacewell, D. *Ptex: Per-Face Texture Mapping for Production Rendering*. EGSR 2008. <https://disneyanimation.com/publications/ptex-per-face-texture-mapping-for-production-rendering/>
- Yuksel, C.; Keyser, J.; House, D. *Mesh Colors*. ACM TOG 2010. <https://www.cemyuksel.com/research/meshcolors/meshcolors_SIGGRAPH2010.pdf>
- Mikkelsen, M. *Practical Real-Time Hex-Tiling*. JCGT 2022 (já usado na engine).
- Möller, T.; Trumbore, B. *Fast, Minimum Storage Ray-Triangle Intersection*. 1997. <https://www.scratchapixel.com/lessons/3d-basic-rendering/ray-tracing-rendering-a-triangle/moller-trumbore-ray-triangle-intersection.html>

**Produção de jogos**
- Olsen, M. (Insomniac). *The Ultimate Trim: Texturing Techniques of Sunset Overdrive*. GDC 2015. <https://gdcvault.com/play/1022323/The-Ultimate-Trim-Texturing-Techniques>
- Adobe. *Texture dilation or padding* (Substance 3D Painter). <https://experienceleague.adobe.com/en/docs/substance-3d-painter/using/technical-support/workflow-issues/export-issues/texture-dilation-or-padding>
- Foundry. *Texel Density* (Modo). <https://learn.foundry.com/modo/12.0/content/help/pages/uving/texel_density.html>
- Khronos. *KHR_texture_transform*. <https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_texture_transform/README.md>

**Bibliotecas**
- xatlas: <https://www.github.com/jpcy/xatlas>
- meshoptimizer: <https://github.com/zeux/meshoptimizer/releases>
- cgltf: <https://github.com/jkuhlmann/cgltf>
- tinybvh: <https://github.com/jbikker/tinybvh>
- Basis Universal: <https://github.com/binomialllc/basis_universal>
