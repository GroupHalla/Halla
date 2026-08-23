#pragma once

#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QVector>

class ScreenShareDialog : public QDialog {
    Q_OBJECT
public:
    explicit ScreenShareDialog(int maxWidth, int maxHeight, int maxFps,
                               int maxBitrateKbps, QWidget* parent = nullptr);
    int selectedSourceType() const { return m_selectedSourceType; } // 0 = screen, 1 = window
    quintptr selectedSourceId() const { return m_selectedSourceId; } // WId / HWND
    int selectedWidth() const;
    int selectedHeight() const;
    int selectedFps() const;
    int selectedBitrateKbps() const;
    bool captureSystemAudio() const;

private:
    struct ResolutionOption {
        int width = 854;
        int height = 480;
        QString label;
    };

    void populateWindows();
    void populateScreens();
    void populateResolutionOptions(int maxWidth, int maxHeight);
    void populateFpsOptions(int maxFps);
    void updateRecommendedBitrate();
    int recommendedBitrateKbps(int width, int height, int fps) const;

    QTabWidget* m_tabs;
    QListWidget* m_screenList;
    QListWidget* m_windowList;
    QComboBox* m_qualityCombo = nullptr;
    QComboBox* m_fpsCombo = nullptr;
    QSpinBox* m_bitrateSpin = nullptr;
    QComboBox* m_audioCombo = nullptr;
    QVector<ResolutionOption> m_resolutionOptions;
    int m_maxBitrateKbps = 8000;
    
    int m_selectedSourceType = 0;
    quintptr m_selectedSourceId = 0;
};
