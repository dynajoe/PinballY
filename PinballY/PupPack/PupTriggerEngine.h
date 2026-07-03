// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY
//
// PUP trigger engine (portable core)
// ----------------------------------
// Holds the live machine state (solenoids/switches/lamps/GI/DOF/mech/DMD
// match ids), accepts game events, and resolves fired triggers into concrete
// PlayCommands with per-screen priority arbitration and restSeconds debounce.
//
// This is pure logic with no UI: the Windows render backend feeds it events
// (from the PinUpPlayer.PinDisplay COM interface or the libpinmame/MsgPlugin
// bus) and consumes the emitted PlayCommands by driving CustomView sprites.
// The same engine is exercised headlessly by tool/PupPackTool.cpp.

#pragma once
#include "PupModel.h"
#include <unordered_map>
#include <vector>
#include <functional>
#include <random>
#include <string>

namespace pup
{
	class TriggerEngine
	{
	public:
		// Construction builds the per-device trigger index (see byDevice
		// below).  The pack's trigger list must not change for the engine's
		// lifetime; the host rebuilds the engine whenever it swaps packs.
		explicit TriggerEngine(const Pack& pack);

		// A game event: device type letter (S/W/L/G/E/M/D), number, new state.
		// 'timeSec' is a monotonic clock used only for restSeconds debounce,
		// so tests can supply a virtual clock and stay deterministic.
		struct Event { char type; int num; int state; };

		// Feed one event. Returns any PlayCommands produced (usually 0 or 1).
		std::vector<PlayCommand> Post(const Event& ev, double timeSec);

		// The screen's media finished or was stopped: clear its arbitration
		// entry so that any future trigger can claim the screen.  The host
		// MUST call this when trigger media ends, or the finished command's
		// priority would suppress lower-priority triggers forever.
		void ClearActive(int screenNum) { active.erase(screenNum); }

		// Record externally initiated media (e.g., a script playlistplayex)
		// in the arbitration state, so triggers can't stomp it and a stop
		// releases the screen.  An empty mediaPath clears the entry.
		void SetActive(const PlayCommand& cmd)
		{
			if (cmd.screenNum < 0)
				return;
			if (cmd.mediaPath.empty())
				active.erase(cmd.screenNum);
			else
				active[cmd.screenNum] = cmd;
		}

		// Current media showing on each screen (screenNum -> command).
		const std::unordered_map<int, PlayCommand>& ActiveByScreen() const { return active; }

	private:
		const Pack& pack;

		// live device state, keyed by (type<<24 | num); the packed key is
		// integral, so std::hash covers it with no custom hasher
		std::unordered_map<uint32_t, int> state;

		// Per-device trigger index, built once at construction: device key ->
		// indices of the triggers whose checks reference that device.  Post()
		// re-evaluates only the posted device's bucket, since a device change
		// can't alter the match state of a trigger that never reads it.  This
		// keeps the hot path O(bucket) instead of O(all triggers) - important
		// once the pinmame event source starts streaming switch/lamp traffic.
		std::unordered_map<uint32_t, std::vector<size_t>> byDevice;

		// playlist folder -> playlist, built once at construction, so the
		// hot-path playlist lookups (rest gating + Resolve inheritance) are
		// O(1) instead of a linear scan of pack.playlists per fire
		std::unordered_map<std::string, const Playlist*> playlistByFolder;

		// rising-edge tracking + debounce per trigger (by index)
		std::vector<char> matched;          // was the trigger fully matched last eval?
		std::unordered_map<size_t, double> lastFire;   // trigger index -> last fire time

		// Per-playlist rest tracking: playlist folder -> time of the last
		// play actually dispatched from it.  A playlist's restSeconds gates
		// replays from ANY trigger targeting it, on top of each trigger's
		// own restSeconds window (both must be satisfied).
		std::unordered_map<std::string, double> lastPlaylistPlay;

		// Counter rotation groups, built at construction: active trigger
		// rows that share one trigger expression AND carry a Counter value
		// fire one row per rising edge, round-robin in Counter order.
		// (Rows without a Counter keep the fire-together behavior - packs
		// use duplicate expressions to hit several screens at once.)
		struct RotationGroup
		{
			std::vector<size_t> members;    // trigger indices, sorted by Counter
			size_t   next = 0;              // rotation cursor into members
			uint64_t lastEvent = 0;         // Post() serial that last handled the group
		};
		std::vector<RotationGroup> rotGroups;
		std::unordered_map<size_t, size_t> rotGroupOf;  // trigger index -> rotGroups index
		uint64_t eventSerial = 0;           // Post() call counter, for once-per-event gating

		// Engine-owned PRNG for AlphaSort=0 playlists (random file per play).
		// The fixed seed keeps headless tests deterministic; plays still vary
		// from one to the next within a session.
		std::minstd_rand rng{ 20260701u };

		// what's currently playing per screen
		std::unordered_map<int, PlayCommand> active;

		static uint32_t Key(char type, int num) { return ((uint32_t)(unsigned char)type << 24) | (uint32_t)num; }
		const Playlist* FindPlaylist(const std::string& folder) const;   // O(1) via playlistByFolder
		bool CheckMatches(const Trigger::Check& c) const;
		bool TriggerMatches(const Trigger& t) const;
		PlayCommand Resolve(const Trigger& t);   // non-const: draws from rng
	};
}
