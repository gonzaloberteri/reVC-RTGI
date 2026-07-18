uniform sampler2D tex0;
uniform sampler2D u_aoTex;

uniform vec4 u_rtgiParams;	// x = ao strength, y = 1/width, z = 1/height

FSIN vec4 v_color;
FSIN vec3 v_amb;
FSIN vec2 v_tex0;
FSIN float v_fog;

void
main(void)
{
	vec4 t0 = texture(tex0, vec2(v_tex0.x, 1.0-v_tex0.y));

	float ao = texture(u_aoTex, gl_FragCoord.xy * u_rtgiParams.yz).r;
	ao = mix(1.0, ao, u_rtgiParams.x);

	vec4 light = v_color + vec4(v_amb*ao, 0.0);
	vec4 color = t0 * light;
	color.a = t0.a * v_color.a;

	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);
	DoAlphaTest(color.a);

	FRAGCOLOR(color);
}
