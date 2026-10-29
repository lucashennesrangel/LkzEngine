#pragma once

#include <concepts>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>

namespace lkz {

/// Nó base do Scene Graph: hierarquia pai/filho e transformações em cascata.
///
/// worldMatrix = parentWorld * localMatrix só é recalculada quando o nó, ou um
/// ancestral, foi alterado (dirty flag). worldMatrix é o snapshot "atual" do passo
/// fixo; previousWorldMatrix é o do passo anterior, usado na interpolação visual.
class Entity {
public:
    explicit Entity(std::string name);
    virtual ~Entity();

    Entity(const Entity&) = delete;
    Entity& operator=(const Entity&) = delete;
    Entity(Entity&&) = delete;
    Entity& operator=(Entity&&) = delete;

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] Entity* parent() const noexcept { return parent_; }
    [[nodiscard]] const std::vector<std::unique_ptr<Entity>>& children() const noexcept { return children_; }

    /// Transfere a posse do filho para este nó.
    Entity& addChild(std::unique_ptr<Entity> child);

    /// Constrói o filho no lugar e devolve uma referência já tipada.
    template <std::derived_from<Entity> T, typename... Args>
    T& emplaceChild(Args&&... args) {
        auto child = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *child;
        addChild(std::move(child));
        return ref;
    }

    /// Remove o filho da hierarquia e devolve a posse (nullptr se não for filho direto).
    std::unique_ptr<Entity> detachChild(Entity& child);

    void setLocalMatrix(const glm::mat4& local) noexcept;
    [[nodiscard]] const glm::mat4& localMatrix() const noexcept { return localMatrix_; }
    [[nodiscard]] const glm::mat4& worldMatrix() const noexcept { return worldMatrix_; }
    [[nodiscard]] const glm::mat4& previousWorldMatrix() const noexcept { return previousWorld_; }

    [[nodiscard]] bool isDirty() const noexcept { return isDirty_; }
    void markDirty() noexcept { isDirty_ = true; }

    /// Lógica de passo fixo: chama onUpdate() neste nó e depois nos descendentes.
    /// Adicionar filhos dentro de onUpdate() é seguro; destruir o próprio nó não é.
    void update(float dt);

    /// Guarda previousWorld e recalcula worldMatrix apenas onde houve mudança.
    void updateWorldTransform(bool parentChanged = false);

protected:
    virtual void onUpdate(float dt) = 0;

private:
    std::uint64_t id_;
    std::string name_;
    Entity* parent_ = nullptr;
    std::vector<std::unique_ptr<Entity>> children_;

    glm::mat4 localMatrix_{1.0f};
    glm::mat4 worldMatrix_{1.0f};
    glm::mat4 previousWorld_{1.0f};
    bool isDirty_ = true;
    bool hasHistory_ = false; // sem passo anterior ainda: previous = world (evita interpolar a partir da origem)
};

} // namespace lkz
