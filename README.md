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
development branch. Assert both `FE_API_VERSION` (the C embedding contract)
and `FE_LANGUAGE_VERSION` (the Lisp language evaluated) at compile time, run
`make core` with both GCC and Clang when updating the pin, and review the
version policy in `doc/c-api.md`. Preserve `LICENSE` and the copyright/SPDX
notices in vendored source. An update consists of selecting the new released
commit, reviewing any API- or language-version change, advancing the
submodule pin, and rerunning the downstream build and tests.

## Regular Expression Extension

If compiled with the regular expression extension (e.g., in `kg`), the following functions are available:

### `(compile-re pattern [flags])`
Compiles a regular expression pattern string. `flags` is an optional list containing the symbol `'icase` for case-insensitive matching.
Returns a regex object on success, or `(error code message)` on failure (e.g., bad pattern, too complex).

### `(match-re regex text [offset])`
Matches the compiled `regex` against `text` string, starting at the optional byte `offset` (defaults to 0).
Returns a list of match spans if a match is found, or `nil` if there is no match.
Each span is a list `(start end)` of byte indexes. The first span is the overall match, followed by capture spans; unmatched optional captures are `nil`.

## License

This program is free software; you can redistribute it and/or modify it under
the terms of the MIT license. See [LICENSE](LICENSE) for details.
