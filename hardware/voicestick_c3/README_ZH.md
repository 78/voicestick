# VoiceStick C3 v1 硬件

[English](README.md) · 简体中文

VoiceStick C3 v1 是自研硬件的首个公开版本。所有电气数据只有一个来源：[tools/design.py](tools/design.py)。原理图、BOM、PCB 网络表都由脚本生成，不手改 KiCad 文件。

原理图按功能分成带标题框的模块（USB、充电、电源路径、LDO、MCU、Flash、晶振、天线匹配、外设电源、按键等）。模块内的走线和元件位置写在 [tools/schematic_layout.py](tools/schematic_layout.py)；全局标签只用于跨模块网络，GND / +3V3 用电源符号，模块内部网络用线上的名字框标出。布局文件只决定怎么画，连接关系始终来自 `design.py`，画错线会在 `check_netlist.py` 里报不一致。

## 目录

| 目录 | 用途 |
| --- | --- |
| [kicad/](kicad/) | 原理图、PCB、元件库和电气检查报告 |
| [fab/](fab/README_ZH.md) | PCB 下单、贴片和上电检查 |
| [enclosure/](enclosure/README_ZH.md) | 打印零件、整机模型、辅料和装配 |
| [tools/](tools/) | 元件定义、布局布线和文件导出脚本 |

## 硬件规格

- ESP32-C3（QFN32，无内置 Flash）+ 外挂 8MB SPI Flash GD25Q64E（双 OTA 各 3MB + 1.9MB storage，沿用 `firmware/partitions_ota.csv`）。
- 40MHz 主晶振 + 32.768kHz 晶振（BLE 低功耗时钟）。
- 控制台只用 USB-Serial-JTAG（GPIO18/19），UART0 引脚释放为 GPIO。
- 电源：TP4054 线性充电 100mA（0.4C）；VBUS 经 B5819W、电池经 AO3401A 自动切换到 VSYS；RT9080 3.3V LDO（Iq 2µA）。无电源开关，空闲 1 小时深睡，主键或插 USB 唤醒。
- 硬件充电灯：3V3 → 1k → 红灯 → TP4054 CHRG（开漏），GPIO10 读同一节点。充满后 CHRG 只剩约 20µA 弱下拉，红灯会微亮；GPIO10 要开内部上拉才能可靠读到高电平。
- 外设电源：AO3401A 由 GPIO2 控制，供 WS2812B-2020-V6 和麦克风，深睡时断开。
- 麦克风：Goertek SD18OB371-041 底部进声 PDM，100Ω + 4.7µF + 100nF 滤波。v1 固件默认使用 1.536 MHz 时钟，经 96 倍抽取得到 16 kHz 音频。
- 4 个按键：主键 CPG151101D05，三个侧键 Alps SKRTLBE010。
- 板子：50×30mm，四层 1.0mm，单面贴片，元件全部 0402 及以上。
- 外壳：maker FDM 打印，见[打印与装配](enclosure/README_ZH.md)。

## GPIO

| GPIO | 网络 | 用途 |
| --- | --- | --- |
| 0 / 1 | X32_P / X32_N | 32.768kHz 晶振 |
| 2 | PERIPH_EN | 外设电源，低电平打开；10k 上拉（启动引脚要求高电平） |
| 3 | BAT_ADC | 电池电压，1M/1M 分压 |
| 4 | KEY_MAIN | 主键，深睡低电平唤醒，100k 上拉 |
| 5 | VBUS_SENSE | USB 插入检测，100k/200k 分压（5V → 3.33V），深睡高电平唤醒 |
| 6 | PDM_CLK_MCU | PDM 时钟，串 33Ω |
| 7 | PDM_DAT | PDM 数据 |
| 8 | KEY_PREV | 上一页，10k 上拉（启动引脚要求高电平） |
| 9 | KEY_CANCEL | 取消 / BOOT，10k 上拉 |
| 10 | CHRG | 充电状态，低 = 充电中；需开内部上拉 |
| 11–17 | VDD_SPI, SPI_* | 外挂 Flash |
| 18 / 19 | USB_DN / USB_DP | USB 控制台 |
| 20 | LED_DIN | WS2812 数据 |
| 21 | KEY_NEXT_MCU | 下一页，串 1k（ROM 启动时在此脚打印日志） |

**与计划的差异**：计划里 GPIO8 接 WS2812、GPIO20 接上一页。WS2812 在深睡时断电，GPIO8 为满足启动引脚而加的上拉会经 DIN 的保护二极管向灯倒灌约 0.27mA，是整机深睡预算的 20 多倍。所以对调：GPIO8 接带上拉的上一页键（平时高电平、不耗电），WS2812 改到 GPIO20。固件进深睡前需把 GPIO20 设为输入、不上拉。

## 器件

完整清单在复刻包的 `fab/voicestick-c3-v1-bom.csv` 中，每一行都有 LCSC 料号。选型原则是：优先基础库，库存充足，用通用型号。阻容的料号在 `design.PASSIVES` 里按封装和数值统一指定。

- 阻容全部是基础库。为此改了三个阻值：
  - R3 从 8.2k 改为 10k，充电电流 122 → 100mA；
  - R9 从 150k 改为 200k；
  - R14 从 47Ω 改为 100Ω。
- 40MHz 晶振（扬兴 X322540MPB4SI）、0402 射频电感和 1.5pF 匹配电容没有基础库可选，用扩展库里库存最多的大厂料。晶振负载电容是 15pF，所以 C14/C15 用 22pF。
- 原定的华邦 W25Q64JVSSIQ 库存只剩 17 片，WS2812B-2020（C965555）库存为 0，分别换成兆易 GD25Q64ESIGR 和 Worldsemi WS2812B-2020-V6。引脚、封装都相同，已对照数据手册核对过。
- 射频按乐鑫设计指南做了两组 π 型匹配：
  - 芯片侧 C18/L3/C19 紧贴芯片，贴推荐范围的中间值 1.5pF / 2.4nH / 1.5pF；
  - 天线侧 C25/R20/C26 在禁布区边上，R20 贴 0Ω，C25/C26 默认不贴，装壳后用 VNA 调。
- 自定义封装的 3D 模型放在 `kicad/3dmodels/`，只用于显示，干涉检查仍用外壳脚本的包络体：
  - 侧键、麦克风、天线、侧发光充电灯：从立创 EasyEDA 下载（C127481、C3040249、C127629、C125114），已按我们的焊盘对齐。麦克风模型是同尺寸的 ST MP23ABS1，天线模型只是一个 3.2×1.6×1.2 的方块。
  - 主键：立创没有模型，由 `enclosure/build_switch_model.py` 按接口文件尺寸生成，和外壳脚本里的主键是同一套几何。
  - 同一目录还有两个 KiCad 10 自带库缺失的模型：USB-C 插座，以及 WS2812B-2020（取自 EasyEDA C965555，焊盘与 KiCad 封装一致）。

## 生成流程

要重建包含全部元件的整板和整机 STEP，先按[发布附件说明](../README_ZH.md#v1-发布附件)解压对应版本的元件模型包。只修改电路、布线或运行电气检查时可以直接使用源码。单独重建外壳时，如需装入带元件的 PCB，可先将复刻包中的 `fab/voicestick-c3-v1.step` 放回同一路径。

```bash
cd hardware/voicestick_c3
./make.sh            # 全流程，约 1 分钟；任何一步失败都会停下
./make.sh enclosure  # 只重建外壳
```

重建脚本使用 macOS 的应用安装路径，依赖如下：
- KiCad 10（自带的 python 和 kicad-cli）；
- [KiCadRoutingTools](https://github.com/drandyhaas/KiCadRoutingTools)，装在 `~/tools/KiCadRoutingTools`，可用环境变量 `KRT` 改路径；
- FreeCAD 1.1。

| 阶段 | 脚本 | 产物 |
| --- | --- | --- |
| 库、主键 3D 模型、原理图、ERC、网络表核对、BOM | `build_library.py`、`enclosure/build_switch_model.py`、`build_schematic.py`（+ `schematic_layout.py`）、`check_netlist.py`、`export_bom.py` | `kicad/*.kicad_sch`、`kicad/3dmodels/CPG151101D05.step`、`bom.csv`、原理图 PDF |
| 布局、板框、禁布区、网络类、DRC 规则 | `build_pcb.py`（坐标在 `placement.py`） | `build/01_placed` |
| 手布：RF 馈线、USB、晶振、MCU 去耦、电源干线，全部锁定 | `route_critical.py` | `build/02_critical` |
| KRT：内层平面（In1 GND、In2 +3V3）、其余网络 | KRT `route_planes` / `route` | `build/03`、`04` |
| 大电流过孔加大到 0.6/0.3（放不下就拆掉，用 0.6 过孔重布） | `upsize_power_vias.py` | `build/05*` |
| 外层铺地、2.5 GHz 缝合过孔、板边过孔墙，KRT 收尾 | KRT | `build/06`、`07` |
| 灌铜、装板、DRC（含原理图一致性）、板级检查 | `finish_board.py`、kicad-cli、`check_board.py` | `kicad/voicestick-c3-v1.kicad_pcb` |
| 制造文件 | kicad-cli、`export_cpl.py` | [fab/](fab/) |
| 外壳 | `export_envelopes.py`、`enclosure/build_enclosure.py`、`render_preview.py` | [enclosure/out/](enclosure/) |

关于可复现：KiCad 保存时按 UUID 给封装排序，而 KRT 按文件顺序布线，UUID 随机就会导致每次布线结果不同，有时还会失败。因此：
- 每个会新建 KiCad 对象的脚本都给 UUID 生成器设了固定种子（`design.KIID_SEEDS`）；
- 每次构建前清空 `build/` 里的阶段文件，因为 KiCad 会读取旁边遗留的旧工程文件，结果会跟着变。

连续两次构建、或在干净目录里构建，得到的铜皮完全相同，只有 KRT 自己写入的 UUID 不同。

种子组（`design.KIID_SEED_SET`）决定 KRT 的布线顺序。`python3 tools/seed_search.py --sets 0-29 --jobs 5` 会：
- 在 `/tmp` 下的独立副本里并行布多组（30 组在 6 核上约 6 分钟）；
- 用 `tools/board_metrics.py` 打分排名，比较窄电源分支、信号过孔、信号线长和外层地铜。

Flash 附近余量很小，KRT 在那里很容易失败（早先 0–9 组只有 1 组布通）。所以 U1 到 Flash 的 6 根 SPI 线、R12 上拉和 VDD_SPI 去耦由 `route_critical.py` 的 `route_flash` 手布：
- HD、CLK、D 直接落到 Flash 上排焊盘；
- WP 从 7、6 脚之间穿过封装底下；
- CS、Q 在 B.Cu 上交叉后回到 1、2 脚。

WS2812 挪到主键端以后，左侧的按键线会把 U1 的 16 脚（CHRG）围死，30 组全部失败。所以 CHRG 在 16 脚下方打孔到 B.Cu 的这一小段也改为手布。之后 0–29 组有 21 组布通。目前固定为排名第一的第 8 组。改动布局后应重新搜索。

## 验收结果（最近一次 `./make.sh`）

- ERC 0；DRC 0 错误、0 未连接、0 原理图不一致。
- 过孔共 220 个：95 个 0.45/0.3，125 个 0.6/0.3，最小孔径 0.3 mm，在嘉立创免费档。没有焊盘内过孔，唯一例外是 U1 散热焊盘的 9 个 GND 过孔。禁布区内没有过孔。
- 四层、1.0 mm、板框 50×30 与接口文件一致。
- 外壳：静止、主键按下、侧键按下三种状态无干涉；关键间隙全部达标；9 个 STL 都是封闭实体，没有非流形边。

制造与下单见 [fab/README_ZH.md](fab/README_ZH.md)，打印与装配见 [enclosure/README_ZH.md](enclosure/README_ZH.md)。

## 与计划的差异

- **GPIO8 与 GPIO20 对调**：原因见上文 GPIO 一节。
- **0.6/0.3 过孔只用在真正大电流的网络上**：VBUS、VBAT、VSYS、VDD3P3，以及 +3V3 的入口（LDO 输出、MCU 31/32 脚、VDD3P3 滤波）和全部地缝合过孔。+3V3 平面到各上拉、去耦电容的分支过孔，以及灯和麦克风、麦克风滤波、Flash 这几路小电源，用 0.45/0.3：电流都在几毫安量级，0.3 mm 孔可以过 1 A 以上；如果全部用 0.6，MCU 周围放不下。
- **U1 散热焊盘有焊盘内过孔**：共 9 个，是乐鑫要求的最少数量，用于散热和接地。钢网已分块避开过孔，不需要塞孔。
- **对照乐鑫硬件设计指南（ESP32-C3）**：
  - 已按指南处理：
    - 芯片侧和天线侧各一组 CLC 匹配；
    - 散热焊盘 9 个地过孔；
    - VDD3P3 线宽 0.5 mm；
    - 40 MHz 晶振底下的背面禁止走线。
  - 没有照做的建议，以及原因：
    - SPI Flash 线上串 0Ω 电阻、SPI_CLK 加 RC：板上放不下。如果出现射频干扰，先在固件里把 Flash 时钟降到 40 MHz，或调低驱动强度。
    - USB 线上预留串联电阻和对地电容：全速 USB 走线很短，有 ESD 保护。差分阻抗约 74 Ω（指南建议 90 Ω），全速下没有影响。
    - 匹配用 0201 封装，并做二次谐波短截线：嘉立创贴 0402 更稳。指南说明 0402 及以上不需要短截线。
    - 外壳内天线四周留 15 mm 净空：机身尺寸做不到，只能靠两组匹配调回来，装壳后实测通信距离。
    - VDD3P3 上的 10 µF：和 +3V3 入口共用 C6（指南允许入口靠近时共用）。
    - 电源监控芯片（电源缓慢下降时可靠复位）：不加，由固件处理。
      - BAT_ADC 读到电池低于约 3.5 V 时，固件关掉外设，进入只有插 USB（GPIO5）能唤醒的深睡，主键不再唤醒。
      - 3.5 V 时 RT9080 仍能稳到 3.3 V，离 ESP32-C3 的 3.0 V 下限还有余量。
      - 电池必须带保护板（enclosure/README_ZH.md 的物料表已注明），过放时由保护板断开，作为最后一道保护。
- **少数短颈线宽**：KRT 报告 +3V3 有约 7.5 mm、VDD3P3 有约 1.5 mm 的分支窄到 0.15–0.2 mm，都在焊盘出线处，电流不到 50 mA。
- **布局调整**：
  - J2 旋转 180°，插口朝板内，因为 −Y 侧到壳壁只有 0.4 mm；
  - WS2812 放在主键端 −Y 角，和麦克风关于 X 轴对称，经导光筒从顶面的灯窗透出。这个角原来是 H1 螺钉，已经拿掉。
  - 充电灯换成侧发光的 Lite-On LTST-S270KRKT（C125114），贴在 USB 端板边、USB 口旁边，透镜正对端面，光从 USB 端面的灯窗透出。插着线就能看到充电状态，也不和 WS2812 混色。
  - 电池分压、VBUS 分压和按键上拉电阻移位，给布线让路。
- **外壳**：2 颗螺钉代替 4 颗（天线角改为塑料支撑，WS2812 角改为塑料支撑并加脚垫），脚垫 1.0 mm，等等，见 [enclosure/README_ZH.md](enclosure/README_ZH.md#与计划的差异)。

## 待办（下单 / 首板前）

1. 上传 BOM 后，在嘉立创（国内站）的匹配结果里确认库存，并确认没有被自动替换成别的料。
2. 用嘉立创阻抗计算器核对天线馈线（JLC04101H-3313，共面波导，线宽 0.15、间隙 0.25，估算 47.7 Ω）。
3. 在嘉立创 DFM 预览里逐个核对坐标文件的旋转角（极性件和多脚件）。
4. 先打公差试打件和声学测试盒，确认参数后再打整壳。
5. 首板先调芯片侧匹配（C18/L3/C19），装壳后再用 VNA 调天线侧（C25/C26 默认不贴），步骤见 [fab/README_ZH.md](fab/README_ZH.md)。
6. 用 SD18OB371-041 的 Demo 板飞线复测 PDM 格式、时钟和底噪（计划中的风险项）。
7. 首板验证固件低电保护：电池连续三次采样低于 3.5 V 后，确认在电池供电下进入深睡，并且只有插 USB 能唤醒。
8. 首板只接电池、不插 USB，在 J2 测深睡电流（估算 12–20 µA）：
   - 同时测 D1（B5819W）在约 4 V 反压下的漏电，估算常温 1–5 µA、45 °C 时可能到 10–20 µA。超过约 10 µA 再换低漏电、≥0.5 A 的 SOD-123 二极管，正向压降大一些也没关系。
   - 确认不插 USB 时充电红灯完全不亮；如果微亮，说明 TP4054 CHRG 有约 1 mA 漏电。
