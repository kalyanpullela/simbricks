# Copyright 2026
# DPU Behavioral Model (BlueField-3) for HCOP experiments.

include mk/subdir_pre.mk

bin_dpu_bm := $(d)dpu_bm

OBJS := $(addprefix $(d),dpu_bm.o dpu_config.o arm_core_pool.o dram_store.o \
    paxos_dpu_handler.o lock_dpu_handler.o barrier_dpu_handler.o)

$(OBJS): CPPFLAGS := $(CPPFLAGS) -I$(d)

$(bin_dpu_bm): $(OBJS) $(lib_hcop_a) $(lib_nicbm) $(lib_nicif) $(lib_netif) $(lib_pcie) \
    $(lib_base) -lboost_fiber -lboost_context -lpthread

CLEAN := $(bin_dpu_bm) $(OBJS)
ALL := $(bin_dpu_bm)
include mk/subdir_post.mk
