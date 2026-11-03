#include "mainwindow.h"

#include <QApplication>
#include <QFile>
#include <QLocale>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QTranslator>

#include <cstdio>

// GUI 程序（WIN32 子系统）没有控制台，Qt 默认把 qWarning 丢给 OutputDebugString，
// 命令行下看不到。这里接管消息处理器写到 stderr，OpenGL 上下文失败这类问题才能被发现。
static void toStderr(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    const char *tag = "INFO";
    switch (type) {
    case QtDebugMsg:    tag = "DEBUG"; break;
    case QtInfoMsg:     tag = "INFO";  break;
    case QtWarningMsg:  tag = "WARN";  break;
    case QtCriticalMsg: tag = "ERROR"; break;
    case QtFatalMsg:    tag = "FATAL"; break;
    }
    fprintf(stderr, "[%s] %s\n", tag, msg.toUtf8().constData());
    fflush(stderr);
}

// 深色主题分两层：
//   1) Fusion 风格 + 深色调色板 —— 管住样式表没覆盖到的部分（下拉箭头、菜单、滚动条等原生控件）
//   2) theme.qss —— 管住具体外观（卡片、圆角、字号、按钮层级）
// 只改观感，不碰任何业务逻辑。
static void applyTheme(QApplication &app)
{
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        app.setStyle(fusion);

    QPalette pal;
    pal.setColor(QPalette::Window,          QColor(0x0b, 0x12, 0x20));
    pal.setColor(QPalette::WindowText,      QColor(0xe8, 0xee, 0xfb));
    pal.setColor(QPalette::Base,            QColor(0x0d, 0x16, 0x26));
    pal.setColor(QPalette::AlternateBase,   QColor(0x13, 0x1d, 0x33));
    pal.setColor(QPalette::Text,            QColor(0xe8, 0xee, 0xfb));
    pal.setColor(QPalette::Button,          QColor(0x1a, 0x27, 0x40));
    pal.setColor(QPalette::ButtonText,      QColor(0xd7, 0xe3, 0xf8));
    pal.setColor(QPalette::BrightText,      QColor(0xff, 0xff, 0xff));
    pal.setColor(QPalette::Highlight,       QColor(0x1d, 0x6f, 0xd6));
    pal.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    pal.setColor(QPalette::ToolTipBase,     QColor(0x10, 0x1c, 0x31));
    pal.setColor(QPalette::ToolTipText,     QColor(0xdf, 0xe9, 0xfa));
    pal.setColor(QPalette::PlaceholderText, QColor(0x6b, 0x7c, 0x9c));
    pal.setColor(QPalette::Disabled, QPalette::Text,       QColor(0x5a, 0x6a, 0x86));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x5a, 0x6a, 0x86));
    app.setPalette(pal);

    QFile qss(QStringLiteral(":/theme.qss"));
    if (qss.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
}

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsSet("AO_SELFTEST"))
        qInstallMessageHandler(toStderr);

    QApplication a(argc, argv);
    applyTheme(a);

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = "AoMonitor_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            a.installTranslator(&translator);
            break;
        }
    }
    MainWindow w;
    w.show();
    return QApplication::exec();
}
