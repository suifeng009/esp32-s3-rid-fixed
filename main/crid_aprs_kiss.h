/**
 * crid_aprs_kiss.h — APRS KISS 协议输出 (基于 BLE UART)
 */

#ifndef CRID_APRS_KISS_H
#define CRID_APRS_KISS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 APRS KISS BLE UART 服务器与任务
 */
void crid_aprs_ble_init(void);

#ifdef __cplusplus
}
#endif

#endif // CRID_APRS_KISS_H
