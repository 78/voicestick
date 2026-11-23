# Voice Stick

[English](README.md) · 简体中文

Voice Stick 是一个适用于 macOS、Windows 和 Linux 的蓝牙语音输入按键。你可以用现成的 M5Stack StickS3，也可以自己制作 **VoiceStick C3 v1**。复刻自研版所需的打板文件、外壳 STL 和 STEP、装配顺序及烧录命令都在[硬件复刻指南](hardware/README_ZH.md)。

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

| | M5Stack StickS3 | VoiceStick C3 v1 |
| --- | --- | --- |
| 固件构建目标 | `m5stick_s3`（默认） | `voicestick_c3` |
| 主控与音频 | ESP32-S3-PICO-1-N8R8，ES8311 的 I2S 接口输出 16 kHz 单声道 PCM | ESP32-C3、外挂 8 MB Flash，SD18OB371 PDM 麦克风默认 1.536 MHz，经 CIC3 抽取得到 16 kHz PCM |
| 按键 | 主键 GPIO11，侧键 GPIO12 | 主键 GPIO4、取消 GPIO9、上一页 GPIO8、下一页 GPIO21 |
| 状态显示 | ST7789P3 竖屏，135 × 240，GPIO38 控制背光 | GPIO20 驱动 WS2812B 状态灯 |
| 电源与唤醒 | PMIC 中断 GPIO13，电池供电空闲 5 分钟进入深睡 | GPIO2 控制外设供电，GPIO3 采样电池，GPIO10 读充电状态。正常深睡可由主键或插 USB 唤醒 |

StickS3 的引脚见[板级头文件](firmware/boards/m5stick_s3/components/stick_s3_board/include/stick_s3_board.h)。自研板的晶振、麦克风、电源和完整引脚表见 [C3 v1 电路说明](hardware/voicestick_c3/README_ZH.md)。

## 录音、确认与配对

设备广播名为 `VS-XXXX`，末四位来自 eFuse MAC 地址。将已烧录的设备开机，在 macOS 菜单栏选择 `Pair Device...`，选中相符的设备并点击 `Pair`。可以重复配对多台设备，应用会分别显示连接状态，也可通过配置项 `paired_device_ids` 管理设备。

| 状态（默认按住说话） | 主键 | 侧键 / 取消键 |
| --- | --- | --- |
| 未连接 | 等待连接，StickS3 屏幕显示设备名 | 无操作 |
| 已连接、空闲 | 按住开始录音 | 恢复上次可找回的待确认文本 |
| 录音中 | 松开结束录音 | 继续录音 |
| 等待识别 | 等待当前识别结束 | 取消识别 |
| 1.2 秒粘贴倒计时 | 暂停倒计时，转为手动确认 | 取消待粘贴文本 |
| 手动确认 | 确认粘贴 | 取消待粘贴文本 |

设备上报 `button_down`、`button_up` 和按键角色，桌面应用管理交互状态并回传 `ui_state`。StickS3 显示配对、就绪、录音、识别、待确认、错误和电池状态，空闲 30 秒后调暗背光。接 USB 或充电时保持在调暗状态。

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

两种板型使用各自的构建目录。选一组命令，将串口替换为实际端口。

```sh
# M5Stack StickS3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor

# VoiceStick C3 v1
idf.py -B build-c3 -DBOARD=voicestick_c3 build
idf.py -B build-c3 -p /dev/cu.usbmodemXXXX flash monitor
```

`boards/<board>/board.cmake` 选择芯片，板级默认配置叠加在固件公共配置之上。可通过 `idf.py -B build-c3 menuconfig` 调整 C3 的麦克风时钟、声道和增益。公共依赖包括 `espressif/button`、`espressif/cjson` 和 `78/esp-opus`；StickS3 另用 `espressif/esp_codec_dev` 和 `lvgl/lvgl`，C3 另用 `espressif/led_strip`。

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

推送与 `VERSION` 匹配的 `v<version>` 标签会触发发布流程，生成 macOS 安装包、StickS3 和 VoiceStick C3 固件，并部署网站和 appcast。每款固件都包括 BLE OTA 应用镜像、从偏移 0x0 写入的合并镜像和 `manifest.json`。StickS3 沿用原有 OSS 路径，C3 的版本目录和 `latest/` 位于 `voicestick/firmwares/voicestick_c3/` 下。macOS 在应用启动、设备连接或重连时检查固件，也可从菜单手动刷新；运行期间的定时检查最多每 24 小时一次。开始 OTA 前会校验镜像大小和 SHA-256。

CI 使用 `ALIYUN_OSS_ACCESS_KEY_ID`、`ALIYUN_OSS_ACCESS_KEY_SECRET`、`ALIYUN_OSS_ENDPOINT`、`ALIYUN_OSS_BUCKET`，公开下载地址由 `ALIYUN_OSS_PUBLIC_BASE_URL` 配置，`ALIYUN_OSS_PREFIX` 默认是 `voicestick/firmwares`。完整步骤见[发布说明](docs/release.md)。

## macOS 本地配置

从仓库根目录复制示例：

```sh
mkdir -p "$HOME/Library/Application Support/VoiceStick"
cp desktop/macos/Config/config.example.toml "$HOME/Library/Application Support/VoiceStick/config.toml"
```

```toml
asr_provider = "volcengine"
voicestick_api_key = ""
voicestick_cloud_url = "wss://api.xiaozhi.me/voicestick/asr/"
volcengine_api_key = "your_volcengine_asr_api_key"
llm_base_url = "https://api.openai.com/v1"
llm_api_key = "your_openai_compatible_llm_api_key"
llm_model = "gpt-5.5"
interaction_mode = "hold_to_talk"
resource_id = "volc.seedasr.sauc.duration"
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
| `asr_provider` | `volcengine` 表示直连，`voicestick_cloud` 表示中继。两者分别使用对应的 API Key 和服务地址 |
| `llm_base_url`、`llm_api_key`、`llm_model` | 翻译服务的兼容接口地址、凭据和模型 |
| `interaction_mode` | `hold_to_talk` 按住说话，`click_to_talk` 点击切换 |
| `resource_id` | 火山资源可选 `volc.seedasr.sauc.duration`、`volc.seedasr.sauc.concurrent`、`volc.bigasr.sauc.duration`、`volc.bigasr.sauc.concurrent` |
| `asr_hotwords` | 逗号分隔的热词，翻译时也用作术语提示 |
| `paired_device_ids`、`device_theme_colors`、`device_overlay_positions` | 配对设备 ID、各设备主题色与浮层位置 |
| `auto_enter` | 粘贴后按回车 |
| `[output]`、`[device.<id>.output]` | 选择当前输入框或字幕、原文或翻译以及目标语言，可按设备覆盖 |
| `debug_audio_cache`、`debug_audio_dir` | 开启 Ogg Opus 调试录音缓存及设置保存目录 |

API Key 仅保存在本机。开启调试缓存后，有效会话保存到 `~/Library/Application Support/VoiceStick/DebugAudio`，文件名包含设备 ID。短于 0.5 秒的录音会被丢弃。

## 许可证

项目采用 [MIT 许可证](LICENSE)。
