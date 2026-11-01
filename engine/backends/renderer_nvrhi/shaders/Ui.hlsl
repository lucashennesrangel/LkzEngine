// Interface (RmlUi): posição em pixels, cor com alfa pré-multiplicado e coordenada de
// textura; a matriz já leva os pixels para a tela (projeção * transformação da RmlUi).

struct Draw {
    float4x4 transform;
    float4 translation; // xy, em pixels
};
[[vk::push_constant]] ConstantBuffer<Draw> draw : register(b0);

Texture2D image : register(t0);
SamplerState imageSampler : register(s0);

struct VertexIn {
    float2 position : POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
};

struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
};

VertexOut mainVS(VertexIn input) {
    VertexOut output;
    output.position = mul(draw.transform, float4(input.position + draw.translation.xy, 0.0, 1.0));
    output.color = input.color;
    output.uv = input.uv;
    return output;
}

float4 mainPS(VertexOut input) : SV_Target {
    return image.Sample(imageSampler, input.uv) * input.color;
}
