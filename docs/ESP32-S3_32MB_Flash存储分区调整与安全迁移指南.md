# ESP32-S3 32MB Flash 存储分区调整与安全迁移指南

> 适用项目：YGSoul XiaoZhi ESP32 固件
> 目标硬件：Waveshare ESP32-S3-Touch-AMOLED-1.75C
> 已验证硬件：ESP32-S3、32MB Flash、8MB PSRAM
> 文档日期：2026-08-27

## 1. 文档目标

本文不是一次烧录记录，而是一份可复用的工程指南，用于解决以下问题：

- 在不能扩大 Flash、不能增加外置存储的情况下重新分配 32MB 空间。
- 同时保留双槽 OTA、系统 UI 资源、离线歌曲、小游戏资源和用户存档空间。
- 在改变分区边界时，始终保持至少一个可恢复的启动路径。
- 识别“物理写入正确，但运行时映射错误”这类普通烧录校验发现不了的问题。
- 建立从设计、构建、备份、迁移、回读、启动到人工验收的完整门禁。

本文以当前项目的实际实现为准。相关文件：

- 分区表：`partitions/v2/32m.csv`
- 构建校验：`scripts/verify_storage_build.sh`
- 分区与迁移契约测试：`scripts/test_storage_foundation.py`
- 安全迁移脚本：`scripts/flash_32m_storage_migration.sh`
- 可写内容服务：`main/storage/content_storage.h`、`main/storage/content_storage.cc`
- 资源加载实现：`main/assets.cc`
- 板级幽光资源加载：`main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc`

## 2. 最重要的工程结论

### 2.1 分区容量正确，不等于分区位置正确

最初采用的容量同样是“6MiB + 6MiB + 6MiB + 12MiB”，但顺序为：

```text
ota_0  @ 0x00200000, 6MiB
ota_1  @ 0x00800000, 6MiB
assets @ 0x00E00000, 6MiB
content@ 0x01400000, 12MiB
```

`assets` 从 14MiB 开始并跨过 16MiB 物理地址边界。烧录工具对资源文件的回读摘要完全一致，但固件通过 `esp_partition_mmap()` 映射整个 Assets 分区后，运行时校验值错误，导致资源包被判定无效，启动画面和对话动画回退到轻量界面。

本轮现场证据：

1. 本地生成的资源包头、长度和校验和一致。
2. `verify_flash` 证明 Flash 中的字节与 `generated_assets.bin` 一致。
3. 同一资源包位于 `0x00E00000` 时，运行时计算值与包内校验值不一致。
4. 未修改资源内容，只把 Assets 移到 `0x00200000`～`0x00800000` 后，运行时校验恢复正常。
5. 连续两次真机重启均打印“全部幽光 CBin 资源加载成功”，启动、待机、聆听和说话状态恢复。

因此，涉及大块只读映射时必须同时验证：

- 文件生成校验；
- 烧录后物理回读校验；
- 固件运行时映射校验；
- 真机视觉与交互结果。

> 该现象是当前目标芯片、Flash、ESP-IDF 版本和映射方式组合下的实测结果，不应未经验证推导成所有 ESP32-S3 的通用限制。但在本项目中，禁止再让完整 Assets 映射范围跨越 16MiB 物理边界。

### 2.2 分区迁移的安全性取决于写入顺序

“先写新分区表，再写应用”存在断电窗口：新表生效后，目标应用槽可能还没有有效镜像。

正确顺序是：

1. 保持旧分区表不动。
2. 先写未来 `ota_0`，立即回读校验。
3. 再写未来 `ota_1`，立即回读校验。
4. 两个未来应用槽都写入有效镜像并通过物理回读后才写新分区表。
5. 再写 Assets、Content 和 OTA 选择数据。
6. 最后一次性回读所有新布局产物。

这一顺序保证迁移全过程不存在“没有已知可启动应用”的状态。`verify_flash` 只证明镜像字节一致；备用槽是否能独立启动，仍必须在迁移完成后通过受控 OTA 槽切换验证。

### 2.3 不允许把运行时自动格式化当作修复

Content 挂载失败可能来自断电、介质错误、分区表错误或文件系统损坏。如果启用 `format_if_mount_failed=true`，一次普通启动就可能永久清空离线歌曲、游戏资源和存档。

当前项目采用：

- 构建阶段生成预格式化的 12MiB wear-levelled FAT 镜像；
- 首次迁移时写入该镜像；
- 运行时始终使用 `format_if_mount_failed=false`；
- 挂载失败时报告 `Corrupt` 或 `Unavailable`，但不破坏可恢复数据。

### 2.4 映射资源的指针生命周期必须显式管理

Assets 刷新、重新初始化或取消映射后，LVGL 不能继续持有原映射地址。否则可能出现随机花屏、崩溃或资源切换后失效。

当前幽光图片采用以下策略：

1. 从 Assets 包校验 `YGI1` 头、颜色格式、尺寸、步长和负载长度。
2. 把像素负载复制到 PSRAM 管理的 `LvglAllocatedImage`。
3. LVGL 只持有生命周期稳定的 PSRAM 指针。
4. Assets 后续取消映射不会使当前显示对象悬空。

PSRAM 的用途是运行时图像缓存和稳定指针，不应被误认为可以替代 Flash 持久存储。

## 3. 当前推荐分区布局

### 3.1 分区表

当前最终布局如下：

| 分区 | 类型 | 起始地址 | 大小 | 结束地址 | 职责 |
|---|---|---:|---:|---:|---|
| `nvsfactory` | data/nvs | `0x00009000` | 200KiB | `0x0003B000` | 工厂配置和设备身份 |
| `nvs` | data/nvs | `0x0003B000` | 840KiB | `0x0010D000` | Wi-Fi、用户配置和运行参数 |
| `otadata` | data/ota | `0x0010D000` | 8KiB | `0x0010F000` | OTA 启动槽选择 |
| `phy_init` | data/phy | `0x0010F000` | 4KiB | `0x00110000` | 射频校准数据 |
| 保留区 | — | `0x00110000` | 960KiB | `0x00200000` | 地址对齐和后续底层扩展余量 |
| `assets` | data/spiffs | `0x00200000` | 6MiB | `0x00800000` | 只读字体、UI、音效和幽光动画资源 |
| `ota_0` | app/ota_0 | `0x00800000` | 6MiB | `0x00E00000` | 当前或候选固件 |
| `ota_1` | app/ota_1 | `0x00E00000` | 6MiB | `0x01400000` | 当前或候选固件 |
| `content` | data/fat | `0x01400000` | 12MiB | `0x02000000` | 歌曲、游戏资源、封面和存档 |

布局必须满足：

```text
assets.end  == ota_0.start
ota_0.end   == ota_1.start
ota_1.end   == content.start
content.end == 0x02000000
```

任何分区变更都必须由测试解析 CSV 和生成后的二进制分区表，不能只靠人工目测十六进制地址。

### 3.2 为什么采用 6 + 6 + 6 + 12

#### 双 6MiB OTA

- 两个应用槽大小必须相同，才能稳定轮换 OTA。
- 当前应用约 2.83MiB，单槽仍有约 3.17MiB 余量。
- 音乐、皮肤和关卡不能塞入应用镜像，否则两个 OTA 槽会重复存储同一内容。

#### 6MiB Assets

- 当前资源包约 3.37MiB，仍有约 2.63MiB 余量。
- 只存随固件发布的只读资源，不存用户下载内容。
- 动效优先使用“高清公共底图 + 小范围差分帧”，避免每个状态重复保存整张 280×280 图。

#### 12MiB Content

- FATFS 实际可用容量会略低于分区标称容量。
- 当前真机挂载结果约为总容量 12140KiB、初始可用 12120KiB。
- 内容配额为音乐 8MiB、游戏 2MiB、小文件 0.5MiB，并强制保留 1.5MiB 安全余量。

## 4. 容量规划方法

### 4.1 应用槽

应用槽不能只判断“当前能否放下”，还要为未来代码、库升级和链接波动留空间。

建议门槛：

| 使用比例 | 状态 | 处理方式 |
|---:|---|---|
| `< 70%` | 健康 | 正常开发 |
| `70%～80%` | 关注 | 检查大数组、字体、重复资源和调试组件 |
| `80%～90%` | 高风险 | 禁止继续把媒体资源编译进应用 |
| `>= 90%` | 不可发布 | 必须减小镜像或重新评估总体布局 |

构建硬门禁仍是应用镜像严格小于 6MiB。高水位告警不能替代硬门禁。

### 4.2 动画资源

完整 280×280 RGB565A8 图像约 235KiB。若七种表情每种保存 3～5 张完整帧，仅角色图就可能消耗数 MiB。

推荐结构：

```text
公共无嘴底图：1 张
说话嘴型：3 张小差分图
眨眼/笑眼：按眼睛最小包围盒保存
眼泪：按泪滴和必要眼部区域保存
表情切换：代码控制持续时间和轮播顺序
```

当前实测资源：

- 启动画面：约 424KiB；
- 280×280 无嘴高清底图：约 230KiB；
- 单个 38×29 嘴型：约 3.2KiB；
- 三嘴型合计：约 10KiB。

差分帧必须满足：

- 所有帧使用相同画布坐标系；
- 差分区域只覆盖确实变化的像素；
- 不重采样公共底图；
- 不增加发光边、描边、杂色或背景；
- 透明通道和色彩格式保持一致；
- 默认状态始终使用静态闭嘴帧。

### 4.3 离线音乐

压缩音频估算公式：

```text
文件字节数 ≈ 码率(kbit/s) × 时长(s) × 125
```

以单首 4 分钟为例：

| Opus 码率 | 单首约占用 | 5 首约占用 |
|---:|---:|---:|
| 24kbps | 0.69MiB | 3.43MiB |
| 32kbps | 0.92MiB | 4.58MiB |
| 48kbps | 1.37MiB | 6.87MiB |

音乐配额为 8MiB，3～5 首可以实现，但必须在下载前使用实际 `Content-Length` 做配额检查。不要按歌曲数量判断空间。

## 5. 构建前门禁

### 5.1 环境确认

```bash
idf.py --version
python3 -m esptool version
```

若系统 `python3` 没有 `esptool`，应使用 ESP-IDF 已安装依赖的 Python，并通过 `PYTHON` 环境变量传给迁移脚本。不要在系统 Python 和 ESP-IDF Python 之间混用依赖。

### 5.2 静态契约测试

在项目根目录执行：

```bash
python3 scripts/test_storage_foundation.py
bash scripts/test_ygsoul_display_contract.sh
bash scripts/test_content_storage_policy.sh
python3 scripts/export_ygsoul_cbin.py --check
bash -n scripts/flash_32m_storage_migration.sh
```

这些检查覆盖：

- 分区地址、容量、顺序和 32MB 结束边界；
- Assets、OTA 和 Content 不重叠；
- 迁移脚本不包含整片擦除、eFuse 写入或关闭下载模式；
- 两个未来应用槽先于新分区表写入；
- 幽光 CBin 头、尺寸、步长和像素负载无损；
- Content 配额、文件名安全和禁止自动格式化策略。

### 5.3 构建与产物校验

```bash
idf.py set-target esp32s3
idf.py -B build-175c \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.175c" \
  reconfigure
idf.py -B build-175c build
scripts/verify_storage_build.sh build-175c
git diff --check
```

构建校验必须确认：

- `xiaozhi.bin < 6MiB`；
- `generated_assets.bin < 6MiB`；
- `content.bin == 12MiB`；
- 二进制分区表与 `partitions/v2/32m.csv` 完全一致；
- 所有待烧录文件生成 SHA-256。

本轮通过验收的产物快照：

| 产物 | 字节数 | 分区余量 |
|---|---:|---:|
| `xiaozhi.bin` | 2,967,968 | 3,323,488 |
| `generated_assets.bin` | 3,531,606 | 2,759,850 |
| `content.bin` | 12,582,912 | 固定填满分区镜像 |

这些数值用于历史对照，不应写成未来构建的固定断言；每次交付都要重新生成并记录。

## 6. 防变砖迁移流程

### 6.1 安全门禁的定义

本项目的“安全门禁”不是一个等待动画，而是写入前必须全部成立的条件：

1. 串口连接到预期 ESP32-S3。
2. 芯片报告的 Flash 容量为 32MB。
3. 完整读取 32MB Flash 成功。
4. `nvsfactory`、`nvs`、`otadata`、`phy_init` 单独备份成功。
5. 所有备份文件尺寸正确并生成 SHA-256。
6. 旧应用镜像可被 `esptool image_info` 识别。
7. 新应用、Assets、Content 和分区表通过构建门禁。
8. 没有显式设置 `YGSOUL_FLASH_CONFIRM=YES` 时，脚本必须硬复位设备并停止写入。

完整读取 32MB、擦除 12MiB Content、写入双应用槽和逐地址回读都会耗时。耗时本身不是卡死；只有端口断开、命令非零退出、摘要不一致或长时间无进度且进程消失才属于失败。

### 6.2 第一次执行：只备份，不写入

```bash
export YGSOUL_DEVICE_BACKUP_ROOT=/absolute/path/outside-repository/device-backups
scripts/flash_32m_storage_migration.sh <PORT> build-175c
```

预期结果：

- 输出备份目录；
- 生成完整 Flash 和四个关键分区备份；
- 输出全部 SHA-256；
- 提示需要设置 `YGSOUL_FLASH_CONFIRM=YES`；
- 退出码为 `2`；
- 设备被硬复位，不会停在下载模式。

备份目录必须位于仓库外，不得提交到 Git。

### 6.3 第二次执行：复用已验证备份并授权写入

```bash
YGSOUL_REUSE_BACKUP_DIR=/absolute/path/to/verified-backup \
YGSOUL_FLASH_CONFIRM=YES \
scripts/flash_32m_storage_migration.sh <PORT> build-175c
```

如果需要指定 ESP-IDF Python：

```bash
PYTHON=/absolute/path/to/esp-idf-python \
YGSOUL_REUSE_BACKUP_DIR=/absolute/path/to/verified-backup \
YGSOUL_FLASH_CONFIRM=YES \
scripts/flash_32m_storage_migration.sh <PORT> build-175c
```

### 6.4 写入顺序与每一步的可恢复性

| 阶段 | 写入内容 | 此时可启动路径 |
|---|---|---|
| 0 | 尚未写入 | 旧分区表、旧应用 |
| 1 | 新应用 → `0x00800000`，回读通过 | 旧应用仍保留；未来 `ota_0` 已有效 |
| 2 | 新应用 → `0x00E00000`，回读通过 | 旧应用仍保留；两个未来 OTA 槽均已具备字节一致的应用镜像 |
| 3 | 新分区表 → `0x00008000`，回读通过 | 新表下已实测的 `ota_0` 可启动；`ota_1` 待受控槽切换验证 |
| 4 | Assets → `0x00200000` | 应用已可启动；资源写入中可使用回退界面 |
| 5 | Content → `0x01400000` | 应用和资源不受 Content 写入影响 |
| 6 | OTA 数据 → `0x0010D000` | 明确选择新 `ota_0` |
| 7 | 全部地址统一回读 | 所有目标字节与构建产物一致 |
| 8 | 硬复位 | 进入正常启动与真机验收 |

严禁：

- `erase_flash`；
- 写入或烧录 eFuse；
- 关闭 ROM Download Mode；
- 在没有完整备份时改写分区表；
- 在未来应用槽尚未校验前改写分区表；
- 使用未解析的环境变量、通配符或模糊地址烧录；
- 把另一个设备的 NVS、工厂配置或完整 Flash 备份写入当前设备。

## 7. 真机闭环验收

### 7.1 串口启动验收

```bash
idf.py -B build-175c -p <PORT> monitor
```

必须观察到等价日志：

```text
ContentStorage: Content storage ready: total=... KiB free=... KiB
Assets: The partition size is 6144 KB
Assets: The checksum calculation time is ... ms
WaveshareEsp32s3TouchAMOLED1inch75: Loaded all YGSoul CBin assets from the assets partition
Ota: Running partition: ota_0
StateMachine: State: activating -> idle
```

不得出现：

```text
calculated checksum ... does not match stored checksum ...
Failed to mmap assets partition
missing YGSoul asset
Failed to mount content partition without data loss
Guru Meditation Error
reboot loop
```

### 7.2 视觉和交互验收

至少执行两次软件复位和一次断电冷启动，并逐项检查：

- 启动画面显示高清幽光形象；
- 进入对话前跑马灯文字颜色仍为白色；
- 待机默认显示不说话的静态帧；
- 唤醒后进入聆听状态，底部状态文字显示正常；
- TTS 播放期间只切换三个嘴型，主体、眼睛、爱心和位置保持固定；
- 对话结束立即恢复闭嘴帧；
- 动画无闪烁、切割痕迹、白底、绿底、毛边或透明杂质；
- Wi-Fi、MQTT、麦克风、扬声器和触摸均正常；
- Content 挂载成功且重启后仍可读；
- 没有资源校验失败或持续内存下降。

双槽 OTA 还需单独完成一次闭环：从当前 `ota_0` 通过正常 OTA 接口写入 `ota_1`、切换启动、确认业务正常，再执行下一次 OTA 回到另一槽。禁止通过手工伪造 `otadata` 代替正常 OTA 链路测试。

物理回读通过但视觉异常，仍然判定为不通过。串口日志正常但用户看不到启动画面或嘴型变化，也判定为不通过。

## 8. 典型故障诊断

| 现象 | 优先检查 | 正确处理 |
|---|---|---|
| 启动画面和动图同时消失 | Assets 运行时校验日志 | 检查分区位置、映射范围、包头和运行时校验，不先修改 UI |
| `verify_flash` 成功但运行时 checksum 失败 | 物理地址边界与 `esp_partition_mmap()` 范围 | 用同一镜像换到不跨关键边界的位置复验 |
| UI 可启动但使用回退形象 | Assets 无效或必需 CBin 缺失 | 保留回退能力，修复资源包或布局后重新烧录 |
| 动图花屏或刷新后崩溃 | LVGL 是否持有已取消映射的指针 | 把已校验像素复制到生命周期稳定的 PSRAM 对象 |
| Content 挂载失败 | 分区表、预格式化镜像和 FAT/WL 参数 | 保留数据并报告损坏，禁止自动格式化 |
| 设备烧录后停在下载模式 | 烧录工具的 reset 参数 | 显式执行 hard reset，不重复擦写 Flash |
| `python3 -m esptool` 不可用 | Python 环境漂移 | 使用 ESP-IDF Python，不重新安装一套不受控工具链 |
| 应用或 Assets 接近 6MiB | 大数组、完整动画帧、字体和重复资源 | 迁移到正确分区、使用差分资源、建立高水位告警 |
| 烧录进程长时间无新日志 | 是否正在擦除/校验 12MiB Content | 先检查进程和串口状态，不在写入中拔线 |

### 8.1 本轮校验异常的排查顺序

这次问题最终能定位，是因为没有直接重画资源或反复修改 UI，而是按证据逐层排除：

1. 比较本地资源包的长度、头部和校验和。
2. 对烧入 Flash 的实际字节执行 `verify_flash`。
3. 检查目标编译器的 `char` 有符号性，排除简单求和类型差异。
4. 对比资源在不同物理地址的运行时结果。
5. 保持资源文件完全不变，只调整分区位置。
6. 连续重启并观察运行时校验、UI 和对话状态。

同一假设连续两次无效后，应回到证据收集，不要叠加更多图像处理、边缘修补或 UI 回退补丁。

## 9. 恢复策略

### 9.1 恢复前提

只要满足以下条件，软件分区迁移通常可恢复：

- ROM Download Mode 未关闭；
- 没有写入 eFuse；
- 有当前这台设备的完整 32MB 备份；
- 备份 SHA-256 和尺寸已验证；
- Bootloader、NVS 和 PHY 没有被无依据地覆盖。

### 9.2 恢复原则

1. 优先恢复旧分区表、旧应用和旧 OTA 数据。
2. 只有确认 NVS 或 PHY 本身损坏时才恢复对应分区。
3. 完整 Flash 恢复只能使用同一台设备、同一迁移前生成的备份。
4. 恢复时同样禁止全片擦除；直接按明确地址写回并回读校验。
5. 恢复后必须重新验证设备身份、Wi-Fi、屏幕、音频和对话。

紧急情况下可把同设备的完整镜像从 `0x00000000` 写回，但这是恢复操作，不是日常烧录方式。执行前必须再次核对设备、文件大小和 SHA-256，并保证供电稳定。

## 10. 后续扩展规则

### 10.1 增加新表情动效

- 先复用公共 280×280 高清底图；
- 只新增眼睛、嘴巴、泪滴或手部的最小差分区域；
- 每套状态只保留一套，不生成重复预览资源；
- 构建后比较 Assets 增量字节数；
- 真机验证位置固定、透明边缘、内存峰值和切换耗时；
- Assets 达到 70% 使用率时开始资源审计，达到 80% 时停止无预算扩张。

### 10.2 增加音乐、系统设置和小游戏

- 音频和可删除资源进入 `/content`，不能进入 OTA 应用槽；
- 游戏程序随固件更新，关卡和皮肤进入 `/content/games`；
- 系统设置数据进入 NVS 或小文件目录，按是否需要原子更新和恢复决定；
- 所有下载先写 `/content/tmp/*.part`，完成哈希校验后再原子重命名；
- 下载前同时检查分类配额、总剩余空间和 1.5MiB 安全余量；
- OTA 更新不得格式化 Content，也不得覆盖用户歌曲和存档。

### 10.3 再次调整分区的触发条件

只有满足以下任一条件，才应重新讨论分区：

- 应用槽持续超过 80%，且资源迁移、裁剪和组件精简后仍无法下降；
- Assets 持续超过 80%，且差分资源、字体子集化和压缩优化后仍不足；
- Content 的真实业务数据无法在现有配额下完成目标；
- 新功能要求新的文件系统、加密区或可靠性隔离边界。

再次调整前必须重新制定完整迁移图，不允许直接修改 CSV 后使用普通 `idf.py flash` 覆盖设备。

## 11. 发布前最终检查表

- [ ] 目标设备确认是 ESP32-S3、32MB Flash。
- [ ] 主工作区和发布工作树状态均已检查，没有覆盖用户改动。
- [ ] 分区 CSV 与二进制分区表一致。
- [ ] 应用和 Assets 均严格小于 6MiB。
- [ ] Content 镜像严格等于 12MiB 且已预格式化。
- [ ] 所有静态契约测试通过。
- [ ] 完整 32MB 备份及关键分区备份已生成并校验。
- [ ] 双未来应用槽在切换分区表前写入并回读成功。
- [ ] 所有新布局地址最终回读一致。
- [ ] 设备硬复位后正常启动，没有复位循环。
- [ ] 通过正常 OTA 链路从备用槽启动成功，并验证下一次 OTA 可回到另一槽。
- [ ] Assets 运行时校验成功。
- [ ] Content 挂载成功且不会自动格式化。
- [ ] 启动画面、默认闭嘴、聆听和说话动画真机通过。
- [ ] Wi-Fi、MQTT、触摸、麦克风和扬声器通过。
- [ ] 至少两次复位和一次冷启动通过。
- [ ] 用户人工验收通过后再合并主分支和推送远程仓库。

## 12. 结语

设备存储重分配不是单纯的 CSV 修改。真正可靠的交付必须同时处理容量预算、物理地址、OTA 可启动不变量、资源指针生命周期、文件系统数据保护、备份恢复和真机视觉验收。

本轮最有价值的经验可以概括为：

> 永远不要把“构建成功”或“烧录摘要一致”当作设备功能完成；对映射资源和新分区，必须在真实硬件上验证运行时校验、完整启动链路和用户实际看到的画面。
