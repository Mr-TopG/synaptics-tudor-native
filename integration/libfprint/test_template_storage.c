/* SPDX-License-Identifier: MIT */
/* Exercise the exact lab storage functions with a synthetic FpPrint. */
int tudor_matching_lab_entry(int argc, char **argv);
#define main tudor_matching_lab_entry
#include "matching_lab.c"
#undef main
#include "fpi-print.h"
#include <glib/gstdio.h>

int main(void)
{
    g_autoptr(GError) error=NULL;
    g_autofree char *dir=g_dir_make_tmp("tudor-storage-test.XXXXXX",&error);
    g_assert_no_error(error); g_assert_nonnull(dir);
    g_autofree char *path=g_build_filename(dir,"template.fprint",NULL);
    g_autofree char *alias=g_build_filename(dir,"alias",NULL);
    g_autoptr(FpPrint) print=g_object_new(FP_TYPE_PRINT,"driver","tudor_native_lab","device-id","0",NULL);
    g_object_ref_sink(print);
    fpi_print_set_type(print,FPI_PRINT_RAW);
    g_object_set(print,"fpi-data",g_variant_new("(s)","synthetic-storage-test"),NULL);
    g_assert(save_print(print,path,&error)); g_assert_no_error(error);
    struct stat st; g_assert_cmpint(stat(path,&st),==,0); g_assert_cmpint(st.st_mode&0777,==,0600);
    g_autoptr(FpPrint) loaded=load_print(path,&error);
    g_assert_no_error(error); g_assert_nonnull(loaded); g_assert(fp_print_equal(print,loaded));
    g_assert(!save_print(print,path,&error)); g_assert_error(error,G_IO_ERROR,G_IO_ERROR_FAILED); g_clear_error(&error);
    g_clear_object(&loaded); loaded=load_print(path,&error);
    g_assert_no_error(error); g_assert(fp_print_equal(print,loaded));
    g_assert_cmpint(symlink(path,alias),==,0);
    g_assert_null(load_print(alias,&error)); g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    g_assert(!save_print(print,alias,&error)); g_clear_error(&error);
    g_assert_cmpint(unlink(alias),==,0);
    g_assert_cmpint(link(path,alias),==,0);
    g_assert_null(load_print(path,&error)); g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    g_assert_cmpint(unlink(alias),==,0);
    g_assert_cmpint(chmod(path,0644),==,0);
    g_assert_null(load_print(path,&error)); g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    g_assert_cmpint(unlink(path),==,0);
    g_assert_cmpint(mkfifo(path,0600),==,0);
    g_assert_null(load_print(path,&error)); g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    g_assert_cmpint(unlink(path),==,0);
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600); g_assert_cmpint(fd,>=,0);
    g_assert_cmpint(ftruncate(fd,1024*1024+1),==,0); close(fd);
    g_assert_null(load_print(path,&error)); g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    g_assert_cmpint(unlink(path),==,0);
    g_assert(g_file_set_contents(path,"invalid",7,&error)); g_assert_cmpint(chmod(path,0600),==,0);
    g_assert_null(load_print(path,&error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_cmpint(unlink(path),==,0); g_assert_cmpint(g_rmdir(dir),==,0);
    g_print("Template storage passed: private roundtrip, no overwrite, symlink/hardlink/public-mode/FIFO/oversize/malformed rejection. Synthetic data only.\n");
    return 0;
}
