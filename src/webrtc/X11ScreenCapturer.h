#pragma once

// Screen/window capture for native WebRTC on Linux (X11).
//
// The Linux equivalent of the Windows DxgiScreenCapturer: XShmGetImage on the
// root window (whole monitor or a window region), PHYSICAL resolution of each
// monitor via XRandR, and cursor composition via XFixes. Lives entirely on
// the HallaWebRtcSession capture thread (the GUI cannot -- QScreen::grabWindow
// only runs on the GUI thread); therefore it keeps its own Display*, opened
// and closed on the same thread.
//
// Compiles empty (no symbols) outside "Linux + native WebRTC", following the
// MediaFoundationH264 pattern -- CMake adds the sources to every build.

#include <QImage>
#include <QString>
#include <memory>

#if defined(HALLA_WEBRTC_NATIVE) && defined(Q_OS_LINUX)
class X11ScreenCapturer {
public:
    X11ScreenCapturer();
    ~X11ScreenCapturer();

    X11ScreenCapturer(const X11ScreenCapturer&) = delete;
    X11ScreenCapturer& operator=(const X11ScreenCapturer&) = delete;

    bool valid() const;

    // Captures a whole monitor. `outputName` is the XRandR output name
    // (QScreen::name() returns exactly this name on X11, e.g. "HDMI-0").
    // Empty/not found falls back to the primary monitor. Returns the PHYSICAL
    // monitor resolution (ignores compositor fractional scaling).
    QImage grabMonitor(const QString& outputName);

    // Captures the screen region occupied by a window (root coordinates,
    // physical pixels). Windows covered by others capture what is on top
    // (X11 limitation without XComposite redirect -- same technique as
    // QScreen::grabWindow). Returns a null QImage if the window vanished or
    // is invalid.
    QImage grabWindowRegion(quintptr windowId);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
#else
class X11ScreenCapturer {
public:
    X11ScreenCapturer() = default;
    bool valid() const { return false; }
    QImage grabMonitor(const QString&) { return {}; }
    QImage grabWindowRegion(quintptr) { return {}; }
};
#endif
