// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY
//
// Loads a PuP pack from disk into the pup::Pack model.

#pragma once
#include "PupModel.h"
#include <string>

namespace pup
{
	struct LoadResult
	{
		bool ok = false;
		std::string error;
		std::vector<std::string> warnings;
	};

	// Load the pack rooted at packDir (the folder that directly contains
	// screens.pup / playlists.pup / triggers.pup and the media folders).
	// Older packs may omit some .pup files (defined at runtime in the table
	// script instead); those are reported as warnings, not errors.
	//
	// allowScriptOnly: succeed even with no usable .pup files, yielding an
	// empty model the table script populates at runtime through the
	// PinDisplay interface (Init/playlistadd/setScreenEx).
	LoadResult LoadPack(const std::string& packDir, Pack& out, bool allowScriptOnly = false);
}
