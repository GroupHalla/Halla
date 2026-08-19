#pragma once

#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QComboBox>
#include <QVector>

class ScreenShareDialog : public QDialog {
    Q_OBJECT
public:
    explicit ScreenShareDialog(int maxWidth, int maxHeight, int maxFps,
                               int maxBitrateKbps, QWidget* parent = nullptr);
    int selectedSourceType() const { return m_selectedSourceType; } // 0 = screen, 1 = window
    quintptr selectedSourceId() const { return m_selectedSourceId; } // WId / HWND
    int selectedQualityProfile() const;
    int selectedWidth() const;
    int selectedHeight() const;
    int selectedFps() const;
    int selectedBitrateKbps() const;
    bool captureSystemAudio() const;

private:
    struct QualityProfile {
        int width = 1280;
        int height = 720;
        int fps = 30;
        int bitrateKbps = 2500;
    };

    void populateWindows();
    void populateScreens();
    void populateQualityProfiles(int maxWidth, int maxHeight, int maxFps,
                                 int maxBitrateKbps);
    const QualityProfile& selectedProfile() const;

    QTabWidget* m_tabs;
    QListWidget* m_screenList;
    QListWidget* m_windowList;
    QComboBox* m_qualityCombo = nullptr;
    QComboBox* m_audioCombo = nullptr;
    QVector<QualityProfile> m_qualityProfiles;
    
    int m_selectedSourceType = 0;
    quintptr m_selectedSourceId = 0;
};
