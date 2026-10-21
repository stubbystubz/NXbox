# SPDX-License-Identifier: GPL-3.0-or-later
"""Mechanical host-only MSL adapter for production HLSL math (no SDK binaries)."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PRESENT = ROOT / "src/video_core/renderer_d3d12/d3d12_present"


def function(text, name):
    start = text.index(name + "(")
    start = text.rfind("\n", 0, start) + 1
    brace = text.rfind("}", start, text.index(name + "("))
    if brace >= 0:
        start = brace + 1
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def adapt(text):
    text = re.sub(r"\[numthreads\([^]]*\)\]|\[unroll\]|\[loop\]", "", text)
    text = text.replace(" : SV_DispatchThreadID", "")
    text = text.replace("Texture2D<float4>", "texture2d<float>")
    text = text.replace("lerp(", "mix(").replace("frac(", "fract(")
    while match := re.search(r"(\w+)\.SampleLevel\(", text):
        start = match.end()
        end = start
        depth = 1
        while depth:
            depth += (text[end] == "(") - (text[end] == ")")
            end += 1
        arguments = text[start : end - 1]
        uv, mip = arguments[len("linear_sampler,") :].rsplit(",", 1)
        replacement = f"{match[1]}.sample(linear_sampler,{uv}, level({mip}))"
        text = text[: match.start()] + replacement + text[end:]
    text = re.sub(r"(\w+)\.Load\(", r"Read(\1, ", text)
    text = text.replace(
        "motion.GetDimensions(mw, mh);", "mw = motion.get_width(); mh = motion.get_height();"
    )
    text = re.sub(
        r"(accumulated|motion_out|generated|luma_out)\[id.xy\] = (.*?);",
        r"\1.write(\2, id.xy);",
        text,
        flags=re.S,
    )
    return text


def build():
    common = """#include <metal_stdlib>
using namespace metal;
float4 Read(texture2d<float> image, int3 p) { return image.read(uint2(p.xy)); }
"""
    temporal = (PRESENT / "temporal_upscaler.hlsl").read_text()
    temporal = temporal[temporal.index("float3 ToYCoCg") :]
    temporal = adapt(temporal)
    fields = """texture2d<float> current_frame, history_frame, motion, previous_frame, spatial_frame;
texture2d<float, access::write> accumulated;
sampler linear_sampler;
uint2 source_size, output_size;
uint history_valid;
"""
    result = common + "struct Temporal {\n" + fields + temporal + "\n};\n"
    result += """kernel void temporal(texture2d<float> current [[texture(0)]],
texture2d<float> history [[texture(1)]], texture2d<float> motion [[texture(2)]],
texture2d<float> previous [[texture(3)]], texture2d<float> spatial [[texture(4)]],
texture2d<float, access::write> output [[texture(5)]],
constant uint& valid [[buffer(0)]], uint3 id [[thread_position_in_grid]]) {
constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
Temporal t{current, history, motion, previous, spatial, output, s,
uint2(current.get_width(), current.get_height()), uint2(output.get_width(), output.get_height()), valid};
t.TemporalCS(id);
}
"""
    fg = (PRESENT / "frame_generation.hlsl").read_text()
    flow = "\n".join(function(fg, n) for n in ("ReadLuma", "Cost", "Search", "MotionCS"))
    flow = adapt(flow).replace(
        "return Read(luma, int3(clamp(p, int2(0, 0), int2(blocks) - 1), 0));",
        "return Read(luma, int3(clamp(p, int2(0, 0), int2(blocks) - 1), 0)).xy;",
    )
    result += (
        "struct Flow { texture2d<float> luma; texture2d<float, access::write> motion_out; uint2 blocks;\n"
        + flow
        + "\n};\n"
    )
    result += """kernel void motion(texture2d<float> luma [[texture(0)]],
texture2d<float, access::write> output [[texture(1)]], uint3 id [[thread_position_in_grid]]) {
Flow f{luma, output, uint2(output.get_width(), output.get_height())}; f.MotionCS(id);
}
"""
    # Extract the exact four-tap prefilter from LumaCS; atomics/readback have no
    # effect on the flow or image and are excluded from this host cost proxy.
    luma = function(fg, "LumaCS")
    luma = luma[luma.index("    float2 value") : luma.index("    luma_difference[lane]")]
    luma = adapt(luma).replace(
        "luma_out.write(value, id.xy);", "luma_out.write(float4(value, 0, 0), id.xy);"
    )
    # Metal disallows nested functions, so insert Luma at file scope.
    result += function(fg, "Luma") + "\n"
    result += (
        """kernel void luma(texture2d<float> previous_frame [[texture(0)]],
texture2d<float> current_frame [[texture(1)]], texture2d<float, access::write> luma_out [[texture(2)]],
uint3 id [[thread_position_in_grid]]) {
constexpr sampler linear_sampler(coord::normalized, address::clamp_to_edge, filter::linear);
uint2 size(current_frame.get_width(), current_frame.get_height()); uint2 blocks(luma_out.get_width(), luma_out.get_height());
"""
        + luma
        + "\n}\n"
    )
    header = (ROOT / "externals/FidelityFX-FSR/ffx-fsr/ffx_fsr1.h").read_text()
    fsr = "\n".join(
        function(header, n)
        for n in ("FsrEasuCon", "FsrEasuTapF", "FsrEasuSetF", "FsrEasuF", "FsrRcasF")
    )
    fsr = re.sub(r"\binout (AF[1234])\b", r"thread \1&", fsr)
    fsr = re.sub(r"\bout (AF[1234])\b", r"thread \1&", fsr)
    fsr = fsr.replace("outAU4", "thread AU4&").replace("A_STATIC ", "")
    macros = """
#define A_GPU 1
#define FSR_RCAS_PASSTHROUGH_ALPHA 1
#define FSR_RCAS_LIMIT (0.25-(1.0/16.0))
#define AF1 float
#define AF2 float2
#define AF3 float3
#define AF4 float4
#define AU1 uint
#define AU2 uint2
#define AU4 uint4
#define ASU2 int2
#define AP1 bool
#define AF1_(x) float(x)
#define AF2_(x) float2(x)
#define AF3_(x) float3(x)
#define AF4_(x) float4(x)
#define AU1_(x) uint(x)
#define AU1_AF1(x) as_type<uint>(x)
#define AF1_AU1(x) as_type<float>(x)
#define AF2_AU2(x) as_type<float2>(x)
#define ARcpF1(x) (1.f/(x))
#define AMin3F1(a,b,c) min(a,min(b,c))
#define AMax3F1(a,b,c) max(a,max(b,c))
#define AMin3F3(a,b,c) min(a,min(b,c))
#define AMax3F3(a,b,c) max(a,max(b,c))
#define ASatF1(x) saturate(x)
float APrxLoRcpF1(float a) { return as_type<float>(0x7ef07ebbu-as_type<uint>(a)); }
float APrxMedRcpF1(float a) { float b=as_type<float>(0x7ef19fffu-as_type<uint>(a)); return b*(-b*a+2.f); }
float APrxLoRsqF1(float a) { return as_type<float>(0x5f347d74u-(as_type<uint>(a)>>1u)); }
"""
    callbacks = """
texture2d<float> source;
sampler s;
float4 FsrEasuRF(float2 p) { return source.gather(s,p,component::x); }
float4 FsrEasuGF(float2 p) { return source.gather(s,p,component::y); }
float4 FsrEasuBF(float2 p) { return source.gather(s,p,component::z); }
float4 FsrRcasLoadF(int2 p) { return source.read(uint2(clamp(p,int2(0),int2(source.get_width(),source.get_height())-1))); }
void FsrRcasInputF(thread float& r, thread float& g, thread float& b) {}
"""
    result += macros + "\nstruct Fsr {\n" + callbacks + fsr + "\n};\n"
    result += """kernel void easu(texture2d<float> source [[texture(0)]],
texture2d<float,access::write> output [[texture(1)]], uint2 id [[thread_position_in_grid]]) {
if(any(id>=uint2(output.get_width(),output.get_height()))) return;
constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
Fsr f{source,s}; uint4 a,b,c,d; float3 rgb;
f.FsrEasuCon(a,b,c,d, float(source.get_width()),float(source.get_height()),float(source.get_width()),float(source.get_height()),float(output.get_width()),float(output.get_height()));
f.FsrEasuF(rgb,id,a,b,c,d); output.write(float4(rgb,1),id);
}
kernel void rcas(texture2d<float> source [[texture(0)]],
texture2d<float,access::write> output [[texture(1)]], uint2 id [[thread_position_in_grid]]) {
if(any(id>=uint2(output.get_width(),output.get_height()))) return;
constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
Fsr f{source,s}; float r,g,b,a; uint4 c(as_type<uint>(exp2(-.5f)),0,0,0);
f.FsrRcasF(r,g,b,a,id,c); output.write(float4(r,g,b,a),id);
}
"""
    return result


if __name__ == "__main__":
    import sys

    Path(sys.argv[1]).write_text(build())
