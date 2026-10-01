/*
 * Copyright (c) 2026 CTCaer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include <bdk.h>

#include "gui.h"
#include "gui_tools.h"
#include "gui_file_manager.h"
#include "../config.h"
#include "../hos/hos.h"
#include "fe_emummc_tools.h"
#include <libs/fatfs/diskio.h>
#include <libs/fatfs/ff.h>

#define FM_PATH_MAX 512
#define FM_BUF      SDXC_BUF_ALIGNED
#define FM_BUF_SIZE SZ_4M

// free_ptr tags for the drive entries.
#define FM_DRIVE_SYSTEM    ((void *)1)
#define FM_DRIVE_USER      ((void *)2)
#define FM_DRIVE_EMU_SYS   ((void *)3)
#define FM_DRIVE_EMU_USER  ((void *)4)

typedef struct _fm_ctx_t
{
	lv_obj_t *win;
	lv_obj_t *path_label;
	lv_obj_t *list;
	char path[FM_PATH_MAX];
	char clip_path[FM_PATH_MAX];
	bool clip_is_dir;
	// eMMC/emuMMC BIS state.
	bool bis_mounted;
	bool bis_is_emummc;
	bool bis_user_part;
	bool bis_keys_derived;
	FATFS bis_fs;
	link_t bis_gpt;
	const char *src_name;
	// Raw partition emuMMC info.
	bool emummc_raw_available;
	u32  emummc_sector;
	// File based emuMMC info.
	bool emummc_file_available;
	u32  emummc_file_part_size; // Sectors per part file.
	char *emummc_file_dir;      // <path>/eMMC. Part files appended as NN.
} fm_ctx_t;

static fm_ctx_t fm_ctx;

// Context for the current file menu. Single instance GUI, so static is safe.
static char fm_menu_path[FM_PATH_MAX];
static bool fm_menu_is_dir;

static void _fm_list_dir(fm_ctx_t *ctx);
static lv_res_t _fm_delete_confirm_action(lv_obj_t *btns, const char *txt);
static lv_res_t _fm_file_menu_action(lv_obj_t *btns, const char *txt);
static lv_res_t _fm_action_close_emmc(lv_obj_t *btn);

// Read sectors from a file based emuMMC. Handles part file spanning.
static int _fm_emummc_file_read_sectors(u32 sector, u32 count, void *buff)
{
	u32 dir_len = strlen(fm_ctx.emummc_file_dir);
	char *path = (char *)malloc(dir_len + 4);
	u8 *buf = (u8 *)buff;
	if (!path)
		return 1;

	int res = 0;
	while (count && !res)
	{
		u32 file_part = sector / fm_ctx.emummc_file_part_size;
		u32 file_sector = sector % fm_ctx.emummc_file_part_size;
		u32 chunk = MIN(count, fm_ctx.emummc_file_part_size - file_sector);

		strcpy(path, fm_ctx.emummc_file_dir);
		path[dir_len] = '/';
		if (file_part >= 10)
			itoa(file_part, path + dir_len + 1, 10);
		else
		{
			path[dir_len + 1] = '0';
			itoa(file_part, path + dir_len + 2, 10);
		}

		FIL fp;
		UINT read_bytes = 0;
		if (f_open(&fp, path, FA_READ))
			res = 1;
		else
		{
			if (f_lseek(&fp, (u64)file_sector << 9) ||
				f_read(&fp, buf, (u64)chunk << 9, &read_bytes) || read_bytes != chunk << 9)
				res = 1;
			f_close(&fp);
		}

		sector += chunk;
		count -= chunk;
		buf += chunk << 9;
	}

	free(path);

	return res;
}

// Parse the emuMMC GPT from a file based emuMMC. Mirrors emmc_gpt_parse.
static int _fm_emummc_file_gpt_parse(link_t *gpt)
{
	gpt_t *gpt_buf = (gpt_t *)zalloc(GPT_NUM_BLOCKS * EMMC_BLOCKSIZE);

	if (_fm_emummc_file_read_sectors(GPT_FIRST_LBA, GPT_NUM_BLOCKS, gpt_buf))
	{
		free(gpt_buf);
		return 1;
	}

	// Check if no GPT or more than max allowed entries.
	if (memcmp(&gpt_buf->header.signature, "EFI PART", 8) || gpt_buf->header.num_part_ents > 128)
	{
		free(gpt_buf);
		return 1;
	}

	for (u32 i = 0; i < gpt_buf->header.num_part_ents; i++)
	{
		emmc_part_t *part = (emmc_part_t *)zalloc(sizeof(emmc_part_t));

		if (gpt_buf->entries[i].lba_start < gpt_buf->header.first_use_lba)
			continue;

		part->index     = i;
		part->lba_start = gpt_buf->entries[i].lba_start;
		part->lba_end   = gpt_buf->entries[i].lba_end;
		part->attrs     = gpt_buf->entries[i].attrs;

		// ASCII conversion. Copy only the LSByte of the UTF-16LE name.
		for (u32 j = 0; j < 36; j++)
			part->name[j] = gpt_buf->entries[i].name[j];
		part->name[35] = 0;

		list_append(gpt, &part->link);
	}

	free(gpt_buf);

	return 0;
}

// Parse the emuMMC GPT from the SD image base sector. Mirrors emmc_gpt_parse.
static int _fm_emummc_gpt_parse(link_t *gpt, u32 emu_offset)
{
	gpt_t *gpt_buf = (gpt_t *)zalloc(GPT_NUM_BLOCKS * EMMC_BLOCKSIZE);

	if (sdmmc_storage_read(&sd_storage, GPT_FIRST_LBA + emu_offset, GPT_NUM_BLOCKS, gpt_buf))
	{
		free(gpt_buf);
		return 1;
	}

	// Check if no GPT or more than max allowed entries.
	if (memcmp(&gpt_buf->header.signature, "EFI PART", 8) || gpt_buf->header.num_part_ents > 128)
	{
		free(gpt_buf);
		return 1;
	}

	for (u32 i = 0; i < gpt_buf->header.num_part_ents; i++)
	{
		emmc_part_t *part = (emmc_part_t *)zalloc(sizeof(emmc_part_t));

		if (gpt_buf->entries[i].lba_start < gpt_buf->header.first_use_lba)
			continue;

		part->index     = i;
		part->lba_start = gpt_buf->entries[i].lba_start;
		part->lba_end   = gpt_buf->entries[i].lba_end;
		part->attrs     = gpt_buf->entries[i].attrs;

		// ASCII conversion. Copy only the LSByte of the UTF-16LE name.
		for (u32 j = 0; j < 36; j++)
			part->name[j] = gpt_buf->entries[i].name[j];
		part->name[35] = 0;

		list_append(gpt, &part->link);
	}

	free(gpt_buf);

	return 0;
}

static void _fm_bis_unmount(fm_ctx_t *ctx)
{
	if (!ctx->bis_mounted)
		return;

	f_unmount("bis:");
	nx_emmc_bis_end();
	emmc_gpt_free(&ctx->bis_gpt);
	list_init(&ctx->bis_gpt);
	if (!ctx->bis_is_emummc)
		emmc_end();

	// Only invalidate clipboard if it points to the BIS volume.
	if (ctx->clip_path[0] && !strncmp(ctx->clip_path, "bis:", 4))
	{
		ctx->clip_path[0] = 0;
		ctx->clip_is_dir = false;
	}

	ctx->bis_mounted = false;
	ctx->bis_is_emummc = false;
	ctx->bis_user_part = false;
	ctx->src_name = NULL;
}

static int _fm_bis_mount(fm_ctx_t *ctx, bool user_part, bool emummc)
{
	int res = 1;
	u32 emu_offset = 0;

	// Reset list head in case of a previous parse. Ensures free below is safe.
	list_init(&ctx->bis_gpt);

	if (emummc)
	{
		// BIS reads go through the SD image offset or part files. No eMMC init needed.
		if (!ctx->emummc_raw_available && !ctx->emummc_file_available)
			goto out;
		emu_offset = ctx->emummc_sector;
	}
	else
	{
		if (emmc_initialize(false))
			goto out;
	}

	if (!ctx->bis_keys_derived)
	{
		if (hos_bis_keygen())
			goto out;
		ctx->bis_keys_derived = true;
	}

	if (emummc)
	{
		if (ctx->emummc_raw_available)
		{
			if (_fm_emummc_gpt_parse(&ctx->bis_gpt, emu_offset))
				goto out;
		}
		else if (_fm_emummc_file_gpt_parse(&ctx->bis_gpt))
			goto out;
	}
	else
	{
		emmc_set_partition(EMMC_GPP);
		emmc_gpt_parse(&ctx->bis_gpt);
	}

	emmc_part_t *part = emmc_part_find(&ctx->bis_gpt, user_part ? "USER" : "SYSTEM");
	if (!part)
		goto out;

	// Initialize BIS for eMMC/emuMMC and set its size for FatFS.
	if (emummc && !ctx->emummc_raw_available && ctx->emummc_file_available)
		nx_emmc_bis_init_file(part, true, ctx->emummc_file_dir, ctx->emummc_file_part_size);
	else
		nx_emmc_bis_init(part, true, emu_offset);
	u32 part_sectors = part->lba_end - part->lba_start + 1;
	disk_set_info(DRIVE_BIS, SET_SECTOR_COUNT, &part_sectors);

	// Mount the partition. Write protected by default.
	if (f_mount(&ctx->bis_fs, "bis:", 1))
	{
		nx_emmc_bis_end();
		goto out;
	}

	ctx->bis_mounted = true;
	ctx->bis_is_emummc = emummc;
	ctx->bis_user_part = user_part;
	ctx->src_name = emummc ? "emuMMC" : "eMMC";
	res = 0;

out:
	if (res)
	{
		emmc_gpt_free(&ctx->bis_gpt);
		list_init(&ctx->bis_gpt);
		if (!emummc)
			emmc_end();
	}

	return res;
}

static void _fm_error_box(const char *text)
{
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\251", "\222OK", "\251", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 6);

	lv_mbox_set_text(mbox, text);

	lv_mbox_add_btns(mbox, mbox_btn_map, nyx_mbox_action); // Important. After set_text.
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);
}

static void _fm_path_join(char *dst, const char *dir, const char *name)
{
	u32 len = strlen(dir);

	// Reject paths that would overflow.
	if (len + strlen(name) + 2 >= FM_PATH_MAX)
	{
		dst[0] = 0;
		return;
	}

	strcpy(dst, dir);
	if (len && dst[len - 1] != '/')
		dst[len++] = '/';
	strcpy(&dst[len], name);
}

static lv_obj_t *_fm_progress_box_create(const char *title, lv_obj_t **bar)
{
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 5);

	lv_mbox_set_text(mbox, title);

	*bar = lv_bar_create(mbox, NULL);
	lv_obj_set_size(*bar, LV_HOR_RES * 4 / 9, LV_DPI / 3);
	lv_bar_set_value(*bar, 0);

	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	return dark_bg;
}

static void _fm_progress_box_close(lv_obj_t *dark_bg)
{
	lv_obj_del(dark_bg); // Deletes children also (mbox, bar).
	manual_system_maintenance(true);
}

static void _fm_progress_update(lv_obj_t *bar, u32 done, u32 total)
{
	u32 pct = (u32)((u64)done * 100u / total);
	lv_bar_set_value(bar, pct);
	manual_system_maintenance(false);
}

static bool _fm_copy_file(const char *src, const char *dst, lv_obj_t *bar)
{
	FIL src_fp;
	FIL dst_fp;
	FRESULT res;

	res = f_open(&src_fp, src, FA_READ);
	if (res != FR_OK)
		return false;

	res = f_open(&dst_fp, dst, FA_CREATE_ALWAYS | FA_WRITE);
	if (res != FR_OK)
	{
		f_close(&src_fp);
		return false;
	}

	u32 size_left = f_size(&src_fp);
	u32 total = size_left ? size_left : 1;
	u32 done = 0;

	bool result = true;
	while (size_left)
	{
		u32 chunk = MIN(size_left, FM_BUF_SIZE);
		UINT read_bytes = 0;
		UINT written_bytes = 0;

		if (f_read(&src_fp, (void *)FM_BUF, chunk, &read_bytes) || !read_bytes)
		{
			result = false;
			break;
		}
		if (f_write(&dst_fp, (void *)FM_BUF, read_bytes, &written_bytes) || written_bytes != read_bytes)
		{
			result = false;
			break;
		}

		size_left -= chunk;
		done += chunk;
		if (bar)
			_fm_progress_update(bar, done, total);
	}

	f_close(&dst_fp);
	f_close(&src_fp);

	// Remove partial file on error.
	if (!result)
		f_unlink(dst);

	return result;
}

static bool _fm_copy_dir(const char *src, const char *dst, lv_obj_t *bar)
{
	DIR dir;
	FILINFO fno;
	FRESULT res;

	res = f_opendir(&dir, src);
	if (res != FR_OK)
		return false;

	// Create destination folder. Skip if it already exists.
	if (f_mkdir(dst) && f_stat(dst, NULL))
	{
		f_closedir(&dir);
		return false;
	}

	char *src_path = (char *)malloc(FM_PATH_MAX);
	char *dst_path = (char *)malloc(FM_PATH_MAX);
	u32 src_len = strlen(src);
	u32 dst_len = strlen(dst);

	bool result = true;
	for (;;)
	{
		res = f_readdir(&dir, &fno);
		if (res != FR_OK || !fno.fname[0])
			break;

		// Skip dot entries.
		if (fno.fname[0] == '.')
			continue;

		strcpy(src_path, src);
		if (src_len && src_path[src_len - 1] != '/')
			strcat(src_path, "/");
		strcat(src_path, fno.fname);

		strcpy(dst_path, dst);
		if (dst_len && dst_path[dst_len - 1] != '/')
			strcat(dst_path, "/");
		strcat(dst_path, fno.fname);

		if (fno.fattrib & AM_DIR)
			result = _fm_copy_dir(src_path, dst_path, bar);
		else
			result = _fm_copy_file(src_path, dst_path, bar);

		if (!result)
			break;
	}

	f_closedir(&dir);
	free(src_path);
	free(dst_path);

	return result;
}

static bool _fm_delete_dir(const char *path)
{
	DIR dir;
	FILINFO fno;
	FRESULT res;

	res = f_opendir(&dir, path);
	if (res != FR_OK)
		return false;

	char *entry_path = (char *)malloc(FM_PATH_MAX);
	u32 len = strlen(path);

	bool result = true;
	for (;;)
	{
		res = f_readdir(&dir, &fno);
		if (res != FR_OK || !fno.fname[0])
			break;

		// Skip dot entries.
		if (fno.fname[0] == '.')
			continue;

		strcpy(entry_path, path);
		if (len && entry_path[len - 1] != '/')
			strcat(entry_path, "/");
		strcat(entry_path, fno.fname);

		if (fno.fattrib & AM_DIR)
			result = _fm_delete_dir(entry_path);
		else
			result = !f_unlink(entry_path);

		if (!result)
			break;
	}

	f_closedir(&dir);
	free(entry_path);

	if (!result)
		return false;

	return !f_unlink(path);
}

static lv_res_t _fm_action_up(lv_obj_t *btn)
{
	fm_ctx_t *ctx = &fm_ctx;

	// Drive root: go back to drive selection. Keep BIS mounted for cross-volume copy.
	if (!strcmp(ctx->path, "sd:/") || !strcmp(ctx->path, "bis:/"))
		ctx->path[0] = 0;
	else
	{
		int len = strlen(ctx->path);
		if (len && ctx->path[len - 1] == '/')
			ctx->path[len - 1] = 0;
		char *slash = strrchr(ctx->path, '/');
		if (slash)
			slash[1] = 0; // Keep drive prefix and trailing slash.
		else
			ctx->path[0] = 0;
	}

	_fm_list_dir(ctx);

	return LV_RES_OK;
}

static lv_res_t _fm_action_refresh(lv_obj_t *btn)
{
	_fm_list_dir(&fm_ctx);

	return LV_RES_OK;
}

static lv_res_t _fm_action_new_folder(lv_obj_t *btn)
{
	fm_ctx_t *ctx = &fm_ctx;
	char *path = (char *)malloc(FM_PATH_MAX);

	// Auto-name the folder, avoiding collisions.
	for (u32 i = 0; i < 1000; i++)
	{
		if (i)
			s_printf(path, "%s/NewFolder%d", ctx->path, i);
		else
			s_printf(path, "%s/NewFolder", ctx->path);

		if (f_stat(path, NULL))
			break;
	}

	if (f_mkdir(path))
		_fm_error_box("#FFDD00 Failed to create folder!#");

	free(path);
	_fm_list_dir(ctx);

	return LV_RES_OK;
}

static lv_res_t _fm_action_paste(lv_obj_t *btn)
{
	fm_ctx_t *ctx = &fm_ctx;

	char *name = strrchr(ctx->clip_path, '/');
	if (name)
		name++;
	else
		name = ctx->clip_path;

	char *src = (char *)malloc(FM_PATH_MAX);
	char *dst = (char *)malloc(FM_PATH_MAX);

	strcpy(src, ctx->clip_path);
	_fm_path_join(dst, ctx->path, name);

	// eMMC/emuMMC partitions are browsed read-only.
	if (!strncmp(ctx->path, "bis:", 4))
	{
		_fm_error_box("#FFDD00 eMMC/emuMMC is read-only!#");
		goto out;
	}

	// Do not allow pasting a folder into itself.
	u32 clip_len = strlen(ctx->clip_path);
	if (ctx->clip_is_dir && dst[0] &&
		!strncmp(dst, ctx->clip_path, clip_len) &&
		(dst[clip_len] == '/' || dst[clip_len] == 0))
	{
		_fm_error_box("#FFDD00 Can't paste a folder into itself!#");
		goto out;
	}

	// Do not allow pasting a file onto itself.
	if (!ctx->clip_is_dir && !strcmp(src, dst))
	{
		_fm_error_box("#FFDD00 Source and destination are the same!#");
		goto out;
	}

	lv_obj_t *bar = NULL;
	lv_obj_t *dark_bg = _fm_progress_box_create("#00DDFF Copying...#\n ", &bar);
	manual_system_maintenance(true);

	if (ctx->clip_is_dir)
	{
		if (!_fm_copy_dir(src, dst, bar))
			_fm_error_box("#FFDD00 Failed to copy folder!#");
	}
	else
	{
		if (!_fm_copy_file(src, dst, bar))
			_fm_error_box("#FFDD00 Failed to copy file!#");
	}

	_fm_progress_box_close(dark_bg);

out:
	free(src);
	free(dst);
	_fm_list_dir(ctx);

	return LV_RES_OK;
}

static void _fm_delete_confirm(void)
{
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\222Delete", "\222Cancel", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 5);

	char *txt_buf = (char *)malloc(SZ_4K);
	s_printf(txt_buf,
			 "#FF8000 Delete %s#\n\n"
			 "%s\n\n"
			 "#FF3C28 This operation can not be undone!#",
			 fm_menu_is_dir ? "folder" : "file", fm_menu_path);
	lv_mbox_set_text(mbox, txt_buf);
	free(txt_buf);

	lv_mbox_add_btns(mbox, mbox_btn_map, _fm_delete_confirm_action);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);
}

static lv_res_t _fm_delete_confirm_action(lv_obj_t *btns, const char *txt)
{
	lv_obj_t *mbox = lv_mbox_get_from_btn(btns);
	lv_obj_t *dark_bg = lv_obj_get_parent(mbox);

	if (!strcmp(txt, "Delete"))
	{
		lv_obj_del(dark_bg);

		bool result = fm_menu_is_dir ? _fm_delete_dir(fm_menu_path) : !f_unlink(fm_menu_path);
		if (!result)
			_fm_error_box("#FFDD00 Failed to delete!#");

		_fm_list_dir(&fm_ctx);

		return LV_RES_INV;
	}

	lv_obj_del(dark_bg);

	return LV_RES_INV;
}

static void _fm_file_menu(const char *path, bool is_dir)
{
	strcpy(fm_menu_path, path);
	fm_menu_is_dir = is_dir;

	bool bis = !strncmp(fm_ctx.path, "bis:", 4);

	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\222Copy", "\222Delete", "\222Cancel", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 6);

	char *txt_buf = (char *)malloc(SZ_4K);
	if (strlen(path) > 96) // Truncate long paths, keeping the tail visible.
		s_printf(txt_buf, "%s\n\n...\n%s", is_dir ? "#FF8000 Folder#" : "#FF8000 File#", path + strlen(path) - 92);
	else
		s_printf(txt_buf, "%s\n\n%s", is_dir ? "#FF8000 Folder#" : "#FF8000 File#", path);

	if (bis)
		strcat(txt_buf, "\n\n#C7EA46 Read-only. Copy to paste into the SD card.#");
	else if (is_dir)
		strcat(txt_buf, "\n\n#C7EA46 Copy pastes the whole folder. Long press to open menu.#");
	else
		strcat(txt_buf, "\n\n#C7EA46 Long press a folder to open its menu.#");

	lv_mbox_set_text(mbox, txt_buf);
	free(txt_buf);

	if (bis)
	{
		// Copy only. The BIS volume is read-only.
		static const char *mbox_btn_map_bis[] = { "\222Copy", "\222Close", "" };
		lv_mbox_add_btns(mbox, mbox_btn_map_bis, _fm_file_menu_action);
	}
	else
		lv_mbox_add_btns(mbox, mbox_btn_map, _fm_file_menu_action);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);
}

static lv_res_t _fm_file_menu_action(lv_obj_t *btns, const char *txt)
{
	lv_obj_t *mbox = lv_mbox_get_from_btn(btns);
	lv_obj_t *dark_bg = lv_obj_get_parent(mbox);

	if (!strcmp(txt, "Copy"))
	{
		strcpy(fm_ctx.clip_path, fm_menu_path);
		fm_ctx.clip_is_dir = fm_menu_is_dir;
	}
	else if (!strcmp(txt, "Delete"))
	{
		lv_obj_del(dark_bg);
		_fm_delete_confirm();

		return LV_RES_INV;
	}

	lv_obj_del(dark_bg);
	_fm_list_dir(&fm_ctx);

	return LV_RES_INV;
}

static lv_res_t _fm_action_entry(lv_obj_t *btn)
{
	fm_ctx_t *ctx = &fm_ctx;
	lv_obj_t *label = lv_list_get_btn_label(btn);
	const char *name = lv_label_get_text(label);

	// Drive selection.
	if (!ctx->path[0])
	{
		void *drive = lv_obj_get_free_ptr(btn);
		bool user_part = drive == FM_DRIVE_USER || drive == FM_DRIVE_EMU_USER;
		bool emummc = drive == FM_DRIVE_EMU_SYS || drive == FM_DRIVE_EMU_USER;

		if (drive)
		{
			// Switching partitions unmounts the previous one. Same one keeps it mounted.
			if (ctx->bis_mounted &&
				(ctx->bis_is_emummc != emummc || ctx->bis_user_part != user_part))
				_fm_bis_unmount(ctx);

			if (!ctx->bis_mounted && _fm_bis_mount(ctx, user_part, emummc))
			{
				_fm_error_box("#FFDD00 Failed to mount partition!#");
				return LV_RES_OK;
			}
			strcpy(ctx->path, "bis:/");
		}
		else
			strcpy(ctx->path, "sd:/");

		_fm_list_dir(ctx);

		return LV_RES_OK;
	}

	char *path = (char *)malloc(FM_PATH_MAX);
	_fm_path_join(path, ctx->path, name);

	FILINFO fno;
	bool is_dir = path[0] && !f_stat(path, &fno) && (fno.fattrib & AM_DIR);

	// Tap on folder enters it.
	if (is_dir)
	{
		strcat(path, "/");
		strcpy(ctx->path, path);
		free(path);
		_fm_list_dir(ctx);

		return LV_RES_OK;
	}

	// Tap on file opens the file menu.
	if (path[0])
		_fm_file_menu(path, false);
	free(path);

	return LV_RES_OK;
}

static lv_res_t _fm_action_entry_long(lv_obj_t *btn)
{
	fm_ctx_t *ctx = &fm_ctx;
	lv_obj_t *label = lv_list_get_btn_label(btn);
	const char *name = lv_label_get_text(label);

	// No menu for root entries.
	if (!ctx->path[0])
		return LV_RES_OK;

	char *path = (char *)malloc(FM_PATH_MAX);
	_fm_path_join(path, ctx->path, name);

	FILINFO fno;
	bool is_dir = path[0] && !f_stat(path, &fno) && (fno.fattrib & AM_DIR);

	if (path[0])
		_fm_file_menu(path, is_dir);
	free(path);

	return LV_RES_OK;
}

static void _fm_list_dir(fm_ctx_t *ctx)
{
	lv_list_clean(ctx->list);

	lv_list_add(ctx->list, NULL, SYMBOL_UP" ..", _fm_action_up);

	if (!ctx->path[0]) // Drive selection.
	{
		lv_obj_t *entry = lv_list_add(ctx->list, NULL, SYMBOL_SD" SD Card", _fm_action_entry);
		lv_obj_set_free_ptr(entry, NULL); // No tag needed for SD.

		entry = lv_list_add(ctx->list, NULL, SYMBOL_CHIP" eMMC SYSTEM", _fm_action_entry);
		lv_obj_set_free_ptr(entry, FM_DRIVE_SYSTEM);

		entry = lv_list_add(ctx->list, NULL, SYMBOL_CHIP" eMMC USER", _fm_action_entry);
		lv_obj_set_free_ptr(entry, FM_DRIVE_USER);

		// Raw partition or file based emuMMC can be browsed.
		if (ctx->emummc_raw_available || ctx->emummc_file_available)
		{
			entry = lv_list_add(ctx->list, NULL, SYMBOL_CHIP" emuMMC SYSTEM", _fm_action_entry);
			lv_obj_set_free_ptr(entry, FM_DRIVE_EMU_SYS);

			entry = lv_list_add(ctx->list, NULL, SYMBOL_CHIP" emuMMC USER", _fm_action_entry);
			lv_obj_set_free_ptr(entry, FM_DRIVE_EMU_USER);
		}
	}
	else
	{
		lv_list_add(ctx->list, NULL, SYMBOL_REFRESH" Refresh", _fm_action_refresh);

		// eMMC/emuMMC partitions are browsed read-only.
		if (!strncmp(ctx->path, "bis:", 4))
		{
			char *txt_buf = (char *)malloc(SZ_4K);
			s_printf(txt_buf, SYMBOL_CLOSE" Close %s", ctx->src_name);
			lv_list_add(ctx->list, NULL, txt_buf, _fm_action_close_emmc);
			free(txt_buf);
		}
		else
		{
			lv_list_add(ctx->list, NULL, SYMBOL_DIRECTORY" New Folder", _fm_action_new_folder);

			if (ctx->clip_path[0])
				lv_list_add(ctx->list, NULL, SYMBOL_COPY" Paste", _fm_action_paste);
		}

		dirlist_t *dir = dirlist(ctx->path, NULL, DIR_SHOW_HIDDEN | DIR_SHOW_DIRS);
		if (dir)
		{
			for (u32 i = 0; dir->name[i]; i++)
			{
				lv_obj_t *entry = lv_list_add(ctx->list, NULL, dir->name[i], _fm_action_entry);
				lv_btn_set_action(entry, LV_BTN_ACTION_LONG_PR, _fm_action_entry_long);
			}
			free(dir);
		}
	}

	// Update path label.
	char *txt_buf = (char *)malloc(SZ_4K);
	if (ctx->path[0] && ctx->bis_mounted && ctx->src_name)
		s_printf(txt_buf, "Path: #C7EA46 [%s] %s#", ctx->src_name, ctx->path);
	else
		s_printf(txt_buf, "Path: #C7EA46 %s#", ctx->path);
	lv_label_set_text(ctx->path_label, txt_buf);
	free(txt_buf);

	manual_system_maintenance(true);
}

static lv_res_t _fm_action_close_emmc(lv_obj_t *btn)
{
	fm_ctx_t *ctx = &fm_ctx;

	ctx->path[0] = 0;
	_fm_bis_unmount(ctx);

	_fm_list_dir(ctx);

	return LV_RES_OK;
}

static lv_res_t _fm_action_close(lv_obj_t *btn)
{
	fm_ctx.clip_path[0] = 0;
	fm_ctx.win = NULL;

	if (fm_ctx.bis_mounted)
		_fm_bis_unmount(&fm_ctx);

	// Clear BIS keys if they were derived.
	if (fm_ctx.bis_keys_derived)
	{
		hos_bis_keys_clear();
		fm_ctx.bis_keys_derived = false;
	}

	sd_unmount();

	return nyx_win_close_action(btn);
}

lv_res_t create_window_file_manager_tool(lv_obj_t *btn)
{
	if (sd_mount())
	{
		_fm_error_box("#FFDD00 Failed to init SD!#");
		return LV_RES_OK;
	}

	free(fm_ctx.emummc_file_dir);

	memset(&fm_ctx, 0, sizeof(fm_ctx));
	list_init(&fm_ctx.bis_gpt);

	// Detect raw partition or file based emuMMC from emuMMC config.
	if (!h_cfg.emummc_force_disable)
	{
		emummc_cfg_t emu_info;
		load_emummc_cfg(&emu_info);

		if (emu_info.enabled && emu_info.sector)
		{
			fm_ctx.emummc_raw_available = true;
			fm_ctx.emummc_sector = emu_info.sector;
		}
		else if (emu_info.enabled && emu_info.path)
		{
			// File based. Part files live under <path>/eMMC.
			u32 len = strlen(emu_info.path);
			fm_ctx.emummc_file_dir = malloc(len + 6);
			if (fm_ctx.emummc_file_dir)
			{
				strcpy(fm_ctx.emummc_file_dir, emu_info.path);
				if (len && fm_ctx.emummc_file_dir[len - 1] != '/')
					strcat(fm_ctx.emummc_file_dir, "/");
				strcat(fm_ctx.emummc_file_dir, "eMMC");

				char *path = malloc(len + 12);
				if (path)
				{
					strcpy(path, fm_ctx.emummc_file_dir);
					strcat(path, "/00");

					// Part file size defines the emuMMC sector mapping.
					FILINFO fno;
					if (!f_stat(path, &fno) && (fno.fsize >> 9))
					{
						fm_ctx.emummc_file_available = true;
						fm_ctx.emummc_file_part_size = fno.fsize >> 9;
					}

					free(path);
				}
			}
			free(emu_info.path);
		}

		free(emu_info.nintendo_path);
	}

	lv_obj_t *win = nyx_create_standard_window(SYMBOL_DRIVE" File Manager", _fm_action_close);
	fm_ctx.win = win;

	// Create path label container.
	lv_obj_t *path_cont = lv_cont_create(win, NULL);
	lv_obj_set_size(path_cont, LV_HOR_RES * 10 / 11, LV_DPI / 2);

	fm_ctx.path_label = lv_label_create(path_cont, NULL);
	lv_label_set_recolor(fm_ctx.path_label, true);
	lv_label_set_long_mode(fm_ctx.path_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(fm_ctx.path_label, LV_HOR_RES * 10 / 11 - LV_DPI / 2);
	lv_obj_align(fm_ctx.path_label, NULL, LV_ALIGN_IN_LEFT_MID, LV_DPI / 8, 0);

	// Create file list.
	fm_ctx.list = lv_list_create(win, NULL);
	lv_obj_set_size(fm_ctx.list, LV_HOR_RES * 10 / 11, LV_VER_RES - (LV_DPI * 11 / 7) - LV_DPI * 5 / 7);
	lv_obj_align(fm_ctx.list, path_cont, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 8);

	_fm_list_dir(&fm_ctx);

	return LV_RES_OK;
}
