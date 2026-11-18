# 自制 VoiceStick C3 v1

[English](README.md) · 简体中文

VoiceStick C3 v1 是自研蓝牙语音按键的首个公开硬件版本，采用 ESP32-C3，带一个主键、三个侧键、PDM 麦克风和 RGB 状态灯。PCB 为 50 × 30 mm、四层、1.0 mm 厚。

每款板子在 `hardware/` 下各有一个与固件板型同名的目录。当前板型为 [voicestick_c3](voicestick_c3/README_ZH.md)，硬件版本为 v1。

## v1 发布附件

打板和打印用 `voicestick-c3-v1-build-files.zip`，解压后按目录取件。需要在 KiCad 中查看含元件的整板，或重新导出完整整板模型时，将 `voicestick-c3-v1-component-models.zip` 解压到源码的 `hardware/voicestick_c3/` 下，七个 STEP 会落在 `kicad/3dmodels/`。[VoiceStick C3 v1 发布页](https://github.com/78/voicestick/releases/tag/hw-voicestick-c3-v1)

| 步骤 | 文件和说明 |
| --- | --- |
| 1. 打板和贴片 | 在复刻包的 `fab/` 中取件。按[制造说明](voicestick_c3/fab/README_ZH.md)设置板厚和叠层，上传 Gerber ZIP、BOM 和贴片坐标。C25、C26 默认不贴，提交订单前在贴片预览里核对方向。 |
| 2. 打印和装配 | 先打公差试件 ToleranceCoupon。STL 零件（`enclosure/out/stl/`）已按打印方向摆好；单件 STEP（`enclosure/out/step/`） 可用于 CAD 修改或导入支持 STEP 的切片器，按零件表选择贴床面。再按[打印参数、辅料和装配顺序](voicestick_c3/enclosure/README_ZH.md)制作。使用带保护板的 602025 两线 250 mAh 电池，J2 为 JST SH 1.0 mm 两针，1 脚接电池正极，2 脚接负极。 |
| 3. 上电和烧录 | 先按制造说明做无电池 USB 上电检查。激活 ESP-IDF release-v6.1，在仓库根目录执行下方命令，将串口占位符换成板子的实际端口。公开发布的预编译镜像适用于 StickS3，v1 使用 `voicestick_c3` 目标。 |

```sh
cd firmware
idf.py -B build-c3 -DBOARD=voicestick_c3 build
idf.py -B build-c3 -p /dev/cu.usbmodemXXXX flash monitor
```

**验证进度**　仓库附有 [ERC 报告](voicestick_c3/kicad/erc.rpt)、[DRC 报告](voicestick_c3/kicad/drc.rpt)和[外壳干涉与间隙检查结果](voicestick_c3/enclosure/out/enclosure-report.json)。整机的麦克风底噪、装壳后的天线匹配和深睡电流仍待首板实测。复刻前可先看[首板检查项目](voicestick_c3/README_ZH.md#待办下单--首板前)。

修改设计时可查阅[电路与 GPIO 说明](voicestick_c3/README_ZH.md)、原理图 PDF（`kicad/voicestick-c3-v1-schematic.pdf`）和 [KiCad 工程](voicestick_c3/kicad/voicestick-c3-v1.kicad_pro)。整机 STEP（`enclosure/out/voicestick-c3-v1-assembly.step`）包含 PCB、电池和外壳，可用于对照装配位置。
