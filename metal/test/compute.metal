// mtlcompute's kernel: out[i] = in[i] * 3 + i * k, as vkcompute's GLSL does (compared word for word).
#include <metal_stdlib>
using namespace metal;

struct Params {
	uint k;
	uint count;
};

kernel void scale(device const uint *src [[buffer(0)]], device uint *dst [[buffer(1)]],
                  constant Params &p [[buffer(2)]], uint i [[thread_position_in_grid]])
{
	if (i < p.count)
		dst[i] = src[i] * 3u + i * p.k;
}
