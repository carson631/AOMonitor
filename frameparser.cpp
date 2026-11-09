#include "frameparser.h"

#include <QString>

FrameParser::FrameParser(QObject *parent)
    : QObject(parent)
{
}

void FrameParser::feed(const QByteArray &bytes)
{
    m_rx += bytes;                                      // 先无脑累积
    if (m_rx.size() > kMaxBuffer)                       // 防止坏波特率下无限膨胀
        m_rx.remove(0, m_rx.size() - kMaxBuffer);
    parseFrames();
}

void FrameParser::reset()
{
    m_rx.clear();
}

void FrameParser::parseFrames()
{
    for (;;) {
        int start = m_rx.indexOf('$');
        if (start < 0) { m_rx.clear(); return; }        // 纯噪声，全丢
        if (start > 0)  m_rx.remove(0, start);          // 丢帧头之前的垃圾

        int end = m_rx.indexOf('#');
        if (end < 0) return;                            // 半包：留着等下一批数据

        QByteArray frame = m_rx.left(end + 1);          // "$DATA,xxxx#"
        m_rx.remove(0, end + 1);

        int comma = frame.indexOf(',');                 // $DATA,<数值>#
        if (comma < 0) continue;                        // 格式不对，丢弃这帧
        bool ok = false;
        qint64 value = frame.mid(comma + 1,
                                 frame.size() - comma - 2)  // 去掉结尾 '#'
                          .trimmed().toLongLong(&ok);
        if (!ok) continue;                              // 解析失败的坏帧，不显示

        emit frameParsed(value);
    }
}

QString FrameParser::formatValue(qint64 v)
{
    return QStringLiteral("%1").arg(v, 4, 10, QLatin1Char('0'));
}
