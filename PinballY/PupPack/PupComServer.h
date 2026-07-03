// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY
//
// PupComServer - a minimal COM local server exposing the PinUpPlayer
// "PinDisplay" automation interface, so that unmodified VPX table scripts
// (Set PuPlayer = CreateObject("PinUpPlayer.PinDisplay") : PuPlayer.pupevent N)
// feed live events into the PUP trigger engine while a game is running.
//
// The server registers a class object in the running PinballY process and
// writes ProgID/CLSID entries under HKCU\Software\Classes, which shadow any
// machine-wide PinUP Popper registration for this user only, and only while
// active: the entries are written when a pack-equipped game starts and
// removed when it ends.  The whole feature is gated by the config var
// PupPack.PinDisplayCOM (default off), since on a machine with a real PinUP
// install the shadow redirects tables' PuP connection to PinballY.
//
// Threading: PinballY's main thread runs an STA (OleInitialize), and the
// class object is registered from it, so all IDispatch::Invoke calls arrive
// on the main thread via the message pump - safe to drive windows directly.

#pragma once

// PupEventBus - live pinmame hardware events (S/W/L/G/M triggers).
//
// Start() registers the VPinMAME.Controller tee proxy (an HKCU shadow of
// the VPinMAME ProgID pointing at PupPinMameProxy32/64.dll next to the
// PinballY executable) and creates the invisible receiver window the
// proxy sends observed events to via WM_COPYDATA.  The proxy forwards
// every controller call to the real VPinMAME, so the game is unaffected;
// we just see the state traffic the table already polls.  Same
// game-scoped lifecycle as the PinDisplay shim; the manager starts it
// only for packs that actually use hardware triggers.
class PupEventBus
{
public:
	static bool Start();
	static void Stop();
	static bool IsRunning();

	// Create/destroy the DMD frame-sink window.  Its existence tells the
	// proxy that DMD frames are wanted (the current pack loaded PuPCapture
	// patterns); without it the proxy skips all DMD sampling.
	static void EnableFrameSink(bool enable, bool wantColor = false);
};

class PupComServer
{
public:
	// Register the class object and the HKCU ProgID/CLSID entries.
	// Idempotent; returns true if the server is (now) available.
	static bool Start();

	// Revoke the class object and remove our registry entries (only if
	// they still point at our own CLSID).
	static void Stop();

	static bool IsRunning();
};
