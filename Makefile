CXXFLAGS += -std=c++17 -Wall -I.

ulm.path := $(patsubst %/,%,$(shell cat "path-to-ulm"))
ulm.as := $(ulm.path)/ulmas
ulm.ulm := $(ulm.path)/ulm
install.dir := $(HOME)/bin

#
# patch: If user has not defined CC and default value does not exist use gcc
#
ifeq ($(origin CXX),default)
    cxx_check := $(shell $(CXX) -v > /dev/null 2>&1 && echo "sane")
    ifneq ($(strip $(cxx_check)),sane)
        CXX := g++
    endif
endif

target := ulmld ulmranlib_mkindex

gen := ./include_call_start
gen.in := call_start.o
gen.out := call_start.hpp

all: $(target)

install: ulmld
	install $< $(install.dir)
	

ulmld : ulmld.cpp $(gen.out)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

$(gen.out) : $(gen) $(gen.in) path-to-ulm
	./$^

include_call_start : include_call_start.cpp | call_start.o

%.o : %.s
	$(ulm.as) -o $@ $^

clean:
	$(RM) $(target) $(gen.in) $(gen.out) $(gen)
