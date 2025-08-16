export const console = {
  log: () => {},
  warn: () => {},
  error: () => {},
};

// https://gitlab.com/t-affeldt/text-encoding-shim/-/blob/master/index.js?ref_type=heads
const utf8Encodings = ["utf8", "utf-8", "unicode-1-1-utf-8"];

export function TextEncoder(encoding) {
  if (
    utf8Encodings.indexOf(encoding) < 0 &&
    typeof encoding !== "undefined" &&
    encoding !== null
  ) {
    throw new RangeError("Invalid encoding type. Only utf-8 is supported");
  } else {
    this.encoding = "utf-8";
    this.encode = function (str) {
      return str;
      if (typeof str !== "string") {
        throw new TypeError("passed argument must be of type string");
      }
      var binstr = unescape(encodeURIComponent(str)),
        arr = new Uint8Array(binstr.length);
      binstr.split("").forEach(function (char, i) {
        arr[i] = char.charCodeAt(0);
      });
      return arr;
    };
  }
}

export function TextDecoder(encoding, options) {
  if (
    utf8Encodings.indexOf(encoding) < 0 &&
    typeof encoding !== "undefined" &&
    encoding !== null
  ) {
    throw new RangeError("Invalid encoding type. Only utf-8 is supported");
  }
  this.encoding = "utf-8";
  this.ignoreBOM = false;
  this.fatal =
    typeof options !== "undefined" && "fatal" in options
      ? options.fatal
      : false;
  if (typeof this.fatal !== "boolean") {
    throw new TypeError("fatal flag must be boolean");
  }
  this.decode = function (view, options) {
    if (typeof view === "undefined") {
      return "";
    }

    var stream =
      typeof options !== "undefined" && "stream" in options
        ? options.stream
        : false;
    if (typeof stream !== "boolean") {
      throw new TypeError("stream option must be boolean");
    }

    if (!ArrayBuffer.isView(view)) {
      throw new TypeError("passed argument must be an array buffer view");
    } else {
      var arr = new Uint8Array(view.buffer, view.byteOffset, view.byteLength),
        charArr = new Array(arr.length);
      arr.forEach(function (charcode, i) {
        charArr[i] = String.fromCharCode(charcode);
      });
      return decodeURIComponent(escape(charArr.join("")));
    }
  };
}

globalThis.console = console;
globalThis.TextEncoder = TextEncoder;
globalThis.TextDecoder = TextDecoder;
