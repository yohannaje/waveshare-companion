// Cerebro del compañero (Cloudflare Workers, plan GRATIS).
// POST /ask  : audio PCM 16 kHz 16-bit mono  -> respuesta en texto (sin tildes, corta)
// GET  /lista: lista de compras en JSON (para el tablero)
// Regla: costo cero. Tope diario propio muy por debajo de los límites gratis.

const MAX_POR_DIA = 40;            // preguntas por día. Con voz son ~200 neuronas cada una -> 8.000 de las 10.000 gratis por día
const MAX_SEGUNDOS = 10;           // audio máximo
const CORDOBA = { lat: 37.8882, lon: -4.7794 };
// Feriados locales de Córdoba capital (tradicionales: Virgen de la Fuensanta y San Rafael). Corregir si cambian.
const LOCALES_CORDOBA = ["2026-09-08", "2026-10-24", "2027-09-08", "2027-10-25"];
const STT = "@cf/openai/whisper-large-v3-turbo";
const LLM = "@cf/meta/llama-3.1-8b-instruct-fast";
const TTS = "@cf/deepgram/aura-2-es";   // 2727 neuronas cada 1000 letras
const VOCES = ["antonia", "aquila"];   // antonia = argentina; si no esta disponible, aquila (latinoamericana neutra)

const CORS = { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "*" };
function text(s, status) { return new Response(s, { status: status || 200, headers: { "Content-Type": "text/plain; charset=utf-8", ...CORS } }); }
function conVoz(pantalla, hablado) {
  const h = { "Content-Type": "text/plain; charset=utf-8", "Access-Control-Expose-Headers": "X-Voz", ...CORS };
  h["X-Voz"] = encodeURIComponent(String(hablado || pantalla).replace(/[\r\n]+/g, " ").slice(0, 90));
  return new Response(pantalla, { headers: h });
}
function json(o) { return new Response(JSON.stringify(o), { headers: { "Content-Type": "application/json", ...CORS } }); }
function sinTildes(s) { return s.normalize("NFD").replace(/[̀-ͯ]/g, "").replace(/[¿¡]/g, "").replace(/[^\x20-\x7E]/g, ""); }

function hoyMadrid() {
  const f = new Intl.DateTimeFormat("es-ES", { timeZone: "Europe/Madrid", year: "numeric", month: "2-digit", day: "2-digit", weekday: "long", hour: "2-digit", minute: "2-digit" });
  const p = Object.fromEntries(f.formatToParts(new Date()).map(x => [x.type, x.value]));
  return { iso: p.year + "-" + p.month + "-" + p.day, texto: p.weekday + " " + p.day + "/" + p.month + "/" + p.year + " " + p.hour + ":" + p.minute, year: parseInt(p.year) };
}

async function cached(url, ttl) {
  const cache = caches.default;
  const req = new Request(url);
  let r = await cache.match(req);
  if (!r) {
    r = await fetch(url);
    if (!r.ok) return null;
    r = new Response(r.body, r);
    r.headers.set("Cache-Control", "max-age=" + ttl);
    await cache.put(req, r.clone());
  }
  return r.json();
}

async function feriados(hoy) {
  const out = [];
  for (const y of [hoy.year, hoy.year + 1]) {
    const data = await cached("https://date.nager.at/api/v3/PublicHolidays/" + y + "/ES", 86400).catch(() => null);
    if (!data) continue;
    for (const h of data) {
      const c = h.counties;
      const madrid = !c || c.includes("ES-MD");
      const andalucia = !c || c.includes("ES-AN");
      if (madrid || andalucia) out.push({ fecha: h.date, nombre: h.localName, madrid, cordoba: andalucia });
    }
  }
  for (const d of LOCALES_CORDOBA) out.push({ fecha: d, nombre: "fiesta local de Cordoba", madrid: false, cordoba: true });
  return out.filter(h => h.fecha >= hoy.iso).sort((a, b) => a.fecha < b.fecha ? -1 : 1).slice(0, 8);
}

async function clima() {
  const u = "https://api.open-meteo.com/v1/forecast?latitude=" + CORDOBA.lat + "&longitude=" + CORDOBA.lon +
    "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max&current=temperature_2m&timezone=Europe%2FMadrid&forecast_days=3";
  const d = await cached(u, 1800).catch(() => null);
  if (!d || !d.daily) return "sin datos";
  const dias = ["hoy", "manana", "pasado"];
  const partes = d.daily.time.map((t, i) => dias[i] + ": min " + Math.round(d.daily.temperature_2m_min[i]) + ", max " + Math.round(d.daily.temperature_2m_max[i]) + ", lluvia " + d.daily.precipitation_probability_max[i] + "%");
  return "ahora " + Math.round(d.current.temperature_2m) + " grados. " + partes.join(" | ");
}

function wav(pcm) {
  const h = new ArrayBuffer(44), v = new DataView(h);
  const w = (o, s) => { for (let i = 0; i < s.length; i++) v.setUint8(o + i, s.charCodeAt(i)); };
  w(0, "RIFF"); v.setUint32(4, 36 + pcm.length, true); w(8, "WAVE"); w(12, "fmt ");
  v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 1, true); v.setUint32(24, 16000, true);
  v.setUint32(28, 32000, true); v.setUint16(32, 2, true); v.setUint16(34, 16, true); w(36, "data"); v.setUint32(40, pcm.length, true);
  const out = new Uint8Array(44 + pcm.length); out.set(new Uint8Array(h), 0); out.set(pcm, 44); return out;
}
function b64(u8) { let s = ""; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000)); return btoa(s); }

function parseJson(s) {
  const m = s.match(/\{[\s\S]*\}/);
  if (!m) return { respuesta: s };
  try { return JSON.parse(m[0]); } catch (e) { return { respuesta: s.replace(/[{}"]/g, "") }; }
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (req.method === "OPTIONS") return new Response(null, { headers: CORS });
    if (url.pathname === "/lista") return json({ lista: JSON.parse((await env.KV.get("lista")) || "[]") });
    if (url.pathname === "/voz" && req.method === "POST") {
      const dev = req.headers.get("X-Device") || "";
      if (!dev || dev !== (await env.KV.get("aparato"))) return text("No te conozco", 403);
      const clave = "v:" + hoyMadrid().iso;
      const n = parseInt((await env.KV.get(clave)) || "0");
      if (n >= MAX_POR_DIA) return text("tope", 429);
      let frase = "";
      try { frase = decodeURIComponent(await req.text()); } catch (e) { frase = ""; }
      frase = frase.trim().slice(0, 90);
      if (!frase) return text("vacio", 400);
      await env.KV.put(clave, String(n + 1), { expirationTtl: 172800 });
      for (const v of VOCES) {
        try {
          const r = await env.AI.run(TTS, { text: frase, speaker: v, encoding: "linear16", container: "none", sample_rate: 16000 }, { returnRawResponse: true });
          if (!r.ok) continue;
          return new Response(r.body, { headers: { "Content-Type": "application/octet-stream", "X-Voz-Usada": v, "Access-Control-Expose-Headers": "X-Voz-Usada", ...CORS } });
        } catch (e) { /* prueba la siguiente voz */ }
      }
      return text("sin voz", 500);
    }
    if (url.pathname !== "/ask" || req.method !== "POST") return text("Companero listo");

    // Solo responde al primer aparato que se conectó (tu Waveshare).
    const dev = req.headers.get("X-Device") || "";
    if (!dev) return text("Falta el aparato", 400);
    let dueno = await env.KV.get("aparato");
    if (!dueno) { await env.KV.put("aparato", dev); dueno = dev; }
    if (dev !== dueno) return text("No te conozco", 403);

    // Tope diario propio.
    const hoy = hoyMadrid();
    const clave = "n:" + hoy.iso;
    const n = parseInt((await env.KV.get(clave)) || "0");
    if (n >= MAX_POR_DIA) return text("Por hoy ya hable mucho. Manana sigo!");
    await env.KV.put(clave, String(n + 1), { expirationTtl: 172800 });

    let pcm = new Uint8Array(await req.arrayBuffer());
    if (pcm.length < 16000) return text("No te escuche. Mantene el boton mientras hablas.");
    if (pcm.length > 32000 * MAX_SEGUNDOS) pcm = pcm.subarray(0, 32000 * MAX_SEGUNDOS);

    let oido = "";
    try {
      const r = await env.AI.run(STT, { audio: b64(wav(pcm)), language: "es" });
      oido = (r.text || "").trim();
    } catch (e) { return text("Uy, no pude escuchar. Proba de nuevo."); }
    if (!oido) return text("No te escuche bien. Proba de nuevo.");

    const [fer, cli] = await Promise.all([feriados(hoy), clima()]);
    const lista = JSON.parse((await env.KV.get("lista")) || "[]");
    const sistema =
      "Sos Chipi, una mascota tierna (tipo ardillita) que vive en un aparatito en una casa de Cordoba (Andalucia, Espana). " +
      "Hablas en espanol rioplatense, con carino y muy breve. " +
      "Responde SOLO con un JSON asi: {\"respuesta\":\"...\",\"agregar\":[],\"quitar\":[],\"vaciar\":false,\"leer\":false}. " +
      "La respuesta tiene que tener como maximo 60 caracteres, sin emojis. " +
      "Si piden anotar o agregar cosas a la lista de compras, ponelas en agregar (en minuscula). Si dicen que ya compraron algo o que lo saquen, en quitar. Si piden borrar toda la lista, vaciar true. Si preguntan que hay que comprar o piden leer la lista, leer true (la respuesta la completa el sistema). " +
      "Datos reales (usalos, no inventes): Fecha y hora: " + hoy.texto + ". " +
      "Proximos feriados (madrid=feriado en Madrid, donde trabaja su marido; cordoba=feriado en Cordoba): " + JSON.stringify(fer) + ". " +
      "Clima en Cordoba: " + cli + ". " +
      "Lista de compras actual: " + (lista.length ? lista.join(", ") : "vacia") + ". " +
      "Si no sabes algo, decilo con gracia.";
    let res = { respuesta: "" };
    try {
      const r = await env.AI.run(LLM, { messages: [{ role: "system", content: sistema }, { role: "user", content: oido }], max_tokens: 160, temperature: 0.4 });
      const raw = typeof r.response === "string" ? r.response : (r.choices && r.choices[0] && r.choices[0].message && r.choices[0].message.content) || "";
      res = parseJson(raw);
    } catch (e) { return text("Me maree pensando: " + sinTildes(String(e && e.message || e)).slice(0, 40)); }

    let nueva = lista.slice();
    if (res.vaciar === true) nueva = [];
    for (const a of (res.agregar || [])) { const x = String(a).toLowerCase().trim(); if (x && !nueva.includes(x)) nueva.push(x); }
    for (const q of (res.quitar || [])) { const x = String(q).toLowerCase().trim(); nueva = nueva.filter(i => i !== x); }
    if (JSON.stringify(nueva) !== JSON.stringify(lista)) await env.KV.put("lista", JSON.stringify(nueva.slice(0, 50)));

    // Leer la lista lo arma el codigo (el modelo chico a veces la ignora).
    const pideLista = /(que|qué).{0,25}(compr|falta|anot)|lista.{0,15}(compra|super|mercado)?|(leer|decime|dime|mostra).{0,15}lista/i.test(oido);
    const modificó = (res.agregar && res.agregar.length) || (res.quitar && res.quitar.length) || res.vaciar === true;
    if (res.leer === true || (pideLista && !modificó)) {
      if (!nueva.length) return text("La lista esta vacia. Todo comprado!");
      let t = "Hay que comprar: ";
      let i = 0;
      for (; i < nueva.length; i++) { const add = (i ? ", " : "") + nueva[i]; if ((t + add).length > 60) break; t += add; }
      if (i < nueva.length) t += " y " + (nueva.length - i) + " mas";
      return conVoz(sinTildes(t).slice(0, 68), t);
    }
    const hablado = String(res.respuesta || "Mmm, no sé qué decir.").replace(/[{}"]/g, "").slice(0, 68);
    return conVoz(sinTildes(hablado).slice(0, 68), hablado);
  }
};
