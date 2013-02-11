/*
 * This file is part of mplayer2.
 *
 * mplayer2 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mplayer2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mplayer2; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

// Note that this file is not directly passed as shader, but run through some
// text processing functions, and in fact contains multiple vertex and fragment
// shaders.

// inserted at the beginning of all shaders
#!section prelude

// GLSL 1.20 compatibility layer
// texture() should be assumed to always map to texture2D()
#if __VERSION__ >= 130
# define texture1D texture
# define texture3D texture
# define DECLARE_FRAGPARMS \
    out vec4 out_color;
#else
# define texture texture2D
# define DECLARE_FRAGPARMS
# define out_color gl_FragColor
# define in varying
vec3 mix(vec3 x, vec3 y, bvec3 a) {
    return mix(x, y, vec3(a));
}
#endif

#!section vertex_all

#if __VERSION__ < 130
# undef in
# define in attribute
# define out varying
#endif

uniform mat3 transform;
uniform sampler3D lut_3d;
uniform vec3 lut_3d_size;

in vec2 vertex_position;
in vec4 vertex_color;
out vec4 color;
in vec2 vertex_texcoord;
out vec2 texcoord;

void main() {
    vec3 position = vec3(vertex_position, 1);
#ifndef FIXED_SCALE
    position = transform * position;
#endif
    gl_Position = vec4(position, 1);
    color = vertex_color;
#ifdef USE_3DLUT
    vec3 lut_3d_texcoord = color.rgb - (color.rgb - 0.5) / lut_3d_size;
    color = vec4(texture3D(lut_3d, lut_3d_texcoord).rgb, color.a);
#endif
    texcoord = vertex_texcoord;
}

#!section frag_eosd
uniform sampler2D textures[3];

in vec2 texcoord;
in vec4 color;
DECLARE_FRAGPARMS

void main() {
    out_color = vec4(color.rgb, color.a * texture(textures[0], texcoord).r);
}

#!section frag_osd
uniform sampler2D textures[3];

in vec2 texcoord;
in vec4 color;
DECLARE_FRAGPARMS

void main() {
    out_color = texture(textures[0], texcoord).rrrg * color;
}

#!section frag_video
uniform sampler2D textures[3];
uniform vec2 textures_size[3];
uniform vec2 textures_offset[3];
uniform sampler1D lut_c_1d;
uniform sampler1D lut_l_1d;
uniform sampler2D lut_c_2d;
uniform sampler2D lut_l_2d;
uniform sampler3D lut_3d;
uniform sampler1D lut_invgamma;
uniform sampler2D lut_invgamma_rounded;
uniform sampler2D dither;
uniform mat4x3 colormatrix;
uniform vec3 inv_gamma;
uniform float conv_gamma;
uniform float dither_quantization;
uniform float filter_param1;
uniform vec3 lut_3d_size;
uniform float lut_invgamma_size;
uniform vec2 dither_size;

in vec2 texcoord;
DECLARE_FRAGPARMS

vec4 sample_bilinear(sampler2D tex, vec2 texsize, vec2 texcoord) {
    return texture(tex, texcoord);
}

// Explanation how bicubic scaling with only 4 texel fetches is done:
//   http://www.mate.tue.nl/mate/pdfs/10318.pdf
//   'Efficient GPU-Based Texture Interpolation using Uniform B-Splines'
// Explanation why this algorithm normally always blurs, even with unit scaling:
//   http://bigwww.epfl.ch/preprints/ruijters1001p.pdf
//   'GPU Prefilter for Accurate Cubic B-spline Interpolation'
vec4 calcweights(float s) {
    vec4 t = vec4(-0.5, 0.1666, 0.3333, -0.3333) * s + vec4(1, 0, -0.5, 0.5);
    t = t * s + vec4(0, 0, -0.5, 0.5);
    t = t * s + vec4(-0.6666, 0, 0.8333, 0.1666);
    vec2 a = vec2(1 / t.z, 1 / t.w);
    t.xy = t.xy * a + vec2(1, 1);
    t.x = t.x + s;
    t.y = t.y - s;
    return t;
}

vec4 sample_bicubic_fast(sampler2D tex, vec2 texsize, vec2 texcoord) {
    vec2 pt = 1 / texsize;
    vec2 fcoord = fract(texcoord * texsize + vec2(0.5, 0.5));
    vec4 parmx = calcweights(fcoord.x);
    vec4 parmy = calcweights(fcoord.y);
    vec4 cdelta;
    cdelta.xz = parmx.rg * vec2(-pt.x, pt.x);
    cdelta.yw = parmy.rg * vec2(-pt.y, pt.y);
    // first y-interpolation
    vec4 ar = texture(tex, texcoord + cdelta.xy);
    vec4 ag = texture(tex, texcoord + cdelta.xw);
    vec4 ab = mix(ag, ar, parmy.b);
    // second y-interpolation
    vec4 br = texture(tex, texcoord + cdelta.zy);
    vec4 bg = texture(tex, texcoord + cdelta.zw);
    vec4 aa = mix(bg, br, parmy.b);
    // x-interpolation
    return mix(aa, ab, parmx.b);
}

float[2] weights2(sampler1D lookup, float f) {
    vec4 c = texture1D(lookup, f);
    float array[2];
    array[0] = c.r;
    array[1] = c.g;
    return array;
}

float[4] weights4(sampler1D lookup, float f) {
    vec4 c = texture1D(lookup, f);
    float array[4];
    array[0] = c.r;
    array[1] = c.g;
    array[2] = c.b;
    array[3] = c.a;
    return array;
}

float[6] weights6(sampler2D lookup, float f) {
    vec4 c1 = texture(lookup, vec2(0.25, f));
    vec4 c2 = texture(lookup, vec2(0.75, f));
    float array[6];
    array[0] = c1.r;
    array[1] = c1.g;
    array[2] = c1.b;
    array[3] = c2.r;
    array[4] = c2.g;
    array[5] = c2.b;
    return array;
}

float[8] weights8(sampler2D lookup, float f) {
    vec4 c1 = texture(lookup, vec2(0.25, f));
    vec4 c2 = texture(lookup, vec2(0.75, f));
    float array[8];
    array[0] = c1.r;
    array[1] = c1.g;
    array[2] = c1.b;
    array[3] = c1.a;
    array[4] = c2.r;
    array[5] = c2.g;
    array[6] = c2.b;
    array[7] = c2.a;
    return array;
}

float[12] weights12(sampler2D lookup, float f) {
    vec4 c1 = texture(lookup, vec2(1.0/6.0, f));
    vec4 c2 = texture(lookup, vec2(0.5, f));
    vec4 c3 = texture(lookup, vec2(5.0/6.0, f));
    float array[12];
    array[0] = c1.r;
    array[1] = c1.g;
    array[2] = c1.b;
    array[3] = c1.a;
    array[4] = c2.r;
    array[5] = c2.g;
    array[6] = c2.b;
    array[7] = c2.a;
    array[8] = c3.r;
    array[9] = c3.g;
    array[10] = c3.b;
    array[11] = c3.a;
    return array;
}

float[16] weights16(sampler2D lookup, float f) {
    vec4 c1 = texture(lookup, vec2(0.125, f));
    vec4 c2 = texture(lookup, vec2(0.375, f));
    vec4 c3 = texture(lookup, vec2(0.625, f));
    vec4 c4 = texture(lookup, vec2(0.875, f));
    float array[16];
    array[0] = c1.r;
    array[1] = c1.g;
    array[2] = c1.b;
    array[3] = c1.a;
    array[4] = c2.r;
    array[5] = c2.g;
    array[6] = c2.b;
    array[7] = c2.a;
    array[8] = c3.r;
    array[9] = c3.g;
    array[10] = c3.b;
    array[11] = c3.a;
    array[12] = c4.r;
    array[13] = c4.g;
    array[14] = c4.b;
    array[15] = c4.a;
    return array;
}

#define CONVOLUTION_SEP_N(NAME, N)                                          \
    vec4 NAME(sampler2D tex, vec2 texcoord, vec2 pt, float weights[N]) {    \
        vec4 res = vec4(0);                                                 \
        for (int n = 0; n < N; n++) {                                       \
            res += weights[n] * texture(tex, texcoord + pt * n);            \
        }                                                                   \
        return res;                                                         \
    }

CONVOLUTION_SEP_N(convolution_sep2, 2)
CONVOLUTION_SEP_N(convolution_sep4, 4)
CONVOLUTION_SEP_N(convolution_sep6, 6)
CONVOLUTION_SEP_N(convolution_sep8, 8)
CONVOLUTION_SEP_N(convolution_sep12, 12)
CONVOLUTION_SEP_N(convolution_sep16, 16)

// The dir parameter is (0, 1) or (1, 0), and we expect the shader compiler to
// remove all the redundant multiplications and additions.
#define SAMPLE_CONVOLUTION_SEP_N(NAME, N, SAMPLERT, CONV_FUNC, WEIGHTS_FUNC)\
    vec4 NAME(vec2 dir, SAMPLERT lookup, sampler2D tex, vec2 texsize,       \
              vec2 texcoord) {                                              \
        vec2 pt = (1 / texsize) * dir;                                      \
        float fcoord = dot(fract(texcoord * texsize - 0.5), dir);           \
        vec2 base = texcoord - fcoord * pt;                                 \
        return CONV_FUNC(tex, base - pt * (N / 2 - 1), pt,                  \
                         WEIGHTS_FUNC(lookup, fcoord));                     \
    }

SAMPLE_CONVOLUTION_SEP_N(sample_convolution_sep2, 2, sampler1D, convolution_sep2, weights2)
SAMPLE_CONVOLUTION_SEP_N(sample_convolution_sep4, 4, sampler1D, convolution_sep4, weights4)
SAMPLE_CONVOLUTION_SEP_N(sample_convolution_sep6, 6, sampler2D, convolution_sep6, weights6)
SAMPLE_CONVOLUTION_SEP_N(sample_convolution_sep8, 8, sampler2D, convolution_sep8, weights8)
SAMPLE_CONVOLUTION_SEP_N(sample_convolution_sep12, 12, sampler2D, convolution_sep12, weights12)
SAMPLE_CONVOLUTION_SEP_N(sample_convolution_sep16, 16, sampler2D, convolution_sep16, weights16)


#define CONVOLUTION_N(NAME, N)                                               \
    vec4 NAME(sampler2D tex, vec2 texcoord, vec2 pt, float taps_x[N],        \
              float taps_y[N]) {                                             \
        vec4 res = vec4(0);                                                  \
        for (int y = 0; y < N; y++) {                                        \
            vec4 line = vec4(0);                                             \
            for (int x = 0; x < N; x++)                                      \
                line += taps_x[x] * texture(tex, texcoord + pt * vec2(x, y));\
            res += taps_y[y] * line;                                         \
        }                                                                    \
        return res;                                                          \
    }

CONVOLUTION_N(convolution2, 2)
CONVOLUTION_N(convolution4, 4)
CONVOLUTION_N(convolution6, 6)
CONVOLUTION_N(convolution8, 8)
CONVOLUTION_N(convolution12, 12)
CONVOLUTION_N(convolution16, 16)

#define SAMPLE_CONVOLUTION_N(NAME, N, SAMPLERT, CONV_FUNC, WEIGHTS_FUNC)    \
    vec4 NAME(SAMPLERT lookup, sampler2D tex, vec2 texsize, vec2 texcoord) {\
        vec2 pt = 1 / texsize;                                              \
        vec2 fcoord = fract(texcoord * texsize - 0.5);                      \
        vec2 base = texcoord - fcoord * pt;                                 \
        return CONV_FUNC(tex, base - pt * (N / 2 - 1), pt,                  \
                         WEIGHTS_FUNC(lookup, fcoord.x),                    \
                         WEIGHTS_FUNC(lookup, fcoord.y));                   \
    }

SAMPLE_CONVOLUTION_N(sample_convolution2, 2, sampler1D, convolution2, weights2)
SAMPLE_CONVOLUTION_N(sample_convolution4, 4, sampler1D, convolution4, weights4)
SAMPLE_CONVOLUTION_N(sample_convolution6, 6, sampler2D, convolution6, weights6)
SAMPLE_CONVOLUTION_N(sample_convolution8, 8, sampler2D, convolution8, weights8)
SAMPLE_CONVOLUTION_N(sample_convolution12, 12, sampler2D, convolution12, weights12)
SAMPLE_CONVOLUTION_N(sample_convolution16, 16, sampler2D, convolution16, weights16)


// Unsharp masking
vec4 sample_sharpen3(sampler2D tex, vec2 texsize, vec2 texcoord) {
    vec2 pt = 1 / texsize;
    vec2 st = pt * 0.5;
    vec4 p = texture(tex, texcoord);
    vec4 sum = texture(tex, texcoord + st * vec2(+1, +1))
             + texture(tex, texcoord + st * vec2(+1, -1))
             + texture(tex, texcoord + st * vec2(-1, +1))
             + texture(tex, texcoord + st * vec2(-1, -1));
    return p + (p - 0.25 * sum) * filter_param1;
}

vec4 sample_sharpen5(sampler2D tex, vec2 texsize, vec2 texcoord) {
    vec2 pt = 1 / texsize;
    vec2 st1 = pt * 1.2;
    vec4 p = texture(tex, texcoord);
    vec4 sum1 = texture(tex, texcoord + st1 * vec2(+1, +1))
              + texture(tex, texcoord + st1 * vec2(+1, -1))
              + texture(tex, texcoord + st1 * vec2(-1, +1))
              + texture(tex, texcoord + st1 * vec2(-1, -1));
    vec2 st2 = pt * 1.5;
    vec4 sum2 = texture(tex, texcoord + st2 * vec2(+1,  0))
              + texture(tex, texcoord + st2 * vec2( 0, +1))
              + texture(tex, texcoord + st2 * vec2(-1,  0))
              + texture(tex, texcoord + st2 * vec2( 0, -1));
    vec4 t = p * 0.859375 + sum2 * -0.1171875 + sum1 * -0.09765625;
    return p + t * filter_param1;
}

struct rounded_colors {
    vec3 floored, ceiled;
};

#ifdef USE_3DLUT
vec3 display_gamma_expand(vec3 color) {
    // Map 0 and 1 to texel centers
    color -= (color - 0.5) / lut_invgamma_size;
    float r = texture1D(lut_invgamma, color.r).r;
    float g = texture1D(lut_invgamma, color.g).g;
    float b = texture1D(lut_invgamma, color.b).b;
    return vec3(r, g, b);
}
rounded_colors display_gamma_expand_rounded(vec3 color) {
    vec2 r = texture(lut_invgamma_rounded, vec2(color.r, 0.5 / 3)).xy;
    vec2 g = texture(lut_invgamma_rounded, vec2(color.g, 1.5 / 3)).xy;
    vec2 b = texture(lut_invgamma_rounded, vec2(color.b, 2.5 / 3)).xy;
    return rounded_colors(vec3(r.x, g.x, b.x), vec3(r.y, g.y, b.y));
}
#else
// Assume sRGB
vec3 display_gamma_compress(vec3 color) {
    return mix(color * 12.92,
               (pow(color, vec3(1.0/2.4)) * 1055 - 55) / 1000,
               greaterThan(color, vec3(0.0031308)));
}
vec3 display_gamma_expand(vec3 color) {
    return mix(color / 12.92,
               pow((color * 1000 + 55) / 1055, vec3(2.4)),
               greaterThan(color, vec3(0.040449936)));
}
rounded_colors display_gamma_expand_rounded(vec3 color) {
    color *= dither_quantization;
    vec3 floored = display_gamma_expand(floor(color) / dither_quantization);
    vec3 ceiled = display_gamma_expand(ceil(color) / dither_quantization);
    return rounded_colors(floored, ceiled);
}
#endif

void main() {
#ifdef USE_PLANAR
    vec3 color = vec3(SAMPLE_L(textures[0], textures_size[0], texcoord + textures_offset[0]).r,
                      SAMPLE_C(textures[1], textures_size[1], texcoord + textures_offset[1]).r,
                      SAMPLE_C(textures[2], textures_size[2], texcoord + textures_offset[2]).r);
#else
    vec3 color = SAMPLE_L(textures[0], textures_size[0], texcoord).rgb;
#endif
#ifdef USE_GBRP
    color.gbr = color;
#endif
#ifdef USE_YGRAY
    // NOTE: actually slightly wrong for 16 bit input video, and completely
    //       wrong for 9/10 bit input
    color.gb = vec2(128.0/255.0);
#endif
#ifdef USE_COLORMATRIX
    color = mat3(colormatrix[0], colormatrix[1], colormatrix[2]) * color;
    color += colormatrix[3];
#endif
#ifdef USE_LINEAR_CONV
    color = pow(color, vec3(2.2));
#endif
#ifdef USE_LINEAR_CONV_INV
    // Convert from linear RGB to gamma RGB before putting it through the 3D-LUT
    // in the final stage.
    color = pow(color, vec3(1.0/2.2));
#endif
#ifdef USE_GAMMA_POW
    color = pow(color, inv_gamma);
#endif
#ifdef USE_3DLUT
    // The LUT has texels representing inputs from 0 to 1 inclusive.
    // Sample the center of the first texel for 0 and of the last for 1.
    color = texture3D(lut_3d, color - (color - 0.5) / lut_3d_size).rgb;
#endif
#ifdef USE_DITHER
    float dither_value = texture(dither, gl_FragCoord.xy / dither_size).r;
    // dither_value was originally an integer from the range 0..256,
    // but OpenGL assumed 0..255 when converting it to float. Correct this.
    dither_value *= 255.0 / 256.0;
    // The numbers in the WxH dither matrix are multiples of 1/WH
    // with values from zero inclusive to one exclusive. Add an offset
    // to center them around one half, so that the aggregate error
    // introduced by the dither is zero. This also makes dither_value
    // uniformly distributed on the entire range from zero to one
    // and, if repeated periodically, on the whole real line.
    dither_value += 0.5 / (dither_size.x * dither_size.y);
#ifdef USE_LINEAR_OUTPUT
    vec3 original = color;
    color = display_gamma_compress(color);
#else
    vec3 original = display_gamma_expand(color);
#endif
    rounded_colors rounded = display_gamma_expand_rounded(color);
    vec3 threshold = dither_value * (rounded.ceiled - rounded.floored);
    bvec3 selector = greaterThanEqual(original - rounded.floored, threshold);
    color *= dither_quantization;
    color = mix(floor(color), ceil(color), selector) / dither_quantization;
#ifdef USE_LINEAR_OUTPUT
    color = display_gamma_expand(color);
#endif
#endif
    out_color = vec4(color, 1);
}
