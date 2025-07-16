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
      Application.fetch_env!(:jsockd_client, :bytecode_module_public_key)

    children = [
      {JSockDClient.JsServerManager,
       %{
         n_threads: n_threads,
         bytecode_module_file: bytecode_module_file,
         bytecode_module_public_key: bytecode_module_public_key
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
