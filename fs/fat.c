/* fat.c - FAT filesystem */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2000,2001,2002,2003,2004  Free Software Foundation, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <grub/fs.h>
#include <grub/disk.h>
#include <grub/file.h>
#include <grub/types.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/err.h>
#include <grub/dl.h>

#define GRUB_FAT_DIR_ENTRY_SIZE	32

#define GRUB_FAT_ATTR_READ_ONLY	0x01
#define GRUB_FAT_ATTR_HIDDEN	0x02
#define GRUB_FAT_ATTR_SYSTEM	0x04
#define GRUB_FAT_ATTR_VOLUME_ID	0x08
#define GRUB_FAT_ATTR_DIRECTORY	0x10
#define GRUB_FAT_ATTR_ARCHIVE	0x20

#define GRUB_FAT_ATTR_LONG_NAME	(GRUB_FAT_ATTR_READ_ONLY \
				 | GRUB_FAT_ATTR_HIDDEN \
				 | GRUB_FAT_ATTR_SYSTEM \
				 | GRUB_FAT_ATTR_VOLUME_ID)
#define GRUB_FAT_ATTR_VALID	(GRUB_FAT_ATTR_READ_ONLY \
				 | GRUB_FAT_ATTR_HIDDEN \
				 | GRUB_FAT_ATTR_SYSTEM \
				 | GRUB_FAT_ATTR_DIRECTORY \
				 | GRUB_FAT_ATTR_ARCHIVE)

struct grub_fat_bpb
{
  grub_uint8_t jmp_boot[3];
  grub_uint8_t oem_name[8];
  grub_uint16_t bytes_per_sector;
  grub_uint8_t sectors_per_cluster;
  grub_uint16_t num_reserved_sectors;
  grub_uint8_t num_fats;
  grub_uint16_t num_root_entries;
  grub_uint16_t num_total_sectors_16;
  grub_uint8_t media;
  grub_uint16_t sectors_per_fat_16;
  grub_uint16_t sectors_per_track;
  grub_uint16_t num_heads;
  grub_uint32_t num_hidden_sectors;
  grub_uint32_t num_total_sectors_32;
  
  /* The following fields are only used by FAT32.  */
  grub_uint32_t sectors_per_fat_32;
  grub_uint16_t extended_flags;
  grub_uint16_t fs_version;
  grub_uint32_t root_cluster;
  grub_uint16_t fs_info;
  grub_uint16_t backup_boot_sector;
} __attribute__ ((packed));

struct grub_fat_dir_entry
{
  grub_uint8_t name[11];
  grub_uint8_t attr;
  grub_uint8_t nt_reserved;
  grub_uint8_t c_time_tenth;
  grub_uint16_t c_time;
  grub_uint16_t c_date;
  grub_uint16_t a_date;
  grub_uint16_t first_cluster_high;
  grub_uint16_t w_time;
  grub_uint16_t w_date;
  grub_uint16_t first_cluster_low;
  grub_uint32_t file_size;
} __attribute__ ((packed));

struct grub_fat_long_name_entry
{
  grub_uint8_t id;
  grub_uint16_t name1[5];
  grub_uint8_t attr;
  grub_uint8_t reserved;
  grub_uint8_t checksum;
  grub_uint16_t name2[6];
  grub_uint16_t first_cluster;
  grub_uint16_t name3[2];
} __attribute__ ((packed));

struct grub_fat_data
{
  int logical_sector_bits;
  grub_uint32_t num_sectors;
  
  grub_uint16_t fat_sector;
  grub_uint32_t sectors_per_fat;
  int fat_size;
  
  grub_uint32_t root_cluster;
  grub_uint32_t root_sector;
  grub_uint32_t num_root_sectors;
  
  int cluster_bits;
  grub_uint32_t cluster_eof_mark;
  grub_uint32_t cluster_sector;
  grub_uint32_t num_clusters;

  grub_uint8_t attr;
  grub_ssize_t file_size;
  grub_uint32_t file_cluster;
  grub_uint32_t cur_cluster_num;
  grub_uint32_t cur_cluster;
};

#ifndef GRUB_UTIL
static grub_dl_t my_mod;
#endif

static int
fat_log2 (unsigned x)
{
  int i;
  
  if (x == 0)
    return -1;

  for (i = 0; (x & 1) == 0; i++)
    x >>= 1;

  if (x != 1)
    return -1;

  return i;
}

static struct grub_fat_data *
grub_fat_mount (grub_disk_t disk)
{
  struct grub_fat_bpb bpb;
  struct grub_fat_data *data = 0;
  grub_uint32_t first_fat, magic;

  if (! disk)
    goto fail;

  data = (struct grub_fat_data *) grub_malloc (sizeof (*data));
  if (! data)
    goto fail;

  /* Read the BPB.  */
  if (grub_disk_read (disk, 0, 0, sizeof (bpb), (char *) &bpb))
    goto fail;

  /* Get the sizes of logical sectors and clusters.  */
  data->logical_sector_bits =
    fat_log2 (grub_le_to_cpu16 (bpb.bytes_per_sector));
  if (data->logical_sector_bits < GRUB_DISK_SECTOR_BITS)
    goto fail;
  data->logical_sector_bits -= GRUB_DISK_SECTOR_BITS;
  
  data->cluster_bits = fat_log2 (bpb.sectors_per_cluster);
  if (data->cluster_bits < 0)
    goto fail;
  data->cluster_bits += data->logical_sector_bits;

  /* Get information about FATs.  */
  data->fat_sector = (grub_le_to_cpu16 (bpb.num_reserved_sectors)
		      << data->logical_sector_bits);
  if (data->fat_sector == 0)
    goto fail;

  data->sectors_per_fat = ((bpb.sectors_per_fat_16
			    ? grub_le_to_cpu16 (bpb.sectors_per_fat_16)
			    : grub_le_to_cpu32 (bpb.sectors_per_fat_32))
			   << data->logical_sector_bits);
  if (data->sectors_per_fat == 0)
    goto fail;

  /* Get the number of sectors in this volume.  */
  data->num_sectors = ((bpb.num_total_sectors_16
			? grub_le_to_cpu16 (bpb.num_total_sectors_16)
			: grub_le_to_cpu32 (bpb.num_total_sectors_32))
		       << data->logical_sector_bits);
  if (data->num_sectors == 0)
    goto fail;

  /* Get information about the root directory.  */
  if (bpb.num_fats == 0)
    goto fail;

  data->root_sector = data->fat_sector + bpb.num_fats * data->sectors_per_fat;
  data->num_root_sectors
    = ((((grub_uint32_t) grub_le_to_cpu16 (bpb.num_root_entries)
	 * GRUB_FAT_DIR_ENTRY_SIZE
	 + grub_le_to_cpu16 (bpb.bytes_per_sector) - 1)
	>> (data->logical_sector_bits + GRUB_DISK_SECTOR_BITS))
       << (data->logical_sector_bits));

  data->cluster_sector = data->root_sector + data->num_root_sectors;
  data->num_clusters = (((data->num_sectors - data->cluster_sector)
			 >> (data->cluster_bits + data->logical_sector_bits))
			+ 2);

  if (data->num_clusters <= 2)
    goto fail;

  if (! bpb.sectors_per_fat_16)
    {
      /* FAT32.  */
      grub_uint16_t flags = grub_le_to_cpu16 (bpb.extended_flags);
      
      data->root_cluster = grub_le_to_cpu32 (bpb.root_cluster);
      data->fat_size = 32;
      data->cluster_eof_mark = 0x0ffffff8;
      
      if (flags & 0x80)
	{
	  /* Get an active FAT.  */
	  unsigned active_fat = flags & 0xf;
	  
	  if (active_fat > bpb.num_fats)
	    goto fail;

	  data->fat_sector += active_fat * data->sectors_per_fat;
	}

      if (bpb.num_root_entries != 0 || bpb.fs_version != 0)
	goto fail;
    }
  else
    {
      /* FAT12 or FAT16.  */
      data->root_cluster = ~0UL;

      if (data->num_clusters <= 4085 + 2)
	{
	  /* FAT12.  */
	  data->fat_size = 12;
	  data->cluster_eof_mark = 0x0ff8;
	}
      else
	{
	  /* FAT16.  */
	  data->fat_size = 16;
	  data->cluster_eof_mark = 0xfff8;
	}
    }

  /* More sanity checks.  */
  if (data->num_sectors <= data->fat_sector)
    goto fail;

  if (grub_disk_read (disk,
		      data->fat_sector,
		      0,
		      sizeof (first_fat),
		      (char *) &first_fat))
    goto fail;

  first_fat = grub_le_to_cpu32 (first_fat);

  if (data->fat_size == 32)
    {
      first_fat &= 0x0fffffff;
      magic = 0x0fffff00;
    }
  else if (data->fat_size == 16)
    {
      first_fat &= 0x0000ffff;
      magic = 0xff00;
    }
  else
    {
      first_fat &= 0x00000fff;
      magic = 0x0f00;
    }
  
  if (first_fat != (magic | bpb.media))
    goto fail;

  /* Start from the root directory.  */
  data->file_cluster = data->root_cluster;
  data->cur_cluster_num = ~0UL;
  data->attr = GRUB_FAT_ATTR_DIRECTORY;
  return data;

 fail:

  grub_free (data);
  grub_error (GRUB_ERR_BAD_FS, "not a fat filesystem");
  return 0;
}

static grub_ssize_t
grub_fat_read_data (grub_disk_t disk, struct grub_fat_data *data,
		    void (*read_hook) (unsigned long sector,
				       unsigned offset, unsigned length),
		    grub_ssize_t offset, grub_ssize_t len, char *buf)
{
  grub_ssize_t size;
  grub_uint32_t logical_cluster;
  unsigned logical_cluster_bits;
  grub_ssize_t ret = 0;
  unsigned long sector;
  
  /* This is a special case. FAT12 and FAT16 doesn't have the root directory
     in clusters.  */
  if (data->file_cluster == ~0UL)
    {
      size = (data->num_root_sectors << GRUB_DISK_SECTOR_BITS) - offset;
      if (size > len)
	size = len;

      if (grub_disk_read (disk, data->root_sector, offset, size, buf))
	return -1;

      return size;
    }

  /* Calculate the logical cluster number and offset.  */
  logical_cluster_bits = (data->cluster_bits
			  + data->logical_sector_bits
			  + GRUB_DISK_SECTOR_BITS);
  logical_cluster = offset >> logical_cluster_bits;
  offset &= (1 << logical_cluster_bits) - 1;

  if (logical_cluster < data->cur_cluster_num)
    {
      data->cur_cluster_num = 0;
      data->cur_cluster = data->file_cluster;
    }

  while (len)
    {
      while (logical_cluster > data->cur_cluster_num)
	{
	  /* Find next cluster.  */
	  grub_uint32_t next_cluster;
	  unsigned long fat_offset;

	  switch (data->fat_size)
	    {
	    case 32:
	      fat_offset = data->cur_cluster << 2;
	      break;
	    case 16:
	      fat_offset = data->cur_cluster << 1;
	      break;
	    default:
	      /* case 12: */
	      fat_offset = data->cur_cluster + (data->cur_cluster >> 1);
	      break;
	    }

	  /* Read the FAT.  */
	  if (grub_disk_read (disk, data->fat_sector, fat_offset,
			      (data->fat_size + 7) >> 3,
			      (char *) &next_cluster))
	    return -1;

	  next_cluster = grub_le_to_cpu32 (next_cluster);
	  switch (data->fat_size)
	    {
	    case 16:
	      next_cluster &= 0xFFFF;
	      break;
	    case 12:
	      if (data->cur_cluster & 1)
		next_cluster >>= 4;
	      
	      next_cluster &= 0x0FFF;
	      break;
	    }

#if 0
	  grub_printf ("%s:%d: fat_size=%d, next_cluster=%u\n",
		       __FILE__, __LINE__, data->fat_size, next_cluster);
#endif
	  
	  /* Check the end.  */
	  if (next_cluster >= data->cluster_eof_mark)
	    return ret;

	  if (next_cluster < 2 || next_cluster >= data->num_clusters)
	    {
	      grub_error (GRUB_ERR_BAD_FS, "invalid cluster %u",
			  next_cluster);
	      return -1;
	    }

	  data->cur_cluster = next_cluster;
	  data->cur_cluster_num++;
	}

      /* Read the data here.  */
      sector = (data->cluster_sector
		+ ((data->cur_cluster - 2)
		   << (data->cluster_bits + data->logical_sector_bits)));
      size = (1 << logical_cluster_bits) - offset;
      if (size > len)
	size = len;

      disk->read_hook = read_hook;
      grub_disk_read (disk, sector, offset, size, buf);
      disk->read_hook = 0;
      if (grub_errno)
	return -1;

      len -= size;
      buf += size;
      ret += size;
      logical_cluster++;
      offset = 0;
    }

  return ret;
}

/* Find the underlying directory or file in PATH and return the
   next path. If there is no next path or an error occurs, return NULL.
   If HOOK is specified, call it with each file name.  */
static char *
grub_fat_find_dir (grub_disk_t disk, struct grub_fat_data *data,
		   const char *path,
		   int (*hook) (const char *filename, int dir))
{
  struct grub_fat_dir_entry dir;
  char *dirname, *dirp;
  char *filename, *filep = 0;
  grub_uint16_t *unibuf;
  int slot = -1, slots = -1;
  int checksum = -1;
  grub_ssize_t offset = -sizeof(dir);
  int call_hook;
  
  if (! (data->attr & GRUB_FAT_ATTR_DIRECTORY))
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "not a directory");
      return 0;
    }
  
  /* Extract a directory name.  */
  while (*path == '/')
    path++;

  dirp = grub_strchr (path, '/');
  if (dirp)
    {
      unsigned len = dirp - path;
      
      dirname = grub_malloc (len + 1);
      if (! dirname)
	return 0;

      grub_memcpy (dirname, path, len);
      dirname[len] = '\0';
    }
  else
    /* This is actually a file.  */
    dirname = grub_strdup (path);

  call_hook = (! dirp && hook);
  
  /* Allocate space enough to hold a long name.  */
  filename = grub_malloc (0x40 * 13 * 4 + 1);
  unibuf = (grub_uint16_t *) grub_malloc (0x40 * 13 * 2);
  if (! filename || ! unibuf)
    {
      grub_free (filename);
      grub_free (unibuf);
      grub_free (dirname);
      return 0;
    }
      
  while (1)
    {
      unsigned i;

      /* Adjust the offset.  */
      offset += sizeof (dir);

      /* Read a directory entry.  */
      if ((grub_fat_read_data (disk, data, 0,
			       offset, sizeof (dir), (char *) &dir)
	   != sizeof (dir))
	  || dir.name[0] == 0)
	{
	  if (grub_errno == GRUB_ERR_NONE && ! call_hook)
	    grub_error (GRUB_ERR_FILE_NOT_FOUND, "file not found");

	  break;
	}

      /* Handle long name entries.  */
      if (dir.attr == GRUB_FAT_ATTR_LONG_NAME)
	{
	  struct grub_fat_long_name_entry *long_name
	    = (struct grub_fat_long_name_entry *) &dir;
	  grub_uint8_t id = long_name->id;
	  
	  if (id & 0x40)
	    {
	      id &= 0x3f;
	      slots = slot = id;
	      checksum = long_name->checksum;
	    }

	  if (id != slot || slot == 0 || checksum != long_name->checksum)
	    {
	      checksum = -1;
	      continue;
	    }

	  slot--;
	  grub_memcpy (unibuf + slot * 13, long_name->name1, 5 * 2);
	  grub_memcpy (unibuf + slot * 13 + 5, long_name->name2, 6 * 2);
	  grub_memcpy (unibuf + slot * 13 + 11, long_name->name3, 2 * 2);
	  continue;
	}

      /* Check if this entry is valid.  */
      if (dir.name[0] == 0xe5 || (dir.attr & ~GRUB_FAT_ATTR_VALID))
	continue;

      /* This is a workaround for Japanese.  */
      if (dir.name[0] == 0x05)
	dir.name[0] = 0xe5;
      
      if (checksum != -1 && slot == 0)
	{
	  grub_uint8_t sum;
	  
	  for (sum = 0, i = 0; i < sizeof (dir.name); i++)
	    sum = ((sum >> 1) | (sum << 7)) + dir.name[i];

	  if (sum == checksum)
	    {
	      int u;

	      for (u = 0; u < slots * 13; u++)
		unibuf[u] = grub_le_to_cpu16 (unibuf[u]);
	      
	      *grub_utf16_to_utf8 (filename, unibuf, slots * 13) = '\0';
	      
	      if (*dirname == '\0' && call_hook)
		{
		  if (hook (filename, dir.attr & GRUB_FAT_ATTR_DIRECTORY))
		    break;
		  
		  checksum = -1;
		  continue;
		}

	      if (grub_strcmp (dirname, filename) == 0)
		{
		  if (call_hook)
		    hook (filename, dir.attr & GRUB_FAT_ATTR_DIRECTORY);
		  
		  break;
		}
	    }

	  checksum = -1;
	}

      /* Convert the 8.3 file name.  */
      filep = filename;
      
      for (i = 0; i < 8 && dir.name[i] && ! grub_isspace (dir.name[i]); i++)
	*filep++ = grub_tolower (dir.name[i]);
      
      *filep = '.';
      
      for (i = 8; i < 11 && dir.name[i] && ! grub_isspace (dir.name[i]); i++)
	*++filep = grub_tolower (dir.name[i]);

      if (*filep != '.')
	filep++;
      
      *filep = '\0';

      if (*dirname == '\0' && call_hook)
	{
	  if (hook (filename, dir.attr & GRUB_FAT_ATTR_DIRECTORY))
	    break;
	}
      else if (grub_strcmp (dirname, filename) == 0)
	{
	  if (call_hook)
	    hook (filename, dir.attr & GRUB_FAT_ATTR_DIRECTORY);

	  break;
	}
    }

  grub_free (filename);
  grub_free (dirname);

  data->attr = dir.attr;
  data->file_size = grub_le_to_cpu32 (dir.file_size);
  data->file_cluster = ((grub_le_to_cpu16 (dir.first_cluster_high) << 16)
			| grub_le_to_cpu16 (dir.first_cluster_low));
  data->cur_cluster_num = ~0UL;
  
  return dirp;
}

static grub_err_t
grub_fat_dir (grub_device_t device, const char *path,
	      int (*hook) (const char *filename, int dir))
{
  struct grub_fat_data *data = 0;
  grub_disk_t disk = device->disk;
  char *p = (char *) path;

#ifndef GRUB_UTIL
  grub_dl_ref (my_mod);
#endif
  
  data = grub_fat_mount (disk);
  if (! data)
    goto fail;

  do
    {
      p = grub_fat_find_dir (disk, data, p, hook);
    }
  while (p && grub_errno == GRUB_ERR_NONE);

 fail:
  
  grub_free (data);
  
#ifndef GRUB_UTIL
  grub_dl_unref (my_mod);
#endif
  
  return grub_errno;
}

static grub_err_t
grub_fat_open (grub_file_t file, const char *name)
{
  struct grub_fat_data *data = 0;
  char *p = (char *) name;

#ifndef GRUB_UTIL
  grub_dl_ref (my_mod);
#endif
  
  data = grub_fat_mount (file->device->disk);
  if (! data)
    goto fail;

  do
    {
      p = grub_fat_find_dir (file->device->disk, data, p, 0);
      if (grub_errno != GRUB_ERR_NONE)
	goto fail;
    }
  while (p);

  if (data->attr & GRUB_FAT_ATTR_DIRECTORY)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "not a file");
      goto fail;
    }

  file->data = data;
  file->size = data->file_size;
  
  return GRUB_ERR_NONE;

 fail:
  
  grub_free (data);
  
#ifndef GRUB_UTIL
  grub_dl_unref (my_mod);
#endif
  
  return grub_errno;
}

static grub_ssize_t
grub_fat_read (grub_file_t file, char *buf, grub_ssize_t len)
{
  return grub_fat_read_data (file->device->disk, file->data, file->read_hook,
			     file->offset, len, buf);
}

static grub_err_t
grub_fat_close (grub_file_t file)
{
  grub_free (file->data);
  
#ifndef GRUB_UTIL
  grub_dl_unref (my_mod);
#endif
  
  return grub_errno;
}

static grub_err_t
grub_fat_label (grub_device_t device, char **label)
{
  struct grub_fat_data *data;
  grub_disk_t disk = device->disk;
  grub_ssize_t offset = -sizeof(struct grub_fat_dir_entry);


#ifndef GRUB_UTIL
  grub_dl_ref (my_mod);
#endif
  
  data = grub_fat_mount (disk);
  if (! data)
    goto fail;

  if (! (data->attr & GRUB_FAT_ATTR_DIRECTORY))
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "not a directory");
      return 0;
    }

  while (1)
    {
      struct grub_fat_dir_entry dir;

      /* Adjust the offset.  */
      offset += sizeof (dir);
      
      /* Read a directory entry.  */
      if ((grub_fat_read_data (disk, data, 0,
			       offset, sizeof (dir), (char *) &dir)
	   != sizeof (dir))
	  || dir.name[0] == 0)
	{
	  if (grub_errno != GRUB_ERR_NONE)
	    goto fail;
	  else
	    {
	      *label = 0;
	      return GRUB_ERR_NONE;
	    }
	}

      if (dir.attr == GRUB_FAT_ATTR_VOLUME_ID)
	{
	  *label = grub_strndup (dir.name, 11);
	  return GRUB_ERR_NONE;
	}
    }

  *label = 0;
  
 fail:

#ifndef GRUB_UTIL
  grub_dl_unref (my_mod);
#endif

  grub_free (data);

  return grub_errno;
}

static struct grub_fs grub_fat_fs =
  {
    .name = "fat",
    .dir = grub_fat_dir,
    .open = grub_fat_open,
    .read = grub_fat_read,
    .close = grub_fat_close,
    .label = grub_fat_label,
    .next = 0
  };

#ifdef GRUB_UTIL
void
grub_fat_init (void)
{
  grub_fs_register (&grub_fat_fs);
}

void
grub_fat_fini (void)
{
  grub_fs_unregister (&grub_fat_fs);
}
#else /* ! GRUB_UTIL */
GRUB_MOD_INIT
{
  grub_fs_register (&grub_fat_fs);
  my_mod = mod;
}

GRUB_MOD_FINI
{
  grub_fs_unregister (&grub_fat_fs);
}
#endif /* ! GRUB_UTIL */
