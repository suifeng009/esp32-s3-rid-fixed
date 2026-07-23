#include "crid_aprs_kiss.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "crid_tracker.h"
#include "crid_rx_types.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <assert.h>

#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"

static const char *TAG = "APRS_BLE";

#define FEND  0xC0
#define FESC  0xDB
#define TFEND 0xDC
#define TFESC 0xDD

static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t nus_tx_handle = 0;
static uint8_t own_addr_type;

// NUS UUIDs (128-bit)
// 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t gatt_svr_svc_nus_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);

// RX 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t gatt_svr_chr_nus_rx_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);

// TX 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t gatt_svr_chr_nus_tx_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static int gatt_svr_chr_access_nus(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // Ignore incoming data for APRS output
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_nus_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &gatt_svr_chr_nus_tx_uuid.u,
                .access_cb = gatt_svr_chr_access_nus,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &nus_tx_handle,
            },
            {
                .uuid = &gatt_svr_chr_nus_rx_uuid.u,
                .access_cb = gatt_svr_chr_access_nus,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                0, // Null terminator
            }
        },
    },
    {
        0, // Null terminator
    },
};

static void ble_app_advertise(void);

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "BLE GAP Connect %s", event->connect.status == 0 ? "OK" : "Failed");
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
        } else {
            ble_app_advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE GAP Disconnect, reason: %d", event->disconnect.reason);
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_app_advertise();
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU update: %d", event->mtu.value);
        break;
    }
    return 0;
}

static void ble_app_advertise(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)"ESP32_RID_APRS";
    fields.name_len = strlen("ESP32_RID_APRS");
    fields.name_is_complete = 1;
    fields.uuids128 = (ble_uuid128_t[]) { gatt_svr_svc_nus_uuid };
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    
    ble_gap_adv_set_fields(&fields);

    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
}

static void ble_app_on_sync(void) {
    ble_hs_id_infer_auto(0, &own_addr_type);
    ble_app_advertise();
}

static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static size_t build_kiss_frame(const uint8_t *data, size_t len, uint8_t *out_buf, size_t out_max) {
    size_t out_len = 0;
    if (out_max < 2) return 0;
    
    out_buf[out_len++] = FEND;
    out_buf[out_len++] = 0x00; 
    
    for (size_t i = 0; i < len; i++) {
        if (out_len + 2 >= out_max) break;
        if (data[i] == FEND) {
            out_buf[out_len++] = FESC;
            out_buf[out_len++] = TFEND;
        } else if (data[i] == FESC) {
            out_buf[out_len++] = FESC;
            out_buf[out_len++] = TFESC;
        } else {
            out_buf[out_len++] = data[i];
        }
    }
    
    if (out_len < out_max) {
        out_buf[out_len++] = FEND;
    }
    return out_len;
}

static void format_lat_lon(double lat, double lon, char *lat_str, char *lon_str) {
    char lat_dir = (lat >= 0) ? 'N' : 'S';
    char lon_dir = (lon >= 0) ? 'E' : 'W';
    lat = fabs(lat);
    lon = fabs(lon);
    int lat_deg = (int)lat;
    double lat_min = (lat - lat_deg) * 60.0;
    int lon_deg = (int)lon;
    double lon_min = (lon - lon_deg) * 60.0;
    snprintf(lat_str, 10, "%02d%05.2f%c", lat_deg, lat_min, lat_dir);
    snprintf(lon_str, 11, "%03d%05.2f%c", lon_deg, lon_min, lon_dir);
}

// 分包 notify，确保不超过 MTU
static void ble_notify_data(const uint8_t *data, size_t len) {
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    
    // Default BLE MTU is 23, usable payload is 20 unless negotiated larger.
    uint16_t mtu = 20; 

    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > mtu) chunk = mtu;

        struct os_mbuf *om = os_msys_get_pkthdr(chunk, 0);
        if (om) {
            os_mbuf_append(om, data + sent, chunk);
            ble_gatts_notify_custom(conn_handle, nus_tx_handle, om);
        }
        sent += chunk;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void aprs_ble_task(void *pvParameters) {
    char aprs_msg[128];
    uint8_t kiss_buf[256];
    char lat_str[10];
    char lon_str[11];

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        if (conn_handle == BLE_HS_CONN_HANDLE_NONE) continue;

        SemaphoreHandle_t mutex = crid_tracker_get_mutex();
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            uav_track_t *table = crid_tracker_get_table();
            for (int i = 0; i < MAX_TRACKED_UAVS; i++) {
                if (table[i].active && table[i].location.valid) {
                    char uav_id[16];
                    snprintf(uav_id, sizeof(uav_id), "U%02X%02X%02X", 
                             table[i].mac[3], table[i].mac[4], table[i].mac[5]);
                    
                    format_lat_lon(table[i].location.latitude, table[i].location.longitude, lat_str, lon_str);
                    
                    int course = (int)(table[i].location.direction);
                    if (course < 0 || course > 360) course = 0;
                    if (course == 0) course = 360; 
                    
                    int speed_knots = (int)(table[i].location.speed_horizontal * 1.94384f);
                    int alt_feet = (int)(table[i].location.altitude_geo * 3.28084f);
                    if (alt_feet < 0) alt_feet = 0;
                    
                    snprintf(aprs_msg, sizeof(aprs_msg),
                             "%s>APRS,TCPIP*:!%s/%s^%03d/%03d/A=%06d",
                             uav_id, lat_str, lon_str, course, speed_knots, alt_feet);
                             
                    size_t kiss_len = build_kiss_frame((uint8_t *)aprs_msg, strlen(aprs_msg), kiss_buf, sizeof(kiss_buf));
                    
                    ble_notify_data(kiss_buf, kiss_len);
                }
            }
            xSemaphoreGive(mutex);
        }
    }
}

void crid_aprs_ble_init(void) {
    nimble_port_init();

    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    assert(rc == 0);

    ble_hs_cfg.sync_cb = ble_app_on_sync;

    nimble_port_freertos_init(ble_host_task);
    
    xTaskCreate(aprs_ble_task, "aprs_ble", 4096, NULL, 3, NULL);
}
