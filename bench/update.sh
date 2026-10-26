#!/usr/bin/bash

pushd include/rust
cargo update
popd

pushd include/rust-nightly
cargo +nightly update
popd
