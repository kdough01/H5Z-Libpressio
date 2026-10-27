/* ============================================================================
 * H5Zpressio.h  --  public API of the LibPressio-backed HDF5 filter (H5Zcomp)
 *
 * Link against libH5Zcomp (or declare these yourself) to attach the filter to
 * a DCPL. A reader that only needs to DECODE a small-config dataset does not
 * need this header at all: point HDF5_PLUGIN_PATH at the directory holding
 * libH5Zcomp.so and a plain H5Dread / h5dump will load the filter on demand.
 * ==========================================================================*/
#ifndef H5ZPRESSIO_H
#define H5ZPRESSIO_H

#include "hdf5.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Placeholder filter id -- register a permanent one with The HDF Group. */
#define H5Z_FILTER_COMP 33010

/* Register the filter with the library explicitly (optional when the plugin
 * is found through HDF5_PLUGIN_PATH). Safe to call more than once. */
herr_t H5Z_register_comp(void);

/* Attach the filter to a dataset creation property list.
 *   compressor_id : any LibPressio compressor id ("sz3", "zfp", "cuszp", ...)
 *   options_json  : LibPressio options as JSON, or NULL / "" for defaults
 * Configs up to ~60 KiB are stored inline in cd_values and are fully
 * self-describing. (The pipeline is one object-header message, capped at
 * 64 KiB, so the nominal 65535-value cd_values limit is not reachable.)
 * Larger configs are stored in the filter blob. A process that WROTE the
 * dataset can read it back directly; any other reader must first call
 * H5Pget_comp_json() on the dataset's DCPL (or pass the JSON with
 * H5Pset_comp_dxpl()) before H5Dread. */
herr_t H5Pset_comp(hid_t dcpl_id, const char *compressor_id, const char *options_json);

/* Large-config path only: hand the JSON to the filter for H5Dread in a
 * process that has not seen it. The JSON is copied, and is checked against the
 * content tag stored with the dataset. (Not needed for writes: the writer
 * already registered it in H5Pset_comp.) */
herr_t H5Pset_comp_dxpl(hid_t dxpl_id, const char *options_json);

/* Large-config path only: recover the JSON stored in the blob of a reopened
 * dataset's DCPL (H5Dget_create_plist) AND register it with the filter, after
 * which a plain H5Dread decodes the dataset. Caller frees *options_json with
 * free(). Returns negative if the filter is absent or carries no blob. */
herr_t H5Pget_comp_json(hid_t dcpl_id, char **options_json);

/* ----------------------------------------------------------------------------
 * Dataset-level path (needs an HDF5 whose H5Z_class3_t has dset_write /
 * dset_read; H5Zcomp_has_dset_io() says whether this build has it).
 *
 * Two settings are part of the dataset and go in the options JSON given to
 * H5Pset_comp, under the filter's own "h5z:" prefix:
 *
 *   "h5z:group":"4,1,1"     one compression unit = 4x1x1 storage chunks.
 *                           0 in a position = every chunk along that
 *                           dimension; "all" (or 0) = the whole dataset.
 *   "h5z:progressive":true  write each unit so a prefix of its stream decodes
 *                           (codec must support it: sperr).
 *
 * The rest are per-transfer and go on the DXPL passed to H5Dwrite / H5Dread.
 * A DXPL with none of them, on a dataset without "h5z:group", behaves exactly
 * as before (per-chunk filter).
 * --------------------------------------------------------------------------*/

/* The buffer passed to H5Dwrite/H5Dread with this DXPL is memory of LibPressio
 * domain `domain` (e.g. "cudamalloc" for a CUDA device pointer). HDF5 does not
 * touch it; the codec reads/writes it in place and only compressed bytes cross
 * to the host. Requirements: memory type == dataset type, whole compression
 * units selected, each unit contiguous in the buffer (units spanning all but
 * the first dimension). NULL or "" clears the setting. */
herr_t H5Pset_comp_native_buffer(hid_t dxpl_id, const char *domain);

/* Progressive read: fetch and decode only the first `pct`% (1..100) of every
 * compression unit's stream. With "h5z:group" the stream is striped over the
 * unit's storage chunks, so this reads only the first storage chunks of each
 * unit. The dataset must have been written with "h5z:progressive":true. */
herr_t H5Pset_comp_progressive(hid_t dxpl_id, unsigned pct);

/* Region-of-interest form: units intersecting the box [start, start+count)
 * (element coordinates, `rank` = dataset rank) are read at `pct_in`%, all
 * other units at `pct_out`%. */
herr_t H5Pset_comp_progressive_roi(hid_t dxpl_id, int rank, const hsize_t *start, const hsize_t *count,
                                   unsigned pct_in, unsigned pct_out);

/* Send an ordinary host-buffer H5Dwrite/H5Dread through the dataset-level path
 * as well: units are compressed straight from (decoded straight into) the
 * application's buffer inside the call, bypassing the chunk cache. Off by
 * default; the environment variable H5ZCOMP_DIRECT=1 turns it on everywhere. */
herr_t H5Pset_comp_direct(hid_t dxpl_id, int enable);

/* What the last H5Dwrite/H5Dread on the calling thread did. */
typedef struct H5Z_comp_io_stats_t {
    int                handled;      /* 1: it went through the dataset-level path           */
    int                native;       /* 1: the buffer was used in its own domain            */
    unsigned long long units;        /* compression units touched                           */
    unsigned long long chunks;       /* storage chunks read or written                      */
    unsigned long long bytes_raw;    /* uncompressed bytes delivered / consumed             */
    unsigned long long bytes_stored; /* stored size of the touched units (all their chunks) */
    unsigned long long bytes_io;     /* bytes actually read from / written to the file      */
    unsigned long long bytes_staged; /* bytes copied through a host scratch buffer (0 =     */
                                     /* every unit was (de)coded in place)                  */
} H5Z_comp_io_stats_t;
herr_t H5Zcomp_get_io_stats(H5Z_comp_io_stats_t *stats);

/* 1 if this build of the filter has the dataset-level path, else 0. */
int H5Zcomp_has_dset_io(void);

#ifdef __cplusplus
}
#endif

#endif /* H5ZPRESSIO_H */
