# cityc

`cityc` is a minimal C-subset compiler (WIP) that is meant to test `city` code generation.

It takes a list of sources as its argument, JIT compiles them, looks for a symbol named "__entry" and runs it.