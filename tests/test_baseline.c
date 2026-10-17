/* Exercise the production cache helpers; unused USB operations are discarded. */
#include "../src/syna_device.c"
#include <glib/gstdio.h>

int main(void)
{
    char dir[] = "/tmp/syna-baseline-XXXXXX";
    g_assert_nonnull(mkdtemp(dir));
    size_t n = SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES;
    float *data = g_new0(float, n), *out = g_new0(float, n);
    for (size_t i = 0; i < n; i++) data[i] = 100.0f;
    char *path = get_baseline_cache_path(dir);
    syna_save_baseline_cache(dir, data);
    g_assert_true(syna_load_baseline_cache(dir, out));
    g_assert_cmpmem(data, n * sizeof(float), out, n * sizeof(float));
    struct stat st;
    g_assert_cmpint(stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);

    FILE *f = fopen(path, "ab"); g_assert_nonnull(f);
    fputc(1, f); fclose(f);
    g_assert_false(syna_load_baseline_cache(dir, out));

    data[0] = NAN;
    f = fopen(path, "wb"); g_assert_nonnull(f);
    g_assert_cmpuint(fwrite(data, sizeof(float), n, f), ==, n); fclose(f);
    g_assert_false(syna_load_baseline_cache(dir, out));
    data[0] = 100.0f;
    syna_save_baseline_cache(dir, data);
    g_assert_true(syna_load_baseline_cache(dir, out));
    g_assert_cmpint(truncate(path, 8), ==, 0);
    g_assert_false(syna_load_baseline_cache(dir, out));

    /* A stale predictable temp symlink must never be opened/truncated. */
    char *victim = g_build_filename(dir, "untouched", NULL);
    g_assert_true(g_file_set_contents(victim, "safe", 4, NULL));
    char *old_tmp = g_strdup_printf("%s.tmp.%d", path, (int)getpid());
    g_assert_cmpint(symlink(victim, old_tmp), ==, 0);
    syna_save_baseline_cache(dir, data);
    gchar *contents = NULL; gsize len;
    g_assert_true(g_file_get_contents(victim, &contents, &len, NULL));
    g_assert_cmpuint(len, ==, 4); g_assert_cmpmem(contents, len, "safe", 4);
    g_free(contents);
    unlink(old_tmp); unlink(victim); unlink(path); rmdir(dir);
    g_free(old_tmp); g_free(victim); g_free(path); g_free(data); g_free(out);

    /* Verify class features and empty-gallery identify contract (no GError on empty gallery). */
    FpDeviceClass *dev_class = g_type_class_ref(FPI_TYPE_DEVICE_SYNA_00DC);
    g_assert_nonnull(dev_class);
    g_assert_true((dev_class->features & FP_DEVICE_FEATURE_IDENTIFY) != 0);
    g_assert_true((dev_class->features & FP_DEVICE_FEATURE_VERIFY) != 0);
    g_assert_true((dev_class->features & FP_DEVICE_FEATURE_DUPLICATES_CHECK) != 0);

    FpiDeviceSyna00dc *dev = g_object_new(FPI_TYPE_DEVICE_SYNA_00DC, NULL);
    GTask *empty_task = g_task_new(dev, NULL, NULL, NULL);
    GPtrArray *empty_entries = g_ptr_array_new_with_free_func(syna_identify_entry_free);
    syna_identify_thread(empty_task, dev, empty_entries, NULL);
    GError *id_err = NULL;
    gpointer matched = g_task_propagate_pointer(empty_task, &id_err);
    g_assert_no_error(id_err);
    g_assert_null(matched);
    g_ptr_array_unref(empty_entries);
    g_object_unref(empty_task);
    g_object_unref(dev);
    g_type_class_unref(dev_class);

    return 0;
}
