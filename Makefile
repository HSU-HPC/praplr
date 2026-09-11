INSTALL_DIR=${HOME}/.local/bin

.PHONY: format

default: build

build: praplr
install: ${INSTALL_DIR}/praplr

format:
	clang-format -i *.cpp
	black .

praplr: praplr.cpp
	${CXX} $^ -o $@

${INSTALL_DIR}/praplr: praplr
	mkdir -p ${INSTALL_DIR}
	cp $< $@
