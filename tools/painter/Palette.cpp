#include "Palette.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <system_error>

#include <glm/common.hpp>
#include <glm/exponential.hpp>
#include <glm/geometric.hpp>
#include <lkz/Asset/Image.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

namespace painter {

namespace {

namespace fs = std::filesystem;

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

fs::path configOf(const fs::path& image) {
    fs::path config = image;
    return config.replace_extension(".json");
}

nlohmann::json readJson(const fs::path& file) {
    std::ifstream in{file};
    if (!in) {
        return nlohmann::json::object();
    }
    nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
    return json.is_object() ? json : nlohmann::json::object();
}

bool isImage(const fs::path& file) {
    const std::string extension = lower(utf8(file.extension()));
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg";
}

/// A imagem e o .json ao lado dela.
std::optional<PaletteMaterial> readMaterial(const fs::path& file) {
    auto image = lkz::asset::loadImage(file);
    if (!image) {
        return std::nullopt;
    }
    const nlohmann::json config = readJson(configOf(file));
    PaletteMaterial material;
    material.id = utf8(file.stem());
    material.name = material.id;
    material.image = file;
    if (const auto name = config.find("nome"); name != config.end() && name->is_string()) {
        material.name = name->get<std::string>();
    }
    if (const auto meters = config.find("metrosPorRepeticao"); meters != config.end() && meters->is_number()) {
        material.metersPerRepeat = std::max(meters->get<float>(), 0.01f);
    }
    if (const auto width = config.find("larguraMetros"); width != config.end() && width->is_number()) {
        material.widthMeters = std::max(width->get<float>(), 0.01f);
    }
    material.transparent = lkz::asset::hasTransparency(*image);
    material.mips.push_back(std::move(*image));
    return material;
}

/// Letra sem acento para um caractere de 2 bytes do UTF-8 que começa com 0xC3 (Latin-1).
char unaccent(unsigned char second) {
    const unsigned char c = second >= 0xA0 ? static_cast<unsigned char>(second - 0x20) : second; // minúscula -> maiúscula
    if (c >= 0x80 && c <= 0x85) return 'a';
    if (c == 0x87) return 'c';
    if (c >= 0x88 && c <= 0x8B) return 'e';
    if (c >= 0x8C && c <= 0x8F) return 'i';
    if (c == 0x91) return 'n';
    if (c >= 0x92 && c <= 0x96) return 'o';
    if (c >= 0x99 && c <= 0x9C) return 'u';
    if (c == 0x9D || c == 0x9F) return 'y';
    return '-';
}

} // namespace

Palette Palette::load(const fs::path& folder, std::string& report, Kind kind) {
    Palette palette;
    palette.kind_ = kind;
    std::error_code ec;
    palette.folder_ = fs::absolute(folder, ec).lexically_normal();
    const char* what = kind == Kind::Materials ? "Paleta" : "Decalques";
    if (!fs::is_directory(folder, ec)) {
        report = fmt::format("{}: a pasta {} ainda não existe", what, utf8(folder));
        return palette;
    }
    std::vector<fs::path> images;
    for (const auto& entry : fs::directory_iterator{folder, ec}) {
        if (entry.is_regular_file(ec) && isImage(entry.path())) {
            images.push_back(fs::absolute(entry.path(), ec).lexically_normal());
        }
    }
    std::sort(images.begin(), images.end());
    std::vector<std::string> failed;
    for (const fs::path& file : images) {
        if (auto material = readMaterial(file)) {
            palette.add(std::move(*material));
        } else {
            failed.push_back(utf8(file.filename()));
        }
    }
    report = fmt::format("{}: {} {}", what, palette.size(), palette.size() == 1 ? "imagem" : "imagens");
    if (!failed.empty()) {
        std::string list;
        for (const std::string& name : failed) {
            list += (list.empty() ? "" : ", ") + name;
        }
        report += " (não abriram: " + list + ")";
    }
    return palette;
}

void Palette::add(PaletteMaterial material) {
    if (material.mips.empty() || material.mips.front().empty()) {
        return;
    }
    material.transparent = lkz::asset::hasTransparency(material.mips.front());
    material.mips = lkz::asset::mipChain(material.mips.front());
    materials_.push_back(std::move(material));
}

int Palette::find(std::string_view id) const {
    for (std::size_t i = 0; i < materials_.size(); ++i) {
        if (materials_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool Palette::setMetersPerRepeat(int index, float meters, std::string& error) {
    if (index < 0 || index >= size()) {
        error = "Material da paleta inexistente";
        return false;
    }
    PaletteMaterial& material = materials_[static_cast<std::size_t>(index)];
    material.metersPerRepeat = std::max(meters, 0.01f);
    if (material.image.empty()) {
        return true; // feito na memória (testes): nada a gravar
    }
    const fs::path file = configOf(material.image);
    nlohmann::json config = readJson(file);
    config["nome"] = material.name;
    config["metrosPorRepeticao"] = std::round(material.metersPerRepeat * 1000.0f) / 1000.0f;
    std::ofstream out{file, std::ios::trunc};
    out << config.dump(2) << '\n';
    if (!out) {
        error = "Não consegui gravar " + utf8(file);
        return false;
    }
    return true;
}

std::string Palette::importName(const fs::path& source) {
    const std::string stem = utf8(source.stem());
    std::string name;
    for (std::size_t i = 0; i < stem.size(); ++i) {
        const auto c = static_cast<unsigned char>(stem[i]);
        char out = '-';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out = static_cast<char>(c);
        } else if (c >= 'A' && c <= 'Z') {
            out = static_cast<char>(c - 'A' + 'a');
        } else if (c == 0xC3 && i + 1 < stem.size()) {
            out = unaccent(static_cast<unsigned char>(stem[++i]));
        } else if (c >= 0x80) {
            while (i + 1 < stem.size() && (static_cast<unsigned char>(stem[i + 1]) & 0xC0) == 0x80) {
                ++i; // o resto do caractere
            }
        }
        if (out != '-' || (!name.empty() && name.back() != '-')) {
            name += out;
        }
    }
    while (!name.empty() && name.back() == '-') {
        name.pop_back();
    }
    if (name.empty()) {
        name = "imagem";
    }
    std::string extension = lower(utf8(source.extension()));
    if (extension != ".png" && extension != ".jpg" && extension != ".jpeg") {
        extension = ".png";
    }
    return name + extension;
}

int Palette::importFile(const fs::path& source, const std::string& fileName, std::string& error) {
    if (folder_.empty()) {
        error = "Sem pasta para importar (defina-a no arquivo de projeto)";
        return -1;
    }
    std::error_code ec;
    fs::create_directories(folder_, ec);
    const fs::path target = folder_ / fromUtf8(fileName);
    if (!fs::exists(target, ec) || !fs::equivalent(source, target, ec)) {
        fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "Não consegui copiar " + utf8(source.filename()) + ": " + ec.message();
            return -1;
        }
    }
    // O .json com o nome original e a escala padrão, se ainda não houver um.
    if (const fs::path config = configOf(target); !fs::exists(config, ec)) {
        nlohmann::json json = nlohmann::json::object();
        json["nome"] = utf8(source.stem());
        json[kind_ == Kind::Materials ? "metrosPorRepeticao" : "larguraMetros"] = 1.0;
        std::ofstream out{config, std::ios::trunc};
        out << json.dump(2) << '\n';
    }
    auto material = readMaterial(target);
    if (!material) {
        error = utf8(source.filename()) + " não é uma imagem que eu consiga abrir";
        return -1;
    }
    material->transparent = lkz::asset::hasTransparency(material->mips.front());
    material->mips = lkz::asset::mipChain(material->mips.front());
    if (const int index = find(material->id); index >= 0) {
        materials_[static_cast<std::size_t>(index)] = std::move(*material);
        return index;
    }
    materials_.push_back(std::move(*material));
    return size() - 1;
}

glm::vec4 Palette::sample(int index, const glm::vec2& uv, float texelsPerMeter) const {
    const PaletteMaterial& material = at(index);
    const lkz::asset::ImageData& top = material.mips.front();
    // Quantos pixels da imagem cabem num texel de quem pinta: o mipmap desse tamanho.
    const float imagePerMeter = static_cast<float>(top.width) / material.metersPerRepeat;
    const float level = std::log2(std::max(imagePerMeter / std::max(texelsPerMeter, 1e-3f), 1.0f));
    const int last = static_cast<int>(material.mips.size()) - 1;
    const int low = std::min(static_cast<int>(level), last);
    const int high = std::min(low + 1, last);
    const auto at = [&](int l) {
        const lkz::asset::ImageData& image = material.mips[static_cast<std::size_t>(l)];
        return lkz::asset::sampleBilinear(image, uv * glm::vec2{static_cast<float>(image.width), static_cast<float>(image.height)});
    };
    const float t = level - static_cast<float>(low);
    return low == high || t <= 0.0f ? at(low) : glm::mix(at(low), at(high), t);
}

SurfaceAxes surfaceAxes(const glm::vec3& n) {
    SurfaceAxes axes;
    if (std::abs(n.y) > 0.97f) {
        // Quase horizontal: x e z, levados para o plano da face.
        axes.u = glm::normalize(glm::vec3{1.0f, 0.0f, 0.0f} - n * n.x);
        const glm::vec3 z = glm::vec3{0.0f, 0.0f, 1.0f} - n * n.z;
        axes.v = glm::normalize(z - axes.u * glm::dot(z, axes.u));
        return axes;
    }
    axes.u = glm::normalize(glm::vec3{n.z, 0.0f, -n.x});
    axes.v = glm::normalize(glm::cross(axes.u, n)); // para baixo pela face
    return axes;
}

glm::vec2 surfaceMeters(const glm::vec3& position, const glm::vec3& faceNormal) {
    const SurfaceAxes axes = surfaceAxes(faceNormal);
    return {glm::dot(position, axes.u), glm::dot(position, axes.v)};
}

} // namespace painter
