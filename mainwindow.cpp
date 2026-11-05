#include "mainwindow.h"

#include "cdpclient.h"
#include "installpatch.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateEdit>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QTimeEdit>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QStatusBar>
#include <QStyle>
#include <QCloseEvent>

namespace {

QString siteScript()
{
    // 优先读 exe 同目录的 js/site.js（可现场改选择器，不用重编）；
    // 没有就从资源里读，并顺手释放一份出来。
    const QString extPath = QCoreApplication::applicationDirPath() + QStringLiteral("/js/site.js");
    QFile ext(extPath);
    if (ext.open(QIODevice::ReadOnly))
        return QString::fromUtf8(ext.readAll());

    QFile res(QStringLiteral(":/js/site.js"));
    if (!res.open(QIODevice::ReadOnly))
        return QString();

    const QByteArray data = res.readAll();
    QDir().mkpath(QCoreApplication::applicationDirPath() + QStringLiteral("/js"));
    QFile out(extPath);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        out.write(data);
    return QString::fromUtf8(data);
}

QLabel *mutedLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setProperty("role", "muted");
    return l;
}

// 左侧卡片的字段名：固定宽度，让几行输入区的左边缘对齐（纯排版）
QLabel *fieldLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setFixedWidth(60);
    l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return l;
}

// 定时发布排期：第 idx 章（从 0 数）落在「起始时间 + (idx/每天章数) 天 + (idx%每天章数)×间隔分钟」
QDateTime slotTime(const QDateTime &start, int idx, int perDay, int gapMin)
{
    const int per = qMax(1, perDay);
    const int day = idx / per;
    const int slot = idx % per;
    return QDateTime(start.date().addDays(day),
                     start.time().addSecs(qint64(slot) * qMax(0, gapMin) * 60));
}

// chapterStore 的 status：1=草稿 2=已发布 5=已定时 7=回收站
QString statusLabel(const QString &st)
{
    if (st == QLatin1String("1")) return QStringLiteral("草稿");
    if (st == QLatin1String("2")) return QStringLiteral("已发布");
    if (st == QLatin1String("5")) return QStringLiteral("已定时发布");
    if (st == QLatin1String("7")) return QStringLiteral("回收站");
    return st.isEmpty() ? QStringLiteral("目录里没有") : QStringLiteral("状态 %1").arg(st);
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("起点作家助手 · 章节自动上传"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
    resize(1280, 820);
    setMinimumSize(1040, 680);

    m_cdp    = new CdpClient(this);
    m_worker = new UploadWorker(m_cdp, this);
    m_pub    = new PublishWorker(m_cdp, this);

    // 日志批量刷新：跑校对时一秒可能蹦十几行，逐条 appendPlainText 会让控件反复重排，
    // 攒 120ms 一批再插，界面和状态机都不被日志拖慢（内容、顺序完全不变）
    m_logTimer = new QTimer(this);
    m_logTimer->setSingleShot(true);
    m_logTimer->setInterval(120);
    connect(m_logTimer, &QTimer::timeout, this, &MainWindow::flushLog);

    auto *central = new QWidget;
    central->setObjectName(QStringLiteral("root"));
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(18, 14, 18, 10);
    root->setSpacing(12);

    // 品牌头：纯装饰，不含任何逻辑
    auto *header = new QWidget;
    header->setObjectName(QStringLiteral("appHeader"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(16, 9, 14, 9);
    hl->setSpacing(10);
    auto *logo = new QLabel;
    logo->setObjectName(QStringLiteral("appLogo"));
    logo->setPixmap(QIcon(QStringLiteral(":/icons/app-mark.png")).pixmap(24, 24));
    logo->setFixedSize(24, 24);
    hl->addWidget(logo);
    auto *appTitle = new QLabel(QStringLiteral("章节自动上传 & 草稿校对"));
    appTitle->setObjectName(QStringLiteral("appTitle"));
    hl->addWidget(appTitle);
    hl->addStretch();
    auto *appSub = new QLabel(QStringLiteral("起点作家助手 · DevTools 驱动"));
    appSub->setObjectName(QStringLiteral("appSub"));
    hl->addWidget(appSub);
    hl->addSpacing(4);
    auto *ver = new QLabel(QStringLiteral("v1.1.0"));
    ver->setObjectName(QStringLiteral("verChip"));
    hl->addWidget(ver);
    root->addWidget(header);

    root->addWidget(buildTopBar());

    auto *split = new QSplitter(Qt::Horizontal);
    split->addWidget(buildLeftPanel());
    split->addWidget(buildRightPanel());
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    root->addWidget(split, 1);

    setCentralWidget(central);

    // 卡片投影：纯观感。QSS 画不了阴影，用 effect 实现（只影响绘制，不影响任何行为）
    for (QFrame *card : findChildren<QFrame *>()) {
        if (card->objectName() != QLatin1String("card"))
            continue;
        auto *fx = new QGraphicsDropShadowEffect(card);
        fx->setBlurRadius(22);
        fx->setOffset(0, 4);
        fx->setColor(QColor(15, 23, 42, 34));
        card->setGraphicsEffect(fx);
    }

    statusBar()->showMessage(QStringLiteral("就绪"));

    // ---- 信号 ----
    connect(m_cdp, &CdpClient::log, this, &MainWindow::log);
    connect(m_cdp, &CdpClient::failed, this, [this](const QString &why) {
        log(QStringLiteral("✗ %1").arg(why));
        setConnUi(false, QStringLiteral("未连接"));
    });
    connect(m_cdp, &CdpClient::disconnected, this, [this]() {
        setConnUi(false, QStringLiteral("未连接"));
    });
    connect(m_cdp, &CdpClient::connected, this, [this](const QString &url, const QString &title) {
        setConnUi(true, QStringLiteral("已连接 · %1").arg(title));
        log(QStringLiteral("页面：%1").arg(url));
        injectSiteScript();
        // 停在书架页时顺手把作品名读进下拉（静默，不切页面）
        QTimer::singleShot(1500, this, [this]() { refreshBooksImpl(true); });
        // 用户之前点了「进入作品」但还没连接 → 连上后自动进书
        if (!m_pendingBook.isEmpty()) {
            const QString b = m_pendingBook;
            m_pendingBook.clear();
            QTimer::singleShot(1800, this, [this, b]() {
                m_bookName->setEditText(b);
                enterBook();
            });
        }
    });

    connect(m_worker, &UploadWorker::log, this, &MainWindow::log);
    connect(m_worker, &UploadWorker::chapterStatus, this, [this](int index, const QString &status) {
        const int row = m_rowOffset + index;
        if (row >= 0 && row < m_table->rowCount())
            m_table->item(row, 3)->setText(status);
    });
    connect(m_worker, &UploadWorker::chapterDone, this, [this](int index, bool ok, const QString &) {
        const int row = m_rowOffset + index;
        if (row >= 0 && row < m_table->rowCount()) {
            m_table->item(row, 3)->setText(ok ? QStringLiteral("完成") : QStringLiteral("失败"));
            m_table->selectRow(row);
        }
        m_progress->setValue(m_progress->value() + 1);
        statusBar()->showMessage(QStringLiteral("进度 %1 / %2")
                                     .arg(m_progress->value()).arg(m_progress->maximum()));
    });
    connect(m_worker, &UploadWorker::finished, this, [this](int ok, int fail) {
        m_running = false;
        updateRunUi();
        log(QStringLiteral("=== 结束：成功 %1，失败 %2 ===").arg(ok).arg(fail));
        flushLog();                       // 收尾这一行立刻可见，不用等批量刷新
        if (ok > 0 && fail == 0) {
            // 记录进度，下次默认从这里继续
            QSettings s;
            const Chapter &last = m_subset.isEmpty() ? Chapter() : m_subset.last();
            if (last.no > 0)
                s.setValue(QStringLiteral("progress/%1").arg(m_store.chapters().isEmpty()
                                                             ? QString() : m_srcDir->text()), last.no);
        }
    });

    // ---- 定时发布 ----
    connect(m_pub, &PublishWorker::log, this, &MainWindow::log);
    connect(m_pub, &PublishWorker::itemStatus, this, [this](int index, const QString &status) {
        const int row = index;                       // 发布模式的表格就是按计划重建的，行号即下标
        if (row >= 0 && row < m_table->rowCount() && m_table->item(row, 3))
            m_table->item(row, 3)->setText(status);
    });
    connect(m_pub, &PublishWorker::itemDone, this, [this](int index, bool ok, const QString &) {
        const int row = index;
        if (row >= 0 && row < m_table->rowCount() && m_table->item(row, 3)) {
            m_table->item(row, 3)->setText(ok ? QStringLiteral("已定时") : QStringLiteral("失败"));
            m_table->selectRow(row);
        }
        m_progress->setValue(m_progress->value() + 1);
        statusBar()->showMessage(QStringLiteral("进度 %1 / %2")
                                     .arg(m_progress->value()).arg(m_progress->maximum()));
    });
    connect(m_pub, &PublishWorker::finished, this, [this](int ok, int fail) {
        m_running = false;
        updateRunUi();
        log(QStringLiteral("=== 结束：成功 %1，失败 %2 ===").arg(ok).arg(fail));
        flushLog();
    });

    m_autoConnect = new QTimer(this);
    m_autoConnect->setInterval(2500);
    connect(m_autoConnect, &QTimer::timeout, this, [this]() {
        if (m_connected || ++m_autoTry > 24) {
            m_autoConnect->stop();
            if (!m_connected)
                log(QStringLiteral("自动连接超时（60 秒）。客户端里登录/进到写作页后，再手动点「连接页面」。"));
            return;
        }
        m_cdp->discoverAndConnect();
    });

    // 点开作品后轮询：等客户端把写作页标签拉起来再切过去
    m_bookPoll = new QTimer(this);
    m_bookPoll->setInterval(1500);
    connect(m_bookPoll, &QTimer::timeout, this, [this]() {
        if (!m_connected || ++m_bookPollTry > 20) {
            m_bookPoll->stop();
            m_bookBtn->setEnabled(true);
            if (m_bookPollTry > 20)
                log(QStringLiteral("✗ 30 秒还没出现这本书的写作页（客户端里可能还停在书架）。"));
            return;
        }
        m_cdp->switchToChapterTarget([this](bool ok, const QString &) {
            if (!ok)
                return;                       // 标签还没出现，等下一次
            m_bookPoll->stop();
            m_autoConnect->stop();
            log(QStringLiteral("✓ 已进入「%1」的写作页").arg(m_bookName->currentText().trimmed()));
            QTimer::singleShot(900, this, [this]() {
                injectSiteScript();
                m_bookBtn->setEnabled(true);
            });
        });
    });

    loadSettings();
    updateModeUi();

    // 首次启动自动尝试找一下客户端安装位置
    if (m_exePath->text().isEmpty()) {
        const QString dir = InstallPatch::defaultProgramDir();
        if (!dir.isEmpty())
            m_exePath->setText(InstallPatch::exePath(dir));
    }

    updateRunUi();
    log(QStringLiteral("提示：先点右边「启用调试并启动客户端」，等客户端打开、登录后点「连接」。"));

    // ---- 无人值守自测：CU_SRC 指定目录、CU_SELFTEST=秒数 截图后退出 ----
    if (!qEnvironmentVariable("CU_SRC").isEmpty()) {
        m_srcDir->setText(qEnvironmentVariable("CU_SRC"));
        reloadChapters();
    }
    const int selfTest = qEnvironmentVariableIntValue("CU_SELFTEST");
    if (selfTest > 0) {
        // 自测时可以用 CU_MODE=draft / upload 直接切模式截图
        if (qEnvironmentVariable("CU_MODE") == QLatin1String("draft"))
            m_mode->setCurrentIndex(1);
        else if (qEnvironmentVariable("CU_MODE") == QLatin1String("upload"))
            m_mode->setCurrentIndex(0);
        else if (qEnvironmentVariable("CU_MODE") == QLatin1String("publish"))
            m_mode->setCurrentIndex(2);
        if (!qEnvironmentVariable("CU_FROM").isEmpty() || !qEnvironmentVariable("CU_TO").isEmpty()) {
            m_from->setValue(qEnvironmentVariableIntValue("CU_FROM"));
            m_to->setValue(qEnvironmentVariableIntValue("CU_TO"));
        }
        const QString shot = qEnvironmentVariable("CU_SHOT");
        if (!shot.isEmpty()) {
            QTimer::singleShot(selfTest * 1000, this, [this, shot]() {
                grab().save(shot);
            });
        }
        QTimer::singleShot(selfTest * 1000 + 600, qApp, &QCoreApplication::quit);
    }
}

// ---------------- 界面 ----------------

QWidget *MainWindow::buildTopBar()
{
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("card"));
    auto *row = new QHBoxLayout(card);
    row->setContentsMargins(14, 10, 14, 10);
    row->setSpacing(10);

    row->addWidget(new QLabel(QStringLiteral("客户端")));
    m_exePath = new QLineEdit;
    m_exePath->setPlaceholderText(QStringLiteral("yuewenedit.exe 的路径"));
    row->addWidget(m_exePath, 1);

    auto *browse = new QPushButton(QStringLiteral("选择…"));
    connect(browse, &QPushButton::clicked, this, &MainWindow::chooseClientExe);
    row->addWidget(browse);

    m_launchBtn = new QPushButton(QStringLiteral("启用调试并启动客户端"));
    m_launchBtn->setObjectName(QStringLiteral("primary"));
    connect(m_launchBtn, &QPushButton::clicked, this, &MainWindow::enableAndLaunchClient);
    row->addWidget(m_launchBtn);

    auto *revert = new QPushButton(QStringLiteral("还原客户端"));
    revert->setToolTip(QStringLiteral("撤掉调试端口补丁，恢复客户端原始入口文件"));
    connect(revert, &QPushButton::clicked, this, &MainWindow::revertClientPatch);
    row->addWidget(revert);

    m_connDot = new QLabel(QStringLiteral("●"));
    m_connDot->setObjectName(QStringLiteral("connDot"));
    row->addWidget(m_connDot);
    m_connText = new QLabel(QStringLiteral("未连接"));
    m_connText->setProperty("role", "muted");
    row->addWidget(m_connText);

    m_connectBtn = new QPushButton(QStringLiteral("连接页面"));
    connect(m_connectBtn, &QPushButton::clicked, this, &MainWindow::toggleConnect);
    row->addWidget(m_connectBtn);

    return card;
}

QWidget *MainWindow::buildLeftPanel()
{
    auto *wrap = new QWidget;
    auto *v = new QVBoxLayout(wrap);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);

    // 作品（书名）：进书入口，候选来自客户端书架
    auto *bookCard = new QFrame;
    bookCard->setObjectName(QStringLiteral("card"));
    auto *bookRow = new QHBoxLayout(bookCard);
    bookRow->setContentsMargins(14, 10, 14, 10);
    bookRow->setSpacing(8);
    bookRow->addWidget(fieldLabel(QStringLiteral("作品")));
    m_bookName = new QComboBox;
    m_bookName->setEditable(true);
    m_bookName->setInsertPolicy(QComboBox::NoInsert);
    m_bookName->setMinimumWidth(220);
    m_bookName->setToolTip(QStringLiteral("填书名（或连接后点「刷新列表」从书架选），\n点「进入作品」直接在客户端里打开这本书的写作页。"));
    if (m_bookName->lineEdit())
        m_bookName->lineEdit()->setPlaceholderText(
            QStringLiteral("输入书名，或连接后点「刷新列表」选择"));
    bookRow->addWidget(m_bookName, 1);

    auto *refreshBooksBtn = new QPushButton(QStringLiteral("刷新列表"));
    refreshBooksBtn->setToolTip(QStringLiteral("从客户端书架读取作品名；当前不在书架页会自动切过去读"));
    connect(refreshBooksBtn, &QPushButton::clicked, this, &MainWindow::refreshBooks);
    bookRow->addWidget(refreshBooksBtn);

    m_bookBtn = new QPushButton(QStringLiteral("进入作品"));
    m_bookBtn->setObjectName(QStringLiteral("primary"));
    m_bookBtn->setToolTip(QStringLiteral("在客户端里点开这本书，切到它的写作页"));
    connect(m_bookBtn, &QPushButton::clicked, this, &MainWindow::enterBook);
    bookRow->addWidget(m_bookBtn);

    v->addWidget(bookCard);

    // 源目录
    m_dirCard = new QFrame;
    m_dirCard->setObjectName(QStringLiteral("card"));
    auto *dirCard = m_dirCard;
    auto *dirRow = new QHBoxLayout(dirCard);
    dirRow->setContentsMargins(14, 10, 14, 10);
    dirRow->setSpacing(8);
    dirRow->addWidget(fieldLabel(QStringLiteral("章节目录")));
    m_srcDir = new QLineEdit;
    m_srcDir->setPlaceholderText(QStringLiteral("分章 txt 所在目录"));
    dirRow->addWidget(m_srcDir, 1);
    auto *pick = new QPushButton(QStringLiteral("浏览…"));
    connect(pick, &QPushButton::clicked, this, &MainWindow::chooseSourceDir);
    dirRow->addWidget(pick);
    auto *reload = new QPushButton(QStringLiteral("重新载入"));
    connect(reload, &QPushButton::clicked, this, &MainWindow::reloadChapters);
    dirRow->addWidget(reload);
    v->addWidget(dirCard);

    // 区间 + 选项
    auto *optCard = new QFrame;
    optCard->setObjectName(QStringLiteral("card"));
    auto *optV = new QVBoxLayout(optCard);
    optV->setContentsMargins(14, 10, 14, 10);
    optV->setSpacing(8);

    auto *rangeRow = new QHBoxLayout;
    rangeRow->setSpacing(8);
    rangeRow->addWidget(fieldLabel(QStringLiteral("模式")));
    m_mode = new QComboBox;
    m_mode->setObjectName(QStringLiteral("modeCombo"));   // QSS 里做成蓝底强调样式
    m_mode->addItem(QStringLiteral("上传新章"), QStringLiteral("upload"));
    m_mode->addItem(QStringLiteral("校对已有草稿"), QStringLiteral("draft"));
    m_mode->addItem(QStringLiteral("定时发布草稿"), QStringLiteral("publish"));
    m_mode->setToolTip(QStringLiteral("上传新章：从本地分章 txt 新建章节并写入。\n"
                                      "校对已有草稿：不动标题正文，只把侧栏里已有的章节逐章点开、\n"
                                      "跑「错别字 → 病句」检测并把建议逐条改掉。\n"
                                      "定时发布草稿：进入作品的「草稿」列表，按区间逐章点「发布」、\n"
                                      "勾「定时发布」并填好日期时间，最后点「定时发布」。"));
    m_mode->setMinimumWidth(150);
    rangeRow->addWidget(m_mode);
    rangeRow->addSpacing(10);
    m_rangeTag = new QLabel(QStringLiteral("上传范围：第"));
    rangeRow->addWidget(m_rangeTag);
    m_from = new QSpinBox;
    m_from->setRange(1, 99999);
    rangeRow->addWidget(m_from);
    rangeRow->addWidget(new QLabel(QStringLiteral("章 到 第")));
    m_to = new QSpinBox;
    m_to->setRange(1, 99999);
    rangeRow->addWidget(m_to);
    rangeRow->addWidget(new QLabel(QStringLiteral("章")));
    rangeRow->addStretch();
    optV->addLayout(rangeRow);

    // 第二行：校对 / 跳过 + 收尾动作（挤在第一行会把「校对并逐条修改病句」压断）
    auto *checkRow = new QHBoxLayout;
    checkRow->setSpacing(14);
    m_proofread = new QCheckBox(QStringLiteral("校对并逐条修改病句"));
    m_proofread->setChecked(true);
    checkRow->addWidget(m_proofread);
    m_skipExisting = new QCheckBox(QStringLiteral("跳过已存在章节"));
    m_skipExisting->setChecked(true);
    m_skipExisting->setToolTip(QStringLiteral("侧栏里已经有「第N章」时跳过，防止重跑生成重复章节"));
    checkRow->addWidget(m_skipExisting);
    checkRow->addStretch();
    checkRow->addWidget(new QLabel(QStringLiteral("收尾动作：")));
    m_finishMode = new QComboBox;
    m_finishMode->addItem(QStringLiteral("不点按钮（草稿自动保存）"), QStringLiteral("none"));
    m_finishMode->addItem(QStringLiteral("点「保存」"), QStringLiteral("save"));
    m_finishMode->addItem(QStringLiteral("点「发布」"), QStringLiteral("publish"));
    m_finishMode->setMinimumWidth(190);
    checkRow->addWidget(m_finishMode);
    optV->addLayout(checkRow);

    // 第三行：标题选项
    auto *optRow = new QHBoxLayout;
    optRow->setSpacing(14);
    m_titleWithNo = new QCheckBox(QStringLiteral("标题加「第N章」"));
    m_titleWithNo->setChecked(true);
    optRow->addWidget(m_titleWithNo);
    m_titleUseStem = new QCheckBox(QStringLiteral("标题用文件名（含编号）"));
    m_titleUseStem->setChecked(true);
    optRow->addWidget(m_titleUseStem);
    optRow->addStretch();
    optV->addLayout(optRow);

    v->addWidget(optCard);

    // ---- 定时发布排期（只在「定时发布草稿」模式可用）----
    m_schedCard = new QFrame;
    m_schedCard->setObjectName(QStringLiteral("card"));
    auto *scV = new QVBoxLayout(m_schedCard);
    scV->setContentsMargins(14, 10, 14, 10);
    scV->setSpacing(8);

    auto *schedRow = new QHBoxLayout;
    schedRow->setSpacing(8);
    schedRow->addWidget(fieldLabel(QStringLiteral("起始")));
    m_startDate = new QDateEdit(QDate::currentDate());
    m_startDate->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_startDate->setCalendarPopup(true);
    m_startDate->setToolTip(QStringLiteral("第一章的发布日期（格式 2026-11-06）。\n"
                                           "如果这个时间点已经过去，会自动顺延到第二天同一时间。"));
    schedRow->addWidget(m_startDate);
    m_startTime = new QTimeEdit(QTime(19, 0));
    m_startTime->setDisplayFormat(QStringLiteral("HH:mm"));
    m_startTime->setToolTip(QStringLiteral("每天第一本的发布时间（格式 19:00）"));
    schedRow->addWidget(m_startTime);

    schedRow->addSpacing(10);
    schedRow->addWidget(new QLabel(QStringLiteral("每天")));
    m_perDay = new QSpinBox;
    m_perDay->setRange(1, 20);
    m_perDay->setValue(3);
    m_perDay->setSuffix(QStringLiteral(" 章"));
    m_perDay->setToolTip(QStringLiteral("每天发几章（默认 3 章）"));
    schedRow->addWidget(m_perDay);
    schedRow->addWidget(new QLabel(QStringLiteral("间隔")));
    m_gapMin = new QSpinBox;
    m_gapMin->setRange(0, 240);
    m_gapMin->setValue(5);
    m_gapMin->setSuffix(QStringLiteral(" 分钟"));
    m_gapMin->setToolTip(QStringLiteral("同一天相邻两章隔几分钟（默认 5 分钟）"));
    schedRow->addWidget(m_gapMin);
    schedRow->addStretch();
    scV->addLayout(schedRow);

    auto *schedRow2 = new QHBoxLayout;
    schedRow2->setSpacing(10);
    m_dryRun = new QCheckBox(QStringLiteral("演练：只填表不提交"));
    m_dryRun->setToolTip(QStringLiteral("逐章打开发布弹窗、勾定时发布、选好日期时间，然后关掉不点「定时发布」。\n"
                                        "第一次用建议先演练一遍，确认日志里的日期时间和预期一致。"));
    schedRow2->addWidget(m_dryRun);
    m_schedHint = new QLabel;
    m_schedHint->setProperty("role", "muted");
    m_schedHint->setWordWrap(true);
    schedRow2->addWidget(m_schedHint, 1);
    scV->addLayout(schedRow2);

    v->addWidget(m_schedCard);

    connect(m_mode, &QComboBox::currentIndexChanged, this, [this]() {
        updateModeUi();
        refreshRangeBounds();
        rebuildTable();
    });
    // 草稿模式下改区间就实时重建表格，一眼能看到要跑哪些章
    connect(m_from, &QSpinBox::valueChanged, this, [this]() {
        if (isDraftMode() || isPublishMode()) rebuildTable();
    });
    connect(m_to, &QSpinBox::valueChanged, this, [this]() {
        if (isDraftMode() || isPublishMode()) rebuildTable();
    });
    // 排期参数一改，预览表跟着变
    for (QSpinBox *sp : {m_perDay, m_gapMin}) {
        connect(sp, &QSpinBox::valueChanged, this, [this]() {
            refreshScheduleHint();
            if (isPublishMode()) rebuildTable();
        });
    }
    connect(m_startDate, &QDateEdit::dateChanged, this, [this]() {
        refreshScheduleHint();
        if (isPublishMode()) rebuildTable();
    });
    connect(m_startTime, &QTimeEdit::timeChanged, this, [this]() {
        refreshScheduleHint();
        if (isPublishMode()) rebuildTable();
    });

    // 章节表
    m_table = new QTableWidget(0, 4);
    m_table->setHorizontalHeaderLabels({QStringLiteral("编号"), QStringLiteral("标题"),
                                        QStringLiteral("字数"), QStringLiteral("状态")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(28);   // 行高松一点，不挤
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    v->addWidget(m_table, 1);

    // 运行控制
    auto *runRow = new QHBoxLayout;
    runRow->setSpacing(10);
    m_startBtn = new QPushButton(QStringLiteral("开始上传"));
    m_startBtn->setObjectName(QStringLiteral("primary"));
    m_startBtn->setMinimumHeight(36);
    m_startBtn->setMinimumWidth(110);
    connect(m_startBtn, &QPushButton::clicked, this, &MainWindow::startRun);
    runRow->addWidget(m_startBtn);

    m_stopBtn = new QPushButton(QStringLiteral("停止"));
    m_stopBtn->setObjectName(QStringLiteral("danger"));
    m_stopBtn->setMinimumHeight(36);
    connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::stopRun);
    runRow->addWidget(m_stopBtn);

    m_progress = new QProgressBar;
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    m_progress->setMinimumHeight(28);   // 和左右两个按钮视觉齐平
    runRow->addWidget(m_progress, 1);
    v->addLayout(runRow);

    return wrap;
}

QWidget *MainWindow::buildRightPanel()
{
    auto *wrap = new QWidget;
    auto *v = new QVBoxLayout(wrap);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);

    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("card"));
    auto *cv = new QVBoxLayout(card);
    cv->setContentsMargins(12, 10, 12, 10);
    cv->setSpacing(8);

    auto *head = new QHBoxLayout;
    auto *t = new QLabel(QStringLiteral("运行日志"));
    t->setObjectName(QStringLiteral("sectionTitle"));
    head->addWidget(t);
    head->addStretch();
    auto *dumpBtn = new QPushButton(QStringLiteral("抓取页面结构"));
    connect(dumpBtn, &QPushButton::clicked, this, &MainWindow::dumpPage);
    head->addWidget(dumpBtn);
    auto *jsBtn = new QPushButton(QStringLiteral("执行 JS…"));
    connect(jsBtn, &QPushButton::clicked, this, &MainWindow::runCustomJs);
    head->addWidget(jsBtn);
    auto *clearBtn = new QPushButton(QStringLiteral("清空"));
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        if (m_logTimer) m_logTimer->stop();
        m_logBuf.clear();
        m_log->clear();
    });
    head->addWidget(clearBtn);
    cv->addLayout(head);

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setUndoRedoEnabled(false);     // 只读日志用不到撤销栈，开着白吃内存（长跑几小时差别明显）
    m_log->setMaximumBlockCount(4000);    // 只留最近 4000 行：控件不会越跑越慢
    m_log->setObjectName(QStringLiteral("log"));
    cv->addWidget(m_log, 1);

    v->addWidget(card, 1);
    return wrap;
}

// ---------------- 动作 ----------------

void MainWindow::chooseSourceDir()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择分章 txt 目录"), m_srcDir->text());
    if (dir.isEmpty())
        return;
    m_srcDir->setText(dir);
    reloadChapters();
}

void MainWindow::reloadChapters()
{
    QString err;
    if (!m_store.scan(m_srcDir->text(), &err)) {
        log(QStringLiteral("✗ %1").arg(err));
        m_table->setRowCount(0);
        return;
    }

    refreshRangeBounds();
    rebuildTable();              // 里面会按模式决定填本地 txt 还是区间
    const auto &list = m_store.chapters();
    log(QStringLiteral("载入 %1 章（第 %2 ~ %3 章）")
            .arg(list.size()).arg(list.first().no).arg(list.last().no));
}

// 表格按当前模式重建：草稿模式列「区间里的每个章节号」，上传模式列本地 txt，
// 发布模式列「区间 + 各自排到的时间」
void MainWindow::rebuildTable()
{
    m_table->setRowCount(0);
    applyTableHeaders();

    if (isDraftMode()) {
        const int lo = m_from->value();
        const int hi = m_to->value();
        if (lo > hi)
            return;
        const int n = hi - lo + 1;
        m_table->setRowCount(n);
        for (int i = 0; i < n; ++i) {
            m_table->setItem(i, 0, new QTableWidgetItem(QString::number(lo + i)));
            m_table->setItem(i, 1, new QTableWidgetItem(QStringLiteral("（客户端里的草稿）")));
            m_table->setItem(i, 2, new QTableWidgetItem(QStringLiteral("—")));
            m_table->setItem(i, 3, new QTableWidgetItem(QStringLiteral("待校对")));
        }
        return;
    }

    if (isPublishMode()) {
        // 预览：按区间排出时间（真正开始时还会去掉已发布/已定时的章，时间随之重排）
        const int lo = m_from->value();
        const int hi = m_to->value();
        if (lo > hi)
            return;
        const QDateTime start = scheduleStart();
        const int per = m_perDay->value();
        const int gap = m_gapMin->value();
        const int n = hi - lo + 1;
        m_table->setRowCount(n);
        for (int i = 0; i < n; ++i) {
            const QDateTime when = slotTime(start, i, per, gap);
            m_table->setItem(i, 0, new QTableWidgetItem(QString::number(lo + i)));
            m_table->setItem(i, 1, new QTableWidgetItem(when.toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
            m_table->setItem(i, 2, new QTableWidgetItem(QStringLiteral("（草稿）")));
            m_table->setItem(i, 3, new QTableWidgetItem(QStringLiteral("待发布")));
        }
        refreshScheduleHint();
        return;
    }

    if (m_store.count() == 0)
        return;
    const auto &list = m_store.chapters();
    m_table->setRowCount(list.size());
    for (int i = 0; i < list.size(); ++i) {
        const Chapter &c = list[i];
        m_table->setItem(i, 0, new QTableWidgetItem(QString::number(c.no)));
        m_table->setItem(i, 1, new QTableWidgetItem(c.title(true, true)));
        m_table->setItem(i, 2, new QTableWidgetItem(QString::number(c.chars)));
        m_table->setItem(i, 3, new QTableWidgetItem(QStringLiteral("待上传")));
    }
}

// 发布模式第二列是「定时时间」，其他模式是标题
void MainWindow::applyTableHeaders()
{
    auto *h = m_table->horizontalHeader();
    if (isPublishMode()) {
        m_table->setHorizontalHeaderLabels({QStringLiteral("编号"), QStringLiteral("定时时间"),
                                            QStringLiteral("标题"), QStringLiteral("状态")});
        h->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        h->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        h->setSectionResizeMode(2, QHeaderView::Stretch);
        h->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        return;
    }
    m_table->setHorizontalHeaderLabels({QStringLiteral("编号"), QStringLiteral("标题"),
                                        QStringLiteral("字数"), QStringLiteral("状态")});
    h->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    h->setSectionResizeMode(1, QHeaderView::Stretch);
    h->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    h->setSectionResizeMode(3, QHeaderView::ResizeToContents);
}

bool MainWindow::isDraftMode() const
{
    return m_mode && m_mode->currentData().toString() == QLatin1String("draft");
}

bool MainWindow::isPublishMode() const
{
    return m_mode && m_mode->currentData().toString() == QLatin1String("publish");
}

// 起始时间已经过去就顺延到第二天同一时间：平台不接受过去的时间点
QDateTime MainWindow::scheduleStart() const
{
    QDateTime start(m_startDate->date(), m_startTime->time());
    const QDateTime now = QDateTime::currentDateTime();
    while (start <= now.addSecs(60))
        start = start.addDays(1);
    return start;
}

void MainWindow::refreshScheduleHint()
{
    if (!m_schedHint || !m_perDay || !m_gapMin || !m_from || !m_to)
        return;
    if (!isPublishMode()) {
        m_schedHint->setText(QStringLiteral("仅「定时发布草稿」模式使用"));
        return;
    }
    const int lo = m_from->value();
    const int hi = m_to->value();
    if (lo > hi) {
        m_schedHint->setText(QStringLiteral("起止章号反了"));
        return;
    }
    const QDateTime start = scheduleStart();
    const int n = hi - lo + 1;
    const QDateTime last = slotTime(start, n - 1, m_perDay->value(), m_gapMin->value());
    const int days = (n + m_perDay->value() - 1) / qMax(1, m_perDay->value());

    QStringList day1;
    const int per = m_perDay->value();
    for (int i = 0; i < per; ++i)
        day1 << slotTime(start, i, per, m_gapMin->value()).toString(QStringLiteral("HH:mm"));

    QString txt = QStringLiteral("共 %1 章 · 每天 %2 章（%3）· %4 天发完 · 最后一章 %5")
                      .arg(n).arg(per).arg(day1.join(QStringLiteral(" / ")))
                      .arg(days)
                      .arg(last.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    if (QDateTime(QDate(m_startDate->date()), m_startTime->time()) <= QDateTime::currentDateTime())
        txt += QStringLiteral("　·　起始时间已过，自动顺延到 %1")
                   .arg(start.toString(QStringLiteral("yyyy-MM-dd")));
    m_schedHint->setText(txt);
}

// 三种模式能改的选项不一样：草稿模式不动标题/正文，发布模式只控制排期
void MainWindow::updateModeUi()
{
    const bool draft = isDraftMode();
    const bool publish = isPublishMode();
    const bool idle  = !m_running;
    const bool plain = !draft && !publish;        // 上传新章

    if (m_rangeTag) {
        m_rangeTag->setText(draft    ? QStringLiteral("校对范围：第")
                            : publish ? QStringLiteral("发布范围：第")
                                      : QStringLiteral("上传范围：第"));
    }
    if (m_dirCard)
        m_dirCard->setEnabled(plain && idle);
    if (m_proofread) {
        if (draft)
            m_proofread->setChecked(true);      // 草稿模式必校对
        m_proofread->setEnabled(plain && idle);
    }
    if (m_skipExisting)   m_skipExisting->setEnabled(plain && idle);
    if (m_titleWithNo)    m_titleWithNo->setEnabled(plain && idle);
    if (m_titleUseStem)   m_titleUseStem->setEnabled(plain && idle);
    if (m_finishMode)     m_finishMode->setEnabled(plain && idle);
    if (m_mode)           m_mode->setEnabled(idle);

    if (m_schedCard)      m_schedCard->setEnabled(publish && idle);
    for (QWidget *w : {static_cast<QWidget *>(m_startDate), static_cast<QWidget *>(m_startTime),
                       static_cast<QWidget *>(m_perDay), static_cast<QWidget *>(m_gapMin),
                       static_cast<QWidget *>(m_dryRun)}) {
        if (w) w->setEnabled(publish && idle);
    }
    refreshScheduleHint();

    if (m_startBtn)
        m_startBtn->setText(draft   ? QStringLiteral("开始校对草稿")
                            : publish ? QStringLiteral("开始定时发布")
                                      : QStringLiteral("开始上传"));
}

void MainWindow::refreshRangeBounds()
{
    if (isDraftMode() || isPublishMode()) {   // 这两种模式不受本地目录限制，范围自己填
        m_from->setRange(1, 99999);
        m_to->setRange(1, 99999);
        return;
    }
    if (m_store.count() == 0) {
        m_from->setRange(1, 99999);
        m_to->setRange(1, 99999);
        return;
    }
    const int lo = m_store.chapters().first().no;
    const int hi = m_store.chapters().last().no;
    m_from->setRange(lo, hi);
    m_to->setRange(lo, hi);

    // 有记录就从下一章开始
    QSettings s;
    const int last = s.value(QStringLiteral("progress/%1").arg(m_srcDir->text()), 0).toInt();
    if (last >= lo && last < hi) {
        m_from->setValue(last + 1);
        m_to->setValue(hi);
        log(QStringLiteral("检测到上次上传到第 %1 章，默认从第 %2 章继续").arg(last).arg(last + 1));
    } else {
        m_from->setValue(lo);
        m_to->setValue(hi);
    }
}

void MainWindow::chooseClientExe()
{
    const QString exe = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 yuewenedit.exe"), m_exePath->text(),
        QStringLiteral("作家助手 (yuewenedit.exe)"));
    if (exe.isEmpty())
        return;
    m_exePath->setText(QDir::toNativeSeparators(exe));
    QSettings().setValue(QStringLiteral("client/programDir"), QFileInfo(exe).absolutePath());
}

void MainWindow::enableAndLaunchClient()
{
    const QString exe = m_exePath->text().trimmed();
    if (exe.isEmpty() || !QFileInfo::exists(exe)) {
        QMessageBox::warning(this, QStringLiteral("没选客户端"),
                             QStringLiteral("先点「选择…」指定 yuewenedit.exe。"));
        return;
    }
    const QString programDir = QFileInfo(exe).absolutePath();

    // 客户端是单实例的，已经开着就没法带调试端口重启
    QProcess probe;
    probe.start(QStringLiteral("tasklist"), {QStringLiteral("/FI"), QStringLiteral("IMAGENAME eq yuewenedit.exe")});
    probe.waitForFinished(4000);
    const QString out = QString::fromLocal8Bit(probe.readAllStandardOutput());
    if (out.contains(QStringLiteral("yuewenedit.exe"))) {
        QMessageBox::information(
            this, QStringLiteral("客户端正在运行"),
            QStringLiteral("作家助手已经在运行了。它带单实例锁，必须先完全退出（任务栏右下角也要退），"
                           "再由这里启动才会带上调试端口。\n\n退出后回来点这个按钮就行。"));
        return;
    }

    if (!InstallPatch::isPatched(programDir)) {
        const auto ret = QMessageBox::question(
            this, QStringLiteral("要改客户端的一个入口文件"),
            QStringLiteral("需要在客户端目录写两个文件：\n"
                           "  resources/app/cdp-main.js（新增，只加调试端口开关）\n"
                           "  resources/app/package.json（main 指向它，原文件备份为 package.json.ywbak）\n\n"
                           "随时可以点「还原客户端」恢复。继续吗？"));
        if (ret != QMessageBox::Yes)
            return;
        QString err;
        if (!InstallPatch::enable(programDir, &err)) {
            QMessageBox::critical(this, QStringLiteral("打补丁失败"), err);
            return;
        }
        log(QStringLiteral("已给客户端开调试端口：%1").arg(programDir));
    }

    QSettings().setValue(QStringLiteral("client/programDir"), programDir);
    if (!QProcess::startDetached(exe, {}, programDir)) {
        QMessageBox::critical(this, QStringLiteral("启动失败"), QStringLiteral("起不来：%1").arg(exe));
        return;
    }
    log(QStringLiteral("已启动客户端（调试端口 9222），会自动尝试连接 60 秒…"));
    statusBar()->showMessage(QStringLiteral("等待客户端启动…"));
    m_autoTry = 0;
    m_autoConnect->start();
}

void MainWindow::toggleConnect()
{
    if (m_connected) {
        m_cdp->close();
        setConnUi(false, QStringLiteral("未连接"));
        m_connectBtn->setText(QStringLiteral("连接页面"));
        return;
    }
    m_connectBtn->setEnabled(false);
    log(QStringLiteral("正在连接 127.0.0.1:9222 …"));
    m_cdp->discoverAndConnect();
    QTimer::singleShot(6000, this, [this]() { m_connectBtn->setEnabled(true); });
}

void MainWindow::injectSiteScript()
{
    const QString js = siteScript();
    if (js.isEmpty()) {
        log(QStringLiteral("✗ 找不到 js/site.js"));
        return;
    }
    m_cdp->evaluate(js, [this](bool ok, const QJsonValue &v, const QString &err) {
        if (!ok) {
            log(QStringLiteral("✗ 注入站点脚本失败：%1").arg(err));
            return;
        }
        log(QStringLiteral("站点脚本就绪：%1").arg(v.toString()));
        m_cdp->evaluate(QStringLiteral("JSON.stringify(__yw.info())"),
                        [this](bool ok2, const QJsonValue &v2, const QString &) {
            if (ok2)
                log(QStringLiteral("当前页面：%1").arg(v2.toString()));
        });
    });
}

void MainWindow::startRun()
{
    if (m_running)
        return;
    if (isPublishMode()) {
        startPublish();
        return;
    }
    if (!m_connected) {
        QMessageBox::information(this, QStringLiteral("还没连接"),
                                 QStringLiteral("先启动客户端并点「连接页面」，连上后才能操作页面。"));
        return;
    }
    const bool draft = isDraftMode();
    if (!draft && m_store.count() == 0) {
        QMessageBox::information(this, QStringLiteral("没有章节"),
                                 QStringLiteral("先选一个分章 txt 目录并载入。"));
        return;
    }
    // 从草稿模式切回来时，表格里还是草稿的行，先按本地目录重建一遍
    if (!draft && m_table->rowCount() != m_store.count())
        reloadChapters();

    const int lo = m_from->value();
    const int hi = m_to->value();
    if (lo > hi) {
        QMessageBox::warning(this, QStringLiteral("范围不对"), QStringLiteral("起始章不能大于结束章。"));
        return;
    }

    m_subset.clear();
    m_rowOffset = -1;
    if (draft) {
        // 草稿校对：章节号直接来自区间，表格只是进度视图
        rebuildTable();
        m_rowOffset = 0;
        for (int no = lo; no <= hi; ++no) {
            Chapter c;
            c.no = no;
            m_subset.append(c);
        }
    } else {
        for (int i = 0; i < m_store.count(); ++i) {
            const Chapter &c = m_store.chapters()[i];
            if (c.no < lo || c.no > hi)
                continue;
            if (m_rowOffset < 0)
                m_rowOffset = i;
            m_subset.append(c);
            if (auto *it = m_table->item(i, 3))
                it->setText(QStringLiteral("排队中"));
        }
    }
    if (m_subset.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("区间为空"), QStringLiteral("这个范围内没有章节。"));
        return;
    }

    // 前置检查：页面里得能认出「新建章」
    m_cdp->evaluate(QStringLiteral("__yw.info()"), [this](bool ok, const QJsonValue &, const QString &err) {
        if (!ok) {
            log(QStringLiteral("✗ 页面脚本不在（页面刷新过？）：%1").arg(err));
            injectSiteScript();
        }
    });

    UploadOptions opt;
    opt.draftMode    = draft;
    opt.proofread    = draft ? true : m_proofread->isChecked();
    opt.finishMode   = draft ? QStringLiteral("none") : m_finishMode->currentData().toString();
    opt.titleWithNo  = m_titleWithNo->isChecked();
    opt.titleUseStem = m_titleUseStem->isChecked();
    opt.bookName     = m_bookName->currentText().trimmed();
    opt.bookCbid     = m_bookName->currentData().toString();
    {
        const int bi = m_bookName->currentIndex();
        if (bi < 0 || m_bookName->itemText(bi) != m_bookName->currentText())
            opt.bookCbid.clear();          // 手改过书名，CBID 不可信
    }
    if (opt.bookName.isEmpty() && !draft) {
        // 目录名通常就是书名：三国：曹贼的科技与狠活前传_分章txt → 三国：曹贼的科技与狠活前传
        QString base = QFileInfo(m_srcDir->text()).fileName();
        base.remove(QRegularExpression(QStringLiteral("[_\\-\\s]*(分章)?(txt)?$")));
        opt.bookName = base;
        if (!base.isEmpty())
            log(QStringLiteral("作品名留空，按目录名推导为「%1」").arg(base));
    }
    opt.skipExisting = m_skipExisting->isChecked();

    if (draft) {
        log(QStringLiteral("草稿校对：第 %1 ~ %2 章，共 %3 章（只跑错别字/病句检测并改建议，不动标题正文）")
                .arg(lo).arg(hi).arg(m_subset.size()));
        if (opt.bookName.isEmpty())
            log(QStringLiteral("提示：书名留空 —— 只要客户端此刻停在目标作品的写作页就能正常跑"));
    }

    m_progress->setRange(0, m_subset.size());
    m_progress->setValue(0);

    m_running = true;
    updateRunUi();
    m_worker->setChapters(m_subset);
    m_worker->start(opt);
}

void MainWindow::stopRun()
{
    m_worker->stop();
    m_pub->stop();
    m_running = false;
    updateRunUi();
}

// 定时发布：先读客户端的全量目录，只排「草稿」章（已发布 / 已定时 / 回收站都跳过），
// 再按「每天 N 章、间隔 M 分钟」把时间算好，交给 PublishWorker 逐章执行。
void MainWindow::startPublish()
{
    if (!m_connected) {
        QMessageBox::information(this, QStringLiteral("还没连接"),
                                 QStringLiteral("先启动客户端并点「连接页面」，连上后才能发布。"));
        return;
    }
    const int lo = m_from->value();
    const int hi = m_to->value();
    if (lo > hi) {
        QMessageBox::warning(this, QStringLiteral("范围不对"),
                             QStringLiteral("起始章不能大于结束章。"));
        return;
    }
    if (m_bookName->currentText().trimmed().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("还没有书名"),
                                 QStringLiteral("先在左上角「作品」里填/选一本书，再点「进入作品」。"));
        return;
    }

    const int per = m_perDay->value();
    const int gap = m_gapMin->value();
    const bool dry = m_dryRun->isChecked();
    const QDateTime start = scheduleStart();
    if (QDateTime(QDate(m_startDate->date()), m_startTime->time()) <= QDateTime::currentDateTime()) {
        log(QStringLiteral("起始时间已过，自动顺延到 %1 %2")
                .arg(start.toString(QStringLiteral("yyyy-MM-dd")),
                     start.toString(QStringLiteral("HH:mm"))));
    }

    m_progress->setRange(0, 0);
    statusBar()->showMessage(QStringLiteral("读取章节目录…"));

    m_cdp->evaluate(QStringLiteral("__yw.chapterList()"),
                    [this, lo, hi, per, gap, dry, start](bool ok, const QJsonValue &v, const QString &err) {
        QHash<int, QString> titles, status;
        if (ok && v.isArray()) {
            for (const QJsonValue &x : v.toArray()) {
                const QJsonObject o = x.toObject();
                const int no = o.value(QStringLiteral("no")).toInt();
                titles.insert(no, o.value(QStringLiteral("title")).toString());
                status.insert(no, o.value(QStringLiteral("status")).toString());
            }
        }
        const bool haveIndex = !titles.isEmpty();
        if (!haveIndex) {
            // 没目录就没法判断哪些是草稿、该排哪天，宁可停下也不乱发
            m_progress->setRange(0, 1);
            m_progress->setValue(0);
            statusBar()->showMessage(QStringLiteral("就绪"));
            log(QStringLiteral("✗ 读不到客户端章节目录（%1）")
                    .arg(ok ? QStringLiteral("目录为空，可能还停在书架页")
                            : err));
            QMessageBox::information(
                this, QStringLiteral("先进入作品"),
                QStringLiteral("读不到这本书的章节目录。\n\n"
                               "请先在左上角点「进入作品」，等客户端切到这本书的写作页"
                               "（侧栏能看到「草稿/已发布」两个标签），再点「开始定时发布」。"));
            return;
        }

        QVector<PublishItem> plan;
        QStringList skipped;
        int idx = 0;
        for (int no = lo; no <= hi; ++no) {
            const QString st = status.value(no);
            if (st.isEmpty()) {
                skipped << QStringLiteral("第%1章（目录里没有）").arg(no);
                continue;
            }
            if (st != QLatin1String("1")) {          // 只发 status=1 的草稿
                skipped << QStringLiteral("第%1章（%2）").arg(no).arg(statusLabel(st));
                continue;
            }
            PublishItem it;
            it.no = no;
            it.title = titles.value(no);
            it.when = slotTime(start, idx++, per, gap);
            plan.append(it);
        }

        if (!skipped.isEmpty()) {
            const int keep = 6;
            QString list = skipped.mid(0, keep).join(QStringLiteral("、"));
            if (skipped.size() > keep)
                list += QStringLiteral("…（共 %1 章）").arg(skipped.size());
            log(QStringLiteral("跳过 %1 章非草稿：%2").arg(skipped.size()).arg(list));
        }
        if (plan.isEmpty()) {
            m_progress->setRange(0, 1);
            m_progress->setValue(0);
            statusBar()->showMessage(QStringLiteral("就绪"));
            QMessageBox::information(this, QStringLiteral("没有可发布的草稿"),
                                     QStringLiteral("第 %1 ~ %2 章里没有「草稿」状态的章节"
                                                    "（已发布/已定时/回收站都会跳过）。")
                                         .arg(lo).arg(hi));
            return;
        }

        // 表格改成「实际计划」
        m_table->setRowCount(plan.size());
        applyTableHeaders();
        for (int i = 0; i < plan.size(); ++i) {
            const PublishItem &it = plan.at(i);
            m_table->setItem(i, 0, new QTableWidgetItem(QString::number(it.no)));
            m_table->setItem(i, 1, new QTableWidgetItem(it.when.toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
            m_table->setItem(i, 2, new QTableWidgetItem(it.title.isEmpty()
                                                            ? QStringLiteral("（草稿）") : it.title));
            m_table->setItem(i, 3, new QTableWidgetItem(QStringLiteral("待发布")));
        }

        log(QStringLiteral("发布计划：%1 章，%2 起，每天 %3 章、间隔 %4 分钟，最后一章 %5%6")
                .arg(plan.size())
                .arg(start.toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                .arg(per).arg(gap)
                .arg(plan.last().when.toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                .arg(dry ? QStringLiteral("（演练模式，不提交）") : QString()));

        PublishOptions opt;
        opt.bookName = m_bookName->currentText().trimmed();
        opt.bookCbid = m_bookName->currentData().toString();
        const int bi = m_bookName->currentIndex();
        if (bi < 0 || m_bookName->itemText(bi) != m_bookName->currentText())
            opt.bookCbid.clear();
        opt.dryRun = dry;

        m_progress->setRange(0, plan.size());
        m_progress->setValue(0);
        m_running = true;
        updateRunUi();
        m_pub->setPlan(plan);
        m_pub->start(opt);
    });
}

void MainWindow::dumpPage()
{
    if (!m_connected) {
        QMessageBox::information(this, QStringLiteral("未连接"), QStringLiteral("先连上页面。"));
        return;
    }
    m_cdp->evaluate(QStringLiteral("JSON.stringify(__yw.dump())"),
                    [this](bool ok, const QJsonValue &v, const QString &err) {
        if (!ok) {
            log(QStringLiteral("✗ 抓取失败：%1").arg(err));
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(v.toString().toUtf8()).object();

        QStringList btns;
        for (const QJsonValue &b : o.value(QStringLiteral("buttons")).toArray())
            btns << b.toString();
        log(QStringLiteral("页面按钮：%1").arg(btns.join(QStringLiteral(" / "))));

        const QJsonArray eds = o.value(QStringLiteral("editables")).toArray();
        log(QStringLiteral("可编辑区 %1 个：").arg(eds.size()));
        for (const QJsonValue &e : eds) {
            const QJsonObject eo = e.toObject();
            log(QStringLiteral("  <%1 class=\"%2\"> top=%3 h=%4 w=%5 text=\"%6\"")
                    .arg(eo.value(QStringLiteral("tag")).toString())
                    .arg(eo.value(QStringLiteral("cls")).toString())
                    .arg(eo.value(QStringLiteral("top")).toInt())
                    .arg(eo.value(QStringLiteral("h")).toInt())
                    .arg(eo.value(QStringLiteral("w")).toInt())
                    .arg(eo.value(QStringLiteral("text")).toString()));
        }

        const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/debug");
        QDir().mkpath(dir);
        const QString file = dir + QStringLiteral("/page-%1.json")
                                        .arg(QDateTime::currentDateTime().toString(QStringLiteral("MMdd-hhmmss")));
        QFile f(file);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
            f.close();
            log(QStringLiteral("完整结构已存：%1").arg(QDir::toNativeSeparators(file)));
        }
    });
}

void MainWindow::runCustomJs()
{
    if (!m_connected) {
        QMessageBox::information(this, QStringLiteral("未连接"), QStringLiteral("先连上页面。"));
        return;
    }
    bool ok = false;
    const QString js = QInputDialog::getMultiLineText(
        this, QStringLiteral("执行 JS"), QStringLiteral("在页面里执行（结果按 JSON 回显）："),
        QStringLiteral("__yw.info()"), &ok);
    if (!ok || js.trimmed().isEmpty())
        return;
    m_cdp->evaluate(QStringLiteral("JSON.stringify(%1)").arg(js),
                    [this](bool okr, const QJsonValue &v, const QString &err) {
        if (!okr) {
            log(QStringLiteral("✗ %1").arg(err));
            return;
        }
        log(QStringLiteral("→ %1").arg(v.toString()));
    });
}

// ---------------- 书名 / 进入作品 ----------------

QString MainWindow::jsString(const QString &s) const
{
    QString e = s;
    e.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    e.replace(QLatin1Char('\''), QStringLiteral("\\'"));
    e.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return QStringLiteral("'%1'").arg(e);
}

static QStringList bookTitles(const QJsonArray &arr)
{
    QStringList ts;
    for (const QJsonValue &v : arr)
        ts << (v.isObject() ? v.toObject().value(QStringLiteral("title")).toString()
                            : v.toString());
    ts.removeAll(QString());
    return ts;
}

void MainWindow::applyBookList(const QJsonArray &books)
{
    const QString keep = m_bookName->currentText().trimmed();
    m_bookName->blockSignals(true);
    m_bookName->clear();
    for (const QJsonValue &v : books) {
        // 页面返回 [{cbid,title}]；兼容早期的纯字符串数组
        QString title, cbid;
        if (v.isObject()) {
            const QJsonObject o = v.toObject();
            title = o.value(QStringLiteral("title")).toString().trimmed();
            cbid  = o.value(QStringLiteral("cbid")).toString();
        } else {
            title = v.toString().trimmed();
        }
        if (title.isEmpty())
            continue;
        if (m_bookName->findText(title) < 0)
            m_bookName->addItem(title, cbid);
    }
    m_bookName->blockSignals(false);

    // 已经输入的书名优先保留；全新用户则默认选中第一本
    if (!keep.isEmpty()) {
        const int idx = m_bookName->findText(keep);
        if (idx >= 0)
            m_bookName->setCurrentIndex(idx);
        else
            m_bookName->setEditText(keep);       // 手输的书名可能不在列表里
    } else if (m_bookName->count() > 0) {
        m_bookName->setCurrentIndex(0);
    }
}

void MainWindow::refreshBooks()
{
    refreshBooksImpl(false);
}

void MainWindow::refreshBooksImpl(bool silent)
{
    if (!m_connected) {
        if (!silent)
            log(QStringLiteral("还没连接页面，读不到书架；直接手输书名也可以。"));
        return;
    }

    m_cdp->evaluate(QStringLiteral("__yw.listBooks()"),
                    [this, silent](bool ok, const QJsonValue &v, const QString &err) {
        const QJsonArray here = ok ? v.toArray() : QJsonArray();
        if (!here.isEmpty()) {
            applyBookList(here);
            if (!silent) {
                log(QStringLiteral("已读到 %1 本作品：%2").arg(here.size()).arg(bookTitles(here).join(QStringLiteral("、"))));
            }
            return;
        }
        if (silent) {
            if (!ok && !err.isEmpty())
                log(QStringLiteral("（静默刷新作品列表失败：%1）").arg(err));
            return;                       // 静默模式不切 target
        }

        // 当前不在书架页：切过去读一遍，再切回原来的写作页
        const bool fromWrite = m_cdp->currentUrl().contains(QStringLiteral("#/chapter/"));
        log(QStringLiteral("当前页面里没有书架，切到书架读作品列表…"));
        m_cdp->switchToBooksTarget([this, fromWrite](bool switched, const QString &why) {
            if (!switched) {
                log(QStringLiteral("✗ 切到书架失败：%1").arg(why));
                return;
            }
            QTimer::singleShot(1400, this, [this, fromWrite]() {   // 等新页面把书架渲染出来
                m_cdp->evaluate(QStringLiteral("__yw.listBooks()"),
                                [this, fromWrite](bool ok2, const QJsonValue &v2, const QString &e2) {
                    if (!ok2) {
                        log(QStringLiteral("✗ 读作品列表失败：%1").arg(e2));
                        return;
                    }
                    const QJsonArray arr = v2.toArray();
                    if (arr.isEmpty()) {
                        log(QStringLiteral("书架是空的，或者书架还没加载完"));
                        return;
                    }
                    applyBookList(arr);
                    log(QStringLiteral("读到 %1 本：%2").arg(arr.size()).arg(bookTitles(arr).join(QStringLiteral("、"))));

                    if (fromWrite) {
                        m_cdp->switchToChapterTarget([this](bool sw, const QString &w) {
                            log(sw ? QStringLiteral("已切回写作页")
                                   : QStringLiteral("切回写作页失败：%1").arg(w));
                        });
                    }
                });
            });
        });
    });
}

void MainWindow::enterBook()
{
    const QString name = m_bookName->currentText().trimmed();
    if (name.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("还没有书名"),
                                 QStringLiteral("先填书名，或连接后点「刷新列表」从书架里选一本。"));
        return;
    }
    saveSettings();

    if (!m_connected) {
        log(QStringLiteral("还没连接页面：书名记为「%1」，先连上再进书…").arg(name));
        m_pendingBook = name;
        if (!m_autoConnect->isActive()) {
            m_autoTry = 0;
            m_autoConnect->start();
        }
        m_cdp->discoverAndConnect();
        return;
    }

    // 下拉里选中的项带着 CBID；手改过文本则 CBID 作废，退回按书名匹配
    QString cbid = m_bookName->currentData().toString();
    const int idx = m_bookName->currentIndex();
    if (idx < 0 || m_bookName->itemText(idx) != m_bookName->currentText())
        cbid.clear();

    m_bookBtn->setEnabled(false);
    m_cdp->evaluate(QStringLiteral("__yw.where()"), [this, name, cbid](bool ok, const QJsonValue &v, const QString &) {
        const QString page = ok ? v.toObject().value(QStringLiteral("page")).toString() : QString();
        if (page == QLatin1String("write")) {
            // 写作页里没有书架，要先回到书架再点书
            log(QStringLiteral("当前在写作页，先切回书架…"));
            m_cdp->switchToBooksTarget([this, name, cbid](bool switched, const QString &why) {
                if (!switched) {
                    log(QStringLiteral("✗ 切到书架失败：%1").arg(why));
                    m_bookBtn->setEnabled(true);
                    return;
                }
                QTimer::singleShot(1400, this, [this, name, cbid]() { doOpenBook(name, cbid); });
            });
        } else {
            doOpenBook(name, cbid);
        }
    });
}

void MainWindow::doOpenBook(const QString &name, const QString &cbid)
{
    // 刚切过 target 时 __yw 可能还没注入，先确认再动手
    m_cdp->evaluate(QStringLiteral("typeof window.__yw"), [this, name, cbid](bool ok, const QJsonValue &v, const QString &) {
        if (!ok || v.toString() != QLatin1String("object")) {
            log(QStringLiteral("页面脚本还没就绪，重新注入后重试…"));
            injectSiteScript();
            QTimer::singleShot(900, this, [this, name, cbid]() { doOpenBookNow(name, cbid); });
            return;
        }
        doOpenBookNow(name, cbid);
    });
}

void MainWindow::doOpenBookNow(const QString &name, const QString &cbid)
{
    log(QStringLiteral("在书架里找「%1」%2…")
            .arg(name, cbid.isEmpty() ? QString() : QStringLiteral("（CBID %1）").arg(cbid)));
    const QString js = QStringLiteral("__yw.openBook({cbid:%1, title:%2})")
                           .arg(jsString(cbid), jsString(name));
    m_cdp->evaluate(js, [this](bool ok, const QJsonValue &v, const QString &err) {
        const QJsonObject r = v.toObject();
        if (!ok || !r.value(QStringLiteral("ok")).toBool()) {
            QString why = ok ? r.value(QStringLiteral("why")).toString() : err;
            const QJsonArray titles = r.value(QStringLiteral("titles")).toArray();
            if (!titles.isEmpty()) {
                why += QStringLiteral("\n书架上有：") + bookTitles(titles).join(QStringLiteral("、"));
                applyBookList(titles);          // 顺手把可选书名填进下拉
            }
            if (r.contains(QStringLiteral("html")))
                why += QStringLiteral("\n卡片结构：") + r.value(QStringLiteral("html")).toString();
            log(QStringLiteral("✗ %1").arg(why));
            m_bookBtn->setEnabled(true);
            return;
        }
        log(QStringLiteral("已用 %1 打开「%2」，等写作页标签出现…")
                .arg(r.value(QStringLiteral("via")).toString(),
                     r.value(QStringLiteral("opened")).toString()));
        m_bookPollTry = 0;
        m_bookPoll->start();
    });
}

void MainWindow::revertClientPatch()
{
    const QString exe = m_exePath->text().trimmed();
    if (exe.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("没有路径"),
                                 QStringLiteral("先指定 yuewenedit.exe 的位置。"));
        return;
    }
    QString err;
    if (!InstallPatch::revert(QFileInfo(exe).absolutePath(), &err)) {
        QMessageBox::warning(this, QStringLiteral("还原失败"), err);
        return;
    }
    log(QStringLiteral("已还原客户端原始入口文件（package.json.ywbak）。"));
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    flushLog();
    saveSettings();
    m_worker->stop();
    m_pub->stop();
    m_cdp->close();
    QMainWindow::closeEvent(e);
}

// ---------------- 杂项 ----------------

void MainWindow::log(const QString &msg)
{
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss"));
    m_logBuf << QStringLiteral("[%1] %2").arg(ts, msg);
    if (!m_log)                       // 控件还没建好（构造早期），先攒着
        return;
    if (m_logBuf.size() >= 200)       // 一次刷太多行反而卡，先刷一批
        flushLog();
    else if (m_logTimer && !m_logTimer->isActive())
        m_logTimer->start();
}

void MainWindow::flushLog()
{
    if (m_logTimer)
        m_logTimer->stop();
    if (!m_log || m_logBuf.isEmpty())
        return;
    m_log->appendPlainText(m_logBuf.join(QLatin1Char('\n')));   // 一批一次插入
    m_logBuf.clear();
}

void MainWindow::setConnUi(bool connected, const QString &detail)
{
    m_connected = connected;
    m_connDot->setProperty("state", connected ? QStringLiteral("on") : QStringLiteral("off"));
    m_connDot->style()->unpolish(m_connDot);
    m_connDot->style()->polish(m_connDot);
    m_connText->setText(detail);
    m_connectBtn->setText(connected ? QStringLiteral("断开") : QStringLiteral("连接页面"));
    m_connectBtn->setEnabled(true);
    updateRunUi();
}

void MainWindow::updateRunUi()
{
    m_startBtn->setEnabled(!m_running && m_connected);
    m_stopBtn->setEnabled(m_running);
    m_from->setEnabled(!m_running);
    m_to->setEnabled(!m_running);
    updateModeUi();          // 里面会连带处理 proofread / 标题 / 收尾动作的可用状态
}

void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue(QStringLiteral("ui/srcDir"), m_srcDir->text());
    s.setValue(QStringLiteral("ui/exePath"), m_exePath->text());
    s.setValue(QStringLiteral("ui/proofread"), m_proofread->isChecked());
    s.setValue(QStringLiteral("ui/titleNo"), m_titleWithNo->isChecked());
    s.setValue(QStringLiteral("ui/titleStem"), m_titleUseStem->isChecked());
    s.setValue(QStringLiteral("ui/finish"), m_finishMode->currentIndex());
    s.setValue(QStringLiteral("ui/bookName"), m_bookName->currentText());
    s.setValue(QStringLiteral("ui/skipExisting"), m_skipExisting->isChecked());
    s.setValue(QStringLiteral("ui/mode"), m_mode->currentIndex());
    s.setValue(QStringLiteral("pub/date"), m_startDate->date());
    s.setValue(QStringLiteral("pub/time"), m_startTime->time());
    s.setValue(QStringLiteral("pub/perDay"), m_perDay->value());
    s.setValue(QStringLiteral("pub/gapMin"), m_gapMin->value());
    s.setValue(QStringLiteral("pub/dryRun"), m_dryRun->isChecked());
}

void MainWindow::loadSettings()
{
    QSettings s;
    m_srcDir->setText(s.value(QStringLiteral("ui/srcDir")).toString());
    m_exePath->setText(s.value(QStringLiteral("ui/exePath")).toString());
    m_proofread->setChecked(s.value(QStringLiteral("ui/proofread"), true).toBool());
    m_titleWithNo->setChecked(s.value(QStringLiteral("ui/titleNo"), true).toBool());
    m_titleUseStem->setChecked(s.value(QStringLiteral("ui/titleStem"), true).toBool());
    m_finishMode->setCurrentIndex(s.value(QStringLiteral("ui/finish"), 0).toInt());
    m_bookName->setEditText(s.value(QStringLiteral("ui/bookName")).toString());
    m_skipExisting->setChecked(s.value(QStringLiteral("ui/skipExisting"), true).toBool());
    m_mode->setCurrentIndex(s.value(QStringLiteral("ui/mode"), 0).toInt());

    m_startDate->setDate(s.value(QStringLiteral("pub/date"), QDate::currentDate()).toDate());
    m_startTime->setTime(s.value(QStringLiteral("pub/time"), QTime(19, 0)).toTime());
    m_perDay->setValue(s.value(QStringLiteral("pub/perDay"), 3).toInt());
    m_gapMin->setValue(s.value(QStringLiteral("pub/gapMin"), 5).toInt());
    m_dryRun->setChecked(s.value(QStringLiteral("pub/dryRun"), true).toBool());   // 首次默认演练，安全

    if (!m_srcDir->text().isEmpty() && QFileInfo::exists(m_srcDir->text()))
        QTimer::singleShot(0, this, [this]() { reloadChapters(); });
}
