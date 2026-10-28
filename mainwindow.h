#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSerialPort>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void on_refreshButton_clicked();    // 刷新串口
    void on_openButton_clicked();       // 打开/关闭串口（二次点击为关闭）
    void on_searchButton_clicked();     // 主动要一帧：发任意字节，STM32 立即补发（并入库）
    void on_exportButton_clicked();     // 导出数据库为 CSV
    void on_clearButton_clicked();      // 清空数据库
    void onReadyRead();                 // 收到字节 → 进缓冲 → 切帧

private:
    void refreshPorts();                // 枚举可用串口，填充 chuankouBox
    void parseFrames();                 // 从缓冲中切出 $DATA,<AO># 帧并显示
    void initDatabase();                // 打开 SQLite + 建表（仅启动时一次）
    void insertRecord(qint64 value);    // 写一条记录

    Ui::MainWindow *ui;
    QSerialPort *m_serial = nullptr;    // 以 this 为父对象，窗口析构自动回收
    QByteArray m_rx;                    // 接收缓冲（字节流 ≠ 消息流，必须缓存切帧）
    qint64 m_lastValue = -1;            // 最近一次成功解析的光照值
    QLineSeries *m_series = nullptr;
    QValueAxis  *m_axisX  = nullptr;
    qint64 m_pointCount = 0;
    bool m_pendingLog = false;          // 点了"查询一次"后置位：下一帧解析成功即入库
};
#endif // MAINWINDOW_H
