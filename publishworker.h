#ifndef PUBLISHWORKER_H
#define PUBLISHWORKER_H

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class CdpClient;

// 一条待发布的安排：第 no 章，定在 when 发布
struct PublishItem
{
    int no = 0;
    QString title;
    QDateTime when;
};

struct PublishOptions
{
    QString bookName;
    QString bookCbid;
    bool dryRun = false;   // 只打开弹窗、填好日期时间就关掉，不提交（用来核对流程）
};

// 定时发布状态机：切草稿列表 → 打开该章 → 点「发布」→ 勾定时发布 → 日历选日期
// → 时间面板选时分 + 确定 → 点「定时发布」→ 校验章节状态变成「已定时」。
//
// 这里的 runJs / poll / waitThen 与 uploadworker 里的是同一套写法。
// 没有抽公共基类是有意的：上传链路已实测稳定，不想为了复用去改它。
class PublishWorker : public QObject
{
    Q_OBJECT

public:
    explicit PublishWorker(CdpClient *cdp, QObject *parent = nullptr);

    void setPlan(const QVector<PublishItem> &plan) { m_plan = plan; }
    void start(const PublishOptions &opt);
    void stop();

    bool isRunning() const { return m_running; }

signals:
    void log(const QString &msg);
    void itemStatus(int index, const QString &status);
    void itemDone(int index, bool ok, const QString &detail);
    void finished(int okCount, int failCount);

private:
    enum Step {
        P_DraftTab = 0,   // 确认侧栏在「草稿」列表
        P_Select,         // 跳到这一章
        P_WaitReady,      // 等标题和正文加载完
        P_OpenDialog,     // 点右上角「发布」
        P_WaitDialog,     // 等「确认章节信息」弹窗
        P_Schedule,       // 勾定时发布 + 日历选日期 + 时间面板选时分并确定
        P_Submit,         // 点「定时发布」
        P_WaitResult,     // 等弹窗关闭 + 章节状态变「已定时」
        P_Next,
    };

    void ensureWritePage();
    void waitForWriteTarget(int attempt);
    void failStart(const QString &why);

    void beginItem();
    void nextStep();
    void scheduleNext(int step, int delayMs);
    void failItem(const QString &why, bool closeDialog);

    void runJs(const QString &js, std::function<void(bool, const QJsonObject &)> done);
    void poll(const QString &js,
              std::function<bool(const QJsonObject &)> pred,
              int intervalMs, int timeoutMs,
              std::function<void(bool ok, const QJsonObject &obj)> done);
    void waitThen(int step, const QString &js,
                  std::function<bool(const QJsonObject &)> pred,
                  int capMs, int minMs = 0);
    void stopPoll() { ++m_pollSeq; }

    CdpClient *m_cdp = nullptr;
    QVector<PublishItem> m_plan;
    PublishOptions m_opt;

    bool m_running = false;
    int  m_index = -1;
    int  m_step = 0;
    int  m_ok = 0, m_fail = 0;
    int  m_gen = 0;
    int  m_selectRetry = 0;
    int  m_dialogRetry = 0;
    int  m_formRetry = 0;
    int  m_extraConfirm = 0;
    int  m_consecutiveFails = 0;
    bool m_itemFailed = false;
    QStringList m_seenMsgs;      // 提交后页面弹出的提示，写进日志用

    std::function<void()> m_pollLoop;
    int m_pollSeq = 0;
};

#endif // PUBLISHWORKER_H
