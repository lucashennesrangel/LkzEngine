#pragma once

#include <string>

#include <entt/entity/registry.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "lkz/Scene/Entity.hpp"

namespace lkz {

/// Ponte híbrida: um nó do Scene Graph que é dono de uma entidade do ECS.
///
/// A entidade nasce com Position/Rotation/Scale e um SceneLinkComponent apontando
/// para este nó. A localMatrix só é reconstruída quando o observer da cena (reactive
/// storage do EnTT) registra mudança nesses componentes — nunca por polling.
///
/// Para dar comportamento a um nó, herde e sobrescreva onUpdate().
class SceneNode : public Entity {
public:
    SceneNode(std::string name, entt::registry& registry);
    ~SceneNode() override;

    [[nodiscard]] entt::entity handle() const noexcept { return handle_; }
    [[nodiscard]] entt::registry& registry() const noexcept { return registry_; }

    // Setters passam por registry.patch() e por isso disparam o observer.
    void setPosition(const glm::vec3& value);
    void setRotation(const glm::quat& value);
    void setScale(const glm::vec3& value);

    [[nodiscard]] const glm::vec3& position() const;
    [[nodiscard]] const glm::quat& rotation() const;
    [[nodiscard]] const glm::vec3& scale() const;

    /// Reconstrói a localMatrix (T * R * S) a partir dos componentes e marca o nó sujo.
    /// Chamado pelo World apenas para as entidades que o observer marcou.
    void syncFromEcs();

protected:
    void onUpdate(float /*dt*/) override {}

private:
    entt::registry& registry_;
    entt::entity handle_;
};

} // namespace lkz
