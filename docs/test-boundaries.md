# Standalone versus product-fixture tests

The extracted headless suite contains two different responsibilities: standalone
engine/protocol behavior, and Panda OS fixture provisioning. Fifteen existing
fixture tests call the consumer Toolchain or copy its EPUB/font/course fixtures.
Those assertions must stay intact, but cannot be hidden implicit dependencies
of `cargo test` in a clone that deliberately does not contain Murphy.

The standalone target runs the generic suite by default. The named
`product-fixtures` Cargo feature retains every product-fixture assertion and is
required by the Murphy integration gate, with an explicit
`PANDA_SIMULATOR_PROJECT_ROOT` pointing at the consumer source/fixture root.
A missing source root or asset fails that product gate; it is never a passing
skip. Tests are not deleted and their failure results remain evidence until
that gate runs. Product integration and standalone counts are reported separately.
The public minimum guest example still builds and runs independently.
