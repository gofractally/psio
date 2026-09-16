#!/usr/bin/env bash
# Verify implemented library behavior and independently consumable artifacts.
# This is not a claim that missing language/format implementations are complete.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=${1:-"$root/build/independent-check"}
mkdir -p "$work"
work=$(cd "$work" && pwd)
cmake -S "$root" -B "$work/cpp" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPSIO_ENABLE_TESTS=ON -DCMAKE_INSTALL_PREFIX="$work/install"
cmake --build "$work/cpp" --parallel "${PSIO_BUILD_JOBS:-4}"
ctest --test-dir "$work/cpp" --output-on-failure --parallel "${PSIO_BUILD_JOBS:-4}"
python3 "$root/tools/check-cpp-golden.py" "$work/cpp/cpp/conformance/psio_cpp_golden"
cmake --install "$work/cpp"
cmake -S "$root/examples/consumer" -B "$work/installed-consumer" -G Ninja \
  -DCMAKE_PREFIX_PATH="$work/install"
cmake --build "$work/installed-consumer"
ctest --test-dir "$work/installed-consumer" --output-on-failure
cmake -S "$root/examples/consumer" -B "$work/source-consumer" -G Ninja \
  -DPSIO_SOURCE_DIR="$root"
cmake --build "$work/source-consumer"
ctest --test-dir "$work/source-consumer" --output-on-failure
export CARGO_TARGET_DIR="$work/rust-target"
cargo test --manifest-path "$root/rust/Cargo.toml" --workspace --locked
cargo build --manifest-path "$root/rust/Cargo.toml" --locked --example pjson_validate
cargo package --manifest-path "$root/rust/Cargo.toml" --workspace --allow-dirty
python3 "$root/tools/check-rust-package.py" "$work/rust-target/package"
npm ci --prefix "$root/js" --ignore-scripts --no-audit --no-fund
npm run build --prefix "$root/js"
npm test --prefix "$root/js"
python3 "$root/tools/check-pjson-validation.py" \
  "$work/cpp/cpp/conformance/psio_pjson_validate" "$work/rust-target/debug/examples/pjson_validate"
python3 "$root/tools/check-npm-package.py"
