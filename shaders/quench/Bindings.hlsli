#ifndef QUENCH_BINDINGS_HLSLI
#define QUENCH_BINDINGS_HLSLI

// Resource registers in the layout SDL GPU expects (SDL_CreateGPUShader):
//   vertex stage:   sampled textures t[n] + samplers s[n] in space0, read-only storage buffers t[samplers + n] in
//                   space0, uniform slot n -> b[n] in space1;
//   fragment stage: the same in space2, uniforms in space3;
//   compute stage:  read-only resources in space0, read-write storage buffers u[n] in space1 (QUENCH_RW_STORAGE),
//                   uniforms in space2.
// Vertex attributes use TEXCOORD<location>; varyings are matched by TEXCOORD index.

#if defined(QUENCH_STAGE_VERTEX)
#define QUENCH_RESOURCE_SPACE space0
#define QUENCH_UNIFORM_SPACE space1
#elif defined(QUENCH_STAGE_COMPUTE)
#define QUENCH_RESOURCE_SPACE space0
#define QUENCH_UNIFORM_SPACE space2
#else
#define QUENCH_RESOURCE_SPACE space2
#define QUENCH_UNIFORM_SPACE space3
#endif

#define QUENCH_TEXTURE(n) register(t##n, QUENCH_RESOURCE_SPACE)
#define QUENCH_SAMPLER(n) register(s##n, QUENCH_RESOURCE_SPACE)
#define QUENCH_STORAGE(n) register(t##n, QUENCH_RESOURCE_SPACE)
#define QUENCH_RW_STORAGE(n) register(u##n, space1)
#define QUENCH_UNIFORM(n) register(b##n, QUENCH_UNIFORM_SPACE)

#endif
