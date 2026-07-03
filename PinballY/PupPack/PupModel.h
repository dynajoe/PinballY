// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY
//
// PUP-pack data model (portable core)
// ------------------------------------
// Plain, platform-independent representation of a "PuP pack" (PinUP Player
// pack): the parsed contents of screens.pup, playlists.pup and triggers.pup,
// plus the on-disk media folder layout under PUPVIDEOS\<romname>\.
//
// This header is deliberately free of any Windows / Direct3D / MFC
// dependency so the same model can be:
//   * unit-tested on any platform (see tool/PupPackTool.cpp), and
//   * compiled into PinballY proper, where a thin render backend maps
//     PupScreen -> CustomView and PupPlayCommand -> a VideoSprite.
//
// Strings are UTF-8 std::string in the core. The PinballY integration layer
// is responsible for widening to TCHAR at the boundary.

#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace pup
{
	// A screen is one logical display surface in the pack (topper, DMD,
	// backglass, playfield, full-DMD, etc). screens.pup assigns each a
	// stable ScreenNum that triggers/playlists reference.
	struct Screen
	{
		int         screenNum = -1;     // ScreenNum - referenced by playlists & triggers
		std::string description;        // ScreenDes - human label ("Topper", "BackGlass", ...)
		std::string mode;               // sysModes / Pviewer mode if present ("show","overlay",...).
		                                // Stored verbatim from the Active column; besides the
		                                // named modes, some packs use the numeric convention
		                                // "0" = off and "1" = show, which the host's screen-mode
		                                // tests honor when deciding whether to create a window.
		std::string backgroundPlaylist; // PlayList for the always-on background, if any
		std::string backgroundFile;     // PlayFile for the always-on background, if any
		bool        loopBackground = true;

		// CustomPos: optional explicit placement on a shared physical screen,
		// stored verbatim plus a best-effort parse. Format seen in the wild:
		//   "screenRef,x,y,width,height" (percentages) with an optional popup
		// flag, where screenRef names the screen the placement is relative to.
		std::string customPosRaw;
		bool        hasCustomPos = false;
		int         posScreenRef = -1;
		double      posXPct = 0, posYPct = 0, posWPct = 100, posHPct = 100;
	};

	// A playlist is a named folder of media under the pack, with default
	// playback attributes. triggers reference a playlist (and optionally a
	// specific file within it).
	struct Playlist
	{
		int         screenNum = -1;     // default screen this playlist targets
		std::string folder;             // folder name under PUPVIDEOS\<rom>
		std::string description;
		bool        alphaSort = true;   // 1: play the first file (files sorted at load);
		                                // 0: the engine draws a random file per play
		int         restSeconds = 0;    // min seconds before ANY trigger may replay this
		                                // playlist (engine-tracked per folder, enforced in
		                                // addition to each trigger's own restSeconds)
		int         volume = 100;       // 0..100
		int         priority = 0;       // default priority for plays from this list

		// Resolved at load time: media files actually present in the folder.
		std::vector<std::string> files;
	};

	// One row of triggers.pup: "when the machine reaches state X, play Y".
	struct Trigger
	{
		bool        active = true;      // Active flag
		std::string id;                 // optional ID/Descript
		std::string description;
		std::string triggerExpr;        // raw trigger string, e.g. "S16" or "W4=1,W5=0"
		int         screenNum = -1;     // target screen
		std::string playlist;           // target playlist (folder)
		std::string playFile;           // specific file, or empty -> use playlist
		int         volume = -1;        // 0..100; -1 = unset -> inherit from playlist
		int         priority = -1;      // -1 = unset -> inherit from playlist
		int         restSeconds = 0;    // debounce: ignore re-fire within N seconds
		int         lengthSecs = 0;     // max play time in seconds; 0 = natural length
		int         counter = -1;       // Counter column: rotation order among rows that
		                                // share one trigger expression - consecutive fires
		                                // walk such rows round-robin in Counter order;
		                                // -1 = unset (row fires normally, no rotation)
		bool        loop = false;

		// PlayAction column. The engine implements: "Play" (default), "Loop"
		// (play looping), "StopPlayer"/"Stop" (clear the screen), "SetBG"
		// (play looping AND make the media the screen's new background), and
		// "SkipSamePrty" (play, but never preempt an EQUAL-priority incumbent).
		// "SplashReset"/"SplashReturn" behave like "Play": the host already
		// returns to the screen's background when trigger media finishes.
		std::string playAction;

		// Parsed form of triggerExpr (see PupTriggerEngine).
		struct Check
		{
			char type = 0;   // S,W,L,G,E,M,D  (solenoid/switch/light/GI/DOF/mech/DMD)
			int  num  = 0;   // device number
			int  state = 1;  // required state; default 1 (active) when "=state" omitted
			bool hasState = false;
		};
		std::vector<Check> checks;
	};

	// The whole pack.
	struct Pack
	{
		std::string romName;            // <romname> folder under PUPVIDEOS
		std::string rootPath;           // absolute path to the pack folder
		std::vector<Screen>   screens;
		std::vector<Playlist> playlists;
		std::vector<Trigger>  triggers;

		const Screen*   FindScreen(int num) const;
		const Playlist* FindPlaylist(const std::string& folder) const;

		// Does any trigger key off live pinmame hardware state (switch,
		// solenoid, light, GI, or mech)?  These need the VPinMAME event
		// tee; packs with only E (pupevent) or D (DMD) triggers don't.
		bool HasHardwareTriggers() const;
	};

	// A concrete decision produced by the engine: "show this media file on
	// this screen now, at this priority/volume, looping or not". This is the
	// exact payload the Windows render backend will translate into a
	// CustomView VideoSprite. Kept UI-agnostic on purpose.
	struct PlayCommand
	{
		int         screenNum = -1;
		std::string mediaPath;   // absolute path to the media file to play ("" => stop)
		int         priority = 0;
		int         volume = 100;
		bool        loop = false;
		int         lengthSecs = 0;   // max play time in seconds; 0 = natural length
		bool        setAsBackground = false;  // "SetBG": the media also becomes the
		                                      // screen's new background (host loops it
		                                      // and returns to it after later media)
		bool        skipSamePriority = false; // "SkipSamePrty": yield to an active entry
		                                      // of EQUAL priority instead of restarting
		                                      // it (higher still preempts, lower is
		                                      // suppressed as usual)
		std::string sourceTriggerId;  // for diagnostics
	};
}
