defmodule JSockDClient.Application do
  @moduledoc false

  @behaviour Application
  @behaviour Supervisor

  def start(_type, args) do
    Supervisor.start_link(__MODULE__, args, strategy: :one_for_one)
  end

  def init(_) do
    n_threads =
      Application.fetch_env!(:jsockd_client, :n_threads)

    bytecode_module_file = get_bytecode_module_file()

    bytecode_module_public_key =
      Application.get_env(:jsockd_client, :bytecode_module_public_key)

    js_server_exec =
      Application.get_env(:jsockd_client, :js_server_exec)

    source_map =
      Application.get_env(:jsockd_client, :source_map)

    max_command_runtime_us =
      Application.get_env(:jsockd_client, :max_command_runtime_us)

    children = [
      {JSockDClient.JsServerManager,
       %{
         n_threads: n_threads,
         bytecode_module_file: bytecode_module_file,
         bytecode_module_public_key: bytecode_module_public_key,
         js_server_exec: js_server_exec,
         source_map: source_map,
         max_command_runtime_us: max_command_runtime_us
       }}
    ]

    Supervisor.init(children, strategy: :one_for_one)
  end

  def stop(_state) do
    :ok
  end

  defp get_bytecode_module_file do
    :jsockd_client
    |> Application.fetch_env!(:bytecode_module_file)
    |> case do
      {:priv, application, file} -> Path.join([:code.priv_dir(application), file])
      file -> file
    end
  end
end
