#include "lkz/Asset/Gltf.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <unordered_map>

#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "lkz/Asset/Image.hpp"

// A raylib embute outra cópia do cgltf (funções em C com os mesmos nomes): a nossa ganha
// o prefixo lkz_ (CgltfPrefix.hpp). Esta é a única cópia da engine fora da raylib.
#include "CgltfPrefix.hpp"
#define CGLTF_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <cgltf.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace lkz::asset {

std::size_t ModelData::triangleCount() const {
    std::size_t count = 0;
    for (const ModelPart& part : parts) {
        count += asset::triangleCount(part.mesh);
    }
    return count;
}

std::size_t ModelData::vertexCount() const {
    std::size_t count = 0;
    for (const ModelPart& part : parts) {
        count += part.mesh.positions.size();
    }
    return count;
}

std::size_t ModelData::estimatedTextureMemory() const {
    std::size_t bytes = 0;
    for (const ImageData& image : images) {
        if (image.uri.empty()) {
            bytes += static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4 * 4 / 3;
        }
    }
    return bytes;
}

namespace {

// ---------------------------------------------------------------- leitura

std::string extrasJson(const cgltf_data* data, const cgltf_extras& extras) {
    cgltf_size size = 0;
    if (cgltf_copy_extras_json(data, &extras, nullptr, &size) != cgltf_result_success || size <= 1) {
        return {};
    }
    std::string json(size, '\0');
    cgltf_copy_extras_json(data, &extras, json.data(), &size);
    json.resize(std::char_traits<char>::length(json.c_str()));
    return json;
}

/// Arquivo de uma imagem externa (uri relativa à pasta do glTF, com %XX decodificado).
std::filesystem::path externalFile(const cgltf_image& image, const std::filesystem::path& directory) {
    if (image.buffer_view != nullptr || image.uri == nullptr || std::strncmp(image.uri, "data:", 5) == 0) {
        return {};
    }
    std::string uri = image.uri;
    cgltf_decode_uri(uri.data());
    uri.resize(std::char_traits<char>::length(uri.c_str()));
    std::error_code error;
    const std::filesystem::path file = directory / std::filesystem::path{std::u8string{uri.begin(), uri.end()}};
    const std::filesystem::path absolute = std::filesystem::absolute(file, error);
    return (error ? file : absolute).lexically_normal();
}

std::optional<ImageData> readImage(const cgltf_image& image, const std::filesystem::path& directory,
                                   const GltfLoadOptions& options) {
    const std::string name = image.name != nullptr ? image.name : (image.uri != nullptr ? image.uri : "imagem");
    if (image.buffer_view != nullptr && image.buffer_view->buffer->data != nullptr) {
        const auto* bytes = static_cast<const std::uint8_t*>(image.buffer_view->buffer->data) + image.buffer_view->offset;
        auto decoded = decodeImage(bytes, image.buffer_view->size, name);
        if (decoded) {
            decoded->name = image.name != nullptr ? image.name : "";
        }
        return decoded;
    }
    if (const std::filesystem::path file = externalFile(image, directory); !file.empty()) {
        std::optional<ImageData> decoded = options.externalImagePixels ? loadImage(file) : std::optional<ImageData>{ImageData{}};
        if (decoded) {
            decoded->uri = image.uri;
            decoded->file = file;
            decoded->name = image.name != nullptr ? image.name : "";
        }
        return decoded;
    }
    spdlog::warn("glTF: imagem '{}' em data URI não é suportada; fica de fora", name);
    return std::nullopt;
}

/// extras.lkz do material: destino, material da paleta e escala.
void readTarget(MaterialData& material) {
    if (material.extras.empty()) {
        return;
    }
    const nlohmann::json extras = nlohmann::json::parse(material.extras, nullptr, false);
    if (!extras.is_object() || !extras.contains("lkz") || !extras["lkz"].is_object()) {
        return;
    }
    const nlohmann::json& lkz = extras["lkz"];
    const auto text = [&](const char* key) {
        const auto it = lkz.find(key);
        return it != lkz.end() && it->is_string() ? it->get<std::string>() : std::string{};
    };
    material.shared = text("shared");
    if (const auto meters = lkz.find("metersPerRepeat"); meters != lkz.end() && meters->is_number()) {
        material.metersPerRepeat = meters->get<float>();
    }
    std::string target = text("target");
    if (target.empty()) {
        target = material.shared.empty() ? "unique" : "shared";
    }
    material.target = target == "shared"  ? SurfaceTarget::Shared
                      : target == "color" ? SurfaceTarget::Color
                      : target == "decal" ? SurfaceTarget::Decal
                                          : SurfaceTarget::Unique;
}

void readPrimitive(const cgltf_data* data, const cgltf_mesh& mesh, const cgltf_primitive& primitive, const glm::mat4& world,
                   ModelData& model) {
    if (primitive.type != cgltf_primitive_type_triangles) {
        return;
    }
    const cgltf_accessor* positions = cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
    if (positions == nullptr) {
        return;
    }
    const cgltf_accessor* normals = cgltf_find_accessor(&primitive, cgltf_attribute_type_normal, 0);
    const cgltf_accessor* uvs = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 0);
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3{world}));

    ModelPart part;
    part.name = mesh.name != nullptr ? mesh.name : "";
    part.material = primitive.material != nullptr ? static_cast<int>(primitive.material - data->materials) : -1;
    MeshData& out = part.mesh;
    out.positions.resize(positions->count);
    out.normals.resize(positions->count, glm::vec3{0.0f, 1.0f, 0.0f});
    out.uvs.resize(positions->count, glm::vec2{0.0f});
    for (cgltf_size i = 0; i < positions->count; ++i) {
        float value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cgltf_accessor_read_float(positions, i, value, 3);
        out.positions[i] = glm::vec3{world * glm::vec4{value[0], value[1], value[2], 1.0f}};
        if (normals != nullptr && i < normals->count) {
            cgltf_accessor_read_float(normals, i, value, 3);
            const glm::vec3 n = normalMatrix * glm::vec3{value[0], value[1], value[2]};
            const float length = glm::length(n);
            out.normals[i] = length > 1e-12f ? n / length : glm::vec3{0.0f, 1.0f, 0.0f};
        }
        if (uvs != nullptr && i < uvs->count) {
            cgltf_accessor_read_float(uvs, i, value, 2);
            out.uvs[i] = {value[0], value[1]};
        }
    }
    if (primitive.indices != nullptr) {
        out.indices.resize(primitive.indices->count);
        for (cgltf_size i = 0; i < primitive.indices->count; ++i) {
            out.indices[i] = static_cast<std::uint32_t>(cgltf_accessor_read_index(primitive.indices, i));
        }
    } else {
        out.indices.resize(positions->count - positions->count % 3);
        for (std::size_t i = 0; i < out.indices.size(); ++i) {
            out.indices[i] = static_cast<std::uint32_t>(i);
        }
    }
    // Nó espelhado (determinante negativo): a frente dos triângulos se inverte.
    if (glm::determinant(glm::mat3{world}) < 0.0f) {
        for (std::size_t i = 0; i + 2 < out.indices.size(); i += 3) {
            std::swap(out.indices[i + 1], out.indices[i + 2]);
        }
    }
    model.parts.push_back(std::move(part));
}

void readNode(const cgltf_data* data, const cgltf_node* node, ModelData& model) {
    if (node->mesh != nullptr) {
        float matrix[16];
        cgltf_node_transform_world(node, matrix);
        const glm::mat4 world = glm::make_mat4(matrix);
        for (cgltf_size i = 0; i < node->mesh->primitives_count; ++i) {
            readPrimitive(data, *node->mesh, node->mesh->primitives[i], world, model);
        }
    }
    for (cgltf_size i = 0; i < node->children_count; ++i) {
        readNode(data, node->children[i], model);
    }
}

// ---------------------------------------------------------------- gravação

std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<int>(c));
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

std::string number(float value) {
    return std::isfinite(value) ? std::format("{}", value) : std::string{"0"};
}

/// Bloco binário do .glb com os buffer views (cada um alinhado a 4 bytes).
struct BinaryBuilder {
    std::vector<std::uint8_t> bytes;
    std::vector<std::string> views; // JSON de cada bufferView

    int add(const void* data, std::size_t size, int target) {
        while (bytes.size() % 4 != 0) {
            bytes.push_back(0);
        }
        const std::size_t offset = bytes.size();
        const auto* begin = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), begin, begin + size);
        std::string view = std::format(R"({{"buffer":0,"byteOffset":{},"byteLength":{})", offset, size);
        if (target != 0) {
            view += std::format(R"(,"target":{})", target);
        }
        views.push_back(view + "}");
        return static_cast<int>(views.size()) - 1;
    }
};

const char* alphaModeName(AlphaMode mode) {
    switch (mode) {
    case AlphaMode::Mask: return "MASK";
    case AlphaMode::Blend: return "BLEND";
    default: return "OPAQUE";
    }
}

} // namespace

std::vector<std::filesystem::path> materialImageFiles(const std::filesystem::path& path) {
    // Só o JSON: num .glb, o bloco do começo (os dados binários, com as imagens embutidas,
    // ficam sem ler).
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return {};
    }
    std::string json;
    std::uint32_t header[5] = {};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (in.gcount() == sizeof(header) && header[0] == 0x46546C67 && header[4] == 0x4E4F534A) { // "glTF", "JSON"
        json.resize(header[3]);
        in.read(json.data(), static_cast<std::streamsize>(json.size()));
        json.resize(static_cast<std::size_t>(in.gcount()));
    } else {
        in.clear();
        in.seekg(0);
        json.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    }
    cgltf_options options{};
    options.type = cgltf_file_type_gltf;
    cgltf_data* data = nullptr;
    if (cgltf_parse(&options, json.data(), json.size(), &data) != cgltf_result_success) {
        return {};
    }
    std::vector<std::filesystem::path> files(data->materials_count);
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material& material = data->materials[i];
        if (material.has_pbr_metallic_roughness) {
            const cgltf_texture* texture = material.pbr_metallic_roughness.base_color_texture.texture;
            if (texture != nullptr && texture->image != nullptr) {
                files[i] = externalFile(*texture->image, path.parent_path());
            }
        }
    }
    cgltf_free(data);
    return files;
}

std::optional<ModelData> loadGltf(const std::filesystem::path& path, const GltfLoadOptions& loadOptions) {
    const std::string file = path.string();
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, file.c_str(), &data) != cgltf_result_success) {
        spdlog::error("glTF: não abriu '{}'", file);
        return std::nullopt;
    }
    if (cgltf_load_buffers(&options, data, file.c_str()) != cgltf_result_success) {
        spdlog::error("glTF: não leu os dados de '{}'", file);
        cgltf_free(data);
        return std::nullopt;
    }
    ModelData model;
    const std::filesystem::path directory = path.parent_path();

    // Imagens: índice no arquivo -> índice em model.images (as que não abrem ficam de fora).
    std::vector<int> imageIndex(data->images_count, -1);
    for (cgltf_size i = 0; i < data->images_count; ++i) {
        if (auto image = readImage(data->images[i], directory, loadOptions)) {
            imageIndex[i] = static_cast<int>(model.images.size());
            model.images.push_back(std::move(*image));
        }
    }
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material& source = data->materials[i];
        MaterialData material;
        material.name = source.name != nullptr ? source.name : "";
        if (source.has_pbr_metallic_roughness) {
            const auto& pbr = source.pbr_metallic_roughness;
            material.baseColor = glm::make_vec4(pbr.base_color_factor);
            material.metallic = pbr.metallic_factor;
            material.roughness = pbr.roughness_factor;
            if (pbr.base_color_texture.texture != nullptr && pbr.base_color_texture.texture->image != nullptr) {
                material.image = imageIndex[static_cast<std::size_t>(pbr.base_color_texture.texture->image - data->images)];
            }
        }
        material.doubleSided = source.double_sided != 0;
        material.alphaMode = source.alpha_mode == cgltf_alpha_mode_mask    ? AlphaMode::Mask
                             : source.alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::Blend
                                                                           : AlphaMode::Opaque;
        material.alphaCutoff = source.alpha_cutoff;
        material.extras = extrasJson(data, source.extras);
        readTarget(material);
        model.materials.push_back(std::move(material));
    }
    const cgltf_scene* scene = data->scene != nullptr ? data->scene : (data->scenes_count > 0 ? data->scenes : nullptr);
    if (scene != nullptr) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
            readNode(data, scene->nodes[i], model);
        }
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; ++i) {
            if (data->nodes[i].parent == nullptr) {
                readNode(data, &data->nodes[i], model);
            }
        }
    }
    model.extras = extrasJson(data, data->asset.extras);
    cgltf_free(data);
    return model;
}

bool saveGlb(const std::filesystem::path& path, const ModelData& model, const GlbOptions& options) {
    BinaryBuilder binary;
    std::vector<std::string> accessors;
    std::vector<std::string> meshes;
    std::vector<std::string> nodes;

    for (std::size_t p = 0; p < model.parts.size(); ++p) {
        const ModelPart& part = model.parts[p];
        const MeshData& mesh = part.mesh;
        if (mesh.positions.empty()) {
            continue;
        }
        const std::size_t count = mesh.positions.size();
        glm::vec3 low{std::numeric_limits<float>::max()};
        glm::vec3 high{std::numeric_limits<float>::lowest()};
        for (const glm::vec3& v : mesh.positions) {
            low = glm::min(low, v);
            high = glm::max(high, v);
        }
        const int positionView = binary.add(mesh.positions.data(), count * sizeof(glm::vec3), 34962);
        accessors.push_back(std::format(
            R"({{"bufferView":{},"componentType":5126,"count":{},"type":"VEC3","min":[{},{},{}],"max":[{},{},{}]}})",
            positionView, count, number(low.x), number(low.y), number(low.z), number(high.x), number(high.y), number(high.z)));
        std::string attributes = std::format(R"("POSITION":{})", accessors.size() - 1);
        if (mesh.normals.size() == count) {
            const int view = binary.add(mesh.normals.data(), count * sizeof(glm::vec3), 34962);
            accessors.push_back(std::format(R"({{"bufferView":{},"componentType":5126,"count":{},"type":"VEC3"}})", view, count));
            attributes += std::format(R"(,"NORMAL":{})", accessors.size() - 1);
        }
        if (mesh.uvs.size() == count) {
            const int view = binary.add(mesh.uvs.data(), count * sizeof(glm::vec2), 34962);
            accessors.push_back(std::format(R"({{"bufferView":{},"componentType":5126,"count":{},"type":"VEC2"}})", view, count));
            attributes += std::format(R"(,"TEXCOORD_0":{})", accessors.size() - 1);
        }
        std::vector<std::uint32_t> indices = mesh.indices;
        if (indices.empty()) {
            indices.resize(count - count % 3);
            for (std::size_t i = 0; i < indices.size(); ++i) {
                indices[i] = static_cast<std::uint32_t>(i);
            }
        }
        int indexAccessor = -1;
        if (count <= 65535) {
            std::vector<std::uint16_t> shortIndices(indices.begin(), indices.end());
            const int view = binary.add(shortIndices.data(), shortIndices.size() * sizeof(std::uint16_t), 34963);
            accessors.push_back(std::format(R"({{"bufferView":{},"componentType":5123,"count":{},"type":"SCALAR"}})", view,
                                            shortIndices.size()));
        } else {
            spdlog::warn("glTF: parte '{}' tem {} vértices (mais que 65 535): índices de 32 bits", part.name, count);
            const int view = binary.add(indices.data(), indices.size() * sizeof(std::uint32_t), 34963);
            accessors.push_back(std::format(R"({{"bufferView":{},"componentType":5125,"count":{},"type":"SCALAR"}})", view,
                                            indices.size()));
        }
        indexAccessor = static_cast<int>(accessors.size()) - 1;
        std::string primitive = std::format(R"({{"attributes":{{{}}},"indices":{},"mode":4)", attributes, indexAccessor);
        if (part.material >= 0 && static_cast<std::size_t>(part.material) < model.materials.size()) {
            primitive += std::format(R"(,"material":{})", part.material);
        }
        primitive += "}";
        const std::string name = part.name.empty() ? std::format("parte_{}", p) : part.name;
        meshes.push_back(std::format(R"({{"name":{},"primitives":[{}]}})", quoted(name), primitive));
        nodes.push_back(std::format(R"({{"name":{},"mesh":{}}})", quoted(name), meshes.size() - 1));
    }

    // Imagens: embutidas (PNG com transparência, senão JPEG) ou externas pela uri.
    std::vector<std::string> images;
    for (const ImageData& image : model.images) {
        const std::string name = image.name.empty() ? std::string{} : std::format(R"("name":{},)", quoted(image.name));
        if (options.keepExternal && !image.uri.empty()) {
            images.push_back(std::format(R"({{{}"uri":{}}})", name, quoted(image.uri)));
            continue;
        }
        const bool png = options.forcePng || image.lossless || hasTransparency(image);
        const std::vector<std::uint8_t> encoded = png ? encodePng(image) : encodeJpeg(image, options.jpegQuality);
        const int view = binary.add(encoded.data(), encoded.size(), 0);
        images.push_back(std::format(R"({{{}"bufferView":{},"mimeType":"{}"}})", name, view, png ? "image/png" : "image/jpeg"));
    }
    std::vector<std::string> materials;
    for (const MaterialData& material : model.materials) {
        std::string pbr = std::format(R"("baseColorFactor":[{},{},{},{}],"metallicFactor":{},"roughnessFactor":{})",
                                      number(material.baseColor.r), number(material.baseColor.g), number(material.baseColor.b),
                                      number(material.baseColor.a), number(material.metallic), number(material.roughness));
        if (material.image >= 0 && static_cast<std::size_t>(material.image) < model.images.size()) {
            pbr += std::format(R"(,"baseColorTexture":{{"index":{}}})", material.image); // textura i = imagem i
        }
        std::string json = std::format(R"({{"name":{},"pbrMetallicRoughness":{{{}}})", quoted(material.name), pbr);
        if (material.doubleSided) {
            json += R"(,"doubleSided":true)";
        }
        if (material.alphaMode != AlphaMode::Opaque) {
            json += std::format(R"(,"alphaMode":"{}")", alphaModeName(material.alphaMode));
            if (material.alphaMode == AlphaMode::Mask) {
                json += std::format(R"(,"alphaCutoff":{})", number(material.alphaCutoff));
            }
        }
        if (material.target == SurfaceTarget::Decal) {
            json += std::format(R"(,"extras":{{"lkz":{{"target":"decal","shared":{}}}}})", quoted(material.shared));
        } else if (material.target == SurfaceTarget::Shared || !material.shared.empty()) {
            json += std::format(R"(,"extras":{{"lkz":{{"target":"shared","shared":{},"metersPerRepeat":{}}}}})",
                                quoted(material.shared), number(material.metersPerRepeat));
        } else if (material.target == SurfaceTarget::Color) {
            json += R"(,"extras":{"lkz":{"target":"color"}})";
        } else if (!material.extras.empty()) {
            json += R"(,"extras":)" + material.extras;
        }
        materials.push_back(json + "}");
    }

    const auto join = [](const std::vector<std::string>& items) {
        std::string out;
        for (std::size_t i = 0; i < items.size(); ++i) {
            out += (i > 0 ? "," : "") + items[i];
        }
        return out;
    };
    std::string json = R"json({"asset":{"version":"2.0","generator":"Lkz Engine (lkz::asset)")json";
    if (!model.extras.empty()) {
        json += R"(,"extras":)" + model.extras;
    }
    json += "}";
    std::vector<std::string> sceneNodes;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        sceneNodes.push_back(std::to_string(i));
    }
    json += std::format(R"(,"scene":0,"scenes":[{{"nodes":[{}]}}],"nodes":[{}],"meshes":[{}])", join(sceneNodes), join(nodes),
                        join(meshes));
    if (!materials.empty()) {
        json += std::format(R"(,"materials":[{}])", join(materials));
    }
    if (!images.empty()) {
        std::vector<std::string> textures;
        for (std::size_t i = 0; i < images.size(); ++i) {
            textures.push_back(std::format(R"({{"sampler":0,"source":{}}})", i));
        }
        // Repetição nos dois eixos, filtro linear com mipmaps.
        json += std::format(
            R"(,"samplers":[{{"magFilter":9729,"minFilter":9987,"wrapS":10497,"wrapT":10497}}],"images":[{}],"textures":[{}])",
            join(images), join(textures));
    }
    json += std::format(R"(,"accessors":[{}],"bufferViews":[{}],"buffers":[{{"byteLength":{}}}]}})", join(accessors),
                        join(binary.views), binary.bytes.size());
    while (json.size() % 4 != 0) {
        json += ' ';
    }
    while (binary.bytes.size() % 4 != 0) {
        binary.bytes.push_back(0);
    }

    // Cabeçalho + bloco JSON + bloco binário.
    const auto u32 = [](std::vector<std::uint8_t>& out, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
        }
    };
    std::vector<std::uint8_t> file;
    const auto total = static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + binary.bytes.size());
    u32(file, 0x46546C67); // "glTF"
    u32(file, 2);
    u32(file, total);
    u32(file, static_cast<std::uint32_t>(json.size()));
    u32(file, 0x4E4F534A); // "JSON"
    file.insert(file.end(), json.begin(), json.end());
    u32(file, static_cast<std::uint32_t>(binary.bytes.size()));
    u32(file, 0x004E4942); // "BIN"
    file.insert(file.end(), binary.bytes.begin(), binary.bytes.end());

    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        if (!out) {
            spdlog::error("glTF: não consegui gravar '{}'", temporary.string());
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        spdlog::error("glTF: não consegui substituir '{}': {}", path.string(), error.message());
        return false;
    }
    return true;
}

} // namespace lkz::asset
