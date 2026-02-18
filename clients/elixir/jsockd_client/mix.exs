defmodule JsockdClient.MixProject do
  use Mix.Project

  # __jsockd_version_check__
  @jsockd_version "0.0.146"
  @jsockd_binary_public_key "b136fca8fbfc42fe6dc95dedd035b0b50ad93b6a5d6fcaf8fc0552e9d29ee406"

  def project do
    [
      app: :jsockd_client,
      version: "0.1.0",
      elixir: "~> 1.14",
      start_permanent: Mix.env() == :prod,
      deps: deps(),
      compilers: Mix.compilers() ++ [:download_js_server_binary],
      aliases: [
        "compile.download_js_server_binary": &download_js_server_binary/1
      ]
    ]
  end

  # Run "mix help compile.app" to learn about applications.
  def application do
    [
      extra_applications: [:logger, :inets, :ssl],
      mod: {JSockDClient.Application, []},
      env: [
        n_threads: nil,
        bytecode_module_file: nil,
        bytecode_module_public_key: "",
        jsockd_exec: nil,
        source_map: nil,
        max_command_runtime_us: nil,
        max_idle_time_us: nil,
        timeout_ms: 15_000,
        use_filc_when_available?: false,
        skip_jsockd_version_check?: false
      ]
    ]
  end

  # Run "mix help deps" to learn about dependencies.
  defp deps do
    [
      # {:dep_from_hexpm, "~> 0.3.0"}
      # {:dep_from_git, git: "https://github.com/elixir-lang/my_dep.git", tag: "0.1.0"}
      {:jason, "~> 1.4.4"}
    ]
  end

  defp download_js_server_binary(_) do
    platform = :erlang.system_info(:system_architecture) |> to_string()
    use_filc_when_available? = Application.get_env(:jsockd_client, :use_filc_when_available?)

    {release_url, signature_file_url} = get_release_urls(platform, use_filc_when_available?)

    priv_dir = Path.join(__DIR__, "priv")
    File.mkdir_p!(priv_dir)
    release_filename = Path.basename(release_url)
    release_path = Path.join(priv_dir, release_filename)
    release_extracted_dirname = String.replace(release_filename, ".tar.gz", "")

    js_server_binary_filename =
      Path.join([
        priv_dir,
        release_extracted_dirname,
        "jsockd"
      ])

    unless File.exists?(js_server_binary_filename) and
             File.exists?(
               Path.join([
                 priv_dir,
                 get_version_tag(use_filc_when_available?)
               ])
             ) do
      ensure_app!(:inets)
      ensure_app!(:ssl)

      # https://security.erlef.org/secure_coding_and_deployment_hardening/inets
      # https://github.com/phoenixframework/phoenix/blob/a106791ea9ee94b4f1e9286e45a319794b3838e2/lib/mix/tasks/phx.gen.release.ex#L334
      http_options = [
        ssl: [
          verify: :verify_peer,
          cacerts: :public_key.cacerts_get(),
          depth: 3,
          customize_hostname_check: [
            match_fun: :public_key.pkix_verify_hostname_match_fun(:https)
          ],
          versions: protocol_versions()
        ]
      ]

      {:ok, {{_, status, _}, _, body}} =
        :httpc.request(:get, {release_url, []}, http_options, body_format: :binary)

      if status != 200 do
        raise "Error downloading release from #{release_url}: #{status} - #{inspect(body)}"
      end

      File.write!(release_path, body)

      {:ok, {{_, signature_status, _}, _, signature_body}} =
        :httpc.request(:get, {signature_file_url, []}, http_options, body_format: :binary)

      if signature_status != 200 do
        raise "Error downloading signature file from #{signature_file_url}: #{signature_status} - #{inspect(signature_body)}"
      end

      sigs =
        signature_body |> String.trim() |> String.split("\n") |> Enum.map(&String.split(&1, "\t"))

      sig_base64 = Enum.find(sigs, fn [_, f] -> f == Path.basename(release_filename) end)

      if sig_base64 == nil do
        raise "Could not find signature for #{release_filename} in #{signature_file_url}"
      end

      unless verify_signature(
               Base.decode64!(Enum.at(sig_base64, 0)),
               File.read!(release_path)
             ) do
        raise "Signature verification failed for JSockD server binary."
      end

      :ok =
        :erl_tar.extract(String.to_charlist(release_path), [
          {:cwd, String.to_charlist(priv_dir)},
          :compressed
        ])

      File.rm!(release_path)

      File.chmod!(
        js_server_binary_filename,
        0o500
      )

      File.touch!(Path.join([priv_dir, get_version_tag(use_filc_when_available?)]))

      File.rm_rf!(Path.join([priv_dir, "jsockd-release-artifacts"]))

      # Rename the extracted directory to a fixed name so that the rest of the code can find it.
      File.rename(
        Path.join([priv_dir, release_extracted_dirname]),
        Path.join([priv_dir, "jsockd-release-artifacts"])
      )
    end

    :ok
  end

  defp get_release_urls(platform, use_filc_when_available?) do
    release_file =
      cond do
        String.contains?(platform, "x86_64") and String.contains?(platform, "linux") ->
          if use_filc_when_available? do
            "jsockd-#{@jsockd_version}-linux-x86_64_filc.tar.gz"
          else
            "jsockd-#{@jsockd_version}-linux-x86_64.tar.gz"
          end

        String.contains?(platform, "aarch64") and String.contains?(platform, "linux") ->
          "jsockd-#{@jsockd_version}-linux-arm64.tar.gz"

        String.contains?(platform, "apple-darwin") and String.contains?(platform, "aarch64") ->
          "jsockd-#{@jsockd_version}-macos-arm64.tar.gz"

        true ->
          raise "Unsupported platform: #{platform}. Supported platforms are x86_64 and arm64 on Linux, and arm64 on MacOS."
      end

    signature_file = "ed25519_signatures.txt"

    {"https://github.com/addrummond/jsockd/releases/download/v#{@jsockd_version}/#{release_file}",
     "https://github.com/addrummond/jsockd/releases/download/v#{@jsockd_version}/#{signature_file}"}
  end

  # https://github.com/phoenixframework/phoenix/blob/a106791ea9ee94b4f1e9286e45a319794b3838e2/lib/mix/tasks/phx.gen.release.ex#L308C1-L314C6
  defp ensure_app!(app) do
    if function_exported?(Mix, :ensure_application!, 1) do
      apply(Mix, :ensure_application!, [app])
    else
      {:ok, _} = Application.ensure_all_started(app)
    end
  end

  # https://github.com/phoenixframework/phoenix/blob/a106791ea9ee94b4f1e9286e45a319794b3838e2/lib/mix/tasks/phx.gen.release.ex#L353C1-L356C6
  defp protocol_versions do
    otp_major_vsn = :erlang.system_info(:otp_release) |> List.to_integer()
    if otp_major_vsn < 25, do: [:"tlsv1.2"], else: [:"tlsv1.2", :"tlsv1.3"]
  end

  defp verify_signature(signature, challenge) do
    public_key = Base.decode16!(@jsockd_binary_public_key, case: :mixed)
    :crypto.verify(:eddsa, :none, challenge, signature, [public_key, :ed25519])
  end

  defp get_version_tag(use_filc_when_available?) do
    "jsockd_js_server_version_tag_#{@jsockd_version}#{if use_filc_when_available?, do: "_filc", else: ""}"
  end
end
