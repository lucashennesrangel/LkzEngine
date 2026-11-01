#include "lkz/Systems/RenderSystem.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>
#include <tracy/Tracy.hpp>

#include "lkz/ECS/Components.hpp"
#include "lkz/Scene/Camera.hpp"

namespace lkz {

namespace {

auto renderGroup(entt::registry& registry) {
    // Owning de RenderComponent; Position é só lida (o dono dela é o grupo da física).
    // Nós do Scene Graph ficam de fora: eles entram pela worldMatrix já resolvida.
    return registry.group<RenderComponent>(entt::get<const PositionComponent>, entt::exclude<SceneLinkComponent>);
}

/// Interpola duas matrizes de mundo: glm::mix na translação e na escala, slerp na
/// rotação. Misturar as 16 componentes direto deformaria o objeto ao girar.
glm::mat4 interpolateWorld(const glm::mat4& previous, const glm::mat4& current, float alpha) {
    if (previous == current) {
        return current;
    }

    const glm::vec3 previousScale{glm::length(glm::vec3{previous[0]}), glm::length(glm::vec3{previous[1]}),
                                  glm::length(glm::vec3{previous[2]})};
    const glm::vec3 currentScale{glm::length(glm::vec3{current[0]}), glm::length(glm::vec3{current[1]}),
                                 glm::length(glm::vec3{current[2]})};

    // Escala ~zero ou espelhamento não decompõem bem em TRS: cai no mix por coluna.
    constexpr float epsilon = 1e-6f;
    const glm::mat3 previousBasis{previous};
    const glm::mat3 currentBasis{current};
    if (glm::min(previousScale.x, glm::min(previousScale.y, previousScale.z)) < epsilon ||
        glm::min(currentScale.x, glm::min(currentScale.y, currentScale.z)) < epsilon ||
        glm::determinant(previousBasis) < 0.0f || glm::determinant(currentBasis) < 0.0f) {
        glm::mat4 mixed{1.0f};
        for (int column = 0; column < 4; ++column) {
            mixed[column] = glm::mix(previous[column], current[column], alpha);
        }
        return mixed;
    }

    const glm::quat previousRotation = glm::quat_cast(glm::mat3{
        previousBasis[0] / previousScale.x, previousBasis[1] / previousScale.y, previousBasis[2] / previousScale.z});
    const glm::quat currentRotation = glm::quat_cast(glm::mat3{
        currentBasis[0] / currentScale.x, currentBasis[1] / currentScale.y, currentBasis[2] / currentScale.z});

    const glm::quat rotation = glm::slerp(previousRotation, currentRotation, alpha);
    const glm::vec3 scale = glm::mix(previousScale, currentScale, alpha);
    const glm::vec3 translation = glm::mix(glm::vec3{previous[3]}, glm::vec3{current[3]}, alpha);

    glm::mat4 result = glm::mat4_cast(rotation);
    result[0] *= scale.x;
    result[1] *= scale.y;
    result[2] *= scale.z;
    result[3] = glm::vec4{translation, 1.0f};
    return result;
}

} // namespace

RenderSystem::RenderSystem(entt::registry& registry)
    : registry_{registry} {
    static_cast<void>(renderGroup(registry_));
}

void RenderSystem::pack(RenderFrame& frame, const Camera* camera, float alpha, std::uint64_t tick) {
    ZoneScoped;

    frame.tick = tick;
    frame.alpha = alpha;

    frame.camera.active = camera != nullptr;
    if (camera != nullptr) {
        frame.camera.previousWorld = camera->previousWorldMatrix();
        frame.camera.currentWorld = camera->worldMatrix();
        frame.camera.fovY = camera->projection.fovY;
        frame.camera.nearPlane = camera->projection.nearPlane;
        frame.camera.farPlane = camera->projection.farPlane;
    }

    // clear() mantém a capacidade: depois dos primeiros frames não há mais alocação.
    frame.nodes.clear();
    registry_.view<const RenderComponent, const SceneLinkComponent>().each(
        [&frame](const RenderComponent& drawable, const SceneLinkComponent& link) {
            frame.nodes.push_back({link.node->previousWorldMatrix(), link.node->worldMatrix(), drawable});
        });

    frame.instances.clear();
    renderGroup(registry_).each([&frame](const RenderComponent& drawable, const PositionComponent& position) {
        frame.instances.push_back({position.previous, position.value, drawable});
    });
}

void RenderSystem::render(const RenderFrame& frame, RenderBackend& backend) {
    ZoneScoped;

    CameraView view{};
    if (frame.camera.active) {
        const glm::mat4 cameraWorld = interpolateWorld(frame.camera.previousWorld, frame.camera.currentWorld, frame.alpha);
        view.view = glm::inverse(cameraWorld);
        view.position = glm::vec3{cameraWorld[3]};
        view.fovY = frame.camera.fovY;
        view.nearPlane = frame.camera.nearPlane;
        view.farPlane = frame.camera.farPlane;
    }

    commands_.clear();
    for (const auto& node : frame.nodes) {
        commands_.push_back({interpolateWorld(node.previousWorld, node.currentWorld, frame.alpha), node.render});
    }
    for (const auto& instance : frame.instances) {
        const glm::vec3 position = glm::mix(instance.previous, instance.current, frame.alpha);
        commands_.push_back({glm::translate(glm::mat4{1.0f}, position), instance.render});
    }

    backend.beginFrame(view);
    backend.submit(commands_);
    if (!frame.overlay.empty()) {
        backend.drawOverlay(frame.overlay);
    }
    if (!frame.ui.empty()) {
        backend.updateUi(frame.ui);
    }
    backend.setMouseCursor(frame.cursor);
    backend.endFrame();
}

} // namespace lkz
