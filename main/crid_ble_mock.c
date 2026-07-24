#include "crid_ble_mock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include "crid_web.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"

static const char *TAG = "BLE_MOCK";

static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t loc_chr_handle = 0;
static uint8_t own_addr_type;

// Custom Service UUID: 0xFF00
static const ble_uuid16_t gatt_svr_svc_uuid = BLE_UUID16_INIT(0xFF00);

// Location Characteristic UUID: 0xFF01
static const ble_uuid16_t gatt_svr_chr_loc_uuid = BLE_UUID16_INIT(0xFF01);

#pragma pack(push, 1)
typedef struct {
    float lat;
    float lon;
    float alt;
    float speed;
} mock_loc_t;
#pragma pack(pop)

static mock_loc_t current_loc = {
    .lat = 24.9433f, // Quanzhou Qingyuan Mountain
    .lon = 118.6015f,
    .alt = 100.0f,
    .speed = 5.0f
};

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr_handle == loc_chr_handle) {
            int rc = os_mbuf_append(ctxt->om, &current_loc, sizeof(current_loc));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &gatt_svr_chr_loc_uuid.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &loc_chr_handle,
            },
            { 0 } // terminator
        }
    },
    { 0 } // terminator
};

static void ble_mock_task(void *pv) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            // Read from WEB simulator if running, else default movement
            if (crid_web_is_sim_running()) {
                sim_control_t *sim = crid_web_get_sim();
                if (sim && sim->count > 0) {
                    current_loc.lat = sim->drones[0].latitude;
                    current_loc.lon = sim->drones[0].longitude;
                    current_loc.alt = sim->drones[0].altitude_msl;
                    current_loc.speed = sim->drones[0].speed_horizontal;
                }
            } else {
                current_loc.lat += 0.0001f;
                current_loc.lon += 0.0001f;
            }

            struct os_mbuf *om = ble_hs_mbuf_from_flat(&current_loc, sizeof(current_loc));
            if (om) {
                int rc = ble_gatts_notify_custom(conn_handle, loc_chr_handle, om);
                if (rc != 0) {
                    ESP_LOGW(TAG, "Failed to notify: %d", rc);
                }
            }
        }
    }
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "Connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
        } else {
            ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, NULL, ble_gap_event_cb, NULL);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnect; reason=%d", event->disconnect.reason);
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, NULL, ble_gap_event_cb, NULL);
        break;
        
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU update to %d", event->mtu.value);
        break;
    }
    return 0;
}

static void ble_app_on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    assert(rc == 0);

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    // Set advertising data
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    const char *name = "RID_MOCK";
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event_cb, NULL);
    ESP_LOGI(TAG, "BLE Mock Advertising started...");
}

static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void crid_ble_mock_init(void) {
    ESP_LOGI(TAG, "Initializing BLE Mock Server");
    
    nimble_port_init();
    
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    
    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    assert(rc == 0);
    
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    assert(rc == 0);

    nimble_port_freertos_init(ble_host_task);
    
    xTaskCreate(ble_mock_task, "ble_mock_timer", 4096, NULL, 5, NULL);
}
