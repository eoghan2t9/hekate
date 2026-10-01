/*
 * eMMC BIS driver for Nintendo Switch
 *
 * Copyright (c) 2019-2020 shchmue
 * Copyright (c) 2019-2022 CTCaer
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

#include <string.h>
#include <stdlib.h>

#include <memory_map.h>

#include <mem/heap.h>
#include <sec/se.h>
#include <storage/emmc.h>
#include <storage/sd.h>
#include <storage/sdmmc.h>
#include <utils/types.h>

#define BIS_CLUSTER_SECTORS   32
#define BIS_CLUSTER_SIZE      16384
#define BIS_CACHE_MAX_ENTRIES 16384
#define BIS_CACHE_LOOKUP_TBL_EMPTY_ENTRY -1

typedef struct _cluster_cache_t
{
	u32  cluster_idx;            // Index of the cluster in the partition.
	bool dirty;                  // Has been modified without write-back flag.
	u8   data[BIS_CLUSTER_SIZE]; // The cached cluster itself. Aligned to 8 bytes for DMA engine.
} cluster_cache_t;

typedef struct _bis_cache_t
{
	bool full;
	bool enabled;
	u32  dirty_cnt;
	u32  top_idx;
	u8   dma_buff[BIS_CLUSTER_SIZE]; // Aligned to 8 bytes for DMA engine.
	cluster_cache_t clusters[];
} bis_cache_t;

static u8  ks_crypt = 0;
static u8  ks_tweak = 0;
static u32 emu_offset = 0;
static emmc_part_t *system_part = NULL;
static u32 *cache_lookup_tbl = (u32 *)NX_BIS_LOOKUP_ADDR;
static bis_cache_t *bis_cache = (bis_cache_t *)NX_BIS_CACHE_ADDR;

// File backed emuMMC state. Path is NULL when not in file backed mode.
// A single FIL handle is kept open and re-opened only when the part file changes.
static FIL   bis_file_fp = { 0 };
static char *bis_file_path = NULL;
static u32   bis_file_part_size = 0;  // Sectors per part file.
static bool  bis_file_fp_open = false;
static u32   bis_file_fp_part = 0;    // Part index the open handle belongs to.
static bool  bis_file_fp_write = false; // Open mode of the open handle.

static int _nx_emmc_bis_file_read(u32 sector, u32 count, void *buff);
static int _nx_emmc_bis_file_write(u32 sector, u32 count, void *buff);

static int nx_emmc_bis_write_block(u32 sector, u32 count, void *buff, bool flush)
{
	if (!system_part)
		return 3; // Not ready.

	int res;
	u8   tweak[SE_KEY_128_SIZE] __attribute__((aligned(4)));
	u32  cluster = sector / BIS_CLUSTER_SECTORS;
	u32  aligned_sector = cluster * BIS_CLUSTER_SECTORS;
	u32  sector_in_cluster = sector % BIS_CLUSTER_SECTORS;
	u32  lookup_idx = cache_lookup_tbl[cluster];
	bool is_cached = lookup_idx != (u32)BIS_CACHE_LOOKUP_TBL_EMPTY_ENTRY;

	// Write to cached cluster.
	if (is_cached)
	{
		if (buff)
			memcpy(bis_cache->clusters[lookup_idx].data + sector_in_cluster * EMMC_BLOCKSIZE, buff, count * EMMC_BLOCKSIZE);
		else
			buff = bis_cache->clusters[lookup_idx].data;
		if (!bis_cache->clusters[lookup_idx].dirty)
			bis_cache->dirty_cnt++;
		bis_cache->clusters[lookup_idx].dirty = true;

		if (!flush)
			return 0; // Success.

		// Reset args to trigger a full cluster flush to emmc.
		sector_in_cluster = 0;
		sector = aligned_sector;
		count = BIS_CLUSTER_SECTORS;
	}

	// Encrypt cluster.
	if (se_aes_crypt_xts_sec_nx(ks_tweak, ks_crypt, ENCRYPT, cluster, tweak, true, sector_in_cluster, bis_cache->dma_buff, buff, count * EMMC_BLOCKSIZE))
		return 1; // Encryption error.

	// If not reading from cache, do a regular read and decrypt.
	if (bis_file_path)
		res = _nx_emmc_bis_file_write(system_part->lba_start + sector, count, bis_cache->dma_buff);
	else if (!emu_offset)
		res = emmc_part_write(system_part, sector, count, bis_cache->dma_buff);
	else
		res = sdmmc_storage_write(&sd_storage, emu_offset + system_part->lba_start + sector, count, bis_cache->dma_buff);
	if (res)
		return 1; // R/W error.

	// Mark cache entry not dirty if write succeeds.
	if (is_cached)
	{
		bis_cache->clusters[lookup_idx].dirty = false;
		bis_cache->dirty_cnt--;
	}

	return 0; // Success.
}

static void _nx_emmc_bis_cluster_cache_init(bool enable_cache)
{
	u32 cache_lookup_tbl_size = (system_part->lba_end - system_part->lba_start + 1) / BIS_CLUSTER_SECTORS * sizeof(*cache_lookup_tbl);

	// Clear cache header.
	memset(bis_cache, 0, sizeof(bis_cache_t));

	// Clear cluster lookup table.
	memset(cache_lookup_tbl, BIS_CACHE_LOOKUP_TBL_EMPTY_ENTRY, cache_lookup_tbl_size);

	// Enable cache.
	bis_cache->enabled = enable_cache;
}

static void _nx_emmc_bis_flush_cache()
{
	if (!bis_cache->enabled || !bis_cache->dirty_cnt)
		return;

	for (u32 i = 0; i < bis_cache->top_idx && bis_cache->dirty_cnt; i++)
	{
		if (bis_cache->clusters[i].dirty) {
			nx_emmc_bis_write_block(bis_cache->clusters[i].cluster_idx * BIS_CLUSTER_SECTORS, BIS_CLUSTER_SECTORS, NULL, true);
			bis_cache->dirty_cnt--;
		}
	}

	_nx_emmc_bis_cluster_cache_init(true);
}

static int nx_emmc_bis_read_block_normal(u32 sector, u32 count, void *buff)
{
	static u32 prev_cluster = -1;
	static u32 prev_sector = 0;
	static u8  tweak[SE_KEY_128_SIZE] __attribute__((aligned(4)));

	int  res;
	bool regen_tweak = true;
	u32  tweak_exp = 0;
	u32  cluster = sector / BIS_CLUSTER_SECTORS;
	u32  sector_in_cluster = sector % BIS_CLUSTER_SECTORS;

	// If not reading from cache, do a regular read and decrypt.
	if (bis_file_path)
		res = _nx_emmc_bis_file_read(system_part->lba_start + sector, count, bis_cache->dma_buff);
	else if (!emu_offset)
		res = emmc_part_read(system_part, sector, count, bis_cache->dma_buff);
	else
		res = sdmmc_storage_read(&sd_storage, emu_offset + system_part->lba_start + sector, count, bis_cache->dma_buff);
	if (res)
		return 1; // R/W error.

	if (prev_cluster != cluster) // Sector in different cluster than last read.
	{
		prev_cluster = cluster;
		tweak_exp = sector_in_cluster;
	}
	else if (sector > prev_sector) // Sector in same cluster and past last sector.
	{
		// Calculates the new tweak using the saved one, reducing expensive _gf256_mul_x_le calls.
		tweak_exp = sector - prev_sector - 1;
		regen_tweak = false;
	}
	else // Sector in same cluster and before or same as last sector.
		tweak_exp = sector_in_cluster;

	// Maximum one cluster (1 XTS crypto block 16KB).
	if (se_aes_crypt_xts_sec_nx(ks_tweak, ks_crypt, DECRYPT, prev_cluster, tweak, regen_tweak, tweak_exp, buff, bis_cache->dma_buff, count * EMMC_BLOCKSIZE))
		return 1; // R/W error.

	prev_sector = sector + count - 1;

	return 0; // Success.
}

static int nx_emmc_bis_read_block_cached(u32 sector, u32 count, void *buff)
{
	int res;
	u8  cache_tweak[SE_KEY_128_SIZE] __attribute__((aligned(4)));
	u32 cluster = sector / BIS_CLUSTER_SECTORS;
	u32 cluster_sector = cluster * BIS_CLUSTER_SECTORS;
	u32 sector_in_cluster = sector % BIS_CLUSTER_SECTORS;
	u32 lookup_idx = cache_lookup_tbl[cluster];

	// Read from cached cluster.
	if (lookup_idx != (u32)BIS_CACHE_LOOKUP_TBL_EMPTY_ENTRY)
	{
		memcpy(buff, bis_cache->clusters[lookup_idx].data + sector_in_cluster * EMMC_BLOCKSIZE, count * EMMC_BLOCKSIZE);

		return 0; // Success.
	}

	// Flush cache if full.
	if (bis_cache->top_idx >= BIS_CACHE_MAX_ENTRIES)
		_nx_emmc_bis_flush_cache();

	// Set new cached cluster parameters.
	bis_cache->clusters[bis_cache->top_idx].cluster_idx = cluster;
	bis_cache->clusters[bis_cache->top_idx].dirty = false;
	cache_lookup_tbl[cluster] = bis_cache->top_idx;

	// Read the whole cluster the sector resides in.
	if (bis_file_path)
		res = _nx_emmc_bis_file_read(system_part->lba_start + cluster_sector, BIS_CLUSTER_SECTORS, bis_cache->dma_buff);
	else if (!emu_offset)
		res = emmc_part_read(system_part, cluster_sector, BIS_CLUSTER_SECTORS, bis_cache->dma_buff);
	else
		res = sdmmc_storage_read(&sd_storage, emu_offset + system_part->lba_start + cluster_sector, BIS_CLUSTER_SECTORS, bis_cache->dma_buff);
	if (res)
		return 1; // R/W error.

	// Decrypt cluster.
	if (se_aes_crypt_xts_sec_nx(ks_tweak, ks_crypt, DECRYPT, cluster, cache_tweak, true, 0, bis_cache->dma_buff, bis_cache->dma_buff, BIS_CLUSTER_SIZE))
		return 1; // Decryption error.

	// Copy to cluster cache.
	memcpy(bis_cache->clusters[bis_cache->top_idx].data, bis_cache->dma_buff, BIS_CLUSTER_SIZE);
	memcpy(buff, bis_cache->dma_buff + sector_in_cluster * EMMC_BLOCKSIZE, count * EMMC_BLOCKSIZE);

	// Increment cache count.
	bis_cache->top_idx++;

	return 0; // Success.
}

static int nx_emmc_bis_read_block(u32 sector, u32 count, void *buff)
{
	if (!system_part)
		return 3; // Not ready.

	if (bis_cache->enabled)
		return nx_emmc_bis_read_block_cached(sector, count, buff);
	else
		return nx_emmc_bis_read_block_normal(sector, count, buff);
}

int nx_emmc_bis_read(u32 sector, u32 count, void *buff)
{
	u8 *buf = (u8 *)buff;
	u32 curr_sct = sector;

	while (count)
	{
		// Get sector index in cluster and use it as boundary check.
		u32 cnt_max = (curr_sct % BIS_CLUSTER_SECTORS);
		cnt_max = BIS_CLUSTER_SECTORS - cnt_max;

		u32 sct_cnt = MIN(count, cnt_max); // Only allow cluster sized access.

		if (nx_emmc_bis_read_block(curr_sct, sct_cnt, buf))
			return 1;

		count    -= sct_cnt;
		curr_sct += sct_cnt;
		buf      += sct_cnt * EMMC_BLOCKSIZE;
	}

	return 0;
}

int nx_emmc_bis_write(u32 sector, u32 count, void *buff)
{
	u8 *buf = (u8 *)buff;
	u32 curr_sct = sector;

	while (count)
	{
		// Get sector index in cluster and use it as boundary check.
		u32 cnt_max = (curr_sct % BIS_CLUSTER_SECTORS);
		cnt_max = BIS_CLUSTER_SECTORS - cnt_max;

		u32 sct_cnt = MIN(count, cnt_max); // Only allow cluster sized access.

		if (nx_emmc_bis_write_block(curr_sct, sct_cnt, buf, false))
			return 1;

		count    -= sct_cnt;
		curr_sct += sct_cnt;
		buf      += sct_cnt * EMMC_BLOCKSIZE;
	}

	return 0;
}

static void _nx_emmc_bis_file_close()
{
	if (bis_file_fp_open)
	{
		f_close(&bis_file_fp);
		bis_file_fp_open = false;
	}
}

// Open the part file that contains the sector, if not open already.
// The handle is cached and re-opened only when the part index or mode changes.
static int _nx_emmc_bis_file_seek(u32 sector, u32 count, bool write)
{
	u32 file_part = sector / bis_file_part_size;
	u32 file_sector = sector % bis_file_part_size;

	// Re-open when switching part files or modes. FatFs handles cannot switch
	// between read and write without re-opening.
	if (!bis_file_fp_open || bis_file_fp_part != file_part || bis_file_fp_write != write)
	{
		_nx_emmc_bis_file_close();

		// Set part file index. Path is always null terminated with 2 digit index.
		if (file_part >= 10)
			itoa(file_part, bis_file_path + strlen(bis_file_path) - 2, 10);
		else
		{
			bis_file_path[strlen(bis_file_path) - 2] = '0';
			itoa(file_part, bis_file_path + strlen(bis_file_path) - 1, 10);
		}

		if (f_open(&bis_file_fp, bis_file_path, write ? FA_WRITE : FA_READ))
			return 1;

		bis_file_fp_open = true;
		bis_file_fp_part = file_part;
		bis_file_fp_write = write;
	}

	// Skip the seek if the file pointer is already at the target. Cheap win
	// for the common sequential access pattern.
	if (f_tell(&bis_file_fp) != ((u64)file_sector << 9) &&
		f_lseek(&bis_file_fp, (u64)file_sector << 9))
	{
		_nx_emmc_bis_file_close();
		return 1;
	}

	return 0;
}

static int _nx_emmc_bis_file_read(u32 sector, u32 count, void *buff)
{
	UINT read_bytes = 0;

	if (_nx_emmc_bis_file_seek(sector, count, false))
		return 1;

	if (f_read(&bis_file_fp, buff, (u64)count << 9, &read_bytes) || read_bytes != count << 9)
	{
		_nx_emmc_bis_file_close();
		return 1;
	}

	return 0;
}

static int _nx_emmc_bis_file_write(u32 sector, u32 count, void *buff)
{
	UINT written_bytes = 0;

	if (_nx_emmc_bis_file_seek(sector, count, true))
		return 1;

	if (f_write(&bis_file_fp, buff, (u64)count << 9, &written_bytes) || written_bytes != count << 9)
	{
		_nx_emmc_bis_file_close();
		return 1;
	}

	return 0;
}

void nx_emmc_bis_init(emmc_part_t *part, bool enable_cache, u32 emummc_offset)
{
	// Reset any previous file backed mode.
	_nx_emmc_bis_file_close();
	if (bis_file_path)
	{
		free(bis_file_path);
		bis_file_path = NULL;
	}
	bis_file_part_size = 0;

	system_part = part;
	emu_offset = emummc_offset;

	_nx_emmc_bis_cluster_cache_init(enable_cache);

	if (!strcmp(part->name, "PRODINFO") || !strcmp(part->name, "PRODINFOF"))
	{
		ks_crypt = 0;
		ks_tweak = 1;
	}
	else if (!strcmp(part->name, "SAFE"))
	{
		ks_crypt = 2;
		ks_tweak = 3;
	}
	else if (!strcmp(part->name, "SYSTEM") || !strcmp(part->name, "USER"))
	{
		ks_crypt = 4;
		ks_tweak = 5;
	}
	else
		system_part = NULL;
}

void nx_emmc_bis_init_file(emmc_part_t *part, bool enable_cache, const char *path, u32 part_size)
{
	// Reset any previous session. Also handles key selection.
	nx_emmc_bis_init(part, enable_cache, 0);
	if (!system_part)
		return;

	// Copy path and append part file index.
	u32 len = strlen(path);
	char *new_path = malloc(len + 4);
	if (!new_path)
	{
		system_part = NULL;
		return;
	}
	strcpy(new_path, path);
	if (len && new_path[len - 1] != '/')
		strcat(new_path, "/");
	strcat(new_path, "00");

	bis_file_path = new_path;
	bis_file_part_size = part_size;
}

void nx_emmc_bis_end()
{
	_nx_emmc_bis_flush_cache();
	system_part = NULL;

	_nx_emmc_bis_file_close();
	if (bis_file_path)
	{
		free(bis_file_path);
		bis_file_path = NULL;
	}
	bis_file_part_size = 0;
}
