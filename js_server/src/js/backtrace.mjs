export function parseBacktrace(backtrace) {
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
      const location = match[2] ?? "unknown location";
      const lineNumber = match[3] ?? null;
      const colNumber = match[3] ?? null;
      trace.push({ functionName, location, lineNumber, colNumber });
      foundMatch = true;
    } else if (!foundMatch) {
      errorMessage += line;
      foundMatch = true;
    }
  }

  return JSON.stringify({
    errorMessage: errorMessage.trim(),
    trace,
    raw: backtrace.trim(),
  });
}
