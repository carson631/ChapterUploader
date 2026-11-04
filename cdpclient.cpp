#include "cdpclient.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

CdpClient::CdpClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_ws, &QWebSocket::connected, this, [this]() {
        log(QStringLiteral("CDP 已连接：%1").arg(m_ws.requestUrl().toString()));
    });
    connect(&m_ws, &QWebSocket::disconnected, this, [this]() {
        failPending(QStringLiteral("调试连接已断开"));
        emit disconnected();
    });
    connect(&m_ws, &QWebSocket::textMessageReceived, this, &CdpClient::handleMessage);
    connect(&m_ws, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit failed(QStringLiteral("WebSocket 错误：%1").arg(m_ws.errorString()));
    });
}

void CdpClient::discoverAndConnect()
{
    if (isConnected()) {
        m_ws.close();
        return;
    }

    QUrl url(QStringLiteral("http://127.0.0.1:%1/json/list").arg(m_port));
    QNetworkRequest req(url);
    req.setTransferTimeout(4000);
    QNetworkReply *reply = m_net.get(req);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit failed(QStringLiteral("连不上调试端口 %1：%2\n客户端没带调试端口启动？先点「启用调试并启动客户端」")
                            .arg(m_port).arg(reply->errorString()));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isArray()) {
            emit failed(QStringLiteral("/json/list 返回的不是数组"));
            return;
        }
        const QJsonArray arr = doc.array();
        QJsonObject best, chapter;
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("type")).toString() != QLatin1String("page"))
                continue;
            const QString u = o.value(QStringLiteral("url")).toString();
            if (u.startsWith(QLatin1String("devtools")))
                continue;
            best = o;                                    // 先取第一个 page
            if (u.contains(QLatin1String("#/chapter/"))) {
                chapter = o;                             // 写作页优先
                break;
            }
        }
        if (!chapter.isEmpty())
            best = chapter;
        if (best.isEmpty()) {
            emit failed(QStringLiteral("调试端口通了，但没找到页面 target（客户端窗口还没加载完？）"));
            return;
        }
        connectToTarget(best);
    });
}

void CdpClient::connectToTarget(const QJsonObject &target)
{
    const QString wsUrl = target.value(QStringLiteral("webSocketDebuggerUrl")).toString();
    if (wsUrl.isEmpty()) {
        emit failed(QStringLiteral("target 没有 webSocketDebuggerUrl"));
        return;
    }
    m_url = target.value(QStringLiteral("url")).toString();
    log(QStringLiteral("目标页：%1  (%2)")
            .arg(target.value(QStringLiteral("title")).toString(), m_url));
    // 切 target 时旧连接还在，必须先断开，否则 open() 直接失败
    if (m_ws.state() != QAbstractSocket::UnconnectedState)
        m_ws.abort();
    // Chromium 92+ 会校验 DevTools WS 的 Origin；补丁里已加 --remote-allow-origins=*，这里不用额外处理
    m_ws.open(QUrl(wsUrl));

    // 连上后打个招呼，确认 Runtime 可用
    QTimer::singleShot(300, this, [this, target]() {
        if (!isConnected())
            return;
        evaluate(QStringLiteral("1+1"), [this, target](bool ok, const QJsonValue &v, const QString &e) {
            if (!ok) { emit failed(QStringLiteral("Runtime.evaluate 失败：%1").arg(e)); return; }
            emit connected(target.value(QStringLiteral("url")).toString(),
                           target.value(QStringLiteral("title")).toString());
            Q_UNUSED(v)
        });
    });
}

void CdpClient::switchToChapterTarget(std::function<void(bool switched, const QString &why)> cb)
{
    QUrl url(QStringLiteral("http://127.0.0.1:%1/json/list").arg(m_port));
    QNetworkRequest req(url);
    req.setTransferTimeout(4000);
    QNetworkReply *reply = m_net.get(req);

    connect(reply, &QNetworkReply::finished, this, [this, reply, cb]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            cb(false, reply->errorString());
            return;
        }
        const QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).array();
        QJsonObject chapter;
        int chapterCount = 0;
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("type")).toString() != QLatin1String("page"))
                continue;
            const QString u = o.value(QStringLiteral("url")).toString();
            if (u.startsWith(QLatin1String("devtools")) || !u.contains(QLatin1String("#/chapter/")))
                continue;
            ++chapterCount;
            if (chapter.isEmpty())
                chapter = o;
        }
        if (chapter.isEmpty()) {
            cb(false, QStringLiteral("还没有写作页标签"));
            return;
        }
        const QString targetUrl = chapter.value(QStringLiteral("url")).toString();
        if (isConnected() && m_url == targetUrl) {
            cb(true, QString());                     // 已经连着它了
            return;
        }
        log(QStringLiteral("切换到写作页标签：%1").arg(targetUrl));
        connectToTarget(chapter);
        cb(true, QString());
    });
}

void CdpClient::switchToBooksTarget(std::function<void(bool switched, const QString &why)> cb)
{
    QUrl url(QStringLiteral("http://127.0.0.1:%1/json/list").arg(m_port));
    QNetworkRequest req(url);
    req.setTransferTimeout(4000);
    QNetworkReply *reply = m_net.get(req);

    connect(reply, &QNetworkReply::finished, this, [this, reply, cb]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            cb(false, reply->errorString());
            return;
        }
        const QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).array();
        QJsonObject books;
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("type")).toString() != QLatin1String("page"))
                continue;
            const QString u = o.value(QStringLiteral("url")).toString();
            if (u.startsWith(QLatin1String("devtools")) || u.contains(QLatin1String("#/chapter/")))
                continue;                                        // 写作页不算书架
            if (!u.contains(QLatin1String("index.html")))
                continue;
            books = o;
            break;
        }
        if (books.isEmpty()) {
            cb(false, QStringLiteral("没找到书架页标签（客户端首页）"));
            return;
        }
        const QString targetUrl = books.value(QStringLiteral("url")).toString();
        if (isConnected() && m_url == targetUrl) {
            cb(true, QString());
            return;
        }
        log(QStringLiteral("切换到书架页：%1").arg(targetUrl));
        connectToTarget(books);
        cb(true, QString());
    });
}

void CdpClient::close()
{
    if (m_ws.state() != QAbstractSocket::UnconnectedState)
        m_ws.close();
}

void CdpClient::evaluate(const QString &js, EvalCallback cb, int timeoutMs)
{
    if (!isConnected()) {
        cb(false, QJsonValue(), QStringLiteral("调试连接未建立"));
        return;
    }

    const int id = m_nextId++;
    m_pending.insert(id, cb);

    QJsonObject params;
    params.insert(QStringLiteral("expression"), js);
    params.insert(QStringLiteral("returnByValue"), true);
    params.insert(QStringLiteral("awaitPromise"), true);
    params.insert(QStringLiteral("userGesture"), true);      // 让 click() 被当成用户手势

    QJsonObject msg;
    msg.insert(QStringLiteral("id"), id);
    msg.insert(QStringLiteral("method"), QStringLiteral("Runtime.evaluate"));
    msg.insert(QStringLiteral("params"), params);

    m_ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));

    QTimer::singleShot(timeoutMs, this, [this, id]() {
        auto it = m_pending.find(id);
        if (it == m_pending.end())
            return;
        const EvalCallback cb2 = it.value();
        m_pending.erase(it);
        cb2(false, QJsonValue(), QStringLiteral("JS 执行超时"));
    });
}

void CdpClient::handleMessage(const QString &text)
{
    const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8()).object();
    const int id = msg.value(QStringLiteral("id")).toInt(-1);
    if (id < 0)
        return;                                  // 事件通知，不关心

    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;
    const EvalCallback cb = it.value();
    m_pending.erase(it);

    if (msg.contains(QStringLiteral("error"))) {
        cb(false, QJsonValue(), msg.value(QStringLiteral("error")).toObject()
                                    .value(QStringLiteral("message")).toString());
        return;
    }

    const QJsonObject result = msg.value(QStringLiteral("result")).toObject();
    if (result.contains(QStringLiteral("exceptionDetails"))) {
        const QJsonObject ex = result.value(QStringLiteral("exceptionDetails")).toObject();
        QString detail = ex.value(QStringLiteral("text")).toString();
        const QJsonObject eo = ex.value(QStringLiteral("exception")).toObject();
        if (!eo.value(QStringLiteral("description")).toString().isEmpty())
            detail += QStringLiteral(" · ") + eo.value(QStringLiteral("description")).toString();
        cb(false, QJsonValue(), detail);
        return;
    }

    cb(true, result.value(QStringLiteral("result")).toObject().value(QStringLiteral("value")), QString());
}

void CdpClient::failPending(const QString &why)
{
    const auto callbacks = m_pending;
    m_pending.clear();
    for (const EvalCallback &cb : callbacks)
        cb(false, QJsonValue(), why);
}
