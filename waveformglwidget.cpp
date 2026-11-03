#include "waveformglwidget.h"

#include <QSurfaceFormat>
#include <QTimer>
#include <QtMath>

// 顶点着色器：把"槽位号 + 采样值"直接映射到 NDC
//   - 水平：用 u_left / u_span 做窗口平移，滚动时只改 uniform，不重建顶点
//   - 垂直：0..4095 固定映射（与图表纵轴一致）
//   - u_xOffset：环形缓冲跨尾时，第二段整体 +kCap，保证两段接得上
static const char *kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aPos;

uniform float u_xOffset;
uniform float u_left;
uniform float u_span;
uniform float u_yMin;
uniform float u_yMax;
uniform float u_yOffset;   // 垂直微偏移，多画几遍凑出线宽

void main()
{
    float x = (aPos.x + u_xOffset - u_left) / u_span;
    float y = (aPos.y - u_yMin) / (u_yMax - u_yMin);
    gl_Position = vec4(x * 2.0 - 1.0, y * 2.0 - 1.0 + u_yOffset, 0.0, 1.0);
}
)";

static const char *kFragmentShader = R"(#version 330 core
uniform vec3 u_color;
out vec4 fragColor;

void main()
{
    fragColor = vec4(u_color, 1.0);
}
)";

// 网格用的一套极简管线的着色器：顶点已经是 NDC，只做"原样输出"
static const char *kGridVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aPos;

void main()
{
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char *kGridFragmentShader = R"(#version 330 core
uniform vec3 u_color;
out vec4 fragColor;

void main()
{
    fragColor = vec4(u_color, 1.0);
}
)";

WaveformGLWidget::WaveformGLWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    // 3.3 Core：可以写现代 GLSL（in/out、layout(location=)），也便于移植到其它平台。
    // 故意不用兼容模式——岗位面试里"Core profile + VAO"是个明确的区分点。
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSamples(4);              // MSAA：折线边缘不锯齿
    fmt.setSwapInterval(1);
    setFormat(fmt);

    // x 分量就是槽位号，初始化一次后不再变化；数据更新只动 y
    m_verts.resize(kCap * 2);
    for (int i = 0; i < kCap; ++i) {
        m_verts[i * 2]     = float(i);
        m_verts[i * 2 + 1] = 0.0f;
    }

    m_statsTimer.start();
    m_lastData.start();
    setMinimumHeight(240);

    // 60Hz 固定节拍重绘：滚动动画与数据到达解耦（真实示波器类界面都是这么做的）。
    // 只在有数据流入时运转；数据停 800ms 后自动停，避免空转浪费 CPU/GPU。
    m_repaintTimer = new QTimer(this);
    m_repaintTimer->setInterval(16);
    connect(m_repaintTimer, &QTimer::timeout, this, [this]() {
        if (m_lastData.elapsed() > 800)
            m_repaintTimer->stop();
        update();
    });
}

WaveformGLWidget::~WaveformGLWidget()
{
    // GL 资源必须在上下文当前时销毁，否则是未定义行为
    makeCurrent();
    m_vbo.destroy();
    m_vao.destroy();
    m_gridVbo.destroy();
    m_gridVao.destroy();
    m_program.removeAllShaders();
    m_gridProgram.removeAllShaders();
    doneCurrent();
}

void WaveformGLWidget::setWindowSize(int points)
{
    m_window = qBound(2, points, kCap);
    update();
}

void WaveformGLWidget::appendValue(qint64 value)
{
    const int i = m_head;
    m_verts[i * 2 + 1] = float(qBound(qint64(0), value, qint64(4095)));

    if (m_appendTotal == 0)
        m_appendFirst = i;
    if (m_appendTotal < kCap)
        ++m_appendTotal;

    m_head = (m_head + 1) % kCap;
    if (m_count < kCap)
        ++m_count;

    m_lastData.restart();
    if (!m_repaintTimer->isActive())
        m_repaintTimer->start();   // 绘制由 60Hz 节拍驱动，这里只负责记录数据
}

void WaveformGLWidget::clearData()
{
    for (int i = 0; i < kCap; ++i)
        m_verts[i * 2 + 1] = 0.0f;

    m_head = 0;
    m_count = 0;
    m_appendFirst = 0;
    m_appendTotal = kCap;   // 整体重传一次
    update();
}

void WaveformGLWidget::initializeGL()
{
    if (!initializeOpenGLFunctions()) {
        emit glReady(QStringLiteral("OpenGL 3.3 Core 上下文创建失败"));
        return;
    }

    const bool linked =
        m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        && m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)
        && m_program.link();
    if (!linked) {
        emit glReady(QStringLiteral("着色器编译/链接失败：%1").arg(m_program.log()));
        return;
    }

    m_vao.create();
    m_vao.bind();

    m_vbo.create();
    m_vbo.bind();
    m_vbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    m_vbo.allocate(m_verts.constData(), m_verts.size() * int(sizeof(float)));

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * int(sizeof(float)), nullptr);

    m_vao.release();
    m_vbo.release();

    glDisable(GL_DEPTH_TEST);
    glClearColor(0.051f, 0.086f, 0.149f, 1.0f);   // 与卡片底色 #0d1626 一致，控件边界自然融入

    // ---- 背景网格：5 条横线 + 6 条竖线，坐标直接写 NDC，一次静态上传 ----
    m_gridOk = m_gridProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, kGridVertexShader)
               && m_gridProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, kGridFragmentShader)
               && m_gridProgram.link();

    if (m_gridOk) {
        QVector<float> grid;
        for (int i = 0; i < 5; ++i) {                    // 横网格：对齐 0 / 1023 / 2047 / 3071 / 4095
            const float y = -1.0f + float(i) * 0.5f;
            grid << -1.0f << y << 1.0f << y;
        }
        for (int i = 0; i <= 5; ++i) {                   // 竖网格：等分 5 格
            const float x = -1.0f + float(i) * 0.4f;
            grid << x << -1.0f << x << 1.0f;
        }
        m_gridVerts = grid.size() / 2;

        m_gridVao.create();
        m_gridVao.bind();
        m_gridVbo.create();
        m_gridVbo.bind();
        m_gridVbo.setUsagePattern(QOpenGLBuffer::StaticDraw);
        m_gridVbo.allocate(grid.constData(), grid.size() * int(sizeof(float)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * int(sizeof(float)), nullptr);
        m_gridVao.release();
        m_gridVbo.release();
    }

    m_appendTotal = 0;
    m_glOk = true;

    emit glReady(QStringLiteral("OpenGL %1 | %2")
                     .arg(QString::fromLatin1(reinterpret_cast<const char *>(glGetString(GL_VERSION))),
                          QString::fromLatin1(reinterpret_cast<const char *>(glGetString(GL_RENDERER)))));
}

void WaveformGLWidget::resizeGL(int w, int h)
{
    // QOpenGLWidget 的坐标系是设备像素，HiDPI 屏要乘缩放比
    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, int(w * dpr), int(h * dpr));
}

void WaveformGLWidget::paintGL()
{
    m_frameTimer.start();

    glClear(GL_COLOR_BUFFER_BIT);
    drawGrid();                 // 先铺网格，波形压在上面

    if (m_glOk) {
        uploadPending();          // 只传增量，不传整块

        if (m_count >= 2) {
            const int   W    = qMin(m_count, m_window);        // 可见点数
            const float span = float(qMax(1, W - 1));
            const int   start = (m_head - W + kCap) % kCap;    // 可见窗口最左点的槽位
            const int   len1  = qMin(W, kCap - start);         // 第一段：start..缓冲区尾
            const int   len2  = W - len1;                      // 第二段：跨尾后从 0 开始

            m_program.bind();
            m_vao.bind();

            m_program.setUniformValue("u_span", span);
            m_program.setUniformValue("u_yMin", 0.0f);
            m_program.setUniformValue("u_yMax", 4095.0f);
            m_program.setUniformValue("u_color", 0.29f, 0.83f, 0.98f);   // 与左侧图表同色系（#4ad4fa）
            m_program.setUniformValue("u_left", float(start));

            // GL_LINE_STRIP 在 Core profile 下宽度固定 1px，用三次 y 微偏移叠出约 2.5px
            for (int k = -1; k <= 1; ++k) {
                m_program.setUniformValue("u_yOffset", float(k) * 0.0026f);

                m_program.setUniformValue("u_xOffset", 0.0f);
                glDrawArrays(GL_LINE_STRIP, start, len1);

                if (len2 > 0) {
                    // 第二段的槽位号是 0..len2-1，整体 +kCap 才接在第一段后面
                    m_program.setUniformValue("u_xOffset", float(kCap));
                    glDrawArrays(GL_LINE_STRIP, 0, len2);
                }
            }

            m_vao.release();
            m_program.release();
        }
    }

    m_frameUs += m_frameTimer.nsecsElapsed() / 1000;   // ns → us
    ++m_frames;

    const qint64 win = m_statsTimer.elapsed();
    if (win >= 500) {
        const double fps = m_frames * 1000.0 / double(win);
        const double ms  = m_frames > 0 ? double(m_frameUs) / 1000.0 / double(m_frames) : 0.0;
        emit statsUpdated(fps, ms);
        m_frames  = 0;
        m_frameUs = 0;
        m_statsTimer.restart();
    }
}

void WaveformGLWidget::drawGrid()
{
    if (!m_gridOk || m_gridVerts <= 0)
        return;

    // 静态几何 + 一个颜色 uniform：每帧成本只有一次 draw call，不影响波形本身的帧率对比
    m_gridProgram.bind();
    m_gridVao.bind();
    m_gridProgram.setUniformValue("u_color", 0.133f, 0.192f, 0.310f);   // #22314f，与图表网格同色系
    glDrawArrays(GL_LINES, 0, m_gridVerts);
    m_gridVao.release();
    m_gridProgram.release();
}

void WaveformGLWidget::uploadPending()
{
    if (m_appendTotal <= 0)
        return;

    m_vbo.bind();

    const int s = m_appendFirst;
    const int n = m_appendTotal;

    if (n >= kCap) {
        glBufferSubData(GL_ARRAY_BUFFER, 0, m_verts.size() * int(sizeof(float)), m_verts.constData());
    } else if (s + n <= kCap) {
        glBufferSubData(GL_ARRAY_BUFFER,
                        s * 2 * int(sizeof(float)),
                        n * 2 * int(sizeof(float)),
                        m_verts.constData() + s * 2);
    } else {
        // 跨过缓冲尾：拆成两段上传
        const int first = kCap - s;
        glBufferSubData(GL_ARRAY_BUFFER,
                        s * 2 * int(sizeof(float)),
                        first * 2 * int(sizeof(float)),
                        m_verts.constData() + s * 2);
        glBufferSubData(GL_ARRAY_BUFFER,
                        0,
                        (n - first) * 2 * int(sizeof(float)),
                        m_verts.constData());
    }

    m_vbo.release();
    m_appendTotal = 0;
}
