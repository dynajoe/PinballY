// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY
//
// PupView - D3D view child for a PUP-pack screen window (PupWin)
//
// This is a stripped-down sibling of CustomView: a SecondaryView with no
// game media types, so the game-selection media sync machinery never loads
// anything into it, and no Javascript coupling.  The PUP pack manager
// drives it directly: the pack screen's background/attract media plays in
// the BaseView videoOverlay slot, and trigger media replaces it, returning
// to the background when it finishes.

#pragma once

#include <map>
#include "../D3D.h"
#include "../D3DWin.h"
#include "../D3DView.h"
#include "../BaseView.h"
#include "../SecondaryView.h"
#include "../Resource.h"

class PupView : public SecondaryView
{
public:
	PupView(const TCHAR *configVarPrefix);

	// The pack screen number this view hosts, and whether it's an
	// audio-only (MusicOnly) screen.  Set by the manager at creation.
	void SetScreenNum(int n) { screenNum = n; }
	void SetAudioOnly(bool f) { audioOnly = f; }

	// Play a media file (video, audio, or still image) as this screen's
	// current media.  lengthSecs > 0 cuts playback off after that many
	// seconds (the triggers.pup Length column); 0 plays to natural end.
	// Returns true if the media load was successfully initiated.
	bool PlayMedia(const TCHAR *path, bool loop, int volPct, int lengthSecs = 0);

	// Remember this screen's background media.  When non-looping trigger
	// media finishes, we return to this, mirroring PuP's behavior.
	void SetBackground(const TCHAR *path, int volPct) { bgPath = path; bgVol = volPct; }

	// Toggle looping on the current media (PinDisplay SetLoop)
	void SetMediaLooping(bool loop);

	// Set the currently-playing media's volume live (PinDisplay
	// setVolumeCurrent - ducking).  Adjusts only the live player; the
	// screen's recorded default (curVol/bgVol) is left alone, so a later
	// replay or return-to-background uses the original level.
	void SetCurrentVolume(int volPct);

	// Pause/resume the current media (PinDisplay playpause/playresume).
	// Pause stops playback in place without discarding the sprite, per
	// the drawing-layer pause convention (BaseView::JsDrawingLayerPause),
	// so the last presented frame stays on screen.
	void PauseMedia();
	void ResumeMedia();

	// Promote the current media to this screen's background (PinDisplay
	// SetBackground): non-looping trigger media will return to it when
	// it finishes.  Returns false if no media has played on the screen.
	bool SetCurrentAsBackground();

	// remove the background association (PinDisplay SetBackground mode 0)
	void ClearBackground() { bgPath.clear(); }

	// Full stop (script playstop): discard current media AND the
	// background association, leaving the screen blank until something
	// else plays.
	void StopAll();

	// stop and discard our media
	virtual void ClearMedia() override;

	// ------------------------------------------------------------------
	// Text label overlay (PinDisplay LabelNew/LabelSet/LabelShowPage).
	// All of a screen's visible labels composite into ONE sprite drawn
	// above the media layer; rebuilds are coalesced through a short timer,
	// since real tables make label calls in bursts (1000+ LabelSets per
	// minute observed in attract mode).  PuP conventions: the font size
	// is a percentage of the window height, positions are percentages of
	// the window width/height, and the color is a COLORREF-style BGR int
	// straight from the table script's RGB() call.

	// Create or restyle a label (LabelNew).  Restyling an existing label
	// keeps its current text.
	void SetLabelStyle(const TCHAR *name, const TCHAR *font, float sizePct,
		COLORREF color, int xAlign, int yAlign, float xPct, float yPct,
		int page, bool visible);

	// Set a label's text and visibility (LabelSet).  A LabelSet without a
	// prior LabelNew creates the label with default styling, for tolerance
	// of loosely written table scripts.  The "special" JSON argument
	// (animated label effects) is accepted but ignored, with an at-most-
	// once-per-label log line so the log shows what the table wanted.
	void SetLabelText(const TCHAR *name, const TCHAR *text, bool visible, const TCHAR *special);

	// set the visible label page (LabelShowPage)
	void SetLabelPage(int page);

protected:
	// PUP windows never load game media from the media database
	virtual const MediaType *GetBackgroundImageType() const override { return nullptr; }
	virtual const MediaType *GetBackgroundVideoType() const override { return nullptr; }
	virtual const TCHAR *GetDefaultSystemImage() const override { return _T(""); }
	virtual const TCHAR *GetDefaultSystemVideo() const override { return _T(""); }
	virtual const TCHAR *GetDefaultBackgroundImage() const override { return _T(""); }
	virtual const TCHAR *GetDefaultBackgroundVideo() const override { return _T(""); }
	virtual const TCHAR *StartupVideoName() const override { return _T(""); }

	// PUP overlays only exist while a game is running, so always keep
	// the media going in running-game mode.  (Per-game suppression is
	// handled at window-creation time by the manager, via the standard
	// show-when-running lists.)
	virtual bool ShowMediaWhenRunning(GameListItem *game, GameSystem *system) const override { return true; }
	virtual const TCHAR *ShowWhenRunningWindowId() const override { return _T("puppack"); }

	// We never take part in the game-selection media sync, but the base
	// class requires a next-window command; hand off to the custom view
	// chain terminator, which no-ops once all custom views are synced.
	virtual UINT GetNextWindowSyncCommand() const override { return ID_SYNC_USERDEFINED; }

	// return to the background media when a non-looping video ends
	virtual void OnEndOverlayVideo() override;

	// Frame show/hide: audio-only windows are permanently hidden hosts
	// and must keep sounding; a visible window hidden by the user stops
	// its media (like every other PinballY window).
	virtual void OnShowHideFrameWindow(bool show) override;

	// draw the label overlay sprite above the media layer
	virtual void UpdateDrawingList() override;
	virtual void ScaleSprites() override;

	// Video-format detection: the overlay video isn't sized to fill the
	// window until its true frame dimensions arrive here (like the base
	// class does for its drawing layers), so a non-16:9 clip would
	// otherwise render as a squished, un-filled square.
	virtual bool OnAppMessage(UINT msg, WPARAM wParam, LPARAM lParam) override;

	// How pack media fills the screen window: false (default) stretches to
	// fill, matching real PuP's fitToWindow; true preserves the clip's
	// aspect ratio (letterbox).  Config: PupPack.MediaKeepAspect.
	bool mediaKeepAspect = false;

	// media length-limit timer (triggers.pup Length column) and the
	// label overlay rebuild timer
	virtual bool OnTimer(WPARAM timer, LPARAM callback) override;
	static const int lengthTimerID = 4201;
	static const int labelTimerID = 4202;

	// End the current media now (length limit reached): discard it and
	// run the same end-of-media path as a natural video end.
	void EndOverlayMedia();

	// shared end-of-media path: tell the manager (which releases the
	// engine's arbitration slot) and restart the background, if any
	void OnMediaFinished();

	// apply or remove black color-key transparency on our frame window
	// (used for alpha-carrying overlay images; see PlayMedia)
	void SetColorKeyTransparency(bool on);

	// Flag a label change and make sure the rebuild timer is running.
	// The timer coalesces rebuilds: no matter how fast the label calls
	// arrive, the sprite re-renders at most once per tick, and only
	// when something actually changed.
	void MarkLabelsDirty();

	// render all visible labels of the current page into labelSprite
	void RenderLabels();

	// one label record
	struct Label
	{
		TSTRING text;                     // display text (empty draws nothing)
		TSTRING fontName = _T("Arial");   // font family name
		float sizePct = 5.0f;             // font height, percent of window height
		COLORREF color = RGB(255, 255, 255);  // text color (BGR int from the script)
		int xAlign = 0;                   // 0=left, 1=center, 2=right of the position
		int yAlign = 0;                   // 0=top, 1=center, 2=bottom of the position
		float xPct = 0.0f;                // position, percent of window width
		float yPct = 0.0f;                // position, percent of window height
		int page = 1;                     // label page (0 shows on every page)
		bool visible = true;              // is the label showing?
		bool specialLogged = false;       // "special" JSON already reported once
	};

	// the label store, keyed by label name (case-insensitive, as the
	// names come from case-insensitive VBScript table code)
	struct LabelNameLess
	{
		bool operator()(const TSTRING &a, const TSTRING &b) const
			{ return _tcsicmp(a.c_str(), b.c_str()) < 0; }
	};
	std::map<TSTRING, Label, LabelNameLess> labels;

	// the currently visible label page (LabelShowPage)
	int labelPage = 1;

	// the composite overlay sprite for the visible labels, and its
	// deferred-rebuild state
	RefPtr<Sprite> labelSprite;
	bool labelsDirty = false;
	bool labelTimerRunning = false;

	// this screen's background media, if any
	TSTRING bgPath;
	int bgVol = 100;

	// The current media, as last passed to PlayMedia (the volume is the
	// already-scaled effective volume).  SetBackground promotion uses
	// these to make the current media the new background.
	TSTRING curPath;
	int curVol = 100;

	// pack screen identity, set by the manager
	int screenNum = -1;
	bool audioOnly = false;
};
