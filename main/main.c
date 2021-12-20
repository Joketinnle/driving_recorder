#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "softap_task.h"
#include "sdcard_task.h"






void app_main(void)
{
	xTaskCreate(softap_task, "softap_task", 4096, NULL, 3, NULL);
	xTaskCreate(sdcard_task, "sdcard_task", 8192, NULL, 3, NULL);

	while (1) 
	{
		vTaskDelay(10000 / portTICK_PERIOD_MS);
	}
}

