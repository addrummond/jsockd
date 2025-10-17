import "web-streams-polyfill/polyfill";

export const console = {};

// console.log will be filled in in main.c
console.warn =
  console.error =
  console.log =
  console.info =
  console.debug =
  console.log;

globalThis.console = console;
