#ifndef CDPCLIENT_H
#define CDPCLIENT_H

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QObject>
#include <QWebSocket>

#include <functional>

// 极简 DevTools 协议客户端：
//   1) HTTP GET /json/list 找到目标页（作家助手窗口里那个页面）
//   2) WebSocket 连上去，只用到 Runtime.evaluate —— 在页面里跑一段 JS 拿返回值
// 之所以不用 QWebEngine：MinGW 版 Qt 没有 WebEngine 模块，而且目标本来就是客户端自己的窗口
class CdpClient : public QObject
{
    Q_OBJECT

public:
    using EvalCallback = std::function<void(bool ok, const QJsonValue &value, const QString &err)>;

    explicit CdpClient(QObject *parent = nullptr);

    void setPort(int port) { m_port = port; }
    int  port() const { return m_port; }

    bool isConnected() const { return m_ws.state() == QAbstractSocket::ConnectedState; }

    void discoverAndConnect();     // 列出 target，优先连写作页（#/chapter/），否则连首页
    void close();

    // 在页面里执行 JS（表达式，最后的值会被 returnByValue 带回来）
    void evaluate(const QString &js, EvalCallback cb, int timeoutMs = 20000);

    QString currentUrl() const { return m_url; }

    // 扫描 /json/list，若存在写作页 target（url 含 #/chapter/）就连过去；
    // 已连着它则原样返回。cb(switched, why)：switched=false 表示没有写作页标签。
    void switchToChapterTarget(std::function<void(bool switched, const QString &why)> cb);
    void switchToBooksTarget(std::function<void(bool switched, const QString &why)> cb);

signals:
    void log(const QString &msg);
    void connected(const QString &url, const QString &title);
    void disconnected();
    void failed(const QString &why);

private:
    void connectToTarget(const QJsonObject &target);
    void handleMessage(const QString &text);
    void failPending(const QString &why);

    QNetworkAccessManager m_net;
    QWebSocket            m_ws;
    QString               m_url;                 // 当前连接 target 的页面地址
    int                   m_port = 9222;
    int                   m_nextId = 1;
    QHash<int, EvalCallback> m_pending;
};

#endif // CDPCLIENT_H
