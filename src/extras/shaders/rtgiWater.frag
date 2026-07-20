uniform sampler2D tex0;
uniform sampler2D u_reflTex;

uniform vec4 u_rtgiParams;	// x = time (s), y = 1/width, z = 1/height, w = caustic strength
// camera position rides in u_gbParams.xyz (w = 1 when v_color must be
// replaced by u_rtgiGIParams: the wavy/mask ATOMICS carry static daylight
// prelight, wrong at night); librw's uniform registry (MAX_UNIFORMS 40)
// is full, so the water pass reuses registered slots
uniform vec4 u_gbParams;
uniform vec4 u_rtgiGIParams;	// timecycle water RGBA (atomic path)

FSIN vec4 v_color;
FSIN vec2 v_tex0;
FSIN float v_fog;
FSIN vec3 v_worldpos;

// classic iterative interference caustic (the well-known Shadertoy water
// shimmer), tiled in world space so it is stable under camera motion
float
caustic(vec2 wp, float t)
{
	vec2 p = mod(wp*0.5, 6.28318530718) - 250.0;
	vec2 i = p;
	float c = 1.0;
	float inten = 0.005;
	for(int n = 0; n < 5; n++){
		float tt = t * (1.0 - (3.5/float(n + 1)));
		i = p + vec2(cos(tt - i.x) + sin(tt + i.y), sin(tt - i.y) + cos(tt + i.x));
		c += 1.0/length(vec2(p.x/(sin(i.x + tt)/inten), p.y/(cos(i.y + tt)/inten)));
	}
	c /= 5.0;
	c = 1.17 - pow(c, 1.4);
	return pow(abs(c), 8.0);
}

void
main(void)
{
	vec4 tex = texture(tex0, vec2(v_tex0.x, 1.0-v_tex0.y));

	vec4 color;
	if(u_rtgiParams.w > 0.0){
		// fully procedural surface: the vanilla water texture, including
		// its baked reflective sparkle, is dropped entirely. Base tint
		// is the timecycle water color carried in v_color (day/night art
		// still tracks); the caustic shimmer rides on top, tinted by the
		// base so night water stays dark. Texture ALPHA is kept: it is
		// the shore fade mask.
		float dist = length(v_worldpos - u_gbParams.xyz);
		float fade = 1.0 - smoothstep(60.0, 220.0, dist);
		float ca = 0.0;
		if(fade > 0.0)
			ca = caustic(v_worldpos.xy, u_rtgiParams.x*0.5 + 23.0) * fade * u_rtgiParams.w;
		// 0.62 stands in for the mean of the dropped texture so the
		// procedural sea keeps the vanilla art tone
		vec4 wcol = mix(v_color, u_rtgiGIParams, u_gbParams.w);
		vec3 base = wcol.rgb*0.62;
		// the white additive sparkle dims with the base luminance so
		// night water does not glow
		float lum = clamp(dot(base, vec3(0.299, 0.587, 0.114))*3.0, 0.0, 1.0);
		color.rgb = clamp(base*(1.0 + 1.8*ca) + 0.10*ca*lum, 0.0, 1.0);
		// near water stays translucent (shore sand shows through, as
		// vanilla); far water goes opaque so the seabed LOD prelight
		// cannot grid-pattern through the clean procedural surface
		// vanilla only reads the floor through the first ~40 m of
		// shallows; past that the seabed mip pattern grids through
		color.a = mix(wcol.a * tex.a, 1.0, smoothstep(30.0, 100.0, dist));
	}else{
		// caustics toggled off: vanilla textured look
		color = v_color*tex;
	}

	// the ray traced sea reflection; a carries the fresnel-shaped strength
	// the reflection pass computed for the water surface
	vec4 refl = texture(u_reflTex, gl_FragCoord.xy * u_rtgiParams.yz);
	color.rgb = mix(color.rgb, refl.rgb, refl.a);

	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);
	DoAlphaTest(color.a);
	FRAGCOLOR(color);
}
