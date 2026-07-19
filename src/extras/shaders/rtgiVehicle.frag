uniform sampler2D tex0;
uniform sampler2D u_aoTex;
uniform sampler2D u_giTex;
uniform sampler2D u_reflTex;

uniform vec4 u_rtgiReflParams;	// x = reflections enabled

uniform vec4 u_rtgiParams;	// x = ao strength, y = 1/width, z = 1/height, w = sun shadow strength
uniform vec4 u_rtgiGIParams;	// x = gi blend, y = gi exposure, z/w unused
uniform vec4 u_rtgiVehParams;	// x = env-map replacement scale for this material

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

	// ambient: blend the flat timecycle term toward ray traced GI, same as
	// the world composite; vehicles are in the G-buffer so the signals are
	// their own surface's
	vec3 gi = texture(u_giTex, uv).rgb * u_rtgiGIParams.y;
	vec3 ambient = mix(v_amb*ao, gi * max(v_amb, vec3(0.02))*3.0, u_rtgiGIParams.x);

	vec4 light = v_color + vec4(ambient, 0.0);
	vec4 color = t0 * light;
	color.a = t0.a * v_color.a;

	// ray traced sun shadow: vehicles receive shade from buildings etc.
	color.rgb *= mix(1.0 - u_rtgiParams.w, 1.0, rt.g);

	// ray traced reflections replace the matFX env-map pass on materials
	// that carried one (car paint, chrome)
	if(u_rtgiReflParams.x > 0.5 && u_rtgiVehParams.x > 0.0){
		vec4 refl = texture(u_reflTex, uv);
		color.rgb = mix(color.rgb, refl.rgb, refl.a * u_rtgiVehParams.x);
	}

	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);
	DoAlphaTest(color.a);

	FRAGCOLOR(color);
}
