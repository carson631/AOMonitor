#ifndef FRAMEPARSER_H
#define FRAMEPARSER_H

#include <QByteArray>
#include <QObject>

// 串口帧解析器 —— 从 MainWindow 抽出的纯逻辑类。
//
// 协议：$DATA,<数值>#  （'$' 帧头，'#' 帧尾，逗号后是要解析的整数）
//
// 设计要点（也是可测性的来源）：
//   - 不依赖任何 Widget：只吃 QByteArray，吐 frameParsed 信号，
//     因此可以脱离界面、脱离 QApplication 单独测试；
//   - 串口是字节流不是消息流：数据分批到达时缓冲区累积、半包等待、
//     找到帧头后丢弃前导垃圾，逻辑与 MainWindow 原实现逐行等价；
//   - 缓冲上限 4096 字节：坏波特率等异常下缓冲不会无限膨胀。
class FrameParser : public QObject
{
    Q_OBJECT

public:
    explicit FrameParser(QObject *parent = nullptr);

    // 喂入一批新到达的字节（对应 readyRead 里的 readAll()）
    void feed(const QByteArray &bytes);

    // 清空接收缓冲（对应重新打开串口时的残留清理）
    void reset();

    // 当前缓冲里的待切字节数（测试半包状态用）
    int buffered() const { return m_rx.size(); }

    // 4 位定宽显示（12 位 ADC 最大 4095），零填充防显示位数跳动
    static QString formatValue(qint64 v);

signals:
    // 每解析出一帧有效数据发一次，参数是数值部分
    void frameParsed(qint64 value);

private:
    void parseFrames();

    QByteArray m_rx;                    // 接收缓冲（字节流 ≠ 消息流，必须缓存切帧）
    static constexpr int kMaxBuffer = 4096;
};

#endif // FRAMEPARSER_H
