import Config

config :jsockd_client,
  n_threads: 2,
  bytecode_module_file: {:priv, :example_app, "example_module.qjsbc"},
  bytecode_module_public_key: "f9d557e655368f4c9443d0d7c1182cc18bceede292199c72aec5341f2e29bd86"
