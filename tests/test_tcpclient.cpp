// TcpClient 单元测试 —— 测试内起一个 QTcpServer 做"真实环回"，不 mock。
// 能 mock 的只有网络两端，环回连接本身是真的：握手、半包、断开全是操作系统行为。
#include <QtTest>

#include "tcpclient.h"
#include "frameparser.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>

class TestTcpClient : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void connectRefused();          // 连不上的端口 → errorOccurred 人话
    void connectAndParseFrames();   // server 发 3 帧 → FrameParser 吐 3 个值
    void halfPacketAcrossTcp();     // 半包分批到达 → 缓冲重组，值不丢
    void downlinkBytes();           // send() 下行 → server 收到原样字节
    void peerClosedSignals();       // 对端关闭 → disconnected 信号

private:
    QTcpServer *m_server = nullptr;
    quint16      m_port  = 0;
};

void TestTcpClient::initTestCase()
{
    m_server = new QTcpServer(this);
    QVERIFY(m_server->listen(QHostAddress::LocalHost));   // 随机可用端口，避免占 9000
    m_port = m_server->serverPort();
}

void TestTcpClient::cleanupTestCase()
{
    m_server->close();
}

// 连一个大概率没人听的端口：errorOccurred 必须带人话描述，UI 才能直接展示
void TestTcpClient::connectRefused()
{
    TcpClient c;
    QSignalSpy err(&c, &TcpClient::errorOccurred);

    c.connectTo(QStringLiteral("127.0.0.1"), 1);          // 端口 1 几乎必然拒绝
    QTRY_VERIFY(err.count() > 0);                          // 最多 5s，本机环回毫秒级

    const QString msg = err.first().first().toString();
    QVERIFY2(!msg.isEmpty(), "错误信息不能为空");
    QVERIFY2(!msg.contains(QStringLiteral("QAbstractSocket")),
             "错误信息应是人话，不该把枚举名抛给用户");
}

// 主链路：连接成功 → 收字节 → FrameParser 切出正确数值
void TestTcpClient::connectAndParseFrames()
{
    TcpClient c;
    FrameParser p;
    connect(&c, &TcpClient::bytesReceived, &p, &FrameParser::feed);

    QSignalSpy conn(&c, &TcpClient::connected);
    QSignalSpy frames(&p, &FrameParser::frameParsed);

    c.connectTo(QStringLiteral("127.0.0.1"), m_port);
    QTRY_VERIFY(conn.count() == 1);
    QCOMPARE(c.isConnected(), true);
    QCOMPARE(conn.first().first().toString(),
             QStringLiteral("127.0.0.1:%1").arg(m_port));

    // server 端接住这条连接，一次写 3 个完整帧
    QVERIFY(m_server->hasPendingConnections());
    QTcpSocket *peer = m_server->nextPendingConnection();
    QVERIFY(peer);
    const QByteArray payload = "$DATA,100#\r\n$DATA,2048#\r\n$DATA,4095#\r\n";
    peer->write(payload);                                  // 注意：帧长随位数变化，12+13+13=38

    QTRY_COMPARE(frames.count(), 3);
    QCOMPARE(frames.at(0).first().toLongLong(), qint64(100));
    QCOMPARE(frames.at(1).first().toLongLong(), qint64(2048));
    QCOMPARE(frames.at(2).first().toLongLong(), qint64(4095));
    QCOMPARE(c.bytesReceivedTotal(), qint64(payload.size()));

    peer->close();
    c.disconnectFromHost();
}

// TCP 分段是不可控的：一个帧可能被拆成任意几片到达。
// 验证半包场景下 FrameParser 的缓冲重组 —— 与串口共用同一套逻辑，这是抽 FrameParser 的意义
void TestTcpClient::halfPacketAcrossTcp()
{
    TcpClient c;
    FrameParser p;
    connect(&c, &TcpClient::bytesReceived, &p, &FrameParser::feed);

    QSignalSpy conn(&c, &TcpClient::connected);
    QSignalSpy frames(&p, &FrameParser::frameParsed);

    c.connectTo(QStringLiteral("127.0.0.1"), m_port);
    QTRY_VERIFY(conn.count() == 1);

    QTcpSocket *peer = m_server->nextPendingConnection();
    QVERIFY(peer);

    // 一帧切成三段发，中间隔开事件循环，模拟最恶劣的到达方式
    peer->write("$DA");
    peer->flush();
    QTRY_COMPARE(p.buffered(), 3);                         // 前导停在缓冲里等

    peer->write("TA,777");
    peer->flush();
    QTRY_COMPARE(p.buffered(), 9);

    peer->write("#\r\n$DATA,888#\r\n");                    // 半包+完整帧一起到
    peer->flush();

    QTRY_COMPARE(frames.count(), 2);
    QCOMPARE(frames.at(0).first().toLongLong(), qint64(777));
    QCOMPARE(frames.at(1).first().toLongLong(), qint64(888));

    peer->close();
    c.disconnectFromHost();
}

// 下行：send() 写出的字节必须原样到达对端（"查询一次"依赖这条链路）
void TestTcpClient::downlinkBytes()
{
    TcpClient c;
    QSignalSpy conn(&c, &TcpClient::connected);

    c.connectTo(QStringLiteral("127.0.0.1"), m_port);
    QTRY_VERIFY(conn.count() == 1);

    QTcpSocket *peer = m_server->nextPendingConnection();
    QVERIFY(peer);

    QSignalSpy rx(peer, &QTcpSocket::readyRead);
    c.send("?");
    QTRY_VERIFY(rx.count() >= 1);
    QCOMPARE(peer->readAll(), QByteArray("?"));

    peer->close();
    c.disconnectFromHost();
}

// 对端主动断开 → disconnected 信号（UI 状态胶囊要归位）
void TestTcpClient::peerClosedSignals()
{
    TcpClient c;
    QSignalSpy conn(&c, &TcpClient::connected);
    QSignalSpy gone(&c, &TcpClient::disconnected);

    c.connectTo(QStringLiteral("127.0.0.1"), m_port);
    QTRY_VERIFY(conn.count() == 1);

    QTcpSocket *peer = m_server->nextPendingConnection();
    QVERIFY(peer);
    peer->close();                                         // 对端断开

    QTRY_VERIFY(gone.count() >= 1);                        // 客户端必须感知到
    QCOMPARE(c.isConnected(), false);
    c.disconnectFromHost();
}

QTEST_MAIN(TestTcpClient)
#include "test_tcpclient.moc"
