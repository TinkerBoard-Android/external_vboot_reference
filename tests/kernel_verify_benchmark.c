/* Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Timing benchmark for verifying a firmware image.
 */

#include <stdio.h>
#include <stdlib.h>

#include "file_keys.h"
#include "kernel_image.h"
#include "padding.h"
#include "rsa_utility.h"
#include "timer_utils.h"
#include "utility.h"

#define FILE_NAME_SIZE 128

#define NUM_OPERATIONS 30  /* Number of verify operations to time.
                            * We use a smaller number here (30 vs. 100)
                            * since there are many more cases to consider
                            * (one for each combination of firmware and kernel
                            * signature algorithm.
                            */

#define KERNEL_SIZE_SMALL 512000
#define KERNEL_SIZE_MEDIUM 1024000
#define KERNEL_SIZE_LARGE 4096000
const uint64_t g_kernel_sizes_to_test[] = {
  KERNEL_SIZE_SMALL,
  KERNEL_SIZE_MEDIUM,
  KERNEL_SIZE_LARGE
};
const char* g_kernel_size_labels[] = {
  "small",
  "medium",
  "large"
};
#define NUM_SIZES_TO_TEST (sizeof(g_kernel_sizes_to_test) / \
                           sizeof(g_kernel_sizes_to_test[0]))

uint8_t* GenerateTestKernelBlob(int firmware_sign_algorithm,
                                int kernel_sign_algorithm,
                                int kernel_len,
                                const uint8_t* kernel_sign_key,
                                const char* firmware_key_file,
                                const char* kernel_key_file) {
  KernelImage* image = KernelImageNew();
  uint8_t* kernel_blob = NULL;
  uint64_t kernel_blob_len = 0;

  Memcpy(image->magic, KERNEL_MAGIC, KERNEL_MAGIC_SIZE);
  image->header_version = 1;
  image->firmware_sign_algorithm = firmware_sign_algorithm;
  image->kernel_sign_algorithm = kernel_sign_algorithm;
  image->kernel_key_version = 1;
  image->kernel_sign_key = (uint8_t*) Malloc(
      RSAProcessedKeySize(image->kernel_sign_algorithm));
  Memcpy(image->kernel_sign_key, kernel_sign_key,
         RSAProcessedKeySize(image->kernel_sign_algorithm));

  /* Update correct header length. */
  image->header_len = GetKernelHeaderLen(image);

  /* Calculate SHA-512 digest on header and populate header_checksum. */
  CalculateKernelHeaderChecksum(image, image->header_checksum);

  /* Populate kernel options and data with dummy data. */
  image->kernel_version = 1;
  image->options.version[0] = 1;
  image->options.version[1] = 0;
  Memset(image->options.cmd_line, 0, sizeof(image->options.cmd_line));
  image->options.kernel_len = kernel_len;
  image->options.kernel_load_addr = 0;
  image->options.kernel_entry_addr = 0;
  image->kernel_key_signature = image->kernel_signature = NULL;
  image->kernel_data = Malloc(kernel_len);
  /* TODO(gauravsh): Populate this with random data, to remove data-dependent
   * timing artificats. */
  Memset(image->kernel_data, 'K', kernel_len);

  if (!AddKernelKeySignature(image, firmware_key_file)) {
    fprintf(stderr, "Couldn't create key signature.\n");
    KernelImageFree(image);
    return NULL;
  }

  if (!AddKernelSignature(image, kernel_key_file)) {
    fprintf(stderr, "Couldn't create kernel option and kernel  signature.\n");
    KernelImageFree(image);
    return NULL;
  }

  kernel_blob = GetKernelBlob(image, &kernel_blob_len);
  KernelImageFree(image);
  return kernel_blob;
}

int SpeedTestAlgorithm(int firmware_sign_algorithm,
                       int kernel_sign_algorithm) {
  int i, j, error_code = 0;
  int firmware_key_size, kernel_key_size;
  ClockTimerState ct;
  double msecs;
  uint64_t len;
  uint8_t* kernel_sign_key = NULL;
  uint8_t* firmware_key_blob = NULL;
  char firmware_sign_key_file[FILE_NAME_SIZE];
  char kernel_sign_key_file[FILE_NAME_SIZE];
  char file_name[FILE_NAME_SIZE];  /* Temp to hold a constructed file name */
  char* sha_strings[] = {  /* Maps algorithm->SHA algorithm. */
    "sha1", "sha256", "sha512",  /* RSA-1024 */
    "sha1", "sha256", "sha512",  /* RSA-2048 */
    "sha1", "sha256", "sha512",  /* RSA-4096 */
    "sha1", "sha256", "sha512",  /* RSA-8192 */
  };
  uint8_t* kernel_blobs[NUM_SIZES_TO_TEST];
  for (i = 0; i < NUM_SIZES_TO_TEST; ++i)
    kernel_blobs[i] = NULL;

  /* Get all needed test keys. */
  firmware_key_size = siglen_map[firmware_sign_algorithm] * 8;  /* in bits. */
  kernel_key_size = siglen_map[kernel_sign_algorithm] * 8;  /* in bits. */
  snprintf(firmware_sign_key_file, FILE_NAME_SIZE, "testkeys/key_rsa%d.pem",
           firmware_key_size);
  snprintf(kernel_sign_key_file, FILE_NAME_SIZE, "testkeys/key_rsa%d.pem",
           kernel_key_size);
  snprintf(file_name, FILE_NAME_SIZE, "testkeys/key_rsa%d.keyb",
           kernel_key_size);
  kernel_sign_key = BufferFromFile(file_name, &len);
  if (!kernel_sign_key) {
    fprintf(stderr, "Couldn't read pre-processed public kernel signing key.\n");
    error_code = 1;
    goto cleanup;
  }

  /* Generate test images. */
  for (i = 0; i < NUM_SIZES_TO_TEST; ++i) {
    kernel_blobs[i] = GenerateTestKernelBlob(firmware_sign_algorithm,
                                             kernel_sign_algorithm,
                                             g_kernel_sizes_to_test[i],
                                             kernel_sign_key,
                                             firmware_sign_key_file,
                                             kernel_sign_key_file);
    if (!kernel_blobs[i]) {
      fprintf(stderr, "Couldn't generate test firmware images.\n");
      error_code = 1;
      goto cleanup;
    }
  }

  /* Get pre-processed key used for verification. */
  snprintf(file_name, FILE_NAME_SIZE, "testkeys/key_rsa%d.keyb",
           firmware_key_size);
  firmware_key_blob = BufferFromFile(file_name, &len);
  if (!firmware_key_blob) {
    fprintf(stderr, "Couldn't read pre-processed firmware public key.\n");
    error_code = 1;
    goto cleanup;
  }

  /* Now run the timing tests. */
  for (i = 0; i < NUM_SIZES_TO_TEST; ++i) {
    StartTimer(&ct);
    for (j = 0; j < NUM_OPERATIONS; ++j) {
      if (VERIFY_KERNEL_SUCCESS !=
          VerifyKernel(firmware_key_blob, kernel_blobs[i], 0))
        fprintf(stderr, "Warning: Kernel Verification Failed.\n");
    }
    StopTimer(&ct);
    msecs = (float) GetDurationMsecs(&ct) / NUM_OPERATIONS;
    fprintf(stderr,
            "# Kernel (%s, Algo = %s / %s):"
            "\t%.02f ms/verification\n",
            g_kernel_size_labels[i],
            algo_strings[firmware_sign_algorithm],
            algo_strings[kernel_sign_algorithm],
            msecs);
    fprintf(stdout, "ms_firmware_%s_rsa%d_%s_rsa%d_%s:%.02f\n",
            g_kernel_size_labels[i],
            firmware_key_size,
            sha_strings[firmware_sign_algorithm],
            kernel_key_size,
            sha_strings[kernel_sign_algorithm],
            msecs);
  }

 cleanup:
  for (i = 0; i < NUM_SIZES_TO_TEST; ++i)
    Free(kernel_blobs[i]);
  Free(firmware_key_blob);
  Free(kernel_sign_key);
  return error_code;
}


int main(int argc, char* argv[]) {
  int i, j, error_code = 0;
  for (i = 0; i < kNumAlgorithms; ++i) {  /* Firmware Signing Algorithm. */
    for (j = 0; j < kNumAlgorithms; ++j) {  /* Kernel Signing Algorithm. */
      /* Only measure if the kernel signing algorithm is weaker or equal to
       * the firmware signing algorithm. */
      if (siglen_map[j] > siglen_map[i])
        continue;
      if (siglen_map[j] == siglen_map[i] && hash_size_map[j] > hash_size_map[i])
        continue;
      if (0 != (error_code = SpeedTestAlgorithm(i, j)))
        return error_code;
    }
  }
  return 0;
}
