// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY
//
// PupPinMameProxy - VPinMAME.Controller pass-through tee
//
// A tiny in-process COM server that stands in for VPinMAME.Controller
// while a PUP-pack game is running.  PinballY registers it as an HKCU
// shadow of the VPinMAME ProgID before the game process starts (the same
// game-scoped shadow scheme as the PinDisplay shim), so the table script's
// CreateObject("VPinMAME.Controller") - or B2S.Server's, which creates
// the controller the same late-bound way - resolves here.  We create the
// REAL VPinMAME controller from its HKLM registration and forward every
// IDispatch call to it verbatim.
//
// The point of the tee: the pinmame state traffic the table already
// polls (ChangedLamps, ChangedSolenoids, ChangedGIStrings, Switch writes,
// GetMech) passes through our Invoke, so we can observe it and forward
// hardware events to PinballY's PUP event bus window via WM_COPYDATA -
// no extra polling, no second pinmame instance, no modification of the
// installed VPinMAME/B2S/DOF stack.  If a host binds the controller's
// dual-interface vtable instead of IDispatch (no known table script does;
// VBScript can't), QueryInterface forwards to the real controller so the
// host still works - we just see no events from that interface.
//
// When PinballY's frame sink window exists (the pack loaded PuPCapture
// DMD patterns), we also sample RawDmdPixels - piggybacked on the same
// observed polls, so always on the script thread with no marshaling -
// and forward changed frames for D<n> trigger matching.
//
// The DLL is built for both bitnesses (PupPinMameProxy32/64.dll): the
// proxy must match the host process (VPinballX64 loads the 64-bit one).
// PinballY (32-bit) receives everything cross-process via WM_COPYDATA;
// the wire protocol shared with PinballY lives in ../PupBusProtocol.h.
//
// This file implements only the public COM plumbing and observes only
// the documented VPinMAME automation surface; it contains no VPinMAME,
// PinUP, or B2S code.

#include <windows.h>
#include <oleauto.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <new>
#include "../PupBusProtocol.h"

// The real VPinMAME.Controller CLSID, read from HKLM at first use so we
// bypass our own HKCU ProgID shadow; this constant is the documented
// VPinMAME CLSID and serves as the fallback if the HKLM read fails.
static const CLSID CLSID_VPinMAME_Default =
	{ 0xf389c8b7, 0x144f, 0x4c63, { 0xa2, 0xe3, 0x24, 0x6d, 0x16, 0x8f, 0x9d, 0x39 } };

static HMODULE hModule;
static LONG lockCount;

// ---------------------------------------------------------------------
// Minimal diagnostics log (append-only, tiny traffic: a few lines per
// game session).  %TEMP%\PupPinMameProxy.log
static void Log(const char *fmt, ...)
{
	char path[MAX_PATH];
	DWORD n = GetTempPathA(MAX_PATH, path);
	if (n == 0 || n > MAX_PATH - 32)
		return;
	strcat_s(path, "PupPinMameProxy.log");
	FILE *fp = nullptr;
	if (fopen_s(&fp, path, "a") == 0 && fp != nullptr)
	{
		SYSTEMTIME st;
		GetLocalTime(&st);
		fprintf(fp, "%02d:%02d:%02d.%03d [%lu] ",
			st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
			GetCurrentProcessId());
		va_list ap;
		va_start(ap, fmt);
		vfprintf(fp, fmt, ap);
		va_end(ap);
		fputc('\n', fp);
		fclose(fp);
	}
}

// ---------------------------------------------------------------------
// Bus senders.  Receiver handles are cached and re-found when stale; the
// frame sink is optional (it exists only when PinballY wants frames).

static HWND FindBusWindow()
{
	static HWND hwnd = nullptr;
	if (hwnd == nullptr || !IsWindow(hwnd))
		hwnd = FindWindowW(PUP_BUS_WINDOW_CLASS, nullptr);
	return hwnd;
}

static HWND FindFrameSink()
{
	// The sink can appear after we start (pack activation timing), so a
	// failed lookup retries on later calls; the sampler's own throttle
	// keeps the retry rate harmless.
	static HWND hwnd = nullptr;
	if (hwnd == nullptr || !IsWindow(hwnd))
		hwnd = FindWindowW(PUP_FRAME_SINK_CLASS, nullptr);
	return hwnd;
}

// Send one payload to the bus window.  A short timeout so a wedged
// receiver can never stall the game's script thread; dropped payloads
// are strictly better than jitter.
static void SendToBus(DWORD magic, void *data, DWORD cb)
{
	HWND hwnd = FindBusWindow();
	if (hwnd == nullptr)
		return;
	COPYDATASTRUCT cds = { magic, cb, data };
	DWORD_PTR result;
	// Short cap: this runs on the game's script thread, so a busy (but not
	// hung) PinballY UI thread must never stall it for long - a dropped
	// event/frame is better than table stutter.  SMTO_ABORTIFHUNG only
	// covers a truly hung target, so the explicit 20ms cap does the work.
	SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, (LPARAM)&cds,
		SMTO_ABORTIFHUNG | SMTO_BLOCK, 20, &result);
}

// ---------------------------------------------------------------------
// VARIANT -> int, with a direct fast path for the numeric types VPinMAME
// actually returns (these hold no resources, so no VariantClear needed)
// and a VariantChangeType fallback for anything exotic.
static bool VarToI4(const VARIANT *v, INT32 &out)
{
	switch (V_VT(v))
	{
	case VT_I4:   out = V_I4(v); return true;
	case VT_I2:   out = V_I2(v); return true;
	case VT_UI1:  out = V_UI1(v); return true;
	case VT_INT:  out = V_INT(v); return true;
	case VT_BOOL: out = V_BOOL(v) ? 1 : 0; return true;
	default:
		{
			VARIANT cv;
			VariantInit(&cv);
			bool ok = SUCCEEDED(VariantChangeType(&cv, const_cast<VARIANT*>(v), 0, VT_I4));
			if (ok)
				out = V_I4(&cv);
			VariantClear(&cv);
			return ok;
		}
	}
}

// ---------------------------------------------------------------------
// Change-list parser.  The VPinMAME Changed* properties return a 2-D
// SAFEARRAY (rows x 2: number, state), element type VARIANT or I4
// depending on the property.  Walk the raw data in one lock; SAFEARRAY
// data is column-major, so element (row, col) sits at [col * rows + row].

static void ForwardChangeList(const VARIANT *result, INT32 typeCode)
{
	if (result == nullptr || !(V_VT(result) & VT_ARRAY))
		return;
	SAFEARRAY *sa = (V_VT(result) & VT_BYREF) ? *V_ARRAYREF(result) : V_ARRAY(result);
	if (sa == nullptr || SafeArrayGetDim(sa) != 2)
		return;

	LONG lo0, hi0, lo1, hi1;
	if (FAILED(SafeArrayGetLBound(sa, 1, &lo0)) || FAILED(SafeArrayGetUBound(sa, 1, &hi0))
		|| FAILED(SafeArrayGetLBound(sa, 2, &lo1)) || FAILED(SafeArrayGetUBound(sa, 2, &hi1)))
		return;
	LONG rows = hi0 - lo0 + 1, cols = hi1 - lo1 + 1;
	if (rows <= 0 || cols < 2)
		return;

	VARTYPE vt;
	if (FAILED(SafeArrayGetVartype(sa, &vt)))
		return;

	// the change lists tables poll are small; clamp defensively
	BusEvent events[256];
	int count = 0;
	void *data = nullptr;
	if (FAILED(SafeArrayAccessData(sa, &data)))
		return;
	for (LONG row = 0; row < rows && count < 256; ++row)
	{
		INT32 num = 0, state = 0;
		bool ok = false;
		if (vt == VT_VARIANT)
		{
			auto pv = static_cast<const VARIANT*>(data);
			ok = VarToI4(&pv[row], num) && VarToI4(&pv[rows + row], state);
		}
		else if (vt == VT_I4 || vt == VT_INT)
		{
			auto p32 = static_cast<const INT32*>(data);
			num = p32[row];
			state = p32[rows + row];
			ok = true;
		}
		else if (vt == VT_I2)
		{
			auto p16 = static_cast<const SHORT*>(data);
			num = p16[row];
			state = p16[rows + row];
			ok = true;
		}
		if (ok)
			events[count++] = { typeCode, num, state };
	}
	SafeArrayUnaccessData(sa);

	if (count > 0)
		SendToBus(PUP_EVENT_MAGIC, events, count * sizeof(BusEvent));
}

// ---------------------------------------------------------------------
// The controller tee

class ControllerTee : public IDispatch
{
public:
	ControllerTee() : refCnt(1), inner(nullptr)
	{
		InterlockedIncrement(&lockCount);
		memset(dispids, -1, sizeof(dispids));
	}

	~ControllerTee()
	{
		if (inner != nullptr)
			inner->Release();
		InterlockedDecrement(&lockCount);
	}

	// Create the REAL VPinMAME controller.  Read its CLSID from the
	// HKLM ProgID entry explicitly (our shadow lives in HKCU, so this
	// can't recurse into us), falling back to the documented CLSID.
	HRESULT CreateInner()
	{
		CLSID clsid = CLSID_VPinMAME_Default;
		wchar_t buf[64];
		DWORD cb = sizeof(buf);
		if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\VPinMAME.Controller\\CLSID",
			nullptr, RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS)
		{
			CLSID parsed;
			if (SUCCEEDED(CLSIDFromString(buf, &parsed)))
				clsid = parsed;
		}

		HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
			IID_IDispatch, (void**)&inner);
		Log("inner controller create hr=%08lx", hr);
		return hr;
	}

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
	{
		if (riid == IID_IUnknown || riid == IID_IDispatch)
		{
			*ppv = static_cast<IDispatch*>(this);
			AddRef();
			return S_OK;
		}

		// Unknown interface: forward to the real controller so vtable-
		// binding hosts keep working (they bypass the tee, by design).
		if (inner != nullptr)
		{
			HRESULT hr = inner->QueryInterface(riid, ppv);
			if (SUCCEEDED(hr))
			{
				Log("QI forwarded to inner (tee bypassed for this interface)");
				return hr;
			}
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refCnt); }
	STDMETHODIMP_(ULONG) Release()
	{
		ULONG n = InterlockedDecrement(&refCnt);
		if (n == 0) delete this;
		return n;
	}

	// IDispatch - pure pass-through, with observation
	STDMETHODIMP GetTypeInfoCount(UINT *pctinfo)
		{ return inner != nullptr ? inner->GetTypeInfoCount(pctinfo) : E_NOTIMPL; }
	STDMETHODIMP GetTypeInfo(UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo)
		{ return inner != nullptr ? inner->GetTypeInfo(iTInfo, lcid, ppTInfo) : E_NOTIMPL; }

	STDMETHODIMP GetIDsOfNames(REFIID riid, LPOLESTR *rgszNames, UINT cNames,
		LCID lcid, DISPID *rgDispId)
	{
		if (inner == nullptr)
			return E_FAIL;
		HRESULT hr = inner->GetIDsOfNames(riid, rgszNames, cNames, lcid, rgDispId);

		// learn the DISPIDs of the members we observe
		if (SUCCEEDED(hr) && cNames >= 1 && rgszNames[0] != nullptr)
		{
			static const wchar_t *names[ObsCount] = {
				L"ChangedLamps", L"ChangedSolenoids", L"ChangedGIStrings",
				L"Switch", L"GetMech", L"Run", L"Stop"
			};
			for (int i = 0; i < ObsCount; ++i)
			{
				if (_wcsicmp(rgszNames[0], names[i]) == 0)
				{
					dispids[i] = rgDispId[0];
					break;
				}
			}
		}
		return hr;
	}

	STDMETHODIMP Invoke(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags,
		DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo,
		UINT *puArgErr)
	{
		if (inner == nullptr)
			return E_FAIL;

		// An unlearned observed member sits at the -1 init sentinel; guard
		// on it so a real Invoke with DISPID -1 (DISPID_UNKNOWN) can't match
		// an unlearned slot and get misparsed as a change list.  Valid
		// learned DISPIDs are always >= 0.
		bool observable = dispIdMember >= 0;

		// A caller that doesn't want the result still produces one for
		// the observed change lists, so we can parse it after the call.
		VARIANT localResult;
		VariantInit(&localResult);
		bool wantObserve = observable
			&& (dispIdMember == dispids[ObsChangedLamps]
			|| dispIdMember == dispids[ObsChangedSolenoids]
			|| dispIdMember == dispids[ObsChangedGI]);
		VARIANT *pResult = pVarResult;
		if (wantObserve && pResult == nullptr)
			pResult = &localResult;

		HRESULT hr = inner->Invoke(dispIdMember, riid, lcid, wFlags,
			pDispParams, pResult, pExcepInfo, puArgErr);

		if (SUCCEEDED(hr) && observable)
		{
			if (dispIdMember == dispids[ObsChangedLamps])
			{
				ForwardChangeList(pResult, 'L');
				SampleDmd();
			}
			else if (dispIdMember == dispids[ObsChangedSolenoids])
			{
				ForwardChangeList(pResult, 'S');
				SampleDmd();
			}
			else if (dispIdMember == dispids[ObsChangedGI])
				ForwardChangeList(pResult, 'G');
			else if (dispIdMember == dispids[ObsSwitch] && (wFlags & DISPATCH_PROPERTYPUT) != 0)
			{
				// Switch(n) = state: named-arg layout puts the new value
				// first (DISPID_PROPERTYPUT) and the index after it
				INT32 num, state;
				if (pDispParams != nullptr && pDispParams->cArgs >= 2
					&& VarToI4(&pDispParams->rgvarg[1], num)
					&& VarToI4(&pDispParams->rgvarg[0], state))
				{
					BusEvent ev = { 'W', num, state };
					SendToBus(PUP_EVENT_MAGIC, &ev, sizeof(ev));
				}
			}
			else if (dispIdMember == dispids[ObsGetMech] && pResult != nullptr
				&& pDispParams != nullptr && pDispParams->cArgs >= 1)
			{
				INT32 num, state;
				if (VarToI4(&pDispParams->rgvarg[0], num) && VarToI4(pResult, state))
				{
					BusEvent ev = { 'M', num, state };
					SendToBus(PUP_EVENT_MAGIC, &ev, sizeof(ev));
				}
			}
			else if (dispIdMember == dispids[ObsRun])
				Log("controller Run observed");
			else if (dispIdMember == dispids[ObsStop])
				Log("controller Stop observed");
		}

		VariantClear(&localResult);
		return hr;
	}

protected:
	ULONG refCnt;
	IDispatch *inner;

	// observed member DISPIDs, learned through GetIDsOfNames
	enum Obs
	{
		ObsChangedLamps, ObsChangedSolenoids, ObsChangedGI,
		ObsSwitch, ObsGetMech, ObsRun, ObsStop, ObsCount
	};
	DISPID dispids[ObsCount];

	// ------------------------------------------------------------------
	// DMD frame sampling (PuPCapture D<n> triggers).  Piggybacked on the
	// table's own polling: each time we observe a Changed* poll we may
	// also read RawDmdPixels from the real controller - same thread, so
	// no cross-apartment marshaling - throttled, and skipped entirely
	// unless PinballY's frame sink window exists (i.e., the current pack
	// actually loaded DMD patterns).  Unchanged frames aren't resent.

	DISPID dispidRawPixels = -2, dispidRawWidth = -2, dispidRawHeight = -2;
	DISPID dispidColoredPixels = -2;   // RawDmdColoredPixels (exact-color packs)
	ULONGLONG lastSampleTicks = 0;
	ULONGLONG lastTitleTicks = 0;      // last sink-title (color-mode) read
	bool cachedWantColor = false;
	UINT32 lastFrameHash = 0;
	int dmdW = 0, dmdH = 0;    // cached after the first successful read

	// Unpack a VPinMAME pixel SAFEARRAY of length >= n into a caller sink,
	// which receives (index, pixelValue) per pixel.  Both RawDmdPixels and
	// RawDmdColoredPixels return VT_ARRAY|VT_VARIANT (elements VT_UI1 resp.
	// VT_UI4) in real VPinMAME; the bare VT_UI1/int SAFEARRAY forms are
	// accepted defensively.  Returns false on any unexpected shape.
	template <class Sink>
	bool UnpackDmdArray(VARIANT &v, int n, Sink sink)
	{
		if ((V_VT(&v) & VT_ARRAY) == 0)
			return false;
		SAFEARRAY *sa = (V_VT(&v) & VT_BYREF) ? *V_ARRAYREF(&v) : V_ARRAY(&v);
		if (sa == nullptr || SafeArrayGetDim(sa) != 1)
			return false;
		LONG lo, hi;
		SafeArrayGetLBound(sa, 1, &lo);
		SafeArrayGetUBound(sa, 1, &hi);
		if ((hi - lo + 1) < n)
			return false;
		VARTYPE vt;
		SafeArrayGetVartype(sa, &vt);
		void *data = nullptr;
		if (SafeArrayAccessData(sa, &data) != S_OK)
			return false;
		bool ok = true;
		if (vt == VT_UI1)
		{
			auto p = static_cast<const BYTE*>(data);
			for (int i = 0; i < n; ++i) sink(i, (UINT32)p[i]);
		}
		else if (vt == VT_I4 || vt == VT_UI4 || vt == VT_INT)
		{
			auto p = static_cast<const INT32*>(data);
			for (int i = 0; i < n; ++i) sink(i, (UINT32)p[i]);
		}
		else if (vt == VT_VARIANT)
		{
			auto pv = static_cast<const VARIANT*>(data);
			for (int i = 0; i < n; ++i)
			{
				INT32 iv;
				if (!VarToI4(&pv[i], iv)) { ok = false; break; }
				sink(i, (UINT32)iv);
			}
		}
		else
			ok = false;
		SafeArrayUnaccessData(sa);
		return ok;
	}

	void SampleDmd()
	{
		// Throttle FIRST, so a pack with no DMD capture (the common case,
		// frame sink absent) does not run a top-level window enumeration on
		// every lamp/solenoid poll (~60-100Hz) - only ~15Hz.
		ULONGLONG now = GetTickCount64();
		if (now - lastSampleTicks < 66)
			return;
		lastSampleTicks = now;
		HWND sink = FindFrameSink();
		if (sink == nullptr)
			return;

		// The sink title tells us which frames this pack wants (color marker
		// -> RGB, empty -> luminance).  It only changes on pack activation, so
		// read it at most ~once/sec rather than on every poll - it's a
		// cross-process WM_GETTEXT round-trip.
		if (now - lastTitleTicks > 1000)
		{
			wchar_t sinkTitle[16] = {};
			GetWindowTextW(sink, sinkTitle, 16);
			cachedWantColor = (wcscmp(sinkTitle, PUP_FRAME_SINK_COLOR_TITLE) == 0);
			lastTitleTicks = now;
		}
		bool wantColor = cachedWantColor;

		// resolve the raw-DMD DISPIDs once (-2 = unresolved, -1 = absent)
		if (dispidRawPixels == -2)
		{
			auto resolve = [this](const wchar_t *name) -> DISPID
			{
				LPOLESTR n = const_cast<LPOLESTR>(name);
				DISPID id = -1;
				if (FAILED(inner->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id)))
					id = -1;
				return id;
			};
			dispidRawPixels = resolve(L"RawDmdPixels");
			dispidRawWidth = resolve(L"RawDmdWidth");
			dispidRawHeight = resolve(L"RawDmdHeight");
			dispidColoredPixels = resolve(L"RawDmdColoredPixels");
			Log("raw DMD dispids: pixels=%ld w=%ld h=%ld colored=%ld",
				(long)dispidRawPixels, (long)dispidRawWidth, (long)dispidRawHeight,
				(long)dispidColoredPixels);
		}
		if (dispidRawPixels < 0 || dispidRawWidth < 0 || dispidRawHeight < 0)
			return;

		auto propGet = [this](DISPID id, VARIANT &out) -> bool
		{
			DISPPARAMS dp = { nullptr, nullptr, 0, 0 };
			return SUCCEEDED(inner->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
				DISPATCH_PROPERTYGET, &dp, &out, nullptr, nullptr));
		};

		// the DMD size is fixed for the session; read it once
		if (dmdW <= 0 || dmdH <= 0)
		{
			VARIANT vw, vh;
			VariantInit(&vw); VariantInit(&vh);
			INT32 w = 0, h = 0;
			if (propGet(dispidRawWidth, vw))  VarToI4(&vw, w);
			if (propGet(dispidRawHeight, vh)) VarToI4(&vh, h);
			VariantClear(&vw); VariantClear(&vh);
			if (w <= 0 || h <= 0 || w > PUP_DMD_MAX_W || h > PUP_DMD_MAX_H)
				return;
			dmdW = w;
			dmdH = h;
		}

		// Send buffer: {INT32 w, INT32 h, then w*h luminance bytes OR w*h*3
		// RGB bytes}.  Sized for the color (3x) case.
		static BYTE buf[8 + PUP_DMD_MAX_W * PUP_DMD_MAX_H * 3];
		BYTE *out = buf + 8;
		int n = dmdW * dmdH;
		int payloadBytes = 0;   // n (luminance) or n*3 (RGB) once a sampler fills the buffer

		// Exact-color packs want RGB frames: sample RawDmdColoredPixels.  Per
		// VPinMAME's own source (pinmame Controller.cpp get_RawDmdColoredPixels
		// + core.c), each pixel is packed r | (g<<8) | (b<<16) - R is the LOW
		// byte, no useful alpha.  On any failure we leave payloadBytes 0 and
		// fall through to luminance, so color can never break DMD triggers.
		if (wantColor && dispidColoredPixels >= 0)
		{
			VARIANT vc;
			VariantInit(&vc);
			if (propGet(dispidColoredPixels, vc)
				&& UnpackDmdArray(vc, n, [out](int i, UINT32 c)
					{
						out[i*3 + 0] = (BYTE)(c & 0xff);          // R (low byte)
						out[i*3 + 1] = (BYTE)((c >> 8) & 0xff);   // G
						out[i*3 + 2] = (BYTE)((c >> 16) & 0xff);  // B
					}))
				payloadBytes = n * 3;
			VariantClear(&vc);
		}

		// Luminance path: the default, and the fallback when color is
		// unavailable.  Same unpack, keeping only the low byte per pixel.
		if (payloadBytes == 0)
		{
			VARIANT vp;
			VariantInit(&vp);
			bool ok = propGet(dispidRawPixels, vp)
				&& UnpackDmdArray(vp, n, [out](int i, UINT32 c) { out[i] = (BYTE)(c & 0xff); });
			VariantClear(&vp);
			if (!ok)
				return;
			payloadBytes = n;
		}

		// skip unchanged frames (FNV-1a over the payload)
		UINT32 hash = 2166136261u;
		for (int i = 0; i < payloadBytes; ++i)
			hash = (hash ^ out[i]) * 16777619u;
		if (hash == lastFrameHash)
			return;
		lastFrameHash = hash;

		reinterpret_cast<INT32*>(buf)[0] = dmdW;
		reinterpret_cast<INT32*>(buf)[1] = dmdH;
		SendToBus(PUP_FRAME_MAGIC, buf, 8 + (DWORD)payloadBytes);
	}
};

// ---------------------------------------------------------------------
// class factory

class TeeFactory : public IClassFactory
{
public:
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
	{
		if (riid == IID_IUnknown || riid == IID_IClassFactory)
		{
			*ppv = static_cast<IClassFactory*>(this);
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() { return 2; }
	STDMETHODIMP_(ULONG) Release() { return 1; }

	STDMETHODIMP CreateInstance(IUnknown *pUnkOuter, REFIID riid, void **ppv)
	{
		*ppv = nullptr;
		if (pUnkOuter != nullptr)
			return CLASS_E_NOAGGREGATION;

		ControllerTee *tee = new (std::nothrow) ControllerTee();
		if (tee == nullptr)
			return E_OUTOFMEMORY;

		HRESULT hr = tee->CreateInner();
		if (FAILED(hr))
		{
			// no real VPinMAME: get out of the way entirely
			tee->Release();
			return hr;
		}

		hr = tee->QueryInterface(riid, ppv);
		tee->Release();
		return hr;
	}
	STDMETHODIMP LockServer(BOOL fLock)
	{
		if (fLock) InterlockedIncrement(&lockCount);
		else InterlockedDecrement(&lockCount);
		return S_OK;
	}
};

static TeeFactory factory;

// ---------------------------------------------------------------------
// DLL exports

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
	if (rclsid == CLSID_PupPinMameProxy)
		return factory.QueryInterface(riid, ppv);
	*ppv = nullptr;
	return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow()
{
	return lockCount == 0 ? S_OK : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		hModule = hinst;
		DisableThreadLibraryCalls(hinst);
		Log("proxy loaded");
	}
	return TRUE;
}
