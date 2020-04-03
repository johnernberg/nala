BUILD = build
EXE = $(BUILD)/app
INC += $(BUILD)
INC += $(CURDIR)
SRC += $(BUILD)/nala_mocks.c
SRC += $(TESTS)
OBJ = $(patsubst %,$(BUILD)%,$(abspath $(SRC:%.c=%.o)))
OBJDEPS = $(OBJ:%=%.d)
MOCKGENDEPS = $(BUILD)/nala_mocks.ldflags.d
DEPS = $(OBJDEPS) $(MOCKGENDEPS)
CFLAGS += $(INC:%=-I%)
CFLAGS += -g -O0 -no-pie
ifeq ($(SANITIZE), yes)
CFLAGS += -fsanitize=address
CFLAGS += -fsanitize=undefined
endif
MOCKGENFLAGS += $(IMPLEMENTATION:%=-i %)
MOCKGENFLAGS += $(NO_IMPLEMENTATION:%=-n %)
NALA ?= PYTHONPATH=$(NALA_ROOT) python3 -m nala

all:
	$(MAKE) -C $(NALA_ROOT) dist
	$(MAKE) $(BUILD)/nala_mocks.ldflags
	$(MAKE) $(EXE)
	$(EXE) $(ARGS)

clean:
	rm -rf $(BUILD)

gdb:
	gdb \
	    -ex "b $(TEST)_before_fork" \
	    -ex r \
	    -ex "set follow-fork-mode child" \
	    -ex c \
	    $(EXE)

$(EXE): $(OBJ)
	echo "LD $@"
	$(CC) $(CFLAGS) @$(BUILD)/nala_mocks.ldflags $^ -o $@

define COMPILE_template
$(patsubst %.c,$(BUILD)%.o,$(abspath $1)): $1
	@echo "CC $1"
	mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) -c -o $$@ $$<
	$(NALA) wrap_internal_symbols $(BUILD)/nala_mocks.ldflags $$@
	$(CC) -MM -MT $$@ $(CFLAGS) -o $$@.d $$<
endef
$(foreach file,$(SRC),$(eval $(call COMPILE_template,$(file))))

$(BUILD)/nala_mocks.ldflags: $(TESTS)
	echo "MOCKGEN $(TESTS)"
	mkdir -p $(@D)
	[ -f $(BUILD)/nala_mocks.h ] || touch $(BUILD)/nala_mocks.h
	cat $(TESTS) \
	    | $(CC) $(CFLAGS) -DNALA_GENERATE_MOCKS -x c -E - \
	    | $(NALA) generate_mocks $(MOCKGENFLAGS) -o $(BUILD)
	cat $(TESTS) \
	    | $(CC) -MM -MT $@ $(CFLAGS) -x c -o $@.d -
	touch $@

-include $(DEPS)
