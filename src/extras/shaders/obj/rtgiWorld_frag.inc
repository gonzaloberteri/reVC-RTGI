const char *rtgiWorld_frag_src =
"uniform sampler2D tex0;\n"
"uniform sampler2D u_aoTex;\n"
"uniform sampler2D u_giTex;\n"

"uniform vec4 u_rtgiParams;	// x = ao strength, y = 1/width, z = 1/height, w = sun shadow strength\n"
"uniform vec4 u_rtgiGIParams;	// x = gi blend, y = gi exposure, z/w unused\n"

"FSIN vec4 v_color;\n"
"FSIN vec3 v_amb;\n"
"FSIN vec2 v_tex0;\n"
"FSIN float v_fog;\n"

"void\n"
"main(void)\n"
"{\n"
"	vec4 t0 = texture(tex0, vec2(v_tex0.x, 1.0-v_tex0.y));\n"

"	vec2 uv = gl_FragCoord.xy * u_rtgiParams.yz;\n"
"	vec2 rt = texture(u_aoTex, uv).rg;\n"
"	float ao = mix(1.0, rt.r, u_rtgiParams.x);\n"

"	// ambient: blend the flat timecycle term toward ray traced GI. The GI\n"
"	// signal carries its own occlusion, so AO only applies to the flat part.\n"
"	vec3 gi = texture(u_giTex, uv).rgb * u_rtgiGIParams.y;\n"
"	vec3 ambient = mix(v_amb*ao, gi * max(v_amb, vec3(0.02))*3.0, u_rtgiGIParams.x);\n"

"	vec4 light = v_color + vec4(ambient, 0.0);\n"
"	vec4 color = t0 * light;\n"
"	color.a = t0.a * v_color.a;\n"

"	// ray traced shadow (vehicles only for now; replaces the blob shadows)\n"
"	color.rgb *= mix(1.0 - u_rtgiParams.w, 1.0, rt.g);\n"

"	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);\n"
"	DoAlphaTest(color.a);\n"

"	FRAGCOLOR(color);\n"
"}\n"
;
