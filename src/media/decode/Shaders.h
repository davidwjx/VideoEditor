#pragma once

namespace VideoEditor::shaders {

inline constexpr const char *kFullscreenVs = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    float2 p = float2(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0);
    VSOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.uv = float2((p.x + 1.0) * 0.5, 1.0 - (p.y + 1.0) * 0.5);
    return o;
}
)";

inline constexpr const char *kRotateAndSample = R"(
cbuffer Params : register(b0)
{
    float rotation;
    float3 padding;
};

float2 rotateUv(float2 uv)
{
    float2 centered = uv - 0.5;
    float s = sin(rotation);
    float c = cos(rotation);
    return float2(c * centered.x - s * centered.y,
                  s * centered.x + c * centered.y) + 0.5;
}
)";

inline constexpr const char *kNv12Ps = R"(
Texture2D<float>  luma   : register(t0);
Texture2D<float2> chroma : register(t1);
SamplerState      samp   : register(s0);

cbuffer Params : register(b0)
{
    float rotation;
    float3 padding;
};

float2 rotateUv(float2 uv)
{
    float2 centered = uv - 0.5;
    float s = sin(rotation);
    float c = cos(rotation);
    return float2(c * centered.x - s * centered.y,
                  s * centered.x + c * centered.y) + 0.5;
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    float2 rotated = rotateUv(uv);
    float y = (luma.Sample(samp, rotated).r - 16.0 / 255.0) * (255.0 / 219.0);
    float2 c = chroma.Sample(samp, rotated);
    float u = (c.x - 128.0 / 255.0) * (255.0 / 224.0);
    float v = (c.y - 128.0 / 255.0) * (255.0 / 224.0);
    float3 rgb = float3(y + 1.5748 * v,
                        y - 0.1873 * u - 0.4681 * v,
                        y + 1.8556 * u);
    return float4(saturate(rgb), 1.0);
}
)";

inline constexpr const char *kRgbaPs = R"(
Texture2D<float4> src  : register(t0);
SamplerState      samp : register(s0);

cbuffer Params : register(b0)
{
    float rotation;
    float3 padding;
};

float2 rotateUv(float2 uv)
{
    float2 centered = uv - 0.5;
    float s = sin(rotation);
    float c = cos(rotation);
    return float2(c * centered.x - s * centered.y,
                  s * centered.x + c * centered.y) + 0.5;
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    return src.Sample(samp, rotateUv(uv));
}
)";

} // namespace VideoEditor::shaders
