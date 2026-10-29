#include "lkz/Scene/Entity.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

namespace lkz {

namespace {

std::uint64_t nextEntityId() noexcept {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

Entity::Entity(std::string name)
    : id_{nextEntityId()}
    , name_{std::move(name)} {}

Entity::~Entity() = default;

Entity& Entity::addChild(std::unique_ptr<Entity> child) {
    if (child == nullptr) {
        spdlog::error("Entity '{}': addChild recebeu nullptr", name_);
        return *this;
    }

    child->parent_ = this;
    child->markDirty();
    children_.push_back(std::move(child));
    return *children_.back();
}

std::unique_ptr<Entity> Entity::detachChild(Entity& child) {
    const auto it = std::ranges::find_if(children_, [&child](const auto& c) { return c.get() == &child; });
    if (it == children_.end()) {
        return nullptr;
    }

    std::unique_ptr<Entity> detached = std::move(*it);
    children_.erase(it);
    detached->parent_ = nullptr;
    detached->markDirty();
    return detached;
}

void Entity::setLocalMatrix(const glm::mat4& local) noexcept {
    localMatrix_ = local;
    isDirty_ = true;
}

void Entity::update(float dt) {
    ZoneScoped;
    onUpdate(dt);

    // Por índice: onUpdate() de um filho pode adicionar irmãos e realocar o vetor.
    for (std::size_t i = 0; i < children_.size(); ++i) {
        children_[i]->update(dt);
    }
}

void Entity::updateWorldTransform(bool parentChanged) {
    ZoneScoped;
    const bool changed = isDirty_ || parentChanged;

    // Copiado sempre, mesmo sem mudança: senão um nó que parou ficaria interpolando
    // a partir de um snapshot antigo.
    previousWorld_ = worldMatrix_;

    if (changed) {
        worldMatrix_ = parent_ != nullptr ? parent_->worldMatrix_ * localMatrix_ : localMatrix_;
        isDirty_ = false;
    }

    if (!hasHistory_) {
        previousWorld_ = worldMatrix_;
        hasHistory_ = true;
    }

    for (const auto& child : children_) {
        child->updateWorldTransform(changed);
    }
}

} // namespace lkz
