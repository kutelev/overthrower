#!/usr/bin/env bash
set -ev

execute_tests()
{
    if [[ "$(uname)" == "Linux" ]]; then
        LD_LIBRARY_PATH=. LD_PRELOAD=liboverthrower.so ./overthrower_tests
    elif [[ "$(uname)" == "Darwin" ]]; then
        DYLD_FORCE_FLAT_NAMESPACE=1 DYLD_INSERT_LIBRARIES=./overthrower.framework/Versions/Current/overthrower ./overthrower_tests
    fi
}

git clone --branch release-1.10.0 --depth 1 https://github.com/google/googletest.git

export BUILD_DIR=${TRAVIS_BUILD_DIR:-.}

mkdir "${BUILD_DIR}/default"
cd "${BUILD_DIR}/default" || exit 1
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=1 "${BUILD_DIR}"
cmake --build .
execute_tests

if [[ "${MEASURE_COVERAGE}" == "1" ]]; then
    export CMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping"
    export CMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping"
    export CMAKE_BUILD_TYPE=Debug
    if [[ "$(uname)" == "Linux" ]]; then
        export OVERTHROWER_LIBRARY_PATH=./liboverthrower.so
        export LLVM_PROFDATA=llvm-profdata
        export LLVM_COV=llvm-cov
        export CC=clang
        export CXX=clang++
    elif [[ "$(uname)" == "Darwin" ]]; then
        export OVERTHROWER_LIBRARY_PATH=./overthrower.framework/Versions/Current/overthrower
        export LLVM_PROFDATA="xcrun llvm-profdata"
        export LLVM_COV="xcrun llvm-cov"
    fi
    mkdir "${BUILD_DIR}/coverage"
    cd "${BUILD_DIR}/coverage" || exit 1
    cmake -DCMAKE_C_FLAGS="${CMAKE_C_FLAGS}" -DCMAKE_CXX_FLAGS="${CMAKE_CXX_FLAGS}" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_VERBOSE_MAKEFILE=1 "${BUILD_DIR}"
    cmake --build .
    execute_tests
    ${LLVM_PROFDATA} merge -sparse default.profraw -o default.profdata
    ${LLVM_COV} show ${OVERTHROWER_LIBRARY_PATH} -instr-profile=default.profdata -Xdemangler=c++filt
    ${LLVM_COV} report ${OVERTHROWER_LIBRARY_PATH} -instr-profile=default.profdata
fi
