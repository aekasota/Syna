// Video presentation shader. Every texture is bound as a single-slice Texture2DArray so decoder
// output (array slices) and uploaded software frames share one code path.

cbuffer Params : register(b0)
{
    float4 rowR;          // rgb = rowX . (y, cb, cr, 1)
    float4 rowG;
    float4 rowB;
    float2 lumaTexSize;   // texels
    float2 uvScale;       // visible area / texture size
    float2 chromaOffset;  // normalized offset for chroma siting
    uint layout;          // 0 = 3 planes, 1 = luma + interleaved chroma, 2 = packed RGB
    uint transfer;        // 0 = SDR, 1 = PQ, 2 = HLG
    uint bicubic;
    float ditherAmp;
    float peakNits;       // mastering peak for tone mapping
    float pad0;
    float3 gamut0;        // BT.2020 -> BT.709 rows (only used for HDR)
    float pad1;
    float3 gamut1;
    float pad2;
    float3 gamut2;
    float pad3;
};

Texture2DArray<float4> tex0 : register(t0);
Texture2DArray<float4> tex1 : register(t1);
Texture2DArray<float4> tex2 : register(t2);
SamplerState linearClamp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID)
{
    float2 uv = float2(id & 1, id >> 1);  // triangle strip: (0,0) (1,0) (0,1) (1,1)
    VSOut o;
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    o.uv = uv;
    return o;
}

float4 sampleBilinear(Texture2DArray<float4> t, float2 uv)
{
    return t.SampleLevel(linearClamp, float3(uv, 0), 0);
}

// Catmull-Rom with 9 bilinear taps.
float4 sampleCatmullRom(Texture2DArray<float4> t, float2 uv, float2 size)
{
    float2 samplePos = uv * size;
    float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;
    float2 f = samplePos - texPos1;
    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / w12;
    float2 p0 = (texPos1 - 1.0f) / size;
    float2 p3 = (texPos1 + 2.0f) / size;
    float2 p12 = (texPos1 + offset12) / size;
    float4 r = 0;
    r += t.SampleLevel(linearClamp, float3(p0.x, p0.y, 0), 0) * w0.x * w0.y;
    r += t.SampleLevel(linearClamp, float3(p12.x, p0.y, 0), 0) * w12.x * w0.y;
    r += t.SampleLevel(linearClamp, float3(p3.x, p0.y, 0), 0) * w3.x * w0.y;
    r += t.SampleLevel(linearClamp, float3(p0.x, p12.y, 0), 0) * w0.x * w12.y;
    r += t.SampleLevel(linearClamp, float3(p12.x, p12.y, 0), 0) * w12.x * w12.y;
    r += t.SampleLevel(linearClamp, float3(p3.x, p12.y, 0), 0) * w3.x * w12.y;
    r += t.SampleLevel(linearClamp, float3(p0.x, p3.y, 0), 0) * w0.x * w3.y;
    r += t.SampleLevel(linearClamp, float3(p12.x, p3.y, 0), 0) * w12.x * w3.y;
    r += t.SampleLevel(linearClamp, float3(p3.x, p3.y, 0), 0) * w3.x * w3.y;
    return r;
}

float3 pqToLinear(float3 e)  // -> nits / 10000
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f;
    const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float3 p = pow(max(e, 0.0f), 1.0f / m2);
    return pow(max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

float3 hlgToLinear(float3 e)  // scene light, 0..1 (1 = nominal peak)
{
    const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    float3 lo = e * e / 3.0f;
    float3 hi = (exp((e - c) / a) + b) / 12.0f;
    return lerp(lo, hi, step(0.5f, e));
}

float3 toneMapToSdr(float3 linearNits)
{
    // Linear light relative to SDR reference white (203 nits), extended Reinhard on luminance.
    const float refWhite = 203.0f;
    float3 x = linearNits / refWhite;
    float3 bt709 = float3(dot(gamut0, x), dot(gamut1, x), dot(gamut2, x));
    bt709 = max(bt709, 0.0f);
    float l = dot(bt709, float3(0.2126f, 0.7152f, 0.0722f));
    float lw = max(peakNits / refWhite, 1.0f);
    float lm = l * (1.0f + l / (lw * lw)) / (1.0f + l);
    float3 mapped = l > 0.0f ? bt709 * (lm / l) : bt709;
    return pow(saturate(mapped), 1.0f / 2.2f);
}

float ign(float2 pos)
{
    return frac(52.9829189f * frac(dot(pos, float2(0.06711056f, 0.00583715f))));
}

float4 ps_main(VSOut i) : SV_Target
{
    float2 uv = i.uv * uvScale;
    float3 rgb;
    if (layout == 2)
    {
        rgb = (bicubic ? sampleCatmullRom(tex0, uv, lumaTexSize) : sampleBilinear(tex0, uv)).rgb;
    }
    else
    {
        float y = (bicubic ? sampleCatmullRom(tex0, uv, lumaTexSize) : sampleBilinear(tex0, uv)).r;
        float2 cuv = uv + chromaOffset;
        float cb, cr;
        if (layout == 1)
        {
            float2 c = sampleBilinear(tex1, cuv).rg;
            cb = c.x;
            cr = c.y;
        }
        else
        {
            cb = sampleBilinear(tex1, cuv).r;
            cr = sampleBilinear(tex2, cuv).r;
        }
        float4 yuv = float4(y, cb, cr, 1.0f);
        rgb = float3(dot(rowR, yuv), dot(rowG, yuv), dot(rowB, yuv));
    }

    if (transfer == 1)
        rgb = toneMapToSdr(pqToLinear(saturate(rgb)) * 10000.0f);
    else if (transfer == 2)
        rgb = toneMapToSdr(pow(hlgToLinear(saturate(rgb)), 1.2f) * 1000.0f);

    rgb += (ign(i.pos.xy) - 0.5f) * ditherAmp;
    return float4(saturate(rgb), 1.0f);
}
