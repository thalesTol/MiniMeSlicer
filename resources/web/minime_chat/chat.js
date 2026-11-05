// MiniMe Slicer: Claude chat panel.
// MiniMe (src/slic3r/GUI/MiniMeChat.cpp) runs Claude Code and passes its stream-json messages here through
// window.mm.recv(); this page sends commands back with window.wx.postMessage().
(function () {
  "use strict";

  // ------------------------------------------------------------------------------------------------------------
  // icons (stroke, 24 grid)
  const I = {
    plus: '<path d="M12 5v14M5 12h14"/>',
    clock: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
    more: '<circle cx="5" cy="12" r="1.3" fill="currentColor"/><circle cx="12" cy="12" r="1.3" fill="currentColor"/><circle cx="19" cy="12" r="1.3" fill="currentColor"/>',
    close: '<path d="M6 6l12 12M18 6L6 18"/>',
    back: '<path d="M15 6l-6 6 6 6"/>',
    clip: '<path d="M20 11.5l-8.2 8.2a5 5 0 0 1-7.1-7.1l8.5-8.5a3.3 3.3 0 0 1 4.7 4.7l-8.5 8.5a1.7 1.7 0 0 1-2.4-2.4l7.8-7.8"/>',
    send: '<path d="M12 19V5M6 11l6-6 6 6"/>',
    stop: '<rect x="7" y="7" width="10" height="10" rx="2" fill="currentColor" stroke="none"/>',
    check: '<path d="M5 12.5l4.5 4.5L19 7.5"/>',
    x: '<path d="M7 7l10 10M17 7L7 17"/>',
    chev: '<path d="M9 6l6 6-6 6"/>',
    down: '<path d="M6 9l6 6 6-6"/>',
    spark: '<path d="M12 3l1.9 5.1L19 10l-5.1 1.9L12 17l-1.9-5.1L5 10l5.1-1.9z"/>',
    wrench: '<path d="M14.7 6.3a4 4 0 0 0-5.4 5.1L4 16.7 7.3 20l5.3-5.3a4 4 0 0 0 5.1-5.4l-2.5 2.5-2.4-.6-.6-2.4z"/>',
    eye: '<path d="M2 12s3.6-7 10-7 10 7 10 7-3.6 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>',
    layers: '<path d="M12 3l9 5-9 5-9-5 9-5z"/><path d="M3 13l9 5 9-5"/>',
    sliders: '<path d="M4 6h10M18 6h2M4 12h4M12 12h8M4 18h12M20 18h0"/><circle cx="16" cy="6" r="2"/><circle cx="10" cy="12" r="2"/><circle cx="18" cy="18" r="2"/>',
    move: '<path d="M12 3v18M3 12h18M12 3l-3 3M12 3l3 3M12 21l-3-3M12 21l3-3M3 12l3-3M3 12l3 3M21 12l-3-3M21 12l-3 3"/>',
    brush: '<path d="M18 3l3 3-9 9-3-3z"/><path d="M9 12c-3 0-5 2-5 5 0 1.5-1 2.5-2 3 3 1 7 0 8-3"/>',
    cut: '<circle cx="6" cy="6" r="3"/><circle cx="6" cy="18" r="3"/><path d="M8.6 7.6L20 18M8.6 16.4L20 6"/>',
    undo: '<path d="M9 14L4 9l5-5"/><path d="M4 9h11a5 5 0 0 1 0 10h-3"/>',
    file: '<path d="M14 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z"/><path d="M14 3v6h6"/>',
    globe: '<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"/>',
    terminal: '<path d="M4 17l6-5-6-5M12 19h8"/>',
    search: '<circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/>',
    plate: '<rect x="3" y="14" width="18" height="5" rx="1.5"/><path d="M8 14V9a4 4 0 0 1 8 0v5"/>',
  };
  function icon(name, cls) {
    return '<svg class="' + (cls || "") + '" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' + (I[name] || "") + "</svg>";
  }

  const $ = (id) => document.getElementById(id);
  const els = {
    status: $("status"), scroll: $("scroll"), messages: $("messages"), empty: $("empty"), input: $("input"), send: $("btn-send"),
    box: $("box"), attachments: $("attachments"), menu: $("menu"), models: $("models"), account: $("account"),
    history: $("history"), historyBody: $("history-body"), gate: $("gate"), gateBody: $("gate-body"), modelChip: $("model-chip"),
    lightbox: $("lightbox"), lightboxImg: $("lightbox-img"),
  };
  $("btn-new").innerHTML = icon("plus");
  $("btn-history").innerHTML = icon("clock");
  $("btn-menu").innerHTML = icon("more");
  $("btn-close").innerHTML = icon("close");
  $("history-back").innerHTML = icon("back");
  $("btn-attach").innerHTML = icon("clip");
  $("gate-close").innerHTML = icon("close");
  $("gate-close").addEventListener("click", () => post({ cmd: "close" }));

  // ------------------------------------------------------------------------------------------------------------
  // talking to MiniMe
  function post(obj) {
    try {
      if (window.wx && window.wx.postMessage) window.wx.postMessage(JSON.stringify(obj));
      else console.log("to MiniMe", obj);
    } catch (e) { console.error(e); }
  }

  // ------------------------------------------------------------------------------------------------------------
  // state
  const state = {
    busy: false,
    auth: "checking",
    model: "",
    models: null,           // from Claude Code when it starts
    attachments: [],        // {media_type, data, name}
    alwaysAllow: new Set(), // tools the user allowed for this chat
    sessionId: "",
    connected: false,
    pendingText: [],        // streamed text elements waiting for their final message
    curText: null,          // {el, text}
    chips: {},              // tool_use id -> chip
    toolInputs: {},         // tool_use id -> streamed json text
    thinkingEl: null,
    started: false,
    stopped: false,
  };

  const MODELS = [
    { value: "", label: "Default", sub: "Claude Code's choice" },
    { value: "opus", label: "Opus", sub: "Most capable" },
    { value: "sonnet", label: "Sonnet", sub: "Fast and capable" },
    { value: "haiku", label: "Haiku", sub: "Fastest" },
  ];

  // ------------------------------------------------------------------------------------------------------------
  // small helpers
  function esc(s) {
    return String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  }
  function el(tag, cls, html) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (html != null) e.innerHTML = html;
    return e;
  }
  function nearBottom() {
    const s = els.scroll;
    return s.scrollHeight - s.scrollTop - s.clientHeight < 80;
  }
  let stick = true;
  els.scroll.addEventListener("scroll", () => { stick = nearBottom(); });
  function toBottom(force) {
    if (force || stick) els.scroll.scrollTop = els.scroll.scrollHeight;
  }
  function add(node) {
    els.empty.classList.add("hidden");
    els.messages.appendChild(node);
    toBottom();
    return node;
  }

  function setStatus(text, kind) {
    els.status.innerHTML = '<span class="dot ' + (kind || "") + '"></span><span>' + esc(text) + "</span>";
  }
  function refreshStatus() {
    if (state.auth === "no_claude") return setStatus("Claude Code is not installed", "bad");
    if (state.auth === "signed_out") return setStatus("Not signed in", "bad");
    if (state.busy) return setStatus("Working…", "busy");
    if (state.started && !state.connected) return setStatus("MiniMe tools not connected", "bad");
    setStatus(state.started ? "Ready · sees MiniMe" : "Ready", "ok");
  }

  // ------------------------------------------------------------------------------------------------------------
  // markdown (safe: everything is escaped first)
  function inline(s) {
    s = esc(s);
    const codes = [];
    s = s.replace(/`([^`]+)`/g, (m, c) => { codes.push(c); return "\u0000" + (codes.length - 1) + "\u0000"; });
    s = s.replace(/\[([^\]]+)\]\((https?:\/\/[^\s)]+)\)/g, '<a href="$2" data-link="$2">$1</a>');
    s = s.replace(/(^|[\s(])(https?:\/\/[^\s<)]+)/g, '$1<a href="$2" data-link="$2">$2</a>');
    s = s.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
    s = s.replace(/(^|[^*\w])\*([^*\n]+)\*(?!\w)/g, "$1<em>$2</em>");
    s = s.replace(/(^|[^_\w])_([^_\n]+)_(?!\w)/g, "$1<em>$2</em>");
    s = s.replace(/~~([^~]+)~~/g, "<del>$1</del>");
    s = s.replace(/\u0000(\d+)\u0000/g, (m, i) => "<code>" + codes[+i] + "</code>");
    return s;
  }
  function md(src) {
    const lines = String(src).replace(/\r/g, "").split("\n");
    let out = "";
    let i = 0;
    while (i < lines.length) {
      const line = lines[i];
      const fence = line.match(/^\s*```(\S*)/);
      if (fence) {
        const code = [];
        i++;
        while (i < lines.length && !/^\s*```/.test(lines[i])) code.push(lines[i++]);
        i++;
        out += '<div class="codeblock"><button class="copy" data-copy>Copy</button><pre><code>' + esc(code.join("\n")) + "</code></pre></div>";
        continue;
      }
      if (/^\s*$/.test(line)) { i++; continue; }
      const h = line.match(/^(#{1,4})\s+(.*)/);
      if (h) { out += "<h" + h[1].length + ">" + inline(h[2]) + "</h" + h[1].length + ">"; i++; continue; }
      if (/^\s*([-*_])\s*\1\s*\1[\s\1]*$/.test(line)) { out += "<hr>"; i++; continue; }
      if (/^\s*\|.*\|\s*$/.test(line) && i + 1 < lines.length && /^\s*\|?\s*:?-{2,}/.test(lines[i + 1])) {
        const row = (l) => l.trim().replace(/^\||\|$/g, "").split("|").map((c) => c.trim());
        let t = "<table><thead><tr>" + row(line).map((c) => "<th>" + inline(c) + "</th>").join("") + "</tr></thead><tbody>";
        i += 2;
        while (i < lines.length && /^\s*\|.*\|\s*$/.test(lines[i])) t += "<tr>" + row(lines[i++]).map((c) => "<td>" + inline(c) + "</td>").join("") + "</tr>";
        out += t + "</tbody></table>";
        continue;
      }
      if (/^\s*>/.test(line)) {
        const q = [];
        while (i < lines.length && /^\s*>/.test(lines[i])) q.push(lines[i++].replace(/^\s*>\s?/, ""));
        out += "<blockquote>" + md(q.join("\n")) + "</blockquote>";
        continue;
      }
      const li = line.match(/^(\s*)([-*+]|\d+[.)])\s+(.*)/);
      if (li) {
        const ordered = /\d/.test(li[2]);
        let list = ordered ? "<ol>" : "<ul>";
        while (i < lines.length) {
          const m = lines[i].match(/^(\s*)([-*+]|\d+[.)])\s+(.*)/);
          if (m) { list += "<li>" + inline(m[3]); i++; }
          else if (/^\s{2,}\S/.test(lines[i])) { list += "<br>" + inline(lines[i].trim()); i++; continue; }
          else break;
          list += "</li>";
        }
        out += list + (ordered ? "</ol>" : "</ul>");
        continue;
      }
      const para = [];
      while (i < lines.length && !/^\s*$/.test(lines[i]) && !/^\s*```/.test(lines[i]) && !/^#{1,4}\s/.test(lines[i]) &&
             !/^(\s*)([-*+]|\d+[.)])\s+/.test(lines[i]) && !/^\s*>/.test(lines[i]) && !/^\s*\|.*\|\s*$/.test(lines[i])) para.push(lines[i++]);
      if (!para.length) { out += "<p>" + inline(lines[i++]) + "</p>"; continue; }
      out += "<p>" + para.map(inline).join("<br>") + "</p>";
    }
    return out;
  }

  // links and copy buttons inside messages
  document.addEventListener("click", (e) => {
    const a = e.target.closest("a[data-link]");
    if (a) { e.preventDefault(); post({ cmd: "open_link", url: a.getAttribute("data-link") }); return; }
    const c = e.target.closest("[data-copy]");
    if (c) {
      const text = c.dataset.text != null ? c.dataset.text : (c.parentElement.querySelector("code") || c.parentElement).innerText;
      copyText(text);
      const old = c.textContent;
      c.textContent = "Copied";
      setTimeout(() => (c.textContent = old), 1200);
      return;
    }
    const img = e.target.closest("img[data-zoom]");
    if (img) { els.lightboxImg.src = img.src; els.lightbox.classList.remove("hidden"); }
  });
  els.lightbox.addEventListener("click", () => els.lightbox.classList.add("hidden"));
  function copyText(text) {
    if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).catch(() => fallbackCopy(text));
    else fallbackCopy(text);
  }
  function fallbackCopy(text) {
    const t = document.createElement("textarea");
    t.value = text; document.body.appendChild(t); t.select();
    try { document.execCommand("copy"); } catch (e) {}
    t.remove();
  }

  // ------------------------------------------------------------------------------------------------------------
  // tools: friendly words for what Claude does
  const TOOLS = {
    get_scene: ["eye", "Looking at the plate", "Looked at the plate"],
    render_view: ["eye", "Looking at the view", "Looked at the view"],
    slice: ["layers", "Slicing", "Sliced"],
    support_report: ["layers", "Reading the supports", "Read the supports"],
    find_settings: ["search", "Finding settings", "Found settings"],
    get_settings: ["sliders", "Reading settings", "Read settings"],
    set_settings: ["sliders", "Changing settings", "Changed settings"],
    transform_object: ["move", "Moving an object", "Moved an object"],
    paint_supports: ["brush", "Painting supports", "Painted supports"],
    support_zone: ["plate", "Editing a support zone", "Edited a support zone"],
    cut_object: ["cut", "Cutting an object", "Cut an object"],
    add_model: ["file", "Adding a model", "Added a model"],
    select_plate: ["plate", "Switching plate", "Switched plate"],
    undo: ["undo", "Undoing", "Undid"],
    Read: ["file", "Reading a file", "Read a file"],
    Glob: ["search", "Looking for files", "Looked for files"],
    Grep: ["search", "Searching files", "Searched files"],
    WebSearch: ["globe", "Searching the web", "Searched the web"],
    WebFetch: ["globe", "Opening a web page", "Opened a web page"],
    Bash: ["terminal", "Running a command", "Ran a command"],
    PowerShell: ["terminal", "Running a command", "Ran a command"],
    Write: ["file", "Writing a file", "Wrote a file"],
    Edit: ["file", "Editing a file", "Edited a file"],
    TodoWrite: ["check", "Planning", "Planned"],
    Task: ["spark", "Working on a sub-task", "Worked on a sub-task"],
    Agent: ["spark", "Working on a sub-task", "Worked on a sub-task"],
  };
  const HIDDEN_TOOLS = new Set(["ToolSearch"]);
  function toolKey(name) { return String(name || "").replace(/^mcp__minime__/, ""); }
  function toolWords(name) {
    const k = toolKey(name);
    return TOOLS[k] || ["wrench", "Using " + k.replace(/^mcp__[^_]+__/, ""), "Used " + k.replace(/^mcp__[^_]+__/, "")];
  }
  function objName(v) { return v == null ? "" : typeof v === "number" ? "object " + (v + 1) : String(v); }
  // a few words about the arguments
  function toolDetail(name, input) {
    if (!input || typeof input !== "object") return "";
    const k = toolKey(name);
    try {
      if (k === "set_settings") return Object.keys(input.values || {}).slice(0, 3).join(", ") + (Object.keys(input.values || {}).length > 3 ? "…" : "");
      if (k === "get_settings") return (input.keys || []).slice(0, 3).join(", ");
      if (k === "find_settings") return "“" + (input.query || "") + "”";
      if (k === "render_view") return [input.screen === "preview" ? "preview" : input.screen === "3d" ? "3D" : "", input.view && input.view !== "keep" ? input.view : ""].filter(Boolean).join(", ");
      if (k === "paint_supports") return [input.paint, objName(input.object)].filter(Boolean).join(" on ");
      if (k === "transform_object" || k === "cut_object") return objName(input.object);
      if (k === "support_zone") return [input.action || "add", (input.zone || "").replace(/_/g, " ")].join(" ");
      if (k === "undo") return (input.redo ? "redo " : "") + (input.steps || 1) + " step" + ((input.steps || 1) > 1 ? "s" : "");
      if (k === "Read" || k === "Write" || k === "Edit") return String(input.file_path || "").split(/[\\/]/).pop();
      if (k === "Bash" || k === "PowerShell") return String(input.command || "").slice(0, 60);
      if (k === "WebSearch") return "“" + (input.query || "") + "”";
      if (k === "WebFetch") return String(input.url || "").replace(/^https?:\/\//, "").slice(0, 50);
      if (k === "Grep" || k === "Glob") return String(input.pattern || "");
    } catch (e) {}
    return "";
  }
  // the useful numbers of a result
  function toolSummary(name, text) {
    const k = toolKey(name);
    let j = null;
    try { j = JSON.parse(text); } catch (e) { return ""; }
    if (!j || typeof j !== "object") return "";
    if (k === "slice") {
      const parts = [];
      if (j.print_time) parts.push(j.print_time);
      const g = j.filament_g != null ? j.filament_g : j.filament && j.filament.g;
      if (g != null) parts.push(Math.round(g * 10) / 10 + " g");
      if (j.support_print_time) parts.push("supports " + j.support_print_time);
      return parts.join(" · ");
    }
    if (k === "get_scene" && Array.isArray(j.objects)) return j.objects.length + " object" + (j.objects.length === 1 ? "" : "s");
    return "";
  }

  function resultText(content) {
    if (typeof content === "string") return content;
    if (!Array.isArray(content)) return "";
    return content.filter((b) => b && b.type === "text").map((b) => b.text).join("\n");
  }
  function resultImages(content) {
    if (!Array.isArray(content)) return [];
    return content.filter((b) => b && b.type === "image" && b.source && b.source.data).map((b) => "data:" + (b.source.media_type || "image/png") + ";base64," + b.source.data);
  }

  function makeChip(id, name) {
    if (id && state.chips[id]) return state.chips[id];
    const words = toolWords(name);
    const wrap = el("div", "tool running");
    const head = el("button", "tool-head");
    head.innerHTML = '<span class="ti"><span class="spinner"></span></span><span class="label"></span>' + icon("chev", "chev");
    const body = el("div", "tool-body");
    const pics = el("div", "tool-pics");
    wrap.appendChild(head); wrap.appendChild(body); wrap.appendChild(pics);
    head.addEventListener("click", () => wrap.classList.toggle("open"));
    const chip = { id, name, wrap, head, body, pics, input: null, done: false, words };
    chip.render = function () {
      const detail = toolDetail(name, chip.input);
      const verb = chip.done ? (chip.failed ? "Could not: " + words[2].toLowerCase() : words[2]) : words[1] + "…";
      const extra = chip.summary ? " · " + chip.summary : detail ? " · " + detail : "";
      head.querySelector(".label").innerHTML = "<b>" + esc(verb) + "</b>" + esc(extra);
      head.title = verb + (detail ? " (" + detail + ")" : "");
      let html = "";
      if (chip.input && Object.keys(chip.input).length) html += "<pre>" + esc(JSON.stringify(chip.input, null, 2)) + "</pre>";
      if (chip.result) html += "<pre>" + esc(chip.result.length > 6000 ? chip.result.slice(0, 6000) + " …" : chip.result) + "</pre>";
      body.innerHTML = html;
    };
    chip.finish = function (content, isError) {
      chip.done = true;
      chip.failed = !!isError;
      chip.result = resultText(content);
      chip.summary = isError ? "" : toolSummary(name, chip.result);
      wrap.classList.remove("running");
      wrap.classList.add(isError ? "failed" : "done");
      head.querySelector(".ti").innerHTML = icon(isError ? "x" : words[0]);
      resultImages(content).forEach((src) => {
        const img = el("img");
        img.src = src; img.alt = "MiniMe view"; img.setAttribute("data-zoom", "1");
        pics.appendChild(img);
      });
      chip.render();
      toBottom();
    };
    chip.render();
    if (id) state.chips[id] = chip;
    // Claude Code's own housekeeping (loading its tool list) is not worth a line
    if (!HIDDEN_TOOLS.has(toolKey(name))) add(wrap);
    return chip;
  }

  // ------------------------------------------------------------------------------------------------------------
  // messages
  function userMessage(text, images) {
    const wrap = el("div", "msg-user");
    if (images && images.length) {
      const pics = el("div", "pics");
      images.forEach((src) => { const i = el("img"); i.src = src; i.alt = "attached picture"; i.setAttribute("data-zoom", "1"); pics.appendChild(i); });
      wrap.appendChild(pics);
    }
    if (text) { const b = el("div", "bubble"); b.textContent = text; wrap.appendChild(b); }
    add(wrap);
    toBottom(true);
  }
  function textElement() {
    const e = el("div", "msg-text");
    add(e);
    return e;
  }
  function renderText(e, text, live) {
    e.innerHTML = md(text) + (live ? "" : '<button class="copy" data-copy>Copy</button>');
    const copy = e.querySelector(":scope > .copy");
    if (copy) copy.dataset.text = text;
    e.classList.toggle("caret", !!live);
    toBottom();
  }
  function notice(text, kind, buttonLabel, onClick) {
    const n = el("div", "notice " + (kind || ""));
    n.textContent = text;
    if (buttonLabel) {
      const b = el("button", "btn", esc(buttonLabel));
      b.addEventListener("click", onClick);
      n.appendChild(el("br"));
      n.appendChild(b);
    }
    return add(n);
  }
  function showThinking(on) {
    if (on && !state.thinkingEl) {
      state.thinkingEl = el("div", "thinking", '<span class="dots"><span></span><span></span><span></span></span><span>Thinking</span>');
      add(state.thinkingEl);
    } else if (!on && state.thinkingEl) {
      state.thinkingEl.remove();
      state.thinkingEl = null;
    }
  }
  // keeps the "thinking" dots at the end while Claude works
  function bumpThinking() {
    if (!state.busy) return showThinking(false);
    if (state.curText) return showThinking(false);
    if (state.thinkingEl) els.messages.appendChild(state.thinkingEl);
    else showThinking(true);
    toBottom();
  }

  function setBusy(b) {
    state.busy = b;
    els.send.classList.toggle("stop", b);
    els.send.innerHTML = icon(b ? "stop" : "send");
    els.send.title = b ? "Stop (Esc)" : "Send (Enter)";
    els.send.setAttribute("aria-label", b ? "Stop" : "Send");
    if (!b) { showThinking(false); finishStreaming(); }
    updateSendEnabled();
    refreshStatus();
  }
  function updateSendEnabled() {
    els.send.disabled = !state.busy && !els.input.value.trim() && !state.attachments.length;
  }
  function finishStreaming() {
    if (state.curText) { renderText(state.curText.el, state.curText.text, false); state.curText = null; }
    state.pendingText.forEach((p) => { if (p.el.classList.contains("caret")) renderText(p.el, p.text, false); });
  }

  let rafPending = false;
  function scheduleLive() {
    if (rafPending) return;
    rafPending = true;
    requestAnimationFrame(() => {
      rafPending = false;
      if (state.curText) renderText(state.curText.el, state.curText.text, true);
    });
  }

  // one Claude Code message
  function onClaude(m) {
    if (!m || typeof m !== "object") return;
    if (m.parent_tool_use_id) return; // inside a sub-task
    switch (m.type) {
      case "system":
        if (m.subtype === "init") {
          state.started = true;
          state.sessionId = m.session_id || state.sessionId;
          const mm = (m.mcp_servers || []).find((s) => s.name === "minime");
          state.connected = !!(mm && mm.status === "connected");
          if (!state.connected) notice("Claude cannot reach MiniMe's tools right now" + (mm ? " (" + mm.status + ")" : "") + ". It can still talk, but cannot see the plate.", "warn");
          refreshStatus();
        } else if (m.subtype === "api_retry") {
          setStatus("Connection hiccup, retrying…", "busy");
        } else if (m.subtype === "compact_boundary") {
          add(el("div", "divider", "Earlier messages summarised"));
        }
        break;
      case "stream_event": {
        const ev = m.event || {};
        if (ev.type === "content_block_start") {
          const b = ev.content_block || {};
          if (b.type === "text") {
            showThinking(false);
            state.curText = { el: textElement(), text: b.text || "" };
            state.pendingText.push(state.curText);
            scheduleLive();
          } else if (b.type === "tool_use") {
            showThinking(false);
            makeChip(b.id, b.name);
            state.toolInputs[b.id] = "";
            state.lastToolId = b.id;
          } else if (b.type === "thinking") {
            bumpThinking();
          }
        } else if (ev.type === "content_block_delta") {
          const d = ev.delta || {};
          if (d.type === "text_delta" && state.curText) { state.curText.text += d.text; scheduleLive(); }
          else if (d.type === "input_json_delta" && state.lastToolId != null) state.toolInputs[state.lastToolId] += d.partial_json || "";
        } else if (ev.type === "content_block_stop") {
          if (state.curText) { renderText(state.curText.el, state.curText.text, false); state.curText = null; }
          if (state.lastToolId && state.toolInputs[state.lastToolId]) {
            const chip = state.chips[state.lastToolId];
            try { if (chip && !chip.input) { chip.input = JSON.parse(state.toolInputs[state.lastToolId]); chip.render(); } } catch (e) {}
          }
          bumpThinking();
        } else if (ev.type === "message_start") {
          bumpThinking();
        }
        break;
      }
      case "assistant":
        renderAssistant(m.message, true);
        bumpThinking();
        break;
      case "user":
        renderUserResults(m.message);
        bumpThinking();
        break;
      case "result":
        state.sessionId = m.session_id || state.sessionId;
        setBusy(false);
        if (m.is_error || (m.subtype && m.subtype !== "success")) {
          const text = String(m.result || (m.errors && m.errors.join("\n")) || m.subtype || "Something went wrong.");
          const shown = state.lastText && state.lastText.trim() === text.trim();
          if (/auth|log ?in|sign ?in|oauth|credential|401/i.test(text)) {
            if (!shown) notice(text, "error");
            post({ cmd: "check_sign_in" });
          } else if (!state.stopped && !shown) {
            notice(text, "error");
          }
        }
        state.stopped = false;
        break;
      case "control_request":
        onControlRequest(m);
        break;
      case "control_response": {
        const r = m.response || {};
        if (r.request_id === "minime-init" && r.subtype === "success" && r.response && Array.isArray(r.response.models) && r.response.models.length) {
          state.models = r.response.models.map((x) => ({ value: !x.value || x.value === "default" ? "" : x.value, label: x.displayName || x.value || "Default", sub: x.description || "" }));
          renderModels();
        }
        break;
      }
    }
  }

  // an assistant message: live (after streaming) or from an earlier chat
  function renderAssistant(message, live) {
    if (!message || !Array.isArray(message.content)) return;
    message.content.forEach((b) => {
      if (b.type === "text") {
        if (!b.text || !b.text.trim()) return;
        const p = live ? state.pendingText.shift() : null;
        if (p) { if (state.curText === p) state.curText = null; renderText(p.el, b.text, false); }
        else renderText(textElement(), b.text, false);
        state.lastText = b.text;
      } else if (b.type === "tool_use") {
        const chip = makeChip(b.id, b.name);
        chip.input = b.input || chip.input;
        chip.render();
      }
    });
  }
  function renderUserResults(message) {
    if (!message) return;
    const content = message.content;
    if (!Array.isArray(content)) return;
    content.forEach((b) => {
      if (b.type === "tool_result") {
        const chip = state.chips[b.tool_use_id];
        if (chip && !chip.done) chip.finish(b.content, b.is_error);
      }
    });
  }

  // ------------------------------------------------------------------------------------------------------------
  // permission questions from Claude Code
  function onControlRequest(m) {
    const req = m.request || {};
    if (req.subtype !== "can_use_tool") {
      post({ cmd: "control_error", request_id: m.request_id });
      return;
    }
    const name = req.tool_name || "a tool";
    const input = req.input || {};
    const answer = (allow, always) => {
      if (always) state.alwaysAllow.add(name);
      post({ cmd: "permission", request_id: m.request_id, allow, input });
    };
    if (state.alwaysAllow.has(name)) return answer(true);
    showThinking(false);
    const words = toolWords(name);
    const card = el("div", "ask");
    const detail = name === "Bash" || name === "PowerShell" ? input.command : name === "WebFetch" ? input.url :
                   input.file_path ? input.file_path : JSON.stringify(input, null, 2);
    const ASK = { Bash: "run a command", PowerShell: "run a command", WebFetch: "open a web page", Write: "write a file",
                  Edit: "edit a file", NotebookEdit: "edit a notebook", Read: "read a file", Task: "start a sub-task", Agent: "start a sub-task" };
    const what = ASK[name] || "use " + toolKey(name).replace(/^mcp__([^_]+)__/, "$1: ");
    card.innerHTML = '<div class="ask-title">Claude wants to ' + esc(what) + "</div>" +
                     (req.decision_reason || req.reason ? '<div class="sub">' + esc(req.decision_reason || req.reason) + "</div>" : "") +
                     (detail ? "<pre>" + esc(String(detail).slice(0, 4000)) + "</pre>" : "");
    const row = el("div", "ask-buttons");
    const yes = el("button", "btn primary", "Allow");
    const always = el("button", "btn", "Allow for this chat");
    const no = el("button", "btn", "Don't allow");
    const done = (text) => { card.classList.add("answered"); row.innerHTML = '<span class="sub">' + esc(text) + "</span>"; bumpThinking(); };
    yes.onclick = () => { answer(true); done("Allowed"); };
    always.onclick = () => { answer(true, true); done("Allowed for this chat"); };
    no.onclick = () => { answer(false); done("Not allowed"); };
    row.append(yes, always, no);
    card.appendChild(row);
    add(card);
    yes.focus();
  }

  // ------------------------------------------------------------------------------------------------------------
  // sending
  function send(textOverride) {
    if (state.busy) return;
    const text = (textOverride != null ? textOverride : els.input.value).trim();
    if (!text && !state.attachments.length) return;
    if (state.auth === "signed_out" || state.auth === "no_claude") { showGate(); return; }
    const content = [];
    state.attachments.forEach((a) => content.push({ type: "image", source: { type: "base64", media_type: a.media_type, data: a.data } }));
    if (text) content.push({ type: "text", text });
    userMessage(text, state.attachments.map((a) => "data:" + a.media_type + ";base64," + a.data));
    state.attachments = [];
    renderAttachments();
    els.input.value = "";
    autosize();
    state.pendingText = [];
    state.lastText = "";
    setBusy(true);
    showThinking(true);
    post({ cmd: "send", content });
  }
  function stop() {
    if (!state.busy) return;
    state.stopped = true;
    post({ cmd: "interrupt" });
    add(el("div", "divider", "Stopped"));
  }
  els.send.addEventListener("click", () => (state.busy ? stop() : send()));
  els.input.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && !e.shiftKey && !e.isComposing) { e.preventDefault(); send(); }
  });
  function autosize() {
    els.input.style.height = "auto";
    els.input.style.height = Math.min(180, els.input.scrollHeight) + "px";
    updateSendEnabled();
  }
  els.input.addEventListener("input", autosize);
  els.input.addEventListener("focus", () => els.box.classList.add("focus"));
  els.input.addEventListener("blur", () => els.box.classList.remove("focus"));
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") {
      if (!els.lightbox.classList.contains("hidden")) return els.lightbox.classList.add("hidden");
      if (!els.menu.classList.contains("hidden")) return closeMenu();
      if (!els.history.classList.contains("hidden")) return els.history.classList.add("hidden");
      if (state.busy) return stop();
    }
    if ((e.ctrlKey || e.metaKey) && e.shiftKey && (e.key === "j" || e.key === "J")) { e.preventDefault(); post({ cmd: "close" }); }
  });

  // ------------------------------------------------------------------------------------------------------------
  // pictures: attach button, paste, drop
  function addAttachment(a) {
    if (state.attachments.length >= 6) return;
    state.attachments.push(a);
    renderAttachments();
  }
  function renderAttachments() {
    els.attachments.innerHTML = "";
    state.attachments.forEach((a, i) => {
      const d = el("div", "att");
      const img = el("img"); img.src = "data:" + a.media_type + ";base64," + a.data; img.alt = a.name || "picture";
      const x = el("button", null, "×"); x.title = "Remove"; x.setAttribute("aria-label", "Remove picture");
      x.onclick = () => { state.attachments.splice(i, 1); renderAttachments(); };
      d.append(img, x);
      els.attachments.appendChild(d);
    });
    updateSendEnabled();
  }
  function readImageFile(file) {
    if (!file || !/^image\/(png|jpeg|gif|webp)$/.test(file.type)) return;
    if (file.size > 5 * 1024 * 1024) { notice("Pictures can be up to 5 MB.", "warn"); return; }
    const r = new FileReader();
    r.onload = () => {
      const s = String(r.result);
      addAttachment({ media_type: file.type, data: s.slice(s.indexOf(",") + 1), name: file.name });
    };
    r.readAsDataURL(file);
  }
  $("btn-attach").addEventListener("click", () => post({ cmd: "attach_image" }));
  els.input.addEventListener("paste", (e) => {
    const items = (e.clipboardData && e.clipboardData.items) || [];
    for (const it of items) if (it.kind === "file" && it.type.startsWith("image/")) { e.preventDefault(); readImageFile(it.getAsFile()); }
  });
  ["dragenter", "dragover"].forEach((t) => els.box.addEventListener(t, (e) => { e.preventDefault(); els.box.classList.add("drop"); }));
  ["dragleave", "drop"].forEach((t) => els.box.addEventListener(t, () => els.box.classList.remove("drop")));
  els.box.addEventListener("drop", (e) => {
    e.preventDefault();
    for (const f of (e.dataTransfer && e.dataTransfer.files) || []) readImageFile(f);
  });

  // ------------------------------------------------------------------------------------------------------------
  // header: new chat, earlier chats, options, close
  function clearChat() {
    els.messages.innerHTML = "";
    els.empty.classList.remove("hidden");
    state.chips = {}; state.toolInputs = {}; state.pendingText = []; state.curText = null; state.thinkingEl = null;
    state.alwaysAllow = new Set(); state.started = false; state.connected = false; state.sessionId = "";
    setBusy(false);
  }
  $("btn-new").addEventListener("click", () => {
    if (state.busy) post({ cmd: "interrupt" });
    post({ cmd: "new_chat" });
    clearChat();
    els.input.focus();
  });
  $("btn-close").addEventListener("click", () => post({ cmd: "close" }));
  $("btn-history").addEventListener("click", () => {
    els.historyBody.innerHTML = '<div class="sheet-note">Loading…</div>';
    els.history.classList.remove("hidden");
    post({ cmd: "list_chats" });
  });
  $("history-back").addEventListener("click", () => els.history.classList.add("hidden"));

  function when(t) {
    const d = new Date(t * 1000), now = new Date();
    const days = Math.floor((now - d) / 86400000);
    const time = d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
    if (d.toDateString() === now.toDateString()) return "Today " + time;
    if (days < 2) return "Yesterday " + time;
    if (days < 7) return d.toLocaleDateString([], { weekday: "long" }) + " " + time;
    return d.toLocaleDateString([], { day: "numeric", month: "short", year: d.getFullYear() === now.getFullYear() ? undefined : "numeric" });
  }
  function folderName(cwd) { return String(cwd || "").split(/[\\/]/).filter(Boolean).pop() || "another folder"; }
  function showChats(list) {
    els.historyBody.innerHTML = "";
    const mine = list.filter((c) => c.mine), others = list.filter((c) => !c.mine);
    const group = (title, items, note) => {
      if (!items.length) return;
      els.historyBody.appendChild(el("div", "group-title", esc(title)));
      if (note) els.historyBody.appendChild(el("div", "sheet-note", esc(note)));
      items.forEach((c) => {
        const b = el("button", "chat-item");
        b.innerHTML = '<span class="t">' + esc(c.title || "Untitled chat") + '</span><span class="m">' + esc(when(c.when)) + (c.mine ? "" : " · " + esc(folderName(c.cwd))) + "</span>";
        b.title = c.title || "";
        b.addEventListener("click", () => {
          els.history.classList.add("hidden");
          post({ cmd: "open_chat", path: c.path, fork: !c.mine });
        });
        els.historyBody.appendChild(b);
      });
    };
    group("MiniMe chats", mine);
    group("Other Claude Code chats", others, "From the Claude app and Claude Code. You continue a copy here, so the original stays as it was.");
    if (!list.length) els.historyBody.innerHTML = '<div class="sheet-note">No earlier chats yet.</div>';
  }
  function showTranscript(t) {
    clearChat();
    add(el("div", "divider", esc(t.fork ? "Continuing a copy of “" + (t.title || "a chat") + "”" : t.title || "Earlier chat")));
    (t.entries || []).forEach((e) => {
      if (e.role === "assistant") renderAssistant({ content: e.content }, false);
      else if (e.role === "user") {
        const c = e.content;
        if (typeof c === "string") { if (!/^\s*</.test(c) && !/^Caveat:/.test(c)) userMessage(c, []); return; }
        if (!Array.isArray(c)) return;
        if (c.some((b) => b.type === "tool_result")) return renderUserResults({ content: c });
        const text = c.filter((b) => b.type === "text").map((b) => b.text).join("\n");
        const pics = c.filter((b) => b.type === "image" && b.source && b.source.data).map((b) => "data:" + b.source.media_type + ";base64," + b.source.data);
        if ((text && !/^\s*</.test(text) && !/^Caveat:/.test(text)) || pics.length) userMessage(/^\s*</.test(text) ? "" : text, pics);
      }
    });
    Object.values(state.chips).forEach((ch) => { if (!ch.done) ch.finish("", false); });
    toBottom(true);
    els.input.focus();
  }

  // options menu
  function currentModels() { return state.models || MODELS; }
  function modelLabel(v) {
    const m = currentModels().find((x) => x.value === v) || MODELS.find((x) => x.value === v);
    return m ? m.label : v || "Default";
  }
  function renderModels() {
    els.models.innerHTML = "";
    currentModels().forEach((m) => {
      const b = el("button", "menu-item");
      b.setAttribute("role", "menuitemradio");
      b.setAttribute("aria-checked", String(m.value === state.model));
      b.innerHTML = '<span class="check">' + (m.value === state.model ? icon("check") : "") + "</span><span>" + esc(m.label) + (m.sub ? '<span class="sub">' + esc(m.sub) + "</span>" : "") + "</span>";
      b.addEventListener("click", () => { state.model = m.value; post({ cmd: "set_model", model: m.value }); renderModels(); closeMenu(); });
      els.models.appendChild(b);
    });
    els.modelChip.innerHTML = esc(modelLabel(state.model)) + icon("down");
  }
  function openMenu() { els.menu.classList.remove("hidden"); $("btn-menu").setAttribute("aria-expanded", "true"); }
  function closeMenu() { els.menu.classList.add("hidden"); $("btn-menu").setAttribute("aria-expanded", "false"); }
  $("btn-menu").addEventListener("click", (e) => { e.stopPropagation(); els.menu.classList.contains("hidden") ? openMenu() : closeMenu(); });
  els.modelChip.addEventListener("click", (e) => { e.stopPropagation(); openMenu(); });
  document.addEventListener("click", (e) => { if (!els.menu.contains(e.target)) closeMenu(); });
  $("menu-signin").addEventListener("click", () => { closeMenu(); post({ cmd: "sign_in" }); showGate("waiting"); });

  // ------------------------------------------------------------------------------------------------------------
  // sign-in gate
  function showGate(mode) {
    const m = mode || state.auth;
    let html = '<img src="minime.png" alt="">';
    if (m === "no_claude") {
      html += "<h2>Claude Code is needed</h2><p>The chat runs Claude Code with your Claude account. It comes with the Claude desktop app, or can be installed on its own.</p>" +
              '<div class="row"><button class="btn primary big" id="g-get">Get Claude</button><button class="btn big" id="g-check">Check again</button></div>';
    } else if (m === "waiting") {
      html += "<h2>Finish signing in</h2><p>A window opened to sign in with your Claude account; it continues in your browser. This screen goes away by itself when you are signed in.</p>" +
              '<div class="row"><button class="btn big" id="g-check">Check now</button><button class="btn big" id="g-again">Open sign-in again</button></div>';
    } else if (m === "checking") {
      html += '<h2>One moment</h2><p>Checking your Claude sign-in…</p>';
    } else {
      html += "<h2>Sign in to Claude</h2><p>MiniMe talks to Claude with your own Claude account (Pro, Max, Team or Enterprise). You sign in once, in your browser.</p>" +
              '<div class="row"><button class="btn primary big" id="g-signin">Sign in</button><button class="btn big" id="g-check">I already did</button></div>' +
              '<div class="small">Claude can look at and change the plate in MiniMe; it cannot print.</div>';
    }
    els.gateBody.innerHTML = html;
    els.gate.classList.remove("hidden");
    const on = (id, f) => { const b = $(id); if (b) b.addEventListener("click", f); };
    on("g-get", () => post({ cmd: "open_link", url: "https://claude.ai/download" }));
    on("g-check", () => post({ cmd: "check_sign_in" }));
    on("g-signin", () => { post({ cmd: "sign_in" }); showGate("waiting"); });
    on("g-again", () => post({ cmd: "sign_in" }));
  }
  function hideGate() { els.gate.classList.add("hidden"); }

  function onAuth(a) {
    if (a.state === "checking") { if (state.auth !== "signed_in") { state.auth = "checking"; } refreshStatus(); return; }
    const waiting = !els.gate.classList.contains("hidden") && els.gateBody.querySelector("#g-again");
    state.auth = a.state;
    if (a.state === "signed_in") {
      hideGate();
      els.account.textContent = a.email ? "Signed in as " + a.email : "Signed in to Claude";
      els.input.focus();
    } else if (a.state === "signed_out") {
      els.account.textContent = "Not signed in";
      if (!waiting) showGate("signed_out");
    } else if (a.state === "no_claude") {
      showGate("no_claude");
    } else {
      hideGate(); // could not tell: let the user try
    }
    refreshStatus();
  }

  // ------------------------------------------------------------------------------------------------------------
  // from MiniMe
  window.mm = {
    recv(msg) {
      try {
        switch (msg.kind) {
          case "config":
            document.body.classList.toggle("dark", !!msg.dark);
            state.model = msg.model || "";
            renderModels();
            break;
          case "auth": onAuth(msg); break;
          case "claude": (msg.lines || []).forEach(onClaude); break;
          case "started": state.started = false; break;
          case "exit":
            if (state.busy) {
              setBusy(false);
              const err = String(msg.stderr || "").trim();
              notice("Claude Code stopped" + (err ? ": " + err.split("\n").slice(-3).join(" ") : "."), "error", "Try again", () => post({ cmd: "check_sign_in" }));
            }
            state.started = false;
            refreshStatus();
            break;
          case "chats": showChats(msg.chats || []); break;
          case "loading_chat": clearChat(); add(el("div", "divider", "Opening the chat…")); break;
          case "transcript": showTranscript(msg); break;
          case "attachment": addAttachment({ media_type: msg.media_type, data: msg.data, name: msg.name }); break;
          case "error": setBusy(false); notice(msg.text, "error"); break;
          case "focus": if (els.gate.classList.contains("hidden")) els.input.focus(); break;
        }
      } catch (e) {
        console.error("MiniMe chat", e);
      }
    },
  };

  // suggestions for an empty chat
  [
    ["eye", "Check this plate for problems before I print"],
    ["brush", "Make the supports easier to remove"],
    ["layers", "Slice it and tell me the time and filament"],
    ["spark", "How can I print this faster without losing quality?"],
  ].forEach(([ic, text]) => {
    const b = el("button", "suggestion", icon(ic) + "<span>" + esc(text) + "</span>");
    b.addEventListener("click", () => send(text));
    $("suggestions").appendChild(b);
  });

  renderModels();
  setBusy(false);
  setStatus("Starting…");
  if (window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches && !(window.wx && window.wx.postMessage)) document.body.classList.add("dark");
  post({ cmd: "ready" });
  els.input.focus();
})();
