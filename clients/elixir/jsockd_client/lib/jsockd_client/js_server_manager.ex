defmodule JSockDClient.JsServerManager do
  @moduledoc """
  This module creates and manages the JSockD js_server process.
  """

  use GenServer
  require Logger

  @impl true
  def init(
        opts = %{
          n_threads: n_threads,
          bytecode_module_file: bytecode_module_file,
          bytecode_module_public_key: bytecode_module_public_key,
          js_server_exec: js_server_exec
        }
      ) do
    n_threads = n_threads || :erlang.system_info(:logical_processors_online)

    uid = :crypto.strong_rand_bytes(16) |> Base.encode16()

    unix_socket_paths =
      1..n_threads
      |> Enum.map(fn i ->
        # TODO: in /tmp?
        "/tmp/jsockd_#{uid}_#{i}.sock"
      end)

    exec =
      js_server_exec ||
        Path.join([:code.priv_dir(:jsockd_client), "release-artifacts/jsockd/js_server"])

    port_id =
      Port.open({:spawn_executable, exec}, [
        :use_stdio,
        :in,
        :exit_status,
        line: 80,
        args: [
          "-b",
          "00",
          "-m",
          bytecode_module_file,
          "-s",
          "--"
          | unix_socket_paths
        ],
        env: [
          {~c"JSOCKD_BYTECODE_MODULE_PUBLIC_KEY", String.to_charlist(bytecode_module_public_key)}
        ]
      ])

    # we now wait for the handle_info call for the ready message

    {:ok,
     %{
       opts: opts,
       port_id: port_id,
       unix_socket_paths: unix_socket_paths,
       unix_sockets_with_threads: [],
       pending_calls: %{}
     }}
  end

  @impl true
  def terminate(_reason, state) do
    Enum.each(state.unix_sockets_with_threads, fn {{_path, sock}, _pid} ->
      :ok = :socket.close(sock)
    end)

    Port.close(state.port_id)
  end

  def start_link(arg) do
    GenServer.start_link(__MODULE__, arg, name: __MODULE__)
  end

  @impl true
  def handle_info({_port, {:data, {_, msg}}}, state) do
    if state.unix_sockets_with_threads != [] do
      raise "Unexpected message received from js_server_exec: #{inspect(msg)}"
    end

    msg = List.to_string(msg)

    case Regex.run(~r/^READY (\d+)/, msg) do
      nil ->
        raise "Bad message received from js_server_exec: #{msg}"

      [_, n_threads] ->
        n_threads = String.to_integer(n_threads)

        Process.flag(:trap_exit, true)

        # Start one process to own IO access to each socket.
        unix_sockets_with_threads =
          state.unix_socket_paths
          |> Enum.take(n_threads)
          |> Enum.map(fn unix_socket_path ->
            sock = make_unix_socket(unix_socket_path)
            pid = start_socket_thread(sock)
            {{unix_socket_path, sock}, pid}
          end)

        {:noreply, %{state | unix_sockets_with_threads: unix_sockets_with_threads}}
    end
  end

  @impl true
  def handle_info({:EXIT, pid, :client_down}, state) do
    {new, unix_sockets_with_threads} =
      Enum.reduce(state.unix_sockets_with_threads, {nil, []}, fn elem = {{path, sock}, usockpid},
                                                                 {new, acc} ->
        if usockpid == pid do
          {{make_unix_socket(path), start_socket_thread(sock)}, acc}
        else
          {new, [elem | acc]}
        end
      end)

    case new do
      nil ->
        {:noreply, state}

      new ->
        {:noreply, %{state | unix_sockets_with_threads: [new | unix_sockets_with_threads]}}
    end
  end

  @impl true
  def handle_info({_port, {:exit_status, status}}, _state) do
    raise "js_server_exec unexpectedly exited with status #{status}"
  end

  @impl true
  def handle_info({:send_reply, from, reply}, state) do
    [uuid, contents] = String.split(reply, " ", parts: 2)

    expected_uuid = Map.get(state.pending_calls, from)

    if uuid != expected_uuid do
      Logger.warning(
        "Received reply with mismatched UUID: expected #{expected_uuid}, got #{uuid}"
      )

      {:noreply, state}
    else
      GenServer.reply(from, contents)
      {:noreply, %{state | pending_calls: Map.delete(state.pending_calls, from)}}
    end
  end

  @impl true
  def handle_info(:hup, state) do
    Port.close(state.port_id)
    {:ok, state} = init(state.opts)
    {:noreply, nil, state}
  end

  @impl true
  def handle_call({:send_command, message_uuid, function, argument_string}, from, state) do
    if state.unix_sockets_with_threads == [] do
      raise "JSockDClient.JsServerManager not ready to send commands: no unix sockets available"
    end

    {_, usockpid} = Enum.random(state.unix_sockets_with_threads)
    send(usockpid, {:send, message_uuid, function, argument_string, from, self()})

    {:noreply, %{state | pending_calls: Map.put(state.pending_calls, from, message_uuid)}}
  end

  defp start_socket_thread(sock) do
    spawn_link(fn ->
      loop = fn loop ->
        receive do
          {:send, message_uuid, function, argument, from, reply_pid} ->
            function = String.trim(function)
            argument = String.trim(argument)

            if String.contains?(function, "\x00") or
                 String.contains?(argument, "\x00") do
              raise "Function or argument contains null byte: #{inspect(function)}, #{inspect(argument)}"
            end

            :ok = :socket.send(sock, [message_uuid, "\x00", function, "\x00", argument, "\x00"])

            recv_loop = fn recv_loop, acc ->
              case :socket.recv(sock, 0) do
                {:ok, data} ->
                  acc = [data | acc]

                  cond do
                    String.ends_with?(data, "\n") ->
                      send(
                        reply_pid,
                        {:send_reply, from, String.trim(acc |> Enum.reverse() |> Enum.join(""))}
                      )

                    String.contains?(data, "\n") ->
                      raise "Unexpected data received: #{inspect(data)}"

                    true ->
                      recv_loop.(recv_loop, acc)
                  end
              end
            end

            recv_loop.(recv_loop, [])
        end

        loop.(loop)
      end

      loop.(loop)
    end)
  end

  defp make_unix_socket(path) do
    {:ok, sock} = :socket.open(:local, :stream, :default)
    :ok = :socket.connect(sock, %{family: :local, path: path})
    sock
  end
end
