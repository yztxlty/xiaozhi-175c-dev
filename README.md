# YGSoul XiaoZhi ESP32 固件

这是基于 [XiaoZhi ESP32](https://github.com/78/xiaozhi-esp32) 的设备端固件项目，针对 **Waveshare ESP32-S3-Touch-AMOLED-1.75C** 开发板进行了适配和界面定制。

本项目将语音交互、网络连接、音频处理和设备显示整合到 ESP32-S3 硬件上，并保留 XiaoZhi 原有的多板卡、多语言和 MCP 扩展能力。

## 项目特点

- 支持 Wi-Fi 网络连接和网页配网。
- 支持 WebSocket、MQTT + UDP 两种通信方式。
- 支持流式语音识别（ASR）、大语言模型（LLM）和语音合成（TTS）交互链路。
- 支持 OPUS 音频编解码、扬声器播放和麦克风采集。
- 支持 ESP-SR 离线语音唤醒能力。
- 支持设备端 MCP，可用于控制设备音量、灯光、GPIO 等能力。
- 支持电池状态显示和电源管理。
- 支持中文、英文、日文等多语言界面。
- 支持 LVGL 图形界面和触摸屏设备。
- 为 YGSoul 设备提供启动画面、角色图像、说话动画和定制化配色。

## 支持硬件

当前项目重点适配：

- 主控：Espressif ESP32-S3
- 开发板：Waveshare ESP32-S3-Touch-AMOLED-1.75C
- 屏幕：1.75 英寸 AMOLED 触摸屏
- Flash：32 MB
- 图形库：LVGL 9.4
- ESP-IDF：5.5.2 或更高版本

相关板卡实现位于：

~~~text
main/boards/waveshare/esp32-s3-touch-amoled-1.75/
~~~

上游工程仍保留其他 ESP32-C3、ESP32-S3、ESP32-P4 等板卡的通用支持，但本仓库的默认定制目标是上述 1.75C 开发板。

## 开发环境

建议使用以下环境：

- Linux、macOS 或 Windows
- ESP-IDF 5.5.2 及以上版本
- 已配置好 ESP-IDF 环境变量的终端
- USB 数据线和可识别的 ESP32-S3 串口设备

确认 ESP-IDF 已生效：

~~~bash
idf.py --version
~~~

## 获取代码

~~~bash
git clone https://github.com/yztxlty/xiaozhi-175c-dev.git
cd xiaozhi-175c-dev
~~~

## 编译 YGSoul 1.75C 固件

### 方法一：使用项目预置配置

仓库中的 sdkconfig.175c 已包含 1.75C 开发板所需的关键配置，包括：

- 选择 Waveshare ESP32-S3-Touch-AMOLED-1.75C 板卡。
- 启用设备端回声消除。
- 使用 32 MB Flash。
- 使用 partitions/v2/32m.csv 分区表。

在项目根目录执行：

~~~bash
idf.py set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.175c" reconfigure
idf.py build
~~~

### 方法二：使用菜单配置

~~~bash
idf.py set-target esp32s3
idf.py menuconfig
~~~

在菜单中完成以下设置：

1. Xiaozhi Assistant → Board Type → Waveshare ESP32-S3-Touch-AMOLED-1.75C。
2. Serial flasher config → Flash size → 32 MB。
3. Partition Table → 选择自定义分区表 partitions/v2/32m.csv。
4. 根据需要配置默认语言、通信协议、唤醒词和资源。

保存配置后执行：

~~~bash
idf.py build
~~~

## 烧录与串口监视

将开发板通过 USB 连接到电脑，替换下面的串口路径：

~~~bash
idf.py -p /dev/ttyUSB0 flash monitor
~~~

Windows 示例：

~~~bash
idf.py -p COM3 flash monitor
~~~

退出串口监视器：

~~~text
Ctrl + ]
~~~

首次启动或设备没有保存 Wi-Fi 信息时，固件会进入 Wi-Fi 配置流程。按照屏幕和配网页面的提示完成网络配置即可。

## 使用说明

1. 烧录完成后重启开发板。
2. 按照 Wi-Fi 配网提示连接网络。
3. 通过 XiaoZhi 服务端或兼容服务配置设备。
4. 完成配置后即可进行语音交互。

设备默认使用 XiaoZhi 项目对应的服务配置。如需使用其他服务端，请在项目配置中修改 OTA 地址、通信方式或服务端参数，并重新编译烧录。

## YGSoul 界面定制

YGSoul 定制资源和显示逻辑位于：

~~~text
main/boards/waveshare/esp32-s3-touch-amoled-1.75/
├── assets/ygsoul_companion.png       # 角色图像资源
├── ygsoul_boot_lvgl.h                # 启动画面资源
├── ygsoul_companion_lvgl.h           # 角色图像 LVGL 资源
├── ygsoul_logo_png.h                 # Logo 图像资源
└── esp32-s3-touch-amoled-1.75.cc     # 板卡显示和交互实现
~~~

当前定制包括启动 Logo、角色显示、待机/说话动画、界面背景色、状态文字颜色和聊天文字颜色处理。公共语音交互和协议状态机仍由 XiaoZhi 通用模块负责。

## 目录结构

~~~text
main/                  # 固件主程序、协议、音频和板卡实现
main/boards/            # 各型号开发板适配代码
main/assets/            # 音频、语言和界面资源
managed_components/     # ESP-IDF Component Manager 下载的依赖组件
partitions/             # 分区表配置
scripts/                # 构建、资源处理和检查脚本
docs/                   # 协议、开发和设计文档
~~~

## 相关文档

- [自定义开发板指南](docs/custom-board.md)
- [WebSocket 通信协议](docs/websocket.md)
- [MQTT + UDP 通信协议](docs/mqtt-udp.md)
- [MCP 协议交互流程](docs/mcp-protocol.md)
- [MCP 物联网控制用法](docs/mcp-usage.md)
- [YGSoul 圆形触摸屏设备端 UI 设计规范](docs/设计规范/YGSoul_圆形触摸屏设备端_UI设计规范_V1.0.md)
- [Waveshare ESP32-S3-Touch-AMOLED-1.75C 板卡说明](main/boards/waveshare/esp32-s3-touch-amoled-1.75/README.md)

## 开发与检查

显示定制的静态契约检查脚本：

~~~bash
bash scripts/test_ygsoul_display_contract.sh
~~~

提交代码前请检查：

~~~bash
git diff --check
git status --short
~~~

## 忽略文件说明

仓库不会提交以下本地生成内容：

- build/、dist/ 等构建产物。
- components/、未修改的 managed_components/ 等依赖缓存。
- sdkconfig、dependencies.lock 等本地生成配置。
- .env、密钥、令牌、日志和编辑器配置。
- .DS_Store、Python 缓存和临时文件。

设备相关的 sdkconfig.175c、YGSoul 图片资源、必要的 Wi-Fi 配网页面和源码会保留在仓库中，以便复现该设备版本。

## 许可证

本项目遵循仓库中的 [MIT License](LICENSE)。使用第三方组件时，请同时遵守对应组件的许可证要求。

## 致谢

本项目基于 XiaoZhi ESP32 开源项目，感谢原项目及相关开源组件作者对 AI 硬件生态的贡献。
