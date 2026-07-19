uniform mat4 u_boneMatrices[64];

VSIN(ATTRIB_POS)	vec3 in_pos;

VSOUT vec3 v_worldpos;
VSOUT float v_depth;

void
main(void)
{
	vec3 SkinVertex = vec3(0.0, 0.0, 0.0);
	for(int i = 0; i < 4; i++)
		SkinVertex += (u_boneMatrices[int(in_indices[i])] * vec4(in_pos, 1.0)).xyz * in_weights[i];

	vec4 Vertex = u_world * vec4(SkinVertex, 1.0);
	gl_Position = u_proj * u_view * Vertex;
	v_worldpos = Vertex.xyz;
	v_depth = gl_Position.w;
}
