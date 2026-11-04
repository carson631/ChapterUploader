#ifndef CHAPTERSTORE_H
#define CHAPTERSTORE_H

#include <QString>
#include <QVector>

// 一章的本地数据（来自分章 txt）
struct Chapter
{
    int     no = 0;        // 章节编号（取自文件名前缀）
    QString stem;          // 文件名去掉扩展名：170白衣拜将
    QString name;          // 去掉编号后的章名：白衣拜将
    QString path;          // 绝对路径
    QString content;       // 正文（已去 BOM、统一换行）
    int     chars = 0;     // 正文字数（不含空白）
    bool    done = false;  // 是否已上传（运行期状态，不持久化到文件本身）

    // 标题：起点那边的习惯是「第170章 170白衣拜将」
    QString title(bool addChapterNo, bool useStemName) const;
    // 正文转成编辑器能吃的一串 <p>
    QString html() const;
};

// 扫描目录 → 章节列表（按编号升序）
class ChapterStore
{
public:
    bool scan(const QString &dir, QString *err = nullptr);

    const QVector<Chapter> &chapters() const { return m_chapters; }
    int count() const { return m_chapters.size(); }

    // 按编号找下标，找不到返回 -1
    int indexOfNo(int no) const;

private:
    QVector<Chapter> m_chapters;
    QString m_dir;
};

#endif // CHAPTERSTORE_H
