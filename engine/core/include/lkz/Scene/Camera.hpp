#pragma once

#include <string>

#include "lkz/Scene/SceneNode.hpp"

namespace lkz {

struct CameraProjection {
    float fovY = 1.0471976f; // 60 graus, em radianos
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
};

/// Câmera é um nó comum: a worldMatrix dela é a pose da câmera (view = inverse(world)).
/// A projeção fica a cargo do backend, que conhece o aspect ratio e a convenção de
/// profundidade da API gráfica.
class Camera : public SceneNode {
public:
    Camera(std::string name, entt::registry& registry);

    CameraProjection projection{};
};

} // namespace lkz
