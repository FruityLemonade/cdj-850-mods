import stock from "./stock.mjs";

const get = (id) => document.getElementById(id);
const picker = get("firmware");
const pick = get("pick");
const dropzone = get("dropzone");
const buildOptions = get("builds");
const buildDetails = get("build-details");
const alwaysOnList = get("always-on-list");
const toggleableList = get("toggleable-list");
const version = get("version");
const build = get("build");
const download = get("download");
const patchCount = get("patch-count");

let input = null;
let checking = false;
let busy = false;
let fileAttempt = 0;
let downloadUrl = null;
let displayedPatchCount = -1;

const buildsById = new Map(stock.builds.map((item) => [item.id, item]));
const selectedBuildId = () => buildOptions.querySelector("input:checked")?.value ?? stock.builds[0]?.id ?? "stable";
const validVersion = () => /^[0-9]\.[0-9]{2}$/.test(version.value);

for (const [index, option] of stock.builds.entries()) {
  const card = get("build-template").content.cloneNode(true);
  const radio = card.querySelector("input");
  radio.value = option.id;
  radio.checked = index === 0;
  card.querySelector(".build-title strong").textContent = option.title;
  const badge = card.querySelector(".badge");
  badge.textContent = option.badge;
  badge.dataset.kind = option.id;
  card.querySelector(".build-desc").textContent = option.description;
  get("build-options").append(card);
}

function renderFeatureList(list, items) {
  list.replaceChildren();
  for (const label of items) {
    const li = document.createElement("li");
    li.textContent = label;
    list.append(li);
  }
}

function status(message, tone = "") {
  get("status").textContent = message;
  get("status").dataset.tone = tone;
}

async function updatePatchCount(method) {
  if (!stock.counterUrl) return;
  try {
    const response = await fetch(stock.counterUrl, {
      method,
      credentials: "omit",
      referrerPolicy: "no-referrer",
      keepalive: method === "POST",
    });
    if (!response.ok) return;
    const { count } = await response.json();
    if (!Number.isSafeInteger(count) || count <= displayedPatchCount) return;
    displayedPatchCount = count;
    patchCount.textContent = `${count.toLocaleString()} firmware ${count === 1 ? "repack" : "repacks"} completed`;
    patchCount.hidden = false;
  } catch {
    // The counter is independent of the firmware download.
  }
}

function resetDownload() {
  if (downloadUrl) URL.revokeObjectURL(downloadUrl);
  downloadUrl = null;
  download.removeAttribute("href");
  download.hidden = true;
  build.hidden = false;
}

function renderControls() {
  const chosen = buildsById.get(selectedBuildId());
  const invalid = !validVersion();
  get("input-summary").textContent = input ? `Verified v${stock.stockVersion}` : checking ? "Verifying…" : "Awaiting file";
  get("build-summary").textContent = chosen ? chosen.title : "—";
  if (chosen && (chosen.alwaysOn.length || chosen.toggleable.length)) {
    renderFeatureList(alwaysOnList, chosen.alwaysOn);
    renderFeatureList(toggleableList, chosen.toggleable);
    buildDetails.hidden = false;
  } else {
    buildDetails.hidden = true;
  }
  version.setAttribute("aria-invalid", String(invalid));
  build.disabled = busy || !input || invalid;
  pick.disabled = buildOptions.disabled = version.disabled = busy;
  if (!busy && !downloadUrl) {
    if (invalid) status("Enter a version like 1.15.", "error");
    else if (!input) status("Choose an original firmware file.");
    else if (chosen?.id === "stock") status("Ready to repack stock firmware.");
    else status(`Ready to build ${chosen?.title.toLowerCase()}.`);
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
buildOptions.addEventListener("change", updateSelections);
version.addEventListener("input", updateSelections);

build.addEventListener("click", () => {
  const buildId = selectedBuildId();
  if (!input || busy || !validVersion()) return;
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
        void updatePatchCount("POST");
      } else if (data.type === "error") {
        finish(data.message, "error");
      }
    });
    worker.addEventListener("error", () => finish("Could not start the patching engine. Reload and try again.", "error"));
    const copy = input.slice(0);
    worker.postMessage({ input: copy, build: buildId, version: label }, [copy]);
  } catch {
    finish("Could not start the patching engine. Reload and try again.", "error");
  }
});

renderControls();
void updatePatchCount("GET");
