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

    bytecode_module_file = get_file_from_config(:bytecode_module_file)

    bytecode_module_public_key =
      Application.get_env(:jsockd_client, :bytecode_module_public_key)

    jsockd_exec = get_file_from_config(:jsockd_exec)

    source_map = get_file_from_config(:source_map)

    max_command_runtime_us =
      Application.get_env(:jsockd_client, :max_command_runtime_us)

    max_idle_time_us =
      Application.get_env(:jsockd_client, :max_idle_time_us)

    use_filc_when_available? =
      Application.get_env(:jsockd_client, :use_filc_when_available?, false)

    children = [
      {JSockDClient.JsServerManager,
       %{
         n_threads: n_threads,
         bytecode_module_file: bytecode_module_file,
         bytecode_module_public_key: bytecode_module_public_key,
         jsockd_exec: jsockd_exec,
         source_map: source_map,
         max_command_runtime_us: max_command_runtime_us,
         max_idle_time_us: max_idle_time_us,
         use_filc_when_available?: use_filc_when_available?
       }}
    ]

    Supervisor.init(children, strategy: :one_for_one)
  end

  def stop(_state) do
    :ok
  end

  defp get_file_from_config(key) do
    Application.fetch_env!(:jsockd_client, key)
    |> case do
      {:priv, application, file} -> Path.join([:code.priv_dir(application), file])
      file -> file
    end
  end
end
