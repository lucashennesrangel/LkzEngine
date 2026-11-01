"""Gera assets/house.glb, o modelo de teste do sample window.

Uso:
    python samples/window/make_house_glb.py

Uma casinha em glTF 2.0 binário: paredes (caixa 4 x 3 x 4 m) e telhado (pirâmide de
2 m), duas malhas com um material de cor cada (sem imagem). A base fica em y = 0 e o
centro em x = z = 0, como se espera de um modelo para colocar no chão. Só a
biblioteca padrão do Python.
"""

import json
import struct
from pathlib import Path

OUTPUT = Path(__file__).resolve().parent / "assets" / "house.glb"


def normal(a, b, c):
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
    length = sum(x * x for x in n) ** 0.5
    return [x / length for x in n]


def flat(triangles):
    """Triângulos anti-horários (vistos de fora) -> posições, normais e índices."""
    positions, normals, indices = [], [], []
    for triangle in triangles:
        n = normal(*triangle)
        for corner in triangle:
            indices.append(len(positions))
            positions.append(corner)
            normals.append(n)
    return positions, normals, indices


def walls():
    w, h = 2.0, 3.0
    corners = [(-w, -w), (w, -w), (w, w), (-w, w)]  # (x, z) em volta
    triangles = []
    for i in range(4):
        x0, z0 = corners[i]
        x1, z1 = corners[(i + 1) % 4]
        a, b = [x0, 0.0, z0], [x1, 0.0, z1]
        c, d = [x1, h, z1], [x0, h, z0]
        triangles += [[a, c, b], [a, d, c]]  # face virada para fora
    return flat(triangles)


def roof():
    w, base, top = 2.3, 3.0, 5.0
    corners = [(-w, -w), (w, -w), (w, w), (-w, w)]
    apex = [0.0, top, 0.0]
    triangles = []
    for i in range(4):
        x0, z0 = corners[i]
        x1, z1 = corners[(i + 1) % 4]
        triangles.append([[x0, base, z0], apex, [x1, base, z1]])
    a, b, c, d = ([x, base, z] for x, z in corners)
    triangles += [[a, b, c], [a, c, d]]  # beiral, visto de baixo
    return flat(triangles)


def main() -> None:
    meshes = [("Paredes", walls(), [0.92, 0.86, 0.72, 1.0]), ("Telhado", roof(), [0.72, 0.22, 0.16, 1.0])]
    binary = bytearray()
    gltf = {"asset": {"version": "2.0", "generator": "Lkz Engine samples/window"},
            "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [], "materials": [],
            "accessors": [], "bufferViews": []}

    def view(data: bytes, target: int) -> int:
        while len(binary) % 4:
            binary.append(0)
        gltf["bufferViews"].append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data), "target": target})
        binary.extend(data)
        return len(gltf["bufferViews"]) - 1

    def accessor(values, kind, component, pack):
        data = b"".join(struct.pack(pack, *v) if isinstance(v, list) else struct.pack(pack, v) for v in values)
        entry = {"bufferView": view(data, 34963 if kind == "SCALAR" else 34962), "componentType": component,
                 "count": len(values), "type": kind}
        if kind == "VEC3":
            entry["min"] = [min(v[i] for v in values) for i in range(3)]
            entry["max"] = [max(v[i] for v in values) for i in range(3)]
        gltf["accessors"].append(entry)
        return len(gltf["accessors"]) - 1

    for name, (positions, normals, indices), color in meshes:
        gltf["materials"].append({"name": name, "pbrMetallicRoughness": {
            "baseColorFactor": color, "metallicFactor": 0.0, "roughnessFactor": 0.9}})
        primitive = {"attributes": {"POSITION": accessor(positions, "VEC3", 5126, "<3f"),
                                    "NORMAL": accessor(normals, "VEC3", 5126, "<3f")},
                     "indices": accessor(indices, "SCALAR", 5123, "<H"),
                     "material": len(gltf["materials"]) - 1}
        gltf["meshes"].append({"name": name, "primitives": [primitive]})
        gltf["nodes"].append({"name": name, "mesh": len(gltf["meshes"]) - 1})
        gltf["scenes"][0]["nodes"].append(len(gltf["nodes"]) - 1)

    while len(binary) % 4:
        binary.append(0)
    gltf["buffers"] = [{"byteLength": len(binary)}]
    text = json.dumps(gltf, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    chunks = struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(binary), 0x004E4942) + binary
    OUTPUT.write_bytes(struct.pack("<III", 0x46546C67, 2, 12 + len(chunks)) + chunks)
    print(f"{OUTPUT.name}: {len(OUTPUT.read_bytes())} bytes")


if __name__ == "__main__":
    main()
