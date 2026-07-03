// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY
//
// PupDmdMatch - PuPCapture DMD frame matcher (portable, no Windows deps)
//
// PuP-packs with DMD-matched triggers ("D<n>" trigger expressions) ship a
// PupCapture folder of reference frames: 24bpp 128x32 BMPs named by
// trigger number (52.bmp fires D52).  Magenta pixels (~253,0,253) are
// wildcards ("don't care"); everything else is an expected DMD pixel.
//
// Matching model: binary lit/unlit comparison with a tolerance.  A
// reference pixel is "lit" if its brightest channel clears a threshold; a
// live DMD pixel is "lit" if its raw level is nonzero.  A pattern matches
// when the fraction of agreeing non-wildcard pixels meets matchFraction
// (1.0 = exact).  Real DMD frames captured live differ slightly from the
// authored reference (animation phase, level quantization), so exact
// match rarely fires - a small tolerance is what real PuP uses too.
//
// Representation: each pattern's lit/care planes are packed into 64-bit
// words (4096 pixels = 64 words) so a frame is compared with word-wide
// XOR/AND/popcount instead of per-byte branches - the frame is binarized
// once per frame, not once per pattern.  The live frame arrives as one
// byte per pixel (raw pinmame levels; any nonzero = lit), row-major,
// top-down.

#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace pup
{
	class DmdMatcher
	{
	public:
		static const int W = 128, H = 32;
		static const int WORDS = (W * H) / 64;   // 64 uint64 words = 4096 bits

		// One reference pattern (fixed 128x32), packed to bit planes
		struct Pattern
		{
			int id = 0;                 // trigger number (from the file name)
			uint64_t lit[WORDS] = {};   // expected lit bit per pixel
			uint64_t care[WORDS] = {};   // 1 = compare, 0 = wildcard (magenta)
			bool isMatched = false;     // currently matching (transition state)
			// Exact-color mode only: the reference frame's full RGB (3 bytes
			// per pixel, R,G,B, row-major top-down) plus the precomputed list
			// of non-wildcard pixel indices to compare.  Empty/zero in the
			// default luminance mode (which uses the packed lit/care planes).
			std::vector<uint8_t> rgb;
			std::vector<uint16_t> careIdx;   // pixel indices with care==1
		};

		// Load all <number>.bmp patterns from a PupCapture directory
		// (UTF-8 path).  Returns the number of patterns loaded; if
		// bmpCandidates is non-null, it receives the count of numeric-named
		// BMP files seen (so the caller can distinguish "no BMPs" from
		// "BMPs present but none usable at this DMD size").
		int LoadDir(const std::string &dirUtf8, int *bmpCandidates = nullptr);

		// Parse one 24bpp BMP buffer into a pattern.  Exposed for tests.
		bool AddPattern(int id, const uint8_t *bmp, size_t len);

		// Fraction of non-wildcard pixels that must agree for a match
		// (0..1; 1.0 = exact).  Default exact.
		void SetMatchFraction(float f) { matchFraction = f; }

		// Enable exact-color matching (PuPCapture ExactColorMatch.txt).  Must
		// be set BEFORE LoadDir/AddPattern so patterns retain their full RGB.
		// In this mode OnFrameColor compares per-pixel RGB (within colorTol per
		// channel) instead of the lit/unlit Jaccard metric.
		void SetExactColor(bool e) { exactColor = e; }
		bool IsExactColor() const { return exactColor; }

		// Process a live COLOR frame: 3 bytes per pixel (R,G,B), row-major
		// top-down.  Only meaningful in exact-color mode; emits the same
		// (triggerNum, state) transitions as OnFrame.
		void OnFrameColor(const uint8_t *rgb, int w, int h,
			const std::function<void(int num, int state)> &emit);

		// Process a live frame: one byte per pixel, row-major top-down,
		// any nonzero = lit.  Frames of other sizes are ignored.  Emits
		// (triggerNum, state) for each pattern whose match state changed:
		// state 1 when the frame starts matching, 0 when it stops.
		void OnFrame(const uint8_t *px, int w, int h,
			const std::function<void(int num, int state)> &emit);

		// number of loaded patterns
		size_t GetPatternCount() const { return patterns.size(); }

		// reset match state (new game)
		void Reset() { for (auto &p : patterns) p.isMatched = false; }

	protected:
		std::vector<Pattern> patterns;
		float matchFraction = 1.0f;
		bool exactColor = false;
		// Per-channel tolerance for exact-color matching.  Fixed at 0 (exact):
		// colorized DMDs use a discrete palette, so live and reference colors
		// are bit-identical.  Kept as a named seam for a future config knob.
		int colorTol = 0;
	};
}
