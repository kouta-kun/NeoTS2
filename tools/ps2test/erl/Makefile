# Usage: make PS2DEV=~/.sdk/ps2 PS2SDK=~/.sdk/ps2/ps2sdk
EE_CXXFLAGS += -std=c++20 -fexceptions -U__SIZEOF_INT128__
EE_LIBS += -lerl -lpthread -ldebug -lkernel
include $(PS2SDK)/samples/Makefile.pref
all: erl_test.elf
module.o: module.cpp
	$(EE_CXX) $(EE_CXXFLAGS) $(EE_INCS) -c $< -o $@
eh_begin.o: eh_begin.S
	$(EE_CC) -c $< -o $@
eh_end.o: eh_end.S
	$(EE_CC) -c $< -o $@
# Local labels are kept: string-literal relocations refer to them, so strip only debug info.
module.erl: eh_begin.o module.o eh_end.o
	$(EE_CXX) -nostartfiles -nostdlib -o $@ $^ -Wl,-r -Wl,-d
	$(EE_STRIP) --strip-debug -R .mdebug.eabi64 -R .reginfo -R .comment $@
.DELETE_ON_ERROR:
symbols.cpp: module.erl gen_symbols.py
	python3 -I gen_symbols.py $(EE_TOOL_PREFIX)nm module.erl > $@
blob.o: blob.S module.erl
	$(EE_CC) -c blob.S -o $@
erl_test.elf: host.o symbols.o blob.o ../atomics_shim.o
	$(EE_CXX) -T$(EE_LINKFILE) $(EE_OPTFLAGS) -o $@ $^ $(EE_LDFLAGS) $(EE_LIBS)
../atomics_shim.o:
	$(MAKE) -C .. atomics_shim.o
clean:
	rm -f *.o *.erl *.elf symbols.cpp
include $(PS2SDK)/samples/Makefile.eeglobal_cpp
