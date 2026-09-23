const SERVICE_UUID = "7e400001-b5a3-f393-e0a9-e50e24dcca9e";
const COMMAND_UUID = "7e400002-b5a3-f393-e0a9-e50e24dcca9e";
const EVENTS_UUID = "7e400003-b5a3-f393-e0a9-e50e24dcca9e";
const $ = (selector) => document.querySelector(selector);
const decoder = new TextDecoder();
const encoder = new TextEncoder();
const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
let device, commandCharacteristic;
let tracks = [], mappings = new Map(), cardLabels = new Map(), cardShuffles = new Map();
let currentUid = "", currentPlaylist = [];
let uploadBusy = false;
const messageWaiters = [];
const logLines = [];

const activity = (message) => { $("#activity").textContent = message; };
const prettyUid = (uid) => uid.match(/.{1,2}/g)?.join(":") ?? uid;
const humanTitle = (path) => {
  const file = path.split("/").filter(Boolean).at(-1) || path;
  return file.replace(/^\._/, "").replace(/\.[^.]+$/, "").replace(/[-_]+/g, " ")
    .replace(/\b\w/g, (letter) => letter.toUpperCase());
};
const connected = () => Boolean(commandCharacteristic && device?.gatt?.connected);
const isHiddenPath = (path) => path.split(/[\\/]/).some((part) => {
  const lower = part.toLowerCase();
  return lower.startsWith(".") || lower.startsWith("_") ||
    ["meta", "metadata", "__macosx", "sound-effects", "sound_effects", "sound effects", "sfx", "card-tap.mp3"].includes(lower);
});
const log = (direction, message) => {
  const time = new Date().toLocaleTimeString([], { hour12: false });
  logLines.push(`${time} ${direction} ${message}`);
  if (logLines.length > 250) logLines.splice(0, logLines.length - 250);
  const view = $("#serialLog");
  view.textContent = logLines.join("\n");
  view.scrollTop = view.scrollHeight;
};

async function send(command) {
  if (!commandCharacteristic) throw new Error("Player is not connected");
  await commandCharacteristic.writeValueWithResponse(encoder.encode(command));
  log("TX>", command);
  // Keep commands from overtaking multi-notification responses such as TRACKS.
  await sleep(120);
}

function waitForMessage(prefix, timeoutMs = 8000) {
  return new Promise((resolve, reject) => {
    const waiter = { prefix, resolve, reject };
    messageWaiters.push(waiter);
    setTimeout(() => {
      const index = messageWaiters.indexOf(waiter);
      if (index >= 0) messageWaiters.splice(index, 1);
      reject(new Error(`Timed out waiting for ${prefix}`));
    }, timeoutMs);
  });
}

function rejectMessageWaiters(error) {
  while (messageWaiters.length) messageWaiters.shift().reject(error);
}

async function uploadFile(file) {
  if (!file.name.toLowerCase().endsWith(".mp3")) throw new Error(`${file.name} is not an MP3 file.`);
  // Keep the extension when shortening long names. Truncating the whole string
  // used to turn ".mp3" into ".m", which the firmware correctly rejected.
  const safeStem = file.name.slice(0, -4).replace(/[|/\\]/g, "_").slice(0, 92);
  const safeName = `${safeStem}.mp3`;
  const ready = waitForMessage(`UPLOAD_READY|${safeName}`);
  await send(`UPLOAD_BEGIN|${safeName}|${file.size}`);
  await ready;

  const bytes = new Uint8Array(await file.arrayBuffer());
  const chunkSize = 160;
  let chunksSinceSync = 0;
  for (let offset = 0; offset < bytes.length; offset += chunkSize) {
    const chunk = bytes.subarray(offset, Math.min(offset + chunkSize, bytes.length));
    const packet = new Uint8Array(chunk.length + 1);
    packet[0] = 1;
    packet.set(chunk, 1);
    if (typeof commandCharacteristic.writeValueWithoutResponse === "function") {
      await commandCharacteristic.writeValueWithoutResponse(packet);
    } else {
      await commandCharacteristic.writeValueWithResponse(packet);
    }
    const sent = Math.min(offset + chunk.length, bytes.length);
    const percent = Math.round((sent / bytes.length) * 100);
    $("#uploadProgress").value = percent;
    $("#uploadStatus").textContent = `${safeName}: ${percent}%`;
    chunksSinceSync += 1;
    if (chunksSinceSync === 16 || sent === bytes.length) {
      const acknowledged = waitForMessage(`UPLOAD_ACK|${sent}`, 10000);
      await send(`UPLOAD_SYNC|${sent}`);
      await acknowledged;
      chunksSinceSync = 0;
    }
  }

  const done = waitForMessage(`UPLOAD_DONE|${safeName}|`, 15000);
  await send("UPLOAD_END");
  await done;
}

async function uploadSelectedFiles() {
  const files = [...$("#uploadInput").files];
  if (!files.length) throw new Error("Choose one or more MP3 files first.");
  uploadBusy = true;
  $("#uploadProgress").hidden = false;
  updateActions();
  try {
    for (const file of files) await uploadFile(file);
    $("#uploadStatus").textContent = `${files.length} MP3 file(s) uploaded successfully.`;
    $("#uploadInput").value = "";
  } catch (error) {
    $("#uploadStatus").textContent = `Upload failed: ${error.message}`;
    try { await send("UPLOAD_CANCEL"); } catch {}
    throw error;
  } finally {
    uploadBusy = false;
    updateActions();
  }
}

function setConnected(value) {
  $("#statusDot").classList.toggle("connected", value);
  $("#connectionStatus").textContent = value ? "Player connected" : "Not connected";
  $("#connectButton").textContent = value ? "Reconnect" : "Connect player";
  $("#refreshButton").disabled = !value;
  if (!value) commandCharacteristic = undefined;
  updateActions();
}

async function connect() {
  if (!("bluetooth" in navigator)) throw new Error("Use Chrome or Edge on Android or desktop for Web Bluetooth.");
  activity("Choose “Glyph Soundbox” in the Bluetooth picker…");
  device = await navigator.bluetooth.requestDevice({ filters: [{ services: [SERVICE_UUID] }] });
  device.addEventListener("gattserverdisconnected", () => { setConnected(false); activity("Player disconnected."); });
  const server = await device.gatt.connect();
  const service = await server.getPrimaryService(SERVICE_UUID);
  commandCharacteristic = await service.getCharacteristic(COMMAND_UUID);
  const events = await service.getCharacteristic(EVENTS_UUID);
  await events.startNotifications();
  events.addEventListener("characteristicvaluechanged", onEvent);
  setConnected(true);
  $("#deviceStatus").textContent = device.name || "Glyph Soundbox";
  activity("Connected. Loading cards and SD tracks…");
  for (const command of ["HELLO", "STATUS", "VOLUME", "TAP_MODE", "PERF", "TRACKS", "MAPS"]) await send(command);
}

function csvToPlaylist(csv) {
  return csv ? csv.split(",").map(Number).filter(Number.isInteger) : [];
}

function onEvent(event) {
  const message = decoder.decode(event.target.value);
  log("RX<", message);
  activity(message);
  for (let i = messageWaiters.length - 1; i >= 0; --i) {
    if (message.startsWith(messageWaiters[i].prefix)) {
      const [waiter] = messageWaiters.splice(i, 1);
      waiter.resolve(message);
    }
  }
  const [type, ...parts] = message.split("|");
  if (type === "ERROR") rejectMessageWaiters(new Error(`Player error: ${parts.join(" ").replaceAll("_", " ")}`));
  if (type === "TRACKS_BEGIN") { tracks = []; renderLibrary(); }
  else if (type === "TRACK") {
    const path = parts.slice(1).join("|");
    if (!isHiddenPath(path)) tracks[Number(parts[0])] = path;
    // Do not depend on TRACKS_END: notifications can occasionally be dropped.
    renderLibrary();
  }
  else if (type === "TRACKS_END") renderLibrary();
  else if (type === "MAPS_BEGIN") { mappings = new Map(); cardLabels = new Map(); cardShuffles = new Map(); }
  else if (type === "MAP") {
    mappings.set(parts[0], csvToPlaylist(parts[1] || ""));
    cardLabels.set(parts[0], parts[2] || `Card ${cardLabels.size + 1}`);
    cardShuffles.set(parts[0], parts[3] === "1");
    renderCards();
  } else if (type === "MAPS_END") {
    renderCards();
    if (currentUid && mappings.has(currentUid)) selectCard(currentUid, false);
  } else if (type === "CARD") {
    if (!mappings.has(parts[0])) mappings.set(parts[0], []);
    if (!cardLabels.has(parts[0])) cardLabels.set(parts[0], parts[1] || `Card ${cardLabels.size + 1}`);
    if (!cardShuffles.has(parts[0])) cardShuffles.set(parts[0], false);
    renderCards(); selectCard(parts[0], false);
  } else if (type === "SAVED") {
    mappings.set(parts[0], csvToPlaylist(parts[1] || ""));
    cardShuffles.set(parts[0], parts[2] === "1");
    selectCard(parts[0], false);
    activity(`Playlist saved for ${cardLabels.get(parts[0]) || prettyUid(parts[0])}.`);
  } else if (type === "RENAMED") {
    cardLabels.set(parts[0], parts.slice(1).join("|"));
    renderCards();
    if (currentUid === parts[0]) selectCard(parts[0], false);
    activity(`Card renamed to ${cardLabels.get(parts[0])}.`);
  } else if (type === "SHUFFLE") {
    cardShuffles.set(parts[0], parts[1] === "1");
    if (currentUid === parts[0]) $("#shuffleToggle").checked = parts[1] === "1";
    activity(`Shuffle ${parts[1] === "1" ? "enabled" : "disabled"} for ${cardLabels.get(parts[0]) || prettyUid(parts[0])}.`);
  } else if (type === "CLEARED") {
    mappings.set(parts[0], []);
    cardShuffles.set(parts[0], false);
    if (currentUid === parts[0]) { currentPlaylist = []; $("#shuffleToggle").checked = false; }
    renderPlaylist();
  } else if (type === "PLAYING") {
    $("#nowPlaying").textContent = `${cardLabels.get(parts[0]) || "Card"}: ${humanTitle(tracks[Number(parts[2])] || "Track")}`;
  } else if (type === "PLAYING_TRACK") {
    $("#nowPlaying").textContent = `Preview: ${humanTitle(tracks[Number(parts[0])] || "Track")}`;
  } else if (type === "PLAYING_TAP_SOUND") {
    $("#nowPlaying").textContent = "Card tap sound";
  } else if (type === "UPLOAD_DONE") {
    $("#uploadStatus").textContent = `${parts[0]} uploaded (${Number(parts[1]).toLocaleString()} bytes).`;
  } else if (type === "PAUSED") $("#nowPlaying").textContent = "Paused";
  else if (type === "STATUS") {
    $("#deviceStatus").textContent = `${parts[0]} · ${parts[1]} · ${parts[2]} MP3 track(s)`;
    if (parts[3] && parts[3] !== "NO_CARD") currentUid = parts[3];
  } else if (type === "VOLUME") {
    const volume = Math.max(0, Math.min(100, Number(parts[0]) || 0));
    $("#volumeSlider").value = volume;
    $("#volumeValue").value = `${volume}%`;
  } else if (type === "TAP_MODE") {
    $("#tapMode").value = parts[0] === "toggle" ? "toggle" : "presence";
    renderTapModeHelp();
  } else if (type === "PERF") {
    const load = (Number(parts[0]) / 10).toFixed(1);
    $("#performance").textContent = `Audio load: ${load}% · max frame ${parts[1]} µs · ${parts[2]} short writes`;
  } else if (type === "ERROR") activity(`Player error: ${parts.join(" ").replaceAll("_", " ")}`);
  updateActions();
}

function renderCards() {
  const select = $("#cardSelect"), selected = currentUid;
  select.replaceChildren(new Option("Choose a saved card…", ""));
  for (const [uid] of mappings) select.add(new Option(cardLabels.get(uid) || "Unnamed card", uid));
  select.disabled = !connected() || mappings.size === 0;
  if (selected && mappings.has(selected)) select.value = selected;
}

function selectCard(uid, notifyPlayer = true) {
  currentUid = uid;
  currentPlaylist = [...(mappings.get(uid) || [])].filter((index) => tracks[index] != null);
  $("#cardSelect").value = uid;
  $("#cardNameInput").value = cardLabels.get(uid) || "";
  $("#cardUid").textContent = `Card ID · ${prettyUid(uid)}`;
  $("#shuffleToggle").checked = cardShuffles.get(uid) === true;
  renderPlaylist();
  if (notifyPlayer) send(`SELECT|${uid}`).catch((error) => activity(error.message));
}

function renderLibrary() {
  const root = $("#library");
  root.classList.toggle("empty", tracks.length === 0); root.replaceChildren();
  $("#trackCount").textContent = tracks.filter(Boolean).length;
  if (!tracks.length) { root.textContent = "No MP3 files found on the SD card."; return; }
  tracks.forEach((path, index) => {
    if (path == null) return;
    const item = $("#libraryItemTemplate").content.firstElementChild.cloneNode(true);
    item.querySelector(".track-index").textContent = String(index + 1).padStart(2, "0");
    item.querySelector(".track-name").textContent = humanTitle(path); item.title = path;
    item.querySelector(".preview").addEventListener("click", () => send(`PLAY_TRACK|${index}`).catch((error) => activity(error.message)));
    item.querySelector(".add").addEventListener("click", () => {
      if (!currentUid) return activity("Tap or choose a card first.");
      currentPlaylist.push(index); renderPlaylist();
    });
    root.append(item);
  });
}

function renderPlaylist() {
  const root = $("#playlist");
  root.classList.toggle("empty", currentPlaylist.length === 0); root.replaceChildren();
  $("#playlistCount").textContent = currentPlaylist.length;
  if (!currentPlaylist.length) {
    root.textContent = currentUid ? "Use + beside a song to add it." : "Tap or choose a card first.";
    updateActions(); return;
  }
  currentPlaylist.forEach((trackIndex, position) => {
    const item = $("#playlistItemTemplate").content.firstElementChild.cloneNode(true);
    item.querySelector(".track-name").textContent = `${position + 1}. ${humanTitle(tracks[trackIndex] || `Track ${trackIndex + 1}`)}`;
    item.querySelector(".up").disabled = position === 0;
    item.querySelector(".down").disabled = position === currentPlaylist.length - 1;
    item.querySelector(".up").addEventListener("click", () => move(position, -1));
    item.querySelector(".down").addEventListener("click", () => move(position, 1));
    item.querySelector(".remove").addEventListener("click", () => { currentPlaylist.splice(position, 1); renderPlaylist(); });
    root.append(item);
  });
  updateActions();
}

function move(position, offset) {
  const target = position + offset;
  [currentPlaylist[position], currentPlaylist[target]] = [currentPlaylist[target], currentPlaylist[position]];
  renderPlaylist();
}

function renderTapModeHelp() {
  $("#tapModeHelp").textContent = $("#tapMode").value === "toggle"
    ? "Tap once to play, again to pause or resume. Card removal has no effect."
    : "Playback continues only while the card remains on the reader.";
}

function updateActions() {
  const online = connected();
  $("#saveButton").disabled = !online || uploadBusy || !currentUid || !currentPlaylist.length;
  $("#clearButton").disabled = !online || uploadBusy || !currentUid;
  $("#playButton").disabled = !online || uploadBusy || !currentUid || !currentPlaylist.length;
  $("#pauseButton").disabled = !online || uploadBusy;
  $("#tapMode").disabled = !online || uploadBusy;
  $("#volumeSlider").disabled = !online || uploadBusy;
  $("#uploadButton").disabled = !online || uploadBusy || !$("#uploadInput").files.length;
  $("#cardNameInput").disabled = !online || uploadBusy || !currentUid;
  $("#renameButton").disabled = !online || uploadBusy || !currentUid || !$("#cardNameInput").value.trim();
  $("#shuffleToggle").disabled = !online || uploadBusy || !currentUid;
}

$("#connectButton").addEventListener("click", () => connect().catch((error) => activity(error.message)));
$("#refreshButton").addEventListener("click", async () => {
  try { await send("TRACKS"); await send("MAPS"); }
  catch (error) { activity(error.message); }
});
$("#cardSelect").addEventListener("change", (event) => { if (event.target.value) selectCard(event.target.value); });
$("#playButton").addEventListener("click", () => send("PLAY").catch((error) => activity(error.message)));
$("#pauseButton").addEventListener("click", async () => {
  try { await send("PAUSE"); await send("PERF"); }
  catch (error) { activity(error.message); }
});
$("#volumeSlider").addEventListener("input", (event) => { $("#volumeValue").value = `${event.target.value}%`; });
$("#volumeSlider").addEventListener("change", (event) => send(`VOLUME|${event.target.value}`).catch((error) => activity(error.message)));
$("#tapMode").addEventListener("change", (event) => { renderTapModeHelp(); send(`TAP_MODE|${event.target.value}`).catch((error) => activity(error.message)); });
$("#cardNameInput").addEventListener("input", updateActions);
$("#renameButton").addEventListener("click", () => {
  const name = $("#cardNameInput").value.trim().replaceAll("|", " ");
  if (name) send(`RENAME|${currentUid}|${name}`).catch((error) => activity(error.message));
});
$("#shuffleToggle").addEventListener("change", (event) => send(`SHUFFLE|${currentUid}|${event.target.checked ? 1 : 0}`).catch((error) => activity(error.message)));
$("#saveButton").addEventListener("click", () => send(`MAP|${currentUid}|${currentPlaylist.join(",")}|${$("#shuffleToggle").checked ? 1 : 0}`).catch((error) => activity(error.message)));
$("#clearButton").addEventListener("click", () => send(`CLEAR|${currentUid}`).catch((error) => activity(error.message)));
$("#uploadInput").addEventListener("change", () => {
  const count = $("#uploadInput").files.length;
  $("#uploadStatus").textContent = count ? `${count} MP3 file(s) selected.` : "MP3 files are transferred over Bluetooth.";
  updateActions();
});
$("#uploadButton").addEventListener("click", () => uploadSelectedFiles().catch((error) => activity(error.message)));
$("#clearLogButton").addEventListener("click", () => { logLines.length = 0; $("#serialLog").textContent = "Log cleared."; });
if ("serviceWorker" in navigator) navigator.serviceWorker.register("service-worker.js").then((registration) => registration.update());
