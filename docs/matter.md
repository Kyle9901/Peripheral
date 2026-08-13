# Matter 接口说明

硬件型号、供电和 GPIO 接线请参阅[硬件与 GPIO 接线技术文档](hardware.md)。

## 通信结构

设备使用 Matter over Wi-Fi。蓝牙只用于首次 Commissioning，Home Assistant 下发
Wi-Fi 和 Fabric 凭据后，日常通信切换到局域网 IPv6。

```text
ESP32-S3  <-- Matter / IPv6 / Wi-Fi -->  Matter Server
                                             ^
                                             | WebSocket
                                             v
                                      Home Assistant Core
```

固件不会访问 Central，也不会连接 `ws://localhost:5580/ws`。

## 自动识别与 Endpoint

固件只把已经确认存在的 Endpoint 注册到 Matter。Endpoint ID 从 1 开始按实际探测
顺序连续分配，因此只接 PIR 时占用通常为 Endpoint 1，接齐全部传感器时对应下表：

| Endpoint | Matter Device Type | Cluster | 输出属性 | 识别方式 |
| --- | --- | --- | --- | --- |
| 1 | Temperature Sensor | Temperature Measurement `0x0402` | MeasuredValue | DHT 有效应答 |
| 2 | Humidity Sensor | Relative Humidity Measurement `0x0405` | MeasuredValue | 与温度共用 DHT |
| 3 | Light Sensor | Illuminance Measurement `0x0400` | MeasuredValue | BH1750 I²C 应答 |
| 4 | Occupancy Sensor | Occupancy Sensing `0x0406` | Occupancy | PIR 首次有效高电平 |

DHT 和 BH1750 在启动时探测，未识别到的模块每 30 秒重试。一次读取失败不会删除
已经发布的 Endpoint，避免接触不良导致 Home Assistant 实体反复变化。

HC-SR501 只有高低电平，低电平无法说明模块是否接入。因此 PIR 默认不存在；GPIO
连续三次（约 750 ms）为高后确认存在，将结果保存到 NVS，并在运行中启用占用
Endpoint。擦除设备 NVS 后会重新进入未识别状态。

运行中增加 Endpoint 后，固件会触发 Matter Descriptor `PartsList` 变化。部分
Home Assistant/Matter Server 版本会立即生成实体；如果界面未刷新，重新加载 Matter
集成。已经存在的传感器实体，其数值和占用状态均会实时更新。

数据编码：

- 温度：摄氏度乘以 100，例如 `23.56 °C -> 2356`。
- 湿度：百分比乘以 100，例如 `48.2% -> 4820`。
- 光照：Matter Illuminance Measurement 对数格式，0 lux 编码为 0。
- 占用：bit 0，`1` 表示有人，`0` 表示无人。

## PIR 占用语义

HC-SR501 检测红外变化，即人体移动，不能持续判断完全静止的人。固件每 250 ms
检查一次 PIR，状态变化后立即上报，不等待默认 5 秒的环境数据采样周期。每次高
电平会把 `Occupancy` 的有人状态延长，默认保持 30 秒。

保持时间通过 Occupancy Sensing Cluster `0x0406` 的标准可写属性
`PIROccupiedToUnoccupiedDelay`（`0x0010`）设置，单位为秒，范围 1–600 秒。
ESP-Matter 持久化控制器写入的值；Kconfig 的
`CONFIG_INSTACARE_OCCUPANCY_HOLD_MS` 只提供首次默认值。

## 串口验证

只有 PIR 的板子首次启动时显示：

```text
自动探测结果：温湿度=无 光照=无 人体红外=等待首次高电平
```

在 PIR 前移动后显示：

```text
GPIO5 连续输出高电平，已识别 HC-SR501
Matter 能力已动态启用：人体占用
Matter 占用实时更新：有人
```

开发配置会打印 Matter 二维码和手动配对码。正式产品不可继续使用仓库中的测试
凭据配置。
