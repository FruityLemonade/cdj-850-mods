const runtimeUrl = "https://cdn.jsdelivr.net/pyodide/v0.29.3/full/";

self.addEventListener("message", async ({ data: { input, mods } }) => {
  const progress = (message) => self.postMessage({ type: "progress", message });
  try {
    progress("Loading the patching engine…");
    const [{ loadPyodide }, bundle] = await Promise.all([
      import(`${runtimeUrl}pyodide.mjs`),
      fetch(new URL("./patcher.zip", import.meta.url)),
    ]);
    if (!bundle.ok) throw new Error("Could not load the patching tools.");
    const python = await loadPyodide({ indexURL: runtimeUrl });
    python.unpackArchive(await bundle.arrayBuffer(), "zip", { extractDir: "/patcher" });
    python.FS.writeFile("/input.upd", new Uint8Array(input));
    const patch = python.runPython(`
import sys
from pathlib import Path

sys.path[:0] = ["/patcher", "/patcher/mods", "/patcher/scripts/firmware"]
from patcher import patch_update

def patch_file(selected, progress):
    source = Path("/input.upd").read_bytes()
    Path("/output.upd").write_bytes(patch_update(source, selected.split(","), progress))

patch_file
`);
    try {
      patch(mods.join(","), progress);
    } finally {
      patch.destroy();
    }
    const output = python.FS.readFile("/output.upd");
    self.postMessage({ type: "done", output }, [output.buffer]);
  } catch (error) {
    const match = error.type === "ValueError" && error.message.match(/^ValueError: (.+)$/m);
    const message = match ? match[1] : "The patching engine could not finish. Try again after checking your connection.";
    console.error(error);
    self.postMessage({ type: "error", message });
  }
});
