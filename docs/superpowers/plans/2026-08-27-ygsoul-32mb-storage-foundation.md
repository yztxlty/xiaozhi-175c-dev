# 幽光 AI 设备 32MB 存储基础实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 32MB Flash 内完成双 6MB OTA、6MB Assets、12MB Content，并将幽光大图迁出应用镜像后完成可恢复的真机烧录验证。

**Architecture:** 只读 UI 资源继续使用项目现有 Assets mmap 容器，幽光图片以 LVGL CBin/RGB565(A8) 存放并直接映射；可写内容使用 FATFS + wear levelling。首次分区迁移通过 USB 定址烧录，烧录前备份完整 Flash 和关键数据分区，不执行整片擦除。

**Tech Stack:** ESP-IDF、LVGL 9、CBin、FATFS、wear levelling、Python 3、Shell contract tests、esptool。

## Global Constraints

- Flash 固定为 32MB，不增加外置存储。
- 分区固定为 `ota_0=6MB`、`ota_1=6MB`、`assets=6MB`、`content=12MB`。
- Content 配额固定为音乐 8MiB、游戏 2MiB、封面/存档/配置 0.5MiB、安全余量至少 1.5MiB。
- 首次迁移禁止普通 OTA、禁止全片擦除、禁止写 eFuse、禁止关闭 ROM Download Mode。
- 暂不使用虚拟设备平台，只使用真实设备验证。
- 保持现有默认闭嘴、说话三嘴型、主体固定和底部文字颜色逻辑不回退。

---

### Task 1: 分区与镜像边界契约

**Files:**
- Create: `scripts/test_storage_foundation.py`
- Modify: `partitions/v2/32m.csv`

**Interfaces:**
- Consumes: ESP-IDF CSV 分区格式。
- Produces: 固定标签和地址；后续构建、ContentStorage 和烧录脚本均依赖这些地址。

- [ ] **Step 1: 编写失败测试**

测试解析 CSV 并断言：`assets=(0x200000,0x600000)`、`ota_0=(0x800000,0x600000)`、`ota_1=(0xE00000,0x600000)`、`content=(0x1400000,0xC00000,data/fat)`，最后结束于 `0x2000000`，分区不重叠，并确保 Assets 完整位于前 8MiB。

- [ ] **Step 2: 运行测试确认因旧布局失败**

Run: `python3 scripts/test_storage_foundation.py --partition-only`

Expected: FAIL，指出 `ota_0` 仍为 4MB 或缺少 `content`。

- [ ] **Step 3: 写入最小分区实现**

```csv
assets,     data,   spiffs,     0x200000,     6M,
ota_0,      app,    ota_0,      0x800000,     6M,
ota_1,      app,    ota_1,      0xE00000,     6M,
content,    data,   fat,        0x1400000,    12M,
```

- [ ] **Step 4: 运行测试确认通过**

Run: `python3 scripts/test_storage_foundation.py --partition-only`

Expected: `storage partition contract passed`

- [ ] **Step 5: 提交**

```bash
git add partitions/v2/32m.csv scripts/test_storage_foundation.py
git commit -m "feat: add balanced 32MB storage layout"
```

### Task 2: 无损导出并校验幽光 CBin

**Files:**
- Create: `scripts/export_ygsoul_cbin.py`
- Create: `main/boards/waveshare/esp32-s3-touch-amoled-1.75/assets/cbin/ygsoul_boot.cbin`
- Create: `main/boards/waveshare/esp32-s3-touch-amoled-1.75/assets/cbin/ygsoul_companion_nomouth.cbin`
- Create: `main/boards/waveshare/esp32-s3-touch-amoled-1.75/assets/cbin/ygsoul_mouth_1.cbin`
- Create: `main/boards/waveshare/esp32-s3-touch-amoled-1.75/assets/cbin/ygsoul_mouth_2.cbin`
- Create: `main/boards/waveshare/esp32-s3-touch-amoled-1.75/assets/cbin/ygsoul_mouth_3.cbin`
- Modify: `scripts/test_storage_foundation.py`

**Interfaces:**
- Consumes: 当前已真机验收的 C 数组和 LVGL 图像描述。
- Produces: LVGL v9 12 字节头 + 原始像素数据的 CBin；像素负载必须与现有数组逐字节一致。

- [ ] **Step 1: 扩展失败测试**

测试五个 CBin 均存在；头魔数为 `0x19`；启动图为 466×466 RGB565；底图为 280×280 RGB565A8；嘴型均为 38×29 RGB565A8；CBin 像素负载 SHA-256 与原数组一致。

- [ ] **Step 2: 运行确认缺少 CBin 而失败**

Run: `python3 scripts/test_storage_foundation.py --assets-only`

Expected: FAIL，指出 `ygsoul_boot.cbin` 不存在。

- [ ] **Step 3: 实现确定性导出器并生成资源**

导出器只解析指定数组中的 `0xNN` 字节和描述字段，使用 `struct.pack('<BBHHHHH', 0x19, cf, flags, w, h, stride, 0)` 写头，不重新采样、不调色、不压缩像素。

- [ ] **Step 4: 运行资源测试**

Run: `python3 scripts/export_ygsoul_cbin.py --check && python3 scripts/test_storage_foundation.py --assets-only`

Expected: 五个资源 `pixel payload identical`，资源契约通过。

- [ ] **Step 5: 提交**

```bash
git add scripts/export_ygsoul_cbin.py scripts/test_storage_foundation.py main/boards/waveshare/esp32-s3-touch-amoled-1.75/assets/cbin
git commit -m "feat: export lossless YGSoul CBin assets"
```

### Task 3: 幽光资源从应用迁入 Assets

**Files:**
- Modify: `main/CMakeLists.txt`
- Modify: `main/boards/waveshare/esp32-s3-touch-amoled-1.75/esp32-s3-touch-amoled-1.75.cc`
- Modify: `scripts/test_ygsoul_display_contract.sh`
- Modify: `scripts/test_storage_foundation.py`
- Delete: `main/boards/waveshare/esp32-s3-touch-amoled-1.75/ygsoul_boot_lvgl.h`
- Delete: `main/boards/waveshare/esp32-s3-touch-amoled-1.75/ygsoul_companion_nomouth.c`
- Delete: `main/boards/waveshare/esp32-s3-touch-amoled-1.75/ygsoul_mouth_1.c`
- Delete: `main/boards/waveshare/esp32-s3-touch-amoled-1.75/ygsoul_mouth_2.c`
- Delete: `main/boards/waveshare/esp32-s3-touch-amoled-1.75/ygsoul_mouth_3.c`
- Delete: `main/boards/waveshare/esp32-s3-touch-amoled-1.75/ygsoul_companion_lvgl.h`
- Delete: `main/boards/waveshare/esp32-s3-touch-amoled-1.75/ygsoul_speaking_mouth_lvgl.h`

**Interfaces:**
- Consumes: `Assets::GetAssetData(name, ptr, size)`、`LvglCBinImage(void*)`。
- Produces: 五个长期持有的映射图像对象；嘴型数组类型为 `const lv_img_dsc_t*`。

- [ ] **Step 1: 先修改契约测试为期望 Assets 加载**

断言板级 CMake 设置 `DEFAULT_ASSETS_EXTRA_FILES`；板级代码请求五个 `.cbin`；不再包含旧图像头；大 C 数组不进入 SOURCES；资源缺失时保持轻量界面且不解引用空图像。

- [ ] **Step 2: 运行确认旧实现失败**

Run: `bash scripts/test_ygsoul_display_contract.sh && python3 scripts/test_storage_foundation.py --source-only`

Expected: FAIL，指出仍使用 `LvglSourceImage` 或未设置额外资源目录。

- [ ] **Step 3: 实现最小 Assets 加载**

板级显示类增加 `LoadYGSoulAssets()`，逐一调用 `Assets::GetInstance().GetAssetData()` 并构造 `LvglCBinImage`；只有五项全部成功时创建幽光图层。失败时保留父类 Emoji/文本 UI，记录具体资源名。

- [ ] **Step 4: 运行契约回归**

Run: `bash scripts/test_ygsoul_display_contract.sh && python3 scripts/test_storage_foundation.py --source-only`

Expected: 两项 PASS。

- [ ] **Step 5: 提交**

```bash
git add -A main/CMakeLists.txt main/boards/waveshare/esp32-s3-touch-amoled-1.75 scripts
git commit -m "feat: load YGSoul images from assets partition"
```

### Task 4: ContentStorage 配额策略与 FATFS 挂载

**Files:**
- Create: `main/storage/content_storage.h`
- Create: `main/storage/content_storage.cc`
- Create: `scripts/test_content_storage_policy.cpp`
- Create: `scripts/test_content_storage_policy.sh`
- Modify: `main/CMakeLists.txt`
- Modify: `main/main.cc`
- Modify: `scripts/test_storage_foundation.py`

**Interfaces:**
- Produces: `ContentStorage::GetInstance()`、`Initialize()`、`GetStats()`、`CanReserve(ContentCategory,size_t)`、`GetPath(ContentCategory,const std::string&)`、`GetHealth()`。
- Health: `Ready`、`Unformatted`、`Corrupt`、`Unavailable`。

- [ ] **Step 1: 编写宿主机失败测试**

覆盖安全文件名、拒绝 `..`/斜杠、8MiB 音乐配额、2MiB 游戏配额、0.5MiB 小文件配额和 1.5MiB 总安全余量。

- [ ] **Step 2: 运行确认接口不存在而失败**

Run: `bash scripts/test_content_storage_policy.sh`

Expected: 编译失败，缺少 `main/storage/content_storage.h`。

- [ ] **Step 3: 实现纯策略和设备挂载**

使用 `esp_vfs_fat_spiflash_mount_rw_wl("/content", "content", &mount_config, &wl_handle_)`。先以 `format_if_mount_failed=false` 挂载；仅当分区首个扇区全部为 `0xFF` 时允许第二次以 `true` 初始化。已有非空数据挂载失败时返回 `Corrupt`，绝不自动格式化。

- [ ] **Step 4: 接入启动并运行测试**

在 NVS 成功后、Application 初始化前调用 `ContentStorage::GetInstance().Initialize()`；失败只记录日志，不中止基本功能。

Run: `bash scripts/test_content_storage_policy.sh && python3 scripts/test_storage_foundation.py --source-only`

Expected: PASS。

- [ ] **Step 5: 提交**

```bash
git add main/storage main/CMakeLists.txt main/main.cc scripts
git commit -m "feat: add quota-aware content storage"
```

### Task 5: 构建、分区解析与镜像体积门禁

**Files:**
- Create: `scripts/verify_storage_build.sh`
- Modify: `scripts/test_storage_foundation.py`

**Interfaces:**
- Consumes: `build/xiaozhi.bin`、`build/generated_assets.bin`、`build/partition_table/partition-table.bin`。
- Produces: 应用/Assets 字节数、剩余空间、分区解析报告和 SHA-256 清单。

- [ ] **Step 1: 编写镜像门禁测试并确认构建目录缺失时失败**

Run: `bash scripts/verify_storage_build.sh build`

Expected: FAIL，明确缺少构建产物。

- [ ] **Step 2: 配置并完整构建目标板**

Run: `idf.py -B build set-target esp32s3 && cp sdkconfig.175c sdkconfig && idf.py -B build build`

Expected: `Project build complete`。

- [ ] **Step 3: 运行所有自动化门禁**

Run: `python3 scripts/test_storage_foundation.py && bash scripts/test_content_storage_policy.sh && bash scripts/test_ygsoul_display_contract.sh && bash scripts/verify_storage_build.sh build && git diff --check`

Expected: 全部 PASS；应用和 Assets 各小于 6MiB；解析后的分区结束于 32MB。

- [ ] **Step 4: 提交**

```bash
git add scripts
git commit -m "test: add storage image verification gates"
```

### Task 6: 烧录前备份与安全迁移

**Files:**
- Create: `scripts/flash_32m_storage_migration.sh`

**Interfaces:**
- Consumes: 串口、已通过 Task 5 的构建目录。
- Produces: 仓库外备份目录、SHA-256、设备信息、明确地址的烧录记录。

- [ ] **Step 1: 添加脚本静态安全测试**

`scripts/test_storage_foundation.py --flash-script-only` 必须拒绝脚本包含 `erase_flash`、`write_efuse`、`burn_efuse` 或下载模式禁用命令，并要求备份尺寸/哈希成功后才能进入写入。

- [ ] **Step 2: 运行确认脚本缺失而失败**

Run: `python3 scripts/test_storage_foundation.py --flash-script-only`

Expected: FAIL，指出迁移脚本不存在。

- [ ] **Step 3: 实现备份优先的迁移脚本**

脚本顺序固定为：读取芯片信息 → 完整读取 32MB → 单独读取四个关键分区 → 校验尺寸和 SHA-256 → 写入并校验未来 ota_0 → 写入并校验未来 ota_1 → 写入并校验 partition table → 写入并校验 Assets → 写入并校验 Content → 清理 otadata 到 ota_0 初始状态 → 全量定址复核 → 复位。任何一步失败立即停止。

- [ ] **Step 4: 静态安全验收和提交**

Run: `python3 scripts/test_storage_foundation.py --flash-script-only && bash -n scripts/flash_32m_storage_migration.sh`

Expected: PASS。

```bash
git add scripts/flash_32m_storage_migration.sh scripts/test_storage_foundation.py
git commit -m "feat: add recoverable 32MB migration tool"
```

### Task 7: 真机烧录与回归

**Files:**
- No repository file changes required.

**Interfaces:**
- Consumes: `/dev/cu.usbmodem101`、验证通过的同一构建产物。
- Produces: 备份目录、烧录日志、启动日志、真机人工验收结果。

- [ ] **Step 1: 确认串口唯一且设备信息为 32MB ESP32-S3**

Run: `python -m esptool --chip esp32s3 --port /dev/cu.usbmodem101 flash_id`

Expected: 识别 ESP32-S3 和 32MB Flash；不符合则停止。

- [ ] **Step 2: 执行安全迁移脚本**

Run: `scripts/flash_32m_storage_migration.sh /dev/cu.usbmodem101 build`

Expected: 备份、哈希、定址烧录全部成功，无整片擦除。

- [ ] **Step 3: 串口启动验证**

确认 Assets 校验成功、五个 CBin 加载成功、Content 挂载为 Ready、分区容量正确、无复位循环。

- [ ] **Step 4: 真机功能验证**

验证启动形象、跑马灯白色、对话文字深紫色、默认闭嘴、说话三嘴型、联网、音频和连续三次重启；验证 Content 测试文件重启后仍存在并可删除。

- [ ] **Step 5: 用户人工验收后再合并**

用户确认通过前不合并主分支、不推送 GitHub。通过后按完成分支规范合并、主工作区回归、推送并清理工作树。
