// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY

#include "../stdafx.h"
#include "PupComServer.h"
#include "PupPackManager.h"
#include "PupBusProtocol.h"
#include "../LogFile.h"
#include "../Utilities/FileUtil.h"
#include "../Utilities/ComUtil.h"   // VARIANTEx + <propvarutil.h> (VariantTo*WithDefault)

// Our private CLSID for the PinDisplay shim.  This is deliberately NOT
// PinUP's own CLSID; the ProgID entry under HKCU maps the friendly name
// onto our class for this user while the server is active.
// {D8F2A6B1-4E7C-4c59-9AE3-51B0C7D42F86}
static const CLSID CLSID_PupPinDisplay =
{ 0xd8f2a6b1, 0x4e7c, 0x4c59, { 0x9a, 0xe3, 0x51, 0xb0, 0xc7, 0xd4, 0x2f, 0x86 } };

static const TCHAR *ProgIDKey = _T("Software\\Classes\\PinUpPlayer.PinDisplay");
static const TCHAR *ClsidKeyFmt = _T("Software\\Classes\\CLSID\\%s");
static const TCHAR *ClsidStr = _T("{D8F2A6B1-4E7C-4C59-9AE3-51B0C7D42F86}");

// registration cookie for the class object; 0 -> not registered
static DWORD comCookie = 0;

static void ComLog(const TCHAR *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	LogFile::Get()->WriteV(false, LogFile::TableLaunchLogging, fmt, ap);
	va_end(ap);
}

// Scan a JSON-ish message string for a '"key":<int>' pair, without a
// JSON parser: the PinDisplay SendMSG payloads are tiny flat objects,
// and a full parser would be more attack surface than the job needs.
// Bounds-safe against arbitrary input: the scan walks quote-to-quote,
// every character test stops at the terminating null, and the value
// accumulator saturates rather than overflowing.  Returns true and
// sets val if the key is present with an integer value (optionally
// quoted and/or negative); the key match is case-insensitive, as
// table scripts vary.
static bool ScanJsonInt(const WCHAR *s, const WCHAR *key, int &val)
{
	if (s == nullptr || key == nullptr || key[0] == 0)
		return false;

	size_t keyLen = wcslen(key);
	for (const WCHAR *p = wcschr(s, L'"'); p != nullptr; p = wcschr(p + 1, L'"'))
	{
		// Match '"key"'.  The wcsnicmp stops at a null in the message,
		// so the close-quote test only reads within the string.
		if (_wcsnicmp(p + 1, key, keyLen) != 0 || p[1 + keyLen] != L'"')
			continue;

		// skip whitespace to the ':' separator
		const WCHAR *q = p + 2 + keyLen;
		for (; *q == L' ' || *q == L'\t'; ++q);
		if (*q != L':')
			continue;
		for (++q; *q == L' ' || *q == L'\t'; ++q);

		// accept an optionally quoted, optionally signed integer
		if (*q == L'"')
			++q;
		bool neg = (*q == L'-');
		if (neg)
			++q;
		if (*q < L'0' || *q > L'9')
			continue;
		int n = 0;
		for (; *q >= L'0' && *q <= L'9'; ++q)
			n = n < 100000000 ? n * 10 + (*q - L'0') : n;
		val = neg ? -n : n;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------
// The dispatch object handed to table scripts.  Late-bound only (that's
// how VBScript CreateObject uses it), so we implement IDispatch by hand.
// Known methods are routed to the PUP pack manager; everything else is
// accepted and ignored, for maximum compatibility with the wide variety
// of PinDisplay calls that table scripts make.
class PinDisplayDispatch : public IDispatch
{
public:
	PinDisplayDispatch() { }
	virtual ~PinDisplayDispatch() { }

	// dispatch IDs for the methods we act on
	enum
	{
		DISPID_Unhandled = 1000,
		DISPID_pupevent = 1,
		DISPID_B2SInit,
		DISPID_Init,
		DISPID_playlistplayex,
		DISPID_playlistplay,
		DISPID_playstop,
		DISPID_playlistadd,
		DISPID_hide,
		DISPID_setScreenEx,
		DISPID_LabelInit,
		DISPID_LabelNew,
		DISPID_LabelSet,
		DISPID_LabelShowPage,
		DISPID_SetLoop,
		DISPID_playpause,
		DISPID_playresume,
		DISPID_setVolumeCurrent,
		DISPID_SetBackground,
		DISPID_SendMSG
	};

	// IUnknown
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (riid == IID_IUnknown || riid == IID_IDispatch)
		{
			*ppv = static_cast<IDispatch*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCnt); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG n = InterlockedDecrement(&refCnt);
		if (n == 0) delete this;
		return n;
	}

	// IDispatch
	HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT *p) override { *p = 0; return S_OK; }
	HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }

	HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR *names, UINT cNames, LCID, DISPID *dispids) override
	{
		// PinDisplay method name -> DISPID.  One table instead of a long
		// if/else chain, so the name list lives in exactly one place.
		static const struct { const WCHAR *name; DISPID id; } methods[] = {
			{ L"pupevent", DISPID_pupevent },       { L"B2SInit", DISPID_B2SInit },
			{ L"Init", DISPID_Init },               { L"playlistplayex", DISPID_playlistplayex },
			{ L"playlistplay", DISPID_playlistplay },
			{ L"playstop", DISPID_playstop },       { L"playlistadd", DISPID_playlistadd },
			{ L"hide", DISPID_hide },               { L"setScreenEx", DISPID_setScreenEx },
			{ L"LabelInit", DISPID_LabelInit },     { L"LabelNew", DISPID_LabelNew },
			{ L"LabelSet", DISPID_LabelSet },       { L"LabelShowPage", DISPID_LabelShowPage },
			{ L"SetLoop", DISPID_SetLoop },         { L"playpause", DISPID_playpause },
			{ L"playresume", DISPID_playresume },   { L"SetBackground", DISPID_SetBackground },
			{ L"setVolumeCurrent", DISPID_setVolumeCurrent },
			{ L"SendMSG", DISPID_SendMSG },
		};
		for (UINT i = 0; i < cNames; ++i)
		{
			const WCHAR *n = names[i];
			dispids[i] = DISPID_Unhandled;
			for (auto &m : methods)
				if (_wcsicmp(n, m.name) == 0) { dispids[i] = m.id; break; }

			// Accept an unknown name as a no-op, logging it once so we can
			// see what real tables use.
			if (dispids[i] == DISPID_Unhandled)
				ComLog(_T("PUP COM: unhandled PinDisplay method \"%ws\" (accepted as no-op)\n"), n);
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Invoke(DISPID dispid, REFIID, LCID, WORD flags,
		DISPPARAMS *params, VARIANT *result, EXCEPINFO*, UINT*) override
	{
		if ((flags & (DISPATCH_METHOD | DISPATCH_PROPERTYGET)) == 0)
			return DISP_E_MEMBERNOTFOUND;

		// clear any return value
		if (result != nullptr)
			VariantInit(result);

		// Fetch the nth positional argument (rgvarg is in reverse order)
		// coerced to int or string.
		auto ArgCount = [params]() { return (int)params->cArgs; };
		auto IntArg = [params](int n, int dflt) -> int
		{
			if (n >= (int)params->cArgs) return dflt;
			return VariantToInt32WithDefault(params->rgvarg[params->cArgs - 1 - n], dflt);
		};
		auto DblArg = [params](int n, double dflt) -> double
		{
			if (n >= (int)params->cArgs) return dflt;
			return VariantToDoubleWithDefault(params->rgvarg[params->cArgs - 1 - n], dflt);
		};
		auto StrArg = [params](int n) -> TSTRING
		{
			if (n >= (int)params->cArgs) return _T("");
			VARIANTEx v;
			if (SUCCEEDED(VariantChangeType(&v, &params->rgvarg[params->cArgs - 1 - n], 0, VT_BSTR))
				&& v.bstrVal != nullptr)
				return v.bstrVal;
			return _T("");
		};

		auto ppm = PupPackManager::Get();
		switch (dispid)
		{
		case DISPID_pupevent:
			// pupevent <n>: a pulse on the E<n> trigger device
			if (ppm != nullptr && ArgCount() >= 1)
			{
				int n = IntArg(0, -1);
				ComLog(_T("PUP COM: pupevent %d\n"), n);
				if (n >= 0)
				{
					ppm->PostEvent('E', n, 1);
					ppm->PostEvent('E', n, 0);
				}
			}
			break;

		case DISPID_B2SInit:
			// B2SInit(unused, romName) - log the table's own idea of its
			// ROM; the DMD-splash side of this call has no analog here.
			ComLog(_T("PUP COM: B2SInit(\"%s\", \"%s\")\n"),
				StrArg(0).c_str(), StrArg(1).c_str());
			break;

		case DISPID_Init:
			// Init(screenNum, packName): the script-driven screen setup
			// call - select the named pack and materialize the screen
			if (ppm != nullptr)
			{
				int screen = IntArg(0, -1);
				TSTRING name = StrArg(1);
				ComLog(_T("PUP COM: Init(%d, \"%s\")\n"), screen, name.c_str());
				ppm->OnTableInit(screen, name.c_str());
			}
			break;

		case DISPID_playlistadd:
			// playlistadd(screenNum, folder, alphaSort, restSeconds)
			if (ppm != nullptr)
				ppm->OnPlaylistAdd(IntArg(0, -1), StrArg(1).c_str(), IntArg(3, 0));
			break;

		case DISPID_hide:
			if (ppm != nullptr)
			{
				int screen = IntArg(0, -1);
				ComLog(_T("PUP COM: hide(%d)\n"), screen);
				ppm->HideScreen(screen);
			}
			break;

		case DISPID_setScreenEx:
			// setScreenEx(screenNum, xPct, yPct, wPct, hPct, popup)
			if (ppm != nullptr)
				ppm->SetScreenGeometry(IntArg(0, -1), IntArg(1, 0), IntArg(2, 0), IntArg(3, 0), IntArg(4, 0));
			break;

		case DISPID_playlistplayex:
			// playlistplayex(screenNum, playlistDir, playFile, volume, forceplay)
			if (ppm != nullptr)
			{
				int screen = IntArg(0, -1);
				TSTRING playlist = StrArg(1), file = StrArg(2);
				int vol = IntArg(3, 100), force = IntArg(4, 0);
				ComLog(_T("PUP COM: playlistplayex(%d, \"%s\", \"%s\", %d, %d)\n"),
					screen, playlist.c_str(), file.c_str(), vol, force);
				ppm->DirectPlay(screen, playlist.c_str(), file.c_str(), vol);
			}
			break;

		case DISPID_playlistplay:
			// playlistplay(screenNum, playlistDir [, forceplay]): play the
			// playlist without naming a file - the manager picks one (first
			// for an alpha-sorted playlist, random otherwise) and uses the
			// playlist's own volume.  Volume -1 tells DirectPlay to use the
			// playlist default.
			if (ppm != nullptr)
			{
				int screen = IntArg(0, -1);
				TSTRING playlist = StrArg(1);
				ComLog(_T("PUP COM: playlistplay(%d, \"%s\")\n"), screen, playlist.c_str());
				ppm->DirectPlay(screen, playlist.c_str(), _T(""), -1);
			}
			break;

		case DISPID_playstop:
			if (ppm != nullptr)
			{
				int screen = IntArg(0, -1);
				ComLog(_T("PUP COM: playstop(%d)\n"), screen);
				ppm->StopScreen(screen);
			}
			break;

		case DISPID_LabelInit:
			// LabelInit(screenNum): enable labels on a screen
			if (ppm != nullptr)
			{
				int screen = IntArg(0, -1);
				ComLog(_T("PUP COM: LabelInit(%d)\n"), screen);
				ppm->LabelInit(screen);
			}
			break;

		case DISPID_LabelNew:
			// LabelNew(screenNum, labelName, fontName, size, color, rotation,
			//          xAlign, yAlign, xpos, ypos, pageNum, visible)
			// Size is a percentage of the screen height, positions are
			// percentages of the screen width/height, and the color is a
			// COLORREF-style BGR int (VBScript RGB()).
			if (ppm != nullptr)
				ppm->LabelNew(IntArg(0, -1), StrArg(1).c_str(), StrArg(2).c_str(),
					DblArg(3, 0.0), IntArg(4, 0xFFFFFF), IntArg(5, 0),
					IntArg(6, 0), IntArg(7, 0), DblArg(8, 0.0), DblArg(9, 0.0),
					IntArg(10, 1), IntArg(11, 1) != 0);
			break;

		case DISPID_LabelSet:
			// LabelSet(screenNum, labelName, text, visible, special)
			// No per-call logging here: real tables make thousands of
			// these in a single session.
			if (ppm != nullptr)
				ppm->LabelSet(IntArg(0, -1), StrArg(1).c_str(), StrArg(2).c_str(),
					IntArg(3, 1) != 0, StrArg(4).c_str());
			break;

		case DISPID_LabelShowPage:
			// LabelShowPage(screenNum, pageNum, seconds, special)
			if (ppm != nullptr)
			{
				int screen = IntArg(0, -1), page = IntArg(1, 1);
				ComLog(_T("PUP COM: LabelShowPage(%d, %d)\n"), screen, page);
				ppm->LabelShowPage(screen, page, IntArg(2, 0), StrArg(3).c_str());
			}
			break;

		case DISPID_SetLoop:
			// SetLoop(screenNum, loopState): toggle looping on the
			// screen's current media
			if (ppm != nullptr)
			{
				int screen = IntArg(0, -1), state = IntArg(1, 0);
				ComLog(_T("PUP COM: SetLoop(%d, %d)\n"), screen, state);
				ppm->SetLoop(screen, state);
			}
			break;

		case DISPID_playpause:
			// playpause(screenNum): pause the screen's current media
			if (ppm != nullptr)
			{
				int screen = IntArg(0, -1);
				ComLog(_T("PUP COM: playpause(%d)\n"), screen);
				ppm->PauseScreen(screen);
			}
			break;

		case DISPID_playresume:
			// playresume(screenNum): resume the screen's current media
			if (ppm != nullptr)
			{
				int screen = IntArg(0, -1);
				ComLog(_T("PUP COM: playresume(%d)\n"), screen);
				ppm->ResumeScreen(screen);
			}
			break;

		case DISPID_setVolumeCurrent:
			// setVolumeCurrent(screenNum, vol): live-adjust the currently
			// playing media's volume (ducking) without changing the default.
			if (ppm != nullptr)
			{
				int screen = IntArg(0, -1);
				int vol = IntArg(1, 100);
				ComLog(_T("PUP COM: setVolumeCurrent(%d, %d)\n"), screen, vol);
				ppm->SetScreenVolume(screen, vol);
			}
			break;

		case DISPID_SetBackground:
			// SetBackground(screenNum, mode): a nonzero mode marks the
			// screen's CURRENT media as its background (PuP's SetBG);
			// zero removes the association
			if (ppm != nullptr)
			{
				int screen = IntArg(0, -1), mode = IntArg(1, 1);
				ComLog(_T("PUP COM: SetBackground(%d, %d)\n"), screen, mode);
				ppm->SetScreenBackground(screen, mode);
			}
			break;

		case DISPID_SendMSG:
			// SendMSG(jsonString): a free-form JSON message.  The only
			// documented shape is the mt:301 trigger message, carrying a
			// screen number ("SN") and a function code ("FN"); scan for
			// those with the crash-proof string scanner and route them.
			// Anything else - including empty or malformed input - is
			// accepted as a no-op, with a truncated copy in the log so
			// we can see what real tables send.
			{
				TSTRING msg = StrArg(0);
				int mt = -1, sn = -1, fn = -1;
				if (ScanJsonInt(msg.c_str(), L"mt", mt) && mt == 301)
				{
					ScanJsonInt(msg.c_str(), L"SN", sn);
					ScanJsonInt(msg.c_str(), L"FN", fn);
					ComLog(_T("PUP COM: SendMSG trigger message: mt=301 SN=%d FN=%d\n"), sn, fn);
					if (ppm != nullptr)
						ppm->OnSendMsg(sn, fn);
				}
				else
					ComLog(_T("PUP COM: SendMSG accepted as no-op: %.200s\n"), msg.c_str());
			}
			break;

		case DISPID_Unhandled:
		default:
			// accepted no-op
			break;
		}
		return S_OK;
	}

private:
	volatile LONG refCnt = 1;
};

// ---------------------------------------------------------------------
// class factory
class PinDisplayFactory : public IClassFactory
{
public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (riid == IID_IUnknown || riid == IID_IClassFactory)
		{
			*ppv = static_cast<IClassFactory*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
	ULONG STDMETHODCALLTYPE Release() override { return 1; }

	HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID riid, void **ppv) override
	{
		if (outer != nullptr)
			return CLASS_E_NOAGGREGATION;
		PinDisplayDispatch *obj = new PinDisplayDispatch();
		HRESULT hr = obj->QueryInterface(riid, ppv);
		obj->Release();
		if (SUCCEEDED(hr))
			ComLog(_T("PUP COM: PinDisplay object created by a client\n"));
		return hr;
	}
	HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};

// static factory - lifetime of the process, refcount is a no-op
static PinDisplayFactory factory;

// ---------------------------------------------------------------------
// Registry helpers.  PinballY is a 32-bit process, but table scripts run
// in both 32-bit hosts (VBScript in 32-bit VPX/B2S) and 64-bit hosts
// (VPinballX64's script engine, 64-bit cscript), and the HKCU CLSID space
// is WOW64-redirected - so every entry is written to BOTH registry views.

static const REGSAM regViews[] = { KEY_WOW64_32KEY, KEY_WOW64_64KEY };

// Write a single HKCU key value in one WOW64 view.  A null valueName sets
// the key's default value.  The building block for both the both-views
// WriteRegDefault and the per-view tee registration.
static bool WriteRegValueView(const TSTRING &key, const TCHAR *valueName,
	const TCHAR *val, REGSAM view)
{
	HKEY hk;
	if (RegCreateKeyEx(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0,
		KEY_WRITE | view, nullptr, &hk, nullptr) != ERROR_SUCCESS)
		return false;
	bool ok = RegSetValueEx(hk, valueName, 0, REG_SZ,
		(const BYTE*)val, (DWORD)((_tcslen(val) + 1) * sizeof(TCHAR))) == ERROR_SUCCESS;
	RegCloseKey(hk);
	return ok;
}

// Set an HKCU key's default value in BOTH WOW64 views.
static bool WriteRegDefault(const TSTRING &key, const TCHAR *val)
{
	bool ok = true;
	for (REGSAM view : regViews)
		ok = WriteRegValueView(key, nullptr, val, view) && ok;
	return ok;
}

static TSTRING ReadRegDefault(HKEY root, const TSTRING &key)
{
	for (DWORD flag : { RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY })
	{
		TCHAR buf[512];
		DWORD len = sizeof(buf);
		if (RegGetValue(root, key.c_str(), nullptr, RRF_RT_REG_SZ | flag, nullptr, buf, &len) == ERROR_SUCCESS)
			return buf;
	}
	return _T("");
}

// delete a key tree under HKCU\Software\Classes in both registry views
static void DeleteClassesTree(const TCHAR *subkey)
{
	for (REGSAM view : regViews)
	{
		HKEY hk;
		if (RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\Classes"), 0,
			DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE | view, &hk) == ERROR_SUCCESS)
		{
			RegDeleteTree(hk, subkey);
			RegCloseKey(hk);
		}
	}
}

// HKCU class-shadow ownership guard + safe teardown, shared by both COM
// servers: only ever touch a shadow that still carries OUR own CLSID, so a
// real PinUP/VPinMAME registration is never overridden or deleted.
static bool ShadowOwnedByOther(const TSTRING &progIdKey, const TCHAR *clsidStr)
{
	TSTRING existing = ReadRegDefault(HKEY_CURRENT_USER, progIdKey + _T("\\CLSID"));
	return !existing.empty() && _tcsicmp(existing.c_str(), clsidStr) != 0;
}

static void RemoveShadowIfOurs(const TSTRING &progIdKey, const TCHAR *progIdName,
	const TCHAR *clsidStr)
{
	TSTRING existing = ReadRegDefault(HKEY_CURRENT_USER, progIdKey + _T("\\CLSID"));
	if (!existing.empty() && _tcsicmp(existing.c_str(), clsidStr) == 0)
	{
		DeleteClassesTree(progIdName);
		DeleteClassesTree(MsgFmt(_T("CLSID\\%s"), clsidStr).Get());
	}
}

bool PupComServer::Start()
{
	if (comCookie != 0)
		return true;

	// If someone else owns the HKCU ProgID, leave it alone and fail
	if (ShadowOwnedByOther(ProgIDKey, ClsidStr))
	{
		ComLog(_T("PUP COM: HKCU PinUpPlayer.PinDisplay already registered elsewhere; not overriding\n"));
		return false;
	}

	// register the live class object
	HRESULT hr = CoRegisterClassObject(CLSID_PupPinDisplay, &factory,
		CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &comCookie);
	if (FAILED(hr))
	{
		ComLog(_T("PUP COM: CoRegisterClassObject failed, %08lx\n"), hr);
		comCookie = 0;
		return false;
	}

	// write the HKCU ProgID -> CLSID -> LocalServer32 entries
	TCHAR exePath[MAX_PATH];
	GetModuleFileName(nullptr, exePath, countof(exePath));
	TSTRING progidClsidKey = TSTRING(ProgIDKey) + _T("\\CLSID");
	TSTRING clsidKey = MsgFmt(ClsidKeyFmt, ClsidStr).Get();
	bool ok = WriteRegDefault(ProgIDKey, _T("PinballY PUP PinDisplay"))
		&& WriteRegDefault(progidClsidKey, ClsidStr)
		&& WriteRegDefault(clsidKey, _T("PinballY PUP PinDisplay"))
		&& WriteRegDefault(clsidKey + _T("\\LocalServer32"), exePath);
	if (!ok)
	{
		ComLog(_T("PUP COM: registry setup failed\n"));
		Stop();
		return false;
	}

	ComLog(_T("PUP COM: PinUpPlayer.PinDisplay server active (HKCU shadow, %s)\n"), ClsidStr);
	return true;
}

void PupComServer::Stop()
{
	if (comCookie != 0)
	{
		CoRevokeClassObject(comCookie);
		comCookie = 0;
	}

	// remove our registry entries, but only if they're still ours
	RemoveShadowIfOurs(ProgIDKey, _T("PinUpPlayer.PinDisplay"), ClsidStr);
	ComLog(_T("PUP COM: PinUpPlayer.PinDisplay server stopped\n"));
}

bool PupComServer::IsRunning()
{
	return comCookie != 0;
}

// =====================================================================
// PupEventBus - receiver + registration for the VPinMAME controller tee
// (PupPinMameProxy32/64.dll).  See PupComServer.h for the design notes;
// the cross-process wire protocol lives in PupBusProtocol.h.

static const TCHAR *TeeProgIDName = _T("VPinMAME.Controller");
static const TCHAR *TeeProgIDKey = _T("Software\\Classes\\VPinMAME.Controller");

static HWND busHwnd = nullptr;         // hardware-event receiver (always up)
static HWND frameSinkHwnd = nullptr;   // DMD-frame receiver (only with patterns)
static UINT64 busEventCount = 0;

static LRESULT CALLBACK BusWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_COPYDATA)
	{
		auto cds = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
		if (cds != nullptr && cds->dwData == PUP_EVENT_MAGIC
			&& cds->lpData != nullptr && cds->cbData % sizeof(BusEvent) == 0)
		{
			int n = (int)(cds->cbData / sizeof(BusEvent));
			auto events = reinterpret_cast<const BusEvent*>(cds->lpData);
			if (auto ppm = PupPackManager::Get(); ppm != nullptr)
			{
				for (int i = 0; i < n; ++i)
					ppm->PostEvent((char)events[i].type, events[i].num, events[i].state);
			}

			// Log the first batch (proof of life), then every 500th
			// event, so lamp-change floods can't swamp the log.
			UINT64 before = busEventCount;
			busEventCount += n;
			if (before == 0 || (before / 500) != (busEventCount / 500))
				ComLog(_T("PUP event bus: %I64u hardware events received\n"), busEventCount);
			return TRUE;
		}

		// live DMD frame: {INT32 w, INT32 h, then w*h luminance bytes OR
		// w*h*3 RGB bytes} - the channel count is inferred from the payload
		// size (a color frame comes only from an exact-color pack).
		if (cds != nullptr && cds->dwData == PUP_FRAME_MAGIC
			&& cds->lpData != nullptr && cds->cbData >= 8)
		{
			auto hdr = reinterpret_cast<const INT32*>(cds->lpData);
			INT32 w = hdr[0], h = hdr[1];
			if (w > 0 && h > 0 && w <= PUP_DMD_MAX_W && h <= PUP_DMD_MAX_H)
			{
				DWORD payload = cds->cbData - 8;
				int channels = payload == (DWORD)(w * h) ? 1
					: payload == (DWORD)(w * h * 3) ? 3 : 0;
				if (channels != 0)
				{
					if (auto ppm = PupPackManager::Get(); ppm != nullptr)
						ppm->OnDmdFrame(reinterpret_cast<const uint8_t*>(cds->lpData) + 8,
							w, h, channels);
				}
			}
			return TRUE;
		}
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Create one of our invisible receiver windows on the UI thread (so
// payloads dispatch on the same thread as everything else).  Both the
// event bus and the frame sink use the same WndProc, distinguished by
// class name; RegisterClassExW is idempotent-safe (a second call for an
// existing class fails harmlessly, and we only need the class present).
static HWND CreateBusWindow(const wchar_t *className)
{
	WNDCLASSEXW wc = { sizeof(wc) };
	wc.lpfnWndProc = BusWndProc;
	wc.hInstance = GetModuleHandle(nullptr);
	wc.lpszClassName = className;
	RegisterClassExW(&wc);
	return CreateWindowExW(0, className, L"", WS_POPUP,
		0, 0, 0, 0, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
}

// The tee registers a per-view InprocServer32 shadow (below), which the
// PinDisplay server does not need - a LocalServer32 serves both bitnesses
// via COM marshaling from one registration.  Both share the ownership
// guard / safe teardown (ShadowOwnedByOther / RemoveShadowIfOurs) defined
// with the other registry helpers above.

// Register the tee shadow into ONE WOW64 registry view: the
// VPinMAME.Controller ProgID -> our CLSID, and the CLSID's InprocServer32 ->
// the bitness-matching proxy DLL.  This is deliberately per-view: an
// InprocServer32 must match the host process bitness, so we claim the ProgID
// only in a view where we can actually serve it.  The other view is left
// untouched, so a table of the other bitness keeps resolving the real (HKLM)
// VPinMAME - instead of resolving our shadowed ProgID to a CLSID with no
// server in its view, which would leave that-bitness tables with no
// controller at all.
static bool RegisterTeeShadowView(const TCHAR *clsidStr, const TCHAR *friendlyName,
	const TSTRING &dll, REGSAM view)
{
	TSTRING progId = TeeProgIDKey;
	TSTRING clsidKey = MsgFmt(_T("Software\\Classes\\CLSID\\%s"), clsidStr).Get();
	TSTRING inprocKey = clsidKey + _T("\\InprocServer32");
	return WriteRegValueView(progId, nullptr, friendlyName, view)
		&& WriteRegValueView(progId + _T("\\CLSID"), nullptr, clsidStr, view)
		&& WriteRegValueView(clsidKey, nullptr, friendlyName, view)
		&& WriteRegValueView(inprocKey, nullptr, dll.c_str(), view)
		&& WriteRegValueView(inprocKey, _T("ThreadingModel"), _T("Apartment"), view);
}

bool PupEventBus::Start()
{
	if (busHwnd != nullptr)
		return true;

	// if someone else owns an HKCU VPinMAME.Controller shadow, leave it alone
	if (ShadowOwnedByOther(TeeProgIDKey, PUP_TEE_CLSID_STR))
	{
		ComLog(_T("PUP event bus: HKCU VPinMAME.Controller already registered elsewhere; not overriding\n"));
		return false;
	}

	// locate the proxy DLLs next to the executable
	TCHAR exeDir[MAX_PATH];
	GetExeFilePath(exeDir, countof(exeDir));
	TSTRING dll32 = TSTRING(exeDir) + _T("\\") + PUP_TEE_DLL_32;
	TSTRING dll64 = TSTRING(exeDir) + _T("\\") + PUP_TEE_DLL_64;
	bool have32 = FileExists(dll32.c_str());
	bool have64 = FileExists(dll64.c_str());
	if (!have32 && !have64)
	{
		ComLog(_T("PUP event bus: PupPinMameProxy32/64.dll not found next to PinballY.exe; ")
			_T("pinmame hardware events unavailable\n"));
		return false;
	}

	if ((busHwnd = CreateBusWindow(PUP_BUS_WINDOW_CLASS)) == nullptr)
	{
		ComLog(_T("PUP event bus: receiver window creation failed\n"));
		return false;
	}
	busEventCount = 0;

	// Register the tee shadow per-view: only in the view(s) where we have a
	// matching-bitness proxy DLL, so the other bitness keeps resolving the
	// real VPinMAME instead of a serverless shadow.
	static const TCHAR *friendly = _T("PinballY PUP VPinMAME tee");
	bool ok = true;
	if (have32)
		ok = ok && RegisterTeeShadowView(PUP_TEE_CLSID_STR, friendly, dll32, KEY_WOW64_32KEY);
	if (have64)
		ok = ok && RegisterTeeShadowView(PUP_TEE_CLSID_STR, friendly, dll64, KEY_WOW64_64KEY);
	if (!ok)
	{
		ComLog(_T("PUP event bus: registry setup failed\n"));
		Stop();
		return false;
	}

	ComLog(_T("PUP event bus: VPinMAME.Controller tee active (HKCU shadow, %s; 32=%d 64=%d)\n"),
		PUP_TEE_CLSID_STR, have32 ? 1 : 0, have64 ? 1 : 0);
	return true;
}

void PupEventBus::Stop()
{
	RemoveShadowIfOurs(TeeProgIDKey, TeeProgIDName, PUP_TEE_CLSID_STR);
	EnableFrameSink(false);
	if (busHwnd != nullptr)
	{
		DestroyWindow(busHwnd);
		busHwnd = nullptr;
		ComLog(_T("PUP event bus: tee stopped, registry shadow removed\n"));
	}
}

void PupEventBus::EnableFrameSink(bool enable, bool wantColor)
{
	// The frame sink window's existence is how the proxy discovers that
	// DMD frames are wanted; without it, the proxy skips all DMD sampling.
	if (enable && frameSinkHwnd == nullptr && busHwnd != nullptr)
		frameSinkHwnd = CreateBusWindow(PUP_FRAME_SINK_CLASS);
	else if (!enable && frameSinkHwnd != nullptr)
	{
		DestroyWindow(frameSinkHwnd);
		frameSinkHwnd = nullptr;
	}

	// The sink window's title tells the proxy which frames to sample: the
	// color marker for an exact-color pack (RGB), empty for luminance.  Set
	// it whenever the sink exists so a pack switch updates the mode.
	if (frameSinkHwnd != nullptr)
		SetWindowTextW(frameSinkHwnd, wantColor ? PUP_FRAME_SINK_COLOR_TITLE : L"");
}

bool PupEventBus::IsRunning()
{
	return busHwnd != nullptr;
}
