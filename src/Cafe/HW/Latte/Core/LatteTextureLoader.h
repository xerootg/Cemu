#pragma once
#include "util/helpers/ClassWrapper.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "util/ImageWriter/tga.h"

struct LatteTextureLoaderCtx
{
	uint32 physAddress;
	uint32 physMipAddress;
	sint32 width;
	sint32 height;
	sint32 pitch; // stored elements per row
	uint32 mipLevels;
	uint32 sliceIndex;
	sint32 stepX;
	sint32 stepY;
	uint32 pipeSwizzle;
	uint32 bankSwizzle;
	Latte::E_HWTILEMODE tileMode;
	uint32 bpp;
	uint8* inputData;
	sint32 minOffsetOutdated;
	sint32 maxOffsetOutdated;
	// calculated info
	uint32 surfaceInfoHeight;
	uint32 surfaceInfoDepth;
	// mip
	uint32 levelOffset; // relative to physMipAddress
	// info for decoded texture
	sint32 decodedTexelCountX;
	sint32 decodedTexelCountY;
	// decoder
	LatteAddrLib::CachedSurfaceAddrInfo computeAddrInfo;
	// debug dump texture
	bool dump;
	uint8* dumpRGBA;
};

uint8* LatteTextureLoader_GetInput(LatteTextureLoaderCtx* textureLoader, sint32 x, sint32 y);
void LatteTextureLoader_begin(LatteTextureLoaderCtx* textureLoader, uint32 sliceIndex, uint32 mipIndex, MPTR physImagePtr, MPTR physMipPtr, Latte::E_GX2SURFFMT format, Latte::E_DIM dim, uint32 width, uint32 height, uint32 depth, uint32 mipLevels, uint32 pitch, Latte::E_HWTILEMODE tileMode, uint32 swizzle);

#include "Cafe/HW/Latte/LatteAddrLib/AddrLibFastDecode.h"

void decodeBC1Block(uint8* inputData, float* output4x4RGBA);
void decodeBC2Block_UNORM(uint8* inputData, float* imageRGBA);
void decodeBC3Block_UNORM(uint8* inputData, float* imageRGBA);
void decodeBC4Block_UNORM(uint8* blockStorage, float* rOutput);
void decodeBC5Block_UNORM(uint8* blockStorage, float* rgOutput);
void decodeBC5Block_SNORM(uint8* blockStorage, float* rgOutput);
using decodingFn = void (uint8 *, float *);

inline void BC1_GetPixel(uint8* inputData, sint32 x, sint32 y, uint8 rgba[4])
{
	// read colors
	uint16 c0 = *(uint16*)(inputData + 0);
	uint16 c1 = *(uint16*)(inputData + 2);
	// decode colors (RGB565 -> RGB888)
	float r[4];
	float g[4];
	float b[4];
	float a[4];
	b[0] = (float)((c0 >> 0) & 0x1F) / 31.0f;
	b[1] = (float)((c1 >> 0) & 0x1F) / 31.0f;
	g[0] = (float)((c0 >> 5) & 0x3F) / 63.0f;
	g[1] = (float)((c1 >> 5) & 0x3F) / 63.0f;
	r[0] = (float)((c0 >> 11) & 0x1F) / 31.0f;
	r[1] = (float)((c1 >> 11) & 0x1F) / 31.0f;
	a[0] = 1.0f;
	a[1] = 1.0f;
	a[2] = 1.0f;

	if (c0 > c1)
	{
		r[2] = (r[0] * 2.0f + r[1]) / 3.0f;
		r[3] = (r[0] * 1.0f + r[1] * 2.0f) / 3.0f;
		g[2] = (g[0] * 2.0f + g[1]) / 3.0f;
		g[3] = (g[0] * 1.0f + g[1] * 2.0f) / 3.0f;
		b[2] = (b[0] * 2.0f + b[1]) / 3.0f;
		b[3] = (b[0] * 1.0f + b[1] * 2.0f) / 3.0f;
		a[3] = 1.0f;
	}
	else
	{
		r[2] = (r[0] + r[1]) / 2.0f;
		r[3] = 0.0f;
		g[2] = (g[0] + g[1]) / 2.0f;
		g[3] = 0.0f;
		b[2] = (b[0] + b[1]) / 2.0f;
		b[3] = 0.0f;
		a[3] = 0.0f;
	}

	uint8* indexData = inputData + 4;

	uint8 i = (indexData[y] >> (x * 2)) & 3;
	rgba[0] = (uint8)(r[i] * 255.0f);
	rgba[1] = (uint8)(g[i] * 255.0f);
	rgba[2] = (uint8)(b[i] * 255.0f);
	rgba[3] = (uint8)(a[i] * 255.0f);
}

// decodes a specific GPU7 texture format into a native linear format that can be used by the render API
class TextureDecoder
{
public:
	// properties
	virtual sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) = 0;
	virtual sint32 getTexelCountX(LatteTextureLoaderCtx* textureLoader)
	{
		return textureLoader->width;
	}

	virtual sint32 getTexelCountY(LatteTextureLoaderCtx* textureLoader)
	{
		return textureLoader->height;
	}

	// image size
	virtual sint32 calculateImageSize(LatteTextureLoaderCtx* textureLoader)
	{
		return getTexelCountX(textureLoader) * getTexelCountY(textureLoader) * getBytesPerTexel(textureLoader);
	}

	// decode loop
	virtual void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) = 0;

	virtual void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) = 0;
};

class TextureDecoder_R16_G16_B16_A16_FLOAT : public TextureDecoder, public SingletonClass<TextureDecoder_R16_G16_B16_A16_FLOAT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2 * 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint64, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// todo: dump 16 bit float formats properly
		float r16f = (float)(*((uint16*)blockData + 0));
		float g16f = (float)(*((uint16*)blockData + 1));
		float b16f = (float)(*((uint16*)blockData + 2));
		float a16f = (float)(*((uint16*)blockData + 3));
		*(outputPixel + 0) = (uint8)(r16f * 255.0f);
		*(outputPixel + 1) = (uint8)(g16f * 255.0f);
		*(outputPixel + 2) = (uint8)(b16f * 255.0f);
		*(outputPixel + 3) = (uint8)(a16f * 255.0f);
	}
};

class TextureDecoder_R16_G16_FLOAT : public TextureDecoder, public SingletonClass<TextureDecoder_R16_G16_FLOAT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2 * 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint32, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// todo: dump 16 bit float formats properly
		float r16f = (float)(*((uint16*)blockData + 0));
		float g16f = (float)(*((uint16*)blockData + 1));
		*(outputPixel + 0) = (uint8)(r16f * 255.0f);
		*(outputPixel + 1) = (uint8)(g16f * 255.0f);
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R16_SNORM : public TextureDecoder, public SingletonClass<TextureDecoder_R16_SNORM>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 1 * 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint16, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		*(outputPixel + 0) = (uint8)(*(((uint16*)blockData) + 0) >> 8) / 2 + 128;
		*(outputPixel + 1) = 0;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R16_FLOAT : public TextureDecoder, public SingletonClass<TextureDecoder_R16_FLOAT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 1 * 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint16, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// todo: dump 16 bit float formats properly
		float r16f = (float)(*((uint16*)blockData + 0));
		*(outputPixel + 0) = (uint8)(r16f * 255.0f);
		*(outputPixel + 1) = 0;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R32_FLOAT : public TextureDecoder, public SingletonClass<TextureDecoder_R32_FLOAT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 1 * 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint32, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float r32f = *((float*)blockData + 0);
		*(outputPixel + 0) = (uint8)(r32f * 255.0f);
		*(outputPixel + 1) = 0;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R32_G32_FLOAT : public TextureDecoder, public SingletonClass<TextureDecoder_R32_G32_FLOAT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2 * 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint64, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float r32f = *((float*)blockData + 0);
		float g32f = *((float*)blockData + 1);
		*(outputPixel + 0) = (uint8)(r32f * 255.0f);
		*(outputPixel + 1) = (uint8)(g32f * 255.0f);
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R32_G32_UINT : public TextureDecoder, public SingletonClass<TextureDecoder_R32_G32_UINT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2 * 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint64, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		*(outputPixel + 0) = (uint8)(*(((uint32*)blockData) + 0) >> 24);
		*(outputPixel + 1) = (uint8)(*(((uint32*)blockData) + 1) >> 24);
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};


class TextureDecoder_R32_UINT : public TextureDecoder, public SingletonClass<TextureDecoder_R32_UINT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 1 * 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint32, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		*(outputPixel + 0) = (uint8)(*(((uint32*)blockData) + 0) >> 24);
		*(outputPixel + 1) = 0;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R16_UINT : public TextureDecoder, public SingletonClass<TextureDecoder_R16_UINT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 1 * 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint16, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		*(outputPixel + 0) = (uint8)(*(((uint16*)blockData) + 0) >> 8);
		*(outputPixel + 1) = 0;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R8_UINT : public TextureDecoder, public SingletonClass<TextureDecoder_R8_UINT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 1;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint8, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		*(outputPixel + 0) = *(blockData + 0);
		*(outputPixel + 1) = 0;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R32_G32_B32_A32_FLOAT : public TextureDecoder, public SingletonClass<TextureDecoder_R32_G32_B32_A32_FLOAT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4 * 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint64, 2, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float r32f = *((float*)blockData + 0);
		float g32f = *((float*)blockData + 1);
		float b32f = *((float*)blockData + 2);
		float a32f = *((float*)blockData + 3);
		*(outputPixel + 0) = (uint8)(r32f * 255.0f);
		*(outputPixel + 1) = (uint8)(g32f * 255.0f);
		*(outputPixel + 2) = (uint8)(b32f * 255.0f);
		*(outputPixel + 3) = (uint8)(a32f * 255.0f);
	}
};

class TextureDecoder_R32_G32_B32_A32_UINT : public TextureDecoder, public SingletonClass<TextureDecoder_R32_G32_B32_A32_UINT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4 * 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		// note - before 1.15.4 this format was implemented as big-endian
		//optimizedDecodeLoops<uint64, 2, false>(textureLoader, outputData);

		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 pixelOffset = (x + yc * textureLoader->width) * 4 * sizeof(uint32); // write to target buffer
				*(uint32*)(outputData + pixelOffset + 0) = _swapEndianU32(*(uint32*)(blockData + 0));
				*(uint32*)(outputData + pixelOffset + 4) = _swapEndianU32(*(uint32*)(blockData + 4));
				*(uint32*)(outputData + pixelOffset + 8) = _swapEndianU32(*(uint32*)(blockData + 8));
				*(uint32*)(outputData + pixelOffset + 12) = _swapEndianU32(*(uint32*)(blockData + 12));
				// todo: Verify if this format is big-endian
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		*(outputPixel + 0) = (uint8)(*(((uint32*)blockData) + 0) >> 24);
		*(outputPixel + 1) = (uint8)(*(((uint32*)blockData) + 1) >> 24);
		*(outputPixel + 2) = (uint8)(*(((uint32*)blockData) + 2) >> 24);
		*(outputPixel + 3) = (uint8)(*(((uint32*)blockData) + 3) >> 24);
	}
};

class TextureDecoder_R16_G16_B16_A16_UINT : public TextureDecoder, public SingletonClass<TextureDecoder_R16_G16_B16_A16_UINT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4 * 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		// note - before 1.15.4 this format was implemented as big-endian
		//optimizedDecodeLoops<uint64, 1, false>(textureLoader, outputData);

		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 pixelOffset = (x + yc * textureLoader->width) * 4 * sizeof(uint16); // write to target buffer
				*(uint16*)(outputData + pixelOffset + 0) = _swapEndianU16(*(uint16*)(blockData + 0));
				*(uint16*)(outputData + pixelOffset + 2) = _swapEndianU16(*(uint16*)(blockData + 2));
				*(uint16*)(outputData + pixelOffset + 4) = _swapEndianU16(*(uint16*)(blockData + 4));
				*(uint16*)(outputData + pixelOffset + 6) = _swapEndianU16(*(uint16*)(blockData + 6));
				// todo: Verify if this format is big-endian
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		*(outputPixel + 0) = (uint8)(*(((uint16*)blockData) + 0) >> 8);
		*(outputPixel + 1) = (uint8)(*(((uint16*)blockData) + 1) >> 8);
		*(outputPixel + 2) = (uint8)(*(((uint16*)blockData) + 2) >> 8);
		*(outputPixel + 3) = (uint8)(*(((uint16*)blockData) + 3) >> 8);
	}
};


class TextureDecoder_R8_G8_B8_A8_UINT : public TextureDecoder, public SingletonClass<TextureDecoder_R8_G8_B8_A8_UINT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4 * 1;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint32, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		*(outputPixel + 0) = *(blockData + 0);
		*(outputPixel + 1) = *(blockData + 1);
		*(outputPixel + 2) = *(blockData + 2);
		*(outputPixel + 3) = *(blockData + 3);
	}
};

class TextureDecoder_R24_X8 : public TextureDecoder, public SingletonClass<TextureDecoder_R24_X8>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 pixelOffset = (x + yc * textureLoader->width) * 1 * sizeof(uint32);
				*(uint32*)(outputData + pixelOffset + 0) = 0;
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
	}
};

class TextureDecoder_X24_G8_UINT : public TextureDecoder, public SingletonClass<TextureDecoder_X24_G8_UINT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 pixelOffset = (x + yc * textureLoader->width) * 1 * sizeof(uint32);
				*(uint32*)(outputData + pixelOffset + 0) = 0;
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
	}
};

class TextureDecoder_D32_S8_UINT_X24 : public TextureDecoder, public SingletonClass<TextureDecoder_D32_S8_UINT_X24>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 8;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint64, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
	}
};

class TextureDecoder_R4_G4_UNORM_To_RGBA4 : public TextureDecoder, public SingletonClass<TextureDecoder_R4_G4_UNORM_To_RGBA4>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 pixelOffset = (x + yc * textureLoader->width) * 2;
				uint8 v = (*(uint8*)(blockData + 0));
				*(uint8*)(outputData + pixelOffset + 0) = 0; // OpenGL has no RG4 format so we use RGBA4 instead and these two values (blue + alpha) are always set to zero
				*(uint8*)(outputData + pixelOffset + 1) = ((v >> 4) & 0xF) | ((v << 4) & 0xF0); // todo: Is this nibble swap correct?
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint8 v0 = *(blockData + 0);
		uint8 c0 = (v0 & 0xF);
		uint8 c1 = (v0 >> 4) & 0xF;
		c0 = (c0 << 4) | c0;
		c1 = (c1 << 4) | c1;
		*(outputPixel + 0) = c0;
		*(outputPixel + 1) = c1;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R4_G4_UNORM_To_ABGR4 : public TextureDecoder, public SingletonClass<TextureDecoder_R4_G4_UNORM_To_ABGR4>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 pixelOffset = (x + yc * textureLoader->width) * 2;
				uint8 v = (*(uint8*)(blockData + 0));
				*(uint8*)(outputData + pixelOffset + 0) = 0;
				*(uint8*)(outputData + pixelOffset + 1) = v;
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint8 v0 = *(blockData + 0);
		uint8 c0 = (v0 & 0xF);
		uint8 c1 = (v0 >> 4) & 0xF;
		c0 = (c0 << 4) | c0;
		c1 = (c1 << 4) | c1;
		*(outputPixel + 0) = c0;
		*(outputPixel + 1) = c1;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R4G4_UNORM_To_RGBA8 : public TextureDecoder, public SingletonClass<TextureDecoder_R4G4_UNORM_To_RGBA8>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 pixelOffset = (x + yc * textureLoader->width) * 4;
				uint8 v0 = (*(uint8*)(blockData + 0));

				uint8 red4 = (v0 >> 4) & 0xF;
				uint8 green4 = (v0 & 0xF);

				red4 = (red4 << 4) | red4;
				green4 = (green4 << 4) | green4;

				*(uint8*)(outputData + pixelOffset + 0) = red4;
				*(uint8*)(outputData + pixelOffset + 1) = green4;
				*(uint8*)(outputData + pixelOffset + 2) = 0;
				*(uint8*)(outputData + pixelOffset + 3) = 255;
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint8 v0 = *(blockData + 0);
		uint8 red4 = (v0 >> 4) & 0xF;
		uint8 green4 = (v0 & 0xF);
		red4 = (red4 << 4) | red4;
		green4 = (green4 << 4) | green4;
		*(outputPixel + 0) = red4;
		*(outputPixel + 1) = green4;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R4G4_UNORM_To_RG8 : public TextureDecoder, public SingletonClass<TextureDecoder_R4G4_UNORM_To_RG8>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 pixelOffset = (x + yc * textureLoader->width) * 2;
				uint8 v0 = (*(uint8*)(blockData + 0));

				uint8 red4 = (v0 >> 4) & 0xF;
				uint8 green4 = (v0 & 0xF);

				red4 = (red4 << 4) | red4;
				green4 = (green4 << 4) | green4;

				*(uint8*)(outputData + pixelOffset + 0) = red4;
				*(uint8*)(outputData + pixelOffset + 1) = green4;
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint8 v0 = *(blockData + 0);
		uint8 red4 = (v0 >> 4) & 0xF;
		uint8 green4 = (v0 & 0xF);
		red4 = (red4 << 4) | red4;
		green4 = (green4 << 4) | green4;
		*(outputPixel + 0) = red4;
		*(outputPixel + 1) = green4;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R4_G4_B4_A4_UNORM : public TextureDecoder, public SingletonClass<TextureDecoder_R4_G4_B4_A4_UNORM>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 pixelOffset = (x + yc * textureLoader->width) * 2;
				uint8 v0 = (*(uint8*)(blockData + 0));
				uint8 v1 = (*(uint8*)(blockData + 1));
				*(uint8*)(outputData + pixelOffset + 0) = ((v1 >> 4) & 0xF) | ((v1 << 4) & 0xF0); // todo: Verify
				*(uint8*)(outputData + pixelOffset + 1) = ((v0 >> 4) & 0xF) | ((v0 << 4) & 0xF0); // todo: Verify
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint8 v0 = *(blockData + 0);
		uint8 v1 = *(blockData + 1);
		uint8 c0 = (v0 & 0xF);
		uint8 c1 = (v0 >> 4) & 0xF;
		uint8 c2 = (v1 & 0xF);
		uint8 c3 = (v1 >> 4) & 0xF;
		c0 = (c0 << 4) | c0;
		c1 = (c1 << 4) | c1;
		c2 = (c2 << 4) | c2;
		c3 = (c3 << 4) | c3;
		*(outputPixel + 0) = c0;
		*(outputPixel + 1) = c1;
		*(outputPixel + 2) = c2;
		*(outputPixel + 3) = c3;
	}
};

class TextureDecoder_R4G4B4A4_UNORM_To_RGBA8 : public TextureDecoder, public SingletonClass<TextureDecoder_R4G4B4A4_UNORM_To_RGBA8>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 pixelOffset = (x + yc * textureLoader->width) * 4;
				uint8 v0 = (*(uint8*)(blockData + 0));
				uint8 v1 = (*(uint8*)(blockData + 1));

				uint8 red4 = (v0 & 0xF);
				uint8 green4 = (v0 >> 4) & 0xF;
				uint8 blue4 = (v1) & 0xF;
				uint8 alpha4 = (v1 >> 4) & 0xF;

				red4 = (red4 << 4) | red4;
				green4 = (green4 << 4) | green4;
				blue4 = (blue4 << 4) | blue4;
				alpha4 = (alpha4 << 4) | alpha4;

				*(uint8*)(outputData + pixelOffset + 0) = red4;
				*(uint8*)(outputData + pixelOffset + 1) = green4;
				*(uint8*)(outputData + pixelOffset + 2) = blue4;
				*(uint8*)(outputData + pixelOffset + 3) = alpha4;
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint8 v0 = *(blockData + 0);
		uint8 v1 = *(blockData + 1);

		uint8 red4 = (v0 & 0xF);
		uint8 green4 = (v0 >> 4) & 0xF;
		uint8 blue4 = (v1) & 0xF;
		uint8 alpha4 = (v1 >> 4) & 0xF;

		red4 = (red4 << 4) | red4;
		green4 = (green4 << 4) | green4;
		blue4 = (blue4 << 4) | blue4;
		alpha4 = (alpha4 << 4) | alpha4;

		*(outputPixel + 0) = red4;
		*(outputPixel + 1) = green4;
		*(outputPixel + 2) = blue4;
		*(outputPixel + 3) = alpha4;
	}
};

class TextureDecoder_R8_G8_B8_A8 : public TextureDecoder, public SingletonClass<TextureDecoder_R8_G8_B8_A8>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint32, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		*(outputPixel + 0) = *(blockData + 0);
		*(outputPixel + 1) = *(blockData + 1);
		*(outputPixel + 2) = *(blockData + 2);
		*(outputPixel + 3) = *(blockData + 3);
	}
};

class TextureDecoder_D24_S8 : public TextureDecoder, public SingletonClass<TextureDecoder_D24_S8>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint32, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint32 d24 = (*(uint32*)blockData) & 0xFFFFFF;
		uint8 s8 = (*(uint32*)blockData >> 24) & 0xFF;
		*(outputPixel + 0) = (uint8)(d24 >> 16);
		*(outputPixel + 1) = (uint8)(d24 >> 16);
		*(outputPixel + 2) = (uint8)(d24 >> 16);
		*(outputPixel + 3) = s8;
	}
};

class TextureDecoder_NullData32 : public TextureDecoder, public SingletonClass<TextureDecoder_NullData32>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		memset(outputData, 0, sizeof(uint32) * getTexelCountX(textureLoader) * getTexelCountY(textureLoader));
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
	}
};

class TextureDecoder_NullData64 : public TextureDecoder, public SingletonClass<TextureDecoder_NullData64>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 8;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		memset(outputData, 0, sizeof(uint64) * getTexelCountX(textureLoader) * getTexelCountY(textureLoader));
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
	}
};

class TextureDecoder_R8 : public TextureDecoder, public SingletonClass<TextureDecoder_R8>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 1;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint8, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		*(outputPixel + 0) = *(blockData + 0);
		*(outputPixel + 1) = 0;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R8_G8 : public TextureDecoder, public SingletonClass<TextureDecoder_R8_G8>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint16, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		*(outputPixel + 0) = *(blockData + 0);
		*(outputPixel + 1) = *(blockData + 1);
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

// R8G8 → RGBA8 widening as (R, G, 0, 255). Same shape as native R8G8 sampling. Widening to
// RGBA8 sidesteps a Mali Vulkan driver bug that drops the texture-view component swizzle on
// R8G8 sampling. The R8_G8_UNORM swizzle adjustment in LatteTextureVk_AdjustTextureCompSel
// (mirrors BC5: alpha-from-G) is what gives grayscale-with-mask textures their correct alpha.
class TextureDecoder_R8_G8_To_R8G8B8A8 : public TextureDecoder, public SingletonClass<TextureDecoder_R8_G8_To_R8G8B8A8>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* src = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 dstOffset = (x + y * textureLoader->width) * 4;
				outputData[dstOffset + 0] = src[0];
				outputData[dstOffset + 1] = src[1];
				outputData[dstOffset + 2] = 0;
				outputData[dstOffset + 3] = 255;
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		outputPixel[0] = *(blockData + 0);
		outputPixel[1] = *(blockData + 1);
		outputPixel[2] = 0;
		outputPixel[3] = 255;
	}
};

class TextureDecoder_R4_G4 : public TextureDecoder, public SingletonClass<TextureDecoder_R4_G4>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 1;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint8, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint8 v0 = (*(uint8*)(blockData + 0));
		uint8 c0 = (v0 & 0xF);
		uint8 c1 = (v0 >> 4) & 0xF;
		c0 = (c0 << 4) | c0;
		c1 = (c1 << 4) | c1;
		*(outputPixel + 0) = c0;
		*(outputPixel + 1) = c1;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R16_UNORM : public TextureDecoder, public SingletonClass<TextureDecoder_R16_UNORM>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint16, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		*(outputPixel + 0) = (uint8)(*(((uint16*)blockData) + 0) >> 8);
		*(outputPixel + 1) = 0;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R16_G16_B16_A16 : public TextureDecoder, public SingletonClass<TextureDecoder_R16_G16_B16_A16>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4 * 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint64, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		*(outputPixel + 0) = (uint8)(*(((uint16*)blockData) + 0) >> 8);
		*(outputPixel + 1) = (uint8)(*(((uint16*)blockData) + 1) >> 8);
		*(outputPixel + 2) = (uint8)(*(((uint16*)blockData) + 2) >> 8);
		*(outputPixel + 3) = (uint8)(*(((uint16*)blockData) + 3) >> 8);
	}
};

class TextureDecoder_R16_G16 : public TextureDecoder, public SingletonClass<TextureDecoder_R16_G16>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2 * 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint32, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		*(outputPixel + 0) = (uint8)(*(((uint16*)blockData) + 0) >> 8);
		*(outputPixel + 1) = (uint8)(*(((uint16*)blockData) + 1) >> 8);
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R5_G6_B5 : public TextureDecoder, public SingletonClass<TextureDecoder_R5_G6_B5>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint16, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint16 colorData = (*(uint16*)blockData);
		uint8 red5 = (colorData >> 0) & 0x1F;
		uint8 green6 = (colorData >> 5) & 0x3F;
		uint8 blue5 = (colorData >> 11) & 0x1F;
		*(outputPixel + 0) = (red5 << 3) | (red5 >> 2);
		*(outputPixel + 1) = (green6 << 2) | (green6 >> 4);
		*(outputPixel + 2) = (blue5 << 3) | (blue5 >> 2);
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R5_G6_B5_swappedRB : public TextureDecoder, public SingletonClass<TextureDecoder_R5_G6_B5_swappedRB>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 pixelOffset = (x + yc * textureLoader->width) * 2;
				uint16 colorData = (*(uint16*)(blockData + 0));

				// colorData >> 0 -> red
				// colorData >> 5 -> green
				// colorData >> 11 -> blue

				colorData = ((colorData >> 11) & 0x1F) | (colorData & (0x3F << 5)) | ((colorData << 11) & (0x1F << 11));

				//// swap order of components
				////uint8 red5 = (colorData>>0)&0x1F;
				////uint8 green5 = (colorData>>5)&0x1F;
				////uint8 blue5 = (colorData>>10)&0x1F;
				////uint8 alpha1 = (colorData>>15)&0x1;
				////colorData = blue5|(green5<<5)|(red5<<10)|(alpha1<<15);
				////*(uint16*)(outputData+pixelOffset+0) = colorData;
				//uint8 red5 = (colorData >> 11) & 0x1F;
				//uint8 green5 = (colorData >> 6) & 0x1F;
				//uint8 blue5 = (colorData >> 1) & 0x1F;
				//uint8 alpha1 = (colorData >> 0) & 0x1;
				////colorData = blue5|(green5<<5)|(red5<<10)|(alpha1<<15);
				////colorData = (blue5<<11)|(green5<<6)|(red5<<1)|(alpha1<<0);
				* (uint16*)(outputData + pixelOffset + 0) = colorData;
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint16 colorData = (*(uint16*)blockData);
		uint8 red5 = (colorData >> 0) & 0x1F;
		uint8 green6 = (colorData >> 5) & 0x3F;
		uint8 blue5 = (colorData >> 11) & 0x1F;
		*(outputPixel + 0) = (red5 << 3) | (red5 >> 2);
		*(outputPixel + 1) = (green6 << 2) | (green6 >> 4);
		*(outputPixel + 2) = (blue5 << 3) | (blue5 >> 2);
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_R5G6B5_UNORM_To_RGBA8 : public TextureDecoder, public SingletonClass<TextureDecoder_R5G6B5_UNORM_To_RGBA8>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 pixelOffset = (x + yc * textureLoader->width) * 4;
				uint16 v0 = (*(uint16*)(blockData + 0));

				uint8 c0 = (v0 & 0x1F);
				uint8 c1 = (v0 >> 5) & 0x3F;
				uint8 c2 = (v0 >> 11) & 0x1F;

				c0 = (c0 << 3) | c0 >> 3;// blue
				c1 = (c1 << 2) | c1 >> 4;// green
				c2 = (c2 << 3) | c2 >> 3;// red

				*(uint8*)(outputData + pixelOffset + 0) = c0;// blue
				*(uint8*)(outputData + pixelOffset + 1) = c1;// green
				*(uint8*)(outputData + pixelOffset + 2) = c2;// red
				*(uint8*)(outputData + pixelOffset + 3) = 255;//alpha
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint16 v0 = *(uint16*)(blockData + 0);
		uint8 c0 = (v0 & 0x1F);// red
		uint8 c1 = (v0 >> 5) & 0x3F;// green
		uint8 c2 = (v0 >> 11) & 0x1F; // blue
		c0 = (c0 << 3) | c0 >> 3;
		c1 = (c1 << 2) | c1 >> 4;
		c2 = (c2 << 3) | c2 >> 3;
		*(outputPixel + 0) = c0;// red
		*(outputPixel + 1) = c1;// green
		*(outputPixel + 2) = c2;// blue
		*(outputPixel + 3) = 255;// alpha
	}
};

class TextureDecoder_R5_G5_B5_A1_UNORM : public TextureDecoder, public SingletonClass<TextureDecoder_R5_G5_B5_A1_UNORM>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint16, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint16 colorData = (*(uint16*)blockData);
		uint8 red5 = (colorData >> 0) & 0x1F;
		uint8 green5 = (colorData >> 5) & 0x1F;
		uint8 blue5 = (colorData >> 10) & 0x1F;
		uint8 alpha1 = (colorData >> 11) & 0x1;
		*(outputPixel + 0) = (red5 << 3) | (red5 >> 2);
		*(outputPixel + 1) = (green5 << 3) | (green5 >> 2);
		*(outputPixel + 2) = (blue5 << 3) | (blue5 >> 2);
		*(outputPixel + 3) = (alpha1 << 3);
	}
};

class uint16_R5_G5_B5_A1_swapRB
{
public:
	void operator=(const uint16_R5_G5_B5_A1_swapRB& v)
	{
		internalVal = ((v.internalVal >> 10) & (0x1F << 0)) | ((v.internalVal << 10) & (0x1F << 10)) | (v.internalVal & ((0x1F << 5) | 0x8000));
	}

	uint16 internalVal;
};

static_assert(sizeof(uint16_R5_G5_B5_A1_swapRB) == 2);

class TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB : public TextureDecoder, public SingletonClass<TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint16_R5_G5_B5_A1_swapRB, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint16 colorData = (*(uint16*)blockData);
		uint8 red5 = (colorData >> 0) & 0x1F;
		uint8 green5 = (colorData >> 5) & 0x1F;
		uint8 blue5 = (colorData >> 10) & 0x1F;
		uint8 alpha1 = (colorData >> 11) & 0x1;
		*(outputPixel + 0) = (red5 << 3) | (red5 >> 2);
		*(outputPixel + 1) = (green5 << 3) | (green5 >> 2);
		*(outputPixel + 2) = (blue5 << 3) | (blue5 >> 2);
		*(outputPixel + 3) = (alpha1 << 3);
	}
};

class TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB_To_RGBA8 : public TextureDecoder, public SingletonClass<TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB_To_RGBA8>
{
public:
//2656
    sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
    {
        return 4;
    }

    void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
    {
        for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
        {
            sint32 yc = y;
            for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
            {
                uint16* blockData = (uint16*)LatteTextureLoader_GetInput(textureLoader, x, y);
                sint32 pixelOffset = (x + yc * textureLoader->width) * 4;
                uint32 colorData = (*(uint16*)(blockData + 0));
                // swap order of components
                uint8 red = (colorData >> 0) & 0x1F;
                uint8 green = (colorData >> 5) & 0x1F;
                uint8 blue = (colorData >> 10) & 0x1F;
                uint8 alpha = (colorData >> 15) & 0x1;

                red = red << 3 | red >> 2;
                green = green << 3 | green >> 2;
                blue = blue << 3 | blue >> 2;
                alpha = alpha * 0xff;

                // MSB...LSB : ABGR
                colorData = (alpha << 24) | (blue << 16) | (green << 8) | red;
                *(uint32*)(outputData + pixelOffset + 0) = colorData;
            }
        }
    }

    void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
    {
        uint16 colorData = (*(uint16*)blockData);
        uint8 red = (colorData >> 0) & 0x1F;
        uint8 green = (colorData >> 5) & 0x1F;
        uint8 blue = (colorData >> 10) & 0x1F;
        uint8 alpha = (colorData >> 15) & 0x1;
        *(outputPixel + 0) = (red << 3) | (red >> 2);
        *(outputPixel + 1) = (green << 3) | (green >> 2);
        *(outputPixel + 2) = (blue << 3) | (blue >> 2);
        *(outputPixel + 3) = alpha * 0xff;
    }

};

class uint16_R5_G5_B5_A1_swapOpenGL
{
public:
	void operator=(const uint16_R5_G5_B5_A1_swapOpenGL& v)
	{
		uint16 red = (v.internalVal >> 0) & 0x1F;
		uint16 green = (v.internalVal >> 5) & 0x1F;
		uint16 blue = (v.internalVal >> 10) & 0x1F;
		uint16 alpha = (v.internalVal >> 15) & 0x1;
		internalVal = (red << 11) | (green << 6) | (blue << 1) | alpha;
	}
	uint16 internalVal;
};

static_assert(sizeof(uint16_R5_G5_B5_A1_swapOpenGL) == 2);

class TextureDecoder_R5_G5_B5_A1_UNORM_swappedOpenGL : public TextureDecoder, public SingletonClass<TextureDecoder_R5_G5_B5_A1_UNORM_swappedOpenGL>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint16_R5_G5_B5_A1_swapOpenGL, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint16 colorData = (*(uint16*)blockData);
		uint8 red5 = (colorData >> 0) & 0x1F;
		uint8 green5 = (colorData >> 5) & 0x1F;
		uint8 blue5 = (colorData >> 10) & 0x1F;
		uint8 alpha1 = (colorData >> 11) & 0x1;
		*(outputPixel + 0) = (red5 << 3) | (red5 >> 2);
		*(outputPixel + 1) = (green5 << 3) | (green5 >> 2);
		*(outputPixel + 2) = (blue5 << 3) | (blue5 >> 2);
		*(outputPixel + 3) = (alpha1 << 3);
	}
};

class TextureDecoder_A1_B5_G5_R5_UNORM : public TextureDecoder, public SingletonClass<TextureDecoder_A1_B5_G5_R5_UNORM>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint16, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint16 colorData = (*(uint16*)blockData);
		uint8 red5 = (colorData >> 11) & 0x1F;
		uint8 green5 = (colorData >> 6) & 0x1F;
		uint8 blue5 = (colorData >> 1) & 0x1F;
		uint8 alpha1 = (colorData >> 0) & 0x1;
		*(outputPixel + 0) = (red5 << 3) | (red5 >> 2);
		*(outputPixel + 1) = (green5 << 3) | (green5 >> 2);
		*(outputPixel + 2) = (blue5 << 3) | (blue5 >> 2);
		*(outputPixel + 3) = (alpha1 << 3);
	}
};

class TextureDecoder_A1_B5_G5_R5_UNORM_vulkan : public TextureDecoder, public SingletonClass<TextureDecoder_A1_B5_G5_R5_UNORM_vulkan>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 pixelOffset = (x + yc * textureLoader->width) * 2;
				uint16 colorData = (*(uint16*)(blockData + 0));
				// swap order of components
				uint8 red5 = (colorData >> 11) & 0x1F;
				uint8 green5 = (colorData >> 6) & 0x1F;
				uint8 blue5 = (colorData >> 1) & 0x1F;
				uint8 alpha1 = (colorData >> 0) & 0x1;
				colorData = blue5 | (green5 << 5) | (red5 << 10) | (alpha1 << 15);
				*(uint16*)(outputData + pixelOffset + 0) = colorData;
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint16 colorData = (*(uint16*)blockData);
		uint8 red5 = (colorData >> 11) & 0x1F;
		uint8 green5 = (colorData >> 6) & 0x1F;
		uint8 blue5 = (colorData >> 1) & 0x1F;
		uint8 alpha1 = (colorData >> 0) & 0x1;
		*(outputPixel + 0) = (red5 << 3) | (red5 >> 2);
		*(outputPixel + 1) = (green5 << 3) | (green5 >> 2);
		*(outputPixel + 2) = (blue5 << 3) | (blue5 >> 2);
		*(outputPixel + 3) = (alpha1 << 3);
	}
};

class TextureDecoder_A1_B5_G5_R5_UNORM_vulkan_To_RGBA8 : public TextureDecoder, public SingletonClass<TextureDecoder_A1_B5_G5_R5_UNORM_vulkan_To_RGBA8>
{
public:
    sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
    {
        return 4;
    }

    void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
    {
        for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
        {
            sint32 yc = y;
            for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
            {
                uint16* blockData = (uint16*)LatteTextureLoader_GetInput(textureLoader, x, y);
                sint32 pixelOffset = (x + yc * textureLoader->width) * 4;
                uint32 colorData = (*(uint16*)(blockData + 0));
                // swap order of components
                uint8 red = (colorData >> 11) & 0x1F;
                uint8 green = (colorData >> 6) & 0x1F;
                uint8 blue = (colorData >> 1) & 0x1F;
                uint8 alpha = (colorData >> 0) & 0x1;

                red = red << 3 | red >> 2;
                green = green << 3 | green >> 2;
                blue = blue << 3 | blue >> 2;
                alpha = alpha * 0xff;

                // MSB...LSB ABGR
                colorData = red | (green << 8) | (blue << 16) | (alpha << 24);
                *(uint32*)(outputData + pixelOffset + 0) = colorData;
            }
        }
    }

    void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
    {
        uint16 colorData = (*(uint16*)blockData);
        uint8 red5 = (colorData >> 11) & 0x1F;
        uint8 green5 = (colorData >> 6) & 0x1F;
        uint8 blue5 = (colorData >> 1) & 0x1F;
        uint8 alpha1 = (colorData >> 0) & 0x1;
        *(outputPixel + 0) = (red5 << 3) | (red5 >> 2);
        *(outputPixel + 1) = (green5 << 3) | (green5 >> 2);
        *(outputPixel + 2) = (blue5 << 3) | (blue5 >> 2);
        *(outputPixel + 3) = (alpha1 << 3);
    }
};


class TextureDecoder_R10_G10_B10_A2_UNORM : public TextureDecoder, public SingletonClass<TextureDecoder_R10_G10_B10_A2_UNORM>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint32, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint16 r10 = ((*(uint32*)blockData) >> 0) & 0x3FF;
		uint16 g10 = ((*(uint32*)blockData) >> 10) & 0x3FF;
		uint16 b10 = ((*(uint32*)blockData) >> 20) & 0x3FF;
		uint8 a2 = ((*(uint32*)blockData) >> 30) & 0x3;
		*(outputPixel + 0) = (uint8)(r10 >> 6);
		*(outputPixel + 1) = (uint8)(g10 >> 6);
		*(outputPixel + 2) = (uint8)(b10 >> 6);
		*(outputPixel + 3) = a2;
	}
};

class TextureDecoder_R10_G10_B10_A2_UNORM_To_RGBA8 : public TextureDecoder, public SingletonClass<TextureDecoder_R10_G10_B10_A2_UNORM_To_RGBA8>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 pixelOffset = (x + y * textureLoader->width) * 4;
				uint32 v = *(uint32*)blockData;
				*(outputData + pixelOffset + 0) = (uint8)(((v >> 0) & 0x3FF) >> 2);
				*(outputData + pixelOffset + 1) = (uint8)(((v >> 10) & 0x3FF) >> 2);
				*(outputData + pixelOffset + 2) = (uint8)(((v >> 20) & 0x3FF) >> 2);
				*(outputData + pixelOffset + 3) = (uint8)(((v >> 30) & 0x3) * 85);
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint32 v = *(uint32*)blockData;
		*(outputPixel + 0) = (uint8)(((v >> 0) & 0x3FF) >> 2);
		*(outputPixel + 1) = (uint8)(((v >> 10) & 0x3FF) >> 2);
		*(outputPixel + 2) = (uint8)(((v >> 20) & 0x3FF) >> 2);
		*(outputPixel + 3) = (uint8)(((v >> 30) & 0x3) * 85);
	}
};

class TextureDecoder_R10_G10_B10_A2_SNORM_To_RGBA16 : public TextureDecoder, public SingletonClass<TextureDecoder_R10_G10_B10_A2_SNORM_To_RGBA16>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4 * 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		// todo - implement
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			sint32 pixelOffset = (yc * textureLoader->width) * (2 * 4);
			sint16* pixelOutput = (sint16*)(outputData + pixelOffset);
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				uint32 v = (*(uint32*)(blockData + 0));
				*pixelOutput = 0;
				pixelOffset++;
				*pixelOutput = 0;
				pixelOffset++;
				*pixelOutput = 0;
				pixelOffset++;
				*pixelOutput = 0;
				pixelOffset++;
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint16 r10 = ((*(uint32*)blockData) >> 0) & 0x3FF;
		uint16 g10 = ((*(uint32*)blockData) >> 10) & 0x3FF;
		uint16 b10 = ((*(uint32*)blockData) >> 20) & 0x3FF;
		uint8 a2 = ((*(uint32*)blockData) >> 30) & 0x3;
		*(outputPixel + 0) = (uint8)(r10 >> 6) / 2 + 128;
		*(outputPixel + 1) = (uint8)(g10 >> 6) / 2 + 128;
		*(outputPixel + 2) = (uint8)(b10 >> 6) / 2 + 128;
		*(outputPixel + 3) = a2 / 2 + 128;
	}
};

class TextureDecoder_A2_B10_G10_R10_UNORM_To_RGBA16 : public TextureDecoder, public SingletonClass<TextureDecoder_A2_B10_G10_R10_UNORM_To_RGBA16>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4 * 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		// todo - implement
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			sint32 yc = y;
			sint32 pixelOffset = (yc * textureLoader->width) * (2 * 4);
			sint16* pixelOutput = (sint16*)(outputData + pixelOffset);
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				uint32 v = (*(uint32*)(blockData + 0));
				*pixelOutput = 0;
				pixelOffset++;
				*pixelOutput = 0;
				pixelOffset++;
				*pixelOutput = 0;
				pixelOffset++;
				*pixelOutput = 0;
				pixelOffset++;
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		uint8 a2 = ((*(uint32*)blockData) >> 0) & 0x3;
		uint16 r10 = ((*(uint32*)blockData) >> 2) & 0x3FF;
		uint16 g10 = ((*(uint32*)blockData) >> 12) & 0x3FF;
		uint16 b10 = ((*(uint32*)blockData) >> 22) & 0x3FF;
		*(outputPixel + 0) = (uint8)(r10 >> 6);
		*(outputPixel + 1) = (uint8)(g10 >> 6);
		*(outputPixel + 2) = (uint8)(b10 >> 6);
		*(outputPixel + 3) = a2;
	}
};

class TextureDecoder_R11_G11_B10_FLOAT : public TextureDecoder, public SingletonClass<TextureDecoder_R11_G11_B10_FLOAT>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint32, 1, false, false>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// todo: add dumping support
	}
};

class TextureDecoder_BC1_UNORM_uncompress_generic : public TextureDecoder
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4 * 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 blockSizeX = (std::min)(4, textureLoader->width - x);
				sint32 blockSizeY = (std::min)(4, textureLoader->height - y);
				// decode 4x4 pixels at once
				float rgbaBlock[4 * 4 * 4];
				decodeBC1Block(blockData, rgbaBlock);
				for (sint32 py = 0; py < blockSizeY; py++)
				{
					sint32 yc = y + py;
					for (sint32 px = 0; px < blockSizeX; px++)
					{
						sint32 pixelOffset = (x + px + yc * textureLoader->width) * 16; // write to target buffer
						float red = rgbaBlock[(px + py * 4) * 4 + 0];
						float green = rgbaBlock[(px + py * 4) * 4 + 1];
						float blue = rgbaBlock[(px + py * 4) * 4 + 2];
						float alpha = rgbaBlock[(px + py * 4) * 4 + 3];
						*(float*)(outputData + pixelOffset + 0) = red;
						*(float*)(outputData + pixelOffset + 4) = green;
						*(float*)(outputData + pixelOffset + 8) = blue;
						*(float*)(outputData + pixelOffset + 12) = alpha;
					}
				}
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		BC1_GetPixel(blockData, blockOffsetX, blockOffsetY, outputPixel);
	}
};

class TextureDecoder_BC1_UNORM_uncompress : public TextureDecoder_BC1_UNORM_uncompress_generic, public SingletonClass<TextureDecoder_BC1_UNORM_uncompress>
{
	// reuse TextureDecoder_BC1_UNORM_uncompress_generic
};

class TextureDecoder_BC1_SRGB_uncompress : public TextureDecoder_BC1_UNORM_uncompress_generic, public SingletonClass<TextureDecoder_BC1_SRGB_uncompress>
{
	// reuse TextureDecoder_BC1_UNORM_uncompress_generic
};

class TextureDecoder_BC1 : public TextureDecoder, public SingletonClass<TextureDecoder_BC1>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 8;
	}

	sint32 getTexelCountX(LatteTextureLoaderCtx* textureLoader) override
	{
		return (textureLoader->width + 3) / 4;
	}

	sint32 getTexelCountY(LatteTextureLoaderCtx* textureLoader) override
	{
		return (textureLoader->height + 3) / 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint64, 1, false, true>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		BC1_GetPixel(blockData, blockOffsetX, blockOffsetY, outputPixel);
	}
};

class TextureDecoder_BC1_To_R8G8B8A8 : public TextureDecoder, public SingletonClass<TextureDecoder_BC1_To_R8G8B8A8>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 blockSizeX = (std::min)(4, textureLoader->width - x);
				sint32 blockSizeY = (std::min)(4, textureLoader->height - y);
				// decode 4x4 pixels at once
				float rgbaBlock[4 * 4 * 4];
				decodeBC1Block(blockData, rgbaBlock);
				for (sint32 py = 0; py < blockSizeY; py++)
				{
					sint32 yc = y + py;
					for (sint32 px = 0; px < blockSizeX; px++)
					{
						sint32 pixelOffset = (x + px + yc * textureLoader->width) * 4; // write to target buffer
						float red = rgbaBlock[(px + py * 4) * 4 + 0];
						float green = rgbaBlock[(px + py * 4) * 4 + 1];
						float blue = rgbaBlock[(px + py * 4) * 4 + 2];
						float alpha = rgbaBlock[(px + py * 4) * 4 + 3];
						*(outputData + pixelOffset + 0) = red * 255;
						*(outputData + pixelOffset + 1) = green * 255;
						*(outputData + pixelOffset + 2) = blue * 255;
						*(outputData + pixelOffset + 3) = alpha * 255;
					}
				}
			}
		}
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		return;
	}
};

class TextureDecoder_BC2_To_R8G8B8A8 : public TextureDecoder, public SingletonClass<TextureDecoder_BC2_To_R8G8B8A8>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 blockSizeX = (std::min)(4, textureLoader->width - x);
				sint32 blockSizeY = (std::min)(4, textureLoader->height - y);
				// decode 4x4 pixels at once
				float rgbaBlock[4 * 4 * 4];
				decodeBC2Block_UNORM(blockData, rgbaBlock);
				for (sint32 py = 0; py < blockSizeY; py++)
				{
					sint32 yc = y + py;
					for (sint32 px = 0; px < blockSizeX; px++)
					{
						sint32 pixelOffset = (x + px + yc * textureLoader->width) * 4; // write to target buffer
						float red = rgbaBlock[(px + py * 4) * 4 + 0];
						float green = rgbaBlock[(px + py * 4) * 4 + 1];
						float blue = rgbaBlock[(px + py * 4) * 4 + 2];
						float alpha = rgbaBlock[(px + py * 4) * 4 + 3];
						*(outputData + pixelOffset + 0) = red * 255;
						*(outputData + pixelOffset + 1) = green * 255;
						*(outputData + pixelOffset + 2) = blue * 255;
						*(outputData + pixelOffset + 3) = alpha * 255;
					}
				}
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		return;
	}
};

class TextureDecoder_BC2 : public TextureDecoder, public SingletonClass<TextureDecoder_BC2>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4 * 4;
	}

	sint32 getTexelCountX(LatteTextureLoaderCtx* textureLoader) override
	{
		return (textureLoader->width + 3) / 4;
	}

	sint32 getTexelCountY(LatteTextureLoaderCtx* textureLoader) override
	{
		return (textureLoader->height + 3) / 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint64, 2, false, true>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float rgbaBlock[4 * 4 * 4];
		decodeBC2Block_UNORM(blockData, rgbaBlock);
		float red = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 0];
		float green = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 1];
		float blue = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 2];
		float alpha = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 3];
		*(outputPixel + 0) = (uint8)(red * 255.0f);
		*(outputPixel + 1) = (uint8)(green * 255.0f);
		*(outputPixel + 2) = (uint8)(blue * 255.0f);
		*(outputPixel + 3) = (uint8)(alpha * 255.0f);
	}
};

class TextureDecoder_BC2_UNORM_uncompress : public TextureDecoder, public SingletonClass<TextureDecoder_BC2_UNORM_uncompress>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4 * 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 blockSizeX = (std::min)(4, textureLoader->width - x);
				sint32 blockSizeY = (std::min)(4, textureLoader->height - y);
				// decode 4x4 pixels at once
				float rgbaBlock[4 * 4 * 4];
				decodeBC2Block_UNORM(blockData, rgbaBlock);
				for (sint32 py = 0; py < blockSizeY; py++)
				{
					sint32 yc = y + py;
					for (sint32 px = 0; px < blockSizeX; px++)
					{
						sint32 pixelOffset = (x + px + yc * textureLoader->width) * 16; // write to target buffer
						float red = rgbaBlock[(px + py * 4) * 4 + 0];
						float green = rgbaBlock[(px + py * 4) * 4 + 1];
						float blue = rgbaBlock[(px + py * 4) * 4 + 2];
						float alpha = rgbaBlock[(px + py * 4) * 4 + 3];
						*(float*)(outputData + pixelOffset + 0) = red;
						*(float*)(outputData + pixelOffset + 4) = green;
						*(float*)(outputData + pixelOffset + 8) = blue;
						*(float*)(outputData + pixelOffset + 12) = alpha;
					}
				}
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float rgbaBlock[4 * 4 * 4];
		decodeBC2Block_UNORM(blockData, rgbaBlock);
		float red = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 0];
		float green = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 1];
		float blue = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 2];
		float alpha = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 3];
		*(outputPixel + 0) = (uint8)(red * 255.0f);
		*(outputPixel + 1) = (uint8)(green * 255.0f);
		*(outputPixel + 2) = (uint8)(blue * 255.0f);
		*(outputPixel + 3) = (uint8)(alpha * 255.0f);
	}
};

class TextureDecoder_BC2_SRGB_uncompress : public TextureDecoder, public SingletonClass<TextureDecoder_BC2_SRGB_uncompress>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4 * 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		// todo - apply srgb conversion
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 blockSizeX = (std::min)(4, textureLoader->width - x);
				sint32 blockSizeY = (std::min)(4, textureLoader->height - y);
				// decode 4x4 pixels at once
				float rgbaBlock[4 * 4 * 4];
				decodeBC2Block_UNORM(blockData, rgbaBlock);
				for (sint32 py = 0; py < blockSizeY; py++)
				{
					sint32 yc = y + py;
					for (sint32 px = 0; px < blockSizeX; px++)
					{
						sint32 pixelOffset = (x + px + yc * textureLoader->width) * 16; // write to target buffer
						float red = rgbaBlock[(px + py * 4) * 4 + 0];
						float green = rgbaBlock[(px + py * 4) * 4 + 1];
						float blue = rgbaBlock[(px + py * 4) * 4 + 2];
						float alpha = rgbaBlock[(px + py * 4) * 4 + 3];
						*(float*)(outputData + pixelOffset + 0) = red;
						*(float*)(outputData + pixelOffset + 4) = green;
						*(float*)(outputData + pixelOffset + 8) = blue;
						*(float*)(outputData + pixelOffset + 12) = alpha;
					}
				}
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// todo - apply srgb conversion
		float rgbaBlock[4 * 4 * 4];
		decodeBC2Block_UNORM(blockData, rgbaBlock);
		float red = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 0];
		float green = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 1];
		float blue = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 2];
		float alpha = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 3];
		*(outputPixel + 0) = (uint8)(red * 255.0f);
		*(outputPixel + 1) = (uint8)(green * 255.0f);
		*(outputPixel + 2) = (uint8)(blue * 255.0f);
		*(outputPixel + 3) = (uint8)(alpha * 255.0f);
	}
};

class TextureDecoder_BC3_uncompress_generic : public TextureDecoder
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4 * 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 blockSizeX = (std::min)(4, textureLoader->width - x);
				sint32 blockSizeY = (std::min)(4, textureLoader->height - y);
				// decode 4x4 pixels at once
				float rgbaBlock[4 * 4 * 4];
				decodeBC3Block_UNORM(blockData, rgbaBlock);
				for (sint32 py = 0; py < blockSizeY; py++)
				{
					sint32 yc = y + py;
					for (sint32 px = 0; px < blockSizeX; px++)
					{
						sint32 pixelOffset = (x + px + yc * textureLoader->width) * 16; // write to target buffer
						float red = rgbaBlock[(px + py * 4) * 4 + 0];
						float green = rgbaBlock[(px + py * 4) * 4 + 1];
						float blue = rgbaBlock[(px + py * 4) * 4 + 2];
						float alpha = rgbaBlock[(px + py * 4) * 4 + 3];
						*(float*)(outputData + pixelOffset + 0) = red;
						*(float*)(outputData + pixelOffset + 4) = green;
						*(float*)(outputData + pixelOffset + 8) = blue;
						*(float*)(outputData + pixelOffset + 12) = alpha;
					}
				}
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float rgbaBlock[4 * 4 * 4];
		decodeBC3Block_UNORM(blockData, rgbaBlock);
		float red = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 0];
		float green = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 1];
		float blue = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 2];
		float alpha = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 3];
		*(outputPixel + 0) = (uint8)(red * 255.0f);
		*(outputPixel + 1) = (uint8)(green * 255.0f);
		*(outputPixel + 2) = (uint8)(blue * 255.0f);
		*(outputPixel + 3) = (uint8)(alpha * 255.0f);
	}
};

class TextureDecoder_BC3_To_R8G8B8A8 : public TextureDecoder, public SingletonClass<TextureDecoder_BC3_To_R8G8B8A8>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 blockSizeX = (std::min)(4, textureLoader->width - x);
				sint32 blockSizeY = (std::min)(4, textureLoader->height - y);
				// decode 4x4 pixels at once
				float rgbaBlock[4 * 4 * 4];
				decodeBC3Block_UNORM(blockData, rgbaBlock);
				for (sint32 py = 0; py < blockSizeY; py++)
				{
					sint32 yc = y + py;
					for (sint32 px = 0; px < blockSizeX; px++)
					{
						sint32 pixelOffset = (x + px + yc * textureLoader->width) * 4; // write to target buffer
						float red = rgbaBlock[(px + py * 4) * 4 + 0];
						float green = rgbaBlock[(px + py * 4) * 4 + 1];
						float blue = rgbaBlock[(px + py * 4) * 4 + 2];
						float alpha = rgbaBlock[(px + py * 4) * 4 + 3];
						*(outputData + pixelOffset + 0) = (uint8)(red * 255);
						*(outputData + pixelOffset + 1) = (uint8)(green * 255);
						*(outputData + pixelOffset + 2) = (uint8)(blue * 255);
						*(outputData + pixelOffset + 3) = (uint8)(alpha * 255);
					}
				}
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		return;
	}
};

class TextureDecoder_BC3_UNORM_uncompress : public TextureDecoder_BC3_uncompress_generic, public SingletonClass<TextureDecoder_BC3_UNORM_uncompress>
{
	// reuse TextureDecoder_BC3_uncompress_generic
};

class TextureDecoder_BC3_SRGB_uncompress : public TextureDecoder_BC3_uncompress_generic, public SingletonClass<TextureDecoder_BC3_SRGB_uncompress>
{
	// reuse TextureDecoder_BC3_uncompress_generic
};

class TextureDecoder_BC3 : public TextureDecoder, public SingletonClass<TextureDecoder_BC3>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 16;
	}

	sint32 getTexelCountX(LatteTextureLoaderCtx* textureLoader) override
	{
		return (textureLoader->width + 3) / 4;
	}

	sint32 getTexelCountY(LatteTextureLoaderCtx* textureLoader) override
	{
		return (textureLoader->height + 3) / 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint64, 2, false, true>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float rgbaBlock[4 * 4 * 4];
		decodeBC3Block_UNORM(blockData, rgbaBlock);
		float red = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 0];
		float green = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 1];
		float blue = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 2];
		float alpha = rgbaBlock[(blockOffsetX + blockOffsetY * 4) * 4 + 3];
		*(outputPixel + 0) = (uint8)(red * 255.0f);
		*(outputPixel + 1) = (uint8)(green * 255.0f);
		*(outputPixel + 2) = (uint8)(blue * 255.0f);
		*(outputPixel + 3) = (uint8)(alpha * 255.0f);
	}
};

class TextureDecoder_BC4_UNORM_uncompress : public TextureDecoder, public SingletonClass<TextureDecoder_BC4_UNORM_uncompress>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2 * 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 blockSizeX = (std::min)(4, textureLoader->width - x);
				sint32 blockSizeY = (std::min)(4, textureLoader->height - y);
				// decode 4x4 pixels at once
				float rBlock[4 * 4 * 1];
				decodeBC4Block_UNORM(blockData, rBlock);

				for (sint32 py = 0; py < blockSizeY; py++)
				{
					sint32 yc = y + py;
					for (sint32 px = 0; px < blockSizeX; px++)
					{
						sint32 pixelOffset = (x + px + yc * textureLoader->width) * 8; // write to target buffer
						float red = rBlock[(px + py * 4) * 1 + 0];
						*(float*)(outputData + pixelOffset + 0) = red;
						*(float*)(outputData + pixelOffset + 4) = 0.0f;
					}
				}
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float rBlock[4 * 4 * 1];
		decodeBC4Block_UNORM(blockData, rBlock);
		float red = rBlock[(blockOffsetX + blockOffsetY * 4) * 1 + 0];
		*(outputPixel + 0) = (uint8)(red * 255.0f);
		*(outputPixel + 1) = 0;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};


class TextureDecoder_BC4_To_R8 : public TextureDecoder, public SingletonClass<TextureDecoder_BC4_To_R8>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 1;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 blockSizeX = (std::min)(4, textureLoader->width - x);
				sint32 blockSizeY = (std::min)(4, textureLoader->height - y);
				// decode 4x4 pixels at once
				float rBlock[4 * 4 * 1];
				decodeBC4Block_UNORM(blockData, rBlock);

				for (sint32 py = 0; py < blockSizeY; py++)
				{
					sint32 yc = y + py;
					for (sint32 px = 0; px < blockSizeX; px++)
					{
						sint32 pixelOffset = (x + px + yc * textureLoader->width); // write to target buffer
						float red = rBlock[(px + py * 4) * 1 + 0];
						*(outputData + pixelOffset + 0) = (uint8)(red * 255);
					}
				}
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float rBlock[4 * 4 * 1];
		decodeBC4Block_UNORM(blockData, rBlock);
		float red = rBlock[(blockOffsetX + blockOffsetY * 4) * 1 + 0];
		*(outputPixel + 0) = (uint8)(red * 255.0f);
		*(outputPixel + 1) = 0;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

// BC4 → RGBA8 fallback that BROADCASTS the single channel to R, G, B and A.
// Used on platforms where BC4 is unsupported (e.g. Mali Vulkan) AND the driver
// fails to honour the texture view's component swizzle for R8_UNORM samples.
// Native BC4 returns (R, 0, 0, 1); the game's GPU swizzle is normally what turns
// that into (R, R, R, R) for grayscale-mask use. By baking the broadcast into the
// data we no longer depend on that swizzle pathway working.
class TextureDecoder_BC4_To_R8G8B8A8_Broadcast : public TextureDecoder, public SingletonClass<TextureDecoder_BC4_To_R8G8B8A8_Broadcast>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 blockSizeX = (std::min)(4, textureLoader->width - x);
				sint32 blockSizeY = (std::min)(4, textureLoader->height - y);
				float rBlock[4 * 4 * 1];
				decodeBC4Block_UNORM(blockData, rBlock);

				for (sint32 py = 0; py < blockSizeY; py++)
				{
					sint32 yc = y + py;
					for (sint32 px = 0; px < blockSizeX; px++)
					{
						sint32 pixelOffset = (x + px + yc * textureLoader->width) * 4;
						uint8 v = (uint8)(rBlock[(px + py * 4) * 1 + 0] * 255);
						outputData[pixelOffset + 0] = v;
						outputData[pixelOffset + 1] = v;
						outputData[pixelOffset + 2] = v;
						outputData[pixelOffset + 3] = v;
					}
				}
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float rBlock[4 * 4 * 1];
		decodeBC4Block_UNORM(blockData, rBlock);
		uint8 v = (uint8)(rBlock[(blockOffsetX + blockOffsetY * 4) * 1 + 0] * 255.0f);
		outputPixel[0] = v;
		outputPixel[1] = v;
		outputPixel[2] = v;
		outputPixel[3] = v;
	}
};

class TextureDecoder_BC4 : public TextureDecoder, public SingletonClass<TextureDecoder_BC4>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 8;
	}

	sint32 getTexelCountX(LatteTextureLoaderCtx* textureLoader) override
	{
		return (textureLoader->width + 3) / 4;
	}

	sint32 getTexelCountY(LatteTextureLoaderCtx* textureLoader) override
	{
		return (textureLoader->height + 3) / 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint64, 1, false, true>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float rBlock[4 * 4 * 1];
		decodeBC4Block_UNORM(blockData, rBlock);
		float red = rBlock[(blockOffsetX + blockOffsetY * 4) * 1 + 0];
		*(outputPixel + 0) = (uint8)(red * 255.0f);
		*(outputPixel + 1) = 0;
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};
template<decodingFn fn>
class TextureDecoder_BC5_To_R8G8 : public TextureDecoder, public SingletonClass<TextureDecoder_BC5_To_R8G8<fn>>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 blockSizeX = (std::min)(4, textureLoader->width - x);
				sint32 blockSizeY = (std::min)(4, textureLoader->height - y);
				// decode 4x4 pixels at once
				float rgBlock[4 * 4 * 2];
				fn(blockData, rgBlock);

				for (sint32 py = 0; py < blockSizeY; py++)
				{
					sint32 yc = y + py;
					for (sint32 px = 0; px < blockSizeX; px++)
					{
						sint32 pixelOffset = (x + px + yc * textureLoader->width) * 2; // write to target buffer
						float red = rgBlock[(px + py * 4) * 2 + 0];
						float green = rgBlock[(px + py * 4) * 2 + 1];
						*(outputData + pixelOffset + 0) = (uint8)(red * 255);
						*(outputData + pixelOffset + 1) = (uint8)(green * 255);
					}
				}
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float rgBlock[4 * 4 * 2];
		decodeBC5Block_UNORM(blockData, rgBlock);
		float red = rgBlock[(blockOffsetX + blockOffsetY * 4) * 2 + 0];
		float green = rgBlock[(blockOffsetX + blockOffsetY * 4) * 2 + 1];
		*(outputPixel + 0) = (uint8)(red * 255.0f);
		*(outputPixel + 1) = (uint8)(green * 255.0f);
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

// BC5 → RGBA8 fallback. Decodes the 2-channel BC5 data as (R, G, 0, 255) and lets the
// view's VkComponentMapping do the per-game-channel routing. The image is created without
// VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT (see LatteTextureVk constructor) to dodge a Mali
// driver bug that misinterprets mutable-format RGBA8 images derived from a compressed
// fallback as single-channel R8 sampling.
template<void(*fn)(uint8*, float*)>
class TextureDecoder_BC5_To_R8G8B8A8 : public TextureDecoder, public SingletonClass<TextureDecoder_BC5_To_R8G8B8A8<fn>>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 blockSizeX = (std::min)(4, textureLoader->width - x);
				sint32 blockSizeY = (std::min)(4, textureLoader->height - y);
				float rgBlock[4 * 4 * 2];
				fn(blockData, rgBlock);

				for (sint32 py = 0; py < blockSizeY; py++)
				{
					sint32 yc = y + py;
					for (sint32 px = 0; px < blockSizeX; px++)
					{
						sint32 pixelOffset = (x + px + yc * textureLoader->width) * 4;
						uint8 r = (uint8)(rgBlock[(px + py * 4) * 2 + 0] * 255);
						uint8 g = (uint8)(rgBlock[(px + py * 4) * 2 + 1] * 255);
						outputData[pixelOffset + 0] = r;
						outputData[pixelOffset + 1] = g;
						outputData[pixelOffset + 2] = 0;
						outputData[pixelOffset + 3] = 255;
					}
				}
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float rgBlock[4 * 4 * 2];
		fn(blockData, rgBlock);
		outputPixel[0] = (uint8)(rgBlock[(blockOffsetX + blockOffsetY * 4) * 2 + 0] * 255.0f);
		outputPixel[1] = (uint8)(rgBlock[(blockOffsetX + blockOffsetY * 4) * 2 + 1] * 255.0f);
		outputPixel[2] = 0;
		outputPixel[3] = 255;
	}
};

// EAC R11 modifier tables (16 tables, 8 entries each) — Khronos ETC2/EAC spec.
// Per-pixel decoded value (UNORM 11-bit) = clamp(base_codeword*8 + 4 + multiplier_factor *
// table[table_index][pixel_3bit_index], 0, 2047) where multiplier_factor = (M==0)?1:(M*8).
inline constexpr int8_t s_eacModifierTable[16][8] = {
	{ -3, -6,  -9, -15,  2,  5,  8, 14 },
	{ -3, -7, -10, -13,  2,  6,  9, 12 },
	{ -2, -5,  -8, -13,  1,  4,  7, 12 },
	{ -2, -4,  -6, -13,  1,  3,  5, 12 },
	{ -3, -6,  -8, -12,  2,  5,  7, 11 },
	{ -3, -7,  -9, -11,  2,  6,  8, 10 },
	{ -4, -7, -10, -13,  3,  6,  9, 12 },
	{ -3, -5,  -8, -11,  2,  4,  7, 10 },
	{ -2, -6,  -8, -10,  1,  5,  7,  9 },
	{ -2, -5,  -8, -10,  1,  4,  7,  9 },
	{ -2, -4,  -8, -10,  1,  3,  7,  9 },
	{ -2, -5,  -7, -10,  1,  4,  6,  9 },
	{ -3, -4,  -7, -10,  2,  3,  6,  9 },
	{ -1, -2,  -3, -10,  0,  1,  2,  9 },
	{ -4, -6,  -8,  -9,  3,  5,  7,  8 },
	{ -3, -5,  -7,  -9,  2,  4,  6,  8 },
};

// Encodes 16 input values (8-bit UNORM range or signed for SNORM) into one EAC R11 channel
// (8 bytes). Searches a coarse subset of (table, multiplier) combos and picks the best fit
// based on squared error. Input pixel ordering is EAC's column-major (pixel index = col*4+row).
inline void LatteEncodeEacR11Channel(const int* values11_in, bool isSnorm, uint8_t* outBytes)
{
	// Clamp values into the correct range first
	int vmin11 = isSnorm ?  1023 : 2047;
	int vmax11 = isSnorm ? -1024 :    0;
	int v[16];
	for (int i = 0; i < 16; i++)
	{
		int q = values11_in[i];
		if (isSnorm)
		{
			if (q < -1023) q = -1023;
			if (q >  1023) q =  1023;
		}
		else
		{
			if (q < 0) q = 0;
			if (q > 2047) q = 2047;
		}
		v[i] = q;
		if (q < vmin11) vmin11 = q;
		if (q > vmax11) vmax11 = q;
	}

	// Candidate (multiplier_4bit, table_index) pairs — coarse but spans the useful space.
	// M=0 is a special "no scale" mode (factor=1); other M values scale modifier by M*8.
	static constexpr struct { int M; int table; } s_candidates[] = {
		{ 0,  0}, { 0,  6}, { 0, 13}, { 0, 15},
		{ 1,  0}, { 1,  6}, { 1, 13}, { 1, 15},
		{ 2,  0}, { 2,  6}, { 2, 13}, { 2, 15},
		{ 4,  0}, { 4,  6}, { 4, 13}, { 4, 15},
		{ 6,  0}, { 6,  6}, { 6, 13}, { 6, 15},
		{ 8,  0}, { 8,  6}, { 8, 13}, { 8, 15},
		{10,  0}, {10,  6}, {10, 13}, {10, 15},
		{12,  0}, {12,  6}, {12, 13}, {12, 15},
		{14,  0}, {14,  6}, {14, 13}, {14, 15},
		{15,  0}, {15,  6}, {15, 13}, {15, 15},
	};
	constexpr int NUM_CANDIDATES = sizeof(s_candidates) / sizeof(s_candidates[0]);

	int best_error = 0x7FFFFFFF;
	int best_base = 0, best_M = 0, best_table = 0;
	uint8_t best_indices[16] = {0};

	for (int c = 0; c < NUM_CANDIDATES; c++)
	{
		const int M = s_candidates[c].M;
		const int table = s_candidates[c].table;
		const int mfactor = (M == 0) ? 1 : (M * 8);

		// Heuristic base = midpoint of data range, mapped to base_codeword.
		// base_11 = base_codeword * 8 + 4 (for UNORM); base_codeword is signed for SNORM.
		const int target_mid = (vmin11 + vmax11) / 2;
		int center_codeword = (target_mid - 4) / 8;
		if (isSnorm)
		{
			if (center_codeword < -127) center_codeword = -127;
			if (center_codeword >  127) center_codeword =  127;
		}
		else
		{
			if (center_codeword < 0) center_codeword = 0;
			if (center_codeword > 255) center_codeword = 255;
		}

		// Refine base by trying a few values around the heuristic
		for (int db = -2; db <= 2; db++)
		{
			const int bcw = center_codeword + db;
			if (isSnorm) { if (bcw < -127 || bcw > 127) continue; }
			else { if (bcw < 0 || bcw > 255) continue; }
			const int base_11 = (isSnorm ? bcw : bcw) * 8 + (isSnorm ? 0 : 4);

			uint8_t indices[16];
			int error = 0;
			for (int p = 0; p < 16; p++)
			{
				const int target = v[p];
				int best_e = 0x7FFFFFFF, best_i = 0;
				for (int idx = 0; idx < 8; idx++)
				{
					int decoded = base_11 + mfactor * s_eacModifierTable[table][idx];
					if (isSnorm)
					{
						if (decoded < -1023) decoded = -1023;
						if (decoded >  1023) decoded =  1023;
					}
					else
					{
						if (decoded < 0) decoded = 0;
						if (decoded > 2047) decoded = 2047;
					}
					const int e = decoded - target;
					const int e2 = e * e;
					if (e2 < best_e) { best_e = e2; best_i = idx; }
				}
				indices[p] = (uint8_t)best_i;
				error += best_e;
				if (error >= best_error) break; // early-out: can't beat current best
			}

			if (error < best_error)
			{
				best_error = error;
				best_base = bcw;
				best_M = M;
				best_table = table;
				for (int i = 0; i < 16; i++) best_indices[i] = indices[i];
			}
		}
	}

	// Pack into 8 bytes (big-endian per EAC spec):
	//   bits 63..56 base codeword (signed for SNORM)
	//   bits 55..52 multiplier (4 bits)
	//   bits 51..48 table index (4 bits)
	//   bits 47..0  16 × 3-bit pixel indices (pixel 0 at bits 45..47, pixel 15 at bits 0..2)
	uint64_t bits = 0;
	bits |= (uint64_t)((uint32_t)best_base & 0xFFu) << 56;
	bits |= (uint64_t)(best_M & 0xF) << 52;
	bits |= (uint64_t)(best_table & 0xF) << 48;
	for (int p = 0; p < 16; p++)
	{
		const int shift = 45 - p * 3;
		bits |= ((uint64_t)(best_indices[p] & 7)) << shift;
	}
	outBytes[0] = (uint8_t)(bits >> 56);
	outBytes[1] = (uint8_t)(bits >> 48);
	outBytes[2] = (uint8_t)(bits >> 40);
	outBytes[3] = (uint8_t)(bits >> 32);
	outBytes[4] = (uint8_t)(bits >> 24);
	outBytes[5] = (uint8_t)(bits >> 16);
	outBytes[6] = (uint8_t)(bits >>  8);
	outBytes[7] = (uint8_t)(bits >>  0);
}

// BC5 → EAC R11G11 transcoder. Used as the Mali fallback for BC5_UNORM / BC5_SNORM since
// Mali doesn't expose BC5 but does expose EAC R11G11 natively. The transcoded image stays
// compressed (same 16 bytes / 4×4 block as BC5) and Mali samples it via its native ETC2
// hardware path, sidestepping the texture-view component-swizzle quirk that breaks the
// (R, G, 0, 255) RGBA8 widening fallback in Wind Waker HD (red-screen bug).
//
// `fn` is decodeBC5Block_UNORM or decodeBC5Block_SNORM. `isSnorm` selects the EAC variant.
template<void(*fn)(uint8*, float*), bool isSnorm>
class TextureDecoder_BC5_To_EAC_R11G11 : public TextureDecoder, public SingletonClass<TextureDecoder_BC5_To_EAC_R11G11<fn, isSnorm>>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 16; // EAC R11G11: 16 bytes per 4x4 block (matches native BC5 layout)
	}

	sint32 getTexelCountX(LatteTextureLoaderCtx* textureLoader) override
	{
		return (textureLoader->width + 3) / 4;
	}

	sint32 getTexelCountY(LatteTextureLoaderCtx* textureLoader) override
	{
		return (textureLoader->height + 3) / 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		const sint32 blocksPerRow = (textureLoader->width + 3) / 4;

		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				float rgBlock[16 * 2]; // row-major, R and G interleaved
				fn(blockData, rgBlock);

				// Reorder to EAC's column-major layout and quantize to 11-bit.
				// EAC pixel index for (col=px, row=py) is px*4 + py.
				// BC5 decoder output for the same pixel is at index py*4 + px.
				int rVals[16];
				int gVals[16];
				for (int px = 0; px < 4; px++)
				{
					for (int py = 0; py < 4; py++)
					{
						const int eacIdx = px * 4 + py;
						const int srcIdx = (py * 4 + px) * 2;
						if (isSnorm)
						{
							rVals[eacIdx] = (int)(rgBlock[srcIdx + 0] * 1023.0f + (rgBlock[srcIdx + 0] >= 0.0f ? 0.5f : -0.5f));
							gVals[eacIdx] = (int)(rgBlock[srcIdx + 1] * 1023.0f + (rgBlock[srcIdx + 1] >= 0.0f ? 0.5f : -0.5f));
						}
						else
						{
							rVals[eacIdx] = (int)(rgBlock[srcIdx + 0] * 2047.0f + 0.5f);
							gVals[eacIdx] = (int)(rgBlock[srcIdx + 1] * 2047.0f + 0.5f);
						}
					}
				}

				// Output: 16 bytes per block, R channel first (8 bytes), then G channel (8 bytes).
				const sint32 blockX = x / 4;
				const sint32 blockY = y / 4;
				uint8_t* outBlock = outputData + (blockY * blocksPerRow + blockX) * 16;
				LatteEncodeEacR11Channel(rVals, isSnorm, outBlock);
				LatteEncodeEacR11Channel(gVals, isSnorm, outBlock + 8);
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// Used only for dumping. Reuse the BC5 decode and produce RGBA8 (R, G, 0, 255).
		float rgBlock[16 * 2];
		fn(blockData, rgBlock);
		const int srcIdx = (blockOffsetY * 4 + blockOffsetX) * 2;
		float r = rgBlock[srcIdx + 0];
		float g = rgBlock[srcIdx + 1];
		if (isSnorm) { r = r * 0.5f + 0.5f; g = g * 0.5f + 0.5f; }
		if (r < 0.0f) r = 0.0f; if (r > 1.0f) r = 1.0f;
		if (g < 0.0f) g = 0.0f; if (g > 1.0f) g = 1.0f;
		outputPixel[0] = (uint8_t)(r * 255.0f);
		outputPixel[1] = (uint8_t)(g * 255.0f);
		outputPixel[2] = 0;
		outputPixel[3] = 255;
	}
};

class TextureDecoder_BC5_UNORM_uncompress : public TextureDecoder, public SingletonClass<TextureDecoder_BC5_UNORM_uncompress>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2 * 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 blockSizeX = (std::min)(4, textureLoader->width - x);
				sint32 blockSizeY = (std::min)(4, textureLoader->height - y);
				// decode 4x4 pixels at once
				float rgBlock[4 * 4 * 2];
				decodeBC5Block_UNORM(blockData, rgBlock);

				for (sint32 py = 0; py < blockSizeY; py++)
				{
					sint32 yc = y + py;
					for (sint32 px = 0; px < blockSizeX; px++)
					{
						sint32 pixelOffset = (x + px + yc * textureLoader->width) * 8; // write to target buffer
						float red = rgBlock[(px + py * 4) * 2 + 0];
						float green = rgBlock[(px + py * 4) * 2 + 1];
						*(float*)(outputData + pixelOffset + 0) = red;
						*(float*)(outputData + pixelOffset + 4) = green;
					}
				}
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float rgBlock[4 * 4 * 2];
		decodeBC5Block_UNORM(blockData, rgBlock);
		float red = rgBlock[(blockOffsetX + blockOffsetY * 4) * 2 + 0];
		float green = rgBlock[(blockOffsetX + blockOffsetY * 4) * 2 + 1];
		*(outputPixel + 0) = (uint8)(red * 255.0f);
		*(outputPixel + 1) = (uint8)(green * 255.0f);
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_BC5_SNORM_uncompress : public TextureDecoder, public SingletonClass<TextureDecoder_BC5_SNORM_uncompress>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 2 * 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX)
			{
				uint8* blockData = LatteTextureLoader_GetInput(textureLoader, x, y);
				sint32 blockSizeX = (std::min)(4, textureLoader->width - x);
				sint32 blockSizeY = (std::min)(4, textureLoader->height - y);
				// decode 4x4 pixels at once
				float rgBlock[4 * 4 * 2];
				decodeBC5Block_SNORM(blockData, rgBlock);

				for (sint32 py = 0; py < blockSizeY; py++)
				{
					sint32 yc = y + py;
					for (sint32 px = 0; px < blockSizeX; px++)
					{
						sint32 pixelOffset = (x + px + yc * textureLoader->width) * 8; // write to target buffer
						float red = rgBlock[(px + py * 4) * 2 + 0];
						float green = rgBlock[(px + py * 4) * 2 + 1];
						*(float*)(outputData + pixelOffset + 0) = red;
						*(float*)(outputData + pixelOffset + 4) = green;
					}
				}
			}
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// todo: fix BC5 SNORM dumping
		float rgBlock[4 * 4 * 2];
		decodeBC5Block_SNORM(blockData, rgBlock);
		float red = rgBlock[(blockOffsetX + blockOffsetY * 4) * 2 + 0];
		float green = rgBlock[(blockOffsetX + blockOffsetY * 4) * 2 + 1];
		*(outputPixel + 0) = (uint8)((0.5f + red * 0.5f) * 255.0f);
		*(outputPixel + 1) = (uint8)((0.5f + green * 0.5f) * 255.0f);
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};

class TextureDecoder_BC5 : public TextureDecoder, public SingletonClass<TextureDecoder_BC5>
{
public:

	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 16;
	}

	sint32 getTexelCountX(LatteTextureLoaderCtx* textureLoader) override
	{
		return (textureLoader->width + 3) / 4;
	}

	sint32 getTexelCountY(LatteTextureLoaderCtx* textureLoader) override
	{
		return (textureLoader->height + 3) / 4;
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<uint64, 2, false, true>(textureLoader, outputData);
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		float rgBlock[4 * 4 * 2];
		decodeBC5Block_UNORM(blockData, rgBlock);
		float red = rgBlock[(blockOffsetX + blockOffsetY * 4) * 2 + 0];
		float green = rgBlock[(blockOffsetX + blockOffsetY * 4) * 2 + 1];
		*(outputPixel + 0) = (uint8)(red * 255.0f);
		*(outputPixel + 1) = (uint8)(green * 255.0f);
		*(outputPixel + 2) = 0;
		*(outputPixel + 3) = 255;
	}
};
