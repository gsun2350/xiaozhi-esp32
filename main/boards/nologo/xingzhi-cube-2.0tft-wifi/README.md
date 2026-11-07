# 星智 Cube 2.0 TFT Wi-Fi TF 音乐版

独立板型标识为 `xingzhi-cube-2.0tft-wifi-tf-music`，避免与其他官方 OTA 板型混用。硬件引脚来自 `NologoTech/xingzhi-cube-2.0tft` 与 `conghuy93/xingzhicube2.0` 的同板型实现，详见 `docs/xingzhi_cube_tf_music_design.md`。两份参考项目均为 MIT；MP3 解码器使用 `lieff/minimp3` 的 CC0 代码，许可证保留在 `main/music/third_party/`。

TF 卡使用 SPI2：MOSI 40、MISO 39、SCLK 38、CS 46。显示使用 SPI3。GPIO46 的冷启动和刷机行为必须实机确认。

编译命令与测试清单见 `docs/xingzhi_cube_tf_music_build.md`。音乐播放期间关闭语音唤醒检测，保留 Boot 按键进入对话；按键对话恢复和 AEC 尚待实机验证。

首次实机启动发现参考源码的 GPIO16 自动电源键判断会误触发重启/关机，现已禁用该判断；GPIO48 电源锁存、电池检测和音量 ADC 仍保留。物理电源键的自动关机行为待确认电路后再实现。
