# 星智 Cube 2.0 TFT 本地音乐实现说明

## 基线与硬件来源

本分支基于 `78/xiaozhi-esp32` 的 `main` 提交 `a73e9a7e266e3196d1368ba46c30bee0d81f0a64`。ESP-IDF 要求为 v6.0.1 以上，优先 `release/v6.1`。

板级引脚取自 `NologoTech/xingzhi-cube-2.0tft` 的 `main/boards/xingzhi-cube-2.0tft-wifi/config.h`：I2S 为 WS 4、MCLK 5、BCLK 6、DOUT 15、DIN 17；音频 I2C 为 SDA 41、SCL 42；显示 SPI3 为 MOSI 10、CLK 9、DC 8、CS 14、RST 18、背光 13；Boot 为 0；LED 为 45。TF 卡 SPI2 引脚 40/39/38/46 已与第三份源码 `conghuy93/xingzhicube2.0` 的 `config.h` 核对，实机已成功挂载并读取歌曲。GPIO46 是启动配置引脚，冷启动及下载模式仍需实测。板卡参考源码的 MIT 许可证保存在板型目录的 `LICENSE.reference`。

| 硬件组合 | 输入 | 输出 | AEC 参考 | 处理方式 |
| --- | --- | --- | --- | --- |
| ES8311 单芯片，I2C 0x18 | 单声道 | ES8311 | 未证实 | 采用 `Es8311AudioCodec`，不宣称 AEC 可用 |
| ES7210 0x41 + ES8311 0x18 | 由 `BoxAudioCodec` 决定 | ES8311 | 需要实机验证 | 采用 `BoxAudioCodec`，根据其输入布局配置参考声道 |
| 未检测到 ES8311 | 不可用 | 不可用 | 不可用 | 启动时记录明确错误，不猜测替代引脚 |

ES7210/ES8311 地址使用 7 位 I2C 地址。配置里的 0x82 与 0x30 是 8 位地址表示。`AUDIO_INPUT_REFERENCE=true` 只说明参考固件的配置意图，不代表设备的回声通道已通过验证。

## 软件链路

`SDCardManager` 挂载 `/sdcard` FAT32，不自动格式化。歌曲 ID 仅允许 ASCII 字母、数字、下划线和短横线，限制长度；路径优先为 `/sdcard/music/{song_id}.mp3`，纯数字短 ID 在精确文件不存在时可补零到三位。播放器在独立 FreeRTOS 任务中读取和解码，以连续整数相位将非 24 kHz MP3 转成单声道 24 kHz，输出有界 PCM 队列；队列满时等待可用空间而不丢帧。`AudioService::AudioOutputTask()` 是唯一写入 `AudioCodec` 的任务，TTS 和系统提示优先于音乐。

实机 I2C 探测只发现 ES8311 `0x18`，未发现 ES7210 `0x41`；当前设备使用单声道 ES8311 输入分支。参考板电源键 GPIO16 的短按/长按判定曾误触发重启和关机，因此暂不启用该自动关机逻辑，保留电池和音量检测。

音乐状态包含 `STOPPED`、`PLAYING`、`PAUSED`、`INTERRUPTED_BY_VOICE`、`ERROR`。Boot 按键触发 AI 对话前暂停音乐；当待播音乐处于 `INTERRUPTED_BY_VOICE` 时，本轮 TTS 回复结束即回到空闲态，播放器再等待 TTS 音频队列排空后恢复。明确的暂停、停止指令取消自动恢复。播放期间关闭语音唤醒检测，暂停和停止后恢复，满足用户接受的按键唤醒方式。用户已实测《好宝宝》回复后自动播放；AEC 尚未验证。

本地音乐恢复时还必须结束对应的在线语音会话。仅切换设备状态为 `IDLE` 不会关闭 MQTT 音频通道，服务端可能在播放一段时间后发送“我先退下啦”等超时回复。应在本轮 TTS 音频队列排空后主动关闭通道，再保持本地播放；若期间开始新一轮对话，应取消待关闭动作，避免误关新会话。

设备 MCP 工具：`self.music.play`、`pause`、`resume`、`stop`、`next`、`previous`、`get_status`。工具返回“命令已接收”与“正在播放”不同状态，不把入队视为实际播放成功。官方 `xiaozhi.me` 后台不修改。

## 验证门槛

先完成主机测试、ESP-IDF 编译，再在设备上检查启动、I2C 探测、显示、音频、TF 卡、按键打断、MCP 调用与聊天恢复。任何未执行的项目明确列为未验证。
