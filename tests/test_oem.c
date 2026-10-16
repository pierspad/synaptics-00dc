#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "syna_oem.h"

static void fixture(const char *dir, const char *name, size_t size, int first)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    assert(f);
    for (size_t i = 0; i < size; i++) fputc(i == 0 ? first : 0, f);
    assert(fclose(f) == 0);
}

int main(void)
{
    SynaOemData oem;
    /* Pairing is allowed only when the complete credential set is absent. */
    {
        char pair_dir[] = "/tmp/rea-fs-pairing-XXXXXX";
        char key_path[1024], victim_path[1024], empty_target[1024], dir_link[1024];
        assert(mkdtemp(pair_dir));
        assert(syna_oem_pairing_allowed(pair_dir));
        snprintf(empty_target, sizeof(empty_target), "%s/empty", pair_dir);
        snprintf(dir_link, sizeof(dir_link), "%s/dir-link", pair_dir);
        assert(mkdir(empty_target, 0700) == 0);
        assert(symlink(empty_target, dir_link) == 0);
        assert(!syna_oem_pairing_allowed(dir_link));
        assert(unlink(dir_link) == 0);
        assert(rmdir(empty_target) == 0);
        fixture(pair_dir, "host_key.pem", 1, 'x');
        assert(!syna_oem_pairing_allowed(pair_dir));
        fixture(pair_dir, "host_cert.bin", 1, 0);
        fixture(pair_dir, "server_ecdh_pub.bin", 1, 4);
        assert(!syna_oem_pairing_allowed(pair_dir)); /* Present but corrupt is not first boot. */
        snprintf(key_path, sizeof(key_path), "%s/host_key.pem", pair_dir);
        snprintf(victim_path, sizeof(victim_path), "%s/missing", pair_dir);
        assert(unlink(key_path) == 0);
        assert(symlink(victim_path, key_path) == 0);
        assert(!syna_oem_pairing_allowed(pair_dir));
        assert(unlink(key_path) == 0);
        assert(unlink(victim_path) != 0);
        snprintf(key_path, sizeof(key_path), "%s/host_cert.bin", pair_dir);
        assert(unlink(key_path) == 0);
        snprintf(key_path, sizeof(key_path), "%s/server_ecdh_pub.bin", pair_dir);
        assert(unlink(key_path) == 0);
        assert(rmdir(pair_dir) == 0);
    }
    /* Explicit directories must not silently use this computer's real keys. */
    assert(syna_oem_load("/nonexistent/rea-fs-test-credentials", &oem) != 0);
    char dir[] = "/tmp/rea-fs-oem-XXXXXX";
    assert(mkdtemp(dir));
    fixture(dir, "host_key.pem", 32, 'x'); /* Synthetic loader input, not a usable key. */
    fixture(dir, "host_cert.bin", 184, 0);
    fixture(dir, "server_ecdh_pub.bin", 65, 4);
    fixture(dir, "capture_init.bin", 4676, 2);
    fixture(dir, "capture_packet.bin", 1917, 2);
    assert(syna_oem_load(dir, &oem) == 0);
    assert(oem.host_cert_len == 184 && oem.server_ecdh_pub_len == 65);
    assert(oem.capture_init_blob_len == 4676 && oem.capture_packet_len == 1917);
    syna_oem_free(&oem);

    /* Test compiled-in ROM 6.20 capture tables fallback when only keys/certs are on disk */
    {
        char p_init[1024], p_pkt[1024];
        snprintf(p_init, sizeof(p_init), "%s/capture_init.bin", dir);
        snprintf(p_pkt, sizeof(p_pkt), "%s/capture_packet.bin", dir);
        assert(unlink(p_init) == 0);
        assert(unlink(p_pkt) == 0);
        assert(syna_oem_load(dir, &oem) == 0);
        assert(oem.capture_init_blob_len == 4676 && oem.capture_init_blob[0] == 2);
        assert(oem.capture_packet_len == 1917 && oem.capture_packet[0] == 2);
        syna_oem_free(&oem);
        fixture(dir, "capture_init.bin", 4676, 2);
        fixture(dir, "capture_packet.bin", 1917, 2);
    }

    /* A present but malformed capture table must not select built-in fallback. */
    {
        fixture(dir, "capture_init.bin", 4, 2);
        assert(syna_oem_load(dir, &oem) != 0);
        fixture(dir, "capture_init.bin", 4676, 2);
    }

    /* Test syna_oem_save_credentials round-trip */
    {
        uint8_t dummy_cert[184] = { 0 };
        uint8_t dummy_pub[65] = { 0x04 };
        char tmp_path[1024], victim_path[1024];
        snprintf(tmp_path, sizeof(tmp_path), "%s/host_key.pem.tmp", dir);
        snprintf(victim_path, sizeof(victim_path), "%s/victim", dir);
        FILE *victim = fopen(victim_path, "wb");
        assert(victim);
        assert(fwrite("protected", 1, 9, victim) == 9);
        assert(fclose(victim) == 0);
        assert(symlink(victim_path, tmp_path) == 0);
        assert(syna_oem_save_credentials(dir, "-----BEGIN PRIVATE KEY-----\ntest\n-----END PRIVATE KEY-----\n",
                                         dummy_cert, sizeof(dummy_cert),
                                         dummy_pub, sizeof(dummy_pub)) == 0);
        struct stat st;
        assert(lstat(tmp_path, &st) == 0 && S_ISLNK(st.st_mode));
        victim = fopen(victim_path, "rb");
        assert(victim);
        char preserved[10] = {0};
        assert(fread(preserved, 1, 9, victim) == 9);
        assert(fclose(victim) == 0);
        assert(strcmp(preserved, "protected") == 0);
        assert(unlink(victim_path) == 0);
        assert(syna_oem_load(dir, &oem) == 0);
        assert(oem.host_cert_len == 184 && oem.server_ecdh_pub_len == 65);
        syna_oem_free(&oem);
    }

    /* Saving through a symlinked destination directory must fail without writing there. */
    {
        char target[] = "/tmp/rea-fs-oem-target-XXXXXX";
        char linkdir[1024], escaped[1024];
        assert(mkdtemp(target));
        snprintf(linkdir, sizeof(linkdir), "%s/linkdir", dir);
        snprintf(escaped, sizeof(escaped), "%s/host_key.pem", target);
        assert(symlink(target, linkdir) == 0);
        uint8_t dummy_cert[184] = { 0 };
        uint8_t dummy_pub[65] = { 0x04 };
        assert(syna_oem_save_credentials(linkdir, "synthetic", dummy_cert, sizeof(dummy_cert),
                                         dummy_pub, sizeof(dummy_pub)) != 0);
        assert(access(escaped, F_OK) != 0);
        assert(unlink(linkdir) == 0);
        assert(rmdir(target) == 0);
    }

    fixture(dir, "host_cert.bin", 2, 0);
    assert(syna_oem_load(dir, &oem) != 0);
    const char *files[] = {"host_key.pem", "host_cert.bin", "server_ecdh_pub.bin", "capture_init.bin", "capture_packet.bin"};
    for (size_t i = 0; i < 5; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        assert(unlink(path) == 0);
    }
    {
        char victim_path[1024];
        snprintf(victim_path, sizeof(victim_path), "%s/victim", dir);
        assert(access(victim_path, F_OK) != 0);
    }
    {
        char tmp_path[1024];
        snprintf(tmp_path, sizeof(tmp_path), "%s/host_key.pem.tmp", dir);
        assert(unlink(tmp_path) == 0);
    }
    assert(rmdir(dir) == 0);
    return 0;
}
