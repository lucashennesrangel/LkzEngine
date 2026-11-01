// Pintor de modelos: abre um modelo glTF, pinta direto sobre a forma 3D (cor, materiais da
// paleta e borracha), escolhe o destino de cada face com o balde, carimba decalques (janela,
// porta, letreiro) e exporta um .glb.
//
// Ferramenta da engine, não de um jogo: o jogo diz onde ficam os modelos, a paleta e os
// decalques num arquivo de projeto (ver Project.hpp).
//
// Uso: painter [projeto.json] [modelo.glb | modelo.gltf] [--d3d12 | --vulkan]
//
// Mouse: esquerdo pinta, aplica o balde ou carimba (Alt + esquerdo: conta-gotas), direito
// gira, meio ou Shift + direito arrasta, roda aproxima, Ctrl + roda muda o tamanho do pincel.
// Teclas: B pincel, G balde, C carimbo, E borracha, I conta-gotas, [ e ] tamanho, F enquadra,
// R gira o carimbo, Enter confirma o carimbo, Ctrl+Z desfaz, Ctrl+Y ou Ctrl+Shift+Z refaz,
// Ctrl+O abre, Ctrl+S exporta, Esc cancela o carimbo ou fecha a janela aberta.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <lkz/Core/Log.hpp>
#include <lkz/ECS/Components.hpp>
#include <lkz/Scene/Camera.hpp>
#include <lkz/Asset/Unwrap.hpp>
#include <lkz/World.hpp>
#include <lkz/backends/RaylibRenderBackend.hpp>
#if defined(LKZ_WITH_NVRHI)
#include <lkz/backends/NvrhiRenderBackend.hpp>
#endif
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "Document.hpp"
#include "OrbitCamera.hpp"
#include "Palette.hpp"
#include "Project.hpp"

namespace {

namespace fs = std::filesystem;
using lkz::asset::SurfaceTarget;
using Surface = painter::Document::Surface;

constexpr int maxMaterials = 64;                       // materiais do modelo na tela
constexpr int maxPalette = 64;                         // materiais da paleta na tela
constexpr int colorSlot = maxMaterials + maxPalette;   // as amostras de cor lisa
constexpr int decalBase = colorSlot + 1;               // imagens de decalque
constexpr int maxDecals = 64;
constexpr int previewSlot = decalBase + maxDecals;     // prévia do carimbo
constexpr int tintBase = previewSlot + 1;              // material da paleta tingido: um lugar por (material, cor)
constexpr int maxTinted = 32;
constexpr int slotCount = tintBase + maxTinted;
constexpr float designHeight = 720.0f; // 1dp = altura da janela / isto
constexpr int listSlots = 14;          // linhas da janela Abrir
constexpr int rootSlots = 8;
constexpr int folderSlots = 8;
constexpr int paletteSlots = 16;       // miniaturas por página (paleta e decalques)
constexpr glm::vec4 highlightTint{1.0f, 0.78f, 0.3f, 1.0f};

/// Cada superfície ocupa um "lugar": os materiais do modelo, os da paleta, as cores, os
/// decalques e a prévia do carimbo. Um lugar tem uma textura e quatro malhas (um lado ou
/// dois; normal ou realçada), com ids fixos desde a criação da janela.
std::uint32_t meshId(int slot, bool doubleSided, bool highlighted) {
    return static_cast<std::uint32_t>(1 + slot * 4 + (doubleSided ? 2 : 0) + (highlighted ? 1 : 0));
}
std::uint32_t textureId(int slot) {
    return static_cast<std::uint32_t>(1 + slot);
}
int slotOf(const Surface& surface) {
    switch (surface.kind) {
    case Surface::Kind::Material: return surface.index < maxMaterials ? surface.index : -1;
    case Surface::Kind::Shared: return surface.index < maxPalette ? maxMaterials + surface.index : -1;
    case Surface::Kind::Color: return colorSlot;
    case Surface::Kind::Decal: return surface.index < maxDecals ? decalBase + surface.index : -1;
    }
    return -1;
}

/// Superfície de um material da paleta ou de um decalque com cor (diferente de branco).
bool tinted(const Surface& surface) {
    return (surface.kind == Surface::Kind::Shared || surface.kind == Surface::Kind::Decal) &&
           glm::vec3{surface.color} != glm::vec3{1.0f};
}
/// De onde vem a imagem de uma superfície tingida: tipo e índice num número só.
int tintSource(const Surface& surface) {
    return static_cast<int>(surface.kind) * 65536 + surface.index;
}
/// (imagem, cor) de uma superfície tingida.
std::pair<int, std::uint32_t> tintKey(const Surface& surface) {
    const auto byte = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return {tintSource(surface), byte(surface.color.r) << 16 | byte(surface.color.g) << 8 | byte(surface.color.b)};
}

enum class Tool : std::uint8_t { Brush, Bucket, Stamp, Hand, Sculpt, Eraser, Picker };
/// Como o Esculpir seleciona: como o balde (triângulo, parede, plano) ou um retângulo desenhado.
enum class SculptPick : std::uint8_t { Triangle, Wall, Plane, Rectangle };
enum class Paint : std::uint8_t { Color, Material };
enum class Browse : std::uint8_t { Models, ImportPalette, ImportDecals };

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

fs::path fromUtf8(std::string_view text) {
    return fs::path{std::u8string{text.begin(), text.end()}};
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return text;
}

std::string hex(const glm::vec3& color) {
    const auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return fmt::format("#{:02x}{:02x}{:02x}", byte(color.r), byte(color.g), byte(color.b));
}

std::optional<glm::vec3> parseHex(std::string_view text) {
    if (text.size() != 7 || text[0] != '#') {
        return std::nullopt;
    }
    const auto channel = [&](std::size_t at) { return std::strtol(std::string{text.substr(at, 2)}.c_str(), nullptr, 16); };
    return glm::vec3{static_cast<float>(channel(1)), static_cast<float>(channel(3)), static_cast<float>(channel(5))} / 255.0f;
}

/// Matiz (0..1), saturação e brilho.
glm::vec3 toHsv(const glm::vec3& c) {
    const float high = std::max({c.r, c.g, c.b});
    const float low = std::min({c.r, c.g, c.b});
    const float delta = high - low;
    float hue = 0.0f;
    if (delta > 1e-6f) {
        if (high == c.r) {
            hue = std::fmod((c.g - c.b) / delta, 6.0f);
        } else if (high == c.g) {
            hue = (c.b - c.r) / delta + 2.0f;
        } else {
            hue = (c.r - c.g) / delta + 4.0f;
        }
        hue /= 6.0f;
        if (hue < 0.0f) {
            hue += 1.0f;
        }
    }
    return {hue, high > 0.0f ? delta / high : 0.0f, high};
}

glm::vec3 fromHsv(const glm::vec3& hsv) {
    const float h = hsv.x * 6.0f;
    const float c = hsv.z * hsv.y;
    const float x = c * (1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f));
    const float m = hsv.z - c;
    glm::vec3 rgb{0.0f};
    switch (static_cast<int>(h) % 6) {
    case 0: rgb = {c, x, 0.0f}; break;
    case 1: rgb = {x, c, 0.0f}; break;
    case 2: rgb = {0.0f, c, x}; break;
    case 3: rgb = {0.0f, x, c}; break;
    case 4: rgb = {x, 0.0f, c}; break;
    default: rgb = {c, 0.0f, x}; break;
    }
    return rgb + m;
}

std::string lengthText(float meters) {
    return meters < 1.0f ? fmt::format("{:.1f} cm", meters * 100.0f) : fmt::format("{:.2f} m", meters);
}

const char* targetName(SurfaceTarget target, bool restore) {
    if (restore) {
        return "original";
    }
    switch (target) {
    case SurfaceTarget::Shared: return "compartilhado";
    case SurfaceTarget::Color: return "cor";
    default: return "unico";
    }
}

class Painter {
public:
    Painter(lkz::World& world, lkz::Camera& camera, std::optional<painter::Project> project)
        : world_{world}, camera_{camera}, project_{std::move(project)} {
        if (project_) {
            set("projeto", project_->name);
            std::string reports;
            if (!project_->palette.empty()) {
                std::string report;
                palette_ = painter::Palette::load(project_->palette, report, painter::Palette::Kind::Materials);
                spdlog::info("Pintor: {} em {}", report, utf8(project_->palette));
                if (palette_.size() > maxPalette) {
                    spdlog::warn("Pintor: a paleta tem {} materiais; só os {} primeiros aparecem", palette_.size(), maxPalette);
                }
                reports = report;
            }
            if (!project_->decals.empty()) {
                std::string report;
                decals_ = painter::Palette::load(project_->decals, report, painter::Palette::Kind::Decals);
                spdlog::info("Pintor: {} em {}", report, utf8(project_->decals));
                reports += (reports.empty() ? "" : " · ") + report;
            }
            say(reports);
        }
        doc_.setPalette(&palette_);
        doc_.setDecals(&decals_);
        uploadedPalette_.assign(static_cast<std::size_t>(maxPalette), false);
        uploadedDecals_.assign(static_cast<std::size_t>(maxDecals), false);
        paletteIndex_ = palette_.size() > 0 ? 0 : -1;
        decalIndex_ = decals_.size() > 0 ? 0 : -1;
        setColor(brush_.color);
        showBrush();
        showDensity();
        showPalette();
        showDecals();
        showBucket();
    }

    /// Todos os nomes que a interface usa, com o valor inicial.
    static std::vector<std::pair<std::string, lkz::UiValue>> uiValues() {
        std::vector<std::pair<std::string, lkz::UiValue>> values = {
            {"projeto", std::string{"sem projeto"}},
            {"arquivo", std::string{"Nenhum modelo aberto"}},
            {"info", std::string{"Abra um modelo (Ctrl+O)"}},
            {"mensagem", std::string{}},
            {"modificado", false},
            {"ferramenta", std::string{"pincel"}},
            {"tinta", std::string{"cor"}},
            {"cor", std::string{"#000000"}},
            {"corTexto", std::string{}},
            {"matizValor", std::string{}},
            {"saturacaoValor", std::string{}},
            {"brilhoValor", std::string{}},
            {"temPaleta", false},
            {"tingir", false},
            {"palSel", -1},
            {"palPagina", std::string{}},
            {"palMais", false},
            {"matNome", std::string{}},
            {"matEscala", std::string{}},
            {"temDecalques", false},
            {"decSel", -1},
            {"decPagina", std::string{}},
            {"decMais", false},
            {"decNome", std::string{}},
            {"carimbando", false},
            {"carimboInfo", std::string{}},
            {"escSel", std::string{"parede"}},
            {"escModo", std::string{"puxar"}},
            {"escInfo", std::string{"Nada selecionado"}},
            {"escDist", std::string{"0"}},
            {"escAberto", false},
            {"selecionado", false},
            {"carimboCor", std::string{"original"}},
            {"profundidade", std::string{"0 (plano)"}},
            {"selecao", std::string{"parede"}},
            {"baldeDestino", std::string{"compartilhado"}},
            {"destinoAjuda", std::string{}},
            {"selecaoInfo", std::string{}},
            {"tamanho", std::string{}},
            {"dureza", 0},
            {"opacidade", 0},
            {"podeDesfazer", false},
            {"podeRefazer", false},
            {"cursor", false},
            {"cursorX", 0.0},
            {"cursorY", 0.0},
            {"cursorD", 0.0},
            {"abrindo", false},
            {"abrirTitulo", std::string{"Abrir modelo"}},
            {"pastaAtual", std::string{}},
            {"pagina", std::string{}},
            {"raizAtiva", 0},
            {"importConflito", false},
            {"importConflitoTexto", std::string{}},
            {"uvUnico", false},
            {"uvAviso", std::string{}},
            {"densidade", std::string{}},
            {"perguntaUv", false},
            {"perguntaUvTexto", std::string{}},
            {"exportando", false},
            {"destino", std::string{}},
            {"destinoExiste", false},
            {"pastaEscolhida", std::string{}},
        };
        for (int i = 0; i < rootSlots; ++i) {
            values.emplace_back(fmt::format("raiz{}", i), std::string{});
        }
        for (int i = 0; i < listSlots; ++i) {
            values.emplace_back(fmt::format("ent{}", i), std::string{});
            values.emplace_back(fmt::format("entPasta{}", i), false);
        }
        for (int i = 0; i < folderSlots; ++i) {
            values.emplace_back(fmt::format("pasta{}", i), std::string{});
        }
        for (int i = 0; i < paletteSlots; ++i) {
            values.emplace_back(fmt::format("pal{}", i), std::string{});
            values.emplace_back(fmt::format("dec{}", i), std::string{});
        }
        return values;
    }

    void open(const fs::path& path) {
        doc_.endStroke();
        painter::Document next;
        next.setPalette(&palette_);
        next.setDecals(&decals_);
        std::string error;
        if (!next.open(path, error)) {
            say(error);
            return;
        }
        if (next.model().materials.size() > static_cast<std::size_t>(maxMaterials)) {
            say(fmt::format("{} tem {} materiais; o Pintor mostra até {}", utf8(path.filename()),
                            next.model().materials.size(), maxMaterials));
            return;
        }
        cancelStamp();
        doc_ = std::move(next);
        allowOverlap_ = false;
        closeUvQuestion();
        show();
        const glm::vec3 low = doc_.raycaster().boundsMin();
        const glm::vec3 high = doc_.raycaster().boundsMax();
        camera_.frame(low, high);
        // Pincel inicial: 2% da diagonal do modelo.
        setRadius(glm::length(high - low) * 0.02f);
        set("arquivo", utf8(path.filename()));
        say("Aberto: " + shortName(path));
        spdlog::info("Pintor: {} ({})", utf8(path), doc_.summary());
    }

    /// Modelo novo (ou UV novo): as telas e depois as superfícies.
    void show() {
        selected_.clear();
        sculptFaces_.clear();
        sculptRect_.reset();
        sculptPress_.reset();
        sculptDrag_ = SculptDrag::None;
        handPress_.reset();
        for (int m = 0; m < std::min(static_cast<int>(doc_.model().materials.size()), maxMaterials); ++m) {
            lkz::TextureImage image = doc_.canvasImage(m);
            // Sem tela (material sem UV): imagem branca, e a cor vem da instância.
            world_.uploadTexture(textureId(m), image.rgba.empty() ? lkz::TextureImage{1, 1, {255, 255, 255, 255}}
                                                                  : std::move(image));
        }
        uploadedDecals_.assign(static_cast<std::size_t>(maxDecals), false); // os decalques do documento são outros
        std::fill(tintTexture_.begin(), tintTexture_.end(), -1);
        tintSlots_.clear();
        highlight_.clear();
        doc_.takeGeometryChanged();
        showSurfaces();
        set("info", doc_.summary());
        set("uvUnico", doc_.needsUniqueUv());
        set("uvAviso", doc_.uvOverlap() > 0.01f
                           ? fmt::format("UV sobreposto em {:.0f}%: pintar uma face pinta as que dividem a imagem com ela.",
                                         doc_.uvOverlap() * 100.0f)
                           : doc_.needsUniqueUv() ? std::string{"Há materiais sem UV: só a cor deles aparece."}
                                                  : std::string{});
    }

    /// As malhas do documento na tela, com o realce do balde.
    void showSurfaces() {
        auto& registry = world_.registry();
        std::set<std::uint32_t> used;
        bool swatch = false;
        std::vector<Surface> surfaces = doc_.surfaces(highlight_);
        // Cada (imagem, cor) tingida ganha um lugar seu (com a mesma imagem): no lugar da
        // imagem, uma cor apagaria a outra. Quem já tinha lugar fica nele.
        std::map<std::pair<int, std::uint32_t>, int> tints;
        for (const Surface& surface : surfaces) {
            if (tinted(surface)) {
                tints.emplace(tintKey(surface), -1);
            }
        }
        std::vector<char> taken(static_cast<std::size_t>(maxTinted), 0);
        for (auto& [key, place] : tints) {
            if (const auto it = tintSlots_.find(key); it != tintSlots_.end()) {
                place = it->second;
                taken[static_cast<std::size_t>(place)] = 1;
            }
        }
        for (auto& [key, place] : tints) {
            if (place < 0) {
                const auto free = std::find(taken.begin(), taken.end(), 0);
                if (free == taken.end()) {
                    say(fmt::format("Mais de {} combinações de imagem e cor: algumas aparecem sem a cor no Pintor "
                                    "(a exportação sai certa)",
                                    maxTinted));
                    continue;
                }
                *free = 1;
                place = static_cast<int>(free - taken.begin());
            }
        }
        tintSlots_.clear();
        for (const auto& [key, place] : tints) {
            if (place >= 0) {
                tintSlots_.emplace(key, place);
            }
        }
        for (Surface& surface : surfaces) {
            const int slot = slotOf(surface);
            if (slot < 0) {
                continue;
            }
            int drawSlot = slot;
            if (tinted(surface)) {
                if (const int place = tints[tintKey(surface)]; place >= 0) {
                    drawSlot = tintBase + place;
                    if (tintTexture_[static_cast<std::size_t>(place)] != tintSource(surface)) {
                        const lkz::asset::ImageData& image = surface.kind == Surface::Kind::Shared
                                                                 ? palette_.at(surface.index).mips.front()
                                                                 : doc_.decalImage(surface.index);
                        if (!image.empty()) {
                            world_.uploadTexture(textureId(drawSlot), lkz::TextureImage{image.width, image.height, image.rgba});
                        }
                        tintTexture_[static_cast<std::size_t>(place)] = tintSource(surface);
                    }
                }
            }
            if (surface.kind == Surface::Kind::Shared && !uploadedPalette_[static_cast<std::size_t>(surface.index)]) {
                const lkz::asset::ImageData& image = palette_.at(surface.index).mips.front();
                world_.uploadTexture(textureId(slot), lkz::TextureImage{image.width, image.height, image.rgba});
                uploadedPalette_[static_cast<std::size_t>(surface.index)] = true;
            } else if (surface.kind == Surface::Kind::Decal && !uploadedDecals_[static_cast<std::size_t>(surface.index)]) {
                const lkz::asset::ImageData& image = doc_.decalImage(surface.index);
                if (!image.empty()) {
                    world_.uploadTexture(textureId(slot), lkz::TextureImage{image.width, image.height, image.rgba});
                }
                uploadedDecals_[static_cast<std::size_t>(surface.index)] = true;
            } else if (surface.kind == Surface::Kind::Color && !swatch) {
                world_.uploadTexture(textureId(slot), doc_.swatchImage());
                swatch = true;
            }
            const std::uint32_t id = meshId(drawSlot, surface.doubleSided, surface.highlighted);
            const glm::vec4 color = surface.highlighted ? surface.color * highlightTint : surface.color;
            world_.uploadMesh(id, std::move(surface.mesh));
            registry.emplace_or_replace<lkz::RenderComponent>(nodeFor(id).handle(), id, 0u, color);
            used.insert(id);
        }
        const std::uint32_t preview = meshId(previewSlot, true, false);
        for (const auto& [id, node] : nodes_) {
            if (!used.contains(id) && id != preview) {
                registry.remove<lkz::RenderComponent>(node->handle());
            }
        }
    }

    /// O UV único leva uns segundos: a mensagem aparece neste quadro, o trabalho no próximo.
    void requestUniqueUv() {
        if (!doc_.isOpen()) {
            return;
        }
        finishSculpt();
        cancelStamp();
        say("Gerando o UV único… (alguns segundos)");
        uvPending_ = true;
    }

    void makeUniqueUv() {
        lkz::asset::UnwrapOptions options;
        options.texelsPerMeter = density_;
        std::string report;
        if (doc_.makeUniqueUv(options, report)) {
            allowOverlap_ = false;
            show();
        }
        say(report);
        spdlog::info("Pintor: {}", report);
    }

    /// Pintar numa tela com UV sobreposto pinta outras faces junto: pergunta antes.
    void askUniqueUv(float overlap) {
        askingUv_ = true;
        set("cursor", false);
        set("perguntaUv", true);
        set("perguntaUvTexto",
            fmt::format("Este modelo usa a mesma parte da imagem em várias faces ({:.0f}% sobreposto). Pintando assim, "
                        "a tinta aparece em todas elas ao mesmo tempo, e o pincel fica lento. O UV único dá a cada face "
                        "o seu próprio lugar na imagem (leva alguns segundos e apaga o desfazer).",
                        overlap * 100.0f));
    }

    void closeUvQuestion() {
        askingUv_ = false;
        set("perguntaUv", false);
    }

    void showDensity() { set("densidade", fmt::format("{:.0f} px/m", density_)); }

    void tick(lkz::World& world) {
        if (uvPending_) {
            uvPending_ = false;
            makeUniqueUv();
        }
        const lkz::InputState& input = world.input();
        for (const lkz::UiEvent& event : input.uiEvents) {
            handleUi(event);
        }
        handleKeys(input);
        if (!dialogOpen()) {
            camera_.update(input);
            handleMouse(input);
        } else {
            set("cursor", false);
            setHighlight({});
            world_.setMouseCursor(lkz::MouseCursor::Default);
        }
        if (tool_ != Tool::Sculpt && doc_.sculpting()) {
            finishSculpt();
        }
        if (doc_.takeGeometryChanged() || highlightChanged_) {
            highlightChanged_ = false;
            showSurfaces();
        }
        if (previewChanged_) {
            previewChanged_ = false;
            showPreview();
        }
        std::vector<painter::CanvasPatch> patches;
        doc_.takePatches(patches);
        for (painter::CanvasPatch& patch : patches) {
            world.updateTexture(textureId(patch.material), std::move(patch.patch));
        }
        set("podeDesfazer", doc_.canUndo());
        set("podeRefazer", doc_.canRedo());
        set("modificado", doc_.modified());
    }

private:
    [[nodiscard]] bool dialogOpen() const { return browsing_ || exporting_ || askingUv_ || importConflict_; }

    void set(const std::string& name, lkz::UiValue value) {
        const auto it = shown_.find(name);
        if (it != shown_.end() && it->second == value) {
            return;
        }
        shown_[name] = value;
        world_.setUiValue(name, std::move(value));
    }

    void say(std::string text) { set("mensagem", std::move(text)); }

    lkz::SceneNode& nodeFor(std::uint32_t id) {
        auto [it, inserted] = nodes_.try_emplace(id, nullptr);
        if (inserted) {
            it->second = &world_.root().emplaceChild<lkz::SceneNode>(fmt::format("Superfície {}", id), world_.registry());
        }
        return *it->second;
    }

    /// Caminho para mostrar: relativo à pasta do projeto, se estiver nela.
    [[nodiscard]] std::string shortName(const fs::path& path) const {
        if (project_) {
            const fs::path relative = path.lexically_relative(project_->file.parent_path());
            if (!relative.empty() && *relative.begin() != "..") {
                return utf8(relative.generic_u8string());
            }
        }
        return utf8(path.filename());
    }

    // --- Tinta ---

    void setColor(const glm::vec3& color) {
        brush_.color = glm::clamp(color, glm::vec3{0.0f}, glm::vec3{1.0f});
        const glm::vec3 hsv = toHsv(brush_.color);
        set("cor", hex(brush_.color));
        set("corTexto", hex(brush_.color));
        set("matizValor", fmt::format("{:.0f}°", hsv.x * 360.0f));
        set("saturacaoValor", fmt::format("{:.0f}%", hsv.y * 100.0f));
        set("brilhoValor", fmt::format("{:.0f}%", hsv.z * 100.0f));
        if (pending_ && pending_->placement.tintMode != painter::DecalTint::None) {
            pending_->placement.tint = brush_.color;
            previewChanged_ = true;
        }
    }

    void adjustHsv(int component, int step) {
        glm::vec3 hsv = toHsv(brush_.color);
        if (component == 0) {
            hsv.x = std::fmod(hsv.x + static_cast<float>(step) / 36.0f + 1.0f, 1.0f); // 10° por clique
        } else {
            hsv[component] = std::clamp(hsv[component] + static_cast<float>(step) * 0.05f, 0.0f, 1.0f);
        }
        setColor(fromHsv(hsv));
    }

    /// Cor ou material da paleta. O destino do balde acompanha: material -> Compartilhado,
    /// cor -> Só cor (Único e Original valem para os dois).
    void setPaint(Paint paint) {
        if (paint == Paint::Material && paletteIndex_ < 0) {
            say("Sem materiais na paleta: use Importar imagem… (ou ponha imagens na pasta da paleta)");
            paint = Paint::Color;
        }
        paint_ = paint;
        if (!restore_) {
            if (paint_ == Paint::Material && target_ == SurfaceTarget::Color) {
                target_ = SurfaceTarget::Shared;
            } else if (paint_ == Paint::Color && target_ == SurfaceTarget::Shared) {
                target_ = SurfaceTarget::Color;
            }
        }
        set("tinta", paint_ == Paint::Material ? "material" : "cor");
        showBucket();
    }

    /// Material da paleta tingido pela cor (a textura dá os tons, a cor dá a cor).
    void setTint(bool tint) {
        tint_ = tint;
        set("tingir", tint_);
    }

    void choosePaletteMaterial(int index) {
        if (index < 0 || index >= std::min(palette_.size(), maxPalette)) {
            return;
        }
        paletteIndex_ = index;
        palettePage_ = index / paletteSlots;
        showPalette();
        setPaint(Paint::Material);
    }

    /// Miniaturas de uma biblioteca (paleta ou decalques) na página: `prefix`0..15.
    void showThumbnails(const painter::Palette& library, int limit, int selected, int& page, const char* prefix,
                        const char* selectedName, const char* pageName, const char* moreName) {
        const int count = std::min(library.size(), limit);
        const int pages = std::max(1, (count + paletteSlots - 1) / paletteSlots);
        page = std::clamp(page, 0, pages - 1);
        for (int i = 0; i < paletteSlots; ++i) {
            const int index = page * paletteSlots + i;
            set(fmt::format("{}{}", prefix, i), index < count ? utf8(library.at(index).image.generic_u8string()) : std::string{});
        }
        set(selectedName, selected >= 0 && selected / paletteSlots == page ? selected % paletteSlots : -1);
        set(moreName, pages > 1);
        set(pageName, fmt::format("{} de {}", page + 1, pages));
    }

    void showPalette() {
        showThumbnails(palette_, maxPalette, paletteIndex_, palettePage_, "pal", "palSel", "palPagina", "palMais");
        set("temPaleta", palette_.size() > 0);
        if (paletteIndex_ >= 0) {
            const painter::PaletteMaterial& material = palette_.at(paletteIndex_);
            const lkz::asset::ImageData& image = material.mips.front();
            set("matNome", fmt::format("{} · {}×{} px", material.name, image.width, image.height));
            set("matEscala", lengthText(material.metersPerRepeat));
        } else {
            set("matNome", std::string{"Nenhum material na paleta"});
            set("matEscala", std::string{"-"});
        }
    }

    void changeScale(int step) {
        if (paletteIndex_ < 0) {
            return;
        }
        const float meters = palette_.at(paletteIndex_).metersPerRepeat * (step > 0 ? 1.25f : 0.8f);
        std::string error;
        if (palette_.setMetersPerRepeat(paletteIndex_, meters, error)) {
            doc_.paletteChanged();
            say(fmt::format("Escala de {}: uma repetição a cada {} (gravado no .json da paleta)",
                            palette_.at(paletteIndex_).name, lengthText(palette_.at(paletteIndex_).metersPerRepeat)));
        } else {
            say(error);
        }
        showPalette();
    }

    // --- Balde ---

    void showBucket() {
        set("selecao", selection_ == painter::Selection::Triangle ? "triangulo"
                       : selection_ == painter::Selection::Wall   ? "parede"
                       : selection_ == painter::Selection::Plane  ? "plano"
                                                                  : "material");
        set("baldeDestino", targetName(target_, restore_));
        std::string help;
        if (restore_) {
            help = "Volta as faces como estavam ao abrir (destino e pintura).";
        } else if (target_ == SurfaceTarget::Shared) {
            help = "Textura da paleta repetida em metros, nítida e leve: paredes, telhado, calçada. "
                   "Fica fora do modelo (um arquivo para todos os prédios).";
        } else if (target_ == SurfaceTarget::Color) {
            help = "Cor lisa, sem textura: ferragens e detalhes pequenos.";
        } else {
            help = "Pinta na textura do próprio modelo: para detalhes (pichação, sujeira, letreiro).";
        }
        set("destinoAjuda", help);
    }

    void setTarget(std::string_view name) {
        restore_ = name == "original";
        if (name == "compartilhado") {
            target_ = SurfaceTarget::Shared;
            if (paint_ != Paint::Material) {
                setPaint(Paint::Material);
            }
        } else if (name == "cor") {
            target_ = SurfaceTarget::Color;
            if (paint_ != Paint::Color) {
                setPaint(Paint::Color);
            }
        } else if (name == "unico") {
            target_ = SurfaceTarget::Unique;
        }
        showBucket();
    }

    void setHighlight(std::vector<int> faces) {
        if (faces != highlight_) {
            highlight_ = std::move(faces);
            highlightChanged_ = true;
        }
    }

    void applyBucket() {
        if (highlight_.empty()) {
            return;
        }
        painter::Fill fill;
        fill.restore = restore_;
        fill.target = target_;
        fill.material = paint_ == Paint::Material ? paletteIndex_ : -1;
        fill.color = brush_.color;
        fill.tint = fill.material >= 0 && tint_;
        if (!restore_ && target_ == SurfaceTarget::Shared && fill.material < 0) {
            say("Compartilhado precisa de um material da paleta");
            return;
        }
        if (!restore_ && target_ == SurfaceTarget::Unique && !allowOverlap_) {
            float overlap = 0.0f;
            for (const int face : highlight_) {
                overlap = std::max(overlap, doc_.faceOverlap(face));
            }
            if (overlap > 0.01f) {
                askUniqueUv(overlap);
                return;
            }
        }
        int withoutCanvas = 0;
        const int applied = doc_.fill(highlight_, fill, &withoutCanvas);
        const std::string what = restore_ ? std::string{"como ao abrir"}
                                 : fill.material >= 0 ? palette_.at(fill.material).name + (fill.tint ? " " + hex(brush_.color) : "")
                                                      : hex(brush_.color);
        std::string text = fmt::format("Balde: {} faces ({:.1f} m²) → {} ({})", applied, doc_.area(highlight_),
                                       restore_ ? "Original"
                                       : target_ == SurfaceTarget::Shared ? "Compartilhado"
                                       : target_ == SurfaceTarget::Color  ? "Só cor"
                                                                          : "Único",
                                       what);
        if (withoutCanvas > 0) {
            text += fmt::format(" · {} sem tela (sem UV): ficam assim até o UV único", withoutCanvas);
        }
        say(text);
    }

    // --- Carimbo (decalques) ---

    /// Um carimbo sendo posto: o retângulo no plano da face clicada, antes de confirmar.
    struct PendingStamp {
        // Follow: colado (Ctrl+V), segue o mouse de parede em parede até o clique.
        enum class Drag : std::uint8_t { None, Create, Move, Resize, Follow };
        painter::DecalPlacement placement;
        glm::vec3 normal{0.0f, 0.0f, 1.0f};
        float plane = 0.0f; // dot(normal, ponto do plano)
        painter::SurfaceAxes axes;
        Drag drag = Drag::Create;
        glm::vec2 anchor{0.0f}; // Create/Resize: o canto que fica; Move: onde pegou, relativo à origem
        glm::vec2 pressed{0.0f}; // o mouse ao começar (em pixels)
        int grip = -1;           // Resize: o canto pego (gripAt)
        bool lifted = false;     // um carimbo já posto, tirado da parede pela Mão (Document::lift)
        painter::DecalPlacement liftedFrom; // como ele estava
    };

    /// Mão: apertou num carimbo (ou no canto do selecionado), ainda sem arrastar.
    struct HandPress {
        int id = -1;
        int grip = 4; // 4 = mover; 0..3 = o canto
        glm::vec2 mouse{0.0f};
    };

    void setPlane(PendingStamp& stamp, int face) const {
        stamp.placement.face = face;
        stamp.normal = doc_.faceNormal(face);
        stamp.plane = glm::dot(stamp.normal, doc_.faceCenter(face));
        stamp.axes = painter::surfaceAxes(stamp.normal);
    }

    void chooseDecal(int index) {
        if (index < 0 || index >= std::min(decals_.size(), maxDecals)) {
            return;
        }
        decalIndex_ = index;
        decalPage_ = index / paletteSlots;
        if (pending_) {
            pending_->placement.decal = index;
            // Mesma largura, altura pela proporção da imagem nova.
            const glm::vec2 center = pending_->placement.origin + pending_->placement.size * 0.5f;
            const float width = pending_->placement.size.x;
            pending_->placement.size = {width, width * stampAspect(*pending_)};
            pending_->placement.origin = center - pending_->placement.size * 0.5f;
            previewChanged_ = true;
        }
        showDecals();
    }

    void showDecals() {
        showThumbnails(decals_, maxDecals, decalIndex_, decalPage_, "dec", "decSel", "decPagina", "decMais");
        set("temDecalques", decals_.size() > 0);
        if (decalIndex_ >= 0) {
            const painter::PaletteMaterial& decal = decals_.at(decalIndex_);
            const lkz::asset::ImageData& image = decal.mips.front();
            set("decNome", fmt::format("{} · {}×{} px{}", decal.name, image.width, image.height,
                                       decal.transparent ? " · com transparência" : ""));
        } else {
            set("decNome", std::string{"Nenhum decalque: importe uma imagem"});
        }
    }

    /// Altura / largura do retângulo, pela imagem e pelo giro.
    [[nodiscard]] float stampAspect(const PendingStamp& stamp) const {
        const lkz::asset::ImageData& image = decals_.at(stamp.placement.decal).mips.front();
        const float aspect = static_cast<float>(image.height) / static_cast<float>(std::max(image.width, 1));
        return stamp.placement.quarterTurns % 2 == 0 ? aspect : 1.0f / aspect;
    }

    /// O ponto do plano do carimbo sob o mouse, nos eixos dele.
    [[nodiscard]] std::optional<glm::vec2> onPlane(const PendingStamp& stamp, const glm::vec2& mouse,
                                                   const glm::vec2& viewport) const {
        const glm::vec3 eye = camera_.eye();
        const glm::vec3 direction = camera_.rayDirection(mouse, viewport);
        const float facing = glm::dot(stamp.normal, direction);
        if (std::abs(facing) < 1e-6f) {
            return std::nullopt;
        }
        const float t = (stamp.plane - glm::dot(stamp.normal, eye)) / facing;
        if (t <= 0.0f) {
            return std::nullopt;
        }
        const glm::vec3 point = eye + direction * t;
        return glm::vec2{glm::dot(point, stamp.axes.u), glm::dot(point, stamp.axes.v)};
    }

    /// Retângulo com um canto em `fixed`, crescendo até `point`, na proporção da imagem
    /// (`free`: largura e altura soltas).
    void sizeFrom(PendingStamp& stamp, const glm::vec2& fixed, const glm::vec2& point, bool free = false) const {
        const float aspect = stampAspect(stamp);
        const glm::vec2 d = point - fixed;
        glm::vec2 size = glm::max(glm::abs(d), glm::vec2{0.01f});
        if (!free) {
            const float width = std::max(std::abs(d.x), std::abs(d.y) / aspect);
            size = {width, width * aspect};
        }
        stamp.placement.size = size;
        stamp.placement.origin = {d.x >= 0.0f ? fixed.x : fixed.x - size.x, d.y >= 0.0f ? fixed.y : fixed.y - size.y};
    }

    /// Onde o ponto cai no carimbo pendente: num canto (0 = o de menor u e v, 1, 2, 3 em
    /// volta), dentro (4) ou fora (-1).
    [[nodiscard]] int gripAt(const PendingStamp& stamp, const std::optional<glm::vec2>& point) const {
        if (!point || stamp.drag == PendingStamp::Drag::Follow) {
            return -1;
        }
        const glm::vec2 lo = stamp.placement.origin;
        const glm::vec2 hi = lo + stamp.placement.size;
        const float grab = 0.2f * std::min(stamp.placement.size.x, stamp.placement.size.y);
        if (point->x <= lo.x - grab || point->x >= hi.x + grab || point->y <= lo.y - grab || point->y >= hi.y + grab) {
            return -1;
        }
        const glm::vec2 corners[4] = {lo, {hi.x, lo.y}, hi, {lo.x, hi.y}};
        for (int i = 0; i < 4; ++i) {
            if (glm::length(*point - corners[i]) < grab) {
                return i;
            }
        }
        return 4;
    }

    /// Clique no carimbo pendente: dentro, mover; num canto, mudar o tamanho. false = fora.
    bool grabPending(const glm::vec2& mouse, const glm::vec2& viewport) {
        const auto point = onPlane(*pending_, mouse, viewport);
        const int grip = gripAt(*pending_, point);
        if (grip < 0) {
            return false;
        }
        const glm::vec2 lo = pending_->placement.origin;
        const glm::vec2 hi = lo + pending_->placement.size;
        const glm::vec2 corners[4] = {lo, {hi.x, lo.y}, hi, {lo.x, hi.y}};
        pending_->grip = grip;
        if (grip < 4) {
            pending_->drag = PendingStamp::Drag::Resize;
            pending_->anchor = corners[(grip + 2) % 4];
        } else {
            pending_->drag = PendingStamp::Drag::Move;
            pending_->anchor = *point - lo;
        }
        previewChanged_ = true;
        return true;
    }

    void dragPending(const lkz::InputState& input, const glm::vec2& mouse, const glm::vec2& viewport) {
        if (const auto point = onPlane(*pending_, mouse, viewport)) {
            if (pending_->drag == PendingStamp::Drag::Move) {
                pending_->placement.origin = *point - pending_->anchor;
            } else {
                const bool shift = input.down(lkz::Key::LeftShift) || input.down(lkz::Key::RightShift);
                sizeFrom(*pending_, pending_->anchor, *point, shift && pending_->drag == PendingStamp::Drag::Resize);
            }
            previewChanged_ = true;
            showStampInfo();
        }
    }

    /// O ponteiro do mouse: alças do carimbo pendente; a mão sobre um carimbo que dá para pegar.
    [[nodiscard]] lkz::MouseCursor cursorFor(const lkz::InputState& input, const glm::vec2& mouse, const glm::vec2& viewport) const {
        using Drag = PendingStamp::Drag;
        if (input.pointerOverUi) {
            return lkz::MouseCursor::Default;
        }
        const auto corner = [](int grip) {
            return grip == 0 || grip == 2 ? lkz::MouseCursor::ResizeNwse : lkz::MouseCursor::ResizeNesw;
        };
        if (pending_) {
            if (pending_->drag == Drag::Move || pending_->drag == Drag::Follow) {
                return lkz::MouseCursor::Move;
            }
            if (pending_->drag == Drag::Resize) {
                return corner(pending_->grip);
            }
            if (pending_->drag == Drag::None) {
                const int grip = gripAt(*pending_, onPlane(*pending_, mouse, viewport));
                if (grip >= 0) {
                    return grip == 4 ? lkz::MouseCursor::Move : corner(grip);
                }
            }
        }
        if (tool_ == Tool::Sculpt) {
            if (sculptDrag_ == SculptDrag::Pull) {
                return lkz::MouseCursor::Move;
            }
            if (hoverFace_ >= 0 && (doc_.sculpting() || std::binary_search(sculptFaces_.begin(), sculptFaces_.end(), hoverFace_))) {
                return lkz::MouseCursor::Hand; // dá para puxar
            }
            return sculptPick_ == SculptPick::Rectangle ? lkz::MouseCursor::Crosshair : lkz::MouseCursor::Default;
        }
        if (tool_ == Tool::Hand && !pending_ && primaryStamp() >= 0) {
            if (const auto placed = doc_.stampById(primaryStamp())) {
                PendingStamp probe;
                probe.placement = *placed;
                probe.drag = Drag::None;
                setPlane(probe, placed->face);
                if (const int grip = gripAt(probe, onPlane(probe, mouse, viewport)); grip >= 0 && grip < 4) {
                    return corner(grip);
                }
            }
        }
        if (tool_ == Tool::Hand && hoverFace_ >= 0 && doc_.stampAt(hoverFace_)) {
            return lkz::MouseCursor::Hand;
        }
        return lkz::MouseCursor::Default;
    }

    void handleStamp(const lkz::InputState& input, const std::optional<lkz::asset::RayHit>& hit, const glm::vec2& mouse,
                     const glm::vec2& viewport) {
        using Drag = PendingStamp::Drag;
        if (input.pressed(lkz::MouseButton::Left) && !input.pointerOverUi) {
            if (pending_) {
                // Dentro: mover; num canto: mudar o tamanho; fora: confirmar.
                if (!grabPending(mouse, viewport)) {
                    commitStamp();
                }
                return;
            }
            if (!hit) {
                return;
            }
            if (decalIndex_ < 0) {
                say("Sem decalques: use Importar imagem… para trazer uma janela, porta ou letreiro");
                return;
            }
            const int face = doc_.faceIndex(hit->where);
            if (face < 0) {
                return;
            }
            PendingStamp stamp;
            stamp.placement.decal = decalIndex_;
            stamp.placement.size = glm::vec2{0.0f};
            stamp.placement.tintMode = stampTint_;
            stamp.placement.tint = stampTint_ == painter::DecalTint::None ? glm::vec3{1.0f} : brush_.color;
            stamp.placement.depth = stampDepth_;
            setPlane(stamp, face);
            stamp.anchor = {glm::dot(hit->position, stamp.axes.u), glm::dot(hit->position, stamp.axes.v)};
            stamp.placement.origin = stamp.anchor;
            stamp.pressed = mouse;
            pending_ = stamp;
            previewChanged_ = true;
            return;
        }
        if (!pending_ || pending_->drag == Drag::None) {
            return;
        }
        if (input.down(lkz::MouseButton::Left)) {
            dragPending(input, mouse, viewport);
            return;
        }
        // Soltou. Um clique sem arrastar: o tamanho do último carimbo deste decalque (ou o do
        // .json), centrado no clique.
        if (pending_->drag == Drag::Create && glm::length(mouse - pending_->pressed) < 4.0f) {
            const painter::PaletteMaterial& decal = decals_.at(pending_->placement.decal);
            const auto last = lastSize_.find(pending_->placement.decal);
            const float aspect = stampAspect(*pending_);
            glm::vec2 size = last != lastSize_.end() ? last->second : glm::vec2{decal.widthMeters, decal.widthMeters * aspect};
            if (pending_->placement.quarterTurns % 2 != 0) {
                size = {size.y, size.x};
            }
            pending_->placement.size = size;
            pending_->placement.origin = pending_->anchor - size * 0.5f;
        }
        pending_->drag = Drag::None;
        previewChanged_ = true;
        showStampInfo();
    }

    /// Mão: seleciona carimbos postos (Shift soma), e arrastar move ou, num canto do
    /// selecionado, muda o tamanho; soltar confirma na hora. Colando (Ctrl+V), o carimbo segue
    /// o mouse e cada clique põe um, até o Esc.
    void handleHand(const lkz::InputState& input, std::optional<lkz::asset::RayHit> hit, const glm::vec2& mouse,
                    const glm::vec2& viewport) {
        using Drag = PendingStamp::Drag;
        const bool click = input.pressed(lkz::MouseButton::Left) && !input.pointerOverUi;
        const bool shift = input.down(lkz::Key::LeftShift) || input.down(lkz::Key::RightShift);
        if (pending_ && pending_->drag == Drag::Follow) {
            if (hit && !input.pointerOverUi) {
                if (const int face = doc_.faceIndex(hit->where); face >= 0) {
                    setPlane(*pending_, face);
                    const glm::vec2 at{glm::dot(hit->position, pending_->axes.u), glm::dot(hit->position, pending_->axes.v)};
                    pending_->placement.origin = at - pending_->placement.size * 0.5f;
                    previewChanged_ = true;
                }
            }
            if (click && pending_->placement.face >= 0) {
                painter::DecalPlacement next = pending_->placement;
                pending_->drag = Drag::None;
                commitStamp();
                next.face = -1;
                PendingStamp follow;
                follow.placement = next;
                follow.drag = Drag::Follow;
                pending_ = follow;
                previewChanged_ = true;
                say("Colado. Clique para pôr outro igual; Esc para parar");
            }
            return;
        }
        if (click) {
            if (pending_) {
                commitStamp();
                hit = hitAt(mouse, viewport); // a malha mudou
                hoverFace_ = hit ? doc_.faceIndex(hit->where) : -1;
            }
            // Um canto do selecionado: mudar o tamanho.
            if (!shift) {
                if (const int primary = primaryStamp(); primary >= 0) {
                    if (const auto placed = doc_.stampById(primary)) {
                        PendingStamp probe;
                        probe.placement = *placed;
                        probe.drag = Drag::None;
                        setPlane(probe, placed->face);
                        if (const int grip = gripAt(probe, onPlane(probe, mouse, viewport)); grip >= 0 && grip < 4) {
                            handPress_ = HandPress{primary, grip, mouse};
                            return;
                        }
                    }
                }
            }
            const int id = hoverFace_ >= 0 ? doc_.stampId(hoverFace_) : -1;
            if (id < 0) {
                if (hoverFace_ >= 0 && doc_.face(hoverFace_).target == SurfaceTarget::Decal) {
                    say("Este decalque é fixo: veio de um .glb exportado antes de o Pintor gravar os carimbos (ou de fora dele)");
                }
                if (!shift) {
                    selectStamps({});
                }
                return;
            }
            if (shift) {
                std::vector<int> ids = selected_;
                if (const auto it = std::find(ids.begin(), ids.end(), id); it != ids.end()) {
                    ids.erase(it);
                } else {
                    ids.push_back(id);
                }
                selectStamps(std::move(ids));
                return;
            }
            if (std::find(selected_.begin(), selected_.end(), id) == selected_.end()) {
                selectStamps({id});
            } else {
                std::vector<int> ids = selected_; // o clicado passa a ser o principal
                ids.erase(std::find(ids.begin(), ids.end(), id));
                ids.push_back(id);
                selectStamps(std::move(ids));
            }
            handPress_ = HandPress{id, 4, mouse};
            return;
        }
        // Apertou num carimbo: só tira da parede quando o mouse andar (clicar só seleciona).
        if (handPress_) {
            if (!input.down(lkz::MouseButton::Left)) {
                handPress_.reset();
                return;
            }
            if (glm::length(mouse - handPress_->mouse) < 4.0f) {
                return;
            }
            const HandPress press = *handPress_;
            handPress_.reset();
            if (!startHandDrag(press, viewport)) {
                return;
            }
        }
        if (!pending_ || pending_->drag == Drag::None) {
            return;
        }
        if (input.down(lkz::MouseButton::Left)) {
            dragPending(input, mouse, viewport);
            return;
        }
        pending_->drag = Drag::None;
        commitStamp(); // soltou: fica onde está
    }

    /// Tira o carimbo apertado da parede e começa a arrastá-lo (grip 4: mover; 0..3: o canto).
    bool startHandDrag(const HandPress& press, const glm::vec2& viewport) {
        using Drag = PendingStamp::Drag;
        const auto placed0 = doc_.stampById(press.id);
        if (!placed0) {
            return false;
        }
        const auto placed = doc_.lift(placed0->face);
        if (!placed || placed->face < 0) {
            doc_.endLift();
            return false;
        }
        PendingStamp stamp;
        stamp.placement = *placed;
        stamp.lifted = true;
        stamp.liftedFrom = *placed;
        setPlane(stamp, placed->face);
        const glm::vec2 lo = placed->origin;
        const glm::vec2 hi = lo + placed->size;
        const glm::vec2 corners[4] = {lo, {hi.x, lo.y}, hi, {lo.x, hi.y}};
        stamp.grip = press.grip;
        if (press.grip < 4) {
            stamp.drag = Drag::Resize;
            stamp.anchor = corners[(press.grip + 2) % 4];
        } else {
            stamp.drag = Drag::Move;
            const auto point = onPlane(stamp, press.mouse, viewport);
            stamp.anchor = point ? *point - lo : placed->size * 0.5f;
        }
        pending_ = stamp;
        if (placed->decal >= 0 && placed->decal < static_cast<int>(decals_.size())) {
            decalIndex_ = placed->decal;
            showDecals();
        }
        previewChanged_ = true;
        showStampInfo();
        return true;
    }

    // --- Seleção de carimbos postos (Carimbo e Mão) ---

    /// O principal (o último clicado), ou -1.
    [[nodiscard]] int primaryStamp() const { return selected_.empty() ? -1 : selected_.back(); }

    void selectStamps(std::vector<int> ids) {
        selected_ = std::move(ids);
        if (const int primary = primaryStamp(); primary >= 0) {
            if (const auto placed = doc_.stampById(primary)) {
                adoptStamp(*placed); // a cor e a profundidade dele aparecem no painel
            }
        }
        showStampInfo();
    }

    /// Tira da seleção os que não existem mais (desfazer, apagar).
    void pruneSelection() {
        const auto gone = std::remove_if(selected_.begin(), selected_.end(), [&](int id) { return !doc_.stampById(id); });
        if (gone != selected_.end()) {
            selected_.erase(gone, selected_.end());
            showStampInfo();
        }
    }

    [[nodiscard]] std::vector<int> selectedFaces() const {
        std::vector<int> faces;
        for (const int id : selected_) {
            const std::vector<int> part = doc_.stampFacesById(id);
            faces.insert(faces.end(), part.begin(), part.end());
        }
        return faces;
    }

    /// Muda os carimbos selecionados na parede, num passo só de desfazer.
    template <class Change>
    void restyleSelected(Change&& change, std::string_view what) {
        pruneSelection();
        std::vector<std::pair<int, painter::DecalPlacement>> changes;
        for (const int id : selected_) {
            if (const auto placed = doc_.stampById(id)) {
                painter::DecalPlacement next = *placed;
                change(next);
                if (!next.sameAs(*placed)) {
                    changes.emplace_back(id, next);
                }
            }
        }
        if (changes.empty()) {
            return;
        }
        const std::vector<int> ids = doc_.replaceStamps(changes);
        std::vector<int> kept;
        for (const int id : selected_) {
            int now = id;
            for (std::size_t i = 0; i < changes.size(); ++i) {
                if (changes[i].first == id) {
                    now = ids[i];
                }
            }
            if (now >= 0) {
                kept.push_back(now);
            }
        }
        selected_ = std::move(kept);
        showStampInfo();
        say(fmt::format("{}: {} carimbo{} (Ctrl+Z desfaz)", what, changes.size(), changes.size() == 1 ? "" : "s"));
    }

    void copyStamp() {
        std::optional<painter::DecalPlacement> source;
        if (pending_ && pending_->drag != PendingStamp::Drag::Follow && pending_->placement.size.x > 1e-4f) {
            source = pending_->placement;
        } else if (primaryStamp() >= 0) {
            source = doc_.stampById(primaryStamp());
        } else if (hoverFace_ >= 0) {
            source = doc_.stampAt(hoverFace_);
        }
        if (!source || source->decal < 0 || source->decal >= static_cast<int>(decals_.size())) {
            say("Nada para copiar: pegue um carimbo com a Mão (H), ou pare o mouse sobre ele");
            return;
        }
        clipboard_ = *source;
        say(fmt::format("Copiado: {} ({} × {}{}) · Ctrl+V cola, com a cor e a profundidade", decals_.at(source->decal).name,
                        lengthText(source->size.x), lengthText(source->size.y),
                        std::abs(source->depth) > 1e-4f ? ", " + depthText(source->depth) : std::string{}));
    }

    void pasteStamp() {
        if (!clipboard_ || clipboard_->decal >= static_cast<int>(decals_.size())) {
            say("Copie um carimbo antes (Ctrl+C)");
            return;
        }
        commitStamp();
        setTool(Tool::Hand);
        PendingStamp stamp;
        stamp.placement = *clipboard_;
        stamp.placement.face = -1;
        stamp.drag = PendingStamp::Drag::Follow;
        pending_ = stamp;
        adoptStamp(stamp.placement);
        previewChanged_ = true;
        showStampInfo();
        say("Colando: leve o mouse até a parede; cada clique põe um (Esc para)");
    }

    void deleteStamp() {
        if (!pending_ && !selected_.empty()) {
            pruneSelection();
            std::vector<std::pair<int, painter::DecalPlacement>> changes;
            for (const int id : selected_) {
                painter::DecalPlacement gone;
                gone.decal = -1;
                changes.emplace_back(id, gone);
            }
            doc_.replaceStamps(changes);
            say(fmt::format("Apagado{}: {} carimbo{} (Ctrl+Z volta)", changes.size() == 1 ? "" : "s", changes.size(),
                            changes.size() == 1 ? "" : "s"));
            selected_.clear();
            showStampInfo();
            return;
        }
        if (pending_ && pending_->lifted) {
            pending_.reset();
            doc_.endLift();
            previewChanged_ = true;
            showStampInfo();
            say("Carimbo apagado (Ctrl+Z volta)");
        } else if (pending_) {
            cancelStamp();
        } else if (tool_ == Tool::Hand && hoverFace_ >= 0 && doc_.lift(hoverFace_)) {
            doc_.endLift();
            say("Carimbo apagado (Ctrl+Z volta)");
        }
    }

    /// A cor da paleta mudou: vale para os carimbos selecionados tingidos.
    void stampColorChanged() {
        if (pending_ || selected_.empty() || stampTint_ == painter::DecalTint::None ||
            (tool_ != Tool::Stamp && tool_ != Tool::Hand)) {
            return;
        }
        const glm::vec3 tint = brush_.color;
        const painter::DecalTint mode = stampTint_;
        restyleSelected(
            [&](painter::DecalPlacement& placement) {
                placement.tintMode = mode;
                placement.tint = tint;
            },
            "Cor");
    }

    /// Cor dos carimbos novos e do pendente (ou dos selecionados).
    void setStampTint(painter::DecalTint mode) {
        stampTint_ = mode;
        const glm::vec3 tint = mode == painter::DecalTint::None ? glm::vec3{1.0f} : brush_.color;
        if (pending_) {
            pending_->placement.tintMode = mode;
            pending_->placement.tint = tint;
            previewChanged_ = true;
        } else {
            restyleSelected(
                [&](painter::DecalPlacement& placement) {
                    placement.tintMode = mode;
                    placement.tint = tint;
                },
                "Cor");
        }
        showStampOptions();
        showStampInfo();
    }

    /// Profundidade dos carimbos novos e do pendente.
    void setStampDepth(float depth) {
        stampDepth_ = std::clamp(std::round(depth * 100.0f) / 100.0f, -2.0f, 2.0f);
        if (pending_) {
            pending_->placement.depth = stampDepth_;
            previewChanged_ = true;
        } else {
            const float value = stampDepth_;
            restyleSelected([&](painter::DecalPlacement& placement) { placement.depth = value; }, "Profundidade");
        }
        showStampOptions();
        showStampInfo();
    }

    /// A cor e a profundidade de um carimbo pego ou colado passam a ser as atuais.
    void adoptStamp(const painter::DecalPlacement& placement) {
        stampTint_ = placement.tintMode;
        stampDepth_ = placement.depth;
        if (placement.tintMode != painter::DecalTint::None) {
            setColor(placement.tint);
        }
        showStampOptions();
    }

    [[nodiscard]] static std::string depthText(float depth) {
        if (std::abs(depth) < 1e-4f) {
            return "0 (plano)";
        }
        return fmt::format("{:+.0f} cm ({})", depth * 100.0f, depth < 0.0f ? "afunda" : "salta");
    }

    void showStampOptions() {
        set("carimboCor", stampTint_ == painter::DecalTint::Multiply    ? "multiplicar"
                          : stampTint_ == painter::DecalTint::WhiteOnly ? "branco"
                                                                        : "original");
        set("profundidade", depthText(stampDepth_));
    }

    void showStampInfo() {
        const bool adjusting = pending_ && pending_->drag == PendingStamp::Drag::None;
        set("carimbando", adjusting);
        set("selecionado", !pending_ && !selected_.empty());
        if (!pending_ && !selected_.empty()) {
            if (const auto placed = doc_.stampById(primaryStamp())) {
                set("carimboInfo", fmt::format("{}{} × {} · giro {}°{}",
                                               selected_.size() > 1 ? fmt::format("{} selecionados · ", selected_.size()) : "",
                                               lengthText(placed->size.x), lengthText(placed->size.y),
                                               placed->quarterTurns * 90,
                                               std::abs(placed->depth) > 1e-4f ? " · " + depthText(placed->depth)
                                                                               : std::string{}));
                return;
            }
        }
        set("carimboInfo", pending_ ? fmt::format("{} × {} · giro {}°{}", lengthText(pending_->placement.size.x),
                                                  lengthText(pending_->placement.size.y), pending_->placement.quarterTurns * 90,
                                                  std::abs(pending_->placement.depth) > 1e-4f
                                                      ? " · " + depthText(pending_->placement.depth)
                                                      : std::string{})
                                    : std::string{});
    }

    void rotateStamp() {
        if (!pending_) {
            restyleSelected(
                [](painter::DecalPlacement& placement) {
                    const glm::vec2 center = placement.origin + placement.size * 0.5f;
                    placement.quarterTurns = (placement.quarterTurns + 1) % 4;
                    placement.size = {placement.size.y, placement.size.x};
                    placement.origin = center - placement.size * 0.5f;
                },
                "Girado");
            return;
        }
        painter::DecalPlacement& placement = pending_->placement;
        const glm::vec2 center = placement.origin + placement.size * 0.5f;
        placement.quarterTurns = (placement.quarterTurns + 1) % 4;
        placement.size = {placement.size.y, placement.size.x};
        placement.origin = center - placement.size * 0.5f;
        previewChanged_ = true;
        showStampInfo();
    }

    void commitStamp() {
        if (!pending_) {
            return;
        }
        if (pending_->drag == PendingStamp::Drag::Follow) {
            cancelStamp(); // colado sem soltar
            return;
        }
        const painter::DecalPlacement placement = pending_->placement;
        const bool lifted = pending_->lifted;
        const painter::DecalPlacement from = pending_->liftedFrom;
        pending_.reset();
        previewChanged_ = true;
        showStampInfo();
        if (lifted && placement.sameAs(from)) {
            doc_.cancelLift(); // não mudou: volta como estava, sem cortar de novo
            return;
        }
        const int created = doc_.stamp(placement);
        doc_.endLift();
        if (created > 0) {
            // O posto fica selecionado: cor, profundidade e giro mudam ele na hora.
            const int id = doc_.lastStampId();
            if (lifted && !selected_.empty()) {
                selected_.back() = id;
            } else {
                selected_ = {id};
            }
        } else {
            pruneSelection();
        }
        if (created <= 0) {
            say(lifted ? "O carimbo saiu da parede: ficou apagado (Ctrl+Z volta)"
                       : "O retângulo não pegou nenhuma face do plano: nada foi carimbado");
            return;
        }
        // Próximo clique sem arrastar: o mesmo tamanho (como a imagem fica, sem giro).
        lastSize_[placement.decal] = placement.quarterTurns % 2 == 0 ? placement.size
                                                                     : glm::vec2{placement.size.y, placement.size.x};
        say(fmt::format("Carimbado: {} ({} × {}) · {} triângulos", decals_.at(placement.decal).name,
                        lengthText(placement.size.x), lengthText(placement.size.y), created));
    }

    /// Desiste do pendente: um novo some; um pego pela Mão volta como estava.
    void cancelStamp() {
        if (pending_) {
            if (pending_->lifted) {
                doc_.cancelLift();
            }
            pending_.reset();
            previewChanged_ = true;
        }
        showStampInfo();
    }

    void showPreview() {
        const std::uint32_t id = meshId(previewSlot, true, false);
        auto& registry = world_.registry();
        if (sculptRect_ && sculptRect_->placement.size.x > 1e-4f && sculptRect_->placement.size.y > 1e-4f) {
            if (previewDecal_ != -2) { // o retângulo do Esculpir: branco, realçado
                world_.uploadTexture(textureId(previewSlot), lkz::TextureImage{1, 1, {255, 255, 255, 255}});
                previewDecal_ = -2;
                previewWhiteOnly_ = glm::vec3{-1.0f};
            }
            world_.uploadMesh(id, doc_.decalQuad(sculptRect_->placement, 0.01f));
            registry.emplace_or_replace<lkz::RenderComponent>(nodeFor(id).handle(), id, 0u, highlightTint);
            return;
        }
        if (!pending_ || pending_->placement.face < 0 || pending_->placement.size.x <= 1e-4f ||
            pending_->placement.size.y <= 1e-4f) {
            if (nodes_.contains(id)) {
                registry.remove<lkz::RenderComponent>(nodes_[id]->handle());
            }
            return;
        }
        const painter::DecalPlacement& placement = pending_->placement;
        // Só no branco: a imagem tingida; multiplicada: a cor na instância (como no jogo).
        const bool whiteOnly = placement.tintMode == painter::DecalTint::WhiteOnly && placement.tint != glm::vec3{1.0f};
        const glm::vec3 whiteOnlyKey = whiteOnly ? placement.tint : glm::vec3{-1.0f};
        if (previewDecal_ != placement.decal || previewWhiteOnly_ != whiteOnlyKey) {
            const lkz::asset::ImageData& source = decals_.at(placement.decal).mips.front();
            const lkz::asset::ImageData image =
                whiteOnly ? painter::tintedImage(source, painter::DecalTint::WhiteOnly, placement.tint) : source;
            world_.uploadTexture(textureId(previewSlot), lkz::TextureImage{image.width, image.height, image.rgba});
            previewDecal_ = placement.decal;
            previewWhiteOnly_ = whiteOnlyKey;
        }
        // Saltado: a prévia já na frente, onde vai ficar; afundado, rente à parede.
        world_.uploadMesh(id, doc_.decalQuad(placement, std::max(placement.depth, 0.0f) + 0.01f));
        // Enquanto arrasta, um pouco amarelado; parado, a imagem como vai ficar.
        glm::vec4 color = pending_->drag == PendingStamp::Drag::None ? glm::vec4{1.0f} : highlightTint;
        if (placement.tintMode == painter::DecalTint::Multiply) {
            color *= glm::vec4{placement.tint, 1.0f};
        }
        registry.emplace_or_replace<lkz::RenderComponent>(nodeFor(id).handle(), id, 0u, color);
    }

    // --- Esculpir ---

    [[nodiscard]] painter::Selection sculptSelection() const {
        switch (sculptPick_) {
        case SculptPick::Triangle: return painter::Selection::Triangle;
        case SculptPick::Plane: return painter::Selection::Plane;
        default: return painter::Selection::Wall;
        }
    }

    /// A seleção vale só na malha em que foi feita (outra mudança na malha a desfaz).
    void checkSculptSelection() {
        if (!doc_.sculpting() && doc_.geometryVersion() != sculptVersion_ && !sculptFaces_.empty()) {
            sculptFaces_.clear();
            showSculpt();
        }
    }

    void setSculptFaces(std::vector<int> faces) {
        std::sort(faces.begin(), faces.end());
        faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
        sculptFaces_ = std::move(faces);
        sculptVersion_ = doc_.geometryVersion();
        showSculpt();
    }

    void showSculpt() {
        set("escSel", sculptPick_ == SculptPick::Triangle   ? "triangulo"
                      : sculptPick_ == SculptPick::Plane     ? "plano"
                      : sculptPick_ == SculptPick::Rectangle ? "retangulo"
                                                             : "parede");
        set("escModo", sculptMode_ == painter::SculptMode::Move ? "mover" : "puxar");
        const bool open = doc_.sculpting();
        const float distance = doc_.sculptDistance();
        const std::vector<int> faces = open ? doc_.sculptSelection() : sculptFaces_;
        set("escAberto", open);
        set("escDist", open ? fmt::format("{:+.0f} cm", distance * 100.0f) : std::string{"0"});
        set("escInfo", faces.empty() ? std::string{"Nada selecionado"}
                                     : fmt::format("{} triângulos · {:.2f} m²", faces.size(), doc_.area(faces)));
    }

    /// Abre a escultura da seleção (ou continua a aberta).
    bool openSculpt() {
        if (doc_.sculpting()) {
            return true;
        }
        checkSculptSelection();
        if (sculptFaces_.empty()) {
            say("Selecione faces antes: clique numa parede (Shift soma), ou desenhe um retângulo");
            return false;
        }
        return doc_.beginSculpt(sculptFaces_, sculptMode_, sculptAxis_);
    }

    void setSculptDistance(float distance) {
        doc_.updateSculpt(distance);
        showSculpt();
    }

    /// Confirma a escultura aberta: um passo de desfazer, e a seleção passa a ser as faces
    /// esculpidas (dá para puxar de novo).
    void finishSculpt() {
        if (!doc_.sculpting()) {
            return;
        }
        const float distance = doc_.sculptDistance();
        std::vector<int> faces = doc_.endSculpt();
        if (faces.empty()) {
            sculptVersion_ = doc_.geometryVersion(); // não andou: a malha voltou, a seleção vale
            showSculpt();
            return;
        }
        setSculptFaces(std::move(faces));
        say(fmt::format("{} {:+.0f} cm (Ctrl+Z desfaz)", sculptMode_ == painter::SculptMode::Move ? "Movido" : "Puxado",
                        distance * 100.0f));
    }

    void cancelSculpt() {
        if (doc_.sculpting()) {
            doc_.cancelSculpt();
            sculptVersion_ = doc_.geometryVersion();
            showSculpt();
            say("Escultura desfeita");
        }
    }

    /// Onde a linha `origin + axis * t` passa mais perto do raio do mouse (t, em metros).
    [[nodiscard]] float axisParam(const glm::vec3& origin, const glm::vec3& axis, const glm::vec2& mouse,
                                  const glm::vec2& viewport) const {
        const glm::vec3 eye = camera_.eye();
        const glm::vec3 direction = camera_.rayDirection(mouse, viewport);
        const glm::vec3 w = origin - eye;
        const float b = glm::dot(axis, direction);
        const float denominator = 1.0f - b * b;
        if (denominator < 1e-4f) {
            return 0.0f; // olhando de frente para a seta: gire a câmera
        }
        return (b * glm::dot(direction, w) - glm::dot(axis, w)) / denominator;
    }

    /// Esculpir: clique seleciona (Shift soma ou tira); arrastar uma face selecionada puxa
    /// ao longo da normal dela (ao vivo; soltar confirma). No modo Retângulo, arrastar numa
    /// parede desenha o retângulo, que é cortado e fica selecionado.
    void handleSculpt(const lkz::InputState& input, const std::optional<lkz::asset::RayHit>& hit, const glm::vec2& mouse,
                      const glm::vec2& viewport) {
        checkSculptSelection();
        const bool shift = input.down(lkz::Key::LeftShift) || input.down(lkz::Key::RightShift);
        if (input.pressed(lkz::MouseButton::Left) && !input.pointerOverUi) {
            if (!hit) {
                return;
            }
            const int face = doc_.faceIndex(hit->where);
            if (face >= 0) {
                sculptPress_ = SculptPress{mouse, face, hit->position, doc_.faceNormal(face)};
            }
            return;
        }
        if (sculptDrag_ == SculptDrag::Pull) {
            if (input.down(lkz::MouseButton::Left)) {
                float distance = sculptStart_ + axisParam(sculptOrigin_, sculptAxis_, mouse, viewport) - sculptGrab_;
                if (!shift) {
                    distance = std::round(distance * 100.0f) / 100.0f; // passos de 1 cm (Shift: livre)
                }
                if (distance != doc_.sculptDistance()) {
                    setSculptDistance(distance);
                }
            } else {
                sculptDrag_ = SculptDrag::None;
                finishSculpt();
            }
            return;
        }
        if (sculptDrag_ == SculptDrag::Rectangle) {
            if (input.down(lkz::MouseButton::Left)) {
                if (const auto point = onPlane(*sculptRect_, mouse, viewport)) {
                    sculptRect_->placement.origin = glm::min(sculptRect_->anchor, *point);
                    sculptRect_->placement.size = glm::abs(*point - sculptRect_->anchor);
                    previewChanged_ = true;
                }
                return;
            }
            const painter::DecalPlacement rect = sculptRect_->placement;
            const glm::vec3 normal = sculptRect_->normal;
            sculptRect_.reset();
            sculptDrag_ = SculptDrag::None;
            previewChanged_ = true;
            const std::vector<int> inside = doc_.cutRectangle(rect.face, rect.origin, rect.origin + rect.size);
            if (inside.empty()) {
                say("O retângulo não pegou nenhuma face");
                return;
            }
            sculptAxis_ = normal;
            setSculptFaces(inside);
            say(fmt::format("Retângulo de {} × {} selecionado: arraste-o, ou use os botões de distância",
                            lengthText(rect.size.x), lengthText(rect.size.y)));
            return;
        }
        if (!sculptPress_) {
            return;
        }
        const SculptPress press = *sculptPress_;
        if (!input.down(lkz::MouseButton::Left)) {
            // Soltou sem arrastar: selecionar.
            sculptPress_.reset();
            finishSculpt();
            checkSculptSelection();
            const auto again = hitAt(press.mouse, viewport); // a malha pode ter mudado
            if (!again) {
                return;
            }
            const int face = doc_.faceIndex(again->where);
            if (face < 0) {
                return;
            }
            if (sculptPick_ == SculptPick::Rectangle) {
                say("Retângulo: arraste na parede para desenhar");
                return;
            }
            std::vector<int> faces = doc_.select(*again, sculptSelection());
            if (shift) {
                const bool all = std::all_of(faces.begin(), faces.end(), [&](int f) {
                    return std::binary_search(sculptFaces_.begin(), sculptFaces_.end(), f);
                });
                std::vector<int> next;
                if (all) { // já estavam: saem
                    std::set_difference(sculptFaces_.begin(), sculptFaces_.end(), faces.begin(), faces.end(), std::back_inserter(next));
                } else {
                    next = sculptFaces_;
                    next.insert(next.end(), faces.begin(), faces.end());
                }
                faces = std::move(next);
            }
            sculptAxis_ = doc_.faceNormal(face);
            setSculptFaces(std::move(faces));
            return;
        }
        if (glm::length(mouse - press.mouse) < 4.0f) {
            return;
        }
        sculptPress_.reset();
        const bool onSelection = doc_.sculpting() || std::binary_search(sculptFaces_.begin(), sculptFaces_.end(), press.face);
        if (onSelection) {
            if (!doc_.sculpting()) {
                sculptAxis_ = press.normal;
                if (!openSculpt()) {
                    return;
                }
            }
            sculptDrag_ = SculptDrag::Pull;
            sculptOrigin_ = press.point;
            sculptStart_ = doc_.sculptDistance();
            sculptGrab_ = axisParam(sculptOrigin_, sculptAxis_, press.mouse, viewport);
            return;
        }
        if (sculptPick_ == SculptPick::Rectangle) {
            finishSculpt();
            const auto again = hitAt(press.mouse, viewport);
            const int face = again ? doc_.faceIndex(again->where) : -1;
            if (face < 0) {
                return;
            }
            PendingStamp rect;
            setPlane(rect, face);
            rect.anchor = {glm::dot(again->position, rect.axes.u), glm::dot(again->position, rect.axes.v)};
            rect.placement.origin = rect.anchor;
            rect.placement.size = glm::vec2{0.0f};
            sculptRect_ = rect;
            sculptDrag_ = SculptDrag::Rectangle;
            previewChanged_ = true;
            return;
        }
        say("Arraste uma face selecionada (realçada) para puxar; clique para selecionar");
    }

    // --- Pincel ---

    void setRadius(float radius) {
        brush_.radius = std::max(radius, 1e-4f);
        showBrush();
    }

    void showBrush() {
        set("tamanho", lengthText(brush_.radius * 2.0f)); // o diâmetro, que é o que se vê
        set("dureza", static_cast<int>(std::lround(brush_.hardness * 100.0f)));
        set("opacidade", static_cast<int>(std::lround(brush_.opacity * 100.0f)));
        set("ferramenta", tool_ == Tool::Brush    ? "pincel"
                          : tool_ == Tool::Bucket ? "balde"
                          : tool_ == Tool::Stamp  ? "carimbo"
                          : tool_ == Tool::Hand   ? "mao"
                          : tool_ == Tool::Sculpt ? "esculpir"
                          : tool_ == Tool::Eraser ? "borracha"
                                                  : "conta-gotas");
    }

    void setTool(Tool tool) {
        // Trocar de ferramenta: um carimbo pego pela Mão fica onde foi posto; um novo some.
        if (tool != tool_ && pending_) {
            if (pending_->lifted) {
                commitStamp();
            } else {
                cancelStamp();
            }
        }
        if (tool != Tool::Sculpt) {
            finishSculpt();
            sculptRect_.reset();
            sculptPress_.reset();
            sculptDrag_ = SculptDrag::None;
            previewChanged_ = true;
        }
        tool_ = tool;
        handPress_.reset();
        if (tool_ != Tool::Stamp && tool_ != Tool::Hand && !selected_.empty()) {
            selected_.clear();
            showStampInfo();
        }
        if (tool_ != Tool::Bucket) {
            setHighlight({});
            set("selecaoInfo", std::string{});
        }
        showBrush();
    }

    void undo() {
        if (doc_.sculpting()) {
            cancelSculpt();
            return;
        }
        if (pending_) {
            const bool lifted = pending_->lifted;
            cancelStamp();
            say(lifted ? "O carimbo voltou como estava" : "Carimbo cancelado");
            return;
        }
        if (doc_.undo()) {
            say("Desfeito");
        }
        pruneSelection();
        showStampInfo();
    }

    void redo() {
        cancelStamp();
        if (doc_.redo()) {
            say("Refeito");
        }
        pruneSelection();
        showStampInfo();
    }

    void frame() {
        if (doc_.isOpen()) {
            camera_.frame(doc_.raycaster().boundsMin(), doc_.raycaster().boundsMax());
        }
    }

    void handleUi(const lkz::UiEvent& event) {
        const std::string& a = event.action;
        const int step = event.argument == "-1" ? -1 : 1;
        if (a == "abrir") {
            startBrowsing();
        } else if (a == "importar") {
            startImport(event.argument == "decalques" ? Browse::ImportDecals : Browse::ImportPalette);
        } else if (a == "raiz") {
            const int index = std::atoi(event.argument.c_str());
            if (index >= 0 && index < static_cast<int>(roots_.size())) {
                root_ = index;
                browse(roots_[static_cast<std::size_t>(index)].path);
            }
        } else if (a == "entrada") {
            choose(page_ * listSlots + std::atoi(event.argument.c_str()));
        } else if (a == "acima") {
            goUp();
        } else if (a == "pagina") {
            const int pages = std::max(1, (static_cast<int>(entries_.size()) + listSlots - 1) / listSlots);
            page_ = std::clamp(page_ + step, 0, pages - 1);
            showPage();
        } else if (a == "fecharAbrir") {
            closeBrowser();
        } else if (a == "importSubstituir") {
            closeImportConflict();
            finishImport(importSource_, importName_);
        } else if (a == "importOutroNome") {
            closeImportConflict();
            finishImport(importSource_, freeImportName(importName_));
        } else if (a == "importCancelar") {
            closeImportConflict();
        } else if (a == "exportar") {
            startExport();
        } else if (a == "pasta") {
            chosenFolder_ = event.argument;
            showExport();
        } else if (a == "gravar") {
            exportNow();
        } else if (a == "fecharExportar") {
            exporting_ = false;
            set("exportando", false);
        } else if (a == "ferramenta") {
            setTool(event.argument == "borracha"      ? Tool::Eraser
                    : event.argument == "conta-gotas" ? Tool::Picker
                    : event.argument == "balde"       ? Tool::Bucket
                    : event.argument == "carimbo"     ? Tool::Stamp
                    : event.argument == "mao"         ? Tool::Hand
                    : event.argument == "esculpir"    ? Tool::Sculpt
                                                      : Tool::Brush);
        } else if (a == "tingir") {
            setTint(!tint_);
        } else if (a == "tinta") {
            setPaint(event.argument == "material" ? Paint::Material : Paint::Color);
        } else if (a == "cor") {
            if (const auto color = parseHex(event.argument)) {
                setColor(*color); // com um carimbo tingido pendente, vai para ele
                if (tool_ == Tool::Stamp || tool_ == Tool::Hand) {
                    stampColorChanged(); // a cor dos carimbos: Carimbo e Mão continuam
                } else {
                    if (paint_ != Paint::Material || !tint_) { // tingindo, a cor vai para o material
                        setPaint(Paint::Color);
                    }
                    if (tool_ != Tool::Brush && tool_ != Tool::Bucket) {
                        setTool(Tool::Brush);
                    }
                }
            }
        } else if (a == "paleta") {
            choosePaletteMaterial(palettePage_ * paletteSlots + std::atoi(event.argument.c_str()));
            if (tool_ != Tool::Brush && tool_ != Tool::Bucket) {
                setTool(Tool::Bucket);
            }
        } else if (a == "paletaPagina") {
            palettePage_ += step;
            showPalette();
        } else if (a == "decalque") {
            chooseDecal(decalPage_ * paletteSlots + std::atoi(event.argument.c_str()));
        } else if (a == "decalquePagina") {
            decalPage_ += step;
            showDecals();
        } else if (a == "escSelecao") {
            sculptPick_ = event.argument == "triangulo"   ? SculptPick::Triangle
                          : event.argument == "plano"     ? SculptPick::Plane
                          : event.argument == "retangulo" ? SculptPick::Rectangle
                                                          : SculptPick::Wall;
            showSculpt();
        } else if (a == "escModo") {
            finishSculpt();
            sculptMode_ = event.argument == "mover" ? painter::SculptMode::Move : painter::SculptMode::Extrude;
            showSculpt();
        } else if (a == "escDist") {
            if (openSculpt()) {
                setSculptDistance(doc_.sculptDistance() + static_cast<float>(std::atoi(event.argument.c_str())) / 100.0f);
            }
        } else if (a == "escConfirmar") {
            finishSculpt();
        } else if (a == "escCancelar") {
            cancelSculpt();
        } else if (a == "escLimpar") {
            finishSculpt();
            setSculptFaces({});
        } else if (a == "carimboCor") {
            setStampTint(event.argument == "multiplicar" ? painter::DecalTint::Multiply
                         : event.argument == "branco"    ? painter::DecalTint::WhiteOnly
                                                         : painter::DecalTint::None);
        } else if (a == "profundidade") {
            setStampDepth(stampDepth_ + static_cast<float>(std::atoi(event.argument.c_str())) / 100.0f);
        } else if (a == "profundidadeZero") {
            setStampDepth(0.0f);
        } else if (a == "carimboGirar") {
            rotateStamp();
        } else if (a == "carimboConfirmar") {
            commitStamp();
        } else if (a == "carimboCancelar") {
            cancelStamp();
        } else if (a == "carimboCopiar") {
            copyStamp();
        } else if (a == "carimboColar") {
            pasteStamp();
        } else if (a == "carimboApagar") {
            deleteStamp();
        } else if (a == "escala") {
            changeScale(step);
        } else if (a == "selecao") {
            selection_ = event.argument == "triangulo" ? painter::Selection::Triangle
                         : event.argument == "plano"   ? painter::Selection::Plane
                         : event.argument == "material" ? painter::Selection::Material
                                                        : painter::Selection::Wall;
            showBucket();
        } else if (a == "baldeDestino") {
            setTarget(event.argument);
        } else if (a == "matiz") {
            adjustHsv(0, step);
            stampColorChanged();
        } else if (a == "saturacao") {
            adjustHsv(1, step);
            stampColorChanged();
        } else if (a == "brilho") {
            adjustHsv(2, step);
            stampColorChanged();
        } else if (a == "tamanho") {
            setRadius(brush_.radius * (step > 0 ? 1.25f : 0.8f));
        } else if (a == "dureza") {
            brush_.hardness = std::clamp(brush_.hardness + 0.1f * static_cast<float>(step), 0.0f, 1.0f);
            showBrush();
        } else if (a == "opacidade") {
            brush_.opacity = std::clamp(brush_.opacity + 0.1f * static_cast<float>(step), 0.1f, 1.0f);
            showBrush();
        } else if (a == "desfazer") {
            undo();
        } else if (a == "refazer") {
            redo();
        } else if (a == "enquadrar") {
            frame();
        } else if (a == "uvUnico") {
            requestUniqueUv();
        } else if (a == "uvAgora") {
            closeUvQuestion();
            requestUniqueUv();
        } else if (a == "uvAssim") {
            closeUvQuestion();
            allowOverlap_ = true;
            say("Pintando no UV sobreposto: as faces que dividem a imagem recebem a tinta juntas");
        } else if (a == "uvCancelar") {
            closeUvQuestion();
        } else if (a == "densidade") {
            density_ = std::clamp(step > 0 ? density_ * 2.0f : density_ * 0.5f, 4.0f, 256.0f);
            showDensity();
        }
    }

    void handleKeys(const lkz::InputState& input) {
        const bool control = input.down(lkz::Key::LeftControl) || input.down(lkz::Key::RightControl);
        const bool shift = input.down(lkz::Key::LeftShift) || input.down(lkz::Key::RightShift);
        if (input.pressed(lkz::Key::Escape) && tool_ == Tool::Sculpt && !dialogOpen() &&
            (doc_.sculpting() || sculptRect_ || !sculptFaces_.empty())) {
            if (doc_.sculpting()) {
                cancelSculpt();
            } else if (sculptRect_) {
                sculptRect_.reset();
                sculptDrag_ = SculptDrag::None;
                previewChanged_ = true;
            } else {
                setSculptFaces({});
            }
        } else if (input.pressed(lkz::Key::Escape)) {
            if (pending_ && !dialogOpen()) {
                cancelStamp();
            } else if (!selected_.empty() && !dialogOpen()) {
                selectStamps({});
            } else {
                closeBrowser();
                exporting_ = false;
                set("exportando", false);
                closeUvQuestion();
                closeImportConflict();
            }
        }
        if (control) {
            if (input.pressed(lkz::Key::Z)) {
                shift ? redo() : undo();
            } else if (input.pressed(lkz::Key::Y)) {
                redo();
            } else if (input.pressed(lkz::Key::O)) {
                startBrowsing();
            } else if (input.pressed(lkz::Key::S)) {
                startExport();
            } else if (input.pressed(lkz::Key::C)) {
                copyStamp();
            } else if (input.pressed(lkz::Key::V)) {
                pasteStamp();
            }
            if (input.wheel != 0.0f && !dialogOpen()) {
                setRadius(brush_.radius * std::pow(1.15f, input.wheel));
            }
            return;
        }
        if (dialogOpen()) {
            return;
        }
        if (input.pressed(lkz::Key::B)) {
            setTool(Tool::Brush);
        } else if (input.pressed(lkz::Key::G)) {
            setTool(Tool::Bucket);
        } else if (input.pressed(lkz::Key::C)) {
            setTool(Tool::Stamp);
        } else if (input.pressed(lkz::Key::H)) {
            setTool(Tool::Hand);
        } else if (input.pressed(lkz::Key::X)) {
            setTool(Tool::Sculpt);
        } else if (input.pressed(lkz::Key::Delete)) {
            deleteStamp();
        } else if (input.pressed(lkz::Key::E)) {
            setTool(Tool::Eraser);
        } else if (input.pressed(lkz::Key::I)) {
            setTool(Tool::Picker);
        } else if (input.pressed(lkz::Key::F)) {
            frame();
        } else if (input.pressed(lkz::Key::R)) {
            rotateStamp();
        } else if (input.pressed(lkz::Key::Enter)) {
            finishSculpt();
            commitStamp();
        } else if (input.pressed(lkz::Key::LeftBracket)) {
            setRadius(brush_.radius * 0.8f);
        } else if (input.pressed(lkz::Key::RightBracket)) {
            setRadius(brush_.radius * 1.25f);
        }
    }

    std::optional<lkz::asset::RayHit> hitAt(const glm::vec2& pixel, const glm::vec2& viewport) const {
        return doc_.raycaster().intersect(camera_.eye(), camera_.rayDirection(pixel, viewport));
    }

    void pick(const lkz::asset::RayHit& hit) {
        if (const int decal = doc_.decalAt(hit); decal >= 0) {
            chooseDecal(decal);
            setTool(Tool::Stamp);
            say("Decalque do modelo: " + decals_.at(decal).name);
        } else if (const int material = doc_.paletteAt(hit); material >= 0) {
            choosePaletteMaterial(material);
            const auto tint = doc_.tintAt(hit);
            setTint(tint.has_value());
            if (tint) {
                setColor(*tint);
            }
            say("Material do modelo: " + palette_.at(material).name + (tint ? " tingido de " + hex(*tint) : ""));
        } else if (const auto color = doc_.colorAt(hit)) {
            setColor(*color);
            setPaint(Paint::Color);
            say("Cor do modelo: " + hex(*color));
        }
    }

    void handleMouse(const lkz::InputState& input) {
        const glm::vec2 viewport = input.viewport;
        if (!doc_.isOpen() || viewport.x <= 0.0f || viewport.y <= 0.0f) {
            set("cursor", false);
            world_.setMouseCursor(lkz::MouseCursor::Default);
            hoverFace_ = -1;
            return;
        }
        const glm::vec2 mouse = input.mousePosition;
        const auto hit = hitAt(mouse, viewport);
        const bool alt = input.down(lkz::Key::LeftAlt) || input.down(lkz::Key::RightAlt);
        const bool picking = tool_ == Tool::Picker || alt;
        const bool bucket = tool_ == Tool::Bucket && !picking;
        const bool stamping = tool_ == Tool::Stamp && !picking;
        const bool hand = tool_ == Tool::Hand && !picking;
        const bool sculpting = tool_ == Tool::Sculpt && !picking;
        const bool overModel = hit && !input.pointerOverUi;
        hoverFace_ = overModel ? doc_.faceIndex(hit->where) : -1;
        world_.setMouseCursor(cursorFor(input, mouse, viewport));

        // Anel do pincel: o diâmetro dele na distância do ponto sob o mouse. Balde, carimbo e
        // conta-gotas: um ponto.
        const float dp = designHeight / viewport.y;
        if (overModel && !hand) {
            const bool dot = picking || bucket || stamping || sculpting;
            const float diameter = dot ? 6.0f : 2.0f * brush_.radius * camera_.pixelsPerMeter(hit->distance, viewport) * dp;
            set("cursor", true);
            set("cursorD", static_cast<double>(diameter));
            set("cursorX", static_cast<double>(mouse.x * dp - diameter * 0.5f));
            set("cursorY", static_cast<double>(mouse.y * dp - diameter * 0.5f));
        } else {
            set("cursor", false);
        }

        // Balde: as faces sob o mouse ficam realçadas; o clique aplica. Mão: o carimbo sob
        // o mouse.
        if (bucket && overModel && !doc_.stroking()) {
            std::vector<int> faces = doc_.select(*hit, selection_);
            set("selecaoInfo", fmt::format("{} triângulos · {:.1f} m²", faces.size(), doc_.area(faces)));
            setHighlight(std::move(faces));
        } else if (sculpting) {
            // A seleção (na escultura aberta, as faces dela) e o que um clique pegaria.
            checkSculptSelection();
            std::vector<int> faces = doc_.sculpting() ? doc_.sculptSelection() : sculptFaces_;
            if (overModel && sculptDrag_ == SculptDrag::None && !doc_.sculpting() && sculptPick_ != SculptPick::Rectangle &&
                !std::binary_search(sculptFaces_.begin(), sculptFaces_.end(), hoverFace_)) {
                const std::vector<int> hover = doc_.select(*hit, sculptSelection());
                faces.insert(faces.end(), hover.begin(), hover.end());
            }
            std::sort(faces.begin(), faces.end());
            faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
            setHighlight(std::move(faces));
        } else if ((hand || stamping) && !doc_.stroking()) {
            // Os selecionados (menos com o mouse no painel, para ver a cor de verdade) e, na
            // Mão, o carimbo sob o mouse.
            std::vector<int> faces = input.pointerOverUi ? std::vector<int>{} : selectedFaces();
            if (hand && hoverFace_ >= 0 && !pending_) {
                const std::vector<int> hover = doc_.stampFaces(hoverFace_);
                faces.insert(faces.end(), hover.begin(), hover.end());
            }
            std::sort(faces.begin(), faces.end());
            faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
            setHighlight(std::move(faces));
        } else if (!highlight_.empty()) {
            setHighlight({});
            set("selecaoInfo", std::string{});
        }

        if (stamping) {
            handleStamp(input, hit, mouse, viewport);
            return;
        }
        if (hand) {
            handleHand(input, hit, mouse, viewport);
            return;
        }
        if (sculpting) {
            handleSculpt(input, hit, mouse, viewport);
            return;
        }

        if (input.pressed(lkz::MouseButton::Left) && !input.pointerOverUi) {
            if (picking) {
                if (hit) {
                    pick(*hit);
                }
            } else if (bucket) {
                applyBucket();
            } else if (hit) {
                const int face = doc_.faceIndex(hit->where);
                const bool canvas = face >= 0 && doc_.face(face).target == SurfaceTarget::Unique &&
                                    doc_.hasCanvas(doc_.model().parts[static_cast<std::size_t>(hit->where.part)].material);
                if (canvas && !allowOverlap_ && doc_.faceOverlap(face) > 0.01f) {
                    askUniqueUv(doc_.faceOverlap(face));
                    return;
                }
                if (!canvas) {
                    say("Esta face não tem tela para o pincel (é compartilhada, de cor lisa, decalque ou sem UV): "
                        "use o balde com destino Único, ou gere o UV único");
                }
                brush_.erase = tool_ == Tool::Eraser;
                brush_.material = paint_ == Paint::Material && !brush_.erase ? paletteIndex_ : -1;
                brush_.tint = brush_.material >= 0 && tint_;
                doc_.beginStroke();
                doc_.dab(hit->position, camera_.eye(), brush_);
                lastMouse_ = mouse;
            }
            return;
        }
        if (!doc_.stroking()) {
            return;
        }
        if (input.released(lkz::MouseButton::Left) || !input.down(lkz::MouseButton::Left)) {
            doc_.endStroke();
            return;
        }
        // O mouse andou desde o último passo: pinceladas ao longo do caminho na tela, a
        // cada quarto do raio, cada uma no ponto da superfície sob ela.
        const float distance = glm::length(mouse - lastMouse_);
        if (distance < 0.5f) {
            return;
        }
        // Até ~12 ms por quadro: o que sobrar do caminho fica para o próximo, e a tela não trava.
        const float radiusPixels = hit ? brush_.radius * camera_.pixelsPerMeter(hit->distance, viewport) : 8.0f;
        const int steps = std::clamp(static_cast<int>(std::ceil(distance / std::max(radiusPixels * 0.25f, 1.0f))), 1, 64);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 1; i <= steps; ++i) {
            const glm::vec2 at = glm::mix(lastMouse_, mouse, static_cast<float>(i) / static_cast<float>(steps));
            if (const auto under = i == steps ? hit : hitAt(at, viewport)) {
                doc_.dab(under->position, camera_.eye(), brush_);
            }
            if (i < steps && std::chrono::steady_clock::now() - start > std::chrono::milliseconds{12}) {
                lastMouse_ = at;
                return;
            }
        }
        lastMouse_ = mouse;
    }

    // --- Navegador: abrir modelos e importar imagens ---

    struct Root {
        std::string label;
        fs::path path;
    };

    void openBrowser(Browse mode, std::vector<Root> roots, const char* title, const fs::path& start) {
        exporting_ = false;
        set("exportando", false);
        browseMode_ = mode;
        roots_ = std::move(roots);
        for (int i = 0; i < rootSlots; ++i) {
            set(fmt::format("raiz{}", i), i < static_cast<int>(roots_.size()) ? roots_[static_cast<std::size_t>(i)].label : std::string{});
        }
        set("abrirTitulo", std::string{title});
        browsing_ = true;
        set("abrindo", true);
        root_ = 0;
        for (std::size_t i = 0; i < roots_.size(); ++i) {
            const fs::path relative = start.lexically_relative(roots_[i].path);
            if (!start.empty() && !relative.empty() && *relative.begin() != "..") {
                root_ = static_cast<int>(i);
                break;
            }
        }
        browse(start.empty() ? roots_.front().path : start);
    }

    void closeBrowser() {
        browsing_ = false;
        set("abrindo", false);
    }

    void startBrowsing() {
        finishSculpt();
        if (!project_) {
            say("Sem arquivo de projeto: passe o modelo na linha de comando");
            return;
        }
        std::vector<Root> roots;
        for (const fs::path& root : project_->roots) {
            if (roots.size() < static_cast<std::size_t>(rootSlots)) {
                roots.push_back({utf8(root.filename()), root});
            }
        }
        openBrowser(Browse::Models, std::move(roots), "Abrir modelo", modelsFolder_);
    }

    /// Imagens de qualquer lugar: as pastas do usuário, o projeto e os discos.
    void startImport(Browse mode) {
        const bool palette = mode == Browse::ImportPalette;
        if (!project_ || (palette ? project_->palette : project_->decals).empty()) {
            say(palette ? "Defina \"paleta\" no arquivo de projeto para importar materiais"
                        : "Defina \"decalques\" no arquivo de projeto para importar decalques");
            return;
        }
        std::vector<Root> roots;
        std::error_code ec;
        const char* home = std::getenv("USERPROFILE");
        if (home == nullptr) {
            home = std::getenv("HOME");
        }
        if (home != nullptr) {
            const fs::path base = fromUtf8(home);
            for (const auto& [label, folder] : {std::pair{"Imagens", "Pictures"}, std::pair{"Downloads", "Downloads"},
                                                std::pair{"Área de trabalho", "Desktop"}}) {
                if (fs::is_directory(base / folder, ec)) {
                    roots.push_back({label, base / folder});
                }
            }
        }
        roots.push_back({"Projeto", project_->file.parent_path()});
        for (char drive = 'C'; drive <= 'Z' && roots.size() < static_cast<std::size_t>(rootSlots); ++drive) {
            const fs::path path = std::string{drive} + ":/";
            if (fs::exists(path, ec)) {
                roots.push_back({std::string{drive} + ":", path});
            }
        }
        openBrowser(mode, std::move(roots), palette ? "Importar imagem para a paleta" : "Importar imagem de decalque",
                    importFolder_);
    }

    void goUp() {
        if (!browsing_) {
            return;
        }
        if (browseMode_ == Browse::Models) {
            if (folder_ != roots_[static_cast<std::size_t>(root_)].path) {
                browse(folder_.parent_path());
            }
        } else if (folder_.has_parent_path() && folder_.parent_path() != folder_) {
            browse(folder_.parent_path());
        }
    }

    void browse(const fs::path& folder) {
        folder_ = folder;
        (browseMode_ == Browse::Models ? modelsFolder_ : importFolder_) = folder;
        entries_.clear();
        std::error_code ec;
        std::vector<fs::path> folders;
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator{folder, fs::directory_options::skip_permission_denied, ec}) {
            const auto name = entry.path().filename().u8string();
            if (name.empty() || name.front() == u8'.' || name.front() == u8'$') {
                continue;
            }
            if (entry.is_directory(ec)) {
                folders.push_back(entry.path());
                continue;
            }
            const std::string extension = lower(utf8(entry.path().extension()));
            const bool wanted = browseMode_ == Browse::Models
                                    ? extension == ".glb" || extension == ".gltf"
                                    : extension == ".png" || extension == ".jpg" || extension == ".jpeg";
            if (wanted) {
                files.push_back(entry.path());
            }
        }
        std::sort(folders.begin(), folders.end());
        std::sort(files.begin(), files.end());
        for (const fs::path& path : folders) {
            entries_.push_back({path, true});
        }
        for (const fs::path& path : files) {
            entries_.push_back({path, false});
        }
        page_ = 0;
        set("raizAtiva", root_);
        if (browseMode_ == Browse::Models) {
            const fs::path& root = roots_[static_cast<std::size_t>(root_)].path;
            const fs::path relative = folder.lexically_relative(root);
            set("pastaAtual", utf8(root.filename()) + (relative == "." ? "" : "/" + utf8(relative.generic_u8string())));
        } else {
            set("pastaAtual", utf8(folder.generic_u8string()));
        }
        showPage();
    }

    void showPage() {
        const int pages = std::max(1, (static_cast<int>(entries_.size()) + listSlots - 1) / listSlots);
        for (int i = 0; i < listSlots; ++i) {
            const auto index = static_cast<std::size_t>(page_ * listSlots + i);
            const bool exists = index < entries_.size();
            set(fmt::format("ent{}", i), exists ? utf8(entries_[index].path.filename()) : std::string{});
            set(fmt::format("entPasta{}", i), exists && entries_[index].folder);
        }
        set("pagina", entries_.empty() ? std::string{"pasta vazia"}
                                       : fmt::format("{} de {} · {} itens", page_ + 1, pages, entries_.size()));
    }

    void choose(int index) {
        if (index < 0 || index >= static_cast<int>(entries_.size())) {
            return;
        }
        const Entry entry = entries_[static_cast<std::size_t>(index)];
        if (entry.folder) {
            browse(entry.path);
            return;
        }
        if (browseMode_ == Browse::Models) {
            closeBrowser();
            open(entry.path);
            return;
        }
        importImage(entry.path);
    }

    // --- Importar ---

    [[nodiscard]] painter::Palette& importLibrary() {
        return browseMode_ == Browse::ImportDecals ? decals_ : palette_;
    }

    /// Copia para a pasta da biblioteca; se já existe outro arquivo com o mesmo nome, pergunta.
    void importImage(const fs::path& source) {
        painter::Palette& library = importLibrary();
        const std::string name = painter::Palette::importName(source);
        std::error_code ec;
        const fs::path target = library.folder() / fromUtf8(name);
        if (fs::exists(target, ec) && !fs::equivalent(source, target, ec)) {
            importSource_ = source;
            importName_ = name;
            importConflict_ = true;
            set("importConflito", true);
            set("importConflitoTexto",
                fmt::format("Já existe {} em {}. Substituir a imagem (os modelos que a usam mudam junto), "
                            "ou importar com outro nome?",
                            name, shortName(library.folder())));
            return;
        }
        finishImport(source, name);
    }

    /// nome-2.png, nome-3.png...: o primeiro que não existe na pasta.
    [[nodiscard]] std::string freeImportName(const std::string& name) {
        const fs::path file = fromUtf8(name);
        const std::string stem = utf8(file.stem());
        const std::string extension = utf8(file.extension());
        std::error_code ec;
        for (int n = 2;; ++n) {
            const std::string candidate = fmt::format("{}-{}{}", stem, n, extension);
            if (!fs::exists(importLibrary().folder() / fromUtf8(candidate), ec)) {
                return candidate;
            }
        }
    }

    void closeImportConflict() {
        importConflict_ = false;
        set("importConflito", false);
    }

    void finishImport(const fs::path& source, const std::string& name) {
        closeBrowser();
        painter::Palette& library = importLibrary();
        std::string error;
        const int index = library.importFile(source, name, error);
        if (index < 0) {
            say(error);
            return;
        }
        if (browseMode_ == Browse::ImportDecals) {
            uploadedDecals_.assign(static_cast<std::size_t>(maxDecals), false); // a imagem pode ter mudado
            std::fill(tintTexture_.begin(), tintTexture_.end(), -1);
            previewDecal_ = -1;
            doc_.paletteChanged();
            chooseDecal(index);
            setTool(Tool::Stamp);
        } else {
            if (index < maxPalette) {
                uploadedPalette_[static_cast<std::size_t>(index)] = false;
            }
            std::replace(tintTexture_.begin(), tintTexture_.end(), static_cast<int>(Surface::Kind::Shared) * 65536 + index, -1);
            doc_.paletteChanged();
            choosePaletteMaterial(index);
        }
        const painter::PaletteMaterial& item = library.at(index);
        say(fmt::format("Importado: {} ({}×{} px){}", shortName(item.image), item.mips.front().width,
                        item.mips.front().height, item.transparent ? " · com transparência" : ""));
    }

    // --- Exportar ---

    void startExport() {
        finishSculpt();
        if (!doc_.isOpen()) {
            say("Abra um modelo antes de exportar");
            return;
        }
        doc_.endStroke();
        commitStamp();
        closeBrowser();
        folders_ = project_ ? project_->outputFolders() : std::vector<std::string>{};
        if (project_) {
            chosenFolder_ = project_->folderOf(doc_.path()).value_or(chosenFolder_);
        }
        for (int i = 0; i < folderSlots; ++i) {
            set(fmt::format("pasta{}", i), i < static_cast<int>(folders_.size()) ? folders_[static_cast<std::size_t>(i)] : std::string{});
        }
        exporting_ = true;
        set("exportando", true);
        showExport();
    }

    /// Para onde a exportação vai. Sem projeto: ao lado do original, com "_pintado". Com
    /// projeto: na subpasta escolhida da saída; se o modelo já mora nela, no lugar dele.
    [[nodiscard]] std::optional<fs::path> exportTarget() const {
        const fs::path source = doc_.path();
        fs::path name = source.stem();
        if (!project_) {
            name += "_pintado.glb";
            return source.parent_path() / name;
        }
        if (chosenFolder_.empty()) {
            return std::nullopt;
        }
        name += ".glb";
        if (project_->folderOf(source) == chosenFolder_) {
            return source.parent_path() / name;
        }
        return project_->output / fromUtf8(chosenFolder_) / name;
    }

    void showExport() {
        set("pastaEscolhida", chosenFolder_);
        const auto target = exportTarget();
        std::error_code ec;
        const fs::path base = project_ ? project_->file.parent_path() : fs::path{};
        set("destino", target ? utf8(project_ ? target->lexically_relative(base).generic_u8string() : target->generic_u8string())
                              : std::string{"escolha a pasta"});
        set("destinoExiste", target && fs::exists(*target, ec));
    }

    void exportNow() {
        const auto target = exportTarget();
        if (!target) {
            say("Escolha a pasta de destino");
            return;
        }
        std::error_code ec;
        // Antes de substituir: uma cópia do arquivo de antes, uma vez só por modelo.
        if (project_ && fs::exists(*target, ec)) {
            const fs::path backup = project_->work / target->stem() / ("original" + utf8(target->extension()));
            if (!fs::exists(backup, ec)) {
                fs::create_directories(backup.parent_path(), ec);
                fs::copy_file(*target, backup, ec);
                if (ec) {
                    say("Não consegui guardar a cópia do original em " + utf8(backup) + "; nada foi gravado");
                    return;
                }
            }
        }
        std::string report;
        doc_.exportGlb(*target, report);
        say(report);
        spdlog::info("Pintor: {}", report);
        exporting_ = false;
        set("exportando", false);
    }

    struct Entry {
        fs::path path;
        bool folder = false;
    };

    lkz::World& world_;
    painter::OrbitCamera camera_;
    std::optional<painter::Project> project_;
    painter::Palette palette_;
    painter::Palette decals_;
    painter::Document doc_;
    std::map<std::uint32_t, lkz::SceneNode*> nodes_; // por id de malha
    std::vector<bool> uploadedPalette_;              // textura da paleta já enviada
    std::map<std::pair<int, std::uint32_t>, int> tintSlots_; // (imagem, cor) tingida -> lugar extra
    std::vector<int> tintTexture_ = std::vector<int>(static_cast<std::size_t>(maxTinted), -1); // tintSource em cada lugar extra
    std::vector<bool> uploadedDecals_;               // textura do decalque do documento já enviada
    painter::Brush brush_;
    Tool tool_ = Tool::Brush;
    Paint paint_ = Paint::Color;
    bool tint_ = false; // material da paleta tingido pela cor
    int paletteIndex_ = -1;
    int palettePage_ = 0;
    int decalIndex_ = -1;
    int decalPage_ = 0;
    std::optional<PendingStamp> pending_;
    std::optional<painter::DecalPlacement> clipboard_; // Ctrl+C
    std::optional<HandPress> handPress_;
    std::vector<int> selected_; // carimbos selecionados (ids do Document); o último é o principal
    // Esculpir
    SculptPick sculptPick_ = SculptPick::Wall;
    painter::SculptMode sculptMode_ = painter::SculptMode::Extrude;
    std::vector<int> sculptFaces_;    // selecionadas, em ordem (índices do documento)
    std::uint64_t sculptVersion_ = 0; // geometryVersion() em que sculptFaces_ vale
    glm::vec3 sculptAxis_{0.0f, 0.0f, 1.0f}; // a direção: a normal da face clicada
    struct SculptPress {
        glm::vec2 mouse{0.0f};
        int face = -1;
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f, 0.0f, 1.0f};
    };
    std::optional<SculptPress> sculptPress_; // apertou, ainda sem arrastar
    enum class SculptDrag : std::uint8_t { None, Pull, Rectangle };
    SculptDrag sculptDrag_ = SculptDrag::None;
    glm::vec3 sculptOrigin_{0.0f}; // onde o arrasto pegou
    float sculptStart_ = 0.0f;     // a distância quando o arrasto começou
    float sculptGrab_ = 0.0f;      // axisParam onde o arrasto começou
    std::optional<PendingStamp> sculptRect_; // retângulo sendo desenhado (o plano e os cantos)
    int hoverFace_ = -1;                               // face sob o mouse (-1 = nenhuma, ou sobre a interface)
    std::map<int, glm::vec2> lastSize_; // decalque -> largura x altura do último carimbo (sem giro)
    bool previewChanged_ = false;
    int previewDecal_ = -1; // decalque da textura da prévia
    glm::vec3 previewWhiteOnly_{-1.0f}; // a textura da prévia está tingida só no branco com esta cor (-1 = não)
    painter::DecalTint stampTint_ = painter::DecalTint::None; // dos carimbos novos
    float stampDepth_ = 0.0f;                                 // dos carimbos novos (metros)
    painter::Selection selection_ = painter::Selection::Wall;
    SurfaceTarget target_ = SurfaceTarget::Shared;
    bool restore_ = false;
    std::vector<int> highlight_;
    bool highlightChanged_ = false;
    bool askingUv_ = false;     // janela "UV sobreposto" aberta
    bool allowOverlap_ = false; // "Pintar assim mesmo" (até abrir outro modelo)
    bool uvPending_ = false;    // UV único no próximo quadro
    float density_ = painter::Document::texelsPerMeter; // do UV único
    glm::vec2 lastMouse_{0.0f};
    std::map<std::string, lkz::UiValue, std::less<>> shown_;

    bool browsing_ = false;
    Browse browseMode_ = Browse::Models;
    std::vector<Root> roots_;
    int root_ = 0;
    fs::path folder_;       // pasta mostrada agora
    fs::path modelsFolder_; // última pasta de modelos
    fs::path importFolder_; // última pasta de imagens importadas
    std::vector<Entry> entries_;
    int page_ = 0;

    bool importConflict_ = false;
    fs::path importSource_;
    std::string importName_;

    bool exporting_ = false;
    std::vector<std::string> folders_;
    std::string chosenFolder_;
};

} // namespace

int main(int argc, char** argv) {
    lkz::initLogging();

    std::optional<fs::path> projectFile;
    std::optional<fs::path> modelFile;
    std::string_view backendName = "raylib";
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--")) {
            backendName = arg.substr(2);
            continue;
        }
        const fs::path path = fs::absolute(fromUtf8(arg));
        if (lower(utf8(path.extension())) == ".json") {
            projectFile = path;
        } else {
            modelFile = path;
        }
    }
    std::optional<painter::Project> project;
    if (projectFile) {
        std::string error;
        project = painter::Project::load(*projectFile, error);
        if (!project) {
            spdlog::error("Pintor: {}", error);
            return EXIT_FAILURE;
        }
    }

    lkz::RenderConfig config;
    config.title = "Lkz Engine - Pintor de modelos";
    config.width = 1600;
    config.height = 900;
    config.showStats = false;
    config.escapeClosesWindow = false; // Esc fecha as janelas do Pintor, não o programa
    config.clearColor = {0.16f, 0.17f, 0.19f, 1.0f};
    config.lightDirection = {-0.35f, -1.0f, -0.55f};
    for (int slot = 0; slot < slotCount; ++slot) {
        for (const bool doubleSided : {false, true}) {
            for (const bool highlighted : {false, true}) {
                lkz::PrimitiveMesh mesh{lkz::PrimitiveShape::Custom};
                mesh.doubleSided = doubleSided;
                mesh.runtimeTexture = textureId(slot);
                config.meshes[meshId(slot, doubleSided, highlighted)] = mesh;
            }
        }
    }

    std::unique_ptr<lkz::RenderBackend> backend;
#if defined(LKZ_WITH_NVRHI)
    if (const auto api = lkz::parseGraphicsApi(backendName)) {
        config.title += std::string{" ("} + lkz::graphicsApiName(*api) + ")";
        backend = std::make_unique<lkz::NvrhiRenderBackend>(std::move(config), *api);
    }
#endif
    if (!backend) {
        backend = std::make_unique<lkz::RaylibRenderBackend>(std::move(config));
    }
    lkz::World world{std::move(backend)};
    auto& camera = world.root().emplaceChild<lkz::Camera>("Câmera", world.registry());
    world.setActiveCamera(&camera);

    // A interface fica ao lado do executável (o CMake copia tools/painter/ui).
    const fs::path ui = fs::absolute(fromUtf8(argv[0])).parent_path() / "painter-ui";
    lkz::UiDocument document;
    document.path = utf8(ui / "painter.rml");
    document.fonts = {utf8(ui / "Barlow-Regular.ttf"), utf8(ui / "Barlow-SemiBold.ttf")};
    document.designHeight = designHeight;
    document.values = Painter::uiValues();
    world.showUi(std::move(document));

    Painter painter{world, camera, std::move(project)};
    if (modelFile) {
        painter.open(*modelFile);
    }
    world.onFixedUpdate([&painter](lkz::World& w, float /*dt*/) { painter.tick(w); });
    world.run();
    return EXIT_SUCCESS;
}
