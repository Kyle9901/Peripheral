# InstaCare Matter 多传感器

ESP32-S3 原生 Matter over Wi-Fi 多传感器固件，使用官方 ESP-IDF 和
Espressif ESP-Matter 托管组件构建。设备可由 Home Assistant 的 Matter
集成直接配网，不再依赖 Central、私有 TCP 长连接或自定义 TLV 协议。

## 功能

- DHT11（默认）或 DHT22：温度、相对湿度
- BH1750：环境光照度
- HC-SR501：人体移动与占用状态
- Matter BLE 配网和 Wi-Fi 运行
- 四个标准 Matter 传感器 Endpoint
- Home Assistant 可直接发现实体并订阅属性变化

Matter Endpoint 和数值编码见 [Matter 接口说明](docs/matter.md)。

## 接线

| 传感器 | 传感器引脚 | ESP32-S3 | 供电 |
| --- | --- | --- | --- |
| DHT11/DHT22 | DATA | GPIO4 | 3.3V |
| BH1750 | SDA | GPIO21 | 3.3V |
| BH1750 | SCL | GPIO18 | 3.3V |
| HC-SR501 | OUT | GPIO5 | VCC 接 5V，OUT 接 GPIO5 |

所有模块必须与 ESP32 共地。裸 DHT 数据线需要约 4.7 kΩ 上拉电阻；常见
BH1750 模块板通常自带 I2C 上拉。HC-SR501 上电后需预热约 30–60 秒。

## 开发环境

- ESP-IDF 5.4 系列（本机路径为 `/home/alkaid/esp/esp-idf`）
- ESP-Matter 1.4.0，由 IDF Component Manager 根据
  `main/idf_component.yml` 下载
- 目标芯片：ESP32-S3 N8R2，8 MiB Flash、2 MiB OPI PSRAM

本项目不再支持 PlatformIO；请统一使用官方 `idf.py`。

## 编译与烧录

```bash
cd /home/alkaid/Peripheral
source /home/alkaid/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

从旧版 TLV 固件首次切换到 Matter 固件时，建议先清除旧分区和 NVS：

```bash
idf.py -p /dev/ttyACM0 erase-flash
idf.py -p /dev/ttyACM0 flash monitor
```

## Home Assistant 配网

1. 确认 Home Assistant 已安装并运行 Matter Server 集成。
2. ESP32 启动后，从串口日志读取 Matter 二维码或手动配对码。
3. 在 Home Assistant 手机 App 中选择“添加 Matter 设备”并扫码。
4. 配网完成后，设备通过局域网 IPv6/Matter 与 Matter Server 通信。

`ws://localhost:5580/ws` 是 Home Assistant Core 与 Matter Server 之间的
WebSocket 地址，不是 ESP32 的通信目标，ESP32 无需连接该地址。

当前启用了 ESP-Matter 测试配网数据和测试 DAC，仅用于开发验证。正式产品必须
烧录每台设备唯一的 DAC/PAI/CD、配对码和合法的 Vendor ID/Product ID。

## 修改传感器配置

```bash
idf.py menuconfig
# InstaCare Matter 传感器配置
```

可修改 DHT 型号、GPIO、BH1750 地址、采样间隔，以及 PIR 触发后的 Matter
占用默认保持时间。默认每 5 秒更新属性，占用状态默认保持 30 秒。配网后，
Matter 控制器可写 Endpoint 4、Cluster `0x0406`、Attribute `0x0010`
（`PIROccupiedToUnoccupiedDelay`，单位为秒）在线调整为 1–600 秒；新值会持久化。
