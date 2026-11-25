#include "magiclick_2p5.h"

#include "esp_log.h"

enum {
    LIS2DH12_ID = 0x33,
    REG_WHO_AM_I = 0x0f,
    REG_CTRL1 = 0x20,
    REG_CTRL4 = 0x23,
    REG_OUT_X_L = 0x28,
    READ_MULTIPLE = 0x80,
    CTRL1_100_HZ_XYZ = 0x57,
    CTRL4_BDU_4G = 0x90,
    I2C_TIMEOUT_MS = 10,
};

static const char *TAG = "magiclick_shake";
static i2c_master_dev_handle_t s_sensor;

static bool set_enabled(bool enabled)
{
    const uint8_t config[] = {REG_CTRL1, enabled ? CTRL1_100_HZ_XYZ : 0};
    const esp_err_t err = i2c_master_transmit(s_sensor, config, sizeof(config), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set accelerometer %s: %s", enabled ? "active" : "standby", esp_err_to_name(err));
        return false;
    }
    return true;
}

static int32_t read_axis_mg(const uint8_t *sample)
{
    /* Normal mode uses 10 left-aligned bits and 8 mg per count at ±4 g. */
    const int16_t raw = (int16_t)(((uint16_t)sample[1] << 8) | sample[0]);
    return (raw / 64) * 8;
}

static bool read_acceleration(acceleration_mg_t *sample)
{
    const uint8_t reg = REG_OUT_X_L | READ_MULTIPLE;
    uint8_t samples[6];
    if (i2c_master_transmit_receive(s_sensor, &reg, 1, samples, sizeof(samples), I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    *sample = (acceleration_mg_t){read_axis_mg(samples), read_axis_mg(samples + 2), read_axis_mg(samples + 4)};
    return true;
}

const board_confirmation_motion_t *magiclick_2p5_init_shake_sensor(i2c_master_bus_handle_t bus)
{
    static const board_confirmation_motion_t motion = {
        .set_enabled = set_enabled,
        .read_acceleration = read_acceleration,
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
            id == LIS2DH12_ID && set_enabled(false) &&
            i2c_master_transmit(s_sensor, range, sizeof(range), I2C_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "LIS2DH12 ready at 0x%02x", addresses[i]);
            return &motion;
        }
        (void)i2c_master_bus_rm_device(s_sensor);
    }
    ESP_LOGW(TAG, "LIS2DH12 not detected; shake cancellation unavailable");
    return NULL;
}
