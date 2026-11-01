#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "lkz/Render/MeshData.hpp"
#include "lkz/Terrain/Heightmap.hpp"

namespace lkz {

/// Identificador de um corpo na física (o mesmo número enquanto o corpo existir).
struct BodyId {
    std::uint32_t value = 0xffffffffu;
    [[nodiscard]] bool valid() const { return value != 0xffffffffu; }
    friend bool operator==(BodyId, BodyId) = default;
};

enum class BodyMotion : std::uint8_t {
    Static,    // não se move (chão, prédios)
    Kinematic, // movido pelo jogo, empurra os outros
    Dynamic,   // movido pela física
};

struct BoxShape {
    glm::vec3 halfExtents{0.5f};
};
struct SphereShape {
    float radius = 0.5f;
};
struct CapsuleShape { // em pé (eixo Y)
    float halfHeight = 0.5f; // da parte reta
    float radius = 0.3f;
};
using BodyShape = std::variant<BoxShape, SphereShape, CapsuleShape>;

struct BodyDesc {
    BodyShape shape = BoxShape{};
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    BodyMotion motion = BodyMotion::Dynamic;
    float mass = 0.0f; // kg; 0 = pela densidade (1000 kg/m³)
    float friction = 0.5f;
    float restitution = 0.0f;
};

struct BodyState {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 linearVelocity{0.0f};
    glm::vec3 angularVelocity{0.0f};
    bool active = false; // false = parado (dormindo)
};

struct RayHit {
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
    BodyId body;
};

/// Carro de quatro rodas (eixo dianteiro esterçante). Medidas em metros, frente em +Z do
/// corpo, base das rodas apoiada em y = 0 do corpo quando a suspensão está no meio.
struct VehicleDesc {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 halfExtents{0.9f, 0.45f, 2.2f}; // caixa da carroceria
    float mass = 1300.0f;
    float wheelRadius = 0.33f;
    float wheelWidth = 0.22f;
    float wheelInset = 0.45f;         // distância das rodas às pontas da carroceria (Z)
    float suspensionMinLength = 0.25f;
    float suspensionMaxLength = 0.45f;
    float suspensionFrequency = 1.5f; // Hz (mais = mais dura)
    float maxSteerAngle = 0.6f;       // radianos
    float engineTorque = 500.0f;      // N·m
    float maxRpm = 6000.0f;
    bool allWheelDrive = false;       // false = tração dianteira
};

/// Comandos do motorista, de -1 a 1 (freios de 0 a 1).
struct VehicleInput {
    float forward = 0.0f;
    float right = 0.0f;
    float brake = 0.0f;
    float handBrake = 0.0f;
};

struct VehicleState {
    BodyState body;
    std::array<glm::mat4, 4> wheels{}; // transformação de cada roda no mundo (eixo X = eixo da roda)
    float speed = 0.0f;                // m/s ao longo da frente
    float rpm = 0.0f;
    int gear = 0;
};

/// Pessoa (cápsula em pé) que anda, sobe degraus e escorrega em rampas íngremes.
struct CharacterDesc {
    glm::vec3 position{0.0f}; // pés
    float height = 1.8f;
    float radius = 0.3f;
    float maxSlope = 0.87f;   // radianos (50°)
    float mass = 80.0f;
};

struct CharacterState {
    glm::vec3 position{0.0f}; // pés
    glm::vec3 velocity{0.0f};
    bool onGround = false;
};

struct PhysicsSettings {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    std::uint32_t maxBodies = 65536;
    int workerThreads = -1; // -1 = núcleos - 1
};

/// Componente do ECS: liga uma entidade (com SceneLinkComponent) a um corpo; o
/// PhysicsWorld::syncToScene copia posição e rotação do corpo para o nó.
struct PhysicsBodyComponent {
    BodyId body;
};

/// Mundo físico da engine, sobre o Jolt Physics (MIT). Rode step() no passo fixo da
/// lógica (World::onFixedUpdate). Tudo na thread lógica.
class PhysicsWorld {
public:
    explicit PhysicsWorld(PhysicsSettings settings = {});
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // --- Corpos ---
    BodyId addBody(const BodyDesc& desc);
    /// Malha estática (ruas, calçadas, prédios): triângulos como estão, no mundo.
    BodyId addStaticMesh(const MeshData& mesh);
    /// Relevo estático. O heightfield do Jolt é quadrado; a grade vira blocos de
    /// `tileSamples` x `tileSamples` (um corpo por bloco, vizinhos dividem a borda).
    std::vector<BodyId> addHeightfield(const Heightmap& heightmap, int tileSamples = 256);
    void removeBody(BodyId body);
    [[nodiscard]] BodyState bodyState(BodyId body) const;
    void setBodyTransform(BodyId body, glm::vec3 position, glm::quat rotation);
    void setLinearVelocity(BodyId body, glm::vec3 velocity);
    void addImpulse(BodyId body, glm::vec3 impulse);
    [[nodiscard]] std::size_t bodyCount() const;

    // --- Consultas ---
    [[nodiscard]] std::optional<RayHit> raycast(glm::vec3 origin, glm::vec3 direction, float maxDistance) const;

    // --- Veículos ---
    std::uint32_t addVehicle(const VehicleDesc& desc);
    void setVehicleInput(std::uint32_t vehicle, const VehicleInput& input);
    [[nodiscard]] VehicleState vehicleState(std::uint32_t vehicle) const;
    [[nodiscard]] BodyId vehicleBody(std::uint32_t vehicle) const;

    // --- Personagens ---
    std::uint32_t addCharacter(const CharacterDesc& desc);
    /// Velocidade desejada no plano (m/s) e pulo; aplicado no próximo step().
    void moveCharacter(std::uint32_t character, glm::vec3 horizontalVelocity, bool jump);
    [[nodiscard]] CharacterState characterState(std::uint32_t character) const;

    /// Avança a simulação `dt` segundos.
    void step(float dt);

    /// Copia posição e rotação dos corpos para os nós das entidades com PhysicsBodyComponent.
    void syncToScene(entt::registry& registry) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lkz
