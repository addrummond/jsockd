import assert from "node:assert/strict";
import { parseBacktrace } from "./backtrace.mjs";

function parsedBacktrace(sourcemap, backtrace) {
  return JSON.parse(parseBacktrace(sourcemap, backtrace));
}

{
  const parsed = parsedBacktrace(
    null,
    [
      "Error: first line",
      "second line",
      "    at <input>:1:2",
      "    at parse (native)",
      "    at spaced name (<cmdline>:1:45)",
      "    at loader (file:///tmp/bundle.js:12:34)",
    ].join("\n"),
  );

  assert.equal(parsed.errorMessage, "Error: first line\nsecond line");
  assert.deepEqual(parsed.trace, [
    {
      functionName: null,
      source: "<input>",
      line: "1",
      column: "2",
    },
    {
      functionName: "parse",
      source: "native",
      line: null,
      column: null,
    },
    {
      functionName: "spaced name",
      source: "<cmdline>",
      line: "1",
      column: "45",
    },
    {
      functionName: "loader",
      source: "file:///tmp/bundle.js",
      line: "12",
      column: "34",
    },
  ]);
  assert.match(parsed.pretty, /at parse \(native\)/);
  assert.doesNotMatch(parsed.pretty, /Error: first linesecond line/);
}

{
  const sourcemap = {
    version: 3,
    sources: ["original.js"],
    names: ["first", "second"],
    mappings: "KAAAA,KAAAC",
  };
  const parsed = parsedBacktrace(
    JSON.stringify(sourcemap),
    [
      "Error: boom",
      "    at f (bundle.js:1:10)",
      "    at g (bundle.js:1:11)",
    ].join("\n"),
  );

  assert.equal(parsed.trace[0].mapped.functionName, "first");
  assert.equal(parsed.trace[1].mapped.functionName, "second");
}

{
  const makeSourcemap = (source, name) =>
    JSON.stringify({
      version: 3,
      sources: [source],
      names: [name],
      mappings: "AAAAA",
    });
  const backtrace = "Error: boom\n    at f (bundle.js:1:1)";

  assert.equal(
    parsedBacktrace(makeSourcemap("first.js", "first"), backtrace).trace[0]
      .mapped.source,
    "first.js",
  );
  assert.equal(
    parsedBacktrace(makeSourcemap("second.js", "second"), backtrace).trace[0]
      .mapped.source,
    "second.js",
  );
}
