defmodule JSockDClient do
  @moduledoc """
  `JSockDClient` is an Elixir client for JSockD.
  """

  @doc """
  Sends a command to the JSockD server and returns the response, which is either
  `{:ok, value}` or `{:error, error_message_string}`.

  `function` is a string containing a JavaScript expression that evaluates to a function.

  `argument` is JSON-encoded and then passed as the second argument to the function.

  The first argument passed to the JavaScript function is the precompiled bytecode module.
  """

  # keep in sync with config.h
  @default_max_command_runtime_us 250_000

  def send_js(function, argument) do
    message_uuid = :crypto.strong_rand_bytes(8) |> Base.encode64()

    timeout_ms =
      round(
        (Application.get_env(:jsockd_client, :max_command_runtime_us) ||
           @default_max_command_runtime_us) /
          1000 * 10
      )

    try do
      JSockDClient.JsServerManager
      |> GenServer.call(
        {:send_command, message_uuid, function, Jason.encode!(argument)},
        _timeout = timeout_ms
      )
      |> parse_response()
    catch
      :exit, reason ->
        # The JS server implements its own timeout mechanism, so if we've been
        # waiting for two seconds, something is probably up with the server.
        # Send a message requesting that it be restarted.
        send(JSockDClient.JsServerManager, :hup)

        raise "JSockDClient.JsServerManager call timed out: #{inspect(reason)}"
    end
  end

  def parse_response(response) do
    if String.starts_with?(response, "exception ") do
      {:error, Jason.decode!(String.trim_leading(response, "exception "))}
    else
      {:ok, Jason.decode!(response)}
    end
  end

  def jsockd_version, do: "0.0.88"
end
