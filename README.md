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

## License

This program is free software; you can redistribute it and/or modify it under
the terms of the MIT license. See [LICENSE](LICENSE) for details.
