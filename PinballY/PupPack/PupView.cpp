// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY

#include "../stdafx.h"
#include "PupView.h"
#include "PupPackManager.h"
#include "../Application.h"
#include "../VideoSprite.h"
#include "../LogFile.h"

PupView::PupView(const TCHAR *configVarPrefix) :
	SecondaryView(IDR_CUSTOMVIEW_CONTEXT_MENU, configVarPrefix)
{
	mediaKeepAspect = ConfigManager::GetInstance()->GetBool(_T("PupPack.MediaKeepAspect"), false);
}

bool PupView::OnAppMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
	// When the overlay video's true frame size is detected, set the
	// sprite's aspect and rescale, so it fills the window correctly
	// instead of drawing at its placeholder 1:1 load size.  (The base
	// class does this only for its Javascript drawing layers.)
	if (msg == AVPMsgSetFormat && videoOverlay != nullptr
		&& videoOverlay->GetMediaCookie() == static_cast<DWORD>(wParam))
	{
		auto desc = reinterpret_cast<const AudioVideoPlayer::FormatDesc*>(lParam);
		if (desc != nullptr && desc->width != 0 && desc->height != 0)
		{
			videoOverlay->loadSize.y = 1.0f;
			videoOverlay->loadSize.x = static_cast<float>(desc->width) / static_cast<float>(desc->height);
			videoOverlay->ReCreateMesh();
			ScaleSprites();
		}
	}
	return __super::OnAppMessage(msg, wParam, lParam);
}

// Is this file one of the still-image types we load through the D3D
// image path rather than the video player?  Content identification
// comes first: GetImageFileInfo reads the file header, so a misnamed
// image still routes correctly (it recognizes PNG/JPEG/GIF/SWF, all of
// which Sprite::Load handles).  The extension check remains as the
// fallback for image types the sniffer doesn't know - BMP in
// particular - and for anything it can't read.
static bool IsImageFile(const TCHAR *path)
{
	ImageFileDesc desc;
	if (GetImageFileInfo(path, desc))
		return true;

	static const TCHAR *exts[] = { _T(".png"), _T(".jpg"), _T(".jpeg"), _T(".bmp"), _T(".gif") };
	const TCHAR *ext = _tcsrchr(path, _T('.'));
	if (ext == nullptr)
		return false;
	for (auto e : exts)
		if (_tcsicmp(ext, e) == 0)
			return true;
	return false;
}

// Do the image's transparent regions warrant window color keying?
// (Alpha-capable formats only; jpg/bmp have no alpha channel.)
static bool IsAlphaImageFile(const TCHAR *path)
{
	const TCHAR *ext = _tcsrchr(path, _T('.'));
	return ext != nullptr
		&& (_tcsicmp(ext, _T(".png")) == 0 || _tcsicmp(ext, _T(".gif")) == 0);
}

bool PupView::PlayMedia(const TCHAR *path, bool loop, int volPct, int lengthSecs)
{
	// cancel any pending length-limit timer from the previous media
	KillTimer(hWnd, lengthTimerID);

	// Same-file video replay: restart the live player rather than tearing
	// it down and building a new one.  Real tables re-fire the same clip
	// in rapid succession (JP replays one video about once a second
	// through entire modes), and every teardown/rebuild cycles a whole
	// libVLC audio session; churn at that rate eventually loses a race
	// inside VLC's mmdevice audio plugin (an audio-session event callback
	// lands in a freed session -> crash in libmmdevice_plugin.dll,
	// observed in live play 2026-07-01).  Reusing the player sidesteps
	// the churn entirely, and is also what real PuP does.
	if (videoOverlay != nullptr && curPath == path && !IsImageFile(path))
	{
		if (auto *vp = videoOverlay->GetVideoPlayer(); vp != nullptr)
		{
			vp->SetLooping(loop);
			vp->SetVolume(volPct);

			// Restart the clip ONLY if it has finished (presented a frame,
			// then stopped).  A re-fire while it is still opening
			// (!IsFrameReady) OR still playing (IsPlaying) must NOT call
			// Replay(): Replay's stop is a SYNCHRONOUS libvlc stop that
			// joins the playback thread on the UI thread, and real tables
			// re-assert a looping clip constantly (JP fires the same
			// spincar.mp4 many times a second).  Calling Replay() on each
			// re-fire piles up UI-thread joins until one deadlocks - a
			// hard main-thread vlc_join hang, observed live 2026-07-02.
			// The right response to "play the clip that's already playing"
			// is to keep it playing (and just re-apply loop/volume), which
			// is what real PuP does.
			bool ok = true;
			if (vp->IsFrameReady() && !vp->IsPlaying())
			{
				Application::AsyncErrorHandler reh;
				ok = vp->Replay(reh);
			}
			if (ok)
			{
				curVol = volPct;
				if (lengthSecs > 0)
					SetTimer(hWnd, lengthTimerID, lengthSecs * 1000, 0);
				return true;
			}
		}
	}

	// Warn when the media file doesn't exist.  This is the usual cause of a
	// silently blank screen - an incompletely installed pack, or a table
	// naming a folder/file that isn't present (e.g. an empty pack folder, or
	// a .pup playlist/file that was never downloaded).  libVLC accepts a bad
	// path without raising an error and simply renders nothing, so without
	// this the failure leaves no trace in the log.  Checked here on the load
	// path (not the same-file replay early-out above), so it costs nothing on
	// the rapid re-fire hot path.
	if (path != nullptr && path[0] != 0
		&& GetFileAttributes(path) == INVALID_FILE_ATTRIBUTES)
	{
		LogFile::Get()->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: screen %d: media file NOT FOUND: %s")
			_T(" - screen will be blank; check the pack is fully installed\n"),
			screenNum, path);
	}

	// load the media into a new sprite
	Application::AsyncErrorHandler eh;
	RefPtr<VideoSprite> sprite(new VideoSprite());
	sprite->alpha = 1.0f;
	bool isImage = IsImageFile(path);
	if (isImage)
	{
		// still image (e.g., an overlay PNG)
		if (!sprite->Load(path, { 1.0f, 1.0f }, szLayout, hWnd, eh))
			return false;
	}
	else
	{
		// video (or anything else the video player can decode)
		if (!sprite->LoadVideo(path, hWnd, { 1.0f, 1.0f }, eh, _T("PUP pack media"), true, volPct))
			return false;

		// LoadVideo turns looping on by default; honor the caller's setting
		if (!loop)
			sprite->SetLooping(false);
	}

	// Overlay art carries transparency in its alpha channel, and real PuP
	// composites it over the screens beneath; approximate with a black
	// color key while an alpha-capable image is showing, and remove the
	// key for anything else so video black levels render normally.
	SetColorKeyTransparency(isImage && IsAlphaImageFile(path));

	// show it in the overlay video slot
	videoOverlay = sprite;
	videoOverlayID = _T("puppack");
	UpdateDrawingList();

	// remember the current media, for SetBackground promotion
	curPath = path;
	curVol = volPct;

	// start the length-limit timer, if the media has a maximum play time
	if (lengthSecs > 0)
		SetTimer(hWnd, lengthTimerID, lengthSecs * 1000, 0);

	return true;
}

void PupView::SetColorKeyTransparency(bool on)
{
	// The key goes on our frame window.  This works because the swap
	// chain is blt-model (DXGI_SWAP_EFFECT_DISCARD, see D3DWin.cpp);
	// flip-model presentation does not support layered color keying.
	HWND frame = GetParent(hWnd);
	if (frame == nullptr)
		return;
	LONG ex = GetWindowLong(frame, GWL_EXSTYLE);
	if (on)
	{
		SetWindowLong(frame, GWL_EXSTYLE, ex | WS_EX_LAYERED);
		SetLayeredWindowAttributes(frame, RGB(0, 0, 0), 0, LWA_COLORKEY);
	}
	else if ((ex & WS_EX_LAYERED) != 0)
		SetWindowLong(frame, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
}

void PupView::ClearMedia()
{
	// discard our overlay video and any pending length limit
	KillTimer(hWnd, lengthTimerID);
	videoOverlay = nullptr;
	videoOverlayID.clear();
	curPath.clear();
	SetColorKeyTransparency(false);
	UpdateDrawingList();

	// do the base class work
	__super::ClearMedia();
}

void PupView::SetMediaLooping(bool loop)
{
	// Route to the current media's video player, null-guarding every
	// step: the screen may be idle (no overlay), or showing a still
	// image (no video player).
	if (videoOverlay != nullptr)
	{
		if (auto *player = videoOverlay->GetVideoPlayer(); player != nullptr)
			player->SetLooping(loop);
		else
			videoOverlay->SetLooping(loop);   // image sprite (e.g., GIF animation)
	}
}

void PupView::SetCurrentVolume(int volPct)
{
	// only the live video player has an adjustable volume; a still image or
	// an idle screen has nothing to duck
	if (videoOverlay != nullptr)
		if (auto *player = videoOverlay->GetVideoPlayer(); player != nullptr)
			player->SetVolume(volPct);
}

void PupView::PauseMedia()
{
	// Stop playback in place, keeping the sprite (and its last presented
	// frame) on screen, per the drawing-layer pause convention.  A pending
	// length-limit cutoff keeps running on the wall clock; the Length
	// column bounds a trigger's screen time, paused or not.
	if (videoOverlay != nullptr)
	{
		SilentErrorHandler eh;
		videoOverlay->Stop(eh);
	}
}

void PupView::ResumeMedia()
{
	if (videoOverlay != nullptr)
	{
		SilentErrorHandler eh;
		videoOverlay->Play(eh);
	}
}

bool PupView::SetCurrentAsBackground()
{
	// nothing to promote if no media has played (or it was cleared)
	if (curPath.empty())
		return false;

	// the current media becomes the background we return to
	bgPath = curPath;
	bgVol = curVol;
	return true;
}

void PupView::StopAll()
{
	// A script stop leaves the screen blank: clear the background
	// association so nothing resurrects it, then clear the media.
	bgPath.clear();
	ClearMedia();
}

void PupView::OnMediaFinished()
{
	// Release the engine's arbitration slot for this screen, so future
	// triggers of any priority can claim it.
	if (auto ppm = PupPackManager::Get(); ppm != nullptr)
		ppm->OnScreenMediaEnded(screenNum);

	// return to the background media, if the screen has one
	if (!bgPath.empty())
	{
		LogFile::Get()->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: trigger media ended, restarting background %s\n"), bgPath.c_str());
		PlayMedia(bgPath.c_str(), true, bgVol);
	}
}

void PupView::OnEndOverlayVideo()
{
	// do the base class work (startup video handling)
	__super::OnEndOverlayVideo();

	// a video reached its natural end
	KillTimer(hWnd, lengthTimerID);
	OnMediaFinished();
}

void PupView::EndOverlayMedia()
{
	// the length limit was reached - discard whatever is showing
	// (video or still image) and run the shared end-of-media path
	videoOverlay = nullptr;
	videoOverlayID.clear();
	curPath.clear();
	UpdateDrawingList();
	OnMediaFinished();
}

bool PupView::OnTimer(WPARAM timer, LPARAM callback)
{
	if (timer == lengthTimerID)
	{
		KillTimer(hWnd, lengthTimerID);
		EndOverlayMedia();
		return true;
	}
	if (timer == labelTimerID)
	{
		// rebuild the label overlay if it has pending changes
		if (labelsDirty)
			RenderLabels();
		return true;
	}
	return __super::OnTimer(timer, callback);
}

// ---------------------------------------------------------------------
// Text label overlay
//

void PupView::UpdateDrawingList()
{
	// do the base class work
	__super::UpdateDrawingList();

	// the label overlay draws above the media layer
	AddToDrawingList(labelSprite);
}

void PupView::ScaleSprites()
{
	// do the base class work
	__super::ScaleSprites();

	// Size the PUP media to the screen window.  The base class doesn't
	// scale videoOverlay, so without this a video keeps its placeholder
	// 1:1 load size (a squished, un-filled square).  Default stretches to
	// fill (real PuP's fitToWindow); PupPack.MediaKeepAspect letterboxes.
	ScaleSprite(videoOverlay, 1.0f, mediaKeepAspect);

	// the label overlay always exactly fills the window
	ScaleSprite(labelSprite, 1.0f, true);
}

void PupView::MarkLabelsDirty()
{
	labelsDirty = true;

	// Make sure the rebuild timer is running.  It starts on the first
	// label call and then just keeps ticking for the life of the window
	// (which only lasts as long as the current game); a clean tick with
	// no pending changes is a trivial flag test.
	if (!labelTimerRunning && hWnd != nullptr)
	{
		SetTimer(hWnd, labelTimerID, 100, 0);
		labelTimerRunning = true;
	}
}

void PupView::SetLabelStyle(const TCHAR *name, const TCHAR *font, float sizePct,
	COLORREF color, int xAlign, int yAlign, float xPct, float yPct,
	int page, bool visible)
{
	if (name == nullptr || name[0] == 0)
		return;

	// Create or restyle the label, keeping any existing text.  Missing
	// COM arguments arrive as empty/zero defaults; keep the current
	// (or default) font and size in those cases.
	Label &l = labels[name];
	if (font != nullptr && font[0] != 0)
		l.fontName = font;
	if (sizePct > 0.0f)
		l.sizePct = sizePct;
	l.color = color;
	l.xAlign = xAlign;
	l.yAlign = yAlign;
	l.xPct = xPct;
	l.yPct = yPct;
	l.page = page;
	l.visible = visible;
	MarkLabelsDirty();
}

void PupView::SetLabelText(const TCHAR *name, const TCHAR *text, bool visible, const TCHAR *special)
{
	if (name == nullptr || name[0] == 0)
		return;

	// find or create the label and set its text
	Label &l = labels[name];
	l.text = text != nullptr ? text : _T("");
	l.visible = visible;

	// The "special" argument carries JSON for animated label effects,
	// which we don't implement; note the first sighting per label so
	// the log shows what the table wanted without flooding.
	if (special != nullptr && special[0] != 0 && !l.specialLogged)
	{
		l.specialLogged = true;
		LogFile::Get()->Write(LogFile::TableLaunchLogging,
			_T("PUP pack: screen %d: label %s 'special' JSON ignored (not implemented)\n"),
			screenNum, name);
	}

	MarkLabelsDirty();
}

void PupView::SetLabelPage(int page)
{
	if (labelPage != page)
	{
		labelPage = page;
		MarkLabelsDirty();
	}
}

void PupView::RenderLabels()
{
	labelsDirty = false;

	// wait for a usable layout (the window could be zero-sized mid-create)
	int width = szLayout.cx, height = szLayout.cy;
	if (width <= 0 || height <= 0)
	{
		labelsDirty = true;
		return;
	}

	// collect the labels to draw: visible, non-empty, and on the current
	// page (page 0 labels show on every page)
	std::vector<const Label*> vis;
	for (auto &pair : labels)
	{
		const Label &l = pair.second;
		if (l.visible && !l.text.empty() && (l.page == labelPage || l.page == 0))
			vis.push_back(&l);
	}

	// with nothing to draw, drop the sprite entirely
	if (vis.empty())
	{
		if (labelSprite != nullptr)
		{
			labelSprite = nullptr;
			UpdateDrawingList();
		}
		return;
	}

	// render all of the visible labels into one full-window sprite
	Application::AsyncErrorHandler eh;
	RefPtr<Sprite> sprite(new Sprite());
	if (!sprite->Load(width, height, [&vis, width, height](Gdiplus::Graphics &g)
	{
		// Use grayscale antialiasing: the system-default hint (usually
		// ClearType) writes no alpha, which would leave the text invisible
		// against the sprite's transparent background.
		g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

		for (auto l : vis)
		{
			// the font size is a percentage of the window height
			int pixHt = max((int)(l->sizePct * height / 100.0f), 1);
			std::unique_ptr<Gdiplus::Font> font(
				CreateGPFontPixHt(l->fontName.c_str(), pixHt, Gdiplus::FontStyleRegular));

			// The position is a percentage of the window size; the
			// alignment selects which point of the text lands there
			// (0 near, 1 center, 2 far).
			Gdiplus::PointF pt(l->xPct * width / 100.0f, l->yPct * height / 100.0f);
			auto Align = [](int a) {
				return a == 1 ? Gdiplus::StringAlignmentCenter :
					a == 2 ? Gdiplus::StringAlignmentFar : Gdiplus::StringAlignmentNear;
			};
			Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
			fmt.SetAlignment(Align(l->xAlign));
			fmt.SetLineAlignment(Align(l->yAlign));

			// the script's color int is a COLORREF-style BGR value
			Gdiplus::SolidBrush br(GPColorFromCOLORREF(l->color));
			g.DrawString(l->text.c_str(), -1, font.get(), pt, &fmt, &br);
		}
	}, eh, _T("PUP pack label overlay")))
		return;

	// show it above the media layer
	labelSprite = sprite;
	UpdateDrawingList();

	LogFile::Get()->Write(LogFile::TableLaunchLogging,
		_T("PUP pack: screen %d: rendered %d label(s) (page %d)\n"),
		screenNum, (int)vis.size(), labelPage);
}

void PupView::OnShowHideFrameWindow(bool show)
{
	// Audio-only windows are permanently hidden hosts: their media must
	// keep sounding regardless of visibility.  For visible PUP windows,
	// a user-initiated hide stops the media like any other window.
	if (!show && !audioOnly)
		StopAll();
}
