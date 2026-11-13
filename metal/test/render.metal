// Shaders of mtlrender.m (Metal milestone 3): a vertex-descriptor triangle, a textured quad (the
// workload of vulkan/bench, bench.vert/bench.frag in Metal) and a flat-colour quad at a depth.
#include <metal_stdlib>
using namespace metal;

struct ColoredIn {
	float2 position [[attribute(0)]];
	float4 color [[attribute(1)]];
};

struct Out {
	float4 position [[position]];
	float4 color;
	float2 uv;
};

vertex Out colored_vertex(ColoredIn in [[stage_in]])
{
	Out o;
	o.position = float4(in.position, 0.0, 1.0);
	o.color = in.color;
	o.uv = float2(0.0);
	return o;
}

fragment float4 colored_fragment(Out in [[stage_in]])
{
	return in.color;
}

// rect: x, y, w, h in clip space (bench.h's bench_quad); z: the depth
struct Quad {
	float4 rect;
	float z;
};

vertex Out quad_vertex(uint vid [[vertex_id]], constant Quad &q [[buffer(0)]])
{
	float2 c = float2(float(vid & 1), float(vid >> 1));
	Out o;
	o.position = float4(q.rect.xy + c * q.rect.zw, q.z, 1.0);
	o.color = float4(1.0);
	o.uv = c;
	return o;
}

fragment float4 quad_fragment(Out in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]])
{
	return tex.sample(s, in.uv);
}

fragment float4 flat_fragment(Out in [[stage_in]], constant float4 &color [[buffer(0)]])
{
	return color;
}
