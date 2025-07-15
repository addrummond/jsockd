import Config

config :jsockd_client,
  n_threads: 2,
  bytecode_module_file:
    File.cwd!() |> Path.join("../../../example_module.qjsbc") |> Path.expand(),
  bytecode_module_public_key: "dangerously_allow_invalid_signatures"
