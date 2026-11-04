#ifndef UPLOADWORKER_H
#define UPLOADWORKER_H

#include "chapterstore.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QVector>

#include <functional>

class CdpClient;
class QTimer;

struct UploadOptions
{
    bool proofread   = true;    // 是否跑「校对 → 检测 → 逐条修改」
    QString finishMode = QStringLiteral("none");   // none / save / publish
    bool titleWithNo = true;    // 标题加「第N章」
    bool titleUseStem = true;   // 标题用文件名（含编号）
    int  maxFixes    = 200;     // 单章最多点多少次「修改」，防死循环
    QString bookName;           // 作品名：书架页用它找卡片自动进书；留空=点第一本
    QString bookCbid;           // 作品 ID：有它就直接走客户端 IPC 开书，不依赖封面
    bool skipExisting = true;   // 侧栏已有同号章节就跳过（重跑不会生成重复章）
    // 草稿校对模式：不新建章、不写标题正文，只逐章点开 → 校对 → 改病句
    bool draftMode    = false;
};

// 逐章上传的状态机。
// 每一步都是「跑一段 JS → 等结果 → 延时进入下一步」，不阻塞界面线程。
class UploadWorker : public QObject
{
    Q_OBJECT

public:
    explicit UploadWorker(CdpClient *cdp, QObject *parent = nullptr);

    void setChapters(const QVector<Chapter> &list) { m_chapters = list; }
    void start(const UploadOptions &opt);
    void stop();

    bool isRunning() const { return m_running; }

signals:
    void log(const QString &msg);
    void chapterStatus(int index, const QString &status);
    void chapterDone(int index, bool ok, const QString &detail);
    void finished(int okCount, int failCount);

private:
    void ensureWritePage();     // 开始前先确认在写作页；在书架就自动点开作品
    void waitForWriteTarget(int attempt);
    void failStart(const QString &why);
    void beginChapter();
    void nextStep();
    void scheduleNext(int step, int delayMs);
    void failChapter(const QString &why);

    // 轮询：反复跑 js，直到 pred 为真或超时
    // 注意：循环体必须挂在成员上。早先用「局部 shared_ptr + weak_ptr 自引用」，
    // poll() 一返回 shared_ptr 就析构，weak.lock() 永远为空 —— 轮询只跑一次就静默死掉，
    // 连超时都不会触发，表现为「点了检测就一直卡在等待」。
    void poll(const QString &js,
              std::function<bool(const QJsonObject &)> pred,
              int intervalMs, int timeoutMs,
              std::function<void(bool ok, const QJsonObject &obj)> done);

    // 条件等待（waitThen）：把原来「死等 N 毫秒」换成「一就绪就走」。
    // capMs 就是原来那个固定延时 —— 到点不管怎样都继续，所以最坏情况和旧版一模一样，
    // 只可能更快，不会更慢。minMs 给客户端留最小沉降时间，别在写完的下一毫秒就判「好了」。
    void waitThen(int step, const QString &js,
                  std::function<bool(const QJsonObject &)> pred,
                  int capMs, int minMs = 0);

    void stopPoll() { ++m_pollSeq; }

    void runJs(const QString &js, std::function<void(bool, const QJsonObject &)> done);
    void applyOneMore();
    void finishChapter();

    CdpClient *m_cdp = nullptr;
    QVector<Chapter> m_chapters;
    UploadOptions m_opt;

    bool     m_running = false;
    int      m_index   = -1;
    int      m_step    = 0;
    int      m_ok = 0, m_fail = 0;
    int      m_fixes = 0;
    int      m_gen = 0;              // 每次 start/stop 自增，旧链条自动失效
    int      m_titleRetry = 0;
    int      m_focusRetry = 0;         // 把焦点挪进正文区的重试次数
    int      m_bodyRetry  = 0;
    int      m_consecutiveFails = 0;   // 连续失败计数，满 3 停止
    bool     m_chapterFailed = false;  // 本章已记失败，S_NextChapter 别再记成成功
    int      m_selectRetry = 0;        // 侧栏滚动后重找上一章的次数
    QStringList m_checkKinds;          // 要依次跑的检测块：typo / sentence
    int      m_kindIdx = 0;
    QElapsedTimer m_deadline;

    std::function<void()> m_pollLoop;  // 当前轮询的循环体（成员持有，保证存活）
    int      m_pollSeq = 0;            // 轮询代号：自增即作废所有在途 tick
};

#endif // UPLOADWORKER_H
