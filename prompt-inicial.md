Prompt: Framework Pessoal de Engine — “Híbrida ECS + Scene Graph”
1. Ideia central
Construir um framework pessoal em C++20/23 para jogos e simulações com mundos massivos, sem a intenção de distribuição comercial. O objetivo é ter controle total sobre a arquitetura, aprender os fundamentos de performance de baixo nível e poder evoluir cada subsistema no seu ritmo, sem as amarras de uma engine comercial.

O núcleo é híbrido: dados massivos ficam no ECS (data-oriented, memória contígua), enquanto a hierarquia semântica do mundo (pai/filho, transformações em cascata) fica em um Scene Graph baseado em classes. A ponte entre os dois é feita por nós que referenciam entidades do ECS.

2. Conceitos arquiteturais obrigatórios
2.1. ECS com EnTT

Componentes puros (PositionComponent, VelocityComponent, RenderComponent) armazenados de forma contígua.

Sistemas em lote (PhysicsSystem, RenderSystem) que iteram por grupos owning no hot path, não por views genéricas.

Uso de entt::observer para reagir a mudanças de componentes sem polling manual frame a frame.

2.2. Scene Graph com Dirty Flag

Classe base Entity com uint64_t id, nome, parent, std::vector<std::unique_ptr<Entity>> children.

Matrizes localMatrix e worldMatrix (glm::mat4).

Flag isDirty: a multiplicação parentWorld * local só ocorre quando o nó ou um ancestral foi alterado.

2.3. Ponte Híbrida (SceneNode)

Subclasse concreta de Entity que guarda entt::entity e uma referência ao registry.

Sincroniza a matriz local a partir dos componentes ECS apenas quando o observer dispara, eliminando polling desnecessário.

2.4. Game Loop de Alta Precisão

Passo fixo para física/lógica (1/60 s) com acumulador std::chrono.

Cálculo de alpha (accumulator / fixedDt) para interpolação visual entre os dois últimos estados de física.

Renderização em std::jthread separada, com stop_token e sincronização por condition_variable.

2.5. Interpolação Visual

Armazenar dois snapshots de transformação (previousWorld, currentWorld) por nó.

No momento do desenho, interpolar com glm::mix(previous, current, alpha).

3. Stack de pacotes
3.1. Núcleo (obrigatório desde o primeiro dia)
Pacote	Versão alvo	Papel	Integração
EnTT	v4.0.0 (C++20 obrigatório, CMake ≥ 3.28)	ECS, observers, grupos	FetchContent
GLM	1.0.3	Matemática (mat4, vec3, mix)	FetchContent
spdlog	1.17.0	Logging estruturado	FetchContent
3.2. Física (quando o esqueleto estiver estável)
Pacote	Versão alvo	Papel	Integração
Jolt Physics	v5.5.0 ou mais recente	Física rígida multicore	add_subdirectory ou FetchContent
Jolt é hoje a escolha padrão para engines 3D CPU-driven que valorizam escalabilidade multicore. A integração com ECS segue o padrão: ler PositionComponent → passo da simulação → escrever transforms de volta.

3.3. Renderização (escolha uma trilha)
Trilha A — Abstração cross-API (recomendada para começar):

bgfx (via bgfx.cmake, versão v1.136.x ou linha 1.14x). Abstrai Vulkan, DX12, Metal e OpenGL por baixo. Você escreve o backend uma vez e roda em qualquer lugar.

Trilha B — Prototipagem rápida:

Raylib 6.2. API simples, ótima para validar a lógica do jogo antes de partir para um backend Vulkan/DX12 nativo.

Trilha C — Performance máxima (longo prazo):

Vulkan SDK ou DirectX 12 Agility SDK. Maior controle, maior custo de implementação.

3.4. Profiling (não opcional em um framework de performance)
Pacote	Versão alvo	Papel	Integração
Tracy Profiler	0.8.2 ou mais recente	Frame profiler com resolução de nanossegundos	FetchContent ou add_subdirectory com option(TRACY_ENABLE ON)
Tracy deve entrar no primeiro build funcional. Sem medição, otimização de cache locality vira adivinhação.

3.5. Build system
CMake ≥ 3.28 (exigido pelo EnTT 4.x).

FetchContent para dependências pequenas/médias: baixa e compila no configure time, sem gerenciador de pacotes.

find_package apenas para bibliotecas grandes ou já instaladas no sistema.

4. Estrutura de diretórios sugerida
text
meu-framework/
├── CMakeLists.txt
├── cmake/
│   └── Dependencies.cmake        # FetchContent de EnTT, GLM, spdlog, Tracy
├── core/                         # static library
│   ├── include/core/
│   │   ├── ECS/Components.hpp
│   │   ├── Scene/Entity.hpp
│   │   ├── Scene/SceneNode.hpp
│   │   ├── Systems/PhysicsSystem.hpp
│   │   ├── Systems/RenderSystem.hpp
│   │   └── World.hpp
│   └── src/
├── backends/                     # implementações de renderer/física
│   ├── renderer_bgfx/            # ou renderer_raylib, renderer_vulkan
│   └── physics_jolt/
├── game/                         # executável de teste
│   └── main.cpp
└── third_party/                  # cache de dependências (opcional)
O core é uma static library; o game é um executável que linka core + backends escolhidos. Isso permite trocar o backend sem tocar no núcleo.

5. Esqueleto do CMakeLists.txt raiz
cmake
cmake_minimum_required(VERSION 3.28)
project(MeuFramework LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# --- EnTT ---
FetchContent_Declare(
    entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG        v4.0.0
    GIT_SHALLOW    TRUE
)

# --- GLM ---
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.3
    GIT_SHALLOW    TRUE
)

# --- spdlog ---
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE
)

# --- Tracy (opcional, mas recomendado) ---
option(TRACY_ENABLE "" ON)
FetchContent_Declare(
    tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.8.2
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(entt glm spdlog tracy)

add_library(core STATIC)
target_link_libraries(core PUBLIC EnTT::EnTT glm::glm spdlog::spdlog TracyClient)

add_executable(game game/main.cpp)
target_link_libraries(game PRIVATE core)
6. Prompt para orientar a implementação (copie e use como spec)
Missão: Implementar o núcleo do framework “Híbrida ECS + Scene Graph” em C++20, usando EnTT 4.x, GLM 1.0.3, spdlog 1.17 e Tracy. O objetivo é ter um executável de teste que povoe o cenário com entidades persistentes no Scene Graph e 5.000 entidades puras no ECS, rodando um loop de passo fixo com renderização em std::jthread separada.

Entregáveis mínimos:

Entity abstrata com hierarquia, id, parent, children, localMatrix, worldMatrix, isDirty.

SceneNode que referencia entt::entity e sincroniza a matriz local via entt::observer apenas quando PositionComponent for substituído.

PhysicsSystem iterando por grupo owning de PositionComponent + VelocityComponent.

RenderSystem que apenas empacota dados de renderização (backend no-op nesta fase).

World com run() contendo acumulador de passo fixo, cálculo de alpha e thread de renderização com std::jthread.

Interpolação visual armazenando previousWorld e currentWorld por nó e aplicando glm::mix no momento do desenho.

main() que cria Root, Player, Weapon (filho de Player), Camera, chama stressTest(5000) e roda o loop por 2 segundos.

Restrições:

Sem alocações por frame no hot path da física.

Sem polling manual de componentes ECS no SceneNode.

Tracy instrumentation nas funções PhysicsSystem::update, Entity::update e RenderSystem::render.

Logging via spdlog em pontos-chave (criação de entidades, início/fim do loop, erros de observer).

Com esse documento, você tem um plano de execução concreto: a ideia está clara, os conceitos arquiteturais estão travados e a stack está definida com versões e forma de integração. O próximo passo natural é criar o repositório, configurar o CMakeLists.txt e implementar os entregáveis na ordem listada — começando pelo núcleo (core) e deixando os backends gráfico e de física para quando o loop e o ECS já estiverem estáveis.