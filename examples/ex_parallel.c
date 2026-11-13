/* ============================================================================
 * ex_parallel.c  --  dataset-level compression under parallel HDF5
 *
 *     mpiexec -n 4 ./ex_parallel [codec] [options-json-without-braces]
 *     mpiexec -n 4 ./ex_parallel sz3 '"pressio:abs":1e-3'
 *
 * Every rank owns a slab of SLAB planes of a (nprocs * SLAB) x NY x NX field.
 * Storage chunks are 4 planes; "h5z:group":"4,1,1" makes one compression unit
 * out of 4 chunks, i.e. exactly one rank's slab. Each rank:
 *
 *   1. compresses its own unit (on its own GPU, from a device buffer, when the
 *      codec and H5Pset_comp_native_buffer say so) -- no communication;
 *   2. stores the unit's stream in its 4 storage chunks with H5Dwrite_chunk.
 *
 * Under an MPI driver HDF5 holds those chunks and, once every rank's callback
 * has returned, writes them all together: every rank learns every chunk's
 * size, file space is allocated and the chunk index updated identically on
 * all ranks, and each rank writes only its own bytes -- the same steps as a
 * collective write of ordinary filtered chunks. The write must therefore be
 * collective (H5FD_MPIO_COLLECTIVE), and a unit must not span two ranks.
 *
 * For comparison the same field is also written with one unit per chunk,
 * through HDF5's existing parallel filtered-write path. Rank 0 finally reopens
 * the file serially and checks every rank's slab.
 * ==========================================================================*/
#include "hdf5.h"
#include "H5Zpressio.h"
#include <math.h>
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLAB     16 /* planes per rank: one compression unit */
#define NY       64
#define NX       64
#define CZ       4 /* planes per storage chunk */
#define FILENAME "ex_parallel.h5"

static float
value(size_t k, size_t j, size_t i)
{
    return (float)(sin(0.07 * (double)i) * cos(0.05 * (double)j) * sin(0.09 * (double)k + 0.3) +
                   0.002 * (double)(i + j + k));
}

static hid_t
slab_space(hid_t dset, int r)
{
    hsize_t start[3] = {(hsize_t)r * SLAB, 0, 0}, count[3] = {SLAB, NY, NX};
    hid_t   sp       = H5Dget_space(dset);

    H5Sselect_hyperslab(sp, H5S_SELECT_SET, start, NULL, count, NULL);
    return sp;
}

/* Max error of rank R's slab in BUF. */
static double
slab_err(const float *buf, int r)
{
    double m = 0;
    for (size_t k = 0; k < SLAB; k++)
        for (size_t j = 0; j < NY; j++)
            for (size_t i = 0; i < NX; i++) {
                double e = fabs((double)buf[(k * NY + j) * NX + i] - (double)value((size_t)r * SLAB + k, j, i));
                if (!(e <= m))
                    m = e;
            }
    return m;
}

int
main(int argc, char **argv)
{
    const char *codec = (argc > 1) ? argv[1] : "sz3";
    const char *opts  = (argc > 2) ? argv[2] : "\"pressio:abs\":1e-3";
    const char *names[2] = {"one_unit_per_rank", "one_unit_per_chunk"};
    char        grouped[512], plain[512];
    int         rank, nprocs, rc = 0, total = 0;
    size_t      nslab = (size_t)SLAB * NY * NX;
    float      *w, *rd;
    hid_t       fapl, file, space, mspace, xfer;
    hsize_t     stored[2];

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    if (H5Z_register_comp() < 0 || !H5Zcomp_has_dset_io()) {
        if (rank == 0)
            printf("SKIP: this build has no dataset-level filter callbacks\n");
        MPI_Finalize();
        return 77;
    }
    snprintf(grouped, sizeof(grouped), "{%s,\"h5z:group\":\"%d,1,1\"}", opts, SLAB / CZ);
    snprintf(plain, sizeof(plain), "{%s}", opts);

    w  = (float *)malloc(sizeof(float) * nslab);
    rd = (float *)malloc(sizeof(float) * nslab);
    for (size_t k = 0; k < SLAB; k++)
        for (size_t j = 0; j < NY; j++)
            for (size_t i = 0; i < NX; i++)
                w[(k * NY + j) * NX + i] = value((size_t)rank * SLAB + k, j, i);

    fapl = H5Pcreate(H5P_FILE_ACCESS);
    H5Pset_fapl_mpio(fapl, MPI_COMM_WORLD, MPI_INFO_NULL);
    file = H5Fcreate(FILENAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl);
    {
        hsize_t dims[3] = {(hsize_t)nprocs * SLAB, NY, NX}, mdims[3] = {SLAB, NY, NX};
        space  = H5Screate_simple(3, dims, NULL);
        mspace = H5Screate_simple(3, mdims, NULL);
    }
    xfer = H5Pcreate(H5P_DATASET_XFER);
    H5Pset_dxpl_mpio(xfer, H5FD_MPIO_COLLECTIVE); /* required for writes */

    for (int v = 0; v < 2; v++) {
        hsize_t chunk[3] = {CZ, NY, NX};
        hid_t   dcpl = H5Pcreate(H5P_DATASET_CREATE), dset, fsp;

        H5Pset_chunk(dcpl, 3, chunk);
        if (H5Pset_comp(dcpl, codec, v == 0 ? grouped : plain) < 0 ||
            (dset = H5Dcreate2(file, names[v], H5T_NATIVE_FLOAT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0) {
            fprintf(stderr, "[%d] %s: could not create the dataset\n", rank, names[v]);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        H5Pclose(dcpl);

        /* Each rank writes its own slab, all ranks in one collective call. */
        fsp = slab_space(dset, rank);
        if (H5Dwrite(dset, H5T_NATIVE_FLOAT, mspace, fsp, xfer, w) < 0) {
            fprintf(stderr, "[%d] %s: collective H5Dwrite failed\n", rank, names[v]);
            rc = 1;
        }
        H5Sclose(fsp);

        /* Read back the NEXT rank's slab: it was written by another process. */
        fsp = slab_space(dset, (rank + 1) % nprocs);
        memset(rd, 0, sizeof(float) * nslab);
        if (H5Dread(dset, H5T_NATIVE_FLOAT, mspace, fsp, xfer, rd) < 0 || !(slab_err(rd, (rank + 1) % nprocs) < 1.0)) {
            fprintf(stderr, "[%d] %s: reading rank %d's slab failed\n", rank, names[v], (rank + 1) % nprocs);
            rc = 1;
        }
        H5Sclose(fsp);
        stored[v] = H5Dget_storage_size(dset);
        H5Dclose(dset);
    }
    H5Pclose(xfer);
    H5Sclose(mspace);
    H5Sclose(space);
    H5Fclose(file);
    H5Pclose(fapl);

    /* A serial reader sees every rank's data. */
    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) {
        size_t raw = sizeof(float) * nslab * (size_t)nprocs;
        hid_t  f   = H5Fopen(FILENAME, H5F_ACC_RDONLY, H5P_DEFAULT);
        double emax = 0;

        for (int v = 0; v < 2; v++) {
            hid_t dset = H5Dopen2(f, names[v], H5P_DEFAULT);
            hid_t ms   = H5Screate_simple(1, (hsize_t[]){(hsize_t)nslab}, NULL);

            for (int r = 0; r < nprocs; r++) {
                hid_t  fsp = slab_space(dset, r);
                double e;

                if (H5Dread(dset, H5T_NATIVE_FLOAT, ms, fsp, H5P_DEFAULT, rd) < 0) {
                    fprintf(stderr, "serial read of %s, rank %d's slab failed\n", names[v], r);
                    rc = 1;
                }
                e = slab_err(rd, r);
                if (!(e <= emax))
                    emax = e;
                H5Sclose(fsp);
            }
            H5Sclose(ms);
            H5Dclose(dset);
        }
        H5Fclose(f);

        printf("codec '%s' %s, %d ranks x %d planes of %dx%d float, 4-plane storage chunks\n\n", codec, plain,
               nprocs, SLAB, NY, NX);
        printf("  %-20s %-34s %10s %8s\n", "dataset", "written by", "stored B", "ratio");
        printf("  %-20s %-34s %10llu %7.2fx\n", names[0], "dataset-level, 1 unit per rank",
               (unsigned long long)stored[0], (double)raw / (double)stored[0]);
        printf("  %-20s %-34s %10llu %7.2fx\n", names[1], "HDF5 parallel per-chunk pipeline",
               (unsigned long long)stored[1], (double)raw / (double)stored[1]);
        printf("\nserial re-read of every rank's slab: max err %.2e\n", emax);
        rc |= !(emax < 1.0);
    }

    MPI_Allreduce(&rc, &total, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    if (rank == 0)
        printf("\n%s\n", total ? "FAILED" : "OK");
    free(w);
    free(rd);
    MPI_Finalize();
    return total ? 1 : 0;
}
