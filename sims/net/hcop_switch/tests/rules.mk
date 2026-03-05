include mk/subdir_pre.mk

test_fastpath := $(d)test_fastpath
OBJS_test_fastpath := $(d)test_fastpath.o $(d)../primitive_engine.o $(d)../switch_config.o

# Add includes for test
$(d)test_fastpath.o: CPPFLAGS += -Ilib -I$(d)../

$(test_fastpath): $(OBJS_test_fastpath) $(lib_hcop)
	$(LINK) $(OBJS_test_fastpath) $(lib_hcop) -lpthread -o $@

# Add to global tests?
# SimBricks usually puts tests in 'tests' target?
# But for now, just define it. We run it manually.
CLEAN := $(test_fastpath) $(OBJS_test_fastpath)

include mk/subdir_post.mk
