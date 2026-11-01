#include "OrbitCamera.hpp"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

namespace painter {

OrbitCamera::OrbitCamera(lkz::Camera& camera) : camera_{camera} {
    apply();
}

void OrbitCamera::frame(const glm::vec3& min, const glm::vec3& max) {
    target_ = (min + max) * 0.5f;
    radius_ = std::max(glm::length(max - min) * 0.5f, 0.01f);
    distance_ = radius_ / std::sin(camera_.projection.fovY * 0.5f) * 1.05f;
    yaw_ = 0.7f;
    pitch_ = 0.4f;
    apply();
}

void OrbitCamera::update(const lkz::InputState& input) {
    const bool shift = input.down(lkz::Key::LeftShift) || input.down(lkz::Key::RightShift);
    const bool control = input.down(lkz::Key::LeftControl) || input.down(lkz::Key::RightControl);
    const bool pan = input.down(lkz::MouseButton::Middle) || (shift && input.down(lkz::MouseButton::Right));
    if (pan && input.viewport.y > 0.0f) {
        const float metersPerPixel = 1.0f / pixelsPerMeter(distance_, input.viewport);
        const glm::quat r = rotation();
        target_ += (r * glm::vec3{-input.mouseDelta.x, input.mouseDelta.y, 0.0f}) * metersPerPixel;
    } else if (input.down(lkz::MouseButton::Right)) {
        yaw_ -= input.mouseDelta.x * 0.006f;
        pitch_ = std::clamp(pitch_ + input.mouseDelta.y * 0.006f, -1.55f, 1.55f);
    }
    if (input.wheel != 0.0f && !control) {
        distance_ = std::clamp(distance_ * std::pow(0.87f, input.wheel), radius_ * 0.02f, radius_ * 30.0f);
    }
    apply();
}

glm::quat OrbitCamera::rotation() const {
    return glm::angleAxis(yaw_, glm::vec3{0.0f, 1.0f, 0.0f}) * glm::angleAxis(-pitch_, glm::vec3{1.0f, 0.0f, 0.0f});
}

glm::vec3 OrbitCamera::eye() const {
    return target_ + rotation() * glm::vec3{0.0f, 0.0f, distance_};
}

glm::vec3 OrbitCamera::rayDirection(const glm::vec2& pixel, const glm::vec2& viewport) const {
    // No espaço da câmera o plano da imagem fica em z = -1.
    const float tanHalf = std::tan(camera_.projection.fovY * 0.5f);
    const float aspect = viewport.x / std::max(viewport.y, 1.0f);
    const glm::vec2 ndc{2.0f * pixel.x / std::max(viewport.x, 1.0f) - 1.0f, 1.0f - 2.0f * pixel.y / std::max(viewport.y, 1.0f)};
    return rotation() * glm::normalize(glm::vec3{ndc.x * tanHalf * aspect, ndc.y * tanHalf, -1.0f});
}

float OrbitCamera::pixelsPerMeter(float distance, const glm::vec2& viewport) const {
    return viewport.y / (2.0f * std::max(distance, 1e-4f) * std::tan(camera_.projection.fovY * 0.5f));
}

void OrbitCamera::apply() {
    camera_.setPosition(eye());
    camera_.setRotation(rotation());
    // Planos de corte pelo tamanho do que está na tela: perto o bastante para chegar
    // junto da superfície, sem perder precisão de profundidade.
    camera_.projection.nearPlane = std::max(distance_ * 0.01f, 0.001f);
    camera_.projection.farPlane = distance_ + radius_ * 40.0f + 10.0f;
}

} // namespace painter
