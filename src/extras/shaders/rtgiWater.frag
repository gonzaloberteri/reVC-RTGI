uniform sampler2D tex0;
uniform sampler2D u_reflTex;

uniform vec4 u_rtgiParams;	// x = time (s), y = 1/width, z = 1/height, w = caustic strength
uniform vec4 u_rtgiWaterCam;	// xyz = camera position

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
	// librw's im3d shading (the vanilla water look) ...
	vec4 color = v_color*texture(tex0, vec2(v_tex0.x, 1.0-v_tex0.y));

	// ... caustic shimmer riding the vanilla color (keeps art direction —
	// crests brighten, troughs stay) ...
	if(u_rtgiParams.w > 0.0){
		// fade with distance: sub-pixel caustics alias, and the far
		// water sectors keep their vanilla look anyway — a soft ramp
		// hides the geometry LOD boundary
		float dist = length(v_worldpos - u_rtgiWaterCam.xyz);
		float fade = 1.0 - smoothstep(50.0, 130.0, dist);
		if(fade > 0.0){
			float ca = caustic(v_worldpos.xy, u_rtgiParams.x*0.5 + 23.0);
			color.rgb *= 1.0 + u_rtgiParams.w*1.2*ca*fade;
		}
	}

	// ... plus the ray traced sea reflection; a carries the fresnel-shaped
	// strength the reflection pass computed for the water surface
	vec4 refl = texture(u_reflTex, gl_FragCoord.xy * u_rtgiParams.yz);
	color.rgb = mix(color.rgb, refl.rgb, refl.a);

	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);
	DoAlphaTest(color.a);
	FRAGCOLOR(color);
}
