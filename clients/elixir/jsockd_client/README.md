# JSockDClient

An Elixir client for the JSockD server. This library starts and managers the
server process. Commands can be sent to the server using `JSockDClient.send_js/2`:

```elixir
JSockDClient.send_js("(module,params) => 99", %{"param1" => "value1", "param2" => "value2"})
```

## Installation

```elixir
def deps do
  [
    {:jsockd_client, git: "git@github.com:addrummond/jsockd.git", sparse: "/clients/elixir/jsockd_client", tag: "v0.0.96"}
  ]
end
```
