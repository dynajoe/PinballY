// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY
//
// PupPackTool — headless harness for the portable PUP-pack core.
// ---------------------------------------------------------------
// Loads a pack from disk (default: the bundled synthetic fixture), prints the
// parsed model, then drives a scripted sequence of machine events through the
// TriggerEngine and prints the resolved PlayCommands. This proves the
// parse -> trigger -> playlist -> screen resolution path with no Direct3D / MFC
// and no real game attached, so it can run on any platform in CI.
//
// Build (from PinballY/PupPack):
//   cl  /std:c++17 /EHsc /I. tool\PupPackTool.cpp PupLoader.cpp PupTriggerEngine.cpp
//   g++ -std=c++17 -I. tool/PupPackTool.cpp PupLoader.cpp PupTriggerEngine.cpp -o puppacktool
//
// Run:
//   PupPackTool [packDir]
// packDir defaults to tool/fixture/PUPVIDEOS/testrom relative to the cwd.

#include "PupLoader.h"
#include "PupTriggerEngine.h"
#include "PupDmdMatch.h"
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace pup;

// Print a media path relative to the pack root so output is identical
// regardless of where the repo is checked out.
static std::string Rel(const Pack& pack, const std::string& abs)
{
	if (abs.empty()) return "(stop)";
	std::error_code ec;
	fs::path r = fs::relative(fs::u8path(abs), fs::u8path(pack.rootPath), ec);
	std::string s = ec ? abs : r.generic_u8string();
	return s.empty() ? abs : s;
}

static void PrintCommand(const Pack& pack, const PlayCommand& c)
{
	std::cout << "      -> screen " << c.screenNum
	          << " | " << Rel(pack, c.mediaPath)
	          << " | pri " << c.priority
	          << " | vol " << c.volume
	          << (c.loop ? " | loop" : "")
	          << " | from '" << c.sourceTriggerId << "'\n";
}

static void DumpPack(const Pack& pack)
{
	std::cout << "Pack: rom='" << pack.romName << "'  root='" << pack.rootPath << "'\n";
	std::cout << "  Screens (" << pack.screens.size() << "):\n";
	for (auto& s : pack.screens)
		std::cout << "    #" << s.screenNum << "  " << s.description
		          << (s.backgroundPlaylist.empty() ? "" : ("  bg=" + s.backgroundPlaylist)) << "\n";
	std::cout << "  Playlists (" << pack.playlists.size() << "):\n";
	for (auto& p : pack.playlists)
		std::cout << "    " << p.folder << "  screen=" << p.screenNum
		          << "  pri=" << p.priority << "  vol=" << p.volume
		          << "  files=" << p.files.size()
		          << (p.files.empty() ? "" : (" [" + p.files.front() + " ...]")) << "\n";
	std::cout << "  Triggers (" << pack.triggers.size() << "):\n";
	for (auto& t : pack.triggers)
		std::cout << "    '" << t.id << "'  when " << t.triggerExpr
		          << " -> screen " << t.screenNum << " / " << t.playlist
		          << "  pri=" << t.priority << "  rest=" << t.restSeconds << "\n";
}

// One scripted machine event with a virtual timestamp.
struct ScriptStep { double t; char type; int num; int state; const char* note; };

int main(int argc, char** argv)
{
	std::string packDir = (argc > 1)
		? argv[1]
		: "tool/fixture/PUPVIDEOS/testrom";

	Pack pack;
	LoadResult lr = LoadPack(packDir, pack);
	for (auto& w : lr.warnings)
		std::cout << "warning: " << w << "\n";
	if (!lr.ok)
	{
		std::cerr << "load failed: " << lr.error << "\n";
		return 2;
	}

	DumpPack(pack);

	// Scripted sequence exercising: resolution, priority preempt, priority
	// suppress, and restSeconds debounce. See the inline notes.
	static const ScriptStep script[] = {
		{ 0.0, 'L',  1, 1, "attract on -> screen 2 (pri 1)" },
		{ 0.5, 'W',  4, 1, "bonus -> screen 1 (pri 3)" },
		{ 1.0, 'S', 16, 1, "multiball -> screen 1 (pri 10) PREEMPTS bonus; combo (W4+S16) -> screen 3" },
		{ 1.5, 'S', 20, 1, "jackpot -> screen 2 (pri 5) PREEMPTS attract" },
		{ 2.5, 'S', 20, 0, "jackpot off" },
		{ 3.0, 'S', 20, 1, "jackpot re-fire @3.0 -> DEBOUNCED (rest 2, last 1.5)" },
		{ 3.5, 'S', 20, 0, "jackpot off" },
		{ 5.0, 'S', 20, 1, "jackpot re-fire @5.0 -> fires (3.5s since last)" },
		{ 6.0, 'W',  4, 0, "bonus + combo conditions off" },
		{ 6.5, 'W',  4, 1, "bonus again (pri 3) SUPPRESSED by pri 10; combo re-fires on screen 3" },
	};

	std::cout << "\nEvent trace:\n";
	TriggerEngine engine(pack);
	for (auto& s : script)
	{
		std::cout << "  t=" << s.t << "  " << s.type << s.num << "=" << s.state
		          << "   [" << s.note << "]\n";
		auto cmds = engine.Post({ s.type, s.num, s.state }, s.t);
		if (cmds.empty())
			std::cout << "      (no command)\n";
		for (auto& c : cmds)
			PrintCommand(pack, c);
	}

	std::cout << "\nFinal active media by screen:\n";
	for (auto& kv : engine.ActiveByScreen())
		std::cout << "  screen " << kv.first << ": " << Rel(pack, kv.second.mediaPath)
		          << " (pri " << kv.second.priority << ")\n";

	// ------------------------------------------------------------------
	// Regression tests: engine arbitration and inheritance semantics.
	// Hard assertions with a nonzero exit code on failure, so this can
	// run unattended.  These lock down the rules that gameplay triggers
	// depend on: sentinel-free playlist inheritance, arbitration release
	// on media end/stop, and script-media participation in arbitration.
	std::cout << "\nEngine semantics tests:\n";
	int failures = 0;
	auto expect = [&failures](bool cond, const char* what)
	{
		std::cout << (cond ? "  pass: " : "  FAIL: ") << what << "\n";
		if (!cond) ++failures;
	};

	{
		TriggerEngine e(pack);

		// Sentinels: t_jackpot has an explicit Volume=100 on a volume-70
		// playlist and must keep it; t_pupevent leaves Volume empty and
		// must inherit the playlist's 70.
		auto r1 = e.Post({ 'S', 20, 1 }, 0.0);
		expect(r1.size() == 1 && r1[0].volume == 100,
			"explicit Volume=100 kept, not playlist-inherited");
		auto r2 = e.Post({ 'E', 801, 1 }, 0.5);
		expect(r2.size() == 1 && r2[0].volume == 70,
			"empty Volume inherits the playlist volume");
		e.Post({ 'E', 801, 0 }, 0.6);

		// Arbitration release on media end: after the pri-7 pupevent media
		// "finishes" (host calls ClearActive), the pri-5 jackpot trigger
		// can claim screen 2 again.
		e.ClearActive(2);
		e.Post({ 'S', 20, 0 }, 5.0);
		auto r3 = e.Post({ 'S', 20, 1 }, 6.0);
		expect(r3.size() == 1,
			"lower-priority trigger plays after media end releases the screen");

		// Script-driven media participates in arbitration: a SetActive
		// pri-9 entry suppresses the pri-5 trigger; a script stop
		// (SetActive with empty media) releases the screen.
		PlayCommand ext;
		ext.screenNum = 2;
		ext.priority = 9;
		ext.mediaPath = "x";
		e.SetActive(ext);
		e.Post({ 'S', 20, 0 }, 7.0);
		auto r4 = e.Post({ 'S', 20, 1 }, 8.5);
		expect(r4.empty(), "script media (pri 9) suppresses a lower-priority trigger");
		ext.mediaPath.clear();
		e.SetActive(ext);
		e.Post({ 'S', 20, 0 }, 9.0);
		auto r5 = e.Post({ 'S', 20, 1 }, 11.5);
		expect(r5.size() == 1, "script stop releases the screen for triggers");
		expect(e.ActiveByScreen().count(2) == 1, "played media is recorded per screen");
	}

	{
		// Hot-path early-outs: a device no trigger references fires nothing,
		// and a redundant same-state report is a no-op (no spurious re-fire),
		// while a real 0->1 edge after it still fires.
		TriggerEngine e(pack);
		expect(e.Post({ 'L', 999, 1 }, 0.0).empty(), "unreferenced device fires nothing");
		auto a = e.Post({ 'L', 1, 1 }, 0.1);        // attract on -> fires
		expect(a.size() == 1, "L1 rising edge fires");
		expect(e.Post({ 'L', 1, 1 }, 0.2).empty(), "repeated same state is a no-op");
		e.Post({ 'L', 1, 0 }, 0.3);
		expect(e.Post({ 'L', 1, 1 }, 0.4).size() == 1, "genuine re-edge fires again");
	}

	{
		// Multi-condition trigger: t_combo ("W4=1,S16" -> screen 3, pri 4)
		// is an AND of two devices, so it must fire only when the second
		// condition completes the pair - whichever order they arrive in -
		// and must re-arm when either condition drops.  W4 also fires
		// t_bonus and S16 also fires t_mb (both on screen 1), so assert on
		// the combo's presence by trigger id rather than on result counts.
		auto fromCombo = [](const std::vector<PlayCommand>& v)
		{
			for (auto& c : v)
				if (c.sourceTriggerId == "t_combo") return true;
			return false;
		};

		// W first: the switch alone must not fire it; the solenoid
		// completes the pair.
		TriggerEngine e(pack);
		auto c1 = e.Post({ 'W', 4, 1 }, 0.0);
		expect(!fromCombo(c1), "combo: first condition alone does not fire (W first)");
		auto c2 = e.Post({ 'S', 16, 1 }, 0.5);
		expect(fromCombo(c2), "combo: fires when S16 completes a held W4=1");

		// Drop one condition and re-raise it: the rising edge must re-fire.
		auto c3 = e.Post({ 'S', 16, 0 }, 1.0);
		expect(!fromCombo(c3), "combo: no fire when a condition drops");
		auto c4 = e.Post({ 'S', 16, 1 }, 1.5);
		expect(fromCombo(c4), "combo: re-fires after conditions drop and return");

		// S first on a fresh engine: same outcome with the opposite
		// arrival order.
		TriggerEngine e2(pack);
		auto c5 = e2.Post({ 'S', 16, 1 }, 0.0);
		expect(!fromCombo(c5), "combo: first condition alone does not fire (S first)");
		auto c6 = e2.Post({ 'W', 4, 1 }, 0.5);
		expect(fromCombo(c6), "combo: fires when W4=1 completes a held S16");
	}

	{
		// Playlist randomization: the fixture's Multiball playlist is
		// AlphaSort=0, so each t_mb fire draws a random member (mb01/mb02).
		// The engine's PRNG is seeded, so the sequence - and this test -
		// is deterministic across runs.
		TriggerEngine e(pack);
		bool allMembers = true, saw1 = false, saw2 = false;
		for (int n = 0; n < 10; ++n)
		{
			auto r = e.Post({ 'S', 16, 1 }, n * 2.0);
			e.Post({ 'S', 16, 0 }, n * 2.0 + 1.0);
			std::string f;
			for (auto& c : r)
				if (c.sourceTriggerId == "t_mb")
					f = fs::u8path(c.mediaPath).filename().u8string();
			if (f == "mb01.mp4")      saw1 = true;
			else if (f == "mb02.mp4") saw2 = true;
			else                      allMembers = false;
		}
		expect(allMembers, "random playlist (AlphaSort=0) always picks a playlist member");
		expect(saw1 && saw2, "random playlist reaches both files across 10 plays");
	}

	{
		// Per-playlist restSeconds: the Rest playlist (restSeconds=3) gates
		// replays from ANY trigger targeting it; t_restA (own rest 10) and
		// t_restB (no rest of its own) both play it on screen 5, so the two
		// windows can be told apart on the virtual clock.
		TriggerEngine e(pack);
		auto p1 = e.Post({ 'W', 40, 1 }, 0.0);
		expect(p1.size() == 1, "playlist rest: first play goes through");
		auto p2 = e.Post({ 'W', 41, 1 }, 1.0);
		expect(p2.empty(), "playlist rest blocks an immediate replay from a different trigger");
		e.Post({ 'W', 41, 0 }, 1.5);
		auto p3 = e.Post({ 'W', 41, 1 }, 4.0);
		expect(p3.size() == 1, "playlist rest allows the replay after the window elapses");

		// t_restA's own rest (10s) still applies on top: at t=8 the playlist
		// window (3s from the t=4 play) has passed, but the trigger's hasn't.
		e.Post({ 'W', 40, 0 }, 4.5);
		auto p4 = e.Post({ 'W', 40, 1 }, 8.0);
		expect(p4.empty(), "per-trigger rest still enforced on top of the playlist rest");
		e.Post({ 'W', 40, 0 }, 8.5);
		auto p5 = e.Post({ 'W', 40, 1 }, 12.0);
		expect(p5.size() == 1, "playlist rest and trigger rest both elapsed -> replay allowed");
	}

	{
		// Counter rotation: t_rot1/t_rot2 share the expression W30 and carry
		// Counter 1/2 (listed in triggers.pup in reverse Counter order), so
		// consecutive rising edges walk them round-robin in Counter order
		// and wrap.
		const Trigger* rot1 = nullptr;
		for (auto& t : pack.triggers)
			if (t.id == "t_rot1") rot1 = &t;
		expect(rot1 != nullptr && rot1->counter == 1, "Counter column parsed from triggers.pup");

		auto firedBy = [](const std::vector<PlayCommand>& v) {
			return v.size() == 1 ? v[0].sourceTriggerId : std::string();
		};
		TriggerEngine e(pack);
		auto k1 = e.Post({ 'W', 30, 1 }, 0.0); e.Post({ 'W', 30, 0 }, 0.5);
		auto k2 = e.Post({ 'W', 30, 1 }, 1.0); e.Post({ 'W', 30, 0 }, 1.5);
		auto k3 = e.Post({ 'W', 30, 1 }, 2.0);
		expect(firedBy(k1) == "t_rot1" && firedBy(k2) == "t_rot2",
			"counter rotation fires same-expression rows in Counter order");
		expect(firedBy(k3) == "t_rot1", "counter rotation wraps back to the first row");
	}

	{
		// PlayAction "SetBG": the command plays AND becomes the screen's new
		// background, so it must be flagged for the host and loop.
		TriggerEngine e(pack);
		auto b1 = e.Post({ 'W', 50, 1 }, 0.0);
		expect(b1.size() == 1 && b1[0].setAsBackground && b1[0].loop,
			"SetBG resolves with setAsBackground (and loops)");
	}

	{
		// PlayAction "SkipSamePrty": yields to an equal-priority incumbent
		// where a plain Play preempts it.  t_jackpot (Play) and t_skip
		// (SkipSamePrty) both land on screen 2 at priority 5.
		TriggerEngine e(pack);
		e.Post({ 'S', 20, 1 }, 0.0);   // t_jackpot claims screen 2 at pri 5
		auto s1 = e.Post({ 'W', 51, 1 }, 1.0);
		expect(s1.empty(), "SkipSamePrty does not preempt an equal-priority active entry");
		e.Post({ 'S', 20, 0 }, 2.0);
		auto s2 = e.Post({ 'S', 20, 1 }, 3.0);
		expect(s2.size() == 1, "a plain Play preempts at equal priority (SkipSamePrty contrast)");
		e.ClearActive(2);              // media "ends": the screen goes idle
		e.Post({ 'W', 51, 0 }, 3.5);
		auto s3 = e.Post({ 'W', 51, 1 }, 4.0);
		expect(s3.size() == 1 && s3[0].skipSamePriority,
			"SkipSamePrty plays normally onto an idle screen");
	}

	// ------------------------------------------------------------------
	// PuPCapture DMD matcher tests: BMP parsing (bottom-up rows, magenta
	// wildcards), match enter/leave transitions, and size rejection.
	{
		// build a synthetic 128x32 24bpp BMP: a lit 2x2 block at (10,10)
		// (top-down coordinates), one magenta wildcard at (0,0), rest black
		const int W = pup::DmdMatcher::W, H = pup::DmdMatcher::H;
		const int stride = W * 3, pxOff = 54;
		std::vector<uint8_t> bmp(pxOff + stride * H, 0);
		bmp[0] = 'B'; bmp[1] = 'M';
		auto put32 = [&bmp](int off, uint32_t v) {
			bmp[off] = v & 0xff; bmp[off+1] = (v >> 8) & 0xff;
			bmp[off+2] = (v >> 16) & 0xff; bmp[off+3] = (v >> 24) & 0xff;
		};
		put32(2, (uint32_t)bmp.size()); put32(10, pxOff); put32(14, 40);
		put32(18, W); put32(22, H);              // positive height = bottom-up
		bmp[26] = 1; bmp[28] = 24;               // planes, bpp
		auto px = [&bmp, stride, pxOff, H](int x, int y) -> uint8_t* {
			return &bmp[pxOff + stride * (H - 1 - y) + x * 3];   // bottom-up
		};
		for (int y = 10; y < 12; ++y)
			for (int x = 10; x < 12; ++x)
			{ auto p = px(x, y); p[0] = 0; p[1] = 69; p[2] = 255; }   // lit orange
		{ auto p = px(0, 0); p[0] = 253; p[1] = 0; p[2] = 253; }      // magenta wildcard

		pup::DmdMatcher m;
		expect(m.AddPattern(52, bmp.data(), bmp.size()), "DMD pattern BMP parses");

		std::vector<int> events;   // encoded num*10 + state
		auto emit = [&events](int num, int state) { events.push_back(num * 10 + state); };

		// frame 1: exact match (block lit, wildcard pixel lit - ignored)
		std::vector<uint8_t> frame((size_t)W * H, 0);
		for (int y = 10; y < 12; ++y)
			for (int x = 10; x < 12; ++x)
				frame[(size_t)y * W + x] = 3;
		frame[0] = 3;   // wildcard position - must not matter
		m.OnFrame(frame.data(), W, H, emit);
		expect(events.size() == 1 && events[0] == 521, "DMD match fires D52=1 (wildcard ignored)");

		// frame 2: same frame again - no repeated event
		m.OnFrame(frame.data(), W, H, emit);
		expect(events.size() == 1, "unchanged match doesn't re-fire");

		// frame 3: extra lit pixel breaks the match -> leave event
		frame[(size_t)20 * W + 20] = 2;
		m.OnFrame(frame.data(), W, H, emit);
		expect(events.size() == 2 && events[1] == 520, "match leave fires D52=0");

		// wrong-size frames are ignored
		m.OnFrame(frame.data(), 64, 16, emit);
		expect(events.size() == 2, "wrong-size frame ignored");

		// Tolerance is measured over the union of lit pixels (Jaccard), so
		// use a realistically-sized lit shape.  Build a 20x10 = 200-pixel
		// lit block pattern; a frame that differs by a few pixels within
		// that block must still match at 90%, and a mostly-black frame
		// (the DMD false-positive case) must NOT.
		{
			std::vector<uint8_t> big(pxOff + stride * H, 0);
			big[0] = 'B'; big[1] = 'M';
			auto p32 = [&big](int off, uint32_t v) {
				big[off]=v&0xff; big[off+1]=(v>>8)&0xff; big[off+2]=(v>>16)&0xff; big[off+3]=(v>>24)&0xff; };
			p32(2,(uint32_t)big.size()); p32(10,pxOff); p32(14,40);
			p32(18,W); p32(22,H); big[26]=1; big[28]=24;
			auto bpx = [&big,stride,pxOff,H](int x,int y){ return &big[pxOff+stride*(H-1-y)+x*3]; };
			for (int y = 5; y < 15; ++y)
				for (int x = 20; x < 40; ++x)
				{ auto p=bpx(x,y); p[0]=0; p[1]=69; p[2]=255; }

			pup::DmdMatcher mt;
			mt.AddPattern(52, big.data(), big.size());
			mt.SetMatchFraction(0.90f);
			std::vector<int> tev;
			auto temit = [&tev](int num, int state) { tev.push_back(num * 10 + state); };

			// near frame: same block minus 8 pixels -> 8/200 = 4% off -> match
			std::vector<uint8_t> nearFrame((size_t)W * H, 0);
			for (int y = 5; y < 15; ++y)
				for (int x = 20; x < 40; ++x)
					nearFrame[(size_t)y * W + x] = 3;
			for (int k = 0; k < 8; ++k)
				nearFrame[(size_t)5 * W + (20 + k)] = 0;
			mt.OnFrame(nearFrame.data(), W, H, temit);
			expect(tev.size() == 1 && tev[0] == 521, "tolerance match fires within budget");

			// mostly-black frame must NOT match a lit pattern (the DMD
			// black-dominance false positive the union metric prevents)
			std::vector<uint8_t> blackFrame((size_t)W * H, 0);
			blackFrame[0] = 3;
			mt.OnFrame(blackFrame.data(), W, H, temit);
			expect(tev.size() == 2 && tev[1] == 520,
				"sparse/black frame does not false-match a lit pattern");

			// wrong-size frames are ignored
			mt.OnFrame(nearFrame.data(), 64, 16, temit);
			expect(tev.size() == 2, "wrong-size frame ignored");
		}

		// An all-dark reference pattern (blank/DMD-off capture: all care,
		// no lit pixels) must match a blank live frame - the union metric's
		// uni==0 case is a real match, not an exclusion.
		{
			std::vector<uint8_t> dark(pxOff + stride * H, 0);   // all black, no magenta
			dark[0] = 'B'; dark[1] = 'M';
			auto p32 = [&dark](int off, uint32_t v) {
				dark[off]=v&0xff; dark[off+1]=(v>>8)&0xff; dark[off+2]=(v>>16)&0xff; dark[off+3]=(v>>24)&0xff; };
			p32(2,(uint32_t)dark.size()); p32(10,pxOff); p32(14,40);
			p32(18,W); p32(22,H); dark[26]=1; dark[28]=24;

			pup::DmdMatcher md;
			expect(md.AddPattern(7, dark.data(), dark.size()), "all-dark pattern loads");
			std::vector<int> dev;
			auto demit = [&dev](int num, int state) { dev.push_back(num * 10 + state); };
			std::vector<uint8_t> blank((size_t)W * H, 0);
			md.OnFrame(blank.data(), W, H, demit);
			expect(dev.size() == 1 && dev[0] == 71, "all-dark pattern matches a blank frame");
			// a lit frame must break the all-dark match
			std::vector<uint8_t> litFrame((size_t)W * H, 0);
			for (int i = 0; i < 100; ++i) litFrame[i] = 3;
			md.OnFrame(litFrame.data(), W, H, demit);
			expect(dev.size() == 2 && dev[1] == 70, "all-dark pattern leaves on a lit frame");
		}

		// Exact-color matching (PuPCapture ExactColorMatch.txt): two frames
		// identical in lit/unlit but different in COLOR must be distinguished,
		// which the default lit/unlit metric cannot do.
		{
			std::vector<uint8_t> cb(pxOff + stride * H, 0);
			cb[0] = 'B'; cb[1] = 'M';
			auto c32 = [&cb](int off, uint32_t v) {
				cb[off]=v&0xff; cb[off+1]=(v>>8)&0xff; cb[off+2]=(v>>16)&0xff; cb[off+3]=(v>>24)&0xff; };
			c32(2,(uint32_t)cb.size()); c32(10,pxOff); c32(14,40);
			c32(18,W); c32(22,H); cb[26]=1; cb[28]=24;
			auto cpx = [&cb,stride,pxOff,H](int x,int y){ return &cb[pxOff+stride*(H-1-y)+x*3]; }; // B,G,R
			for (int y=10;y<12;++y) for (int x=10;x<12;++x) { auto p=cpx(x,y); p[0]=0;   p[1]=69; p[2]=255; } // orange
			for (int y=20;y<22;++y) for (int x=20;x<22;++x) { auto p=cpx(x,y); p[0]=255; p[1]=0;  p[2]=0;   } // blue
			{ auto p=cpx(0,0); p[0]=253; p[1]=0; p[2]=253; }   // magenta wildcard

			pup::DmdMatcher mc;
			mc.SetExactColor(true);   // must precede AddPattern so RGB is kept
			expect(mc.AddPattern(60, cb.data(), cb.size()), "exact-color pattern loads with RGB");

			std::vector<int> cev;
			auto cemit = [&cev](int num, int state) { cev.push_back(num * 10 + state); };

			// live color frame (R,G,B per pixel): exact colors -> match
			std::vector<uint8_t> cf((size_t)W * H * 3, 0);
			auto setpx = [&cf,W](int x,int y,uint8_t r,uint8_t g,uint8_t b){
				size_t i=((size_t)y*W + x)*3; cf[i]=r; cf[i+1]=g; cf[i+2]=b; };
			for (int y=10;y<12;++y) for (int x=10;x<12;++x) setpx(x,y,255,69,0);  // orange
			for (int y=20;y<22;++y) for (int x=20;x<22;++x) setpx(x,y,0,0,255);   // blue
			setpx(0,0,10,20,30);   // wildcard position: arbitrary, must be ignored
			mc.OnFrameColor(cf.data(), W, H, cemit);
			expect(cev.size()==1 && cev[0]==601, "exact-color match fires on exact colors");

			// recolor the orange block blue: same pixels lit, WRONG color -> leave
			for (int y=10;y<12;++y) for (int x=10;x<12;++x) setpx(x,y,0,0,255);
			mc.OnFrameColor(cf.data(), W, H, cemit);
			expect(cev.size()==2 && cev[1]==600, "exact-color rejects a wrong-color, same-lit frame");
		}

		// undersized/oversized BMPs are rejected
		put32(18, 64);
		pup::DmdMatcher m2;
		expect(!m2.AddPattern(1, bmp.data(), bmp.size()), "non-128x32 BMP rejected");
	}

	std::cout << (failures == 0 ? "All engine tests passed\n"
	                            : "ENGINE TEST FAILURES\n");
	return failures == 0 ? 0 : 3;
}
