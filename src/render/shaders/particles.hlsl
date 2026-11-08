// The particles and trails (src/render/Particles.cpp, docs/particles.md): the step of every system's slots on the
// GPU, then the particles as camera-facing quads or drawn out along their motion, and the trails as camera-facing
// strips, after TAA and upscaling into the output-size image (they glow through bloom), tested against the scene's
// depth and faded where they near it, fogged per vertex.
//
// Buffers hold float4 elements only, so their layout is the same in SPIR-V, MSL and DXIL:
//   state, spawns: three float4 per particle (QuenchParticle, quench/ParticleField.h)
//   instances:     three float4 per slot (QuenchParticleInstance): what the draw reads
//   trail vertices: two float4 per vertex (particles::TrailVertex: position and half width, color and opacity)
//
// Bindings: particle_step_cs uniform slot 0 ParticleUniforms; the draws uniform slot 0 FrameUniforms
// (quench/Frame.hlsli) and slot 1 ParticleDrawUniforms (ParticleUniforms.h), vertex and fragment; the vertex shaders
// read their buffer from storage slot 0, the fragment shaders the scene's depth from texture 0.
//
// Every particle is premultiplied: additive ones write alpha 0 (light added to what is behind), alpha-blended ones
// their coverage; one blend state (One, OneMinusSrcAlpha) draws both.

#include "quench/Bindings.hlsli"
#include "quench/ParticleField.h"

// ---------------------------------------------------------------- the step

#if defined(QUENCH_ENTRY_particle_step_cs)
StructuredBuffer<float4> spawns : QUENCH_STORAGE(0);
RWStructuredBuffer<float4> state : QUENCH_RW_STORAGE(0);
RWStructuredBuffer<float4> instances : QUENCH_RW_STORAGE(1);
cbuffer StepBlock : QUENCH_UNIFORM(0) {
    ParticleUniforms step;
};

// Slot i of the system: this step's spawn, cleared (a restart) or stepped, then what the draw reads of it. The same
// order as particles::Simulation::step.
[numthreads(QUENCH_PARTICLE_GROUP, 1, 1)]
void particle_step_cs(uint3 id : SV_DispatchThreadID) {
    const uint capacity = uint(step.range.y);
    const uint i = id.x;
    if (i >= capacity) return;
    const uint slot = (uint(step.range.x) + i) * 3u;
    const uint spawn = (i + capacity - uint(step.range.z)) % capacity;
    QuenchParticle q;
    if (spawn < uint(step.range.w)) {
        const uint record = (uint(step.clock.z) + spawn) * 3u;
        q.a = spawns[record];
        q.b = spawns[record + 1u];
        q.c = spawns[record + 2u];
    } else if (step.clock.w > 0.5) {
        q.a = float4(0.0, 0.0, 0.0, 0.0);
        q.b = float4(0.0, 0.0, 0.0, 0.0);
        q.c = float4(0.0, 0.0, 0.0, 0.0);
    } else {
        q.a = state[slot];
        q.b = state[slot + 1u];
        q.c = state[slot + 2u];
        if (q.b.w > 0.0 && q.a.w < q.b.w) q = quenchParticleStep(step, q, step.clock.x, step.clock.y);
    }
    state[slot] = q.a;
    state[slot + 1u] = q.b;
    state[slot + 2u] = q.c;
    const QuenchParticleInstance o = quenchParticleLook(step, q, step.clock.y);
    instances[slot] = o.a;
    instances[slot + 1u] = o.b;
    instances[slot + 2u] = o.c;
}
#endif

// ---------------------------------------------------------------- the draws

#if !defined(QUENCH_ENTRY_particle_step_cs)
#include "quench/Common.hlsli"
#include "quench/Fog.hlsli"
#include "quench/Frame.hlsli"

#include "ParticleUniforms.h"

cbuffer ParticleDrawBlock : QUENCH_UNIFORM(1) {
    ParticleDrawUniforms pd;
};

struct ParticleOut {
    float4 position : SV_Position;
    // Camera-facing: xy in the quad (-1..1). Along the motion: x across (-1..1), y along in radii (0 at the particle),
    // z the motion's length in radii. Trails: x across (-1..1).
    float3 coord : TEXCOORD0;
    float4 color : TEXCOORD1; // premultiplied: rgb the light it sends, a its coverage (0 additive)
    float2 info : TEXCOORD2;  // x: its distance from the camera (m), y: its random 0..1
};

// The two triangles of a quad: x -1..1, y -1..1.
static const float2 kParticleCorners[6] = {float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0),
                                           float2(-1.0, 1.0),  float2(1.0, -1.0), float2(1.0, 1.0)};

ParticleOut particleCulled() {
    ParticleOut o;
    o.position = float4(-2.0, -2.0, -2.0, 1.0); // outside the clip volume: all six corners, no area
    o.coord = float3(0.0, 0.0, 0.0);
    o.color = float4(0.0, 0.0, 0.0, 0.0);
    o.info = float2(0.0, 0.0);
    return o;
}

// Straight color and opacity at P to what the draw writes: lit (alpha-blended, lit), fogged, faded near the camera,
// premultiplied.
float4 particleFinish(float3 color, float opacity, float3 P, float distance) {
    const bool alpha = pd.fade.w > 0.5;
    if (alpha && pd.light.x > 0.5) {
        // A soft puff: the sky's light from around it and the sun's, scattered forward a little (no shadows).
        const float3 toCamera = normalize(pd.camera.xyz - P);
        const float forward = 0.25 + 0.25 * phaseHG(dot(-toCamera, quenchFrame.sunDir.xyz), 0.5);
        const float3 sun = quenchFrame.sunColor.rgb * quenchFrame.sunDir.w * (forward / QUENCH_PI);
        color *= lerp(quenchFrame.groundAmbient.rgb, quenchFrame.skyAmbient.rgb, 0.7) + sun;
    }
    const float4 fog = fogAt(P, pd.camera.xyz);
    color = alpha ? lerp(color, fog.rgb, fog.a) : color * (1.0 - fog.a);
    if (pd.fade.y > 0.0) opacity *= saturate((distance - pd.camera.w) / pd.fade.y);
    return float4(color * opacity, alpha ? opacity : 0.0);
}

#if defined(QUENCH_ENTRY_particle_vs)
StructuredBuffer<float4> instances : QUENCH_STORAGE(0);

ParticleOut particle_vs(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    const uint slot = (uint(pd.draw.x) + iid) * 3u;
    const float4 a = instances[slot];
    const float4 b = instances[slot + 1u];
    const float4 c = instances[slot + 2u];
    if (a.w <= 0.0 || b.a <= 0.0) return particleCulled();
    const float3 P = a.xyz;
    const float4 c0 = mul(pd.viewProj, float4(P, 1.0));
    if (c0.w < pd.camera.w) return particleCulled();

    // Its radius in pixels, at least half the least size: smaller ones are drawn that big, fainter by their area.
    const float2 halfTarget = 0.5 * pd.target.xy;
    const float pixels = a.w * pd.right.w / c0.w;
    const float least = max(pd.fade.z, 0.0) * 0.5;
    const float radius = max(pixels, least);
    const float small = pixels < least ? pixels / least : 1.0;
    float opacity = b.a * small * small;
    float3 color = b.rgb;
    const float2 corner = kParticleCorners[vid];

    ParticleOut o;
    float4 position = c0;
    if (pd.draw.z > 0.5) {
        // A capsule from the particle back along its motion over the exposure, its light spread along it.
        const float4 c1 = mul(pd.viewProj, float4(P - c.xyz * pd.draw.w, 1.0));
        float2 axis = c1.w > pd.camera.w ? (c1.xy / c1.w - c0.xy / c0.w) * halfTarget : float2(0.0, 0.0);
        const float streak = length(axis);
        axis = streak > 1e-3 ? axis / streak : float2(0.0, -1.0);
        const float2 across = float2(-axis.y, axis.x);
        const float along = lerp(-radius, streak + radius, corner.y * 0.5 + 0.5);
        position.xy += (across * (corner.x * radius) + axis * along) / halfTarget * position.w;
        o.coord = float3(corner.x, along / radius, streak / radius);
        opacity *= sqrt(2.0 * radius / (2.0 * radius + streak));
    } else {
        const float s = sin(c.w), k = cos(c.w);
        const float2 turned = float2(corner.x * k - corner.y * s, corner.x * s + corner.y * k);
        position.xy += turned * radius / halfTarget * position.w;
        o.coord = float3(corner, 0.0);
    }
    o.position = position;
    const float distance = length(P - pd.camera.xyz);
    o.color = particleFinish(color, opacity, P, distance);
    o.info = float2(c0.w, frac(float(iid) * 0.618034)); // a puff's shape: one per slot
    return o;
}
#endif

#if defined(QUENCH_ENTRY_trail_vs)
StructuredBuffer<float4> vertices : QUENCH_STORAGE(0);

float4 trailClip(uint v) { return mul(pd.viewProj, float4(vertices[v * 2u].xyz, 1.0)); }

// Estimate the frame over the ribbon's width, rather than the last tiny movement of its emitter. This is bounded
// work and stops at a run break or the near plane. Interpolating the last step avoids a frame jump when the width
// crosses a sample. Wide soft ribbons then keep their frame through small cloth/emitter oscillations.
float3 trailNeighbour(uint first, uint count, uint i, bool backwards, float reach) {
    float3 previous = vertices[(first + i) * 2u].xyz;
    float travelled = 0.0;
    for (uint step = 0u; step < 8u; ++step) {
        if (backwards) {
            if (i == 0u || vertices[(first + i) * 2u].w < 0.0) break;
            --i;
        } else {
            if (i + 1u >= count || vertices[(first + i + 1u) * 2u].w < 0.0) break;
            ++i;
        }
        const float3 next = vertices[(first + i) * 2u].xyz;
        if (trailClip(first + i).w < pd.camera.w) break;
        const float distance = length(next - previous);
        if (travelled + distance >= reach)
            return lerp(previous, next, saturate((reach - travelled) / max(distance, 1e-6)));
        travelled += distance;
        previous = next;
    }
    return previous;
}

// A vertex of segment s of the strip: its end (0 or 1) and side (-1, 1). Each end is pushed out across the strip's
// direction on the screen there. The frame depends only on i, never on s: both segments sharing an endpoint must
// produce exactly the same corners, including when an emitter reverses or points along the view.
ParticleOut trail_vs(uint vid : SV_VertexID) {
    const uint first = uint(pd.draw.x);
    const uint count = uint(pd.draw.y);
    const uint s = vid / 6u;
    const float2 corner = kParticleCorners[vid % 6u];
    const uint end = corner.y > 0.0 ? 1u : 0u;
    const uint i = s + end; // in the strip
    if (s + 1u >= count) return particleCulled();
    if (vertices[(first + s + 1u) * 2u].w < 0.0) return particleCulled(); // the strip breaks there
    const float4 p = vertices[(first + i) * 2u];
    const float4 color = vertices[(first + i) * 2u + 1u];
    const float4 c0 = mul(pd.viewProj, float4(p.xyz, 1.0));
    const float4 ca = trailClip(first + s);
    const float4 cb = trailClip(first + s + 1u);
    if (ca.w < pd.camera.w || cb.w < pd.camera.w) return particleCulled();
    const float reach = max(2.0 * abs(p.w), 0.03);
    const float3 from = trailNeighbour(first, count, i, true, reach);
    const float3 to = trailNeighbour(first, count, i, false, reach);
    const float4 before = mul(pd.viewProj, float4(from, 1.0));
    const float4 after = mul(pd.viewProj, float4(to, 1.0));
    const float2 halfTarget = 0.5 * pd.target.xy;
    const float2 incoming = (c0.xy / c0.w - before.xy / before.w) * halfTarget;
    const float2 outgoing = (after.xy / after.w - c0.xy / c0.w) * halfTarget;
    const float inLength = length(incoming), outLength = length(outgoing);
    const float2 chord = incoming + outgoing;
    const float len = length(chord);
    // How much of the strip faces the camera here: its length on the screen over the length it would have seen from
    // the side (the sine of its angle to the view). Where it points along the view its direction on the screen is
    // lost and wide quads would fan out round the vertex, so it fades out there.
    const float side = length(to - from) * pd.right.w / c0.w;
    const float facing = side > 1e-3 ? smoothstep(0.08, 0.3, len / side) : 0.0;
    // An angle bisector handles unequal sample spacing. At a reversal the fallback is still identical in both
    // neighbouring segments; the join below closes to zero instead of swapping the two edges of a wide quad.
    const float2 fallback = inLength > outLength ? incoming / max(inLength, 1e-4)
                                                : outgoing / max(outLength, 1e-4);
    const float2 inDir = inLength > 1e-4 ? incoming / inLength : fallback;
    const float2 outDir = outLength > 1e-4 ? outgoing / outLength : fallback;
    const float2 bisector = inDir + outDir;
    const float2 dir = dot(bisector, bisector) > 1e-8 ? normalize(bisector) : fallback;
    const float2 across = float2(-dir.y, dir.x);

    const float pixels = abs(p.w) * pd.right.w / c0.w;
    const float least = max(pd.fade.z, 0.0) * 0.5;
    float halfWidth = max(pixels, least);
    if (inLength > 1e-4 && outLength > 1e-4) {
        const float alignment = clamp(dot(inDir, outDir), -1.0, 1.0);
        const float sine = sqrt(max((1.0 - alignment) * 0.5, 0.0));
        const float cosine = sqrt(max((1.0 + alignment) * 0.5, 0.0));
        // Keep the inside edge from folding across its adjacent segment at a tight turn. A straight or gently
        // curved strip retains its authored width; a true cusp tapers to one point.
        const float joinWidth = 0.45 * min(inLength, outLength) * cosine / max(sine, 1e-4);
        halfWidth = min(halfWidth, joinWidth);
    }
    const float thin = pixels < least ? pixels / least : 1.0;
    ParticleOut o;
    o.position = c0;
    o.position.xy += across * (corner.x * halfWidth) / halfTarget * c0.w;
    o.coord = float3(corner.x, 0.0, 0.0);
    o.color = particleFinish(color.rgb, color.a * thin * facing, p.xyz, length(p.xyz - pd.camera.xyz));
    o.info = float2(c0.w, 0.0);
    return o;
}
#endif

#if defined(QUENCH_ENTRY_particle_fs) || defined(QUENCH_ENTRY_trail_fs)
Texture2D<float> depthTex : QUENCH_TEXTURE(0);
SamplerState depthSampler : QUENCH_SAMPLER(0);

// How much of a fragment at `distance` (m along the view) is in front of the scene: 0 behind it, fading in over the
// softness (reversed depth: the scene's distance is near / depth).
float particleDepthFade(float4 position, float distance) {
    const float z = depthTex.SampleLevel(depthSampler, position.xy * pd.target.zw, 0.0);
    const float scene = z > 0.0 ? pd.camera.w / z : 1e30;
    const float gap = scene - distance;
    if (gap <= 0.0) return 0.0;
    return pd.fade.x > 0.0 ? saturate(gap / pd.fade.x) : 1.0;
}
#endif

#if defined(QUENCH_ENTRY_particle_fs)
float smoothDisc(float d) {
    const float t = saturate(1.0 - d);
    return t * t * (3.0 - 2.0 * t);
}

// A bright core in a soft halo, 1 at the centre, 0 at the rim.
float glowShape(float d) {
    const float halo = saturate(1.0 - d);
    return exp(-d * d * 18.0) * 0.75 + halo * halo * 0.25;
}

float particleShape(float3 coord, float random) {
    const int sprite = int(pd.draw.y + 0.5);
    if (pd.draw.z > 0.5) {
        // Along the motion: the capsule's distance from its axis, brightest at the particle.
        const float y = coord.y;
        const float dy = y < 0.0 ? y : max(y - coord.z, 0.0);
        const float d = sqrt(coord.x * coord.x + dy * dy);
        const float head = coord.z > 1e-3 ? 1.0 - 0.6 * saturate(y / coord.z) : 1.0;
        return (sprite == QUENCH_PARTICLE_SPRITE_SOFT || sprite == QUENCH_PARTICLE_SPRITE_PUFF ? smoothDisc(d)
                                                                                                 : glowShape(d)) *
               head;
    }
    const float2 p = coord.xy;
    const float d = length(p);
    if (sprite == QUENCH_PARTICLE_SPRITE_SOFT) return smoothDisc(d);
    if (sprite == QUENCH_PARTICLE_SPRITE_STAR) {
        // A small glow with four thin rays out to the quad's edges.
        const float2 q = abs(p);
        const float2 fall = pow(saturate(1.0 - q.yx), 3.0);
        const float rays = exp(-q.x * 60.0) * fall.x + exp(-q.y * 60.0) * fall.y;
        return exp(-d * d * 60.0) + saturate(1.0 - d * 2.0) * 0.15 + rays * 0.5;
    }
    if (sprite == QUENCH_PARTICLE_SPRITE_PUFF) {
        // A soft blob whose rim and density wander with the angle and its random.
        const float angle = atan2(p.y, p.x);
        const float r = random * 6.2831853;
        const float rim =
            1.0 - 0.18 * (0.5 + 0.5 * sin(angle * 3.0 + r)) - 0.12 * (0.5 + 0.5 * sin(angle * 5.0 - r * 2.0));
        const float body = smoothDisc(d / rim);
        const float lumps = 0.8 + 0.2 * sin(p.x * 7.0 + r) * sin(p.y * 6.0 - r * 1.7);
        return body * body * lumps;
    }
    return glowShape(d);
}

float4 particle_fs(ParticleOut vin) : SV_Target0 {
    const float fade = particleDepthFade(vin.position, vin.info.x);
    if (fade <= 0.0) discard;
    return vin.color * (particleShape(vin.coord, vin.info.y) * fade);
}
#endif

#if defined(QUENCH_ENTRY_trail_fs)
// Across the strip: a hot core in a soft edge.
float4 trail_fs(ParticleOut vin) : SV_Target0 {
    const float fade = particleDepthFade(vin.position, vin.info.x);
    if (fade <= 0.0) discard;
    const float x = vin.coord.x;
    const float edge = saturate(1.0 - x * x);
    return vin.color * (edge * (0.35 + 0.65 * exp(-x * x * 7.0)) * fade);
}
#endif

#endif // the draws
