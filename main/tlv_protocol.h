#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 线格式：标签（uint16 大端）、值长度（uint32 大端），随后是值字节。 */
#define TLV_HEADER_SIZE       6
#define TLV_MAX_VALUE_SIZE    8192

typedef enum {
    TLV_SESSION_CHALLENGE = 0x0001,
    TLV_SESSION_OPEN      = 0x0002,
    TLV_SESSION_READY     = 0x0003,
    TLV_PING              = 0x0004,
    TLV_PONG              = 0x0005,
    TLV_ACTION_REGISTER   = 0x0100,
    TLV_ACTION_REGISTERED = 0x0101,
    TLV_ACTION_INVOKE     = 0x0110,
    TLV_ACTION_RESULT     = 0x0111,
    TLV_TELEMETRY         = 0x0200,
    TLV_ERROR             = 0x7fff,
} tlv_tag_t;

typedef enum {
    TLV_SESSION_DISCONNECTED = -1,
    TLV_SESSION_PROTOCOL_ERROR = -2,
    TLV_SESSION_REJECTED = -3,
    TLV_SESSION_OK = 0,
} tlv_session_result_t;

/** 初始化协议发送锁；设备启动时调用一次。 */
int tlv_protocol_init(void);

/** 发送一个完整的 TLV 帧，线程安全。 */
int tlv_send_frame(int socket_fd, uint16_t tag,
                   const void *value, uint32_t value_length);

/**
 * 运行会话建立、能力注册和心跳，直到连接断开。
 * 只有收到对端响应后，状态才会进入 registering 和 operational。
 */
tlv_session_result_t tlv_run_session(int socket_fd);

/** 在已建立的连接上发送一个 UTF-8 JSON 遥测值。 */
int tlv_send_telemetry_json(int socket_fd, const char *json);

#ifdef __cplusplus
}
#endif
