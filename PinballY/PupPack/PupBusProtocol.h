// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY
//
// PupBusProtocol - the wire protocol between the VPinMAME controller tee
// (proxy/PupPinMameProxy.cpp, a standalone DLL living in the game process)
// and PinballY's PUP event bus (PupComServer.cpp).  Everything that must
// agree across the process boundary lives here and only here.  This header
// is dependency-free beyond <windows.h> so the standalone proxy build can
// include it.

#pragma once
#include <windows.h>

// Receiver window classes.  The event bus window always exists while the
// tee is registered; the frame sink window additionally exists only when
// the current pack loaded PuPCapture DMD patterns, so the proxy can skip
// the whole DMD sampling path when nobody wants frames.
#define PUP_BUS_WINDOW_CLASS    L"PinballY.PupEventBus"
#define PUP_FRAME_SINK_CLASS    L"PinballY.PupFrameSink"

// The frame sink window's title carries the wanted sampling mode: an empty
// title means luminance (raw levels, one byte per pixel); this marker means
// the current pack wants exact-color frames, so the proxy samples the
// controller's colored pixels (RGB) instead.  The receiver distinguishes a
// color frame from a luminance frame by payload size (w*h*3 vs w*h), so no
// extra header field is needed.
#define PUP_FRAME_SINK_COLOR_TITLE  L"COLOR"

// WM_COPYDATA payload tags
static const DWORD PUP_EVENT_MAGIC = 0x50555045;   // 'PUPE': BusEvent[]
static const DWORD PUP_FRAME_MAGIC = 0x50555046;   // 'PUPF': {w, h, levels[w*h] or RGB[w*h*3]}

// one hardware event: type is a trigger device letter ('S','W','L','G','M')
struct BusEvent { INT32 type, num, state; };

// DMD frame size cap (frames are {INT32 w, INT32 h, BYTE levels[w*h]})
static const int PUP_DMD_MAX_W = 256;
static const int PUP_DMD_MAX_H = 64;

// The tee proxy's CLSID, in both encodings (struct for the proxy's class
// factory, string for PinballY's registry shadow).  These MUST stay in
// agreement; keeping them adjacent is the point of this header.
// {7C4B1A92-6E0D-4F73-8B25-D91A34C6E5F1}
static const CLSID CLSID_PupPinMameProxy =
	{ 0x7c4b1a92, 0x6e0d, 0x4f73, { 0x8b, 0x25, 0xd9, 0x1a, 0x34, 0xc6, 0xe5, 0xf1 } };
#define PUP_TEE_CLSID_STR L"{7C4B1A92-6E0D-4F73-8B25-D91A34C6E5F1}"

// proxy DLL file names, next to PinballY.exe
#define PUP_TEE_DLL_32 L"PupPinMameProxy32.dll"
#define PUP_TEE_DLL_64 L"PupPinMameProxy64.dll"
