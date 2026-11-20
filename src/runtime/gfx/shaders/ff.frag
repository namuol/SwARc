#version 450
#ifdef HALF
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#endif
// The SDL_GPU backend's fixed-function fragment stage: D3D's texture-stage
// cascade, alpha test and fog, driven by uniforms (see ff.vert).
//
// The uniform layout here and FsUniforms in gpu_device.cpp must agree.
// Bindings follow SDL_GPU's SPIR-V conventions for fragment shaders:
// sampled textures in set 2, uniform buffers in set 3.

layout(std140, set = 3, binding = 0) uniform FsUniforms {
  ivec4 color[8];   // per stage: COLOROP, COLORARG1, COLORARG2, COLORARG0
  ivec4 alpha[8];   // per stage: ALPHAOP, ALPHAARG1, ALPHAARG2, ALPHAARG0
  ivec4 stage[8];   // per stage: RESULTARG, texture bound, projected divisor index (-1 none), cube
  vec4 tfactor;
  vec4 fog_color;
  vec4 fog;         // start, end, density
  ivec4 mode;       // alpha test func (0 off), alpha ref, fog: 0 off / 1 vertex / 2 table, table mode
  ivec4 mode2;      // flat shading, specular enable, -, -
} u;

// The integer state, read through these: from the uniforms here, and as
// constants in the variants gpu_device.cpp compiles for each state at run
// time (SPECIALIZED: it defines k_color, k_alpha, k_stage, k_mode and
// k_mode2 with the uniforms' layout), which lets the compiler unroll the
// cascade and drop the switches. Same operations either way.
// HALF (Metal builds): the colour arithmetic in 16-bit floats, which
// Apple GPUs run at twice the rate (Chepel uncapped at 3456x2234: 179 ->
// 210 fps). Textures are read and the result written in 32 bits; against
// 32-bit arithmetic, outputs differ by at most one step in 255 on a few
// percent of pixels. Vulkan needs a device feature for it, so it stays off
// there. CV/C3/CF are the colour types, H(x) a constant of them.
#ifdef HALF
#define CV f16vec4
#define C3 f16vec3
#define CF float16_t
#define H(x) float16_t(x)
#else
#define CV vec4
#define C3 vec3
#define CF float
#define H(x) (x)
#endif

#ifdef SPECIALIZED
#define F_COLOR(s) k_color[s]
#define F_ALPHA(s) k_alpha[s]
#define F_STAGE(s) k_stage[s]
#define F_MODE k_mode
#define F_MODE2 k_mode2
#else
#define F_COLOR(s) u.color[s]
#define F_ALPHA(s) u.alpha[s]
#define F_STAGE(s) u.stage[s]
#define F_MODE u.mode
#define F_MODE2 u.mode2
#endif

layout(set = 2, binding = 0) uniform sampler2D tx0;
layout(set = 2, binding = 1) uniform sampler2D tx1;
layout(set = 2, binding = 2) uniform sampler2D tx2;
layout(set = 2, binding = 3) uniform sampler2D tx3;
layout(set = 2, binding = 4) uniform sampler2D tx4;
layout(set = 2, binding = 5) uniform sampler2D tx5;
layout(set = 2, binding = 6) uniform sampler2D tx6;
layout(set = 2, binding = 7) uniform sampler2D tx7;
// A stage's cube texture (stage[s].w): sampled with the coordinates' xyz,
// a direction (the reflection vector of TEXCOORDINDEX texgen, say).
layout(set = 2, binding = 8) uniform samplerCube tc0;
layout(set = 2, binding = 9) uniform samplerCube tc1;
layout(set = 2, binding = 10) uniform samplerCube tc2;
layout(set = 2, binding = 11) uniform samplerCube tc3;
layout(set = 2, binding = 12) uniform samplerCube tc4;
layout(set = 2, binding = 13) uniform samplerCube tc5;
layout(set = 2, binding = 14) uniform samplerCube tc6;
layout(set = 2, binding = 15) uniform samplerCube tc7;

layout(location = 0) in vec4 i_diff;
layout(location = 1) in vec4 i_spec;
layout(location = 2) flat in vec4 i_diff_flat;
layout(location = 3) flat in vec4 i_spec_flat;
layout(location = 4) in vec4 i_t[8];
layout(location = 12) in float i_fog;

layout(location = 0) out vec4 o_color;

CV Arg(int a, CV diff, CV cur, CV tex, CV tf, CV spec, CV temp) {
  CV v;
  switch (a & 0xF) {
    case 0: v = diff; break;
    case 1: v = cur; break;
    case 2: v = tex; break;
    case 3: v = tf; break;
    case 4: v = spec; break;
    case 5: v = temp; break;
    default: v = CV(H(0.0)); break;
  }
  if ((a & 0x20) != 0) v = CV(v.a);  // ALPHAREPLICATE
  if ((a & 0x10) != 0) v = H(1.0) - v;    // COMPLEMENT
  return v;
}

CV Op(int op, CV a1, CV a2, CV a0, CV diff, CV tex, CV tf, CV cur) {
  switch (op) {
    case 2: return a1;
    case 3: return a2;
    case 4: return a1 * a2;
    case 5: return a1 * a2 * H(2.0);
    case 6: return a1 * a2 * H(4.0);
    case 7: return a1 + a2;
    case 8: return a1 + a2 - H(0.5);
    case 9: return (a1 + a2 - H(0.5)) * H(2.0);
    case 10: return a1 - a2;
    case 11: return a1 + a2 - a1 * a2;
    case 12: return mix(a2, a1, diff.a);
    case 13: return mix(a2, a1, tex.a);
    case 14: return mix(a2, a1, tf.a);
    case 15: return a1 + a2 * (H(1.0) - tex.a);
    case 16: return mix(a2, a1, cur.a);
    case 17: return a1;  // PREMODULATE: approximated
    case 18: return CV(a1.rgb + a1.a * a2.rgb, a1.a);
    case 19: return CV(a1.rgb * a2.rgb + a1.a, a1.a);
    case 20: return CV((H(1.0) - a1.a) * a2.rgb + a1.rgb, a1.a);
    case 21: return CV((H(1.0) - a1.rgb) * a2.rgb + a1.a, a1.a);
    case 24: { CF d = clamp(H(4.0) * dot(a1.rgb - H(0.5), a2.rgb - H(0.5)), H(0.0), H(1.0)); return CV(d); }
    case 25: return a0 + a1 * a2;
    case 26: return mix(a2, a1, a0);
    default: return a1;  // bump mapping: not modelled
  }
}

bool AlphaPass(int func, float a, int ref) {
  int v = int(roundEven(clamp(a, 0.0, 1.0) * 255.0));
  switch (func) {
    case 1: return false;
    case 2: return v < ref;
    case 3: return v == ref;
    case 4: return v <= ref;
    case 5: return v > ref;
    case 6: return v != ref;
    case 7: return v >= ref;
    default: return true;
  }
}

float FogFactor(int mode, float d, vec4 fog) {
  if (mode == 1) return exp(-d * fog.z);
  if (mode == 2) { float x = d * fog.z; return exp(-x * x); }
  if (mode == 3) return fog.y == fog.x ? (d < fog.y ? 1.0 : 0.0) : (fog.y - d) / (fog.y - fog.x);
  return 1.0;
}

vec4 Sample(int s, vec2 uv) {
  switch (s) {
    case 0: return texture(tx0, uv);
    case 1: return texture(tx1, uv);
    case 2: return texture(tx2, uv);
    case 3: return texture(tx3, uv);
    case 4: return texture(tx4, uv);
    case 5: return texture(tx5, uv);
    case 6: return texture(tx6, uv);
    default: return texture(tx7, uv);
  }
}

vec4 SampleCube(int s, vec3 dir) {
  switch (s) {
    case 0: return texture(tc0, dir);
    case 1: return texture(tc1, dir);
    case 2: return texture(tc2, dir);
    case 3: return texture(tc3, dir);
    case 4: return texture(tc4, dir);
    case 5: return texture(tc5, dir);
    case 6: return texture(tc6, dir);
    default: return texture(tc7, dir);
  }
}

void main() {
  CV diff = CV(F_MODE2.x != 0 ? i_diff_flat : i_diff);
  CV spec = CV(F_MODE2.x != 0 ? i_spec_flat : i_spec);
  CV tfac = CV(u.tfactor);
  // The stages up to the one that ends the cascade, and their textures.
  int n = 0;
  CV texs[8];
  for (int s = 0; s < 8; s++) {
    if (F_COLOR(s).x == 1) break;  // DISABLE ends the cascade
    vec4 tc = i_t[s];
    int proj = F_STAGE(s).z;
    vec2 uv = proj >= 0 ? tc.xy / tc[proj] : tc.xy;
    // No texture: zero, what D3D's alpha arguments read.
    texs[s] = F_STAGE(s).y == 0 ? CV(H(0.0))
              : F_STAGE(s).w != 0 ? CV(SampleCube(s, tc.xyz))
                                  : CV(Sample(s, uv));
    n = s + 1;
  }
#ifdef ALPHA_TEST
  // The alpha test before the colours: a fragment that may be discarded
  // holds up hidden surface removal on tile-based GPUs until its shader
  // decides, so it decides first. Alpha arguments and operations work per
  // component, so the alpha cascade alone gives the alphas the full one
  // below computes, except where DOTPRODUCT3 (24) puts a colour into
  // alpha: then the test stays at the end.
  bool early = true;
  for (int s = 0; s < n; s++)
    if (F_COLOR(s).x == 24 || F_ALPHA(s).x == 24) early = false;
  CF alphas[8];  // each stage's alpha, for the cascade below to reuse
  if (early) {
    CV cur_a = CV(H(0.0), H(0.0), H(0.0), diff.a), temp_a = CV(H(0.0));
    for (int s = 0; s < n; s++) {
      ivec4 a = F_ALPHA(s);
      CF al = cur_a.a;
      if (a.x != 1) {
        CV tex = texs[s];
        CV a1 = Arg(a.y, diff, cur_a, tex, tfac, spec, temp_a);
        CV a2 = Arg(a.z, diff, cur_a, tex, tfac, spec, temp_a);
        CV a0 = Arg(a.w, diff, cur_a, tex, tfac, spec, temp_a);
        al = clamp(Op(a.x, a1, a2, a0, diff, tex, tfac, cur_a).a, H(0.0), H(1.0));
      }
      alphas[s] = al;
      if (F_STAGE(s).x == 5) temp_a = CV(H(0.0), H(0.0), H(0.0), al);
      else cur_a = CV(H(0.0), H(0.0), H(0.0), al);
    }
    if (!AlphaPass(F_MODE.x, float(cur_a.a), u.mode.y)) discard;
  }
#endif
  CV cur = diff, temp = CV(H(0.0));
  for (int s = 0; s < n; s++) {
    int cop = F_COLOR(s).x;
    CV tex = texs[s];
    ivec4 c = F_COLOR(s), a = F_ALPHA(s);
    CV c1 = Arg(c.y, diff, cur, tex, tfac, spec, temp);
    CV c2 = Arg(c.z, diff, cur, tex, tfac, spec, temp);
    CV c0 = Arg(c.w, diff, cur, tex, tfac, spec, temp);
    C3 rgb = clamp(Op(cop, c1, c2, c0, diff, tex, tfac, cur).rgb, H(0.0), H(1.0));
    CF al = cur.a;
#ifdef ALPHA_TEST
    if (early) {
      al = alphas[s];  // the same: alpha operations work per component
    } else
#endif
    if (a.x != 1) {
      CV a1 = Arg(a.y, diff, cur, tex, tfac, spec, temp);
      CV a2 = Arg(a.z, diff, cur, tex, tfac, spec, temp);
      CV a0 = Arg(a.w, diff, cur, tex, tfac, spec, temp);
      al = clamp(Op(a.x, a1, a2, a0, diff, tex, tfac, cur).a, H(0.0), H(1.0));
    }
    if (cop == 24) al = rgb.r;  // DOTPRODUCT3 replicates into alpha too
    if (F_STAGE(s).x == 5) temp = CV(rgb, al);
    else cur = CV(rgb, al);
  }
  if (F_MODE2.y != 0) cur.rgb = clamp(cur.rgb + spec.rgb, H(0.0), H(1.0));
  // Built twice (gpu_device.cpp picks per draw): with ALPHA_TEST for draws
  // whose alpha test is on, and without for the rest, where AlphaPass
  // would pass anyway. A shader that can discard turns off hidden surface
  // removal on tile-based GPUs (Apple's), for every draw that uses it.
#ifdef ALPHA_TEST
  if (!early && !AlphaPass(F_MODE.x, float(cur.a), u.mode.y)) discard;
#endif
  if (F_MODE.z != 0) {
    float f = F_MODE.z == 2 ? FogFactor(F_MODE.w, i_fog, u.fog) : i_fog;
    cur.rgb = C3(mix(u.fog_color.rgb, vec3(cur.rgb), clamp(f, 0.0, 1.0)));
  }
  o_color = vec4(cur);
}
