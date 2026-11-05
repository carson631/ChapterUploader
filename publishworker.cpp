#include "publishworker.h"
#include "cdpclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QTimer>
#include <QtMath>

namespace {

// 把 QString 变成安全的 JS 字面量（含引号）
QString jsStr(const QString &s)
{
    const QByteArray json = QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

} // namespace

PublishWorker::PublishWorker(CdpClient *cdp, QObject *parent)
    : QObject(parent), m_cdp(cdp)
{
}

void PublishWorker::start(const PublishOptions &opt)
{
    if (m_running)
        return;
    ++m_gen;
    ++m_pollSeq;
    m_pollLoop = nullptr;
    m_opt = opt;
    m_running = true;
    m_index = -1;
    m_ok = m_fail = 0;
    m_consecutiveFails = 0;

    emit log(QStringLiteral("=== 开始：定时发布 %1 章（%2 ~ %3）%4 ===")
                 .arg(m_plan.size())
                 .arg(m_plan.isEmpty() ? 0 : m_plan.first().no)
                 .arg(m_plan.isEmpty() ? 0 : m_plan.last().no)
                 .arg(opt.dryRun ? QStringLiteral("【演练模式：只填表不提交】") : QString()));
    ensureWritePage();
}

void PublishWorker::stop()
{
    if (!m_running)
        return;
    ++m_gen;
    ++m_pollSeq;
    m_pollLoop = nullptr;
    m_running = false;
    emit log(QStringLiteral("已停止（已经排上的定时发布不会撤销，需要的话去客户端里改）"));
    emit finished(m_ok, m_fail);
}

// 「发布」按钮在写作页右侧；连上来的可能是书架，先确认在写作页并进书
void PublishWorker::ensureWritePage()
{
    const int gen = m_gen;
    runJs(QStringLiteral("__yw.where()"), [this, gen](bool ok, const QJsonObject &r) {
        if (gen != m_gen) return;
        if (ok && r.value(QStringLiteral("page")).toString() == QLatin1String("write")
            && r.value(QStringLiteral("ready")).toBool()) {
            emit log(QStringLiteral("当前已在写作页，开始"));
            beginItem();
            return;
        }
        if (ok && r.value(QStringLiteral("page")).toString() == QLatin1String("write")) {
            emit log(QStringLiteral("已在写作页，等侧栏渲染完…"));
            waitForWriteTarget(0);
            return;
        }
        runJs(QStringLiteral("__yw.openBook({cbid:%1, title:%2})")
                  .arg(jsStr(m_opt.bookCbid), jsStr(m_opt.bookName)),
              [this, gen](bool ok2, const QJsonObject &r2) {
            if (gen != m_gen) return;
            if (!ok2 || !r2.value(QStringLiteral("ok")).toBool()) {
                failStart(QStringLiteral("进不了作品：%1").arg(r2.value(QStringLiteral("why")).toString()));
                return;
            }
            emit log(QStringLiteral("已打开作品「%1」，等写作页加载…")
                         .arg(r2.value(QStringLiteral("opened")).toString()));
            waitForWriteTarget(0);
        });
    });
}

void PublishWorker::waitForWriteTarget(int attempt)
{
    const int gen = m_gen;
    if (attempt > 60) {                        // 60 × 300ms ≈ 18 秒
        failStart(QStringLiteral("等写作页加载超时（18 秒）。客户端里可能弹了窗，看一眼再重试。"));
        return;
    }
    QTimer::singleShot(300, this, [this, gen, attempt]() {
        if (gen != m_gen || !m_running)
            return;
        runJs(QStringLiteral("__yw.where()"), [this, gen, attempt](bool ok, const QJsonObject &r) {
            if (gen != m_gen) return;
            if (ok && r.value(QStringLiteral("page")).toString() == QLatin1String("write")
                && r.value(QStringLiteral("ready")).toBool()) {
                emit log(QStringLiteral("写作页就绪（侧栏 %1 章）")
                             .arg(r.value(QStringLiteral("items")).toInt()));
                beginItem();
                return;
            }
            m_cdp->switchToChapterTarget([this, gen, attempt](bool, const QString &) {
                if (gen != m_gen) return;
                waitForWriteTarget(attempt + 1);
            });
        });
    });
}

void PublishWorker::failStart(const QString &why)
{
    emit log(QStringLiteral("✗ %1").arg(why));
    m_running = false;
    emit finished(m_ok, m_fail);
}

void PublishWorker::beginItem()
{
    if (!m_running)
        return;
    ++m_index;
    if (m_index >= m_plan.size()) {
        m_running = false;
        emit log(QStringLiteral("=== 全部结束：成功 %1，失败 %2%3 ===")
                     .arg(m_ok).arg(m_fail)
                     .arg(m_opt.dryRun ? QStringLiteral("（演练，未真正提交）") : QString()));
        emit finished(m_ok, m_fail);
        return;
    }

    m_selectRetry = m_dialogRetry = m_formRetry = 0;
    m_extraConfirm = 0;
    m_itemFailed = false;
    m_seenMsgs.clear();

    const PublishItem &it = m_plan.at(m_index);
    emit log(QStringLiteral("[%1/%2] 第%3章 → %4")
                 .arg(m_index + 1).arg(m_plan.size()).arg(it.no)
                 .arg(it.when.toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
    emit itemStatus(m_index, QStringLiteral("进行中"));

    m_step = P_DraftTab;
    nextStep();
}

void PublishWorker::scheduleNext(int step, int delayMs)
{
    const int gen = m_gen;
    QTimer::singleShot(delayMs, this, [this, gen, step]() {
        if (gen != m_gen || !m_running)
            return;
        m_step = step;
        nextStep();
    });
}

void PublishWorker::nextStep()
{
    if (!m_running || m_index < 0 || m_index >= m_plan.size())
        return;

    const PublishItem it = m_plan.at(m_index);
    const int gen = m_gen;

    switch (m_step) {
    // 侧栏必须是「草稿」列表：发布完一章后客户端可能会切到别的列表
    case P_DraftTab:
        runJs(QStringLiteral("__yw.ensureDraftTab()"), [this, gen](bool ok, const QJsonObject &r) {
            if (gen != m_gen) return;
            if (!ok || !r.value(QStringLiteral("ok")).toBool()) {
                emit log(QStringLiteral("  ⚠ 没能切到「草稿」列表：%1（继续尝试）")
                             .arg(r.value(QStringLiteral("why")).toString()));
            } else if (!r.value(QStringLiteral("already")).toBool()) {
                emit log(QStringLiteral("  已切到「草稿」列表"));
            }
            scheduleNext(P_Select, 300);
        });
        break;

    case P_Select:
        runJs(QStringLiteral("__yw.pickChapter(%1)").arg(it.no),
              [this, gen, it](bool ok, const QJsonObject &r) {
            if (gen != m_gen) return;
            if (!ok || !r.value(QStringLiteral("ok")).toBool()) {
                if (r.value(QStringLiteral("definitive")).toBool()) {
                    failItem(r.value(QStringLiteral("why")).toString(), false);
                    return;
                }
                if (++m_selectRetry <= 2) {
                    emit log(QStringLiteral("  第 %1 章没点开（%2），重试 %3/2")
                                 .arg(it.no)
                                 .arg(r.value(QStringLiteral("why")).toString())
                                 .arg(m_selectRetry));
                    scheduleNext(P_Select, 800);
                    return;
                }
                failItem(QStringLiteral("侧栏里找不到第 %1 章").arg(it.no), false);
                return;
            }
            emit log(QStringLiteral("  已切到 %1").arg(r.value(QStringLiteral("text")).toString()));
            scheduleNext(P_WaitReady, 400);
        });
        break;

    case P_WaitReady: {
        // 连续三次「标题对上 + 正文非空」才算加载好
        auto hits = std::make_shared<int>(0);
        poll(QStringLiteral("__yw.draftReady(%1)").arg(it.no),
             [hits](const QJsonObject &o) {
                 const bool ready = o.value(QStringLiteral("ready")).toBool()
                                    || o.value(QStringLiteral("ok")).toBool();
                 if (!ready) { *hits = 0; return false; }
                 return ++*hits >= 3;
             },
             300, 25000,
             [this, gen, it](bool ok, const QJsonObject &o) {
                 if (gen != m_gen) return;
                 const QString title = o.value(QStringLiteral("title")).toString();
                 const int chars = o.value(QStringLiteral("chars")).toInt();
                 QString norm = title;
                 norm.remove(QRegularExpression(QStringLiteral("\\s")));
                 const bool titleOk = norm.startsWith(QStringLiteral("第%1章").arg(it.no));
                 if (chars <= 0 || (!ok && !titleOk)) {
                     failItem(QStringLiteral("第 %1 章没加载出正文（当前「%2」，%3 字）")
                                  .arg(it.no).arg(title).arg(chars), false);
                     return;
                 }
                 emit log(QStringLiteral("  已加载「%1」（%2 字）").arg(title).arg(chars));
                 scheduleNext(P_OpenDialog, 300);
             });
        break;
    }

    case P_OpenDialog:
        runJs(QStringLiteral("__yw.openPublishDialog()"), [this, gen](bool ok, const QJsonObject &r) {
            if (gen != m_gen) return;
            if (!ok || !r.value(QStringLiteral("ok")).toBool()) {
                const QJsonArray btns = r.value(QStringLiteral("buttons")).toArray();
                QStringList names;
                for (const QJsonValue &v : btns) names << v.toString();
                failItem(QStringLiteral("点不到「发布」：%1%2")
                             .arg(r.value(QStringLiteral("why")).toString(),
                                  names.isEmpty() ? QString()
                                                  : QStringLiteral("；可见按钮：%1")
                                                        .arg(names.join(QStringLiteral(" / ")))),
                         false);
                return;
            }
            emit log(QStringLiteral("  已点「发布」，等确认弹窗…"));
            scheduleNext(P_WaitDialog, 600);
        });
        break;

    case P_WaitDialog:
        poll(QStringLiteral("__yw.publishDialog()"),
             [](const QJsonObject &o) { return o.value(QStringLiteral("open")).toBool(); },
             300, 8000,
             [this, gen](bool ok, const QJsonObject &) {
                 if (gen != m_gen) return;
                 if (!ok) {
                     failItem(QStringLiteral("点了「发布」但「确认章节信息」弹窗没出来"), false);
                     return;
                 }
                 scheduleNext(P_Schedule, 200);
             });
        break;

    case P_Schedule: {
        const QString d = it.when.toString(QStringLiteral("yyyy-MM-dd"));
        const QString t = it.when.toString(QStringLiteral("HH:mm"));
        runJs(QStringLiteral("__yw.prepareSchedule(%1, %2)").arg(jsStr(d), jsStr(t)),
              [this, gen, d, t](bool ok, const QJsonObject &r) {
            if (gen != m_gen) return;
            const QString gotDate = r.value(QStringLiteral("date")).toString();
            const QString gotTime = r.value(QStringLiteral("time")).toString();
            const bool good = ok && r.value(QStringLiteral("ok")).toBool()
                              && gotDate == d && gotTime == t;
            if (!good) {
                if (++m_formRetry <= 1) {
                    emit log(QStringLiteral("  ⚠ 日期/时间没填对（期望 %1 %2，实际 %3 %4：%5），重试")
                                 .arg(d, t, gotDate, gotTime,
                                      r.value(QStringLiteral("why")).toString()));
                    scheduleNext(P_Schedule, 800);
                    return;
                }
                failItem(QStringLiteral("定时时间没设置成功（期望 %1 %2，实际「%3 %4」%5）")
                             .arg(d, t, gotDate, gotTime,
                                  r.value(QStringLiteral("why")).toString()), true);
                return;
            }
            emit log(QStringLiteral("  定时发布已填好：%1 %2").arg(gotDate, gotTime));
            scheduleNext(P_Submit, 300);
        });
        break;
    }

    case P_Submit:
        if (m_opt.dryRun) {
            emit log(QStringLiteral("  【演练】日期时间已核对，关闭弹窗，不提交"));
            runJs(QStringLiteral("__yw.closePublishDialog()"), [this, gen](bool, const QJsonObject &) {
                if (gen != m_gen) return;
                scheduleNext(P_Next, 500);
            });
            break;
        }
        runJs(QStringLiteral("__yw.submitSchedule()"), [this, gen](bool ok, const QJsonObject &r) {
            if (gen != m_gen) return;
            if (!ok || !r.value(QStringLiteral("ok")).toBool()) {
                const QJsonArray btns = r.value(QStringLiteral("buttons")).toArray();
                QStringList names;
                for (const QJsonValue &v : btns) names << v.toString();
                failItem(QStringLiteral("点不到「定时发布」：%1%2")
                             .arg(r.value(QStringLiteral("why")).toString(),
                                  names.isEmpty() ? QString()
                                                  : QStringLiteral("；弹窗按钮：%1")
                                                        .arg(names.join(QStringLiteral(" / ")))),
                         true);
                return;
            }
            emit log(QStringLiteral("  已点「%1」，等结果…").arg(r.value(QStringLiteral("text")).toString()));
            scheduleNext(P_WaitResult, 800);
        });
        break;

    case P_WaitResult:
        poll(QStringLiteral("__yw.publishState()"),
             [](const QJsonObject &o) { return !o.value(QStringLiteral("open")).toBool(); },
             500, 25000,
             [this, gen, it](bool closed, const QJsonObject &o) {
                 if (gen != m_gen) return;
                 QStringList msgs;
                 for (const QJsonValue &v : o.value(QStringLiteral("msgs")).toArray())
                     msgs << v.toString();
                 if (!msgs.isEmpty())
                     m_seenMsgs = msgs;

                 if (!closed) {
                     // 弹窗还开着：可能是二次确认，点一次「确定」再等
                     if (m_extraConfirm == 0) {
                         ++m_extraConfirm;
                         emit log(QStringLiteral("  弹窗还开着，试试确认框…"));
                         runJs(QStringLiteral("__yw.clickConfirmIfAsked()"),
                               [this, gen](bool, const QJsonObject &) {
                                   if (gen != m_gen) return;
                                   scheduleNext(P_WaitResult, 1200);
                               });
                         return;
                     }
                     failItem(QStringLiteral("提交后弹窗没关（%1）")
                                  .arg(msgs.isEmpty() ? QStringLiteral("页面没给提示")
                                                      : msgs.join(QStringLiteral(" / "))),
                              true);
                     return;
                 }

                 // 弹窗关了 → 用目录里的状态确认（5 = 已定时发布）
                 runJs(QStringLiteral("__yw.chapterStatus(%1)").arg(it.no),
                       [this, gen, it, msgs](bool ok2, const QJsonObject &s) {
                     if (gen != m_gen) return;
                     const QString st = s.value(QStringLiteral("status")).toString();
                     if (ok2 && st == QLatin1String("5")) {
                         emit log(QStringLiteral("  ✓ 已排入定时发布%1")
                                      .arg(msgs.isEmpty() ? QString() : QStringLiteral("（%1）")
                                                                       .arg(msgs.join(QStringLiteral(" / ")))));
                         scheduleNext(P_Next, 600);
                         return;
                     }
                     // 目录有延迟：再等两秒复核一次
                     QStringList seen = msgs;
                     QTimer::singleShot(2000, this, [this, gen, it, seen]() {
                         if (gen != m_gen || !m_running) return;
                         runJs(QStringLiteral("__yw.chapterStatus(%1)").arg(it.no),
                               [this, gen, it, seen](bool ok3, const QJsonObject &s2) {
                             if (gen != m_gen) return;
                             const QString st2 = s2.value(QStringLiteral("status")).toString();
                             if (ok3 && st2 == QLatin1String("5")) {
                                 emit log(QStringLiteral("  ✓ 已排入定时发布"));
                                 scheduleNext(P_Next, 400);
                                 return;
                             }
                             failItem(QStringLiteral("提交后状态没变成「已定时」（当前状态码「%1」%2）")
                                          .arg(st2,
                                               seen.isEmpty() ? QString()
                                                              : QStringLiteral("；提示：%1")
                                                                    .arg(seen.join(QStringLiteral(" / ")))),
                                      true);
                         });
                     });
                 });
             });
        break;

    case P_Next:
        if (!m_itemFailed) {
            emit itemDone(m_index, true, QString());
            emit itemStatus(m_index, m_opt.dryRun ? QStringLiteral("演练通过")
                                                  : QStringLiteral("已定时"));
            ++m_ok;
            m_consecutiveFails = 0;
        }
        m_itemFailed = false;
        beginItem();
        break;

    default:
        break;
    }
}

void PublishWorker::failItem(const QString &why, bool closeDialog)
{
    emit log(QStringLiteral("  ✗ 失败：%1").arg(why));
    emit itemDone(m_index, false, why);
    emit itemStatus(m_index, QStringLiteral("失败"));
    ++m_fail;
    ++m_consecutiveFails;
    m_itemFailed = true;

    if (m_consecutiveFails >= 3) {
        emit log(QStringLiteral("连续失败 %1 章，停止运行（检查页面状态后重试）").arg(m_consecutiveFails));
        m_running = false;
        emit finished(m_ok, m_fail);
        return;
    }

    if (closeDialog) {
        const int gen = m_gen;
        runJs(QStringLiteral("__yw.closePublishDialog()"), [this, gen](bool, const QJsonObject &) {
            if (gen != m_gen || !m_running) return;
            emit log(QStringLiteral("  跳过这一章，继续下一章"));
            scheduleNext(P_Next, 600);
        });
        return;
    }
    emit log(QStringLiteral("  跳过这一章，继续下一章"));
    scheduleNext(P_Next, 800);
}

// ---------------- 基础设施 ----------------

void PublishWorker::runJs(const QString &js, std::function<void(bool, const QJsonObject &)> done)
{
    const int gen = m_gen;
    m_cdp->evaluate(js, [this, gen, done](bool ok, const QJsonValue &v, const QString &err) {
        if (gen != m_gen)
            return;
        if (!ok) {
            emit log(QStringLiteral("  JS 出错：%1").arg(err));
            done(false, QJsonObject());
            return;
        }
        done(true, v.toObject());
    });
}

void PublishWorker::poll(const QString &js,
                         std::function<bool(const QJsonObject &)> pred,
                         int intervalMs, int timeoutMs,
                         std::function<void(bool, const QJsonObject &)> done){
    const int gen = m_gen;
    const int seq = ++m_pollSeq;
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();

    m_pollLoop = [this, gen, seq, js, pred, intervalMs, timeoutMs, done, elapsed]() {
        if (gen != m_gen || seq != m_pollSeq)
            return;
        runJs(js, [this, gen, seq, pred, intervalMs, timeoutMs, done, elapsed](bool ok, const QJsonObject &o) {
            if (gen != m_gen || seq != m_pollSeq)
                return;
            if (ok && pred(o)) {
                ++m_pollSeq;
                done(true, o);
                return;
            }
            if (elapsed->elapsed() >= timeoutMs) {
                ++m_pollSeq;
                done(false, o);
                return;
            }
            QTimer::singleShot(intervalMs, this, [this, seq]() {
                if (seq == m_pollSeq && m_pollLoop)
                    m_pollLoop();
            });
        });
    };
    m_pollLoop();
}

void PublishWorker::waitThen(int step, const QString &js,
                             std::function<bool(const QJsonObject &)> pred,
                             int capMs, int minMs)
{
    const int gen = m_gen;
    auto t0 = std::make_shared<QElapsedTimer>();
    t0->start();
    poll(js,
         [pred, t0, minMs](const QJsonObject &o) {
             return t0->elapsed() >= minMs && pred(o);
         },
         150, capMs,
         [this, gen, step](bool, const QJsonObject &) {
             if (gen != m_gen || !m_running)
                 return;
             scheduleNext(step, 0);
         });
}
