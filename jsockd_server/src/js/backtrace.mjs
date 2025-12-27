let parsedSourcemap = null;

// Bellard's QuickJS doesn't expose structured backtraces via the public API,
// so we need to parse the backtrace's string representation.
export function parseBacktrace(sourcemap, backtrace) {
  const bt = parseBacktraceHelper(sourcemap, backtrace);
  bt.pretty = formatParsedBacktrace(bt);
  return JSON.stringify(bt);
}

function parseBacktraceHelper(sourcemap, backtrace) {
  const lines = backtrace.split("\n");
  const trace = [];
  let errorMessage = "";
  let foundMatch = false;

  for (const line of lines) {
    const match = line.match(
      /^\s+at ([^\s]+)(?:\s+\(([^:]*):(\d+)(?::(\d+))?\))?\s*$/,
    );
    if (match) {
      const functionName = match[1];
      const source = match[2] ?? "unknown location";
      const line = match[3] ?? null;
      const column = match[4] ?? null;
      trace.push({ functionName, source, line, column });
      foundMatch = true;
    } else if (!foundMatch) {
      errorMessage += line;
    }
  }

  if (parsedSourcemap === null)
    parsedSourcemap = sourcemap ? JSON.parse(sourcemap) : null;

  return {
    errorMessage: errorMessage.trim(),
    trace: sourcemap
      ? mapBacktraceWithSourceMap(parsedSourcemap, trace)
      : trace,
    raw: backtrace.trim(),
  };
}

export function formatBacktrace(sourcemap, backtrace) {
  backtrace = parseBacktraceHelper(sourcemap, backtrace);
  return formatParsedBacktrace(backtrace);
}

function formatParsedBacktrace(backtrace) {
  let bt = `
${backtrace.errorMessage}:
${backtrace.trace
  .map((entry) => {
    return `  at ${entry.functionName ?? "<unknown>"} (${entry.source ?? "<unknown>"}${entry.line !== null ? `:${entry.line}` : ""}${entry.column !== null ? `:${entry.column}` : ""})${(() => {
      if (entry.mapped)
        return ` -> ${entry.mapped.functionName ? `${entry.mapped.functionName} (` : ""}${entry.mapped.source ?? "<unknown>"}${entry.mapped.line !== null ? `:${entry.mapped.line}` : ""}${entry.mapped.column !== null ? `:${entry.mapped.column}` : ""}${entry.mapped.functionName ? ")" : ""}`;
      return "";
    })()}`;
  })
  .join("\n")}`;

  bt = bt.trimEnd();
  if (bt.length > 0 && bt[bt.length - 1] == ":")
    bt = bt.substring(0, bt.length - 1);
  return bt;
}

// WARNING: AI-generated code from this point down. Haven't checked it very
// carefully yet.

/**
 * Maps a backtrace using a sourcemap.
 * @param {Object} sourcemap - The sourcemap object (standard Source Map V3 format).
 * @param {Array} backtrace - Array of objects with 'lineNumber' and 'columnNumber'.
 * @returns {Array} - Array of backtrace entries with mapped source locations.
 */
export function mapBacktraceWithSourceMap(sourcemap, backtrace) {
  // Helper to decode VLQ (Variable-length quantity) used in source maps
  function decodeVLQ(str) {
    const chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let result = [];
    let shift = 0,
      value = 0,
      sign = 1;
    for (let i = 0; i < str.length; ++i) {
      let c = chars.indexOf(str[i]);
      if (c === -1) continue;
      let digit = c & 31;
      value += digit << shift;
      if (c & 32) {
        shift += 5;
      } else {
        sign = value & 1 ? -1 : 1;
        result.push(sign * (value >> 1));
        value = 0;
        shift = 0;
      }
    }
    return result;
  }

  // Parse the mappings string into a line/column map
  function parseMappings(mappings, sources, names) {
    let sourceIndex = 0;
    let originalLine = 0;
    let originalColumn = 0;
    let nameIndex = 0;
    let lines = mappings.split(";");
    let mappingTable = [];

    for (let i = 0; i < lines.length; ++i) {
      let line = lines[i];
      let segments = line.split(",");
      let lineMappings = [];
      let col = 0;
      for (let j = 0; j < segments.length; ++j) {
        if (!segments[j]) continue;
        let decoded = decodeVLQ(segments[j]);
        col += decoded[0] ?? 0;
        if (decoded.length > 1) {
          sourceIndex += decoded[1];
          originalLine += decoded[2];
          originalColumn += decoded[3];
          if (decoded.length > 4) {
            nameIndex += decoded[4];
          }
          lineMappings.push({
            generatedColumn: col,
            source: sources[sourceIndex],
            originalLine: originalLine + 1,
            originalColumn: originalColumn + 1,
            name: names && decoded.length > 4 ? names[nameIndex] : undefined,
          });
        }
      }
      mappingTable.push(lineMappings);
    }
    return mappingTable;
  }

  if (!sourcemap || !sourcemap.mappings || !sourcemap.sources) {
    throw new Error("Invalid sourcemap format");
  }

  const mappingTable = parseMappings(
    sourcemap.mappings,
    sourcemap.sources,
    sourcemap.names,
  );

  function mapLocation(line, column) {
    const lineIdx = Number(line) - 1;
    if (lineIdx < 0 || lineIdx >= mappingTable.length) return null;
    const segments = mappingTable[lineIdx];
    if (!segments || segments.length === 0) return null;
    // Find the closest segment with generatedColumn <= columnNumber
    let best = segments[0];
    for (let i = 1; i < segments.length; ++i) {
      if (segments[i].generatedColumn <= column) {
        best = segments[i];
      } else {
        break;
      }
    }
    return best;
  }

  return backtrace.map((entry) => {
    const mapped = mapLocation(Number(entry.line), Number(entry.column));
    if (mapped) {
      return {
        ...entry,
        mapped: {
          source: mapped.source,
          line: mapped.originalLine,
          column: mapped.originalColumn,
          functionName: mapped.name,
        },
      };
    } else {
      return {
        ...entry,
        mapped: null,
      };
    }
  });
}
