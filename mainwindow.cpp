#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "waveformglwidget.h"
#include "frameparser.h"
#include "tcpclient.h"

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
#include <QTimer>
#include <QtMath>
#include <QWindow>
#include <QFont>
#include <QMargins>
#include <QPen>
#include <QStyle>

#include <cstdio>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_serial(new QSerialPort(this))   // 挂到对象树，窗口析构时自动删除
    , m_tcp(new TcpClient(this))        // TCP 数据源：与串口同级，喂同一个 FrameParser
    , m_parser(new FrameParser(this))   // 切帧与 UI 解耦：Parser 发信号，这里只管展示
{
    ui->setupUi(this);

    // 每切出一帧有效数据 → 显示、按需入库、喂两个波形
    connect(m_parser, &FrameParser::frameParsed,
            this, &MainWindow::onFrameParsed);

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
    setConnectionUi(false, QStringLiteral("串口未连接"));

    // ---- 波形图 ----
    // 背景交给卡片，图表本身透明；配色按深色主题调，避免白底刺眼
    m_series = new QLineSeries(this);
    m_series->setPen(QPen(QColor(0x38, 0xbd, 0xf8), 2));

    auto *chart = new QChart();
    chart->addSeries(m_series);
    chart->legend()->hide();
    chart->setBackgroundVisible(false);
    chart->setPlotAreaBackgroundVisible(false);
    chart->setAnimationOptions(QChart::NoAnimation);      // 实时波形不要动画，省 CPU
    chart->setMargins(QMargins(0, 6, 0, 0));

    m_axisX = new QValueAxis(this);
    m_axisX->setRange(0, 300);                      // 横轴：最近 300 个点
    m_axisX->setLabelsVisible(false);               // 滚动窗口没有固定时间轴，标数字只会晃眼
    m_axisX->setGridLineVisible(false);
    m_axisX->setLinePenColor(QColor(0x26, 0x35, 0x4f));

    auto *axisY = new QValueAxis(this);
    axisY->setRange(0, 4095);                       // 纵轴：12 位 ADC 满量程
    axisY->setTickCount(5);
    axisY->setLabelFormat(QStringLiteral("%d"));
    axisY->setGridLineColor(QColor(0x1b, 0x27, 0x40));
    axisY->setLinePenColor(QColor(0x26, 0x35, 0x4f));
    axisY->setLabelsColor(QColor(0x7f, 0x92, 0xb4));
    axisY->setLabelsFont(QFont(QStringLiteral("Consolas"), 8));

    chart->addAxis(m_axisX, Qt::AlignBottom);
    chart->addAxis(axisY, Qt::AlignLeft);
    m_series->attachAxis(m_axisX);
    m_series->attachAxis(axisY);

    ui->chartArea->setChart(chart);
    ui->chartArea->setRenderHint(QPainter::Antialiasing);
    ui->chartArea->setFrameShape(QFrame::NoFrame);
    ui->chartArea->setStyleSheet(QStringLiteral("background: transparent; border: none;"));

    // ---- OpenGL 波形：与 QtCharts 并排，喂同一份数据 ----
    m_glWidget = ui->glArea;
    m_glWidget->setWindowSize(300);          // 窗口点数与图表一致，才有可比性
    ui->chartSplitLayout->setStretch(0, 1);  // 图表与 GL 各占一半；不设 stretch 时
    ui->chartSplitLayout->setStretch(1, 1);  // GL 控件的 sizeHint 为 0，宽度会被压成 0
    ui->chartCardLayout->setStretch(1, 1);   // 卡片里第 1 项是波形本体：多余高度全给它，
    ui->glCardLayout->setStretch(1, 1);      // 否则会被上下两行文字分走，波形区留白

    connect(m_glWidget, &WaveformGLWidget::glReady, this, [this](const QString &info){
        ui->statusbar->showMessage(info, 8000);   // 启动时打印 GL 版本/渲染器，出问题好排查
        fprintf(stderr, "[gl] %s\n", info.toUtf8().constData());
        fflush(stderr);
    });
    connect(m_glWidget, &WaveformGLWidget::statsUpdated, this, [this](double fps, double ms){
        m_glFps = fps;
        m_glMs  = ms;
        updatePerfLabel();
    });

    // QtCharts 内部 paint 无法直接计时，改为数 viewport 的 Paint 事件算真实刷新率
    m_chartViewport = ui->chartArea->viewport();
    m_chartViewport->installEventFilter(this);
    m_chartTimer.start();
    updatePerfLabel();

    // 串口每来一批字节就触发 onReadyRead（相当于 STM32 的接收中断）
    connect(m_serial, &QSerialPort::readyRead, this, &MainWindow::onReadyRead);

    // ---- TCP 数据源：与串口走完全相同的切帧管线 ----
    // 字节到了喂 FrameParser；连接/断开只切换 UI 状态与解析器清零
    connect(m_tcp, &TcpClient::bytesReceived, this, [this](const QByteArray &bytes) {
        m_parser->feed(bytes);
    });
    connect(m_tcp, &TcpClient::connected, this, [this](const QString &peer) {
        m_parser->reset();                              // 不带上一轮残留
        setConnectionUi(true, QStringLiteral("TCP %1").arg(peer));
        ui->statusbar->showMessage(QStringLiteral("已连接 %1，等待 $DATA 帧…").arg(peer), 5000);
    });
    connect(m_tcp, &TcpClient::disconnected, this, [this]() {
        setConnectionUi(false, QStringLiteral("TCP 未连接"));
        ui->statusbar->showMessage(QStringLiteral("TCP 已断开"), 3000);
    });
    connect(m_tcp, &TcpClient::errorOccurred, this, [this](const QString &why) {
        setConnectionUi(false, QStringLiteral("TCP 未连接"));
        ui->statusbar->showMessage(QStringLiteral("TCP 错误：%1").arg(why), 8000);
    });

    // 数据源切换：串口组 / TCP 组互斥
    connect(ui->sourceBox, &QComboBox::currentIndexChanged,
            this, &MainWindow::setSourceUi);
    setSourceUi(0);                                     // 启动时按默认项摆好两组控件

    // 自测模式（无人值守验证）：AO_AUTO_TCP=host:port 启动后自动连 TCP 数据源，
    // 用于与 AOSimulator 做端到端联调（AO_SELFTEST 仍可叠加）
    const QString autoTcp = qEnvironmentVariable("AO_AUTO_TCP");
    if (!autoTcp.isEmpty()) {
        const int colon = autoTcp.lastIndexOf(QLatin1Char(':'));
        const QString host = colon > 0 ? autoTcp.left(colon) : autoTcp;
        const quint16 port = colon > 0 ? autoTcp.mid(colon + 1).toUShort() : 9000;
        QTimer::singleShot(600, this, [this, host, port]() {
            ui->sourceBox->setCurrentIndex(1);          // 触发 setSourceUi
            m_tcp->connectTo(host, port);
        });
    }

    // 自测模式（无人值守验证）：AO_SELFTEST=<秒数>，自动开压测、每秒打一次帧率、到点退出
    const int selfTestSec = qEnvironmentVariableIntValue("AO_SELFTEST");
    if (selfTestSec > 0) {
        const int lv = qEnvironmentVariableIntValue("AO_STRESS_LEVEL");   // 1 低 / 2 中 / 3 高
        const int clicks = (lv >= 1 && lv <= 3) ? lv : 1;
        QTimer::singleShot(500, this, [this, clicks]() {
            for (int i = 0; i < clicks; ++i)
                on_stressButton_clicked();
        });

        auto *report = new QTimer(this);
        report->setInterval(1000);
        connect(report, &QTimer::timeout, this, [this]() {
            fprintf(stderr, "[selftest] QtCharts=%.1f fps / %.2f ms    OpenGL=%.1f fps / %.3f ms\n",
                    m_chartFps, m_chartMs, m_glFps, m_glMs);
            fflush(stderr);
        });
        report->start();

        QTimer::singleShot(selfTestSec * 1000, qApp, &QCoreApplication::quit);

        // 可选：退出前抓图验证渲染（AO_SCREENSHOT=<路径>）
        const QString shot = qEnvironmentVariable("AO_SCREENSHOT");
        if (!shot.isEmpty()) {
            QTimer::singleShot(selfTestSec * 1000 - 300, this, [this, shot]() {
                grab().save(shot);
                m_glWidget->grabFramebuffer().save(shot + ".gl.png");
                fprintf(stderr, "[selftest] 截图已保存：%s\n", shot.toUtf8().constData());
                fflush(stderr);
            });
        }
    }
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
        m_parser->reset();
        setConnectionUi(false, QStringLiteral("串口未连接"));
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

    m_parser->reset();                                  // 不带上一轮的残留数据
    if (!m_serial->open(QIODevice::ReadWrite)) {
        // 按错误类型给排查提示，和 Windows 上"拒绝访问"的经验对上
        QString why;
        if (m_serial->error() == QSerialPort::PermissionError)
            why = QStringLiteral("被其他程序占用（串口调试助手/另一个实例）");
        else if (m_serial->error() == QSerialPort::DeviceNotFoundError)
            why = QStringLiteral("设备不存在（可能已被拔出）");
        else
            why = m_serial->errorString();
        setConnectionUi(false, QStringLiteral("打开失败"));
        ui->statusbar->showMessage(QStringLiteral("打开失败：%1").arg(why), 5000);
        return;
    }

    setConnectionUi(true, QStringLiteral("%1 @ %2")
                            .arg(portName, ui->botelvBox->currentText()));
    ui->statusbar->showMessage(QStringLiteral("%1 @ %2 已打开")
                                   .arg(portName, ui->botelvBox->currentText()));
}

// 状态胶囊与"打开/关闭串口"按钮同步；connected 是动态属性，改完必须重新抛光
void MainWindow::setConnectionUi(bool connected, const QString &detail)
{
    ui->statusDot->setStyleSheet(connected ? QStringLiteral("color:#22c55e;")
                                           : QStringLiteral("color:#5b6b87;"));
    ui->statusLabel->setText(detail);

    // 当前数据源是 TCP 时，状态胶囊挂的是 tcpButton，别把 openButton 的文案改乱
    if (ui->sourceBox->currentIndex() == 1) {
        ui->tcpButton->setText(connected ? QStringLiteral("断开 TCP")
                                         : QStringLiteral("连接 TCP"));
    } else {
        ui->openButton->setText(connected ? QStringLiteral("关闭串口")
                                          : QStringLiteral("打开串口"));
        ui->openButton->setProperty("connected", connected);
        ui->openButton->style()->unpolish(ui->openButton);
        ui->openButton->style()->polish(ui->openButton);
        ui->openButton->update();
    }
}

// ---------- 数据源切换 ----------

void MainWindow::setSourceUi(int index)
{
    const bool serialTab = (index == 0);

    // 两组控件互斥"显示"而不是只禁用：两组合计约 1540px，卡片只有 1160px，
    // 同时显示会让布局把控件压到重叠（串口下拉框的长文本会糊到"波特率"上）
    const QList<QWidget *> serialWidgets = {
        ui->hSep1, ui->label_2, ui->chuankouBox, ui->refreshButton,
        ui->label_3, ui->botelvBox, ui->openButton, ui->hSep2
    };
    for (QWidget *w : serialWidgets)
        w->setVisible(serialTab);

    const QList<QWidget *> tcpWidgets = {
        ui->hostLabel, ui->hostEdit, ui->portLabel, ui->portSpin, ui->tcpButton
    };
    for (QWidget *w : tcpWidgets)
        w->setVisible(!serialTab);

    // 右侧说明文字原来占一行宽度，窗口缩小时会引发布局溢出重叠，
    // 改成卡片 tooltip：信息还在，不占布局
    ui->deviceCard->setToolTip(serialTab
        ? QStringLiteral("8 数据位 · 无校验 · 1 停止位")
        : QStringLiteral("TCP Client · 连 AOSimulator 模拟器"));

    // 切走时把另一路断干净：连接态只允许存在一个数据源
    if (!serialTab && m_serial->isOpen()) {
        m_serial->close();
        m_parser->reset();
        ui->statusbar->showMessage(QStringLiteral("已切到 TCP，串口已关闭"), 3000);
    } else if (serialTab && m_tcp->isConnected()) {
        m_tcp->disconnectFromHost();
        ui->statusbar->showMessage(QStringLiteral("已切到串口，TCP 已断开"), 3000);
    }

    // 摆正状态胶囊
    if (m_serial->isOpen()) {
        setConnectionUi(true, QStringLiteral("%1 @ %2")
                                .arg(ui->chuankouBox->currentData().toString(),
                                     ui->botelvBox->currentText()));
    } else if (m_tcp->isConnected()) {
        setConnectionUi(true, QStringLiteral("TCP %1").arg(m_tcp->peer()));
    } else {
        setConnectionUi(false, serialTab ? QStringLiteral("串口未连接")
                                         : QStringLiteral("TCP 未连接"));
    }
}

// ---------- 连接 / 断开 TCP ----------

void MainWindow::on_tcpButton_clicked()
{
    if (m_tcp->isConnected()) {                 // 已连接 → 这次点击是"断开"
        m_tcp->disconnectFromHost();
        return;
    }

    const QString host = ui->hostEdit->text().trimmed();
    if (host.isEmpty()) {
        ui->statusbar->showMessage(QStringLiteral("请先填写模拟器地址"), 3000);
        return;
    }

    // connectToHost 是异步的：成功/失败都走 TcpClient 的信号，这里不判断结果
    ui->statusbar->showMessage(
        QStringLiteral("正在连接 %1:%2 …").arg(host).arg(ui->portSpin->value()), 5000);
    m_tcp->connectTo(host, static_cast<quint16>(ui->portSpin->value()));
}

// ---------- 主动查询一帧 ----------

void MainWindow::on_searchButton_clicked()
{
    // 两个数据源都遵循"收到任意字节立即补发一帧"的约定；
    // TCP 模式下 AOSimulator 不响应下行字节，但发送本身无害
    if (m_serial->isOpen()) {
        m_pendingLog = true;
        m_serial->write("?");
    } else if (m_tcp->isConnected()) {
        m_pendingLog = true;
        m_tcp->send("?");
        // 模拟器是单向广播，不理会下行字节；下一帧自然到达时照样入库
        ui->statusbar->showMessage(
            QStringLiteral("已发送查询字节，下一帧到达即入库（模拟器不响应下行）"), 4000);
    } else {
        ui->statusbar->showMessage(QStringLiteral("串口/TCP 均未连接"), 3000);
    }
}

// 4 位定宽显示（12 位 ADC 最大 4095），像仪表一样零填充，位数变化时单位不会左右跳
// （实现挪进 FrameParser::formatValue，逻辑类可测）

// ---------- 收数据与切帧 ----------

void MainWindow::onReadyRead()
{
    m_parser->feed(m_serial->readAll());                // 缓冲、切帧都在 Parser 里
}

void MainWindow::onFrameParsed(qint64 value)
{
    m_lastValue = value;
    ui->AONumber->setText(FrameParser::formatValue(value));

    // 仅"查询一次"请求的下一帧才入库 + 刷新 AONumber_2
    if (m_pendingLog) {
        m_pendingLog = false;
        insertRecord(value);
        ui->AONumber_2->setText(FrameParser::formatValue(value));
    }
    addSample(value);                                   // 同一份数据喂给两个波形控件
}

// ---------- 数据分发：两份波形吃同一份数据 ----------

void MainWindow::addSample(qint64 value)
{
    // QtCharts 路线：每点一个 QGraphicsItem，append/removePoints 都要维护内部索引
    // 这段耗时是 QtCharts 的固有成本，单独累计出来作为"每帧处理成本"的量化指标
    m_ingestTimer.start();
    m_series->append(m_pointCount++, value);
    if (m_series->count() > 300) {
        m_series->removePoints(0, m_series->count() - 300);
        m_axisX->setRange(m_series->at(0).x(),         // 窗口左端 = 最老点的 x
                          m_series->at(0).x() + 299);  // 右端跟着左移，窗口宽度不变
    }
    m_chartUs += m_ingestTimer.nsecsElapsed() / 1000;

    // OpenGL 路线：只写进环形缓冲，绘制时一次 subData + 一次 drawArrays
    if (m_glWidget)
        m_glWidget->appendValue(value);
}

// ---------- 帧率统计 ----------

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_chartViewport && event->type() == QEvent::Paint) {
        ++m_chartFrames;
        const qint64 ms = m_chartTimer.elapsed();
        if (ms >= 500) {                                // 500ms 一个统计窗口，避免抖动
            m_chartFps = m_chartFrames * 1000.0 / double(ms);
            m_chartMs  = m_chartFrames > 0
                             ? double(m_chartUs) / 1000.0 / double(m_chartFrames)
                             : 0.0;
            m_chartFrames = 0;
            m_chartUs = 0;
            m_chartTimer.restart();
            updatePerfLabel();
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::updatePerfLabel()
{
    auto num = [](double v) {
        return v >= 10.0 ? QString::number(v, 'f', 1) : QString::number(v, 'f', 2);
    };
    // 两个卡片脚注各显示自己的数字，视线不用来回跳
    ui->chartStat->setText(QStringLiteral("300 点窗口　%1 fps　%2 ms/帧")
                               .arg(num(m_chartFps), num(m_chartMs)));
    ui->glStat->setText(QStringLiteral("300 点窗口　%1 fps　%2 ms/帧")
                            .arg(num(m_glFps), QString::number(m_glMs, 'f', 3)));
}

// ---------- 压测 ----------

void MainWindow::on_stressButton_clicked()
{
    // 三档压测：点一次升一档，循环。档位越高，QtCharts 退化越明显。
    //   低 ≈ 2k 点/秒   中 ≈ 3k 点/秒（单次 60 点）   高 ≈ 25k 点/秒（单次 500 点）
    static const int    kInterval[] = { 5, 5, 20, 20 };
    static const int    kBatch[]    = { 0, 10, 60, 500 };
    static const char  *kLabel[]    = { "关", "低", "中", "高" };

    m_stressLevel = (m_stressLevel + 1) % 4;

    if (!m_stressTimer) {
        m_stressTimer = new QTimer(this);
        connect(m_stressTimer, &QTimer::timeout, this, [this]() {
            qint64 v = 0;
            for (int i = 0; i < m_stressBatch; ++i) {
                const double t = double(m_stressSeq++) * 0.03;   // 相位步长固定，波形才看得出起伏
                v = qBound(qint64(0), qint64(2048 + 1600 * qSin(t) + 200 * qSin(t * 7.3)), qint64(4095));
                addSample(v);
            }
            ui->AONumber->setText(FrameParser::formatValue(v));  // 压测时大数字也跟着跳
        });
    }

    m_stressBatch = kBatch[m_stressLevel];
    if (m_stressLevel == 0) {
        m_stressTimer->stop();
    } else {
        m_stressTimer->setInterval(kInterval[m_stressLevel]);
        m_stressTimer->start();
    }
    ui->stressButton->setText(QStringLiteral("压测：%1")
                                  .arg(QString::fromUtf8(kLabel[m_stressLevel])));
}

