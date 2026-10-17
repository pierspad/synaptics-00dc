/*
 * Synaptics 06cb:00dc WBDI USB Fingerprint Driver for libfprint
 * Copyright (C) 2026 Pierpaolo Spadafora
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "syna_oem.h"
#include "syna_capture_tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

static uint8_t *
read_file (const char *dir, const char *name, size_t *out_len, int *out_error)
{
  char path[PATH_MAX];
  int n = snprintf (path, sizeof (path), "%s/%s", dir, name);

  if (n < 0 || (size_t) n >= sizeof (path))
    {
      if (out_error)
        *out_error = ENAMETOOLONG;
      return NULL;
    }

  FILE *f = fopen (path, "rb");
  if (!f)
    {
      if (out_error)
        *out_error = errno;
      return NULL;
    }

  if (fseek (f, 0, SEEK_END) != 0)
    {
      int saved = errno ? errno : EIO;
      fclose (f);
      if (out_error)
        *out_error = saved;
      return NULL;
    }
  long sz = ftell (f);
  if (sz <= 0 || sz > 1024 * 1024)
    {
      fclose (f);
      if (out_error)
        *out_error = EINVAL;
      return NULL;
    }
  if (fseek (f, 0, SEEK_SET) != 0)
    {
      int saved = errno ? errno : EIO;
      fclose (f);
      if (out_error)
        *out_error = saved;
      return NULL;
    }

  uint8_t *buf = malloc (sz + 1);
  if (!buf)
    {
      fclose (f);
      if (out_error)
        *out_error = ENOMEM;
      return NULL;
    }

  if (fread (buf, 1, sz, f) != (size_t) sz)
    {
      int saved = ferror (f) ? (errno ? errno : EIO) : EIO;
      free (buf);
      fclose (f);
      if (out_error)
        *out_error = saved;
      return NULL;
    }
  buf[sz] = 0;
  if (fclose (f) != 0)
    {
      int saved = errno ? errno : EIO;
      free (buf);
      if (out_error)
        *out_error = saved;
      return NULL;
    }

  *out_len = (size_t) sz;
  if (out_error)
    *out_error = 0;
  return buf;
}

int
syna_oem_load (const char *data_dir, SynaOemData *oem)
{
  if (!oem)
    return -1;
  memset (oem, 0, sizeof (*oem));

  const char *found_dir = data_dir;
  if (!found_dir)
    found_dir = getenv ("SYNA_TOD_DATA_DIR");
  if (!found_dir)
    found_dir = SYNA_DEFAULT_DATA_DIR;

  /* Load host key */
  oem->host_key_pem = (char *) read_file (found_dir, "host_key.pem", &oem->host_cert_len, NULL);
  if (!oem->host_key_pem)
    {
      syna_oem_free (oem);
      return -3;
    }

  /* Load host cert */
  oem->host_cert = read_file (found_dir, "host_cert.bin", &oem->host_cert_len, NULL);
  if (!oem->host_cert)
    {
      syna_oem_free (oem);
      return -4;
    }

  /* Load server ECDH pubkey (65 bytes uncompressed: 0x04 || X_be || Y_be) */
  oem->server_ecdh_pub = read_file (found_dir, "server_ecdh_pub.bin", &oem->server_ecdh_pub_len, NULL);
  if (!oem->server_ecdh_pub)
    {
      syna_oem_free (oem);
      return -5;
    }

  /* Load capture init blob (or use compiled-in ROM 6.20 table if file absent) */
  int capture_error = 0;
  oem->capture_init_blob = read_file (found_dir, "capture_init.bin", &oem->capture_init_blob_len, &capture_error);
  if (!oem->capture_init_blob)
    {
      if (capture_error != ENOENT)
        {
          syna_oem_free (oem);
          return -6;
        }
      oem->capture_init_blob = malloc (sizeof (SYNA_BUILTIN_CAPTURE_INIT));
      if (!oem->capture_init_blob)
        {
          syna_oem_free (oem);
          return -6;
        }
      memcpy (oem->capture_init_blob, SYNA_BUILTIN_CAPTURE_INIT, sizeof (SYNA_BUILTIN_CAPTURE_INIT));
      oem->capture_init_blob_len = sizeof (SYNA_BUILTIN_CAPTURE_INIT);
    }

  /* Load capture packet (or use compiled-in Module 0x2449 table if file absent) */
  oem->capture_packet = read_file (found_dir, "capture_packet.bin", &oem->capture_packet_len, &capture_error);
  if (!oem->capture_packet)
    {
      if (capture_error != ENOENT)
        {
          syna_oem_free (oem);
          return -7;
        }
      oem->capture_packet = malloc (sizeof (SYNA_BUILTIN_CAPTURE_PACKET));
      if (!oem->capture_packet)
        {
          syna_oem_free (oem);
          return -7;
        }
      memcpy (oem->capture_packet, SYNA_BUILTIN_CAPTURE_PACKET, sizeof (SYNA_BUILTIN_CAPTURE_PACKET));
      oem->capture_packet_len = sizeof (SYNA_BUILTIN_CAPTURE_PACKET);
    }

  if (oem->host_cert_len != 184 || oem->server_ecdh_pub_len != 65 ||
      oem->server_ecdh_pub[0] != 4 || oem->capture_init_blob_len != 4676 ||
      oem->capture_packet_len != 1917 || oem->capture_init_blob[0] != 2 ||
      oem->capture_packet[0] != 2)
    {
      syna_oem_free (oem);
      return -8;
    }
  return 0;
}

bool
syna_oem_pairing_allowed (const char *data_dir)
{
  const char *found_dir = data_dir;

  if (!found_dir)
    found_dir = getenv ("SYNA_TOD_DATA_DIR");
  if (!found_dir)
    found_dir = SYNA_DEFAULT_DATA_DIR;

  struct stat dir_stat;
  if (lstat (found_dir, &dir_stat) == 0)
    {
      if (!S_ISDIR (dir_stat.st_mode) || S_ISLNK (dir_stat.st_mode))
        return false;
    }
  else if (errno != ENOENT)
    {
      return false;
    }

  static const char *credential_names[] = {
    "host_key.pem",
    "host_cert.bin",
    "server_ecdh_pub.bin"
  };
  for (size_t i = 0; i < sizeof (credential_names) / sizeof (credential_names[0]); i++)
    {
      char path[PATH_MAX];
      int n = snprintf (path, sizeof (path), "%s/%s", found_dir, credential_names[i]);
      if (n < 0 || (size_t) n >= sizeof (path))
        return false;
      struct stat st;
      if (lstat (path, &st) == 0 || errno != ENOENT)
        return false;
    }
  return true;
}

static int
write_private_file (const char *dir, const char *name, const uint8_t *buf, size_t len)
{
  char path[PATH_MAX];
  char tmp_path[PATH_MAX];
  struct stat dir_stat;
  int n = snprintf (path, sizeof (path), "%s/%s", dir, name);

  if (n < 0 || (size_t) n >= sizeof (path))
    return -1;

  if (lstat (dir, &dir_stat) == 0)
    {
      if (!S_ISDIR (dir_stat.st_mode) || S_ISLNK (dir_stat.st_mode))
        return -1;
    }
  else if (errno != ENOENT || mkdir (dir, 0700) != 0)
    {
      return -1;
    }
  if (lstat (dir, &dir_stat) != 0 || !S_ISDIR (dir_stat.st_mode) || S_ISLNK (dir_stat.st_mode))
    return -1;
  if (chmod (dir, 0700) != 0)
    return -1;

  n = snprintf (tmp_path, sizeof (tmp_path), "%s/.%s.XXXXXX", dir, name);
  if (n < 0 || (size_t) n >= sizeof (tmp_path))
    return -1;
  int fd = mkstemp (tmp_path);
  if (fd < 0)
    return -1;
  if (fchmod (fd, 0600) != 0)
    {
      close (fd);
      unlink (tmp_path);
      return -2;
    }
  size_t written = 0;
  while (written < len)
    {
      ssize_t count = write (fd, buf + written, len - written);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        {
          close (fd);
          unlink (tmp_path);
          return -2;
        }
      written += (size_t) count;
    }
  if (close (fd) != 0)
    {
      unlink (tmp_path);
      return -2;
    }
  if (rename (tmp_path, path) != 0)
    {
      unlink (tmp_path);
      return -3;
    }
  return 0;
}

int
syna_oem_save_credentials (const char *data_dir,
                           const char *host_key_pem,
                           const uint8_t *host_cert, size_t host_cert_len,
                           const uint8_t *server_ecdh_pub, size_t server_ecdh_pub_len)
{
  if (!host_key_pem || !host_cert || host_cert_len != 184 ||
      !server_ecdh_pub || server_ecdh_pub_len != 65 || server_ecdh_pub[0] != 0x04)
    return -1;

  const char *target_dir = data_dir;
  if (!target_dir)
    target_dir = getenv ("SYNA_TOD_DATA_DIR");
  if (!target_dir)
    target_dir = SYNA_DEFAULT_DATA_DIR;

  if (write_private_file (target_dir, "host_key.pem", (const uint8_t *) host_key_pem, strlen (host_key_pem)) != 0)
    return -2;
  if (write_private_file (target_dir, "host_cert.bin", host_cert, host_cert_len) != 0)
    return -3;
  if (write_private_file (target_dir, "server_ecdh_pub.bin", server_ecdh_pub, server_ecdh_pub_len) != 0)
    return -4;

  return 0;
}

void
syna_oem_free (SynaOemData *oem)
{
  if (!oem)
    return;
  free (oem->host_key_pem);
  free (oem->host_cert);
  free (oem->server_ecdh_pub);
  free (oem->capture_init_blob);
  free (oem->capture_packet);
  memset (oem, 0, sizeof (*oem));
}
