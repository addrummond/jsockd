import React from "react";
import ReactDOM from "react-dom/server.edge";

globalThis.setTimeout = (f) => {
  return 0;
};

export function MyComponent() {
  return React.createElement("div", {});
}

export function myRenderToString(node) {
  return ReactDOM.renderToString(node);
  return ReactDOM.renderToReadableStream(node);
}

export function cmd() {
  return myRenderToString(React.createElement(MyComponent, {}));
}
