const char *rtgiGbufSkin_vert_src =
"uniform mat4 u_boneMatrices[64];\n"

"VSIN(ATTRIB_POS)	vec3 in_pos;\n"

"VSOUT vec3 v_worldpos;\n"
"VSOUT float v_depth;\n"

"void\n"
"main(void)\n"
"{\n"
"	vec3 SkinVertex = vec3(0.0, 0.0, 0.0);\n"
"	for(int i = 0; i < 4; i++)\n"
"		SkinVertex += (u_boneMatrices[int(in_indices[i])] * vec4(in_pos, 1.0)).xyz * in_weights[i];\n"

"	vec4 Vertex = u_world * vec4(SkinVertex, 1.0);\n"
"	gl_Position = u_proj * u_view * Vertex;\n"
"	v_worldpos = Vertex.xyz;\n"
"	v_depth = gl_Position.w;\n"
"}\n"
;
