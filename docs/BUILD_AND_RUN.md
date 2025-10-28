// FILE: docs/BUILD_AND_RUN.md
# Build & Run (Milestone 1)

Prereqs (Linux/macOS):
- cmake >= 3.10
- g++ with C++17 support

1. Create vendor directory and download headers:
```bash
mkdir -p vendor data
cd vendor
curl -L -o httplib.h https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h
curl -L -o json.hpp https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp
curl -L -o catch.hpp https://raw.githubusercontent.com/catchorg/Catch2/devel/single_include/catch2/catch.hpp
cd ..
```

2. Build:
```bash
mkdir -p build && cd build
cmake ..
cmake --build . -- -j4
```

3. Run server:
```bash
./matching_engine 8080
```

4. Run tests:
```bash
ctest --output-on-failure
```


// END OF FILE
