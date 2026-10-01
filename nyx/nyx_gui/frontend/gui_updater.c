/*
 * Copyright (c) 2026 CTCaer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include <bdk.h>

#include <libs/compr/tinf.h>
#include <libs/fatfs/ff.h>

#include "gui.h"
#include "gui_updater.h"

#define UPD_ROOT       "sd:/update"
#define UPD_BACKUP     "sd:/update/backup"

#define UPD_PATH_MAX   512
#define UPD_MAX_PKGS   16
#define UPD_BUF_SIZE   SZ_4M
#define UPD_TXT_SIZE   (SZ_4K * 4)

// Supported update components. Payloads are single files, the rest are merged folders.
enum
{
	COMP_HEKATE = 0,
	COMP_NYX,
	COMP_ATMOSPHERE,
	COMP_BOOTLOADER,
	COMP_SWITCH,
	COMP_SEPT,
	COMP_COUNT,
	COMP_NONE = 0xFF
};

static const char *const comp_names[COMP_COUNT] =
{
	"hekate", "nyx", "atmosphere", "bootloader", "switch", "sept"
};

// Zip file signatures.
#define UPD_SIG_EOCD   0x06054b50 // End of central directory.
#define UPD_SIG_CDENT  0x02014b50 // Central directory entry.
#define UPD_SIG_LOCAL  0x04034b50 // Local file header.

#define UPD_METHOD_STORE   0
#define UPD_METHOD_DEFLATE 8

typedef struct
{
	char *name;   // Path of the file inside the package.
	u32   size;   // Uncompressed size.
	u32   csize;  // Compressed size. Zip only.
	u32   offset; // Local header offset. Zip only.
	u32   crc;    // Expected CRC32 of uncompressed data. Zip only.
	u16   method; // Compression method. Zip only.
	u8    comp;   // Target component.
} upd_file_t;

typedef struct
{
	char       *path; // Package path. Zip file or directory.
	bool        is_zip;
	upd_file_t *files;
	u32         file_cnt;
	u32         total_size; // Sum of uncompressed sizes.
	FIL         fp;         // Kept open during apply. Zip only.
} upd_pkg_t;

static struct
{
	upd_pkg_t pkgs[UPD_MAX_PKGS];
	u32 pkg_cnt;

	lv_obj_t *btn_update;
	lv_obj_t *lbl_pkgs;
} upd_ctx;

static void _upd_error_box(const char *text)
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

static char *_upd_strdup(const char *str)
{
	u32 len = strlen(str) + 1;
	char *out = malloc(len);
	if (out)
		memcpy(out, str, len);

	return out;
}

static u32 _upd_rd16(const u8 *buf)
{
	return buf[0] | (buf[1] << 8);
}

static u32 _upd_rd32(const u8 *buf)
{
	return buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((u32)buf[3] << 24);
}

static void _upd_free_pkg(upd_pkg_t *pkg)
{
	if (!pkg->path)
		return;

	for (u32 i = 0; i < pkg->file_cnt; i++)
		free(pkg->files[i].name);

	free(pkg->files);
	free(pkg->path);
	memset(pkg, 0, sizeof(upd_pkg_t));
}

static void _upd_free_all()
{
	for (u32 i = 0; i < UPD_MAX_PKGS; i++)
		_upd_free_pkg(&upd_ctx.pkgs[i]);

	upd_ctx.pkg_cnt = 0;
}

static bool _upd_pkg_add_file(upd_pkg_t *pkg, const char *name, u8 comp, u32 size, u32 csize, u32 offset, u16 method, u32 crc)
{
	// Grow the file array in chunks to limit heap churn.
	if (!(pkg->file_cnt & 0x1F))
	{
		// The bdk heap has no realloc. Allocate a new block and copy over.
		upd_file_t *tmp = malloc((pkg->file_cnt + 0x20) * sizeof(upd_file_t));
		if (!tmp)
			return false;
		if (pkg->files)
		{
			memcpy(tmp, pkg->files, pkg->file_cnt * sizeof(upd_file_t));
			free(pkg->files);
		}
		pkg->files = tmp;
	}

	upd_file_t *f = &pkg->files[pkg->file_cnt];
	f->name = _upd_strdup(name);
	if (!f->name)
		return false;

	f->comp   = comp;
	f->size   = size;
	f->csize  = csize;
	f->offset = offset;
	f->crc    = crc;
	f->method = method;

	pkg->file_cnt++;
	pkg->total_size += size;

	return true;
}

static u8 _upd_classify(const char *name, bool is_dir, u8 forced)
{
	if (forced != COMP_NONE)
		return forced;

	if (is_dir)
		return COMP_NONE;

	// Folder components are detected by path prefix.
	for (u32 i = COMP_ATMOSPHERE; i < COMP_COUNT; i++)
	{
		u32 len = strlen(comp_names[i]);
		if (!strncasecmp(name, comp_names[i], len) && name[len] == '/')
			return i;
	}

	// Payloads are detected at package root by file name.
	if (!strchr(name, '/'))
	{
		u32 len = strlen(name);
		if (len > 4 && !strcasecmp(name + len - 4, ".bin"))
		{
			if (!strncasecmp(name, "hekate", 6))
				return COMP_HEKATE;
			if (!strcasecmp(name, "nyx.bin"))
				return COMP_NYX;
		}
	}

	return COMP_NONE;
}

// A package named after a folder component applies to that component exclusively.
static u8 _upd_pkg_forced_comp(const char *pkg_name)
{
	for (u32 i = COMP_ATMOSPHERE; i < COMP_COUNT; i++)
		if (!strcasecmp(pkg_name, comp_names[i]))
			return i;

	return COMP_NONE;
}

static bool _upd_zip_scan(upd_pkg_t *pkg, u8 forced)
{
	FIL fp;
	FRESULT res = f_open(&fp, pkg->path, FA_READ);
	if (res != FR_OK)
		return false;

	u32 zip_size = f_size(&fp);
	bool result = false;
	u8 *cd = NULL;
	UINT rd;

	// Search the tail of the file for the End of Central Directory record.
	u32 tail = MIN(zip_size, SZ_64K + 22);
	u8 *buf = malloc(tail);
	if (!buf)
		goto out;

	if (f_lseek(&fp, zip_size - tail) || f_read(&fp, buf, tail, &rd) || rd != tail)
		goto out;

	u8 *eocd = NULL;
	for (s32 i = tail - 22; i >= 0; i--)
	{
		if (_upd_rd32(buf + i) == UPD_SIG_EOCD)
		{
			eocd = buf + i;
			break;
		}
	}
	if (!eocd)
		goto out;

	u32 entries = _upd_rd16(eocd + 10);
	u32 cd_size = _upd_rd32(eocd + 12);
	u32 cd_off  = _upd_rd32(eocd + 16);

	// Zip64 archives are not supported.
	if (entries == 0xFFFF || cd_size == 0xFFFFFFFF || cd_off == 0xFFFFFFFF)
		goto out;

	if (!entries || cd_off >= zip_size || cd_size > zip_size - cd_off)
		goto out;

	cd = malloc(cd_size);
	if (!cd)
		goto out;

	if (f_lseek(&fp, cd_off) || f_read(&fp, cd, cd_size, &rd) || rd != cd_size)
		goto out;

	// Parse central directory entries.
	u32 pos = 0;
	while (pos + 46 <= cd_size)
	{
		if (_upd_rd32(cd + pos) != UPD_SIG_CDENT)
			break;

		u16 method = _upd_rd16(cd + pos + 10);
		u32 csize  = _upd_rd32(cd + pos + 20);
		u32 usize  = _upd_rd32(cd + pos + 24);
		u16 nlen   = _upd_rd16(cd + pos + 28);
		u16 elen   = _upd_rd16(cd + pos + 30);
		u16 clen   = _upd_rd16(cd + pos + 32);
		u32 lho    = _upd_rd32(cd + pos + 42);

		// Guard against overflowing the central directory buffer.
		if (pos + 46 + (u32)nlen + (u32)elen + (u32)clen > cd_size)
			break;

		if (nlen && nlen < UPD_PATH_MAX)
		{
			char name[UPD_PATH_MAX];
			memcpy(name, cd + pos + 46, nlen);
			name[nlen] = 0;

			bool is_dir = name[nlen - 1] == '/';
			u8 comp = _upd_classify(name, is_dir, forced);

			if (!is_dir && comp != COMP_NONE && (method == UPD_METHOD_STORE || method == UPD_METHOD_DEFLATE))
			{
				// Entry must fit inside the zip file.
				if (lho + 30 + (u32)nlen + (u32)elen + csize > zip_size)
					goto out;

				if (!_upd_pkg_add_file(pkg, name, comp, usize, csize, lho, method, _upd_rd32(cd + pos + 16)))
					goto out;
			}
		}

		pos += 46 + (u32)nlen + (u32)elen + (u32)clen;
	}

	result = true;

out:
	free(cd);
	free(buf);
	f_close(&fp);

	return result;
}

static bool _upd_dir_scan(upd_pkg_t *pkg, u8 forced, const char *path, const char *rel)
{
	DIR dir;
	FRESULT res = f_opendir(&dir, path);
	if (res != FR_OK)
		return false;

	bool ok = true;
	FILINFO fno;
	while (!f_readdir(&dir, &fno) && fno.fname[0])
	{
		if (!strcmp(fno.fname, ".") || !strcmp(fno.fname, ".."))
			continue;

		char *sub_path = malloc(UPD_PATH_MAX);
		char *sub_rel = malloc(UPD_PATH_MAX);
		if (!sub_path || !sub_rel)
		{
			free(sub_path);
			free(sub_rel);
			ok = false;
			break;
		}

		bool overflow = strlen(path) + strlen(fno.fname) + 2 >= UPD_PATH_MAX ||
				strlen(rel) + strlen(fno.fname) + 2 >= UPD_PATH_MAX;

		if (overflow)
		{
			free(sub_path);
			free(sub_rel);
			continue;
		}

		s_printf(sub_path, "%s/%s", path, fno.fname);
		s_printf(sub_rel, "%s%s", rel, fno.fname);

		if (fno.fattrib & AM_DIR)
		{
			strcat(sub_rel, "/");
			ok = _upd_dir_scan(pkg, forced, sub_path, sub_rel);
		}
		else
		{
			u8 comp = _upd_classify(sub_rel, false, forced);
			if (comp != COMP_NONE)
				ok = _upd_pkg_add_file(pkg, sub_rel, comp, fno.fsize, 0, 0, UPD_METHOD_STORE, 0);
		}

		free(sub_path);
		free(sub_rel);

		if (!ok)
			break;
	}

	f_closedir(&dir);

	return ok;
}

static bool _upd_scan_packages()
{
	DIR dir;
	FRESULT res = f_opendir(&dir, UPD_ROOT);
	if (res != FR_OK)
		return false;

	FILINFO fno;
	while (!f_readdir(&dir, &fno) && fno.fname[0])
	{
		// The backup folder holds previous versions. Never touch it here.
		if (!strcmp(fno.fname, ".") || !strcmp(fno.fname, "..") || !strcasecmp(fno.fname, "backup"))
			continue;

		if (upd_ctx.pkg_cnt == UPD_MAX_PKGS)
			break;

		char *path = malloc(UPD_PATH_MAX);
		if (!path)
			break;

		if (strlen(fno.fname) + strlen(UPD_ROOT) + 2 >= UPD_PATH_MAX)
		{
			free(path);
			continue;
		}

		s_printf(path, "%s/%s", UPD_ROOT, fno.fname);

		upd_pkg_t *pkg = &upd_ctx.pkgs[upd_ctx.pkg_cnt];
		memset(pkg, 0, sizeof(upd_pkg_t));
		pkg->path = path;

		u8 forced = _upd_pkg_forced_comp(fno.fname);

		bool ok = false;
		if (fno.fattrib & AM_DIR)
		{
			pkg->is_zip = false;
			ok = _upd_dir_scan(pkg, forced, path, "");
		}
		else
		{
			u32 len = strlen(fno.fname);
			if (len > 4 && !strcasecmp(fno.fname + len - 4, ".zip"))
			{
				pkg->is_zip = true;
				ok = _upd_zip_scan(pkg, forced);
			}
		}

		if (ok && pkg->file_cnt)
			upd_ctx.pkg_cnt++;
		else
			_upd_free_pkg(pkg);
	}

	f_closedir(&dir);

	return true;
}

// Relative path of an entry inside its component folder.
static const char *_upd_entry_rel(const upd_file_t *f)
{
	const char *name = f->name;
	u32 len = strlen(comp_names[f->comp]);
	if (!strncasecmp(name, comp_names[f->comp], len) && name[len] == '/')
		name += len + 1;

	return name;
}

static bool _upd_entry_target(const upd_file_t *f, char *out)
{
	// Payloads have fixed destinations.
	if (f->comp == COMP_HEKATE)
	{
		strcpy(out, "sd:/bootloader/update.bin");
		return true;
	}

	if (f->comp == COMP_NYX)
	{
		strcpy(out, "sd:/bootloader/sys/nyx.bin");
		return true;
	}

	const char *name = _upd_entry_rel(f);
	if (!*name)
		return false;

	if (strlen(name) + strlen(comp_names[f->comp]) + 6 >= UPD_PATH_MAX)
		return false;

	s_printf(out, "sd:/%s/%s", comp_names[f->comp], name);

	return true;
}

static bool _upd_entry_backup_path(const upd_file_t *f, char *out)
{
	if (f->comp == COMP_HEKATE)
	{
		strcpy(out, UPD_BACKUP"/hekate.bin");
		return true;
	}

	if (f->comp == COMP_NYX)
	{
		strcpy(out, UPD_BACKUP"/nyx.bin");
		return true;
	}

	const char *name = _upd_entry_rel(f);
	if (!*name)
		return false;

	if (strlen(name) + strlen(comp_names[f->comp]) + strlen(UPD_BACKUP) + 3 >= UPD_PATH_MAX)
		return false;

	s_printf(out, "%s/%s/%s", UPD_BACKUP, comp_names[f->comp], name);

	return true;
}

// Creates every parent folder of a file path.
static void _upd_mkdir_parents(const char *path)
{
	char *tmp = _upd_strdup(path);
	if (!tmp)
		return;

	// Skip the "sd:/" volume prefix.
	for (u32 i = 4; tmp[i]; i++)
	{
		if (tmp[i] == '/')
		{
			tmp[i] = 0;
			f_mkdir(tmp);
			tmp[i] = '/';
		}
	}

	free(tmp);
}

typedef struct
{
	u64 done;
	u64 total;
	lv_obj_t *bar;
	u8 *buf;
} upd_progress_t;

static void _upd_progress_pump(upd_progress_t *pg, u32 bytes)
{
	pg->done += bytes;

	u32 pct = (u32)((pg->done * 100) / pg->total);
	lv_bar_set_value(pg->bar, pct);
	manual_system_maintenance(false);
}

static bool _upd_write_data(const char *dst, const u8 *buf, u32 size, upd_progress_t *pg)
{
	FIL fp;
	if (f_open(&fp, dst, FA_CREATE_ALWAYS | FA_WRITE))
		return false;

	bool ok = true;
	u32 left = size;
	while (left)
	{
		u32 chunk = MIN(left, UPD_BUF_SIZE);
		UINT written = 0;

		if (f_write(&fp, (void *)buf, chunk, &written) || written != chunk)
		{
			ok = false;
			break;
		}

		buf += chunk;
		left -= chunk;
		_upd_progress_pump(pg, chunk);
	}

	f_close(&fp);

	return ok;
}

// count: Add copied bytes to progress accounting.
static bool _upd_copy_file(const char *src, const char *dst, upd_progress_t *pg, bool count)
{
	FIL src_fp;
	FIL dst_fp;

	if (f_open(&src_fp, src, FA_READ))
		return false;

	if (f_open(&dst_fp, dst, FA_CREATE_ALWAYS | FA_WRITE))
	{
		f_close(&src_fp);
		return false;
	}

	bool ok = true;
	while (true)
	{
		UINT read_bytes = 0;
		if (f_read(&src_fp, pg->buf, UPD_BUF_SIZE, &read_bytes) || !read_bytes)
			break;

		UINT written_bytes = 0;
		if (f_write(&dst_fp, pg->buf, read_bytes, &written_bytes) || written_bytes != read_bytes)
		{
			ok = false;
			break;
		}

		if (count)
			_upd_progress_pump(pg, read_bytes);
	}

	f_close(&dst_fp);
	f_close(&src_fp);

	return ok;
}

static bool _upd_zip_extract(upd_pkg_t *pkg, upd_file_t *f, const char *dst, upd_progress_t *pg)
{
	if (f_lseek(&pkg->fp, f->offset))
		return false;

	u8 hdr[30];
	UINT rd = 0;
	if (f_read(&pkg->fp, hdr, 30, &rd) || rd != 30 || _upd_rd32(hdr) != UPD_SIG_LOCAL)
		return false;

	// Local header name/extra lengths can differ from the central directory.
	u32 data_off = f->offset + 30 + (u32)_upd_rd16(hdr + 26) + (u32)_upd_rd16(hdr + 28);
	if (f_lseek(&pkg->fp, data_off))
		return false;

	u8 *src = malloc(f->csize ? f->csize : 1);
	if (!src)
		return false;

	if (f_read(&pkg->fp, src, f->csize, &rd) || rd != f->csize)
	{
		free(src);
		return false;
	}

	bool ok = false;

	if (f->method == UPD_METHOD_STORE)
	{
		if (f->crc == crc32_calc(0, src, f->size))
			ok = _upd_write_data(dst, src, f->size, pg);
	}
	else
	{
		u8 *dst_buf = malloc(f->size ? f->size : 1);
		if (dst_buf)
		{
			u32 dst_len = f->size;

			if (tinf_uncompress(dst_buf, &dst_len, src, f->csize) == TINF_OK &&
			    dst_len == f->size && f->crc == crc32_calc(0, dst_buf, dst_len))
				ok = _upd_write_data(dst, dst_buf, dst_len, pg);

			free(dst_buf);
		}
	}

	free(src);

	return ok;
}

static lv_res_t _upd_apply_files(lv_obj_t *bar, lv_obj_t *label)
{
	u32 errors = 0;
	u32 files_done = 0;
	const char *first_err = NULL;
	char *target = malloc(UPD_PATH_MAX);
	char *backup = malloc(UPD_PATH_MAX);
	char *label_txt = malloc(SZ_4K);

	upd_progress_t pg;
	pg.done = 0;
	pg.bar = bar;
	pg.buf = malloc(UPD_BUF_SIZE);

	u64 total = 0;
	for (u32 i = 0; i < upd_ctx.pkg_cnt; i++)
		total += upd_ctx.pkgs[i].total_size;
	pg.total = total ? total : 1;

	if (!target || !backup || !label_txt || !pg.buf)
	{
		free(target);
		free(backup);
		free(label_txt);
		free(pg.buf);

		return LV_RES_INV;
	}

	for (u32 i = 0; i < upd_ctx.pkg_cnt; i++)
	{
		upd_pkg_t *pkg = &upd_ctx.pkgs[i];

		if (pkg->is_zip && f_open(&pkg->fp, pkg->path, FA_READ))
		{
			errors++;
			continue;
		}

		for (u32 j = 0; j < pkg->file_cnt; j++)
		{
			upd_file_t *f = &pkg->files[j];

			if (!_upd_entry_target(f, target))
			{
				errors++;
				continue;
			}

			// Show the current operation.
			s_printf(label_txt, "%s\n#96FF00 %s#", pkg->path, target);
			lv_label_set_text(label, label_txt);
			manual_system_maintenance(false);

			// Back up the existing file before replacing it.
			if (!f_stat(target, NULL))
			{
				if (!_upd_entry_backup_path(f, backup))
				{
					errors++;
					continue;
				}

				_upd_mkdir_parents(backup);
				if (!_upd_copy_file(target, backup, &pg, false))
					errors++;
			}

			_upd_mkdir_parents(target);

			bool ok = false;
			if (pkg->is_zip)
				ok = _upd_zip_extract(pkg, f, target, &pg);
			else
			{
				char *src_path = malloc(UPD_PATH_MAX);
				if (src_path)
				{
					s_printf(src_path, "%s/%s", pkg->path, f->name);
					ok = _upd_copy_file(src_path, target, &pg, true);
					free(src_path);
				}
			}

			if (ok)
				files_done++;
			else
			{
				errors++;
				if (!first_err)
					first_err = _upd_strdup(target);
			}
		}

		if (pkg->is_zip)
			f_close(&pkg->fp);
	}

	free(pg.buf);
	free(target);
	free(backup);
	free(label_txt);

	// Show the result.
	char *txt = malloc(UPD_TXT_SIZE);
	if (!txt)
	{
		free((void *)first_err);

		return LV_RES_INV;
	}

	if (errors)
		s_printf(txt, "#FF3C28 Update finished with %d error(s)!#\n\n"
			      "%d file(s) were updated.\n"
			      "#FF8000 First failed file:#\n%s\n\n"
			      "Previous versions were saved to #C7EA46 update/backup/#.",
			  errors, files_done, first_err ? first_err : "-");
	else
		s_printf(txt, "#96FF00 Update completed!#\n\n"
			      "%d file(s) were updated.\n"
			      "hekate updates apply on the next boot.\n\n"
			      "Previous versions were saved to #C7EA46 update/backup/#.",
			  files_done);

	_upd_error_box(txt);

	free(txt);
	free((void *)first_err);

	return LV_RES_OK;
}

static lv_res_t _upd_progress_apply(lv_obj_t *btn)
{
	lv_obj_t *bar = NULL;
	lv_obj_t *label = NULL;

	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 5);

	lv_mbox_set_text(mbox, "#C7EA46 Updating...#\n\n");

	label = lv_label_create(mbox, NULL);
	lv_label_set_recolor(label, true);
	lv_label_set_text(label, " ");

	bar = lv_bar_create(mbox, NULL);
	lv_obj_set_size(bar, LV_HOR_RES * 4 / 9, LV_DPI / 3);
	lv_bar_set_value(bar, 0);

	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	lv_res_t res = _upd_apply_files(bar, label);

	lv_obj_del(dark_bg);
	manual_system_maintenance(true);

	return res;
}

static lv_res_t _upd_mbox_update_action(lv_obj_t *btns, const char *txt)
{
	lv_obj_t *mbox = lv_mbox_get_from_btn(btns);
	lv_obj_t *dark_bg = lv_obj_get_parent(mbox);

	if (!strcmp(txt, "Update"))
	{
		lv_obj_del(dark_bg);
		_upd_progress_apply(NULL);

		return LV_RES_INV;
	}

	lv_obj_del(dark_bg);

	return LV_RES_INV;
}

static lv_res_t _upd_action_update(lv_obj_t *btn)
{
	// Summarize the found components.
	u32 comp_files[COMP_COUNT] = { 0 };
	u32 comp_pkgs[COMP_COUNT] = { 0 };

	for (u32 i = 0; i < upd_ctx.pkg_cnt; i++)
	{
		u8 present = 0;
		for (u32 j = 0; j < upd_ctx.pkgs[i].file_cnt; j++)
		{
			u8 comp = upd_ctx.pkgs[i].files[j].comp;
			comp_files[comp]++;
			present |= 1 << comp;
		}
		for (u32 j = 0; j < COMP_COUNT; j++)
			if (present & (1 << j))
				comp_pkgs[j]++;
	}

	char *txt = malloc(UPD_TXT_SIZE);
	if (!txt)
		return LV_RES_OK;

	strcpy(txt, "#C7EA46 The following components were found:#\n\n");
	for (u32 i = 0; i < COMP_COUNT; i++)
	{
		if (!comp_files[i])
			continue;

		char comp_txt[128];
		s_printf(comp_txt, "#96FF00 %s# - %d file(s)", comp_names[i], comp_files[i]);
		if (comp_pkgs[i] > 1)
			s_printf(comp_txt + strlen(comp_txt), " in %d package(s)", comp_pkgs[i]);
		strcat(txt, comp_txt);
		strcat(txt, "\n");
	}
	strcat(txt,
		"\nExisting files will be backed up to\n"
		"#C7EA46 update/backup/# before they are replaced.\n\n"
		"#FF8000 hekate updates apply on the next boot.#\n\n"
		"Continue?");

	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\222Update", "\222Cancel", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 5);

	lv_mbox_set_text(mbox, txt);
	free(txt);

	lv_mbox_add_btns(mbox, mbox_btn_map, _upd_mbox_update_action); // Important. After set_text.
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	return LV_RES_OK;
}

static lv_res_t _upd_action_scan(lv_obj_t *btn)
{
	_upd_free_all();

	lv_label_set_text(upd_ctx.lbl_pkgs, "Scanning #C7EA46 sd:/update#...");
	manual_system_maintenance(false);

	// Create the update folder if it is missing.
	if (f_stat(UPD_ROOT, NULL) && f_mkdir(UPD_ROOT))
	{
		lv_label_set_text(upd_ctx.lbl_pkgs, "#FF3C28 Failed to open sd:/update!#");
		return LV_RES_OK;
	}

	_upd_scan_packages();

	if (!upd_ctx.pkg_cnt)
	{
		lv_label_set_text(upd_ctx.lbl_pkgs,
			"#FF8000 No update packages found in sd:/update.#\n\n"
			"Copy Atmosph\u00e8re/HATS zips or extracted package folders inside it.\n"
			"Payloads named #C7EA46 hekate*.bin# and #C7EA46 nyx.bin# are also applied.\n"
			"A package named after a component (e.g #C7EA46 atmosphere.zip#) is applied to it.");
	}
	else
	{
		char *txt = malloc(UPD_TXT_SIZE);
		if (!txt)
			return LV_RES_OK;

		s_printf(txt, "Found #96FF00 %d# package(s):\n\n", upd_ctx.pkg_cnt);

		for (u32 i = 0; i < upd_ctx.pkg_cnt; i++)
		{
			upd_pkg_t *pkg = &upd_ctx.pkgs[i];
			const char *pkg_name = strrchr(pkg->path, '/') + 1;

			char pkg_txt[UPD_PATH_MAX];
			s_printf(pkg_txt, "#00DDFF %s# - %d file(s), %d MB\n", pkg_name, pkg->file_cnt, pkg->total_size >> 20);
			strcat(txt, pkg_txt);

			u8 present = 0;
			for (u32 j = 0; j < pkg->file_cnt; j++)
				present |= 1 << pkg->files[j].comp;

			for (u32 j = 0; j < COMP_COUNT; j++)
			{
				if (!(present & (1 << j)))
					continue;

				strcat(txt, " #C7EA46 ");
				strcat(txt, comp_names[j]);
				strcat(txt, "#");
			}
			strcat(txt, "\n\n");
		}

		lv_label_set_text(upd_ctx.lbl_pkgs, txt);
		free(txt);
	}

	lv_btn_set_state(upd_ctx.btn_update, upd_ctx.pkg_cnt ? LV_BTN_STATE_REL : LV_BTN_STATE_INA);

	return LV_RES_OK;
}

static lv_res_t _upd_action_close(lv_obj_t *btn)
{
	_upd_free_all();

	return nyx_win_close_action(btn);
}

lv_res_t create_window_updater_tool(lv_obj_t *btn)
{
	if (sd_mount())
	{
		_upd_error_box("#FFDD00 Failed to init SD!#");
		return LV_RES_OK;
	}

	_upd_free_all();

	lv_obj_t *win = nyx_create_standard_window(SYMBOL_DOWNLOAD" SD Updater", _upd_action_close);

	// Create hint.
	lv_obj_t *lbl_hint = lv_label_create(win, NULL);
	lv_label_set_recolor(lbl_hint, true);
	lv_label_set_static_text(lbl_hint,
		"Update hekate, Nyx and Atmosph\u00e8re from packages in #C7EA46 sd:/update/#.\n"
		"Zips and extracted folders are supported. Existing files are backed up\n"
		"to #C7EA46 update/backup/# before they are replaced.");
	lv_obj_set_style(lbl_hint, &hint_small_style);
	lv_obj_align(lbl_hint, NULL, LV_ALIGN_IN_TOP_MID, 0, LV_DPI / 10);

	// Create Scan button.
	lv_obj_t *btn_scan = lv_btn_create(win, NULL);
	lv_btn_set_fit(btn_scan, true, true);
	lv_obj_t *lbl_btn = lv_label_create(btn_scan, NULL);
	lv_label_set_static_text(lbl_btn, SYMBOL_REFRESH"  Scan");
	lv_obj_align(btn_scan, lbl_hint, LV_ALIGN_OUT_BOTTOM_MID, 0, LV_DPI / 4);
	lv_btn_set_action(btn_scan, LV_BTN_ACTION_CLICK, _upd_action_scan);

	// Create Update button.
	upd_ctx.btn_update = lv_btn_create(win, btn_scan);
	lbl_btn = lv_label_create(upd_ctx.btn_update, NULL);
	lv_label_set_static_text(lbl_btn, SYMBOL_UPLOAD"  Update");
	lv_obj_align(upd_ctx.btn_update, btn_scan, LV_ALIGN_OUT_RIGHT_MID, LV_DPI / 4, 0);
	lv_btn_set_action(upd_ctx.btn_update, LV_BTN_ACTION_CLICK, _upd_action_update);
	lv_btn_set_state(upd_ctx.btn_update, LV_BTN_STATE_INA);

	// Create package list.
	upd_ctx.lbl_pkgs = lv_label_create(win, NULL);
	lv_label_set_recolor(upd_ctx.lbl_pkgs, true);
	lv_label_set_text(upd_ctx.lbl_pkgs, "Scan #C7EA46 sd:/update/# for update packages.");
	lv_obj_set_width(upd_ctx.lbl_pkgs, LV_HOR_RES - LV_DPI * 2 / 3);
	lv_label_set_long_mode(upd_ctx.lbl_pkgs, LV_LABEL_LONG_BREAK);
	lv_obj_align(upd_ctx.lbl_pkgs, btn_scan, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);

	// Scan on open.
	_upd_action_scan(btn_scan);

	return LV_RES_OK;
}
