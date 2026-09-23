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
const logLines = [];

const marqueeObserver = new ResizeObserver((entries) => {
  for (const { target } of entries) updateMarquee(target);
});

function updateMarquee(target) {
  const content = target.querySelector(".marquee-content");
  if (!content) return;
  target.classList.remove("is-overflowing");
  const distance = Math.ceil(content.scrollWidth - target.clientWidth);
  if (distance <= 2) return;
  target.style.setProperty("--marquee-distance", `${-distance}px`);
  target.style.setProperty("--marquee-duration", `${Math.max(5, distance / 24 + 3)}s`);
  target.classList.add("is-overflowing");
}

function scrollingText(target, text) {
  target.classList.add("auto-scroll");
  target.replaceChildren(Object.assign(document.createElement("span"), {
    className: "marquee-content",
    textContent: text,
  }));
  target.title = text;
  marqueeObserver.observe(target);
  requestAnimationFrame(() => updateMarquee(target));
}

const activity = (message) => scrollingText($("#activity"), message);
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
  for (const command of ["HELLO", "STATUS", "VOLUME", "TAP_MODE", "RESUME", "PERF", "TRACKS", "MAPS"]) await send(command);
}

function csvToPlaylist(csv) {
  return csv ? csv.split(",").map(Number).filter(Number.isInteger) : [];
}

function onEvent(event) {
  const message = decoder.decode(event.target.value);
  log("RX<", message);
  activity(message);
  const [type, ...parts] = message.split("|");
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
  } else if (type === "CLEARED_CHECKPOINTS") {
    activity("All playback checkpoints cleared.");
  } else if (type === "CLEARED_MAPPINGS") {
    for (const uid of mappings.keys()) mappings.set(uid, []);
    currentPlaylist = [];
    renderPlaylist();
    activity("All playlist mappings cleared.");
  } else if (type === "CLEARED_USER_DATA") {
    mappings = new Map(); cardLabels = new Map(); cardShuffles = new Map();
    currentUid = ""; currentPlaylist = [];
    renderCards(); renderPlaylist();
    $("#cardNameInput").value = "";
    $("#shuffleToggle").checked = false;
    scrollingText($("#cardUid"), "No card selected");
    scrollingText($("#nowPlaying"), "Nothing playing");
    activity("All user data cleared. SD card music was kept.");
  } else if (type === "PLAYING") {
    scrollingText($("#nowPlaying"), `${cardLabels.get(parts[0]) || "Card"}: ${humanTitle(tracks[Number(parts[2])] || "Track")}`);
  } else if (type === "PLAYING_TRACK") {
    scrollingText($("#nowPlaying"), `Preview: ${humanTitle(tracks[Number(parts[0])] || "Track")}`);
  } else if (type === "PLAYING_TAP_SOUND") {
    scrollingText($("#nowPlaying"), "Card tap sound");
  } else if (type === "PAUSED") scrollingText($("#nowPlaying"), "Paused");
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
  } else if (type === "RESUME") {
    $("#resumeToggle").checked = parts[0] === "1";
    activity(`Resume checkpoints ${parts[0] === "1" ? "enabled" : "disabled"}.`);
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
  scrollingText($("#cardUid"), `Card ID · ${prettyUid(uid)}`);
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
    scrollingText(item.querySelector(".track-name"), humanTitle(path)); item.title = path;
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
    scrollingText(item.querySelector(".track-name"), `${position + 1}. ${humanTitle(tracks[trackIndex] || `Track ${trackIndex + 1}`)}`);
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
  $("#saveButton").disabled = !online || !currentUid || !currentPlaylist.length;
  $("#clearButton").disabled = !online || !currentUid;
  $("#playButton").disabled = !online || !currentUid || !currentPlaylist.length;
  $("#pauseButton").disabled = !online;
  $("#tapMode").disabled = !online;
  $("#resumeToggle").disabled = !online;
  $("#volumeSlider").disabled = !online;
  $("#cardNameInput").disabled = !online || !currentUid;
  $("#renameButton").disabled = !online || !currentUid || !$("#cardNameInput").value.trim();
  $("#shuffleToggle").disabled = !online || !currentUid;
  $("#clearCheckpointsButton").disabled = !online;
  $("#clearMappingsButton").disabled = !online;
  $("#clearUserDataButton").disabled = !online;
}

function confirmAndSend(message, command) {
  if (window.confirm(message)) send(command).catch((error) => activity(error.message));
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
$("#resumeToggle").addEventListener("change", (event) => send(`RESUME|${event.target.checked ? 1 : 0}`).catch((error) => activity(error.message)));
$("#cardNameInput").addEventListener("input", updateActions);
$("#renameButton").addEventListener("click", () => {
  const name = $("#cardNameInput").value.trim().replaceAll("|", " ");
  if (name) send(`RENAME|${currentUid}|${name}`).catch((error) => activity(error.message));
});
$("#shuffleToggle").addEventListener("change", (event) => send(`SHUFFLE|${currentUid}|${event.target.checked ? 1 : 0}`).catch((error) => activity(error.message)));
$("#saveButton").addEventListener("click", () => send(`MAP|${currentUid}|${currentPlaylist.join(",")}|${$("#shuffleToggle").checked ? 1 : 0}`).catch((error) => activity(error.message)));
$("#clearButton").addEventListener("click", () => send(`CLEAR|${currentUid}`).catch((error) => activity(error.message)));
$("#clearCheckpointsButton").addEventListener("click", () => confirmAndSend(
  "Clear every saved playback checkpoint? Playlists and card names will be kept.",
  "CLEAR_CHECKPOINTS"
));
$("#clearMappingsButton").addEventListener("click", () => confirmAndSend(
  "Clear every card-to-playlist mapping? Card names, settings, and SD card music will be kept.",
  "CLEAR_MAPPINGS"
));
$("#clearUserDataButton").addEventListener("click", () => confirmAndSend(
  "Clear ALL user data from the player? This resets cards, mappings, checkpoints, names, and preferences. SD card music will be kept.",
  "CLEAR_USER_DATA"
));
$("#clearLogButton").addEventListener("click", () => { logLines.length = 0; $("#serialLog").textContent = "Log cleared."; });
scrollingText($("#nowPlaying"), "Nothing playing");
scrollingText($("#cardUid"), "No card selected");
activity("Waiting for a connection.");
if ("serviceWorker" in navigator) navigator.serviceWorker.register("service-worker.js").then((registration) => registration.update());
