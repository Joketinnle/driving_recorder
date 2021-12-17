#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "softap_task.h"







void app_main(void)
{
	xTaskCreate(softap_task, "softap_task", 4096, NULL, 2, NULL);

	while (1) 
	{
		vTaskDelay(10000 / portTICK_PERIOD_MS);
		printf("main\r\n");
	}
}

