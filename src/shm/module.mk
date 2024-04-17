# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2024 Xilinx, Inc.

# SHM Sync Module Makefile

include mk/pushd.mk


LIB_SRCS_$(d) := sfptpd_shm_module.c

LIB_$(d) := shm


include mk/library.mk
include mk/popd.mk

# fin
