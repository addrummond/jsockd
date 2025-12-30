import React from "react";
import { hydrateRoot } from "react-dom/client";
import { Counter } from "./counter.jsx";

function hydrate() {
  const counterElement = document.getElementById("counter-container");
  if (counterElement) {
    hydrateRoot(counterElement, <Counter initialValue={99} />);
    console.log("Counter hydrated");
  } else {
    console.error("Counter container not found");
  }
}

if (document.readyState === "loading")
  document.addEventListener("DOMContentLoaded", hydrate);
else hydrate();
