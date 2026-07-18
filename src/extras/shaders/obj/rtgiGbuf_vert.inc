const char *rtgiGbuf_vert_src =
"VSIN(ATTRIB_POS)	vec3 in_pos;\n"

"VSOUT vec3 v_normal;\n"
"VSOUT float v_depth;\n"

"void\n"
"main(void)\n"
"{\n"
"	vec4 Vertex = u_world * vec4(in_pos, 1.0);\n"
"	gl_Position = u_proj * u_view * Vertex;\n"
"	v_normal = mat3(u_world) * in_normal;\n"
"	v_depth = gl_Position.w;\n"
"}\n"
;
