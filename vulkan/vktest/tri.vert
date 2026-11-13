#version 450
// vkdraw: one triangle from gl_VertexIndex (no vertex buffer), red/green/blue corners.
layout(location = 0) out vec3 color;

const vec2 pos[3] = vec2[](vec2(0.0, -0.8), vec2(0.8, 0.8), vec2(-0.8, 0.8));
const vec3 col[3] = vec3[](vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1));

void main()
{
	gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
	color = col[gl_VertexIndex];
}
