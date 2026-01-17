/**
 * @file http_server.h
 * @brief HTTP server with bitrate control API
 *
 * Provides HTTP server with static file serving and JSON API for bitrate control.
 * Note: Currently stub implementation - API exists but returns placeholder data.
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize HTTP server
 *
 * @return ESP_OK on success
 */
esp_err_t http_server_init(void);

/**
 * @brief Start HTTP server
 *
 * Starts HTTP server on port 80 with static file serving and API endpoints
 *
 * @return ESP_OK on success
 */
esp_err_t http_server_start(void);

/**
 * @brief Stop HTTP server
 *
 * @return ESP_OK on success
 */
esp_err_t http_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif // HTTP_SERVER_H
