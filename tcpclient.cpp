#include "tcpclient.h"

#include <QTcpSocket>
#include <QTimer>

TcpClient::TcpClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))    // 挂对象树，随父对象销毁
{
    // connected/disconnected 是异步信号：connectTo() 返回时连接还没建立，
    // 必须等信号来了才能开始收发，UI 状态切换也挂在信号上而不是挂在调用处
    connect(m_socket, &QTcpSocket::connected, this, [this]() {
        m_total = 0;
        emit connected(peer());
    });
    connect(m_socket, &QTcpSocket::disconnected,
            this, &TcpClient::disconnected);

    connect(m_socket, &QTcpSocket::readyRead, this, [this]() {
        const QByteArray bytes = m_socket->readAll();
        if (bytes.isEmpty())
            return;
        m_total += bytes.size();
        emit bytesReceived(bytes);
    });

    // Qt 6.6 起 errorOccurred 信号带 QAbstractSocket::SocketError，
    // 这里统一翻译成人话再抛出去，UI 层不接触枚举
    connect(m_socket, &QAbstractSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError err) {
        QString why;
        switch (err) {
        case QAbstractSocket::ConnectionRefusedError:
            why = QStringLiteral("连接被拒绝（模拟器没启动？端口对吗？）");
            break;
        case QAbstractSocket::HostNotFoundError:
            why = QStringLiteral("主机不可解析（检查地址）");
            break;
        case QAbstractSocket::SocketTimeoutError:
            why = QStringLiteral("连接超时");
            break;
        case QAbstractSocket::RemoteHostClosedError:
            why = QStringLiteral("对端已断开");
            break;
        default:
            why = m_socket->errorString();
            break;
        }
        emit errorOccurred(why);
    });
}

TcpClient::~TcpClient() = default;

bool TcpClient::isConnected() const
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

QString TcpClient::peer() const
{
    if (!m_socket || m_socket->peerAddress().isNull())
        return QString();
    return QStringLiteral("%1:%2")
        .arg(m_socket->peerAddress().toString())
        .arg(m_socket->peerPort());
}

void TcpClient::connectTo(const QString &host, quint16 port)
{
    // 重复点击防护：已连接或正在连接先断开，避免 socket 状态混乱
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
    m_socket->connectToHost(host, port);
}

void TcpClient::disconnectFromHost()
{
    m_socket->disconnectFromHost();
}

void TcpClient::send(const QByteArray &bytes)
{
    if (isConnected())
        m_socket->write(bytes);
}
