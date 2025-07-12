defmodule JSockDClient.Application do
  @moduledoc false

  @behaviour Application
  @behaviour Supervisor

  def start(_type, args) do
    Supervisor.start_link(__MODULE__, args, strategy: :one_for_one)
  end

  def init(_) do
    js_server_exec = Application.get_env(:jsockd_client, :js_server_exec)

    children = [
      {JSockDClient.JsServerManager,
       %{
         js_server_exec: js_server_exec
       }}
    ]

    Supervisor.init(children, strategy: :one_for_one)
  end

  def stop(_state) do
    :ok
  end
end
