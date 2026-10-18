# AI 深度任务（设备端）

更新：2026-10-04。目标为无触屏的 Waveshare ESP32-S3 ePaper 3.97（800×480）。
接口依据：协调者冻结的《Reader AI 深度任务改造 —— 接口契约（冻结版 v1）》第 2、3 节；本文只描述设备端（固件）实现。
既有的 [dogzclaw 设备端说明](reader-ai-device.zh-CN.md) 中「首页提供语音提问、恢复任务、历史记录、设置及取消任务」与「前台轮询最多 120 秒」两句已被本文取代（该文归其作者维护，未在本轮修改）。

## 功能概述

AI 首页由 5 项变为 6 项：语音提问、**深度任务**、继续上次任务、历史回答、连接设置、取消上次任务。上下导航沿用 NavPrevious/NavNext 映射，选中项 6 项循环；确认选择，返回退出。

「深度任务」复用既有语音提问流程（告知 → 录音 ≤60 秒 → 上传识别 → 提交问答），区别只有两点：

1. 提交 `POST /questions` 时带 `"mode": "deep"`，由服务端转 dsh-runner 执行（15 秒–3 分钟）。
2. 前台轮询上限由 120 秒放宽到 300 秒。

等待页在服务端上报阶段时显示对应文案，并显示服务端上报的已等待秒数；没有 `stage` 时保持原有「正在处理，返回可停止等待。任务已保存，可稍后继续。」不变。

## 协议层（`src/network/ReaderAiClient.*`）

- `question(..., const std::string& parent = "", Mode mode = Mode::Quick)`：`Mode::Deep` 时序列化 `"mode":"deep"`，`Mode::Quick`（缺省）**完全不写该字段**，payload 与改造前逐字节一致（契约 §2.1：缺省 = quick）。幂等键、父任务、UTF-8 与长度校验均不变。
- `status(id, Status&)`：解析 `GET /jobs/{id}`，`Status` 含 `state`、`stage`、`elapsed`。
  - `state` 仍按 `queued/running/completed/failed/cancelled` 严格校验，非法值仍是 `protocol` 错误。
  - `stage` 与 `elapsed_s` 是可选字段：缺失、类型不符（字符串/负数/浮点/null）一律降级为 `Stage::None` / `0`，不报错。
  - 未知阶段名（例如服务端将来新增）同样降级为 `Stage::None`，保证新服务端不会让设备轮询失败。
- `state(id, std::string&)` 保留为 `status()` 的薄封装，旧调用方与旧测试行为不变。

## 状态上报与 UI

- 工作线程在每次轮询后把阶段发布为 `std::atomic<readerai::Stage>`、把秒数发布为 `std::atomic<uint32_t>`；UI 任务读取枚举而非字符串，避免跨任务读写 `std::string` 的竞态（`elapsed_s` 是仅用于显示的字段，不参与协议判定）。
- 问答有两个等待界面，两者都显示阶段：转写校对关闭（默认）时，问答是在**转写页**上轮询的，阶段替换「深度求索中…」并在同一行右侧显示已等待秒数；转写校对打开、以及恢复/重试任务时，等待界面是 `State::Busy` 页，阶段替换通用文案、秒数画在左下角。阶段文案为：正在检索… / 正在思考… / 深度任务执行中… / 正在整理…；`queued` 与未知阶段沿用原文案。
- 转写等待页（`work_ == Work::Transcribe`）保持「正在识别语音…」不变。
- 秒数只在服务端给出 `elapsed_s` 时显示（`已等待 N 秒`）；重绘只在阶段或秒数变化时触发，即约每个轮询周期一次局部刷新，而不是每 tick 一次。

## 超时与恢复

- `kPollTimeoutMs = 120000`（quick，原值）、`kDeepPollTimeoutMs = 300000`（deep）。
- `poll()` 内部判定：`deep_ && work_ == Work::Question` 才用 300 秒。转写任务（`work_ == Work::Transcribe`）与 quick 问答一律 120 秒。
- 超时仍然只调用 `fail("timeout")`，不删除 `pending.json`；重试/「继续上次任务」继续轮询同一 `answer_job`，不重复创建任务。
- `pending.json` 仅在深度任务时新增 `"deep": true`；旧文件没有该字段时按 false（quick）处理，quick 记录与改造前逐字节一致。因此重启后恢复深度任务仍使用 300 秒上限与 `mode: "deep"`。

## Quick 路径为何不受影响

| 环节 | 代码路径 | deep 改造后的行为 |
|---|---|---|
| 提交 | `Client::question()` 仅在 `mode == Mode::Deep` 时写字段 | quick 缺省 → payload 与旧版逐字节相同 |
| 转写轮询 | `run()` 中 `poll(client, sourceId_, question_)`，此时 `work_ == Work::Transcribe` | 上限仍为 120000 ms |
| 问答轮询 | `poll(client, answerId_, answer_)`，`deep_ == false` | 上限仍为 120000 ms |
| 阶段文案 | 渲染仅在服务端给了 `stage` 时替换（问答的两种等待界面） | 旧服务端无 `stage` → 文案与旧版一致（转写页仍是「深度求索中…」） |
| 秒数 | 仅在服务端给了 `elapsed_s` 时绘制 | 旧服务端无该字段 → 不绘制 |
| 恢复 | `pending.json` 无 `deep` 字段 | 读为 quick |

## 资源占用

| 项目 | 改造前 | 改造后 | 净增 |
|---|---|---|---|
| Flash | 6429483 B（98.1%，剩余 124117 B） | 6431487 B（98.1%，剩余 122113 B） | **+2004 B** |
| 静态 RAM | 127708 B（39.0%） | 127708 B（39.0%） | **0** |

按 ELF 符号归因（符号尺寸合计 +1543 B，其余约 +461 B 为段对齐/填充）：

- `VoiceAssistantActivity`：+917 B（`poll` 换成 `status()` 解析、`render` 新增阶段与秒数、两个等待界面的分支）
- i18n 字符串与偏移表：+610 B（英文 +87、中文 +103、35 种语言偏移表 +12×35 = +420）
- ArduinoJson 模板实例：+82 B；`ReaderAiClient`：+79 B
- 其余为 LTO 重排（无关代码重新排布 -145 B）

`VoiceAssistantActivity` 由 `replaceActivityWith<>()` → `makeUniqueNoThrow<>()` 堆分配，新增成员 `stage_/elapsed_/lastElapsed_/lastStage_/deep_` 约 13 字节只在页面存活期间占用堆。

## 新增 i18n 键

| 键 | 中文 | 英文 |
|---|---|---|
| `STR_AI_DEEP_TASK` | 深度任务 | Deep task |
| `STR_AI_STAGE_SEARCHING` | 正在检索… | Searching… |
| `STR_AI_STAGE_THINKING` | 正在思考… | Thinking… |
| `STR_AI_STAGE_AGENT` | 深度任务执行中… | Running the deep task… |
| `STR_AI_STAGE_WRITING` | 正在整理… | Writing up… |
| `STR_AI_ELAPSED_FMT` | 已等待 %u 秒 | Waiting %u s |

只写中英两份 YAML，其余 33 种语言由 `scripts/gen_i18n.py` 回退到英文。中文用字全部取自现有 `lib/EpdFont/scripts/cn_i18n_chars.txt` 子集（无需重做 CJK 字库，见 `docs/engineering/chinese-build.md`），新增测试会断言这一点，避免出现缺字空白。

## 验证

```bash
python3 scripts/tests/test_voice_deep_task.py     # 新增：协议 + 菜单 + 阶段 + 超时，36 项断言
python3 scripts/tests/test_voice_trim.py
python3 scripts/tests/test_audio_player_controls.py
PLATFORMIO_LIBDEPS_DIR=/private/tmp/crossmux-clean-libdeps python3 scripts/tests/test_reader_ai.py
```

`test_reader_ai.py` 原先把 ArduinoJson 头文件路径写死为 `.pio/libdeps/...`，在 `PLATFORMIO_LIBDEPS_DIR` 指向临时目录的干净构建流程下会找不到依赖；已改为优先读取该环境变量。

## 待真机验证

- 6 项菜单在 480×640 首页的排版（`GUI.drawList` 的行高与 6 行是否全部可见）。
- 转写页与 Busy 页上的阶段文案、秒数位置是否与正文重叠（本文只做了源码级与主机级验证，未刷机）。
- 服务端 `stage`/`elapsed_s` 的真实取值与上报时机（本轮只按契约做了模拟响应测试）。
- 300 秒上限下的功耗与自动休眠（`preventAutoSleep()` 在等待期间已返回 true）。
- 待确认的设计取舍：继续追问沿用上一轮的模式（深度对话的追问仍走 deep），quick 对话不受影响；`queued` 阶段不单独配文案，沿用原通用文案。
