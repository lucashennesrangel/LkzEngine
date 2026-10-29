# Lkz Engine

Framework pessoal em C++20 para jogos e simulações com mundos massivos, com núcleo **híbrido ECS + Scene Graph**. A especificação original está em [prompt-inicial.md](prompt-inicial.md).

- **ECS (EnTT 4.0)**: dados massivos em memória contígua, sistemas em lote sobre grupos owning.
- **Scene Graph**: hierarquia pai/filho com dirty flag; `worldMatrix` só é recalculada quando algo mudou.
- **Ponte (`SceneNode`)**: nó da árvore que é dono de uma entidade do ECS e só sincroniza quando o observer avisa.
- **Game loop**: passo fixo de 1/60 s com acumulador, `alpha` para interpolação, render em `std::jthread` separada.

## Engine e jogos são projetos separados

A engine não conhece nenhum jogo. Cada jogo é um repositório próprio que consome a engine por **tag git**:

```
D:\Dev-Games\
├── tools\        toolchain compartilhado (cmake, ninja, llvm-mingw)
├── Lkz-Engine\   este repositório: bibliotecas lkz::* + samples
└── MafiaCity\    jogo de exemplo, consome a engine por tag
```

A dependência vai só do jogo para a engine. As regras completas (onde cada coisa mora, sinais de violação, fluxo para pedir uma feature nova à engine) estão na skill [.claude/skills/engine-game-boundary/SKILL.md](.claude/skills/engine-game-boundary/SKILL.md).

### Usando a engine num jogo

```cmake
include(FetchContent)
FetchContent_Declare(LkzEngine
    GIT_REPOSITORY D:/Dev-Games/Lkz-Engine      # ou a URL do repositório remoto
    GIT_TAG        v0.1.0)
FetchContent_MakeAvailable(LkzEngine)

lkz_add_game(meu_jogo
    SOURCES   src/main.cpp
    LIBRARIES lkz::renderer_null)
```

EnTT, GLM, spdlog e Tracy chegam pela engine; o jogo não os declara. Para editar engine e jogo ao mesmo tempo sem criar tag, configure o jogo com `-DFETCHCONTENT_SOURCE_DIR_LKZENGINE=D:/Dev-Games/Lkz-Engine` (o MafiaCity já traz isso no preset `engine-dev`).

Pontos de extensão:
- **Comportamento:** subclasse de `lkz::SceneNode` sobrescrevendo `onUpdate()`.
- **Sistemas em lote:** `world.onFixedUpdate(...)`.
- **Dados do jogo:** qualquer struct do jogo vira componente EnTT.
- **Render:** `lkz::RenderBackend`.

### Versionamento

Semver em tags (`v0.1.0`, `v0.2.0`...). Enquanto a versão for 0.x, quebra de API sobe o *minor*. Cada jogo fixa a tag que usa e só muda de versão quando quiser.

## Toolchain (sem Visual Studio)

Toolchain portátil em `D:\Dev-Games\tools`, **ao lado** dos projetos (os presets usam `${sourceParentDir}/tools`), compartilhado entre a engine e todos os jogos:

| Pasta | O quê | Origem |
|---|---|---|
| `tools/cmake/` | CMake 4.4.5 | `cmake-4.4.5-windows-x86_64.zip` em <https://github.com/Kitware/CMake/releases> |
| `tools/ninja/` | Ninja 1.13.2 | `ninja-win.zip` em <https://github.com/ninja-build/ninja/releases> |
| `tools/llvm-mingw/` | clang/LLVM 23.1.4 (llvm-mingw 20261020, UCRT): compilador, `clangd`, `lldb-dap`, `clang-format`, `clang-tidy` | `llvm-mingw-20261020-ucrt-x86_64.zip` em <https://github.com/mstorsjo/llvm-mingw/releases> |

Em outra máquina, basta extrair os três zips com esses nomes de pasta. É preciso ter o Git no PATH. Os executáveis são linkados com `-static` e rodam sem DLLs do toolchain.

## VS Code

Extensões (recomendadas em [.vscode/extensions.json](.vscode/extensions.json)):

- **clangd** (`llvm-vs-code-extensions.vscode-clangd`): autocomplete, ir para definição, referências, erros em tempo real.
- **CMake Tools** (`ms-vscode.cmake-tools`): escolher preset, compilar e rodar pela barra de status.
- **LLDB DAP** (`llvm-vs-code-extensions.lldb-dap`): debugger (F5).

Fluxo: abra a pasta → CMake Tools configura (preset **Clang (../tools) / Debug**) → **F7** compila → **Shift+F5** roda → **F5** debuga. Não instale a extensão *C/C++* da Microsoft: o IntelliSense dela conflita com o clangd.

## Build pelo terminal

```powershell
..\tools\cmake\bin\cmake.exe --preset clang-debug      # ou clang-release (RelWithDebInfo, para profiling)
..\tools\cmake\bin\cmake.exe --build --preset clang-debug
.\build\clang-debug\bin\sandbox.exe 2 5000             # [segundos] [entidades]
```

Opções: `-DLKZ_ENABLE_PROFILING=OFF` desliga o Tracy; `-DLKZ_BUILD_SAMPLES=OFF` compila só as bibliotecas (é o padrão quando a engine é consumida por um jogo).

## Estrutura

```
CMakeLists.txt              raiz: opções; samples só quando top level
CMakePresets.json           presets clang-* (toolchain em ../tools) e ninja-*
cmake/
  Dependencies.cmake        FetchContent: EnTT, GLM, spdlog, Tracy
  LkzEngine.cmake           lkz_target_defaults(), lkz_add_game() — usados também pelos jogos
engine/
  core/                     lkz::core (static library), headers em <lkz/...>
    include/lkz/
      Core/Log.hpp          initLogging()
      ECS/Components.hpp    Position, Velocity, Rotation, Scale, Render, SceneLink
      Scene/Entity.hpp      nó base do Scene Graph (abstrato)
      Scene/SceneNode.hpp   ponte Scene Graph <-> ECS
      Scene/Camera.hpp      câmera como nó da árvore
      Render/RenderFrame.hpp   snapshots enviados para a thread de render
      Render/RenderBackend.hpp interface dos backends gráficos
      Systems/PhysicsSystem.hpp
      Systems/RenderSystem.hpp
      World.hpp             registry + cena + sistemas + game loop
  backends/
    renderer_null/          lkz::renderer_null (<lkz/backends/NullRenderBackend.hpp>)
    renderer_bgfx/          a fazer
    physics_jolt/           a fazer
samples/
  sandbox/                  teste de fumaça do núcleo (não é um jogo)
third_party/                clones locais opcionais das dependências
.claude/skills/             skill engine-game-boundary
```

## Como um frame acontece

**Thread lógica** (`World::run`), a cada passo fixo:

1. callbacks registrados com `world.onFixedUpdate(...)`
2. `Entity::update` → `onUpdate()` de cada nó
3. `PhysicsSystem::update` → grupo owning `<Position, Velocity>`
4. sync da cena → `SceneNode::syncFromEcs()` só para as entidades que o observer marcou
5. `Entity::updateWorldTransform` → dirty flag, guarda `previousWorld` e recalcula `worldMatrix`

Depois dos passos, calcula `alpha = accumulator / fixedDt` e o `RenderSystem::pack` copia os snapshots anterior/atual para um `RenderFrame`.

**Thread de render** (`std::jthread`): espera um frame novo (triple buffer + `condition_variable_any` com `stop_token`), interpola (`glm::mix` na posição e escala, `slerp` na rotação) e entrega `DrawCommand`s ao backend. Ela nunca acessa o registry nem a árvore.

### Regras que mantêm o desempenho

- **Entidade ligada a um `SceneNode`**: mude Position/Rotation/Scale com os setters do nó ou com `registry.patch()`/`replace()`. Escrita direta por referência não avisa o observer e o nó não se move.
- **Entidade pura do ECS**: escreva direto por referência dentro de grupos/views. Nada de `patch()` no hot path.
- Sem alocação por frame no hot path: reaproveite vetores (`clear()` mantém a capacidade).
- Não destrua um nó de dentro do próprio `onUpdate()`.
- `PositionComponent` e `VelocityComponent` pertencem ao grupo owning da física: um jogo pode criar grupos que sejam donos dos *próprios* componentes e só leiam esses.

## Profiling com Tracy

O cliente é o **Tracy v0.13.1**, e o profiler (GUI) precisa ser da mesma versão: <https://github.com/wolfpld/tracy/releases/tag/v0.13.1>. O build usa `TRACY_ON_DEMAND`, então só há coleta com o profiler conectado.

Zonas instrumentadas: `World::fixedUpdate`, `Entity::update`, `Entity::updateWorldTransform`, `PhysicsSystem::update`, sync da cena, `RenderSystem::pack` e `RenderSystem::render`.

## Diferenças em relação ao prompt inicial

- **`entt::observer` não existe no EnTT 4.0**: foi substituído pelo *reactive storage* (`registry.storage<entt::reactive>()` com `on_construct`/`on_update`), com o mesmo papel.
- **Tracy v0.13.1** no lugar de v0.8.2, que é de 2022 e usa outro layout de headers.
- **Jogos fora da engine**, cada um no próprio repositório; a engine só tem samples.
- Além de Position, os nós têm **Rotation** e **Scale** (a `localMatrix` é T·R·S), e há um nó **Camera**.

## Próximos passos

1. Abrir o sandbox no Tracy e conferir o custo de `PhysicsSystem::update`.
2. Backend gráfico: `renderer_bgfx` (trilha A) ou `renderer_raylib` (trilha B). Veja [engine/backends/renderer_bgfx/README.md](engine/backends/renderer_bgfx/README.md).
3. Física com Jolt: [engine/backends/physics_jolt/README.md](engine/backends/physics_jolt/README.md).
4. Input e janela (junto com o backend gráfico).
