##
## Host-side unit tests for libopenwch-tools.
##
## These cover the parts that are pure logic, so they run anywhere, including
## CI with no programmer attached.  Anything that needs a WCH-LinkE is not here
## and cannot be: it needs hardware.
##
## Driven from the top level with `make test`, which passes the path of the
## built binary in PROJECT.
##

CC		?= cc
CFLAGS		?= -O2 -g
CFLAGS		+= -std=c11 -Wall -Wextra -Wpedantic

PROJECT		?= ../wchlink

TOP		= ..
BUILD_DIR	= build

TESTS		= target_test cli_test

all: run

$(BUILD_DIR)/target_test: target_test.c $(TOP)/src/target.c $(TOP)/src/target.h
	@printf "  CC      $<\n"
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(TOP)/src -o $@ target_test.c $(TOP)/src/target.c

## The CLI test drives the real binary, so it is a black-box test of argument
## handling and exit codes rather than of any particular function.
$(BUILD_DIR)/cli_test: cli_test.sh
	@printf "  TEST    $<\n"
	@mkdir -p $(BUILD_DIR)
	@cp cli_test.sh $@
	@chmod +x $@

run: $(BUILD_DIR)/target_test $(BUILD_DIR)/cli_test
	@$(BUILD_DIR)/target_test
	@PROJECT=$(PROJECT) $(BUILD_DIR)/cli_test

clean:
	$(RM) -r $(BUILD_DIR)

.PHONY: all run clean
