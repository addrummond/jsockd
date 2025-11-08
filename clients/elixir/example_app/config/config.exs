import Config

config :jsockd_client,
  n_threads: 2,
  jsockd_exec:
    Path.join(Path.dirname(__ENV__.file), "../../../../jsockd_server/build_Debug/jsockd")
