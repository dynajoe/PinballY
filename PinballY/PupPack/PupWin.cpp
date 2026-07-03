// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY

#include "../stdafx.h"
#include "../Resource.h"
#include "PupWin.h"
#include "PupView.h"

PupWin::PupWin(const TCHAR *configVarPrefix, const TCHAR *title) :
	FrameWin(configVarPrefix, title, IDI_MAINICON, IDI_MAINICON_GRAY),
	configVarPrefix(configVarPrefix),
	title(title)
{
}

BaseView *PupWin::CreateViewWin()
{
	// create our view
	PupView *view = new PupView(configVarPrefix.c_str());
	if (!view->Create(hWnd, title.c_str()))
	{
		view->Release();
		return nullptr;
	}

	// remember it and return it
	pupView = view;
	return view;
}
