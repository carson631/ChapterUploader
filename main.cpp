#include "mainwindow.h"

#include <QApplication>
#include <QFile>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

static void applyTheme(QApplication &app)
{
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        app.setStyle(fusion);

    QPalette pal;
    pal.setColor(QPalette::Window,          QColor(0xf4, 0xf6, 0xfa));
    pal.setColor(QPalette::WindowText,      QColor(0x1f, 0x29, 0x37));
    pal.setColor(QPalette::Base,            QColor(0xff, 0xff, 0xff));
    pal.setColor(QPalette::AlternateBase,   QColor(0xf8, 0xfa, 0xfd));
    pal.setColor(QPalette::Text,            QColor(0x1f, 0x29, 0x37));
    pal.setColor(QPalette::Button,          QColor(0xff, 0xff, 0xff));
    pal.setColor(QPalette::ButtonText,      QColor(0x1f, 0x29, 0x37));
    pal.setColor(QPalette::Highlight,       QColor(0x25, 0x63, 0xeb));
    pal.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    pal.setColor(QPalette::ToolTipBase,     QColor(0xff, 0xff, 0xff));
    pal.setColor(QPalette::ToolTipText,     QColor(0x1f, 0x29, 0x37));
    pal.setColor(QPalette::PlaceholderText, QColor(0x9c, 0xa3, 0xaf));
    app.setPalette(pal);

    QFile qss(QStringLiteral(":/theme.qss"));
    if (qss.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setApplicationName(QStringLiteral("ChapterUploader"));
    a.setOrganizationName(QStringLiteral("carson"));
    applyTheme(a);

    MainWindow w;
    w.show();
    return QApplication::exec();
}
