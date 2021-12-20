#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "app_softap.h"
#include "app_sdcard.h"
#include "app_camera.h"





void app_main(void)
{
	app_wifi_main();
	// app_sdcard_main();
	app_camera_main();
	// xTaskCreate(sdcard_task, "sdcard_task", 8192, NULL, 3, NULL);
	// xTaskCreate(camera_task, "camera_task", 8192, NULL, 2, NULL);
	while (1) 
	{
		vTaskDelay(10000 / portTICK_PERIOD_MS);
		printf("free heap size:%d\n", esp_get_free_heap_size());
		
	}
}

