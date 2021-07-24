#!/usr/bin/env bash
set -ev

if [[ "$(uname)" == "Linux" ]]; then
  SOURCE_DIR="$(dirname "$(readlink -f "$0")")"
  export SOURCE_DIR=${SOURCE_DIR}
elif [[ "$(uname)" == "Darwin" ]]; then
  SOURCE_DIR="$(dirname "$(realpath "$0")")"
  export SOURCE_DIR=${SOURCE_DIR}
fi
export BUILD_DIR=${APPVEYOR_BUILD_FOLDER:-$(pwd)}

build_tests() {
  mkdir -p "${BUILD_DIR}/$1"
  cd "${BUILD_DIR}/$1" || exit 1
  cmake -DCMAKE_C_FLAGS="${CMAKE_C_FLAGS}" -DCMAKE_CXX_FLAGS="${CMAKE_CXX_FLAGS}" -DCMAKE_BUILD_TYPE="$2" -DCMAKE_VERBOSE_MAKEFILE=1 -DOVERTHROWER_WITH_LIBUNWIND="${USE_LIBUNWIND}" "${SOURCE_DIR}"
  cmake --build .
}

execute_tests() {
  if [[ "$(uname)" == "Linux" ]]; then
    LD_LIBRARY_PATH=. LD_PRELOAD=liboverthrower.so ./overthrower_tests
  elif [[ "$(uname)" == "Darwin" ]]; then
    DYLD_FORCE_FLAT_NAMESPACE=1 DYLD_INSERT_LIBRARIES=./Frameworks/overthrower.framework/Versions/Current/overthrower ./overthrower_tests
  fi
}

if [[ ! -d "${SOURCE_DIR}/googletest" ]]; then
  git clone --branch release-1.11.0 --depth 1 https://github.com/google/googletest.git "${SOURCE_DIR}/googletest"
fi

if [[ "$(uname)" == "Linux" ]]; then
  if [[ -n "${GCC_VERSION}" ]]; then
    export CC="gcc-${GCC_VERSION}"
    export CXX="g++-${GCC_VERSION}"
  else
    export CC="gcc"
    export CXX="g++"
  fi
fi
build_tests default Release
execute_tests

export CMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping"
export CMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping"
export CMAKE_BUILD_TYPE=Debug
if [[ "$(uname)" == "Linux" ]]; then
  export OVERTHROWER_LIBRARY_PATH=./liboverthrower.so
  export LLVM_PROFDATA=llvm-profdata-10
  export LLVM_COV=llvm-cov-10
  export CC=clang
  export CXX=clang++
elif [[ "$(uname)" == "Darwin" ]]; then
  export OVERTHROWER_LIBRARY_PATH=./Frameworks/overthrower.framework/Versions/Current/overthrower
  export LLVM_PROFDATA="xcrun llvm-profdata"
  export LLVM_COV="xcrun llvm-cov"
fi
build_tests coverage Debug
execute_tests
${LLVM_PROFDATA} merge -sparse default.profraw -o default.profdata
${LLVM_COV} show ${OVERTHROWER_LIBRARY_PATH} -instr-profile=default.profdata -Xdemangler=c++filt
${LLVM_COV} report ${OVERTHROWER_LIBRARY_PATH} -instr-profile=default.profdata
