#pragma once

#include "Cafe/HW/Latte/Renderer/RendererShader.h"
#include "util/math/vector2.h"

#include "Cafe/HW/Latte/Core/LatteTexture.h"

class RendererOutputShader
{
public:
	struct OutputUniformVariables
	{
		Vector2f textureSrcResolution;
		Vector2f nativeResolution;
		Vector2f outputResolution;
		uint32 applySRGBEncoding;
		float targetGamma;
		float displayGamma;
		// Pad to the next std140 vec4 alignment boundary (offset 48). Without this the
		// vertexRotation field would land at offset 36, but vec4's base alignment is 16.
		float _pad0[3];
		// Packed 2x2 rotation matrix applied to the full-screen-quad vertex positions in
		// the vertex shader. (x,y,z,w) = (m00, m10, m01, m11) — column-major as GLSL mat2.
		// Used on Android to compensate for a non-identity swapchain preTransform so the
		// presentation engine doesn't pay a compositor rotation pass.
		float vertexRotation[4];
	};
	enum Shader
	{
		kCopy,
		kBicubic,
		kHermit,
	};
	RendererOutputShader(const std::string& vertex_source, const std::string& fragment_source);
	virtual ~RendererOutputShader() = default;

	OutputUniformVariables FillUniformBlockBuffer(const LatteTextureView& texture_view, const Vector2i& output_res, const bool padView) const;

	RendererShader* GetVertexShader() const
	{
		return m_vertex_shader.get();
	}

	RendererShader* GetFragmentShader() const
	{
		return m_fragment_shader.get();
	}

	static void InitializeStatic();
	static void ShutdownStatic();

	static RendererOutputShader* s_copy_shader;
	static RendererOutputShader* s_copy_shader_ud;

	static RendererOutputShader* s_bicubic_shader;
	static RendererOutputShader* s_bicubic_shader_ud;

	static RendererOutputShader* s_hermit_shader;
	static RendererOutputShader* s_hermit_shader_ud;

	static std::string GetOpenGlVertexSource(bool render_upside_down);
	static std::string GetVulkanVertexSource(bool render_upside_down);
	static std::string GetMetalVertexSource(bool render_upside_down);

	static std::string PrependFragmentPreamble(const std::string& shaderSrc);

protected:
	std::unique_ptr<RendererShader> m_vertex_shader;
	std::unique_ptr<RendererShader> m_fragment_shader;


private:
	static const std::string s_copy_shader_source;
	static const std::string s_bicubic_shader_source;
	static const std::string s_hermite_shader_source;

	static const std::string s_bicubic_shader_source_vk;
	static const std::string s_hermite_shader_source_vk;

	static const std::string s_copy_shader_source_mtl;
	static const std::string s_bicubic_shader_source_mtl;
	static const std::string s_hermite_shader_source_mtl;
};
