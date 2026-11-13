# AOMonitor QTest 单元测试执行手顺

> 适用环境：Windows + Qt 6.8.3 (MinGW) + Ninja + CMake ≥ 3.19
> 测试目标：`aomonitor_tests`（27 个用例，覆盖 FrameParser 切帧/坏帧/缓冲上限/格式化）

---

## 1. 环境准备（每次新开终端都要做）

Qt 的 bin 和 MinGW 的 bin 必须在 PATH 里，否则报 `找不到 Qt6Core.dll`。

**Git Bash 写法：**

```bash
export PATH="/d/Qt/6.8.3/mingw_64/bin:/d/Qt/Tools/mingw1310_64/bin:$PATH"
export QT_QPA_PLATFORM=offscreen   # 不弹窗口，CI 也能跑
```

**CMD 写法：**

```cmd
set PATH=D:\Qt\6.8.3\mingw_64\bin;D:\Qt\Tools\mingw1310_64\bin;%PATH%
set QT_QPA_PLATFORM=offscreen
```

> 注意：MinGW 工具链路径以 `D:\Qt\Tools\` 下实际目录为准（本机为 `mingw1310_64`）。

## 2. 配置（CMake configure）

```bash
cd /d/project/QT/AOMonitor
cmake -S . -B build-test -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/mingw_64
```

- 只需执行一次；改了 CMakeLists.txt 或换编译器后重新执行。
- `build-test` 与日常开发用的 `build` 目录互不干扰。

## 3. 构建

```bash
cmake --build build-test
```

增量构建，只重编改动过的文件。测试目标叫 `aomonitor_tests`，与主程序一起编译。

## 4. 运行测试（三种方式，任选其一）

### 方式 A：ctest（推荐，CI 标准做法）

```bash
ctest --test-dir build-test -V
```

`-V` 显示详细输出。末尾看到 `100% tests passed` 即全部通过。

### 方式 B：直接运行测试 exe + 结果输出到文件

```bash
./build-test/aomonitor_tests.exe -o ut_result.txt,txt
```

> 注意：本工程测试 exe 是 GUI 子系统，**直接双击或控制台直跑看不到输出**，
> 必须用 `-o 文件,txt` 把结果落盘，或者走方式 A 的 ctest。

打开 `ut_result.txt`，末尾一行应为：

```
Totals: 27 passed, 0 failed, 0 skipped, 0 blacklisted
```

### 方式 C：只跑某一个用例（调试单个函数）

```bash
./build-test/aomonitor_tests.exe halfPacket -o ut_one.txt,txt
```

函数名 = test 文件里的 `private slots` 名（如 `halfPacket`、`badFrames`、`formatValues`）。
数据驱动用例后缀 `:_data` 会被自动带上。

## 5. 常用辅助命令

| 目的 | 命令 |
|---|---|
| 列出全部用例名 | `./build-test/aomonitor_tests.exe -functions` |
| 输出 XML（CI 报表） | `... -o result.xml,xunitxml` |
| 带详细逐行输出 | ctest 加 `-V`；exe 加 `-o -,txt`（写到 stdout） |
| 全部重来 | 删掉 `build-test` 目录后回到第 2 步 |

## 6. 结果判读

- **passed**：用例通过。
- **failed**：会打印期望值 vs 实际值（QCOMPARE）或失败表达式（QVERIFY），
  以及所在文件：行号，对照 `tests/test_ao_monitor.cpp` 修复。
- **0 skipped, 0 blacklisted**：没有跳过/拉黑用例。

## 7. 生成覆盖率 HTML 报告（gcov + gcovr）

QTest 本身**不管覆盖率**，它只出测试结果。覆盖率靠 GCC 的 gcov 插桩 + gcovr 渲染。
CMakeLists 里已内置开关 `ENABLE_COVERAGE`（只给测试目标插桩，主程序不受影响）。

### 7.1 一次性准备

```bash
python -m pip install gcovr          # 本机已装 8.8
gcovr --version
```

MinGW 的 `gcov.exe` 在 `D:\Qt\Tools\mingw1310_64\bin\`，**该路径必须在 PATH 里**，否则 gcovr 找不到 gcov。

### 7.2 配置独立构建目录（不能复用 build-test）

`build-test` 没插桩，`.gcda` 永远不会有。必须新建一个目录：

```bash
cd /d/project/QT/AOMonitor
cmake -S . -B build-cov -G Ninja \
      -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/mingw_64 \
      -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
```

CMD 写法把 `\` 换行去掉即可。

### 7.3 编译并运行测试

```bash
cmake --build build-cov --target aomonitor_tests
ctest --test-dir build-cov
```

跑完确认插桩数据已生成：

```bash
ls build-cov/CMakeFiles/aomonitor_tests.dir/*.gcda
# 应看到 frameparser.cpp.gcda 与 tests/test_ao_monitor.cpp.gcda
```

### 7.4 生成 HTML

```bash
gcovr -r . \
      --object-directory build-cov/CMakeFiles/aomonitor_tests.dir \
      --filter frameparser \
      --exclude-throw-branches --exclude-unreachable-branches \
      --html-details docs/coverage/index.html \
      --html-title "AOMonitor FrameParser Coverage"
```

打开：`start docs\coverage\index.html`（或 `explorer docs\coverage`）。

产物（**必须整目录一起拷**，明细页靠相对路径引 index.css / index.js）：

| 文件 | 内容 |
|---|---|
| `index.html` | 汇总页：行/分支/函数三项覆盖率 + 文件列表 |
| `index.frameparser.cpp.<hash>.html` | 逐行明细，左侧是执行次数，绿=覆盖 红=未覆盖 |
| `index.functions.html` | 按函数统计 |
| `index.css` / `index.js` | 样式与交互，**不能单独漏掉** |

### 7.5 参数为什么这么写（踩过的坑）

| 参数 | 不加会怎样 |
|---|---|
| `--filter frameparser` | 把 `tests/test_ao_monitor.cpp` 也算进总数，用测试代码给自己刷分，数字好看但没意义 |
| `--exclude-throw-branches` | **最关键**。C++ 每个可能抛异常的调用（`indexOf`/`remove`/`mid`/`QString` 构造）GCC 都会生成异常展开分支，这些分支永远走不到。不加时本工程分支覆盖率显示 71%，加了才是真实的 100% |
| `--exclude-unreachable-branches` | 滤掉编译器生成的死分支（配合上一条用） |
| `--object-directory` | 指定 `.gcda` 所在目录。不指定时 gcovr 会去源码目录找，找不到就报 0% |
| `-r .`（root） | 决定报告里的相对路径基准，写错会导致源码文件匹配不上而被静默排除 |

### 7.6 数据陈旧问题（最容易误判）

`.gcda` 是**累加**的，不是覆盖。改代码重编后如果没清，新旧数据会混在一起算：

```bash
# 重跑前清掉插桩数据
find build-cov -name "*.gcda" -delete
# 或者最省事：rm -rf build-cov 重新配置
```

判断数据是否对得上：核对报告里的"总行数"和你当前源文件的可执行行数是否一致。

### 7.7 本工程实测结果（2026-11-10）

```
frameparser.cpp    31 行 / 31 执行 = 100%    分支 21/21 = 100%    函数 5/5
frameparser.h       1 行 /  1 执行 = 100%
总计               32/32 行，21/21 分支，6/6 函数，全部 100%
```

`frameparser.cpp` 只有 51 行源码（31 行可执行），27 个用例做到行+分支双百是合理的。
**别把 100% 当成绩**——它只说明这个类被测试喂满了，不代表 mainwindow/waveformglwidget 有测试。
覆盖率的价值在于暴露"哪块没测"，不是刷一个好看的数。

---

## 8. 测试覆盖内容（27 用例一览）

| 测试函数 | 覆盖点 |
|---|---|
| `halfPacket`（数据驱动） | 半包分批到达、多帧混在一包、一次喂完 |
| `noiseAndJunk` | 帧头前垃圾丢弃、纯噪声全丢、缓冲不膨胀 |
| `badFrames`（数据驱动） | 5 种坏帧：无逗号/非数字/空值/只有帧头/尾后残留 |
| `bufferLimit` | 4096 缓冲上限、reset() 清空 |
| `formatValues`（数据驱动） | formatValue 7 组：0/12/4095/负数越界等 |
