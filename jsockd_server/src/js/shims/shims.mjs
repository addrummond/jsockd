import { TextDecoder, TextEncoder } from 'fastestsmallesttextencoderdecoder';
import "web-streams-polyfill/polyfill";

export const console = {};

// console.log will be filled in in main.c
console.warn =
  console.error =
  console.log =
  console.info =
  console.debug =
    console.log;

globalThis.TextEncoder = TextEncoder;
globalThis.TextDecoder = TextDecoder;
globalThis.console = console;
