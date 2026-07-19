const char *rtgiSkin_vert_src =
"uniform mat4 u_boneMatrices[64];\n"

"VSIN(ATTRIB_POS)	vec3 in_pos;\n"

"VSOUT vec4 v_color;	// prelight + dynamic lights, x material color\n"
"VSOUT vec3 v_amb;	// ambient term, x material color; AO-modulated per pixel\n"
"VSOUT vec2 v_tex0;\n"
"VSOUT float v_fog;\n"

"void\n"
"main(void)\n"
"{\n"
"	vec3 SkinVertex = vec3(0.0, 0.0, 0.0);\n"
"	vec3 SkinNormal = vec3(0.0, 0.0, 0.0);\n"
"	for(int i = 0; i < 4; i++){\n"
"		SkinVertex += (u_boneMatrices[int(in_indices[i])] * vec4(in_pos, 1.0)).xyz * in_weights[i];\n"
"		SkinNormal += (mat3(u_boneMatrices[int(in_indices[i])]) * in_normal) * in_weights[i];\n"
"	}\n"

"	vec4 Vertex = u_world * vec4(SkinVertex, 1.0);\n"
"	gl_Position = u_proj * u_view * Vertex;\n"
"	vec3 Normal = mat3(u_world) * SkinNormal;\n"

"	v_tex0 = in_tex0;\n"

"	// same math as librw's skin.vert but with the ambient term kept\n"
"	// separate so the fragment shader can modulate it with ray traced AO\n"
"	vec4 c = in_color;\n"
"	c.rgb += DoDynamicLight(Vertex.xyz, Normal)*surfDiffuse;\n"
"	c = clamp(c, 0.0, 1.0);\n"
"	v_color = c * u_matColor;\n"
"	v_amb = clamp(u_ambLight.rgb*surfAmbient, 0.0, 1.0) * u_matColor.rgb;\n"

"	v_fog = DoFog(gl_Position.z);\n"
"}\n"
;
