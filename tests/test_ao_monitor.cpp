// AOMonitor 自动化单测（Qt Test / QTest）
//
// 被测对象：FrameParser（从 MainWindow 抽出的切帧纯逻辑类）
// 测试策略：
//   - 业务逻辑全部下探到 FrameParser，测试不创建任何窗口，无头可跑；
//   - 数据驱动（_data 槽）覆盖正常帧 / 半包 / 坏帧 / 噪声的边界值；
//   - 用 QSignalSpy 验证"发没发信号、发的参数对不对"。
//
// 运行：build 目录下 ctest 或直接执行 aomonitor_tests

#include <QtTest>
#include <QSignalSpy>

#include "frameparser.h"

using namespace Qt::Literals::StringLiterals;   // u"..."_s / "..."_qba

class TestAoMonitor : public QObject
{
    Q_OBJECT

private slots:
    // ---------- 帧解析：正常路径 ----------
    void testSingleFrame();
    void testSingleFrame_data();            // 数据驱动
    void testMultipleFramesOneFeed();       // 一次喂入多帧
    void testHalfPacketAcrossFeeds();       // 半包跨批次重组（粘包的核心场景）
    void testGarbageBeforeFrame();          // 帧头前的垃圾被丢弃
    void testNoiseOnly();                   // 纯噪声：缓冲清空，不影响后续
    void testBufferOverflowRecovery();      // 超 4096 字节后仍能恢复解析
    void testResetClearsPendingData();      // reset 后半包不残留
    void testBufferedCount();               // buffered() 反映半包等待状态

    // ---------- 帧解析：坏帧（发坏数据不应有信号） ----------
    void testBadFramesDropped();
    void testBadFramesDropped_data();       // 数据驱动

    // ---------- 用 QSignalSpy 验证信号参数 ----------
    void testSignalArgsViaSpy();

    // ---------- 显示格式 ----------
    void testFormatValue();
    void testFormatValue_data();            // 数据驱动
};

// ============ 正常路径 ============

void TestAoMonitor::testSingleFrame_data()
{
    QTest::addColumn<QByteArray>("input");
    QTest::addColumn<qint64>("expected");

    QTest::newRow("正常值")         << "$DATA,1250#"_qba << qint64(1250);
    QTest::newRow("零值")           << "$DATA,0#"_qba    << qint64(0);
    QTest::newRow("满量程4095")     << "$DATA,4095#"_qba << qint64(4095);
    QTest::newRow("带空格会被trim") << "$DATA, 42 #"_qba << qint64(42);
    QTest::newRow("负数也解析")     << "$DATA,-5#"_qba   << qint64(-5);
}

void TestAoMonitor::testSingleFrame()
{
    QFETCH(QByteArray, input);
    QFETCH(qint64, expected);

    FrameParser p;
    QSignalSpy spy(&p, &FrameParser::frameParsed);
    p.feed(input);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).value<qint64>(), expected);
    QCOMPARE(p.buffered(), 0);              // 帧消费完，缓冲清空
}

void TestAoMonitor::testMultipleFramesOneFeed()
{
    FrameParser p;
    QSignalSpy spy(&p, &FrameParser::frameParsed);
    p.feed("$DATA,1#$DATA,2#$DATA,3#"_qba);

    QCOMPARE(spy.count(), 3);
    QCOMPARE(spy.at(0).at(0).value<qint64>(), qint64(1));
    QCOMPARE(spy.at(1).at(0).value<qint64>(), qint64(2));
    QCOMPARE(spy.at(2).at(0).value<qint64>(), qint64(3));
}

void TestAoMonitor::testHalfPacketAcrossFeeds()
{
    // 串口是字节流：一批只到一半，必须留着等下一批拼出完整帧
    FrameParser p;
    QSignalSpy spy(&p, &FrameParser::frameParsed);

    p.feed("$DATA,12"_qba);                 // 半包，无 '#'（8 字节）
    QCOMPARE(spy.count(), 0);               // 不该有信号
    QCOMPARE(p.buffered(), 8);              // "$DATA,12" 留在缓冲里等

    p.feed("50#"_qba);                      // 补上后半段
    QCOMPARE(spy.count(), 1);               // 重组出完整帧
    QCOMPARE(spy.at(0).at(0).value<qint64>(), qint64(1250));
}

void TestAoMonitor::testGarbageBeforeFrame()
{
    FrameParser p;
    QSignalSpy spy(&p, &FrameParser::frameParsed);

    // 注意：QByteArray(const char*) 按 strlen 截断，含 \0 必须显式给长度
    // "xx" + \x00 + \xff + "$DATA,77#" = 13 字节
    p.feed(QByteArray("xx\x00\xff$DATA,77#", 13));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).value<qint64>(), qint64(77));
}

void TestAoMonitor::testNoiseOnly()
{
    FrameParser p;
    QSignalSpy spy(&p, &FrameParser::frameParsed);

    p.feed("xyzzy"_qba);                    // 连 '$' 都没有：纯噪声
    QCOMPARE(spy.count(), 0);
    QCOMPARE(p.buffered(), 0);              // 缓冲被清空，不积累

    // 清空后仍能正常解析（噪声没有污染状态）
    p.feed("$DATA,9#"_qba);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).value<qint64>(), qint64(9));
}

void TestAoMonitor::testBufferOverflowRecovery()
{
    // 坏波特率场景：连续 5000 字节垃圾把缓冲顶到 4096 上限，
    // 之后来的正常帧必须还能解析出来（不能被撑死）
    FrameParser p;
    QSignalSpy spy(&p, &FrameParser::frameParsed);

    p.feed(QByteArray(5000, 'x'));
    QCOMPARE(spy.count(), 0);

    p.feed("$DATA,88#"_qba);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).value<qint64>(), qint64(88));
    QCOMPARE(p.buffered(), 0);
}

void TestAoMonitor::testResetClearsPendingData()
{
    FrameParser p;
    QSignalSpy spy(&p, &FrameParser::frameParsed);

    p.feed("$DATA,12"_qba);                 // 留下一个半包
    p.reset();                              // 模拟重开串口

    p.feed("$DATA,33#"_qba);                // 新连接的数据
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).value<qint64>(), qint64(33));  // 不会拼上残留的 "12"
}

void TestAoMonitor::testBufferedCount()
{
    FrameParser p;
    QCOMPARE(p.buffered(), 0);

    p.feed("xx$DATA,12"_qba);               // 垃圾被丢，只剩半包
    QCOMPARE(p.buffered(), 8);
}

// ============ 坏帧 ============

void TestAoMonitor::testBadFramesDropped_data()
{
    QTest::addColumn<QByteArray>("input");

    QTest::newRow("无逗号")       << "$ABCD#"_qba;
    QTest::newRow("空数值")       << "$DATA,#"_qba;
    QTest::newRow("非数字")       << "$DATA,abc#"_qba;
    QTest::newRow("裸井号")       << "$#"_qba;
    QTest::newRow("坏帧后接好帧") << "$DATA,x#zz$DATA,66#"_qba;
}

void TestAoMonitor::testBadFramesDropped()
{
    QFETCH(QByteArray, input);

    FrameParser p;
    QSignalSpy spy(&p, &FrameParser::frameParsed);
    p.feed(input);

    // 无论哪种坏帧，只要输入里有合法帧（最后一组），就只应收到它
    const bool hasValidFrame = input.contains("$DATA,66#"_qba);
    QCOMPARE(spy.count(), hasValidFrame ? 1 : 0);
    if (hasValidFrame)
        QCOMPARE(spy.at(0).at(0).value<qint64>(), qint64(66));
}

// ============ QSignalSpy 演示：真实串口时序的信号序列 ============

void TestAoMonitor::testSignalArgsViaSpy()
{
    // 模拟真实串口时序：垃圾、被切断的帧头、半包混在几批字节里
    FrameParser p;
    QSignalSpy spy(&p, &FrameParser::frameParsed);

    p.feed("noise$DA"_qba);                 // 'DA' 是被切断的帧头前缀，留在缓冲
    p.feed("TA,100#$DATA,"_qba);
    p.feed("200#"_qba);

    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(0).at(0).value<qint64>(), qint64(100));
    QCOMPARE(spy.at(1).at(0).value<qint64>(), qint64(200));
    QCOMPARE(p.buffered(), 0);
}

// ============ 显示格式 ============

void TestAoMonitor::testFormatValue_data()
{
    QTest::addColumn<qint64>("value");
    QTest::addColumn<QString>("expected");

    QTest::newRow("零")         << qint64(0)     << u"0000"_s;
    QTest::newRow("个位")       << qint64(9)     << u"0009"_s;
    QTest::newRow("两位")       << qint64(42)    << u"0042"_s;
    QTest::newRow("三位")       << qint64(125)   << u"0125"_s;
    QTest::newRow("四位")       << qint64(1250)  << u"1250"_s;
    QTest::newRow("满量程")     << qint64(4095)  << u"4095"_s;
    QTest::newRow("超宽不截断") << qint64(12345) << u"12345"_s;
}

void TestAoMonitor::testFormatValue()
{
    QFETCH(qint64, value);
    QFETCH(QString, expected);
    QCOMPARE(FrameParser::formatValue(value), expected);
}

QTEST_GUILESS_MAIN(TestAoMonitor)
#include "test_ao_monitor.moc"
