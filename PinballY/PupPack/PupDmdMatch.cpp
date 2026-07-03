// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY
//
// PuPCapture DMD frame matcher - see PupDmdMatch.h

#include "PupDmdMatch.h"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstdlib>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace fs = std::filesystem;

namespace pup
{

// Apply a match result to a pattern, emitting the 0/1 edge on a change.  The
// transition bookkeeping is identical for the luminance and color paths, so
// it lives here once (tiny and header-free, so it inlines into both loops).
static inline void EmitTransition(DmdMatcher::Pattern &pat, bool match,
	const std::function<void(int, int)> &emit)
{
	if (match && !pat.isMatched)
	{
		pat.isMatched = true;
		emit(pat.id, 1);
	}
	else if (!match && pat.isMatched)
	{
		pat.isMatched = false;
		emit(pat.id, 0);
	}
}

// little-endian field readers (BMP headers are packed little-endian)
static inline uint32_t RdU32(const uint8_t *p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int32_t RdI32(const uint8_t *p) { return (int32_t)RdU32(p); }
static inline uint16_t RdU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// portable 64-bit popcount
static inline int PopCount64(uint64_t v)
{
#if defined(_MSC_VER) && defined(_M_X64)
	return (int)__popcnt64(v);
#elif defined(_MSC_VER) && defined(_M_IX86)
	// PinballY.exe is a 32-bit build: MSVC x86 has no __popcnt64, but the
	// 32-bit __popcnt intrinsic covers the DMD matcher's hot loop.
	return (int)(__popcnt((unsigned)v) + __popcnt((unsigned)(v >> 32)));
#elif defined(__GNUC__)
	return __builtin_popcountll(v);
#else
	v = v - ((v >> 1) & 0x5555555555555555ull);
	v = (v & 0x3333333333333333ull) + ((v >> 2) & 0x3333333333333333ull);
	v = (v + (v >> 4)) & 0x0f0f0f0f0f0f0f0full;
	return (int)((v * 0x0101010101010101ull) >> 56);
#endif
}

static inline void SetBit(uint64_t *words, int i) { words[i >> 6] |= (1ull << (i & 63)); }

bool DmdMatcher::AddPattern(int id, const uint8_t *bmp, size_t len)
{
	// validate the header: uncompressed 24bpp, exactly our DMD size.
	// (Height may be negative for top-down rows.)
	if (len < 54 || bmp[0] != 'B' || bmp[1] != 'M')
		return false;
	uint32_t pxOff = RdU32(bmp + 10);
	int32_t w = RdI32(bmp + 18), h = RdI32(bmp + 22);
	uint16_t bpp = RdU16(bmp + 28);
	uint32_t compression = RdU32(bmp + 30);
	bool topDown = h < 0;
	if (topDown)
	{
		if (h == INT32_MIN)   // -INT32_MIN is signed-overflow UB
			return false;
		h = -h;
	}
	if (w != W || h != H || bpp != 24 || compression != 0)
		return false;
	size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
	size_t span = stride * (size_t)h;   // w,h are validated constants here
	// Bounds check written to avoid the pxOff + span overflow: pxOff is an
	// attacker-controlled uint32 from the BMP header, and on a 32-bit build
	// size_t is 32-bit, so the naive sum wraps and a huge pxOff would pass.
	if (pxOff > len || span > len - pxOff)
		return false;

	Pattern pat;
	pat.id = id;
	// In exact-color mode, keep the reference frame's full RGB so OnFrameColor
	// can compare actual colors (wildcards stay zero but are skipped via care).
	if (exactColor)
		pat.rgb.assign((size_t)W * H * 3, 0);
	int carePixels = 0;
	for (int y = 0; y < H; ++y)
	{
		// BMP rows are bottom-up unless the height was negative
		const uint8_t *row = bmp + pxOff + stride * (topDown ? y : (H - 1 - y));
		for (int x = 0; x < W; ++x)
		{
			uint8_t b = row[x*3], g = row[x*3 + 1], r = row[x*3 + 2];
			int i = y * W + x;
			if (r >= 200 && b >= 200 && g < 100)
				continue;   // magenta wildcard: leave care bit clear
			SetBit(pat.care, i);
			++carePixels;
			uint8_t maxch = r > g ? (r > b ? r : b) : (g > b ? g : b);
			if (maxch >= 32)
				SetBit(pat.lit, i);
			if (exactColor)
			{
				pat.rgb[(size_t)i*3 + 0] = r;
				pat.rgb[(size_t)i*3 + 1] = g;
				pat.rgb[(size_t)i*3 + 2] = b;
				pat.careIdx.push_back((uint16_t)i);
			}
		}
	}

	// an all-wildcard pattern would match everything; reject it
	if (carePixels == 0)
		return false;

	patterns.push_back(pat);
	return true;
}

int DmdMatcher::LoadDir(const std::string &dirUtf8, int *bmpCandidates)
{
	int n = 0;
	if (bmpCandidates != nullptr)
		*bmpCandidates = 0;
	std::error_code ec;
	fs::path dir = fs::u8path(dirUtf8);
	for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
	{
		if (!it->is_regular_file(ec))
			continue;
		fs::path p = it->path();
		std::string ext = p.extension().u8string();
		if (ext.size() != 4 || (ext[1] != 'b' && ext[1] != 'B'))
			continue;

		// the file name (sans extension) is the trigger number
		std::string stem = p.stem().u8string();
		char *end = nullptr;
		long id = strtol(stem.c_str(), &end, 10);
		if (end == stem.c_str() || *end != 0 || id < 0)
			continue;

		// a numeric-named BMP is a pattern candidate: count it so the
		// caller can tell "no patterns" from "patterns present but none
		// usable at this DMD size" without walking the directory again
		if (bmpCandidates != nullptr)
			++*bmpCandidates;

		std::ifstream f(p, std::ios::binary);
		if (!f)
			continue;
		std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if (AddPattern((int)id, buf.data(), buf.size()))
			++n;
	}
	return n;
}

void DmdMatcher::OnFrame(const uint8_t *px, int w, int h,
	const std::function<void(int num, int state)> &emit)
{
	if (px == nullptr || w != W || h != H)
		return;

	// binarize the live frame once into bit words (nonzero = lit)
	uint64_t frame[WORDS] = {};
	for (int i = 0, n = W * H; i < n; ++i)
		if (px[i] != 0)
			frame[i >> 6] |= (1ull << (i & 63));

	// loop-invariant tolerance factor, hoisted out of the per-pattern loop
	// (the opaque emit() call otherwise blocks the compiler from caching it)
	const float slack = 1.0f - matchFraction;

	for (auto &pat : patterns)
	{
		// Agreement is measured over the UNION of lit pixels (the pixels
		// lit in the pattern or the frame), not over all pixels: DMD
		// frames are mostly black, so counting shared background would let
		// any sparse frame match any sparse pattern.  This is the Jaccard
		// ratio of the two lit shapes - mismatched lit pixels over their
		// union - which is what actually distinguishes DMD content.
		int mismatch = 0, uni = 0;
		for (int j = 0; j < WORDS; ++j)
		{
			uint64_t care = pat.care[j];
			mismatch += PopCount64((frame[j] ^ pat.lit[j]) & care);   // disagreeing lit pixels
			uni += PopCount64((frame[j] | pat.lit[j]) & care);        // union of lit pixels
		}
		// Budget rounds rather than truncates, so a small lit shape keeps a
		// proportional tolerance instead of collapsing to exact-match.
		// uni == 0 means the frame and pattern agree on an all-dark care
		// region (a blank/DMD-off reference matching a blank frame) - a
		// perfect match (mismatch is necessarily 0 then), so it is NOT
		// excluded; the union denominator already prevents a lit pattern
		// from matching a blank frame.
		int budget = (int)(uni * slack + 0.5f);
		bool match = mismatch <= budget;

		EmitTransition(pat, match, emit);
	}
}

void DmdMatcher::OnFrameColor(const uint8_t *rgb, int w, int h,
	const std::function<void(int num, int state)> &emit)
{
	if (rgb == nullptr || w != W || h != H)
		return;

	const float slack = 1.0f - matchFraction;

	for (auto &pat : patterns)
	{
		// exact-color match needs the reference RGB (and its care-pixel list);
		// a pattern loaded outside color mode has none, so it can't take part
		if (pat.rgb.empty())
			continue;

		// Count non-wildcard pixels whose live RGB differs from the reference
		// by more than colorTol on any channel.  Unlike the luminance path,
		// this is a straight per-pixel color compare (the whole point of
		// ExactColorMatch), with matchFraction giving the same count slack.
		// careIdx is the precomputed list of pixels to compare, so there is no
		// per-frame full scan or wildcard bit-test.
		int mismatch = 0;
		for (uint16_t idx : pat.careIdx)
		{
			const uint8_t *lp = &rgb[(size_t)idx*3], *rp = &pat.rgb[(size_t)idx*3];
			if (std::abs(lp[0] - rp[0]) > colorTol
				|| std::abs(lp[1] - rp[1]) > colorTol
				|| std::abs(lp[2] - rp[2]) > colorTol)
				++mismatch;
		}

		int care = (int)pat.careIdx.size();
		int budget = (int)(care * slack + 0.5f);
		bool match = care > 0 && mismatch <= budget;

		EmitTransition(pat, match, emit);
	}
}

}
