uniform sampler2D u_reflTex;

uniform vec4 u_rtgiParams;	// y = 1/width, z = 1/height

FSIN vec4 v_color;
FSIN vec2 v_tex0;
FSIN float v_fog;
FSIN vec3 v_worldpos;

void
main(void)
{
	// breakable (code-)glass panes: the vanilla pane visual is a sliding
	// fake-reflection quad — replace it with the ray traced mirror the
	// reflection pass computed at these pixels (the G-buffer prepass
	// marked the pane as glass). refl.a carries the Fresnel strength;
	// the vertex alpha keeps the vanilla 30-40 m fade-out (100 = full)
	vec4 refl = texture(u_reflTex, gl_FragCoord.xy * u_rtgiParams.yz);
	float fade = clamp(v_color.a*2.55, 0.0, 1.0);
	vec4 color = vec4(refl.rgb, refl.a*fade);

	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);
	DoAlphaTest(color.a);
	FRAGCOLOR(color);
}
