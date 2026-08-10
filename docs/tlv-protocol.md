# InstaCare Peripheral TLV 线协议

> 临时协议：配网文档引用了 `device-capability.md`，但实现本模块时仓库中
> 没有该文件。如果正式 Action 协议使用不同的标签或载荷，应保留现有传输
> 生命周期，并替换本协议定义。

## 帧格式

每条 TCP 消息均使用网络字节序编码。接收方必须缓冲 TCP 字节流，不能假定
一次 `recv()` 调用恰好返回一个完整帧。

| 偏移 | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 2 | 标签 tag（`uint16`） |
| 2 | 4 | 值长度（`uint32`，最大 8192） |
| 6 | N | 值 |

## 标签与会话顺序

| 标签 | 名称 | 方向 | 值 |
| ---: | --- | --- | --- |
| `0x0001` | `session.challenge` | Central → Peripheral | 16–256 字节不透明数据 |
| `0x0002` | `session.open` | Peripheral → Central | 设备 ID（16）、challenge 长度（u16）、challenge |
| `0x0003` | `session.ready` | Central → Peripheral | 由实现定义或为空 |
| `0x0004` | `ping` | 双向 | 不透明关联数据 |
| `0x0005` | `pong` | 双向 | 复制收到的关联数据 |
| `0x0100` | `action.register` | Peripheral → Central | UTF-8 JSON 能力清单 |
| `0x0101` | `action.registered` | Central → Peripheral | 由实现定义或为空 |
| `0x0110` | `action.invoke` | Central → Peripheral | UTF-8 JSON 调用请求 |
| `0x0111` | `action.result` | Peripheral → Central | UTF-8 JSON 调用结果 |
| `0x0200` | `telemetry` | Peripheral → Central | UTF-8 JSON 遥测记录 |
| `0x7fff` | `error` | 双向 | UTF-8 JSON 错误信息 |

传感器遥测值示例：

```json
{
  "spec": "instacare.device/1.0",
  "sequence": 0,
  "uptime_ms": 12345,
  "dht_model": "dht11",
  "values": {
    "temperature_c": 25.3,
    "humidity_percent": 58.1,
    "illuminance_lux": 126.7,
    "motion": false,
    "motion_detected": true
  },
  "quality": {
    "dht": "ok",
    "bh1750": "ok"
  }
}
```

读取失败的数值使用 JSON `null`，同时对应的 `quality` 为 `read_error`。

Peripheral 只有在收到 `session.ready` 后才报告 `registering`，并且只有在
收到 `action.registered` 后才报告 `operational`。设备每 20 秒发送一次协议
心跳；如果连续 60 秒没有收到对端帧，则断开并重新连接。

临时 `session.open` 会原样返回 challenge，因此尚未提供完整能力协议要求的
ECDSA 身份认证。生产发布前必须将其替换为正式的签名信封；TCP、重试、拆帧、
能力注册、心跳和遥测代码均与这项替换隔离。
