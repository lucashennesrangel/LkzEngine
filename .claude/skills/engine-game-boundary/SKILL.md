---
name: engine-game-boundary
description: Fronteira entre a Lkz Engine e os jogos que a usam (ex. MafiaCity). Use SEMPRE antes de criar, mover ou editar código C++/CMake em D:\Dev-Games\Lkz-Engine ou em qualquer jogo em D:\Dev-Games\<Jogo>, ao decidir onde uma feature deve morar, ao adicionar componentes, sistemas, nós, assets ou dependências, e ao revisar mudanças que tocam os dois lados.
---

# Fronteira engine ↔ jogo

A Lkz Engine é um framework genérico usado por **vários** jogos. Ela nunca pode ficar acoplada a um jogo específico.

```
D:\Dev-Games\
├── tools\        toolchain compartilhado (cmake, ninja, llvm-mingw) — fora de qualquer repo
├── Lkz-Engine\   repo da engine  → bibliotecas lkz::*  + samples/ (não são jogos)
└── MafiaCity\    repo do jogo    → consome a engine por tag git (FetchContent)
```

## Regra única

**A dependência só vai do jogo para a engine.** O jogo inclui `<lkz/...>` e linka `lkz::*`. A engine nunca inclui, linka, nomeia ou assume nada de um jogo.

## Onde cada coisa mora

Pergunta-chave: *"um segundo jogo, de outro gênero, usaria isto sem mudar nada?"*

| Se for... | Vai para |
|---|---|
| Mecanismo genérico: loop, ECS, Scene Graph, render/física/input/áudio, math, logging, profiling, assets genéricos | **Engine** (`engine/core` ou `engine/backends/*`) |
| Regra, conteúdo ou tema do jogo: gangsters, famílias, distritos, economia, IA específica, balanceamento, nomes, assets, ids de mesh | **Jogo** (`<Jogo>/src`, `<Jogo>/assets`) |
| Útil para 2+ jogos mas nasceu num jogo | Generalize (sem nomes do jogo) e suba para a engine **depois** que o segundo uso existir ou for certo |
| Demo/teste de uma API da engine | `Lkz-Engine/samples/` (nunca um jogo dentro da engine) |

## Sinais de violação (pare e corrija)

- Arquivo da engine com nome ou termo de jogo (`Mafia`, `Gangster`, `Family`, `District`, `Player` em código de produção...).
- `#include` na engine apontando para fora de `lkz/` ou das dependências (EnTT, GLM, spdlog, Tracy; raylib só dentro de `engine/backends/renderer_raylib/src`).
- `if (jogo == ...)`, `#ifdef MAFIACITY` ou flag de CMake específica de um jogo na engine.
- Componente/sistema que só faz sentido para um jogo sendo adicionado em `engine/core/include/lkz/ECS/Components.hpp`.
- Jogo editando arquivos da engine para "encaixar" uma feature, em vez de usar um ponto de extensão.
- Jogo criado dentro de `Lkz-Engine/` (ex.: `Lkz-Engine/games/`).
- Jogo redeclarando EnTT/GLM/spdlog/Tracy/raylib com outra versão (elas chegam pela engine), ou incluindo `<raylib.h>` direto em vez de usar `lkz::RaylibRenderBackend`.
- Caminho absoluto para um jogo, ou para `D:\Dev-Games\<Jogo>`, em qualquer arquivo da engine.

## Pontos de extensão (o jogo usa estes, sem editar a engine)

- Comportamento de objeto persistente: subclasse de `lkz::SceneNode`, sobrescrevendo `onUpdate(float dt)`.
- Lógica em lote: `world.onFixedUpdate([](lkz::World&, float dt) { ... })` com grupos/views no `world.registry()`.
- Dados próprios: qualquer struct do jogo como componente EnTT (namespace do jogo, não `lkz`).
- Render/plataforma: implementar `lkz::RenderBackend` (backend genérico vai para `engine/backends`; algo exclusivo do jogo fica no jogo).
- Build: `lkz_add_game(<nome> SOURCES ... LIBRARIES lkz::...)`.

Se um ponto de extensão não existe, a mudança na engine deve criar um **mecanismo genérico** (hook, interface, callback), nunca código do jogo.

## Fluxo quando o jogo precisa de algo novo da engine

1. Projete a feature de forma genérica, com nomes neutros (`lkz::`), sem referência ao jogo.
2. Implemente na engine e cubra num sample em `Lkz-Engine/samples/` (ou estenda o `sandbox`).
3. Compile a engine sozinha: `..\tools\cmake\bin\cmake.exe --build --preset clang-debug` em `Lkz-Engine`.
4. No jogo, teste com o preset `engine-dev` (usa o código local da engine, sem tag).
5. Commit na engine → nova tag semver (`v0.2.0`; quebra de API = minor enquanto for 0.x) → atualize `LKZ_ENGINE_TAG` no `CMakeLists.txt` do jogo.

## Checagem rápida antes de concluir uma mudança

Na raiz da engine, isto não deve retornar nada:

```bash
grep -rniE 'mafia|gangster|famil|distrito|district|MafiaCity' engine cmake CMakeLists.txt
grep -rnE '#include ["<]' engine | grep -vE '["<](lkz/|entt/|glm/|spdlog/|tracy/|[a-z_]+(\.h)?>|windows\.h)'
```

(Atualize a primeira lista com termos de cada jogo novo.) Também confirme que a engine compila sozinha **e** que o jogo compila com `engine-dev`.
