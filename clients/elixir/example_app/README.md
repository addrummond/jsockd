# ExampleApp

An bare-bones example of an Elixir app that uses the `jsockd` client library.

For ease of experimentation, this uses a debug build of jsockd and sets
`bytecode_module_public_key: "dangerously_allow_invalid_signatures"` in `config.exs`.
In real world usage, `bytecode_module_public_key` should be set to a real public
key and `jsockd_exec` should be unset.

Run `./bundle.sh` to generate bytecode from the example module in `example_module.mjs`.
