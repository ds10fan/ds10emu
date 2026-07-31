/* Drives the WebAssembly build: pick a page, render it, put the pixels on the
 * canvases. The C side owns the framebuffers; this only copies them out. */
"use strict";

/* The pages under /data/mainbg that are screens rather than fragments. The
 * "_i" variants are the inverted/highlighted versions the game swaps in. */
const BOTTOM_PAGES = [
  "61_SYN_EDIT", "70_PATCH", "71_KAOSS", "81_KEY", "30_MIXER", "52_FX",
  "32_PATTERN", "51_ALL_DRUMS", "53_DRUMEDIT", "54_DRUM_FX", "55_DRUMEDITMAP",
  "60_SYN_SEQ_PIANO", "60_SYN_SEQ_GATE", "60_SYN_SEQ_PAN", "60_SYN_SEQ_VOL",
  "60_SYN_SEQ_KAOSS", "62_DRUM_SEQ_GATE", "63_DRUM_EDIT_PATCH",
  "33_DRUM_SELECTER", "01_MAIN_MENU", "15_OPTION", "31_SONG", "90_MAP",
];
const TOP_PAGES = [
  "90_MAP", "AD0_TITLE", "AD1_KORG", "AD2_PROCYONSTUDIO", "AD3_AQI",
  "AD4_CAVIA", "01_MAIN_MENU",
];

const status = document.getElementById("status");
const canvases = {
  top: document.getElementById("top"),
  bottom: document.getElementById("bottom"),
};
const ctx = {
  top: canvases.top.getContext("2d"),
  bottom: canvases.bottom.getContext("2d"),
};

let api = null;

function fill(select, names, initial) {
  for (const n of names) {
    const o = document.createElement("option");
    o.value = o.textContent = n;
    select.appendChild(o);
  }
  select.value = initial;
}

function blit(which, ptr, w, h) {
  const bytes = new Uint8ClampedArray(Module.HEAPU8.buffer, ptr, w * h * 4);
  ctx[which].putImageData(new ImageData(bytes, w, h), 0, 0);
}

function draw() {
  api.render();
  const w = api.width(), h = api.height();
  blit("top", api.top(), w, h);
  blit("bottom", api.bottom(), w, h);
}

function show() {
  const bottom = document.getElementById("pick-bottom").value;
  const top = document.getElementById("pick-top").value;
  if (api.page(bottom, top) < 0) {
    status.textContent = "error: " + api.error();
    return;
  }
  draw();
  status.textContent = bottom + "  /  " + top;
}

Module.onRuntimeInitialized = () => {
  api = {
    init: Module.cwrap("ds10_web_init", "number", []),
    page: Module.cwrap("ds10_web_page", "number", ["string", "string"]),
    render: Module.cwrap("ds10_web_render", null, []),
    top: Module.cwrap("ds10_web_top", "number", []),
    bottom: Module.cwrap("ds10_web_bottom", "number", []),
    width: Module.cwrap("ds10_web_width", "number", []),
    height: Module.cwrap("ds10_web_height", "number", []),
    error: Module.cwrap("ds10_web_error", "string", []),
  };
  if (api.init() < 0) {
    status.textContent = "error: " + api.error();
    return;
  }
  fill(document.getElementById("pick-bottom"), BOTTOM_PAGES, "61_SYN_EDIT");
  fill(document.getElementById("pick-top"), TOP_PAGES, "90_MAP");
  for (const id of ["pick-bottom", "pick-top"])
    document.getElementById(id).addEventListener("change", show);
  show();
};
