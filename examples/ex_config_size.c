/* ============================================================================
 * ex_config_size.c  --  where H5Zcomp switches from inline cd_values to blob.
 *
 * v2 carries small configs inline in cd_values (autonomous) and routes large
 * configs to the filter blob (needs the DXPL preamble at runtime, see
 * ex_large_config.c). This program calls H5Pset_comp with growing configs and
 * reports which path each took, by checking whether a blob was stored.
 * ==========================================================================*/
#include "hdf5.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern herr_t H5Pset_comp(hid_t dcpl, const char *id, const char *json);

static char *make_json(size_t target) {
    const char *head = "{\"pressio:abs\":1e-3,\"_pad\":\"", *tail = "\"}";
    size_t h = strlen(head), t = strlen(tail), pad = target > h + t ? target - h - t : 1;
    char *s = (char *)malloc(h + pad + t + 1);
    memcpy(s, head, h); memset(s + h, 'A', pad); memcpy(s + h + pad, tail, t);
    s[h + pad + t] = '\0';
    return s;
}

int main(void) {
    printf("cd_values ceiling = %u uints = %u KiB\n\n",
           (unsigned)H5Z_MAX_CD_NELMTS, (unsigned)(H5Z_MAX_CD_NELMTS * 4u / 1024u));
    size_t targets[] = { 1u<<10, 64u<<10, 200u<<10, 256u<<10, 300u<<10, 1u<<20, 4u<<20 };

    for (size_t k = 0; k < sizeof(targets)/sizeof(targets[0]); k++) {
        char *json = make_json(targets[k]);
        hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
        hsize_t ch[1] = {16}; H5Pset_chunk(dcpl, 1, ch);

        herr_t r = H5Pset_comp(dcpl, "sz3", json);
        size_t blobsz = 0;
        int external = (r >= 0 && H5Pget_filter_blob(dcpl, 0, 0, NULL, &blobsz) >= 0 && blobsz > 0);

        printf("config ~%4zu KiB : %s\n", targets[k] >> 10,
               (r < 0)      ? "H5Pset_comp FAILED" :
               external     ? "blob (external) -- needs H5Pset_comp_dxpl at runtime"
                            : "inline cd_values -- autonomous");
        H5Pclose(dcpl); free(json);
    }
    return 0;
}
