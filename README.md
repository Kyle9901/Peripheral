# InstaCare Matter 传感器设备

ESP32-S3 原生 Matter over Wi-Fi 传感器固件，使用官方 ESP-IDF 和
Espressif ESP-Matter 托管组件构建。设备可由 Home Assistant 的 Matter
集成直接配网，并通过标准 Matter 属性上报传感器数据。

## 功能

- 温度、相对湿度和环境光照度采集
- 人体移动检测与 Matter 占用状态
- Matter BLE 配网和 Wi-Fi 运行
- 单一固件自动识别当前接入的传感器并按需发布 Matter Endpoint
- PIR 默认不发布，GPIO 首次输出有效高电平后自动登记并实时上报
- Home Assistant 可直接发现实体并订阅属性变化

## 文档

- [硬件型号、供电与 GPIO 接线](docs/hardware.md)
- [Matter Endpoint、属性与数据编码](docs/matter.md)

## 开发环境

- ESP-IDF 5.4 系列（本机路径为 `/home/alkaid/esp/esp-idf`）
- ESP-Matter 1.4.0，由 IDF Component Manager 根据
  `main/idf_component.yml` 下载

本项目不再支持 PlatformIO；请统一使用官方 `idf.py`。

## 传感器识别

固件启动时读取 DHT11/DHT22，并探测 BH1750 的 `0x23`、`0x5C` 地址，只为成功
应答的传感器发布 Matter Endpoint。未接光照模块时不会在 Home Assistant 中创建
照度实体。

HC-SR501 无法通过协议区分“没有接入”和“暂时没有移动”，所以初始状态为未识别。
GPIO 连续三次采到高电平后登记为已接入并保存到 NVS，随后创建人体占用 Endpoint。
不同 ESP32 是独立 Matter 节点，各自保存 Wi-Fi、Fabric 和传感器登记状态。

## 编译与烧录

```bash
cd /home/alkaid/Peripheral
source /home/alkaid/esp/esp-idf/export.sh
idf.py build
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
