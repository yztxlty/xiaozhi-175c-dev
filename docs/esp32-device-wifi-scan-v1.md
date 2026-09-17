# YGSoul ESP32-S3 设备端 Wi-Fi 扫描协议 V1.0

日期：2026-09-17。状态：开发分支，未烧录验收，不得作为已发布固件。

## 范围

保留 YGSoul BLE 1910 / 2B11 / 2B10 与 0x01–0x04 配网协议，只增加设备侧 2.4 GHz 扫描。手机只选择设备列表，不调用手机 Wi-Fi 扫描。A100/T5 未实现此扩展；不改其命令号。不修改语音、图库、手表、云端绑定成功门禁。

## 能力与请求

设备信息 0x02 响应增加 `wifiScan: 1`。客户端只有在 ESP32S3 且该字段严格等于 1 时才发扫描请求。旧固件提示升级，不自动退回手机扫描。

请求 0x05、响应 0x06，沿用 AA/CMD/LEN/JSON/CRC16 帧，载荷不超过 238 字节。请求格式：`{"id":"12345678","scan":"abcdef01","op":"start","index":-1}`。id 和 scan 均为 8 位小写十六进制关联号，不是密钥。op 为 start/get/cancel；get 的 index=-1 查询状态，0..31 读取一条结果。每次事务用新的 id，整轮扫描共用 scan。

开始响应及轮询状态：`{"id":"12345678","scan":"abcdef01","state":"SCANNING","total":0,"truncated":false}`。扫描完成是 DONE；只有 DONE 且 total=0 是空结果。ERROR 必须有 error，不能转换成空数组。错误包括 INVALID_REQUEST、BUSY、STALE_SCAN、INDEX_OUT_OF_RANGE、WIFI_DRIVER_ERROR、NO_MEMORY。取消为 CANCELLED。

结果页：`{"id":"12345678","scan":"abcdef01","state":"DONE","total":1,"truncated":false,"index":0,"ap":{"s":"486f6d65","b":"001122334455","r":-48,"c":6,"a":3}}`。

ap.s 为 SSID 原始字节的十六进制，不 trim、不根据名字猜频段；ap.b 为 BSSID，ap.r 为设备 Wi-Fi RSSI dBm，ap.c 为设备扫描出的 2.4 GHz 信道，ap.a 为 ESP-IDF wifi_auth_mode_t。协议 v1 明确仅扫描 2.4 GHz，信道 1..13 对应 2407+5*c MHz，14 为 2484 MHz。无效 UTF-8 SSID 可显示十六进制并禁止错误编码下发，不能替换字符后配网。

## 调度与资源

扫描在专用任务执行，不阻塞 BLE 回调。按设备配置的国家信道逐信道扫描，每信道最大 120 ms。最多保留 32 条，超出返回 truncated=true，按 RSSI 排序，BSSID 去重；每页一条避免最长 SSID 超载。客户端串行拉取固定快照，不并行扫描。

显式配网时，未连接站点通过已有 WifiManager.StopStation 暂停竞争的自动重连；已建立 Wi-Fi 不主动断开。仅扫描任务自己启动的 radio 在结束后停止。扫描不修改已保存 SSID、密码或配网回执；退出配网继续使用已有恢复逻辑。运行中拒绝写入 Wi-Fi，清理结束后才发布 DONE。取消、断线、换连接用代次隔离迟到结果。

Notify 帧级串行，保留 20 字节切片，检查每片发送返回码；失败断开连接而不是继续追加残缺帧。客户端必须匹配设备、连接代次、命令、id 与 scan，CRC 错误不能当空结果。

## 验证与发布门禁

主机协议/SDK替身测试不是真机射频测试。需执行新增 tests/test_device_wifi_scan.py 与已有 tests/test_pairing_receipt.py、完整项目回归，再使用项目原有 ESP-IDF >=5.5.2 与已验证 components override 构建；不得用上游新库覆盖本地补丁。

当前云端基线 main/idf_component.yml 引用的 components/78__esp-wifi-connect 等本地组件未在已读取远端树中提供，且本会话没有完整 IDF 工具链，因此未宣称整机编译通过。合并前必须补齐相同版本依赖并核对镜像分区余量。

真机门禁：手机 Wi-Fi 关闭但 BLE/移动数据正常；iOS/Android 各自扫描、刷新、选网、密码错误重试、断线重连、32 条上限、默认 MTU、中文及含5G名称、旧回执不能成功、退出配网及断电重启恢复旧网络。先验证新固件与对应小程序分支，再发布；不得直接推送 OTA 或把单测结果当真机验收。
