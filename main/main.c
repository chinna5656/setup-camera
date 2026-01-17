#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "driver/gpio.h"
#include "rtsp_server.h"
#include "camera_encoder.h"
#include "camera_pattern.h"
#include "http_server.h"

static const char *TAG = "main";

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED)
	{
		ESP_LOGI(TAG, "Ethernet up");
	}
	else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED)
	{
		ESP_LOGI(TAG, "Ethernet down");
	}
	else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP)
	{
		ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
		ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));

		rtsp_server_start();
		http_server_start();
		ESP_LOGI(TAG, "RTSP: rtsp://" IPSTR ":8554", IP2STR(&event->ip_info.ip));
		ESP_LOGI(TAG, "HTTP: http://" IPSTR, IP2STR(&event->ip_info.ip));
	}
}

static void ethernet_init(void)
{
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
	esp_netif_t *netif = esp_netif_new(&cfg);

	ESP_ERROR_CHECK(gpio_install_isr_service(0));

	// PHY reset if configured
	if (CONFIG_ETH_PHY_RST_GPIO >= 0)
	{
		gpio_config_t cfg = {
			.pin_bit_mask = (1ULL << CONFIG_ETH_PHY_RST_GPIO),
			.mode = GPIO_MODE_OUTPUT,
		};
		gpio_config(&cfg);
		gpio_set_level(CONFIG_ETH_PHY_RST_GPIO, 0);
		vTaskDelay(pdMS_TO_TICKS(100));
		gpio_set_level(CONFIG_ETH_PHY_RST_GPIO, 1);
		vTaskDelay(pdMS_TO_TICKS(50));
	}

	// Setup MAC and PHY
	eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
	eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
	phy_cfg.phy_addr = CONFIG_ETH_PHY_ADDR;
	phy_cfg.reset_gpio_num = CONFIG_ETH_PHY_RST_GPIO;

	eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
	esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
	esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);

	esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
	esp_eth_handle_t eth_handle;
	ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth_handle));

	ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_event_handler, NULL));
	ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth_handle)));
	ESP_ERROR_CHECK(esp_eth_start(eth_handle));

	ESP_LOGI(TAG, "Ethernet init done");
}

void app_main(void)
{
	ESP_LOGI(TAG, "Starting...");

	// Init NVS
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		ESP_ERROR_CHECK(nvs_flash_init());
	}

	ESP_ERROR_CHECK(rtsp_server_init());
	ESP_ERROR_CHECK(http_server_init());
	ethernet_init();

	// Try camera, fallback to test pattern
	ret = camera_encoder_init();
	if (ret != ESP_OK)
	{
		ESP_LOGW(TAG, "No camera, using test pattern");
		ESP_ERROR_CHECK(pattern_init());
		ESP_ERROR_CHECK(pattern_start());
	}
	else
	{
		ESP_ERROR_CHECK(camera_encoder_start());
	}

	ESP_LOGI(TAG, "Running");
}
