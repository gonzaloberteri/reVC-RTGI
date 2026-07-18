uniform sampler2D tex0;
uniform sampler2D u_aoTex;
uniform sampler2D u_giTex;

uniform vec4 u_rtgiParams;	// x = ao strength, y = 1/width, z = 1/height, w = sun shadow strength
uniform vec4 u_rtgiGIParams;	// x = gi blend, y = gi exposure, z/w unused

FSIN vec4 v_color;
FSIN vec3 v_amb;
FSIN vec2 v_tex0;
FSIN float v_fog;

void
main(void)
{
	vec4 t0 = texture(tex0, vec2(v_tex0.x, 1.0-v_tex0.y));

	vec2 uv = gl_FragCoord.xy * u_rtgiParams.yz;
	vec2 rt = texture(u_aoTex, uv).rg;
	float ao = mix(1.0, rt.r, u_rtgiParams.x);

	// ambient: blend the flat timecycle term toward ray traced GI. The GI
	// signal carries its own occlusion, so AO only applies to the flat part.
	vec3 gi = texture(u_giTex, uv).rgb * u_rtgiGIParams.y;
	vec3 ambient = mix(v_amb*ao, gi * max(v_amb, vec3(0.02))*3.0, u_rtgiGIParams.x);

	vec4 light = v_color + vec4(ambient, 0.0);
	vec4 color = t0 * light;
	color.a = t0.a * v_color.a;

	// ray traced shadow (vehicles only for now; replaces the blob shadows)
	color.rgb *= mix(1.0 - u_rtgiParams.w, 1.0, rt.g);

	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);
	DoAlphaTest(color.a);

	FRAGCOLOR(color);
}
