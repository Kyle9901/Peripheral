# InstaCare Peripheral — ESP32 设备端

基于 [InstaCare 设备接入协议](./docs/provisioning.md) 实现的 ESP32 Peripheral 设备端。

## 已实现功能

- [x] BLE 5 Extended Advertising（128-bit Service UUID + 20 字节 Service Data）
- [x] GATT Provisioning Service（4 个特征）
  - `DeviceInfo` (Read) — 设备信息 JSON
  - `CertificateChain` (Read) — 设备证书链（二进制）
  - `NetworkConfig` (Write) — 接收 Wi-Fi 配置
  - `Status` (Read/Notify) — 配网状态上报
- [x] 设备证书链 DER 编码
- [x] 状态机：`idle → config_received → ... → operational`
- [x] 长读写 / Prepare Write / Execute Write 支持
- [x] NVS 持久化 device_id / config_revision
- [x] Wi-Fi STA 连接（配置校验、NVS 恢复、错误状态、全抖动退避）
- [x] TCP 长连接（DNS/IPv4/IPv6、连接超时、keepalive、自动重连）
- [x] TLV 帧通信（会话、注册、心跳、Action 拒绝响应、遥测入口）
- [x] DHT11/DHT22 温湿度采集（可配置，默认 DHT11）
- [x] BH1750 光照采集
- [x] HC-SR501 人体红外采集
- [x] 传感器数据周期 TLV 上报

## 网络与 TLV 流程

`NetworkConfig` 会使用 cJSON 完整解析并校验，Wi-Fi 密码不会写入日志。
设备重启后会从 NVS 恢复最后接受的配置并自动重连。TCP 会话只有在收到
`session.ready` 后注册清单，并在收到 `action.registered` 后才进入
`operational`。

设备 Action 协议原文目前不在仓库内，因此 TLV tag 和临时载荷约定记录在
[docs/tlv-protocol.md](docs/tlv-protocol.md)。正式发布前应使用规范中的签名
`session.open` 载荷替换当前 challenge 回显载荷。

## 传感器接线

ESP32-S3 没有 GPIO22，因此 BH1750 的 SCL 默认使用 GPIO18。

| 传感器 | 传感器引脚 | ESP32-S3 | 供电 |
| --- | --- | --- | --- |
| DHT11（默认）或 DHT22 | DATA | GPIO4 | 3.3V |
| BH1750 | SDA | GPIO21 | 3.3V |
| BH1750 | SCL | GPIO18 | 3.3V |
| HC-SR501 | OUT | GPIO5 | 模块 VCC 接 5V，OUT 接 GPIO5 |

所有模块必须与 ESP32 共地。裸 DHT11/DHT22 的 DATA 与 3.3V 之间需要约 4.7k
上拉电阻；BH1750 裸板的 SDA/SCL 也需要 I2C 上拉，常见模块板已自带。
HC-SR501 上电后通常需要约 30–60 秒预热，预热期间输出可能不稳定。

DHT11 与 DHT22 接线相同，但启动时序和数据编码不同，不能直接共用同一种
解码方式。本项目默认选择 DHT11；如改用 DHT22，可在 `menuconfig` 的
`InstaCare 传感器配置 → DHT 温湿度传感器型号` 中切换。

默认每 5 秒采样一次。引脚、BH1750 地址和采样间隔可通过以下命令修改：

```bash
idf.py menuconfig
# InstaCare 传感器配置
```

设备进入 `operational` 后会通过 TLV `telemetry` 帧上报：温度、湿度、光照、
当前人体感应状态，以及本上报周期内是否曾检测到人体移动。离线期间继续采样，
但当前版本不缓存历史遥测。

## 构建

```bash
# 安装 ESP-IDF v5.0+
# 设置 Python 依赖
pip install cryptography

# 生成测试证书
python scripts/generate_certs.py --device-id <32位hex> --output main/cert_chain.c

# 编译烧录
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

## 配置

编辑 `main/dev_info.c` 中的设备信息，或通过 `idf.py menuconfig` 配置。
