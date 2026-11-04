#include "uploadworker.h"
#include "cdpclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QTimer>
#include <QtMath>

#include <memory>

namespace {

enum Step {
    S_SelectPrev = 0,     // 先选中上一章（新建章插在选中章之后）
    S_SkipDone,
    S_NewChapter,
    S_Title,
    S_FocusBody,          // 标题填完把焦点挪进正文区（否则 setContent 写不进当前章）
    S_Body,
    S_Verify,
    S_ProofreadOpen,
    S_PanelInfo,          // 等校对面板渲染出来，把各检测块的状态记进日志
    S_CheckClick,
    S_WaitResult,
    S_AfterChecks,        // 收尾：复核还有没有漏网的红线
    S_FixBlock,           // 把这一块的红线改完，再决定是继续下一块检测还是收尾
    S_FixOne,             // 改一条红线：点红线 → 修改框 → 蓝字
    S_Apply,
    S_Finish,
    S_ConfirmPublish,
    S_NextChapter,
    S_DraftSelect,        // 草稿模式：在侧栏点开第 N 章
    S_DraftWait,          // 草稿模式：等这一章的标题和正文加载进来
};

// 把 QString 变成安全的 JS 字面量（含引号）
QString jsStr(const QString &s)
{
    const QByteArray json = QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

QString labelOfKind(const QString &kind)
{
    if (kind == QLatin1String("typo"))     return QStringLiteral("错别字");
    if (kind == QLatin1String("sentence")) return QStringLiteral("病句");
    return kind;
}

} // namespace

UploadWorker::UploadWorker(CdpClient *cdp, QObject *parent)
    : QObject(parent), m_cdp(cdp)
{
}

void UploadWorker::start(const UploadOptions &opt)
{
    if (m_running)
        return;
    ++m_gen;
    m_opt = opt;
    m_running = true;
    m_index = -1;
    m_ok = m_fail = 0;
    if (opt.draftMode) {
        emit log(QStringLiteral("=== 开始：校对已有草稿 %1 章（第 %2 ~ %3 章）===")
                     .arg(m_chapters.size())
                     .arg(m_chapters.isEmpty() ? 0 : m_chapters.first().no)
                     .arg(m_chapters.isEmpty() ? 0 : m_chapters.last().no));
    } else {
        emit log(QStringLiteral("=== 开始：%1 章 → 新章/标题/正文%2%3 ===")
                     .arg(m_chapters.size())
                     .arg(opt.proofread ? QStringLiteral("/校对修改") : QString())
                     .arg(opt.finishMode == QLatin1String("none") ? QString()
                          : opt.finishMode == QLatin1String("publish") ? QStringLiteral("/发布") : QStringLiteral("/保存")));
    }
    ensureWritePage();
}

// 「新建章」按钮只在进书之后的写作页里；连接上来的可能是书架首页。
// 先判断在不在写作页，不在就从书架点开作品（会新开写作标签，可能换 target）。
void UploadWorker::ensureWritePage()
{
    const int gen = m_gen;
    runJs(QStringLiteral("__yw.where()"), [this, gen](bool ok, const QJsonObject &r) {
        if (gen != m_gen) return;
        if (ok && r.value(QStringLiteral("page")).toString() == QLatin1String("write")
            && r.value(QStringLiteral("ready")).toBool()) {
            emit log(QStringLiteral("当前已在写作页，直接开始"));
            beginChapter();
            return;
        }
        if (ok && r.value(QStringLiteral("page")).toString() == QLatin1String("write")) {
            emit log(QStringLiteral("已在写作页，等标题框和章节列表渲染完…"));
            waitForWriteTarget(0);
            return;
        }
        // 在书架或其他页面 → 点开作品
        runJs(QStringLiteral("__yw.openBook({cbid:%1, title:%2})")
                  .arg(jsStr(m_opt.bookCbid), jsStr(m_opt.bookName)),
              [this, gen](bool ok2, const QJsonObject &r2) {
            if (gen != m_gen) return;
            if (!ok2 || !r2.value(QStringLiteral("ok")).toBool()) {
                QStringList titles;
                for (const QJsonValue &v : r2.value(QStringLiteral("titles")).toArray())
                    titles << (v.isObject()
                                   ? v.toObject().value(QStringLiteral("title")).toString()
                                   : v.toString());
                failStart(QStringLiteral("进不了作品：%1%2")
                              .arg(r2.value(QStringLiteral("why")).toString(),
                                   titles.isEmpty() ? QString()
                                                    : QStringLiteral("；书架里有：%1").arg(titles.join(QStringLiteral("、")))));
                return;
            }
            emit log(QStringLiteral("已用 %1 打开作品「%2」，等写作页加载…")
                         .arg(r2.value(QStringLiteral("via")).toString(),
                              r2.value(QStringLiteral("opened")).toString()));
            waitForWriteTarget(0);
        });
    });
}

// 等写作页出现：要么当前页面 hash 变成 /chapter/，要么 /json/list 出现新的写作页 target。
// site.js 由主窗口在 connected 信号里重新注入，所以这里反复探测 __yw 是否就绪即可。
void UploadWorker::waitForWriteTarget(int attempt)
{
    const int gen = m_gen;
    if (attempt > 60) {                          // 60 × 300ms ≈ 18 秒
        failStart(QStringLiteral("等写作页加载超时（18 秒）。客户端里可能弹了窗，看一眼再重试。"));
        return;
    }
    QTimer::singleShot(300, this, [this, gen, attempt]() {
        if (gen != m_gen || !m_running)
            return;
        // 当前连接的页面变成写作页了？
        runJs(QStringLiteral("__yw.where()"), [this, gen, attempt](bool ok, const QJsonObject &r) {
            if (gen != m_gen) return;
            if (ok && r.value(QStringLiteral("page")).toString() == QLatin1String("write")
                && r.value(QStringLiteral("ready")).toBool()) {
                emit log(QStringLiteral("写作页就绪（侧栏 %1 章）")
                             .arg(r.value(QStringLiteral("items")).toInt()));
                beginChapter();
                return;
            }
            // 没变 → 看有没有新的写作页 target 可以切过去
            m_cdp->switchToChapterTarget([this, gen, attempt](bool switched, const QString &) {
                if (gen != m_gen) return;
                Q_UNUSED(switched);
                waitForWriteTarget(attempt + 1);
            });
        });
    });
}

void UploadWorker::failStart(const QString &why)
{
    emit log(QStringLiteral("✗ %1").arg(why));
    m_running = false;
    emit finished(m_ok, m_fail);
}

void UploadWorker::stop()
{
    if (!m_running)
        return;
    ++m_gen;
    ++m_pollSeq;                 // 顺手掐掉在途的轮询 tick
    m_pollLoop = nullptr;
    m_running = false;
    emit log(QStringLiteral("已停止（已写入客户端的内容保留在草稿里）"));
    emit finished(m_ok, m_fail);
}

void UploadWorker::beginChapter()
{
    if (!m_running)
        return;
    ++m_index;
    if (m_index >= m_chapters.size()) {
        m_running = false;
        emit log(QStringLiteral("=== 全部结束：成功 %1，失败 %2 ===").arg(m_ok).arg(m_fail));
        emit finished(m_ok, m_fail);
        return;
    }

    m_fixes = 0;
    m_chapterFailed = false;
    m_titleRetry = m_bodyRetry = 0;
    m_focusRetry = 0;
    // 校对面板里错别字、病句是两块独立检测，按面板顺序各跑一次
    m_checkKinds = {QStringLiteral("typo"), QStringLiteral("sentence")};
    m_kindIdx = 0;
    m_selectRetry = 0;
    m_deadline.start();

    const Chapter &c = m_chapters[m_index];
    if (m_opt.draftMode) {
        emit log(QStringLiteral("[%1/%2] 第%3章 草稿校对")
                     .arg(m_index + 1).arg(m_chapters.size()).arg(c.no));
    } else {
        emit log(QStringLiteral("[%1/%2] 第%3章 %4（%5 字）")
                     .arg(m_index + 1).arg(m_chapters.size()).arg(c.no).arg(c.name).arg(c.chars));
    }
    emit chapterStatus(m_index, QStringLiteral("进行中"));

    // 草稿模式：先点开侧栏里这一章，等正文加载完再校对（不动标题和正文）
    m_step = m_opt.draftMode ? S_DraftSelect : S_SelectPrev;
    nextStep();
}

void UploadWorker::scheduleNext(int step, int delayMs)
{
    const int gen = m_gen;
    QTimer::singleShot(delayMs, this, [this, gen, step]() {
        if (gen != m_gen || !m_running)
            return;
        m_step = step;
        nextStep();
    });
}

void UploadWorker::nextStep()
{
    if (!m_running || m_index < 0 || m_index >= m_chapters.size())
        return;

    const Chapter &c = m_chapters[m_index];
    const int gen = m_gen;

    switch (m_step) {
    // ---------- 草稿校对：点开侧栏里的第 N 章，等正文加载 ----------
    case S_DraftSelect:
        runJs(QStringLiteral("__yw.pickChapter(%1)").arg(c.no),
              [this, gen, c](bool ok, const QJsonObject &r) {
            if (gen != m_gen) return;
            if (!ok || !r.value(QStringLiteral("ok")).toBool()) {
                // 目录里确实没这一章（chapterStore 全量索引查过）→ 重试也没用，直接判失败
                if (r.value(QStringLiteral("definitive")).toBool()) {
                    failChapter(r.value(QStringLiteral("why")).toString());
                    return;
                }
                if (++m_selectRetry <= 2) {
                    emit log(QStringLiteral("  第 %1 章没点开（%2），重试 %3/2")
                                 .arg(c.no)
                                 .arg(r.value(QStringLiteral("why")).toString())
                                 .arg(m_selectRetry));
                    scheduleNext(S_DraftSelect, 800);
                    return;
                }
                QStringList sample;
                for (const QJsonValue &v : r.value(QStringLiteral("sample")).toArray())
                    sample << v.toString();
                failChapter(QStringLiteral("侧栏里找不到第 %1 章%2")
                                .arg(c.no)
                                .arg(sample.isEmpty() ? QString()
                                                      : QStringLiteral("；侧栏现有：%1")
                                                            .arg(sample.join(QStringLiteral(" / ")))));
                return;
            }
            const bool viaRoute = r.value(QStringLiteral("via")).toString() == QStringLiteral("route");
            const QString openMsg = viaRoute
                ? QStringLiteral("  已切到 %1（按目录跳章），等正文加载…")
                : QStringLiteral("  已点开 %1，等正文加载…");
            emit log(openMsg.arg(r.value(QStringLiteral("text")).toString()));
            scheduleNext(S_DraftWait, 400);
        });
        break;

    case S_DraftWait: {
        // 连续三次读到「标题对上 + 正文非空」才算加载好，避免标题先换、正文还在路上
        // （原来 700ms 两次；现在 300ms 三次 ≈ 0.6 秒的稳定窗口，比原来的 1.4 秒短）
        auto hits = std::make_shared<int>(0);
        poll(QStringLiteral("__yw.draftReady(%1)").arg(c.no),
             [hits](const QJsonObject &o) {
                 // 老版本脚本只回 ok、不回 ready，两个都认，别再因为字段名对不上白等到超时
                 const bool ready = o.value(QStringLiteral("ready")).toBool()
                                    || o.value(QStringLiteral("ok")).toBool();
                 if (!ready) {
                     *hits = 0;
                     return false;
                 }
                 return ++*hits >= 3;
             },
             300, 25000,
             [this, gen, c](bool ok, const QJsonObject &o) {
                 if (gen != m_gen) return;
                 const QString title = o.value(QStringLiteral("title")).toString();
                 const int chars = o.value(QStringLiteral("chars")).toInt();
                 const QString norm = QString(title).remove(QRegularExpression(QStringLiteral("\\s")));
                 const bool titleOk = norm.startsWith(QStringLiteral("第%1章").arg(c.no));

                 if (chars <= 0) {
                     // 真空章（还没写内容）就别校对了，直接跳过
                     emit log(QStringLiteral("  ⚠ 第 %1 章没加载出正文（标题「%2」，%3 字），跳过")
                                  .arg(c.no).arg(title).arg(chars));
                     failChapter(QStringLiteral("第 %1 章正文为空或加载超时").arg(c.no));
                     return;
                 }
                 if (!ok) {
                     // 超时但正文已经有了：标题对得上就照常往下走，别再当成空章丢掉
                     if (!titleOk) {
                         emit log(QStringLiteral("  ⚠ 第 %1 章标题对不上（当前「%2」，%3 字），跳过")
                                      .arg(c.no).arg(title).arg(chars));
                         failChapter(QStringLiteral("第 %1 章标题未切换到目标章").arg(c.no));
                         return;
                     }
                     emit log(QStringLiteral("  ⚠ 第 %1 章加载判定超时，但正文已在（%2 字），继续校对")
                                  .arg(c.no).arg(chars));
                     scheduleNext(S_ProofreadOpen, 400);
                     return;
                 }
                 emit log(QStringLiteral("  已加载「%1」（%2 字）").arg(title).arg(chars));
                 scheduleNext(S_ProofreadOpen, 400);
             });
        break;
    }

    case S_SelectPrev: {
        // 0) 已有同号章节 → 跳过，避免重跑生成重复章。
        //    但「只有编号、0 字的空占位章」不算已存在（客户端里常有先建壳的章），
        //    否则这一章会被误跳过、永远空着。
        runJs(QStringLiteral("__yw.hasChapter(%1)").arg(c.no), [this, gen, c](bool ok, const QJsonObject &r) {
            if (gen != m_gen) return;
            const bool exists = ok && r.value(QStringLiteral("has")).toBool();
            const bool empty  = ok && r.value(QStringLiteral("empty")).toBool();
            const QString gotTitle = r.value(QStringLiteral("title")).toString();
            const int words = r.value(QStringLiteral("words")).toInt();

            if (exists && m_opt.skipExisting) {
                emit log(words > 0
                             ? QStringLiteral("  侧栏已有第 %1 章（%2 字），跳过（想强制重传就取消勾选「跳过已存在章节」）")
                                   .arg(c.no).arg(words)
                             : QStringLiteral("  侧栏已有第 %1 章，跳过（想强制重传就取消勾选「跳过已存在章节」）")
                                   .arg(c.no));
                scheduleNext(S_SkipDone, 200);
                return;
            }

            // 0.5) 同号但正文为空：直接在原章里写内容，不点「新建章」。
            //      「新建章」是插在当前选中章之后，会把这条空章顶下去，结果多出一条空章。
            if (empty) {
                emit log(QStringLiteral("  第 %1 章是空章（标题「%2」，%3 字），原地填充，不新建章节")
                             .arg(c.no)
                             .arg(gotTitle.isEmpty() ? QStringLiteral("第%1章").arg(c.no) : gotTitle)
                             .arg(words));
                runJs(QStringLiteral("__yw.pickChapter(%1)").arg(c.no),
                      [this, gen, c](bool ok2, const QJsonObject &r2) {
                    if (gen != m_gen) return;
                    if (!ok2 || !r2.value(QStringLiteral("ok")).toBool()) {
                        failChapter(QStringLiteral("打不开第 %1 章：%2")
                                        .arg(c.no)
                                        .arg(r2.value(QStringLiteral("why")).toString()));
                        return;
                    }
                    emit log(QStringLiteral("  已切到空章 %1，开始写标题/正文")
                                 .arg(r2.value(QStringLiteral("text")).toString()));
                    // 标题真的切到这一章再动手，避免把内容写进别的章
                    const QString k1 = QStringLiteral("第%1章").arg(c.no);
                    const QString k2 = QStringLiteral("第 %1 章").arg(c.no);
                    poll(QStringLiteral("__yw.currentTitle()"),
                         [k1, k2](const QJsonObject &o) {
                             const QString all = o.value(QStringLiteral("title")).toString()
                                                 + QLatin1Char(' ')
                                                 + o.value(QStringLiteral("active")).toString();
                             return all.contains(k1) || all.contains(k2);
                         },
                         200, 6000,
                         [this, gen, c](bool loaded, const QJsonObject &o) {
                             if (gen != m_gen) return;
                             if (!loaded) {
                                 failChapter(QStringLiteral("切到第 %1 章后标题没对上（当前「%2」）")
                                                 .arg(c.no)
                                                 .arg(o.value(QStringLiteral("title")).toString()));
                                 return;
                             }
                             scheduleNext(S_Title, 400);
                         });
                });
                return;
            }

            // 1) 「新建章」插在当前选中章之后 → 先把光标落到上一章
            const int prev = c.no - 1;
            if (prev <= 0) {
                scheduleNext(S_NewChapter, 200);
                return;
            }
            runJs(QStringLiteral("__yw.selectChapter(%1)").arg(prev),
                  [this, gen, prev](bool ok2, const QJsonObject &r2) {
                if (gen != m_gen) return;
                if (!ok2 || !r2.value(QStringLiteral("ok")).toBool()) {
                    if (r2.value(QStringLiteral("scrolled")).toBool() && m_selectRetry < 3) {
                        ++m_selectRetry;
                        emit log(QStringLiteral("  第 %1 章不在侧栏视口里，滚动后重找（第 %2 次）")
                                     .arg(prev).arg(m_selectRetry));
                        scheduleNext(S_SelectPrev, 700);
                        return;
                    }
                    emit log(QStringLiteral("  ⚠ 没选中第 %1 章：%2（照常点新建章）")
                                 .arg(prev)
                                 .arg(r2.value(QStringLiteral("why")).toString()));
                    scheduleNext(S_NewChapter, 300);
                    return;
                }
                emit log(QStringLiteral("  已选中第 %1 章：%2")
                             .arg(prev)
                             .arg(r2.value(QStringLiteral("text")).toString()));

                // 等编辑器真的切到那一章（标题里出现「第N章」），否则新建章会插错位置
                const QString k1 = QStringLiteral("第%1章").arg(prev);
                const QString k2 = QStringLiteral("第 %1 章").arg(prev);
                poll(QStringLiteral("__yw.currentTitle()"),
                     [k1, k2](const QJsonObject &o) {
                         const QString all = o.value(QStringLiteral("title")).toString()
                                             + QLatin1Char(' ')
                                             + o.value(QStringLiteral("active")).toString();
                         return all.contains(k1) || all.contains(k2);
                     },
                     200, 6000,
                     [this, gen, prev](bool loaded, const QJsonObject &o) {
                         if (gen != m_gen) return;
                         if (!loaded)
                             emit log(QStringLiteral("  ⚠ 没等到第 %1 章加载完（当前标题：%2），继续点新建章")
                                          .arg(prev)
                                          .arg(o.value(QStringLiteral("title")).toString()));
                         scheduleNext(S_NewChapter, 500);
                     });
            });
        });
        break;
    }

    case S_SkipDone:
        emit chapterDone(m_index, true, QStringLiteral("已存在，跳过"));
        emit chapterStatus(m_index, QStringLiteral("跳过"));
        beginChapter();
        break;

    case S_NewChapter:
        // 点「新建章」之前先记下 hash 和侧栏条目数：点完靠它俩判断新章是不是真建出来了
        runJs(QStringLiteral("(()=>{window.__cuDlg=0;"
                             "window.__cuMark={h:location.hash,"
                             "n:document.querySelectorAll('[id^=\"chapter_\"]').length};"
                             "return {ok:true}})()"),
              [this, gen, c](bool, const QJsonObject &) {
            if (gen != m_gen) return;
            runJs(QStringLiteral("__yw.clickNewChapter()"), [this, gen, c](bool ok, const QJsonObject &r) {
                if (gen != m_gen) return;
                if (!ok || !r.value(QStringLiteral("ok")).toBool()) {
                    const QJsonArray btns = r.value(QStringLiteral("buttons")).toArray();
                    QStringList names;
                    for (const QJsonValue &v : btns) names << v.toString();
                    failChapter(QStringLiteral("点不到「新建章」：%1；可见按钮：%2")
                                    .arg(r.value(QStringLiteral("why")).toString(),
                                         names.join(QStringLiteral(" / "))));
                    return;
                }
                // 原来是「死等 1600 + 1000 毫秒」。现在边等边探：新章一出来立刻写标题，
                // 顺带把「内容未保存，是否继续」这类弹窗点掉（每章只点一次）。
                // 探测的最长等待仍是 2600ms —— 到点无条件继续，最坏情况与旧版一致。
                const QString no = QString::number(c.no);
                const QString probe = QStringLiteral(
                    "(()=>{const m=window.__cuMark||{};"
                    "const t=document.querySelector('#chapter-title');"
                    "const title=t?t.value.trim():'';"
                    "const n=document.querySelectorAll('[id^=\"chapter_\"]').length;"
                    "const d=[...document.querySelectorAll('button')].filter(x=>x.offsetParent)"
                    ".find(x=>/^(确定|继续|保存并继续|是)$/.test((x.innerText||'').trim()));"
                    "if(d&&!window.__cuDlg){window.__cuDlg=1;try{d.click()}catch(e){}}"
                    "return {grew:(location.hash!==(m.h||''))||(n>(m.n||0)),"
                    "mark:!!m.h,title:title,n:n,dlg:!!d}})()");
                waitThen(S_Title, probe,
                         [no](const QJsonObject &o) {
                             if (!o.value(QStringLiteral("mark")).toBool())
                                 return false;              // 打点没打上（页面刚刷新？）→ 等满再走
                             if (o.value(QStringLiteral("dlg")).toBool())
                                 return false;              // 弹窗还挂着，点掉再说
                             if (!o.value(QStringLiteral("grew")).toBool())
                                 return false;              // 新章还没出现
                             const QString title = o.value(QStringLiteral("title")).toString();
                             // 标题框要么是空的（新章还没名字），要么已经是这一章 ——
                             // 否则很可能还是上一章的标题，再等等，别把标题写进上一章
                             return title.isEmpty()
                                    || title.contains(QStringLiteral("第%1章").arg(no))
                                    || title.contains(QStringLiteral("第 %1 章").arg(no));
                         },
                         2600, 250);
            });
        });
        break;

    case S_Title: {
        const QString title = c.title(m_opt.titleWithNo, m_opt.titleUseStem);
        runJs(QStringLiteral("__yw.setTitle(%1)").arg(jsStr(title)),
              [this, gen](bool ok, const QJsonObject &r) {
                  if (gen != m_gen) return;
                  if (!ok || !r.value(QStringLiteral("ok")).toBool()) {
                      if (++m_titleRetry <= 2) {
                          emit log(QStringLiteral("  标题没写进去（%1），重试")
                                       .arg(r.value(QStringLiteral("why")).toString()));
                          scheduleNext(S_Title, 1200);
                          return;
                      }
                      failChapter(QStringLiteral("标题填充失败：%1")
                                      .arg(r.value(QStringLiteral("why")).toString()));
                      return;
                  }
                  emit log(QStringLiteral("  标题 → %1").arg(r.value(QStringLiteral("got")).toString()));
                  scheduleNext(S_FocusBody, 700);
              });
        break;
    }

    // 标题框里还留着焦点：TinyMCE 的 activeEditor 可能挂在上一章的编辑器上，
    // 这时 setContent 写进去页面上看不见 —— 必须先把焦点（和光标）挪进正文区。
    case S_FocusBody:
        runJs(QStringLiteral("__yw.focusBody()"), [this, gen](bool ok, const QJsonObject &r) {
            if (gen != m_gen) return;
            if (!ok || !r.value(QStringLiteral("ok")).toBool()) {
                if (++m_focusRetry <= 3) {
                    emit log(QStringLiteral("  正文区还没就绪（%1），重试 %2/3")
                                 .arg(r.value(QStringLiteral("why")).toString())
                                 .arg(m_focusRetry));
                    scheduleNext(S_FocusBody, 800);
                    return;
                }
                emit log(QStringLiteral("  ⚠ 没能把焦点移到正文，直接写入试试"));
                scheduleNext(S_Body, 300);
                return;
            }
            if (!r.value(QStringLiteral("focused")).toBool() && m_focusRetry < 3) {
                ++m_focusRetry;
                emit log(QStringLiteral("  光标还没进正文（第 %1 次），再点一下").arg(m_focusRetry));
                scheduleNext(S_FocusBody, 700);
                return;
            }
            emit log(QStringLiteral("  焦点 → 正文区（%1 %2，当前 %3 字）")
                         .arg(r.value(QStringLiteral("via")).toString(),
                              r.value(QStringLiteral("id")).toString())
                         .arg(r.value(QStringLiteral("chars")).toInt()));
            scheduleNext(S_Body, 500);
        });
        break;

    case S_Body:
        runJs(QStringLiteral("__yw.setBody(%1)").arg(jsStr(c.html())),
              [this, gen, want = c.chars](bool ok, const QJsonObject &r) {
                  if (gen != m_gen) return;
                  if (!ok || !r.value(QStringLiteral("ok")).toBool()) {
                      failChapter(QStringLiteral("正文填充失败：%1")
                                      .arg(r.value(QStringLiteral("why")).toString()));
                      return;
                  }
                  emit log(QStringLiteral("  正文写入（%1 %2）")
                               .arg(r.value(QStringLiteral("via")).toString(),
                                    r.value(QStringLiteral("id")).toString()));
                  // 原来是死等 1500ms 再校验。现在盯着编辑器字数：够了就走（留 500ms 让
                  // 客户端把 blur/自动保存的抖动过去），不够就等满 1500ms 仍去校验（照旧会报错重写）
                  waitThen(S_Verify, QStringLiteral("__yw.bodyState()"),
                           [want](const QJsonObject &o) {
                               const int got = o.value(QStringLiteral("chars")).toInt();
                               return got > 0 && qAbs(got - want) <= qMax(20, want / 50);
                           },
                           1500, 500);
              });
        break;

    case S_Verify: {
        const QString anchor = m_opt.titleUseStem ? m_chapters[m_index].stem
                                                  : m_chapters[m_index].name;
        runJs(QStringLiteral("__yw.bodyState()"), [this, gen, anchor](bool ok, const QJsonObject &r) {
            if (gen != m_gen) return;
            const int got  = r.value(QStringLiteral("chars")).toInt();
            const int want = m_chapters[m_index].chars;
            QString gotTitle = r.value(QStringLiteral("title")).toString();
            gotTitle.remove(QRegularExpression(QStringLiteral("\\s+")));
            QString anchor2 = anchor;
            anchor2.remove(QRegularExpression(QStringLiteral("\\s+")));
            // 标题框被客户端重刷（重取章节）时，正文一般也一起被刷没了 —— 一起补
            const bool titleLost = anchor2.isEmpty() || !gotTitle.contains(anchor2);
            if (!ok || qAbs(got - want) > qMax(20, want / 50) || titleLost) {   // 允许 2% 或 20 字误差
                if (++m_bodyRetry <= 2) {
                    emit log(QStringLiteral("  校验没过（期望 %1 字 / 实际 %2，编辑器 %3%4），重写")
                                 .arg(want).arg(got)
                                 .arg(r.value(QStringLiteral("editor")).toString(),
                                      titleLost ? QStringLiteral("，标题被刷成「%1」").arg(gotTitle)
                                                : QString()));
                    m_titleRetry = 0;
                    scheduleNext(titleLost ? S_Title : S_FocusBody, 1200);
                    return;
                }
                failChapter(QStringLiteral("正文校验失败：期望 %1 字，编辑器里 %2 字"
                                           "（看一眼客户端里是不是弹了窗，或正文区没获得焦点）")
                                .arg(want).arg(got));
                return;
            }
            emit log(QStringLiteral("  校验通过：%1 字（编辑器 %2%3）")
                         .arg(got)
                         .arg(r.value(QStringLiteral("editor")).toString(),
                              r.value(QStringLiteral("focused")).toBool() ? QStringLiteral("，光标在正文")
                                                                          : QString()));
            scheduleNext(m_opt.proofread ? S_ProofreadOpen : S_Finish, 400);
        });
        break;
    }

    case S_ProofreadOpen:
        runJs(QStringLiteral("__yw.openProofread()"), [this, gen](bool ok, const QJsonObject &r) {
            if (gen != m_gen) return;
            if (!ok || !r.value(QStringLiteral("ok")).toBool()) {
                emit log(QStringLiteral("  打开校对面板失败：%1（跳过校对）")
                             .arg(r.value(QStringLiteral("why")).toString()));
                scheduleNext(S_Finish, 300);
                return;
            }
            if (r.value(QStringLiteral("reset")).toBool())
                emit log(QStringLiteral("  校对面板本来是展开的（还挂着上一章结果），已收起重开"));
            else if (r.value(QStringLiteral("already")).toBool())
                emit log(QStringLiteral("  校对面板已展开，直接用（不重复点「校对」，那是开关）"));
            // 等面板渲染出来（原来是死等 900ms）：检测按钮的文案都出来了才往下走，
            // 免得面板还没渲染完就去点「检测」被当成没有这一块
            waitThen(S_PanelInfo, QStringLiteral("({k: __yw.checkKinds()})"),
                     [](const QJsonObject &o) {
                         for (const QJsonValue &v : o.value(QStringLiteral("k")).toArray()) {
                             if (!v.toObject().value(QStringLiteral("button")).toString().trimmed().isEmpty())
                                 return true;
                         }
                         return false;
                     },
                     900, 200);
        });
        break;

    // 面板就绪：先把「有几块检测、各自什么状态」记进日志，方便核对选择器
    case S_PanelInfo:
        runJs(QStringLiteral("({k: __yw.checkKinds()})"),
              [this, gen](bool ok2, const QJsonObject &r2) {
            if (gen != m_gen) return;
            if (ok2) {
                QStringList parts;
                for (const QJsonValue &v : r2.value(QStringLiteral("k")).toArray()) {
                    const QJsonObject o = v.toObject();
                    parts << QStringLiteral("%1（%2｜%3）")
                                 .arg(o.value(QStringLiteral("label")).toString(),
                                      o.value(QStringLiteral("header")).toString(),
                                      o.value(QStringLiteral("state")).toString());
                }
                emit log(parts.isEmpty()
                             ? QStringLiteral("  ⚠ 校对面板里没找到检测块")
                             : QStringLiteral("  校对面板：%1").arg(parts.join(QStringLiteral(" / "))));
            }
            m_kindIdx = 0;
            scheduleNext(S_CheckClick, 200);
        });
        break;

    case S_CheckClick: {
        // 校对面板里「错别字」和「病句」各有一个「检测」按钮，必须两个都点，
        // 只点第一个的话病句这块永远是「未检测」。
        if (m_kindIdx >= m_checkKinds.size()) {
            scheduleNext(S_AfterChecks, 300);
            break;
        }
        const QString kind  = m_checkKinds.at(m_kindIdx);
        const QString label = labelOfKind(kind);
        runJs(QStringLiteral("__yw.clickCheck(%1)").arg(jsStr(kind)),
              [this, gen, label, kind](bool ok, const QJsonObject &r) {
            if (gen != m_gen) return;
            if (!ok || !r.value(QStringLiteral("ok")).toBool()) {
                QStringList kinds;
                for (const QJsonValue &v : r.value(QStringLiteral("kinds")).toArray())
                    kinds << v.toObject().value(QStringLiteral("header")).toString();
                emit log(QStringLiteral("  跳过「%1」检测：%2%3")
                             .arg(label, r.value(QStringLiteral("why")).toString(),
                                  kinds.isEmpty() ? QString()
                                                  : QStringLiteral("；面板里只有：%1")
                                                        .arg(kinds.join(QStringLiteral(" / ")))));
                ++m_kindIdx;
                scheduleNext(S_CheckClick, 400);
                return;
            }
            const bool already = r.value(QStringLiteral("already")).toBool();
            emit log(QStringLiteral("  %1检测 → %2，等结果（最长 60 秒）")
                         .arg(label, already ? QStringLiteral("已在检测中")
                                             : r.value(QStringLiteral("header")).toString()));
            // 原来是死等 3500ms（已在检测中则 1000ms）才开始查状态。
            // 现在一看到「不再是未检测」就走 —— 客户端通常 200~400ms 就置成检测中，
            // 每块检测能省 3 秒左右，两块就是一章 5~6 秒。
            const int cap = already ? 1000 : 3500;
            waitThen(S_WaitResult, QStringLiteral("__yw.checkState(%1)").arg(jsStr(kind)),
                     [](const QJsonObject &o) {
                         if (!o.value(QStringLiteral("found")).toBool())
                             return true;                       // 没这一块，别等
                         const QString st = o.value(QStringLiteral("state")).toString();
                         return !st.isEmpty() && st != QLatin1String("idle");
                     },
                     cap, 150);
        });
        break;
    }

    case S_WaitResult: {
        const QString kind = (m_kindIdx >= 0 && m_kindIdx < m_checkKinds.size())
                                 ? m_checkKinds.at(m_kindIdx) : QString();
        const QString label = labelOfKind(kind);
        auto ticks = std::make_shared<int>(0);
        auto idleTicks = std::make_shared<int>(0);      // 连续多少秒还显示「未检测」
        auto reclicks  = std::make_shared<int>(0);
        poll(QStringLiteral("__yw.checkState(%1)").arg(jsStr(kind)),
             [this, gen, label, kind, ticks, idleTicks, reclicks](const QJsonObject &o) {
                 if (!o.value(QStringLiteral("found")).toBool())
                     return true;                    // 没有这一块，别傻等
                 // state 由页面侧算好：busy=检测中 / done=出结果 / idle=未检测 / unknown
                 // 只看这一块自己的标题，不会把隔壁那块的「未检测」算进来
                 const QString st = o.value(QStringLiteral("state")).toString();
                 if (gen == m_gen && ++*ticks % 16 == 1)     // 0.5 秒一拍，每 8 秒打一行
                     emit log(QStringLiteral("    …等「%1」：%2%3")
                                  .arg(label, st.isEmpty() ? QStringLiteral("unknown") : st,
                                       o.value(QStringLiteral("header")).toString().isEmpty()
                                           ? QString()
                                           : QStringLiteral("（%1）")
                                                 .arg(o.value(QStringLiteral("header")).toString())));
                 // 点了没反应（一直是「未检测」）就补点，最多两次
                 if (st == QLatin1String("idle")) {
                     if (++*idleTicks >= 12 && *reclicks < 2 && gen == m_gen) {   // 12 拍 = 6 秒
                         ++*reclicks;
                         *idleTicks = 0;
                         emit log(QStringLiteral("  「%1」还显示未检测，补点一次检测按钮").arg(label));
                         runJs(QStringLiteral("__yw.clickCheck(%1)").arg(jsStr(kind)),
                               [](bool, const QJsonObject &) { });
                     }
                 } else {
                     *idleTicks = 0;
                 }
                 return st == QLatin1String("done");
             },
             500, 60000,
             [this, gen, label](bool ok, const QJsonObject &o) {
                 if (gen != m_gen) return;
                 if (!ok) {
                     emit log(QStringLiteral("  「%1」等结果超时（60 秒），先改已有的再说").arg(label));
                 } else {
                     const QString head = o.value(QStringLiteral("header")).toString();
                     emit log(QStringLiteral("  「%1」结果：%2")
                                  .arg(label, head.isEmpty()
                                                ? o.value(QStringLiteral("sectionText")).toString().left(60)
                                                : head));
                 }
                 ++m_kindIdx;
                 // 这一块检测完就先把这一块改干净，再去做下一块检测（改字会改变下一块的结果）
                 scheduleNext(S_FixBlock, 500);
             });
        break;
    }

    // 把当前这一块的红线改完；改完再决定是继续下一块检测，还是收尾
    case S_FixBlock:
        runJs(QStringLiteral("__yw.fixInfo()"), [this, gen](bool ok, const QJsonObject &r) {
            if (gen != m_gen || !m_running) return;
            const int red   = ok ? r.value(QStringLiteral("red")).toInt() : 0;
            const int typos = ok ? r.value(QStringLiteral("typos")).toInt() : 0;
            if (red > 0) {
                scheduleNext(S_FixOne, 200);            // 正文里有红线：逐条 hover → 点蓝字
                return;
            }
            if (typos > 0) {
                scheduleNext(S_Apply, 200);             // 只剩列表里的「修改」按钮
                return;
            }
            if (m_kindIdx < m_checkKinds.size()) {
                emit log(QStringLiteral("  这一块已改完，继续下一块检测"));
                scheduleNext(S_CheckClick, 800);
            } else {
                scheduleNext(S_AfterChecks, 300);
            }
        });
        break;

    // 两块都检测完，看还有多少要改的
    case S_AfterChecks:
        runJs(QStringLiteral("({n: __yw.checkState().suggestions, f: __yw.fixInfo(), "
                             "k: __yw.checkKinds()})"),
              [this, gen](bool ok, const QJsonObject &r) {
            if (gen != m_gen) return;
            QStringList states;
            for (const QJsonValue &v : r.value(QStringLiteral("k")).toArray()) {
                const QJsonObject o = v.toObject();
                states << (o.value(QStringLiteral("header")).toString().isEmpty()
                               ? o.value(QStringLiteral("label")).toString()
                               : o.value(QStringLiteral("header")).toString());
            }
            if (!states.isEmpty())
                emit log(QStringLiteral("  面板状态：%1").arg(states.join(QStringLiteral(" / "))));
            const QJsonObject f = r.value(QStringLiteral("f")).toObject();
            const int marks = f.value(QStringLiteral("marks")).toInt();
            const int typos = f.value(QStringLiteral("typos")).toInt();
            const int red   = f.value(QStringLiteral("red")).toInt();
            emit log(QStringLiteral("  收尾检查：错别字红线 %1 条 / 病句红线 %2 处").arg(typos).arg(marks));
            if (red > 0) {
                scheduleNext(S_FixBlock, 300);       // 还有漏网的红线，再改一轮
                return;
            }
            scheduleNext(S_Finish, 400);
        });
        break;

    // 改一条病句：滚到红线 → 唤出修改框 → 点框里的蓝字
    case S_FixOne:
        runJs(QStringLiteral("__yw.fixOne()"), [this, gen](bool ok, const QJsonObject &r) {
            if (gen != m_gen || !m_running)
                return;
            if (!ok) {
                emit log(QStringLiteral("  改病句出错，停止校对（内容仍保留）"));
                scheduleNext(S_Finish, 300);
                return;
            }
            const QString origin = r.value(QStringLiteral("origin")).toString();
            if (r.value(QStringLiteral("stuck")).toBool()) {
                emit log(QStringLiteral("  ✗ 卡在「%1」：%2（剩余病句跳过）")
                             .arg(origin.left(24), r.value(QStringLiteral("why")).toString()));
                scheduleNext(S_Finish, 300);
                return;
            }

            ++m_fixes;
            const int left = r.value(QStringLiteral("left")).toInt();
            if (r.value(QStringLiteral("ignored")).toBool()) {
                emit log(QStringLiteral("  第 %1 处「%2」：没有建议，已忽略（剩 %3）")
                             .arg(QString::number(m_fixes), origin.left(18)).arg(left));
            } else {
                emit log(QStringLiteral("  第 %1 处：%2 → %3（剩 %4%5）")
                             .arg(QString::number(m_fixes),
                                  origin.left(18),
                                  r.value(QStringLiteral("sug")).toString().left(18))
                             .arg(left)
                             .arg(r.value(QStringLiteral("via")).toString() == QLatin1String("direct")
                                      ? QStringLiteral("，直接替换")
                                      : QString()));
            }

            if (left <= 0) {
                scheduleNext(S_FixBlock, 300);      // 这块的红线清完了，回去判断下一步
                return;
            }
            if (m_fixes > m_opt.maxFixes) {
                emit log(QStringLiteral("  超过修改上限 %1，停手（可能建议反复出现）").arg(m_opt.maxFixes));
                scheduleNext(S_Finish, 300);
                return;
            }
            scheduleNext(S_FixOne, 260);
        });
        break;

    case S_Apply:
        applyOneMore();
        break;

    case S_Finish:
        // 草稿模式只做校对，不点保存/发布（客户端本身会自动存草稿）
        if (m_opt.draftMode) {
            emit log(QStringLiteral("  本章校对完成（草稿自动保存），下一章"));
            scheduleNext(S_NextChapter, 500);
            break;
        }
        runJs(QStringLiteral("__yw.finish(%1)").arg(jsStr(m_opt.finishMode)),
              [this, gen](bool ok, const QJsonObject &r) {
                  if (gen != m_gen) return;
                  if (m_opt.finishMode == QLatin1String("none")) {
                      emit log(QStringLiteral("  内容已进客户端（自动保存草稿），不点发布"));
                      scheduleNext(S_NextChapter, 800);
                      return;
                  }
                  if (!ok || !r.value(QStringLiteral("ok")).toBool()) {
                      emit log(QStringLiteral("  收尾点击失败：%1（内容仍在草稿里）")
                                   .arg(r.value(QStringLiteral("why")).toString()));
                      scheduleNext(S_NextChapter, 800);
                      return;
                  }
                  emit log(QStringLiteral("  已点「%1」").arg(r.value(QStringLiteral("text")).toString()));
                  scheduleNext(S_ConfirmPublish, 1500);
              });
        break;

    case S_ConfirmPublish:
        runJs(QStringLiteral("__yw.confirmPublish()"), [this, gen](bool, const QJsonObject &) {
            if (gen != m_gen) return;
            scheduleNext(S_NextChapter, 1500);
        });
        break;

    case S_NextChapter:
        // 失败章已经在 failChapter 里记过分、标过状态，这里别再当成成功记一遍
        if (!m_chapterFailed) {
            emit chapterDone(m_index, true, QString());
            emit chapterStatus(m_index, QStringLiteral("完成"));
            ++m_ok;
            m_consecutiveFails = 0;
        }
        m_chapterFailed = false;
        beginChapter();
        break;

    default:
        break;
    }
}

void UploadWorker::applyOneMore()
{
    const int gen = m_gen;
    runJs(QStringLiteral("__yw.applyOne()"), [this, gen](bool ok, const QJsonObject &r) {
        if (gen != m_gen || !m_running)
            return;
        if (!ok) {
            emit log(QStringLiteral("  点「修改」出错，停止本章校对"));
            scheduleNext(S_Finish, 300);
            return;
        }
        const int left = r.value(QStringLiteral("remaining")).toInt();
        if (r.value(QStringLiteral("done")).toBool() || left <= 0) {
            scheduleNext(S_FixBlock, 400);
            return;
        }
        ++m_fixes;
        if (m_fixes > m_opt.maxFixes) {
            emit log(QStringLiteral("  超过修改上限 %1，停手（可能建议反复出现）").arg(m_opt.maxFixes));
            scheduleNext(S_Finish, 300);
            return;
        }
        if (m_fixes <= 5 || m_fixes % 10 == 0)
            emit log(QStringLiteral("  第 %1 处：%2（剩 %3）")
                         .arg(m_fixes).arg(r.value(QStringLiteral("item")).toString()).arg(left));
        scheduleNext(S_Apply, 700);
    });
}

void UploadWorker::failChapter(const QString &why)
{
    emit log(QStringLiteral("  ✗ 失败：%1").arg(why));
    emit chapterDone(m_index, false, why);
    emit chapterStatus(m_index, QStringLiteral("失败"));
    ++m_fail;
    ++m_consecutiveFails;
    m_chapterFailed = true;
    if (m_consecutiveFails >= 3) {
        emit log(QStringLiteral("连续失败 %1 章，停止运行（检查页面状态后重试）").arg(m_consecutiveFails));
        m_running = false;
        emit finished(m_ok, m_fail);
        return;
    }
    emit log(QStringLiteral("  跳过本章，继续下一章"));
    scheduleNext(S_NextChapter, 800);
}

// ---------------- 基础设施 ----------------

void UploadWorker::runJs(const QString &js, std::function<void(bool, const QJsonObject &)> done)
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

void UploadWorker::poll(const QString &js,
                        std::function<bool(const QJsonObject &)> pred,
                        int intervalMs, int timeoutMs,
                        std::function<void(bool, const QJsonObject &)> done)
{
    const int gen = m_gen;
    const int seq = ++m_pollSeq;                 // 作废上一条轮询的残留 tick
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();

    m_pollLoop = [this, gen, seq, js, pred, intervalMs, timeoutMs, done, elapsed]() {
        if (gen != m_gen || seq != m_pollSeq)
            return;
        runJs(js, [this, gen, seq, pred, intervalMs, timeoutMs, done, elapsed](bool ok, const QJsonObject &o) {
            if (gen != m_gen || seq != m_pollSeq)
                return;
            if (ok && pred(o)) {
                ++m_pollSeq;                     // 结束：让排队的 tick 全部失效
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

// 条件等待：和 poll 同一套失效机制，只是「条件满足」和「等够了」都进入下一步。
// 原来是 QTimer::singleShot(1600) 那种死等，现在最多 120ms 一轮探测。
void UploadWorker::waitThen(int step, const QString &js,
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
         120, capMs,
         [this, gen, step](bool, const QJsonObject &) {
             if (gen != m_gen || !m_running)
                 return;
             scheduleNext(step, 0);
         });
}
