FSIN vec3 v_worldpos;
FSIN float v_depth;

layout(location = 0) out vec4 out_normal;
layout(location = 1) out vec4 out_depth;

void
main(void)
{
	// face normal from position derivatives: most VC world geometry is
	// prelit and carries no vertex normals at all
	vec3 n = normalize(cross(dFdx(v_worldpos), dFdy(v_worldpos)));
	out_normal = vec4(n, 0.0);
	out_depth = vec4(v_depth, 0.0, 0.0, 0.0);
}
