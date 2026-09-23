const SERVICE_UUID = "7e400001-b5a3-f393-e0a9-e50e24dcca9e";
const COMMAND_UUID = "7e400002-b5a3-f393-e0a9-e50e24dcca9e";
const EVENTS_UUID = "7e400003-b5a3-f393-e0a9-e50e24dcca9e";
const $ = (selector) => document.querySelector(selector);
const decoder = new TextDecoder();
const encoder = new TextEncoder();
let device, commandCharacteristic;
let tracks = [], mappings = new Map(), cardLabels = new Map();
let currentUid = "", currentPlaylist = [];

const activity = (message) => { $("#activity").textContent = message; };
const prettyUid = (uid) => uid.match(/.{1,2}/g)?.join(":") ?? uid;
const humanTitle = (path) => {
  const file = path.split("/").filter(Boolean).at(-1) || path;
  return file.replace(/^\._/, "").replace(/\.[^.]+$/, "").replace(/[-_]+/g, " ")
    .replace(/\b\w/g, (letter) => letter.toUpperCase());
};
const connected = () => Boolean(commandCharacteristic && device?.gatt?.connected);

async function send(command) {
  if (!commandCharacteristic) throw new Error("Player is not connected");
  await commandCharacteristic.writeValueWithoutResponse(encoder.encode(command));
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
  for (const command of ["HELLO", "STATUS", "TRACKS", "MAPS"]) await send(command);
}

function csvToPlaylist(csv) {
  return csv ? csv.split(",").map(Number).filter(Number.isInteger) : [];
}

function onEvent(event) {
  const message = decoder.decode(event.target.value);
  activity(message);
  const [type, ...parts] = message.split("|");
  if (type === "TRACKS_BEGIN") tracks = [];
  else if (type === "TRACK") tracks[Number(parts[0])] = parts.slice(1).join("|");
  else if (type === "TRACKS_END") renderLibrary();
  else if (type === "MAPS_BEGIN") { mappings = new Map(); cardLabels = new Map(); }
  else if (type === "MAP") {
    mappings.set(parts[0], csvToPlaylist(parts[1] || ""));
    cardLabels.set(parts[0], parts[2] || `Card ${cardLabels.size + 1}`);
  } else if (type === "MAPS_END") {
    renderCards();
    if (currentUid && mappings.has(currentUid)) selectCard(currentUid, false);
  } else if (type === "CARD") {
    if (!mappings.has(parts[0])) mappings.set(parts[0], []);
    if (!cardLabels.has(parts[0])) cardLabels.set(parts[0], parts[1] || `Card ${cardLabels.size + 1}`);
    renderCards(); selectCard(parts[0], false);
  } else if (type === "SAVED") {
    mappings.set(parts[0], csvToPlaylist(parts[1] || ""));
    selectCard(parts[0], false);
    activity(`Playlist saved for ${cardLabels.get(parts[0]) || prettyUid(parts[0])}.`);
  } else if (type === "CLEARED") {
    mappings.set(parts[0], []);
    if (currentUid === parts[0]) currentPlaylist = [];
    renderPlaylist();
  } else if (type === "PLAYING") {
    $("#nowPlaying").textContent = `${cardLabels.get(parts[0]) || "Card"}: ${humanTitle(tracks[Number(parts[2])] || "Track")}`;
  } else if (type === "PLAYING_TRACK") {
    $("#nowPlaying").textContent = `Preview: ${humanTitle(tracks[Number(parts[0])] || "Track")}`;
  } else if (type === "PAUSED") $("#nowPlaying").textContent = "Paused";
  else if (type === "STATUS") {
    $("#deviceStatus").textContent = `${parts[0]} · ${parts[1]} · ${parts[2]} MP3 track(s)`;
    if (parts[3] && parts[3] !== "NO_CARD") currentUid = parts[3];
  } else if (type === "ERROR") activity(`Player error: ${parts.join(" ").replaceAll("_", " ")}`);
  updateActions();
}

function renderCards() {
  const select = $("#cardSelect"), selected = currentUid;
  select.replaceChildren(new Option("Choose a saved card…", ""));
  for (const [uid] of mappings) select.add(new Option(`${cardLabels.get(uid) || "Card"} — ${prettyUid(uid)}`, uid));
  select.disabled = !connected() || mappings.size === 0;
  if (selected && mappings.has(selected)) select.value = selected;
}

function selectCard(uid, notifyPlayer = true) {
  currentUid = uid;
  currentPlaylist = [...(mappings.get(uid) || [])].filter((index) => tracks[index] != null);
  $("#cardSelect").value = uid;
  $("#cardUid").textContent = `${cardLabels.get(uid) || "Card"} · ${prettyUid(uid)}`;
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

function updateActions() {
  const online = connected();
  $("#saveButton").disabled = !online || !currentUid || !currentPlaylist.length;
  $("#clearButton").disabled = !online || !currentUid;
  $("#playButton").disabled = !online || !currentUid || !currentPlaylist.length;
  $("#pauseButton").disabled = !online;
}

$("#connectButton").addEventListener("click", () => connect().catch((error) => activity(error.message)));
$("#refreshButton").addEventListener("click", async () => {
  try { await send("TRACKS"); await send("MAPS"); }
  catch (error) { activity(error.message); }
});
$("#cardSelect").addEventListener("change", (event) => { if (event.target.value) selectCard(event.target.value); });
$("#playButton").addEventListener("click", () => send("PLAY").catch((error) => activity(error.message)));
$("#pauseButton").addEventListener("click", () => send("PAUSE").catch((error) => activity(error.message)));
$("#saveButton").addEventListener("click", () => send(`MAP|${currentUid}|${currentPlaylist.join(",")}`).catch((error) => activity(error.message)));
$("#clearButton").addEventListener("click", () => send(`CLEAR|${currentUid}`).catch((error) => activity(error.message)));
if ("serviceWorker" in navigator) navigator.serviceWorker.register("service-worker.js").then((registration) => registration.update());
