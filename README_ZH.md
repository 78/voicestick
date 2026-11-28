# Voice Stick

[English](README.md) · 简体中文

Voice Stick 是一个适用于 macOS、Windows 和 Linux 的蓝牙语音输入按键。你可以用现成的 M5Stack StickS3 或 Magiclick 2P5，也可以自己制作 **VoiceStick C3 v1**。复刻自研版所需的打板文件、外壳 STL 和 STEP、装配顺序及烧录命令都在[硬件复刻指南](hardware/README_ZH.md)。

默认模式下，按住主键录音，松开后桌面应用将音频送去识别。确认倒计时结束，识别结果会粘贴到当前输入框。设置里的 `auto_enter` 控制粘贴后是否按回车。

## 项目目录

| 目录 | 内容 |
| --- | --- |
| [hardware/](hardware/README_ZH.md) | 自研硬件的 KiCad 工程、制造文件、外壳和装配说明 |
| [firmware/](firmware/) | ESP-IDF 固件。每种板型在 `boards/` 下有自己的实现 |
| [desktop/macos/](desktop/macos/) | Swift 原生菜单栏应用 |
| [desktop/windows/](desktop/windows/README.md) · [desktop/linux/](desktop/linux/README.md) | 各桌面平台的工程和构建说明 |
| [docs/](docs/) | BLE 协议、火山引擎 ASR 和发布流程 |
| [scripts/](scripts/) | 打包、发布和固件图像资源转换工具 |

## 板型

| | M5Stack StickS3 | VoiceStick C3 v1 | Magiclick 2P5 |
| --- | --- | --- | --- |
| 固件构建目标 | `m5stack_stick_s3`（默认） | `voicestick_c3` | `magiclick_2p5` |
| 主控与音频 | ESP32-S3-PICO-1-N8R8，ES8311 的 I2S 接口输出 16 kHz 单声道 PCM | ESP32-C3、外挂 8 MB Flash，SD18OB371 PDM 麦克风默认 1.536 MHz，经 CIC3 抽取得到 16 kHz PCM | ESP32-S3，ES8311 采集 16 kHz 单声道 PCM |
| 按键 | 主键 GPIO11，侧键 GPIO12 | 主键 GPIO4、取消 GPIO9、上一页 GPIO8、下一页 GPIO21 | 中键 GPIO21 录音和配对，左键 GPIO0 取消 |
| 状态显示 | ST7789P3 竖屏，135 × 240，GPIO38 控制背光 | GPIO20 驱动 WS2812B 状态灯 | 128 × 128，按 PCB 版本选择 GC9107 或 ST7735 |
| 电源与唤醒 | PMIC 中断 GPIO13，电池供电空闲 5 分钟进入深睡 | GPIO2 控制外设供电，GPIO3 采样电池，GPIO10 读充电状态。正常深睡可由主键或插 USB 唤醒 | GPIO7 采样电池，GPIO48 读充电状态，中键从深睡唤醒 |

StickS3 的引脚见[板级头文件](firmware/boards/m5stack_stick_s3/components/stick_s3_board/include/stick_s3_board.h)。自研板的晶振、麦克风、电源和完整引脚表见 [C3 v1 电路说明](hardware/voicestick_c3/README_ZH.md)。

Magiclick 2P5 复位时按住左键可进入 USB 下载模式。

## 录音、确认与配对

设备会自动连回已配对的电脑，固件升级后也是如此。首次配对或换电脑时，按以下步骤操作。

1. 从未配对过的设备开机或唤醒后，自动打开 60 秒配对窗口，可直接跳到第 3 步。
2. 如果是换电脑，先从原电脑断开设备，再按一下设备的语音主键。配对窗口持续 60 秒，C3 在窗口打开时快闪蓝灯。
3. 在要配对的 Mac 上，从菜单栏选择 `Pair Device...`，选中相符的 `VS-XXXX` 并点击 `Pair`。如果 macOS 弹出蓝牙配对确认，点「配对」。

设备名末四位来自 eFuse MAC 地址。每台设备记住一台电脑，Mac 应用可以同时连接多台设备。设备进入深睡后，先按语音主键唤醒。

| 状态（默认按住说话） | 主键 | 侧键 / 取消键 |
| --- | --- | --- |
| 未连接 | 按一下进入配对模式 | 无操作 |
| 已连接、空闲 | 按住开始录音 | 恢复上次可找回的待确认文本 |
| 录音中 | 松开结束录音 | 继续录音 |
| 等待识别 | 等待当前识别结束 | 取消识别 |
| 粘贴倒计时（默认 1 秒） | 暂停倒计时，在同一浮层的正文下方展开确认按钮 | 取消待粘贴文本 |
| 手动确认 | 粘贴窗口中编辑后的文字 | 取消待粘贴文本 |

macOS 菜单栏里的 `Send Confirmation` 可以选择立即粘贴、1、2、3、5、10、15 秒倒计时或手动确认。确认窗也可以用触控板编辑文字、点 **Send** 粘贴或点 **Cancel** 取消。

StickS3 和 MagiClick 2P5 在暂停确认时也可以摇晃设备，取消当前草稿。

语音和游戏共用 `button_down` / `button_up` 事件，设备上报按键 ID 和设备时间戳。VoiceStick 0.5.0 需要搭配固件 0.5.0 才能使用输入功能，旧固件仍可连接并从设备菜单升级。桌面应用管理交互状态并回传 `ui_state`。StickS3 显示配对、就绪、录音、识别、待确认、错误和电池状态，空闲 30 秒后调暗背光。接 USB 或充电时保持在调暗状态。

## 音频和 BLE 协议

固件将单声道 PCM 编码成 Opus，通过 BLE 通知发送。桌面应用将 Opus 封装成 Ogg Opus，经 WebSocket 送往火山引擎或 VoiceStick Cloud，结果可显示为浮层、字幕或粘贴到当前输入框。

GATT 服务 UUID 为 `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100`。以下特征与服务共用前面的 UUID 字段。

| 特征 | 最后四位 | 方向与操作 |
| --- | --- | --- |
| `audio_tx` | `5101` | 设备 → 桌面，notify |
| `state_tx` | `5102` | 设备 → 桌面，notify |
| `control_rx` | `5103` | 桌面 → 设备，write without response |

完整消息格式和命令见 [BLE 协议](docs/protocol.md)。火山引擎接入细节见 [ASR 说明](docs/volcengine-asr.md)。

## 编译和烧录固件

先激活 ESP-IDF release-v6.1。例如，使用 EIM 安装的环境可以执行：

```sh
source ~/.espressif/tools/activate_idf_release-v6.1.sh
cd firmware
```

每种板型使用各自的构建目录。选一组命令，将串口替换为实际端口。

```sh
# M5Stack StickS3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor

# VoiceStick C3 v1
idf.py -B build-c3 -DBOARD=voicestick_c3 build
idf.py -B build-c3 -p /dev/cu.usbmodemXXXX flash monitor

# Magiclick 2P5
idf.py -B build-magiclick-2p5 -DBOARD=magiclick_2p5 build
idf.py -B build-magiclick-2p5 -p /dev/cu.usbmodemXXXX flash monitor
```

`boards/<board>/board.cmake` 选择芯片，板级默认配置叠加在固件公共配置之上。可通过 `idf.py -B build-c3 menuconfig` 调整 C3 的麦克风时钟、声道和增益。公共依赖包括 `espressif/button`、`espressif/cjson` 和 `78/esp-opus`。StickS3 和 Magiclick 2P5 使用 `espressif/esp_codec_dev` 和 `lvgl/lvgl`。StickS3 用 `espressif/bmi270` 支持摇晃取消，Magiclick 的 GC9107 屏幕使用 `espressif/esp_lcd_gc9a01`。Magiclick 和 C3 共用 `espressif/adc_battery_estimation` 估算电量，C3 用 `espressif/led_strip` 驱动状态灯。

分区表包含两个 3 MB OTA 槽和一个 1984 KB 存储区。使用旧单应用分区表的 StickS3，需要先通过 USB 运行一次 `idf.py -p /dev/cu.usbmodemXXXX erase-flash flash monitor`，再使用 BLE OTA。该命令会擦除设备上的现有数据。

## 桌面应用与发布

macOS 应用支持 macOS 12 及以上版本：

```sh
cd desktop/macos
swift build
swift run VoiceStickApp
```

首次运行时允许蓝牙访问。应用通过模拟 Command-V 粘贴文字，如果系统拦截按键，请在系统设置的「辅助功能」中授权运行它的终端或应用。

Linux 客户端使用 GTK4 和 libadwaita，配置文件为 `~/.config/voicestick/config.toml`。依赖、编译命令和 GNOME 顶栏设置见 [Linux 客户端说明](desktop/linux/README.md)。

从仓库根目录生成带 Sparkle 更新支持的 macOS 安装包：

```sh
SPARKLE_PUBLIC_ED_KEY="..." scripts/build-macos.sh --release
scripts/make-dmg.sh
```

生成的 App、ZIP 和签名在 `build/` 下。Windows 在本地签名机运行 `scripts\build-msi.bat`，脚本签名可执行文件、WinSparkle DLL 和 MSI。发布时将 MSI 上传到对应的 GitHub Release，再运行 `Deploy Website to GitHub Pages` 更新 appcast。

推送与 `VERSION` 匹配的 `v<version>` 标签会触发发布流程，生成 macOS 安装包和 StickS3、VoiceStick C3、MagiClick 2P5 固件，并部署网站和 appcast。每款固件都包括 OTA 应用镜像、从偏移 `0x0` 写入的合并镜像和清单。StickS3 沿用原有 OSS 路径。C3 和 MagiClick 2P5 的版本目录和 `latest/` 分别位于 `voicestick/firmwares/voicestick_c3/` 和 `voicestick/firmwares/magiclick_2p5/` 下。macOS 在应用启动、设备连接或重连时检查支持 OTA 的设备，也可从菜单手动刷新。运行期间的定时检查最多每 24 小时一次。开始 OTA 前会校验镜像大小和 SHA-256。

CI 使用 `ALIYUN_OSS_ACCESS_KEY_ID`、`ALIYUN_OSS_ACCESS_KEY_SECRET`、`ALIYUN_OSS_ENDPOINT`、`ALIYUN_OSS_BUCKET`，公开下载地址由 `ALIYUN_OSS_PUBLIC_BASE_URL` 配置，`ALIYUN_OSS_PREFIX` 默认是 `voicestick/firmwares`。完整步骤见[发布说明](docs/release.md)。

### StickS3 休息球场（macOS 原型）

给 StickS3 刷入本分支的固件，启动同一分支构建的 Mac App，从菜单栏选择 **休息一下 · 3D 网球**。拿稳后轻挥一次即可绑定球拍，进入无球、无倒计时的自由试拍。绿色标记朝向拍面正面。可点「动作测试」逐项检查左右翻腕、拍头倾斜和快挥识别，点「开始比赛」后才发球。多台同时挥动时，放稳其他设备再试。识别窗口为 12 秒，超时可点「重新识别」。球拍菜单也支持重新挥动识别、按副键指定和手动选择。来球弹起后，接近拍面时拍框会亮起。挥动球拍迎上球即可回球，漏球后由对面重新发球。每局两分钟，球拍自动移位。绑定后可随时转动手腕调整拍面，握稳后按 R 可将当前握姿设为正中。关闭球场窗口即可继续按键说话。没有体感设备时可以先按空格体验。

USB 构建与刷写命令见上方 StickS3 说明。开发时先运行 `swift build`，让游戏和菜单栏 App 两个可执行文件位于同一构建目录。[体感数据与本地接口](docs/protocol.md#sticks3-motion-sessions-v1)

## macOS 本地配置

从仓库根目录复制示例：

```sh
mkdir -p "$HOME/Library/Application Support/VoiceStick"
cp desktop/macos/Config/config.example.toml "$HOME/Library/Application Support/VoiceStick/config.toml"
```

```toml
asr_provider = "voicestick_cloud"
voicestick_api_key = ""
voicestick_cloud_url = "wss://api.xiaozhi.me/voicestick/asr/"
openai_api_key = ""
interaction_mode = "hold_to_talk"
asr_hotwords = "小智,VoiceStick"
paired_device_ids = ""
device_theme_colors = ""
device_overlay_positions = ""
auto_enter = true
debug_audio_cache = false
# debug_audio_dir = "~/Library/Application Support/VoiceStick/DebugAudio"

[output]
target = "focused_app"
transform = "original"
translation_target = "en"

# 可选：按设备设置字幕翻译
# [device.C3D8.output]
# transform = "translate"
# translation_target = "en"
```

| 配置 | 用途 |
| --- | --- |
| `asr_provider` | macOS 可选 `voicestick_cloud` 或 `openai` |
| `openai_api_key` | macOS 上 OpenAI 实时转写、整理和翻译共用的 API Key |
| `llm_base_url`、`llm_api_key`、`llm_model` | 旧版自托管 Cloud 中继可用的文本接口配置 |
| `interaction_mode` | `hold_to_talk` 按住说话，`click_to_talk` 点击切换 |
| `asr_hotwords` | 逗号分隔的热词，整理和翻译时也用作术语提示 |
| `paired_device_ids`、`device_theme_colors`、`device_overlay_positions` | 配对设备 ID、各设备主题色与浮层位置 |
| `auto_enter` | 粘贴后按回车 |
| `[output]`、`[device.<id>.output]` | 选择当前输入框或字幕，输出原文、整理稿（macOS）或译文，可按设备覆盖 |
| `debug_audio_cache`、`debug_audio_dir` | 开启 Ogg Opus 调试录音缓存及设置保存目录 |

API Key 仅保存在本机。开启调试缓存后，有效会话保存到 `~/Library/Application Support/VoiceStick/DebugAudio`，文件名包含设备 ID。短于 0.5 秒的录音会被丢弃。

在 macOS 上，从设备菜单选择 **Text Output → Refine**，即可纠正明确的识别错误，并用原语言整理口述内容。ASR 选 **OpenAI Realtime** 时，整理和翻译自动通过同一把 OpenAI API Key 使用 GPT-6 Luna。
选择 **VoiceStick Cloud** 时，整理和翻译共用 Cloud Key，处理结果通过转写连接返回。

原先选择火山直连的 macOS 用户打开新版后会看到 **VoiceStick Cloud**。可以使用已有的 Cloud Key，或在设置里点 **Apply Trial**。

## 许可证

项目采用 [MIT 许可证](LICENSE)。
