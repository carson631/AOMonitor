#ifndef TCPCLIENT_H
#define TCPCLIENT_H

#include <QObject>
#include <QAbstractSocket>

class QTcpSocket;

// TCP 数据源 —— 与 QSerialPort 同级的数据入口。
//
// 为什么单独抽一类（与 FrameParser 抽类的理由一致）：
//   - 串口和 TCP 对上层都是"字节流"，切帧逻辑完全共用 FrameParser；
//   - TcpClient 只负责连接管理与吐字节，不碰任何 Widget，
//     可以配一个 QTcpServer 在测试里做真实环回，不需要界面。
//
// 协议与串口完全相同：$DATA,<数值>#，只是传输层换成了 TCP。
class TcpClient : public QObject
{
    Q_OBJECT

public:
    explicit TcpClient(QObject *parent = nullptr);
    ~TcpClient() override;

    bool isConnected() const;

    QString peer() const;               // "127.0.0.1:9000"，未连接返回空串
    qint64  bytesReceivedTotal() const { return m_total; }   // 累计收到的字节数（状态栏/测试用）

public slots:
    void connectTo(const QString &host, quint16 port);
    void disconnectFromHost();
    void send(const QByteArray &bytes); // 对应串口的"查询一次"：主动写下行字节

signals:
    void connected(const QString &peer);          // 握手完成（三次握手后才会发，不是 connectTo 返回）
    void disconnected();
    void bytesReceived(const QByteArray &bytes);  // 每批新到达的字节，喂给 FrameParser::feed
    void errorOccurred(const QString &message);   // 连不上/被拒/超时等人话描述

private:
    QTcpSocket *m_socket = nullptr;
    qint64      m_total  = 0;
};

#endif // TCPCLIENT_H
