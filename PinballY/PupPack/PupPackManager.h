// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY
//
// PupPackManager - Windows-side host for the portable PUP-pack core
// (PupLoader/PupTriggerEngine).  The lifecycle mirrors DOFClient: the
// singleton is created at program startup (gated by the PupPack.Enable
// config var), notified when a game enters and leaves running-game mode,
// and shut down at program exit.
//
// On game start we resolve the game's ROM name (the game database ROM
// field if set, else the VPinMAME registry match), look for a pack folder
// under <PupPack.VideosPath>\<rom>, load it with the portable parser, and
// stand up a TriggerEngine.  The render/output layer is the next stage of
// the integration; for now the manager proves the discovery/load pipeline
// on real game launches and reports what it finds in the log file.

#pragma once
#include <memory>
#include <map>
#include <vector>
#include "PupModel.h"
#include "PupTriggerEngine.h"
#include "PupDmdMatch.h"

class GameListItem;
class GameSystem;
class PupWin;
class PupView;

class PupPackManager
{
public:
	// Create the global singleton, if enabled in the configuration
	static void Init();

	// destroy the global singleton at program exit
	static void Shutdown();

	// get the global singleton (null if disabled)
	static PupPackManager *Get() { return inst; }

	// Game launch notifications.  BeginRunningGameMode is called at
	// PRE-LAUNCH (PlayfieldView::LaunchQueuedGame), before the game
	// process is created, so that the PinDisplay COM server is already
	// registered when the table script loads and connects.
	// EndRunningGameMode is called from Application::EndRunningGameMode.
	void BeginRunningGameMode(GameListItem *game, GameSystem *system);
	void EndRunningGameMode();

	// The game's window is up (Application::BeginRunningGameMode):
	// re-assert our windows at the top of the topmost band, above the
	// built-in windows that just took their own topmost slots.
	void OnGameLoaded();

	// current pack status, valid while a game is running
	bool IsPackActive() const { return pack != nullptr; }
	const pup::Pack *GetPack() const { return pack.get(); }
	pup::TriggerEngine *GetEngine() const { return engine.get(); }

	// Are any PUP screen windows playing video?  The application counts
	// these as background videos, to keep the D3D rendering loop running
	// at full speed while the game has the foreground.
	bool HasActiveVideos() const { return anyActiveVideo; }

	// Post a live machine event (from the PinDisplay COM server or any
	// future event source) to the trigger engine, dispatching any play
	// commands it produces to the screen windows.  Device types per the
	// trigger expression grammar: S/W/L/G/E/M/D.
	void PostEvent(char type, int num, int state);

	// A live DMD frame from the event bus (one byte per pixel, row-major
	// top-down, nonzero = lit): match against the pack's PuPCapture
	// patterns and post D<n> transitions to the engine.
	// A live DMD frame from the tee.  channels is 1 (luminance, one byte per
	// pixel) or 3 (RGB, for exact-color packs).
	void OnDmdFrame(const uint8_t *px, int w, int h, int channels = 1);

	// Direct play/stop requests (PinDisplay playlistplayex/playstop)
	void DirectPlay(int screenNum, const TCHAR *playlist, const TCHAR *file, int volPct);
	void StopScreen(int screenNum);

	// Adjust a screen's currently-playing media volume live (PinDisplay
	// setVolumeCurrent - ducking).  volPct is the pack's 0-100 level; it is
	// scaled by the global video volume, as playback volume is.
	void SetScreenVolume(int screenNum, int volPct);

	// Playback control on a screen's current media (PinDisplay SetLoop/
	// playpause/playresume).  All tolerate a missing pack/window/media
	// (logged no-op).
	void SetLoop(int screenNum, int state);
	void PauseScreen(int screenNum);
	void ResumeScreen(int screenNum);

	// Mark a screen's CURRENT media as its background (PinDisplay
	// SetBackground, PuP's SetBG): non-looping trigger media returns to
	// it when it finishes.  A zero mode removes the association.
	void SetScreenBackground(int screenNum, int mode);

	// A parsed PinDisplay SendMSG trigger message (mt:301), carrying a
	// screen number and a numeric function code.  The function-code
	// table isn't publicly documented, and none of the codes observed
	// so far need action beyond the dedicated PinDisplay methods, so
	// this accepts them as no-ops; the COM layer logs the parsed values
	// to show what real tables send.
	void OnSendMsg(int screenNum, int fn);

	// A screen's media finished playing (called by PupView): release the
	// trigger engine's arbitration slot for that screen.
	void OnScreenMediaEnded(int screenNum);

	// Script-driven pack interface (PinDisplay Init/playlistadd/hide/
	// setScreenEx): tables using script-only packs define their screens
	// and playlists at runtime through these.
	void OnTableInit(int screenNum, const TCHAR *packName);
	void OnPlaylistAdd(int screenNum, const TCHAR *folder, int restSeconds);
	void HideScreen(int screenNum);
	void SetScreenGeometry(int screenNum, int xPct, int yPct, int wPct, int hPct);

	// PinDisplay label interface (LabelInit/LabelNew/LabelSet/
	// LabelShowPage): text overlays drawn above a screen's media layer.
	// All of these tolerate a missing pack/window/screen (logged no-op).
	void LabelInit(int screenNum);
	void LabelNew(int screenNum, const TCHAR *name, const TCHAR *font,
		double sizePct, int color, int rotation, int xAlign, int yAlign,
		double xPos, double yPos, int pageNum, bool visible);
	void LabelSet(int screenNum, const TCHAR *name, const TCHAR *text,
		bool visible, const TCHAR *special);
	void LabelShowPage(int screenNum, int pageNum, int seconds, const TCHAR *special);

protected:
	// Defined out-of-line: the members (std::list<RefPtr<PupWin>>) need
	// the complete PupWin type for their cleanup paths, which only the
	// implementation file includes.
	PupPackManager();
	~PupPackManager();

	// Resolve the ROM name to use for the pack folder lookup
	// Pack-folder name candidates for a game, in priority order: the
	// database ROM, the VPinMAME registry match, then any PupPack.RomAlias.*
	// override (PinballY's stand-in for PinUP's ROMALT).
	std::vector<TSTRING> ResolveRomCandidates(GameListItem *game) const;

	// Create/destroy the on-screen windows for the loaded pack's
	// displayable screens
	void CreateScreenWindows();
	void DestroyScreenWindows();

	// create the window for a single pack screen
	PupWin *CreateScreenWindow(const pup::Screen &s);

	// ensure a window exists for a script-defined screen
	void EnsureScreenWindow(int screenNum, bool audioOnly);

	// look up the view hosting a screen's window, if the window exists
	PupView *GetScreenView(int screenNum) const;

	// Carry out one engine play command on its target screen window.
	// Returns true if media actually started (or the stop was applied).
	bool Dispatch(const pup::PlayCommand &cmd);

	// Bring a freshly loaded pack live (engine, DMD patterns, windows,
	// hardware-event tee, pack-startup trigger).  Shared by the ROM-folder
	// and script-driven (OnTableInit) activation paths.
	void ActivatePack(std::unique_ptr<pup::Pack> p, const std::string &packDirU8);

	// Load the pack's PuPCapture DMD patterns, if present.  Returns true
	// if any loaded (the caller owns the tee/frame-sink lifecycle).
	bool LoadDmdPatterns(const std::string &packDirU8);

	// the loaded pack and its trigger engine, while a game is running
	std::unique_ptr<pup::Pack> pack;
	std::unique_ptr<pup::TriggerEngine> engine;

	// the pack's PuPCapture DMD matcher, if the pack ships patterns
	std::unique_ptr<pup::DmdMatcher> dmdMatcher;

	// per-pack DMD frame diagnostics (reset at ActivatePack): dump dir
	// and PGM counter for PupPack.DmdDumpDir, running frame count for the
	// log, and a re-entrancy guard for OnDmdFrame
	TSTRING dmdDumpDir;
	int dmdDumpNum = 0;
	UINT64 dmdFrameCount = 0;
	bool inDmdFrame = false;
	// reused scratch for reducing a color frame to luminance (OnDmdFrame),
	// so no per-frame heap allocation
	std::vector<uint8_t> dmdLumBuf;

	// the open PUP screen windows by pack screen number, while a game
	// is running
	std::map<int, RefPtr<PupWin>> windows;

	// is video playing in any of our windows?
	bool anyActiveVideo = false;

	// global singleton
	static PupPackManager *inst;
};
