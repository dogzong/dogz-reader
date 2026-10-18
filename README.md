# Dogz Reader

**Dogz Reader（狗宗阅读器）**：面向微雪 **ESP32-S3 ePaper 3.97** 的中文墨水屏阅读固件。以阅读为主，加入本地录音、音乐播放和可配置的语音 AI 助手，使用实体按键操作，无需触屏。

基于 [CrossMux](https://github.com/0x1abin/crossmux) 和 [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) 二次开发，保留上游许可证、版权声明与第三方资源许可。Dogz Reader 是独立社区项目，不代表上游或硬件厂商。

## 硬件与状态

- 主要适配设备：Waveshare ESP32-S3 ePaper 3.97，无触屏版本。
- 此仓库是当前开发源码快照，不是经过所有场景验收的稳定发行版；仓库中的其他板型配置来自上游，不代表已完成对应真机验证。
- ESP32-C3 与 ESP32-S3 固件不能混刷；本设备请使用 `waveshare_epaper_397` 构建目标。
- 当前仅发布源码。没有提供本项目的预编译 Release、网页刷机或 OTA 服务；不要将上游发行页中的固件当作 Dogz Reader。

## 当前功能

| 功能 | 说明 |
|---|---|
| 中文阅读 | 保留上游电子书、书库、字体、书签等能力，主要阅读格式含 EPUB / TXT |
| 微信读书 | 保留 CrossMux 的非官方微信读书功能，依赖相关服务接口 |
| 本地录音 | 录音、录音文件浏览与回放，依赖板载麦克风及音频硬件 |
| 本地音乐 | MP3 / WAV、独立播放页、大封面、进度、音量、暂停与继续 |
| 语音 AI | 录音转文字、问答和上下文；需要另行配置兼容网关、ASR 与问答服务 |
| 其他应用 | 保留上游应用及本分支待机等功能；联网功能需自行配置服务 |

### 音乐与封面

音频放在 TF 卡 `/Music/`；macOS 的 `._` 隐藏副本不会列入歌曲列表。
同名 BMP 可作单曲封面，`/Music/cover.bmp` 可作通用封面，没有封面时显示黑胶图。
可用 `python3 scripts/prepare_music_cover.py input.jpg output.bmp` 转换图片，需要 Pillow。

播放页：

- 侧面上 / 下：上一曲 / 下一曲。
- 正面左 / 右：音量加 / 减，每次 5%。
- 确认：暂停 / 继续；返回：停止并返回列表。

详细说明见 [音乐播放器](docs/music-player.zh-CN.md)。**当前退出播放器会停止音乐，尚不支持边读书边后台播放。**

## 获取源码

```sh
git clone --recurse-submodules https://github.com/dogzong/dogz-reader.git
cd dogz-reader
```

已有工作副本可运行 `git submodule update --init --recursive`。配套 [FreeInk SDK](https://github.com/dogzong/freeink-sdk) 固定到具体提交，需保留该版本以复现当前代码。

**当前配套 SDK 仍为私有仓库，等待独立公开授权；没有读取权限的用户暂时无法递归拉取或完成构建。** 下述构建步骤以能够访问 SDK 为前提。

## 构建

安装 Git、Python 和 PlatformIO Core，并准备可访问依赖下载站点的网络环境：

```sh
python3 -m venv .venv
# macOS / Linux
source .venv/bin/activate
python -m pip install platformio
pio run -e waveshare_epaper_397
```

Windows 可使用 `.venv\Scripts\activate` 激活环境。首次构建会下载 ESP32 工具链、SDK 依赖，并执行字体和资源生成，耗时取决于网络与主机。更多信息见 [构建说明](docs/engineering/build-system.md)。

主要输出位于本项目配置的 `.pio/waveshare_epaper_397/`：

- `firmware.bin`：应用镜像，不含完整首次安装所需的引导与分区信息。
- `firmware.factory.bin`：该目标构建生成的合并镜像。

## 刷机

先确认板卡型号、串口和构建目标，备份自己的 TF 卡与设备配置。通过 USB 连接后：

```sh
pio run -e waveshare_epaper_397 -t upload --upload-port <你的串口>
```

需要手工写入时，应按该次构建的上传日志确认镜像与偏移；本目标应用分区为 `0x10000`，合并镜像从 `0x0` 写入。应用镜像与合并镜像不可交换偏移。合并镜像可能覆盖已有分区内配置，日常升级优先按构建工具的上传流程操作。
代码发布不意味着本次又完成了全量编译或真机测试；历史测试结果不可替代新硬件、新依赖环境下的验收。

## AI 与隐私

AI 功能不附送可用服务，也不包含作者的设备令牌、Wi-Fi 密码、DeepSeek 或 ASR API key。
固件可修改网关地址；历史默认地址指向作者的服务，但未经授权无法使用。自行部署兼容网关后，按 [设备配置与接口说明](docs/reader-ai-device.zh-CN.md) 设置 HTTPS 地址、设备令牌、可信 CA 和网络时间。
上游模型密钥应只保存在服务端，设备仅保存网关令牌。使用语音 AI 时录音会上传到所配置的网关，再由其调用 ASR；转写文字及会话上下文会发送给问答服务。费用及数据处理规则取决于你选择的服务。

本公开仓库仅包含固件、配套构建工具和相关文档；不公开作者的服务端实现、私人部署记录、服务器环境配置或设备 NVS。没有配置 AI 不影响本地阅读、录音和音乐。
已有部分网络资源和更新入口继承自 CrossMux；它们属于上游服务，不是 Dogz Reader 自有服务。使用上游 OTA 可能替换为上游固件，当前建议自行构建并通过 USB 更新。

## 后续迭代

1. **MP3 后台播放**：前台阅读时持续播放，协调 TF 卡访问、录音打断与按键。
2. **3.5 mm 耳机输出**：核查电路后扩展耳机接口与输出切换。
3. **耳机线控**：上一曲、下一曲、播放 / 暂停，需验证目标耳机和检测电路。

以上尚未交付，硬件扩展不代表裸板现有接口可以直接接耳机。详见 [音频迭代计划](docs/audio-roadmap.zh-CN.md)。

## 反馈与贡献

通过 [Issues](https://github.com/dogzong/dogz-reader/issues) 提交问题，请附板卡型号、提交版本、复现步骤及去除令牌和个人信息的日志。
开发约定见 [AGENTS.md](AGENTS.md) 和 [工程文档](docs/engineering/index.md)。上游历史文档可能仍使用 CrossMux / CrossPoint 名称，其历史能力不等同于本分支完成验收。

## 许可证与致谢

主体代码沿用 [MIT License](LICENSE)，第三方代码、字体、图标及 SDK 按各自目录中的许可证分发，不能将主体许可证理解为对所有资源统一重新授权。
感谢 CrossMux、CrossPoint Reader、FreeInk SDK 及其贡献者。保留原始版权声明；Dogz Reader 的独立命名不改变上游代码归属。
