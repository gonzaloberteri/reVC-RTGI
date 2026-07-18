VSIN(ATTRIB_POS)	vec3 in_pos;

VSOUT vec3 v_worldpos;
VSOUT float v_depth;

void
main(void)
{
	vec4 Vertex = u_world * vec4(in_pos, 1.0);
	gl_Position = u_proj * u_view * Vertex;
	v_worldpos = Vertex.xyz;
	v_depth = gl_Position.w;
}
