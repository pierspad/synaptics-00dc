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

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SYNA_DEFAULT_DATA_DIR "/var/lib/fprint/syna_00dc"

typedef struct
{
  char    *host_key_pem;
  uint8_t *host_cert;
  size_t   host_cert_len;
  uint8_t *server_ecdh_pub;
  size_t   server_ecdh_pub_len;
  uint8_t *capture_init_blob;
  size_t   capture_init_blob_len;
  uint8_t *capture_packet;
  size_t   capture_packet_len;
} SynaOemData;

/**
 * Load OEM configuration and pairing credentials from directory.
 *
 * Looks in data_dir (or SYNA_DEFAULT_DATA_DIR if data_dir is NULL).
 * If files are not present in data_dir, also checks the local session directory
 * if available.
 *
 * @param data_dir Path to configuration directory, or NULL.
 * @param oem Output struct receiving allocated buffers.
 * @return 0 on success, negative error code if files are missing or unreadable.
 */
int syna_oem_load (const char  *data_dir,
                   SynaOemData *oem);

/**
 * Return true only when all pairing credential paths are absent.
 * A present, symlinked, or inaccessible path disables automatic pairing.
 */
bool syna_oem_pairing_allowed (const char *data_dir);

/**
 * Atomically persist newly paired credentials (host_key.pem, host_cert.bin, server_ecdh_pub.bin)
 * to data_dir (or SYNA_DEFAULT_DATA_DIR if NULL) with 0700 dir / 0600 file permissions.
 */
int syna_oem_save_credentials (const char    *data_dir,
                               const char    *host_key_pem,
                               const uint8_t *host_cert,
                               size_t         host_cert_len,
                               const uint8_t *server_ecdh_pub,
                               size_t         server_ecdh_pub_len);

/**
 * Free memory in SynaOemData.
 */
void syna_oem_free (SynaOemData *oem);
