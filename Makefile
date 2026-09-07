.PHONY: setup check-env build test

setup:
	./scripts/setup-codex.sh

check-env:
	./scripts/check-environment.sh

build:
	./scripts/build-p4.sh

test:
	cmake -S tests -B build-host
	cmake --build build-host --parallel
	ctest --test-dir build-host --output-on-failure
