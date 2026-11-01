// Malhas com luz, com o mesmo visual do backend raylib: matriz de modelo, cor e
// transformação das coordenadas de textura chegam por instância; a imagem da mesh
// (branca quando não há) é lida em grade comum ou em hex-tiling e multiplicada pela cor
// da instância e pela do material; luz direcional + ambiente.
//
// Hex-tiling: Mikkelsen, "Practical Real-Time Hex-Tiling", JCGT 11(3), 2022 (versão para
// cor do demo oficial, MIT: github.com/mmikk/hextile-demo), com o hash inteiro pcg2d
// (Jarzynski e Olano, JCGT 2020). Detalhes no fragment shader do backend raylib.

struct Frame {
    float4x4 viewProjection;
    float4 lightDirection; // xyz: para onde a luz aponta
};
ConstantBuffer<Frame> frame : register(b0);

struct Material {
    float4 color;
    float4 tiling; // x: 0 = grade comum, 1 = hex-tiling; y: rotação; z: contraste
};
[[vk::push_constant]] ConstantBuffer<Material> material : register(b1);

Texture2D image : register(t0);
SamplerState imageSampler : register(s0);

struct VertexIn {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float4 model0 : MODEL0; // colunas da matriz de modelo
    float4 model1 : MODEL1;
    float4 model2 : MODEL2;
    float4 model3 : MODEL3;
    float4 color : COLOR;
    float4 uvTransform : UVTRANSFORM; // xy: escala, zw: deslocamento
};

struct VertexOut {
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

VertexOut mainVS(VertexIn input) {
    float4 world = input.model0 * input.position.x + input.model1 * input.position.y +
                   input.model2 * input.position.z + input.model3;
    VertexOut output;
    output.position = mul(frame.viewProjection, float4(world.xyz, 1.0));
    output.normal = input.model0.xyz * input.normal.x + input.model1.xyz * input.normal.y +
                    input.model2.xyz * input.normal.z;
    output.uv = input.uv * input.uvTransform.xy + input.uvTransform.zw;
    output.color = input.color;
    return output;
}

static const float pi = 3.14159265;
static const float hexScale = 3.46410162; // 2 * sqrt(3): hexágono ~0,29 repetição da imagem

float2 hash2(int2 p) {
    uint2 v = uint2(p) * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return float2(v) * (1.0 / 4294967296.0);
}

// Curva de ganho: r > 0.5 aproxima os pesos de 0 ou 1 (transição mais nítida).
float3 gain(float3 x, float r) {
    float k = log(1.0 - r) / log(0.5);
    float3 s = 2.0 * step(0.5, x);
    float3 m = 2.0 * (1.0 - s);
    float3 result = 0.5 * s + 0.25 * m * pow(max(float3(0.0, 0.0, 0.0), s + x * m), float3(k, k, k));
    return result / (result.x + result.y + result.z);
}

// A grade e as rotações ficam num espaço com a proporção da imagem (unidade = altura
// dela): numa imagem não quadrada, girar direto em UV esticaria o desenho.
float4 hexTiled(float2 uvIn, float2 uvDx, float2 uvDy) {
    float width;
    float height;
    image.GetDimensions(width, height);
    float2 aspect = float2(width / height, 1.0);
    float2 st = uvIn * aspect;
    float2 dx = uvDx * aspect;
    float2 dy = uvDy * aspect;
    float2 grid = st * hexScale;
    float2 skewed = float2(grid.x - 0.57735027 * grid.y, 1.15470054 * grid.y);
    int2 base = int2(floor(skewed));
    float3 t = float3(frac(skewed), 0.0);
    t.z = 1.0 - t.x - t.y;
    float s = step(0.0, -t.z);
    float s2 = 2.0 * s - 1.0;
    float3 barycentric = max(float3(-t.z * s2, s - t.y * s2, s - t.x * s2), float3(0.0, 0.0, 0.0));
    int si = int(s);
    int2 corners[3] = { base + int2(si, si), base + int2(si, 1 - si), base + int2(1 - si, si) };

    float4 samples[3];
    float3 luminance;
    [unroll]
    for (int i = 0; i < 3; ++i) {
        float2 corner = float2(corners[i]);
        float2 center = float2(corner.x + 0.5 * corner.y, corner.y * 0.8660254) / hexScale;
        float angle = (hash2(corners[i] + int2(7919, 104729)).x * 2.0 - 1.0) * pi * material.tiling.y;
        float c = cos(angle);
        float n = sin(angle);
        // As mesmas contas do mat2(c, -n, n, c) * v do GLSL (matriz por colunas).
        float2x2 rotation = float2x2(c, n, -n, c);
        float2 uv = (mul(rotation, st - center) + center + hash2(corners[i])) / aspect;
        samples[i] = image.SampleGrad(imageSampler, uv, mul(rotation, dx) / aspect, mul(rotation, dy) / aspect);
        luminance[i] = dot(samples[i].rgb, float3(0.299, 0.587, 0.114));
    }
    float3 weight = lerp(float3(1.0, 1.0, 1.0), luminance, 0.6) * pow(barycentric, float3(7.0, 7.0, 7.0));
    weight /= weight.x + weight.y + weight.z;
    if (material.tiling.z != 0.5) {
        weight = gain(weight, material.tiling.z);
    }
    return weight.x * samples[0] + weight.y * samples[1] + weight.z * samples[2];
}

float4 mainPS(VertexOut input, bool frontFace : SV_IsFrontFace) : SV_Target {
    float2 dx = ddx(input.uv);
    float2 dy = ddy(input.uv);
    float4 texel = material.tiling.x > 0.5 ? hexTiled(input.uv, dx, dy) : image.Sample(imageSampler, input.uv);
    float4 base = texel * input.color * material.color;
    float3 normal = normalize(input.normal) * (frontFace ? 1.0 : -1.0);
    float diffuse = max(dot(normal, -frame.lightDirection.xyz), 0.0);
    return float4(base.rgb * (0.35 + 0.65 * diffuse), base.a);
}
