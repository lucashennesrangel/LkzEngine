#include "Document.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <numeric>
#include <system_error>
#include <tuple>
#include <utility>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat2x2.hpp>
#include <glm/matrix.hpp>
#include <lkz/Asset/Bake.hpp>
#include <lkz/Asset/Gltf.hpp>
#include <lkz/Asset/Image.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include "Palette.hpp"

namespace painter {

using lkz::asset::SurfaceTarget;
using lkz::asset::TexelRect;
namespace fs = std::filesystem;

namespace {

int wrap(int value, int size) {
    const int r = value % size;
    return r < 0 ? r + size : r;
}

TexelRect unite(const TexelRect& a, const TexelRect& b) {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

/// Junta `rect` à lista, unindo com os retângulos perto dele; muitos viram um só.
void addRect(std::vector<TexelRect>& rects, TexelRect rect) {
    if (rect.empty()) {
        return;
    }
    constexpr int near = 16; // texels: perto assim, um retângulo só sai mais barato
    for (bool merged = true; merged;) {
        merged = false;
        for (auto it = rects.begin(); it != rects.end(); ++it) {
            if (it->x0 <= rect.x1 + near && rect.x0 <= it->x1 + near && it->y0 <= rect.y1 + near && rect.y0 <= it->y1 + near) {
                rect = unite(rect, *it);
                rects.erase(it);
                merged = true;
                break;
            }
        }
    }
    rects.push_back(rect);
    if (rects.size() > 16) {
        TexelRect all{};
        for (const TexelRect& r : rects) {
            all = unite(all, r);
        }
        rects.assign(1, all);
    }
}

std::uint8_t toByte(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

std::array<std::uint8_t, 4> toBytes(const glm::vec4& color) {
    return {toByte(color.r), toByte(color.g), toByte(color.b), toByte(color.a)};
}

/// A cor que multiplica a imagem da paleta numa face (branca = sem tinta).
glm::vec4 tintOf(const painter::Face& face) {
    return face.colored ? glm::vec4{face.color[0], face.color[1], face.color[2], 255.0f} / 255.0f : glm::vec4{1.0f};
}

/// A tinta como chave de agrupamento (sem tinta = branca).
std::uint32_t tintKey(const painter::Face& face) {
    const std::array<std::uint8_t, 4> c = face.colored ? face.color : std::array<std::uint8_t, 4>{255, 255, 255, 255};
    return static_cast<std::uint32_t>(c[0]) << 16 | static_cast<std::uint32_t>(c[1]) << 8 | c[2];
}

/// Troca os triângulos de uma malha. Sem nenhum, esvazia a malha (índices vazios querem
/// dizer "sem índices": os vértices voltariam a ser triângulos).
void setTriangles(lkz::MeshData& mesh, std::vector<std::uint32_t> indices) {
    if (indices.empty()) {
        mesh.clear();
        return;
    }
    mesh.indices = std::move(indices);
}

/// Normais por vértice, médias das faces vizinhas pesadas pela área.
void computeNormals(lkz::MeshData& mesh) {
    mesh.normals.assign(mesh.positions.size(), glm::vec3{0.0f});
    const std::size_t count = lkz::asset::triangleCount(mesh);
    for (std::size_t t = 0; t < count; ++t) {
        const auto index = lkz::asset::triangleAt(mesh, t);
        const glm::vec3 n = glm::cross(mesh.positions[index[1]] - mesh.positions[index[0]],
                                       mesh.positions[index[2]] - mesh.positions[index[0]]);
        for (const std::uint32_t i : index) {
            mesh.normals[i] += n;
        }
    }
    for (glm::vec3& n : mesh.normals) {
        const float length = glm::length(n);
        n = length > 0.0f ? n / length : glm::vec3{0.0f, 1.0f, 0.0f};
    }
}

std::string sizeText(int width, int height) {
    return width == height ? fmt::format("{}²", width) : fmt::format("{}×{}", width, height);
}

std::string generic(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

/// Caminho relativo como URI do glTF: o que não for letra, dígito ou - . _ ~ / vira %XX.
std::string encodeUri(const std::string& path) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : path) {
        const auto byte = static_cast<unsigned char>(c);
        const bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
                           c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
        if (plain) {
            out += c;
        } else {
            out += '%';
            out += digits[byte >> 4];
            out += digits[byte & 15];
        }
    }
    return out;
}

int nextPowerOfTwo(int value) {
    int power = 1;
    while (power < value) {
        power *= 2;
    }
    return power;
}

/// Amostras de cor lisa: blocos de 4 x 4 px, 16 por linha.
struct SwatchLayout {
    static constexpr int columns = 16;
    int width = Document::swatchSize;
    int height = Document::swatchSize;

    explicit SwatchLayout(std::size_t count) {
        const int n = std::max(1, static_cast<int>(count));
        width = nextPowerOfTwo(std::min(n, columns) * Document::swatchSize);
        height = nextPowerOfTwo(((n + columns - 1) / columns) * Document::swatchSize);
    }
    /// Centro da amostra `i`: todo o triângulo de cor lisa usa este UV.
    [[nodiscard]] glm::vec2 center(int i) const {
        const float half = static_cast<float>(Document::swatchSize) * 0.5f;
        return {(static_cast<float>((i % columns) * Document::swatchSize) + half) / static_cast<float>(width),
                (static_cast<float>((i / columns) * Document::swatchSize) + half) / static_cast<float>(height)};
    }
};

lkz::asset::ImageData swatchOf(const std::vector<std::array<std::uint8_t, 4>>& colors) {
    const SwatchLayout layout{colors.size()};
    lkz::asset::ImageData image;
    image.width = layout.width;
    image.height = layout.height;
    image.name = "cores";
    image.lossless = true; // JPEG misturaria as amostras vizinhas
    image.rgba.assign(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4, 255);
    for (std::size_t i = 0; i < colors.size(); ++i) {
        const int x0 = static_cast<int>(i % SwatchLayout::columns) * Document::swatchSize;
        const int y0 = static_cast<int>(i / SwatchLayout::columns) * Document::swatchSize;
        for (int y = y0; y < y0 + Document::swatchSize; ++y) {
            for (int x = x0; x < x0 + Document::swatchSize; ++x) {
                std::copy_n(colors[i].data(), 4, &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4]);
            }
        }
    }
    return image;
}

/// Junta triângulos numa malha nova; vértices da mesma origem com o mesmo UV são um só.
class MeshBuilder {
public:
    void add(int part, const lkz::MeshData& from, const std::array<std::uint32_t, 3>& index, const glm::vec2 (&uv)[3]) {
        const bool normals = from.normals.size() == from.positions.size();
        for (int k = 0; k < 3; ++k) {
            const auto key = std::make_tuple(part, index[static_cast<std::size_t>(k)], std::bit_cast<std::uint32_t>(uv[k].x),
                                             std::bit_cast<std::uint32_t>(uv[k].y));
            const auto [it, inserted] = vertices_.try_emplace(key, static_cast<std::uint32_t>(mesh.positions.size()));
            if (inserted) {
                mesh.positions.push_back(from.positions[index[static_cast<std::size_t>(k)]]);
                mesh.normals.push_back(normals ? from.normals[index[static_cast<std::size_t>(k)]] : glm::vec3{0.0f, 1.0f, 0.0f});
                mesh.uvs.push_back(uv[k]);
            }
            mesh.indices.push_back(it->second);
        }
    }

    lkz::MeshData mesh;

private:
    std::map<std::tuple<int, std::uint32_t, std::uint32_t, std::uint32_t>, std::uint32_t> vertices_;
};

/// As partes de `from` cujo material é (ou não é) de destino único, com os materiais e
/// as imagens que elas usam.
lkz::asset::ModelData subset(const lkz::asset::ModelData& from, bool unique) {
    lkz::asset::ModelData out;
    out.extras = from.extras;
    std::vector<int> materials(from.materials.size(), -1);
    std::vector<int> images(from.images.size(), -1);
    for (const lkz::asset::ModelPart& part : from.parts) {
        const lkz::asset::MaterialData& material = from.materials[static_cast<std::size_t>(part.material)];
        if ((material.target == SurfaceTarget::Unique) != unique) {
            continue;
        }
        int& index = materials[static_cast<std::size_t>(part.material)];
        if (index < 0) {
            lkz::asset::MaterialData copy = material;
            if (copy.image >= 0) {
                int& image = images[static_cast<std::size_t>(copy.image)];
                if (image < 0) {
                    image = static_cast<int>(out.images.size());
                    out.images.push_back(from.images[static_cast<std::size_t>(copy.image)]);
                }
                copy.image = image;
            }
            index = static_cast<int>(out.materials.size());
            out.materials.push_back(std::move(copy));
        }
        lkz::asset::ModelPart copy = part;
        copy.material = index;
        out.parts.push_back(std::move(copy));
    }
    return out;
}

void append(lkz::asset::ModelData& into, lkz::asset::ModelData from) {
    const int images = static_cast<int>(into.images.size());
    const int materials = static_cast<int>(into.materials.size());
    for (lkz::asset::ImageData& image : from.images) {
        into.images.push_back(std::move(image));
    }
    for (lkz::asset::MaterialData& material : from.materials) {
        if (material.image >= 0) {
            material.image += images;
        }
        into.materials.push_back(std::move(material));
    }
    for (lkz::asset::ModelPart& part : from.parts) {
        part.material += materials;
        into.parts.push_back(std::move(part));
    }
}

} // namespace

bool Document::open(const fs::path& path, std::string& error) {
    auto loaded = lkz::asset::loadGltf(path);
    if (!loaded) {
        error = "Não abriu " + path.filename().string() + " (detalhes no log)";
        return false;
    }
    if (loaded->parts.empty()) {
        error = path.filename().string() + " não tem malhas";
        return false;
    }
    const Palette* palette = palette_;
    const Palette* decals = decalLibrary_;
    *this = Document{};
    palette_ = palette;
    decalLibrary_ = decals;
    setModel(std::move(*loaded));
    path_ = path;
    return true;
}

void Document::setModel(lkz::asset::ModelData model) {
    sculpt_.reset();
    model_ = std::move(model);
    canvases_.clear();
    undo_.clear();
    redo_.clear();
    current_ = Stroke{};
    stroking_ = false;
    geometryChanged_ = true;

    // Toda parte com um material (o padrão do glTF é branco) e com normais.
    int fallback = -1;
    for (lkz::asset::ModelPart& part : model_.parts) {
        if (part.material < 0 || part.material >= static_cast<int>(model_.materials.size())) {
            if (fallback < 0) {
                fallback = static_cast<int>(model_.materials.size());
                lkz::asset::MaterialData material;
                material.name = "padrao";
                model_.materials.push_back(material);
            }
            part.material = fallback;
        }
        if (part.mesh.normals.size() != part.mesh.positions.size()) {
            computeNormals(part.mesh);
        }
    }
    raycaster_ = lkz::asset::MeshRaycaster{model_};

    // Materiais que já vieram com destino (gravados pelo Pintor): da paleta, as amostras
    // de cor lisa ou um decalque. Eles não ganham tela.
    const std::size_t materialCount = model_.materials.size();
    std::vector<int> paletteOf(materialCount, -1);
    std::vector<char> swatches(materialCount, 0);
    decals_.clear();
    decalOf_.assign(materialCount, -1);
    for (std::size_t m = 0; m < materialCount; ++m) {
        const lkz::asset::MaterialData& material = model_.materials[m];
        const lkz::asset::ImageData* image = material.image >= 0 && material.image < static_cast<int>(model_.images.size()) &&
                                                     !model_.images[static_cast<std::size_t>(material.image)].empty()
                                                 ? &model_.images[static_cast<std::size_t>(material.image)]
                                                 : nullptr;
        if (material.target == SurfaceTarget::Shared && palette_ != nullptr) {
            paletteOf[m] = palette_->find(material.shared);
        } else if (material.target == SurfaceTarget::Color && image != nullptr) {
            swatches[m] = 1;
        } else if (material.target == SurfaceTarget::Decal) {
            // Da biblioteca se aponta para ela (ou se veio sem imagem, do UV único); senão,
            // a imagem do arquivo (a composta de um carimbo com transparência).
            const int library = decalLibrary_ != nullptr ? decalLibrary_->find(material.shared) : -1;
            if (library >= 0 && (image == nullptr || !image->uri.empty())) {
                decalOf_[m] = libraryDecal(library);
            } else if (image != nullptr) {
                DecalEntry entry;
                entry.library = library;
                entry.id = material.shared.empty() ? material.name : material.shared;
                entry.composite = *image;
                entry.composite.uri.clear();
                entry.composite.file.clear();
                decalOf_[m] = static_cast<int>(decals_.size());
                decals_.push_back(std::move(entry));
            }
        }
    }

    // Uma tela por material com UV em todas as suas partes.
    canvasOf_.assign(materialCount, -1);
    int withoutUv = 0;
    std::map<std::pair<int, int>, int> sizes;
    std::size_t bytes = 0;
    for (std::size_t m = 0; m < materialCount; ++m) {
        bool used = false;
        bool hasUv = true;
        double surfaceArea = 0.0;
        double uvArea = 0.0;
        for (const lkz::asset::ModelPart& part : model_.parts) {
            if (part.material != static_cast<int>(m)) {
                continue;
            }
            used = true;
            const lkz::MeshData& mesh = part.mesh;
            if (mesh.uvs.size() != mesh.positions.size()) {
                hasUv = false;
                break;
            }
            const std::size_t count = lkz::asset::triangleCount(mesh);
            for (std::size_t t = 0; t < count; ++t) {
                const auto i = lkz::asset::triangleAt(mesh, t);
                surfaceArea += 0.5 * glm::length(glm::cross(mesh.positions[i[1]] - mesh.positions[i[0]],
                                                            mesh.positions[i[2]] - mesh.positions[i[0]]));
                const glm::vec2 a = mesh.uvs[i[1]] - mesh.uvs[i[0]];
                const glm::vec2 b = mesh.uvs[i[2]] - mesh.uvs[i[0]];
                uvArea += 0.5 * std::abs(static_cast<double>(a.x * b.y - a.y * b.x));
            }
        }
        if (!used || paletteOf[m] >= 0 || swatches[m] != 0 || decalOf_[m] >= 0) {
            continue;
        }
        if (!hasUv) {
            ++withoutUv;
            continue;
        }
        const lkz::asset::MaterialData& material = model_.materials[m];
        Canvas canvas;
        canvas.material = static_cast<int>(m);
        canvas.tint = material.baseColor;
        if (material.image >= 0 && material.image < static_cast<int>(model_.images.size()) &&
            !model_.images[static_cast<std::size_t>(material.image)].empty()) {
            canvas.source = &model_.images[static_cast<std::size_t>(material.image)];
            canvas.width = canvas.source->width;
            canvas.height = canvas.source->height;
        } else {
            if (uvArea <= 1e-12 || surfaceArea <= 1e-12) {
                ++withoutUv; // UV sem área: não há onde pintar
                continue;
            }
            // Só cor: tela nova com a densidade padrão (potência de 2, de 64 a 2048).
            const double side = texelsPerMeter * std::sqrt(surfaceArea / uvArea);
            int size = 64;
            while (size < side && size < 2048) {
                size *= 2;
            }
            canvas.width = size;
            canvas.height = size;
        }
        canvas.pixels.resize(static_cast<std::size_t>(canvas.width) * static_cast<std::size_t>(canvas.height) * 4);
        for (int y = 0; y < canvas.height; ++y) {
            for (int x = 0; x < canvas.width; ++x) {
                original(canvas, x, y, &canvas.pixels[(static_cast<std::size_t>(y) * canvas.width + x) * 4]);
            }
        }
        canvas.blocksX = (canvas.width + blockSize - 1) / blockSize;
        canvas.blocksY = (canvas.height + blockSize - 1) / blockSize;
        ++sizes[{canvas.width, canvas.height}];
        bytes += canvas.pixels.size() * 4 / 3;
        canvasOf_[m] = static_cast<int>(canvases_.size());
        canvases_.push_back(std::move(canvas));
    }

    // Destinos iniciais: único, ou o que o material gravado diz.
    buildFaceGeometry();
    faces_.assign(faceRef_.size(), Face{});
    int sharedFaces = 0;
    int colorFaces = 0;
    int decalFaces = 0;
    for (std::size_t p = 0; p < model_.parts.size(); ++p) {
        const lkz::asset::ModelPart& part = model_.parts[p];
        const auto m = static_cast<std::size_t>(part.material);
        const std::size_t count = lkz::asset::triangleCount(part.mesh);
        for (std::size_t t = 0; t < count; ++t) {
            Face& face = faces_[static_cast<std::size_t>(firstFace_[p]) + t];
            if (decalOf_[m] >= 0) {
                face.target = SurfaceTarget::Decal;
                face.material = decalOf_[m];
                const glm::vec4 tint = model_.materials[m].baseColor;
                if (glm::vec3{tint} != glm::vec3{1.0f}) {
                    face.colored = true;
                    face.color = {toByte(tint.r), toByte(tint.g), toByte(tint.b), 255};
                }
                ++decalFaces;
            } else if (paletteOf[m] >= 0) {
                face.target = SurfaceTarget::Shared;
                face.material = paletteOf[m];
                const glm::vec4 tint = model_.materials[m].baseColor;
                if (glm::vec3{tint} != glm::vec3{1.0f}) {
                    face.colored = true;
                    face.color = {toByte(tint.r), toByte(tint.g), toByte(tint.b), 255};
                }
                ++sharedFaces;
            } else if (swatches[m] != 0) {
                const lkz::asset::ImageData& image = model_.images[static_cast<std::size_t>(model_.materials[m].image)];
                const auto i = lkz::asset::triangleAt(part.mesh, t);
                glm::vec2 uv{0.0f};
                if (part.mesh.uvs.size() == part.mesh.positions.size()) {
                    uv = (part.mesh.uvs[i[0]] + part.mesh.uvs[i[1]] + part.mesh.uvs[i[2]]) / 3.0f;
                }
                const int x = wrap(static_cast<int>(std::floor(uv.x * static_cast<float>(image.width))), image.width);
                const int y = wrap(static_cast<int>(std::floor(uv.y * static_cast<float>(image.height))), image.height);
                face.target = SurfaceTarget::Color;
                face.colored = true;
                std::copy_n(&image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4], 4, face.color.data());
                ++colorFaces;
            }
        }
    }
    initialFaces_ = faces_;
    family_.resize(faces_.size());
    std::iota(family_.begin(), family_.end(), 0);
    partId_.resize(model_.parts.size());
    std::iota(partId_.begin(), partId_.end(), 0);
    nextPartId_ = static_cast<int>(model_.parts.size());
    stampOf_.assign(model_.parts.size(), nullptr);
    liftOpen_ = false;
    const int restoredStamps = restoreStamps(); // os carimbos gravados voltam a ser ajustáveis

    std::string list;
    for (auto it = sizes.rbegin(); it != sizes.rend(); ++it) {
        list += fmt::format("{}{}{}", list.empty() ? "" : ", ", sizeText(it->first.first, it->first.second),
                            it->second > 1 ? fmt::format(" ×{}", it->second) : "");
    }
    summary_ = fmt::format("{} triângulos · {} materiais · {} telas{} · ~{:.1f} MiB", model_.triangleCount(),
                           model_.materials.size(), canvases_.size(), list.empty() ? "" : " (" + list + ")",
                           static_cast<double>(bytes) / (1024.0 * 1024.0));
    if (withoutUv > 0) {
        summary_ += fmt::format(" · {} sem UV (só a cor)", withoutUv);
    }
    if (sharedFaces > 0 || colorFaces > 0) {
        summary_ += fmt::format(" · {} faces da paleta, {} de cor lisa", sharedFaces, colorFaces);
    }
    if (decalFaces > 0) {
        summary_ += fmt::format(" · {} triângulos de decalque", decalFaces);
    }
    if (restoredStamps > 0) {
        summary_ += fmt::format(" · {} carimbo{} ajustáve{}", restoredStamps, restoredStamps == 1 ? "" : "s",
                                restoredStamps == 1 ? "l" : "is");
    }
    // UV sobreposto: pintar uma face pinta as outras que dividem os mesmos texels.
    overlap_ = 0.0f;
    for (Canvas& canvas : canvases_) {
        canvas.overlap = lkz::asset::measureUvOverlap(model_, canvas.material, 256).ratio();
        overlap_ = std::max(overlap_, canvas.overlap);
    }
    needsUniqueUv_ = withoutUv > 0 || overlap_ > 0.01f;
}

void Document::buildFaceGeometry() {
    ++geometryVersion_;
    firstFace_.clear();
    faceRef_.clear();
    faceNormal_.clear();
    faceCenter_.clear();
    faceArea_.clear();
    neighbors_.clear();
    // Vértices soldados por posição (0,1 mm): as partes de um modelo costumam ter os
    // vértices repetidos nas emendas de UV e de material.
    std::map<std::tuple<long long, long long, long long>, int> welded;
    const auto weld = [&](const glm::vec3& p) {
        const auto key = std::make_tuple(std::llround(p.x * 1e4f), std::llround(p.y * 1e4f), std::llround(p.z * 1e4f));
        return welded.try_emplace(key, static_cast<int>(welded.size())).first->second;
    };
    std::map<std::pair<int, int>, std::vector<int>> edges;
    for (std::size_t p = 0; p < model_.parts.size(); ++p) {
        const lkz::MeshData& mesh = model_.parts[p].mesh;
        firstFace_.push_back(static_cast<int>(faceRef_.size()));
        const std::size_t count = lkz::asset::triangleCount(mesh);
        for (std::size_t t = 0; t < count; ++t) {
            const int f = static_cast<int>(faceRef_.size());
            const auto index = lkz::asset::triangleAt(mesh, t);
            const glm::vec3 a = mesh.positions[index[0]];
            const glm::vec3 b = mesh.positions[index[1]];
            const glm::vec3 c = mesh.positions[index[2]];
            const glm::vec3 n = glm::cross(b - a, c - a);
            const float length = glm::length(n);
            faceRef_.push_back({static_cast<int>(p), static_cast<int>(t)});
            faceNormal_.push_back(length > 1e-12f ? n / length : glm::vec3{0.0f, 1.0f, 0.0f});
            faceCenter_.push_back((a + b + c) / 3.0f);
            faceArea_.push_back(0.5f * length);
            const int ids[3] = {weld(a), weld(b), weld(c)};
            for (int k = 0; k < 3; ++k) {
                const int u = ids[k];
                const int v = ids[(k + 1) % 3];
                if (u != v) {
                    edges[{std::min(u, v), std::max(u, v)}].push_back(f);
                }
            }
        }
    }
    neighbors_.resize(faceRef_.size());
    for (const auto& [edge, list] : edges) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            for (std::size_t j = i + 1; j < list.size(); ++j) {
                neighbors_[static_cast<std::size_t>(list[i])].push_back(list[j]);
                neighbors_[static_cast<std::size_t>(list[j])].push_back(list[i]);
            }
        }
    }
}

int Document::faceIndex(const lkz::asset::TriangleRef& ref) const {
    if (ref.part < 0 || ref.part >= static_cast<int>(model_.parts.size()) || ref.triangle < 0) {
        return -1;
    }
    const auto count = static_cast<int>(lkz::asset::triangleCount(model_.parts[static_cast<std::size_t>(ref.part)].mesh));
    return ref.triangle < count ? firstFace_[static_cast<std::size_t>(ref.part)] + ref.triangle : -1;
}

int Document::materialOfFace(int face) const {
    return model_.parts[static_cast<std::size_t>(faceRef_[static_cast<std::size_t>(face)].part)].material;
}

int Document::canvasOfFace(int face) const {
    return canvasOf_[static_cast<std::size_t>(materialOfFace(face))];
}

float Document::faceOverlap(int face) const {
    if (face < 0 || face >= faceCount()) {
        return 0.0f;
    }
    const int canvas = canvasOfFace(face);
    return canvas >= 0 ? canvases_[static_cast<std::size_t>(canvas)].overlap : 0.0f;
}

Document::Look Document::lookOf(int face) const {
    const Face& state = faces_[static_cast<std::size_t>(face)];
    const bool hasPalette = palette_ != nullptr && state.material >= 0 && state.material < palette_->size();
    switch (state.target) {
    case SurfaceTarget::Shared: return hasPalette ? Look::Palette : Look::Original;
    case SurfaceTarget::Color: return Look::Color;
    case SurfaceTarget::Decal:
        return state.material >= 0 && state.material < decalCount() ? Look::Decal : Look::Original;
    default: break;
    }
    if (canvasOfFace(face) >= 0) {
        return Look::Canvas;
    }
    return hasPalette ? Look::Palette : state.colored ? Look::Color : Look::Original;
}

bool Document::coplanar(int seed, int face) const {
    const auto s = static_cast<std::size_t>(seed);
    const auto f = static_cast<std::size_t>(face);
    if (faceArea_[f] <= 1e-10f) {
        return false;
    }
    constexpr float cosTwoDegrees = 0.99939f;
    return glm::dot(faceNormal_[s], faceNormal_[f]) >= cosTwoDegrees &&
           std::abs(glm::dot(faceNormal_[s], faceCenter_[f] - faceCenter_[s])) <= 0.01f;
}

std::vector<int> Document::select(const lkz::asset::RayHit& hit, Selection mode) const {
    const int seed = faceIndex(hit.where);
    if (seed < 0) {
        return {};
    }
    std::vector<int> out;
    const int material = materialOfFace(seed);
    switch (mode) {
    case Selection::Triangle: out.push_back(seed); break;
    case Selection::Wall: {
        std::vector<char> seen(faces_.size(), 0);
        std::vector<int> stack{seed};
        seen[static_cast<std::size_t>(seed)] = 1;
        while (!stack.empty()) {
            const int f = stack.back();
            stack.pop_back();
            out.push_back(f);
            for (const int g : neighbors_[static_cast<std::size_t>(f)]) {
                if (seen[static_cast<std::size_t>(g)] == 0 && materialOfFace(g) == material && coplanar(seed, g)) {
                    seen[static_cast<std::size_t>(g)] = 1;
                    stack.push_back(g);
                }
            }
        }
        break;
    }
    case Selection::Plane:
        for (int f = 0; f < faceCount(); ++f) {
            if (f == seed || coplanar(seed, f)) {
                out.push_back(f);
            }
        }
        break;
    case Selection::Material:
        for (int f = 0; f < faceCount(); ++f) {
            if (materialOfFace(f) == material) {
                out.push_back(f);
            }
        }
        break;
    }
    std::sort(out.begin(), out.end());
    return out;
}

float Document::area(const std::vector<int>& faces) const {
    float total = 0.0f;
    for (const int f : faces) {
        total += faceArea_[static_cast<std::size_t>(f)];
    }
    return total;
}

std::array<std::uint8_t, 4> Document::sourceColor(int paletteMaterial, const glm::vec3& color, const glm::vec3& position,
                                                  int face, float texels, bool tint) const {
    if (palette_ != nullptr && paletteMaterial >= 0 && paletteMaterial < palette_->size()) {
        const PaletteMaterial& material = palette_->at(paletteMaterial);
        const glm::vec2 uv = surfaceMeters(position, faceNormal_[static_cast<std::size_t>(face)]) / material.metersPerRepeat;
        const glm::vec4 sample = palette_->sample(paletteMaterial, uv, texels);
        return toBytes(tint ? sample * glm::vec4{color, 1.0f} : sample);
    }
    return {toByte(color.r), toByte(color.g), toByte(color.b), 255};
}

template <class ColorAt>
void Document::paintFace(int face, bool keepAlpha, ColorAt&& colorAt) {
    const int canvasIndex = canvasOfFace(face);
    if (canvasIndex < 0) {
        return;
    }
    const Canvas& canvas = canvases_[static_cast<std::size_t>(canvasIndex)];
    const lkz::asset::TriangleRef ref = faceRef_[static_cast<std::size_t>(face)];
    const lkz::MeshData& mesh = model_.parts[static_cast<std::size_t>(ref.part)].mesh;
    const auto index = lkz::asset::triangleAt(mesh, static_cast<std::size_t>(ref.triangle));
    const glm::vec3 p[3] = {mesh.positions[index[0]], mesh.positions[index[1]], mesh.positions[index[2]]};
    const glm::vec2 uv[3] = {mesh.uvs[index[0]], mesh.uvs[index[1]], mesh.uvs[index[2]]};
    const glm::vec2 size{static_cast<float>(canvas.width), static_cast<float>(canvas.height)};
    const auto toUv = lkz::asset::SurfaceToUv::from(p, uv);
    const float texels =
        toUv ? std::max(glm::length(toUv->du) * size.x, glm::length(toUv->dv) * size.y) : texelsPerMeter;
    // UV que se repete (maior que a imagem): cada texel uma vez basta.
    const glm::vec2 lo = glm::min(uv[0], glm::min(uv[1], uv[2])) * size;
    const glm::vec2 hi = glm::max(uv[0], glm::max(uv[1], uv[2])) * size;
    const int x0 = static_cast<int>(std::floor(lo.x)) - 1;
    const int y0 = static_cast<int>(std::floor(lo.y)) - 1;
    const TexelRect clip{x0, y0, std::min(static_cast<int>(std::ceil(hi.x)) + 1, x0 + canvas.width + 2),
                         std::min(static_cast<int>(std::ceil(hi.y)) + 1, y0 + canvas.height + 2)};
    lkz::asset::forEachTexel(uv, canvas.width, canvas.height, 1.0f, clip, [&](int x, int y, const glm::vec3& b) {
        const glm::vec3 position = p[0] * b.x + p[1] * b.y + p[2] * b.z;
        const int tx = wrap(x, canvas.width);
        const int ty = wrap(y, canvas.height);
        paint(canvasIndex, tx, ty, 1.0f, colorAt(tx, ty, position, texels), keepAlpha);
    });
    addRect(canvases_[static_cast<std::size_t>(canvasIndex)].dirty, touched_);
    touched_ = {};
}

int Document::fill(const std::vector<int>& faces, const Fill& fill, int* withoutCanvas) {
    int skipped = 0;
    int applied = 0;
    if (faces.empty()) {
        if (withoutCanvas != nullptr) {
            *withoutCanvas = 0;
        }
        return 0;
    }
    beginStroke();
    for (const int f : faces) {
        if (f < 0 || f >= faceCount()) {
            continue;
        }
        const Face before = faces_[static_cast<std::size_t>(f)];
        Face next;
        const int canvasIndex = canvasOfFace(f);
        if (fill.restore) {
            next = initialFaces_[static_cast<std::size_t>(f)];
            paintFace(f, false, [&](int x, int y, const glm::vec3&, float) {
                std::array<std::uint8_t, 4> rgba{};
                original(canvases_[static_cast<std::size_t>(canvasIndex)], x, y, rgba.data());
                return rgba;
            });
        } else if (fill.target == SurfaceTarget::Shared) {
            if (palette_ == nullptr || fill.material < 0 || fill.material >= palette_->size()) {
                continue;
            }
            next.target = SurfaceTarget::Shared;
            next.material = fill.material;
            if (fill.tint) {
                next.colored = true;
                next.color = {toByte(fill.color.r), toByte(fill.color.g), toByte(fill.color.b), 255};
            }
        } else if (fill.target == SurfaceTarget::Color) {
            next.target = SurfaceTarget::Color;
            next.colored = true;
            next.color = {toByte(fill.color.r), toByte(fill.color.g), toByte(fill.color.b), 255};
        } else if (canvasIndex >= 0) {
            paintFace(f, true, [&](int, int, const glm::vec3& position, float texels) {
                return sourceColor(fill.material, fill.color, position, f, texels, fill.tint);
            });
        } else {
            // Sem tela: a face fica com a aparência pedida até o UV único assá-la.
            ++skipped;
            next.material = fill.material;
            next.colored = fill.material < 0 || fill.tint;
            next.color = {toByte(fill.color.r), toByte(fill.color.g), toByte(fill.color.b), 255};
        }
        ++applied;
        if (next != before) {
            current_.faces.push_back({f, before});
            faces_[static_cast<std::size_t>(f)] = next;
            geometryChanged_ = true;
        }
    }
    endStroke();
    if (withoutCanvas != nullptr) {
        *withoutCanvas = skipped;
    }
    return applied;
}

bool Document::takeGeometryChanged() {
    const bool changed = geometryChanged_;
    geometryChanged_ = false;
    return changed;
}

std::vector<std::array<std::uint8_t, 4>> Document::swatchColors() const {
    std::vector<std::array<std::uint8_t, 4>> colors;
    std::map<std::array<std::uint8_t, 4>, int> seen;
    for (int f = 0; f < faceCount(); ++f) {
        if (lookOf(f) == Look::Color) {
            const auto& color = faces_[static_cast<std::size_t>(f)].color;
            if (seen.try_emplace(color, static_cast<int>(colors.size())).second) {
                colors.push_back(color);
            }
        }
    }
    return colors;
}

std::vector<Document::Surface> Document::surfaces(const std::vector<int>& highlight) const {
    std::vector<char> lit(faces_.size(), 0);
    for (const int f : highlight) {
        if (f >= 0 && f < faceCount()) {
            lit[static_cast<std::size_t>(f)] = 1;
        }
    }
    const auto colors = swatchColors();
    std::map<std::array<std::uint8_t, 4>, int> colorIndex;
    for (std::size_t i = 0; i < colors.size(); ++i) {
        colorIndex.emplace(colors[i], static_cast<int>(i));
    }
    const SwatchLayout layout{colors.size()};

    // (tipo, índice, tinta, dois lados, realce) -> superfície
    std::map<std::tuple<int, int, std::uint32_t, bool, bool>, std::pair<Surface, MeshBuilder>> groups;
    for (std::size_t p = 0; p < model_.parts.size(); ++p) {
        const lkz::asset::ModelPart& part = model_.parts[p];
        const lkz::asset::MaterialData& material = model_.materials[static_cast<std::size_t>(part.material)];
        const bool hasUv = part.mesh.uvs.size() == part.mesh.positions.size();
        const std::size_t count = lkz::asset::triangleCount(part.mesh);
        for (std::size_t t = 0; t < count; ++t) {
            const int f = firstFace_[p] + static_cast<int>(t);
            const Face& face = faces_[static_cast<std::size_t>(f)];
            const auto index = lkz::asset::triangleAt(part.mesh, t);
            Surface::Kind kind = Surface::Kind::Material;
            int which = part.material;
            std::uint32_t tint = 0xFFFFFF;
            glm::vec2 uv[3] = {glm::vec2{0.0f}, glm::vec2{0.0f}, glm::vec2{0.0f}};
            switch (lookOf(f)) {
            case Look::Canvas:
            case Look::Original:
                for (int k = 0; k < 3 && hasUv; ++k) {
                    uv[k] = part.mesh.uvs[index[static_cast<std::size_t>(k)]];
                }
                break;
            case Look::Palette: {
                kind = Surface::Kind::Shared;
                which = face.material;
                tint = tintKey(face);
                const float meters = palette_->at(face.material).metersPerRepeat;
                for (int k = 0; k < 3; ++k) {
                    uv[k] = surfaceMeters(part.mesh.positions[index[static_cast<std::size_t>(k)]],
                                          faceNormal_[static_cast<std::size_t>(f)]) /
                            meters;
                }
                break;
            }
            case Look::Color:
                kind = Surface::Kind::Color;
                which = 0;
                uv[0] = uv[1] = uv[2] = layout.center(colorIndex[face.color]);
                break;
            case Look::Decal:
                kind = Surface::Kind::Decal;
                which = face.material;
                tint = tintKey(face);
                for (int k = 0; k < 3 && hasUv; ++k) {
                    uv[k] = part.mesh.uvs[index[static_cast<std::size_t>(k)]];
                }
                break;
            }
            const bool highlighted = lit[static_cast<std::size_t>(f)] != 0;
            auto& [surface, builder] = groups[{static_cast<int>(kind), which, tint, material.doubleSided, highlighted}];
            if (builder.mesh.positions.empty()) {
                surface.kind = kind;
                surface.index = which;
                surface.doubleSided = material.doubleSided;
                surface.highlighted = highlighted;
                const bool tinted = kind == Surface::Kind::Material && canvasOf_[static_cast<std::size_t>(part.material)] < 0;
                surface.color = tinted                          ? material.baseColor
                                : kind == Surface::Kind::Shared || kind == Surface::Kind::Decal ? tintOf(face)
                                                                                                : glm::vec4{1.0f};
            }
            builder.add(static_cast<int>(p), part.mesh, index, uv);
        }
    }
    std::vector<Surface> out;
    out.reserve(groups.size());
    for (auto& [key, group] : groups) {
        group.first.mesh = std::move(group.second.mesh);
        out.push_back(std::move(group.first));
    }
    return out;
}

bool Document::hasCanvas(int material) const {
    return material >= 0 && material < static_cast<int>(canvasOf_.size()) && canvasOf_[static_cast<std::size_t>(material)] >= 0;
}

lkz::TextureImage Document::canvasImage(int material) const {
    if (!hasCanvas(material)) {
        return {};
    }
    const Canvas& canvas = canvases_[static_cast<std::size_t>(canvasOf_[static_cast<std::size_t>(material)])];
    return lkz::TextureImage{canvas.width, canvas.height, canvas.pixels};
}

lkz::TextureImage Document::swatchImage() const {
    lkz::asset::ImageData image = swatchOf(swatchColors());
    return lkz::TextureImage{image.width, image.height, std::move(image.rgba)};
}

void Document::beginStroke() {
    if (stroking_) {
        endStroke();
    }
    stroking_ = true;
    current_ = Stroke{};
}

void Document::dab(const glm::vec3& center, const glm::vec3& eye, const Brush& brush) {
    if (!stroking_ || brush.radius <= 0.0f) {
        return;
    }
    const float radius = brush.radius;
    const float radiusSquared = radius * radius;
    raycaster_.near(center, radius, near_);
    for (const lkz::asset::TriangleRef& ref : near_) {
        const int f = faceIndex(ref);
        if (f < 0 || faces_[static_cast<std::size_t>(f)].target != SurfaceTarget::Unique) {
            continue; // compartilhadas e de cor lisa não têm tela
        }
        const lkz::asset::ModelPart& part = model_.parts[static_cast<std::size_t>(ref.part)];
        const int canvasIndex = canvasOf_[static_cast<std::size_t>(part.material)];
        if (canvasIndex < 0) {
            continue;
        }
        const Canvas& canvas = canvases_[static_cast<std::size_t>(canvasIndex)];
        const bool bothSides = model_.materials[static_cast<std::size_t>(part.material)].doubleSided;
        const lkz::MeshData& mesh = part.mesh;
        const auto index = lkz::asset::triangleAt(mesh, static_cast<std::size_t>(ref.triangle));
        const glm::vec3 p[3] = {mesh.positions[index[0]], mesh.positions[index[1]], mesh.positions[index[2]]};
        const glm::vec3 n[3] = {mesh.normals[index[0]], mesh.normals[index[1]], mesh.normals[index[2]]};
        const glm::vec2 uv[3] = {mesh.uvs[index[0]], mesh.uvs[index[1]], mesh.uvs[index[2]]};
        const auto toUv = lkz::asset::SurfaceToUv::from(p, uv);
        if (!toUv) {
            continue;
        }
        const glm::vec3 faceNormal = faceNormal_[static_cast<std::size_t>(f)];
        const float planeDistance = glm::dot(center - p[0], faceNormal);
        if (std::abs(planeDistance) > radius) {
            continue;
        }
        // A esfera corta o plano do triângulo num disco; só os texels dele interessam.
        const float discRadius = std::sqrt(radiusSquared - planeDistance * planeDistance);
        const glm::vec2 discUv = toUv->at(center - faceNormal * planeDistance);
        const glm::vec2 extent = discRadius * glm::vec2{glm::length(toUv->du), glm::length(toUv->dv)};
        const glm::vec2 size{static_cast<float>(canvas.width), static_cast<float>(canvas.height)};
        const float texels = std::max(glm::length(toUv->du) * size.x, glm::length(toUv->dv) * size.y);
        const glm::vec2 lo = (discUv - extent) * size;
        const glm::vec2 hi = (discUv + extent) * size;
        const TexelRect clip{static_cast<int>(std::floor(lo.x)) - 2, static_cast<int>(std::floor(lo.y)) - 2,
                             static_cast<int>(std::ceil(hi.x)) + 2, static_cast<int>(std::ceil(hi.y)) + 2};
        lkz::asset::forEachTexel(uv, canvas.width, canvas.height, 1.0f, clip, [&](int x, int y, const glm::vec3& b) {
            const glm::vec3 position = p[0] * b.x + p[1] * b.y + p[2] * b.z;
            const glm::vec3 offset = position - center;
            const float distanceSquared = glm::dot(offset, offset);
            if (distanceSquared > radiusSquared) {
                return;
            }
            const glm::vec3 normal = n[0] * b.x + n[1] * b.y + n[2] * b.z;
            if (!bothSides && glm::dot(normal, position - eye) >= 0.0f) {
                return; // de costas para a câmera: o outro lado de uma parede fina
            }
            const float t = std::sqrt(distanceSquared) / radius;
            const float falloff = t <= brush.hardness ? 1.0f : 1.0f - glm::smoothstep(brush.hardness, 1.0f, t);
            const int tx = wrap(x, canvas.width);
            const int ty = wrap(y, canvas.height);
            std::array<std::uint8_t, 4> target{};
            if (brush.erase) {
                original(canvas, tx, ty, target.data());
            } else {
                target = sourceColor(brush.material, brush.color, position, f, texels, brush.tint);
            }
            paint(canvasIndex, tx, ty, falloff * brush.opacity, target, !brush.erase);
        });
        addRect(canvases_[static_cast<std::size_t>(canvasIndex)].dirty, touched_);
        touched_ = {};
    }
}

Document::SavedBlock& Document::saveBlock(int canvasIndex, int block) {
    Canvas& canvas = canvases_[static_cast<std::size_t>(canvasIndex)];
    int& slot = canvas.strokeBlock[static_cast<std::size_t>(block)];
    if (slot < 0) {
        slot = static_cast<int>(current_.blocks.size());
        SavedBlock saved;
        saved.canvas = canvasIndex;
        saved.block = block;
        saved.pixels.assign(static_cast<std::size_t>(blockSize * blockSize * 4), 0);
        const TexelRect rect = blockRect(canvas, block);
        const auto rowBytes = static_cast<std::size_t>(rect.x1 - rect.x0) * 4;
        for (int y = rect.y0; y < rect.y1; ++y) {
            std::copy_n(&canvas.pixels[(static_cast<std::size_t>(y) * canvas.width + rect.x0) * 4], rowBytes,
                        &saved.pixels[static_cast<std::size_t>((y - rect.y0) * blockSize) * 4]);
        }
        current_.blocks.push_back(std::move(saved));
    }
    return current_.blocks[static_cast<std::size_t>(slot)];
}

TexelRect Document::blockRect(const Canvas& canvas, int block) const {
    const int x0 = (block % canvas.blocksX) * blockSize;
    const int y0 = (block / canvas.blocksX) * blockSize;
    return {x0, y0, std::min(x0 + blockSize, canvas.width), std::min(y0 + blockSize, canvas.height)};
}

void Document::paint(int canvasIndex, int x, int y, float alpha, const std::array<std::uint8_t, 4>& target, bool keepAlpha) {
    Canvas& canvas = canvases_[static_cast<std::size_t>(canvasIndex)];
    if (canvas.strokeAlpha.empty()) {
        canvas.strokeAlpha.assign(static_cast<std::size_t>(canvas.width) * static_cast<std::size_t>(canvas.height), 0);
        canvas.strokeBlock.assign(static_cast<std::size_t>(canvas.blocksX * canvas.blocksY), -1);
    }
    const std::uint8_t coverage = toByte(alpha);
    const std::size_t texel = static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas.width) + static_cast<std::size_t>(x);
    // A tinta de um traço não se acumula onde as pinceladas se sobrepõem: vale a maior
    // cobertura, sempre misturada com o que havia antes do traço.
    if (coverage <= canvas.strokeAlpha[texel]) {
        return;
    }
    canvas.strokeAlpha[texel] = coverage;
    const int block = (y / blockSize) * canvas.blocksX + x / blockSize;
    const SavedBlock& saved = saveBlock(canvasIndex, block);
    const std::uint8_t* before =
        &saved.pixels[static_cast<std::size_t>(((y % blockSize) * blockSize + x % blockSize) * 4)];
    std::array<std::uint8_t, 4> goal = target;
    if (keepAlpha) {
        goal[3] = before[3]; // a cor não muda a transparência (folhas recortadas continuam recortadas)
    }
    const float weight = static_cast<float>(coverage) / 255.0f;
    std::uint8_t* pixel = &canvas.pixels[texel * 4];
    for (int k = 0; k < 4; ++k) {
        pixel[k] = static_cast<std::uint8_t>(std::lround(before[k] + (goal[static_cast<std::size_t>(k)] - before[k]) * weight));
    }
    touched_ = unite(touched_, TexelRect{x, y, x + 1, y + 1});
}

void Document::endStroke() {
    if (!stroking_) {
        return;
    }
    stroking_ = false;
    for (const SavedBlock& saved : current_.blocks) {
        Canvas& canvas = canvases_[static_cast<std::size_t>(saved.canvas)];
        const TexelRect rect = blockRect(canvas, saved.block);
        for (int y = rect.y0; y < rect.y1; ++y) {
            std::fill_n(&canvas.strokeAlpha[static_cast<std::size_t>(y) * canvas.width + rect.x0], rect.x1 - rect.x0, 0);
        }
        canvas.strokeBlock[static_cast<std::size_t>(saved.block)] = -1;
        addRect(canvas.finish, rect);
    }
    if (current_.blocks.empty() && current_.faces.empty()) {
        return;
    }
    pushUndo(std::move(current_));
    current_ = Stroke{};
}

void Document::pushUndo(Stroke stroke) {
    liftOpen_ = false;
    undo_.push_back(std::move(stroke));
    redo_.clear();
    ++revision_;
    std::size_t total = 0;
    for (const Stroke& saved : undo_) {
        total += saved.bytes();
    }
    while (total > undoBudget && undo_.size() > 1) {
        total -= undo_.front().bytes();
        undo_.erase(undo_.begin());
    }
}

namespace {

/// Vértice de um pedaço de triângulo: posição nos eixos do carimbo (u, v) e baricêntricas
/// no triângulo original.
struct ClipVertex {
    glm::vec2 p{0.0f};
    glm::vec3 b{0.0f};
};
using Polygon = std::vector<ClipVertex>;

/// A parte de `polygon` onde sign * (p[axis] - limit) >= 0 (Sutherland–Hodgman, um lado).
/// A ordem dos vértices (e o sentido do polígono) não muda. Um vértice em cima da linha
/// não ganha uma cópia (ela viraria um triângulo de área zero no leque).
Polygon clipSide(const Polygon& polygon, int axis, float limit, float sign) {
    Polygon out;
    const std::size_t n = polygon.size();
    for (std::size_t i = 0; i < n; ++i) {
        const ClipVertex& a = polygon[i];
        const ClipVertex& b = polygon[(i + 1) % n];
        const float da = sign * (a.p[axis] - limit);
        const float db = sign * (b.p[axis] - limit);
        if (da >= 0.0f) {
            out.push_back(a);
        }
        if ((da > 0.0f && db < 0.0f) || (da < 0.0f && db > 0.0f)) {
            const float t = da / (da - db);
            out.push_back({glm::mix(a.p, b.p, t), glm::mix(a.b, b.b, t)});
        }
    }
    // Vizinhos repetidos (o corte encostou num vértice): um só.
    Polygon unique;
    for (const ClipVertex& v : out) {
        if (unique.empty() || glm::length(v.p - unique.back().p) > 1e-6f) {
            unique.push_back(v);
        }
    }
    while (unique.size() > 1 && glm::length(unique.front().p - unique.back().p) <= 1e-6f) {
        unique.pop_back();
    }
    return unique;
}

/// Um triângulo do leque com área (vértices em linha não viram triângulo).
bool fanArea(const ClipVertex& a, const ClipVertex& b, const ClipVertex& c) {
    const glm::vec2 u = b.p - a.p;
    const glm::vec2 v = c.p - a.p;
    return std::abs(u.x * v.y - u.y * v.x) > 1e-10f;
}

float polygonArea(const Polygon& polygon) {
    float twice = 0.0f;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const glm::vec2& a = polygon[i].p;
        const glm::vec2& b = polygon[(i + 1) % polygon.size()].p;
        twice += a.x * b.y - a.y * b.x;
    }
    return std::abs(twice) * 0.5f;
}

/// (u, v) no retângulo -> UV da imagem do decalque, girada em quartos de volta (horário).
glm::vec2 decalUv(const DecalPlacement& placement, const glm::vec2& p) {
    const glm::vec2 ab = (p - placement.origin) / placement.size;
    switch (placement.quarterTurns & 3) {
    case 1: return {ab.y, 1.0f - ab.x};
    case 2: return {1.0f - ab.x, 1.0f - ab.y};
    case 3: return {1.0f - ab.y, ab.x};
    default: return ab;
    }
}

} // namespace

float neutralWeight(const glm::vec3& rgb) {
    const float high = std::max(rgb.r, std::max(rgb.g, rgb.b));
    const float low = std::min(rgb.r, std::min(rgb.g, rgb.b));
    const float saturation = high > 1e-4f ? (high - low) / high : 0.0f;
    return 1.0f - glm::smoothstep(0.08f, 0.25f, saturation);
}

glm::vec3 tintPixel(const glm::vec3& rgb, DecalTint mode, const glm::vec3& tint) {
    switch (mode) {
    case DecalTint::Multiply: return rgb * tint;
    case DecalTint::WhiteOnly: return rgb * glm::mix(glm::vec3{1.0f}, tint, neutralWeight(rgb));
    case DecalTint::None: break;
    }
    return rgb;
}

lkz::asset::ImageData tintedImage(const lkz::asset::ImageData& image, DecalTint mode, const glm::vec3& tint) {
    lkz::asset::ImageData out = image;
    out.uri.clear();
    out.file.clear();
    for (std::size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
        const glm::vec3 rgb = glm::vec3{out.rgba[i], out.rgba[i + 1], out.rgba[i + 2]} / 255.0f;
        const glm::vec3 color = tintPixel(rgb, mode, tint);
        out.rgba[i] = toByte(color.r);
        out.rgba[i + 1] = toByte(color.g);
        out.rgba[i + 2] = toByte(color.b);
    }
    return out;
}

Document::Origin Document::reshaped(const Origin& origin, const std::array<glm::vec3, 3>& b) {
    Origin out = origin;
    for (std::size_t k = 0; k < 3; ++k) {
        out.uv[k] = origin.uv[0] * b[k].x + origin.uv[1] * b[k].y + origin.uv[2] * b[k].z;
    }
    if (origin.under) {
        out.under = std::make_shared<const Origin>(reshaped(*origin.under, b));
    }
    return out;
}

lkz::MeshData Document::decalQuad(const DecalPlacement& placement, float lift) const {
    lkz::MeshData quad;
    if (placement.face < 0 || placement.face >= faceCount()) {
        return quad;
    }
    const glm::vec3 normal = faceNormal_[static_cast<std::size_t>(placement.face)];
    const SurfaceAxes axes = surfaceAxes(normal);
    const float plane = glm::dot(normal, faceCenter_[static_cast<std::size_t>(placement.face)]) + lift;
    const glm::vec2 lo = placement.origin;
    const glm::vec2 hi = placement.origin + placement.size;
    for (const glm::vec2 corner : {lo, glm::vec2{hi.x, lo.y}, hi, glm::vec2{lo.x, hi.y}}) {
        quad.positions.push_back(normal * plane + axes.u * corner.x + axes.v * corner.y);
        quad.normals.push_back(normal);
        quad.uvs.push_back(decalUv(placement, corner));
    }
    // u x v = -normal: no plano (u, v) o sentido é o contrário do da face.
    quad.indices = {0, 2, 1, 0, 3, 2};
    return quad;
}

int Document::stamp(const DecalPlacement& placement) {
    if (decalLibrary_ == nullptr || placement.decal < 0 || placement.decal >= decalLibrary_->size() ||
        placement.face < 0 || placement.face >= faceCount() || placement.size.x <= 1e-4f || placement.size.y <= 1e-4f) {
        return 0;
    }
    // Depois de um lift, este carimbo completa o passo de desfazer dele.
    const bool joining = liftOpen_;
    liftOpen_ = false;
    endStroke();
    const int seed = placement.face;
    const SurfaceAxes axes = surfaceAxes(faceNormal_[static_cast<std::size_t>(seed)]);
    const glm::vec2 lo = placement.origin;
    const glm::vec2 hi = placement.origin + placement.size;
    const auto local = [&](const glm::vec3& p) { return glm::vec2{glm::dot(p, axes.u), glm::dot(p, axes.v)}; };

    // As faces do plano da clicada que encostam no retângulo.
    std::vector<char> touched(faces_.size(), 0);
    std::vector<int> cut;
    for (int f = 0; f < faceCount(); ++f) {
        if (f != seed && !coplanar(seed, f)) {
            continue;
        }
        const lkz::asset::TriangleRef ref = faceRef_[static_cast<std::size_t>(f)];
        const lkz::MeshData& mesh = model_.parts[static_cast<std::size_t>(ref.part)].mesh;
        const auto index = lkz::asset::triangleAt(mesh, static_cast<std::size_t>(ref.triangle));
        glm::vec2 low{1e30f};
        glm::vec2 high{-1e30f};
        for (const std::uint32_t i : index) {
            low = glm::min(low, local(mesh.positions[i]));
            high = glm::max(high, local(mesh.positions[i]));
        }
        if (low.x < hi.x && high.x > lo.x && low.y < hi.y && high.y > lo.y) {
            touched[static_cast<std::size_t>(f)] = 1;
            cut.push_back(f);
        }
    }
    if (cut.empty()) {
        return 0;
    }
    const PaletteMaterial& library = decalLibrary_->at(placement.decal);
    const bool tinting = placement.tintMode != DecalTint::None && placement.tint != glm::vec3{1.0f};
    // Com transparência, cada carimbo tem a sua imagem (composta abaixo, já com a cor). Só no
    // branco, uma imagem por cor. Senão, todos os carimbos do decalque dividem a da
    // biblioteca; multiplicada, a cor vai na face (no .glb, na cor do material).
    const int entry = library.transparent ? decalCount()
                      : tinting && placement.tintMode == DecalTint::WhiteOnly
                          ? whiteOnlyDecal(placement.decal, placement.tint)
                          : libraryDecal(placement.decal);
    Face decalFace{SurfaceTarget::Decal, entry, false, {255, 255, 255, 255}};
    if (tinting && !library.transparent && placement.tintMode == DecalTint::Multiply) {
        decalFace.colored = true;
        decalFace.color = {toByte(placement.tint.r), toByte(placement.tint.g), toByte(placement.tint.b), 255};
    }

    // Corta: os pedaços de fora ficam na parte (com o UV e o destino de antes); os de dentro
    // vão para uma parte nova do decalque, com o UV no retângulo da imagem.
    std::vector<lkz::asset::ModelPart> parts = model_.parts;
    std::vector<Face> faces;
    std::vector<Face> initial;
    std::vector<int> families;
    std::vector<StampRef> records = stampOf_;
    lkz::asset::ModelPart decalPart;
    decalPart.name = "decal_" + library.id;
    std::vector<Face> decalFaces;
    std::vector<int> decalFamilies;
    StampRecord decalRecord;
    decalRecord.placement = placement;
    decalRecord.placement.face = -1;
    const struct Side {
        int axis;
        float limit;
        float sign;
    } sides[4] = {{0, lo.x, 1.0f}, {0, hi.x, -1.0f}, {1, lo.y, 1.0f}, {1, hi.y, -1.0f}};
    // Profundidade: as arestas dos pedaços de dentro que caem na borda do retângulo são as
    // bordas do buraco na parede; cada uma vira uma parede do vão.
    struct SideEdge {
        glm::vec3 a{0.0f};
        glm::vec3 b{0.0f};
        glm::vec2 uvA{0.0f};
        glm::vec2 uvB{0.0f};
        Face face;
        Face initial;
        int family = -1;
        int material = 0;
    };
    std::vector<SideEdge> sideEdges;
    const bool relief = std::abs(placement.depth) > 1e-4f;
    const auto onBorder = [&](const glm::vec2& a, const glm::vec2& b) {
        constexpr float tolerance = 1e-4f;
        return std::any_of(std::begin(sides), std::end(sides), [&](const Side& side) {
            return std::abs(a[side.axis] - side.limit) < tolerance && std::abs(b[side.axis] - side.limit) < tolerance;
        });
    };
    for (std::size_t p = 0; p < parts.size(); ++p) {
        lkz::MeshData& mesh = parts[p].mesh;
        const bool hasUv = mesh.uvs.size() == mesh.positions.size();
        const bool hasNormals = mesh.normals.size() == mesh.positions.size();
        const std::size_t count = lkz::asset::triangleCount(mesh);
        const StampRecord* record = stampOf_[p].get(); // a parte é um carimbo: as origens seguem os pedaços
        std::vector<Origin> origins;
        std::vector<std::uint32_t> kept;
        kept.reserve(count * 3);
        for (std::size_t t = 0; t < count; ++t) {
            const auto f = static_cast<std::size_t>(firstFace_[p]) + t;
            const auto index = lkz::asset::triangleAt(mesh, t);
            const auto keep = [&]() {
                kept.insert(kept.end(), index.begin(), index.end());
                faces.push_back(faces_[f]);
                initial.push_back(initialFaces_[f]);
                families.push_back(family_[f]);
                if (record != nullptr) {
                    origins.push_back(record->origins[t]);
                }
            };
            if (touched[f] == 0) {
                keep();
                continue;
            }
            Polygon inside = {{local(mesh.positions[index[0]]), {1.0f, 0.0f, 0.0f}},
                              {local(mesh.positions[index[1]]), {0.0f, 1.0f, 0.0f}},
                              {local(mesh.positions[index[2]]), {0.0f, 0.0f, 1.0f}}};
            std::vector<Polygon> outside;
            for (const Side& side : sides) {
                Polygon out = clipSide(inside, side.axis, side.limit, -side.sign);
                if (out.size() >= 3 && polygonArea(out) > 1e-8f) {
                    outside.push_back(std::move(out));
                }
                inside = clipSide(inside, side.axis, side.limit, side.sign);
            }
            if (inside.size() < 3 || polygonArea(inside) <= 1e-8f) {
                keep(); // só encostou na borda
                continue;
            }
            // Vértice novo pelas baricêntricas no triângulo original.
            const glm::vec3 pos[3] = {mesh.positions[index[0]], mesh.positions[index[1]], mesh.positions[index[2]]};
            glm::vec3 nor[3] = {faceNormal_[f], faceNormal_[f], faceNormal_[f]};
            glm::vec2 tex[3] = {glm::vec2{0.0f}, glm::vec2{0.0f}, glm::vec2{0.0f}};
            for (int k = 0; k < 3; ++k) {
                if (hasNormals) {
                    nor[k] = mesh.normals[index[static_cast<std::size_t>(k)]];
                }
                if (hasUv) {
                    tex[k] = mesh.uvs[index[static_cast<std::size_t>(k)]];
                }
            }
            const auto add = [&](lkz::MeshData& to, const ClipVertex& v, std::optional<glm::vec2> uv) {
                const glm::vec3 n = nor[0] * v.b.x + nor[1] * v.b.y + nor[2] * v.b.z;
                const float length = glm::length(n);
                to.positions.push_back(pos[0] * v.b.x + pos[1] * v.b.y + pos[2] * v.b.z);
                to.normals.push_back(length > 1e-12f ? n / length : faceNormal_[f]);
                to.uvs.push_back(uv.value_or(tex[0] * v.b.x + tex[1] * v.b.y + tex[2] * v.b.z));
                return static_cast<std::uint32_t>(to.positions.size() - 1);
            };
            // Leque: os pedaços são convexos e mantêm o sentido do triângulo.
            for (const Polygon& piece : outside) {
                const std::uint32_t first = add(mesh, piece[0], std::nullopt);
                std::uint32_t previous = add(mesh, piece[1], std::nullopt);
                for (std::size_t i = 2; i < piece.size(); ++i) {
                    const std::uint32_t next = add(mesh, piece[i], std::nullopt);
                    if (!fanArea(piece[0], piece[i - 1], piece[i])) {
                        previous = next;
                        continue;
                    }
                    kept.insert(kept.end(), {first, previous, next});
                    faces.push_back(faces_[f]);
                    initial.push_back(initialFaces_[f]);
                    families.push_back(family_[f]);
                    if (record != nullptr) {
                        origins.push_back(reshaped(record->origins[t], {piece[0].b, piece[i - 1].b, piece[i].b}));
                    }
                    previous = next;
                }
            }
            const std::uint32_t first = add(decalPart.mesh, inside[0], decalUv(placement, inside[0].p));
            std::uint32_t previous = add(decalPart.mesh, inside[1], decalUv(placement, inside[1].p));
            for (std::size_t i = 2; i < inside.size(); ++i) {
                const std::uint32_t next = add(decalPart.mesh, inside[i], decalUv(placement, inside[i].p));
                if (!fanArea(inside[0], inside[i - 1], inside[i])) {
                    previous = next;
                    continue;
                }
                decalPart.mesh.indices.insert(decalPart.mesh.indices.end(), {first, previous, next});
                decalFaces.push_back(decalFace);
                decalFamilies.push_back(family_[f]);
                const std::array<glm::vec3, 3> b{inside[0].b, inside[i - 1].b, inside[i].b};
                Origin origin;
                origin.part = partId_[p];
                origin.family = family_[f];
                origin.face = faces_[f];
                origin.initial = initialFaces_[f];
                for (std::size_t k = 0; k < 3; ++k) {
                    origin.uv[k] = tex[0] * b[k].x + tex[1] * b[k].y + tex[2] * b[k].z;
                }
                if (record != nullptr) {
                    origin.under = std::make_shared<const Origin>(reshaped(record->origins[t], b));
                }
                decalRecord.origins.push_back(std::move(origin));
                previous = next;
            }
            if (relief) {
                const auto at = [&](const glm::vec3& b) { return pos[0] * b.x + pos[1] * b.y + pos[2] * b.z; };
                const auto uvAt = [&](const glm::vec3& b) { return tex[0] * b.x + tex[1] * b.y + tex[2] * b.z; };
                for (std::size_t i = 0; i < inside.size(); ++i) {
                    const ClipVertex& a = inside[i];
                    const ClipVertex& b = inside[(i + 1) % inside.size()];
                    if (glm::length(b.p - a.p) > 1e-5f && onBorder(a.p, b.p)) {
                        sideEdges.push_back({at(a.b), at(b.b), uvAt(a.b), uvAt(b.b), faces_[f], initialFaces_[f], family_[f],
                                             parts[p].material});
                    }
                }
            }
        }
        if (record != nullptr) {
            auto copy = std::make_shared<StampRecord>(*record);
            copy->origins = std::move(origins);
            records[p] = std::move(copy);
        }
        if (!hasUv) {
            mesh.uvs.clear(); // a parte não tinha UV: continua sem
        }
        setTriangles(mesh, std::move(kept));
    }
    if (decalFaces.empty()) {
        return 0;
    }

    // Decalque com transparência: a imagem dele sobre o que as faces mostravam (antes do
    // corte, que ainda é o estado do documento).
    if (library.transparent) {
        DecalEntry composite;
        composite.library = placement.decal;
        composite.id = library.id;
        composite.composite = library.mips.front();
        lkz::asset::ImageData& image = composite.composite;
        image.uri.clear();
        image.file.clear();
        const lkz::asset::ImageData& source = library.mips.front();
        for (const int f : cut) {
            const lkz::asset::TriangleRef ref = faceRef_[static_cast<std::size_t>(f)];
            const lkz::MeshData& mesh = model_.parts[static_cast<std::size_t>(ref.part)].mesh;
            const auto index = lkz::asset::triangleAt(mesh, static_cast<std::size_t>(ref.triangle));
            const glm::vec3 pos[3] = {mesh.positions[index[0]], mesh.positions[index[1]], mesh.positions[index[2]]};
            const glm::vec2 uv[3] = {decalUv(placement, local(pos[0])), decalUv(placement, local(pos[1])),
                                     decalUv(placement, local(pos[2]))};
            lkz::asset::forEachTexel(uv, image.width, image.height, 1.0f, TexelRect{0, 0, image.width, image.height},
                                     [&](int x, int y, const glm::vec3& b) {
                                         const glm::vec4 under = lookColor(f, pos[0] * b.x + pos[1] * b.y + pos[2] * b.z, b);
                                         const std::size_t at = (static_cast<std::size_t>(y) * image.width + x) * 4;
                                         const float alpha = static_cast<float>(source.rgba[at + 3]) / 255.0f;
                                         const glm::vec3 over = tintPixel(
                                             glm::vec3{source.rgba[at], source.rgba[at + 1], source.rgba[at + 2]} / 255.0f,
                                             placement.tintMode, placement.tint);
                                         for (std::size_t c = 0; c < 3; ++c) {
                                             const auto k = static_cast<int>(c);
                                             image.rgba[at + c] = toByte(over[k] * alpha + under[k] * (1.0f - alpha));
                                         }
                                     });
        }
        for (std::size_t i = 3; i < image.rgba.size(); i += 4) {
            image.rgba[i] = 255;
        }
        decals_.push_back(std::move(composite));
    }

    // Material da parte nova: um por decalque (e por lado), dividido pelos carimbos.
    std::shared_ptr<Geometry> before = joining ? nullptr : snapshot();
    const bool doubleSided = model_.materials[static_cast<std::size_t>(materialOfFace(seed))].doubleSided;
    int material = -1;
    for (std::size_t m = 0; m < model_.materials.size(); ++m) {
        if (decalOf_[m] == entry && model_.materials[m].doubleSided == doubleSided) {
            material = static_cast<int>(m);
        }
    }
    if (material < 0) {
        lkz::asset::MaterialData data;
        data.name = "decal_" + library.id;
        data.target = SurfaceTarget::Decal;
        data.shared = library.id;
        data.doubleSided = doubleSided;
        material = static_cast<int>(model_.materials.size());
        model_.materials.push_back(std::move(data));
        canvasOf_.push_back(-1);
        decalOf_.push_back(entry);
    }
    decalPart.material = material;
    const glm::vec3 normal = faceNormal_[static_cast<std::size_t>(seed)];
    const glm::vec3 offset = relief ? normal * placement.depth : glm::vec3{0.0f};
    for (glm::vec3& position : decalPart.mesh.positions) {
        position += offset;
    }
    decalRecord.offset = offset;
    const int created = static_cast<int>(decalFaces.size());
    parts.push_back(std::move(decalPart));
    faces.insert(faces.end(), decalFaces.begin(), decalFaces.end());
    initial.insert(initial.end(), decalFaces.begin(), decalFaces.end());
    families.insert(families.end(), decalFamilies.begin(), decalFamilies.end());
    records.push_back(std::make_shared<const StampRecord>(decalRecord));
    const int decalId = nextPartId_++;
    partId_.push_back(decalId);
    lastStampId_ = decalId;

    // Paredes do vão: uma parte por material das paredes cortadas, com o destino delas. O
    // UV é o da borda esticado (no Compartilhado, a projeção em metros já fica certa).
    struct SidePart {
        lkz::asset::ModelPart part;
        std::vector<Face> faces;
        std::vector<Face> initial;
        std::vector<int> families;
    };
    std::map<int, SidePart> sideParts;
    for (const SideEdge& edge : sideEdges) {
        SidePart& side = sideParts[edge.material];
        lkz::MeshData& mesh = side.part.mesh;
        // De fora do retângulo: à direita da aresta (os pedaços seguem o sentido da face).
        const glm::vec3 outward = glm::cross(edge.b - edge.a, normal);
        const glm::vec3 facing = placement.depth < 0.0f ? -outward : outward; // afundado: o vão olha para dentro
        const float length = glm::length(facing);
        if (length <= 1e-8f) {
            continue;
        }
        const glm::vec3 corners[4] = {edge.a, edge.b, edge.b + offset, edge.a + offset};
        const glm::vec2 uvs[4] = {edge.uvA, edge.uvB, edge.uvB, edge.uvA};
        const auto base = static_cast<std::uint32_t>(mesh.positions.size());
        for (int k = 0; k < 4; ++k) {
            mesh.positions.push_back(corners[k]);
            mesh.normals.push_back(facing / length);
            mesh.uvs.push_back(uvs[k]);
        }
        const bool forward = glm::dot(glm::cross(corners[1] - corners[0], corners[2] - corners[0]), facing) > 0.0f;
        if (forward) {
            mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        } else {
            mesh.indices.insert(mesh.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
        }
        for (int k = 0; k < 2; ++k) {
            side.faces.push_back(edge.face);
            side.initial.push_back(edge.initial);
            side.families.push_back(edge.family);
        }
    }
    for (auto& [sideMaterial, side] : sideParts) {
        const std::size_t count = side.faces.size();
        if (count == 0) {
            continue;
        }
        side.part.name = "vao_" + library.id;
        side.part.material = sideMaterial;
        auto record = std::make_shared<StampRecord>();
        record->placement = decalRecord.placement;
        record->origins.assign(count, Origin{}); // não voltam a lugar nenhum: saem com o carimbo
        record->owner = decalId;
        parts.push_back(std::move(side.part));
        faces.insert(faces.end(), side.faces.begin(), side.faces.end());
        initial.insert(initial.end(), side.initial.begin(), side.initial.end());
        families.insert(families.end(), side.families.begin(), side.families.end());
        records.push_back(std::move(record));
        partId_.push_back(nextPartId_++);
    }
    model_.parts = std::move(parts);
    faces_ = std::move(faces);
    initialFaces_ = std::move(initial);
    family_ = std::move(families);
    stampOf_ = std::move(records);
    buildFaceGeometry();
    raycaster_ = lkz::asset::MeshRaycaster{model_};
    geometryChanged_ = true;
    if (joining) {
        ++revision_; // o passo de desfazer é o do lift
    } else {
        Stroke stroke;
        stroke.geometry = std::move(before);
        pushUndo(std::move(stroke));
    }
    return created;
}

void Document::swapStroke(Stroke& stroke) {
    for (SavedBlock& saved : stroke.blocks) {
        Canvas& canvas = canvases_[static_cast<std::size_t>(saved.canvas)];
        const TexelRect rect = blockRect(canvas, saved.block);
        const auto rowBytes = static_cast<std::size_t>(rect.x1 - rect.x0) * 4;
        for (int y = rect.y0; y < rect.y1; ++y) {
            std::swap_ranges(&canvas.pixels[(static_cast<std::size_t>(y) * canvas.width + rect.x0) * 4],
                             &canvas.pixels[(static_cast<std::size_t>(y) * canvas.width + rect.x0) * 4] + rowBytes,
                             &saved.pixels[static_cast<std::size_t>((y - rect.y0) * blockSize) * 4]);
        }
        addRect(canvas.finish, rect);
    }
    // Ao contrário: no mesmo traço, a primeira mudança de uma face guarda o estado mais antigo.
    for (auto it = stroke.faces.rbegin(); it != stroke.faces.rend(); ++it) {
        std::swap(faces_[static_cast<std::size_t>(it->face)], it->state);
        geometryChanged_ = true;
    }
    if (stroke.geometry) {
        swapGeometry(*stroke.geometry);
    }
}

std::size_t Document::Geometry::bytes() const {
    std::size_t total = (faces.size() + initialFaces.size()) * sizeof(Face) + family.size() * sizeof(int);
    for (const StampRef& stamp : stampOf) {
        total += stamp ? stamp->origins.size() * sizeof(Origin) : 0;
    }
    for (const lkz::asset::ModelPart& part : parts) {
        total += part.mesh.positions.size() * (sizeof(glm::vec3) * 2 + sizeof(glm::vec2)) +
                 part.mesh.indices.size() * sizeof(std::uint32_t);
    }
    return total;
}

void Document::swapGeometry(Geometry& other) {
    std::swap(model_.parts, other.parts);
    std::swap(model_.materials, other.materials);
    std::swap(canvasOf_, other.canvasOf);
    std::swap(decalOf_, other.decalOf);
    std::swap(faces_, other.faces);
    std::swap(initialFaces_, other.initialFaces);
    std::swap(partId_, other.partId);
    std::swap(stampOf_, other.stampOf);
    std::swap(family_, other.family);
    buildFaceGeometry();
    raycaster_ = lkz::asset::MeshRaycaster{model_};
    geometryChanged_ = true;
}

std::shared_ptr<Document::Geometry> Document::snapshot() const {
    auto geometry = std::make_shared<Geometry>();
    geometry->parts = model_.parts;
    geometry->materials = model_.materials;
    geometry->canvasOf = canvasOf_;
    geometry->decalOf = decalOf_;
    geometry->faces = faces_;
    geometry->initialFaces = initialFaces_;
    geometry->partId = partId_;
    geometry->stampOf = stampOf_;
    geometry->family = family_;
    return geometry;
}

std::optional<DecalPlacement> Document::stampAt(int face) const {
    if (face < 0 || face >= faceCount()) {
        return std::nullopt;
    }
    const StampRef& stamp = stampOf_[static_cast<std::size_t>(faceRef_[static_cast<std::size_t>(face)].part)];
    if (!stamp || stamp->owner >= 0 || lookOf(face) != Look::Decal) {
        return std::nullopt;
    }
    DecalPlacement placement = stamp->placement;
    placement.face = face;
    return placement;
}

std::vector<int> Document::stampFaces(int face) const {
    std::vector<int> faces;
    if (!stampAt(face)) {
        return faces;
    }
    const auto part = static_cast<std::size_t>(faceRef_[static_cast<std::size_t>(face)].part);
    const auto count = static_cast<int>(lkz::asset::triangleCount(model_.parts[part].mesh));
    for (int t = 0; t < count; ++t) {
        faces.push_back(firstFace_[part] + t);
    }
    return faces;
}

int Document::stampId(int face) const {
    if (!stampAt(face)) {
        return -1;
    }
    return partId_[static_cast<std::size_t>(faceRef_[static_cast<std::size_t>(face)].part)];
}

int Document::partOfStamp(int id) const {
    if (id < 0) {
        return -1;
    }
    for (std::size_t p = 0; p < partId_.size(); ++p) {
        if (partId_[p] == id && stampOf_[p] && stampOf_[p]->owner < 0 && lkz::asset::triangleCount(model_.parts[p].mesh) > 0) {
            return static_cast<int>(p);
        }
    }
    return -1;
}

std::optional<DecalPlacement> Document::stampById(int id) const {
    const int part = partOfStamp(id);
    return part < 0 ? std::nullopt : stampAt(firstFace_[static_cast<std::size_t>(part)]);
}

std::vector<int> Document::stampFacesById(int id) const {
    const int part = partOfStamp(id);
    return part < 0 ? std::vector<int>{} : stampFaces(firstFace_[static_cast<std::size_t>(part)]);
}

std::vector<int> Document::replaceStamps(const std::vector<std::pair<int, DecalPlacement>>& changes) {
    std::vector<int> ids(changes.size(), -1);
    endStroke();
    liftOpen_ = false;
    std::shared_ptr<Geometry> before = snapshot();
    bool changed = false;
    for (std::size_t i = 0; i < changes.size(); ++i) {
        const auto& [id, placement] = changes[i];
        const int part = partOfStamp(id);
        if (part < 0) {
            continue;
        }
        const int seed = removeStamp(static_cast<std::size_t>(part)); // uma face do plano, já sem o carimbo
        changed = true;
        if (placement.decal < 0 || seed < 0) {
            continue;
        }
        DecalPlacement next = placement;
        next.face = seed;
        liftOpen_ = true; // o stamp entra no passo de desfazer deste replace
        if (stamp(next) > 0) {
            ids[i] = lastStampId_;
        }
        liftOpen_ = false;
    }
    if (changed) {
        Stroke stroke;
        stroke.geometry = std::move(before);
        pushUndo(std::move(stroke));
    }
    return ids;
}

std::optional<DecalPlacement> Document::lift(int face) {
    std::optional<DecalPlacement> placement = stampAt(face);
    if (!placement) {
        return std::nullopt;
    }
    endStroke();
    std::shared_ptr<Geometry> before = snapshot();
    const int seed = removeStamp(static_cast<std::size_t>(faceRef_[static_cast<std::size_t>(face)].part));
    Stroke stroke;
    stroke.geometry = std::move(before);
    pushUndo(std::move(stroke));
    liftOpen_ = true;
    placement->face = seed;
    return placement;
}

void Document::cancelLift() {
    if (!liftOpen_ || undo_.empty()) {
        liftOpen_ = false;
        return;
    }
    liftOpen_ = false;
    Stroke stroke = std::move(undo_.back());
    undo_.pop_back();
    swapStroke(stroke);
    ++revision_;
}

int Document::removeStamp(std::size_t removed) {
    const StampRef record = stampOf_[removed];
    const std::size_t partCount = model_.parts.size();
    // Destinos por parte, para devolver triângulos ao fim de cada uma.
    std::vector<std::vector<Face>> faces(partCount);
    std::vector<std::vector<Face>> initial(partCount);
    std::vector<std::vector<int>> families(partCount);
    std::map<std::pair<int, int>, Face> current; // (parte, família) -> destino de uma face que ficou
    for (std::size_t p = 0; p < partCount; ++p) {
        const auto count = static_cast<std::size_t>(lkz::asset::triangleCount(model_.parts[p].mesh));
        for (std::size_t t = 0; t < count; ++t) {
            const auto f = static_cast<std::size_t>(firstFace_[p]) + t;
            faces[p].push_back(faces_[f]);
            initial[p].push_back(initialFaces_[f]);
            families[p].push_back(family_[f]);
            if (p != removed) {
                current.try_emplace({partId_[p], family_[f]}, faces_[f]);
            }
        }
    }
    std::map<int, std::size_t> partIndex;
    for (std::size_t p = 0; p < partCount; ++p) {
        partIndex.emplace(partId_[p], p);
    }
    std::vector<StampRef> records = stampOf_;
    std::map<std::size_t, std::shared_ptr<StampRecord>> changed;
    std::pair<std::size_t, int> seed{0, -1};

    const lkz::MeshData& from = model_.parts[removed].mesh;
    const std::size_t count = lkz::asset::triangleCount(from);
    for (std::size_t t = 0; t < count && t < record->origins.size(); ++t) {
        const Origin& origin = record->origins[t];
        const auto target = partIndex.find(origin.part);
        if (target == partIndex.end() || target->second == removed) {
            continue;
        }
        const std::size_t q = target->second;
        lkz::MeshData& to = model_.parts[q].mesh;
        const bool hasUv = to.uvs.size() == to.positions.size();
        const auto index = lkz::asset::triangleAt(from, t);
        for (std::size_t k = 0; k < 3; ++k) {
            to.positions.push_back(from.positions[index[k]] - record->offset); // de volta ao plano da parede
            to.normals.push_back(from.normals[index[k]]);
            if (hasUv) {
                to.uvs.push_back(origin.uv[k]);
            }
            to.indices.push_back(static_cast<std::uint32_t>(to.positions.size() - 1));
        }
        // O destino atual do resto da face (o balde pode ter mudado a parede depois do carimbo).
        const auto now = current.find({origin.part, origin.family});
        faces[q].push_back(now != current.end() ? now->second : origin.face);
        initial[q].push_back(origin.initial);
        families[q].push_back(origin.family);
        if (records[q]) {
            auto& copy = changed[q];
            if (!copy) {
                copy = std::make_shared<StampRecord>(*records[q]);
                records[q] = copy;
            }
            copy->origins.push_back(origin.under ? *origin.under : Origin{});
        }
        if (seed.second < 0) {
            seed = {q, static_cast<int>(faces[q].size()) - 1};
        }
    }

    // A parte do carimbo e as paredes do vão dele saem juntas (de trás para a frente).
    std::vector<std::size_t> gone{removed};
    for (std::size_t p = 0; p < partCount; ++p) {
        if (p != removed && records[p] && records[p]->owner == partId_[removed]) {
            gone.push_back(p);
        }
    }
    std::sort(gone.rbegin(), gone.rend());
    for (const std::size_t p : gone) {
        const auto at = static_cast<std::ptrdiff_t>(p);
        model_.parts.erase(model_.parts.begin() + at);
        faces.erase(faces.begin() + at);
        initial.erase(initial.begin() + at);
        families.erase(families.begin() + at);
        records.erase(records.begin() + at);
        partId_.erase(partId_.begin() + at);
        if (seed.first > p) {
            --seed.first;
        }
    }
    faces_.clear();
    initialFaces_.clear();
    family_.clear();
    for (std::size_t p = 0; p < faces.size(); ++p) {
        faces_.insert(faces_.end(), faces[p].begin(), faces[p].end());
        initialFaces_.insert(initialFaces_.end(), initial[p].begin(), initial[p].end());
        family_.insert(family_.end(), families[p].begin(), families[p].end());
    }
    stampOf_ = std::move(records);
    buildFaceGeometry();
    raycaster_ = lkz::asset::MeshRaycaster{model_};
    geometryChanged_ = true;
    return seed.second < 0 ? -1 : firstFace_[seed.first] + seed.second;
}

bool Document::undo() {
    if (sculpt_) {
        cancelSculpt(); // a escultura em andamento é o passo mais novo
        return true;
    }
    if (!canUndo()) {
        return false;
    }
    liftOpen_ = false;
    Stroke stroke = std::move(undo_.back());
    undo_.pop_back();
    swapStroke(stroke); // o guardado passa a ter o depois: serve para refazer
    redo_.push_back(std::move(stroke));
    ++revision_;
    return true;
}

bool Document::redo() {
    if (!canRedo()) {
        return false;
    }
    liftOpen_ = false;
    Stroke stroke = std::move(redo_.back());
    redo_.pop_back();
    swapStroke(stroke);
    undo_.push_back(std::move(stroke));
    ++revision_;
    return true;
}

glm::vec4 Document::lookColor(int f, const glm::vec3& position, const glm::vec3& b) const {
    const Face& face = faces_[static_cast<std::size_t>(f)];
    const lkz::asset::TriangleRef ref = faceRef_[static_cast<std::size_t>(f)];
    const lkz::asset::ModelPart& part = model_.parts[static_cast<std::size_t>(ref.part)];
    const auto index = lkz::asset::triangleAt(part.mesh, static_cast<std::size_t>(ref.triangle));
    glm::vec2 uv{0.0f};
    if (part.mesh.uvs.size() == part.mesh.positions.size()) {
        uv = part.mesh.uvs[index[0]] * b.x + part.mesh.uvs[index[1]] * b.y + part.mesh.uvs[index[2]] * b.z;
    }
    switch (lookOf(f)) {
    case Look::Palette: {
        const glm::vec2 meters = surfaceMeters(position, faceNormal_[static_cast<std::size_t>(f)]) /
                                 palette_->at(face.material).metersPerRepeat;
        return palette_->sample(face.material, meters, 1e9f) * tintOf(face);
    }
    case Look::Color: return glm::vec4{face.color[0], face.color[1], face.color[2], face.color[3]} / 255.0f;
    case Look::Original: return model_.materials[static_cast<std::size_t>(part.material)].baseColor;
    case Look::Decal: {
        const lkz::asset::ImageData& image = decalImage(face.material);
        if (image.empty()) {
            return glm::vec4{1.0f};
        }
        return lkz::asset::sampleBilinear(image, uv * glm::vec2{static_cast<float>(image.width), static_cast<float>(image.height)}) *
               tintOf(face);
    }
    case Look::Canvas: break;
    }
    const Canvas& canvas = canvases_[static_cast<std::size_t>(canvasOfFace(f))];
    const int x = wrap(static_cast<int>(std::floor(uv.x * static_cast<float>(canvas.width))), canvas.width);
    const int y = wrap(static_cast<int>(std::floor(uv.y * static_cast<float>(canvas.height))), canvas.height);
    const std::uint8_t* p = &canvas.pixels[(static_cast<std::size_t>(y) * canvas.width + x) * 4];
    return glm::vec4{p[0], p[1], p[2], p[3]} / 255.0f;
}

std::optional<glm::vec3> Document::colorAt(const lkz::asset::RayHit& hit) const {
    const int f = faceIndex(hit.where);
    if (f < 0) {
        return std::nullopt;
    }
    return glm::vec3{lookColor(f, hit.position, hit.barycentric)};
}

int Document::paletteAt(const lkz::asset::RayHit& hit) const {
    const int f = faceIndex(hit.where);
    return f >= 0 && lookOf(f) == Look::Palette ? faces_[static_cast<std::size_t>(f)].material : -1;
}

std::optional<glm::vec3> Document::tintAt(const lkz::asset::RayHit& hit) const {
    const int f = faceIndex(hit.where);
    if (f < 0 || lookOf(f) != Look::Palette || !faces_[static_cast<std::size_t>(f)].colored) {
        return std::nullopt;
    }
    return glm::vec3{tintOf(faces_[static_cast<std::size_t>(f)])};
}

int Document::decalAt(const lkz::asset::RayHit& hit) const {
    const int f = faceIndex(hit.where);
    return f >= 0 && lookOf(f) == Look::Decal ? decals_[static_cast<std::size_t>(faces_[static_cast<std::size_t>(f)].material)].library
                                              : -1;
}

const lkz::asset::ImageData& Document::decalImage(int decal) const {
    const DecalEntry& entry = decals_[static_cast<std::size_t>(decal)];
    if (!entry.composite.empty() || decalLibrary_ == nullptr || entry.library < 0 || entry.library >= decalLibrary_->size()) {
        return entry.composite;
    }
    return decalLibrary_->at(entry.library).mips.front();
}

int Document::libraryDecal(int library) {
    for (std::size_t i = 0; i < decals_.size(); ++i) {
        if (decals_[i].library == library && decals_[i].composite.empty()) {
            return static_cast<int>(i);
        }
    }
    DecalEntry entry;
    entry.library = library;
    entry.id = decalLibrary_->at(library).id;
    decals_.push_back(std::move(entry));
    return static_cast<int>(decals_.size()) - 1;
}

int Document::whiteOnlyDecal(int library, const glm::vec3& tint) {
    for (std::size_t i = 0; i < decals_.size(); ++i) {
        if (decals_[i].library == library && decals_[i].whiteOnly == tint) {
            return static_cast<int>(i);
        }
    }
    const PaletteMaterial& source = decalLibrary_->at(library);
    DecalEntry entry;
    entry.library = library;
    entry.id = source.id;
    entry.composite = tintedImage(source.mips.front(), DecalTint::WhiteOnly, tint);
    entry.whiteOnly = tint;
    decals_.push_back(std::move(entry));
    return static_cast<int>(decals_.size()) - 1;
}

void Document::takePatches(std::vector<CanvasPatch>& out) {
    for (Canvas& canvas : canvases_) {
        // Fim de traço: os blocos do traço (e o que ainda não foi), o último com strokeEnd
        // para refazer os mipmaps.
        const bool strokeEnd = !canvas.finish.empty();
        std::vector<TexelRect> rects = std::move(canvas.dirty);
        for (const TexelRect& rect : canvas.finish) {
            addRect(rects, rect);
        }
        canvas.dirty.clear();
        canvas.finish.clear();
        for (std::size_t i = 0; i < rects.size(); ++i) {
            const TexelRect& rect = rects[i];
            CanvasPatch piece;
            piece.material = canvas.material;
            piece.patch.x = rect.x0;
            piece.patch.y = rect.y0;
            piece.patch.width = rect.x1 - rect.x0;
            piece.patch.height = rect.y1 - rect.y0;
            piece.patch.strokeEnd = strokeEnd && i + 1 == rects.size();
            const auto rowBytes = static_cast<std::size_t>(piece.patch.width) * 4;
            piece.patch.rgba.resize(rowBytes * static_cast<std::size_t>(piece.patch.height));
            for (int y = rect.y0; y < rect.y1; ++y) {
                std::copy_n(&canvas.pixels[(static_cast<std::size_t>(y) * canvas.width + rect.x0) * 4], rowBytes,
                            &piece.patch.rgba[static_cast<std::size_t>(y - rect.y0) * rowBytes]);
            }
            out.push_back(std::move(piece));
        }
    }
}

lkz::asset::ModelData Document::composed(Compose mode, const fs::path& output, std::string* notes) const {
    using lkz::asset::ImageData;
    using lkz::asset::MaterialData;
    lkz::asset::ModelData out;
    out.extras = extrasWithStamps(); // os carimbos ajustáveis, para reabrir
    const auto colors = swatchColors();
    std::map<std::array<std::uint8_t, 4>, int> colorIndex;
    for (std::size_t i = 0; i < colors.size(); ++i) {
        colorIndex.emplace(colors[i], static_cast<int>(i));
    }
    const SwatchLayout layout{colors.size()};
    std::error_code ec;
    const fs::path folder = mode == Compose::Export ? fs::absolute(output, ec).parent_path().lexically_normal() : fs::path{};

    const auto addImage = [&](ImageData image) {
        out.images.push_back(std::move(image));
        return static_cast<int>(out.images.size()) - 1;
    };
    const auto addMaterial = [&](MaterialData material) {
        out.materials.push_back(std::move(material));
        return static_cast<int>(out.materials.size()) - 1;
    };

    // Materiais do modelo (faces únicas): a tela vira a imagem, com a cor base dentro dela.
    std::vector<int> modelMaterial(model_.materials.size(), -1);
    const auto materialFor = [&](int m) {
        int& index = modelMaterial[static_cast<std::size_t>(m)];
        if (index < 0) {
            MaterialData material = model_.materials[static_cast<std::size_t>(m)];
            if (material.target != SurfaceTarget::Unique) {
                material.extras.clear(); // o destino antigo não vale mais
            }
            material.target = SurfaceTarget::Unique;
            material.shared.clear();
            material.metersPerRepeat = 0.0f;
            material.image = -1;
            if (const int c = canvasOf_[static_cast<std::size_t>(m)]; c >= 0) {
                const Canvas& canvas = canvases_[static_cast<std::size_t>(c)];
                ImageData image;
                image.width = canvas.width;
                image.height = canvas.height;
                image.rgba = canvas.pixels;
                image.name = material.name;
                material.image = addImage(std::move(image));
                material.baseColor = glm::vec4{1.0f};
            }
            index = addMaterial(std::move(material));
        }
        return index;
    };

    // Materiais da paleta: na exportação, apontam para a imagem dela (relativa ao .glb).
    std::map<std::tuple<int, std::uint32_t, bool, bool>, int> paletteMaterial; // (paleta, tinta, dois lados, assar)
    std::vector<std::string> references;
    int embedded = 0;
    // Imagem de uma biblioteca (paleta ou decalques) por caminho relativo ao .glb; noutro
    // disco, embutida.
    const auto external = [&](const PaletteMaterial& source) {
        const fs::path relative = source.image.empty() ? fs::path{} : source.image.lexically_relative(folder);
        ImageData image;
        if (!relative.empty()) {
            image.width = source.mips.front().width;
            image.height = source.mips.front().height;
            image.uri = encodeUri(generic(relative));
            image.file = source.image;
            references.push_back(generic(relative));
        } else {
            image = source.mips.front();
            ++embedded;
        }
        image.name = source.id;
        return image;
    };
    const auto paletteFor = [&](int k, const Face& face, bool doubleSided, bool bake) {
        const auto key = std::make_tuple(k, tintKey(face), doubleSided, bake);
        if (const auto it = paletteMaterial.find(key); it != paletteMaterial.end()) {
            return it->second;
        }
        const PaletteMaterial& source = palette_->at(k);
        MaterialData material;
        material.name = (bake ? "unico_" : "paleta_") + source.id +
                        (face.colored ? fmt::format("_{:06x}", tintKey(face)) : std::string{}) + (doubleSided ? "_2lados" : "");
        material.doubleSided = doubleSided;
        material.baseColor = tintOf(face); // a tinta: o jogo multiplica a imagem por ela
        if (bake) {
            ImageData image = source.mips.front(); // o UV único assa a partir dela
            image.name = source.id;
            material.image = addImage(std::move(image));
        } else {
            material.target = SurfaceTarget::Shared;
            material.shared = source.id;
            material.metersPerRepeat = source.metersPerRepeat;
            if (mode == Compose::Export) {
                material.image = addImage(external(source));
            }
        }
        const int index = addMaterial(std::move(material));
        paletteMaterial.emplace(key, index);
        return index;
    };

    // Cores lisas: um material com as amostras.
    int swatch = -1;
    std::map<std::pair<bool, bool>, int> colorMaterial; // (dois lados, assar)
    const auto colorFor = [&](bool doubleSided, bool bake) {
        const auto key = std::make_pair(doubleSided, bake);
        if (const auto it = colorMaterial.find(key); it != colorMaterial.end()) {
            return it->second;
        }
        if (swatch < 0) {
            swatch = addImage(swatchOf(colors));
        }
        MaterialData material;
        material.name = std::string{bake ? "unico_cores" : "cores"} + (doubleSided ? "_2lados" : "");
        material.target = bake ? SurfaceTarget::Unique : SurfaceTarget::Color;
        material.doubleSided = doubleSided;
        material.image = swatch;
        const int index = addMaterial(std::move(material));
        colorMaterial.emplace(key, index);
        return index;
    };

    // Decalques: os da biblioteca apontam para a imagem dela, como a paleta (no UV único,
    // só o nome); os compostos de um carimbo com transparência vão embutidos.
    std::map<std::tuple<int, std::uint32_t, bool>, int> decalMaterial; // (decalque, cor, dois lados)
    const auto decalFor = [&](int decal, const Face& face, bool doubleSided) {
        const auto key = std::make_tuple(decal, tintKey(face), doubleSided);
        if (const auto it = decalMaterial.find(key); it != decalMaterial.end()) {
            return it->second;
        }
        const DecalEntry& entry = decals_[static_cast<std::size_t>(decal)];
        MaterialData material;
        material.name = "decal_" + entry.id + (entry.composite.empty() ? std::string{} : fmt::format("_{}", decal)) +
                        (face.colored ? fmt::format("_{:06x}", tintKey(face)) : std::string{}) + (doubleSided ? "_2lados" : "");
        material.baseColor = tintOf(face); // multiplicado: o jogo multiplica a imagem por ela
        material.target = SurfaceTarget::Decal;
        material.shared = entry.id;
        material.doubleSided = doubleSided;
        if (!entry.composite.empty()) {
            ImageData image = entry.composite;
            image.name = material.name;
            image.lossless = true; // letreiro, placa: JPEG borraria as letras
            material.image = addImage(std::move(image));
        } else if (mode == Compose::Export) {
            material.image = addImage(external(decalLibrary_->at(entry.library)));
        }
        const int index = addMaterial(std::move(material));
        decalMaterial.emplace(key, index);
        return index;
    };

    // Grupos: as faces únicas por parte (mantém os nomes do arquivo); as outras por material.
    struct Group {
        int material = -1;
        std::string name;
        MeshBuilder builder;
    };
    std::map<std::tuple<int, int, std::uint32_t, bool>, Group> groups;
    int pending = 0;
    for (std::size_t p = 0; p < model_.parts.size(); ++p) {
        const lkz::asset::ModelPart& part = model_.parts[p];
        const bool doubleSided = model_.materials[static_cast<std::size_t>(part.material)].doubleSided;
        const bool hasUv = part.mesh.uvs.size() == part.mesh.positions.size();
        const std::size_t count = lkz::asset::triangleCount(part.mesh);
        for (std::size_t t = 0; t < count; ++t) {
            const int f = firstFace_[p] + static_cast<int>(t);
            const Face& face = faces_[static_cast<std::size_t>(f)];
            const auto index = lkz::asset::triangleAt(part.mesh, t);
            const bool unique = face.target == SurfaceTarget::Unique;
            glm::vec2 uv[3] = {glm::vec2{0.0f}, glm::vec2{0.0f}, glm::vec2{0.0f}};
            std::tuple<int, int, std::uint32_t, bool> key;
            int material = -1;
            std::string name;
            switch (lookOf(f)) {
            case Look::Canvas:
            case Look::Original:
                key = {0, static_cast<int>(p), 0u, doubleSided};
                material = materialFor(part.material);
                name = part.name;
                for (int k = 0; k < 3 && hasUv; ++k) {
                    uv[k] = part.mesh.uvs[index[static_cast<std::size_t>(k)]];
                }
                break;
            case Look::Palette: {
                // Única sem tela: assa no UV único; na exportação, vai como compartilhada.
                const bool bake = unique && mode == Compose::Bake;
                pending += unique && mode == Compose::Export ? 1 : 0;
                key = {bake ? 1 : 2, face.material, tintKey(face), doubleSided};
                material = paletteFor(face.material, face, doubleSided, bake);
                name = out.materials[static_cast<std::size_t>(material)].name;
                const float meters = palette_->at(face.material).metersPerRepeat;
                for (int k = 0; k < 3; ++k) {
                    uv[k] = surfaceMeters(part.mesh.positions[index[static_cast<std::size_t>(k)]],
                                          faceNormal_[static_cast<std::size_t>(f)]) /
                            meters;
                }
                break;
            }
            case Look::Color: {
                const bool bake = unique && mode == Compose::Bake;
                pending += unique && mode == Compose::Export ? 1 : 0;
                key = {bake ? 3 : 4, 0, 0u, doubleSided};
                material = colorFor(doubleSided, bake);
                name = out.materials[static_cast<std::size_t>(material)].name;
                uv[0] = uv[1] = uv[2] = layout.center(colorIndex[face.color]);
                break;
            }
            case Look::Decal:
                key = {5, face.material, tintKey(face), doubleSided};
                material = decalFor(face.material, face, doubleSided);
                name = out.materials[static_cast<std::size_t>(material)].name;
                for (int k = 0; k < 3 && hasUv; ++k) {
                    uv[k] = part.mesh.uvs[index[static_cast<std::size_t>(k)]];
                }
                break;
            }
            Group& group = groups[key];
            if (group.material < 0) {
                group.material = material;
                group.name = name;
            }
            group.builder.add(static_cast<int>(p), part.mesh, index, uv);
        }
    }
    for (auto& [key, group] : groups) {
        lkz::asset::ModelPart part;
        part.name = group.name;
        part.mesh = std::move(group.builder.mesh);
        part.material = group.material;
        out.parts.push_back(std::move(part));
    }

    if (notes != nullptr) {
        notes->clear();
        std::sort(references.begin(), references.end());
        references.erase(std::unique(references.begin(), references.end()), references.end());
        for (const std::string& reference : references) {
            *notes += (notes->empty() ? " · por referência: " : ", ") + reference;
        }
        if (embedded > 0) {
            *notes += fmt::format(" · {} da biblioteca embutida(s) (outro disco)", embedded);
        }
        if (pending > 0) {
            *notes += fmt::format(" · {} faces únicas ainda sem tela foram como compartilhadas/cor (gere o UV único)", pending);
        }
    }
    return out;
}

bool Document::exportGlb(const fs::path& path, std::string& report) {
    if (!isOpen()) {
        report = "Nenhum modelo aberto";
        return false;
    }
    std::string notes;
    const lkz::asset::ModelData out = composed(Compose::Export, path, &notes);

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    lkz::asset::GlbOptions options;
    options.keepExternal = true; // só os materiais da paleta têm uri: o resto vai embutido
    if (!lkz::asset::saveGlb(path, out, options)) {
        report = "Falhou ao gravar " + path.string() + " (detalhes no log)";
        return false;
    }
    // Conferência: o arquivo relido, com as imagens da paleta pelo caminho gravado.
    const auto check = lkz::asset::loadGltf(path);
    if (!check) {
        report = "Gravou " + path.filename().string() + ", mas não conseguiu reler o arquivo";
        return false;
    }
    for (const lkz::asset::MaterialData& material : check->materials) {
        if ((material.target == SurfaceTarget::Shared || material.target == SurfaceTarget::Decal) && material.image < 0) {
            report = "Gravou " + path.filename().string() + ", mas a imagem de '" + material.shared +
                     "' não abre pelo caminho gravado (detalhes no log)";
            return false;
        }
    }
    std::string images;
    for (const lkz::asset::ImageData& image : check->images) {
        if (image.uri.empty()) {
            images += (images.empty() ? "" : ", ") + sizeText(image.width, image.height);
        }
    }
    const auto fileSize = std::filesystem::file_size(path, ec);
    const double kilobytes = ec ? 0.0 : static_cast<double>(fileSize) / 1024.0;
    report = fmt::format("Gravado {} · {} · {} triângulos · {} materiais · imagens próprias: {}{} · ~{:.1f} MiB de vídeo{}",
                         path.filename().string(),
                         kilobytes < 1024.0 ? fmt::format("{:.0f} KB", kilobytes) : fmt::format("{:.1f} MB", kilobytes / 1024.0),
                         check->triangleCount(), check->materials.size(), images.empty() ? std::string{"nenhuma"} : "",
                         images, static_cast<double>(check->estimatedTextureMemory()) / (1024.0 * 1024.0), notes);
    savedRevision_ = revision_;
    return true;
}

bool Document::makeUniqueUv(const lkz::asset::UnwrapOptions& options, std::string& report) {
    if (!isOpen()) {
        report = "Nenhum modelo aberto";
        return false;
    }
    endStroke();
    const lkz::asset::ModelData all = composed(Compose::Bake, {});
    lkz::asset::ModelData unique = subset(all, true);
    lkz::asset::ModelData kept = subset(all, false);
    if (unique.parts.empty()) {
        report = "Todas as faces são da paleta ou de cor lisa: não há o que assar";
        return false;
    }
    std::string baked;
    auto model = lkz::asset::bakeUnique(unique, options, &baked);
    if (!model) {
        report = "Não consegui gerar o UV único (detalhes no log)";
        return false;
    }
    std::size_t keptFaces = 0;
    for (const lkz::asset::ModelPart& part : kept.parts) {
        keptFaces += lkz::asset::triangleCount(part.mesh);
    }
    append(*model, std::move(kept));
    model->extras = all.extras; // os carimbos continuam ajustáveis depois do UV único
    setModel(std::move(*model));
    ++revision_;
    report = "UV único: " + baked;
    if (keptFaces > 0) {
        report += fmt::format(" · {} faces da paleta ou de cor ficaram como estavam", keptFaces);
    }
    return true;
}

void Document::original(const Canvas& canvas, int x, int y, std::uint8_t* rgba) const {
    glm::vec4 color = canvas.tint;
    if (canvas.source != nullptr) {
        const std::uint8_t* p = &canvas.source->rgba[(static_cast<std::size_t>(y) * canvas.source->width + x) * 4];
        color *= glm::vec4{p[0], p[1], p[2], p[3]} / 255.0f;
    }
    for (int k = 0; k < 4; ++k) {
        rgba[k] = toByte(color[k]);
    }
}

// --- Esculpir ---

namespace {

/// Solda posições a até 0,1 mm num id só (grade de 1 mm: sem o erro de arredondar na borda
/// de uma célula).
class Welder {
public:
    int id(const glm::vec3& p) {
        const glm::ivec3 cell{glm::floor(p / cellSize)};
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    const auto it = cells_.find(key(cell + glm::ivec3{dx, dy, dz}));
                    if (it == cells_.end()) {
                        continue;
                    }
                    for (const int i : it->second) {
                        if (glm::length(points_[static_cast<std::size_t>(i)] - p) <= tolerance) {
                            return i;
                        }
                    }
                }
            }
        }
        const auto i = static_cast<int>(points_.size());
        points_.push_back(p);
        cells_[key(cell)].push_back(i);
        return i;
    }
    [[nodiscard]] const std::vector<glm::vec3>& points() const { return points_; }

private:
    static constexpr float cellSize = 1e-3f;
    static constexpr float tolerance = 1e-4f;
    static std::tuple<int, int, int> key(const glm::ivec3& c) { return {c.x, c.y, c.z}; }
    std::map<std::tuple<int, int, int>, std::vector<int>> cells_;
    std::vector<glm::vec3> points_;
};

/// Normais refeitas só nos vértices dos triângulos que têm um vértice que andou (média das
/// faces pela área, como computeNormals).
void refreshNormals(lkz::MeshData& mesh, const std::vector<char>& moved) {
    std::vector<char> affected(mesh.positions.size(), 0);
    const std::size_t count = lkz::asset::triangleCount(mesh);
    for (std::size_t t = 0; t < count; ++t) {
        const auto index = lkz::asset::triangleAt(mesh, t);
        if (moved[index[0]] != 0 || moved[index[1]] != 0 || moved[index[2]] != 0) {
            for (const std::uint32_t i : index) {
                affected[i] = 1;
            }
        }
    }
    for (std::size_t i = 0; i < affected.size(); ++i) {
        if (affected[i] != 0) {
            mesh.normals[i] = glm::vec3{0.0f};
        }
    }
    for (std::size_t t = 0; t < count; ++t) {
        const auto index = lkz::asset::triangleAt(mesh, t);
        if (affected[index[0]] == 0 && affected[index[1]] == 0 && affected[index[2]] == 0) {
            continue;
        }
        const glm::vec3 n = glm::cross(mesh.positions[index[1]] - mesh.positions[index[0]],
                                       mesh.positions[index[2]] - mesh.positions[index[0]]);
        for (const std::uint32_t i : index) {
            if (affected[i] != 0) {
                mesh.normals[i] += n;
            }
        }
    }
    for (std::size_t i = 0; i < affected.size(); ++i) {
        if (affected[i] != 0) {
            const float length = glm::length(mesh.normals[i]);
            mesh.normals[i] = length > 1e-12f ? mesh.normals[i] / length : glm::vec3{0.0f, 1.0f, 0.0f};
        }
    }
}

/// Paredes novas (do Puxar), juntadas por material.
struct SidePart {
    lkz::MeshData mesh;
    std::vector<Face> faces;
    std::vector<Face> initial;
    std::vector<int> families;
};

/// Um quadrilátero de A-B até A+offset-B+offset, virado para `facing`.
void addWall(SidePart& side, const glm::vec3& a, const glm::vec3& b, const glm::vec3& offset, const glm::vec2& uvA,
             const glm::vec2& uvB, const glm::vec3& facing, const Face& face, const Face& initial, int family) {
    const float length = glm::length(facing);
    if (length <= 1e-8f || glm::length(b - a) <= 1e-6f) {
        return;
    }
    lkz::MeshData& mesh = side.mesh;
    const glm::vec3 corners[4] = {a, b, b + offset, a + offset};
    const glm::vec2 uvs[4] = {uvA, uvB, uvB, uvA};
    const auto base = static_cast<std::uint32_t>(mesh.positions.size());
    for (int k = 0; k < 4; ++k) {
        mesh.positions.push_back(corners[k]);
        mesh.normals.push_back(facing / length);
        mesh.uvs.push_back(uvs[k]);
    }
    if (glm::dot(glm::cross(corners[1] - corners[0], corners[2] - corners[0]), facing) > 0.0f) {
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    } else {
        mesh.indices.insert(mesh.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    }
    for (int k = 0; k < 2; ++k) {
        side.faces.push_back(face);
        side.initial.push_back(initial);
        side.families.push_back(family);
    }
}

} // namespace

void Document::restoreGeometry(const Geometry& geometry) {
    Geometry copy = geometry;
    swapGeometry(copy);
}

std::vector<int> Document::cutRectangle(int face, const glm::vec2& lo, const glm::vec2& hi) {
    if (face < 0 || face >= faceCount() || hi.x - lo.x <= 1e-4f || hi.y - lo.y <= 1e-4f) {
        return {};
    }
    endStroke();
    liftOpen_ = false;
    std::shared_ptr<Geometry> before = snapshot();
    const SurfaceAxes axes = surfaceAxes(faceNormal_[static_cast<std::size_t>(face)]);
    const auto local = [&](const glm::vec3& p) { return glm::vec2{glm::dot(p, axes.u), glm::dot(p, axes.v)}; };
    const struct Side {
        int axis;
        float limit;
        float sign;
    } sides[4] = {{0, lo.x, 1.0f}, {0, hi.x, -1.0f}, {1, lo.y, 1.0f}, {1, hi.y, -1.0f}};

    std::vector<Face> faces;
    std::vector<Face> initial;
    std::vector<int> families;
    std::vector<char> inside;
    std::vector<StampRef> records = stampOf_;
    bool cutAny = false;
    for (std::size_t p = 0; p < model_.parts.size(); ++p) {
        lkz::MeshData& mesh = model_.parts[p].mesh;
        const std::size_t count = lkz::asset::triangleCount(mesh);
        const bool hasUv = mesh.uvs.size() == mesh.positions.size();
        const StampRecord* record = stampOf_[p].get();
        std::vector<Origin> origins;
        std::vector<std::uint32_t> kept;
        for (std::size_t t = 0; t < count; ++t) {
            const auto f = static_cast<std::size_t>(firstFace_[p]) + t;
            const auto index = lkz::asset::triangleAt(mesh, t);
            const auto keep = [&](char flag) {
                kept.insert(kept.end(), index.begin(), index.end());
                faces.push_back(faces_[f]);
                initial.push_back(initialFaces_[f]);
                families.push_back(family_[f]);
                inside.push_back(flag);
                if (record != nullptr) {
                    origins.push_back(record->origins[t]);
                }
            };
            if (static_cast<int>(f) != face && !coplanar(face, static_cast<int>(f))) {
                keep(0);
                continue;
            }
            const glm::vec3 pos[3] = {mesh.positions[index[0]], mesh.positions[index[1]], mesh.positions[index[2]]};
            glm::vec2 low{1e30f};
            glm::vec2 high{-1e30f};
            for (const glm::vec3& v : pos) {
                low = glm::min(low, local(v));
                high = glm::max(high, local(v));
            }
            if (!(low.x < hi.x && high.x > lo.x && low.y < hi.y && high.y > lo.y)) {
                keep(0);
                continue;
            }
            Polygon in = {{local(pos[0]), {1.0f, 0.0f, 0.0f}}, {local(pos[1]), {0.0f, 1.0f, 0.0f}}, {local(pos[2]), {0.0f, 0.0f, 1.0f}}};
            std::vector<Polygon> outside;
            for (const Side& side : sides) {
                Polygon out = clipSide(in, side.axis, side.limit, -side.sign);
                if (out.size() >= 3 && polygonArea(out) > 1e-8f) {
                    outside.push_back(std::move(out));
                }
                in = clipSide(in, side.axis, side.limit, side.sign);
            }
            if (in.size() < 3 || polygonArea(in) <= 1e-8f) {
                keep(0);
                continue;
            }
            if (outside.empty()) {
                keep(1); // inteira dentro: não precisa cortar
                continue;
            }
            cutAny = true;
            const bool hasNormals = mesh.normals.size() == mesh.positions.size();
            const auto add = [&](const ClipVertex& v) {
                const glm::vec3 n = hasNormals ? mesh.normals[index[0]] * v.b.x + mesh.normals[index[1]] * v.b.y +
                                                     mesh.normals[index[2]] * v.b.z
                                               : faceNormal_[f];
                const float length = glm::length(n);
                mesh.positions.push_back(pos[0] * v.b.x + pos[1] * v.b.y + pos[2] * v.b.z);
                mesh.normals.push_back(length > 1e-12f ? n / length : faceNormal_[f]);
                if (hasUv) {
                    mesh.uvs.push_back(mesh.uvs[index[0]] * v.b.x + mesh.uvs[index[1]] * v.b.y + mesh.uvs[index[2]] * v.b.z);
                }
                return static_cast<std::uint32_t>(mesh.positions.size() - 1);
            };
            const auto emit = [&](const Polygon& piece, char flag) {
                const std::uint32_t first = add(piece[0]);
                std::uint32_t previous = add(piece[1]);
                for (std::size_t i = 2; i < piece.size(); ++i) {
                    const std::uint32_t next = add(piece[i]);
                    if (fanArea(piece[0], piece[i - 1], piece[i])) {
                        kept.insert(kept.end(), {first, previous, next});
                        faces.push_back(faces_[f]);
                        initial.push_back(initialFaces_[f]);
                        families.push_back(family_[f]);
                        inside.push_back(flag);
                        if (record != nullptr) {
                            origins.push_back(reshaped(record->origins[t], {piece[0].b, piece[i - 1].b, piece[i].b}));
                        }
                    }
                    previous = next;
                }
            };
            for (const Polygon& piece : outside) {
                emit(piece, 0);
            }
            emit(in, 1);
        }
        if (record != nullptr) {
            auto copy = std::make_shared<StampRecord>(*record);
            copy->origins = std::move(origins);
            records[p] = std::move(copy);
        }
        setTriangles(mesh, std::move(kept));
    }
    faces_ = std::move(faces);
    initialFaces_ = std::move(initial);
    family_ = std::move(families);
    stampOf_ = std::move(records);
    buildFaceGeometry();
    raycaster_ = lkz::asset::MeshRaycaster{model_};
    geometryChanged_ = true;
    if (cutAny) {
        Stroke stroke;
        stroke.geometry = std::move(before);
        pushUndo(std::move(stroke));
    }
    std::vector<int> out;
    for (std::size_t f = 0; f < inside.size(); ++f) {
        if (inside[f] != 0) {
            out.push_back(static_cast<int>(f));
        }
    }
    return out;
}

void Document::splitTJunctions(std::vector<char>& selected) {
    const auto corners = [&](std::size_t f) {
        const lkz::asset::TriangleRef ref = faceRef_[f];
        const lkz::MeshData& mesh = model_.parts[static_cast<std::size_t>(ref.part)].mesh;
        const auto index = lkz::asset::triangleAt(mesh, static_cast<std::size_t>(ref.triangle));
        return std::array<glm::vec3, 3>{mesh.positions[index[0]], mesh.positions[index[1]], mesh.positions[index[2]]};
    };
    // A região: a caixa das faces marcadas, um pouco maior (pega as vizinhas).
    glm::vec3 low{1e30f};
    glm::vec3 high{-1e30f};
    for (std::size_t f = 0; f < selected.size(); ++f) {
        if (selected[f] != 0) {
            for (const glm::vec3& v : corners(f)) {
                low = glm::min(low, v);
                high = glm::max(high, v);
            }
        }
    }
    low -= glm::vec3{1e-3f};
    high += glm::vec3{1e-3f};
    const auto inRegion = [&](const std::array<glm::vec3, 3>& v) {
        const glm::vec3 a = glm::min(v[0], glm::min(v[1], v[2]));
        const glm::vec3 b = glm::max(v[0], glm::max(v[1], v[2]));
        return a.x <= high.x && a.y <= high.y && a.z <= high.z && b.x >= low.x && b.y >= low.y && b.z >= low.z;
    };
    Welder welder;
    for (std::size_t f = 0; f < faces_.size(); ++f) {
        const auto v = corners(f);
        if (inRegion(v)) {
            for (const glm::vec3& p : v) {
                welder.id(p);
            }
        }
    }
    const std::vector<glm::vec3>& points = welder.points();

    std::vector<Face> faces;
    std::vector<Face> initial;
    std::vector<int> families;
    std::vector<char> flags;
    std::vector<StampRef> records = stampOf_;
    bool any = false;
    for (std::size_t p = 0; p < model_.parts.size(); ++p) {
        lkz::MeshData& mesh = model_.parts[p].mesh;
        const std::size_t count = lkz::asset::triangleCount(mesh);
        const bool hasUv = mesh.uvs.size() == mesh.positions.size();
        const StampRecord* record = stampOf_[p].get();
        std::vector<Origin> origins;
        std::vector<std::uint32_t> kept;
        for (std::size_t t = 0; t < count; ++t) {
            const auto f = static_cast<std::size_t>(firstFace_[p]) + t;
            const auto index = lkz::asset::triangleAt(mesh, t);
            const std::array<glm::vec3, 3> v = {mesh.positions[index[0]], mesh.positions[index[1]], mesh.positions[index[2]]};
            const auto keep = [&]() {
                kept.insert(kept.end(), index.begin(), index.end());
                faces.push_back(faces_[f]);
                initial.push_back(initialFaces_[f]);
                families.push_back(family_[f]);
                flags.push_back(selected[f]);
                if (record != nullptr) {
                    origins.push_back(record->origins[t]);
                }
            };
            if (!inRegion(v)) {
                keep();
                continue;
            }
            // Os vértices de outros triângulos no meio de cada aresta, em ordem.
            struct OnEdge {
                float s;
                glm::vec3 position;
                glm::vec3 b;
            };
            std::array<std::vector<OnEdge>, 3> on;
            bool split = false;
            for (int k = 0; k < 3; ++k) {
                const glm::vec3 a = v[static_cast<std::size_t>(k)];
                const glm::vec3 c = v[static_cast<std::size_t>((k + 1) % 3)];
                const glm::vec3 ac = c - a;
                const float length2 = glm::dot(ac, ac);
                if (length2 < 1e-12f) {
                    continue;
                }
                for (const glm::vec3& q : points) {
                    if (glm::length(q - a) <= 1e-4f || glm::length(q - c) <= 1e-4f) {
                        continue;
                    }
                    const float s = glm::dot(q - a, ac) / length2;
                    if (s <= 0.0f || s >= 1.0f || glm::length(a + ac * s - q) > 1e-4f) {
                        continue;
                    }
                    glm::vec3 b{0.0f};
                    b[k] = 1.0f - s;
                    b[(k + 1) % 3] = s;
                    on[static_cast<std::size_t>(k)].push_back({s, q, b});
                    split = true;
                }
                std::sort(on[static_cast<std::size_t>(k)].begin(), on[static_cast<std::size_t>(k)].end(),
                          [](const OnEdge& x, const OnEdge& y) { return x.s < y.s; });
            }
            if (!split) {
                keep();
                continue;
            }
            any = true;
            // Leque a partir do centro: os pontos em linha numa aresta não viram triângulo
            // achatado, e o sentido do triângulo fica.
            const bool hasNormals = mesh.normals.size() == mesh.positions.size();
            const auto add = [&](const glm::vec3& position, const glm::vec3& b) {
                const glm::vec3 n = hasNormals ? mesh.normals[index[0]] * b.x + mesh.normals[index[1]] * b.y +
                                                     mesh.normals[index[2]] * b.z
                                               : faceNormal_[f];
                const float length = glm::length(n);
                mesh.positions.push_back(position);
                mesh.normals.push_back(length > 1e-12f ? n / length : faceNormal_[f]);
                if (hasUv) {
                    mesh.uvs.push_back(mesh.uvs[index[0]] * b.x + mesh.uvs[index[1]] * b.y + mesh.uvs[index[2]] * b.z);
                }
                return static_cast<std::uint32_t>(mesh.positions.size() - 1);
            };
            std::vector<glm::vec3> ringB;
            std::vector<std::uint32_t> ring;
            for (int k = 0; k < 3; ++k) {
                glm::vec3 corner{0.0f};
                corner[k] = 1.0f;
                ring.push_back(index[static_cast<std::size_t>(k)]);
                ringB.push_back(corner);
                for (const OnEdge& e : on[static_cast<std::size_t>(k)]) {
                    ring.push_back(add(e.position, e.b));
                    ringB.push_back(e.b);
                }
            }
            const glm::vec3 centerB{1.0f / 3.0f};
            const std::uint32_t center = add((v[0] + v[1] + v[2]) / 3.0f, centerB);
            for (std::size_t i = 0; i < ring.size(); ++i) {
                const std::size_t j = (i + 1) % ring.size();
                kept.insert(kept.end(), {center, ring[i], ring[j]});
                faces.push_back(faces_[f]);
                initial.push_back(initialFaces_[f]);
                families.push_back(family_[f]);
                flags.push_back(selected[f]);
                if (record != nullptr) {
                    origins.push_back(reshaped(record->origins[t], {centerB, ringB[i], ringB[j]}));
                }
            }
        }
        if (record != nullptr) {
            auto copy = std::make_shared<StampRecord>(*record);
            copy->origins = std::move(origins);
            records[p] = std::move(copy);
        }
        setTriangles(mesh, std::move(kept));
    }
    if (!any) {
        return;
    }
    faces_ = std::move(faces);
    initialFaces_ = std::move(initial);
    family_ = std::move(families);
    stampOf_ = std::move(records);
    buildFaceGeometry();
    selected = std::move(flags);
}

bool Document::beginSculpt(const std::vector<int>& faces, SculptMode mode, const glm::vec3& direction) {
    if (sculpt_) {
        cancelSculpt();
    }
    std::vector<char> selected(faces_.size(), 0);
    bool any = false;
    for (const int f : faces) {
        if (f >= 0 && f < faceCount()) {
            selected[static_cast<std::size_t>(f)] = 1;
            any = true;
        }
    }
    const float length = glm::length(direction);
    if (!any || length < 1e-6f) {
        return false;
    }
    endStroke();
    liftOpen_ = false;
    auto sculpt = std::make_shared<Sculpt>();
    sculpt->before = snapshot();
    splitTJunctions(selected);
    sculpt->prepared = snapshot();
    sculpt->selected = std::move(selected);
    sculpt->mode = mode;
    sculpt->direction = direction / length;
    sculpt_ = std::move(sculpt);
    raycaster_ = lkz::asset::MeshRaycaster{model_};
    geometryChanged_ = true;
    return true;
}

std::vector<int> Document::sculptSelection() const {
    std::vector<int> faces;
    if (sculpt_) {
        for (std::size_t f = 0; f < sculpt_->selected.size() && f < faces_.size(); ++f) {
            if (sculpt_->selected[f] != 0) {
                faces.push_back(static_cast<int>(f));
            }
        }
    }
    return faces;
}

float Document::sculptDistance() const {
    return sculpt_ ? sculpt_->distance : 0.0f;
}

void Document::updateSculpt(float distance) {
    if (sculpt_) {
        sculpt_->distance = distance;
        applySculpt();
    }
}

std::vector<int> Document::endSculpt() {
    std::vector<int> faces;
    if (!sculpt_) {
        return faces;
    }
    const std::shared_ptr<Sculpt> sculpt = std::move(sculpt_);
    sculpt_.reset();
    if (std::abs(sculpt->distance) < 1e-5f) {
        restoreGeometry(*sculpt->before); // não andou: nem os cortes dos vértices em T ficam
        return faces;
    }
    for (std::size_t f = 0; f < sculpt->selected.size(); ++f) {
        if (sculpt->selected[f] != 0) {
            faces.push_back(static_cast<int>(f));
        }
    }
    Stroke stroke;
    stroke.geometry = sculpt->before;
    pushUndo(std::move(stroke));
    return faces;
}

void Document::cancelSculpt() {
    if (!sculpt_) {
        return;
    }
    const std::shared_ptr<Sculpt> sculpt = std::move(sculpt_);
    sculpt_.reset();
    restoreGeometry(*sculpt->before);
}

void Document::applySculpt() {
    const Sculpt& sculpt = *sculpt_;
    {
        const Geometry& from = *sculpt.prepared;
        model_.parts = from.parts;
        model_.materials = from.materials;
        canvasOf_ = from.canvasOf;
        decalOf_ = from.decalOf;
        faces_ = from.faces;
        initialFaces_ = from.initialFaces;
        partId_ = from.partId;
        stampOf_ = from.stampOf;
        family_ = from.family;
    }
    buildFaceGeometry();
    const glm::vec3 offset = sculpt.direction * sculpt.distance;
    if (std::abs(sculpt.distance) >= 1e-6f) {
        // Vértices soldados por posição: a mesma quina em partes diferentes anda junto.
        Welder welder;
        std::vector<std::vector<int>> ids(model_.parts.size());
        for (std::size_t p = 0; p < model_.parts.size(); ++p) {
            for (const glm::vec3& position : model_.parts[p].mesh.positions) {
                ids[p].push_back(welder.id(position));
            }
        }
        std::vector<char> selectedVertex(welder.points().size(), 0);
        for (std::size_t f = 0; f < faces_.size(); ++f) {
            if (sculpt.selected[f] != 0) {
                const lkz::asset::TriangleRef ref = faceRef_[f];
                const auto p = static_cast<std::size_t>(ref.part);
                for (const std::uint32_t i : lkz::asset::triangleAt(model_.parts[p].mesh, static_cast<std::size_t>(ref.triangle))) {
                    selectedVertex[static_cast<std::size_t>(ids[p][i])] = 1;
                }
            }
        }

        if (sculpt.mode == SculptMode::Move) {
            // Todos os vértices da seleção andam, em qualquer parte: as vizinhas esticam.
            for (std::size_t p = 0; p < model_.parts.size(); ++p) {
                lkz::MeshData& mesh = model_.parts[p].mesh;
                std::vector<char> moved(mesh.positions.size(), 0);
                bool touched = false;
                for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
                    if (selectedVertex[static_cast<std::size_t>(ids[p][i])] != 0) {
                        mesh.positions[i] += offset;
                        moved[i] = 1;
                        touched = true;
                    }
                }
                if (touched) {
                    refreshNormals(mesh, moved);
                }
            }
        } else {
            // Arestas no sentido de cada triângulo: a da seleção sem a volta na seleção é borda.
            struct EdgeUse {
                std::size_t face;
                std::size_t part;
                std::uint32_t a;
                std::uint32_t b;
            };
            std::map<std::pair<int, int>, std::vector<EdgeUse>> selectedEdges;
            std::map<std::pair<int, int>, std::vector<EdgeUse>> otherEdges;
            for (std::size_t f = 0; f < faces_.size(); ++f) {
                const lkz::asset::TriangleRef ref = faceRef_[f];
                const auto p = static_cast<std::size_t>(ref.part);
                const auto index = lkz::asset::triangleAt(model_.parts[p].mesh, static_cast<std::size_t>(ref.triangle));
                for (std::size_t k = 0; k < 3; ++k) {
                    const std::uint32_t a = index[k];
                    const std::uint32_t b = index[(k + 1) % 3];
                    const std::pair<int, int> key{ids[p][a], ids[p][b]};
                    if (key.first != key.second) {
                        (sculpt.selected[f] != 0 ? selectedEdges : otherEdges)[key].push_back({f, p, a, b});
                    }
                }
            }
            std::map<int, SidePart> sides;
            for (const auto& [key, uses] : selectedEdges) {
                if (selectedEdges.contains({key.second, key.first})) {
                    continue; // dentro da seleção
                }
                for (const EdgeUse& use : uses) {
                    const lkz::MeshData& mesh = model_.parts[use.part].mesh;
                    const glm::vec3 a = mesh.positions[use.a];
                    const glm::vec3 b = mesh.positions[use.b];
                    const glm::vec3 n = faceNormal_[use.face];
                    // De fora da seleção: à direita da aresta. Saindo pela frente, a parede
                    // olha para fora; entrando, para dentro do buraco.
                    const glm::vec3 outward = glm::cross(b - a, n);
                    const glm::vec3 facing = glm::dot(offset, n) >= 0.0f ? outward : -outward;
                    // O acabamento é o da face de fora encostada na aresta (a parede em volta).
                    std::size_t from = use.face;
                    std::size_t part = use.part;
                    glm::vec2 uvA{0.0f};
                    glm::vec2 uvB{0.0f};
                    if (const auto it = otherEdges.find({key.second, key.first}); it != otherEdges.end()) {
                        const EdgeUse& other = it->second.front(); // vai de b para a
                        from = other.face;
                        part = other.part;
                        const lkz::MeshData& outer = model_.parts[part].mesh;
                        if (outer.uvs.size() == outer.positions.size()) {
                            uvA = outer.uvs[other.b];
                            uvB = outer.uvs[other.a];
                        }
                    } else if (mesh.uvs.size() == mesh.positions.size()) {
                        uvA = mesh.uvs[use.a];
                        uvB = mesh.uvs[use.b];
                    }
                    addWall(sides[model_.parts[part].material], a, b, offset, uvA, uvB, facing, faces_[from],
                            initialFaces_[from], family_[from]);
                }
            }
            // A seleção anda com vértices próprios: as vizinhas ficam no lugar.
            for (std::size_t p = 0; p < model_.parts.size(); ++p) {
                lkz::MeshData& mesh = model_.parts[p].mesh;
                const std::size_t count = lkz::asset::triangleCount(mesh);
                if (mesh.indices.empty()) {
                    mesh.indices.resize(mesh.positions.size());
                    std::iota(mesh.indices.begin(), mesh.indices.end(), 0u);
                }
                const bool hasUv = mesh.uvs.size() == mesh.positions.size();
                std::map<std::uint32_t, std::uint32_t> moved;
                for (std::size_t t = 0; t < count; ++t) {
                    if (sculpt.selected[static_cast<std::size_t>(firstFace_[p]) + t] == 0) {
                        continue;
                    }
                    for (std::size_t k = 0; k < 3; ++k) {
                        std::uint32_t& i = mesh.indices[t * 3 + k];
                        const auto [it, fresh] = moved.try_emplace(i, static_cast<std::uint32_t>(mesh.positions.size()));
                        if (fresh) {
                            const glm::vec3 position = mesh.positions[i] + offset;
                            const glm::vec3 normal = mesh.normals[i];
                            mesh.positions.push_back(position);
                            mesh.normals.push_back(normal);
                            if (hasUv) {
                                const glm::vec2 uv = mesh.uvs[i];
                                mesh.uvs.push_back(uv);
                            }
                        }
                        i = it->second;
                    }
                }
            }
            for (auto& [material, side] : sides) {
                if (side.faces.empty()) {
                    continue;
                }
                lkz::asset::ModelPart part;
                part.name = "esculpir";
                part.material = material;
                part.mesh = std::move(side.mesh);
                model_.parts.push_back(std::move(part));
                faces_.insert(faces_.end(), side.faces.begin(), side.faces.end());
                initialFaces_.insert(initialFaces_.end(), side.initial.begin(), side.initial.end());
                family_.insert(family_.end(), side.families.begin(), side.families.end());
                partId_.push_back(nextPartId_++);
                stampOf_.push_back(nullptr);
            }
            buildFaceGeometry();
        }
        if (sculpt.mode == SculptMode::Move) {
            buildFaceGeometry();
        }
    }
    raycaster_ = lkz::asset::MeshRaycaster{model_};
    geometryChanged_ = true;
}

// --- Carimbos gravados no arquivo ---

namespace {

constexpr const char* stampsKey = "pintorCarimbos"; // em extras do modelo

const char* tintName(DecalTint mode) {
    switch (mode) {
    case DecalTint::Multiply: return "multiplicar";
    case DecalTint::WhiteOnly: return "branco";
    case DecalTint::None: break;
    }
    return "original";
}

DecalTint tintFromName(const std::string& name) {
    return name == "multiplicar" ? DecalTint::Multiply : name == "branco" ? DecalTint::WhiteOnly : DecalTint::None;
}

} // namespace

std::string Document::extrasWithStamps() const {
    nlohmann::json extras = model_.extras.empty() ? nlohmann::json::object() : nlohmann::json::parse(model_.extras, nullptr, false);
    if (!extras.is_object()) {
        extras = nlohmann::json::object();
    }
    nlohmann::json list = nlohmann::json::array();
    for (std::size_t p = 0; p < model_.parts.size(); ++p) {
        const StampRecord* record = stampOf_[p].get();
        if (record == nullptr || record->owner >= 0 || lkz::asset::triangleCount(model_.parts[p].mesh) == 0 ||
            lookOf(firstFace_[p]) != Look::Decal ||
            decalLibrary_ == nullptr || record->placement.decal < 0 || record->placement.decal >= decalLibrary_->size()) {
            continue;
        }
        const DecalPlacement& placement = record->placement;
        // O plano do maior triângulo: o de uma lasca fina tem a normal imprecisa.
        auto f = static_cast<std::size_t>(firstFace_[p]);
        const std::size_t count = lkz::asset::triangleCount(model_.parts[p].mesh);
        for (std::size_t t = 1; t < count; ++t) {
            const auto g = static_cast<std::size_t>(firstFace_[p]) + t;
            if (faceArea_[g] > faceArea_[f]) {
                f = g;
            }
        }
        const glm::vec3 normal = faceNormal_[f];
        const float plane = glm::dot(normal, faceCenter_[f]) - placement.depth; // a parede, sem a profundidade
        list.push_back({{"decalque", decalLibrary_->at(placement.decal).id},
                        {"normal", {normal.x, normal.y, normal.z}},
                        {"plano", plane},
                        {"origem", {placement.origin.x, placement.origin.y}},
                        {"tamanho", {placement.size.x, placement.size.y}},
                        {"giro", placement.quarterTurns},
                        {"cor", tintName(placement.tintMode)},
                        {"tinta", {placement.tint.r, placement.tint.g, placement.tint.b}},
                        {"profundidade", placement.depth}});
    }
    if (list.empty()) {
        extras.erase(stampsKey);
    } else {
        extras[stampsKey] = std::move(list);
    }
    return extras.empty() ? std::string{} : extras.dump();
}

int Document::restoreStamps() {
    if (decalLibrary_ == nullptr) {
        return 0;
    }
    const nlohmann::json extras =
        model_.extras.empty() ? nlohmann::json::object() : nlohmann::json::parse(model_.extras, nullptr, false);
    const nlohmann::json list = extras.is_object() && extras.contains(stampsKey) && extras[stampsKey].is_array()
                                    ? extras[stampsKey]
                                    : nlohmann::json::array();
    struct Saved {
        DecalPlacement placement;
        glm::vec3 normal{0.0f};
        float plane = 0.0f;
    };
    std::vector<Saved> saved;
    for (const nlohmann::json& item : list) {
        try {
            Saved stamp;
            stamp.placement.decal = decalLibrary_->find(item.at("decalque").get<std::string>());
            const auto& n = item.at("normal");
            stamp.normal = glm::vec3{n.at(0).get<float>(), n.at(1).get<float>(), n.at(2).get<float>()};
            stamp.plane = item.at("plano").get<float>();
            stamp.placement.origin = {item.at("origem").at(0).get<float>(), item.at("origem").at(1).get<float>()};
            stamp.placement.size = {item.at("tamanho").at(0).get<float>(), item.at("tamanho").at(1).get<float>()};
            stamp.placement.quarterTurns = item.value("giro", 0) & 3;
            stamp.placement.tintMode = tintFromName(item.value("cor", std::string{"original"}));
            if (item.contains("tinta")) {
                const auto& t = item.at("tinta");
                stamp.placement.tint = glm::vec3{t.at(0).get<float>(), t.at(1).get<float>(), t.at(2).get<float>()};
            }
            stamp.placement.depth = item.value("profundidade", 0.0f);
            const float length = glm::length(stamp.normal);
            if (stamp.placement.decal < 0 || length < 1e-6f) {
                continue; // o decalque não está mais na biblioteca: fica fixo
            }
            stamp.normal /= length;
            saved.push_back(stamp);
        } catch (const nlohmann::json::exception&) {
            continue;
        }
    }

    // Cada triângulo vai para um carimbo só: o mais novo primeiro (os de cima).
    const std::size_t faceTotal = faces_.size();
    std::vector<int> decalOwner(faceTotal, -1);
    std::vector<int> sideOwner(faceTotal, -1);
    const auto corners = [&](std::size_t f) {
        const lkz::asset::TriangleRef ref = faceRef_[f];
        const lkz::MeshData& mesh = model_.parts[static_cast<std::size_t>(ref.part)].mesh;
        const auto index = lkz::asset::triangleAt(mesh, static_cast<std::size_t>(ref.triangle));
        return std::array<glm::vec3, 3>{mesh.positions[index[0]], mesh.positions[index[1]], mesh.positions[index[2]]};
    };
    constexpr float tolerance = 1e-3f;
    for (int s = static_cast<int>(saved.size()) - 1; s >= 0; --s) {
        const Saved& stamp = saved[static_cast<std::size_t>(s)];
        const SurfaceAxes axes = surfaceAxes(stamp.normal);
        const glm::vec2 lo = stamp.placement.origin;
        const glm::vec2 hi = lo + stamp.placement.size;
        const float depth = stamp.placement.depth;
        const auto local = [&](const glm::vec3& p) { return glm::vec2{glm::dot(p, axes.u), glm::dot(p, axes.v)}; };
        const auto inside = [&](const glm::vec2& q) {
            return q.x >= lo.x - tolerance && q.x <= hi.x + tolerance && q.y >= lo.y - tolerance && q.y <= hi.y + tolerance;
        };
        for (std::size_t f = 0; f < faceTotal; ++f) {
            if (decalOwner[f] >= 0 || sideOwner[f] >= 0) {
                continue;
            }
            const glm::vec3 center = faceCenter_[f];
            const glm::vec3 normal = faceNormal_[f];
            const Face& face = faces_[f];
            if (face.target == SurfaceTarget::Decal) {
                if (glm::dot(normal, stamp.normal) < 0.999f ||
                    std::abs(glm::dot(stamp.normal, center) - (stamp.plane + depth)) > tolerance || !inside(local(center)) ||
                    face.material < 0 || face.material >= decalCount() ||
                    decals_[static_cast<std::size_t>(face.material)].library != stamp.placement.decal) {
                    continue;
                }
                // O UV também tem de ser o deste carimbo: com carimbos iguais sobrepostos, o
                // retângulo sozinho não diz de quem é o triângulo.
                const lkz::asset::TriangleRef ref = faceRef_[f];
                const lkz::MeshData& mesh = model_.parts[static_cast<std::size_t>(ref.part)].mesh;
                bool same = mesh.uvs.size() == mesh.positions.size();
                const auto index = lkz::asset::triangleAt(mesh, static_cast<std::size_t>(ref.triangle));
                for (std::size_t k = 0; k < 3 && same; ++k) {
                    same = glm::length(decalUv(stamp.placement, local(mesh.positions[index[k]])) - mesh.uvs[index[k]]) < 2e-3f;
                }
                if (!same) {
                    continue;
                }
                decalOwner[f] = s;
            } else if (std::abs(depth) > 1e-4f && std::abs(glm::dot(normal, stamp.normal)) < 1e-3f) {
                // Parede do vão: entre a parede e a frente, em cima de uma borda do retângulo.
                bool between = true;
                for (const glm::vec3& q : corners(f)) {
                    const float h = glm::dot(stamp.normal, q) - stamp.plane;
                    between = between && h >= std::min(0.0f, depth) - tolerance && h <= std::max(0.0f, depth) + tolerance;
                }
                const glm::vec2 c = local(center);
                const bool onX = (std::abs(c.x - lo.x) < tolerance || std::abs(c.x - hi.x) < tolerance) &&
                                 c.y >= lo.y - tolerance && c.y <= hi.y + tolerance;
                const bool onY = (std::abs(c.y - lo.y) < tolerance || std::abs(c.y - hi.y) < tolerance) &&
                                 c.x >= lo.x - tolerance && c.x <= hi.x + tolerance;
                if (between && (onX || onY)) {
                    sideOwner[f] = s;
                }
            }
        }
    }

    // Os que não estão na lista (um .glb de antes dela): o UV de cada grupo de triângulos de
    // decalque diz onde a imagem foi posta, e daí sai o retângulo e o giro. A profundidade
    // não dá para saber: só os rentes à parede voltam.
    std::vector<char> visited(faceTotal, 0);
    const std::size_t adoptedFrom = saved.size();
    for (std::size_t seed = 0; seed < faceTotal; ++seed) {
        const Face& seedFace = faces_[seed];
        if (visited[seed] != 0 || decalOwner[seed] >= 0 || seedFace.target != SurfaceTarget::Decal || seedFace.material < 0 ||
            seedFace.material >= decalCount()) {
            continue;
        }
        const DecalEntry& entry = decals_[static_cast<std::size_t>(seedFace.material)];
        if (entry.library < 0 || entry.library >= decalLibrary_->size()) {
            continue;
        }
        // O grupo: ligados por arestas, da mesma imagem e cor, no mesmo plano.
        std::vector<std::size_t> group;
        std::vector<std::size_t> stack{seed};
        visited[seed] = 1;
        while (!stack.empty()) {
            const std::size_t f = stack.back();
            stack.pop_back();
            group.push_back(f);
            for (const int g : neighbors_[f]) {
                const auto n = static_cast<std::size_t>(g);
                if (visited[n] == 0 && decalOwner[n] < 0 && faces_[n] == seedFace && coplanar(static_cast<int>(seed), g)) {
                    visited[n] = 1;
                    stack.push_back(n);
                }
            }
        }
        std::size_t largest = seed; // o plano do maior triângulo (o de uma lasca é impreciso)
        for (const std::size_t f : group) {
            if (faceArea_[f] > faceArea_[largest]) {
                largest = f;
            }
        }
        const glm::vec3 normal = faceNormal_[largest];
        const SurfaceAxes axes = surfaceAxes(normal);
        const auto local = [&](const glm::vec3& p) { return glm::vec2{glm::dot(p, axes.u), glm::dot(p, axes.v)}; };
        // O mapa do plano para o UV da imagem, de um triângulo com área.
        const auto triangle = [&](std::size_t f, glm::vec2 (&l)[3], glm::vec2 (&uv)[3]) {
            const lkz::asset::TriangleRef ref = faceRef_[f];
            const lkz::MeshData& mesh = model_.parts[static_cast<std::size_t>(ref.part)].mesh;
            if (mesh.uvs.size() != mesh.positions.size()) {
                return false;
            }
            const auto index = lkz::asset::triangleAt(mesh, static_cast<std::size_t>(ref.triangle));
            for (std::size_t k = 0; k < 3; ++k) {
                l[k] = local(mesh.positions[index[k]]);
                uv[k] = mesh.uvs[index[k]];
            }
            return true;
        };
        std::optional<glm::mat2> toUv;
        glm::vec2 l0{0.0f};
        glm::vec2 uv0{0.0f};
        for (const std::size_t f : group) {
            glm::vec2 l[3];
            glm::vec2 uv[3];
            if (!triangle(f, l, uv)) {
                continue;
            }
            const glm::mat2 edges{l[1] - l[0], l[2] - l[0]};
            if (std::abs(glm::determinant(edges)) < 1e-8f) {
                continue;
            }
            toUv = glm::mat2{uv[1] - uv[0], uv[2] - uv[0]} * glm::inverse(edges);
            l0 = l[0];
            uv0 = uv[0];
            break;
        }
        if (!toUv || std::abs(glm::determinant(*toUv)) < 1e-8f) {
            continue;
        }
        bool consistent = true;
        for (const std::size_t f : group) {
            glm::vec2 l[3];
            glm::vec2 uv[3];
            if (!triangle(f, l, uv)) {
                consistent = false;
                break;
            }
            for (int k = 0; k < 3; ++k) {
                consistent = consistent && glm::length(*toUv * (l[k] - l0) + uv0 - uv[k]) < 1e-3f;
            }
        }
        if (!consistent) {
            continue; // não é um retângulo de carimbo: fica fixo
        }
        const glm::mat2 back = glm::inverse(*toUv);
        glm::vec2 lo{1e30f};
        glm::vec2 hi{-1e30f};
        for (const glm::vec2 corner : {glm::vec2{0.0f, 0.0f}, glm::vec2{1.0f, 0.0f}, glm::vec2{0.0f, 1.0f}, glm::vec2{1.0f, 1.0f}}) {
            const glm::vec2 q = back * (corner - uv0) + l0;
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
        }
        if (hi.x - lo.x < 1e-3f || hi.y - lo.y < 1e-3f) {
            continue;
        }
        Saved stamp;
        stamp.normal = normal;
        stamp.plane = glm::dot(normal, faceCenter_[largest]);
        stamp.placement.decal = entry.library;
        stamp.placement.origin = lo;
        stamp.placement.size = hi - lo;
        // Para onde anda o u da imagem: com o u da parede, giro 0 ou 2; com o v, 1 ou 3.
        const glm::mat2& m = *toUv;
        stamp.placement.quarterTurns = std::abs(m[0][0]) >= std::abs(m[1][0]) ? (m[0][0] > 0.0f ? 0 : 2) : (m[1][0] > 0.0f ? 1 : 3);
        // O retângulo tem de reproduzir o UV de todos os triângulos (um UV torto não é um
        // carimbo: fica fixo).
        bool exact = true;
        for (const std::size_t f : group) {
            glm::vec2 l[3];
            glm::vec2 uv[3];
            triangle(f, l, uv);
            for (int k = 0; k < 3 && exact; ++k) {
                exact = glm::length(decalUv(stamp.placement, l[k]) - uv[k]) < 2e-3f;
            }
        }
        if (!exact) {
            continue;
        }
        if (entry.whiteOnly.x >= 0.0f) {
            stamp.placement.tintMode = DecalTint::WhiteOnly;
            stamp.placement.tint = entry.whiteOnly;
        } else if (seedFace.colored) {
            stamp.placement.tintMode = DecalTint::Multiply;
            stamp.placement.tint = glm::vec3{seedFace.color[0], seedFace.color[1], seedFace.color[2]} / 255.0f;
        }
        // Um carimbo pode ter virado grupos sem aresta em comum: os iguais são um só.
        int s = -1;
        for (std::size_t other = adoptedFrom; other < saved.size(); ++other) {
            const Saved& same = saved[other];
            if (same.placement.decal == stamp.placement.decal && same.placement.quarterTurns == stamp.placement.quarterTurns &&
                same.placement.tintMode == stamp.placement.tintMode && glm::length(same.placement.tint - stamp.placement.tint) < 1e-3f &&
                glm::dot(same.normal, stamp.normal) > 0.999f && std::abs(same.plane - stamp.plane) < tolerance &&
                glm::length(same.placement.origin - stamp.placement.origin) < tolerance &&
                glm::length(same.placement.size - stamp.placement.size) < tolerance) {
                s = static_cast<int>(other);
                break;
            }
        }
        if (s < 0) {
            s = static_cast<int>(saved.size());
            saved.push_back(stamp);
        }
        for (const std::size_t f : group) {
            decalOwner[f] = s;
        }
    }
    if (saved.empty()) {
        return 0;
    }

    // O que fica embaixo de cada carimbo: a face da parede em volta mais perto do retângulo
    // (o destino e o UV dela, estendido ao buraco).
    struct Under {
        std::size_t face = 0;
        std::optional<lkz::asset::SurfaceToUv> toUv;
        bool found = false;
    };
    std::vector<Under> under(saved.size());
    for (std::size_t s = 0; s < saved.size(); ++s) {
        const Saved& stamp = saved[s];
        const SurfaceAxes axes = surfaceAxes(stamp.normal);
        const glm::vec2 lo = stamp.placement.origin;
        const glm::vec2 hi = lo + stamp.placement.size;
        float best = 1e30f;
        for (std::size_t f = 0; f < faceTotal; ++f) {
            if (faces_[f].target == SurfaceTarget::Decal || decalOwner[f] >= 0 || sideOwner[f] >= 0 ||
                glm::dot(faceNormal_[f], stamp.normal) < 0.999f ||
                std::abs(glm::dot(stamp.normal, faceCenter_[f]) - stamp.plane) > tolerance || faceArea_[f] <= 1e-10f) {
                continue;
            }
            const glm::vec2 c{glm::dot(faceCenter_[f], axes.u), glm::dot(faceCenter_[f], axes.v)};
            const float distance = glm::length(glm::max(glm::max(lo - c, c - hi), glm::vec2{0.0f}));
            if (distance < best) {
                best = distance;
                under[s].face = f;
                under[s].found = true;
            }
        }
        if (under[s].found) {
            const lkz::asset::TriangleRef ref = faceRef_[under[s].face];
            const lkz::MeshData& mesh = model_.parts[static_cast<std::size_t>(ref.part)].mesh;
            if (mesh.uvs.size() == mesh.positions.size()) {
                const auto index = lkz::asset::triangleAt(mesh, static_cast<std::size_t>(ref.triangle));
                const glm::vec3 p[3] = {mesh.positions[index[0]], mesh.positions[index[1]], mesh.positions[index[2]]};
                const glm::vec2 uv[3] = {mesh.uvs[index[0]], mesh.uvs[index[1]], mesh.uvs[index[2]]};
                under[s].toUv = lkz::asset::SurfaceToUv::from(p, uv);
            }
        }
    }

    // Partes novas: os triângulos de cada carimbo (por material) e as paredes do vão dele.
    struct NewPart {
        lkz::asset::ModelPart part;
        std::vector<Face> faces;
        std::vector<Face> initial;
        std::vector<int> families;
        std::vector<Origin> origins;
        int stamp = -1;
        bool side = false;
    };
    std::vector<NewPart> added;
    std::map<std::tuple<int, int, bool>, std::size_t> addedIndex; // (carimbo, material, vão)
    std::vector<std::vector<std::uint32_t>> kept(model_.parts.size());
    std::vector<std::vector<Face>> keptFaces(model_.parts.size());
    std::vector<std::vector<Face>> keptInitial(model_.parts.size());
    std::vector<std::vector<int>> keptFamilies(model_.parts.size());
    int restored = 0;
    std::vector<char> counted(saved.size(), 0);
    for (std::size_t p = 0; p < model_.parts.size(); ++p) {
        const lkz::MeshData& mesh = model_.parts[p].mesh;
        const bool hasUv = mesh.uvs.size() == mesh.positions.size();
        const std::size_t count = lkz::asset::triangleCount(mesh);
        for (std::size_t t = 0; t < count; ++t) {
            const auto f = static_cast<std::size_t>(firstFace_[p]) + t;
            const auto index = lkz::asset::triangleAt(mesh, t);
            const int stamp = decalOwner[f] >= 0 ? decalOwner[f] : sideOwner[f];
            if (stamp < 0 || !under[static_cast<std::size_t>(stamp)].found) {
                kept[p].insert(kept[p].end(), index.begin(), index.end());
                keptFaces[p].push_back(faces_[f]);
                keptInitial[p].push_back(initialFaces_[f]);
                keptFamilies[p].push_back(family_[f]);
                continue;
            }
            const bool side = decalOwner[f] < 0;
            const auto key = std::make_tuple(stamp, model_.parts[p].material, side);
            auto [it, fresh] = addedIndex.try_emplace(key, added.size());
            if (fresh) {
                NewPart part;
                part.part.name = (side ? "vao_" : "decal_") + decalLibrary_->at(saved[static_cast<std::size_t>(stamp)].placement.decal).id;
                part.part.material = model_.parts[p].material;
                part.stamp = stamp;
                part.side = side;
                added.push_back(std::move(part));
            }
            NewPart& to = added[it->second];
            const Saved& info = saved[static_cast<std::size_t>(stamp)];
            const Under& below = under[static_cast<std::size_t>(stamp)];
            const glm::vec3 offset = info.normal * info.placement.depth;
            Origin origin;
            origin.part = partId_[static_cast<std::size_t>(faceRef_[below.face].part)];
            origin.family = family_[below.face];
            origin.face = faces_[below.face];
            origin.initial = initialFaces_[below.face];
            for (std::size_t k = 0; k < 3; ++k) {
                const glm::vec3 position = mesh.positions[index[k]];
                to.part.mesh.positions.push_back(position);
                to.part.mesh.normals.push_back(mesh.normals.size() == mesh.positions.size() ? mesh.normals[index[k]] : faceNormal_[f]);
                to.part.mesh.uvs.push_back(hasUv ? mesh.uvs[index[k]] : glm::vec2{0.0f});
                to.part.mesh.indices.push_back(static_cast<std::uint32_t>(to.part.mesh.positions.size() - 1));
                if (below.toUv) {
                    origin.uv[k] = below.toUv->at(position - offset); // onde ele fica de volta na parede
                }
            }
            to.faces.push_back(faces_[f]);
            to.initial.push_back(initialFaces_[f]);
            to.families.push_back(family_[f]);
            to.origins.push_back(side ? Origin{} : origin);
            if (!side && counted[static_cast<std::size_t>(stamp)] == 0) {
                counted[static_cast<std::size_t>(stamp)] = 1;
                ++restored;
            }
        }
    }
    if (added.empty()) {
        return 0;
    }
    for (std::size_t p = 0; p < model_.parts.size(); ++p) {
        setTriangles(model_.parts[p].mesh, std::move(kept[p]));
    }
    faces_.clear();
    initialFaces_.clear();
    family_.clear();
    for (std::size_t p = 0; p < model_.parts.size(); ++p) {
        faces_.insert(faces_.end(), keptFaces[p].begin(), keptFaces[p].end());
        initialFaces_.insert(initialFaces_.end(), keptInitial[p].begin(), keptInitial[p].end());
        family_.insert(family_.end(), keptFamilies[p].begin(), keptFamilies[p].end());
    }
    // As partes do decalque primeiro (os ids delas são os donos das paredes do vão).
    std::vector<int> decalIdOf(saved.size(), -1);
    for (int pass = 0; pass < 2; ++pass) {
        for (NewPart& part : added) {
            if (part.side != (pass == 1)) {
                continue;
            }
            const Saved& info = saved[static_cast<std::size_t>(part.stamp)];
            auto record = std::make_shared<StampRecord>();
            record->placement = info.placement;
            record->placement.face = -1;
            record->origins = std::move(part.origins);
            record->offset = info.normal * info.placement.depth;
            const int id = nextPartId_++;
            if (part.side) {
                record->owner = decalIdOf[static_cast<std::size_t>(part.stamp)];
                if (record->owner < 0) {
                    record.reset(); // o decalque dela não foi achado: vira parede comum
                }
            } else if (decalIdOf[static_cast<std::size_t>(part.stamp)] < 0) {
                decalIdOf[static_cast<std::size_t>(part.stamp)] = id;
            }
            model_.parts.push_back(std::move(part.part));
            faces_.insert(faces_.end(), part.faces.begin(), part.faces.end());
            initialFaces_.insert(initialFaces_.end(), part.initial.begin(), part.initial.end());
            family_.insert(family_.end(), part.families.begin(), part.families.end());
            partId_.push_back(id);
            stampOf_.push_back(std::move(record));
        }
    }
    buildFaceGeometry();
    raycaster_ = lkz::asset::MeshRaycaster{model_};
    return restored;
}

} // namespace painter
