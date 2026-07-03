// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY
//
// PupWin - frame window for one PUP-pack screen
//
// A stripped-down sibling of CustomWin without the Javascript coupling.
// PupPackManager creates one of these per displayable pup::Screen when a
// game with a pack starts, and destroys them when the game ends.  The
// config var prefix ("PupScreenN") gives each pack screen number a stable,
// user-adjustable window position that persists across games.

#pragma once

#include "../FrameWin.h"

class PupView;

class PupWin : public FrameWin
{
public:
	PupWin(const TCHAR *configVarPrefix, const TCHAR *title);

	// get my view
	PupView *GetPupView() const { return pupView; }

protected:
	// create my view window
	virtual BaseView *CreateViewWin() override;

	// hide the window on minimize or close
	virtual bool IsHideable() const override { return true; }

	// PUP screens are always borderless overlays (never a title bar or
	// sizing frame), like the DMD and instruction-card windows.  Marking
	// the window permanently borderless is the correct FrameWin mechanism:
	// it creates borderless up front and avoids the deferred
	// ID_WINDOW_BORDERS_INIT toggle that FrameWin posts for user-switchable
	// windows - which otherwise raced our per-launch SetBorderless call and
	// left a title bar on alternating launches.
	virtual bool IsBorderless() const override { return true; }

	// configuration variable prefix and window title
	TSTRING configVarPrefix;
	TSTRING title;

	// my view (owned by the FrameWin view reference)
	PupView *pupView = nullptr;
};
