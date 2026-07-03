// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY
//
// PUP trigger engine (portable core) — implementation.
//
// See PupTriggerEngine.h for the contract. This file is pure logic with no
// Windows/Direct3D/MFC dependency so it can be exercised headlessly by
// tool/PupPackTool.cpp and compiled unchanged into PinballY proper.

#include "PupTriggerEngine.h"
#include "PupCsv.h"   // for the shared IEquals helper
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <utility>

namespace fs = std::filesystem;

namespace pup
{
	// Build the per-device trigger index: for each device a trigger's checks
	// reference, record the trigger's index in that device's bucket.  Post()
	// then re-evaluates only the posted device's bucket - a device change
	// can't alter the match state of a trigger that never reads it.
	TriggerEngine::TriggerEngine(const Pack& pack) : pack(pack)
	{
		matched.resize(pack.triggers.size(), 0);
		for (size_t i = 0; i < pack.triggers.size(); ++i)
		{
			for (auto& c : pack.triggers[i].checks)
			{
				// a trigger listing the same device twice gets one slot
				// (its checks are contiguous, so back() suffices)
				auto& bucket = byDevice[Key(c.type, c.num)];
				if (bucket.empty() || bucket.back() != i)
					bucket.push_back(i);
			}
		}

		// Counter rotation groups: active rows sharing one trigger expression
		// AND carrying a Counter value take turns instead of firing together,
		// walking the rows in Counter order on consecutive fires.  Rows with
		// no Counter are left out so that Counter-less duplicate expressions
		// keep firing simultaneously (packs use those to hit several screens
		// from one machine event).
		std::map<std::string, std::vector<size_t>> byExpr;
		for (size_t i = 0; i < pack.triggers.size(); ++i)
		{
			const Trigger& t = pack.triggers[i];
			if (t.active && t.counter >= 0 && !t.checks.empty())
				byExpr[t.triggerExpr].push_back(i);
		}
		for (auto& kv : byExpr)
		{
			if (kv.second.size() < 2)
				continue;
			RotationGroup g;
			g.members = kv.second;
			std::stable_sort(g.members.begin(), g.members.end(),
				[&pack](size_t a, size_t b) { return pack.triggers[a].counter < pack.triggers[b].counter; });
			for (size_t m : g.members)
				rotGroupOf[m] = rotGroups.size();
			rotGroups.push_back(std::move(g));
		}

		// playlist folder -> playlist, for O(1) hot-path lookups
		for (auto& p : pack.playlists)
			playlistByFolder.emplace(p.folder, &p);
	}

	const Playlist* TriggerEngine::FindPlaylist(const std::string& folder) const
	{
		auto it = playlistByFolder.find(folder);
		return it != playlistByFolder.end() ? it->second : nullptr;
	}

	// Does a single parsed check match the current live state?
	//   * "S16"   (hasState == false) -> matches when device is *active* (state != 0)
	//   * "W4=1"  (hasState == true)  -> matches when device equals the named value
	// Unknown devices read as 0 (inactive), so a "=0" check matches before the
	// device has ever reported, which mirrors PuP's "off by default" model.
	bool TriggerEngine::CheckMatches(const Trigger::Check& c) const
	{
		auto it = state.find(Key(c.type, c.num));
		int cur = (it == state.end()) ? 0 : it->second;
		return c.hasState ? (cur == c.state) : (cur != 0);
	}

	// A trigger matches only when it has at least one check and *all* of its
	// checks match (PuP triggers are an AND of up to 5 conditions). An empty
	// expression never fires — otherwise it would match every event.
	bool TriggerEngine::TriggerMatches(const Trigger& t) const
	{
		if (t.checks.empty()) return false;
		for (auto& c : t.checks)
			if (!CheckMatches(c)) return false;
		return true;
	}

	// Turn a fired trigger into a concrete PlayCommand, inheriting screen /
	// priority / volume from the target playlist where the trigger left them at
	// their defaults, and choosing the media file (explicit PlayFile, else a
	// file from the playlist folder: the first when AlphaSort is on — the list
	// is sorted at load time — or a random member when it's off).
	PlayCommand TriggerEngine::Resolve(const Trigger& t)
	{
		PlayCommand cmd;
		cmd.screenNum       = t.screenNum;
		cmd.priority        = t.priority;
		cmd.volume          = t.volume;
		cmd.setAsBackground = IEquals(t.playAction, "SetBG");
		cmd.skipSamePriority= IEquals(t.playAction, "SkipSamePrty");
		cmd.lengthSecs      = t.lengthSecs;
		cmd.sourceTriggerId = !t.id.empty() ? t.id : t.description;

		// "SetBG" media becomes the screen's background, and backgrounds
		// loop.  ("SplashReset"/"SplashReturn" need no case of their own:
		// they play like "Play", and the host already returns to the
		// screen's background when trigger media finishes.)
		cmd.loop            = t.loop || IEquals(t.playAction, "Loop") || cmd.setAsBackground;

		// Inherit unset values (-1 in the model) from the target playlist.
		// The sentinels are out-of-band, so a trigger explicitly authored
		// with Volume=100 or Priority=0 keeps its value.
		const Playlist* pl = FindPlaylist(t.playlist);
		if (pl != nullptr)
		{
			if (cmd.screenNum < 0)  cmd.screenNum = pl->screenNum;
			if (cmd.priority < 0)   cmd.priority  = pl->priority;
			if (cmd.volume < 0)     cmd.volume    = pl->volume;
		}

		// final defaults if still unset (no playlist to inherit from)
		if (cmd.priority < 0) cmd.priority = 0;
		if (cmd.volume < 0)   cmd.volume = 100;

		// "StopPlayer" (and kin) clear the screen: leave mediaPath empty.
		if (IEquals(t.playAction, "StopPlayer") || IEquals(t.playAction, "Stop"))
			return cmd;

		// No explicit PlayFile -> draw from the playlist: AlphaSort plays the
		// first (sorted) file; otherwise pick a random member per play.  A
		// plain modulo keeps the draw deterministic across platforms for the
		// engine's fixed seed (the tiny modulo bias is irrelevant here).
		std::string file = t.playFile;
		if (file.empty() && pl != nullptr && !pl->files.empty())
			file = (pl->alphaSort || pl->files.size() == 1)
				? pl->files.front()
				: pl->files[(size_t)(rng() % pl->files.size())];

		// Model strings are UTF-8; u8path/u8string avoid the ACP conversion
		// in path::string(), which throws on out-of-code-page media names.
		if (!file.empty() && !t.playlist.empty())
			cmd.mediaPath = (fs::u8path(pack.rootPath) / fs::u8path(t.playlist) / fs::u8path(file)).u8string();
		else if (!file.empty())
			cmd.mediaPath = (fs::u8path(pack.rootPath) / fs::u8path(file)).u8string();

		return cmd;
	}

	std::vector<PlayCommand> TriggerEngine::Post(const Event& ev, double timeSec)
	{
		std::vector<PlayCommand> out;
		const uint32_t key = Key(ev.type, ev.num);

		// Only triggers whose checks reference the posted device can change
		// match state.  If no trigger reads this device, nothing can fire and
		// nothing reads its state - skip without even recording it.  (The
		// index buckets a trigger under every device in its checks, so a
		// combo's other devices always have buckets and are recorded.)
		auto bucket = byDevice.find(key);
		if (bucket == byDevice.end())
			return out;

		// Unchanged state can't create a rising edge: matched[] is already
		// current for every trigger in this bucket (each device change
		// re-evaluates them), so a same-value report fires nothing.  This
		// drops the bulk of pinmame traffic, which is redundant reports.
		auto st = state.find(key);
		if (st != state.end() && st->second == ev.state)
			return out;
		state[key] = ev.state;
		++eventSerial;

		// Collect at most one winning command per screen for this tick, choosing
		// the highest-priority trigger when several fire onto the same screen.
		// The source playlist folder rides along so a dispatched play can stamp
		// the playlist's rest clock.  Most events fire nothing, so the map
		// (ordered, for a deterministic output order) is only materialized once
		// a trigger actually fires.
		std::optional<std::map<int, std::pair<PlayCommand, std::string>>> winners;

		for (size_t i : bucket->second)
		{
			const Trigger& t = pack.triggers[i];

			bool now    = t.active && TriggerMatches(t);
			bool rising = now && !matched[i];
			matched[i]  = now ? 1 : 0;
			if (!rising) continue;

			// Counter rotation: rows sharing this row's expression fire one
			// at a time, so route the rising edge to the group's current
			// member - which may be a different row than the one we're
			// walking - and let the group act only once per event (every
			// member sees the same edge in this bucket).
			size_t fireIdx = i;
			RotationGroup* grp = nullptr;
			auto rg = rotGroupOf.find(i);
			if (rg != rotGroupOf.end())
			{
				grp = &rotGroups[rg->second];
				if (grp->lastEvent == eventSerial)
					continue;
				grp->lastEvent = eventSerial;
				fireIdx = grp->members[grp->next];
			}
			const Trigger& ft = pack.triggers[fireIdx];

			// restSeconds debounce: ignore a re-fire that arrives too soon after
			// this trigger's previous fire.
			auto lf = lastFire.find(fireIdx);
			if (lf != lastFire.end() && ft.restSeconds > 0
				&& (timeSec - lf->second) < (double)ft.restSeconds)
				continue;

			// Per-playlist rest: the target playlist's own restSeconds gates
			// replays from ANY trigger, in addition to the per-trigger window
			// above - both must be satisfied to fire.
			const Playlist* pl = FindPlaylist(ft.playlist);
			if (pl != nullptr && pl->restSeconds > 0)
			{
				auto lp = lastPlaylistPlay.find(pl->folder);
				if (lp != lastPlaylistPlay.end()
					&& (timeSec - lp->second) < (double)pl->restSeconds)
					continue;
			}
			lastFire[fireIdx] = timeSec;

			// the fire consumed the group's turn - advance the rotation
			if (grp != nullptr)
				grp->next = (grp->next + 1) % grp->members.size();

			PlayCommand cmd = Resolve(ft);
			if (cmd.screenNum < 0) continue;

			if (!winners.has_value()) winners.emplace();
			int screen = cmd.screenNum, pri = cmd.priority;
			auto w = winners->find(screen);
			if (w == winners->end() || pri >= w->second.first.priority)
				(*winners)[screen] = { std::move(cmd), pl != nullptr ? pl->folder : std::string() };
		}
		if (!winners.has_value())
			return out;

		// Arbitrate each winner against what's already on its screen: an equal
		// or higher priority preempts; a lower priority is suppressed (the
		// current media keeps playing); a SkipSamePrty command additionally
		// yields to an EQUAL-priority incumbent instead of restarting it.
		for (auto& kv : *winners)
		{
			const int screen = kv.first;
			PlayCommand& cmd  = kv.second.first;

			auto a = active.find(screen);
			if (a != active.end()
				&& (cmd.priority < a->second.priority
					|| (cmd.skipSamePriority && cmd.priority == a->second.priority)))
				continue;

			// A stop command leaves the screen idle - clear its arbitration
			// entry rather than recording the stop's priority, so any future
			// trigger can claim the screen.
			if (cmd.mediaPath.empty())
				active.erase(screen);
			else
			{
				active[screen] = cmd;

				// an actual play stamps the source playlist's rest clock
				if (!kv.second.second.empty())
					lastPlaylistPlay[kv.second.second] = timeSec;
			}
			out.push_back(std::move(cmd));
		}
		return out;
	}
}
