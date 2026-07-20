const char *rtgiGlassMirror_frag_src =
"uniform sampler2D u_reflTex;\n"

"uniform vec4 u_rtgiParams;	// y = 1/width, z = 1/height\n"

"FSIN vec4 v_color;\n"
"FSIN vec2 v_tex0;\n"
"FSIN float v_fog;\n"
"FSIN vec3 v_worldpos;\n"

"void\n"
"main(void)\n"
"{\n"
"	// breakable (code-)glass panes: the vanilla pane visual is a sliding\n"
"	// fake-reflection quad — replace it with the ray traced mirror the\n"
"	// reflection pass computed at these pixels (the G-buffer prepass\n"
"	// marked the pane as glass). refl.a carries the Fresnel strength;\n"
"	// the vertex alpha keeps the vanilla 30-40 m fade-out (100 = full)\n"
"	vec4 refl = texture(u_reflTex, gl_FragCoord.xy * u_rtgiParams.yz);\n"
"	float fade = clamp(v_color.a*2.55, 0.0, 1.0);\n"
"	vec4 color = vec4(refl.rgb, refl.a*fade);\n"

"	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);\n"
"	DoAlphaTest(color.a);\n"
"	FRAGCOLOR(color);\n"
"}\n"
;
