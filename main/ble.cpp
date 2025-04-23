#include "ble.h"
#include "main_queue.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include <host/ble_gap.h>
#include <host/util/util.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <host/ble_gatt.h>
#include <services/gatt/ble_svc_gatt.h>
#include <time.h>

#define BLE_GAP_APPEARANCE_GENERIC_TAG 0x0200
#define BLE_GAP_URI_PREFIX_HTTPS 0x17
#define BLE_GAP_LE_ROLE_PERIPHERAL 0x00

/* Private variables */
static const char* TAG = "ble";
static uint8_t own_addr_type;
static uint8_t addr_val[6] = {0};
static uint8_t esp_uri[] = {BLE_GAP_URI_PREFIX_HTTPS, '/', '/', 'e', 's', 'p', 'r', 'e', 's', 's', 'i', 'f', '.', 'c', 'o', 'm'};

/* Private function declarations */
static int characteristic_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gap_event_handler(struct ble_gap_event *event, void *arg);
void gatt_svr_subscribe_cb(struct ble_gap_event *event);
extern "C" void ble_store_config_init(void);


/* Private variables */
#define CHRONOS_SVC_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHRONOS_RX_CHARACTERISTIC_UUID "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define CHRONOS_TX_CHARACTERISTIC_UUID "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
static ble_uuid_any_t svc_uuid;
static ble_uuid_any_t rx_chr_uuid;
static ble_uuid_any_t tx_chr_uuid;
static uint16_t rx_char_handle;
static uint16_t tx_char_handle;
static bool subscribed = false;
static bool sleeping = false;
static int current_connection_handle = -1;

Notification::Notification(size_t text_size) {
  text = (char *)malloc(text_size + 1);
  text[text_size] = 0;
}

Notification::~Notification() {
  if (text)
    free(text);
}

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &svc_uuid.u,
     .includes = 0,
     .characteristics =
         (struct ble_gatt_chr_def[]){
             {
               .uuid = &rx_chr_uuid.u,
              .access_cb = characteristic_access,
              .arg = 0,
              .descriptors = 0,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
              .min_key_size = 0,
              .val_handle = &rx_char_handle,
              .cpfd = 0},
             {
               .uuid = &tx_chr_uuid.u,
              .access_cb = characteristic_access,
              .arg = 0,
              .descriptors = 0,
              .flags = BLE_GATT_CHR_F_NOTIFY,
              .min_key_size = 0,
              .val_handle = &tx_char_handle,
              .cpfd = NULL},
             {}}},

    {},
};

/* Private functions */
inline static void format_addr(char *addr_str, uint8_t addr[]) {
    sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X", addr[0], addr[1],
            addr[2], addr[3], addr[4], addr[5]);
}

static void print_conn_desc(struct ble_gap_conn_desc *desc) {
    /* Local variables */
    char addr_str[18] = {0};

    /* Connection handle */
    ESP_LOGI(TAG, "connection handle: %d", desc->conn_handle);

    /* Local ID address */
    format_addr(addr_str, desc->our_id_addr.val);
    ESP_LOGI(TAG, "device id address: type=%d, value=%s",
             desc->our_id_addr.type, addr_str);

    /* Peer ID address */
    format_addr(addr_str, desc->peer_id_addr.val);
    ESP_LOGI(TAG, "peer id address: type=%d, value=%s", desc->peer_id_addr.type,
             addr_str);

    /* Connection info */
    ESP_LOGI(TAG,
             "conn_itvl=%d, conn_latency=%d, supervision_timeout=%d, "
             "encrypted=%d, authenticated=%d, bonded=%d\n",
             desc->conn_itvl, desc->conn_latency, desc->supervision_timeout,
             desc->sec_state.encrypted, desc->sec_state.authenticated,
             desc->sec_state.bonded);
}

static void start_advertising(void) {

  if (sleeping)
    return;
  
  /* Local variables */
  int rc = 0;
  const char *name;
  struct ble_hs_adv_fields adv_fields = {};
  struct ble_hs_adv_fields rsp_fields = {};
  struct ble_gap_adv_params adv_params = {};

  /* Set advertising flags */
  adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

  /* Set device name */
  name = ble_svc_gap_device_name();
  adv_fields.name = (uint8_t *)name;
  adv_fields.name_len = strlen(name);
  adv_fields.name_is_complete = 1;

  /* Set device tx power */
  adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
  adv_fields.tx_pwr_lvl_is_present = 1;

  /* Set device appearance */
  adv_fields.appearance = BLE_GAP_APPEARANCE_GENERIC_TAG;
  adv_fields.appearance_is_present = 1;

  /* Set device LE role */
  adv_fields.le_role = BLE_GAP_LE_ROLE_PERIPHERAL;
  adv_fields.le_role_is_present = 1;

  /* Set advertiement fields */
  rc = ble_gap_adv_set_fields(&adv_fields);
  if (rc != 0) {
    ESP_LOGE(TAG, "failed to set advertising data, error code: %d", rc);
    return;
  }

  /* Set device address */
  rsp_fields.device_addr = addr_val;
  rsp_fields.device_addr_type = own_addr_type;
  rsp_fields.device_addr_is_present = 1;

  /* Set URI */
  rsp_fields.uri = esp_uri;
  rsp_fields.uri_len = sizeof(esp_uri);

  /* Set advertising interval */
  rsp_fields.adv_itvl = BLE_GAP_ADV_ITVL_MS(3000);
  rsp_fields.adv_itvl_is_present = 1;

  /* Set scan response fields */
  rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
  if (rc != 0) {
    ESP_LOGE(TAG, "failed to set scan response data, error code: %d", rc);
    return;
  }

  /* Set non-connetable and general discoverable mode to be a beacon */
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  /* Set advertising interval */
  adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(3000);
  adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(4000);

  /* Start advertising */
  rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                         gap_event_handler, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "failed to start advertising, error code: %d", rc);
    return;
  }
  ESP_LOGI(TAG, "advertising started!");
}

/*
 * NimBLE applies an event-driven model to keep GAP service going
 * gap_event_handler is a callback function registered when calling
 * ble_gap_adv_start API and called when a GAP event arrives
 */
static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    /* Local variables */
    int rc = 0;
    struct ble_gap_conn_desc desc;

    /* Handle different GAP event */
    switch (event->type) {

    /* Connect event */
    case BLE_GAP_EVENT_CONNECT: {
        /* A new connection was established or a connection attempt failed. */
        ESP_LOGI(TAG, "connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);

        /* Connection succeeded */
        if (event->connect.status == 0) {
            /* Check connection handle */
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc != 0) {
                ESP_LOGE(TAG,
                         "failed to find connection by handle, error code: %d",
                         rc);
                return rc;
            }

            /* Print connection descriptor */
            print_conn_desc(&desc);

            /* Try to update connection parameters */
            struct ble_gap_upd_params params = {.itvl_min = 500, // 625ms
                                                .itvl_max = 1000, // 1250ms
                                                .latency = 1, // up to 12.5s
                                                .supervision_timeout = 3000, // 20s
                                                .min_ce_len = 0,
                                                .max_ce_len = 0};
            rc = ble_gap_update_params(event->connect.conn_handle, &params);
            if (rc != 0) {
                ESP_LOGE(
                    TAG,
                    "failed to update connection parameters, error code: %d",
                    rc);
                return rc;
            }

            current_connection_handle = event->connect.conn_handle;
            struct Message msg = {CLIENT_CONNECTED, 0};
            xQueueSend(main_queue, &msg, 0);

        }
        /* Connection failed, restart advertising */
        else {
            start_advertising();
        }
        return rc;
    }

    /* Disconnect event */
    case BLE_GAP_EVENT_DISCONNECT: {
        /* A connection was terminated, print connection descriptor */
        ESP_LOGI(TAG, "disconnected from peer; reason=%d",
                 event->disconnect.reason);

        current_connection_handle = -1;
        struct Message msg = {CLIENT_DISCONNECTED, 0};
        xQueueSend(main_queue, &msg, 0);

        /* Restart advertising */
        start_advertising();
        return rc;
    }

    /* Connection parameters update event */
    case BLE_GAP_EVENT_CONN_UPDATE:
        /* The central has updated the connection parameters. */
        ESP_LOGI(TAG, "connection updated; status=%d",
                 event->conn_update.status);

        /* Print connection descriptor */
        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        if (rc != 0) {
            ESP_LOGE(TAG, "failed to find connection by handle, error code: %d",
                     rc);
            return rc;
        }
        print_conn_desc(&desc);
        return rc;

    /* Advertising complete event */
    case BLE_GAP_EVENT_ADV_COMPLETE:
        /* Advertising completed, restart advertising */
        ESP_LOGI(TAG, "advertise complete; reason=%d",
                 event->adv_complete.reason);
        start_advertising();
        return rc;

    /* Notification sent event */
    case BLE_GAP_EVENT_NOTIFY_TX:
        if ((event->notify_tx.status != 0) &&
            (event->notify_tx.status != BLE_HS_EDONE)) {
            /* Print notification info on error */
            ESP_LOGI(TAG,
                     "notify event; conn_handle=%d attr_handle=%d "
                     "status=%d is_indication=%d",
                     event->notify_tx.conn_handle, event->notify_tx.attr_handle,
                     event->notify_tx.status, event->notify_tx.indication);
        }
        return rc;

    /* Subscribe event */
    case BLE_GAP_EVENT_SUBSCRIBE:
        /* Print subscription info to log */
        ESP_LOGI(TAG,
                 "subscribe event; conn_handle=%d attr_handle=%d "
                 "reason=%d prevn=%d curn=%d previ=%d curi=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle,
                 event->subscribe.reason, event->subscribe.prev_notify,
                 event->subscribe.cur_notify, event->subscribe.prev_indicate,
                 event->subscribe.cur_indicate);

        /* GATT subscribe event callback */
        gatt_svr_subscribe_cb(event);
        return rc;

    /* MTU update event */
    case BLE_GAP_EVENT_MTU:
        /* Print MTU update info to log */
        ESP_LOGI(TAG, "mtu update event; conn_handle=%d cid=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.channel_id,
                 event->mtu.value);
        return rc;
    }

    return rc;
}


/* Public functions */
void adv_init(void) {
    /* Local variables */
    int rc = 0;
    char addr_str[18] = {0};

    /* Make sure we have proper BT identity address set (random preferred) */
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "device does not have any available bt address!");
        return;
    }

    /* Figure out BT address to use while advertising (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to infer address type, error code: %d", rc);
        return;
    }

    /* Printing ADDR */
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to copy device address, error code: %d", rc);
        return;
    }
    format_addr(addr_str, addr_val);
    ESP_LOGI(TAG, "device address: %s", addr_str);

    /* Start advertising. */
    start_advertising();
}

int gap_init(const char* name) {
    /* Local variables */
    int rc = 0;

    /* Call NimBLE GAP initialization API */
    ble_svc_gap_init();

    /* Set GAP device name */
    rc = ble_svc_gap_device_name_set(name);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set device name to %s, error code: %d",
                 name, rc);
        return rc;
    }
    return rc;
}

void dump_mbuf_meta(os_mbuf* mbuf) {
  ESP_LOGI(TAG, "mbuf, len=%u", mbuf->om_len);
  ESP_LOGI(TAG, "pool, name=%s, block_size=%lu, blocks=%u, free=%u",
           mbuf->om_omp->omp_pool->name,
           mbuf->om_omp->omp_pool->mp_block_size,
           mbuf->om_omp->omp_pool->mp_num_blocks,
           mbuf->om_omp->omp_pool->mp_num_free);
}

struct Accumulator {
  uint16_t length;
  os_mbuf* data;
};

Accumulator accumulator = {};

void parse_accumulated() {
  ESP_LOGI(TAG, "Packet, code: 0x%x", accumulator.data->om_data[4]);
  if (accumulator.data->om_data[0] == 0xab) {
    switch (accumulator.data->om_data[4]) {
    case 0x72: {
      accumulator.data = os_mbuf_pullup(accumulator.data, 8);
      int text_len = OS_MBUF_PKTLEN(accumulator.data) - 8;
      Notification *nt = new Notification(text_len);
      nt->icon = accumulator.data->om_data[6];
      nt->state = accumulator.data->om_data[7];
      os_mbuf_copydata(accumulator.data, 8, text_len, nt->text);
      struct Message msg = {CLIENT_NOTIFICATION, nt};
      xQueueSend(main_queue, &msg, 0);
      break;
    }
    case 0x71: {
      struct Message msg = {CLIENT_FIND, 0};
      xQueueSend(main_queue, &msg, 0);
      break;
    }
    case 0x73:
      // alarm
      break;
    case 0x93: {
      accumulator.data = os_mbuf_pullup(accumulator.data, 14);
      tm* t = new tm();
      t->tm_year = accumulator.data->om_data[7] * 256 + accumulator.data->om_data[8] - 1900;
      t->tm_mon = accumulator.data->om_data[9] - 1;
      t->tm_mday = accumulator.data->om_data[10];
      t->tm_hour = accumulator.data->om_data[11];
      t->tm_min = accumulator.data->om_data[12];
      t->tm_sec = accumulator.data->om_data[13];
      mktime(t);
      struct Message msg = {CLIENT_TIME, t};
      xQueueSend(main_queue, &msg, 0);
      break;
    }
    }
  }
}

void accumulate(os_mbuf* buf) {
  if ((buf->om_data[0] == 0xab || buf->om_data[0] == 0xea) &&
      (buf->om_data[3] == 0xfe || buf->om_data[3] == 0xff)) {
    accumulator.length = buf->om_data[1] * 256 + buf->om_data[2] + 3;
    accumulator.data = buf;
    uint16_t received = os_mbuf_len(buf);
    ESP_LOGI(TAG, "New packet, %u", received);
    if (accumulator.length <= received) {
      parse_accumulated();
      return;
    }
    accumulator.data = os_mbuf_dup(buf);
  } else {
    ESP_LOGI(TAG, "Packet fragment, %u", os_mbuf_len(buf));
    os_mbuf_appendfrom(accumulator.data, buf, 1, os_mbuf_len(buf) - 1);
    if (accumulator.length <= OS_MBUF_PKTLEN(accumulator.data)) {
      parse_accumulated();
      os_mbuf_free_chain(accumulator.data);
    }
  }
}

static int characteristic_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    int rc = 0;

    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
      ESP_LOGD(TAG, "characteristic access; conn_handle=%d attr_handle=%d",
               conn_handle, attr_handle);
    } else {
      ESP_LOGD(TAG, "characteristic access by nimble stack; attr_handle=%d",
               attr_handle);
    }

    switch (ctxt->op) {

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (attr_handle == rx_char_handle) {
          ESP_LOGD(TAG, "write the characteristic");
          ESP_LOGD(TAG, "%u bytes received", os_mbuf_len(ctxt->om));
          // dump_mbuf_meta(ctxt->om);
          accumulate(ctxt->om);
          return rc;
        }
        goto error;

    default:
      goto error;
    }

error:
    ESP_LOGE(TAG,
             "unexpected access operation to a characteristic, opcode: %d",
             ctxt->op);
    return BLE_ATT_ERR_UNLIKELY;
}

void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg) {
    /* Local variables */
    char buf[BLE_UUID_STR_LEN];

    /* Handle GATT attributes register events */
    switch (ctxt->op) {

    /* Service register event */
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGD(TAG, "registered service %s with handle=%d",
                 ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                 ctxt->svc.handle);
        break;

    /* Characteristic register event */
    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGD(TAG,
                 "registering characteristic %s with "
                 "def_handle=%d val_handle=%d",
                 ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                 ctxt->chr.def_handle, ctxt->chr.val_handle);
        break;

    /* Descriptor register event */
    case BLE_GATT_REGISTER_OP_DSC:
        ESP_LOGD(TAG, "registering descriptor %s with handle=%d",
                 ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                 ctxt->dsc.handle);
        break;

    /* Unknown event */
    default:
        assert(0);
        break;
    }
}

/*
 *  GATT server subscribe event callback
 *      1. Update heart rate subscription status
 */

void gatt_svr_subscribe_cb(struct ble_gap_event *event) {
    /* Check connection handle */
    if (event->subscribe.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "subscribe event; conn_handle=%d attr_handle=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle);
    } else {
        ESP_LOGI(TAG, "subscribe by nimble stack; attr_handle=%d",
                 event->subscribe.attr_handle);
    }

    /* Check attribute handle */
    if (event->subscribe.attr_handle == tx_char_handle) {
        /* Update heart rate subscription status */
        subscribed = true;
        Message msg = {};
        msg.type = CLIENT_SUBSCRIBED;
        xQueueSend(main_queue, &msg, 0);
    }
}

/*
 *  GATT server initialization
 *      1. Initialize GATT service
 *      2. Update NimBLE host GATT services counter
 *      3. Add GATT services to server
 */
int gatt_svc_init(void) {
    /* Local variables */
    int rc;

    /* 1. GATT service initialization */
    ble_svc_gatt_init();

    ble_uuid_from_str(&svc_uuid, CHRONOS_SVC_UUID);
    ble_uuid_from_str(&rx_chr_uuid, CHRONOS_RX_CHARACTERISTIC_UUID);
    ble_uuid_from_str(&tx_chr_uuid, CHRONOS_TX_CHARACTERISTIC_UUID);

    /* 2. Update GATT services counter */
    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    /* 3. Add GATT services */
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

/* Private functions */
/*
 *  Stack event callback functions
 *      - on_stack_reset is called when host resets BLE stack due to errors
 *      - on_stack_sync is called when host has synced with controller
 */
static void on_stack_reset(int reason) {
    /* On reset, print reset reason to console */
    ESP_LOGI(TAG, "nimble stack reset, reset reason: %d", reason);
}

static void on_stack_sync(void) {
    /* On stack sync, do advertising initialization */
    adv_init();
}

static void nimble_host_config_init(void) {
    /* Set host callbacks */
    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb = on_stack_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Store host configuration */
    ble_store_config_init();
}

static void nimble_host_task(void *param) {
    /* Task entry log */
    ESP_LOGI(TAG, "nimble host task has been started!");

    /* This function won't return until nimble_port_stop() is executed */
    nimble_port_run();

    /* Clean up at exit */
    vTaskDelete(NULL);
}

void send_command(uint8_t *command, size_t length) {
  if (subscribed && current_connection_handle >= 0) {
    struct os_mbuf* buf = ble_hs_mbuf_from_flat(command, length);
    ble_gatts_notify_custom(current_connection_handle,
                            tx_char_handle, buf);
  }
}

#define CHRONOSESP_VERSION_MAJOR 1
#define CHRONOSESP_VERSION_MINOR 6
#define CHRONOSESP_VERSION_PATCH 0

void send_info() {
  uint8_t infoCmd[] = {0xab, 0x00, 0x11, 0xff, 0x92, 0xc0,
                       CHRONOSESP_VERSION_MAJOR, 
                       (CHRONOSESP_VERSION_MINOR * 10 + CHRONOSESP_VERSION_PATCH),
                       0x00, 0xfb, 0x1e, 0x40, 0xc0, 0x0e,
                       0x32, 0x28, 0x00, 0xe2, 0, 0x80};
  send_command(infoCmd, 20);
}

void send_battery(uint8_t level) {
  // uint8_t c = _isCharging ? 0x01 : 0x00;
  uint8_t c = 0x0;
  uint8_t batCmd[] = {0xAB, 0x00, 0x05, 0xFF, 0x91, 0x80, c, level};
  send_command(batCmd, 8);
}

void setup_ble(const char* name, bool inactive) {
    /* Local variables */
    int rc;
    esp_err_t ret;

    sleeping = inactive;

    /*
     * NVS flash initialization
     * Dependency of BLE stack to store configurations
     */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nvs flash, error code: %d ", ret);
        return;
    }

    /* NimBLE stack initialization */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nimble stack, error code: %d ",
                 ret);
        return;
    }

    /* GAP service initialization */
    rc = gap_init(name);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to initialize GAP service, error code: %d", rc);
        return;
    }

    /* GATT server initialization */
    rc = gatt_svc_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to initialize GATT server, error code: %d", rc);
        return;
    }

    /* NimBLE host configuration initialization */
    nimble_host_config_init();

    /* Start NimBLE host task thread and return */
    xTaskCreate(nimble_host_task, "NimBLE Host", 4*1024, NULL, 5, NULL);
    // xTaskCreate(heart_rate_task, "Heart Rate", 4*1024, NULL, 5, NULL);
    return;
  
}

void ble_sleep() {
  sleeping = true;
  if (ble_gap_adv_active())
    ble_gap_adv_stop();
  if (current_connection_handle >= 0)
    ble_gap_terminate(current_connection_handle, 0x16);
}

void ble_wakeup() {
  sleeping = false;
  start_advertising();
}
