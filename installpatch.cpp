#include "installpatch.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

namespace {

QString appDir(const QString &programDir)
{
    return programDir + QStringLiteral("/resources/app");
}

QString packageJsonPath(const QString &programDir)
{
    return appDir(programDir) + QStringLiteral("/package.json");
}

QString backupPath(const QString &programDir)
{
    return packageJsonPath(programDir) + QStringLiteral(".ywbak");
}

QString mainJsPath(const QString &programDir)
{
    return appDir(programDir) + QStringLiteral("/cdp-main.js");
}

const char *kMainJs = R"(// 由 ChapterUploader 写入：给客户端加本地调试端口，便于自动化操作页面。
// 只追加了一个开关，随后立即加载原入口 loader.asar/main.js，不改变客户端任何业务逻辑。
try {
  const { app } = require('electron');
  app.commandLine.appendSwitch('remote-debugging-port', '9222');
  app.commandLine.appendSwitch('remote-allow-origins', '*');
} catch (e) { console.error('cdp patch failed', e); }
require('./loader.asar/main.js');
)";

QJsonObject readJson(const QString &path, bool *ok)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (ok) *ok = false;
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (ok) *ok = doc.isObject();
    return doc.object();
}

bool writeJson(const QString &path, const QJsonObject &obj)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

} // namespace

namespace InstallPatch {

QString defaultProgramDir()
{
    // 优先用上次用户在界面上选过的
    QSettings s;
    const QString saved = s.value(QStringLiteral("client/programDir")).toString();
    if (looksLikeProgramDir(saved))
        return saved;

    const QStringList guesses = {
        QStringLiteral("D:/AppStoreSoftstore/Install/YuewenEdit/program"),
        QStringLiteral("C:/Program Files/YuewenEdit/program"),
        QStringLiteral("C:/Program Files (x86)/YuewenEdit/program"),
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/YuewenEdit/program"),
    };
    for (const QString &g : guesses)
        if (looksLikeProgramDir(g))
            return g;
    return {};
}

bool looksLikeProgramDir(const QString &dir)
{
    return !dir.isEmpty() && QFileInfo::exists(packageJsonPath(dir));
}

QString exePath(const QString &programDir)
{
    return programDir + QStringLiteral("/yuewenedit.exe");
}

bool isPatched(const QString &programDir)
{
    if (!looksLikeProgramDir(programDir))
        return false;
    bool ok = false;
    const QJsonObject pkg = readJson(packageJsonPath(programDir), &ok);
    return ok && pkg.value(QStringLiteral("main")).toString() == QLatin1String("./cdp-main.js")
           && QFileInfo::exists(mainJsPath(programDir));
}

bool enable(const QString &programDir, QString *err)
{
    if (!looksLikeProgramDir(programDir)) {
        if (err) *err = QStringLiteral("这不是作家助手安装目录（找不到 resources/app/package.json）");
        return false;
    }
    if (isPatched(programDir))
        return true;

    QString pkgPath = packageJsonPath(programDir);
    const QString bak = backupPath(programDir);
    if (!QFileInfo::exists(bak)) {
        if (!QFile::copy(pkgPath, bak)) {
            if (err) *err = QStringLiteral("备份 package.json 失败（客户端目录只读？请用管理员身份运行）");
            return false;
        }
    }

    bool ok = false;
    QJsonObject pkg = readJson(pkgPath, &ok);
    if (!ok) {
        if (err) *err = QStringLiteral("package.json 解析失败");
        return false;
    }
    pkg.insert(QStringLiteral("main"), QStringLiteral("./cdp-main.js"));
    if (!writeJson(pkgPath, pkg)) {
        if (err) *err = QStringLiteral("写 package.json 失败");
        return false;
    }

    QFile js(mainJsPath(programDir));
    if (!js.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = QStringLiteral("写 cdp-main.js 失败");
        return false;
    }
    js.write(kMainJs);
    js.close();

    QSettings().setValue(QStringLiteral("client/programDir"), programDir);
    return true;
}

bool revert(const QString &programDir, QString *err)
{
    const QString bak = backupPath(programDir);
    if (!QFileInfo::exists(bak)) {
        if (err) *err = QStringLiteral("没有找到备份，可能本来就没打过补丁");
        return false;
    }
    if (QFileInfo::exists(packageJsonPath(programDir)) && !QFile::remove(packageJsonPath(programDir))) {
        if (err) *err = QStringLiteral("删除现 package.json 失败");
        return false;
    }
    if (!QFile::copy(bak, packageJsonPath(programDir))) {
        if (err) *err = QStringLiteral("还原 package.json 失败");
        return false;
    }
    QFile::remove(mainJsPath(programDir));
    return true;
}

} // namespace InstallPatch
