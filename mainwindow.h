#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QElapsedTimer>
#include <QMainWindow>
#include <QSerialPort>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class WaveformGLWidget;
class QTimer;
class FrameParser;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    // 挂在图表 viewport 上数 Paint 事件，用来统计 QtCharts 的真实刷新帧率
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void on_refreshButton_clicked();    // 刷新串口
    void on_openButton_clicked();       // 打开/关闭串口（二次点击为关闭）
    void on_searchButton_clicked();     // 主动要一帧：发任意字节，STM32 立即补发（并入库）
    void on_exportButton_clicked();     // 导出数据库为 CSV
    void on_clearButton_clicked();      // 清空数据库
    void on_stressButton_clicked();     // 压测开关：合成数据灌给两边，放大性能差异
    void onReadyRead();                 // 收到字节 → 喂给 FrameParser 切帧
    void onFrameParsed(qint64 value);   // FrameParser 切出一帧 → 显示/入库/喂波形

private:
    void refreshPorts();                // 枚举可用串口，填充 chuankouBox
    void initDatabase();                // 打开 SQLite + 建表（仅启动时一次）
    void insertRecord(qint64 value);    // 写一条记录

    void addSample(qint64 value);       // 同一份数据同时喂给 QtCharts 与 OpenGL 波形
    void updatePerfLabel();             // 刷新两侧卡片的帧率/耗时文字
    void setConnectionUi(bool connected, const QString &detail);   // 状态胶囊 + 打开按钮联动

    Ui::MainWindow *ui;
    QSerialPort *m_serial = nullptr;    // 以 this 为父对象，窗口析构自动回收
    FrameParser *m_parser = nullptr;    // 切帧逻辑在纯逻辑类里，可脱离界面单测
    qint64 m_lastValue = -1;            // 最近一次成功解析的光照值
    QLineSeries *m_series = nullptr;
    QValueAxis  *m_axisX  = nullptr;
    qint64 m_pointCount = 0;
    bool m_pendingLog = false;          // 点了"查询一次"后置位：下一帧解析成功即入库

    WaveformGLWidget *m_glWidget = nullptr;
    QObject *m_chartViewport = nullptr; // chartArea->viewport()，接收 Paint 事件计数

    QTimer *m_stressTimer = nullptr;    // 压测定时器
    int     m_stressLevel = 0;          // 0 关 / 1 低 / 2 中 / 3 高
    int     m_stressBatch = 0;          // 每次定时器触发灌多少点
    qint64  m_stressSeq   = 0;

    // 帧率统计
    QElapsedTimer m_chartTimer;
    QElapsedTimer m_ingestTimer;        // 计时：QtCharts 的单点入列成本
    qint64 m_chartUs     = 0;           // 本统计窗口内图表数据处理累计耗时（微秒）
    int    m_chartFrames = 0;
    double m_chartFps    = 0.0;
    double m_chartMs     = 0.0;         // QtCharts 每帧平均数据处理耗时
    double m_glFps       = 0.0;
    double m_glMs        = 0.0;
};
#endif // MAINWINDOW_H
