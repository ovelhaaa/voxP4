.PHONY: test
test:
	cmake -S tests -B build-host
	cmake --build build-host -j
	ctest --test-dir build-host --output-on-failure
