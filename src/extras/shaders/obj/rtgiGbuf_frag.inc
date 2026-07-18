const char *rtgiGbuf_frag_src =
"FSIN vec3 v_normal;\n"
"FSIN float v_depth;\n"

"layout(location = 0) out vec4 out_normal;\n"
"layout(location = 1) out vec4 out_depth;\n"

"void\n"
"main(void)\n"
"{\n"
"	out_normal = vec4(normalize(v_normal), 0.0);\n"
"	out_depth = vec4(v_depth, 0.0, 0.0, 0.0);\n"
"}\n"
;
