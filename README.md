# Fe

Fe is a tiny, embeddable, Lisp-like language implemented in C23.

This version is a refactored and extended version of [the beautiful original by
rxi](https://github.com/rxi/fe).

## Overview

The language offers the following features:

* Numbers, symbols, strings, pairs, lambdas, and macros
* Lexically scoped variables
* Closures
* Variadic functions
* Mark-and-sweep garbage collector
* Stack traceback on error

The implementation aims to fulfill the following goals:

* Practical for small scripts (extension scripts, configuration files)
* Small memory usage within a fixed-size, caller-allocated arena — no `malloc`s
* Simple mark-and-sweep garbage collector
* Easy-to-use C API
* Concise, readable, and portable implementation
* Extensible — an extension API allows the core to remain small and stable

## Documentation

* [Example scripts](scripts) and their [test expectations](tests)
* [C API overview](doc/c-api.md)
* [Language overview](doc/language.md)
* [Implementation overview](doc/implementation.md)
* [Fuzzing and crash triage](doc/FUZZING.md)

## Contributing

Bug reports, pull requests, and questions are welcome.

## Downstream Embedding

Downstreams such as kg should include Fe as a git submodule pinned to an exact
released tag and commit; they should not track the moving `analyzers-etc`
development branch. Assert `FE_API_VERSION` at compile time, run `make core`
with both GCC and Clang when updating the pin, and review the API-version policy
in `doc/c-api.md`. Preserve `LICENSE` and the copyright/SPDX notices in vendored
source. An update consists of selecting the new released commit, reviewing any
API-version change, advancing the submodule pin, and rerunning the downstream
build and tests.

## License

This program is free software; you can redistribute it and/or modify it under
the terms of the MIT license. See [LICENSE](LICENSE) for details.
