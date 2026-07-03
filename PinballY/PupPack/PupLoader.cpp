// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY

#include "PupLoader.h"
#include "PupCsv.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace pup
{
	const Screen* Pack::FindScreen(int num) const
	{
		for (auto& s : screens) if (s.screenNum == num) return &s;
		return nullptr;
	}

	const Playlist* Pack::FindPlaylist(const std::string& folder) const
	{
		for (auto& p : playlists)
			if (p.folder == folder) return &p;
		return nullptr;
	}

	bool Pack::HasHardwareTriggers() const
	{
		for (auto& t : triggers)
			for (auto& c : t.checks)
				if (c.type == 'S' || c.type == 'W' || c.type == 'L'
					|| c.type == 'G' || c.type == 'M')
					return true;
		return false;
	}

	static std::string ReadFile(const fs::path& p)
	{
		std::ifstream f(p, std::ios::binary);
		if (!f) return "";
		std::stringstream ss; ss << f.rdbuf();
		return ss.str();
	}

	// Case-insensitive search for one of the .pup files in a directory
	// (real packs are inconsistent about case: screens.pup vs Screens.pup).
	// Note: all path<->string conversions in this module go through
	// u8string()/u8path(), never path::string(), because the narrow-ACP
	// conversion throws on Windows for filenames outside the active code
	// page — and real packs do contain such media names.
	static fs::path FindFile(const fs::path& dir, const std::string& name)
	{
		std::error_code ec;
		if (!fs::is_directory(dir, ec)) return {};
		try
		{
			for (auto& e : fs::directory_iterator(dir, ec))
				if (e.is_regular_file(ec) && IEquals(e.path().filename().u8string(), name))
					return e.path();
		}
		catch (...) { }
		return {};
	}

	// Parse a trigger expression like "S16" or "W4=1,W5=0" into checks.
	void ParseTriggerExpr(const std::string& expr, std::vector<Trigger::Check>& out)
	{
		out.clear();
		std::stringstream ss(expr);
		std::string tok;
		while (std::getline(ss, tok, ','))
		{
			// trim
			size_t a = tok.find_first_not_of(" \t");
			if (a == std::string::npos) continue;
			size_t b = tok.find_last_not_of(" \t");
			tok = tok.substr(a, b - a + 1);
			if (tok.empty()) continue;

			Trigger::Check c;
			c.type = (char)std::toupper((unsigned char)tok[0]);
			size_t eq = tok.find('=');
			std::string numStr = (eq == std::string::npos)
				? tok.substr(1)
				: tok.substr(1, eq - 1);
			try { c.num = std::stoi(numStr); } catch (...) { continue; }
			if (eq != std::string::npos)
			{
				try { c.state = std::stoi(tok.substr(eq + 1)); c.hasState = true; }
				catch (...) { c.state = 1; }
			}
			out.push_back(c);
		}
	}

	// Find, read, and parse a pack .pup CSV, emitting the standard
	// not-found / empty-invalid warnings.  Returns false (warned) if the
	// file is missing or unparseable.
	static bool OpenPupCsv(const fs::path& dir, const std::string& name, CsvTable& t, LoadResult& r)
	{
		fs::path f = FindFile(dir, name);
		if (f.empty()) { r.warnings.push_back(name + " not found"); return false; }
		if (!t.Parse(ReadFile(f))) { r.warnings.push_back(name + " empty/invalid"); return false; }
		return true;
	}

	static void LoadScreens(const fs::path& dir, Pack& pack, LoadResult& r)
	{
		CsvTable t;
		if (!OpenPupCsv(dir, "screens.pup", t, r)) return;
		for (size_t i = 0; i < t.RowCount(); ++i)
		{
			Screen s;
			s.screenNum         = t.GetInt(i, {"ScreenNum", "Screen", "ScreenNo"}, -1);
			s.description       = t.Get   (i, {"ScreenDes", "Description", "Des"});
			s.mode              = t.Get   (i, {"Active", "sysModes", "Pviewer", "Mode"});
			s.backgroundPlaylist= t.Get   (i, {"PlayList", "Playlist"});
			s.backgroundFile    = t.Get   (i, {"PlayFile", "File"});
			s.loopBackground    = t.GetBool(i, {"Loopit", "Loop"}, true);
			s.customPosRaw      = t.Get   (i, {"CustomPos"});
			if (!s.customPosRaw.empty())
			{
				// "screenRef,x,y,w,h" - a target screen reference followed by
				// position/size percentages (best-effort).
				std::vector<double> v; std::stringstream cs(s.customPosRaw); std::string p;
				while (std::getline(cs, p, ',')) { try { v.push_back(std::stod(p)); } catch (...) {} }
				if (v.size() >= 5) { s.hasCustomPos = true; s.posScreenRef = (int)v[0]; s.posXPct = v[1]; s.posYPct = v[2]; s.posWPct = v[3]; s.posHPct = v[4]; }
			}
			if (s.screenNum >= 0) pack.screens.push_back(s);
		}
	}

	static void LoadPlaylists(const fs::path& dir, Pack& pack, LoadResult& r)
	{
		CsvTable t;
		if (!OpenPupCsv(dir, "playlists.pup", t, r)) return;
		for (size_t i = 0; i < t.RowCount(); ++i)
		{
			Playlist p;
			p.screenNum  = t.GetInt(i, {"ScreenNum", "Screen"}, -1);
			p.folder     = t.Get   (i, {"Folder", "PlayList", "Playlist", "Description"});
			p.description= t.Get   (i, {"Description", "Des"});
			p.alphaSort  = t.GetBool(i, {"Alphasort", "AlphaSort"}, true);
			p.restSeconds= t.GetInt(i, {"RestSeconds", "Rest"}, 0);
			p.volume     = t.GetInt(i, {"Volume", "Vol"}, 100);
			p.priority   = t.GetInt(i, {"Priority", "Pri", "GfxPriority"}, 0);

			// Resolve media files present in the folder.
			fs::path folderPath = dir / fs::u8path(p.folder);
			std::error_code ec;
			if (fs::is_directory(folderPath, ec))
			{
				try
				{
					for (auto& e : fs::directory_iterator(folderPath, ec))
						if (e.is_regular_file(ec))
							p.files.push_back(e.path().filename().u8string());
				}
				catch (const std::exception& ex)
				{
					r.warnings.push_back("error scanning playlist folder " + p.folder + ": " + ex.what());
				}
				if (p.alphaSort)
					std::sort(p.files.begin(), p.files.end());
			}
			else
			{
				r.warnings.push_back("playlist folder missing: " + p.folder);
			}
			if (!p.folder.empty()) pack.playlists.push_back(p);
		}
	}

	static void LoadTriggers(const fs::path& dir, Pack& pack, LoadResult& r)
	{
		CsvTable t;
		if (!OpenPupCsv(dir, "triggers.pup", t, r)) return;
		for (size_t i = 0; i < t.RowCount(); ++i)
		{
			Trigger tr;
			tr.active     = t.GetBool(i, {"Active"}, true);
			tr.id         = t.Get   (i, {"ID", "Descript", "Description"});
			tr.description= t.Get   (i, {"Descript", "Description"});
			tr.triggerExpr= t.Get   (i, {"trigger", "Trigger"});
			tr.screenNum  = t.GetInt(i, {"Screen", "ScreenNum"}, -1);
			tr.playlist   = t.Get   (i, {"PlayList", "Playlist"});
			tr.playFile   = t.Get   (i, {"PlayFile", "File"});
			tr.volume     = t.GetInt(i, {"Volume", "Vol"}, -1);
			tr.priority   = t.GetInt(i, {"Priority", "Pri"}, -1);
			tr.restSeconds= t.GetInt(i, {"RestSeconds", "Rest"}, 0);
			tr.lengthSecs = t.GetInt(i, {"Length", "PlayLength"}, 0);
			tr.counter    = t.GetInt(i, {"Counter"}, -1);
			tr.loop       = t.GetBool(i, {"Loop"}, false);
			tr.playAction = t.Get   (i, {"PlayAction", "Type", "Action"}, "Play");
			ParseTriggerExpr(tr.triggerExpr, tr.checks);
			if (!tr.triggerExpr.empty()) pack.triggers.push_back(tr);
		}
	}

	LoadResult LoadPack(const std::string& packDir, Pack& out, bool allowScriptOnly)
	{
		LoadResult r;
		fs::path dir(packDir);
		std::error_code ec;
		if (!fs::is_directory(dir, ec))
		{
			r.error = "pack directory not found: " + packDir;
			return r;
		}
		out.rootPath = fs::absolute(dir, ec).u8string();
		out.romName  = dir.filename().u8string();

		LoadScreens(dir, out, r);
		LoadPlaylists(dir, out, r);
		LoadTriggers(dir, out, r);

		r.ok = allowScriptOnly || !out.screens.empty() || !out.triggers.empty();
		if (!r.ok && r.error.empty())
			r.error = "no usable screens.pup/triggers.pup found (old script-only pack?)";
		return r;
	}
}
