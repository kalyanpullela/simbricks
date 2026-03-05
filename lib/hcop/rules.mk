# HCOP shared protocol library rules

include mk/subdir_pre.mk

lib_hcop := $(d)libhcop.a

OBJS := $(addprefix $(d),paxos_state.o lock_state.o barrier_state.o)

$(OBJS): CPPFLAGS := $(CPPFLAGS) -I$(d)../

$(lib_hcop): $(OBJS)

CLEAN := $(lib_hcop) $(OBJS)
lib_hcop_a := $(lib_hcop)

include mk/subdir_post.mk
