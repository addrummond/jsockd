import React from "react";
import ReactDOM from "react-dom/server.edge";

globalThis.setTimeout = (f, t) => {
  if (t === 0) {
    f();
    return 0;
  }
  throw new Error("Timeout requested " + t);
  return 0;
};

export function MyComponent() {
  return React.createElement("div", {});
}

export function myRenderToString(node) {
  return ReactDOM.renderToReadableStream(node);
}

export function cmd() {
  return myRenderToString(React.createElement(MyComponent, {}));
}
