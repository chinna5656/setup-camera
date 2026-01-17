#include "http_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

static const char *TAG = "http_server";
static httpd_handle_t s_server = NULL;

// Stub values for bitrate (since camera encoder doesn't support VBR)
static uint32_t s_stub_bitrate = 4000000;
static uint32_t s_stub_variance_min = 500000;
static uint32_t s_stub_variance_max = 8000000;
static uint8_t s_stub_motion_level = 50;
static const char *s_stub_mode = "constant";

// Forward declarations
static esp_err_t static_file_handler(httpd_req_t *req);
static esp_err_t bitrate_get_handler(httpd_req_t *req);
static esp_err_t bitrate_post_handler(httpd_req_t *req);

static esp_err_t static_file_handler(httpd_req_t *req)
{
	char filepath[600];
	const char *uri = req->uri;

	// Skip /api/ routes
	if (strncmp(uri, "/api/", 5) == 0)
	{
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
		return ESP_FAIL;
	}

	// Default to index.html for root or missing extension
	if (strlen(uri) == 1 || strchr(uri, '.') == NULL)
	{
		snprintf(filepath, sizeof(filepath), "/static/index.html");
	}
	else
	{
		snprintf(filepath, sizeof(filepath), "/static%s", uri);
	}

	// Try to open the file
	int fd = open(filepath, O_RDONLY);
	if (fd == -1)
	{
		// SPA fallback: serve index.html for non-API routes
		ESP_LOGW(TAG, "File not found: %s, falling back to index.html", filepath);
		snprintf(filepath, sizeof(filepath), "/static/index.html");
		fd = open(filepath, O_RDONLY);
		if (fd == -1)
		{
			httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
			return ESP_FAIL;
		}
	}

	// Get file size
	struct stat st;
	if (fstat(fd, &st) != 0)
	{
		close(fd);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to stat file");
		return ESP_FAIL;
	}

	// Set content type based on file extension
	const char *ext = strrchr(filepath, '.');
	if (ext)
	{
		if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
			httpd_resp_set_type(req, "text/html");
		else if (strcmp(ext, ".css") == 0)
			httpd_resp_set_type(req, "text/css");
		else if (strcmp(ext, ".js") == 0)
			httpd_resp_set_type(req, "application/javascript");
		else if (strcmp(ext, ".json") == 0)
			httpd_resp_set_type(req, "application/json");
		else if (strcmp(ext, ".png") == 0)
			httpd_resp_set_type(req, "image/png");
		else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
			httpd_resp_set_type(req, "image/jpeg");
		else if (strcmp(ext, ".svg") == 0)
			httpd_resp_set_type(req, "image/svg+xml");
		else if (strcmp(ext, ".ico") == 0)
			httpd_resp_set_type(req, "image/x-icon");
	}

	// Send file
	char *buffer = malloc(st.st_size);
	if (!buffer)
	{
		close(fd);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate buffer");
		return ESP_FAIL;
	}

	ssize_t read_bytes = read(fd, buffer, st.st_size);
	close(fd);

	if (read_bytes != st.st_size)
	{
		free(buffer);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read file");
		return ESP_FAIL;
	}

	esp_err_t err = httpd_resp_send(req, buffer, read_bytes);
	free(buffer);

	return err;
}

static esp_err_t bitrate_get_handler(httpd_req_t *req)
{
	// Return stub values (camera encoder doesn't support VBR)
	cJSON *root = cJSON_CreateObject();
	cJSON *mode = cJSON_CreateString(s_stub_mode);
	cJSON_AddItemToObject(root, "mode", mode);

	cJSON_AddNumberToObject(root, "constant", s_stub_bitrate);
	cJSON_AddNumberToObject(root, "variance_min", s_stub_variance_min);
	cJSON_AddNumberToObject(root, "variance_max", s_stub_variance_max);

	cJSON *stats_obj = cJSON_CreateObject();
	cJSON_AddNumberToObject(stats_obj, "current_bitrate", s_stub_bitrate);
	cJSON_AddNumberToObject(stats_obj, "avg_frame_size", 0);
	cJSON_AddNumberToObject(stats_obj, "motion_level", s_stub_motion_level);
	cJSON_AddItemToObject(root, "stats", stats_obj);

	char *response = cJSON_PrintUnformatted(root);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, response, strlen(response));

	free(response);
	cJSON_Delete(root);
	return ESP_OK;
}

static esp_err_t bitrate_post_handler(httpd_req_t *req)
{
	// Get content length
	size_t recv_size = req->content_len;
	if (recv_size == 0)
	{
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request body");
		return ESP_FAIL;
	}

	// Allocate buffer for request body
	char *buffer = malloc(recv_size + 1);
	if (!buffer)
	{
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate buffer");
		return ESP_FAIL;
	}

	// Receive request body
	int received = httpd_req_recv(req, buffer, recv_size);
	if (received <= 0)
	{
		free(buffer);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive request");
		return ESP_FAIL;
	}
	buffer[received] = '\0';

	// Parse JSON
	cJSON *root = cJSON_Parse(buffer);
	free(buffer);

	if (!root)
	{
		cJSON *error = cJSON_CreateObject();
		cJSON_AddBoolToObject(error, "success", false);
		cJSON_AddStringToObject(error, "error", "Invalid JSON");
		char *response = cJSON_PrintUnformatted(error);
		httpd_resp_set_type(req, "application/json");
		httpd_resp_send(req, response, strlen(response));
		free(response);
		cJSON_Delete(error);
		return ESP_FAIL;
	}

	// Extract parameters (stub - just store values but don't apply them)
	cJSON *mode_json = cJSON_GetObjectItem(root, "mode");
	cJSON *constant_json = cJSON_GetObjectItem(root, "constant");
	cJSON *variance_min_json = cJSON_GetObjectItem(root, "variance_min");
	cJSON *variance_max_json = cJSON_GetObjectItem(root, "variance_max");

	bool success = true;
	const char *error_msg = NULL;

	// Validate and store mode (stub)
	if (mode_json && cJSON_IsString(mode_json))
	{
		const char *mode_str = mode_json->valuestring;
		if (strcmp(mode_str, "constant") == 0 ||
			strcmp(mode_str, "scene") == 0 ||
			strcmp(mode_str, "network") == 0)
		{
			s_stub_mode = mode_str;
			ESP_LOGW(TAG, "HTTP API: mode set to '%s' (stub - not applied to encoder)", mode_str);
		}
		else
		{
			success = false;
			error_msg = "Invalid mode (must be: constant, scene, or network)";
		}
	}

	// For constant mode, store the constant bitrate (stub)
	if (success && mode_json && cJSON_IsString(mode_json) &&
		strcmp(mode_json->valuestring, "constant") == 0)
	{
		if (constant_json && cJSON_IsNumber(constant_json))
		{
			uint32_t constant = (uint32_t)constant_json->valuedouble;
			if (constant >= 100000 && constant <= 20000000)
			{
				s_stub_bitrate = constant;
				ESP_LOGW(TAG, "HTTP API: bitrate set to %u bps (stub - not applied to encoder)", constant);
			}
			else
			{
				success = false;
				error_msg = "Invalid constant bitrate value";
			}
		}
		else
		{
			success = false;
			error_msg = "Missing 'constant' field for constant mode";
		}
	}

	// For non-constant modes, store variance range (stub)
	if (success && mode_json && cJSON_IsString(mode_json) &&
		strcmp(mode_json->valuestring, "constant") != 0)
	{
		if (variance_min_json && variance_max_json &&
			cJSON_IsNumber(variance_min_json) && cJSON_IsNumber(variance_max_json))
		{
			uint32_t min = (uint32_t)variance_min_json->valuedouble;
			uint32_t max = (uint32_t)variance_max_json->valuedouble;
			if (min < max && min >= 100000 && max <= 20000000)
			{
				s_stub_variance_min = min;
				s_stub_variance_max = max;
				ESP_LOGW(TAG, "HTTP API: variance range set to %u-%u bps (stub - not applied to encoder)", min, max);
			}
			else
			{
				success = false;
				error_msg = "Invalid variance range";
			}
		}
		else
		{
			success = false;
			error_msg = "Missing 'variance_min' or 'variance_max' for non-constant mode";
		}
	}

	cJSON_Delete(root);

	// Send response
	cJSON *response = cJSON_CreateObject();
	cJSON_AddBoolToObject(response, "success", success);
	if (!success && error_msg)
	{
		cJSON_AddStringToObject(response, "error", error_msg);
	}

	char *response_str = cJSON_PrintUnformatted(response);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, response_str, strlen(response_str));
	free(response_str);
	cJSON_Delete(response);

	return ESP_OK;
}

esp_err_t http_server_init(void)
{
	ESP_LOGI(TAG, "Initializing HTTP server (stub - API exists but encoder functions not available)");
	return ESP_OK;
}

esp_err_t http_server_start(void)
{
	if (s_server)
	{
		ESP_LOGW(TAG, "HTTP server already running");
		return ESP_OK;
	}

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port = 80;
	config.max_uri_handlers = 10;

	// Register URI handlers
	httpd_uri_t uri_bitrate_get = {
		.uri = "/api/settings/video.bitrate",
		.method = HTTP_GET,
		.handler = bitrate_get_handler,
		.user_ctx = NULL};

	httpd_uri_t uri_bitrate_post = {
		.uri = "/api/settings/video.bitrate",
		.method = HTTP_POST,
		.handler = bitrate_post_handler,
		.user_ctx = NULL};

	httpd_uri_t uri_static = {
		.uri = "/*",
		.method = HTTP_GET,
		.handler = static_file_handler,
		.user_ctx = NULL};

	if (httpd_start(&s_server, &config) == ESP_OK)
	{
		// Register API handlers first (more specific)
		httpd_register_uri_handler(s_server, &uri_bitrate_get);
		httpd_register_uri_handler(s_server, &uri_bitrate_post);

		// Register catch-all static handler last (for SPA)
		httpd_register_uri_handler(s_server, &uri_static);

		ESP_LOGI(TAG, "HTTP: server started");
		return ESP_OK;
	}

	ESP_LOGE(TAG, "Failed to start HTTP server");
	return ESP_FAIL;
}

esp_err_t http_server_stop(void)
{
	if (!s_server)
	{
		return ESP_OK;
	}

	if (httpd_stop(s_server) == ESP_OK)
	{
		s_server = NULL;
		ESP_LOGI(TAG, "HTTP server stopped");
		return ESP_OK;
	}

	return ESP_FAIL;
}
