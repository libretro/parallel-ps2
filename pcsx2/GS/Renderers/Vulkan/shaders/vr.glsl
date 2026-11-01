// The VR passes, behind the utility pipelines' vertex shader (convert.glsl)
// and their one sampler and push constants. Compiled ahead of time by
// tools/vk_spirv.py into VKShaders.inc.

#version 460 core
#extension GL_EXT_samplerless_texture_functions : require

#ifdef FRAGMENT_SHADER

layout(location = 0) in vec2 v_tex;
layout(location = 0) out vec4 o_col0;

layout(set = 0, binding = 0) uniform sampler2D samp0;

#ifdef ps_vr_range
layout(push_constant) uniform cb10
{
	ivec4 SrcRect;
	ivec4 Params;
	vec4 FParams;
};
#endif

#ifdef ps_vr_eye
layout(push_constant) uniform cb10
{
	vec4 EyePos;
	vec4 EyeQuat;
	vec4 FovTan;
	vec4 Screen;
	vec4 DepthMap;
};
#endif


// The passes themselves, shared with the OpenGL backend: tools/vk_spirv.py
// puts VR_GLSL_BODY of ../Common/vr_glsl_body.h here.
// @VR_GLSL_BODY

#endif
