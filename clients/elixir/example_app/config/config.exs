import Config

config :jsockd_client,
  n_threads: 2,
  bytecode_module_file: {:priv, :example_app, "example_module.qjsbc"},
  jsockd_exec:
    Path.join(Path.dirname(__ENV__.file), "../../../../jsockd_server/build_Debug/jsockd"),
  bytecode_module_public_key: "dangerously_allow_invalid_signatures"
