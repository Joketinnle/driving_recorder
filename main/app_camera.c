#include <esp_event_loop.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
// #include "driver/ledc.h"
#include "esp_camera.h"
#include "sdkconfig.h"
#include "driver/ledc.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

#include "app_camera.h"

#define MOUNT_POINT "/sdcard"

static const char *TAG = "camera_task";

void camera_task(void *pvParameters)
{
	for (;;) {

		ESP_LOGI(TAG, "Taking picture...");
        camera_fb_t *pic = esp_camera_fb_get();
		int64_t timestamp = esp_timer_get_time();

		char *pic_name = malloc(17 + sizeof(int64_t));
		sprintf(pic_name, "/sdcard/pic_%lli.jpg", timestamp);
		FILE *file = fopen(pic_name, "w");
		if (file != NULL)
		{
			size_t err = fwrite(pic->buf, 1, pic->len, file);
			ESP_LOGI(TAG, "File saved: %s", pic_name);
		}
		else
		{
			ESP_LOGE(TAG, "Could not open file =(");
		}


        // use pic->buf to access the image
        ESP_LOGI(TAG, "Picture taken! Its size was: %zu bytes", pic->len);


		fclose(file);
		free(pic_name);

        vTaskDelay(5000 / portTICK_RATE_MS);
	}
}



static void camera_init()
{

#if CONFIG_CAMERA_MODEL_ESP_EYE || CONFIG_CAMERA_MODEL_ESP32_CAM_BOARD
	/* IO13, IO14 is designed for JTAG by default,
	* to use it as generalized input,
	* firstly declair it as pullup input */
	gpio_config_t conf;
	conf.mode = GPIO_MODE_INPUT;
	conf.pull_up_en = GPIO_PULLUP_ENABLE;
	conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
	conf.intr_type = GPIO_INTR_DISABLE;
	conf.pin_bit_mask = 1LL << 13;
	gpio_config(&conf);
	conf.pin_bit_mask = 1LL << 14;
	gpio_config(&conf);
#endif

#ifdef CONFIG_LED_ILLUMINATOR_ENABLED
	gpio_set_direction(CONFIG_LED_LEDC_PIN,GPIO_MODE_OUTPUT);
	ledc_timer_config_t ledc_timer = {
		.duty_resolution = LEDC_TIMER_8_BIT,            // resolution of PWM duty
		.freq_hz         = 1000,                        // frequency of PWM signal
		.speed_mode      = LEDC_LOW_SPEED_MODE,  // timer mode
		.timer_num       = CONFIG_LED_LEDC_TIMER        // timer index
	};
	ledc_channel_config_t ledc_channel = {
		.channel    = CONFIG_LED_LEDC_CHANNEL,
		.duty       = 0,
		.gpio_num   = CONFIG_LED_LEDC_PIN,
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.hpoint     = 0,
		.timer_sel  = CONFIG_LED_LEDC_TIMER
	};
	#ifdef CONFIG_LED_LEDC_HIGH_SPEED_MODE
	ledc_timer.speed_mode = ledc_channel.speed_mode = LEDC_HIGH_SPEED_MODE;
	#endif
	switch (ledc_timer_config(&ledc_timer)) {
		case ESP_ERR_INVALID_ARG: ESP_LOGE(TAG, "ledc_timer_config() parameter error"); break;
		case ESP_FAIL: ESP_LOGE(TAG, "ledc_timer_config() Can not find a proper pre-divider number base on the given frequency and the current duty_resolution"); break;
		case ESP_OK: if (ledc_channel_config(&ledc_channel) == ESP_ERR_INVALID_ARG) {
			ESP_LOGE(TAG, "ledc_channel_config() parameter error");
		}
		break;
		default: break;
	}
#endif

	camera_config_t config;
	config.ledc_channel = LEDC_CHANNEL_0;
	config.ledc_timer = LEDC_TIMER_0;
	config.pin_d0 = Y2_GPIO_NUM;
	config.pin_d1 = Y3_GPIO_NUM;
	config.pin_d2 = Y4_GPIO_NUM;
	config.pin_d3 = Y5_GPIO_NUM;
	config.pin_d4 = Y6_GPIO_NUM;
	config.pin_d5 = Y7_GPIO_NUM;
	config.pin_d6 = Y8_GPIO_NUM;
	config.pin_d7 = Y9_GPIO_NUM;
	config.pin_xclk = XCLK_GPIO_NUM;
	config.pin_pclk = PCLK_GPIO_NUM;
	config.pin_vsync = VSYNC_GPIO_NUM;
	config.pin_href = HREF_GPIO_NUM;
	config.pin_sscb_sda = SIOD_GPIO_NUM;
	config.pin_sscb_scl = SIOC_GPIO_NUM;
	config.pin_pwdn = PWDN_GPIO_NUM;
	config.pin_reset = RESET_GPIO_NUM;
	//XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
	config.xclk_freq_hz = 20000000;
	config.pixel_format = PIXFORMAT_JPEG;	//YUV422,GRAYSCALE,RGB565,JPEG
	//init with high specs to pre-allocate larger buffers
	config.frame_size = FRAMESIZE_QSXGA;	//QQVGA-UXGA Do not use sizes above QVGA when not JPEG
	config.jpeg_quality = 10;				//0-63 lower number means higher quality
	config.fb_count = 1;					//if more than one, i2s runs in continuous mode.
	// camera init
	esp_err_t err = esp_camera_init(&config);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
		return;
	}

	sensor_t * s = esp_camera_sensor_get();
	s->set_vflip(s, 1);//flip it back
	//initial sensors are flipped vertically and colors are a bit saturated
	if (s->id.PID == OV3660_PID) {
		s->set_brightness(s, 1);//up the blightness just a bit
		s->set_saturation(s, -2);//lower the saturation
	}
	//drop down frame size for higher initial frame rate
	s->set_framesize(s, FRAMESIZE_HD);

}

static void sdcard_init()
{
	esp_err_t ret;




	esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
		.format_if_mount_failed = true,
#else
		.format_if_mount_failed = false,
#endif
		.max_files = 5,
		.allocation_unit_size = 16 * 1024
	};

	sdmmc_card_t *card;
	const char mount_point[] = MOUNT_POINT;
	sdmmc_host_t host = SDMMC_HOST_DEFAULT();
	sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
	slot_config.width = 4;
	slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
	ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
	if (ret != ESP_OK) {
		if (ret == ESP_FAIL) {
			ESP_LOGE(TAG, "Failed to mount filesystem. "
					"If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
		} else {
			ESP_LOGE(TAG, "Failed to initialize the card (%s). "
					"Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
		}
		return;
	}
	ESP_LOGI(TAG, "Filesystem mounted");
	sdmmc_card_print_info(stdout, card);



}



void app_camera_main()
{
	camera_init();
	sdcard_init();

	// xTaskCreate(camera_task, "camera_task", 8192, NULL, 2, NULL);

}