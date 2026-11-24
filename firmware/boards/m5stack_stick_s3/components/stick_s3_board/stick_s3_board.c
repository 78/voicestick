#include "stick_s3_board.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "stick_s3_board";
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_pmic_dev;

#define STICK_S3_I2C_FREQ_HZ 100000

#define M5PM1_ADDR 0x6e
#define M5PM1_REG_DEVICE_ID 0x00
#define M5PM1_REG_PWR_CFG 0x06
#define M5PM1_REG_HOLD_CFG 0x07
#define M5PM1_REG_I2C_CFG 0x09
#define M5PM1_REG_GPIO_MODE 0x10
#define M5PM1_REG_GPIO_OUT 0x11
#define M5PM1_REG_GPIO_IN 0x12
#define M5PM1_REG_GPIO_DRV 0x13
#define M5PM1_REG_GPIO_FUNC0 0x16
#define M5PM1_REG_BAT_L 0x22
#define M5PM1_REG_VIN_L 0x24
#define M5PM1_REG_IRQ_STATUS1 0x40
#define M5PM1_REG_IRQ_STATUS2 0x41
#define M5PM1_REG_IRQ_STATUS3 0x42
#define M5PM1_REG_IRQ_MASK1 0x43
#define M5PM1_REG_IRQ_MASK2 0x44
#define M5PM1_REG_IRQ_MASK3 0x45

#define M5PM1_PWR_CFG_LDO_EN BIT(2)
#define M5PM1_PWR_CFG_LED_CTRL BIT(4)
#define M5PM1_HOLD_CFG_LDO_HOLD BIT(5)
#define M5PM1_GPIO2_L3B_POWER_EN BIT(2)
#define M5PM1_GPIO_FUNC_MASK(pin) (0x03 << ((pin) * 2))
#define M5PM1_GPIO_FUNC_GPIO(pin) (0x00 << ((pin) * 2))
#define M5PM1_GPIO_FUNC_IRQ(pin)  (0x01 << ((pin) * 2))
#define M5PM1_IRQ_SYS_5VIN_INSERT BIT(0)
#define M5PM1_IRQ_SYS_5VIN_REMOVE BIT(1)

static bool read_active_low_button(gpio_num_t pin)
{
    return gpio_get_level(pin) == 0;
}

static esp_err_t pmic_read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_pmic_dev, &reg, 1, value, 1, 100);
}

static esp_err_t pmic_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_pmic_dev, &reg, 1, data, len, 100);
}

static esp_err_t pmic_write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_pmic_dev, data, sizeof(data), 100);
}

static esp_err_t pmic_update_reg(uint8_t reg, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t value = 0;
    esp_err_t err = pmic_read_reg(reg, &value);
    if (err != ESP_OK) {
        return err;
    }

    value &= ~clear_mask;
    value |= set_mask;
    return pmic_write_reg(reg, value);
}

static esp_err_t pmic_clear_irq_reg(uint8_t reg, uint8_t *status)
{
    uint8_t value = 0;
    esp_err_t err = pmic_read_reg(reg, &value);
    if (err != ESP_OK) {
        return err;
    }

    if (status) {
        *status = value;
    }

    return pmic_write_reg(reg, value ? (uint8_t)~value : 0x00);
}

static esp_err_t init_i2c_on(i2c_port_t port, gpio_num_t sda, gpio_num_t scl)
{
    if (s_i2c_bus) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        s_pmic_dev = NULL;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (err != ESP_OK) {
        return err;
    }

    const i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = M5PM1_ADDR,
        .scl_speed_hz = STICK_S3_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_pmic_dev);
}

static esp_err_t init_i2c(void)
{
    const struct {
        i2c_port_t port;
        gpio_num_t sda;
        gpio_num_t scl;
    } candidates[] = {
        {I2C_NUM_1, STICK_S3_PIN_I2C_SDA, STICK_S3_PIN_I2C_SCL},
        {I2C_NUM_1, STICK_S3_PIN_I2C_SCL, STICK_S3_PIN_I2C_SDA},
        {I2C_NUM_0, STICK_S3_PIN_I2C_SDA, STICK_S3_PIN_I2C_SCL},
        {I2C_NUM_0, STICK_S3_PIN_I2C_SCL, STICK_S3_PIN_I2C_SDA},
    };
    esp_err_t last_err = ESP_FAIL;

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        last_err = init_i2c_on(candidates[i].port, candidates[i].sda, candidates[i].scl);
        if (last_err != ESP_OK) {
            ESP_LOGW(TAG, "I2C port %d sda=%d scl=%d init failed: %s",
                     candidates[i].port, candidates[i].sda, candidates[i].scl,
                     esp_err_to_name(last_err));
            continue;
        }

        uint8_t device_id = 0;
        last_err = pmic_read_reg(M5PM1_REG_DEVICE_ID, &device_id);
        if (last_err == ESP_OK) {
            ESP_LOGI(TAG, "I2C probe port %d sda=%d scl=%d -> ok id=0x%02x",
                     candidates[i].port, candidates[i].sda, candidates[i].scl, device_id);
            return ESP_OK;
        }

        ESP_LOGI(TAG, "I2C probe port %d sda=%d scl=%d -> %s",
                 candidates[i].port, candidates[i].sda, candidates[i].scl,
                 esp_err_to_name(last_err));
    }

    return last_err;
}

static void init_pmic(void)
{
    uint8_t device_id = 0;
    esp_err_t err = pmic_read_reg(M5PM1_REG_DEVICE_ID, &device_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "M5PM1 not found at 0x%02x: %s", M5PM1_ADDR, esp_err_to_name(err));
        return;
    }

    uint8_t pwr_cfg = 0;
    uint8_t hold_cfg = 0;
    err = pmic_read_reg(M5PM1_REG_PWR_CFG, &pwr_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read PMIC power config failed: %s", esp_err_to_name(err));
        return;
    }

    err = pmic_read_reg(M5PM1_REG_HOLD_CFG, &hold_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read PMIC hold config failed: %s", esp_err_to_name(err));
        return;
    }

    const uint8_t new_pwr_cfg = (pwr_cfg | M5PM1_PWR_CFG_LDO_EN) & ~M5PM1_PWR_CFG_LED_CTRL;
    const uint8_t new_hold_cfg = hold_cfg | M5PM1_HOLD_CFG_LDO_HOLD;

    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_write_reg(M5PM1_REG_PWR_CFG, new_pwr_cfg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_write_reg(M5PM1_REG_HOLD_CFG, new_hold_cfg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_update_reg(M5PM1_REG_GPIO_FUNC0,
                                                  M5PM1_GPIO2_L3B_POWER_EN, 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_update_reg(M5PM1_REG_GPIO_MODE,
                                                  0, M5PM1_GPIO2_L3B_POWER_EN));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_update_reg(M5PM1_REG_GPIO_DRV,
                                                  M5PM1_GPIO2_L3B_POWER_EN, 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_update_reg(M5PM1_REG_GPIO_OUT,
                                                  0, M5PM1_GPIO2_L3B_POWER_EN));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_write_reg(M5PM1_REG_I2C_CFG, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_update_reg(M5PM1_REG_GPIO_FUNC0, BIT(0), 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_update_reg(M5PM1_REG_GPIO_MODE, BIT(0), 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_write_reg(M5PM1_REG_IRQ_MASK1, 0x1F));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_write_reg(M5PM1_REG_IRQ_MASK3, 0x07));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_write_reg(M5PM1_REG_IRQ_STATUS1, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_write_reg(M5PM1_REG_IRQ_STATUS2, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_write_reg(M5PM1_REG_IRQ_STATUS3, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_write_reg(M5PM1_REG_IRQ_MASK2,
                                                 0x3F & ~(M5PM1_IRQ_SYS_5VIN_INSERT |
                                                          M5PM1_IRQ_SYS_5VIN_REMOVE)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_update_reg(M5PM1_REG_GPIO_FUNC0,
                                                  M5PM1_GPIO_FUNC_MASK(1),
                                                  M5PM1_GPIO_FUNC_IRQ(1)));

    ESP_LOGI(TAG, "M5PM1 id=0x%02x pwr=0x%02x->0x%02x hold=0x%02x->0x%02x l3b_power=on",
             device_id, pwr_cfg, new_pwr_cfg, hold_cfg, new_hold_cfg);
}

esp_err_t stick_s3_board_init(void)
{
    esp_err_t err = init_i2c();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C init failed: %s", esp_err_to_name(err));
    } else {
        init_pmic();
    }

    const gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << STICK_S3_PIN_BUTTON_FRONT) |
                        (1ULL << STICK_S3_PIN_BUTTON_SIDE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&button_config);
}

bool stick_s3_front_button_pressed(void)
{
    return read_active_low_button(STICK_S3_PIN_BUTTON_FRONT);
}

i2c_master_bus_handle_t stick_s3_board_i2c_bus(void)
{
    return s_i2c_bus;
}

esp_err_t stick_s3_board_battery_voltage_mv(int *voltage_mv)
{
    if (!voltage_mv) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_pmic_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[2] = {0};
    esp_err_t err = pmic_read_regs(M5PM1_REG_BAT_L, data, sizeof(data));
    if (err != ESP_OK) {
        return err;
    }

    *voltage_mv = (data[1] << 8) | data[0];
    return *voltage_mv > 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t stick_s3_board_vbus_voltage_mv(int *voltage_mv)
{
    if (!voltage_mv) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_pmic_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[2] = {0};
    esp_err_t err = pmic_read_regs(M5PM1_REG_VIN_L, data, sizeof(data));
    if (err != ESP_OK) {
        return err;
    }

    *voltage_mv = (data[1] << 8) | data[0];
    return ESP_OK;
}

esp_err_t stick_s3_board_battery_level(int *level_percent)
{
    if (!level_percent) {
        return ESP_ERR_INVALID_ARG;
    }

    int voltage_mv = 0;
    esp_err_t err = stick_s3_board_battery_voltage_mv(&voltage_mv);
    if (err != ESP_OK) {
        return err;
    }

    int level = (voltage_mv - 3300) * 100 / (4150 - 3350);
    if (level < 0) {
        level = 0;
    } else if (level > 100) {
        level = 100;
    }

    *level_percent = level;
    return ESP_OK;
}

esp_err_t stick_s3_board_battery_charging(bool *charging)
{
    if (!charging) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_pmic_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t gpio_in = 0;
    esp_err_t err = pmic_read_reg(M5PM1_REG_GPIO_IN, &gpio_in);
    if (err != ESP_OK) {
        return err;
    }

    *charging = (gpio_in & BIT(0)) == 0;
    return ESP_OK;
}

esp_err_t stick_s3_board_usb_powered(bool *usb_powered)
{
    if (!usb_powered) {
        return ESP_ERR_INVALID_ARG;
    }

    int voltage_mv = 0;
    esp_err_t err = stick_s3_board_vbus_voltage_mv(&voltage_mv);
    if (err != ESP_OK) {
        return err;
    }

    *usb_powered = voltage_mv > 4500;
    return ESP_OK;
}

esp_err_t stick_s3_board_clear_power_irqs(uint8_t *sys_status)
{
    if (!s_pmic_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = pmic_clear_irq_reg(M5PM1_REG_IRQ_STATUS2, sys_status);
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_write_reg(M5PM1_REG_IRQ_STATUS1, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_write_reg(M5PM1_REG_IRQ_STATUS3, 0x00));
    return ESP_OK;
}

void stick_s3_board_prepare_deep_sleep(void)
{
    if (!s_pmic_dev) {
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_update_reg(M5PM1_REG_GPIO_OUT,
                                                  M5PM1_GPIO2_L3B_POWER_EN, 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_update_reg(M5PM1_REG_GPIO_DRV,
                                                  M5PM1_GPIO2_L3B_POWER_EN, 0));
}

bool stick_s3_side_button_pressed(void)
{
    return read_active_low_button(STICK_S3_PIN_BUTTON_SIDE);
}
