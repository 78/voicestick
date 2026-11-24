# 思澈语音棒 A0 原理图与 PCB

当前 25.70 mm 双层版使用 29 × 62 mm 外壳，USB-C 位于底端中央。配套 250 mAh 单节保护电池包；R3 设定 4.20 V 充满和 500 mA 输入限流档，R4 设定 90.9 mA 标称快充。SW4 由电池正极 BAT_P 经 R13 供电，R13 与 R4 共用 3.3 kΩ、1%、0402 基础料 C25890。[当前 KiCad 工程包](studies/straight-edge-25.70mm/voicestick-sf32-25.70mm-trial.zip) · [单页原理图 PDF](voicestick-sf32-schematic.pdf) · [原理图预览](voicestick-sf32-schematic.png) · [板载 BOM](studies/straight-edge-25.70mm/BOM.csv) · [采购与核价表](studies/straight-edge-25.70mm/嘉立创SMT元件核价-2026-11-22.xlsx) · [整板走线图](studies/straight-edge-25.70mm/routes.png) · [系统框图](framework.pdf) · [框图源文件](framework.svg)

## 嘉立创打样

主板尺寸为 58 × 25.70 mm。制板选 2 层、1.0 mm 板厚，贴片选正面。贴片 BOM 与坐标表包含相同的 42 个位号。SW1 键盘轴另行安装焊接。

| 上传项 | 文件 |
| --- | --- |
| 制板与钻孔 | [Gerber ZIP](studies/straight-edge-25.70mm/fab/VoiceStick-SF32-A0-Gerber.zip) |
| 贴片用料 | [BOM-SMT.csv](studies/straight-edge-25.70mm/fab/BOM-SMT.csv) |
| 贴片坐标 | [CPL-SMT.csv](studies/straight-edge-25.70mm/fab/CPL-SMT.csv) |

## 结构与图纸

这支语音棒用于手持长按对话。板宽和侧键位置以单手握持时拇指自然按到、持续按住仍舒服为目标，手感用装壳样机验证。正面主键帽的上、左、右三边要求留白等宽，LED 和麦克风在主键上方紧凑排成一行。外观效果图采用白色加天蓝色，通过有尺寸的 CAD 正视图核对留白和开孔。

正面 BAT 插座从上方插入两线插头。烧录焊盘在背面横排，直径 1.5 mm，间距 2.54 mm。[PCB STEP](studies/straight-edge-25.70mm/PCB-25.70mm.step) · [外壳与键帽 STEP](studies/straight-edge-25.70mm/enclosure-29x62.step) · [下壳 STEP](studies/straight-edge-25.70mm/lower-29x62-trial.step) · [整板 3D 预览](studies/straight-edge-25.70mm/pcb-3d.png)

系统框图上半部用绿色箭头画出 USB、电池和外设的供电路径，下半部用蓝色箭头画出按键、麦克风和蓝牙的数据方向。

整机电路在一张 A2 横版图上。01–03 为 USB、电池与充电，04–06 为主控、PDM 麦克风和 RGB 灯，07–10 为按键、烧录与测试点。实心点表示导线相连，同名网络相连，× 表示引脚未接。工程包内的 `colors/VoiceStick.json` 保存了原理图配色。

## 麦克风与参考板

立创黄山派板载模拟 MEMS 麦克风，模组 36 脚供偏置，37 脚接收模拟信号。立创小智训练营的两线驻极体咪头也走这一路，教程中的 10 kΩ 用于咪头偏置。思澈模组手册图 7-4 的 M115 是另一张模拟 MEMS 参考电路。这些参考电路适合不同的模拟器件。[黄山派引脚表](https://openkits-wiki.easyeda.com/zh-hans/hspi-sf32lb52/hardware/board.html) · [小智硬件电路分析](https://wiki.lckfb.com/zh-hans/hspi-sf32lb52/training/xiaozhi-ai/hardware.html) · [思澈模组规格书 V0.2](https://downloads.sifli.com/silicon/DS5203-SF32LB52-MOD-1%E6%8A%80%E6%9C%AF%E8%A7%84%E6%A0%BC%E4%B9%A6%20V0p2.pdf)

本机的 ZTS6538SE（C5155190）从顶部进音，内部转换成 PDM。模组 PA07（40 脚）经 33 Ω 驱动 CLK，PA08（39 脚）接 DATA。麦克风 1、3 脚接地，2 脚 SEL 接地，4 脚为 CLK，5 脚为 DATA，6 脚接可控 3.3 V，并就近放置 1 µF。SEL 为低时麦克风在时钟下降沿之后驱动数据。固件先使能 VDD33_VOUT2，再启动 PDM1；按数据手册预留唤醒时间后再使用样本。关电前先停止时钟并将引脚置于合适的低功耗状态。思澈提供 SF32LB52 系列的 PDM 录音示例，移植时按本板更改管脚和电源使能。[钰太麦克风规格书](https://atta.szlcsc.com/upload/public/pdf/source/20220826/E32BC96636C6E3E020DFE347104B08CE.pdf) · [思澈 SDK PDM 示例](https://docs.sifli.com/projects/sdk/latest/en/sf32lb52x/example/rt_device/pdm/README.html)

## 电池与电源

- J2 使用首韩 1.25-2P（立创 C393945），两针立贴，电池线束从板面正上方插入。1 脚接保护板输出正极 BAT_P，2 脚接负极 GND。配套胶壳为 C7437163，压接端子为 C19192974。采购成品线束时按针序核对正负极。[立创商城料号与配套件](https://item.szlcsc.com/372557.html) · [首韩尺寸图](https://atta.szlcsc.com/upload/public/pdf/source/20190521/C393945_901596BDF8E8ACEC9A1CC39C3294128D.pdf)。配套电池为 250 mAh、4.20 V 充满的单节带保护板电池包。
- U2 采用 BQ25185DLHR。R3 为 18 kΩ ±1%（0402，C25762），设置 4.20 V 终止电压和 500 mA 输入限流档。R4 为 3.3 kΩ ±1%（0402，C25890），标称快充电流 90.9 mA，相当于 250 mAh 电池的 0.36 C。系统负载优先，充电电流可随输入余量下降。TH1 为 10 kΩ、B25/85 = 3435 K 的板上热敏电阻，布局时靠近电池并验证它与电芯的温差。充电参数最终按成品电池规格确认。[TI BQ25185 数据手册](https://www.ti.com/lit/ds/symlink/bq25185.pdf)
- USB 供电时 SYS 典型值为 4.5 V，连接模组 27 脚。电池保护板输出另接 61 脚 VBATS。模组 17 脚的 3.3 V 输出默认关闭，由固件使能，原理图上该路外接总标称电容为 2 µF。模组已集成 Flash、PSRAM、时钟及天线。[立创模组硬件说明](https://wiki.lckfb.com/zh-hans/hspi-sf32lb52/lckfb-open-sf32lb52-mod/overview.html)
- 状态灯 D2 和逻辑缓冲器 U3 由 SYS 供电。D2 指定 WS2812B-2020-V6（C52917434），原厂给出的 5 V 静态电流为 ≤1 µA。U3（SN74LV1T34）在 3.3 V 或 5 V、25 °C、输入为低且输出空载时，静态电流上限为 1 µA。电池供电低于 3.4 V 时熄灯。进入待机前发送全黑并锁存，随后保持 PA32 为低。麦克风用完后关闭 VOUT2。USB 挂起时可将 PA11 置高暂停充电并关闭高耗电负载，挂起电流在首板测量。[Worldsemi V6 规格书](https://datasheet.lcsc.com/datasheet/pdf/3d7b9e7b14ab294153a71be7a578635c.pdf?productCode=C52917434) · [Worldsemi V6 升级公告](https://www.world-semi.com/web/index.php?classid=22&id=587&lanstr=en&topclassid=19) · [TI 缓冲器规格书](https://www.ti.com/lit/ds/symlink/sn74lv1t34.pdf)

板上 TH1 按典型参数估算，冷、热停充点约为 2 °C 和 60 °C。打样后需实测板上探头和电芯之间的温差，并按实际电池包的充电规格核对停充范围。[TI BQ25185 数据手册](https://www.ti.com/lit/ds/symlink/bq25185.pdf)

### 每天录音 15 分钟的续航预算

按标称 250 mAh、预留 20% 容量估算，可用电量为 200 mAh。目标 90 天对应每天 2.22 mAh，全天平均约 93 µA。若电池端待机（含实际蓝牙广播或保持连接）平均 30 µA，每天待机 23 小时 45 分钟用去约 0.71 mAh，录音、编码、蓝牙传输和灯效合计约剩 1.51 mAh。按 15 分钟折算，工作电流预算约 6.04 mA。若传输延续到松键之后，尾段电量也从这 1.51 mAh 中扣除。

首板用电池供电，在电池端记录正常蓝牙待机的平均电流，以及从按键唤醒到传输结束、回到待机的每次耗电量。另在室温比较熄灯后接入与断开 D2、U3 的待机电流差。若确认超出给这一路预留的 5 µA，再用高侧开关同时切断灯和缓冲器。以上为设计预算，续航以实测为准。

## 固件引脚

| 功能 | 模组脚 / GPIO | 连接方式 |
| --- | --- | --- |
| USB D+ / D− | 13 / PA35，12 / PA36 | 原生全速设备，USBLC6-2SC6 保护 |
| VBUS 检测 | 2 / PA44 | 100 kΩ 与 120 kΩ 分压 |
| 录音 / 上一项 / 下一项 | 3 / PA43，4 / PA42，7 / PA41 | 低有效，固件配置内部上拉及待机唤醒 |
| 侧键取消 / 复位 | 14 / PA34 | 装电池时，按下 SW4 由 BAT_P 经 R13（3.3 kΩ）拉高 PA34，R14（10 kΩ）下拉。长按约 10 秒触发硬件复位。仅接 USB 时无需支持此键 |
| 麦克风时钟 / 数据 | 40 / PA07，39 / PA08 | PDM1 |
| RGB 数据 | 16 / PA32 | SN74LV1T34 缓冲到 SYS 电平 |
| 充电 / 故障状态 | 8 / PA40，9 / PA39 | 开启 VOUT2 后读取两个状态位，按 BQ25185 状态表解码 |
| 暂停充电 | 31 / PA11 | 高电平禁充，100 kΩ 下拉使独立充电默认开启 |
| 调试 TX / RX | 29 / PA19，30 / PA18 | 各串 100 Ω 到 TP1、TP2；与 SWCLK、SWDIO 复用 |

首烧用独立供电的 3.3 V USB-UART 转接器。转接器 RX 接 TP1，TX 接 TP2，GND 接 TP3。目标板由底端 USB-C 或电池供电。TP4 是软件开启后才有电压的 VOUT2 参考点，TP5 是 SYS 测量点。[立创烧录调试指南](https://wiki.lckfb.com/zh-hans/hspi-sf32lb52/lckfb-open-sf32lb52-mod/design-guide-burn.html)

## 封装与验证范围

模组和麦克风使用嘉立创 EDA 官方器件库 C42924267、C5155190 的焊盘。主键、侧键与侧发光充电灯沿用项目原封装。本版沿用 62 mm 外壳和天线端主板缺口。装壳后的蓝牙射频性能、USB 枚举、录音底噪、待机电流和充电温升留待首板实测。[模组封装](https://jlcpcb.com/user-center/lcsvg/svg.html?code=C42924267&detail=1) · [麦克风器件页](https://item.szlcsc.com/5783847.html)
