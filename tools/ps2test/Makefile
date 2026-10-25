# Usage: make PS2DEV=~/.sdk/ps2 PS2SDK=~/.sdk/ps2/ps2sdk
EE_INCS += -I$(PS2DEV)/gsKit/include
EE_LDFLAGS += -L$(PS2DEV)/gsKit/lib
EE_CXXFLAGS += -std=c++20 -fexceptions -U__SIZEOF_INT128__
EE_LIBS += -lpthread -ldebug -lmc -lpatches -lkernel
include $(PS2SDK)/samples/Makefile.pref

all: atomics_test.elf memcard_bench.elf gs_hello.elf

atomics_test.elf: atomics_test.o atomics_shim.o
	$(EE_CXX) -T$(EE_LINKFILE) $(EE_OPTFLAGS) -o $@ $^ $(EE_LDFLAGS) $(EE_LIBS)
memcard_bench.elf: memcard_bench.o
	$(EE_CXX) -T$(EE_LINKFILE) $(EE_OPTFLAGS) -o $@ $^ $(EE_LDFLAGS) $(EE_LIBS)
clean:
	rm -f *.o *.elf
include $(PS2SDK)/samples/Makefile.eeglobal_cpp

gs_hello.elf: gs_hello.o
	$(EE_CXX) -T$(EE_LINKFILE) $(EE_OPTFLAGS) -o $@ $^ $(EE_LDFLAGS) -lgskit -ldmakit $(EE_LIBS)
