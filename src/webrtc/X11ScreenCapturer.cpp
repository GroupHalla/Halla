#include "X11ScreenCapturer.h"

#if defined(HALLA_WEBRTC_NATIVE) && defined(Q_OS_LINUX)

#include "core/AppLog.h"

#include <QByteArray>
#include <QPainter>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xfixes.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace {

// Synchronous X11 errors (BadWindow for a window that died between
// enumeration and grab, BadMatch for XShmGetImage with an invalid rectangle)
// must not kill the process: the default handler terminates the app. We
// swallow the error DURING the risky call and restore the handler right
// after (same pattern as OBS). The handler is global, but errors are
// dispatched synchronously by the calling thread -- the execution window is
// microseconds.
int swallowX11Errors(Display*, XErrorEvent*)
{
    return 0;
}

class XErrorGuard {
public:
    explicit XErrorGuard(Display* display)
        : m_display(display), m_previous(XSetErrorHandler(swallowX11Errors)) {}
    ~XErrorGuard()
    {
        XFlush(m_display);
        XSetErrorHandler(m_previous);
    }
private:
    Display* m_display = nullptr;
    int (*m_previous)(Display*, XErrorEvent*) = nullptr;
};

} // namespace

struct X11ScreenCapturer::Impl {
    Display* display = nullptr;
    Window root = None;
    bool shmUsable = false;
    bool fixesUsable = false;

    // XShm segment reused across frames (single attach).
    XImage* image = nullptr;
    XShmSegmentInfo segment{};
    int shmWidth = 0;
    int shmHeight = 0;

    // XRandR monitor cache (physical geometry) -- the XRRGetMonitors
    // round-trip every frame would cost more than the capture itself.
    struct MonitorInfo {
        QByteArray name;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        bool primary = false;
    };
    std::vector<MonitorInfo> monitors;
    std::chrono::steady_clock::time_point monitorsStamp;

    ~Impl()
    {
        teardownShm();
        if (display) {
            XCloseDisplay(display);
            display = nullptr;
        }
    }

    bool initialize()
    {
        if (display) return true;
        // XInitThreads ONCE per process, BEFORE the first XOpenDisplay from
        // here: the capturer is created by the capture thread and destroyed
        // by the GUI (session destructor) -- without Xlib internal locks that
        // is UB. On X11 Qt already called it (no-op); on Wayland via XWayland
        // we are the first to touch Xlib.
        static const bool threadsInitialized = [] {
            XInitThreads();
            return true;
        }();
        (void)threadsInitialized;
        display = XOpenDisplay(nullptr);
        if (!display) {
            AppLog::warn(QStringLiteral(
                "X11: não foi possível abrir o display — captura de tela indisponível "
                "(Wayland nativo sem XWayland não é suportado nesta versão)"));
            return false;
        }
        root = DefaultRootWindow(display);
        // MIT-SHM: the real signature is XShmQueryExtension(Display*) -- the
        // event/error base is not needed for synchronous XShmGetImage.
        shmUsable = XShmQueryExtension(display) != False;
        if (!shmUsable)
            AppLog::warn(QStringLiteral("X11: extensão MIT-SHM ausente; captura indisponível"));
        int fixesEventBase = 0, fixesErrorBase = 0;
        fixesUsable = XFixesQueryExtension(display, &fixesEventBase, &fixesErrorBase) != False;
        return shmUsable;
    }

    void teardownShm()
    {
        if (!display) {
            image = nullptr;
            shmWidth = shmHeight = 0;
            return;
        }
        if (image) {
            XShmDetach(display, &segment);
            // image->data points into the shared-memory segment: null it out
            // so XDestroyImage does not free memory it does not own (the
            // segment itself is released by shmdt below).
            image->data = nullptr;
            XDestroyImage(image);
            image = nullptr;
        }
        if (segment.shmid != -1) {
            shmdt(segment.shmaddr);
            segment = {};
        }
        shmWidth = shmHeight = 0;
    }

    // Ensures an XImage + shm segment of w x h bytes (reused across frames).
    bool ensureShm(int width, int height)
    {
        if (image && width == shmWidth && height == shmHeight) return true;
        teardownShm();
        std::memset(&segment, 0, sizeof(segment));
        segment.shmid = -1;
        image = XShmCreateImage(display,
                                DefaultVisual(display, DefaultScreen(display)),
                                DefaultDepth(display, DefaultScreen(display)),
                                ZPixmap, nullptr, &segment, unsigned(width), unsigned(height));
        if (!image) return false;
        // 0600: the frame buffer is screen content -- never world-readable.
        segment.shmid = shmget(IPC_PRIVATE, size_t(image->bytes_per_line) * size_t(height),
                               IPC_CREAT | 0600);
        if (segment.shmid == -1) {
            image->data = nullptr;
            XDestroyImage(image);
            image = nullptr;
            return false;
        }
        segment.shmaddr = static_cast<char*>(shmat(segment.shmid, nullptr, 0));
        segment.readOnly = False;
        if (segment.shmaddr == reinterpret_cast<char*>(-1)) {
            shmctl(segment.shmid, IPC_RMID, nullptr);
            segment.shmid = -1;
            image->data = nullptr;
            XDestroyImage(image);
            image = nullptr;
            return false;
        }
        image->data = segment.shmaddr;
        // IPC_RMID right away: the segment dies when the last detach happens,
        // even if the process crashes midway (no IPC leak on crash).
        shmctl(segment.shmid, IPC_RMID, nullptr);
        if (!XShmAttach(display, &segment)) {
            shmdt(segment.shmaddr);
            shmctl(segment.shmid, IPC_RMID, nullptr);
            segment.shmid = -1;
            image->data = nullptr;
            XDestroyImage(image);
            image = nullptr;
            return false;
        }
        XSync(display, False);
        shmWidth = width;
        shmHeight = height;
        return true;
    }

    void refreshMonitors()
    {
        const auto now = std::chrono::steady_clock::now();
        if (!monitors.empty() && now - monitorsStamp < std::chrono::seconds(2)) return;
        monitors.clear();
        monitorsStamp = now;
        int count = 0;
        XRRMonitorInfo* infos = XRRGetMonitors(display, root, True /*active only*/, &count);
        if (!infos || count <= 0) {
            if (infos) XRRFreeMonitors(infos);
            // No XRandR (e.g. hand-configured screens): a root-sized
            // "monitor" keeps capture working.
            monitors.push_back({"", 0, 0,
                                DisplayWidth(display, DefaultScreen(display)),
                                DisplayHeight(display, DefaultScreen(display)),
                                true});
            return;
        }
        // Resources fetched ONCE per refresh: XRRGetOutputInfo requires
        // (Display*, XRRScreenResources*, RROutput) -- the OUTPUT name (which
        // is Qt QScreen::name()), not the monitor name.
        XRRScreenResources* resources = XRRGetScreenResources(display, root);
        for (int i = 0; i < count; ++i) {
            MonitorInfo info;
            info.x = infos[i].x;
            info.y = infos[i].y;
            info.width = std::max(1, int(infos[i].width));
            info.height = std::max(1, int(infos[i].height));
            info.primary = infos[i].primary != 0;
            // Name of the monitor's first output (the name Qt QScreen::name()
            // exposes on the GUI thread).
            if (resources && infos[i].noutput > 0) {
                XRROutputInfo* output = XRRGetOutputInfo(display, resources,
                                                         infos[i].outputs[0]);
                if (output && output->name) info.name = QByteArray(output->name);
                if (output) XRRFreeOutputInfo(output);
            }
            monitors.push_back(std::move(info));
        }
        if (resources) XRRFreeScreenResources(resources);
        XRRFreeMonitors(infos);
    }

    const MonitorInfo* monitorForName(const QString& name)
    {
        refreshMonitors();
        const QByteArray wanted = name.toUtf8();
        if (!wanted.isEmpty()) {
            for (const auto& monitor : monitors)
                if (monitor.name == wanted) return &monitor;
        }
        for (const auto& monitor : monitors)
            if (monitor.primary) return &monitor;
        return monitors.empty() ? nullptr : &monitors.front();
    }

    // Captures a root region in physical pixels; returns an ARGB32 QImage.
    QImage grabRootRect(int x, int y, int width, int height)
    {
        if (width < 8 || height < 8) return {};
        // Clips to the root (XShmGetImage with an out-of-bounds rectangle
        // returns BadMatch).
        const int rootWidth = DisplayWidth(display, DefaultScreen(display));
        const int rootHeight = DisplayHeight(display, DefaultScreen(display));
        if (x < 0) { width += x; x = 0; }
        if (y < 0) { height += y; y = 0; }
        width = std::min(width, rootWidth - x);
        height = std::min(height, rootHeight - y);
        if (width < 8 || height < 8) return {};
        if (!ensureShm(width, height)) return {};

        {
            XErrorGuard guard(display);
            // Real signature: XShmGetImage(dpy, d, ximage, x, y, plane_mask)
            // -- the format (ZPixmap) comes from the XImage created in
            // ensureShm.
            if (!XShmGetImage(display, root, image, x, y, AllPlanes)) return {};
        }
        if (image->depth < 24 || image->bits_per_pixel != 32) {
            AppLog::warn(QStringLiteral("X11: profundidade %1/%2 bpp não suportada para captura")
                             .arg(image->depth).arg(image->bits_per_pixel));
            return {};
        }
        // 32bpp little-endian ZPixmap == QImage ARGB32 layout (root alpha
        // usually comes as 0xff or garbage -- the consumer ignores the channel).
        QImage frame(reinterpret_cast<uchar*>(image->data), width, height,
                     image->bytes_per_line, QImage::Format_ARGB32);
        return frame.copy(); // detach before reusing the shm buffer
    }

    // Composites the system cursor (root position) into the frame --
    // streaming without a cursor makes desktop streams practically
    // unwatcheable.
    void compositeCursor(QImage& frame, int originX, int originY)
    {
        if (!fixesUsable || frame.isNull()) return;
        XFixesCursorImage* cursor = nullptr;
        {
            XErrorGuard guard(display);
            cursor = XFixesGetCursorImage(display);
        }
        if (!cursor) return;
        const int cursorX = int(cursor->x) - int(cursor->xhot) - originX;
        const int cursorY = int(cursor->y) - int(cursor->yhot) - originY;
        const int cursorW = int(cursor->width);
        const int cursorH = int(cursor->height);
        // Outside the frame (cursor on another monitor): discard without painting.
        if (cursorX + cursorW > 0 && cursorY + cursorH > 0
                && cursorX < frame.width() && cursorY < frame.height()) {
            QImage cursorImage(cursorW, cursorH, QImage::Format_ARGB32);
            // XFixesCursorImage::pixels is ARGB in one unsigned long per pixel.
            for (int row = 0; row < cursorH; ++row) {
                QRgb* line = reinterpret_cast<QRgb*>(cursorImage.scanLine(row));
                const unsigned long* source = cursor->pixels + row * cursorW;
                for (int col = 0; col < cursorW; ++col) {
                    const unsigned long pixel = source[col];
                    const unsigned int argb = unsigned(pixel & 0xffffffffUL);
                    line[col] = qRgba((argb >> 16) & 0xff, argb >> 8 & 0xff, argb & 0xff,
                                      (argb >> 24) & 0xff);
                }
            }
            QPainter painter(&frame);
            painter.drawImage(cursorX, cursorY, cursorImage);
        }
        XFree(cursor);
    }
};

X11ScreenCapturer::X11ScreenCapturer()
    : m_impl(std::make_unique<Impl>()) {}

X11ScreenCapturer::~X11ScreenCapturer() = default;

bool X11ScreenCapturer::valid() const
{
    return m_impl && m_impl->display && m_impl->shmUsable;
}

QImage X11ScreenCapturer::grabMonitor(const QString& outputName)
{
    if (!m_impl->initialize()) return {};
    const Impl::MonitorInfo* monitor = m_impl->monitorForName(outputName);
    if (!monitor) return {};
    QImage frame = m_impl->grabRootRect(monitor->x, monitor->y,
                                        monitor->width, monitor->height);
    m_impl->compositeCursor(frame, monitor->x, monitor->y);
    return frame;
}

QImage X11ScreenCapturer::grabWindowRegion(quintptr windowId)
{
    if (!m_impl->initialize()) return {};
    Window window = static_cast<Window>(windowId);
    if (window == None) return {};

    int x = 0, y = 0;
    unsigned int width = 0, height = 0;
    Window rootReturn = None;
    int rootX = 0, rootY = 0;
    unsigned int borderWidth = 0, depthReturn = 0;
    {
        XErrorGuard guard(m_impl->display);
        Status geometryOk = XGetGeometry(m_impl->display, window, &rootReturn,
                                         &x, &y, &width, &height, &borderWidth, &depthReturn);
        if (!geometryOk) return {};
        // XGetGeometry x/y are relative to the window PARENT and width/height
        // are the CLIENT AREA (without the X11 border); translate the client
        // area origin into root coordinates.
        if (!XTranslateCoordinates(m_impl->display, window, m_impl->root,
                                   0, 0, &rootX, &rootY, &window)) return {};
    }
    QImage frame = m_impl->grabRootRect(rootX, rootY, int(width), int(height));
    m_impl->compositeCursor(frame, rootX, rootY);
    return frame;
}

#endif
