#!/bin/sh
# compile RT shaders to SPIR-V byte arrays (committed, so the Vulkan SDK is
# only needed when editing shaders)
GLSLANG="${GLSLANG:-glslangValidator}"
mkdir -p obj
for i in *.comp *.rgen *.rchit *.rmiss; do
	[ -f "$i" ] || continue
	name=$(echo "$i" | sed 's/\./_/g')
	echo "$i"
	"$GLSLANG" -V --target-env vulkan1.2 --vn "${name}_spv" -o "obj/${name}.inc" "$i" || exit 1
done
