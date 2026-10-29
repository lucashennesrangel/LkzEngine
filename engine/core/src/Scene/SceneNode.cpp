#include "lkz/Scene/SceneNode.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include "lkz/ECS/Components.hpp"

namespace lkz {

SceneNode::SceneNode(std::string name, entt::registry& registry)
    : Entity{std::move(name)}
    , registry_{registry}
    , handle_{registry.create()} {
    registry_.emplace<SceneLinkComponent>(handle_, this);
    registry_.emplace<PositionComponent>(handle_);
    registry_.emplace<RotationComponent>(handle_);
    registry_.emplace<ScaleComponent>(handle_);

    spdlog::info("SceneNode '{}' criado (id={}, entt={})", this->name(), id(), entt::to_integral(handle_));
}

SceneNode::~SceneNode() {
    if (registry_.valid(handle_)) {
        registry_.destroy(handle_);
    }
}

void SceneNode::setPosition(const glm::vec3& value) {
    registry_.patch<PositionComponent>(handle_, [&value](PositionComponent& p) { p.value = value; });
}

void SceneNode::setRotation(const glm::quat& value) {
    registry_.patch<RotationComponent>(handle_, [&value](RotationComponent& r) { r.value = value; });
}

void SceneNode::setScale(const glm::vec3& value) {
    registry_.patch<ScaleComponent>(handle_, [&value](ScaleComponent& s) { s.value = value; });
}

const glm::vec3& SceneNode::position() const {
    return registry_.get<PositionComponent>(handle_).value;
}

const glm::quat& SceneNode::rotation() const {
    return registry_.get<RotationComponent>(handle_).value;
}

const glm::vec3& SceneNode::scale() const {
    return registry_.get<ScaleComponent>(handle_).value;
}

void SceneNode::syncFromEcs() {
    const auto* translationData = registry_.try_get<PositionComponent>(handle_);
    if (translationData == nullptr) {
        spdlog::error("SceneNode '{}': observer disparou mas a entidade não tem PositionComponent", name());
        return;
    }

    const auto* rotationData = registry_.try_get<RotationComponent>(handle_);
    const auto* scaleData = registry_.try_get<ScaleComponent>(handle_);

    glm::mat4 local = glm::translate(glm::mat4{1.0f}, translationData->value);
    if (rotationData != nullptr) {
        local *= glm::mat4_cast(rotationData->value);
    }
    if (scaleData != nullptr) {
        local = glm::scale(local, scaleData->value);
    }
    setLocalMatrix(local);
}

} // namespace lkz
