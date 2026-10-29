#include "lkz/Scene/Camera.hpp"

namespace lkz {

Camera::Camera(std::string name, entt::registry& registry)
    : SceneNode{std::move(name), registry} {}

} // namespace lkz
