FSIN vec3 v_normal;
FSIN float v_depth;

layout(location = 0) out vec4 out_normal;
layout(location = 1) out vec4 out_depth;

void
main(void)
{
	out_normal = vec4(normalize(v_normal), 0.0);
	out_depth = vec4(v_depth, 0.0, 0.0, 0.0);
}
