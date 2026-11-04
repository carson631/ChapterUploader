#ifndef INSTALLPATCH_H
#define INSTALLPATCH_H

#include <QString>

// 给已安装的「作家助手」客户端开一个本地调试端口。
// 做法：把 resources/app/package.json 的 main 指向我们写的 cdp-main.js（原文件备份成 .ywbak），
//      cdp-main.js 里 appendSwitch('remote-debugging-port') 后再加载原 loader.asar。
// 只改这一个入口，不动 asar 本体，随时可一键还原。
namespace InstallPatch {

QString defaultProgramDir();                       // 猜安装目录（program 这一层）
bool    looksLikeProgramDir(const QString &dir);   // 目录里有没有 resources/app/package.json
QString exePath(const QString &programDir);

bool    isPatched(const QString &programDir);
bool    enable(const QString &programDir, QString *err);
bool    revert(const QString &programDir, QString *err);

} // namespace InstallPatch

#endif // INSTALLPATCH_H
