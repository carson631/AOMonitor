#ifndef WAVEFORMGLWIDGET_H
#define WAVEFORMGLWIDGET_H

#include <QElapsedTimer>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QVector>

class QTimer;

// GPU 波形控件。
//
// 与 QPainter / QtCharts 的本质区别：
//   QtCharts ：每个点变成 QGraphicsItem 走 CPU 光栅化，点越多越慢
//   本控件   ：点写进环形 VBO，一帧一次 glBufferSubData + 一次 glDrawArrays，
//              坐标变换写在顶点着色器里，CPU 只负责"搬数据"
class WaveformGLWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core
{
    Q_OBJECT

public:
    explicit WaveformGLWidget(QWidget *parent = nullptr);
    ~WaveformGLWidget() override;

    void setWindowSize(int points);        // 可见窗口点数（与图表保持一致）
    int  windowSize() const { return m_window; }

public slots:
    void appendValue(qint64 value);        // 追加一个采样点
    void clearData();                      // 清空缓冲

signals:
    void statsUpdated(double fps, double avgMs);   // 每 500ms 报一次渲染帧率
    void glReady(const QString &info);              // 初始化结果（版本/渲染器/失败原因）

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    static constexpr int kCap = 65536;     // 环形缓冲容量（顶点数，交错 vec2 → 512KB）
    void uploadPending();                  // 只上传上次绘制之后新写入的区间
    void drawGrid();                       // 背景网格：第二套最简着色器，坐标已是 NDC

    QOpenGLShaderProgram     m_program;
    QOpenGLBuffer            m_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject m_vao;

    QOpenGLShaderProgram     m_gridProgram;   // 网格只做"投影后直接输出"，不需要数据坐标变换
    QOpenGLBuffer            m_gridVbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject m_gridVao;
    int  m_gridVerts  = 0;
    bool m_gridOk     = false;

    QVector<float> m_verts;                // 交错布局 [x0,y0,x1,y1,...]，x 固定为槽位号
    int m_head   = 0;                      // 下一个写入槽位
    int m_count  = 0;                      // 有效点数（<= kCap）
    int m_window = 300;                    // 可见点数

    int m_appendFirst = 0;                 // 上次绘制后首个写入槽位
    int m_appendTotal = 0;                 // 上次绘制后写入总数

    QElapsedTimer m_statsTimer;
    QElapsedTimer m_frameTimer;
    QElapsedTimer m_lastData;              // 最近一次收到数据的时刻
    QTimer *m_repaintTimer = nullptr;      // 60Hz 重绘节拍（有数据时才跑）
    int    m_frames  = 0;
    qint64 m_frameUs = 0;
    bool   m_glOk    = false;
};

#endif // WAVEFORMGLWIDGET_H
