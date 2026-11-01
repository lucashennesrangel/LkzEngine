# Pintor de modelos

Abre um modelo glTF (`.glb` ou `.gltf`) e pinta direto sobre a forma 3D, com cores ou materiais repetíveis de uma paleta. Carimba decalques (janela, porta, letreiro), escolhe o destino de cada face e exporta um `.glb`.

É uma ferramenta da engine, não de um jogo. O jogo diz onde ficam os modelos e a paleta num arquivo de projeto.

```
painter [projeto.json] [modelo.glb] [--d3d12 | --vulkan]
```

## Arquivo de projeto

```json
{
  "nome": "MeuJogo",
  "abrir": ["assets/models", "assets/models/_fonte"],
  "saida": "assets/models",
  "projetos": "assets/models/_projetos",
  "paleta": "assets/textures/materials",
  "decalques": "assets/textures/decals"
}
```

Os caminhos são relativos ao próprio arquivo:

| Campo | O que é |
|---|---|
| `abrir` | pastas que a janela **Abrir** mostra |
| `saida` | a exportação vai para uma subpasta dela, escolhida na janela **Exportar** (as que começam com `_` não aparecem). Se o modelo já mora na subpasta escolhida, é substituído no lugar. |
| `projetos` | antes de substituir um arquivo, a versão anterior é copiada para `projetos/<nome>/original.glb`, uma vez só |
| `paleta` | opcional: a pasta dos materiais repetíveis |
| `decalques` | opcional: a pasta das imagens do Carimbo |

Sem projeto, a exportação grava `<nome>_pintado.glb` ao lado do modelo aberto.

## Paleta

Cada imagem da pasta (`.png`, `.jpg`) é um material. O nome do arquivo sem extensão é a chave gravada nos modelos. Um `.json` opcional de mesmo nome fica ao lado:

```json
{ "nome": "Tijolo", "metrosPorRepeticao": 1.0 }
```

`metrosPorRepeticao` é quanto de superfície uma imagem cobre (padrão: 1 m). O botão **Repete a cada − / +** muda a escala e grava o `.json`.

A imagem precisa repetir sem costura.

### Tingir um material

Com **Tingir com a cor** ligado (na aba Material da paleta), o balde e o pincel põem a imagem do material **multiplicada pela cor** escolhida: a textura dá os tons (pinceladas, manchas, furos) e a cor dá a cor. Uma só imagem serve para paredes de qualquer cor.

- Funciona melhor com uma imagem clara, quase branca e sem cor própria (média perto de RGB 225 a 235). Uma imagem colorida também tinge, mas a cor dela se mistura com a escolhida.
- No `.glb`, cada combinação de material e cor é um material próprio que aponta para a **mesma** imagem, com a cor no `baseColorFactor` (nome `paleta_<id>_<rrggbb>`). Não ocupa memória de vídeo a mais.
- O conta-gotas sobre uma face tingida pega o material e a cor.

Sobre o tamanho da imagem: com 1 m por repetição, uma de 512 px já dá 512 px por metro, bem mais do que a câmera de um jogo isométrico mostra. Use 1024 só para ver de muito perto. Cada material ocupa na memória de vídeo uma vez só, por mais modelos que o usem: 1,3 MiB a 512 px e 5,3 MiB a 1024 px.

## Decalques (Carimbo)

Imagens postas uma vez, sem repetir nem esticar: janela, porta, letreiro. A pasta `decalques` funciona como a paleta, com um `.json` opcional ao lado de cada imagem:

```json
{ "nome": "Janela", "larguraMetros": 1.2 }
```

`larguraMetros` é a largura de um carimbo dado com um clique só (padrão: 1 m).

- **Clique e arraste** numa face: o clique é um canto, o arrasto define o tamanho. A altura segue a proporção da imagem, então ela nunca distorce.
- **Antes de confirmar:** arraste dentro para mover, num canto para mudar o tamanho; **R** gira 90°. **Enter**, ou um clique fora, confirma; **Esc** cancela.
- **Um clique sem arrastar** repete o tamanho do último carimbo daquela imagem, para várias janelas iguais.
- **O carimbo vale para o plano da face clicada.** As faces desse plano são **cortadas** no retângulo, e os pedaços de dentro ganham a imagem, com o UV no retângulo. O resto da parede fica como estava: compartilhada, única ou de cor.
  - Por isso funciona em parede Compartilhada, e a janela aparece na resolução da própria imagem, não na do atlas.
- **Na exportação:** o material do decalque aponta para a imagem da pasta (caminho relativo, como a paleta), então todos os carimbos de uma imagem, em qualquer modelo, usam uma textura só na GPU.
  - Imagem com transparência: cada carimbo ganha uma imagem composta sobre o que a face mostrava (vai embutida, em PNG).
- Desfazer devolve a parede inteira. O UV único deixa os decalques como estão.

### Cor do carimbo

Em **Cor do carimbo** (no Carimbo, ou com um carimbo pego pela Mão), escolha o modo e uma cor da paleta:

| Modo | O que faz | No `.glb` |
|---|---|---|
| **Original** | a imagem como é | a imagem da pasta |
| **Multiplicar** | a imagem toda vezes a cor: o branco vira a cor, o escuro continua escuro, as cores próprias escurecem | a mesma imagem da pasta, com a cor no `baseColorFactor` (sem memória a mais) |
| **Só o branco** | só os pixels sem saturação (branco, cinza) pegam a cor; ferrugem, vidro e maçaneta ficam como são | uma imagem tingida embutida, uma por decalque e cor |

"Só o branco" mede a saturação de cada pixel (HSV: (máx − mín) / máx). Abaixo de 8% o pixel pega a cor inteira; acima de 25% fica como é; entre os dois, uma passagem suave, para a borda da ferrugem não serrilhar. É a máscara de tinta dos jogos (o canal que diz onde a cor do jogador vale), calculada da própria imagem em vez de desenhada à mão.

Com transparência, a cor entra na imagem composta de cada carimbo (nos dois modos).

### Profundidade (afundar ou saltar)

**Profundidade** (−5, −1, +1, +5 cm; 0 volta ao plano) vale para o carimbo pendente e para os próximos:
- **negativa** afunda o retângulo na parede: porta, janela, nicho. As bordas do buraco ganham as **paredes do vão** (topo, laterais e soleira), viradas para dentro;
- **positiva** faz o retângulo saltar: peitoril, marquise, placa. As laterais ficam viradas para fora.

As paredes do vão levam o destino da parede de onde saíram. Na parede Compartilhada, a textura fica na escala certa (a projeção em metros segue a normal de cada face). Na Única, o UV da borda é esticado; um balde Compartilhado nelas resolve.

Cada vão custa 8 triângulos por material de parede. A Mão pega o carimbo com o vão: tirá-lo fecha a parede, e movê-lo refaz o vão no lugar novo, um passo só de desfazer. As paredes do vão não são pegáveis sozinhas.

Uma profundidade maior que a espessura da parede atravessa o modelo: o Pintor não confere a espessura.

## Mão (ajustar um carimbo já posto)

Ferramenta **Mão** (H). O ponteiro do mouse mostra o que dá para fazer: mão sobre um carimbo, cruz de mover dentro dele, diagonal nos cantos.

- **Clique num carimbo** para selecioná-lo (fica realçado); **Shift+clique** soma ou tira outros. Clicar fora, ou **Esc**, tira a seleção.
- **Com seleção:** a **cor**, a **profundidade** e o giro (**R**) mudam os selecionados na hora, na parede de verdade, num passo só de desfazer. O último carimbo posto com o Carimbo também fica selecionado, então os botões mudam ele logo depois de pôr.
- **Arraste** um carimbo para mover; **um canto** do selecionado muda o tamanho (**Shift** solta a proporção). Soltar confirma.
- **Ctrl+C** copia o selecionado (com a cor e a profundidade). **Ctrl+V** cola: ele segue o mouse de parede em parede, e **cada clique põe um**, até o **Esc**. Serve para pôr uma fileira de portas iguais.
- **Delete** apaga os selecionados.
- Cada mudança é um passo só de desfazer.
- O buraco de um carimbo que sai pega o destino atual da parede: um balde dado depois do carimbo vale também ali.
- Os carimbos continuam ajustáveis depois de **exportar e reabrir** o `.glb`, e depois do **UV único**:
  - a exportação grava a lista deles em `extras.pintorCarimbos` do modelo: decalque, plano, retângulo, giro, cor e profundidade. O jogo ignora esse campo;
  - ao reabrir, cada carimbo pega de volta os triângulos de decalque do retângulo dele cujo UV é o dele, mais as paredes do vão;
  - num `.glb` de antes da lista, o UV de cada grupo de triângulos de decalque diz o retângulo e o giro, e o carimbo volta, só os rentes à parede;
  - ao tirar um carimbo reaberto, o buraco ganha o acabamento da parede em volta. Uma pincelada à mão que estivesse embaixo não volta;
  - ficam fixos os decalques cujo UV não é um retângulo de carimbo (torto, ou vindo de fora do Pintor).

## Esculpir (mudar a forma do modelo)

Ferramenta **Esculpir** (X): muda a malha de verdade, como um Empurrar/Puxar do SketchUp ou o Extrudar do Blender, só com o que serve para prédios.

1. **Selecione** faces:
   - **Triângulo**, **Parede** ou **Plano**: o clique pega como o balde. **Shift+clique** soma ou tira. **Esc** limpa.
   - **Retângulo**: arraste numa parede para desenhar. O Pintor corta a parede ali, sem mudar a aparência, e o recorte fica selecionado. Serve para fazer um nicho ou uma porta onde não havia nada.
   - Clicar numa porta carimbada pega as faces dela.
2. **Escolha o modo:**
   - **Puxar**: a seleção anda ao longo da normal e ganha paredes novas nas bordas; o resto do modelo fica. Negativo afunda (porta, nicho, janela recuada); positivo salta (marquise, peitoril, platibanda).
   - **Mover**: a seleção anda e as faces vizinhas **esticam junto**, sem paredes novas: deforma o modelo. Serve para inclinar um telhado ou avançar um andar.
3. **Mova:**
   - **arraste** uma face selecionada: a forma muda ao vivo, em passos de 1 cm (**Shift** solta) e ao longo da normal da face clicada. Soltar confirma;
   - ou use **−5, −1, +1, +5 cm**: os botões somam até **Enter** (ou trocar de ferramenta); **Esc** desfaz o que não foi confirmado.

Cada escultura é um passo só de desfazer. Depois de confirmar, as faces esculpidas continuam selecionadas, então dá para puxar de novo.

Como funciona por baixo:
- os vértices na mesma posição (até 0,1 mm) são soldados. É assim que o Pintor sabe quais arestas são a borda da seleção e quais faces são vizinhas;
- antes de mover, as arestas da região que têm um vértice de outro triângulo no meio (**vértice em T**, deixado pelos cortes dos carimbos) são divididas. Sem isso, o Mover abriria frestas e o Puxar criaria paredes falsas por dentro;
- as paredes novas do Puxar levam o acabamento da face de fora encostada na borda (a parede em volta). Na parede Compartilhada, a textura sai na escala certa; na Única, o UV é a borda esticada. O "Gerar UV único" refaz sem distorção.

Limites:
- a direção é sempre a normal da face clicada; ainda não há mover na diagonal, girar ou escalar;
- o Pintor não confere se a forma atravessa outra parte do modelo;
- um carimbo dentro da área esculpida anda junto. A Mão ainda o pega, mas, depois de um Mover que incline a parede, recolocá-lo pode não encaixar.

## Importar imagens

**Importar imagem…** (na aba Material da paleta, ou no Carimbo) abre o navegador fora do projeto: Imagens, Downloads, Área de trabalho, o projeto e os discos.

- O arquivo escolhido é **copiado** para a pasta da paleta ou dos decalques, com o nome em minúsculas, sem acento e com hífen (`Janela Azul.PNG` → `janela-azul.png`), e ganha o `.json` com o nome original e o tamanho padrão.
- Se já existe um arquivo com esse nome: **Substituir** (os modelos que usam a imagem mudam junto) ou **Outro nome** (`janela-azul-2.png`).
- A imagem nova entra na lista na hora, já escolhida.

## Destinos

Cada face tem um destino, que diz como ela vai para o `.glb`:

| Destino | O que mostra | Na exportação |
|---|---|---|
| **Único** | a tela do material dela (o padrão ao abrir) | a tela vira a imagem do material, embutida, no UV do modelo |
| **Compartilhado** | um material da paleta, repetido em metros (tingido ou não) | um material que **aponta** para a imagem da paleta (caminho relativo ao `.glb`), com UV em metros e a tinta no `baseColorFactor`; `extras.lkz` guarda o nome e a escala |
| **Só cor** | uma cor lisa | todas as cores lisas num material só, com uma amostra de 4 × 4 px por cor (PNG, sem perda) |
| **Decalque** | a imagem de um decalque, no UV do retângulo do carimbo | um material que aponta para a imagem do decalque (ou a composta, embutida) |

Na projeção dos materiais compartilhados:
- **Paredes e faces inclinadas:** `u` corre na horizontal, para a direita de quem olha a face, e `v` desce pela face. Numa parede, `v = −y`, então as fiadas de tijolo ficam horizontais e começam no chão.
- **Faces quase horizontais:** `u = x` e `v = z`.

Ao reabrir um `.glb` exportado, os destinos voltam. Um material compartilhado que não está na paleta abre como único, com a imagem dele.

O backend raylib e o NVRHI carregam cada imagem externa **uma vez por arquivo**. Dois prédios com o mesmo tijolo usam a mesma textura na GPU.

## Ferramentas

- **Tinta:** uma cor, ou um material da paleta (clique na miniatura).
- **Pincel:** pinta a tinta na tela das faces únicas. Com um material, pinta a imagem dele na escala real, com a mesma projeção do destino Compartilhado.
  - O pincel mede a distância **na superfície**, em metros, e não no UV, então o traço atravessa as emendas do UV sem quebrar.
  - Só pinta o que está virado para a câmera, então não passa para o outro lado de uma parede fina.
  - Um traço não acumula tinta onde as pinceladas se sobrepõem.
- **Balde:** as faces sob o mouse ficam realçadas, e o clique aplica a tinta com o destino escolhido.
  - **Pegar:**
    - **Triângulo**;
    - **Parede**: as do mesmo plano e do mesmo material, ligadas por arestas;
    - **Plano**: todas do mesmo plano;
    - **Material**: todas do mesmo material do modelo.
  - **Destino:**
    - **Compartilhado**: precisa de um material;
    - **Único**: pinta na tela;
    - **Só cor**: precisa de uma cor;
    - **Original**: volta como abriu, com a pintura.
- **Borracha:** volta a tela à cor que tinha ao abrir.
- **Conta-gotas:** pega a cor, ou o material da paleta, da face.
- **Desfazer:** vale para pinceladas e baldes. Guarda blocos de 64 × 64 px, até 512 MB.

## Telas e UV

- **Uma tela por material.** Cada material com UV ganha uma imagem para pintar: a dele (com a cor base já aplicada) ou, se ele só tinha cor, uma imagem nova de 32 px/m.
- **UV sobreposto** (várias faces na mesma região da imagem, comum nos modelos de atlas): pintar uma face pinta todas as que dividem aquela região, e o pincel fica lento porque a imagem costuma cobrir poucos centímetros. Antes de pintar (pincel, borracha ou balde Único) numa tela assim, o Pintor pergunta:
  - **Gerar UV único** (o normal);
  - **Pintar assim mesmo**, até abrir outro modelo;
  - **Cancelar**.
- **Gerar UV único** dá às faces únicas um UV sem sobreposição e assa a pintura atual nele (`lkz::asset::bakeUnique`).
  - As faces compartilhadas e de cor lisa ficam como estão. Por isso, deixar as paredes grandes como Compartilhado antes do UV único deixa o resto mais nítido.
  - Uma face sem tela (material sem UV) que recebeu Único fica com a aparência pedida até o UV único assá-la.
- **Exportação:**
  - as telas vão embutidas, em JPEG, ou PNG se houver transparência;
  - os materiais da paleta vão por referência;
  - o arquivo é relido para conferir (inclusive se a imagem da paleta abre pelo caminho gravado), e o relatório aparece na tela.

## Comandos

| Ação | Mouse / tecla |
|---|---|
| Pintar / aplicar o balde / carimbar | botão esquerdo |
| Conta-gotas | Alt + esquerdo, ou ferramenta (I) |
| Girar / mover / aproximar | direito / meio ou Shift + direito / roda |
| Tamanho do pincel | Ctrl + roda, `[` e `]`, ou os botões |
| Pincel, balde, carimbo, mão, esculpir, borracha | B, G, C, H, X, E |
| Carimbo: girar, confirmar, cancelar | R, Enter, Esc |
| Mão: copiar, colar, apagar | Ctrl+C, Ctrl+V, Delete |
| Enquadrar | F |
| Desfazer, refazer | Ctrl+Z, Ctrl+Y (ou Ctrl+Shift+Z) |
| Abrir, exportar | Ctrl+O, Ctrl+S |
| Fechar a janela aberta | Esc (Esc não fecha o programa) |

## Código

| Arquivo | O que faz |
|---|---|
| `main.cpp` | janela, interface (`ui/painter.rml`), atalhos, pincel ao longo do mouse, realce do balde, carimbo com prévia, Mão (ponteiro, copiar e colar), navegador (abrir e importar) e exportar |
| `Document.*` | modelo aberto, telas, destinos por face, seleção, balde, pinceladas, carimbo (corte da malha, e tirá-lo de volta: `lift`), desfazer, exportação e UV único. Não depende de janela: testado em `tests/painter_test.cpp` |
| `Palette.*` | as pastas de imagens (paleta e decalques), a importação, a amostragem com mipmaps e os eixos de uma face em metros (`surfaceAxes`, `surfaceMeters`) |
| `OrbitCamera.*` | câmera que gira em volta do modelo |
| `Project.*` | o arquivo de projeto |

Usa do `lkz::asset`:
- `loadGltf` / `saveGlb` (destinos em `extras.lkz`, imagens externas por caminho relativo);
- `MeshRaycaster`: raio do mouse e triângulos perto do pincel;
- `forEachTexel` / `SurfaceToUv`: texels de um triângulo;
- `bakeUnique`, `mipChain` e `sampleBilinear`.

Da engine, usa `World::uploadTexture` / `updateTexture` e `World::setMouseCursor`.
