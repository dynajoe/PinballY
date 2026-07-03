// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY

#include "../stdafx.h"
#include <filesystem>
#include <vector>
#include <algorithm>
#include <set>
#include "PupPackManager.h"
#include "PupLoader.h"
#include "PupWin.h"
#include "PupView.h"
#include "PupComServer.h"
#include "PupDmdMatch.h"
#include "../Application.h"
#include "../LogFile.h"
#include "../GameList.h"
#include "../VPinMAMEIfc.h"
#include "../PlayfieldWin.h"
#include "../BackglassWin.h"
#include "../DMDWin.h"
#include "../TopperWin.h"

namespace fs = std::filesystem;

// statics
PupPackManager *PupPackManager::inst = nullptr;

namespace ConfigVars
{
	static const TCHAR *PupPackEnable = _T("PupPack.Enable");
	static const TCHAR *PupPackVideosPath = _T("PupPack.VideosPath");
	static const TCHAR *PupPackPinDisplayCOM = _T("PupPack.PinDisplayCOM");
	static const TCHAR *PupPackPinMameEvents = _T("PupPack.PinMameEvents");
	static const TCHAR *PupPackDmdMatchPercent = _T("PupPack.DmdMatchPercent");
	static const TCHAR *PupPackDmdDumpDir = _T("PupPack.DmdDumpDir");
	// Per-ROM pack-folder alias key prefix; the full key is
	// "PupPack.RomAlias.<rom>" and its value is the pack folder name to use.
	static const TCHAR *PupPackRomAliasPrefix = _T("PupPack.RomAlias.");
}

// UTF-8 <-> UTF-16 helpers.  The portable pup core stores all of its
// strings in UTF-8 (see PupModel.h); PinballY proper is a Unicode build.
// These are thin wrappers over the shared StringUtil conversions, with
// the code page pinned to UTF-8.  (Unlike the hand-rolled versions they
// replaced, WideToAnsi/AnsiToWide require a non-null input; every call
// site here passes a c_str() or an already-null-checked pointer.)
static inline std::string WideToUtf8(const TCHAR *w) { return WideToAnsi(w, CP_UTF8); }
static inline TSTRING Utf8ToWide(const std::string &s) { return AnsiToWide(s.c_str(), CP_UTF8); }

void PupPackManager::Init()
{
	if (inst == nullptr
		&& ConfigManager::GetInstance()->GetBool(ConfigVars::PupPackEnable, true))
	{
		inst = new PupPackManager();

		// Clean up any stale registry shadows left by an abnormal exit
		// during a past pack game (Stop only removes keys that carry
		// our own CLSIDs, so real PinUP/VPinMAME registrations are
		// never touched).
		PupComServer::Stop();
		PupEventBus::Stop();

		LogFile::Get()->Write(LogFile::TableLaunchLogging, _T("PUP pack manager initialized\n"));
	}
}

// Is PUP suppressed for this game by the standard Show When Running
// lists (per-game stats column, per-system KeepOpen, global config)?
// Unlike the built-in windows, the DEFAULT for PUP screens is to show -
// they only exist during play - so we look for an explicit "puppack" or
// "-puppack" token and show when the lists say nothing.
static bool IsPupSuppressedForGame(GameListItem *game, GameSystem *system)
{
	// scan a space-delimited list for "[-]puppack"; returns true if found,
	// setting 'show' from the token's sense
	auto Scan = [](const TCHAR *p, bool &show) -> bool
	{
		static const TCHAR *id = _T("puppack");
		static const size_t idLen = 7;
		if (p == nullptr)
			return false;
		while (*p != 0)
		{
			const TCHAR *nxt = p;
			for (; *nxt != 0 && !_istspace(*nxt); ++nxt);
			bool sense = true;
			if (*p == '-')
			{
				sense = false;
				++p;
			}
			if (_tcsnicmp(p, id, idLen) == 0)
			{
				show = sense;
				return true;
			}
			for (p = nxt; _istspace(*p); ++p);
		}
		return false;
	};

	bool show;
	if (game != nullptr && Scan(GameList::Get()->GetShowWhenRunning(game), show))
		return !show;
	if (system != nullptr && Scan(system->keepOpen.c_str(), show))
		return !show;
	if (Scan(ConfigManager::GetInstance()->Get(_T("ShowWindowsWhileRunning")), show))
		return !show;

	// nothing configured - default is to show PUP screens
	return false;
}

void PupPackManager::Shutdown()
{
	delete inst;
	inst = nullptr;
}

std::vector<TSTRING> PupPackManager::ResolveRomCandidates(GameListItem *game) const
{
	std::vector<TSTRING> cands;
	auto add = [&cands](const TSTRING &r)
	{
		if (r.length() != 0 && std::find(cands.begin(), cands.end(), r) == cands.end())
			cands.push_back(r);
	};

	// the game database's explicit ROM setting takes precedence
	add(game->rom);

	// the VPinMAME registry match (can differ from the database ROM)
	TSTRING rom;
	if (VPinMAMEIfc::FindRom(rom, game))
		add(rom);

	// Per-ROM pack-folder alias.  PinballY has no ROMALT field like PinUP,
	// so this fills the same role: PupPack.RomAlias.<rom> maps a ROM to a
	// differently-named pack folder (e.g. a table whose ROM is mm_109c using
	// the "mm_10" pack) without renaming the ROM - which would break VPX's
	// game boot, DOF matching, and high-score tracking.  Checked for every
	// candidate found so far.
	for (size_t i = 0, n = cands.size(); i < n; ++i)
	{
		TSTRING key = TSTRING(ConfigVars::PupPackRomAliasPrefix) + cands[i];
		add(ConfigManager::GetInstance()->Get(key.c_str(), _T("")));
	}

	return cands;
}

void PupPackManager::BeginRunningGameMode(GameListItem *game, GameSystem *system)
{
	// forget any previous game's pack
	EndRunningGameMode();

	if (game == nullptr)
		return;

	auto lf = LogFile::Get();

	// Honor the standard Show When Running lists before doing anything:
	// an explicit "-puppack" suppresses the pack for this game entirely.
	if (IsPupSuppressedForGame(game, system))
	{
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: suppressed for this game by a -puppack Show When Running setting\n"));
		return;
	}

	// If enabled, stand up the PinDisplay COM shim now, INDEPENDENT of
	// whether a pack folder resolves below: tables with no database ROM
	// (or whose pack folder is named by the script rather than the ROM)
	// still connect and select their pack through Init at script load.
	if (ConfigManager::GetInstance()->GetBool(ConfigVars::PupPackPinDisplayCOM, false))
		PupComServer::Start();

	// get the PUPVIDEOS root folder from the settings
	TSTRING root = ConfigManager::GetInstance()->Get(ConfigVars::PupPackVideosPath, _T(""));
	if (root.empty())
	{
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: no PupPack.VideosPath setting, skipping pack search\n"));
		return;
	}

	// resolve the ROM name(s) to try for the pack folder lookup
	std::vector<TSTRING> roms = ResolveRomCandidates(game);
	if (roms.empty())
	{
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: no ROM name known for \"%s\", skipping pack search\n"),
			game->title.c_str());
		return;
	}

	// <root>\ prefix
	TSTRING base = root;
	if (base.back() != _T('\\') && base.back() != _T('/'))
		base += _T('\\');

	// use the first candidate ROM that actually has a pack folder
	TSTRING dir;
	for (auto &rom : roms)
	{
		TSTRING d = base + rom;
		if (::GetFileAttributes(d.c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			dir = d;
			break;
		}
	}
	if (dir.empty())
	{
		TSTRING tried;
		for (auto &rom : roms) { if (!tried.empty()) tried += _T(", "); tried += rom; }
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: no pack folder under %s for ROM(s): %s\n"),
			root.c_str(), tried.c_str());
		return;
	}

	// Load the pack.  Note that this scans the pack's media folders, so
	// it does a burst of disk I/O; packs observed in the wild load in
	// well under a second, which is acceptable within the launch flow.
	auto p = std::make_unique<pup::Pack>();
	pup::LoadResult lr = pup::LoadPack(WideToUtf8(dir.c_str()), *p);

	// Log load problems.  Real packs routinely have scores of benign
	// warnings (playlist rows whose folders were removed by the pack's
	// option installer), so cap the noise.
	const size_t maxWarnings = 10;
	for (size_t i = 0; i < lr.warnings.size() && i < maxWarnings; ++i)
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: warning: %s\n"), Utf8ToWide(lr.warnings[i]).c_str());
	if (lr.warnings.size() > maxWarnings)
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: (%d more warnings suppressed)\n"),
			(int)(lr.warnings.size() - maxWarnings));

	if (!lr.ok)
	{
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: load failed: %s\n"), Utf8ToWide(lr.error).c_str());
		return;
	}

	lf->Write(LogFile::TableLaunchLogging,
		_T("PUP pack loaded from %s: %d screens, %d playlists, %d triggers\n"),
		dir.c_str(), (int)p->screens.size(), (int)p->playlists.size(),
		(int)p->triggers.size());

	ActivatePack(std::move(p), WideToUtf8(dir.c_str()));
}

// Honor a pack-local pinupplayer.ini display layout (defined below).
static void LoadPackDisplayIni(const std::string &packRootU8);

// Bring a freshly loaded pack live: engine, DMD patterns, on-screen
// windows, the hardware-event tee (only if the pack needs it), and the
// pack-startup pseudo-trigger.  Shared by the ROM-folder path (game
// start) and the script-driven path (OnTableInit) so every activation
// step happens exactly once, at both entry points.
void PupPackManager::ActivatePack(std::unique_ptr<pup::Pack> p, const std::string &packDirU8)
{
	pack = std::move(p);
	engine = std::make_unique<pup::TriggerEngine>(*pack);

	// reset per-pack DMD diagnostic state (dump dir/counters live on the
	// manager so a pack switch doesn't inherit the previous pack's config
	// or PGM numbering)
	dmdDumpDir = ConfigManager::GetInstance()->Get(ConfigVars::PupPackDmdDumpDir, _T(""));
	dmdDumpNum = 0;
	dmdFrameCount = 0;

	// load the pack's PuPCapture DMD patterns, if it ships any
	bool dmd = LoadDmdPatterns(packDirU8);

	// honor a pack-local pinupplayer.ini display layout, if the pack ships one
	// (must precede CreateScreenWindows, which reads the resolved rects)
	LoadPackDisplayIni(packDirU8);

	// create the on-screen windows for the pack's displayable screens
	CreateScreenWindows();

	// Tee lifecycle, decided once here (not scattered): the VPinMAME
	// controller tee is needed if the pack keys off live hardware state
	// OR ships DMD patterns (DMD frames arrive through the same tee).
	// Either way it is gated by PupPack.PinMameEvents, so a user who
	// disables it to protect their VPinMAME/B2S stack is honored for
	// every pack.  EnableFrameSink is set unconditionally (false tears
	// down a stale sink left by a previous DMD pack on a mid-game switch).
	bool teeOn = false;
	bool wantTee = pack->HasHardwareTriggers() || dmd;
	bool pinMameEnabled = ConfigManager::GetInstance()->GetBool(ConfigVars::PupPackPinMameEvents, true);
	if (pinMameEnabled && wantTee)
		teeOn = PupEventBus::Start();
	// Ask the proxy for color frames only when this pack does exact-color
	// matching; every other pack keeps the cheaper luminance path unchanged.
	bool wantColor = dmd && teeOn && dmdMatcher != nullptr && dmdMatcher->IsExactColor();
	PupEventBus::EnableFrameSink(dmd && teeOn, wantColor);

	// Explain why live triggers aren't running when a pack wants them, so
	// a config-driven "nothing happens" is diagnosable instead of silent.
	if (wantTee && !pinMameEnabled)
	{
		// the user turned the tee off - name what that disables for THIS pack
		LogFile::Get()->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: this pack uses %s%s%s triggers, but PupPack.PinMameEvents is off; ")
			_T("those triggers are disabled\n"),
			pack->HasHardwareTriggers() ? _T("hardware (switch/lamp/GI/solenoid/mech)") : _T(""),
			(pack->HasHardwareTriggers() && dmd) ? _T(" and ") : _T(""),
			dmd ? _T("DMD (D<n>)") : _T(""));
	}
	else if (dmd && !teeOn)
	{
		// wanted the tee, tried to start it, and it failed - the specific
		// reason (proxy DLL missing, or the VPinMAME shadow owned by a real
		// PinUP install) was already logged by PupEventBus::Start.
		LogFile::Get()->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: DMD patterns loaded but the VPinMAME event tee could not start ")
			_T("(see the reason above); D<n> triggers disabled\n"));
	}

	// Fire PuP's pack-startup pseudo-trigger.  D0=1 rows are how CSV
	// packs establish their base layer at load time (default backgrounds,
	// overlays, attract loops, usually with a SetBG action); real PuP
	// fires it unconditionally when the pack starts, before any DMD
	// matching is involved.  A no-op for packs without D0 rows.
	PostEvent('D', 0, 1);
}

void PupPackManager::PostEvent(char type, int num, int state)
{
	if (engine == nullptr)
		return;

	// feed the engine, on a seconds-based monotonic clock
	auto cmds = engine->Post({ type, num, state }, GetTickCount64() / 1000.0);
	for (auto &c : cmds)
		Dispatch(c);
}

// Load the pack's PuPCapture reference frames (DMD-matched D<n>
// triggers), if the pack ships a PupCapture folder.  Returns true if any
// patterns loaded; the caller (ActivatePack) owns the tee/frame-sink
// lifecycle so it stays in one place.
bool PupPackManager::LoadDmdPatterns(const std::string &packDirU8)
{
	dmdMatcher.reset();
	fs::path capDir = fs::u8path(packDirU8) / "PupCapture";
	std::error_code ec;
	if (!fs::is_directory(capDir, ec))
		return false;

	auto m = std::make_unique<pup::DmdMatcher>();
	// A PuPCapture folder can ship an empty "ExactColorMatch.txt" to request
	// exact per-pixel color matching (the dmdext/pupcap "exact" method).
	// Detect it BEFORE loading so each pattern keeps its full reference RGB.
	std::error_code ecx;
	bool exactColor = fs::exists(capDir / "ExactColorMatch.txt", ecx);
	m->SetExactColor(exactColor);
	int bmpCandidates = 0;
	int n = m->LoadDir(capDir.u8string(), &bmpCandidates);
	if (n == 0)
	{
		// PupCapture had numeric-named BMPs but none loaded: a size
		// mismatch (the matcher requires the pack's DMD size).  Don't let
		// that fail silently - the author gets no D triggers otherwise.
		if (bmpCandidates > 0)
			LogFile::Get()->Write(LogFile::TableLaunchLogging,
				_T("PUP pack: PupCapture has %d BMP(s) but none are usable at %dx%d; ")
				_T("D<n> triggers unavailable\n"),
				bmpCandidates, pup::DmdMatcher::W, pup::DmdMatcher::H);
		return false;
	}

	// live DMD frames differ slightly from the authored reference frames
	// (animation phase, level quantization), so match on a fraction of
	// agreeing pixels; PupPack.DmdMatchPercent (default 92) tunes it,
	// 100 = exact.
	int pct = ConfigManager::GetInstance()->GetInt(ConfigVars::PupPackDmdMatchPercent, 92);
	if (pct < 50) pct = 50;
	if (pct > 100) pct = 100;

	// Exact-color packs ship precise, stable frames, so the fuzzy tolerance
	// would only invite false matches - require exact (100%) agreement,
	// overriding the configured percent.
	if (exactColor)
		pct = 100;
	m->SetMatchFraction(pct / 100.0f);

	dmdMatcher = std::move(m);
	LogFile::Get()->Write(LogFile::TableLaunchLogging,
		_T("PUP pack: loaded %d PuPCapture DMD pattern(s)%s\n"), n,
		exactColor ? _T(" (ExactColorMatch: per-pixel RGB)") : _T(""));
	return true;
}

// A live DMD frame arrived from the event bus (the VPinMAME tee samples
// frames on the table's own polling cadence).  Match it against the
// pack's PuPCapture patterns and post D<n> transitions to the engine.
void PupPackManager::OnDmdFrame(const uint8_t *px, int w, int h, int channels)
{
	if (dmdMatcher == nullptr || engine == nullptr)
		return;

	// Re-entrancy guard: a D<n> match dispatches media, which can pump the
	// message loop and deliver another WM_COPYDATA frame; re-entering
	// OnFrame while it iterates its patterns would corrupt match state.
	if (inDmdFrame)
		return;
	inDmdFrame = true;

	// log arrival (first frame, then every 500th), mirroring the event
	// counter, so the logs show whether the frame path is alive
	if (dmdFrameCount++ % 500 == 0)
		LogFile::Get()->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: DMD frame stream: %I64u unique frame(s) received (%dx%d, %dch)\n"),
			dmdFrameCount, w, h, channels);

	auto emit = [this](int num, int state)
	{
		if (state != 0)
			LogFile::Get()->Write(LogFile::TableLaunchLogging,
				_T("PUP pack: DMD match -> D%d\n"), num);
		PostEvent('D', num, state);
	};

	// An exact-color pack fed a color frame matches on per-pixel RGB; every
	// other case matches on luminance.
	bool colorMatch = (channels == 3 && dmdMatcher->IsExactColor());

	// A luminance view is needed for the diagnostic dump (always PGM) and for
	// the luminance match path; a color frame is reduced to its brightest
	// channel per pixel - the "lit level" AddPattern derives from the
	// reference BMP.  Derive it once, into a reused member buffer, only when
	// something below actually needs it.
	const uint8_t *lum = px;
	if (channels == 3 && (!colorMatch || !dmdDumpDir.empty()))
	{
		dmdLumBuf.resize((size_t)w * h);
		for (int i = 0, n = w * h; i < n; ++i)
		{
			uint8_t r = px[(size_t)i*3], g = px[(size_t)i*3+1], b = px[(size_t)i*3+2];
			dmdLumBuf[i] = r > g ? (r > b ? r : b) : (g > b ? g : b);   // brightest channel
		}
		lum = dmdLumBuf.data();
	}

	// Diagnostic frame dump for pack authors (PupPack.DmdDumpDir), for both
	// luminance and color packs; the sender already de-duplicates, so this
	// writes one PGM per distinct frame.
	if (!dmdDumpDir.empty())
	{
		TSTRINGEx fname;
		fname.Format(_T("%s\\frame%05d.pgm"), dmdDumpDir.c_str(), dmdDumpNum++);
		FILE *fp = nullptr;
		if (_tfopen_s(&fp, fname.c_str(), _T("wb")) == 0 && fp != nullptr)
		{
			fprintf(fp, "P5 %d %d 255 ", w, h);
			for (int i = 0, n = w * h; i < n; ++i)
				fputc(lum[i], fp);
			fclose(fp);
		}
	}

	if (colorMatch)
		dmdMatcher->OnFrameColor(px, w, h, emit);
	else
		dmdMatcher->OnFrame(lum, w, h, emit);

	inDmdFrame = false;
}

bool PupPackManager::Dispatch(const pup::PlayCommand &cmd)
{
	auto lf = LogFile::Get();
	auto it = windows.find(cmd.screenNum);
	PupView *view = it != windows.end() ? it->second->GetPupView() : nullptr;
	if (view == nullptr)
	{
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: play command for screen %d ignored (no window)\n"), cmd.screenNum);
		return false;
	}

	if (cmd.mediaPath.empty())
	{
		// Stop command: blank the screen (clearing its background
		// association so nothing resurrects it) and release the engine's
		// arbitration slot.
		view->StopAll();
		if (engine != nullptr)
			engine->ClearActive(cmd.screenNum);
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: screen %d: stop (trigger %s)\n"),
			cmd.screenNum, Utf8ToWide(cmd.sourceTriggerId).c_str());
		return true;
	}

	TSTRING wpath = Utf8ToWide(cmd.mediaPath);
	int vol = cmd.volume * Application::Get()->GetVideoVolume() / 100;

	// A "SetBG" trigger's media also becomes the screen's new background,
	// so future non-looping trigger media returns to it - record it before
	// starting playback, and play backgrounds looping (Resolve sets .loop
	// for SetBG too; the OR below just makes the invariant local).
	if (cmd.setAsBackground)
		view->SetBackground(wpath.c_str(), vol);

	if (view->PlayMedia(wpath.c_str(), cmd.loop || cmd.setAsBackground, vol, cmd.lengthSecs))
	{
		anyActiveVideo = true;
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: screen %d: trigger %s -> playing %s\n"),
			cmd.screenNum, Utf8ToWide(cmd.sourceTriggerId).c_str(), wpath.c_str());
		return true;
	}

	// The media failed to load; make sure the engine doesn't hold an
	// arbitration slot for media that never started.
	if (engine != nullptr)
		engine->ClearActive(cmd.screenNum);
	lf->Write(LogFile::TableLaunchLogging,
		_T("PUP pack: screen %d: trigger %s -> failed to load %s\n"),
		cmd.screenNum, Utf8ToWide(cmd.sourceTriggerId).c_str(), wpath.c_str());
	return false;
}

void PupPackManager::OnScreenMediaEnded(int screenNum)
{
	if (engine != nullptr)
		engine->ClearActive(screenNum);
}

void PupPackManager::OnGameLoaded()
{
	// The built-in windows took their topmost slots when running-game
	// mode began; put our overlay windows back above them.  (Skip hidden
	// audio-only hosts - SWP_NOACTIVATE keeps focus with the game.)
	for (auto &w : windows)
	{
		HWND h = w.second != nullptr ? w.second->GetHWnd() : nullptr;
		if (h != nullptr && IsWindow(h) && IsWindowVisible(h))
			SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
}

void PupPackManager::DirectPlay(int screenNum, const TCHAR *playlist, const TCHAR *file, int volPct)
{
	if (pack == nullptr)
		return;

	// resolve the file: an explicit name wins; otherwise pick one from the
	// playlist - the first file when it's alpha-sorted, a varying
	// (pseudo-random) one otherwise, matching real PuP's playlistplay.
	std::string playlistU8 = WideToUtf8(playlist), fileU8 = WideToUtf8(file);
	const pup::Playlist *pl = pack->FindPlaylist(playlistU8);
	if (fileU8.empty() && pl != nullptr && !pl->files.empty())
	{
		size_t idx = 0;
		if (!pl->alphaSort && pl->files.size() > 1)
			idx = static_cast<size_t>(GetTickCount64() % pl->files.size());
		fileU8 = pl->files[idx];
	}
	if (fileU8.empty())
		return;

	pup::PlayCommand cmd;
	cmd.screenNum = screenNum >= 0 ? screenNum : (pl != nullptr ? pl->screenNum : -1);
	// A negative volume means "use the playlist's own volume" (playlistplay);
	// playlistplayex passes an explicit level.
	cmd.volume = volPct >= 0 ? volPct : (pl != nullptr ? pl->volume : 100);
	cmd.priority = pl != nullptr ? pl->priority : 0;
	cmd.loop = false;
	cmd.sourceTriggerId = "playlistplayex";
	cmd.mediaPath = (fs::u8path(pack->rootPath) / fs::u8path(playlistU8) / fs::u8path(fileU8)).u8string();

	// Record script-driven media in the engine's arbitration state, so
	// triggers respect its priority and its end releases the screen.
	if (Dispatch(cmd) && engine != nullptr)
		engine->SetActive(cmd);
}

void PupPackManager::StopScreen(int screenNum)
{
	pup::PlayCommand cmd;
	cmd.screenNum = screenNum;
	cmd.sourceTriggerId = "playstop";
	Dispatch(cmd);
}

void PupPackManager::SetScreenVolume(int screenNum, int volPct)
{
	auto it = windows.find(screenNum);
	if (it == windows.end())
		return;
	if (PupView *view = it->second->GetPupView(); view != nullptr)
	{
		// scale by the global video volume, matching DirectPlay's playback level
		int vol = volPct * Application::Get()->GetVideoVolume() / 100;
		view->SetCurrentVolume(vol);
	}
}

void PupPackManager::SetLoop(int screenNum, int state)
{
	if (auto view = GetScreenView(screenNum); view != nullptr)
		view->SetMediaLooping(state != 0);
	else
		LogFile::Get()->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: SetLoop for screen %d ignored (no window)\n"), screenNum);
}

void PupPackManager::PauseScreen(int screenNum)
{
	if (auto view = GetScreenView(screenNum); view != nullptr)
		view->PauseMedia();
	else
		LogFile::Get()->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: playpause for screen %d ignored (no window)\n"), screenNum);
}

void PupPackManager::ResumeScreen(int screenNum)
{
	if (auto view = GetScreenView(screenNum); view != nullptr)
		view->ResumeMedia();
	else
		LogFile::Get()->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: playresume for screen %d ignored (no window)\n"), screenNum);
}

void PupPackManager::SetScreenBackground(int screenNum, int mode)
{
	auto lf = LogFile::Get();
	auto view = GetScreenView(screenNum);
	if (view == nullptr)
	{
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: SetBackground for screen %d ignored (no window)\n"), screenNum);
		return;
	}

	if (mode != 0)
	{
		// promote the screen's current media to its background
		if (view->SetCurrentAsBackground())
			lf->Write(LogFile::TableLaunchLogging,
				_T("PUP pack: screen %d: SetBackground: current media is now the background\n"),
				screenNum);
		else
			lf->Write(LogFile::TableLaunchLogging,
				_T("PUP pack: screen %d: SetBackground ignored (nothing playing)\n"), screenNum);
	}
	else
	{
		// mode 0 removes the background association
		view->ClearBackground();
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: screen %d: SetBackground(0): background association cleared\n"),
			screenNum);
	}
}

void PupPackManager::OnSendMsg(int screenNum, int fn)
{
	// SendMSG mt:301 trigger messages carry a screen number and a numeric
	// function code.  The function-code table isn't publicly documented,
	// and none of the codes observed so far require action beyond what
	// the dedicated PinDisplay methods already provide, so accept them
	// as no-ops; the COM layer has already logged the parsed values, so
	// the log shows what real tables send.
	(void)screenNum;
	(void)fn;
}

// Screen mode tests.  The modes seen in the wild are Show/ForceOn/
// ForceBack/ForcePoP/ForcePopBack (visible), MusicOnly (audio only),
// off, and empty (unused screen slot); some packs use the numeric
// convention "0" (off) and "1" (show) instead of the names (see
// Screen::mode in PupModel.h).  "1" is simply an active mode, so only
// "0" needs explicit handling here.  MusicOnly screens get a window
// too - a permanently hidden one - so that they can use the same media
// playback, looping, and background-restart machinery as visible screens.
static bool IsAudioOnlyScreenMode(const std::string &mode)
{
	return _stricmp(mode.c_str(), "musiconly") == 0;
}
static bool IsActiveScreenMode(const std::string &mode)
{
	return !mode.empty() && _stricmp(mode.c_str(), "off") != 0 && mode != "0";
}

// Resolution states for a PuP display area
enum class PupDisplayLoc
{
	Rect,      // resolved to a display rectangle
	Off,       // explicitly disabled (PupDisplay<N>.Position = off)
	Unknown    // no configuration and no window to map onto
};

// Per-pack display-rect overrides parsed from a pack-local pinupplayer.ini
// (see LoadPackDisplayIni).  Keyed by PuP display number; GetPupDisplayRect
// consults this before the global config.  Rebuilt on each pack activation.
static std::map<int, RECT> g_packDisplayOverride;

// Honor a pack-local pinupplayer.ini.  Real PuP uses a pinupplayer.ini in the
// pack's root folder, if present, in place of the global one for its window
// rects.  The display-number -> [INFO#] mapping still lives in the global
// config, so we recover it by matching each imported PupDisplay<N>.Position
// against the global ini's [INFO#] rects (a few px of tolerance, since import
// can round), then override that display with the pack ini's rect for the same
// INFO#.  No PinUP-database dependency; degrades to a logged no-op if the inis
// aren't found or don't line up.
static void LoadPackDisplayIni(const std::string &packRootU8)
{
	g_packDisplayOverride.clear();

	std::error_code ec;
	fs::path packIniPath = fs::u8path(packRootU8) / "pinupplayer.ini";
	if (!fs::exists(packIniPath, ec))
		return;

	auto lf = LogFile::Get();
	lf->Write(LogFile::TableLaunchLogging, _T("PUP pack: found pack-local pinupplayer.ini\n"));

	// the global ini sits two levels up: <PinUP>/PUPVideos/<rom>/
	fs::path globalIniPath = fs::u8path(packRootU8).parent_path().parent_path() / "PinUpPlayer.ini";

	auto readRect = [](const std::wstring &ini, int info, RECT &rc) -> bool
	{
		MsgFmt sec(_T("INFO%d"), info);
		int cw = ::GetPrivateProfileInt(sec, _T("ScreenWidth"), 0, ini.c_str());
		int ch = ::GetPrivateProfileInt(sec, _T("ScreenHeight"), 0, ini.c_str());
		if (cw <= 0 || ch <= 0) return false;
		int x = ::GetPrivateProfileInt(sec, _T("ScreenXPos"), 0, ini.c_str());
		int y = ::GetPrivateProfileInt(sec, _T("ScreenYPos"), 0, ini.c_str());
		rc = { x, y, x + cw, y + ch };
		return true;
	};

	std::wstring packIni = packIniPath.wstring(), globalIni = globalIniPath.wstring();
	std::map<int, RECT> globalInfo, packInfo;
	for (int i = 0; i <= 10; ++i)
	{
		RECT rc;
		if (readRect(globalIni, i, rc)) globalInfo[i] = rc;
		if (readRect(packIni, i, rc))   packInfo[i]   = rc;
	}
	if (packInfo.empty())
	{
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: pack-local pinupplayer.ini has no usable [INFO#] rects; ignoring\n"));
		return;
	}

	auto rectsNear = [](const RECT &a, const RECT &b) -> bool
	{
		auto d = [](int p, int q) { return abs(p - q) <= 8; };
		return d(a.left, b.left) && d(a.top, b.top) && d(a.right, b.right) && d(a.bottom, b.bottom);
	};

	for (int n = 0; n <= 10; ++n)
	{
		MsgFmt var(_T("PupDisplay%d.Position"), n);
		RECT cur = ConfigManager::GetInstance()->GetRect(var);
		if (IsRectEmpty(&cur))
			continue;
		for (auto &gi : globalInfo)
		{
			if (rectsNear(gi.second, cur))
			{
				if (auto pit = packInfo.find(gi.first); pit != packInfo.end())
				{
					g_packDisplayOverride[n] = pit->second;
					lf->Write(LogFile::TableLaunchLogging,
						_T("PUP pack: pinupplayer.ini overrides display %d -> %d,%d %dx%d\n"),
						n, pit->second.left, pit->second.top,
						pit->second.right - pit->second.left,
						pit->second.bottom - pit->second.top);
				}
				break;
			}
		}
	}

	if (g_packDisplayOverride.empty())
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: pinupplayer.ini present but no displays matched the global ")
			_T("INFO layout; using the global display config\n"));
}

// Resolve the base display area for a PuP display number.  The standard
// PuP display numbering is 0=Topper, 1=DMD, 2=Backglass, 3=Playfield,
// 4=Music (no display), 5=FullDMD/Apron, 6+=frontend extras.  An explicit
// PupDisplay<N>.Position config var takes precedence (these can be
// imported directly from a PinUP install's GlobalSettings display mapping
// + PinUpPlayer.ini [INFO#] rects, which is the same 3-hop scheme real
// PuP uses); "off" marks the display as intentionally absent (e.g., a cab
// with no topper, or a DMD monitor owned by dmd-extensions).  With no
// config, we map the standard numbers onto PinballY's own windows, which
// occupy the same physical monitors on a typical cab.
static PupDisplayLoc GetPupDisplayRect(int displayNum, RECT &rc)
{
	// a pack-local pinupplayer.ini (if the pack shipped one) overrides the
	// global display layout for this pack - it wins
	if (auto it = g_packDisplayOverride.find(displayNum); it != g_packDisplayOverride.end())
	{
		rc = it->second;
		return PupDisplayLoc::Rect;
	}

	// explicit config: PupDisplay<N>.Position = left,top,right,bottom | off
	MsgFmt var(_T("PupDisplay%d.Position"), displayNum);
	const TCHAR *raw = ConfigManager::GetInstance()->Get(var, _T(""));
	if (_tcsicmp(raw, _T("off")) == 0)
		return PupDisplayLoc::Off;
	RECT rcVar = ConfigManager::GetInstance()->GetRect(var);
	if (!IsRectEmpty(&rcVar))
	{
		rc = rcVar;
		return PupDisplayLoc::Rect;
	}

	// A non-empty value that didn't parse to a usable rect is a config
	// typo (not four integers, or a degenerate/inverted rect).  Warn once
	// per display number instead of silently falling back to the default
	// mapping - otherwise a bad PupDisplay<N>.Position looks like it did
	// nothing.  (An empty value is the normal "not configured" case and
	// is intentionally silent.)
	if (raw[0] != 0)
	{
		// Warn once per display number.  Unlike the per-pack DMD diagnostic
		// state (reset each activation), this is intentionally process-
		// scoped: PupDisplay<N>.Position is read from the settings file at
		// startup and doesn't change between packs, so a bad value stays bad
		// for the session - warning once is right, not once per game.
		static std::set<int> warned;
		if (warned.insert(displayNum).second)
			LogFile::Get()->Write(LogFile::TableLaunchLogging,
				_T("PUP pack: PupDisplay%d.Position = \"%s\" is not a valid ")
				_T("\"left,top,right,bottom\" rectangle (or is off/inverted); ")
				_T("ignoring it and using the default display mapping\n"),
				displayNum, raw);
	}

	// map onto the corresponding PinballY window
	auto app = Application::Get();
	FrameWin *fw = nullptr;
	switch (displayNum)
	{
	case 0:  fw = app->GetTopperWin(); break;
	case 1:
	case 5:  fw = app->GetDMDWin(); break;
	case 2:  fw = app->GetBackglassWin(); break;
	case 3:  fw = app->GetPlayfieldWin(); break;
	}
	if (fw != nullptr && fw->GetHWnd() != nullptr && IsWindow(fw->GetHWnd()))
	{
		GetWindowRect(fw->GetHWnd(), &rc);
		return PupDisplayLoc::Rect;
	}
	return PupDisplayLoc::Unknown;
}

void PupPackManager::CreateScreenWindows()
{
	auto lf = LogFile::Get();

	// the playfield window serves as the nominal owner
	HWND parent = nullptr;
	if (auto pfw = Application::Get()->GetPlayfieldWin(); pfw != nullptr)
		parent = pfw->GetHWnd();

	// Create back-layer screens (ForceBack/ForcePopBack) before front-layer
	// screens (ForceOn/ForcePoP/show), so that within the topmost band the
	// front-layer windows stack above the back-layer ones, mirroring PuP's
	// layering intent.
	std::vector<const pup::Screen*> ordered;
	for (auto &s : pack->screens)
		if (IsActiveScreenMode(s.mode))
			ordered.push_back(&s);
	std::stable_sort(ordered.begin(), ordered.end(),
		[](const pup::Screen *a, const pup::Screen *b) {
			auto rank = [](const pup::Screen *s) {
				return _strnicmp(s->mode.c_str(), "ForceBack", 9) == 0
					|| _strnicmp(s->mode.c_str(), "ForcePopBack", 12) == 0 ? 0 : 1;
			};
			return rank(a) < rank(b);
		});

	int nCreated = 0;
	for (auto ps : ordered)
		if (CreateScreenWindow(*ps) != nullptr)
			++nCreated;

	lf->Write(LogFile::TableLaunchLogging,
		_T("PUP pack: %d screen window(s) created\n"), nCreated);
}

// Create the window for one pack screen, register it in the windows map,
// and start its background media.  Returns the window, or null on failure.
PupWin *PupPackManager::CreateScreenWindow(const pup::Screen &s)
{
	auto lf = LogFile::Get();

	// Guard against duplicate ScreenNum rows (malformed packs only).
	// Creating a second window would silently orphan the first when it
	// took over the map slot: the RefPtr release doesn't destroy the
	// window, whose WM_NCCREATE self-reference keeps it alive on screen
	// with no owner.  Keep the original window and warn.
	if (auto it = windows.find(s.screenNum); it != windows.end())
	{
		lf->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: duplicate screen number %d ignored (screen window already exists)\n"),
			s.screenNum);
		return it->second;
	}

	// the playfield window serves as the nominal owner
	HWND parent = nullptr;
	if (auto pfw = Application::Get()->GetPlayfieldWin(); pfw != nullptr)
		parent = pfw->GetHWnd();

	{
		// Create the frame window.  The config var prefix keys the window
		// position to the pack screen number, so the layout is stable
		// across games and user-adjustable like any PinballY window.
		// Audio-only (MusicOnly) screens get a permanently hidden window,
		// purely as a host for their sound playback.
		bool audioOnly = IsAudioOnlyScreenMode(s.mode);
		MsgFmt prefix(_T("PupScreen%d"), s.screenNum);
		TSTRING title = _T("PUP ") + Utf8ToWide(s.description);
		RefPtr<PupWin> win(new PupWin(prefix.Get(), title.c_str()));
		if (!win->CreateWin(parent, audioOnly ? SW_HIDE : SW_SHOWNOACTIVATE, title.c_str()))
		{
			lf->Write(LogFile::TableLaunchLogging,
				_T("PUP pack: screen %d (%s): window creation failed\n"),
				s.screenNum, title.c_str());
			return nullptr;
		}

		// make sure audio-only windows stay hidden regardless of any
		// saved visibility config
		if (audioOnly)
			ShowWindow(win->GetHWnd(), SW_HIDE);

		// tell the view which pack screen it hosts
		if (win->GetPupView() != nullptr)
		{
			win->GetPupView()->SetScreenNum(s.screenNum);
			win->GetPupView()->SetAudioOnly(audioOnly);
		}

		// Place the window.  Deterministic, every launch:
		//   1. Pack CustomPos: a borderless sub-region of the referenced
		//      PuP display area (the pack author's explicit layout).
		//   2. Otherwise: fill the screen number's own display area,
		//      borderless (PuP screen numbers ARE display assignments).
		//   3. A display marked "off" or unresolvable: the window becomes
		//      a hidden audio host - media sound still plays, but nothing
		//      ever lands at an arbitrary desktop position.
		RECT rcBase;
		if (!audioOnly && s.hasCustomPos
			&& GetPupDisplayRect(s.posScreenRef, rcBase) == PupDisplayLoc::Rect)
		{
			int bw = rcBase.right - rcBase.left, bh = rcBase.bottom - rcBase.top;
			int x = rcBase.left + (int)(bw * s.posXPct / 100.0);
			int y = rcBase.top + (int)(bh * s.posYPct / 100.0);
			int cx = (int)(bw * s.posWPct / 100.0);
			int cy = (int)(bh * s.posHPct / 100.0);
			SetWindowPos(win->GetHWnd(), HWND_TOPMOST, x, y, cx, cy, SWP_NOACTIVATE);
			lf->Write(LogFile::TableLaunchLogging,
				_T("PUP pack: screen %d: CustomPos ref display %d -> %d,%d %dx%d\n"),
				s.screenNum, s.posScreenRef, x, y, cx, cy);
		}
		else if (!audioOnly)
		{
			if (s.hasCustomPos)
				lf->Write(LogFile::TableLaunchLogging,
					_T("PUP pack: screen %d: CustomPos references display %d, which is off or ")
					_T("unconfigured; falling back to the screen's own display mapping\n"),
					s.screenNum, s.posScreenRef);

			// The screen number is its own display assignment.
			// GetPupDisplayRect owns the whole policy: an explicit
			// PupDisplay<N>.Position for any N, then the standard-number
			// window map (0/1/2/3/5), else Unknown.  Extra pack slots
			// (4, 6+) with no config resolve Unknown and become hidden
			// audio hosts - the same fallback as "off".
			if (GetPupDisplayRect(s.screenNum, rcBase) == PupDisplayLoc::Rect)
			{
				SetWindowPos(win->GetHWnd(), HWND_TOPMOST,
					rcBase.left, rcBase.top,
					rcBase.right - rcBase.left, rcBase.bottom - rcBase.top,
					SWP_NOACTIVATE);
				lf->Write(LogFile::TableLaunchLogging,
					_T("PUP pack: screen %d: display area -> %d,%d %dx%d\n"),
					s.screenNum, rcBase.left, rcBase.top,
					rcBase.right - rcBase.left, rcBase.bottom - rcBase.top);
			}
			else
			{
				// no display for this screen on this cab - hidden audio host
				ShowWindow(win->GetHWnd(), SW_HIDE);
				if (win->GetPupView() != nullptr)
					win->GetPupView()->SetAudioOnly(true);
				lf->Write(LogFile::TableLaunchLogging,
					_T("PUP pack: screen %d: display %d is off/unconfigured; window hidden ")
					_T("(audio only). Set PupDisplay%d.Position = left,top,right,bottom to show it.\n"),
					s.screenNum, s.screenNum, s.screenNum);
			}
		}

		// Resolve the screen's background media: the explicit PlayFile if
		// given, else the first file of the background playlist.
		const pup::Playlist *pl = pack->FindPlaylist(s.backgroundPlaylist);
		std::string file = s.backgroundFile;
		if (file.empty() && pl != nullptr && !pl->files.empty())
			file = pl->files.front();

		if (!s.backgroundPlaylist.empty() && !file.empty())
		{
			// build the full path and play it (PupView handles both video
			// and still-image media)
			std::string mediaPath = (fs::u8path(pack->rootPath)
				/ fs::u8path(s.backgroundPlaylist) / fs::u8path(file)).u8string();
			TSTRING wpath = Utf8ToWide(mediaPath);

			// playlist volume scaled by the global video volume
			int vol = (pl != nullptr ? pl->volume : 100)
				* Application::Get()->GetVideoVolume() / 100;

			// Background/attract media loops.  (screens.pup has a Loopit
			// column, but packs in the wild leave it 0 even for their
			// looping backglass loops, so we always loop the base layer.)
			// Remember it as the screen's background, so the view can
			// return to it when non-looping trigger media finishes.
			if (win->GetPupView() != nullptr)
				win->GetPupView()->SetBackground(wpath.c_str(), vol);
			// (PupView applies black color-key transparency itself when the
			// media is an alpha-capable image, and removes it otherwise.)
			if (win->GetPupView() != nullptr
				&& win->GetPupView()->PlayMedia(wpath.c_str(), true, vol))
			{
				anyActiveVideo = true;
				lf->Write(LogFile::TableLaunchLogging,
					_T("PUP pack: screen %d (%s): playing %s\n"),
					s.screenNum, title.c_str(), wpath.c_str());
			}
			else
				lf->Write(LogFile::TableLaunchLogging,
					_T("PUP pack: screen %d (%s): failed to load %s\n"),
					s.screenNum, title.c_str(), wpath.c_str());
		}

		// keep the window
		PupWin *ret = win;
		windows[s.screenNum].Attach(win.Detach());
		return ret;
	}
}

// Ensure a window exists for a screen the table script names at runtime
// (script-only packs define their screens through PinDisplay Init calls
// rather than screens.pup).  PuP convention: screen 4 is the Music
// (audio-only) screen.
void PupPackManager::EnsureScreenWindow(int screenNum, bool audioOnly)
{
	if (pack == nullptr || screenNum < 0 || windows.count(screenNum) != 0)
		return;

	// Use the pack's screen definition if it has one; otherwise
	// synthesize one for the script-defined screen.  Either way, an
	// explicit Init from the table activates the screen even if the
	// pack's screens.pup marks it "off" - the script variant of the
	// pack expects to drive it.
	pup::Screen s;
	if (const pup::Screen *ps = pack->FindScreen(screenNum); ps != nullptr)
		s = *ps;
	else
	{
		s.screenNum = screenNum;
		s.description = "Screen " + std::to_string(screenNum);
	}
	if (!IsActiveScreenMode(s.mode))
		s.mode = audioOnly ? "MusicOnly" : "show";
	CreateScreenWindow(s);
}

void PupPackManager::OnTableInit(int screenNum, const TCHAR *packName)
{
	auto lf = LogFile::Get();

	// If the script names a different pack than the one we loaded from
	// the ROM lookup, and that folder exists with content, switch to it
	// (script-only packs are allowed).  An empty/missing folder keeps
	// the current pack: on many setups the script's pack name is an
	// empty placeholder folder while the media lives under the ROM name.
	std::string nameU8 = packName != nullptr ? WideToUtf8(packName) : "";
	if (!nameU8.empty() && (pack == nullptr || pack->romName != nameU8))
	{
		TSTRING root = ConfigManager::GetInstance()->Get(ConfigVars::PupPackVideosPath, _T(""));
		if (!root.empty())
		{
			fs::path dir = fs::path(root) / fs::u8path(nameU8);
			std::error_code ec;
			bool hasContent = false;
			if (fs::is_directory(dir, ec))
			{
				try
				{
					for (auto &e : fs::directory_iterator(dir, ec)) { (void)e; hasContent = true; break; }
				}
				catch (...) { }
			}
			if (hasContent)
			{
				lf->Write(LogFile::TableLaunchLogging,
					_T("PUP pack: table Init names pack \"%hs\"; switching to it\n"), nameU8.c_str());
				DestroyScreenWindows();
				auto p = std::make_unique<pup::Pack>();
				pup::LoadResult lr = pup::LoadPack(dir.u8string(), *p, true);
				if (lr.ok)
					ActivatePack(std::move(p), dir.u8string());
			}
			else if (pack != nullptr)
				lf->Write(LogFile::TableLaunchLogging,
					_T("PUP pack: table Init names pack \"%hs\" (missing/empty); keeping %hs\n"),
					nameU8.c_str(), pack->romName.c_str());
		}
	}

	// make sure the named screen exists
	EnsureScreenWindow(screenNum, screenNum == 4);
}

void PupPackManager::OnPlaylistAdd(int screenNum, const TCHAR *folder, int restSeconds)
{
	if (pack == nullptr || folder == nullptr)
		return;
	std::string f = WideToUtf8(folder);
	if (f.empty() || pack->FindPlaylist(f) != nullptr)
		return;

	// register the playlist, scanning its folder for media
	pup::Playlist p;
	p.screenNum = screenNum;
	p.folder = f;
	p.restSeconds = restSeconds;
	fs::path fp = fs::u8path(pack->rootPath) / fs::u8path(f);
	std::error_code ec;
	if (fs::is_directory(fp, ec))
	{
		try
		{
			for (auto &e : fs::directory_iterator(fp, ec))
				if (e.is_regular_file(ec))
					p.files.push_back(e.path().filename().u8string());
		}
		catch (...) { }
		std::sort(p.files.begin(), p.files.end());
	}
	pack->playlists.push_back(p);
}

void PupPackManager::HideScreen(int screenNum)
{
	// PuP "hide": the screen's display goes away but its audio keeps
	// playing - exactly our audio-only window behavior
	auto it = windows.find(screenNum);
	if (it != windows.end() && it->second != nullptr)
	{
		if (it->second->GetPupView() != nullptr)
			it->second->GetPupView()->SetAudioOnly(true);
		if (it->second->GetHWnd() != nullptr && IsWindow(it->second->GetHWnd()))
			ShowWindow(it->second->GetHWnd(), SW_HIDE);
	}
}

void PupPackManager::SetScreenGeometry(int screenNum, int xPct, int yPct, int wPct, int hPct)
{
	// setScreenEx positions the screen within its PuP display area, in
	// percentages; all zeros (or missing) means the full display
	auto it = windows.find(screenNum);
	if (it == windows.end() || it->second == nullptr)
		return;
	RECT rcBase;
	if (GetPupDisplayRect(screenNum, rcBase) != PupDisplayLoc::Rect)
		return;
	int bw = rcBase.right - rcBase.left, bh = rcBase.bottom - rcBase.top;
	RECT rc;
	if (wPct <= 0 || hPct <= 0)
		rc = rcBase;
	else
		rc = { rcBase.left + bw * xPct / 100, rcBase.top + bh * yPct / 100,
		       rcBase.left + bw * (xPct + wPct) / 100, rcBase.top + bh * (yPct + hPct) / 100 };
	SetWindowPos(it->second->GetHWnd(), HWND_TOPMOST,
		rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOACTIVATE);
	LogFile::Get()->Write(LogFile::TableLaunchLogging,
		_T("PUP pack: screen %d: setScreenEx -> %d,%d %dx%d\n"),
		screenNum, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
}

// Look up the view hosting a screen's window, if the window exists
PupView *PupPackManager::GetScreenView(int screenNum) const
{
	auto it = windows.find(screenNum);
	return it != windows.end() && it->second != nullptr ? it->second->GetPupView() : nullptr;
}

void PupPackManager::LabelInit(int screenNum)
{
	// Labels need a window to draw on.  Make sure the screen's window
	// exists, through the same path as a script Init call - but with the
	// current pack only; LabelInit never switches packs.
	if (pack == nullptr)
	{
		LogFile::Get()->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: LabelInit(%d) ignored (no pack loaded)\n"), screenNum);
		return;
	}
	EnsureScreenWindow(screenNum, screenNum == 4);
}

void PupPackManager::LabelNew(int screenNum, const TCHAR *name, const TCHAR *font,
	double sizePct, int color, int rotation, int xAlign, int yAlign,
	double xPos, double yPos, int pageNum, bool visible)
{
	// Rotation isn't implemented; label rotation is essentially unused
	// in packs in the wild.
	(void)rotation;

	if (auto view = GetScreenView(screenNum); view != nullptr)
		view->SetLabelStyle(name, font, (float)sizePct, (COLORREF)color,
			xAlign, yAlign, (float)xPos, (float)yPos, pageNum, visible);
	else
		LogFile::Get()->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: LabelNew for screen %d ignored (no window)\n"), screenNum);
}

void PupPackManager::LabelSet(int screenNum, const TCHAR *name, const TCHAR *text,
	bool visible, const TCHAR *special)
{
	if (auto view = GetScreenView(screenNum); view != nullptr)
		view->SetLabelText(name, text, visible, special);
	else
		LogFile::Get()->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: LabelSet for screen %d ignored (no window)\n"), screenNum);
}

void PupPackManager::LabelShowPage(int screenNum, int pageNum, int seconds, const TCHAR *special)
{
	// The timed-page variation and the "special" JSON aren't implemented;
	// a page selection simply stays until the next LabelShowPage.
	(void)seconds;
	(void)special;

	if (auto view = GetScreenView(screenNum); view != nullptr)
		view->SetLabelPage(pageNum);
	else
		LogFile::Get()->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: LabelShowPage for screen %d ignored (no window)\n"), screenNum);
}

void PupPackManager::DestroyScreenWindows()
{
	for (auto &w : windows)
	{
		if (w.second != nullptr && w.second->GetHWnd() != nullptr && IsWindow(w.second->GetHWnd()))
			DestroyWindow(w.second->GetHWnd());
	}
	windows.clear();
	anyActiveVideo = false;
}

PupPackManager::PupPackManager()
{
}

PupPackManager::~PupPackManager()
{
	PupComServer::Stop();
	DestroyScreenWindows();
}

void PupPackManager::EndRunningGameMode()
{
	if (pack != nullptr)
		LogFile::Get()->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: unloading pack %s\n"), Utf8ToWide(pack->romName).c_str());

	PupComServer::Stop();
	PupEventBus::Stop();
	DestroyScreenWindows();
	dmdMatcher.reset();
	engine.reset();
	pack.reset();
	// drop this pack's display-layout override so it can't leak into the gap
	// before the next pack loads (LoadPackDisplayIni also clears on activate)
	g_packDisplayOverride.clear();
}
