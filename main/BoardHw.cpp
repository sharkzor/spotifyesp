#include "BoardHw.h"

#include "sdkconfig.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef CONFIG_SPOTIFY_ETHERNET
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_mac.h"
#endif

#ifdef CONFIG_SPOTIFY_PCM5122
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#endif

static const char* TAG = "board";

namespace board {

#ifdef CONFIG_SPOTIFY_ETHERNET

static EventGroupHandle_t ethEvents;
static constexpr int ETH_GOT_IP_BIT = BIT0;

static void ethEventHandler(void*, esp_event_base_t base, int32_t id,
                            void*) {
  if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
    ESP_LOGI(TAG, "Ethernet link up");
  } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
    ESP_LOGW(TAG, "Ethernet link down");
  } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
    xEventGroupSetBits(ethEvents, ETH_GOT_IP_BIT);
  }
}

bool ethernetConnect() {
  ethEvents = xEventGroupCreate();
  esp_err_t err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "gpio isr service: %s", esp_err_to_name(err));
    return false;
  }

  spi_bus_config_t bus = {};
  bus.mosi_io_num = CONFIG_SPOTIFY_ETH_SPI_MOSI;
  bus.miso_io_num = CONFIG_SPOTIFY_ETH_SPI_MISO;
  bus.sclk_io_num = CONFIG_SPOTIFY_ETH_SPI_CLK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SPI bus: %s", esp_err_to_name(err));
    return false;
  }

  // Sonocotta runs the module at 8 MHz to avoid receive buffer shortages.
  spi_device_interface_config_t dev = {};
  dev.mode = 0;
  dev.clock_speed_hz = 8 * 1000 * 1000;
  dev.spics_io_num = CONFIG_SPOTIFY_ETH_CS;
  dev.queue_size = 20;

  eth_mac_config_t macCfg = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phyCfg = ETH_PHY_DEFAULT_CONFIG();
  phyCfg.reset_gpio_num = CONFIG_SPOTIFY_ETH_RST;
  eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &dev);
  w5500.int_gpio_num = CONFIG_SPOTIFY_ETH_INT;

  esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500, &macCfg);
  esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phyCfg);
  if (!mac || !phy) {
    ESP_LOGE(TAG, "W5500 driver init failed");
    return false;
  }

  esp_eth_config_t ethCfg = ETH_DEFAULT_CONFIG(mac, phy);
  esp_eth_handle_t eth = nullptr;
  err = esp_eth_driver_install(&ethCfg, &eth);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "No W5500 found (%s), is the module fitted?",
             esp_err_to_name(err));
    mac->del(mac);
    phy->del(phy);
    return false;
  }

  // The W5500 has no MAC address of its own.
  uint8_t addr[6];
  ESP_ERROR_CHECK(esp_read_mac(addr, ESP_MAC_ETH));
  ESP_ERROR_CHECK(esp_eth_ioctl(eth, ETH_CMD_S_MAC_ADDR, addr));

  esp_netif_config_t netifCfg = ESP_NETIF_DEFAULT_ETH();
  esp_netif_t* netif = esp_netif_new(&netifCfg);
  ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
  ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                             &ethEventHandler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                             &ethEventHandler, nullptr));
  ESP_ERROR_CHECK(esp_eth_start(eth));

  EventBits_t bits = xEventGroupWaitBits(
      ethEvents, ETH_GOT_IP_BIT, pdFALSE, pdFALSE,
      pdMS_TO_TICKS(CONFIG_SPOTIFY_ETH_TIMEOUT_S * 1000));
  if (!(bits & ETH_GOT_IP_BIT)) {
    ESP_LOGW(TAG, "No Ethernet address within %d s, falling back to WiFi",
             CONFIG_SPOTIFY_ETH_TIMEOUT_S);
    esp_eth_stop(eth);
    return false;
  }
  return true;
}

#else

bool ethernetConnect() { return false; }

#endif

#ifdef CONFIG_SPOTIFY_PCM5122

static void writeReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
  uint8_t buf[2] = {reg, value};
  esp_err_t err = i2c_master_transmit(dev, buf, sizeof(buf), 100);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "PCM5122 register 0x%02x write failed: %s", reg,
             esp_err_to_name(err));
  }
}

void dacInit() {
  if (CONFIG_SPOTIFY_PCM5122_XSMT >= 0) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << CONFIG_SPOTIFY_PCM5122_XSMT;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_SPOTIFY_PCM5122_XSMT), 1);
  }

  i2c_master_bus_config_t busCfg = {};
  busCfg.i2c_port = I2C_NUM_0;
  busCfg.sda_io_num = static_cast<gpio_num_t>(CONFIG_SPOTIFY_PCM5122_SDA);
  busCfg.scl_io_num = static_cast<gpio_num_t>(CONFIG_SPOTIFY_PCM5122_SCL);
  busCfg.clk_source = I2C_CLK_SRC_DEFAULT;
  busCfg.glitch_ignore_cnt = 7;
  busCfg.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t bus = nullptr;
  if (i2c_new_master_bus(&busCfg, &bus) != ESP_OK) {
    ESP_LOGW(TAG, "PCM5122: I2C bus init failed");
    return;
  }

  i2c_device_config_t devCfg = {};
  devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  devCfg.device_address = CONFIG_SPOTIFY_PCM5122_ADDR;
  devCfg.scl_speed_hz = 100000;
  i2c_master_dev_handle_t dev = nullptr;
  if (i2c_master_bus_add_device(bus, &devCfg, &dev) != ESP_OK ||
      i2c_master_probe(bus, CONFIG_SPOTIFY_PCM5122_ADDR, 100) != ESP_OK) {
    ESP_LOGW(TAG, "PCM5122 not answering at 0x%02x, leaving it as is",
             CONFIG_SPOTIFY_PCM5122_ADDR);
    return;
  }

  writeReg(dev, 0x00, 0x00);  // register page 0
  writeReg(dev, 0x02, 0x00);  // leave standby and power-down
  writeReg(dev, 0x0D, 0x10);  // PLL reference = BCK (SCK is not driven)
  writeReg(dev, 0x04, 0x01);  // enable the PLL
  ESP_LOGI(TAG, "PCM5122 at 0x%02x configured", CONFIG_SPOTIFY_PCM5122_ADDR);
}

#else

void dacInit() {}

#endif

}  // namespace board
