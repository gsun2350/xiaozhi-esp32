# 编译、烧录与验收

## 编译

使用 ESP-IDF `release/v6.1`，最低 v6.0.1。导出环境后，在仓库根目录执行：

```sh
source /path/to/esp-idf/export.sh
idf.py --version
python3 scripts/build.py --list-boards
python3 scripts/build.py nologo/xingzhi-cube-2.0tft-wifi --name xingzhi-cube-2.0tft-wifi-tf-music
```

没有本机 ESP-IDF 时，也可以在仓库根目录用官方容器构建：

```sh
docker run --rm --platform linux/arm64 -v "$PWD":/project -w /project -e HOME=/tmp \
    espressif/idf:release-v6.1 python3 scripts/build.py \
    nologo/xingzhi-cube-2.0tft-wifi --name xingzhi-cube-2.0tft-wifi-tf-music
```

该脚本会按板型配置目标芯片、依赖和分区。编译成功后以 `build/` 中的实际产物与 `flasher_args.json` 为准，不要套用其他板型的烧录偏移量。连接 USB 后先确认串口，再执行：

```sh
idf.py -p /dev/cu.usbmodem11201 flash monitor
```

烧录会覆盖设备现有固件，建议先备份原固件及配网信息。板型标识与官方旧版本不同，OTA 兼容性必须单独核对。

## 实机验收清单

- 冷启动、复位、进入下载模式，确认 GPIO46 不影响启动。
- I2C 日志确认 ES8311 `0x18`、可选 ES7210 `0x41`；记录输入声道和 AEC 实际状态。
- Wi-Fi 连接、xiaozhi.me 激活、屏幕、背光、LED、麦克风、扬声器、Boot 按键、电池显示。
- FAT32 有卡/无卡启动，`0001.mp3`、不存在文件、损坏 MP3、长文件名、热拔卡读取错误。
- 44.1/48 kHz、单/双声道、不同码率；播放、暂停、继续、停止、下一首、上一首、连续切歌。
- 播放时按 Boot 键，对话、TTS 完成后自动恢复；明确暂停/停止/切歌后不错误恢复。
- Wi-Fi 断开时本地音乐继续播放；连续播放两小时并进行二十次 AI 对话。
- 若要评估语音唤醒，另测 1 米、音量 60%、二十次唤醒及误唤醒；此项不是当前最低验收要求。

以下记录只覆盖已经执行的检查。按键对话、实际听感和长时间运行仍需逐项实测。

## 当前环境验证记录（2026-11-06）

- `scripts/build.py --list-boards --json` 已识别新板型，目标为 `esp32s3`。
- 主机侧测试：运行 150 项，149 项通过，1 项因缺少 cJSON 等开发头文件跳过。为兼容 macOS 上没有 `python` 命令的环境，测试时仅在临时目录提供 `python` 包装命令；仓库未因此修改。
- `git diff --check` 通过。
- 使用官方 `espressif/idf:release-v6.1` 镜像完成正式固件编译，版本 `v6.1-829-g9a97f6c54ec`。`build/xiaozhi.bin` 为 2,738,272 字节，OTA 应用分区剩余约 34%。Flash 参数以 `build/flash_args` 为准。
- 设备实测为 ESP32-S3、16 MB Flash、8 MB PSRAM。刷机前完整备份到 `/private/tmp/xingzhi-original-2026-11-06.bin`，文件权限 `0600`；原分区表与本项目 16 MB 分区布局一致。刷机清单没有写 NVS；启动后 NVS 的部分字节由固件自行更新，不能声称分区内容完全不变，但原有 Wi-Fi 配置仍可连接。
- 首次刷机发现参考电源键 GPIO16 判定会在启动后误触发重启和关机。已禁用该自动电源键判断，保留 GPIO48 电源锁存、电池检测和音量 ADC。修复后 50 秒日志只有一次预期复位，没有错误、自动重启或关机。
- 实机启动日志确认：ES8311 `0x18` 被检测到，ES7210 `0x41` 未检测到；约 968 MB TF 卡成功挂载；Wi-Fi 已连接，官方 MQTT 服务完成激活；七个 `self.music.*` 工具已注册。
- 主机使用 `minimp3` 解码了 44.1 kHz 双声道和 48 kHz 单声道样本。设备端从 TF 卡上的实际歌曲读到了 44.1 kHz 双声道 MP3；播放约 2 秒后进度为 1,976 ms，暂停 1 秒进度未变，恢复约 2 秒后进度为 4,082 ms；停止后重新扫描仍有 2 首有效歌曲。
- 设备端确认播放和恢复期间语音唤醒检测关闭，暂停与停止后重新开启；播放期间仅以 Boot 按键唤醒。测试任务和测试 MP3 均已删除，正式固件已刷入；最后一次复位后的 22 秒日志确认 TF 卡挂载、Wi-Fi 与 MQTT 连接、7 个音乐 MCP 工具注册，未出现异常复位。
- 已从 xiaozhi.me 知识库页面读取 `playList.csv` 的三条记录：`1`《稻香》、`2`《孤勇者》、`3`《好宝宝》。TF 卡文件 ID 为 `002`、`003`。补零兼容经设备验证：`play("2")` 打开 `002.mp3`，下一首为 `003`，上一首回到 `002`。`1` 没有对应 MP3，播放《稻香》仍会返回 `file_not_found`。
- 设备端实际听感、Boot 按键对话与自动恢复、屏幕显示、麦克风采集、冷启动、长时间稳定性和 AEC 尚未验收。

## 云端点歌联调记录（2026-11-07）

- 初次实测《稻香》和《好宝宝》均被告知“没有”。历史对话显示调用的是官方在线 `search_music`；当时智能体的知识库下拉框为“不使用知识库”，官方“音乐”服务已启用。
- 经用户确认，已在小智控制台将知识库绑定为“锦鲤的ai知识库”，关闭官方在线“音乐”服务并保存。重新打开配置已确认两项设置持久化。
- 第一次重试《好宝宝》已改为调用 `search_knowledge` 与设备 `self.music.play`，但知识库检索回答仅包含歌名与歌手，没有返回 ID，智能体错误传入 `"0"`，设备返回 `file_not_found`。
- 已将三首已确认歌曲的 ID 明确写入智能体角色提示词，禁止猜测 `"0"`，并重启设备以加载配置。用户复测时，《好宝宝》最终可以播放，但需要再按一次 Boot 键。
- 代码检查发现自动听取模式会在 TTS 结束后回到 `LISTENING`，而播放器只在 `IDLE` 恢复；已增加板级判定，在有待恢复音乐时于 TTS 结束后回到 `IDLE`，同时等待语音输出队列排空。新固件已通过 Docker ESP-IDF 6.1 编译、刷入设备并通过启动检查。用户实测只按一次 Boot 说“播放好宝宝”，设备回复后自动开始播放，无需再次按键。
- 用户随后反馈播放一段时间会听到“我先退下啦”。排查确认此前仅将设备切换为 `IDLE`，未主动结束 MQTT 在线语音会话。现改为等待本轮 TTS 音频队列排空后发送 `goodbye` 并关闭音频通道；若新对话已开始则取消待关闭动作。新固件编译、刷写校验通过。实机日志显示《好宝宝》回复后从 `SPEAKING` 进入 `IDLE`，随后打印 `Closing voice session after music reply`、`Closing audio channel, send_goodbye: 1`，再开始解码 44.1 kHz 双声道 MP3；持续监测期间未见服务端再次播报。用户复测播放约一分钟，确认未再听到“我先退下啦”，音乐持续正常播放。
