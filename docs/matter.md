# Matter 接口说明

硬件型号、供电和 GPIO 接线请参阅[硬件与 GPIO 接线技术文档](hardware.md)。

## 通信结构

设备使用 Matter over Wi-Fi。蓝牙仅用于首次 Commissioning，Home Assistant
下发 Wi-Fi 和 Fabric 凭据后，日常通信切换到局域网 IPv6。

```text
ESP32-S3  <-- Matter / IPv6 / Wi-Fi -->  Matter Server
                                             ^
                                             | WebSocket
                                             v
                                      Home Assistant Core
```

因此，本固件不会访问 Central，也不会向 `ws://localhost:5580/ws` 建立连接。

## Endpoint 与属性

Endpoint ID 在启动时动态分配，并输出到串口日志。当前创建顺序如下：

| 顺序 | Matter Device Type | Cluster | 输出属性 | 来源 |
| --- | --- | --- | --- | --- |
| 1 | Temperature Sensor | Temperature Measurement `0x0402` | MeasuredValue | DHT11/DHT22 温度 |
| 2 | Humidity Sensor | Relative Humidity Measurement `0x0405` | MeasuredValue | DHT11/DHT22 湿度 |
| 3 | Light Sensor | Illuminance Measurement `0x0400` | MeasuredValue | BH1750 光照 |
| 4 | Occupancy Sensor | Occupancy Sensing `0x0406` | Occupancy | HC-SR501 PIR |

设备输出的是标准 Matter 属性，不再输出自定义 JSON 或 TLV 帧：

- 温度：摄氏度乘以 100，例如 `23.56 °C -> 2356`。
- 湿度：百分比乘以 100，例如 `48.2% -> 4820`。
- 光照：按 Matter Illuminance Measurement 的对数格式编码；0 lux 为 0。
- 占用：位图 bit 0，`1` 表示有人，`0` 表示无人。

Home Assistant 会把这些属性转换为可读的传感器实体，通常无需手动解析原始值。

## PIR 占用语义

HC-SR501 检测的是红外变化，即“移动”，不能可靠判断静止的人是否仍在房间。
固件每 250 ms 检查一次 PIR；检测到高电平后，将 Matter `Occupancy` 保持为
有人，默认保持 30 秒。

保持时间通过 Endpoint 4、Occupancy Sensing Cluster `0x0406` 的标准可写属性
`PIROccupiedToUnoccupiedDelay`（`0x0010`）动态配置，单位为秒，允许范围
1–600 秒。ESP-Matter 会持久化写入值，设备重启后仍然生效；Kconfig 中的
`CONFIG_INSTACARE_OCCUPANCY_HOLD_MS` 只提供第一次启动的默认值。

## 串口验证

正常启动后可看到类似日志：

```text
Matter endpoints：温度=1 湿度=2 光照=3 占用=4
传感器已启动：DHT11=GPIO4, PIR=GPIO5, BH1750 SDA=21 SCL=18 地址=0x23
Matter 属性已更新：温度=有效 湿度=有效 光照=有效 占用=否
Matter 配网完成
```

开发配置会在启动日志中打印 Matter 二维码和手动配对码。正式产品不可继续使用
仓库中的测试凭据配置。
