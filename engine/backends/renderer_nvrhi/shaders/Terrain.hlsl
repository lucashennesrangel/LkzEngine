// Terreno no estilo CDLOD (Strugar, 2009), igual ao do backend raylib: a mesma grade em
// [0,1]² é instanciada em cada pedaço escolhido por lkz::TerrainLod; a altura vem da
// textura do heightmap inteiro (bilinear, lida à mão no vertex shader) e cada vértice se
// transforma no da grade do nível seguinte perto do fim da faixa (morph). Mesma luz das
// malhas: um terreno plano sai igual a um plano com a imagem.

struct TerrainConstants {
    float4x4 viewProjection;
    float4 cameraPosition; // xyz
    float4 area;           // origem x, origem z, largura, profundidade
    float4 grid;           // colunas, linhas, espaçamento, cores de LOD (0/1)
    float4 color;          // multiplica a imagem
    float4 lightDirection; // xyz: para onde a luz aponta
};
ConstantBuffer<TerrainConstants> terrain : register(b0);

Texture2D albedo : register(t0);
Texture2D<float> heights : register(t1);
SamplerState albedoSampler : register(s0);

struct VertexIn {
    float3 position : POSITION;    // grade em [0,1]² (XZ)
    float4 nodeArea : NODEAREA;    // origem x, origem z, lado, resolução
    float4 nodeMorph : NODEMORPH;  // início do morph, fim do morph, nível
};

struct VertexOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    nointerpolation float level : LEVEL;
};

float sampleAt(int2 c) {
    c = clamp(c, int2(0, 0), int2(terrain.grid.xy) - 1);
    return heights.Load(int3(c, 0));
}

float heightAt(float2 p) {
    float2 g = clamp((p - terrain.area.xy) / terrain.grid.z, float2(0.0, 0.0), terrain.grid.xy - 1.0);
    int2 c = int2(min(floor(g), terrain.grid.xy - 2.0));
    float2 f = g - float2(c);
    float top = lerp(sampleAt(c), sampleAt(c + int2(1, 0)), f.x);
    float bottom = lerp(sampleAt(c + int2(0, 1)), sampleAt(c + int2(1, 1)), f.x);
    return lerp(top, bottom, f.y);
}

VertexOut mainVS(VertexIn input) {
    float resolution = input.nodeArea.w;
    float2 grid = floor(input.position.xz * resolution + 0.5);
    float quad = input.nodeArea.z / resolution;
    float2 p = input.nodeArea.xy + grid * quad;
    float distanceToCamera = distance(terrain.cameraPosition.xyz, float3(p.x, heightAt(p), p.y));
    float k = saturate((distanceToCamera - input.nodeMorph.x) / max(input.nodeMorph.y - input.nodeMorph.x, 1e-3));
    p -= frac(grid * 0.5) * 2.0 * quad * k;
    p = clamp(p, terrain.area.xy, terrain.area.xy + terrain.area.zw);

    float h = heightAt(p);
    float e = max(quad, terrain.grid.z);
    VertexOut output;
    output.normal = normalize(float3(heightAt(p - float2(e, 0.0)) - heightAt(p + float2(e, 0.0)), 2.0 * e,
                                     heightAt(p - float2(0.0, e)) - heightAt(p + float2(0.0, e))));
    output.uv = (p - terrain.area.xy) / terrain.area.zw;
    output.level = input.nodeMorph.z;
    output.position = mul(terrain.viewProjection, float4(p.x, h, p.y, 1.0));
    return output;
}

float3 levelTint(float level) {
    int l = int(level + 0.5);
    if (l == 0) return float3(1.0, 1.0, 1.0);
    if (l == 1) return float3(0.4, 0.9, 3.0);
    if (l == 2) return float3(3.0, 2.4, 0.3);
    if (l == 3) return float3(3.0, 0.8, 0.3);
    return float3(2.5, 0.4, 2.5);
}

float4 mainPS(VertexOut input) : SV_Target {
    float4 base = albedo.Sample(albedoSampler, input.uv) * terrain.color;
    float diffuse = max(dot(normalize(input.normal), -terrain.lightDirection.xyz), 0.0);
    float3 rgb = base.rgb * (0.35 + 0.65 * diffuse);
    if (terrain.grid.w > 0.5) {
        rgb *= levelTint(input.level);
    }
    return float4(rgb, base.a);
}
