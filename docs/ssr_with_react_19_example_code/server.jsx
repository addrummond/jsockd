import React from "react";
import { renderToString } from "react-dom/server.edge";
import { Counter } from "./counter.jsx";

export function renderCounter(props) {
  return renderToString(<Counter {...props} />);
}
