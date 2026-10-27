/* ============================================================================
 * ex_native_gpu.c  --  H5Dwrite / H5Dread straight from / into GPU memory
 *
 *     ./ex_native_gpu [codec] [options-json] [domain]
 *     ./ex_native_gpu cuszp '{"pressio:abs":1e-3}'              # CUDA build
 *     ./ex_native_gpu sz3   '{"pressio:abs":1e-3}' malloc       # host stand-in
 *
 * ex_roundtrip.c shows what a filter could do until now: HDF5 hands it a HOST
 * chunk, so a GPU codec pays host->device before and device->host after every
 * chunk, and the application must first bring its data to the host at all.
 *
 * Here the application's buffer is a device pointer and stays one:
 *
 *   H5Pset_comp_native_buffer(dxpl, "cudamalloc");
 *   H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, device_ptr);
 *
 *   H5Dwrite  ->  HDF5 offers the request to the filter (dset_write) before
 *                 touching the buffer
 *             ->  per compression unit: LibPressio compresses device_ptr+offset
 *                 in place, in domain "cudamalloc"            [no H2D of the data]
 *             ->  only the COMPRESSED bytes come to the host   [small D2H]
 *             ->  H5Dwrite_chunk stores them in the dataset's own chunks
 *
 * H5Dread is the mirror image: compressed bytes go up, the codec decodes on
 * the device into the application's device buffer. That is the VOL adaptor's
 * device-resident path, in a file that is still a plain filtered dataset: a
 * host program reads it with a normal H5Dread (shown at the end), and so do
 * h5dump / h5py with the plugin on HDF5_PLUGIN_PATH.
 *
 * Without CUDA this program runs the identical code path with a host buffer
 * declared as domain "malloc", so the plumbing can be checked anywhere; the
 * statistics printed below ("in place", "staged 0 B") are what to look at.
 *
 * Requirements of the native path (the filter reports each one if violated):
 *   - memory datatype == dataset datatype (HDF5 cannot convert on the device)
 *   - whole compression units are selected
 *   - each unit is one contiguous block of the buffer: use chunks (or
 *     "h5z:group" units) that span all but the first dimension
 * ==========================================================================*/
#include "hdf5.h"
#include "H5Zpressio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef EX_HAVE_CUDA
#include <cuda_runtime.h>
#define EX_DEFAULT_DOMAIN "cudamalloc"
#define EX_DEFAULT_CODEC  "cuszp"
#else
#define EX_DEFAULT_DOMAIN "malloc"
#define EX_DEFAULT_CODEC  "sz3"
#endif

#define NZ 64
#define NY 96
#define NX 96
#define CZ 16 /* chunk = a 16-plane slab: contiguous in the buffer */
#define FILENAME "ex_native_gpu.h5"
#define NBYTES   (sizeof(float) * NZ * NY * NX)

/* "Device" allocation helpers: CUDA when available, plain host memory otherwise. */
static void *
dev_alloc(size_t n)
{
#ifdef EX_HAVE_CUDA
    void *p = NULL;
    return cudaMalloc(&p, n) == cudaSuccess ? p : NULL;
#else
    return malloc(n);
#endif
}
static void
dev_free(void *p)
{
#ifdef EX_HAVE_CUDA
    cudaFree(p);
#else
    free(p);
#endif
}
static void
dev_upload(void *dev, const void *host, size_t n)
{
#ifdef EX_HAVE_CUDA
    cudaMemcpy(dev, host, n, cudaMemcpyHostToDevice);
#else
    memcpy(dev, host, n);
#endif
}
static void
dev_download(void *host, const void *dev, size_t n)
{
#ifdef EX_HAVE_CUDA
    cudaMemcpy(host, dev, n, cudaMemcpyDeviceToHost);
#else
    memcpy(host, dev, n);
#endif
}

static double
max_err(const float *a, const float *b)
{
    double m = 0;
    for (size_t i = 0; i < (size_t)NZ * NY * NX; i++) {
        double e = fabs((double)a[i] - (double)b[i]);
        if (!(e <= m))
            m = e;
    }
    return m;
}

static void
show(const char *what)
{
    H5Z_comp_io_stats_t st;
    H5Zcomp_get_io_stats(&st);
    printf("  %-22s %s, %s: %llu units, %llu B of data, %llu B to/from the file, staged on host %llu B\n",
           what, st.handled ? "dataset-level path" : "per-chunk path",
           st.native ? "buffer used in place in its domain" : "host buffer", st.units, st.bytes_raw,
           st.bytes_io, st.bytes_staged);
}

int
main(int argc, char **argv)
{
    const char   *codec   = (argc > 1) ? argv[1] : EX_DEFAULT_CODEC;
    const char   *json    = (argc > 2) ? argv[2] : "{\"pressio:abs\":1e-3}";
    const char   *domain  = (argc > 3) ? argv[3] : EX_DEFAULT_DOMAIN;
    const hsize_t dims[3] = {NZ, NY, NX}, chunk[3] = {CZ, NY, NX};
    float        *host    = (float *)malloc(NBYTES), *check = (float *)malloc(NBYTES);
    void         *dev_w, *dev_r;
    double        e_dev, e_host;

    for (int k = 0; k < NZ; k++)
        for (int j = 0; j < NY; j++)
            for (int i = 0; i < NX; i++)
                host[((size_t)k * NY + j) * NX + i] =
                    (float)(sin(0.07 * i) * cos(0.05 * j) * sin(0.09 * k + 0.3) + 0.002 * (i + j + k));

    if (H5Z_register_comp() < 0) {
        fprintf(stderr, "register failed\n");
        return 1;
    }
    if (!H5Zcomp_has_dset_io()) {
        printf("SKIP: this build has no dataset-level filter callbacks (native buffers unavailable)\n");
        return 77;
    }

    /* The application's data lives on the device. */
    dev_w = dev_alloc(NBYTES);
    dev_r = dev_alloc(NBYTES);
    if (!dev_w || !dev_r) {
        fprintf(stderr, "device allocation failed\n");
        return 1;
    }
    dev_upload(dev_w, host, NBYTES);
    printf("codec '%s' %s, buffer domain '%s'%s\n", codec, json, domain,
#ifdef EX_HAVE_CUDA
           " (CUDA device memory)"
#else
           " (host stand-in: built without CUDA)"
#endif
    );

    hid_t file  = H5Fcreate(FILENAME, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hid_t space = H5Screate_simple(3, dims, NULL);
    hid_t dcpl  = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_chunk(dcpl, 3, chunk);
    if (H5Pset_comp(dcpl, codec, json) < 0) {
        fprintf(stderr, "H5Pset_comp failed\n");
        return 1;
    }
    hid_t dset = H5Dcreate2(file, "field", H5T_NATIVE_FLOAT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
    if (dset < 0) {
        fprintf(stderr, "H5Dcreate failed (is codec '%s' in your LibPressio?)\n", codec);
        return 1;
    }

    /* The only new line an application needs: say where the buffer lives. */
    hid_t dxpl = H5Pcreate(H5P_DATASET_XFER);
    H5Pset_comp_native_buffer(dxpl, domain);

    if (H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, dev_w) < 0) {
        fprintf(stderr, "H5Dwrite from the native buffer failed\n");
        return 1;
    }
    show("write (device buffer)");
    printf("  stored %llu B, ratio %.2fx\n", (unsigned long long)H5Dget_storage_size(dset),
           (double)NBYTES / (double)H5Dget_storage_size(dset));
    H5Dclose(dset);
    H5Fclose(file);

    /* Read back into another device buffer. */
    file = H5Fopen(FILENAME, H5F_ACC_RDONLY, H5P_DEFAULT);
    dset = H5Dopen2(file, "field", H5P_DEFAULT);
    if (H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, dev_r) < 0) {
        fprintf(stderr, "H5Dread into the native buffer failed\n");
        return 1;
    }
    show("read  (device buffer)");
    dev_download(check, dev_r, NBYTES);
    e_dev = max_err(host, check);

    /* The same dataset through a plain host H5Dread: the file is an ordinary
     * filtered dataset, so the classic per-chunk filter decodes it. */
    memset(check, 0, NBYTES);
    if (H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, check) < 0) {
        fprintf(stderr, "plain host H5Dread failed\n");
        return 1;
    }
    show("read  (plain host)");
    e_host = max_err(host, check);

    printf("max abs error: device read %.3e, host read %.3e\n", e_dev, e_host);

    H5Pclose(dxpl);
    H5Pclose(dcpl);
    H5Sclose(space);
    H5Dclose(dset);
    H5Fclose(file);
    dev_free(dev_w);
    dev_free(dev_r);
    free(host);
    free(check);
    {
        int ok = isfinite(e_dev) && isfinite(e_host) && e_dev < 1.0 && e_host < 1.0;
        printf("%s\n", ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
}
