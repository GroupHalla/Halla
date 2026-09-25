#pragma once

// Captura de tela/janela para o WebRTC nativo no Linux (X11).
//
// O equivalente Linux do DxgiScreenCapturer do Windows: XShmGetImage no root
// window (monitor inteiro ou região de uma janela), resolução FÍSICA de cada
// monitor via XRandR e composição do cursor via XFixes. Vive inteiramente na
// thread de captura do HallaWebRtcSession (a GUI não pode — QScreen::grabWindow
// só roda na thread da GUI); por isso mantém um Display* próprio, aberto e
// fechado no mesmo fio.
//
// Compila vazio (sem símbolos) fora de "Linux + WebRTC nativo", seguindo o
// padrão do MediaFoundationH264 — o CMake adiciona as fontes em todo build.

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

    // Captura um monitor inteiro. `outputName` é o nome do output XRandR
    // (QScreen::name() devolve exatamente esse nome no X11, ex.: "HDMI-0").
    // Vazio/não encontrado cai no monitor primário. Devolve a resolução
    // FÍSICA do monitor (ignora escala fractional do compositor).
    QImage grabMonitor(const QString& outputName);

    // Captura a região da tela ocupada por uma janela (coords root, pixels
    // físicos). Janelas cobertas por outras capturam o que está por cima
    // (limitação do X11 sem XComposite redirect — mesma técnica do
    // QScreen::grabWindow). Devolve QImage nula se a janela sumiu/inválida.
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
