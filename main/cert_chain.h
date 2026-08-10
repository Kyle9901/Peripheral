#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 获取证书链数据（二进制格式，见协议 4.3 节）。
 * 格式：uint16 count + for each: uint32 len + DER bytes
 */
const uint8_t *cert_chain_get_data(size_t *len);

#ifdef __cplusplus
}
#endif