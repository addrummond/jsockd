defmodule JsockdClientTest do
  use ExUnit.Case
  doctest JsockdClient

  test "greets the world" do
    assert JsockdClient.hello() == :world
  end
end
