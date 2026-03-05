include mk/subdir_pre.mk
# HCOP Switch Build Rules
bin_hcop_switch := $(d)hcop_switch
OBJS_hcop_switch := $(addprefix $(d),device.o primitive_engine.o switch_main.o switch_config.o net_port.o)

$(OBJS_hcop_switch): CPPFLAGS := $(CPPFLAGS) -I$(d)../ -Ilib

$(bin_hcop_switch): $(OBJS_hcop_switch) $(lib_hcop) $(lib_netif) $(lib_base)
	$(LINK) $(OBJS_hcop_switch) $(lib_hcop) $(lib_netif) $(lib_base) -lpthread -o $@

CLEAN := $(bin_hcop_switch) $(OBJS_hcop_switch)

$(eval $(call subdir,tests))

include mk/subdir_post.mk
