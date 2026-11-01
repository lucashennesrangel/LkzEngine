// Testes do lkz::asset: gravar um .glb e relê-lo igual; imagens (PNG com alfa, JPEG,
// resize, dilate).

#include <cmath>
#include <cstdint>
#include <string>
#include <cstdio>
#include <filesystem>

#include <lkz/Asset/Gltf.hpp>
#include <glm/geometric.hpp>
#include <lkz/Asset/Bake.hpp>
#include <lkz/Asset/Image.hpp>
#include <lkz/Asset/Unwrap.hpp>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FALHOU: %s\n", what);
        ++failures;
    }
}

bool near(float a, float b, float tolerance = 1e-4f) {
    return std::abs(a - b) <= tolerance;
}

lkz::asset::ImageData checker(int size, bool alpha) {
    lkz::asset::ImageData image;
    image.width = size;
    image.height = size;
    image.rgba.resize(static_cast<std::size_t>(size * size * 4));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const auto i = static_cast<std::size_t>((y * size + x) * 4);
            const bool dark = ((x / 4) + (y / 4)) % 2 == 0;
            image.rgba[i] = dark ? 40 : 220;
            image.rgba[i + 1] = dark ? 60 : 200;
            image.rgba[i + 2] = dark ? 80 : 180;
            image.rgba[i + 3] = alpha && dark ? 0 : 255;
        }
    }
    return image;
}

/// Dois quadrados (duas partes), materiais com e sem imagem.
lkz::asset::ModelData sample() {
    lkz::asset::ModelData model;
    for (int p = 0; p < 2; ++p) {
        lkz::asset::ModelPart part;
        part.name = p == 0 ? "parede" : "folha";
        const float x = static_cast<float>(p) * 2.0f;
        part.mesh.positions = {{x, 0, 0}, {x, 1, 0}, {x + 1, 1, 0}, {x + 1, 0, 0}};
        part.mesh.normals.assign(4, {0.0f, 0.0f, 1.0f});
        part.mesh.uvs = {{0, 1}, {0, 0}, {3, 0}, {3, 1}}; // u > 1: repete
        part.mesh.indices = {0, 2, 1, 0, 3, 2};
        part.material = p;
        model.parts.push_back(part);
    }
    lkz::asset::MaterialData brick;
    brick.name = "tijolo";
    brick.baseColor = {0.9f, 0.8f, 0.7f, 1.0f};
    brick.image = 0;
    brick.shared = "tijolo-vermelho";
    brick.metersPerRepeat = 2.5f;
    lkz::asset::MaterialData leaf;
    leaf.name = "folha";
    leaf.image = 1;
    leaf.doubleSided = true;
    leaf.alphaMode = lkz::asset::AlphaMode::Mask;
    leaf.alphaCutoff = 0.4f;
    model.materials = {brick, leaf};
    model.images = {checker(16, false), checker(16, true)};
    model.images[0].name = "tijolo";
    model.extras = R"({"lkz":{"versao":1}})";
    return model;
}

void roundTrip() {
    const auto path = std::filesystem::temp_directory_path() / "lkz_asset_test.glb";
    const lkz::asset::ModelData original = sample();
    check(lkz::asset::saveGlb(path, original), "grava o .glb");
    const auto loaded = lkz::asset::loadGltf(path);
    check(loaded.has_value(), "relê o .glb");
    if (!loaded) {
        return;
    }
    check(loaded->parts.size() == 2, "duas partes");
    check(loaded->triangleCount() == 4 && loaded->vertexCount() == 8, "triângulos e vértices");
    check(loaded->materials.size() == 2 && loaded->images.size() == 2, "materiais e imagens");
    bool same = true;
    for (std::size_t p = 0; p < 2 && loaded->parts.size() == 2; ++p) {
        const auto& a = original.parts[p].mesh;
        const auto& b = loaded->parts[p].mesh;
        for (std::size_t i = 0; i < a.positions.size(); ++i) {
            same = same && near(a.positions[i].x, b.positions[i].x) && near(a.positions[i].y, b.positions[i].y) &&
                   near(a.uvs[i].x, b.uvs[i].x) && near(a.normals[i].z, b.normals[i].z);
        }
        same = same && a.indices == b.indices && loaded->parts[p].material == original.parts[p].material;
        same = same && loaded->parts[p].name == original.parts[p].name;
    }
    check(same, "posições, normais, UV (inclusive acima de 1), índices e material iguais");
    if (loaded->materials.size() == 2) {
        const auto& brick = loaded->materials[0];
        const auto& leaf = loaded->materials[1];
        check(brick.name == "tijolo" && near(brick.baseColor.g, 0.8f) && brick.image == 0, "material com imagem");
        check(brick.extras.find("tijolo-vermelho") != std::string::npos && brick.extras.find("2.5") != std::string::npos,
              "material compartilhado guardado em extras.lkz");
        check(leaf.doubleSided && leaf.alphaMode == lkz::asset::AlphaMode::Mask && near(leaf.alphaCutoff, 0.4f),
              "dois lados e recorte por alfa");
    }
    if (loaded->images.size() == 2) {
        check(loaded->images[0].width == 16 && !lkz::asset::hasTransparency(loaded->images[0]), "JPEG sem alfa");
        check(loaded->images[1].rgba == original.images[1].rgba, "PNG com alfa sem perda");
    }
    check(loaded->extras.find("versao") != std::string::npos, "asset.extras guardado");
    std::filesystem::remove(path);
}

void images() {
    const lkz::asset::ImageData image = checker(64, false);
    const lkz::asset::ImageData small = lkz::asset::resize(image, 16, 16);
    check(small.width == 16 && small.height == 16 && small.rgba.size() == 16 * 16 * 4, "resize");

    // Dilatar: só o pixel do meio está pintado; depois de 2 passos, o quadrado 5 x 5 tem a cor dele.
    lkz::asset::ImageData canvas;
    canvas.width = 9;
    canvas.height = 9;
    canvas.rgba.assign(9 * 9 * 4, 0);
    std::vector<std::uint8_t> coverage(81, 0);
    const std::size_t center = 4 * 9 + 4;
    canvas.rgba[center * 4] = 200;
    canvas.rgba[center * 4 + 3] = 255;
    coverage[center] = 1;
    lkz::asset::dilate(canvas, coverage, 2);
    check(coverage[2 * 9 + 2] == 1 && canvas.rgba[(2 * 9 + 2) * 4] == 200, "dilate espalha a cor");
    check(coverage[1 * 9 + 1] == 0, "dilate para no limite de pixels");
}

/// Cubo de 2 m com as 6 faces no mesmo UV (0..1) de uma imagem 4 x 4 de cores: UV todo
/// sobreposto. O UV único tem de separar as faces e manter as cores.
void uniqueUv() {
    lkz::asset::ModelData model;
    lkz::asset::ModelPart part;
    part.name = "cubo";
    const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const glm::vec3& n : normals) {
        // Dois eixos no plano da face, formando base direita com n.
        const glm::vec3 a = std::abs(n.y) > 0.5f ? glm::vec3{1, 0, 0} : glm::vec3{0, 1, 0};
        const glm::vec3 u = glm::cross(a, n);
        const glm::vec3 v = glm::cross(n, u);
        const auto base = static_cast<std::uint32_t>(part.mesh.positions.size());
        const glm::vec2 corners[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (const glm::vec2& c : corners) {
            part.mesh.positions.push_back(n + u * (c.x * 2.0f - 1.0f) + v * (c.y * 2.0f - 1.0f));
            part.mesh.normals.push_back(n);
            part.mesh.uvs.push_back(c);
        }
        part.mesh.indices.insert(part.mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    part.material = 0;
    model.parts.push_back(part);
    lkz::asset::MaterialData material;
    material.name = "cores";
    material.image = 0;
    model.materials.push_back(material);
    lkz::asset::ImageData image;
    image.width = 4;
    image.height = 4;
    for (int i = 0; i < 16; ++i) {
        image.rgba.insert(image.rgba.end(), {static_cast<std::uint8_t>(i * 16), static_cast<std::uint8_t>(255 - i * 16), 128, 255});
    }
    model.images.push_back(image);

    const auto before = lkz::asset::measureUvOverlap(model, 0, 64);
    check(before.ratio() > 0.95f, "UV do cubo: todo sobreposto");

    lkz::asset::UnwrapOptions options;
    options.texelsPerMeter = 16.0f;
    std::string report;
    const auto baked = lkz::asset::bakeUnique(model, options, &report);
    check(baked.has_value(), "bakeUnique do cubo");
    if (!baked) {
        return;
    }
    std::printf("cubo: %s\n", report.c_str());
    check(baked->images.size() == 1 && baked->materials.size() == 1, "uma imagem, um material");
    check(baked->triangleCount() == 12, "os 12 triângulos");
    check(report.rfind("6 cartas", 0) == 0, "6 cartas (uma por face)");
    const auto after = lkz::asset::measureUvOverlap(*baked, 0, 256);
    check(after.covered > 0 && after.ratio() < 0.01f, "UV novo sem sobreposição");
    // Cor no meio de cada face: a do texel do meio da imagem original (bilinear entre 4).
    const lkz::asset::ImageData& atlas = baked->images[0];
    const lkz::MeshData& mesh = baked->parts[0].mesh;
    for (std::size_t t = 0; t < 12; t += 2) {
        const auto i = lkz::asset::triangleAt(mesh, t);
        const auto j = lkz::asset::triangleAt(mesh, t + 1);
        // Centro da face: média dos 4 cantos (0, 1, 2 no primeiro triângulo; 3 no segundo).
        glm::vec2 uv{0.0f};
        for (const std::uint32_t k : {i[0], i[1], i[2], j[2]}) {
            uv += mesh.uvs[k] * 0.25f;
        }
        const int x = static_cast<int>(uv.x * static_cast<float>(atlas.width));
        const int y = static_cast<int>(uv.y * static_cast<float>(atlas.height));
        const std::uint8_t* c = &atlas.rgba[(static_cast<std::size_t>(y) * atlas.width + x) * 4];
        // O meio da imagem 4 x 4 é a média dos texels 5, 6, 9 e 10: (120, 135, 128).
        check(std::abs(c[0] - 120) < 12 && std::abs(c[1] - 135) < 12 && std::abs(c[2] - 128) < 4, "cor no meio da face");
    }
}

/// Destinos dos materiais (extras.lkz), imagem externa por caminho relativo e imagem
/// embutida sem perda.
void targets() {
    namespace fs = std::filesystem;
    const fs::path folder = fs::temp_directory_path() / "lkz_asset_targets";
    fs::remove_all(folder);
    fs::create_directories(folder / "textures");
    fs::create_directories(folder / "models");
    check(lkz::asset::saveImage(folder / "textures" / "tijolo.png", checker(8, false)), "grava a imagem da paleta");

    lkz::asset::ModelData model = sample();
    model.materials[0].target = lkz::asset::SurfaceTarget::Shared;
    lkz::asset::ImageData external;
    external.name = "tijolo";
    external.uri = "../textures/tijolo.png";
    model.images[0] = external;
    model.materials[1].target = lkz::asset::SurfaceTarget::Color;
    model.images[1] = checker(16, false);
    model.images[1].lossless = true;
    const fs::path path = folder / "models" / "alvo.glb";
    check(lkz::asset::saveGlb(path, model), "grava o .glb com destinos");

    const fs::path expected = fs::absolute(folder / "textures" / "tijolo.png").lexically_normal();
    const auto loaded = lkz::asset::loadGltf(path);
    check(loaded.has_value(), "relê o .glb com destinos");
    if (loaded) {
        const lkz::asset::MaterialData& shared = loaded->materials[0];
        check(shared.target == lkz::asset::SurfaceTarget::Shared && shared.shared == "tijolo-vermelho" &&
                  near(shared.metersPerRepeat, 2.5f),
              "destino Compartilhado, nome e escala voltam");
        check(shared.image >= 0 && loaded->images[static_cast<std::size_t>(shared.image)].file == expected &&
                  loaded->images[static_cast<std::size_t>(shared.image)].width == 8,
              "imagem externa aberta pelo caminho relativo");
        check(loaded->materials[1].target == lkz::asset::SurfaceTarget::Color, "destino Só cor volta");
        const auto& swatch = loaded->images[static_cast<std::size_t>(loaded->materials[1].image)];
        check(swatch.rgba == checker(16, false).rgba, "imagem sem perda (PNG) mesmo opaca");
        check(loaded->estimatedTextureMemory() == 16 * 16 * 4 * 4 / 3, "memória sem a imagem externa");
    }
    const auto files = lkz::asset::materialImageFiles(path);
    check(files.size() == 2 && files[0] == expected && files[1].empty(), "materialImageFiles: só a externa");
    lkz::asset::GltfLoadOptions options;
    options.externalImagePixels = false;
    const auto light = lkz::asset::loadGltf(path, options);
    check(light && light->images[static_cast<std::size_t>(light->materials[0].image)].empty() &&
              light->images[static_cast<std::size_t>(light->materials[0].image)].file == expected,
          "externalImagePixels = false: só o caminho");

    model.materials[1].target = lkz::asset::SurfaceTarget::Decal;
    model.materials[1].shared = "janela";
    const fs::path decalPath = folder / "models" / "decalque.glb";
    check(lkz::asset::saveGlb(decalPath, model), "grava o .glb com decalque");
    const auto decal = lkz::asset::loadGltf(decalPath);
    check(decal && decal->materials[1].target == lkz::asset::SurfaceTarget::Decal && decal->materials[1].shared == "janela",
          "destino Decal e nome do decalque voltam");
    fs::remove_all(folder);
}

} // namespace

int main() {
    roundTrip();
    images();
    uniqueUv();
    targets();
    if (failures == 0) {
        std::printf("asset_test: tudo certo\n");
    }
    return failures == 0 ? 0 : 1;
}
