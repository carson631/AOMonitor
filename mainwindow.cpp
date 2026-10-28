#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QSerialPortInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QCoreApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QTextStream>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_serial(new QSerialPort(this))   // 挂到对象树，窗口析构时自动删除
{
    ui->setupUi(this);

    // 波特率下拉框：常用值，默认 9600（与 STM32 端 Serial.c 一致）
    const QList<qint32> bauds = {
        QSerialPort::Baud1200,  QSerialPort::Baud2400,  QSerialPort::Baud4800,
        QSerialPort::Baud9600,  QSerialPort::Baud19200, QSerialPort::Baud38400,
        QSerialPort::Baud57600, QSerialPort::Baud115200
    };
    for (qint32 b : bauds)
        ui->botelvBox->addItem(QString::number(b), b);
    ui->botelvBox->setCurrentText(QStringLiteral("9600"));

    refreshPorts();     // 启动时先扫一遍串口
    initDatabase();     // 打开 SQLite + 建表（仅启动时一次）

    // ---- 波形图 ----
    m_series = new QLineSeries(this);
    auto *chart = new QChart();
    chart->addSeries(m_series);
    chart->legend()->hide();
    chart->setTitle(QStringLiteral("AO 光照值"));

    m_axisX = new QValueAxis(this);
    m_axisX->setRange(0, 300);                      // 横轴：最近 300 个点
    auto *axisY = new QValueAxis(this);
    axisY->setRange(0, 4095);                       // 纵轴：12 位 ADC 满量程
    chart->addAxis(m_axisX, Qt::AlignBottom);
    chart->addAxis(axisY, Qt::AlignLeft);
    m_series->attachAxis(m_axisX);
    m_series->attachAxis(axisY);

    ui->chartArea->setChart(chart);
    ui->chartArea->setRenderHint(QPainter::Antialiasing);
    // 串口每来一批字节就触发 onReadyRead（相当于 STM32 的接收中断）
    connect(m_serial, &QSerialPort::readyRead, this, &MainWindow::onReadyRead);
}

MainWindow::~MainWindow()
{
    delete ui;          // m_serial 有父对象，不用手动 delete
}

// ---------- 数据库 ----------

void MainWindow::initDatabase()
{
    // SQLite：单文件、零部署。文件放 exe 同目录，避免"运行目录"不一致找不到文件
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));
    db.setDatabaseName(QCoreApplication::applicationDirPath() + QStringLiteral("/ao_data.db"));
    if (!db.open()) {
        ui->statusbar->showMessage(QStringLiteral("数据库打开失败：%1").arg(db.lastError().text()));
        return;
    }
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS light_log ("
            "id      INTEGER PRIMARY KEY AUTOINCREMENT,"
            "ts      TEXT    NOT NULL,"      // 采集时间
            "value   INTEGER NOT NULL)")))   // AO 光照值
        ui->statusbar->showMessage(QStringLiteral("建表失败：%1").arg(query.lastError().text()));
}

void MainWindow::insertRecord(qint64 value)
{
    QSqlDatabase db = QSqlDatabase::database();          // 取默认连接
    if (!db.isOpen()) return;

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO light_log (ts, value) VALUES (:ts, :value)"));
    insert.bindValue(":ts", QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    insert.bindValue(":value", value);
    if (!insert.exec())
        ui->statusbar->showMessage(QStringLiteral("入库失败：%1").arg(insert.lastError().text()));
}

void MainWindow::on_exportButton_clicked()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出数据"),
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss")) + QStringLiteral(".csv"),
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty()) return;                          // 用户点了取消

    QSqlDatabase db = QSqlDatabase::database();
    if (!db.isOpen()) {
        ui->statusbar->showMessage(QStringLiteral("数据库未打开"), 3000);
        return;
    }

    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT ts, value FROM light_log ORDER BY id"))) {
        ui->statusbar->showMessage(QStringLiteral("查询失败：%1").arg(query.lastError().text()), 5000);
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        ui->statusbar->showMessage(QStringLiteral("无法创建文件：%1").arg(file.errorString()), 5000);
        return;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out.setGenerateByteOrderMark(true);                  // 带 BOM，Excel 打开中文不乱码
    out << QStringLiteral("时间,光照值\n");
    qint64 rows = 0;
    while (query.next())
        out << query.value(0).toString() << ','
            << query.value(1).toString() << '\n', ++rows;
    file.close();

    ui->statusbar->showMessage(QStringLiteral("已导出 %1 条记录 → %2").arg(rows).arg(path), 5000);
}

void MainWindow::on_clearButton_clicked()
{
    const auto answer = QMessageBox::question(
        this, QStringLiteral("清空数据"),
        QStringLiteral("确定删除 light_log 表中的全部记录吗？此操作不可恢复。"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    QSqlQuery query(QSqlDatabase::database());
    if (!query.exec(QStringLiteral("DELETE FROM light_log"))) {
        ui->statusbar->showMessage(QStringLiteral("清空失败：%1").arg(query.lastError().text()), 5000);
        return;
    }
    ui->statusbar->showMessage(QStringLiteral("已清空全部记录"), 3000);
}

// ---------- 串口枚举 ----------

void MainWindow::refreshPorts()
{
    ui->chuankouBox->clear();

    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    if (ports.isEmpty()) {
        // 没插设备时给个可见的占位项，用户不会以为程序坏了
        ui->chuankouBox->addItem(QStringLiteral("未检测到串口"));
        ui->chuankouBox->setEnabled(false);
        return;
    }

    ui->chuankouBox->setEnabled(true);
    for (const QSerialPortInfo &info : ports) {
        QString label = info.portName();                 // Windows: "COM4"，Linux: "ttyUSB0"
        if (!info.description().isEmpty())
            label += QStringLiteral("  —  ") + info.description();  // 如 "USB-SERIAL CH340"
        ui->chuankouBox->addItem(label, info.portName()); // 显示带描述的全文，userData 存干净的端口名
    }
}

void MainWindow::on_refreshButton_clicked()
{
    refreshPorts();
}

// ---------- 打开 / 关闭串口 ----------

void MainWindow::on_openButton_clicked()
{
    if (m_serial->isOpen()) {                    // 已开着 → 这次点击是"关闭"
        m_serial->close();
        m_rx.clear();
        ui->openButton->setText(QStringLiteral("打开串口"));
        ui->statusbar->showMessage(QStringLiteral("串口已关闭"), 3000);
        return;
    }

    // 未检测到串口时 currentData 是无效的，取出来是空串
    const QString portName = ui->chuankouBox->currentData().toString();
    if (portName.isEmpty()) {
        ui->statusbar->showMessage(QStringLiteral("请先选择有效串口"), 3000);
        return;
    }

    m_serial->setPortName(portName);
    m_serial->setBaudRate(ui->botelvBox->currentData().toInt());
    m_serial->setDataBits(QSerialPort::Data8);          // 8
    m_serial->setParity(QSerialPort::NoParity);         // N
    m_serial->setStopBits(QSerialPort::OneStop);        // 1
    m_serial->setFlowControl(QSerialPort::NoFlowControl);

    m_rx.clear();                                       // 不带上一轮的残留数据
    if (!m_serial->open(QIODevice::ReadWrite)) {
        // 按错误类型给排查提示，和 Windows 上"拒绝访问"的经验对上
        QString why;
        if (m_serial->error() == QSerialPort::PermissionError)
            why = QStringLiteral("被其他程序占用（串口调试助手/另一个实例）");
        else if (m_serial->error() == QSerialPort::DeviceNotFoundError)
            why = QStringLiteral("设备不存在（可能已被拔出）");
        else
            why = m_serial->errorString();
        ui->statusbar->showMessage(QStringLiteral("打开失败：%1").arg(why), 5000);
        return;
    }

    ui->openButton->setText(QStringLiteral("关闭串口"));
    ui->statusbar->showMessage(QStringLiteral("%1 @ %2 已打开")
                                   .arg(portName, ui->botelvBox->currentText()));
}

// ---------- 主动查询一帧 ----------

void MainWindow::on_searchButton_clicked()
{
    if (!m_serial->isOpen()) {
        ui->statusbar->showMessage(QStringLiteral("串口未打开"), 3000);
        return;
    }
    // STM32 端约定：收到任意字节立即补发一帧 $DATA,<AO>#
    // 置位标志 → 下一帧解析成功即入库（只记"查询一次"的值，不记 200ms 自动帧）
    m_pendingLog = true;
    m_serial->write("?");
}

// ---------- 收数据与切帧 ----------

void MainWindow::onReadyRead()
{
    m_rx += m_serial->readAll();                        // 先无脑累积
    if (m_rx.size() > 4096)                             // 防止坏波特率下无限膨胀
        m_rx.remove(0, m_rx.size() - 4096);
    parseFrames();
}

void MainWindow::parseFrames()
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

        m_lastValue = value;
        ui->AONumber->display(static_cast<double>(value));

        // 仅"查询一次"请求的下一帧才入库 + 刷新 AONumber_2
        if (m_pendingLog) {
            m_pendingLog = false;
            insertRecord(value);
            ui->AONumber_2->display(static_cast<double>(value));
        }
        m_series->append(m_pointCount++, value);

        if (m_series->count() > 300) {                  // 滚动窗口：超过 300 点
            m_series->removePoints(0, m_series->count() - 300);
            m_axisX->setRange(m_series->at(0).x(),      // 窗口左端 = 最老点的 x
                              m_series->at(0).x() + 299); // 右端跟着左移，窗口宽度不变
        }
    }
}

