/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright 2022, Intel Corporation */

/*
 * client_pmem_map_file.c -- a function to map PMem using libpmem
 *
 * Please see README.md for a detailed description of this example.
 */

#include "common-example.h"

int
client_pmem_map_file(char *path, int argc, struct example_mem *mem);

void
client_pmem_unmap_file(struct example_mem *mem);
