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
#include <libs/fatfs/ff.h>

#define FM_PATH_MAX 512
#define FM_BUF      SDXC_BUF_ALIGNED
#define FM_BUF_SIZE SZ_4M

typedef struct _fm_ctx_t
{
	lv_obj_t *win;
	lv_obj_t *path_label;
	lv_obj_t *list;
	char path[FM_PATH_MAX];
	char clip_path[FM_PATH_MAX];
	bool clip_is_dir;
} fm_ctx_t;

static fm_ctx_t fm_ctx;

// Context for the current file menu. Single instance GUI, so static is safe.
static char fm_menu_path[FM_PATH_MAX];
static bool fm_menu_is_dir;

static void _fm_list_dir(fm_ctx_t *ctx);
static lv_res_t _fm_delete_confirm_action(lv_obj_t *btns, const char *txt);
static lv_res_t _fm_file_menu_action(lv_obj_t *btns, const char *txt);

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

	// Drive root: go back to drive selection.
	if (!strcmp(ctx->path, "sd:/"))
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

	if (is_dir)
		strcat(txt_buf, "\n\n#C7EA46 Copy pastes the whole folder. Long press to open menu.#");
	else
		strcat(txt_buf, "\n\n#C7EA46 Long press a folder to open its menu.#");

	lv_mbox_set_text(mbox, txt_buf);
	free(txt_buf);

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

	// Only the drive entry exists at root level.
	if (!ctx->path[0])
	{
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
		lv_list_add(ctx->list, NULL, SYMBOL_SD" SD Card", _fm_action_entry);
	}
	else
	{
		lv_list_add(ctx->list, NULL, SYMBOL_REFRESH" Refresh", _fm_action_refresh);
		lv_list_add(ctx->list, NULL, SYMBOL_DIRECTORY" New Folder", _fm_action_new_folder);

		if (ctx->clip_path[0])
			lv_list_add(ctx->list, NULL, SYMBOL_COPY" Paste", _fm_action_paste);

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
	s_printf(txt_buf, "Path: #C7EA46 %s#", ctx->path);
	lv_label_set_text(ctx->path_label, txt_buf);
	free(txt_buf);

	manual_system_maintenance(true);
}

static lv_res_t _fm_action_close(lv_obj_t *btn)
{
	fm_ctx.clip_path[0] = 0;
	fm_ctx.win = NULL;
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

	memset(&fm_ctx, 0, sizeof(fm_ctx));

	lv_obj_t *win = nyx_create_standard_window(SYMBOL_DRIVE" SD File Manager", _fm_action_close);
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
