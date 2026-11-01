#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "lkz/ECS/Components.hpp"
#include "lkz/Input/Input.hpp"
#include "lkz/UI/Ui.hpp"

namespace lkz {

struct CameraSnapshot {
    glm::mat4 previousWorld{1.0f};
    glm::mat4 currentWorld{1.0f};
    float fovY = 1.0471976f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    bool active = false;
};

/// Nó do Scene Graph: transformação completa (hierarquia já resolvida).
struct NodeSnapshot {
    glm::mat4 previousWorld{1.0f};
    glm::mat4 currentWorld{1.0f};
    RenderComponent render{};
};

/// Entidade pura do ECS: só posição.
struct InstanceSnapshot {
    glm::vec3 previous{0.0f};
    glm::vec3 current{0.0f};
    RenderComponent render{};
};

/// Pacote que a thread lógica entrega para a thread de render.
///
/// Carrega os dois últimos snapshots de cada objeto (anterior e atual) e o alpha do
/// acumulador. A interpolação acontece só na hora de desenhar, na thread de render,
/// que nunca toca no registry nem no Scene Graph.
struct RenderFrame {
    std::uint64_t tick = 0;
    float alpha = 0.0f;
    CameraSnapshot camera{};
    std::vector<NodeSnapshot> nodes;
    std::vector<InstanceSnapshot> instances;
    std::string overlay; // texto sobre a imagem (World::setOverlayText); vazio = nada
    std::vector<std::pair<std::string, UiValue>> ui; // valores da interface (World::setUiValue), todos
    MouseCursor cursor = MouseCursor::Default;      // World::setMouseCursor
};

} // namespace lkz
