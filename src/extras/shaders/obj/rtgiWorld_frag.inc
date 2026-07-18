const char *rtgiWorld_frag_src =
"uniform sampler2D tex0;\n"
"uniform sampler2D u_aoTex;\n"

"uniform vec4 u_rtgiParams;	// x = ao strength, y = 1/width, z = 1/height\n"

"FSIN vec4 v_color;\n"
"FSIN vec3 v_amb;\n"
"FSIN vec2 v_tex0;\n"
"FSIN float v_fog;\n"

"void\n"
"main(void)\n"
"{\n"
"	vec4 t0 = texture(tex0, vec2(v_tex0.x, 1.0-v_tex0.y));\n"

"	float ao = texture(u_aoTex, gl_FragCoord.xy * u_rtgiParams.yz).r;\n"
"	ao = mix(1.0, ao, u_rtgiParams.x);\n"

"	vec4 light = v_color + vec4(v_amb*ao, 0.0);\n"
"	vec4 color = t0 * light;\n"
"	color.a = t0.a * v_color.a;\n"

"	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);\n"
"	DoAlphaTest(color.a);\n"

"	FRAGCOLOR(color);\n"
"}\n"
;
