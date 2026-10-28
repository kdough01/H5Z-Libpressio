# VOL vs H5Zcomp filter

Inputs:

- chunk: results/vs_vol_60217, results/vs_vol_60218, results/vs_vol_60220
- direct: results/vs_vol_60217, results/vs_vol_60218, results/vs_vol_60220
- prog_write: results/vs_vol_60222
- vol: /home/kdougherty/hdf5_projs/vol-external-passthrough/tests/results/20260811_140354_52888, /home/kdougherty/hdf5_projs/vol-external-passthrough/tests/results/device_resident_tests
- VOL progressive: /home/kdougherty/hdf5_projs/vol-external-passthrough/tests/results/prog_20260818_140442_53500
- VOL progressive: /home/kdougherty/hdf5_projs/vol-external-passthrough/tests/results/prog_20260818_134437_53492
- VOL progressive: /home/kdougherty/hdf5_projs/vol-external-passthrough/tests/results/prog_20260818_131707_53488

Times are wall clock; write = create+write+flush+sync+close+csync, read = open+read (cold cache). Reps averaged.

## 1. Ratio parity (N=1)

90 pairs; median |filter/VOL - 1| = 0.00%, worst 0.03%.

| path | dataset | codec | VOL ratio | filter ratio |
|---|---|---|---|---|
| chunk | s3d | sz3_1e3 | 19195.33 | 19201.23 |
| direct | s3d | sz3_1e3 | 19195.33 | 19201.23 |
| chunk | miranda | sz3_1e3 | 164.57 | 164.57 |
| direct | miranda | sz3_1e3 | 164.57 | 164.57 |
| chunk | scale-T | sz3_1e3 | 254.02 | 254.03 |

## 2. Filter / VOL time (geometric mean over datasets)

| N | codec | write chunk | write direct | read chunk | read direct |
|---|---|---|---|---|---|
| 1 | cuszp_1e3 | 1.92 | 0.73 | 8.89 | 5.07 |
| 1 | cuszp_1e6 | 1.53 | 0.52 | 6.79 | 4.18 |
| 1 | noop | 1.26 | 0.62 | 1.93 | 1.14 |
| 1 | sz3_1e3 | 1.17 | 1.05 | 1.90 | 1.32 |
| 1 | sz3_1e6 | 1.11 | 0.99 | 1.34 | 1.00 |
| 1 | szx_1e3 | 1.12 | 0.67 | 1.36 | 0.85 |
| 1 | szx_1e6 | 1.16 | 0.68 | 1.34 | 0.86 |
| 1 | zfp_cpu_r12 | 1.08 | 0.96 | 1.36 | 1.15 |
| 1 | zfp_cpu_r16 | 1.10 | 1.01 | 1.29 | 1.10 |
| 1 | zfp_cpu_r4 | 1.11 | 0.99 | 1.51 | 1.11 |
| 1 | zfp_cpu_r8 | 1.11 | 1.02 | 1.44 | 1.12 |
| 1 | zfp_gpu_r12 | 1.43 | 0.75 | 2.77 | 0.88 |
| 1 | zfp_gpu_r16 | 1.50 | 0.80 | 2.23 | 1.08 |
| 1 | zfp_gpu_r4 | 1.56 | 0.89 | 4.19 | 1.12 |
| 1 | zfp_gpu_r8 | 1.44 | 0.75 | 3.10 | 0.90 |
| 8 | cuszp_1e3 | 1.77 | 0.88 | 6.81 | 3.77 |
| 8 | cuszp_1e6 | 1.33 | 0.60 | 6.65 | 3.67 |
| 8 | noop | 1.52 | 0.84 | 2.10 | 0.54 |
| 8 | sz3_1e3 | 1.28 | 1.09 | 1.79 | 1.30 |
| 8 | sz3_1e6 | 1.17 | 1.05 | 1.46 | 1.17 |
| 8 | szx_1e3 | 1.21 | 0.84 | 1.99 | 1.31 |
| 8 | szx_1e6 | 1.26 | 0.85 | 2.31 | 1.53 |
| 8 | zfp_cpu_r12 | 1.08 | 0.98 | 1.25 | 1.04 |
| 8 | zfp_cpu_r16 | 1.11 | 1.00 | 1.27 | 1.08 |
| 8 | zfp_cpu_r4 | 1.11 | 0.99 | 1.40 | 1.07 |
| 8 | zfp_cpu_r8 | 1.13 | 0.98 | 1.30 | 1.08 |
| 8 | zfp_gpu_r12 | 1.62 | 0.74 | 2.79 | 0.79 |
| 8 | zfp_gpu_r16 | 1.24 | 0.67 | 2.48 | 0.73 |
| 8 | zfp_gpu_r4 | 1.64 | 0.70 | 3.93 | 0.99 |
| 8 | zfp_gpu_r8 | 1.50 | 0.69 | 3.58 | 0.99 |

## 3. Decoupling: ratio vs number of storage chunks

**sz3_1e3**

| dataset | N | VOL ratio | filter per-chunk | filter group=all |
|---|---|---|---|---|
| miranda | 1 | 164.57 | 164.57 | - |
| miranda | 8 | 159.59 | 159.59 | - |
| miranda | 16 | 147.11 | 147.12 | - |
| s3d | 1 | 19195.33 | 19201.23 | - |
| s3d | 8 | 13524.66 | 8470.63 | - |
| s3d | 16 | 5274.76 | 4752.81 | - |
| scale-T | 1 | 254.02 | 254.03 | - |
| scale-T | 8 | 216.88 | 191.72 | - |
| scale-T | 16 | 178.46 | 178.47 | - |
| nyx_baryon | 1 | 7.85 | - | - |
| nyx_baryon | 8 | 7.87 | - | - |
| nyx_baryon | 16 | 7.91 | - | - |
| ocean_temp | 1 | 6.49 | - | - |
| ocean_temp | 8 | 5.85 | - | - |
| ocean_temp | 16 | 5.75 | - | - |
| einspline37 | 1 | 3.38 | - | - |
| einspline37 | 8 | 3.37 | 3.37 | - |
| einspline37 | 16 | 3.38 | 3.38 | - |

**szx_1e3**

| dataset | N | VOL ratio | filter per-chunk | filter group=all |
|---|---|---|---|---|
| miranda | 1 | 21.51 | 21.51 | - |
| miranda | 8 | 21.51 | 21.51 | - |
| miranda | 16 | 21.51 | 21.51 | - |
| s3d | 1 | 26.12 | 26.12 | - |
| s3d | 8 | 26.12 | 26.10 | - |
| s3d | 16 | 26.12 | 26.05 | - |
| scale-T | 1 | 5.33 | 5.33 | - |
| scale-T | 8 | 5.33 | 5.31 | - |
| scale-T | 16 | 5.33 | 5.33 | - |
| nyx_baryon | 1 | 2.33 | - | - |
| nyx_baryon | 8 | 2.33 | - | - |
| nyx_baryon | 16 | 2.33 | - | - |
| ocean_temp | 1 | 2.35 | - | - |
| ocean_temp | 8 | 2.35 | - | - |
| ocean_temp | 16 | 2.35 | - | - |
| einspline37 | 1 | 1.60 | - | - |
| einspline37 | 8 | 1.60 | 1.60 | - |
| einspline37 | 16 | 1.60 | 1.60 | - |

**zfp_cpu_r8**

| dataset | N | VOL ratio | filter per-chunk | filter group=all |
|---|---|---|---|---|
| miranda | 1 | 8.00 | 8.00 | - |
| miranda | 8 | 8.00 | 8.00 | - |
| miranda | 16 | 8.00 | 8.00 | - |
| s3d | 1 | 8.00 | 8.00 | - |
| s3d | 8 | 7.87 | 7.81 | - |
| s3d | 16 | 8.00 | 7.81 | - |
| scale-T | 1 | 3.92 | 3.92 | - |
| scale-T | 8 | 3.27 | 3.06 | - |
| scale-T | 16 | 3.50 | 3.50 | - |
| nyx_baryon | 1 | 4.00 | - | - |
| nyx_baryon | 8 | 4.00 | - | - |
| nyx_baryon | 16 | 4.00 | - | - |
| ocean_temp | 1 | 3.78 | - | - |
| ocean_temp | 8 | 2.83 | - | - |
| ocean_temp | 16 | 1.89 | - | - |
| einspline37 | 1 | 4.00 | - | - |
| einspline37 | 8 | 4.00 | 4.00 | - |
| einspline37 | 16 | 4.00 | 4.00 | - |

**cuszp_1e3**

| dataset | N | VOL ratio | filter per-chunk | filter group=all |
|---|---|---|---|---|
| miranda | 1 | 21.06 | 21.06 | - |
| miranda | 8 | 21.06 | 21.06 | - |
| miranda | 16 | 21.06 | 21.06 | - |
| s3d | 1 | 46.22 | 46.22 | - |
| s3d | 8 | 46.21 | 46.15 | - |
| s3d | 16 | 46.19 | 45.99 | - |
| scale-T | 1 | 12.45 | 12.45 | - |
| scale-T | 8 | 12.45 | 12.37 | - |
| scale-T | 16 | 12.45 | 12.45 | - |
| nyx_baryon | 1 | 4.29 | - | - |
| nyx_baryon | 8 | 4.29 | - | - |
| nyx_baryon | 16 | 4.29 | - | - |
| ocean_temp | 1 | 3.71 | - | - |
| ocean_temp | 8 | 3.71 | - | - |
| ocean_temp | 16 | 3.71 | - | - |
| einspline37 | 1 | 5.26 | - | - |
| einspline37 | 8 | 3.37 | 3.37 | - |
| einspline37 | 16 | 3.37 | 3.37 | - |

## 4. Residency arms: end-to-end write time (ms)

| codec | dataset | arm | VOL | filter | filter / VOL |
|---|---|---|---|---|---|
| cuszp_1e3 | miranda | devres | 231.0 | - | - |
| cuszp_1e3 | miranda | rtrip | 505.4 | - | - |
| cuszp_1e3 | miranda | hostsim | 314.6 | 835.3 | 2.65 |
| cuszp_1e3 | s3d | devres | 358.2 | - | - |
| cuszp_1e3 | s3d | rtrip | 1024.3 | - | - |
| cuszp_1e3 | s3d | hostsim | 485.4 | 1046.1 | 2.16 |
| cuszp_1e3 | scale-T | devres | 374.7 | - | - |
| cuszp_1e3 | scale-T | rtrip | 744.6 | - | - |
| cuszp_1e3 | scale-T | hostsim | 550.9 | 685.6 | 1.24 |
| cuszp_1e3 | nyx_baryon | devres | 511.0 | - | - |
| cuszp_1e3 | nyx_baryon | rtrip | 950.1 | - | - |
| cuszp_1e3 | nyx_baryon | hostsim | 569.6 | - | - |
| cuszp_1e3 | ocean_temp | devres | 372.3 | - | - |
| cuszp_1e3 | ocean_temp | rtrip | 472.9 | - | - |
| cuszp_1e3 | ocean_temp | hostsim | 356.9 | - | - |
| cuszp_1e3 | einspline37 | hostsim | 4703.8 | - | - |
| cuszp_1e6 | miranda | devres | 401.9 | - | - |
| cuszp_1e6 | miranda | rtrip | 629.6 | - | - |
| cuszp_1e6 | miranda | hostsim | 463.3 | 580.0 | 1.25 |
| cuszp_1e6 | s3d | devres | 441.0 | - | - |
| cuszp_1e6 | s3d | rtrip | 1167.9 | - | - |
| cuszp_1e6 | s3d | hostsim | 559.6 | 1038.0 | 1.86 |
| cuszp_1e6 | scale-T | devres | 513.8 | - | - |
| cuszp_1e6 | scale-T | rtrip | 953.3 | - | - |
| cuszp_1e6 | scale-T | hostsim | 553.3 | 859.7 | 1.55 |
| cuszp_1e6 | nyx_baryon | devres | 710.5 | - | - |
| cuszp_1e6 | nyx_baryon | rtrip | 1074.6 | - | - |
| cuszp_1e6 | nyx_baryon | hostsim | 813.0 | - | - |
| cuszp_1e6 | ocean_temp | devres | 428.4 | - | - |
| cuszp_1e6 | ocean_temp | rtrip | 547.0 | - | - |
| cuszp_1e6 | ocean_temp | hostsim | 452.1 | - | - |
| zfp_gpu_r12 | miranda | devres | 394.5 | - | - |
| zfp_gpu_r12 | miranda | rtrip | 605.2 | - | - |
| zfp_gpu_r12 | miranda | hostsim | 407.7 | 526.0 | 1.29 |
| zfp_gpu_r12 | s3d | devres | 972.6 | - | - |
| zfp_gpu_r12 | s3d | rtrip | 1260.2 | - | - |
| zfp_gpu_r12 | s3d | hostsim | 726.7 | 1246.5 | 1.72 |
| zfp_gpu_r12 | scale-T | devres | 1143.2 | - | - |
| zfp_gpu_r12 | scale-T | rtrip | 1092.8 | - | - |
| zfp_gpu_r12 | scale-T | hostsim | 777.4 | 1018.5 | 1.31 |
| zfp_gpu_r12 | nyx_baryon | devres | 693.3 | - | - |
| zfp_gpu_r12 | nyx_baryon | rtrip | 933.9 | - | - |
| zfp_gpu_r12 | nyx_baryon | hostsim | 706.4 | - | - |
| zfp_gpu_r12 | ocean_temp | devres | 351.7 | - | - |
| zfp_gpu_r12 | ocean_temp | rtrip | 562.0 | - | - |
| zfp_gpu_r12 | ocean_temp | hostsim | 374.4 | - | - |
| zfp_gpu_r16 | miranda | devres | 758.6 | - | - |
| zfp_gpu_r16 | miranda | rtrip | 696.2 | - | - |
| zfp_gpu_r16 | miranda | hostsim | 462.9 | 584.1 | 1.26 |
| zfp_gpu_r16 | s3d | devres | 699.4 | - | - |
| zfp_gpu_r16 | s3d | rtrip | 1322.7 | - | - |
| zfp_gpu_r16 | s3d | hostsim | 814.9 | 1375.4 | 1.69 |
| zfp_gpu_r16 | scale-T | devres | 635.2 | - | - |
| zfp_gpu_r16 | scale-T | rtrip | 1016.9 | - | - |
| zfp_gpu_r16 | scale-T | hostsim | 718.0 | 1140.0 | 1.59 |
| zfp_gpu_r16 | nyx_baryon | devres | 600.1 | - | - |
| zfp_gpu_r16 | nyx_baryon | rtrip | 989.6 | - | - |
| zfp_gpu_r16 | nyx_baryon | hostsim | 668.9 | - | - |
| zfp_gpu_r16 | ocean_temp | devres | 408.1 | - | - |
| zfp_gpu_r16 | ocean_temp | rtrip | 522.0 | - | - |
| zfp_gpu_r16 | ocean_temp | hostsim | 404.4 | - | - |
| zfp_gpu_r4 | miranda | devres | 347.2 | - | - |
| zfp_gpu_r4 | miranda | rtrip | 572.7 | - | - |
| zfp_gpu_r4 | miranda | hostsim | 410.9 | 493.9 | 1.20 |
| zfp_gpu_r4 | s3d | devres | 464.9 | - | - |
| zfp_gpu_r4 | s3d | rtrip | 1431.6 | - | - |
| zfp_gpu_r4 | s3d | hostsim | 540.1 | 1060.4 | 1.96 |
| zfp_gpu_r4 | scale-T | devres | 475.5 | - | - |
| zfp_gpu_r4 | scale-T | rtrip | 792.0 | - | - |
| zfp_gpu_r4 | scale-T | hostsim | 471.3 | 753.8 | 1.60 |
| zfp_gpu_r4 | nyx_baryon | devres | 366.8 | - | - |
| zfp_gpu_r4 | nyx_baryon | rtrip | 740.4 | - | - |
| zfp_gpu_r4 | nyx_baryon | hostsim | 430.5 | - | - |
| zfp_gpu_r4 | ocean_temp | devres | 361.6 | - | - |
| zfp_gpu_r4 | ocean_temp | rtrip | 428.9 | - | - |
| zfp_gpu_r4 | ocean_temp | hostsim | 391.7 | - | - |
| zfp_gpu_r8 | miranda | devres | 384.7 | - | - |
| zfp_gpu_r8 | miranda | rtrip | 554.9 | - | - |
| zfp_gpu_r8 | miranda | hostsim | 363.7 | 490.5 | 1.35 |
| zfp_gpu_r8 | s3d | devres | 588.0 | - | - |
| zfp_gpu_r8 | s3d | rtrip | 1159.6 | - | - |
| zfp_gpu_r8 | s3d | hostsim | 619.2 | 1089.7 | 1.76 |
| zfp_gpu_r8 | scale-T | devres | 624.1 | - | - |
| zfp_gpu_r8 | scale-T | rtrip | 922.1 | - | - |
| zfp_gpu_r8 | scale-T | hostsim | 645.8 | 812.8 | 1.26 |
| zfp_gpu_r8 | nyx_baryon | devres | 523.5 | - | - |
| zfp_gpu_r8 | nyx_baryon | rtrip | 875.0 | - | - |
| zfp_gpu_r8 | nyx_baryon | hostsim | 659.4 | - | - |
| zfp_gpu_r8 | ocean_temp | devres | 330.3 | - | - |
| zfp_gpu_r8 | ocean_temp | rtrip | 408.0 | - | - |
| zfp_gpu_r8 | ocean_temp | hostsim | 343.8 | - | - |

## 5. Progressive reads

| dataset | codec | pct | VOL bytes read | filter bytes read | VOL read ms | filter read ms | VOL RMSE | filter RMSE |
|---|---|---|---|---|---|---|---|---|
| s3d | sperr_pwe1e3 | 5 | 10.6% | 5.0% | 2901 | 3925 | 4.157e-04 | 5.104e-04 |
| s3d | sperr_pwe1e3 | 10 | 15.6% | 10.0% | 2924 | 4016 | 2.281e-04 | 2.513e-04 |
| s3d | sperr_pwe1e3 | 25 | 30.6% | 25.0% | 2941 | 4080 | 8.799e-05 | 9.149e-05 |
| s3d | sperr_pwe1e3 | 50 | 55.6% | 50.0% | 2946 | 3957 | 4.638e-05 | 4.638e-05 |
| s3d | sperr_pwe1e3 | 75 | 80.6% | 75.0% | 2966 | 3982 | 3.314e-05 | 3.314e-05 |
| s3d | sperr_pwe1e3 | 100 | 100.0% | 100.0% | 2984 | 4031 | 2.036e-05 | 2.036e-05 |
| s3d | sperr_pwe1e6 | 5 | 5.1% | 5.0% | 3372 | 4247 | 8.305e-06 | 1.154e-05 |
| s3d | sperr_pwe1e6 | 10 | 10.1% | 10.0% | 3397 | 4342 | 3.374e-06 | 3.923e-06 |
| s3d | sperr_pwe1e6 | 25 | 25.1% | 25.0% | 3488 | 4373 | 9.352e-07 | 9.352e-07 |
| s3d | sperr_pwe1e6 | 50 | 50.1% | 50.0% | 3556 | 4441 | 3.243e-07 | 3.243e-07 |
| s3d | sperr_pwe1e6 | 75 | 75.1% | 75.0% | 3612 | 4451 | 2.034e-07 | 2.034e-07 |
| s3d | sperr_pwe1e6 | 100 | 100.0% | 100.0% | 3802 | 4842 | 1.191e-07 | 1.191e-07 |
| scale-T | sperr_pwe1e3 | 5 | 5.3% | 5.1% | 3577 | 4826 | 6.120e-04 | 6.120e-04 |
| scale-T | sperr_pwe1e3 | 10 | 10.3% | 10.2% | 3642 | 4832 | 4.289e-04 | 4.289e-04 |
| scale-T | sperr_pwe1e3 | 25 | 25.3% | 25.5% | 3690 | 4924 | 2.602e-04 | 2.602e-04 |
| scale-T | sperr_pwe1e3 | 50 | 50.3% | 50.0% | 3785 | 4995 | 1.793e-04 | 1.793e-04 |
| scale-T | sperr_pwe1e3 | 75 | 75.3% | 75.5% | 3864 | 5013 | 1.446e-04 | 1.446e-04 |
| scale-T | sperr_pwe1e3 | 100 | 100.0% | 100.0% | 4129 | 5251 | 1.085e-04 | 1.085e-04 |

## Recorded failures (rep=-1)

- chunk: einspline37 cuszp_1e3 N=1 hostsim
- chunk: einspline37 cuszp_1e6 N=1 hostsim
- chunk: einspline37 cuszp_1e6 N=8 hostsim
- chunk: einspline37 cuszp_1e6 N=16 hostsim
- chunk: einspline37 noop N=1 hostsim
- chunk: einspline37 sz3_1e3 N=1 hostsim
- chunk: einspline37 sz3_1e6 N=1 hostsim
- chunk: einspline37 szx_1e3 N=1 hostsim
- chunk: einspline37 szx_1e6 N=1 hostsim
- chunk: einspline37 zfp_cpu_r12 N=1 hostsim
- chunk: einspline37 zfp_cpu_r16 N=1 hostsim
- chunk: einspline37 zfp_cpu_r4 N=1 hostsim
- chunk: einspline37 zfp_cpu_r8 N=1 hostsim
- chunk: einspline37 zfp_gpu_r12 N=1 hostsim
- chunk: einspline37 zfp_gpu_r12 N=8 hostsim
- chunk: einspline37 zfp_gpu_r12 N=16 hostsim
- chunk: einspline37 zfp_gpu_r16 N=1 hostsim
- chunk: einspline37 zfp_gpu_r16 N=8 hostsim
- chunk: einspline37 zfp_gpu_r16 N=16 hostsim
- chunk: einspline37 zfp_gpu_r4 N=1 hostsim
- chunk: einspline37 zfp_gpu_r8 N=1 hostsim
- chunk: einspline37 zfp_gpu_r8 N=8 hostsim
- direct: einspline37 cuszp_1e3 N=1 hostsim
- direct: einspline37 cuszp_1e6 N=1 hostsim
- direct: einspline37 cuszp_1e6 N=8 hostsim
- direct: einspline37 cuszp_1e6 N=16 hostsim
- direct: einspline37 noop N=1 hostsim
- direct: einspline37 sz3_1e3 N=1 hostsim
- direct: einspline37 sz3_1e6 N=1 hostsim
- direct: einspline37 szx_1e3 N=1 hostsim
- direct: einspline37 szx_1e6 N=1 hostsim
- direct: einspline37 zfp_cpu_r12 N=1 hostsim
- direct: einspline37 zfp_cpu_r16 N=1 hostsim
- direct: einspline37 zfp_cpu_r4 N=1 hostsim
- direct: einspline37 zfp_cpu_r8 N=1 hostsim
- direct: einspline37 zfp_gpu_r12 N=1 hostsim
- direct: einspline37 zfp_gpu_r12 N=8 hostsim
- direct: einspline37 zfp_gpu_r12 N=16 hostsim
- direct: einspline37 zfp_gpu_r16 N=1 hostsim
- direct: einspline37 zfp_gpu_r16 N=8 hostsim
- direct: einspline37 zfp_gpu_r16 N=16 hostsim
- direct: einspline37 zfp_gpu_r4 N=1 hostsim
- direct: einspline37 zfp_gpu_r8 N=1 hostsim
- direct: einspline37 zfp_gpu_r8 N=8 hostsim

