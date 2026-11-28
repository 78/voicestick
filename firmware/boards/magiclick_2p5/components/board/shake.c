#include "magiclick_2p5.h"

#include "esp_log.h"

enum {
    LIS2DH12_ID = 0x33,
    REG_WHO_AM_I = 0x0f,
    REG_CTRL1 = 0x20,
    REG_CTRL4 = 0x23,
    REG_OUT_X_L = 0x28,
    READ_MULTIPLE = 0x80,
    CTRL1_50_HZ_XYZ = 0x47,
    CTRL1_100_HZ_XYZ = 0x57,
    CTRL4_BDU_4G = 0x90,
    I2C_TIMEOUT_MS = 10,
};

static const char *TAG = "magiclick_imu";
static i2c_master_dev_handle_t s_sensor;

static bool set_enabled(bool enabled, bool include_gyro, uint16_t rate_hz)
{
    if (!s_sensor || (enabled && include_gyro)) {
        return false;
    }
    const uint8_t ctrl1 = !enabled ? 0 : (rate_hz <= 50 ? CTRL1_50_HZ_XYZ : CTRL1_100_HZ_XYZ);
    const uint8_t config[] = {REG_CTRL1, ctrl1};
    const esp_err_t err = i2c_master_transmit(s_sensor, config, sizeof(config), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set accelerometer %s: %s", enabled ? "active" : "standby", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool read_samples(bool include_gyro, uint8_t *out_bytes)
{
    if (!s_sensor || include_gyro) {
        return false;
    }
    const uint8_t reg = REG_OUT_X_L | READ_MULTIPLE;
    return i2c_master_transmit_receive(s_sensor, &reg, 1, out_bytes, 6, I2C_TIMEOUT_MS) == ESP_OK;
}

const board_imu_t *magiclick_2p5_init_imu_sensor(i2c_master_bus_handle_t bus)
{
    /* Normal mode uses 10 left-aligned bits at 8 mg/digit (±4 g) -> 8000 LSB/g. */
    static const board_imu_t imu = {
        .has_gyro = false,
        .accel_lsb_per_g = 8000,
        .gyro_lsb_per_dps = 0.0f,
        .max_rate_hz = 100,
        .set_enabled = set_enabled,
        .read_samples = read_samples,
    };
    const uint8_t addresses[] = {0x19, 0x18};
    for (size_t i = 0; i < sizeof(addresses); ++i) {
        if (i2c_master_probe(bus, addresses[i], I2C_TIMEOUT_MS) != ESP_OK) {
            continue;
        }
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addresses[i],
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(bus, &config, &s_sensor) != ESP_OK) {
            continue;
        }

        const uint8_t id_reg = REG_WHO_AM_I;
        const uint8_t range[] = {REG_CTRL4, CTRL4_BDU_4G};
        uint8_t id;
        if (i2c_master_transmit_receive(s_sensor, &id_reg, 1, &id, 1, I2C_TIMEOUT_MS) == ESP_OK &&
            id == LIS2DH12_ID && set_enabled(false, false, 50) &&
            i2c_master_transmit(s_sensor, range, sizeof(range), I2C_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "LIS2DH12 3-axis IMU ready at 0x%02x", addresses[i]);
            return &imu;
        }
        (void)i2c_master_bus_rm_device(s_sensor);
        s_sensor = NULL;
    }
    ESP_LOGW(TAG, "LIS2DH12 not detected; IMU unavailable");
    return NULL;
}
