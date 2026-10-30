// GPU clock probe (docs/03 §6, R3): a fixed ALU-bound kernel, a dependent integer multiply-add
// chain per thread, 4 thread groups x 64 threads, iteration count fixed per build. Dispatched in
// its own command buffer at frame start every frame, in every mode. The result is written and
// checked on the CPU (lab/src/loads.h, spin_reference), so the compiler cannot remove the loop.
// Integer only: the same work on every GPU (R1).

struct SpinParams
{
    uint iterations;   // dependent steps per thread
    uint seed;
    uint pad0;
    uint pad1;
};

[[vk::binding(0)]] RWStructuredBuffer<uint> probeOut;
[[vk::push_constant]] ConstantBuffer<SpinParams> P;

[shader("compute")]
[numthreads(64, 1, 1)]
void ag_probe(uint3 tid : SV_DispatchThreadID)
{
    uint x = tid.x * 2654435761u + P.seed;
    uint y = x ^ 0x9E3779B9u;
    for (uint i = 0u; i < P.iterations; ++i)
    {
        x = x * 1664525u + 1013904223u;
        y = y * 22695477u + x;
    }
    probeOut[tid.x] = x ^ y;
}
