#pragma once

#include <cstdint>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// Componentes puros: só dados, sem lógica. Ficam contíguos nas storages do EnTT.
//
// Regra de escrita:
// - Entidades puras do ECS: sistemas escrevem direto por referência (hot path, sem sinais).
// - Entidades ligadas a um SceneNode: Position/Rotation/Scale mudam via registry.patch()
//   ou replace() (ou pelos setters do SceneNode), para que o observer da cena seja avisado.

namespace lkz {

class SceneNode;

/// Posição no espaço do pai (nós do Scene Graph) ou no mundo (entidades puras).
/// `previous` é o valor do passo fixo anterior, usado na interpolação visual.
struct PositionComponent {
    glm::vec3 value{0.0f};
    glm::vec3 previous{0.0f};
};

struct VelocityComponent {
    glm::vec3 value{0.0f};
};

struct RotationComponent {
    glm::quat value{1.0f, 0.0f, 0.0f, 0.0f}; // (w, x, y, z): identidade
};

struct ScaleComponent {
    glm::vec3 value{1.0f};
};

/// O que desenhar. Os ids são resolvidos pelo backend de renderização.
struct RenderComponent {
    std::uint32_t mesh = 0;
    std::uint32_t material = 0;
    glm::vec4 color{1.0f};
};

/// Liga uma entidade do ECS ao nó do Scene Graph que a representa.
/// Criado e removido pelo próprio SceneNode.
struct SceneLinkComponent {
    SceneNode* node = nullptr;
};

} // namespace lkz
