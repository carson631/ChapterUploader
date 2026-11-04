#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "chapterstore.h"
#include "uploadworker.h"

#include <QMainWindow>
#include <QJsonArray>

class CdpClient;
class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void chooseSourceDir();
    void reloadChapters();
    void chooseClientExe();
    void enableAndLaunchClient();
    void toggleConnect();
    void startRun();
    void stopRun();
    void dumpPage();
    void runCustomJs();
    void enterBook();        // 在客户端里打开「书名」对应的作品
    void refreshBooks();     // 刷新书名下拉候选（不在书架页会自动切过去读）

private:
    QWidget *buildTopBar();
    QWidget *buildLeftPanel();
    QWidget *buildRightPanel();

    void log(const QString &msg);
    void flushLog();                   // 把攒着的日志一次性刷进控件
    void setConnUi(bool connected, const QString &detail);
    bool isDraftMode() const;          // 模式：上传新章 / 校对已有草稿
    void updateModeUi();
    void injectSiteScript();
    void revertClientPatch();
    void updateRunUi();
    void refreshRangeBounds();
    void rebuildTable();               // 表格跟随模式：草稿=区间章节，上传=本地 txt
    void saveSettings();
    void loadSettings();

    void refreshBooksImpl(bool silent);          // silent：只在当前页有书架时静默填充
    void applyBookList(const QJsonArray &names); // 填充下拉并尽量保留用户已输入的文本
    void doOpenBook(const QString &name, const QString &cbid);   // 确认脚本就绪后开书
    void doOpenBookNow(const QString &name, const QString &cbid); // 实际执行 __yw.openBook
    QString jsString(const QString &s) const;    // 生成安全的 JS 字符串字面量

    ChapterStore  m_store;
    CdpClient    *m_cdp = nullptr;
    UploadWorker *m_worker = nullptr;

    // 顶部
    QLineEdit *m_exePath = nullptr;
    QLabel    *m_connDot = nullptr;
    QLabel    *m_connText = nullptr;
    QPushButton *m_connectBtn = nullptr;
    QPushButton *m_launchBtn = nullptr;

    // 左侧
    QLineEdit      *m_srcDir = nullptr;
    QFrame         *m_dirCard = nullptr;
    QComboBox      *m_mode = nullptr;      // 上传新章 / 校对已有草稿
    QLabel         *m_rangeTag = nullptr;
    QSpinBox       *m_from = nullptr;
    QSpinBox       *m_to = nullptr;
    QTableWidget   *m_table = nullptr;
    QCheckBox      *m_proofread = nullptr;
    QCheckBox      *m_skipExisting = nullptr;
    QComboBox      *m_bookName = nullptr;      // 书名：可编辑下拉，候选来自客户端书架
    QPushButton    *m_bookBtn = nullptr;
    QCheckBox      *m_titleWithNo = nullptr;
    QCheckBox      *m_titleUseStem = nullptr;
    QComboBox      *m_finishMode = nullptr;
    QPushButton    *m_startBtn = nullptr;
    QPushButton    *m_stopBtn = nullptr;
    QProgressBar   *m_progress = nullptr;

    // 右侧
    QPlainTextEdit *m_log = nullptr;
    QStringList     m_logBuf;          // 待刷新的日志行：攒一批再插，别每条都触发一次重排+重绘
    QTimer         *m_logTimer = nullptr;

    QTimer *m_autoConnect = nullptr;   // 启动客户端后自动重试连接
    int     m_autoTry = 0;

    QTimer *m_bookPoll = nullptr;      // 点开作品后轮询写作页标签
    int     m_bookPollTry = 0;
    QString m_pendingBook;                    // 未连接时点了「进入作品」，连上后自动打开

    QVector<Chapter> m_subset;      // 本次要传的章节（范围内）
    int  m_rowOffset = -1;          // 第一个章节在表格里的行号

    bool m_connected = false;
    bool m_running = false;

protected:
    void closeEvent(QCloseEvent *e) override;
};

#endif // MAINWINDOW_H
