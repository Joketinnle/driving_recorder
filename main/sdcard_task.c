/* SD card and FAT filesystem example.
This example code is in the Public Domain (or CC0 licensed, at your option.)

Unless required by applicable law or agreed to in writing, this
software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied.
*/

// This example uses SDMMC peripheral to communicate with SD card.

#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include "vfs_fat_internal.h"
#include "diskio_impl.h"

#include "diskio_rawflash.h"

#include "wear_levelling.h"
#include "diskio_wl.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "vfs_fat_internal.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "soc/soc_caps.h"
#include "driver/sdmmc_defs.h"




#include "sdcard_task.h"

static const char *TAG = "sdcard_task";

#define MOUNT_POINT "/sdcard"

static sdmmc_card_t* s_card = NULL;
static uint8_t s_pdrv = FF_DRV_NOT_USED;
static char * s_base_path = NULL;

#define CHECK_EXECUTE_RESULT(err, str) do { \
    if ((err) !=ESP_OK) { \
        ESP_LOGE(TAG, str" (0x%x).", err); \
        goto cleanup; \
    } \
    } while(0)


static esp_err_t init_sdmmc_host(int slot, const void *slot_config, int *out_slot)
{
    *out_slot = slot;
    return sdmmc_host_init_slot(slot, (const sdmmc_slot_config_t*) slot_config);
}


static void call_host_deinit(const sdmmc_host_t *host_config)
{
    if (host_config->flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        host_config->deinit_p(host_config->slot);
    } else {
        host_config->deinit();
    }
}

static esp_err_t init_sdspi_host_deprecated(int slot, const void *slot_config, int *out_slot)
{
    *out_slot = slot;
    return sdspi_host_init_slot(slot, (const sdspi_slot_config_t*) slot_config);
}

static esp_err_t mount_prepare_mem(const char *base_path,
        BYTE *out_pdrv,
        char **out_dup_path,
        sdmmc_card_t** out_card)
{
    esp_err_t err = ESP_OK;
    char* dup_path = NULL;
    sdmmc_card_t* card = NULL;

    // connect SDMMC driver to FATFS
    BYTE pdrv = FF_DRV_NOT_USED;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == FF_DRV_NOT_USED) {
        ESP_LOGD(TAG, "the maximum count of volumes is already mounted");
        return ESP_ERR_NO_MEM;

    }

    // not using ff_memalloc here, as allocation in internal RAM is preferred
    card = (sdmmc_card_t*)malloc(sizeof(sdmmc_card_t));
    if (card == NULL) {
        ESP_LOGD(TAG, "could not locate new sdmmc_card_t");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    dup_path = strdup(base_path);
    if(!dup_path){
        ESP_LOGD(TAG, "could not copy base_path");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    *out_card = card;
    *out_pdrv = pdrv;
    *out_dup_path = dup_path;
    return ESP_OK;
cleanup:
    free(card);
    free(dup_path);
    return err;
}

static esp_err_t partition_card_user(const esp_vfs_fat_mount_config_t *mount_config,
                                const char *drv, sdmmc_card_t *card, BYTE pdrv)
{
    FRESULT res = FR_OK;
    esp_err_t err;
    const size_t workbuf_size = 4096;
    void* workbuf = NULL;
    ESP_LOGW(TAG, "partitioning card");

    workbuf = ff_memalloc(workbuf_size);
    if (workbuf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    DWORD plist[] = {100, 0, 0, 0};
    res = f_fdisk(pdrv, plist, workbuf);
    if (res != FR_OK) {
        err = ESP_FAIL;
        ESP_LOGD(TAG, "f_fdisk failed (%d)", res);
        goto fail;
    }
    size_t alloc_unit_size = esp_vfs_fat_get_allocation_unit_size(
                card->csd.sector_size,
                mount_config->allocation_unit_size);
    ESP_LOGW(TAG, "formatting card, allocation unit size=%d", alloc_unit_size);
    res = f_mkfs(drv, FM_EXFAT, alloc_unit_size, workbuf, workbuf_size); // FM_FAT FM_FAT32 FM_EXFAT FM_ANY FM_SFD
    if (res != FR_OK) {
        err = ESP_FAIL;
        ESP_LOGD(TAG, "f_mkfs failed (%d)", res);
        goto fail;
    }

    free(workbuf);
    return ESP_OK;
fail:
    free(workbuf);
    return err;
}


static esp_err_t mount_to_vfs_fat_user(const esp_vfs_fat_mount_config_t *mount_config,
										sdmmc_card_t *card, uint8_t pdrv,
                                		const char *base_path)
{
    FATFS* fs = NULL;
    esp_err_t err;
    ff_diskio_register_sdmmc(pdrv, card);
    ESP_LOGD(TAG, "using pdrv=%i", pdrv);
    char drv[3] = {(char)('0' + pdrv), ':', 0};

    // connect FATFS to VFS
    err = esp_vfs_fat_register(base_path, drv, mount_config->max_files, &fs);
    if (err == ESP_ERR_INVALID_STATE) {
        // it's okay, already registered with VFS
    } else if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_vfs_fat_register failed 0x(%x)", err);
        goto fail;
    }

    // Try to mount partition
    FRESULT res = f_mount(fs, drv, 1);
    if (res != FR_OK) {
        err = ESP_FAIL;
        ESP_LOGW(TAG, "failed to mount card (%d)", res);
        if (!((res == FR_NO_FILESYSTEM || res == FR_INT_ERR)
              && mount_config->format_if_mount_failed)) {
            goto fail;
        }

        err = partition_card_user(mount_config, drv, card, pdrv);
        if (err != ESP_OK) {
            goto fail;
        }

        ESP_LOGW(TAG, "mounting again");
        res = f_mount(fs, drv, 0);
        if (res != FR_OK) {
            err = ESP_FAIL;
            ESP_LOGD(TAG, "f_mount failed after formatting (%d)", res);
            goto fail;
        }
    } else {
        err = partition_card_user(mount_config, drv, card, pdrv);
        if (err != ESP_OK) {
            goto fail;
        }

        ESP_LOGW(TAG, "mounting again");
        res = f_mount(fs, drv, 0);
        if (res != FR_OK) {
            err = ESP_FAIL;
            ESP_LOGD(TAG, "f_mount failed after formatting (%d)", res);
            goto fail;
        }
	}
    return ESP_OK;

fail:
    if (fs) {
        f_mount(NULL, drv, 0);
    }
    esp_vfs_fat_unregister_path(base_path);
    ff_diskio_unregister(pdrv);
    return err;
}



esp_err_t sdcard_format(const char* base_path,
						const sdmmc_host_t* host_config,
						const void* slot_config,
						const esp_vfs_fat_mount_config_t* mount_config,
						sdmmc_card_t** out_card)
{
	esp_err_t err;
	int card_handle = -1;   //uninitialized
	sdmmc_card_t* card = NULL;
	BYTE pdrv = FF_DRV_NOT_USED;
	char* dup_path = NULL;
	bool host_inited = false;

	err = mount_prepare_mem(base_path, &pdrv, &dup_path, &card);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "mount_prepare failed");
		return err;
	}

	if (host_config->flags == SDMMC_HOST_FLAG_SPI) {
		//Deprecated API
		//the init() function is usually empty, doesn't require any deinit to revert it
		err = (*host_config->init)();
		CHECK_EXECUTE_RESULT(err, "host init failed");
		err = init_sdspi_host_deprecated(host_config->slot, slot_config, &card_handle);
		CHECK_EXECUTE_RESULT(err, "slot init failed");
		//Set `host_inited` to true to indicate that host_config->deinit() needs
		//to be called to revert `init_sdspi_host_deprecated`; set `card_handle`
		//to -1 to indicate that no other deinit is required.
		host_inited = true;
		card_handle = -1;
	} else {
		err = (*host_config->init)();
		CHECK_EXECUTE_RESULT(err, "host init failed");
		//deinit() needs to be called to revert the init
		host_inited = true;
		//If this failed (indicated by card_handle != -1), slot deinit needs to called()
		//leave card_handle as is to indicate that (though slot deinit not implemented yet.
		err = init_sdmmc_host(host_config->slot, slot_config, &card_handle);
		CHECK_EXECUTE_RESULT(err, "slot init failed");
	}

	// probe and initialize card
	err = sdmmc_card_init(host_config, card);
	CHECK_EXECUTE_RESULT(err, "sdmmc_card_init failed");

	err = mount_to_vfs_fat_user(mount_config, card, pdrv, dup_path);
	CHECK_EXECUTE_RESULT(err, "mount_to_vfs failed");

	if (out_card != NULL) {
		*out_card = card;
	}
	if (s_card == NULL) {
		//store the ctx locally to be back-compatible
		s_card = card;
		s_pdrv = pdrv;
		s_base_path = dup_path;
	} else {
		free(dup_path);
	}
	return ESP_OK;
cleanup:
	if (host_inited) {
		call_host_deinit(host_config);
	}
	free(card);
	free(dup_path);
	return err;


}








void sdcard_task(void *pvParameters)
{
	esp_err_t ret;

	// Options for mounting the filesystem.
	// If format_if_mount_failed is set to true, SD card will be partitioned and
	// formatted in case when mounting fails.
	esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
		.format_if_mount_failed = true,
#else
		.format_if_mount_failed = false,
#endif // EXAMPLE_FORMAT_IF_MOUNT_FAILED
		.max_files = 5,
		.allocation_unit_size = 16 * 1024
	};
	sdmmc_card_t *card;
	const char mount_point[] = MOUNT_POINT;
	ESP_LOGI(TAG, "Initializing SD card");

	// Use settings defined above to initialize SD card and mount FAT filesystem.
	// Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
	// Please check its source code and implement error recovery when developing
	// production applications.

	ESP_LOGI(TAG, "Using SDMMC peripheral");
	sdmmc_host_t host = SDMMC_HOST_DEFAULT();

	// This initializes the slot without card detect (CD) and write protect (WP) signals.
	// Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
	sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

	// To use 1-line SD mode, change this to 1:
	slot_config.width = 4;

	// On chips where the GPIOs used for SD card can be configured, set them in
	// the slot_config structure:
#ifdef SOC_SDMMC_USE_GPIO_MATRIX
	slot_config.clk = GPIO_NUM_14;
	slot_config.cmd = GPIO_NUM_15;
	slot_config.d0 = GPIO_NUM_2;
	slot_config.d1 = GPIO_NUM_4;
	slot_config.d2 = GPIO_NUM_12;
	slot_config.d3 = GPIO_NUM_13;
#endif

	// Enable internal pullups on enabled pins. The internal pullups
	// are insufficient however, please make sure 10k external pullups are
	// connected on the bus. This is for debug / example purpose only.
	slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

	/* debug for format */
	// ESP_LOGI(TAG, "sd card formating...");
	// sdcard_format(mount_point, &host, &slot_config, &mount_config, &card);
	// esp_vfs_fat_sdcard_unmount(mount_point, card);


	ESP_LOGI(TAG, "Mounting filesystem");
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

	// Card has been initialized, print its properties
	sdmmc_card_print_info(stdout, card);

	// Use POSIX and C standard library functions to work with files:

	// First create a file.
	const char *file_hello = MOUNT_POINT"/hello.txt";

	ESP_LOGI(TAG, "Opening file %s", file_hello);
	FILE *f = fopen(file_hello, "w");
	if (f == NULL) {
		ESP_LOGE(TAG, "Failed to open file for writing");
		return;
	}
	fprintf(f, "Hello %s!\n", card->cid.name);
	fclose(f);
	ESP_LOGI(TAG, "File written");

	const char *file_foo = MOUNT_POINT"/foo.txt";

	// Check if destination file exists before renaming
	struct stat st;
	if (stat(file_foo, &st) == 0) {
		// Delete it if it exists
		unlink(file_foo);
	}

	// Rename original file
	ESP_LOGI(TAG, "Renaming file %s to %s", file_hello, file_foo);
	if (rename(file_hello, file_foo) != 0) {
		ESP_LOGE(TAG, "Rename failed");
		return;
	}

	// Open renamed file for reading
	ESP_LOGI(TAG, "Reading file %s", file_foo);
	f = fopen(file_foo, "r");
	if (f == NULL) {
		ESP_LOGE(TAG, "Failed to open file for reading");
		return;
	}

	// Read a line from file
	char line[64];
	fgets(line, sizeof(line), f);
	fclose(f);

	// Strip newline
	char *pos = strchr(line, '\n');
	if (pos) {
		*pos = '\0';
	}
	ESP_LOGI(TAG, "Read from file: '%s'", line);

	vTaskDelay(10000 / portTICK_PERIOD_MS);

	// All done, unmount partition and disable SDMMC peripheral
	esp_vfs_fat_sdcard_unmount(mount_point, card);
	ESP_LOGI(TAG, "Card unmounted");


	// sdcard_format(mount_point, &host, &slot_config, &mount_config, &card);
	// esp_vfs_fat_sdcard_unmount(mount_point, card);

	for (;;)
	{
		vTaskDelay(10000 / portTICK_PERIOD_MS);
	}
}
