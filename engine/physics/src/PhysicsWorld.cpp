#include "lkz/Physics/PhysicsWorld.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <thread>

// Jolt: sempre depois de Jolt.h.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/RegisterTypes.h>
#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include "lkz/ECS/Components.hpp"
#include "lkz/Scene/SceneNode.hpp"

namespace lkz {

namespace {

// Camadas: o que não se move só colide com o que se move.
namespace Layers {
constexpr JPH::ObjectLayer still = 0;
constexpr JPH::ObjectLayer moving = 1;
} // namespace Layers

namespace BroadLayers {
constexpr JPH::BroadPhaseLayer still{0};
constexpr JPH::BroadPhaseLayer moving{1};
constexpr JPH::uint count = 2;
} // namespace BroadLayers

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override { return BroadLayers::count; }
    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == Layers::still ? BroadLayers::still : BroadLayers::moving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    [[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BroadLayers::still ? "parado" : "movendo";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override {
        return layer == Layers::moving || broad == BroadLayers::moving;
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == Layers::moving || b == Layers::moving;
    }
};

JPH::Vec3 toJolt(glm::vec3 v) {
    return {v.x, v.y, v.z};
}
JPH::Quat toJolt(glm::quat q) {
    return {q.x, q.y, q.z, q.w};
}
glm::vec3 toGlm(JPH::Vec3 v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}
glm::quat toGlm(JPH::Quat q) {
    return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()};
}
glm::mat4 toGlm(const JPH::RMat44& m) {
    glm::mat4 out{1.0f};
    for (int column = 0; column < 4; ++column) {
        const JPH::Vec4 c = column < 3 ? JPH::Vec4(m.GetColumn3(static_cast<JPH::uint>(column)), 0.0f)
                                       : JPH::Vec4(JPH::Vec3(m.GetTranslation()), 1.0f);
        out[column] = {c.GetX(), c.GetY(), c.GetZ(), c.GetW()};
    }
    return out;
}

JPH::BodyID toJolt(BodyId id) {
    return JPH::BodyID{id.value};
}
BodyId toLkz(JPH::BodyID id) {
    return id.IsInvalid() ? BodyId{} : BodyId{id.GetIndexAndSequenceNumber()};
}

void trace(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    spdlog::debug("Jolt: {}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool assertFailed(const char* expression, const char* message, const char* file, JPH::uint line) {
    spdlog::error("Jolt: {} ({}) em {}:{}", expression, message != nullptr ? message : "", file, line);
    return true; // para no depurador
}
#endif

/// Inicialização global do Jolt (fábrica de tipos), contada por mundo.
int joltUsers = 0;

void acquireJolt() {
    if (joltUsers++ == 0) {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = trace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = assertFailed;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
}

void releaseJolt() {
    if (--joltUsers == 0) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

struct Vehicle {
    JPH::Body* body = nullptr;
    JPH::Ref<JPH::VehicleConstraint> constraint;
};

struct Character {
    JPH::Ref<JPH::CharacterVirtual> character;
    glm::vec3 desiredVelocity{0.0f};
    bool jump = false;
};

} // namespace

struct PhysicsWorld::Impl {
    explicit Impl(const PhysicsSettings& s)
        : settings{s} {
        acquireJolt();
        temp = std::make_unique<JPH::TempAllocatorImpl>(64 * 1024 * 1024);
        const int threads = s.workerThreads >= 0 ? s.workerThreads
                                                 : std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
        jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);
        system = std::make_unique<JPH::PhysicsSystem>();
        system->Init(s.maxBodies, 0, s.maxBodies, s.maxBodies / 4, broadPhaseLayers, objectVsBroadPhase, objectPairs);
        system->SetGravity(toJolt(s.gravity));
    }

    ~Impl() {
        characters.clear();
        for (auto& vehicle : vehicles) {
            system->RemoveStepListener(vehicle.constraint);
            system->RemoveConstraint(vehicle.constraint);
        }
        vehicles.clear();
        system.reset();
        jobs.reset();
        temp.reset();
        releaseJolt();
    }

    JPH::BodyInterface& bodies() { return system->GetBodyInterface(); }

    BodyId addBody(const JPH::Shape* shape, glm::vec3 position, glm::quat rotation, BodyMotion motion, float mass,
                   float friction, float restitution) {
        const JPH::EMotionType type = motion == BodyMotion::Static
                                          ? JPH::EMotionType::Static
                                          : (motion == BodyMotion::Kinematic ? JPH::EMotionType::Kinematic
                                                                             : JPH::EMotionType::Dynamic);
        JPH::BodyCreationSettings creation{shape, toJolt(position), toJolt(rotation), type,
                                           motion == BodyMotion::Static ? Layers::still : Layers::moving};
        creation.mFriction = friction;
        creation.mRestitution = restitution;
        if (mass > 0.0f && motion == BodyMotion::Dynamic) {
            creation.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            creation.mMassPropertiesOverride.mMass = mass;
        }
        const JPH::BodyID id = bodies().CreateAndAddBody(
            creation, motion == BodyMotion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        if (id.IsInvalid()) {
            spdlog::error("Física: sem espaço para mais corpos (PhysicsSettings::maxBodies)");
        }
        return toLkz(id);
    }

    PhysicsSettings settings;
    BroadPhaseLayers broadPhaseLayers;
    ObjectVsBroadPhase objectVsBroadPhase;
    ObjectPairs objectPairs;
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    std::unique_ptr<JPH::PhysicsSystem> system;
    std::vector<Vehicle> vehicles;
    std::vector<Character> characters;
};

PhysicsWorld::PhysicsWorld(PhysicsSettings settings)
    : impl_{std::make_unique<Impl>(settings)} {}

PhysicsWorld::~PhysicsWorld() = default;

BodyId PhysicsWorld::addBody(const BodyDesc& desc) {
    JPH::RefConst<JPH::Shape> shape;
    if (const auto* box = std::get_if<BoxShape>(&desc.shape)) {
        shape = new JPH::BoxShape(toJolt(glm::max(box->halfExtents, glm::vec3{0.06f})));
    } else if (const auto* sphere = std::get_if<SphereShape>(&desc.shape)) {
        shape = new JPH::SphereShape(std::max(sphere->radius, 0.01f));
    } else if (const auto* capsule = std::get_if<CapsuleShape>(&desc.shape)) {
        shape = new JPH::CapsuleShape(std::max(capsule->halfHeight, 0.01f), std::max(capsule->radius, 0.01f));
    }
    return impl_->addBody(shape, desc.position, desc.rotation, desc.motion, desc.mass, desc.friction, desc.restitution);
}

BodyId PhysicsWorld::addStaticMesh(const MeshData& mesh) {
    JPH::VertexList vertices;
    vertices.reserve(mesh.positions.size());
    for (const auto& p : mesh.positions) {
        vertices.push_back(JPH::Float3{p.x, p.y, p.z});
    }
    JPH::IndexedTriangleList triangles;
    const std::size_t count = mesh.indices.empty() ? mesh.positions.size() : mesh.indices.size();
    const auto corner = [&mesh](std::size_t i) { return mesh.indices.empty() ? static_cast<JPH::uint32>(i) : mesh.indices[i]; };
    triangles.reserve(count / 3);
    for (std::size_t i = 0; i + 2 < count; i += 3) {
        triangles.push_back(JPH::IndexedTriangle{corner(i), corner(i + 1), corner(i + 2)});
    }
    if (triangles.empty()) {
        return {};
    }
    const JPH::MeshShapeSettings settings{std::move(vertices), std::move(triangles)};
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        spdlog::error("Física: malha estática inválida: {}", result.GetError().c_str());
        return {};
    }
    return impl_->addBody(result.Get(), glm::vec3{0.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, BodyMotion::Static, 0.0f,
                          0.6f, 0.0f);
}

std::vector<BodyId> PhysicsWorld::addHeightfield(const Heightmap& heightmap, int tileSamples) {
    std::vector<BodyId> ids;
    if (heightmap.empty()) {
        return ids;
    }
    // Lado do bloco: potência de 2 (o Jolt divide em blocos de mBlockSize).
    int samples = 8;
    while (samples < tileSamples) {
        samples *= 2;
    }
    const int cells = samples - 1; // vizinhos dividem a última coluna/linha
    const int tilesX = (heightmap.columns() - 1 + cells - 1) / cells;
    const int tilesZ = (heightmap.rows() - 1 + cells - 1) / cells;
    std::vector<float> tile(static_cast<std::size_t>(samples) * static_cast<std::size_t>(samples));
    for (int tz = 0; tz < tilesZ; ++tz) {
        for (int tx = 0; tx < tilesX; ++tx) {
            for (int z = 0; z < samples; ++z) {
                for (int x = 0; x < samples; ++x) {
                    const int column = tx * cells + x;
                    const int row = tz * cells + z;
                    const bool inside = column < heightmap.columns() && row < heightmap.rows();
                    tile[static_cast<std::size_t>(z) * static_cast<std::size_t>(samples) + static_cast<std::size_t>(x)] =
                        inside ? heightmap.at(column, row) : JPH::HeightFieldShapeConstants::cNoCollisionValue;
                }
            }
            const glm::vec2 corner = heightmap.origin() + glm::vec2{static_cast<float>(tx * cells), static_cast<float>(tz * cells)} *
                                                              heightmap.spacing();
            JPH::HeightFieldShapeSettings settings{tile.data(), JPH::Vec3{corner.x, 0.0f, corner.y},
                                                   JPH::Vec3{heightmap.spacing(), 1.0f, heightmap.spacing()},
                                                   static_cast<JPH::uint32>(samples)};
            settings.mBlockSize = 4;
            const JPH::ShapeSettings::ShapeResult result = settings.Create();
            if (result.HasError()) {
                spdlog::error("Física: bloco de relevo ({}, {}) inválido: {}", tx, tz, result.GetError().c_str());
                continue;
            }
            ids.push_back(impl_->addBody(result.Get(), glm::vec3{0.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                                         BodyMotion::Static, 0.0f, 0.8f, 0.0f));
        }
    }
    spdlog::info("Física: relevo {}x{} em {} blocos de {} amostras", heightmap.columns(), heightmap.rows(), ids.size(),
                 samples);
    return ids;
}

void PhysicsWorld::removeBody(BodyId body) {
    if (body.valid()) {
        impl_->bodies().RemoveBody(toJolt(body));
        impl_->bodies().DestroyBody(toJolt(body));
    }
}

BodyState PhysicsWorld::bodyState(BodyId body) const {
    BodyState state;
    if (!body.valid()) {
        return state;
    }
    const JPH::BodyLockRead lock{impl_->system->GetBodyLockInterface(), toJolt(body)};
    if (lock.Succeeded()) {
        const JPH::Body& b = lock.GetBody();
        state.position = toGlm(JPH::Vec3(b.GetPosition()));
        state.rotation = toGlm(b.GetRotation());
        state.linearVelocity = toGlm(b.GetLinearVelocity());
        state.angularVelocity = toGlm(b.GetAngularVelocity());
        state.active = b.IsActive();
    }
    return state;
}

void PhysicsWorld::setBodyTransform(BodyId body, glm::vec3 position, glm::quat rotation) {
    impl_->bodies().SetPositionAndRotation(toJolt(body), toJolt(position), toJolt(rotation), JPH::EActivation::Activate);
}

void PhysicsWorld::setLinearVelocity(BodyId body, glm::vec3 velocity) {
    impl_->bodies().SetLinearVelocity(toJolt(body), toJolt(velocity));
}

void PhysicsWorld::addImpulse(BodyId body, glm::vec3 impulse) {
    impl_->bodies().AddImpulse(toJolt(body), toJolt(impulse));
}

std::size_t PhysicsWorld::bodyCount() const {
    return impl_->system->GetNumBodies();
}

std::optional<RayHit> PhysicsWorld::raycast(glm::vec3 origin, glm::vec3 direction, float maxDistance) const {
    if (glm::length(direction) < 1e-6f) {
        return std::nullopt;
    }
    const JPH::RRayCast ray{toJolt(origin), toJolt(glm::normalize(direction) * maxDistance)};
    JPH::RayCastResult result;
    if (!impl_->system->GetNarrowPhaseQuery().CastRay(ray, result)) {
        return std::nullopt;
    }
    RayHit hit;
    const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
    hit.point = toGlm(JPH::Vec3(point));
    hit.distance = result.mFraction * maxDistance;
    hit.body = toLkz(result.mBodyID);
    const JPH::BodyLockRead lock{impl_->system->GetBodyLockInterface(), result.mBodyID};
    if (lock.Succeeded()) {
        hit.normal = toGlm(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
    }
    return hit;
}

std::uint32_t PhysicsWorld::addVehicle(const VehicleDesc& desc) {
    auto& state = *impl_;
    // Carroceria com o centro de massa baixo (fica difícil de capotar).
    const JPH::RefConst<JPH::Shape> shape =
        JPH::OffsetCenterOfMassShapeSettings(JPH::Vec3{0.0f, -desc.halfExtents.y, 0.0f},
                                             new JPH::BoxShape(toJolt(desc.halfExtents)))
            .Create()
            .Get();
    JPH::BodyCreationSettings creation{shape, toJolt(desc.position), toJolt(desc.rotation), JPH::EMotionType::Dynamic,
                                       Layers::moving};
    creation.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    creation.mMassPropertiesOverride.mMass = desc.mass;
    JPH::Body* body = state.bodies().CreateBody(creation);
    state.bodies().AddBody(body->GetID(), JPH::EActivation::Activate);

    JPH::VehicleConstraintSettings vehicle;
    vehicle.mMaxPitchRollAngle = JPH::DegreesToRadians(60.0f);
    const float x = desc.halfExtents.x - desc.wheelWidth * 0.5f;
    const float y = -desc.halfExtents.y;
    const float z = desc.halfExtents.z - desc.wheelInset;
    // Rodas: 0 e 1 na frente (+Z), 2 e 3 atrás; esquerda em +X, como nos exemplos do Jolt.
    const std::array<JPH::Vec3, 4> positions{JPH::Vec3{x, y, z}, JPH::Vec3{-x, y, z}, JPH::Vec3{x, y, -z},
                                             JPH::Vec3{-x, y, -z}};
    for (int i = 0; i < 4; ++i) {
        auto* wheel = new JPH::WheelSettingsWV;
        wheel->mPosition = positions[static_cast<std::size_t>(i)];
        wheel->mSuspensionDirection = JPH::Vec3{0.0f, -1.0f, 0.0f};
        wheel->mSteeringAxis = JPH::Vec3{0.0f, 1.0f, 0.0f};
        wheel->mWheelUp = JPH::Vec3{0.0f, 1.0f, 0.0f};
        wheel->mWheelForward = JPH::Vec3{0.0f, 0.0f, 1.0f};
        wheel->mSuspensionMinLength = desc.suspensionMinLength;
        wheel->mSuspensionMaxLength = desc.suspensionMaxLength;
        wheel->mSuspensionSpring.mFrequency = desc.suspensionFrequency;
        wheel->mSuspensionSpring.mDamping = 0.5f;
        wheel->mRadius = desc.wheelRadius;
        wheel->mWidth = desc.wheelWidth;
        wheel->mMaxSteerAngle = i < 2 ? desc.maxSteerAngle : 0.0f;
        wheel->mMaxHandBrakeTorque = i < 2 ? 0.0f : 4000.0f; // freio de mão nas rodas de trás
        vehicle.mWheels.push_back(wheel);
    }
    auto* controller = new JPH::WheeledVehicleControllerSettings;
    controller->mEngine.mMaxTorque = desc.engineTorque;
    controller->mEngine.mMaxRPM = desc.maxRpm;
    controller->mDifferentials.resize(desc.allWheelDrive ? 2 : 1);
    controller->mDifferentials[0].mLeftWheel = 0;
    controller->mDifferentials[0].mRightWheel = 1;
    if (desc.allWheelDrive) {
        controller->mDifferentials[1].mLeftWheel = 2;
        controller->mDifferentials[1].mRightWheel = 3;
        controller->mDifferentials[0].mEngineTorqueRatio = 0.5f;
        controller->mDifferentials[1].mEngineTorqueRatio = 0.5f;
    }
    vehicle.mController = controller;
    vehicle.mAntiRollBars.resize(2);
    vehicle.mAntiRollBars[0].mLeftWheel = 0;
    vehicle.mAntiRollBars[0].mRightWheel = 1;
    vehicle.mAntiRollBars[1].mLeftWheel = 2;
    vehicle.mAntiRollBars[1].mRightWheel = 3;

    JPH::Ref<JPH::VehicleConstraint> constraint = new JPH::VehicleConstraint(*body, vehicle);
    constraint->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterCastCylinder(Layers::moving));
    // O mesmo ajuste de impulso dos pneus que os exemplos do Jolt usam (ver VehicleConstraintTest).
    static_cast<JPH::WheeledVehicleController*>(constraint->GetController())
        ->SetTireMaxImpulseCallback([](JPH::uint, float& longitudinal, float& lateral, float suspensionImpulse,
                                       float longitudinalFriction, float lateralFriction, float, float, float) {
            longitudinal = 10.0f * longitudinalFriction * suspensionImpulse;
            lateral = lateralFriction * suspensionImpulse;
        });
    state.system->AddConstraint(constraint);
    state.system->AddStepListener(constraint);
    state.vehicles.push_back({body, constraint});
    return static_cast<std::uint32_t>(state.vehicles.size() - 1);
}

void PhysicsWorld::setVehicleInput(std::uint32_t vehicle, const VehicleInput& input) {
    if (vehicle >= impl_->vehicles.size()) {
        return;
    }
    auto& v = impl_->vehicles[vehicle];
    static_cast<JPH::WheeledVehicleController*>(v.constraint->GetController())
        ->SetDriverInput(input.forward, input.right, input.brake, input.handBrake);
    if (input.forward != 0.0f || input.right != 0.0f || input.brake != 0.0f || input.handBrake != 0.0f) {
        impl_->bodies().ActivateBody(v.body->GetID());
    }
}

VehicleState PhysicsWorld::vehicleState(std::uint32_t vehicle) const {
    VehicleState state;
    if (vehicle >= impl_->vehicles.size()) {
        return state;
    }
    const auto& v = impl_->vehicles[vehicle];
    state.body = bodyState(toLkz(v.body->GetID()));
    for (JPH::uint i = 0; i < 4; ++i) {
        state.wheels[i] = toGlm(v.constraint->GetWheelWorldTransform(i, JPH::Vec3::sAxisY(), JPH::Vec3::sAxisX()));
    }
    const glm::vec3 forward = state.body.rotation * glm::vec3{0.0f, 0.0f, 1.0f};
    state.speed = glm::dot(state.body.linearVelocity, forward);
    const auto* controller = static_cast<const JPH::WheeledVehicleController*>(v.constraint->GetController());
    state.rpm = controller->GetEngine().GetCurrentRPM();
    state.gear = controller->GetTransmission().GetCurrentGear();
    return state;
}

BodyId PhysicsWorld::vehicleBody(std::uint32_t vehicle) const {
    return vehicle < impl_->vehicles.size() ? toLkz(impl_->vehicles[vehicle].body->GetID()) : BodyId{};
}

std::uint32_t PhysicsWorld::addCharacter(const CharacterDesc& desc) {
    const float radius = std::max(desc.radius, 0.05f);
    const float halfCylinder = std::max(desc.height * 0.5f - radius, 0.05f);
    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings;
    // Cápsula com os pés em y = 0 do personagem.
    settings->mShape = JPH::RotatedTranslatedShapeSettings(JPH::Vec3{0.0f, halfCylinder + radius, 0.0f}, JPH::Quat::sIdentity(),
                                                           new JPH::CapsuleShape(halfCylinder, radius))
                           .Create()
                           .Get();
    settings->mMaxSlopeAngle = desc.maxSlope;
    settings->mMass = desc.mass;
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius); // só a parte de baixo apoia
    Character character;
    character.character =
        new JPH::CharacterVirtual(settings, toJolt(desc.position), JPH::Quat::sIdentity(), 0, impl_->system.get());
    impl_->characters.push_back(std::move(character));
    return static_cast<std::uint32_t>(impl_->characters.size() - 1);
}

void PhysicsWorld::moveCharacter(std::uint32_t character, glm::vec3 horizontalVelocity, bool jump) {
    if (character < impl_->characters.size()) {
        auto& c = impl_->characters[character];
        c.desiredVelocity = {horizontalVelocity.x, 0.0f, horizontalVelocity.z};
        c.jump = c.jump || jump;
    }
}

CharacterState PhysicsWorld::characterState(std::uint32_t character) const {
    CharacterState state;
    if (character < impl_->characters.size()) {
        const auto& c = *impl_->characters[character].character;
        state.position = toGlm(JPH::Vec3(c.GetPosition()));
        state.velocity = toGlm(c.GetLinearVelocity());
        state.onGround = c.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    }
    return state;
}

void PhysicsWorld::step(float dt) {
    ZoneScopedN("PhysicsWorld::step");
    auto& state = *impl_;
    const JPH::Vec3 gravity = state.system->GetGravity();
    constexpr float jumpSpeed = 4.5f;
    for (auto& c : state.characters) {
        JPH::CharacterVirtual& character = *c.character;
        character.UpdateGroundVelocity();
        const JPH::Vec3 up = JPH::Vec3::sAxisY();
        const JPH::Vec3 vertical = character.GetLinearVelocity().Dot(up) * up;
        const JPH::Vec3 ground = character.GetGroundVelocity();
        JPH::Vec3 velocity;
        if (character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround &&
            (vertical.GetY() - ground.GetY()) < 0.1f) {
            velocity = ground;
            if (c.jump) {
                velocity += jumpSpeed * up;
            }
        } else {
            velocity = vertical;
        }
        velocity += gravity * dt + toJolt(c.desiredVelocity);
        character.SetLinearVelocity(velocity);
        const JPH::CharacterVirtual::ExtendedUpdateSettings update;
        character.ExtendedUpdate(dt, gravity, update, state.system->GetDefaultBroadPhaseLayerFilter(Layers::moving),
                                 state.system->GetDefaultLayerFilter(Layers::moving), {}, {}, *state.temp);
        c.jump = false;
    }
    // Passos de colisão: um por 1/60 s (mais se o passo for maior).
    const int collisionSteps = std::max(1, static_cast<int>(std::ceil(dt * 60.0f - 1e-3f)));
    state.system->Update(dt, collisionSteps, state.temp.get(), state.jobs.get());
}

void PhysicsWorld::syncToScene(entt::registry& registry) const {
    registry.view<const PhysicsBodyComponent, const SceneLinkComponent>().each(
        [this](const PhysicsBodyComponent& body, const SceneLinkComponent& link) {
            if (link.node == nullptr) {
                return;
            }
            const BodyState state = bodyState(body.body);
            link.node->setPosition(state.position);
            link.node->setRotation(state.rotation);
        });
}

} // namespace lkz
