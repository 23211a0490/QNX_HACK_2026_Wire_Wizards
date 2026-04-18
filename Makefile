ARTIFACT  = cpu
ARTIFACT2 = temp_sim
ARTIFACT3 = oled_display

PLATFORM ?= aarch64le
BUILD_PROFILE ?= debug

CONFIG_NAME ?= $(PLATFORM)-$(BUILD_PROFILE)
OUTPUT_DIR = build/$(CONFIG_NAME)

CC = qcc -Vgcc_nto$(PLATFORM)
CXX = q++ -Vgcc_nto$(PLATFORM)_cxx
LD = $(CC)

LIBS += -lsocket -lm

CCFLAGS_release += -O2
CCFLAGS_debug += -g -O0 -fno-builtin
CCFLAGS_coverage += -g -O0 -ftest-coverage -fprofile-arcs
LDFLAGS_coverage += -ftest-coverage -fprofile-arcs
CCFLAGS_profile += -g -O0 -finstrument-functions
LIBS_profile += -lprofilingS

CCFLAGS_all += -Wall -fmessage-length=0
CCFLAGS_all += $(CCFLAGS_$(BUILD_PROFILE))
LDFLAGS_all += $(LDFLAGS_$(BUILD_PROFILE))
LIBS_all += $(LIBS_$(BUILD_PROFILE))

$(OUTPUT_DIR)/src/cpu.o: src/cpu.c
	mkdir -p $(OUTPUT_DIR)/src
	$(CC) -c -o $@ $(INCLUDES) $(CCFLAGS_all) $(CCFLAGS) src/cpu.c

$(OUTPUT_DIR)/src/temp_sim.o: src/temp_sim.c
	mkdir -p $(OUTPUT_DIR)/src
	$(CC) -c -o $@ $(INCLUDES) $(CCFLAGS_all) $(CCFLAGS) src/temp_sim.c

$(OUTPUT_DIR)/src/oled_display.o: src/oled_display.c
	mkdir -p $(OUTPUT_DIR)/src
	$(CC) -c -o $@ $(INCLUDES) $(CCFLAGS_all) $(CCFLAGS) src/oled_display.c

$(OUTPUT_DIR)/$(ARTIFACT): $(OUTPUT_DIR)/src/cpu.o
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $(OUTPUT_DIR)/src/cpu.o $(LIBS_all) $(LIBS)

$(OUTPUT_DIR)/$(ARTIFACT2): $(OUTPUT_DIR)/src/temp_sim.o
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $(OUTPUT_DIR)/src/temp_sim.o $(LIBS_all) $(LIBS)

$(OUTPUT_DIR)/$(ARTIFACT3): $(OUTPUT_DIR)/src/oled_display.o
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $(OUTPUT_DIR)/src/oled_display.o $(LIBS_all) $(LIBS)

all: $(OUTPUT_DIR)/$(ARTIFACT) $(OUTPUT_DIR)/$(ARTIFACT2) $(OUTPUT_DIR)/$(ARTIFACT3)

clean:
	rm -fr $(OUTPUT_DIR)

rebuild: clean all