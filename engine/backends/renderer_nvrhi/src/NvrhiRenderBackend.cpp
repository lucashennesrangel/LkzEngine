#include "lkz/backends/NvrhiRenderBackend.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cctype>
#include <cmath>
#include <deque>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <lkz/ui_rmlui/UiLayer.hpp>
#include <nvrhi/nvrhi.h>
#include <nvrhi/utils.h>
#include <spdlog/spdlog.h>

#include "GraphicsDevice.hpp"
#include "Images.hpp"
#include "Models.hpp"
#include "NvrhiUiRenderer.hpp"
#include "Primitives.hpp"
#include "TerrainPass.hpp"
#include "mesh_ps.h"
#include "mesh_vs.h"

namespace lkz {

using namespace nvrhi_detail;

std::optional<GraphicsApi> parseGraphicsApi(std::string_view name) {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "d3d12" || lower == "dx12" || lower == "directx12") {
        return GraphicsApi::D3D12;
    }
    if (lower == "vulkan" || lower == "vk") {
        return GraphicsApi::Vulkan;
    }
    return std::nullopt;
}

const char* graphicsApiName(GraphicsApi api) {
    return api == GraphicsApi::D3D12 ? "DirectX 12" : "Vulkan";
}

namespace {

constexpr std::size_t framesInFlight = 2;
constexpr std::uint32_t defaultMesh = 0xFFFFFFFFu; // cubo unitário dos ids sem entrada

/// Uma cópia no buffer de instâncias: colunas da matriz de modelo, cor e transformação
/// das coordenadas de textura (96 bytes).
struct Instance {
    glm::vec4 model[4];
    glm::vec4 color;
    glm::vec4 uv;
};
static_assert(sizeof(Instance) == 96);

struct FrameConstants {
    glm::mat4 viewProjection;
    glm::vec4 lightDirection;
};

struct MaterialConstants {
    glm::vec4 color;
    glm::vec4 tiling; // x: hex-tiling (0/1), y: rotação, z: contraste
};

struct GpuPart {
    nvrhi::BufferHandle vertices;
    nvrhi::BufferHandle indices;
    std::uint32_t indexCount = 0;
    glm::vec4 color{1.0f};
    nvrhi::BindingSetHandle bindings; // imagem própria (parte de modelo); vazio = a da mesh
};

struct GpuMesh {
    std::vector<GpuPart> parts;
    nvrhi::BindingSetHandle bindings; // constantes do frame, imagem e amostrador
    bool doubleSided = false;
    glm::vec4 tiling{0.0f, 1.0f, 0.5f, 0.0f};
    float radius = 0.0f; // esfera envolvente em volta da origem do objeto
    std::uint32_t runtimeTexture = 0; // PrimitiveMesh::runtimeTexture (0 = não usa)
    std::vector<Instance> instances; // cópias do frame
};

/// Lote de cópias paradas (World::setInstanceBatch): buffer fixo na GPU, em células.
struct GpuBatch {
    struct Cell {
        glm::vec3 low{0.0f};
        glm::vec3 high{0.0f};
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };
    std::uint32_t mesh = 0;
    nvrhi::BufferHandle instances;
    std::vector<Cell> cells; // em ordem no buffer
    std::size_t total = 0;
};

float radiusOf(const Geometry& geometry) {
    float radius = 0.0f;
    for (const Vertex& vertex : geometry.vertices) {
        radius = std::max(radius, glm::length(vertex.position));
    }
    return radius;
}

/// Planos da pirâmide de visão (Gribb e Hartmann), da matriz no formato do OpenGL.
std::array<glm::vec4, 6> frustumPlanes(const glm::mat4& m) {
    const auto row = [&m](int i) { return glm::vec4{m[0][i], m[1][i], m[2][i], m[3][i]}; };
    std::array<glm::vec4, 6> planes{row(3) + row(0), row(3) - row(0), row(3) + row(1),
                                    row(3) - row(1), row(3) + row(2), row(3) - row(2)};
    for (glm::vec4& plane : planes) {
        plane /= glm::length(glm::vec3{plane});
    }
    return planes;
}

bool boxVisible(const std::array<glm::vec4, 6>& planes, glm::vec3 low, glm::vec3 high) {
    for (const glm::vec4& plane : planes) {
        const glm::vec3 corner{plane.x >= 0.0f ? high.x : low.x, plane.y >= 0.0f ? high.y : low.y,
                               plane.z >= 0.0f ? high.z : low.z};
        if (glm::dot(glm::vec3{plane}, corner) + plane.w < 0.0f) {
            return false;
        }
    }
    return true;
}

std::optional<Key> keyFromScancode(SDL_Scancode code) {
    if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) {
        return static_cast<Key>(static_cast<int>(Key::A) + (code - SDL_SCANCODE_A));
    }
    if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_9) {
        return static_cast<Key>(static_cast<int>(Key::Num1) + (code - SDL_SCANCODE_1));
    }
    if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12) {
        return static_cast<Key>(static_cast<int>(Key::F1) + (code - SDL_SCANCODE_F1));
    }
    switch (code) {
    case SDL_SCANCODE_0: return Key::Num0;
    case SDL_SCANCODE_UP: return Key::Up;
    case SDL_SCANCODE_DOWN: return Key::Down;
    case SDL_SCANCODE_LEFT: return Key::Left;
    case SDL_SCANCODE_RIGHT: return Key::Right;
    case SDL_SCANCODE_SPACE: return Key::Space;
    case SDL_SCANCODE_RETURN: return Key::Enter;
    case SDL_SCANCODE_KP_ENTER: return Key::Enter;
    case SDL_SCANCODE_ESCAPE: return Key::Escape;
    case SDL_SCANCODE_TAB: return Key::Tab;
    case SDL_SCANCODE_BACKSPACE: return Key::Backspace;
    case SDL_SCANCODE_DELETE: return Key::Delete;
    case SDL_SCANCODE_INSERT: return Key::Insert;
    case SDL_SCANCODE_HOME: return Key::Home;
    case SDL_SCANCODE_END: return Key::End;
    case SDL_SCANCODE_PAGEUP: return Key::PageUp;
    case SDL_SCANCODE_PAGEDOWN: return Key::PageDown;
    case SDL_SCANCODE_LSHIFT: return Key::LeftShift;
    case SDL_SCANCODE_RSHIFT: return Key::RightShift;
    case SDL_SCANCODE_LCTRL: return Key::LeftControl;
    case SDL_SCANCODE_RCTRL: return Key::RightControl;
    case SDL_SCANCODE_LALT: return Key::LeftAlt;
    case SDL_SCANCODE_RALT: return Key::RightAlt;
    case SDL_SCANCODE_MINUS: return Key::Minus;
    case SDL_SCANCODE_EQUALS: return Key::Equal;
    case SDL_SCANCODE_LEFTBRACKET: return Key::LeftBracket;
    case SDL_SCANCODE_RIGHTBRACKET: return Key::RightBracket;
    case SDL_SCANCODE_COMMA: return Key::Comma;
    case SDL_SCANCODE_PERIOD: return Key::Period;
    case SDL_SCANCODE_SLASH: return Key::Slash;
    case SDL_SCANCODE_KP_PLUS: return Key::KeypadAdd;
    case SDL_SCANCODE_KP_MINUS: return Key::KeypadSubtract;
    default: return std::nullopt;
    }
}

std::optional<MouseButton> buttonFromSdl(Uint8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return MouseButton::Left;
    case SDL_BUTTON_RIGHT: return MouseButton::Right;
    case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
    default: return std::nullopt;
    }
}

} // namespace

struct NvrhiRenderBackend::Impl {
    RenderConfig config;
    GraphicsApi api = GraphicsApi::D3D12;
    bool debug = false;

    SDL_Window* window = nullptr;
    MouseCursor cursorShape = MouseCursor::Default;
    std::array<SDL_Cursor*, 6> cursors{}; // criados na primeira vez, por MouseCursor
    std::unique_ptr<GraphicsDevice> graphics;
    nvrhi::DeviceHandle device;
    nvrhi::CommandListHandle commands;
    nvrhi::ShaderHandle vertexShader;
    nvrhi::ShaderHandle pixelShader;
    nvrhi::InputLayoutHandle inputLayout;
    nvrhi::BindingLayoutHandle bindingLayout;
    std::array<nvrhi::GraphicsPipelineHandle, 2> pipelines; // [doubleSided]
    nvrhi::BufferHandle frameConstants;
    nvrhi::BufferHandle instanceBuffer;
    std::size_t instanceCapacity = 0;
    nvrhi::SamplerHandle sampler;
    nvrhi::SamplerHandle clampSampler; // imagem do terreno: cobre [0,1] uma vez
    TerrainPass terrain;
    std::unique_ptr<NvrhiUiRenderer> uiRenderer;
    std::unique_ptr<ui_rmlui::UiLayer> ui; // World::showUi; criada no primeiro documento
    std::array<bool, 3> uiHeld{};          // botão apertado sobre a interface (não vai ao jogo)
    nvrhi::TextureHandle white;
    nvrhi::TextureHandle depth;
    std::vector<nvrhi::FramebufferHandle> framebuffers;
    std::unordered_map<std::string, nvrhi::TextureHandle> textures; // por arquivo
    std::unordered_map<std::uint32_t, GpuMesh> meshes;
    std::unordered_map<std::uint32_t, GpuBatch> batches;

    /// Textura feita em tempo de execução (World::uploadTexture): a cópia na CPU refaz os
    /// mipmaps no fim de cada traço.
    struct RuntimeTexture {
        nvrhi::TextureHandle texture;
        nvrhi::BindingSetHandle bindings;
        Image image;
    };
    std::unordered_map<std::uint32_t, RuntimeTexture> runtimeTextures;
    std::unordered_set<std::uint32_t> warned; // ids com algo ainda não suportado, já avisados
    std::deque<nvrhi::EventQueryHandle> inFlight;
    std::vector<nvrhi::EventQueryHandle> queryPool;

    int width = 0;
    int height = 0;
    bool closing = false;
    bool frameReady = false;
    glm::mat4 viewProjection{1.0f};
    glm::mat4 cullViewProjection{1.0f}; // mesma câmera, projeção do OpenGL (lkz::TerrainLod)
    glm::vec3 cameraPosition{0.0f};
    bool lodColors = false; // F3: cor por nível de detalhe
    std::size_t drawCalls = 0;
    std::size_t triangles = 0;
    std::size_t objects = 0;
    int fps = 0;
    int framesThisSecond = 0;
    std::uint64_t secondStart = 0;
    std::bitset<InputState::keyCount> keys; // estado atual do teclado (eventos da SDL)

    bool vulkan() const { return device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN; }

    nvrhi::ShaderHandle shader(nvrhi::ShaderType type, const char* entry, const unsigned char* dxil, std::size_t dxilSize,
                               const unsigned char* spirv, std::size_t spirvSize) {
        nvrhi::ShaderDesc desc;
        desc.shaderType = type;
        desc.entryName = entry;
        return vulkan() ? device->createShader(desc, spirv, spirvSize) : device->createShader(desc, dxil, dxilSize);
    }

    /// Executa já uma lista de comandos de envio (malhas e imagens chegam antes do frame).
    template <class Upload>
    void upload(Upload&& work) {
        commands->open();
        work(commands.Get());
        commands->close();
        device->executeCommandList(commands);
    }

    nvrhi::TextureHandle createTexture(const Image& image, const char* name) {
        std::vector<Image> chain = mipChain(image);
        nvrhi::TextureDesc desc;
        desc.width = static_cast<std::uint32_t>(image.width);
        desc.height = static_cast<std::uint32_t>(image.height);
        desc.mipLevels = static_cast<std::uint32_t>(chain.size());
        desc.format = nvrhi::Format::RGBA8_UNORM;
        desc.debugName = name;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        nvrhi::TextureHandle texture = device->createTexture(desc);
        upload([&](nvrhi::ICommandList* list) {
            for (std::size_t level = 0; level < chain.size(); ++level) {
                list->writeTexture(texture, 0, static_cast<std::uint32_t>(level), chain[level].pixels.data(),
                                   static_cast<std::size_t>(chain[level].width) * 4);
            }
        });
        return texture;
    }

    nvrhi::ITexture* textureFor(const std::string& path) {
        if (path.empty()) {
            return white;
        }
        if (const auto it = textures.find(path); it != textures.end()) {
            return it->second;
        }
        nvrhi::TextureHandle texture;
        if (const auto image = loadImage(path)) {
            texture = createTexture(*image, path.c_str());
        }
        textures.emplace(path, texture);
        return texture ? texture.Get() : white.Get();
    }

    nvrhi::BindingSetHandle bindingsFor(nvrhi::ITexture* texture) {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, frameConstants),
            nvrhi::BindingSetItem::PushConstants(1, sizeof(MaterialConstants)),
            nvrhi::BindingSetItem::Texture_SRV(0, texture),
            nvrhi::BindingSetItem::Sampler(0, sampler),
        };
        return device->createBindingSet(desc, bindingLayout);
    }

    GpuPart createPart(const Geometry& geometry, const char* name) {
        GpuPart part;
        part.indexCount = static_cast<std::uint32_t>(geometry.indices.size());
        if (part.indexCount == 0) {
            return part;
        }
        nvrhi::BufferDesc vertexDesc;
        vertexDesc.byteSize = geometry.vertices.size() * sizeof(Vertex);
        vertexDesc.isVertexBuffer = true;
        vertexDesc.debugName = name;
        vertexDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
        vertexDesc.keepInitialState = true;
        part.vertices = device->createBuffer(vertexDesc);
        nvrhi::BufferDesc indexDesc;
        indexDesc.byteSize = geometry.indices.size() * sizeof(std::uint32_t);
        indexDesc.isIndexBuffer = true;
        indexDesc.debugName = name;
        indexDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
        indexDesc.keepInitialState = true;
        part.indices = device->createBuffer(indexDesc);
        upload([&](nvrhi::ICommandList* list) {
            list->writeBuffer(part.vertices, geometry.vertices.data(), vertexDesc.byteSize);
            list->writeBuffer(part.indices, geometry.indices.data(), indexDesc.byteSize);
        });
        return part;
    }

    GpuMesh& meshFor(std::uint32_t id) {
        if (const auto it = meshes.find(id); it != meshes.end()) {
            return it->second;
        }
        // Id sem entrada no RenderConfig: cubo unitário, sem imagem.
        if (const auto it = meshes.find(defaultMesh); it != meshes.end()) {
            return it->second;
        }
        GpuMesh& mesh = meshes[defaultMesh];
        const Geometry cube = makePrimitive(PrimitiveShape::Box, glm::vec3{1.0f}, glm::vec3{0.0f});
        mesh.parts.push_back(createPart(cube, "Cubo"));
        mesh.radius = radiusOf(cube);
        mesh.bindings = bindingsFor(white);
        return mesh;
    }

    void createMesh(std::uint32_t id, const PrimitiveMesh& description) {
        GpuMesh& mesh = meshes[id];
        mesh.doubleSided = description.doubleSided;
        mesh.runtimeTexture = description.runtimeTexture;
        mesh.tiling = {description.tiling == TextureTiling::Hex ? 1.0f : 0.0f, description.hex.rotation,
                       description.hex.contrast, 0.0f};
        mesh.bindings = bindingsFor(textureFor(description.texture));
        switch (description.shape) {
        case PrimitiveShape::Custom: break; // chega por updateMesh
        case PrimitiveShape::Model:
            // A imagem do RenderConfig, se houver, vale para todas as partes; senão, cada
            // parte usa a do seu material.
            for (ModelPart& model : loadModel(description.model, description.size, description.offset)) {
                GpuPart part = createPart(model.geometry, description.model.c_str());
                part.color = model.color;
                if (description.texture.empty() && !model.imageFile.empty()) {
                    part.bindings = bindingsFor(textureFor(model.imageFile));
                } else if (description.texture.empty() && model.image) {
                    part.bindings = bindingsFor(createTexture(*model.image, description.model.c_str()));
                }
                mesh.radius = std::max(mesh.radius, radiusOf(model.geometry));
                mesh.parts.push_back(std::move(part));
            }
            break;
        default: {
            const Geometry geometry = makePrimitive(description.shape, description.size, description.offset);
            mesh.parts.push_back(createPart(geometry, "Primitiva"));
            mesh.radius = radiusOf(geometry);
            break;
        }
        }
        if (!description.lods.empty() || description.impostor.enabled) {
            spdlog::info("NVRHI: mesh {}: LOD e impostor ainda não são usados (desenha sempre o original)", id);
        }
    }

    /// Formatos do que o backend desenha (pipelines valem para qualquer tamanho de janela).
    [[nodiscard]] nvrhi::FramebufferInfo framebufferInfo() const {
        nvrhi::FramebufferInfo info;
        info.colorFormats.push_back(graphics->format());
        info.depthFormat = nvrhi::Format::D32;
        return info;
    }

    void createTargets() {
        framebuffers.clear();
        depth = nullptr;
        if (width <= 0 || height <= 0 || graphics->backBufferCount() == 0) {
            return; // minimizada: nada a desenhar até voltar
        }
        nvrhi::TextureDesc desc;
        desc.width = static_cast<std::uint32_t>(width);
        desc.height = static_cast<std::uint32_t>(height);
        desc.format = nvrhi::Format::D32;
        desc.debugName = "Profundidade";
        desc.isRenderTarget = true;
        desc.initialState = nvrhi::ResourceStates::DepthWrite;
        desc.keepInitialState = true;
        desc.setClearValue(nvrhi::Color(0.0f)); // profundidade invertida: longe = 0
        depth = device->createTexture(desc);
        for (std::uint32_t i = 0; i < graphics->backBufferCount(); ++i) {
            nvrhi::FramebufferDesc framebuffer;
            framebuffer.addColorAttachment(graphics->backBuffer(i));
            framebuffer.setDepthAttachment(depth);
            framebuffers.push_back(device->createFramebuffer(framebuffer));
        }
    }

    void resize(int newWidth, int newHeight) {
        device->waitForIdle();
        framebuffers.clear();
        depth = nullptr;
        graphics->resize(newWidth, newHeight);
        width = newWidth;
        height = newHeight;
        createTargets();
    }

    void createPipelines() {
        std::array<nvrhi::VertexAttributeDesc, 6> attributes;
        attributes[0].setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
            .setOffset(offsetof(Vertex, position)).setElementStride(sizeof(Vertex));
        attributes[1].setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
            .setOffset(offsetof(Vertex, normal)).setElementStride(sizeof(Vertex));
        attributes[2].setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setBufferIndex(0)
            .setOffset(offsetof(Vertex, uv)).setElementStride(sizeof(Vertex));
        attributes[3].setName("MODEL").setFormat(nvrhi::Format::RGBA32_FLOAT).setArraySize(4).setBufferIndex(1)
            .setOffset(offsetof(Instance, model)).setElementStride(sizeof(Instance)).setIsInstanced(true);
        attributes[4].setName("COLOR").setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(1)
            .setOffset(offsetof(Instance, color)).setElementStride(sizeof(Instance)).setIsInstanced(true);
        attributes[5].setName("UVTRANSFORM").setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(1)
            .setOffset(offsetof(Instance, uv)).setElementStride(sizeof(Instance)).setIsInstanced(true);
        inputLayout = device->createInputLayout(attributes.data(), static_cast<std::uint32_t>(attributes.size()), vertexShader);

        for (int doubleSided = 0; doubleSided < 2; ++doubleSided) {
            nvrhi::GraphicsPipelineDesc desc;
            desc.inputLayout = inputLayout;
            desc.VS = vertexShader;
            desc.PS = pixelShader;
            desc.bindingLayouts = {bindingLayout};
            desc.renderState.rasterState.frontCounterClockwise = true;
            desc.renderState.rasterState.cullMode = doubleSided != 0 ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;
            desc.renderState.depthStencilState.depthTestEnable = true;
            desc.renderState.depthStencilState.depthWriteEnable = true;
            desc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;
            nvrhi::BlendState::RenderTarget& blend = desc.renderState.blendState.targets[0];
            blend.blendEnable = true; // como a raylib: alfa da imagem mistura com o fundo
            blend.srcBlend = nvrhi::BlendFactor::SrcAlpha;
            blend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
            blend.srcBlendAlpha = nvrhi::BlendFactor::One;
            blend.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
            pipelines[static_cast<std::size_t>(doubleSided)] =
                device->createGraphicsPipeline(desc, framebufferInfo());
        }
    }

    void finishFrame() {
        nvrhi::EventQueryHandle query;
        if (queryPool.empty()) {
            query = device->createEventQuery();
        } else {
            query = queryPool.back();
            queryPool.pop_back();
        }
        device->resetEventQuery(query);
        device->setEventQuery(query, nvrhi::CommandQueue::Graphics);
        inFlight.push_back(query);
        while (inFlight.size() > framesInFlight) {
            device->waitEventQuery(inFlight.front());
            queryPool.push_back(inFlight.front());
            inFlight.pop_front();
        }
        device->runGarbageCollection();
    }
};

NvrhiRenderBackend::NvrhiRenderBackend(RenderConfig config, GraphicsApi api, bool debug)
    : impl_{std::make_unique<Impl>()} {
    impl_->config = std::move(config);
    impl_->api = api;
    impl_->debug = debug;
}

NvrhiRenderBackend::~NvrhiRenderBackend() = default;

bool NvrhiRenderBackend::initialize() {
    Impl& impl = *impl_;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        spdlog::error("NVRHI: SDL_Init falhou ({})", SDL_GetError());
        return false;
    }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if (impl.api == GraphicsApi::Vulkan) {
        flags |= SDL_WINDOW_VULKAN;
    }
    impl.window = SDL_CreateWindow(impl.config.title.c_str(), impl.config.width, impl.config.height, flags);
    if (impl.window == nullptr) {
        spdlog::error("NVRHI: não foi possível criar a janela ({})", SDL_GetError());
        return false;
    }
    SDL_GetWindowSizeInPixels(impl.window, &impl.width, &impl.height);

    impl.graphics = impl.api == GraphicsApi::D3D12 ? createD3D12Device() : createVulkanDevice();
    if (!impl.graphics || !impl.graphics->create(impl.window, impl.width, impl.height, impl.config.vsync, impl.debug)) {
        spdlog::error("NVRHI: {} indisponível nesta máquina", graphicsApiName(impl.api));
        impl.graphics.reset();
        return false;
    }
    impl.device = impl.graphics->device();
    spdlog::info("NVRHI: {} em {}{}", graphicsApiName(impl.api), impl.graphics->adapterName(),
                 impl.debug ? " (com validação)" : "");

    impl.commands = impl.device->createCommandList(commandListParameters());
    impl.vertexShader = impl.shader(nvrhi::ShaderType::Vertex, "mainVS", mesh_vs_dxil, sizeof mesh_vs_dxil, mesh_vs_spirv,
                                    sizeof mesh_vs_spirv);
    impl.pixelShader = impl.shader(nvrhi::ShaderType::Pixel, "mainPS", mesh_ps_dxil, sizeof mesh_ps_dxil, mesh_ps_spirv,
                                   sizeof mesh_ps_spirv);

    nvrhi::BindingLayoutDesc layout;
    layout.visibility = nvrhi::ShaderType::All;
    layout.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::PushConstants(1, sizeof(MaterialConstants)),
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Sampler(0),
    };
    impl.bindingLayout = impl.device->createBindingLayout(layout);
    impl.frameConstants = impl.device->createBuffer(
        nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(FrameConstants), "Constantes do frame", 16));
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    samplerDesc.setAllFilters(true);
    samplerDesc.setMaxAnisotropy(16.0f);
    impl.sampler = impl.device->createSampler(samplerDesc);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    impl.clampSampler = impl.device->createSampler(samplerDesc);

    Image white;
    white.width = 1;
    white.height = 1;
    white.pixels = {255, 255, 255, 255};
    impl.white = impl.createTexture(white, "Branco");

    impl.createTargets();
    impl.createPipelines();
    impl.terrain.initialize(impl.device, impl.framebufferInfo());
    for (const auto& [id, description] : impl.config.meshes) {
        impl.createMesh(id, description);
    }
    if (impl.config.msaa) {
        spdlog::info("NVRHI: MSAA ainda não implementado (desenhando sem)");
    }
    return true;
}

void NvrhiRenderBackend::shutdown() {
    Impl& impl = *impl_;
    if (impl.device) {
        impl.device->waitForIdle();
    }
    impl.inFlight.clear();
    impl.queryPool.clear();
    impl.ui.reset(); // a RmlUi devolve texturas e geometria enquanto o device existe
    impl.uiRenderer.reset();
    impl.terrain.release();
    impl.clampSampler = nullptr;
    impl.batches.clear();
    impl.runtimeTextures.clear();
    impl.meshes.clear();
    impl.textures.clear();
    impl.framebuffers.clear();
    impl.pipelines = {};
    impl.inputLayout = nullptr;
    impl.bindingLayout = nullptr;
    impl.instanceBuffer = nullptr;
    impl.frameConstants = nullptr;
    impl.sampler = nullptr;
    impl.white = nullptr;
    impl.depth = nullptr;
    impl.vertexShader = nullptr;
    impl.pixelShader = nullptr;
    impl.commands = nullptr;
    impl.device = nullptr;
    if (impl.graphics) {
        impl.graphics->destroy();
        impl.graphics.reset();
    }
    for (SDL_Cursor*& cursor : impl.cursors) {
        if (cursor != nullptr) {
            SDL_DestroyCursor(cursor);
            cursor = nullptr;
        }
    }
    if (impl.window != nullptr) {
        SDL_DestroyWindow(impl.window);
        impl.window = nullptr;
    }
    SDL_Quit();
}

void NvrhiRenderBackend::updateMesh(std::uint32_t id, const MeshData& mesh) {
    Impl& impl = *impl_;
    GpuMesh& gpu = impl.meshes[id];
    if (!gpu.bindings) {
        gpu.bindings = impl.bindingsFor(impl.white); // id sem entrada no RenderConfig
    }
    gpu.parts.clear();
    gpu.radius = 0.0f;
    if (!mesh.positions.empty()) {
        const Geometry geometry = fromMeshData(mesh);
        gpu.parts.push_back(impl.createPart(geometry, "Malha do jogo"));
        gpu.radius = radiusOf(geometry);
    }
}

void NvrhiRenderBackend::updateInstanceBatch(std::uint32_t id, const InstanceBatch& batch) {
    Impl& impl = *impl_;
    if (batch.transforms.empty()) {
        impl.batches.erase(id);
        return;
    }
    const GpuMesh& mesh = impl.meshFor(batch.mesh);
    // Cópias agrupadas por célula do plano XZ: cada célula é um trecho contínuo do buffer.
    const float cellSize = std::max(batch.cellSize, 1.0f);
    std::vector<std::pair<std::int64_t, std::uint32_t>> order(batch.transforms.size());
    for (std::size_t i = 0; i < batch.transforms.size(); ++i) {
        const glm::vec4& position = batch.transforms[i][3];
        const auto cx = static_cast<std::int64_t>(std::floor(position.x / cellSize));
        const auto cz = static_cast<std::int64_t>(std::floor(position.z / cellSize));
        order[i] = {(cx << 32) ^ (cz & 0xFFFFFFFF), static_cast<std::uint32_t>(i)};
    }
    std::sort(order.begin(), order.end());
    const bool colored = batch.colors.size() == batch.transforms.size();
    GpuBatch gpu;
    gpu.mesh = batch.mesh;
    gpu.total = order.size();
    std::vector<Instance> instances;
    instances.reserve(order.size());
    for (std::size_t k = 0; k < order.size(); ++k) {
        const glm::mat4& model = batch.transforms[order[k].second];
        if (k == 0 || order[k].first != order[k - 1].first) {
            GpuBatch::Cell cell;
            cell.first = static_cast<std::uint32_t>(k);
            cell.low = glm::vec3{std::numeric_limits<float>::max()};
            cell.high = glm::vec3{std::numeric_limits<float>::lowest()};
            gpu.cells.push_back(cell);
        }
        // Caixa da célula: a esfera de cada cópia (raio da mesh vezes a maior escala).
        const float scale = std::max({glm::length(glm::vec3{model[0]}), glm::length(glm::vec3{model[1]}),
                                      glm::length(glm::vec3{model[2]})});
        const glm::vec3 center{model[3]};
        GpuBatch::Cell& cell = gpu.cells.back();
        cell.low = glm::min(cell.low, center - mesh.radius * scale);
        cell.high = glm::max(cell.high, center + mesh.radius * scale);
        ++cell.count;
        Instance instance{};
        for (int column = 0; column < 4; ++column) {
            instance.model[column] = model[column];
        }
        instance.color = colored ? batch.colors[order[k].second] : glm::vec4{1.0f};
        instance.uv = {1.0f, 1.0f, 0.0f, 0.0f};
        instances.push_back(instance);
    }
    nvrhi::BufferDesc desc;
    desc.byteSize = instances.size() * sizeof(Instance);
    desc.isVertexBuffer = true;
    desc.debugName = "Lote de cópias";
    desc.initialState = nvrhi::ResourceStates::VertexBuffer;
    desc.keepInitialState = true;
    gpu.instances = impl.device->createBuffer(desc);
    impl.upload([&](nvrhi::ICommandList* list) { list->writeBuffer(gpu.instances, instances.data(), desc.byteSize); });
    impl.batches[id] = std::move(gpu);
}

void NvrhiRenderBackend::setTerrain(const TerrainDesc& terrain) {
    Impl& impl = *impl_;
    impl.terrain.set(terrain, impl.textureFor(terrain.texture), impl.clampSampler);
}

void NvrhiRenderBackend::updateTerrain(const TerrainPatch& patch) {
    impl_->terrain.update(patch);
}

void NvrhiRenderBackend::uploadTexture(std::uint32_t id, const TextureImage& image) {
    Impl& impl = *impl_;
    if (!impl.device || image.width <= 0 || image.height <= 0 ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4) {
        return;
    }
    Impl::RuntimeTexture& runtime = impl.runtimeTextures[id];
    runtime.image = Image{image.width, image.height, image.rgba};
    runtime.texture = impl.createTexture(runtime.image, "Textura do jogo");
    runtime.bindings = impl.bindingsFor(runtime.texture);
    for (auto& [meshId, mesh] : impl.meshes) {
        if (mesh.runtimeTexture == id) {
            mesh.bindings = runtime.bindings;
            for (GpuPart& part : mesh.parts) {
                part.bindings = nullptr; // a textura do jogo vale para todas as partes
            }
        }
    }
}

void NvrhiRenderBackend::updateTexture(std::uint32_t id, const TexturePatch& patch) {
    Impl& impl = *impl_;
    const auto it = impl.runtimeTextures.find(id);
    if (it == impl.runtimeTextures.end() || patch.width <= 0 || patch.height <= 0 || patch.x < 0 || patch.y < 0 ||
        patch.x + patch.width > it->second.image.width || patch.y + patch.height > it->second.image.height ||
        patch.rgba.size() != static_cast<std::size_t>(patch.width) * static_cast<std::size_t>(patch.height) * 4) {
        spdlog::warn("NVRHI: mudança fora da textura {}; ignorada", id);
        return;
    }
    Impl::RuntimeTexture& runtime = it->second;
    const auto rowBytes = static_cast<std::size_t>(patch.width) * 4;
    for (int row = 0; row < patch.height; ++row) {
        std::copy_n(patch.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * rowBytes), rowBytes,
                    runtime.image.pixels.begin() +
                        static_cast<std::ptrdiff_t>((static_cast<std::size_t>(patch.y + row) * static_cast<std::size_t>(runtime.image.width) +
                                                     static_cast<std::size_t>(patch.x)) * 4));
    }
    if (patch.strokeEnd) {
        // Fim do traço: a imagem toda de novo, com os mipmaps refeitos.
        const std::vector<Image> chain = mipChain(runtime.image);
        impl.upload([&](nvrhi::ICommandList* list) {
            for (std::size_t level = 0; level < chain.size(); ++level) {
                list->writeTexture(runtime.texture, 0, static_cast<std::uint32_t>(level), chain[level].pixels.data(),
                                   static_cast<std::size_t>(chain[level].width) * 4);
            }
        });
        return;
    }
    // Durante o traço: só o pedaço, no nível mais nítido (numa textura do tamanho dele,
    // copiada para o lugar na GPU).
    nvrhi::TextureDesc desc;
    desc.width = static_cast<std::uint32_t>(patch.width);
    desc.height = static_cast<std::uint32_t>(patch.height);
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.debugName = "Pedaço de textura";
    desc.initialState = nvrhi::ResourceStates::CopySource;
    desc.keepInitialState = true;
    const nvrhi::TextureHandle piece = impl.device->createTexture(desc);
    impl.upload([&](nvrhi::ICommandList* list) {
        list->writeTexture(piece, 0, 0, patch.rgba.data(), rowBytes);
        list->copyTexture(runtime.texture,
                          nvrhi::TextureSlice()
                              .setOrigin(static_cast<std::uint32_t>(patch.x), static_cast<std::uint32_t>(patch.y))
                              .setSize(desc.width, desc.height, 1),
                          piece, nvrhi::TextureSlice());
    });
}

void NvrhiRenderBackend::loadUi(const UiDocument& document) {
    Impl& impl = *impl_;
    if (!impl.device) {
        return;
    }
    if (!impl.ui) {
        impl.uiRenderer =
            std::make_unique<NvrhiUiRenderer>(impl.device, impl.framebufferInfo(), impl.white);
        impl.ui = std::make_unique<ui_rmlui::UiLayer>(*impl.uiRenderer);
    }
    impl.ui->load(document);
}

void NvrhiRenderBackend::updateUi(std::span<const std::pair<std::string, UiValue>> values) {
    if (impl_->ui) {
        impl_->ui->setValues(values);
    }
}

void NvrhiRenderBackend::setMouseCursor(MouseCursor cursor) {
    Impl& impl = *impl_;
    if (impl.window == nullptr || cursor == impl.cursorShape) {
        return;
    }
    impl.cursorShape = cursor;
    SDL_Cursor*& shape = impl.cursors[static_cast<std::size_t>(cursor)];
    if (shape == nullptr) {
        const SDL_SystemCursor system = cursor == MouseCursor::Hand         ? SDL_SYSTEM_CURSOR_POINTER
                                        : cursor == MouseCursor::Move       ? SDL_SYSTEM_CURSOR_MOVE
                                        : cursor == MouseCursor::ResizeNwse ? SDL_SYSTEM_CURSOR_NWSE_RESIZE
                                        : cursor == MouseCursor::ResizeNesw ? SDL_SYSTEM_CURSOR_NESW_RESIZE
                                        : cursor == MouseCursor::Crosshair  ? SDL_SYSTEM_CURSOR_CROSSHAIR
                                                                            : SDL_SYSTEM_CURSOR_DEFAULT;
        shape = SDL_CreateSystemCursor(system);
    }
    if (shape != nullptr) {
        SDL_SetCursor(shape);
    }
}

void NvrhiRenderBackend::beginFrame(const CameraView& camera) {
    Impl& impl = *impl_;
    impl.frameReady = false;
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(impl.window, &width, &height);
    if (width <= 0 || height <= 0) {
        return; // minimizada
    }
    if (width != impl.width || height != impl.height) {
        impl.resize(width, height);
    }
    if (!impl.graphics->acquire()) {
        impl.resize(width, height); // swapchain velha, ou a janela voltou de minimizada
        return;
    }
    if (impl.framebuffers.empty()) {
        return;
    }
    impl.frameReady = true;
    // Profundidade invertida: o perto vira 1 e o longe 0 (perspectiva com near e far trocados).
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const glm::mat4 projection = glm::perspectiveRH_ZO(camera.fovY, aspect, camera.farPlane, camera.nearPlane);
    impl.viewProjection = projection * camera.view;
    impl.cullViewProjection = glm::perspective(camera.fovY, aspect, camera.nearPlane, camera.farPlane) * camera.view;
    impl.cameraPosition = camera.position;
    for (auto& [id, mesh] : impl.meshes) {
        mesh.instances.clear();
    }
}

void NvrhiRenderBackend::submit(std::span<const DrawCommand> commands) {
    Impl& impl = *impl_;
    if (!impl.frameReady) {
        return;
    }
    for (const DrawCommand& command : commands) {
        GpuMesh& mesh = impl.meshFor(command.render.mesh);
        Instance instance{};
        for (int column = 0; column < 4; ++column) {
            instance.model[column] = command.model[column];
        }
        instance.color = command.render.color;
        instance.uv = command.render.uvTransform;
        mesh.instances.push_back(instance);
    }
}

void NvrhiRenderBackend::endFrame() {
    Impl& impl = *impl_;
    if (!impl.frameReady) {
        return;
    }
    // Todas as cópias do frame num buffer só, em blocos por mesh.
    std::size_t total = 0;
    for (const auto& [id, mesh] : impl.meshes) {
        total += mesh.instances.size();
    }
    if (total > impl.instanceCapacity) {
        impl.instanceCapacity = std::max<std::size_t>(total + total / 2, 1024);
        nvrhi::BufferDesc desc;
        desc.byteSize = impl.instanceCapacity * sizeof(Instance);
        desc.isVertexBuffer = true;
        desc.debugName = "Instâncias";
        desc.initialState = nvrhi::ResourceStates::VertexBuffer;
        desc.keepInitialState = true;
        impl.instanceBuffer = impl.device->createBuffer(desc);
    }
    std::vector<Instance> all;
    all.reserve(total);
    std::vector<std::pair<GpuMesh*, std::size_t>> batches; // mesh, primeira cópia
    for (auto& [id, mesh] : impl.meshes) {
        if (!mesh.instances.empty()) {
            batches.emplace_back(&mesh, all.size());
            all.insert(all.end(), mesh.instances.begin(), mesh.instances.end());
        }
    }

    const std::uint32_t index = impl.graphics->backBufferIndex();
    nvrhi::IFramebuffer* framebuffer = impl.framebuffers[index];
    nvrhi::ICommandList* list = impl.commands;
    list->open();
    const glm::vec4& clear = impl.config.clearColor;
    list->clearTextureFloat(impl.graphics->backBuffer(index), nvrhi::AllSubresources, nvrhi::Color(clear.r, clear.g, clear.b, clear.a));
    list->clearDepthStencilTexture(impl.depth, nvrhi::AllSubresources, true, 0.0f, false, 0);
    const FrameConstants constants{impl.viewProjection, glm::vec4{glm::normalize(impl.config.lightDirection), 0.0f}};
    list->writeBuffer(impl.frameConstants, &constants, sizeof constants);
    if (!all.empty()) {
        list->writeBuffer(impl.instanceBuffer, all.data(), all.size() * sizeof(Instance));
    }

    impl.drawCalls = 0;
    impl.triangles = 0;
    impl.terrain.draw(list, framebuffer, impl.viewProjection, impl.cullViewProjection, impl.cameraPosition,
                      impl.config.lightDirection, impl.lodColors, impl.drawCalls, impl.triangles);

    const nvrhi::ViewportState viewport =
        nvrhi::ViewportState().addViewportAndScissorRect(framebuffer->getFramebufferInfo().getViewport());
    for (const auto& [mesh, first] : batches) {
        for (const GpuPart& part : mesh->parts) {
            if (part.indexCount == 0) {
                continue;
            }
            nvrhi::GraphicsState state;
            state.pipeline = impl.pipelines[mesh->doubleSided ? 1 : 0];
            state.framebuffer = framebuffer;
            state.viewport = viewport;
            state.bindings = {part.bindings ? part.bindings : mesh->bindings};
            state.vertexBuffers = {
                nvrhi::VertexBufferBinding().setBuffer(part.vertices).setSlot(0).setOffset(0),
                nvrhi::VertexBufferBinding().setBuffer(impl.instanceBuffer).setSlot(1).setOffset(first * sizeof(Instance)),
            };
            state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(part.indices).setFormat(nvrhi::Format::R32_UINT);
            list->setGraphicsState(state);
            const MaterialConstants material{part.color, mesh->tiling};
            list->setPushConstants(&material, sizeof material);
            list->drawIndexed(nvrhi::DrawArguments()
                                  .setVertexCount(part.indexCount)
                                  .setInstanceCount(static_cast<std::uint32_t>(mesh->instances.size())));
            ++impl.drawCalls;
            impl.triangles += static_cast<std::size_t>(part.indexCount / 3) * mesh->instances.size();
        }
    }
    impl.objects = total;
    // Lotes: só as células dentro da câmera; células seguidas viram um draw call.
    const auto planes = frustumPlanes(impl.cullViewProjection);
    for (auto& [id, batch] : impl.batches) {
        GpuMesh& mesh = impl.meshFor(batch.mesh);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges; // primeira cópia, quantas
        for (const GpuBatch::Cell& cell : batch.cells) {
            if (!boxVisible(planes, cell.low, cell.high)) {
                continue;
            }
            if (!ranges.empty() && ranges.back().first + ranges.back().second == cell.first) {
                ranges.back().second += cell.count;
            } else {
                ranges.emplace_back(cell.first, cell.count);
            }
        }
        for (const auto& [first, count] : ranges) {
            impl.objects += count;
            for (const GpuPart& part : mesh.parts) {
                if (part.indexCount == 0) {
                    continue;
                }
                nvrhi::GraphicsState state;
                state.pipeline = impl.pipelines[mesh.doubleSided ? 1 : 0];
                state.framebuffer = framebuffer;
                state.viewport = viewport;
                state.bindings = {part.bindings ? part.bindings : mesh.bindings};
                state.vertexBuffers = {
                    nvrhi::VertexBufferBinding().setBuffer(part.vertices).setSlot(0).setOffset(0),
                    nvrhi::VertexBufferBinding().setBuffer(batch.instances).setSlot(1).setOffset(first * sizeof(Instance)),
                };
                state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(part.indices).setFormat(nvrhi::Format::R32_UINT);
                list->setGraphicsState(state);
                const MaterialConstants material{part.color, mesh.tiling};
                list->setPushConstants(&material, sizeof material);
                list->drawIndexed(nvrhi::DrawArguments().setVertexCount(part.indexCount).setInstanceCount(count));
                ++impl.drawCalls;
                impl.triangles += static_cast<std::size_t>(part.indexCount / 3) * count;
            }
        }
    }
    if (impl.ui && impl.ui->active()) {
        impl.uiRenderer->setTarget(list, framebuffer);
        impl.ui->setStats({impl.fps, impl.drawCalls, impl.triangles, impl.objects, impl.lodColors});
        impl.ui->render(impl.width, impl.height);
    }
    list->close();
    impl.device->executeCommandList(list);
    impl.graphics->present();
    ++impl.framesThisSecond;
    const std::uint64_t now = SDL_GetTicks();
    if (now - impl.secondStart >= 1000) {
        impl.fps = static_cast<int>(impl.framesThisSecond * 1000 / static_cast<int>(now - impl.secondStart));
        impl.framesThisSecond = 0;
        impl.secondStart = now;
    }
    impl.finishFrame();
}

bool NvrhiRenderBackend::closeRequested() const {
    return impl_->closing;
}

void NvrhiRenderBackend::pollInput(InputState& input) {
    Impl& impl = *impl_;
    if (impl.window == nullptr) {
        return;
    }
    // A SDL dá o mouse em pontos da janela; a engine trabalha em pixels.
    const float density = SDL_GetWindowPixelDensity(impl.window);
    ui_rmlui::UiMouse mouse;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: impl.closing = true; break;
        case SDL_EVENT_KEY_DOWN:
            if (const auto key = keyFromScancode(event.key.scancode); key && !event.key.repeat) {
                impl.keys.set(InputState::index(*key));
                input.keysPressed.set(InputState::index(*key));
                if (*key == Key::Escape && impl.config.escapeClosesWindow) {
                    impl.closing = true;
                }
                if (*key == impl.config.lodDebugKey) {
                    impl.lodColors = !impl.lodColors;
                }
                if (*key == impl.config.uiDebuggerKey && impl.ui && impl.ui->active()) {
                    impl.ui->toggleDebugger();
                }
            }
            break;
        case SDL_EVENT_KEY_UP:
            if (const auto key = keyFromScancode(event.key.scancode)) {
                impl.keys.reset(InputState::index(*key));
                input.keysReleased.set(InputState::index(*key));
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            input.mouseDelta += glm::vec2{event.motion.xrel, event.motion.yrel} * density;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (const auto button = buttonFromSdl(event.button.button)) {
                mouse.pressed[InputState::index(*button)] = true;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (const auto button = buttonFromSdl(event.button.button)) {
                mouse.released[InputState::index(*button)] = true;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL: mouse.wheel += event.wheel.y; break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: impl.keys.reset(); break; // teclas soltas fora da janela
        default: break;
        }
    }
    input.keys = impl.keys;
    float x = 0.0f;
    float y = 0.0f;
    const SDL_MouseButtonFlags held = SDL_GetMouseState(&x, &y);
    mouse.position = glm::vec2{x, y} * density;

    // A interface vê o mouse primeiro: o que começa sobre um painel ou botão (clique,
    // arrasto, roda) fica com ela e não chega ao jogo.
    const bool overUi = impl.ui && impl.ui->active() && impl.ui->processMouse(mouse);
    input.pointerOverUi = overUi;
    constexpr std::array<SDL_MouseButtonFlags, 3> masks{SDL_BUTTON_LMASK, SDL_BUTTON_RMASK, SDL_BUTTON_MMASK};
    for (std::size_t b = 0; b < masks.size(); ++b) {
        if (mouse.pressed[b] && overUi) {
            impl.uiHeld[b] = true;
        }
        input.buttons.set(b, (held & masks[b]) != 0 && !impl.uiHeld[b]);
        if (mouse.pressed[b] && !impl.uiHeld[b]) {
            input.buttonsPressed.set(b);
        }
        if (mouse.released[b]) {
            if (!impl.uiHeld[b]) {
                input.buttonsReleased.set(b);
            }
            impl.uiHeld[b] = false;
        }
    }
    input.mousePosition = mouse.position;
    if (!overUi) {
        input.wheel += mouse.wheel;
    }
    if (impl.ui) {
        for (auto& uiEvent : impl.ui->takeEvents()) {
            if (uiEvent.action == "lkz.lodColors") {
                impl.lodColors = !impl.lodColors;
            } else if (!uiEvent.action.starts_with("lkz.")) {
                input.uiEvents.push_back(std::move(uiEvent));
            }
        }
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(impl.window, &width, &height);
    input.viewport = glm::vec2{static_cast<float>(width), static_cast<float>(height)};
}

} // namespace lkz
