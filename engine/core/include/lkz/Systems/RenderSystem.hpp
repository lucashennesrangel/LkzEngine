#pragma once

#include <cstdint>
#include <vector>

#include <entt/entity/registry.hpp>

#include "lkz/Render/RenderBackend.hpp"
#include "lkz/Render/RenderFrame.hpp"

namespace lkz {

class Camera;

/// Divide o trabalho de render entre as duas threads:
/// - pack():   thread lógica. Copia os snapshots anterior/atual para um RenderFrame.
/// - render(): thread de render. Interpola com alpha e entrega DrawCommands ao backend.
///
/// pack() só usa o registry; render() só usa o frame recebido e o buffer interno.
class RenderSystem {
public:
    explicit RenderSystem(entt::registry& registry);

    void pack(RenderFrame& frame, const Camera* camera, float alpha, std::uint64_t tick);
    void render(const RenderFrame& frame, RenderBackend& backend);

private:
    entt::registry& registry_;
    std::vector<DrawCommand> commands_; // reaproveitado entre frames (thread de render)
};

} // namespace lkz
