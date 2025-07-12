defmodule JSockDClient do
  @moduledoc """
  Documentation for `JSockDClient`.
  """

  def send_js(function, argument) do
    message_uuid = :crypto.strong_rand_bytes(8) |> Base.encode64()

    try do
      JSockDClient.JsServerManager
      |> GenServer.call(
        {:send_command, message_uuid, function, JSON.encode!(argument)},
        _timeout = 800
      )
      |> JSON.decode!()
    catch
      :exit, reason ->
        # The JS server implements its own timeout mechanism, so if we've been
        # waiting for two seconds, something is probably up with the server.
        # Send a message requesting that it be restarted.
        send(JSockDClient.JsServerManager, :hup)

        raise "JSockDClient.JsServerManager call timed out: #{inspect(reason)}"
    end
  end
end
