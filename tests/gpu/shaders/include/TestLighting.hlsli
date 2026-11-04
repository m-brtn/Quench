#ifndef QUENCH_TEST_LIGHTING_HLSLI
#define QUENCH_TEST_LIGHTING_HLSLI

float lambert(float3 n, float3 l) {
    return saturate(dot(normalize(n), normalize(l)));
}

#endif
