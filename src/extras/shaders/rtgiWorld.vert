VSIN(ATTRIB_POS)	vec3 in_pos;

VSOUT vec4 v_color;	// prelight + dynamic lights, x material color
VSOUT vec3 v_amb;	// ambient term, x material color; AO-modulated per pixel
VSOUT vec2 v_tex0;
VSOUT float v_fog;

void
main(void)
{
	vec4 Vertex = u_world * vec4(in_pos, 1.0);
	gl_Position = u_proj * u_view * Vertex;
	vec3 Normal = mat3(u_world) * in_normal;

	v_tex0 = in_tex0;

	// same math as librw's default.vert but with the ambient term kept
	// separate so the fragment shader can modulate it with ray traced AO
	vec4 c = in_color;
	c.rgb += DoDynamicLight(Vertex.xyz, Normal)*surfDiffuse;
	c = clamp(c, 0.0, 1.0);
	v_color = c * u_matColor;
	v_amb = clamp(u_ambLight.rgb*surfAmbient, 0.0, 1.0) * u_matColor.rgb;

	v_fog = DoFog(gl_Position.w);
}
