import stock from "./stock.mjs";

const get = (id) => document.getElementById(id);
const picker = get("firmware");
const pick = get("pick");
const dropzone = get("dropzone");
const modOptions = get("mods");
const version = get("version");
const build = get("build");
const download = get("download");

let input = null;
let checking = false;
let busy = false;
let fileAttempt = 0;
let downloadUrl = null;

const selected = () => [...modOptions.querySelectorAll("input:checked")].map((item) => item.value);
const validVersion = () => /^[0-9]\.[0-9]{2}$/.test(version.value);
const hasConflict = (mods) => stock.conflict.every((name) => mods.includes(name));

for (const option of stock.options) {
  const row = get("mod-template").content.cloneNode(true);
  row.querySelector("input").value = option.id;
  row.querySelector("strong").textContent = option.title;
  row.querySelector("strong + span").textContent = option.description;
  get(option.developer ? "developer-mods" : "player-mods").append(row);
}

function status(message, tone = "") {
  get("status").textContent = message;
  get("status").dataset.tone = tone;
}

function resetDownload() {
  if (downloadUrl) URL.revokeObjectURL(downloadUrl);
  downloadUrl = null;
  download.removeAttribute("href");
  download.hidden = true;
  build.hidden = false;
}

function renderControls() {
  const mods = selected();
  const count = mods.length;
  const conflict = hasConflict(mods);
  const invalid = !validVersion();
  get("input-summary").textContent = input ? `Verified v${stock.stockVersion}` : checking ? "Verifying…" : "Awaiting file";
  get("mod-summary").textContent = count;
  const modError = get("mods-error");
  modError.hidden = !conflict;
  modError.textContent = conflict ? "Choose either BPM up to 999 or BPM display probe. Both change the BPM display." : "";
  version.setAttribute("aria-invalid", String(invalid));
  build.disabled = busy || !input || invalid || conflict;
  pick.disabled = modOptions.disabled = version.disabled = busy;
  if (!busy && !downloadUrl) {
    if (invalid) status("Enter a version like 1.15.", "error");
    else if (conflict) status("Resolve the BPM mod conflict before repacking.", "error");
    else if (!input) status("Choose an original firmware file.");
    else if (count) status("Ready to apply your selections.");
    else status("Ready to repack stock firmware.");
  }
}

async function chooseFile(file) {
  if (!file || busy) return;
  const attempt = ++fileAttempt;
  const title = get("file-title");
  const detail = get("file-detail");
  const error = get("file-error");
  error.hidden = true;
  dropzone.dataset.ready = "false";
  input = null;
  checking = true;
  resetDownload();
  title.textContent = file.name;
  detail.textContent = "Checking the firmware fingerprint…";
  renderControls();

  try {
    if (!file.size || file.size > 0x400000) {
      throw new Error("This file is the wrong size. Choose the original CDJ-850 v1.13 C850MAIN.UPD.");
    }
    const bytes = await file.arrayBuffer();
    const hash = await crypto.subtle.digest("SHA-256", bytes);
    const fingerprint = Array.from(new Uint8Array(hash), (byte) => byte.toString(16).padStart(2, "0")).join("");
    if (attempt !== fileAttempt) return;
    if (fingerprint !== stock.stockHash) {
      throw new Error("This file did not match the original CDJ-850 v1.13 C850MAIN.UPD.");
    }
    input = bytes;
    dropzone.dataset.ready = "true";
    detail.textContent = `Verified v${stock.stockVersion} · ${(file.size / 1048576).toFixed(2)} MiB`;
    pick.textContent = "Change file";
  } catch (failure) {
    if (attempt !== fileAttempt) return;
    title.textContent = "Drop your update file here";
    detail.textContent = "Original v1.13 update · .UPD";
    pick.textContent = "Choose file";
    error.textContent = failure.message;
    error.hidden = false;
  } finally {
    if (attempt === fileAttempt) {
      checking = false;
      renderControls();
    }
  }
}

pick.addEventListener("click", () => picker.click());
picker.addEventListener("change", () => {
  chooseFile(picker.files[0]);
  picker.value = "";
});
dropzone.addEventListener("dragover", (event) => {
  if (!event.dataTransfer.types.includes("Files")) return;
  event.preventDefault();
  event.dataTransfer.dropEffect = busy ? "none" : "copy";
  if (!busy) dropzone.classList.add("dragging");
});
dropzone.addEventListener("dragleave", (event) => {
  if (!dropzone.contains(event.relatedTarget)) dropzone.classList.remove("dragging");
});
dropzone.addEventListener("drop", (event) => {
  event.preventDefault();
  dropzone.classList.remove("dragging");
  chooseFile(event.dataTransfer.files[0]);
});
function updateSelections() {
  if (busy) return;
  resetDownload();
  renderControls();
}
modOptions.addEventListener("change", updateSelections);
version.addEventListener("input", updateSelections);

build.addEventListener("click", () => {
  const mods = selected();
  if (!input || busy || !validVersion() || hasConflict(mods)) return;
  const label = version.value;

  busy = true;
  renderControls();
  status("Starting the patching engine…");
  let worker;
  function finish(message, tone) {
    worker?.terminate();
    busy = false;
    renderControls();
    status(message, tone);
  }

  try {
    worker = new Worker(new URL("./worker.mjs", import.meta.url), { type: "module" });
    worker.addEventListener("message", ({ data }) => {
      if (data.type === "progress") {
        status(data.message);
      } else if (data.type === "done") {
        downloadUrl = URL.createObjectURL(new Blob([data.output], { type: "application/octet-stream" }));
        download.href = downloadUrl;
        build.hidden = true;
        download.hidden = false;
        finish(`Firmware v${label} repacked and verified.`, "success");
        download.focus();
      } else if (data.type === "error") {
        finish(data.message, "error");
      }
    });
    worker.addEventListener("error", () => finish("Could not start the patching engine. Reload and try again.", "error"));
    const copy = input.slice(0);
    worker.postMessage({ input: copy, mods, version: label }, [copy]);
  } catch {
    finish("Could not start the patching engine. Reload and try again.", "error");
  }
});

renderControls();
