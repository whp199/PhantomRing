#pragma once
// Just enough of a DXBC reader to know what a Fox shader expects: its constant buffers (name, slot,
// size, variables) and its bound resources (textures/samplers by name and slot), straight from the
// RDEF chunk the shader blob carries. MGSV ships its shaders with reflection data (MGS5VR reflects
// them: cVSScene b2, cPSMaterial b4, ...), and parsing RDEF ourselves costs microseconds per shader,
// so every shader the game creates can be described without keeping its bytecode or calling
// d3dcompiler on the game's loading threads.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace phantom::guest::d3d
{
	struct ShaderVar
	{
		std::string   name;      // struct members are flattened as "outer.member"
		std::uint32_t offset{ 0 };  // bytes from the start of the cbuffer
		std::uint32_t size{ 0 };
		std::uint16_t cls{ 0 };     // D3D_SHADER_VARIABLE_CLASS (2 = matrix rows, 3 = matrix columns, 1 = vector, 0 = scalar, 5 = struct)
		std::uint16_t rows{ 0 }, cols{ 0 }, elements{ 0 };
	};

	struct ShaderCBuffer
	{
		std::string            name;
		std::uint32_t          slot{ ~0u };  // bN
		std::uint32_t          size{ 0 };
		std::vector<ShaderVar> vars;
	};

	struct ShaderResource
	{
		std::string   name;
		std::uint32_t type{ 0 };       // D3D_SHADER_INPUT_TYPE (2 texture, 3 sampler, 0 cbuffer, 5 structured...)
		std::uint32_t dimension{ 0 };  // D3D_SRV_DIMENSION
		std::uint32_t slot{ 0 };
		std::uint32_t count{ 1 };
	};

	// One input the shader really reads (ISGN chunk): Fox binds one input layout with every semantic for
	// all its shaders, the unused ones aliased onto slot 0 offset 0, so only this says what a draw uses.
	struct ShaderInput
	{
		std::string   semantic;
		std::uint32_t index{ 0 };
		std::uint32_t reg{ 0 };
		std::uint8_t  mask{ 0 };  // components declared
		std::uint8_t  used{ 0 };  // components read
	};

	struct ShaderInfo
	{
		std::uint64_t               hash{ 0 };  // content hash of the bytecode (stable across runs)
		std::uint32_t               bytes{ 0 };
		std::uint8_t                stage{ 0 };  // 0 pixel, 1 vertex (DXBC program type)
		bool                        reflected{ false };
		std::vector<ShaderCBuffer>  cbuffers;
		std::vector<ShaderResource> resources;
		std::vector<ShaderInput>    inputs;

		bool                  Reads(std::string_view a_semantic, std::uint32_t a_index = 0) const;

		const ShaderCBuffer*  CBufferAt(std::uint32_t a_slot) const;
		const ShaderCBuffer*  CBufferNamed(std::string_view a_name) const;
		const ShaderResource* TextureAt(std::uint32_t a_slot) const;
		std::string           Describe() const;  // one line per cbuffer/resource, for the log
	};

	// Parses a DXBC blob. reflected = false when the RDEF chunk is missing or malformed.
	ShaderInfo Reflect(const void* a_bytecode, std::size_t a_bytes);

	// Fast 64-bit content hash used for every key PhantomRing derives from MGSV data (meshes, textures,
	// shaders): FNV-1a over 8-byte words (then the tail bytes), seeded. Deterministic on any machine,
	// so another player's guest computes the same keys for the same assets (DESIGN §14c).
	std::uint64_t Hash64(const void* a_data, std::size_t a_bytes, std::uint64_t a_seed = 0xcbf29ce484222325ull);
}
