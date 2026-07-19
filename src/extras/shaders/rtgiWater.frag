uniform sampler2D tex0;
uniform sampler2D u_reflTex;

uniform vec4 u_rtgiParams;	// y = 1/width, z = 1/height

FSIN vec4 v_color;
FSIN vec2 v_tex0;
FSIN float v_fog;

void
main(void)
{
	// librw's im3d shading (the vanilla water look) ...
	vec4 color = v_color*texture(tex0, vec2(v_tex0.x, 1.0-v_tex0.y));

	// ... plus the ray traced sea reflection; a carries the fresnel-shaped
	// strength the reflection pass computed for the water surface
	vec4 refl = texture(u_reflTex, gl_FragCoord.xy * u_rtgiParams.yz);
	color.rgb = mix(color.rgb, refl.rgb, refl.a);

	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);
	DoAlphaTest(color.a);
	FRAGCOLOR(color);
}
