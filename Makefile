.PHONY: format

default: build

build: praplr

format:
	clang-format -i *.cpp
	black .

praplr: praplr.cpp
	${CXX} $^ -o $@
