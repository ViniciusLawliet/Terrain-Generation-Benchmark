.PHONY: all common openmp cuda benchmark clean

all: common openmp benchmark cuda

common:
	$(MAKE) -C common

openmp: common
	$(MAKE) -C openmp

cuda: common
	$(MAKE) -C cuda

benchmark:
	$(MAKE) -C benchmark

clean:
	$(MAKE) -C common clean
	$(MAKE) -C openmp clean
	$(MAKE) -C cuda clean
	$(MAKE) -C benchmark clean
