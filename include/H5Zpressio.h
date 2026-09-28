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

#ifdef __cplusplus
}
#endif

#endif /* H5ZPRESSIO_H */
