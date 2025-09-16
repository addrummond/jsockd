import React from "react";
import ReactDOM from "react-dom/server.edge";

globalThis.setTimeout = (_) => {
  throw new Error("setTimeout not implemented");
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
