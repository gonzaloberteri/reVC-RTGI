const char *rtgiWater_frag_src =
"uniform sampler2D tex0;\n"
"uniform sampler2D u_reflTex;\n"

"uniform vec4 u_rtgiParams;	// y = 1/width, z = 1/height\n"

"FSIN vec4 v_color;\n"
"FSIN vec2 v_tex0;\n"
"FSIN float v_fog;\n"

"void\n"
"main(void)\n"
"{\n"
"	// librw's im3d shading (the vanilla water look) ...\n"
"	vec4 color = v_color*texture(tex0, vec2(v_tex0.x, 1.0-v_tex0.y));\n"

"	// ... plus the ray traced sea reflection; a carries the fresnel-shaped\n"
"	// strength the reflection pass computed for the water surface\n"
"	vec4 refl = texture(u_reflTex, gl_FragCoord.xy * u_rtgiParams.yz);\n"
"	color.rgb = mix(color.rgb, refl.rgb, refl.a);\n"

"	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);\n"
"	DoAlphaTest(color.a);\n"
"	FRAGCOLOR(color);\n"
"}\n"
;
