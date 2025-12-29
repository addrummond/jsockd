import "web-streams-polyfill/polyfill";

// console.log will be filled in in main.c
globalThis.console.warn =
  globalThis.console.error =
  globalThis.console.info =
  globalThis.console.debug =
    globalThis.console.log;
