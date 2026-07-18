const char *rtgiGbuf_frag_src =
"FSIN vec3 v_worldpos;\n"
"FSIN float v_depth;\n"

"layout(location = 0) out vec4 out_normal;\n"
"layout(location = 1) out vec4 out_depth;\n"

"void\n"
"main(void)\n"
"{\n"
"	// face normal from position derivatives: most VC world geometry is\n"
"	// prelit and carries no vertex normals at all\n"
"	vec3 n = normalize(cross(dFdx(v_worldpos), dFdy(v_worldpos)));\n"
"	out_normal = vec4(n, 0.0);\n"
"	out_depth = vec4(v_depth, 0.0, 0.0, 0.0);\n"
"}\n"
;
