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

// A single listed directory entry. Backing storage for the list's free_ptr tags.
typedef struct _fm_entry_t
{
	char name[256];
	u64  size;
	u16  fdate;
	u16  ftime;
	bool is_dir;
} fm_entry_t;

typedef struct _fm_ctx_t
{
	lv_obj_t *win;
	lv_obj_t *path_label;
	lv_obj_t *list;
	char path[FM_PATH_MAX];
	char clip_path[FM_PATH_MAX];
	bool clip_is_dir;
	bool clip_is_cut; // false: Copy, true: Cut (same volume move only).
	// Current directory listing. Freed/reallocated on every _fm_list_dir().
	fm_entry_t *entries;
	u32 entry_count;
	// eMMC/emuMMC BIS state.
	bool bis_mounted;
	bool bis_is_emummc;
	bool bis_user_part;
	bool bis_keys_derived;
	bool bis_write_unlocked;
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

// Context for a pending paste that needs an overwrite confirmation first.
static char fm_paste_src[FM_PATH_MAX];
static char fm_paste_dst[FM_PATH_MAX];
static bool fm_paste_dst_is_dir;

// Context for the generic text input modal (keyboard + text area).
typedef void (*fm_text_input_cb_t)(const char *text);
static lv_obj_t *fm_ti_dark_bg;
static lv_obj_t *fm_ti_ta;
static fm_text_input_cb_t fm_ti_callback;

static void _fm_list_dir(fm_ctx_t *ctx);
static lv_res_t _fm_delete_confirm_action(lv_obj_t *btns, const char *txt);
static lv_res_t _fm_file_menu_action(lv_obj_t *btns, const char *txt);
static lv_res_t _fm_action_close_emmc(lv_obj_t *btn);
static bool _fm_write_access_request();
static void _fm_text_input_open(const char *title, const char *prefill, fm_text_input_cb_t cb);

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

	// Re-enable write protection for the next mount.
	bool allow_writes = false;
	disk_set_info(DRIVE_BIS, SET_WRITE_PROTECT, &allow_writes);
	ctx->bis_write_unlocked = false;

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

// Generic text input modal (on-screen keyboard + text area). Single instance, so static is safe.
static lv_res_t _fm_text_input_ok(lv_obj_t *kb)
{
	// Copy out before deleting the modal (and the text area with it).
	char text[256];
	strncpy(text, lv_ta_get_text(fm_ti_ta), sizeof(text) - 1);
	text[sizeof(text) - 1] = 0;

	lv_obj_del(fm_ti_dark_bg);
	fm_ti_dark_bg = NULL;
	fm_ti_ta = NULL;

	if (fm_ti_callback)
		fm_ti_callback(text);

	return LV_RES_INV;
}

static lv_res_t _fm_text_input_cancel(lv_obj_t *kb)
{
	lv_obj_del(fm_ti_dark_bg);
	fm_ti_dark_bg = NULL;
	fm_ti_ta = NULL;

	return LV_RES_INV;
}

static void _fm_text_input_open(const char *title, const char *prefill, fm_text_input_cb_t cb)
{
	fm_ti_callback = cb;

	fm_ti_dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(fm_ti_dark_bg, &mbox_darken);
	lv_obj_set_size(fm_ti_dark_bg, LV_HOR_RES, LV_VER_RES);

	lv_obj_t *title_label = lv_label_create(fm_ti_dark_bg, NULL);
	lv_label_set_recolor(title_label, true);
	lv_label_set_text(title_label, title);
	lv_obj_align(title_label, NULL, LV_ALIGN_IN_TOP_MID, 0, LV_DPI / 3);

	fm_ti_ta = lv_ta_create(fm_ti_dark_bg, NULL);
	lv_ta_set_one_line(fm_ti_ta, true);
	lv_ta_set_max_length(fm_ti_ta, 254);
	if (prefill)
		lv_ta_set_text(fm_ti_ta, prefill);
	lv_ta_set_cursor_pos(fm_ti_ta, LV_TA_CURSOR_LAST);
	lv_obj_set_width(fm_ti_ta, LV_HOR_RES * 7 / 9);
	lv_obj_align(fm_ti_ta, title_label, LV_ALIGN_OUT_BOTTOM_MID, 0, LV_DPI / 4);

	lv_obj_t *kb = lv_kb_create(fm_ti_dark_bg, NULL);
	lv_kb_set_ta(kb, fm_ti_ta);
	lv_kb_set_ok_action(kb, _fm_text_input_ok);
	lv_kb_set_hide_action(kb, _fm_text_input_cancel);
	lv_obj_set_width(kb, LV_HOR_RES * 8 / 9);
	lv_obj_set_height(kb, LV_VER_RES / 2);
	lv_obj_align(kb, NULL, LV_ALIGN_IN_BOTTOM_MID, 0, -LV_DPI / 6);

	lv_obj_set_top(fm_ti_dark_bg, true);
}

// Format a byte size as a short human readable string. No float support, so compute one decimal by hand.
static void _fm_format_size(char *buf, u64 size)
{
	if (size < SZ_1K)
		s_printf(buf, "%d B", (u32)size);
	else if (size < SZ_1M)
		s_printf(buf, "%d KiB", (u32)(size / SZ_1K));
	else if (size < SZ_1G)
		s_printf(buf, "%d.%d MiB", (u32)(size / SZ_1M), (u32)((size * 10 / SZ_1M) % 10));
	else
		s_printf(buf, "%d.%d GiB", (u32)(size / SZ_1G), (u32)((size * 10 / SZ_1G) % 10));
}

// Format a FatFs date/time pair (fdate/ftime bitfields) as "YYYY-MM-DD HH:MM".
static void _fm_format_date(char *buf, u16 fdate, u16 ftime)
{
	s_printf(buf, "%d-%02d-%02d %02d:%02d",
			 ((fdate >> 9) & 0x7F) + 1980, (fdate >> 5) & 0x0F, fdate & 0x1F,
			 (ftime >> 11) & 0x1F, (ftime >> 5) & 0x3F);
}

// Append the current volume's free space to a path label buffer.
static void _fm_format_free_space(char *buf, const char *path)
{
	DWORD free_clst;
	FATFS *fs;

	if (f_getfree(path, &free_clst, &fs))
	{
		buf[0] = 0;
		return;
	}

	char size_buf[32];
	_fm_format_size(size_buf, (u64)free_clst * fs->csize * 512);
	s_printf(buf, "   Free: %s", size_buf);
}

// Sort folders first, then alphabetically (case-insensitive) within each group.
static int _fm_entry_cmp(const void *a, const void *b)
{
	const fm_entry_t *ea = (const fm_entry_t *)a;
	const fm_entry_t *eb = (const fm_entry_t *)b;

	if (ea->is_dir != eb->is_dir)
		return eb->is_dir - ea->is_dir;

	return strcasecmp(ea->name, eb->name);
}

// Read a directory into a sorted array of entries. Replaces dirlist(), which can only
// return files or folders exclusively, never both, and carries no size/date/type info.
static fm_entry_t *_fm_read_dir(const char *path, u32 *out_count)
{
	DIR dir;
	FILINFO fno;

	*out_count = 0;

	if (f_opendir(&dir, path) != FR_OK)
		return NULL;

	u32 count = 0;
	for (;;)
	{
		if (f_readdir(&dir, &fno) != FR_OK || !fno.fname[0])
			break;
		if (fno.fname[0] == '.')
			continue;
		count++;
	}
	f_closedir(&dir);

	if (!count)
		return NULL;

	fm_entry_t *entries = (fm_entry_t *)calloc(count, sizeof(fm_entry_t));
	if (!entries)
		return NULL;

	if (f_opendir(&dir, path) != FR_OK)
	{
		free(entries);
		return NULL;
	}

	u32 i = 0;
	while (i < count)
	{
		if (f_readdir(&dir, &fno) != FR_OK || !fno.fname[0])
			break;
		if (fno.fname[0] == '.')
			continue;

		strcpy(entries[i].name, fno.fname);
		entries[i].size    = fno.fsize;
		entries[i].fdate   = fno.fdate;
		entries[i].ftime   = fno.ftime;
		entries[i].is_dir  = !!(fno.fattrib & AM_DIR);
		i++;
	}
	f_closedir(&dir);

	*out_count = i;
	qsort(entries, i, sizeof(fm_entry_t), _fm_entry_cmp);

	return entries;
}

// Reject characters FAT/exFAT forbid in a file name. f_rename()/f_mkdir() are still
// the final authority; this is just an early, friendlier error message.
static bool _fm_name_is_valid(const char *name)
{
	return name[0] && !strpbrk(name, "\"*/:<>?\\|");
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

static void _fm_new_folder_cb(const char *text)
{
	fm_ctx_t *ctx = &fm_ctx;

	if (!text[0])
		return; // Cancelled/empty. Leave the listing as is.

	if (!_fm_name_is_valid(text))
	{
		_fm_error_box("#FFDD00 Name has invalid characters!#");
		return;
	}

	char path[FM_PATH_MAX];
	_fm_path_join(path, ctx->path, text);

	if (!path[0])
		_fm_error_box("#FFDD00 Name is too long!#");
	else if (!f_stat(path, NULL))
		_fm_error_box("#FFDD00 That name is already used!#");
	else if (f_mkdir(path))
		_fm_error_box("#FFDD00 Failed to create folder!#");

	_fm_list_dir(ctx);
}

static lv_res_t _fm_action_new_folder(lv_obj_t *btn)
{
	_fm_text_input_open("#FF8000 New folder name#", "NewFolder", _fm_new_folder_cb);

	return LV_RES_OK;
}

// True if src and dst name the same mounted volume (text before the first ':').
static bool _fm_same_volume(const char *src, const char *dst)
{
	const char *src_colon = strchr(src, ':');
	const char *dst_colon = strchr(dst, ':');

	if (!src_colon || !dst_colon || (src_colon - src) != (dst_colon - dst))
		return false;

	return !strncmp(src, dst, src_colon - src);
}

// Executes the actual paste. dst_is_dir/dst_existed describe what, if anything, is
// already at dst (the caller either confirmed overwriting it, or there was nothing there).
static void _fm_do_paste(fm_ctx_t *ctx, const char *src, const char *dst, bool dst_is_dir, bool dst_existed)
{
	// Cut within the same volume is a plain rename: instant, and no temp duplicate on disk.
	if (ctx->clip_is_cut && _fm_same_volume(src, dst))
	{
		bool result = true;

		if (dst_existed)
			result = dst_is_dir ? _fm_delete_dir(dst) : !f_unlink(dst);

		if (result && f_rename(src, dst))
			result = false;

		if (!result)
			_fm_error_box("#FFDD00 Failed to move!#");
		else
			ctx->clip_path[0] = 0; // Source is gone. Copy's clipboard is left for repeated pasting.

		_fm_list_dir(ctx);
		return;
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
	_fm_list_dir(ctx);
}

static lv_res_t _fm_overwrite_confirm_action(lv_obj_t *btns, const char *txt)
{
	lv_obj_t *mbox = lv_mbox_get_from_btn(btns);
	lv_obj_t *dark_bg = lv_obj_get_parent(mbox);
	lv_obj_del(dark_bg);

	if (!strcmp(txt, "Replace"))
		_fm_do_paste(&fm_ctx, fm_paste_src, fm_paste_dst, fm_paste_dst_is_dir, true);
	else
		_fm_list_dir(&fm_ctx);

	return LV_RES_INV;
}

static void _fm_overwrite_confirm(void)
{
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\222Replace", "\222Cancel", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 5);

	char *txt_buf = (char *)malloc(SZ_4K);
	s_printf(txt_buf,
			 "#FF8000 Replace existing %s?#\n\n"
			 "%s\n\n"
			 "#FF3C28 This operation can not be undone!#",
			 fm_paste_dst_is_dir ? "folder" : "file", fm_paste_dst);
	lv_mbox_set_text(mbox, txt_buf);
	free(txt_buf);

	lv_mbox_add_btns(mbox, mbox_btn_map, _fm_overwrite_confirm_action);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);
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

	// eMMC/emuMMC writes are gated behind an explicit unlock.
	if (!strncmp(ctx->path, "bis:", 4) && !_fm_write_access_request())
		goto out;

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
	if (!strcmp(src, dst))
	{
		_fm_error_box("#FFDD00 Source and destination are the same!#");
		goto out;
	}

	// Cut can only be fulfilled as an actual move within the same volume. A cross volume
	// move would need a copy plus a source delete, which is a much riskier operation to
	// leave half-finished on eMMC/emuMMC, so it's not supported here.
	if (ctx->clip_is_cut && !_fm_same_volume(src, dst))
	{
		_fm_error_box("#FFDD00 Can't move between SD and eMMC/emuMMC. Copy instead.#");
		goto out;
	}

	FILINFO dst_fno;
	bool dst_exists = !f_stat(dst, &dst_fno);
	bool dst_is_dir = dst_exists && (dst_fno.fattrib & AM_DIR);
	bool move_now = ctx->clip_is_cut && _fm_same_volume(src, dst);

	if (dst_exists)
	{
		// Folder-into-folder copy keeps its existing silent merge behavior.
		if (!move_now && ctx->clip_is_dir && dst_is_dir)
		{
			_fm_do_paste(ctx, src, dst, true, true);
			goto out;
		}

		if (ctx->clip_is_dir != dst_is_dir)
		{
			_fm_error_box(dst_is_dir ?
				"#FFDD00 Can't replace a folder with a file!#" :
				"#FFDD00 Can't replace a file with a folder!#");
			goto out;
		}

		// A move can't merge folders the way a copy can; renaming over one fails outright.
		if (move_now && dst_is_dir)
		{
			_fm_error_box("#FFDD00 A folder with that name already exists here!#");
			goto out;
		}

		// Something will be overwritten. Confirm first.
		strcpy(fm_paste_src, src);
		strcpy(fm_paste_dst, dst);
		fm_paste_dst_is_dir = dst_is_dir;
		_fm_overwrite_confirm();
		goto out;
	}

	_fm_do_paste(ctx, src, dst, false, false);

out:
	free(src);
	free(dst);

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

	static const char *mbox_btn_map[] = { "\222Copy", "\222Cut", "\222Rename", "\n", "\222Delete", "\222Cancel", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 6);

	char *txt_buf = (char *)malloc(SZ_4K);
	if (strlen(path) > 96) // Truncate long paths, keeping the tail visible.
		s_printf(txt_buf, "%s\n\n...\n%s", is_dir ? "#FF8000 Folder#" : "#FF8000 File#", path + strlen(path) - 92);
	else
		s_printf(txt_buf, "%s\n\n%s", is_dir ? "#FF8000 Folder#" : "#FF8000 File#", path);

	if (bis)
	{
		if (fm_ctx.bis_write_unlocked)
			strcat(txt_buf, "\n\n#FF8000 Write access unlocked! Careful.#");
		else
			strcat(txt_buf, "\n\n#C7EA46 Read-only. Copy to paste into the SD card.#");
	}
	else if (is_dir)
		strcat(txt_buf, "\n\n#C7EA46 Copy pastes the whole folder. Long press to open menu.#");
	else
		strcat(txt_buf, "\n\n#C7EA46 Long press a folder to open its menu.#");

	lv_mbox_set_text(mbox, txt_buf);
	free(txt_buf);

	if (bis && !fm_ctx.bis_write_unlocked)
	{
		// Clipboard staging only, until write access is unlocked. Cut just stages a future
		// move; the unlock is enforced again when the move actually happens, at paste time.
		static const char *mbox_btn_map_bis[] = { "\222Copy", "\222Cut", "\222Close", "" };
		lv_mbox_add_btns(mbox, mbox_btn_map_bis, _fm_file_menu_action);
	}
	else
		lv_mbox_add_btns(mbox, mbox_btn_map, _fm_file_menu_action);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);
}

static void _fm_rename_cb(const char *text)
{
	if (!text[0])
		return; // Cancelled/empty. Leave the listing as is.

	if (!_fm_name_is_valid(text))
	{
		_fm_error_box("#FFDD00 Name has invalid characters!#");
		return;
	}

	char new_path[FM_PATH_MAX];
	char *slash = strrchr(fm_menu_path, '/');
	u32 dir_len = slash ? (u32)(slash - fm_menu_path + 1) : 0;

	if (dir_len + strlen(text) + 1 >= FM_PATH_MAX)
	{
		_fm_error_box("#FFDD00 Name is too long!#");
		return;
	}

	memcpy(new_path, fm_menu_path, dir_len);
	strcpy(new_path + dir_len, text);

	if (!strcmp(new_path, fm_menu_path))
		return; // Unchanged.

	if (!f_stat(new_path, NULL))
	{
		_fm_error_box("#FFDD00 That name is already used!#");
		return;
	}

	if (f_rename(fm_menu_path, new_path))
		_fm_error_box("#FFDD00 Failed to rename!#");
	else if (!strcmp(fm_ctx.clip_path, fm_menu_path)) // Keep the clipboard pointed at the renamed item.
		strcpy(fm_ctx.clip_path, new_path);

	_fm_list_dir(&fm_ctx);
}

static lv_res_t _fm_file_menu_action(lv_obj_t *btns, const char *txt)
{
	lv_obj_t *mbox = lv_mbox_get_from_btn(btns);
	lv_obj_t *dark_bg = lv_obj_get_parent(mbox);

	if (!strcmp(txt, "Copy"))
	{
		strcpy(fm_ctx.clip_path, fm_menu_path);
		fm_ctx.clip_is_dir = fm_menu_is_dir;
		fm_ctx.clip_is_cut = false;
	}
	else if (!strcmp(txt, "Cut"))
	{
		strcpy(fm_ctx.clip_path, fm_menu_path);
		fm_ctx.clip_is_dir = fm_menu_is_dir;
		fm_ctx.clip_is_cut = true;
	}
	else if (!strcmp(txt, "Rename"))
	{
		lv_obj_del(dark_bg);

		char *base = strrchr(fm_menu_path, '/');
		base = base ? base + 1 : fm_menu_path;
		_fm_text_input_open(fm_menu_is_dir ? "#FF8000 Rename folder#" : "#FF8000 Rename file#", base, _fm_rename_cb);

		return LV_RES_INV;
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

	// Regular listing entries carry their backing fm_entry_t as the free_ptr tag.
	fm_entry_t *e = (fm_entry_t *)lv_obj_get_free_ptr(btn);
	if (!e)
		return LV_RES_OK;

	char *path = (char *)malloc(FM_PATH_MAX);
	_fm_path_join(path, ctx->path, e->name);

	// Tap on folder enters it.
	if (e->is_dir)
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

	// No menu for root entries.
	if (!ctx->path[0])
		return LV_RES_OK;

	fm_entry_t *e = (fm_entry_t *)lv_obj_get_free_ptr(btn);
	if (!e)
		return LV_RES_OK;

	char *path = (char *)malloc(FM_PATH_MAX);
	_fm_path_join(path, ctx->path, e->name);

	if (path[0])
		_fm_file_menu(path, e->is_dir);
	free(path);

	return LV_RES_OK;
}

static lv_res_t _fm_action_close_emmc(lv_obj_t *btn)
{
	fm_ctx_t *ctx = &fm_ctx;

	ctx->path[0] = 0;
	_fm_bis_unmount(ctx);

	_fm_list_dir(ctx);

	return LV_RES_OK;
}

static lv_res_t _fm_unlock_action(lv_obj_t *btns, const char *txt)
{
	lv_obj_t *mbox = lv_mbox_get_from_btn(btns);
	lv_obj_t *dark_bg = lv_obj_get_parent(mbox);

	if (!strcmp(txt, "Unlock"))
	{
		bool allow_writes = true;
		disk_set_info(DRIVE_BIS, SET_WRITE_PROTECT, &allow_writes);
		fm_ctx.bis_write_unlocked = true;
	}

	lv_obj_del(dark_bg);
	_fm_list_dir(&fm_ctx);

	return LV_RES_INV;
}

static bool _fm_write_access_request()
{
	// Already unlocked.
	if (fm_ctx.bis_write_unlocked)
		return true;

	// File based emuMMC stays read-only. It is user data, but on the SD card.
	if (fm_ctx.bis_is_emummc && fm_ctx.emummc_file_available)
	{
		_fm_error_box("#FFDD00 File based emuMMC stays read-only!#");
		return false;
	}

	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\222Unlock", "\222Cancel", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 5);

	char *txt_buf = (char *)malloc(SZ_4K);
	s_printf(txt_buf,
			 "#FF8000 Enable write access?#\n\n"
			 "Writes go straight to the %s partition.\n\n"
			 "#FF3C28 Deleting HOS files can brick the OS install!#",
			 fm_ctx.src_name);
	lv_mbox_set_text(mbox, txt_buf);
	free(txt_buf);

	lv_mbox_add_btns(mbox, mbox_btn_map, _fm_unlock_action);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	return false; // Menu action re-lists on dismiss.
}

static lv_res_t _fm_action_lock_emmc(lv_obj_t *btn)
{
	fm_ctx_t *ctx = &fm_ctx;

	// Restore write protection.
	bool allow_writes = false;
	disk_set_info(DRIVE_BIS, SET_WRITE_PROTECT, &allow_writes);
	ctx->bis_write_unlocked = false;

	_fm_list_dir(ctx);

	return LV_RES_OK;
}

static void _fm_list_dir(fm_ctx_t *ctx)
{
	lv_list_clean(ctx->list);

	// Old entries' names back the free_ptr tags of the buttons just destroyed above.
	free(ctx->entries);
	ctx->entries = NULL;
	ctx->entry_count = 0;

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

		// eMMC/emuMMC write actions depend on the unlock state.
		if (!strncmp(ctx->path, "bis:", 4))
		{
			if (ctx->bis_write_unlocked)
			{
				lv_list_add(ctx->list, NULL, SYMBOL_DIRECTORY" New Folder", _fm_action_new_folder);

				if (ctx->clip_path[0])
					lv_list_add(ctx->list, NULL, ctx->clip_is_cut ? SYMBOL_COPY" Paste (Move)" : SYMBOL_COPY" Paste (Copy)", _fm_action_paste);

				lv_list_add(ctx->list, NULL, SYMBOL_KEY" Lock writes", _fm_action_lock_emmc);
			}

			char *txt_buf = (char *)malloc(SZ_4K);
			s_printf(txt_buf, SYMBOL_CLOSE" Close %s", ctx->src_name);
			lv_list_add(ctx->list, NULL, txt_buf, _fm_action_close_emmc);
			free(txt_buf);
		}
		else
		{
			lv_list_add(ctx->list, NULL, SYMBOL_DIRECTORY" New Folder", _fm_action_new_folder);

			if (ctx->clip_path[0])
				lv_list_add(ctx->list, NULL, ctx->clip_is_cut ? SYMBOL_COPY" Paste (Move)" : SYMBOL_COPY" Paste (Copy)", _fm_action_paste);
		}

		ctx->entries = _fm_read_dir(ctx->path, &ctx->entry_count);
		for (u32 i = 0; i < ctx->entry_count; i++)
		{
			fm_entry_t *e = &ctx->entries[i];
			char size_buf[32];
			char date_buf[32];

			if (e->is_dir)
				strcpy(size_buf, "<DIR>");
			else
				_fm_format_size(size_buf, e->size);
			_fm_format_date(date_buf, e->fdate, e->ftime);

			char label_buf[256 + 64];
			s_printf(label_buf, "%s %s    %s    %s", e->is_dir ? SYMBOL_DIRECTORY : SYMBOL_FILE, e->name, size_buf, date_buf);

			lv_obj_t *entry = lv_list_add(ctx->list, NULL, label_buf, _fm_action_entry);
			lv_obj_set_free_ptr(entry, e);
			lv_btn_set_action(entry, LV_BTN_ACTION_LONG_PR, _fm_action_entry_long);
		}
	}

	// Update path label, with free space on the current volume.
	char *txt_buf = (char *)malloc(SZ_4K);
	char free_buf[48];
	free_buf[0] = 0;
	if (ctx->path[0])
		_fm_format_free_space(free_buf, ctx->path);

	if (ctx->path[0] && ctx->bis_mounted && ctx->src_name)
		s_printf(txt_buf, "Path: #C7EA46 [%s] %s#%s", ctx->src_name, ctx->path, free_buf);
	else
		s_printf(txt_buf, "Path: #C7EA46 %s#%s", ctx->path, free_buf);
	lv_label_set_text(ctx->path_label, txt_buf);
	free(txt_buf);

	manual_system_maintenance(true);
}

static lv_res_t _fm_action_close(lv_obj_t *btn)
{
	fm_ctx.clip_path[0] = 0;
	fm_ctx.win = NULL;

	free(fm_ctx.entries);
	fm_ctx.entries = NULL;
	fm_ctx.entry_count = 0;

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
	free(fm_ctx.entries);

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
