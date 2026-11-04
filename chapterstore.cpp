#include "chapterstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>

QString Chapter::title(bool addChapterNo, bool useStemName) const
{
    const QString base = useStemName ? stem : name;
    return addChapterNo ? QStringLiteral("第%1章 %2").arg(no).arg(base) : base;
}

QString Chapter::html() const
{
    QString out;
    const QStringList lines = content.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        QString line = raw;
        line.replace(QLatin1Char('\t'), QLatin1Char(' '));
        line = line.trimmed();
        if (line.isEmpty())
            continue;                      // 段间空行丢掉，编辑器自己管段距
        line.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
        line.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
        line.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
        out += QStringLiteral("<p>%1</p>").arg(line);
    }
    return out;
}

bool ChapterStore::scan(const QString &dir, QString *err)
{
    m_chapters.clear();
    m_dir = dir;

    QDir d(dir);
    if (!d.exists()) {
        if (err) *err = QStringLiteral("目录不存在：%1").arg(dir);
        return false;
    }

    const QRegularExpression re(QStringLiteral("^(\\d+)\\s*(.*)$"));
    const QFileInfoList files = d.entryInfoList({QStringLiteral("*.txt")}, QDir::Files, QDir::Name);

    for (const QFileInfo &fi : files) {
        const QString stem = fi.completeBaseName();
        const auto m = re.match(stem);
        if (!m.hasMatch())
            continue;                       // 命名不合规的直接跳过（pack_log.txt 之类）

        Chapter c;
        c.no   = m.captured(1).toInt();
        c.stem = stem;
        c.name = m.captured(2).trimmed();
        c.path = fi.absoluteFilePath();

        QFile f(c.path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (err) *err = QStringLiteral("读不了：%1").arg(c.path);
            continue;
        }
        QByteArray raw = f.readAll();
        f.close();
        if (raw.startsWith("\xEF\xBB\xBF"))
            raw.remove(0, 3);               // UTF-8 BOM

        QString text = QString::fromUtf8(raw);
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        c.content = text;

        int n = 0;
        for (const QChar ch : text) {
            if (!ch.isSpace())
                ++n;
        }
        c.chars = n;

        m_chapters.append(c);
    }

    std::sort(m_chapters.begin(), m_chapters.end(),
              [](const Chapter &a, const Chapter &b) { return a.no < b.no; });

    if (m_chapters.isEmpty() && err)
        *err = QStringLiteral("目录里没有「编号+章名.txt」格式的文件");
    return !m_chapters.isEmpty();
}

int ChapterStore::indexOfNo(int no) const
{
    for (int i = 0; i < m_chapters.size(); ++i)
        if (m_chapters[i].no == no)
            return i;
    return -1;
}
