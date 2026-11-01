// Testes do relevo: Heightmap (altura, normal, raio, arquivo), TerrainLod (seleção CDLOD)
// e drapeMesh (malha assentada no relevo).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <lkz/Terrain/Drape.hpp>
#include <lkz/Terrain/Heightmap.hpp>
#include <lkz/Terrain/TerrainLod.hpp>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FALHOU: %s\n", what);
        ++failures;
    }
}

bool near(float a, float b, float tolerance = 1e-3f) {
    return std::abs(a - b) <= tolerance;
}

void heightmapBasics() {
    lkz::Heightmap map{3, 3, 10.0f, {0.0f, 0.0f}};
    map.set(1, 1, 8.0f);
    check(near(map.heightAt({10.0f, 10.0f}), 8.0f), "altura na amostra");
    check(near(map.heightAt({5.0f, 10.0f}), 4.0f), "bilinear no meio da aresta");
    check(near(map.heightAt({-50.0f, -50.0f}), 0.0f), "fora da grade usa a borda");
    check(map.normalAt({30.0f, 30.0f}).y > 0.99f, "normal num canto plano");

    const lkz::Heightmap flat = lkz::Heightmap::covering({1000.0f, 400.0f}, 5.0f);
    check(flat.columns() == 201 && flat.rows() == 81, "covering: número de amostras");
    check(near(flat.origin().x, -500.0f) && near(flat.origin().y, -200.0f), "covering: centrado na origem");
}

void raycast() {
    lkz::Heightmap map = lkz::Heightmap::covering({100.0f, 100.0f}, 1.0f);
    for (int r = 0; r < map.rows(); ++r) {
        for (int c = 0; c < map.columns(); ++c) {
            map.set(c, r, static_cast<float>(c) * 0.1f); // rampa ao longo de X
        }
    }
    const auto hit = map.raycast({0.0f, 50.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 200.0f);
    check(hit.has_value(), "raio para baixo acerta");
    if (hit) {
        check(near(hit->y, map.heightAt({0.0f, 0.0f}), 0.01f), "raio acerta na altura da superfície");
    }
    const auto miss = map.raycast({0.0f, 50.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 200.0f);
    check(!miss.has_value(), "raio para cima não acerta");
    const auto slanted = map.raycast({-80.0f, 30.0f, 10.0f}, glm::normalize(glm::vec3{1.0f, -0.4f, 0.0f}), 500.0f);
    check(slanted.has_value() && near(slanted->y, map.heightAt({slanted->x, slanted->z}), 0.01f), "raio inclinado");
}

void fileRoundTrip() {
    lkz::Heightmap map{17, 9, 2.0f, {-16.0f, -8.0f}};
    for (int r = 0; r < map.rows(); ++r) {
        for (int c = 0; c < map.columns(); ++c) {
            map.set(c, r, std::sin(static_cast<float>(c)) * 10.0f + static_cast<float>(r));
        }
    }
    const auto path = std::filesystem::temp_directory_path() / "lkz_terrain_test.lkzh";
    check(map.save(path), "grava");
    const auto loaded = lkz::Heightmap::load(path);
    check(loaded.has_value(), "lê");
    if (loaded) {
        const auto [low, high] = map.range();
        const float tolerance = (high - low) / 65535.0f + 1e-4f;
        bool same = loaded->columns() == map.columns() && loaded->rows() == map.rows();
        for (std::size_t i = 0; same && i < map.samples().size(); ++i) {
            same = near(loaded->samples()[i], map.samples()[i], tolerance);
        }
        check(same, "ida e volta pelo arquivo (16 bits)");
    }
    std::filesystem::remove(path);
}

bool overlap(const lkz::TerrainNode& a, const lkz::TerrainNode& b) {
    const float epsilon = 1e-3f;
    return a.origin.x + a.size > b.origin.x + epsilon && b.origin.x + b.size > a.origin.x + epsilon &&
           a.origin.y + a.size > b.origin.y + epsilon && b.origin.y + b.size > a.origin.y + epsilon;
}

void lodCoverage() {
    // Mapa não quadrado, como o da cidade.
    const lkz::Heightmap map = lkz::Heightmap::covering({18300.0f, 7800.0f}, 5.0f);
    const lkz::TerrainLod lod{map};
    check(lod.levels() >= 7, "níveis suficientes para o mapa");

    struct View {
        glm::vec3 eye;
        bool seesWholeMap;
    };
    for (const View view : {View{{0.0f, 30000.0f, 1.0f}, true}, View{{200.0f, 120.0f, -300.0f}, false}}) {
        const glm::mat4 lookDown =
            glm::lookAt(view.eye, glm::vec3{view.eye.x, 0.0f, view.eye.z + 0.01f}, glm::vec3{0.0f, 0.0f, -1.0f});
        const glm::mat4 projection = glm::perspective(glm::radians(150.0f), 1.0f, 1.0f, 100000.0f);
        std::vector<lkz::TerrainNode> nodes;
        lod.select(view.eye, projection * lookDown, nodes);
        check(!nodes.empty(), "seleção não vazia");

        double area = 0.0;
        bool valid = true;
        bool overlaps = false;
        int levelUnderCamera = -1;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto& node = nodes[i];
            const glm::vec2 far = glm::min(node.origin + node.size, map.origin() + map.size());
            area += static_cast<double>(far.x - node.origin.x) * static_cast<double>(far.y - node.origin.y);
            valid = valid && node.morphStart < node.morphEnd && node.resolution > 0;
            for (std::size_t j = i + 1; j < nodes.size(); ++j) {
                overlaps = overlaps || overlap(node, nodes[j]);
            }
            if (view.eye.x >= node.origin.x && view.eye.x < node.origin.x + node.size && view.eye.z >= node.origin.y &&
                view.eye.z < node.origin.y + node.size) {
                levelUnderCamera = node.level;
            }
        }
        check(!overlaps, "pedaços não se sobrepõem");
        check(valid, "faixas de morph e resolução válidas");
        if (view.seesWholeMap) {
            const double expected = static_cast<double>(map.size().x) * static_cast<double>(map.size().y);
            check(std::abs(area - expected) < expected * 1e-6, "vista de cima: pedaços cobrem o mapa inteiro");
        } else {
            check(levelUnderCamera == 0, "perto do chão: nível mais fino sob a câmera");
        }
    }
}

// Quadrado de `size` no plano, a `y` acima do chão, frente para cima.
lkz::MeshData square(glm::vec2 corner, float size, float y) {
    lkz::MeshData mesh;
    mesh.positions = {{corner.x, y, corner.y},
                      {corner.x, y, corner.y + size},
                      {corner.x + size, y, corner.y + size},
                      {corner.x + size, y, corner.y}};
    mesh.normals.assign(4, glm::vec3{0.0f, 1.0f, 0.0f});
    mesh.uvs = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

void drape() {
    // Rampa em X (10%) com uma lombada em (80, 50), na aresta comum dos dois quadrados abaixo.
    lkz::Heightmap map{41, 41, 5.0f, {0.0f, 0.0f}};
    for (int r = 0; r < map.rows(); ++r) {
        for (int c = 0; c < map.columns(); ++c) {
            map.set(c, r, static_cast<float>(c) * 0.5f + (c == 16 && r == 10 ? 3.0f : 0.0f));
        }
    }

    lkz::MeshData plain = square({20.0f, 20.0f}, 60.0f, 0.3f);
    lkz::subdivideMesh(plain, 5.0f);
    bool shortEdges = true;
    for (std::size_t t = 0; t + 2 < plain.indices.size(); t += 3) {
        for (int e = 0; e < 3; ++e) {
            const glm::vec3 a = plain.positions[plain.indices[t + static_cast<std::size_t>(e)]];
            const glm::vec3 b = plain.positions[plain.indices[t + static_cast<std::size_t>((e + 1) % 3)]];
            shortEdges = shortEdges && glm::distance(a, b) <= 5.0f + 1e-4f;
        }
    }
    check(shortEdges, "subdivide: nenhuma aresta passa do limite");
    double area = 0.0;
    bool up = true;
    for (std::size_t t = 0; t + 2 < plain.indices.size(); t += 3) {
        const glm::vec3 a = plain.positions[plain.indices[t]];
        const glm::vec3 n = glm::cross(plain.positions[plain.indices[t + 1]] - a, plain.positions[plain.indices[t + 2]] - a);
        area += 0.5 * glm::length(n);
        up = up && n.y > 0.0f;
    }
    check(std::abs(area - 3600.0) < 0.01, "subdivide: mesma área");
    check(up, "subdivide: frente continua para cima");

    lkz::MeshData left = square({20.0f, 20.0f}, 60.0f, 0.3f);
    lkz::MeshData right = square({80.0f, 20.0f}, 60.0f, 0.3f);
    lkz::drapeMesh(left, map);
    lkz::drapeMesh(right, map);
    bool onGround = true;
    bool normalsUp = true;
    for (std::size_t i = 0; i < left.positions.size(); ++i) {
        const glm::vec3 p = left.positions[i];
        onGround = onGround && near(p.y, map.heightAt({p.x, p.z}) + 0.3f);
        normalsUp = normalsUp && left.normals[i].y > 0.5f;
    }
    check(onGround, "drape: cada vértice na altura do relevo");
    check(normalsUp, "drape: normais para cima na rampa");

    // Na aresta comum (x = 80) as duas malhas seguem a lombada e não se afastam mais
    // que a tolerância uma da outra (fresta máxima).
    const auto edgeOf = [](const lkz::MeshData& mesh) {
        std::vector<glm::vec3> points;
        for (const glm::vec3& p : mesh.positions) {
            if (p.x == 80.0f) {
                points.push_back(p);
            }
        }
        std::sort(points.begin(), points.end(), [](const glm::vec3& a, const glm::vec3& b) { return a.z < b.z; });
        return points;
    };
    const auto heightOn = [](const std::vector<glm::vec3>& edge, float z) {
        for (std::size_t i = 0; i + 1 < edge.size(); ++i) {
            if (z >= edge[i].z && z <= edge[i + 1].z) {
                const float t = (z - edge[i].z) / std::max(edge[i + 1].z - edge[i].z, 1e-6f);
                return edge[i].y + (edge[i + 1].y - edge[i].y) * t;
            }
        }
        return edge.empty() ? 0.0f : edge.back().y;
    };
    const std::vector<glm::vec3> edgeLeft = edgeOf(left);
    const std::vector<glm::vec3> edgeRight = edgeOf(right);
    bool tight = edgeLeft.size() > 2 && edgeRight.size() > 2;
    for (const glm::vec3& p : edgeLeft) {
        tight = tight && std::abs(heightOn(edgeRight, p.z) - p.y) <= 0.0101f;
    }
    for (const glm::vec3& p : edgeRight) {
        tight = tight && std::abs(heightOn(edgeLeft, p.z) - p.y) <= 0.0101f;
    }
    check(tight, "drape: malhas vizinhas seguem juntas a lombada na aresta comum");

    // Na rampa reta, longe da lombada, nada precisa ser dividido.
    lkz::MeshData ramp = square({100.0f, 120.0f}, 60.0f, 0.3f);
    lkz::drapeMesh(ramp, map);
    check(ramp.indices.size() == 6, "drape: rampa reta continua com 2 triângulos");
    check(near(ramp.positions[2].y, map.heightAt({160.0f, 180.0f}) + 0.3f), "drape: rampa na altura certa");
}

} // namespace

int main() {
    heightmapBasics();
    raycast();
    fileRoundTrip();
    lodCoverage();
    drape();
    if (failures == 0) {
        std::printf("terrain_test: tudo certo\n");
    }
    return failures == 0 ? 0 : 1;
}
