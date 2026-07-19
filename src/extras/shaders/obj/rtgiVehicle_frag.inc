const char *rtgiVehicle_frag_src =
"uniform sampler2D tex0;\n"
"uniform sampler2D u_aoTex;\n"
"uniform sampler2D u_giTex;\n"
"uniform sampler2D u_reflTex;\n"

"uniform vec4 u_rtgiReflParams;	// x = reflections enabled\n"

"uniform vec4 u_rtgiParams;	// x = ao strength, y = 1/width, z = 1/height, w = sun shadow strength\n"
"uniform vec4 u_rtgiGIParams;	// x = gi blend, y = gi exposure, z/w unused\n"
"uniform vec4 u_rtgiVehParams;	// x = env-map replacement scale for this material\n"

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

"	// ambient: blend the flat timecycle term toward ray traced GI, same as\n"
"	// the world composite; vehicles are in the G-buffer so the signals are\n"
"	// their own surface's\n"
"	vec3 gi = texture(u_giTex, uv).rgb * u_rtgiGIParams.y;\n"
"	vec3 ambient = mix(v_amb*ao, gi * max(v_amb, vec3(0.02))*3.0, u_rtgiGIParams.x);\n"

"	vec4 light = v_color + vec4(ambient, 0.0);\n"
"	vec4 color = t0 * light;\n"
"	color.a = t0.a * v_color.a;\n"

"	// ray traced sun shadow: vehicles receive shade from buildings etc.\n"
"	color.rgb *= mix(1.0 - u_rtgiParams.w, 1.0, rt.g);\n"

"	// ray traced reflections replace the matFX env-map pass on materials\n"
"	// that carried one (car paint, chrome)\n"
"	if(u_rtgiReflParams.x > 0.5 && u_rtgiVehParams.x > 0.0){\n"
"		vec4 refl = texture(u_reflTex, uv);\n"
"		color.rgb = mix(color.rgb, refl.rgb, refl.a * u_rtgiVehParams.x);\n"
"	}\n"

"	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);\n"
"	DoAlphaTest(color.a);\n"

"	FRAGCOLOR(color);\n"
"}\n"
;
