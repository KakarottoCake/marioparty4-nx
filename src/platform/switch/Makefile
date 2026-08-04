DEVKITPRO ?= C:/devkitPro
CC := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc
ELF2NRO := $(DEVKITPRO)/tools/bin/elf2nro

# Added -Wno-error=implicit-function-declaration to handle legacy C function declarations
CFLAGS := -g -Wall -O2 -ffunction-sections -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -D__SWITCH__ \
          -include switch_prefix.h -I. -Ibuild/include -I../../../src -I../../../build/GMPE01_00/include \
          -I../../../extern/musyx/include -I"$(DEVKITPRO)/libnx/include" \
          -I"$(DEVKITPRO)/portlibs/switch/include" \
          -Wno-error=incompatible-pointer-types -Wno-error=pointer-compare \
          -Wno-error=implicit-function-declaration -fcommon

LDFLAGS := -specs="$(DEVKITPRO)/libnx/switch.specs" -g -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
           -L"$(DEVKITPRO)/libnx/lib" -L"$(DEVKITPRO)/portlibs/switch/lib"

# GL rendering stack (devkitPro mesa) + math. Order matters: GLESv2 before its deps.
LIBS := -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lstdc++ -lnx -lm

OBJS := switch_main.o controller.o dvd_switch.o sys_switch.o audio_switch.o sfx_index_data.o gfx_switch.o gx_gl.o hsf_switch.o motion_switch.o jmp_switch.o projection_override/projection_override.o \
        ../../game/sprman.o ../../game/sprput.o \
        ../../game/hsfman.o ../../game/pad.o ../../game/main.o ../../game/memory.o ../../game/malloc.o ../../game/process.o \
        ../../game/objdll.o ../../game/objmain.o ../../REL/bootDll/main.o ../../REL/bootDll/language.o \
        ../../game/ovllist.o ../../game/data.o ../../game/decode.o ../../game/frand.o ../../game/dvd.o

TARGET := marioparty4_switch.nro

all: prepare $(TARGET)

prepare:
	@py -3.12 prepare_headers.py

$(TARGET): marioparty4_switch.elf
	"$(ELF2NRO)" "$<" "$@"

marioparty4_switch.elf: $(OBJS)
	"$(CC)" $(LDFLAGS) $(OBJS) $(LIBS) -o "$@"

# The GX->GL layer and the sprite manager need TARGET_PC so GX immediate-mode /
# texture calls compile as real functions instead of GameCube FIFO writes.
gx_gl.o: gx_gl.c
	"$(CC)" $(CFLAGS) -DTARGET_PC -c "$<" -o "$@"
hsf_switch.o: hsf_switch.c
	"$(CC)" $(CFLAGS) -DTARGET_PC -c "$<" -o "$@"
../../game/sprman.o: ../../game/sprman.c
	"$(CC)" $(CFLAGS) -DTARGET_PC -c "$<" -o "$@"
../../game/sprput.o: ../../game/sprput.c
	"$(CC)" $(CFLAGS) -DTARGET_PC -c "$<" -o "$@"

%.o: %.c
	"$(CC)" $(CFLAGS) -c "$<" -o "$@"

%.o: %.s
	"$(CC)" $(CFLAGS) -c "$<" -o "$@"

clean:
	rm -f $(OBJS) marioparty4_switch.elf $(TARGET)
	rm -rf build
