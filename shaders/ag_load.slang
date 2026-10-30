// Synthetic GPU load for stepbench (docs/04 E1) and the heater (docs/03 §7): the probe's
// dependent integer chain on a wide grid, so the work fills the GPU. The iteration count is sized
// in warm milliseconds from the warm-up calibration (lab/src/stepbench.cpp).

struct SpinParams
{
    uint iterations;
    uint seed;
    uint pad0;
    uint pad1;
};

[[vk::binding(0)]] RWStructuredBuffer<uint> loadOut;
[[vk::push_constant]] ConstantBuffer<SpinParams> P;

[shader("compute")]
[numthreads(64, 1, 1)]
void ag_load(uint3 tid : SV_DispatchThreadID)
{
    uint x = tid.x * 2654435761u + P.seed;
    uint y = x ^ 0x9E3779B9u;
    for (uint i = 0u; i < P.iterations; ++i)
    {
        x = x * 1664525u + 1013904223u;
        y = y * 22695477u + x;
    }
    loadOut[tid.x] = x ^ y;
}
