/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <tev/FalseColor.h>
#include <tev/Image.h>
#include <tev/UberShader.h>

using namespace nanogui;
using namespace std;

namespace tev {

static constexpr size_t DITHER_MATRIX_SIZE = 8;
using DitherMatrix = array<float, DITHER_MATRIX_SIZE * DITHER_MATRIX_SIZE>;

static DitherMatrix ditherMatrix(float scale) {
    // 8x8 Bayer dithering matrix scaled to [-0.5f, 0.5f] / 255
    DitherMatrix mat = {
        {0, 32, 8,  40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4, 36, 14, 46, 6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
         3, 35, 11, 43, 1, 33, 9,  41, 51, 19, 59, 27, 49, 17, 57, 25, 15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21}
    };

    for (size_t i = 0; i < DITHER_MATRIX_SIZE * DITHER_MATRIX_SIZE; ++i) {
        mat[i] = (mat[i] / DITHER_MATRIX_SIZE / DITHER_MATRIX_SIZE - 0.5f) * scale;
    }

    return mat;
}

enum class EShaderChannelConfig : int { R = 0, RG, RGB, RA, RGA, RGBA };

UberShader::UberShader(RenderPass* renderPass, float ditherScale) {
    try {
#if defined(NANOGUI_USE_OPENGL) || defined(NANOGUI_USE_GLES)
#    if defined(NANOGUI_USE_OPENGL)
        const string preamble = R"(#version 110)";
#    elif defined(NANOGUI_USE_GLES)
        const string preamble =
            R"(#version 100
        precision highp float;)";
#    endif
        const auto vertexShader = preamble +
            R"glsl(
            uniform vec2 pixelSize;
            uniform vec2 checkerSize;
            uniform float ditherSize;

            uniform vec2 imageScale;
            uniform vec2 imageOffset;

            uniform vec2 referenceScale;
            uniform vec2 referenceOffset;

            attribute vec2 position;

            varying vec2 checkerUv;
            varying vec2 ditherUv;

            varying vec2 imageUv;
            varying vec2 referenceUv;

            void main() {
                // The offset of 0.25 is necessary to avoid sampling exact pixel borders. The offset is 0.25 rather than 0.5, because the
                // domain of the screen shader is [-1, 1] rather than [0, 1].
                checkerUv = (position / pixelSize + 0.25) / checkerSize;
                ditherUv = (position / pixelSize + 0.25) / ditherSize;

                imageUv = position * imageScale + imageOffset;
                referenceUv = position * referenceScale + referenceOffset;

                gl_Position = vec4(position, 1.0, 1.0);
            })glsl";

        const auto fragmentShader = preamble +
            R"glsl(
            #define SRGB        0
            #define GAMMA       1
            #define FALSE_COLOR 2
            #define POS_NEG     3
            #define REINHARD    4

            #define ERROR                   0
            #define ABSOLUTE_ERROR          1
            #define SQUARED_ERROR           2
            #define RELATIVE_ABSOLUTE_ERROR 3
            #define RELATIVE_SQUARED_ERROR  4

            #define CHANNEL_CONFIG_R    0
            #define CHANNEL_CONFIG_RG   1
            #define CHANNEL_CONFIG_RGB  2
            #define CHANNEL_CONFIG_RA   3
            #define CHANNEL_CONFIG_RGA  4
            #define CHANNEL_CONFIG_RGBA 5

            #define SRGB_POW 2.4
            #define SRGB_CUT 0.0031308
            #define SRGB_SCALE 12.92
            #define SRGB_ALPHA 1.055

            uniform sampler2D image;
            uniform sampler2D imageHighlight;
            uniform sampler2D highlightMaskImage;
            uniform bool hasImage;
            uniform bool useHighlightMaskTexture;
            uniform vec2 imageResolution;

            uniform sampler2D reference;
            uniform sampler2D referenceHighlight;
            uniform bool hasReference;
            uniform vec2 referenceResolution;

            uniform int channelConfig;

            uniform sampler2D colormap;
            uniform sampler2D ditherMatrix;

            uniform float exposure;
            uniform float offset;
            uniform float gamma;
            uniform float colorMultiplier;
            uniform bool clipToLdr;
            uniform int tonemap;
            uniform int falseColorScaleMode;
            uniform vec2 falseColorRange;
            uniform int metric;
            uniform vec4 channelMask;
            uniform bool highlightEnabled;
            uniform vec2 highlightRange;
            uniform int highlightMode;
            uniform float highlightScale;
            uniform mat3 highlightMatrix;
            uniform int highlightTransfer;
            uniform int highlightValueMode;
            uniform bool displayUseInspection;

            uniform vec2 cropMin;
            uniform vec2 cropMax;

            uniform vec4 bgColor;

            varying vec2 checkerUv;
            varying vec2 ditherUv;

            varying vec2 imageUv;
            varying vec2 referenceUv;

            float average(vec3 col) {
                return (col.r + col.g + col.b) / 3.0;
            }

            float falseColorScalar(vec3 col) {
                return max(dot(max(col, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722)), 0.0);
            }

            float falseColorCoord(float value) {
                float minValue = max(falseColorRange.x, 0.0);
                float maxValue = max(falseColorRange.y, minValue + 1e-6);

                if (falseColorScaleMode == 0) {
                    return clamp((value - minValue) / (maxValue - minValue), 0.0, 1.0);
                }

                float safeMin = max(minValue, 1e-6);
                float safeValue = max(value, safeMin);
                float base = float(falseColorScaleMode + 1);
                float invLogBase = 1.0 / log(base);
                float minLog = log(safeMin) * invLogBase;
                float maxLog = log(maxValue) * invLogBase;
                float valueLog = log(safeValue) * invLogBase;
                return clamp((valueLog - minLog) / max(maxLog - minLog, 1e-6), 0.0, 1.0);
            }

            vec3 applyExposureAndOffset(vec3 col) {
                return pow(2.0, exposure) * col + offset;
            }

            vec3 falseColor(float v) {
                v = clamp(v, 0.0, 1.0);
                return texture2D(colormap, vec2(v, 0.5)).rgb;
            }

            vec3 mixb(vec3 a, vec3 b, bvec3 mask) {
                return mix(a, b, vec3(mask));
            }

            vec3 invLinPow(vec3 color, float gamma, float thres, float scale, float alpha) {
                bvec3 isLow = lessThanEqual(color.rgb, vec3(thres * scale));
                vec3 lo = color.rgb / scale;
                vec3 hi = pow((color.rgb + alpha - 1.0) / alpha, vec3(gamma));
                return mixb(hi, lo, isLow);
            }

            vec3 linear(vec3 color) {
                return sign(color) * invLinPow(abs(color), SRGB_POW, SRGB_CUT, SRGB_SCALE, SRGB_ALPHA);
            }

            vec3 linPow(vec3 color, float gamma, float thres, float scale, float alpha) {
                bvec3 isLow = lessThanEqual(color.rgb, vec3(thres));
                vec3 lo = color.rgb * scale;
                vec3 hi = pow(color.rgb, vec3(1.0 / gamma)) * alpha - (alpha - 1.0);
                return mixb(hi, lo, isLow);
            }

            vec3 srgb(vec3 color) {
                return sign(color) * linPow(abs(color), SRGB_POW, SRGB_CUT, SRGB_SCALE, SRGB_ALPHA);
            }

            vec3 applyTonemap(vec3 col, vec4 background) {
                if (tonemap == SRGB) {
                    return srgb(col + (linear(background.rgb) - offset) * background.a);
                } else if (tonemap == GAMMA) {
                    col = col + (pow(background.rgb, vec3(gamma)) - offset) * background.a;
                    return sign(col) * pow(abs(col), vec3(1.0 / gamma));
                } else if (tonemap == FALSE_COLOR) {
                    return falseColor(falseColorCoord(falseColorScalar(col))) + background.rgb * background.a;
                } else if (tonemap == POS_NEG) {
                    return vec3(-average(min(col, vec3(0.0))) * 2.0, average(max(col, vec3(0.0))) * 2.0, 0.0) + background.rgb * background.a;
                } else if (tonemap == REINHARD) {
                    col = max(col + (linear(background.rgb) - offset) * background.a, vec3(0.0));
                    float luminance = dot(col, vec3(0.2126, 0.7152, 0.0722));
                    return srgb(col / (1.0 + luminance));
                }

                return vec3(0.0);
            }

            vec3 applyMetric(vec3 col, vec3 reference) {
                if (metric == ERROR) {
                    return col;
                } else if (metric == ABSOLUTE_ERROR) {
                    return abs(col);
                } else if (metric == SQUARED_ERROR) {
                    return col * col;
                } else if (metric == RELATIVE_ABSOLUTE_ERROR) {
                    return abs(col) / (reference + vec3(0.01));
                } else if (metric == RELATIVE_SQUARED_ERROR) {
                    return col * col / (reference * reference + vec3(0.01));
                }

                return vec3(0.0);
            }

            vec4 applyMask(vec4 color, vec4 mask) {
                color.rgb *= mask.rgb;
                color.a = mask.a == 1.0 ? color.a : 1.0;
                return color;
            }

            vec4 sample(sampler2D sampler, vec4 mask, vec2 uv) {
                if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
                    return vec4(0.0);
                }

                vec4 color = texture2D(sampler, uv);

                // Duplicate first channel in monochromatic images and move alpha to end if not already there.
                if (channelConfig == CHANNEL_CONFIG_R) {
                    color = vec4(color.r, color.r, color.r, 1.0);
                } else if (channelConfig == CHANNEL_CONFIG_RA) {
                    color = vec4(color.r, color.r, color.r, color.g);
                } else if (channelConfig == CHANNEL_CONFIG_RGA) {
                    color = vec4(color.r, color.g, 0.0, color.b);
                }

                color = applyMask(color, mask);
                return color;
            }

            vec2 nearestUv(vec2 uv, vec2 resolution) {
                return (floor(uv * resolution) + 0.5) / resolution;
            }

            vec4 dither(vec4 color) {
                color.rgb += texture2D(ditherMatrix, fract(ditherUv)).r;
                return color;
            }

            bool inHighlightRange(float value) {
                return value >= highlightRange.x && value <= highlightRange.y;
            }

            vec3 computeHighlightMask(vec3 value) {
                if (highlightMode == 1) {
                    return vec3(inHighlightRange(value.r) ? 1.0 : 0.0, 0.0, 0.0);
                } else if (highlightMode == 2) {
                    return vec3(0.0, inHighlightRange(value.g) ? 1.0 : 0.0, 0.0);
                } else if (highlightMode == 3) {
                    return vec3(0.0, 0.0, inHighlightRange(value.b) ? 1.0 : 0.0);
                } else if (highlightMode == 4) {
                    return vec3(inHighlightRange(value.r) ? 1.0 : 0.0);
                }
                bool allChannelsInRange = inHighlightRange(value.r) && inHighlightRange(value.g) && inHighlightRange(value.b);
                return allChannelsInRange ? vec3(1.0) : vec3(0.0);
            }

            vec3 applyHighlightInspection(vec3 value) {
                value = highlightMatrix * (value * highlightScale);

                if (highlightTransfer == 13) {
                    value = srgb(value);
                }

                if (highlightValueMode == 1) {
                    float y = dot(value, vec3(0.2126, 0.7152, 0.0722));
                    value = vec3(y);
                } else if (highlightValueMode == 2) {
                    value = vec3(value.g);
                }

                return max(value, vec3(0.0));
            }

            vec4 computeColor() {
                vec3 darkGray = vec3(0.5, 0.5, 0.5);
                vec3 lightGray = vec3(0.55, 0.55, 0.55);
                vec3 checker = abs(mod(floor(checkerUv.x) + floor(checkerUv.y), 2.0)) < 0.5 ? darkGray : lightGray;

                float cropAlpha = imageUv.x < cropMin.x || imageUv.x > cropMax.x || imageUv.y < cropMin.y || imageUv.y > cropMax.y ? 0.3 : 1.0;

                vec4 val = vec4(0.0);
                if (hasImage) {
                    val = sample(image, channelMask, imageUv);
                    val.a *= cropAlpha;
                }

                if (hasReference) {
                    vec4 referenceVal = sample(reference, channelMask, referenceUv);
                    referenceVal.a *= cropAlpha;

                    vec3 difference = val.rgb - referenceVal.rgb;
                    val = vec4(applyMetric(difference, referenceVal.rgb), (val.a + referenceVal.a) * 0.5);
                }

                val += bgColor * (1.0 - val.a);
                vec3 displaySource = displayUseInspection ? max(applyHighlightInspection(val.rgb), vec3(0.0)) : val.rgb;
                vec3 tonemapped = applyTonemap(colorMultiplier * applyExposureAndOffset(displaySource), vec4(checker, 1.0 - val.a));
                vec4 result = vec4(tonemapped, 1.0);
                if (tonemap == FALSE_COLOR) {
                    vec3 falseColorOnly = falseColor(falseColorCoord(falseColorScalar(colorMultiplier * applyExposureAndOffset(displaySource))));
                    result.rgb = mix(checker, falseColorOnly, val.a);
                }

                bool imageInBounds = imageUv.x >= 0.0 && imageUv.x <= 1.0 && imageUv.y >= 0.0 && imageUv.y <= 1.0;
                bool referenceInBounds = referenceUv.x >= 0.0 && referenceUv.x <= 1.0 && referenceUv.y >= 0.0 && referenceUv.y <= 1.0;
                if (highlightEnabled && hasImage && imageInBounds && (!hasReference || referenceInBounds)) {
                    if (useHighlightMaskTexture) {
                        vec4 maskValue = texture2D(highlightMaskImage, nearestUv(imageUv, imageResolution));
                        if (maskValue.a > 0.0) {
                            vec3 overlayColor = clamp(maskValue.rgb, 0.0, 1.0);
                            result.rgb = mix(result.rgb, overlayColor, clamp(maskValue.a, 0.0, 1.0));
                        }
                    } else {
                    vec4 highlightVal = sample(imageHighlight, channelMask, nearestUv(imageUv, imageResolution));
                    if (hasReference) {
                        vec4 referenceVal = sample(referenceHighlight, channelMask, nearestUv(referenceUv, referenceResolution));
                        highlightVal = vec4(applyMetric(highlightVal.rgb - referenceVal.rgb, referenceVal.rgb), (highlightVal.a + referenceVal.a) * 0.5);
                    }
                    highlightVal += bgColor * (1.0 - highlightVal.a);
                    vec3 highlightMask = computeHighlightMask(applyHighlightInspection(highlightVal.rgb));
                    if (highlightMask.r + highlightMask.g + highlightMask.b > 0.0) {
                        vec3 overlayColor = clamp(highlightMask * 0.92 + vec3(0.08), 0.0, 1.0);
                        result.rgb = mix(result.rgb, overlayColor, 0.82);
                    }
                    }
                }

                return result;
            }

            void main() {
                vec4 color = computeColor();
                color.rgb = clamp(color.rgb, clipToLdr ? 0.0 : -64.0, clipToLdr ? 1.0 : 64.0);
                gl_FragColor = dither(color);
            })glsl";
#elif defined(NANOGUI_USE_METAL)
        auto vertexShader =
            R"(using namespace metal;

            struct VertexOut {
                float4 position [[position]];
                float2 checkerUv;
                float2 ditherUv;
                float2 imageUv;
                float2 referenceUv;
            };

            vertex VertexOut vertex_main(
                const device packed_float2* position,
                const constant float2& pixelSize,
                const constant float2& checkerSize,
                const constant float& ditherSize,
                const constant float2& imageScale,
                const constant float2& imageOffset,
                const constant float2& referenceScale,
                const constant float2& referenceOffset,
                uint id [[vertex_id]]
            ) {
                VertexOut vert;
                vert.position = float4(position[id], 1.0f, 1.0f);

                // The offset of 0.25 is necessary to avoid sampling exact pixel borders. The offset is 0.25 rather than 0.5, because the
                // domain of the screen shader is [-1, 1] rather than [0, 1].
                vert.checkerUv = (position[id] / pixelSize + 0.25) / checkerSize;
                vert.ditherUv = (position[id] / pixelSize + 0.25) / ditherSize;

                vert.imageUv = position[id] * imageScale + imageOffset;
                vert.referenceUv = position[id] * referenceScale + referenceOffset;
                return vert;
            })";

        auto fragmentShader =
            R"(using namespace metal;

            #define SRGB        0
            #define GAMMA       1
            #define FALSE_COLOR 2
            #define POS_NEG     3
            #define REINHARD    4

            #define ERROR                   0
            #define ABSOLUTE_ERROR          1
            #define SQUARED_ERROR           2
            #define RELATIVE_ABSOLUTE_ERROR 3
            #define RELATIVE_SQUARED_ERROR  4

            #define CHANNEL_CONFIG_R    0
            #define CHANNEL_CONFIG_RG   1
            #define CHANNEL_CONFIG_RGB  2
            #define CHANNEL_CONFIG_RA   3
            #define CHANNEL_CONFIG_RGA  4
            #define CHANNEL_CONFIG_RGBA 5

            #define FALSE_COLOR_LINEAR 0

            float average(float3 col) {
                return (col.r + col.g + col.b) / 3.0f;
            }

            float falseColorScalar(float3 col) {
                return max(dot(max(col, float3(0.0f)), float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
            }

            float falseColorCoord(float value, int falseColorScaleMode, float2 falseColorRange) {
                const float minValue = max(falseColorRange.x, 0.0f);
                const float maxValue = max(falseColorRange.y, minValue + 1e-6f);

                if (falseColorScaleMode == FALSE_COLOR_LINEAR) {
                    return clamp((value - minValue) / (maxValue - minValue), 0.0f, 1.0f);
                }

                const float safeMin = max(minValue, 1e-6f);
                const float safeValue = max(value, safeMin);
                const float base = float(falseColorScaleMode + 1);
                const float invLogBase = 1.0f / log(base);
                const float minLog = log(safeMin) * invLogBase;
                const float maxLog = log(maxValue) * invLogBase;
                const float valueLog = log(safeValue) * invLogBase;
                return clamp((valueLog - minLog) / max(maxLog - minLog, 1e-6f), 0.0f, 1.0f);
            }

            float3 applyExposureAndOffset(float3 col, float exposure, float offset) {
                return pow(2.0f, exposure) * col + offset;
            }

            float3 falseColor(float v, texture2d<float, access::sample> colormap, sampler colormapSampler) {
                v = clamp(v, 0.0f, 1.0f);
                return colormap.sample(colormapSampler, float2(v, 0.5f)).rgb;
            }

            float3 linear(float3 val) {
                const float3 absVal = abs(val);
                return select(
                    copysign(pow((absVal + 0.055f) / 1.055f, 2.4f), val),
                    val / 12.92f,
                    absVal <= 0.04045f
                );
            }

            float3 srgb(float3 val) {
                const float3 absVal = abs(val);
                return select(
                    copysign(1.055f * pow(absVal, 0.41666f) - 0.055f, val),
                    12.92f * val,
                    absVal <= 0.0031308f
                );
            }

            float3 applyHighlightInspection(float3 value, float3x3 highlightMatrix, float highlightScale, int highlightTransfer, int highlightValueMode) {
                value = highlightMatrix * (value * highlightScale);

                if (highlightTransfer == 13) {
                    value = srgb(value);
                }

                if (highlightValueMode == 1) {
                    const float y = dot(value, float3(0.2126f, 0.7152f, 0.0722f));
                    value = float3(y);
                } else if (highlightValueMode == 2) {
                    value = float3(value.g);
                }

                return value;
            }

            float3 applyTonemap(
                float3 col,
                float4 background,
                int tonemap,
                float offset,
                float gamma,
                texture2d<float, access::sample> colormap,
                sampler colormapSampler,
                int falseColorScaleMode,
                float2 falseColorRange
            ) {
                switch (tonemap) {
                    case SRGB:
                        return srgb(col + (linear(background.rgb) - offset) * background.a);
                    case GAMMA:
                        col = col + (pow(background.rgb, float3(gamma)) - offset) * background.a;
                        return sign(col) * pow(abs(col), float3(1.0 / gamma));
                    case FALSE_COLOR:
                        return falseColor(falseColorCoord(falseColorScalar(col), falseColorScaleMode, falseColorRange), colormap, colormapSampler) + background.rgb * background.a;
                    case POS_NEG:
                        return float3(-average(min(col, float3(0.0f))) * 2.0f, average(max(col, float3(0.0f))) * 2.0f, 0.0f) + background.rgb * background.a;
                    case REINHARD: {
                        col = max(col + (linear(background.rgb) - offset) * background.a, float3(0.0f));
                        const float luminance = dot(col, float3(0.2126f, 0.7152f, 0.0722f));
                        return srgb(col / (1.0f + luminance));
                    }
                }

                return float3(0.0f);
            }

            float3 applyMetric(float3 col, float3 reference, int metric) {
                switch (metric) {
                    case ERROR:                   return col;
                    case ABSOLUTE_ERROR:          return abs(col);
                    case SQUARED_ERROR:           return col * col;
                    case RELATIVE_ABSOLUTE_ERROR: return abs(col) / (reference + float3(0.01f));
                    case RELATIVE_SQUARED_ERROR:  return col * col / (reference * reference + float3(0.01f));
                }

                return float3(0.0f);
            }

            float4 applyMask(float4 color, float4 mask) {
                color.rgb *= mask.rgb;
                color.a = mask.a == 1.0f ? color.a : 1.0f;
                return color;
            }

            float4 sample(texture2d<float, access::sample> texture, sampler textureSampler, float4 mask, float2 uv, int channelConfig) {
                if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
                    return float4(0.0f);
                }

                float4 color = texture.sample(textureSampler, uv);

                // Duplicate first channel in monochromatic images and move alpha to end if not already there.
                if (channelConfig == CHANNEL_CONFIG_R) {
                    color = float4(color.r, color.r, color.r, 1.0f);
                } else if (channelConfig == CHANNEL_CONFIG_RA) {
                    color = float4(color.r, color.r, color.r, color.g);
                } else if (channelConfig == CHANNEL_CONFIG_RGA) {
                    color = float4(color.r, color.g, 0.0f, color.b);
                }

                color = applyMask(color, mask);
                return color;
            }

            float2 nearestUv(float2 uv, float2 resolution) {
                return (floor(uv * resolution) + 0.5f) / resolution;
            }

            struct VertexOut {
                float4 position [[position]];
                float2 checkerUv;
                float2 ditherUv;
                float2 imageUv;
                float2 referenceUv;
            };

            float4 dither(float4 color, texture2d<float, access::sample> ditherMatrix, sampler ditherMatrixSampler, float2 ditherUv) {
                color.rgb += ditherMatrix.sample(ditherMatrixSampler, fract(ditherUv)).r;
                return color;
            }

            fragment float4 fragment_main(
                VertexOut vert [[stage_in]],
                texture2d<float, access::sample> image,
                sampler image_sampler,
                texture2d<float, access::sample> imageHighlight,
                sampler imageHighlight_sampler,
                texture2d<float, access::sample> highlightMaskImage,
                sampler highlightMaskImage_sampler,
                texture2d<float, access::sample> reference,
                sampler reference_sampler,
                texture2d<float, access::sample> referenceHighlight,
                sampler referenceHighlight_sampler,
                texture2d<float, access::sample> colormap,
                sampler colormap_sampler,
                texture2d<float, access::sample> ditherMatrix,
                sampler ditherMatrix_sampler,
                const constant bool& hasImage,
                const constant bool& useHighlightMaskTexture,
                const constant float2& imageResolution,
                const constant bool& hasReference,
                const constant float2& referenceResolution,
                const constant int& channelConfig,
                const constant float& exposure,
                const constant float& offset,
                const constant float& gamma,
                const constant float& colorMultiplier,
                const constant bool& clipToLdr,
                const constant int& tonemap,
                const constant int& falseColorScaleMode,
                const constant float2& falseColorRange,
                const constant int& metric,
                const constant float4& channelMask,
                const constant bool& highlightEnabled,
                const constant float2& highlightRange,
                const constant int& highlightMode,
                const constant float& highlightScale,
                const constant float3x3& highlightMatrix,
                const constant int& highlightTransfer,
                const constant int& highlightValueMode,
                const constant bool& displayUseInspection,
                const constant float2& cropMin,
                const constant float2& cropMax,
                const constant float4& bgColor
            ) {
                float3 darkGray = float3(0.5f, 0.5f, 0.5f);
                float3 lightGray = float3(0.55f, 0.55f, 0.55f);
                float3 checker = int(floor(vert.checkerUv.x) + floor(vert.checkerUv.y)) % 2 == 0 ? darkGray : lightGray;

                float cropAlpha = vert.imageUv.x < cropMin.x || vert.imageUv.x > cropMax.x || vert.imageUv.y < cropMin.y || vert.imageUv.y > cropMax.y ? 0.3f : 1.0f;

                float4 val = float4(0.0f);
                if (hasImage) {
                    val = sample(image, image_sampler, channelMask, vert.imageUv, channelConfig);
                    val.a *= cropAlpha;
                }

                if (hasReference) {
                    float4 referenceVal = sample(reference, reference_sampler, channelMask, vert.referenceUv, channelConfig);
                    referenceVal.a *= cropAlpha;

                    float3 difference = val.rgb - referenceVal.rgb;
                    val = float4(applyMetric(difference, referenceVal.rgb, metric), (val.a + referenceVal.a) * 0.5f);
                }

                val += bgColor * (1.0f - val.a);
                const float3 displaySource = displayUseInspection
                    ? max(applyHighlightInspection(val.rgb, highlightMatrix, highlightScale, highlightTransfer, highlightValueMode), float3(0.0f))
                    : val.rgb;
                float4 color = float4(
                    applyTonemap(
                        colorMultiplier * applyExposureAndOffset(displaySource, exposure, offset),
                        float4(checker, 1.0f - val.a),
                        tonemap,
                        offset,
                        gamma,
                        colormap,
                        colormap_sampler,
                        falseColorScaleMode,
                        falseColorRange
                    ),
                    1.0f
                );
                if (tonemap == FALSE_COLOR) {
                    const float3 falseColorOnly = falseColor(
                        falseColorCoord(falseColorScalar(colorMultiplier * applyExposureAndOffset(displaySource, exposure, offset)), falseColorScaleMode, falseColorRange),
                        colormap,
                        colormap_sampler
                    );
                    color.rgb = mix(checker, falseColorOnly, val.a);
                }
                const bool imageInBounds = vert.imageUv.x >= 0.0f && vert.imageUv.x <= 1.0f && vert.imageUv.y >= 0.0f && vert.imageUv.y <= 1.0f;
                const bool referenceInBounds =
                    vert.referenceUv.x >= 0.0f && vert.referenceUv.x <= 1.0f && vert.referenceUv.y >= 0.0f && vert.referenceUv.y <= 1.0f;
                if (highlightEnabled && hasImage && imageInBounds && (!hasReference || referenceInBounds)) {
                    if (useHighlightMaskTexture) {
                        float4 maskValue = highlightMaskImage.sample(highlightMaskImage_sampler, nearestUv(vert.imageUv, imageResolution));
                        if (maskValue.a > 0.0f) {
                            const float3 overlayColor = clamp(maskValue.rgb, 0.0f, 1.0f);
                            color.rgb = mix(color.rgb, overlayColor, clamp(maskValue.a, 0.0f, 1.0f));
                        }
                    } else {
                    float4 highlightVal = sample(imageHighlight, imageHighlight_sampler, channelMask, nearestUv(vert.imageUv, imageResolution), channelConfig);
                    if (hasReference) {
                        float4 referenceVal = sample(referenceHighlight, referenceHighlight_sampler, channelMask, nearestUv(vert.referenceUv, referenceResolution), channelConfig);
                        highlightVal = float4(applyMetric(highlightVal.rgb - referenceVal.rgb, referenceVal.rgb, metric), (highlightVal.a + referenceVal.a) * 0.5f);
                    }
                    highlightVal += bgColor * (1.0f - highlightVal.a);
                    float3 highlightSource = applyHighlightInspection(highlightVal.rgb, highlightMatrix, highlightScale, highlightTransfer, highlightValueMode);
                    float3 highlightMask = float3(0.0f);
                    if (highlightMode == 1) {
                        highlightMask = float3(highlightSource.r >= highlightRange.x && highlightSource.r <= highlightRange.y ? 1.0f : 0.0f, 0.0f, 0.0f);
                    } else if (highlightMode == 2) {
                        highlightMask = float3(0.0f, highlightSource.g >= highlightRange.x && highlightSource.g <= highlightRange.y ? 1.0f : 0.0f, 0.0f);
                    } else if (highlightMode == 3) {
                        highlightMask = float3(0.0f, 0.0f, highlightSource.b >= highlightRange.x && highlightSource.b <= highlightRange.y ? 1.0f : 0.0f);
                    } else if (highlightMode == 4) {
                        highlightMask = float3(highlightSource.r >= highlightRange.x && highlightSource.r <= highlightRange.y ? 1.0f : 0.0f);
                    } else {
                        const bool allChannelsInRange = highlightSource.r >= highlightRange.x && highlightSource.r <= highlightRange.y &&
                            highlightSource.g >= highlightRange.x && highlightSource.g <= highlightRange.y &&
                            highlightSource.b >= highlightRange.x && highlightSource.b <= highlightRange.y;
                        highlightMask = allChannelsInRange ? float3(1.0f) : float3(0.0f);
                    }
                    if (highlightMask.r + highlightMask.g + highlightMask.b > 0.0f) {
                        const float3 overlayColor = clamp(highlightMask * 0.92f + float3(0.08f), 0.0f, 1.0f);
                        color.rgb = mix(color.rgb, overlayColor, 0.82f);
                    }
                    }
                }
                color.rgb = clamp(color.rgb, clipToLdr ? 0.0f : -64.0f, clipToLdr ? 1.0f : 64.0f);
                return dither(color, ditherMatrix, ditherMatrix_sampler, vert.ditherUv);
            })";
#endif

        mShader = new Shader{renderPass, "ubershader", vertexShader, fragmentShader};
    } catch (const runtime_error& e) { tlog::error("Unable to compile shader: {}", e.what()); }

    // 2 Triangles
    uint32_t indices[3 * 2] = {
        0,
        1,
        2,
        2,
        3,
        0,
    };
    float positions[2 * 4] = {
        -1.f,
        -1.f,
        1.f,
        -1.f,
        1.f,
        1.f,
        -1.f,
        1.f,
    };

    mShader->set_buffer("indices", VariableType::UInt32, {3 * 2}, indices);
    mShader->set_buffer("position", VariableType::Float32, {4, 2}, positions);

    const auto createColorMap = [](span<const float> data) {
        if (data.size() % 4 != 0) {
            throw runtime_error{format("Invalid false color data size. Expected multiple of 4, got {}.", data.size())};
        }

        auto texture = nanogui::ref<Texture>{
            new Texture{Texture::PixelFormat::RGBA, Texture::ComponentFormat::Float32, Vector2i{(int)data.size() / 4, 1}}
        };
        texture->upload(reinterpret_cast<const uint8_t*>(data.data()));
        return texture;
    };

    mColorMaps[0] = createColorMap(colormap::turbo());
    mColorMaps[1] = createColorMap(colormap::viridis());

    mDitherMatrix = new Texture{
        Texture::PixelFormat::R,
        Texture::ComponentFormat::Float32,
        Vector2i{DITHER_MATRIX_SIZE},
        Texture::InterpolationMode::Nearest,
        Texture::InterpolationMode::Nearest,
        Texture::WrapMode::Repeat,
    };

    const auto dmat = ditherMatrix(ditherScale);
    if (dmat.size() != posProd(mDitherMatrix->size())) {
        throw runtime_error{format("Invalid dither matrix size. Expected {}, got {}.", posProd(mDitherMatrix->size()), dmat.size())};
    }

    mDitherMatrix->upload(reinterpret_cast<const uint8_t*>(dmat.data()));
}

UberShader::~UberShader() {}

void UberShader::draw(
    Vector2f pixelSize,
    Vector2f checkerSize,
    Image* image,
    const Matrix3f& transformImage,
    Image* reference,
    const Matrix3f& transformReference,
    string_view requestedChannelGroup,
    EInterpolationMode minFilter,
    EInterpolationMode magFilter,
    float exposure,
    float offset,
    float gamma,
    float colorMultiplier,
    bool clipToLdr,
    Color backgroundColor,
    ETonemap tonemap,
    int falseColorMap,
    int falseColorScaleMode,
    Vector2f falseColorRange,
    EMetric metric,
    EChannelMask channelMask,
    const optional<Vector2f>& highlightRange,
    int highlightMode,
    float highlightScale,
    const Matrix3f& highlightMatrix,
    int highlightTransfer,
    int highlightValueMode,
    bool displayUseInspection,
    const optional<Box2i>& crop,
    Texture* inspectionImageTexture,
    Texture* inspectionReferenceTexture,
    int inspectionTextureColorChannels,
    Texture* highlightMaskTexture
) {
    // We're passing the channels found in `mImage` such that, if some channels don't exist in `mReference`, they're filled with default
    // values (0 for colors, 1 for alpha).
    const auto channels = image ? image->channelsInGroup(requestedChannelGroup) : span<const string>{};
    Texture* const textureImage =
        inspectionImageTexture ? inspectionImageTexture : (image ? image->sourceTexture(channels, minFilter, magFilter) : nullptr);
    Texture* const textureImageHighlight =
        inspectionImageTexture ? inspectionImageTexture : (image ? image->sourceTexture(channels, EInterpolationMode::Nearest, EInterpolationMode::Nearest) : nullptr);
    Texture* const textureReference =
        inspectionReferenceTexture ? inspectionReferenceTexture : (reference ? reference->sourceTexture(channels, minFilter, magFilter) : nullptr);
    Texture* const textureReferenceHighlight = inspectionReferenceTexture ?
        inspectionReferenceTexture :
        (reference ? reference->sourceTexture(channels, EInterpolationMode::Nearest, EInterpolationMode::Nearest) : nullptr);

    const bool usingInspectionTextures = inspectionTextureColorChannels > 0;
    const bool hasAlpha =
        !usingInspectionTextures && channels.size() > 1 && Channel::isAlpha(channels.back()); // Only count A as alpha if it isn't the only channel.
    const int numColorChannels = usingInspectionTextures ? inspectionTextureColorChannels : (int)channels.size() - (hasAlpha ? 1 : 0);

    EShaderChannelConfig channelConfig = EShaderChannelConfig::RGBA;
    switch (numColorChannels) {
        case 0: break; // Just rendering the checkerboard background. Value doesn't matter.
        case 1: channelConfig = hasAlpha ? EShaderChannelConfig::RA : EShaderChannelConfig::R; break;
        case 2: channelConfig = hasAlpha ? EShaderChannelConfig::RGA : EShaderChannelConfig::RG; break;
        case 3: channelConfig = hasAlpha ? EShaderChannelConfig::RGBA : EShaderChannelConfig::RGB; break;
        default: throw runtime_error{"Invalid number of color channels."};
    }

    auto* fallbackColorMap = mColorMaps[0].get();
    bindCheckerboardData(pixelSize, checkerSize, backgroundColor);
    bindImageData(
        textureImage ? textureImage : fallbackColorMap,
        transformImage,
        exposure,
        offset,
        gamma,
        tonemap,
        falseColorMap,
        falseColorScaleMode,
        falseColorRange
    );
    bindReferenceData(textureReference ? textureReference : fallbackColorMap, transformReference, metric);

    const Vector4f uChannelMask = (displayUseInspection || usingInspectionTextures)
        ? Vector4f{1.0f, 1.0f, 1.0f, 1.0f}
        : Vector4f{
              hasFlag(channelMask, EChannelMask::Red) ? 1.0f : 0.0f,
              hasFlag(channelMask, EChannelMask::Green) ? 1.0f : 0.0f,
              hasFlag(channelMask, EChannelMask::Blue) ? 1.0f : 0.0f,
              hasFlag(channelMask, EChannelMask::Alpha) ? 1.0f : 0.0f,
          };
    mShader->set_uniform("channelMask", uChannelMask);
    mShader->set_uniform("highlightEnabled", highlightRange.has_value() || (bool)highlightMaskTexture);
    if (highlightRange.has_value()) {
        mShader->set_uniform("highlightRange", *highlightRange);
    } else {
        mShader->set_uniform("highlightRange", Vector2f{0.0f, 0.0f});
    }
    mShader->set_uniform("highlightMode", highlightMode);
    mShader->set_uniform("highlightScale", highlightScale);
    mShader->set_uniform("highlightMatrix", highlightMatrix);
    mShader->set_uniform("highlightTransfer", highlightTransfer);
    mShader->set_uniform("highlightValueMode", highlightValueMode);
    mShader->set_uniform("displayUseInspection", displayUseInspection);

    mShader->set_uniform("hasImage", (bool)textureImage);
    mShader->set_uniform("useHighlightMaskTexture", (bool)highlightMaskTexture);
    mShader->set_uniform("hasReference", (bool)textureReference);
    mShader->set_uniform("imageResolution", textureImage ? Vector2f{textureImage->size()} : Vector2f{1.0f, 1.0f});
    mShader->set_uniform("referenceResolution", textureReference ? Vector2f{textureReference->size()} : Vector2f{1.0f, 1.0f});
    mShader->set_texture("imageHighlight", textureImageHighlight ? textureImageHighlight : (textureImage ? textureImage : fallbackColorMap));
    mShader->set_texture("highlightMaskImage", highlightMaskTexture ? highlightMaskTexture : fallbackColorMap);
    mShader->set_texture(
        "referenceHighlight",
        textureReferenceHighlight ? textureReferenceHighlight : (textureReference ? textureReference : fallbackColorMap)
    );

    mShader->set_uniform("channelConfig", (int)channelConfig);

    mShader->set_uniform("colorMultiplier", colorMultiplier);
    mShader->set_uniform("clipToLdr", clipToLdr);

    if (crop.has_value() && textureImage) {
        mShader->set_uniform("cropMin", Vector2f{crop->min} / Vector2f{textureImage->size()});
        mShader->set_uniform("cropMax", Vector2f{crop->max} / Vector2f{textureImage->size()});
    } else {
        mShader->set_uniform("cropMin", Vector2f{-numeric_limits<float>::infinity()});
        mShader->set_uniform("cropMax", Vector2f{numeric_limits<float>::infinity()});
    }

    mShader->set_uniform("ditherSize", static_cast<float>(DITHER_MATRIX_SIZE));
    mShader->set_texture("ditherMatrix", mDitherMatrix);

    mShader->begin();
    mShader->draw_array(Shader::PrimitiveType::Triangle, 0, 6, true);
    mShader->end();
}

void UberShader::bindCheckerboardData(Vector2f pixelSize, Vector2f checkerSize, Color backgroundColor) {
    mShader->set_uniform("pixelSize", pixelSize);
    mShader->set_uniform("checkerSize", checkerSize);
    mShader->set_uniform("bgColor", backgroundColor);
}

void UberShader::bindImageData(
    Texture* textureImage,
    const Matrix3f& transformImage,
    float exposure,
    float offset,
    float gamma,
    ETonemap tonemap,
    int falseColorMap,
    int falseColorScaleMode,
    Vector2f falseColorRange
) {
    mShader->set_texture("image", textureImage);

    mShader->set_uniform("imageScale", Vector2f{transformImage.m[0][0], transformImage.m[1][1]});
    mShader->set_uniform("imageOffset", Vector2f{transformImage.m[2][0], transformImage.m[2][1]});

    mShader->set_uniform("exposure", exposure);
    mShader->set_uniform("offset", offset);
    mShader->set_uniform("gamma", gamma);
    mShader->set_uniform("tonemap", static_cast<int>(tonemap));
    mShader->set_texture("colormap", mColorMaps[(size_t)clamp(falseColorMap, 0, 1)].get());
    mShader->set_uniform("falseColorScaleMode", falseColorScaleMode);
    mShader->set_uniform("falseColorRange", falseColorRange);
}

void UberShader::bindReferenceData(Texture* textureReference, const Matrix3f& transformReference, EMetric metric) {
    mShader->set_texture("reference", textureReference);

    mShader->set_uniform("referenceScale", Vector2f{transformReference.m[0][0], transformReference.m[1][1]});
    mShader->set_uniform("referenceOffset", Vector2f{transformReference.m[2][0], transformReference.m[2][1]});

    mShader->set_uniform("metric", static_cast<int>(metric));
}

} // namespace tev
