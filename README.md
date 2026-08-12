# InstaCare Matter 多传感器

ESP32-S3 原生 Matter over Wi-Fi 多传感器固件，使用官方 ESP-IDF 和
Espressif ESP-Matter 托管组件构建。设备可由 Home Assistant 的 Matter
集成直接配网，不再依赖 Central、私有 TCP 长连接或自定义 TLV 协议。

## 功能

- 温度、相对湿度和环境光照度采集
- 人体移动检测与 Matter 占用状态
- Matter BLE 配网和 Wi-Fi 运行
- 四个标准 Matter 传感器 Endpoint
- Home Assistant 可直接发现实体并订阅属性变化

## 文档

- [硬件型号、供电与 GPIO 接线](docs/hardware.md)
- [Matter Endpoint、属性与数据编码](docs/matter.md)

## 开发环境

- ESP-IDF 5.4 系列（本机路径为 `/home/alkaid/esp/esp-idf`）
- ESP-Matter 1.4.0，由 IDF Component Manager 根据
  `main/idf_component.yml` 下载

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

传感器型号、GPIO、总线地址和采样参数的配置说明见
[硬件与 GPIO 接线技术文档](docs/hardware.md)。Matter 属性及动态占用保持时间的
接口说明见 [Matter 接口说明](docs/matter.md)。
