INSTALL_DIR=${HOME}/.local/bin
CXX ?= g++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -O2

.PHONY: all build install format check-rapl-method

default: build

build: praplr

praplr: praplr.cpp praplr.h
	$(CXX) $(CXXFLAGS) praplr.cpp -o $@

install: $(INSTALL_DIR)/praplr

$(INSTALL_DIR)/praplr: praplr
	mkdir -p $(INSTALL_DIR)
	cp $< $@

format:
	clang-format -i *.cpp
	black .
