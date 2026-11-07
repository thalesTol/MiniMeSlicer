// MiniMe Slicer: print price window. MiniMe does the calculation (the same one as the user's pricing artifact) and sends
// the result here with window.mm.recv(); this page sends changes back with window.wx.postMessage().
(function () {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const TIPOS = ["PLA Basic", "PLA Matte", "PLA Silk", "PLA High Flow", "PETG", "PETG HF", "ABS", "ASA", "TPU", "PLA Wood",
    "PLA-CF", "PETG-CF", "PLA Marmorizado", "PLA Glow", "PLA Metálico"];
  let S = null;          // the last state from MiniMe
  let sellPrice = "";
  let editingFil = null;
  let armedDelete = null;
  let armTimer = null;

  // ---------------------------------------------------------------------------------------------------------------
  // helpers
  function post(obj) {
    try {
      if (window.wx && window.wx.postMessage) window.wx.postMessage(JSON.stringify(obj));
      else console.log("to MiniMe", obj);
    } catch (e) { console.error(e); }
  }
  function esc(s) {
    return String(s == null ? "" : s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  }
  function brl(v) {
    if (v == null || isNaN(v)) return "—";
    return Number(v).toLocaleString("pt-BR", { style: "currency", currency: "BRL" });
  }
  function num(v, d) { return Number(v).toLocaleString("pt-BR", { minimumFractionDigits: d, maximumFractionDigits: d }); }
  function parseNum(s) {
    s = String(s == null ? "" : s).trim().replace(/\s/g, "");
    if (s === "") return null;
    if (s.indexOf(",") >= 0) s = s.replace(/\./g, "").replace(",", ".");
    const v = Number(s);
    return isFinite(v) && v >= 0 ? v : NaN;
  }
  function hoursText(h) {
    const total = Math.round(h * 60);
    const hh = Math.floor(total / 60), mm = total % 60;
    return hh > 0 ? hh + "h " + String(mm).padStart(2, "0") + "min" : mm + "min";
  }
  function hoursShort(h) { // for the artifact's time field ("3h6m")
    const total = Math.round(h * 60);
    return Math.floor(total / 60) + "h" + (total % 60) + "m";
  }
  function toast(text, error) {
    const t = $("toast");
    t.textContent = text;
    t.classList.toggle("error", !!error);
    t.classList.add("show");
    clearTimeout(t._timer);
    t._timer = setTimeout(() => t.classList.remove("show"), error ? 4200 : 2200);
  }
  function setValue(el, v) { if (document.activeElement !== el) el.value = v; }
  function debounce(fn, ms) {
    let t = null;
    return function () { const args = arguments; clearTimeout(t); t = setTimeout(() => fn.apply(null, args), ms); };
  }
  function change(c) { post({ cmd: "change", change: c, sell_price: sellPrice }); }
  function setConfig(key, value) { change({ action: "set_config", values: { [key]: value } }); }

  // ---------------------------------------------------------------------------------------------------------------
  // tabs
  document.querySelectorAll(".tab").forEach((b) => b.addEventListener("click", () => {
    document.querySelectorAll(".tab").forEach((x) => { x.classList.toggle("active", x === b); x.setAttribute("aria-selected", x === b); });
    ["calc", "fil", "cfg"].forEach((t) => $("tab-" + t).classList.toggle("hidden", t !== b.dataset.tab));
  }));

  // ---------------------------------------------------------------------------------------------------------------
  // settings fields (data-cfg): saved as they are typed
  document.querySelectorAll("[data-cfg]").forEach((el) => {
    const key = el.dataset.cfg;
    if (el.dataset.text) {
      el.addEventListener("change", () => setConfig(key, el.value.trim()));
      return;
    }
    const send = debounce(() => {
      const v = parseNum(el.value);
      if (v === null) return;
      if (isNaN(v)) { el.classList.add("bad"); return; }
      el.classList.remove("bad");
      setConfig(key, v);
    }, 450);
    el.addEventListener("input", send);
  });
  $("in-fee-type").addEventListener("change", () => setConfig("fee_type", $("in-fee-type").value));
  document.querySelectorAll("#scenario button").forEach((b) => b.addEventListener("click", () => {
    setConfig("scenario", b.dataset.s);
    if (b.dataset.s === "personalizada") setTimeout(() => $("in-custom").focus(), 50);
  }));
  $("in-sell").addEventListener("input", debounce(() => {
    const v = parseNum($("in-sell").value);
    $("in-sell").classList.toggle("bad", isNaN(v));
    if (isNaN(v)) return;
    sellPrice = v === null ? "" : String(v);
    post({ cmd: "refresh", sell_price: sellPrice });
  }, 350));
  $("btn-slice").addEventListener("click", () => { post({ cmd: "slice" }); $("btn-slice").disabled = true; });
  document.querySelectorAll("#scope button").forEach((b) => b.addEventListener("click", () => {
    post({ cmd: "scope", all: b.dataset.all === "1", sell_price: sellPrice });
  }));
  function plateNames(list) {
    return list.length <= 1 ? String(list[0] || "") : list.slice(0, -1).join(", ") + " e " + list[list.length - 1];
  }
  $("btn-artifact").addEventListener("click", () => {
    const url = S && S.config ? S.config.artifact_url : "";
    if (url) post({ cmd: "open_url", url: url });
    else {
      toast("Coloque o link do artefato em Custos e margens.");
      document.querySelector('.tab[data-tab="cfg"]').click();
      $("in-artifact").focus();
    }
  });
  $("btn-copy").addEventListener("click", () => {
    if (!S || !S.result) return;
    post({ cmd: "copy", text: summaryText() });
    toast("Resumo copiado.");
  });

  // ---------------------------------------------------------------------------------------------------------------
  // the calculation tab
  const SOURCE = {
    list: (f) => ["", "do seu cadastro"],
    auto: (f) => ["auto", "automático: " + f.label],
    manual: () => ["", "preço digitado"],
    preset: () => ["", "do perfil do filamento"],
    none: () => ["none", "sem preço"],
  };

  function filamentOptions(f) {
    const list = (S.filaments || []);
    let html = '<option value="">' + (list.length ? "Automático (mesma marca)" : "Nenhum filamento cadastrado") + "</option>";
    list.forEach((x) => { html += '<option value="' + esc(x.id) + '">' + esc(x.label) + " — " + esc(brl(x.price_kg)) + "/kg</option>"; });
    html += '<option value="__manual">Digitar o preço por kg…</option>';
    return html;
  }

  function renderFilaments(usage, result) {
    const host = $("fil-rows");
    const focused = document.activeElement && host.contains(document.activeElement) ? document.activeElement.dataset.key : null;
    if (focused) return; // the user is typing a price here: keep the row as it is
    const byslot = {};
    (result ? result.filaments : usage.filaments).forEach((f) => { byslot[f.slot] = f; });
    host.innerHTML = usage.filaments.map((u) => {
      const f = byslot[u.slot] || u;
      const src = (SOURCE[f.source] || SOURCE.none)(f);
      return '<div class="fil-row">' +
        '<span class="swatch" style="background:' + esc(f.colour || "#888") + '"></span>' +
        '<div><div class="fil-name" title="' + esc(f.preset) + '">Filamento ' + f.slot + " · " + esc(f.preset) + "</div>" +
        '<div class="fil-grams">' + num(f.grams, 1) + " g" + (f.purge_grams > 0.05 ? " (purga " + num(f.purge_grams, 1) + " g)" : "") +
        " · " + esc(brl(f.price_kg)) + "/kg</div></div>" +
        '<div class="fil-cost">' + esc(brl(f.cost)) + "</div>" +
        '<div class="fil-price">' +
        '<select data-key="' + esc(f.key) + '" aria-label="Preço do filamento ' + f.slot + '">' + filamentOptions(f) + "</select>" +
        '<input class="' + (f.source === "manual" ? "" : "hidden") + '" data-key="' + esc(f.key) + '" inputmode="decimal" placeholder="R$/kg" aria-label="Preço por kg do filamento ' + f.slot + '" value="' + (f.source === "manual" ? esc(String(f.price_kg).replace(".", ",")) : "") + '">' +
        '<span class="src ' + src[0] + '">' + esc(src[1]) + "</span></div></div>";
    }).join("");
    host.querySelectorAll("select").forEach((sel, i) => {
      const f = byslot[usage.filaments[i].slot] || usage.filaments[i];
      sel.value = f.source === "list" ? f.filament_id : f.source === "manual" ? "__manual" : "";
      const input = sel.nextElementSibling;
      sel.addEventListener("change", () => {
        if (sel.value === "__manual") { input.classList.remove("hidden"); input.focus(); return; }
        input.classList.add("hidden");
        change(sel.value ? { action: "assign", key: sel.dataset.key, filament: sel.value } : { action: "assign", key: sel.dataset.key });
      });
      input.addEventListener("change", () => {
        const v = parseNum(input.value);
        if (v === null || isNaN(v)) { input.classList.add("bad"); return; }
        input.classList.remove("bad");
        change({ action: "assign", key: sel.dataset.key, price_kg: v });
      });
      input.addEventListener("keydown", (e) => { if (e.key === "Enter") input.blur(); });
    });
  }

  function renderResult(r) {
    const chosen = r.scenarios.find((s) => s.chosen) || r.scenarios[1];
    $("hero-label").textContent = "Preço sugerido · margem " + chosen.label.toLowerCase();
    $("hero-price").textContent = brl(chosen.price);
    $("hero-sub").textContent = "Lucro " + brl(chosen.profit) + " · custo total " + brl(chosen.total_cost) +
      (chosen.price_per_gram ? " · " + brl(chosen.price_per_gram) + "/g" : "");
    $("hero-min").textContent = "Preço mínimo, sem lucro: " + brl(r.break_even_price);

    $("scen-table").querySelector("tbody").innerHTML = r.scenarios.map((s) =>
      '<tr class="' + (s.chosen ? "sel" : "") + '"><td>' + esc(s.label) + (s.chosen ? " ★" : "") + "</td>" +
      '<td class="num">' + brl(s.margin_per_hour) + "/h</td>" +
      '<td class="num">' + brl(s.profit) + "</td>" +
      '<td class="num">' + brl(s.total_cost) + "</td>" +
      '<td class="num"><strong>' + brl(s.price) + "</strong></td></tr>").join("");

    const c = r.costs;
    const rows = [
      ["Filamento (" + num(r.grams, 1) + " g)", brl(c.filament)],
      ["Máquina (" + num(r.hours, 2) + " h × " + brl(r.config.machine_per_hour) + ")", brl(c.machine)],
    ];
    if (c.waste > 0) rows.push(["Perdas e falhas (" + num(r.config.waste_pct, 0) + "%)", brl(c.waste)]);
    rows.push(["Custo de produção", brl(c.production), "total"]);
    rows.push(["Embalagem", brl(c.packaging)]);
    if (c.shipping > 0) rows.push(["Frete", brl(c.shipping)]);
    if (c.fee_value > 0) rows.push(["Taxa do canal", c.fee_type === "pct" ? num(c.fee_value, 1) + "% do preço" : brl(c.fee_value)]);
    rows.push(["Custo antes da taxa", brl(c.base), "total"]);
    $("breakdown").innerHTML = rows.map((x) => '<div class="' + (x[2] || "") + '"><span class="k">' + esc(x[0]) + '</span><span class="v">' + esc(x[1]) + "</span></div>").join("");

    const ev = r.evaluation;
    $("eval").classList.toggle("hidden", !ev);
    if (ev) {
      const v = $("verdict");
      v.className = "verdict " + ev.verdict;
      v.textContent = ev.verdict_text;
      $("ev-fee").textContent = brl(ev.fee);
      $("ev-net").textContent = brl(ev.net_profit);
      $("ev-hour").textContent = brl(ev.profit_per_hour) + "/h";
      $("ev-pct").textContent = num(ev.profit_pct_of_price, 1) + "%";
    }
    const warn = $("price-warn");
    warn.classList.toggle("hidden", !r.warning);
    warn.textContent = r.warning ? "Um filamento está sem preço (R$ 0). Escolha um filamento do seu cadastro ou digite o preço por kg." : "";
  }

  function renderCalc() {
    const u = S.usage;
    document.querySelectorAll("#scope button").forEach((b) => b.setAttribute("aria-checked", (b.dataset.all === "1") === !!S.all));
    $("scope-note").textContent = S.all ? "Um produto feito de várias placas: tempo e filamento somados; embalagem e frete contam uma vez." : "";
    $("not-sliced").classList.toggle("hidden", !!u);
    $("calc-body").classList.toggle("hidden", !u);
    if (!u) {
      const missing = S.unsliced || [];
      if (S.all && S.used_plates === 0) {
        $("not-sliced-title").textContent = "Nenhuma placa tem objetos";
        $("not-sliced-text").textContent = "Coloque objetos nas placas para calcular.";
      } else if (S.all) {
        $("not-sliced-title").textContent = S.slicing ? "Fatiando as placas…" :
          (missing.length === 1 ? "Fatie a placa " + missing[0] : "Fatie as placas " + plateNames(missing)) + " para calcular";
        $("not-sliced-text").textContent = S.slicing ? "O preço aparece quando todas terminarem." :
          "O preço de todas as placas soma o tempo e o filamento de cada uma, e só sai com todas fatiadas.";
      } else {
        $("not-sliced-title").textContent = S.slicing ? "Fatiando a placa…" : "Fatie a placa " + (S.plate || "") + " para calcular";
        $("not-sliced-text").textContent = S.slicing ? "O preço aparece assim que terminar." :
          "O tempo de impressão e o peso de cada filamento vêm da placa fatiada (ela mudou desde a última vez, ou ainda não foi fatiada).";
      }
      $("btn-slice").textContent = S.all ? "Fatiar todas" : "Fatiar placa";
      $("btn-slice").disabled = !!S.slicing || (S.all && S.used_plates === 0);
      return;
    }
    $("btn-slice").disabled = false;
    const plates = u.plates || [];
    $("plate-title").textContent = u.all ? (plates.length === 1 ? "Placa " + plates[0].plate : "Placas " + plateNames(plates.map((x) => x.plate))) : "Placa " + u.plate;
    $("plates-wrap").classList.toggle("hidden", !(u.all && plates.length > 1));
    $("plates-table").querySelector("tbody").innerHTML = plates.map((x) =>
      "<tr><td>Placa " + esc(x.plate) + '</td><td class="num">' + esc(hoursText(x.hours)) + '</td><td class="num">' + esc(num(x.grams, 1)) + " g</td></tr>").join("");
    $("plate-time").textContent = hoursText(u.hours);
    $("plate-project").textContent = u.project || "—";
    $("plate-hours").textContent = hoursText(u.hours) + " (" + num(u.hours, 2) + " h)";
    $("plate-grams").textContent = num(u.grams, 1) + " g";
    renderFilaments(u, S.result);
    if (S.result) renderResult(S.result);
  }

  function renderConfig() {
    const c = S.config;
    document.querySelectorAll("[data-cfg]").forEach((el) => {
      const v = c[el.dataset.cfg];
      if (v === undefined) return;
      if (el.classList.contains("bad") && document.activeElement === el) return;
      setValue(el, el.dataset.text ? v : String(v).replace(".", ","));
    });
    setValue($("in-fee-type"), c.fee_type);
    document.querySelectorAll("#scenario button").forEach((b) => b.setAttribute("aria-checked", b.dataset.s === c.scenario));
    $("custom-field").classList.toggle("hidden", c.scenario !== "personalizada");
  }

  function summaryText() {
    const r = S.result, u = S.usage, c = r.costs;
    const lines = [];
    lines.push("Produto: " + (u.project || "(sem nome)") + (u.all && u.plates && u.plates.length > 1 ? " (" + u.plates.length + " placas)" : ""));
    lines.push("Tempo de impressão: " + hoursShort(r.hours) + " (" + num(r.hours, 2) + " h)");
    lines.push("Filamentos (" + num(r.grams, 1) + " g):");
    r.filaments.forEach((f) => {
      lines.push("- " + (f.source === "list" || f.source === "auto" ? f.label : f.preset) + " — " + brl(f.price_kg) + "/kg — " + num(f.grams, 1) + " g = " + brl(f.cost));
    });
    lines.push("Custo de produção: " + brl(c.production) + " (máquina " + brl(c.machine) + ", filamento " + brl(c.filament) + (c.waste > 0 ? ", perdas " + brl(c.waste) : "") + ")");
    lines.push("Embalagem: " + brl(c.packaging) + (c.shipping > 0 ? " · Frete: " + brl(c.shipping) : "") +
      (c.fee_value > 0 ? " · Taxa: " + (c.fee_type === "pct" ? num(c.fee_value, 1) + "%" : brl(c.fee_value)) : ""));
    r.scenarios.forEach((s) => lines.push("Preço " + s.label + (s.chosen ? " ★" : "") + ": " + brl(s.price) + " (lucro " + brl(s.profit) + ")"));
    lines.push("Preço mínimo sem lucro: " + brl(r.break_even_price));
    if (r.evaluation) lines.push("Cobrando " + brl(r.evaluation.sell_price) + ": lucro " + brl(r.evaluation.net_profit) + " (" + brl(r.evaluation.profit_per_hour) + "/h) — " + r.evaluation.verdict_text);
    return lines.join("\n");
  }

  // ---------------------------------------------------------------------------------------------------------------
  // the filament list
  $("tipos").innerHTML = TIPOS.map((t) => '<option value="' + esc(t) + '">').join("");

  function updatePreview() {
    const kg = parseNum($("ff-kg").value), paid = parseNum($("ff-paid").value);
    $("ff-preview").textContent = kg > 0 && paid >= 0 ? "≈ " + brl(paid / kg) + "/kg · " + brl(paid / kg / 1000) + "/g" : "—";
  }
  $("ff-kg").addEventListener("input", updatePreview);
  $("ff-paid").addEventListener("input", updatePreview);

  function openFilForm(f) {
    editingFil = f ? f.id : null;
    $("ff-brand").value = f ? f.brand : "";
    $("ff-type").value = f ? f.type : "";
    $("ff-color").value = f ? f.color : "";
    $("ff-kg").value = f ? String(f.spool_kg).replace(".", ",") : "1";
    $("ff-paid").value = f ? String(f.paid).replace(".", ",") : "";
    $("ff-save").textContent = f ? "Salvar alterações" : "Salvar filamento";
    $("fil-form").classList.remove("hidden");
    updatePreview();
    $("ff-brand").focus();
  }
  function closeFilForm() { $("fil-form").classList.add("hidden"); editingFil = null; }
  $("btn-new-fil").addEventListener("click", () => openFilForm(null));
  // Claude looks up today's prices on the web and saves them (in the chat, where you can follow it)
  $("btn-find-prices").addEventListener("click", () => {
    if (!S || !(S.filaments || []).length) { toast("Cadastre um filamento primeiro.", true); return; }
    post({ cmd: "find_prices" });
    toast("O Claude está buscando os preços no chat. A lista atualiza sozinha.");
  });
  $("ff-cancel").addEventListener("click", closeFilForm);
  $("fil-form").addEventListener("submit", (e) => {
    e.preventDefault();
    const kg = parseNum($("ff-kg").value), paid = parseNum($("ff-paid").value);
    if (!(kg > 0)) { toast("Informe o peso da bobina em kg.", true); $("ff-kg").focus(); return; }
    if (paid === null || isNaN(paid)) { toast("Informe o preço pago.", true); $("ff-paid").focus(); return; }
    const f = { brand: $("ff-brand").value.trim(), type: $("ff-type").value.trim(), color: $("ff-color").value.trim(), spool_kg: kg, paid: paid };
    if (!f.brand && !f.type) { toast("Informe ao menos a marca ou o tipo.", true); return; }
    if (editingFil) f.id = editingFil;
    change({ action: "save_filament", filament: f });
    toast(editingFil ? "Filamento atualizado." : "Filamento cadastrado.");
    closeFilForm();
  });

  function renderFilList() {
    const host = $("fil-list");
    const list = S.filaments || [];
    if (!list.length) { host.innerHTML = '<div class="empty">Nenhum filamento cadastrado ainda.</div>'; return; }
    host.innerHTML = '<div class="table-wrap"><table class="table"><thead><tr><th>Marca</th><th>Tipo</th><th>Cor</th>' +
      '<th class="num">Bobina</th><th class="num">Preço pago</th><th class="num">Preço/kg</th><th></th></tr></thead><tbody>' +
      list.map((f) => "<tr><td>" + esc(f.brand || "—") + "</td><td>" + esc(f.type || "—") + "</td><td>" + esc(f.color || "—") + "</td>" +
        '<td class="num">' + num(f.spool_kg, 2) + ' kg</td><td class="num">' + brl(f.paid) + '</td><td class="num"><strong>' + brl(f.price_kg) + "</strong></td>" +
        '<td><div class="row-actions" style="margin:0;justify-content:flex-end">' +
        '<button class="btn small" data-find="' + esc(f.id) + '" title="O Claude busca o preço de hoje deste filamento">Buscar preço</button>' +
        '<button class="btn small" data-edit="' + esc(f.id) + '">Editar</button>' +
        '<button class="btn small danger' + (armedDelete === f.id ? " armed" : "") + '" data-del="' + esc(f.id) + '">' + (armedDelete === f.id ? "Confirmar" : "Excluir") + "</button>" +
        "</div></td></tr>").join("") + "</tbody></table></div>";
    host.querySelectorAll("[data-find]").forEach((b) => b.addEventListener("click", () => {
      post({ cmd: "find_prices", id: b.dataset.find });
      toast("O Claude está buscando o preço no chat.");
    }));
    host.querySelectorAll("[data-edit]").forEach((b) => b.addEventListener("click", () => openFilForm(list.find((f) => f.id === b.dataset.edit))));
    host.querySelectorAll("[data-del]").forEach((b) => b.addEventListener("click", () => {
      const id = b.dataset.del;
      if (armedDelete === id) {
        clearTimeout(armTimer); armedDelete = null;
        change({ action: "delete_filament", id: id });
        toast("Filamento excluído.");
      } else {
        armedDelete = id; renderFilList();
        clearTimeout(armTimer);
        armTimer = setTimeout(() => { armedDelete = null; renderFilList(); }, 3000);
      }
    }));
  }

  function parseImport(text) {
    text = text.trim();
    if (!text) return { list: [], errors: [] };
    const pick = (o, a, b) => (o[a] != null ? o[a] : o[b]);
    if (text[0] === "[" || text[0] === "{") {
      let j;
      try { j = JSON.parse(text); } catch (e) { return { list: [], errors: ["JSON inválido"] }; }
      const arr = Array.isArray(j) ? j : (j.filamentos || j.filaments || []);
      return {
        list: arr.map((o) => ({ brand: String(pick(o, "brand", "marca") || ""), type: String(pick(o, "type", "tipo") || ""),
          color: String(pick(o, "color", "cor") || ""), spool_kg: Number(pick(o, "spool_kg", "pesoBobinaKg") || 1), paid: Number(pick(o, "paid", "precoPago") || 0) })),
        errors: [],
      };
    }
    const list = [], errors = [];
    text.split(/\r?\n/).forEach((line, i) => {
      if (!line.trim()) return;
      const p = line.split(/;|\t/).map((x) => x.trim());
      if (p.length < 5) { errors.push("linha " + (i + 1) + ": precisa de 5 campos"); return; }
      const kg = parseNum(p[3]), paid = parseNum(p[4].replace(/^R\$\s*/i, ""));
      if (!(kg > 0) || paid === null || isNaN(paid)) { errors.push("linha " + (i + 1) + ": peso ou preço inválido"); return; }
      list.push({ brand: p[0], type: p[1], color: p[2], spool_kg: kg, paid: paid });
    });
    return { list: list, errors: errors };
  }
  $("btn-import").addEventListener("click", () => {
    const r = parseImport($("import-text").value);
    if (r.errors.length) { $("import-note").textContent = r.errors.join(" · "); toast("Corrija a lista antes de importar.", true); return; }
    if (!r.list.length) { toast("Cole a lista primeiro.", true); return; }
    change({ action: "import_filaments", filaments: r.list });
    $("import-note").textContent = r.list.length + " filamento(s) importado(s).";
    $("import-text").value = "";
  });

  // ---------------------------------------------------------------------------------------------------------------
  // messages from MiniMe
  function renderStatus() {
    const st = $("status");
    if (S.error) st.textContent = "Erro: " + S.error;
    else if (S.usage) st.textContent = (S.usage.all ? "Todas as placas" : "Placa " + S.usage.plate + " fatiada") + " · " + hoursText(S.usage.hours) + " · " + num(S.usage.grams, 1) + " g";
    else if (S.slicing) st.textContent = S.all ? "Fatiando as placas…" : "Fatiando a placa " + (S.plate || "") + "…";
    else if (S.all) st.textContent = (S.unsliced || []).length ? "Placas não fatiadas: " + plateNames(S.unsliced) : "Nenhuma placa com objetos";
    else st.textContent = "Placa " + (S.plate || "") + " não fatiada";
  }

  window.mm = {
    recv(msg) {
      if (!msg) return;
      if (msg.kind === "toast") { toast(msg.text, msg.error); return; }
      if (msg.kind !== "state") return;
      S = msg;
      document.body.classList.toggle("dark", !!S.dark);
      renderStatus();
      if (S.config) renderConfig();
      renderCalc();
      renderFilList();
    },
  };
  post({ cmd: "ready" });
})();
