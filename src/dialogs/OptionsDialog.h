#pragma once

#include <QDialog>
#include <QListWidget>
#include <QStackedWidget>
#include <QStringList>

class QLabel;
struct ServerData;

// "Options" window -- replica of Halla's (classic) options dialog: category
// sidebar with large icons on the left (separated by a 1px line), a light
// gradient header inside the content panel (bold title + subtitle + section icon
// on the right), Windows-style fieldsets and the OK/Cancel/Apply buttons in the
// bottom-right corner.
class OptionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit OptionsDialog(QWidget* parent = nullptr, const ServerData* whisperData = nullptr);
    void selectPage(const QString& pageName);

signals:
    void themeChanged();
    void designChanged();
    void hotkeysChanged();
    void languageChanged();
    void whisperListsChanged();

private:
    QWidget* pageApplication();
    QWidget* pageDesign();
    QWidget* pageNotifications();
    QWidget* pagePlayback();
    QWidget* pageCapture();
    QWidget* pageHotkeys();
    QWidget* pageWhisper();
    QWidget* pageSecurity();
    QWidget* pageAddons();

    void apply();

    QListWidget* m_nav = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_headerTitle = nullptr;    // section title (bold)
    QLabel* m_headerSubtitle = nullptr; // section subtitle (smaller)
    QLabel* m_headerIcon = nullptr;     // section icon in the right corner
    QStringList m_pageSubtitles;        // subtitle of each page
    const ServerData* m_whisperData = nullptr;
};
