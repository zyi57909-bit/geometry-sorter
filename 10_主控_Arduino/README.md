# 主控程序（AutoMeasureSort）

> 全项目**唯一**主线程序目录。编译/烧录只认这里；旧版程序在 `90_历史存档/`，禁止编译。

## 文件

| 文件 | 作用 |
|---|---|
| `AutoMeasureSort.ino` | 主线（2026-10-26 版）：上电传送 → K230 检测 → Timer3 中断停带 → 6 桶分选/开合落料 → 重启循环 |
| `diaglog.h` | 离线诊断日志（EEPROM 408 条 × 10 B，`d` dump / `c` clear）⚠️ 当前 .ino 未 include，见下方说明 |

## 编译与烧录

- 板卡：Arduino Mega 2560（ATmega2560，256 KB Flash / 8 KB SRAM / 4 KB EEPROM）
- 工具链（2026-10-18 实测并固定）：

| 组件 | 版本 | 位置 |
|---|---|---|
| `arduino-cli` | **1.5.2-rc.1** | `%LOCALAPPDATA%\Programs\arduino-cli\`（已加入用户 PATH） |
| `arduino:avr` core | **1.8.8** | `%LOCALAPPDATA%\Arduino15\packages\arduino\hardware\avr\1.8.8` |
| `avr-gcc` | **7.3.0-atmel3.6.1-arduino7** | Arduino15 内 |
| `AccelStepper` | **1.64** | `%USERPROFILE%\Documents\Arduino\libraries` |

- **一条命令**（`_build/` 被 gitignore，不污染仓库）：

```powershell
.\10_主控_Arduino\compile.ps1                      # 编译
.\10_主控_Arduino\compile.ps1 -Upload -Port COM5   # 编译 + 烧录
```

- 裸命令（Arduino 要求 sketch 目录名 = `.ino` 名，脚本会自动暂存到 `_build/AutoMeasureSort/`）：

```powershell
arduino-cli compile --fqbn arduino:avr:mega --warnings more --output-dir _build/out _build/AutoMeasureSort
```

- **实测结果（2026-10-18）**：`0 错误 0 警告`；Flash **17,824 B（7%）**；全局变量 **1,369 B（16%）**，剩 6,823 B
- ⚠️ 警告口径：`--warnings more`（= `-Wall`）本工程 0 警告；`--warnings all`（= `-Wextra`）会多出 **5 条来自 Arduino core `new.cpp` 的上游警告**，不是本工程代码。验收统一用 `more`
- 细节与历史记录：`99_调试记录/2026-10-18_工具链与编译/记录.md`

## 运行与日志

- USB 串口：**115200**（`Serial`），看启动横幅 + 状态迁移日志
- 命令：`d` = dump 诊断日志、`c` = clear；`e` 急停 / `r` 复位 **属阶段 1 新增，当前未实现**
- 注意：运行中发 `d` 会同步打印 408 条，约 1~2 s 不执行电机 → 可能丢步，**停机时再 dump**

## 当前程序结构（2026-10-26 版，单文件顺序流程）

> 2026-10-27 由 `AutoMeasureSort10.23` 源程序同步进来。实际代码**不是**早前 README 描述的
> `RUN / DETECTED / SORT / RELEASE / DELAY / RESET` 六状态机，下面按代码实况描述。

- **无状态机**：`loop()` 顺序执行「发 `image` → 收 K230 帧 → 入桶计数 → HMI 上报 → 6 档分选动作 → 重启传送带」，每圈末尾 `delay(1000)`
- **停带在 Timer3 1 ms 中断里做**（`ISR(TIMER3_COMPA_vect)`，`TCCR3B=CTC+8 分频`、`OCR3A=1999`）：
  主循环里有 `delay()`，靠轮询一圈要 1.4~8 s 才读一次传感器，而物体只经过 0.1~0.3 s，几乎必漏；
  中断只认「高→低」下降沿（重启后同一物体还挡着也不会重复触发），触发后延迟
  `STOP_DELAY_MS = 500` ms 再停带。`stop1/stop2` 因与中断共享已改 `volatile`
- **桶数 6**（`NUM_BUCKETS`）：`codeToBucket()` 先按物料码查旧槽复用，没有再占新槽；
  6 槽占满后新类别**静默丢弃**（无满桶提示、无报警）
- **K230 帧校验** `checkK230Frame()`：只接受 `Re,<标签>,<置信度>`（兼容外层括号），
  标签必须纯数字且在 **0..441**，置信度 0.00~1.00；非法帧丢弃并在 USB 串口打 `[WARN]`
- **类别规模**：13 色 × 34 形状 = **442 类**（`COLORS` / `SHAPES` 两张表）；
  `getObjectName()` 拼中文名（如 91 → `红球体`）
- **HMI 上报**（TJC 串口屏，`Serial1`，**115200**）：`t0.txt` 写物体名称；
  `data0.insert` / `data0.up` 维护「序号^名称^数量」表格；首次占槽时用 `p<槽位>.pic` 发一张图
- **运动**：分选 X 到位 **0 / 325 / 650 / 975 / 1300 / 1625**；
  开合 Y **开 = 0**、**关 = 850**（1、2 号桶）/ **750**（3~6 号桶）；上电 Y 初始位置 850
- **没有**：超时看门狗、急停、满桶处理、运行状态上报、网络上报
- ⚠️ `diaglog.h` 仍随 `compile.ps1` 复制进暂存目录，但**当前 .ino 未 `#include` 它**，
  所以上面「运行与日志」里的 `d`/`c` 诊断命令**当前不生效**，USB 串口只剩 `[WARN]` 与入桶计数

> 目标状态与异常策略见 `00_接口约定/状态机与异常策略.md`；参数唯一值见 `00_接口约定/参数表.md`。
> ⚠️ 注意：`参数表.md` 仍是旧基线（403 类、`t1.val`、TJC 9600），与当前代码（442 类、`data0`、TJC 115200）不一致，待主控与 HMI/视觉确认后统一更新。

## 改动纪律

1. 凡是接口（回包格式、HMI 指令、波特率、N、超时、脉冲数）→ **先改 `00_接口约定/`，打 tag，再改代码**
2. 每改一处 → 立即编译（阶段 1 起必须可重复）→ 语法/警告清零再继续
3. 每次改完 → 日志/现象存 `99_调试记录/<日期>_<主题>/`（模板已给）
4. 不改 `90_历史存档/` 里的任何东西；需要旧版本用 `git show v0.1-baseline:<路径>`
