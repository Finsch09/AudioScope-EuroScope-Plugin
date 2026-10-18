#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <map>
#include "EuroScopePlugIn.h"

#pragma comment(lib, "EuroScopePlugInDll.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

using namespace EuroScopePlugIn;

// Testschalter: Welche eingebaute EuroScope-Funktion soll den Ground-Status ausloesen?
// 28 = TAG_ITEM_FUNCTION_SET_GROUND_STATUS, 46 = TAG_ITEM_FUNCTION_SET_GROUND_STATUS_ADVANCED
#define AS_GS_FUNCTION 46

// 1 = Ground-Status zuerst per Scratchpad-Text setzen (braucht keinen Radarschirm), 0 = nur Tag-Funktion
#define AS_GS_VIA_SCRATCHPAD 1

// Plugin-Version (nur hier aendern)
#define AS_VERSION "0.26"

// 1 = ausfuehrliches Diagnose-Log in EuroScope, 0 = nur Wichtiges (Fehler, Verbindungen, Befehlsprobleme)
#define AS_VERBOSE 0

// Scratchpad-Text, den EuroScope als Ground-Status auswertet.
// Alle sieben Texte sind erprobt. Neue Kandidaten lassen sich mit .asgs TEXT testen.
static const char* ScratchFor(const std::string& key)
{
    if (key == "pending")  return "NSTS";
    if (key == "onfreq")   return "ONFREQ";
    if (key == "startup")  return "ST-UP";
    if (key == "push")     return "PUSH";
    if (key == "taxi")     return "TAXI";
    if (key == "lineup")   return "LINEUP";
    if (key == "depa")     return "DEPA";
    return nullptr;
}


static const int SERVER_PORT = 49081;
// Feste GUID aus RFC 6455 (Abschnitt 1.3). Muss exakt so lauten, sonst lehnt der Client den Handshake ab.
static const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// ---------------------------------------------------------------- Helfer
static std::string Lower(std::string s)
{
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static std::string Esc(const std::string& s)
{
    std::string out;
    for (char ch : s) {
        unsigned char u = (unsigned char)ch;
        if (u < 0x20) { out += ' '; continue; }     // Steuerzeichen/Zeilenumbrüche
        if (u >= 0x80) { out += '?'; continue; }    // Nicht-ASCII (kein gültiges UTF-8 aus EuroScope)
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
    }
    return out;
}

static std::string S(const char* p) { return p ? std::string(p) : std::string(); }

static std::string B64(const unsigned char* d, size_t n)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16;
        if (i + 1 < n) v |= (unsigned)d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        o += t[(v >> 18) & 63];
        o += t[(v >> 12) & 63];
        o += (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        o += (i + 2 < n) ? t[v & 63] : '=';
    }
    return o;
}

// Sec-WebSocket-Accept = Base64(SHA1(key + GUID))
static std::string AcceptKey(const std::string& key)
{
    std::string in = key + WS_GUID;
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    unsigned char dig[20] = { 0 };
    std::string result;

    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, nullptr, 0))) {
        if (BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0)) &&
            BCRYPT_SUCCESS(BCryptHashData(hHash, (PUCHAR)in.data(), (ULONG)in.size(), 0)) &&
            BCRYPT_SUCCESS(BCryptFinishHash(hHash, dig, 20, 0))) {
            result = B64(dig, 20);
        }
        if (hHash) BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }
    return result;
}

// Wert eines HTTP-Headers (Name klein geschrieben) aus der Anfrage
static bool HeaderValue(const std::string& req, const std::string& nameLower, std::string& out)
{
    std::string low = Lower(req);
    std::string needle = "\r\n" + nameLower + ":";
    size_t p = low.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    size_t e = req.find("\r\n", p);
    if (e == std::string::npos) e = req.size();
    out = req.substr(p, e - p);
    size_t a = out.find_first_not_of(" \t");
    size_t b = out.find_last_not_of(" \t\r\n");
    out = (a == std::string::npos) ? "" : out.substr(a, b - a + 1);
    return true;
}

static bool SendAll(SOCKET s, const std::string& d)
{
    size_t sent = 0;
    while (sent < d.size()) {
        int r = send(s, d.data() + sent, (int)(d.size() - sent), 0);
        if (r <= 0) return false;
        sent += (size_t)r;
    }
    return true;
}

// Server -> Client Frames sind nie maskiert
static bool SendFrame(SOCKET s, unsigned char opcode, const std::string& payload)
{
    std::string f;
    f += (char)(0x80 | opcode);
    size_t n = payload.size();
    if (n < 126) {
        f += (char)n;
    }
    else {
        f += (char)126;
        f += (char)((n >> 8) & 0xFF);
        f += (char)(n & 0xFF);
    }
    f += payload;
    return SendAll(s, f);
}

static std::string Json(const std::string& cs, int seq)
{
    return "{\"callsign\":\"" + Esc(cs) + "\",\"seq\":" + std::to_string(seq) + "}";
}

// Anzahl der offenen AudioScope-Radarschirme (0 = keiner). Die App zeigt ohne Radarschirm nur die Remarks.
static std::string StatusJson(int screens)
{
    return "{\"type\":\"status\",\"screens\":" + std::to_string(screens) + "}";
}

// Eigene Plugin-Version: wird der App beim Verbinden gemeldet (die App vergleicht sie mit der neuesten Version)
static std::string VersionJson()
{
    return "{\"type\":\"version\",\"version\":\"" AS_VERSION "\"}";
}

// Wert eines String-Feldes aus einem einfachen JSON ({"key":"wert"}), sonst ""
static std::string JsonStr(const std::string& p, const char* key)
{
    std::string k = std::string("\"") + key + "\"";
    size_t a = p.find(k);
    if (a == std::string::npos) return "";
    size_t c = p.find(':', a + k.size());
    if (c == std::string::npos) return "";
    size_t q1 = p.find('"', c);
    if (q1 == std::string::npos) return "";
    size_t q2 = p.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return p.substr(q1 + 1, q2 - q1 - 1);
}

// nur A-Z, a-z, 0-9, _ und - erlaubt, 1..16 Zeichen
static bool SafeToken(const std::string& s)
{
    if (s.empty() || s.size() > 16) return false;
    for (char ch : s)
        if (!isalnum((unsigned char)ch) && ch != '_' && ch != '-') return false;
    return true;
}

// {"select":"DLH2LY"} -> "DLH2LY"
static std::string ParseSelect(const std::string& p)
{
    std::string cs = JsonStr(p, "select");
    return SafeToken(cs) ? cs : "";
}

// Befehl der App: {"cmd":"clearance"|"gs","callsign":"DLH2LY","arg":"on"|"off"|"push"...}
struct Cmd { std::string cmd, callsign, arg; };

// Ground-Status: Name in der App -> Wert in EuroScope (Ground Radar Plugin).
// Lesen geht direkt, Setzen nur ueber Scratchpad oder die eingebaute Tag-Funktion (oeffnet ein Menue).
struct GroundStateMap { const char* key; const char* es; };
static const GroundStateMap kGS[] = {
    { "pending", "NSTS" },
    { "onfreq",  "ONFREQ" },
    { "startup", "STUP" },
    { "push",    "PUSH" },
    { "taxi",    "TAXI" },
    { "lineup",  "LINEUP" },
    { "depa",    "DEPA" },
};

static std::string NormGS(const std::string& s)
{
    std::string o;
    for (char c : s) {
        if (c == '-' || c == ' ' || c == '/') continue;
        o += (char)toupper((unsigned char)c);
    }
    return o;
}

// ---------------------------------------------------------------- Radarschirm
// Noetig, damit das Plugin den Schirm neu zeichnen lassen und Tag-Funktionen ausloesen kann.
// v0.18: JEDER Radarschirm wird gemerkt (vorher nur der mit dem Namen "Standard ES radar screen").
class CMyScreen;
static std::vector<CMyScreen*> gScreens;
static void LogMsg(const std::string& m);   // wichtige Meldung in die EuroScope-Meldungsliste (unten definiert)
static void Dbg(const std::string& m);      // Diagnosemeldung, nur bei AS_VERBOSE 1 (unten definiert)

class CMyScreen : public CRadarScreen
{
public:
    CMyScreen()
    {
        gScreens.push_back(this);
    }
    virtual ~CMyScreen()
    {
        for (size_t i = 0; i < gScreens.size(); i++) {
            if (gScreens[i] == this) { gScreens.erase(gScreens.begin() + i); break; }
        }
    }
    void OnAsrContentToBeClosed() override
    {
        Dbg("Radarschirm wird geschlossen");
        delete this;
    }
};

// ---------------------------------------------------------------- Plugin
class CMyPlugin : public CPlugIn
{
public:
    CMyPlugin() : CPlugIn(COMPATIBILITY_CODE,
        "AudioScope Plugin", AS_VERSION, "Finnley Schelhaas", "GPL v3")
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);

        // Selbsttest: Beispiel aus RFC 6455 - nur melden, wenn er fehlschlaegt
        std::string t = AcceptKey("dGhlIHNhbXBsZSBub25jZQ==");
        if (t != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
            DisplayUserMessage("AudioScope", "", "FEHLER: WebSocket Accept-Test fehlgeschlagen",
                true, true, true, false, false);
        else
            Dbg("Accept-Test OK");

        m_thread = std::thread(&CMyPlugin::Serve, this);

        std::string msg = "Plugin v" AS_VERSION " geladen, WebSocket auf Port " + std::to_string(SERVER_PORT);
        DisplayUserMessage("AudioScope", "", msg.c_str(), true, true, true, false, false);

        // EuroScope gibt einem Plugin nur Radarschirme der Anzeigetypen, die es selbst anmeldet.
        // Danach erscheint "AudioScope" in der Liste der neu oeffenbaren Anzeigen.
        // Parameter: Name, NeedRadarContent, GeoReferenced, CanBeSaved, CanBeCreated.
        // CanBeSaved = true: EuroScope speichert den Schirm mit der Anzeige-Einstellung (ASR) und oeffnet ihn beim Start wieder.
        RegisterDisplayType("AudioScope", false, false, true, true);
        Dbg("Anzeigetyp \"AudioScope\" angemeldet (Open -> New display)");
    }

    ~CMyPlugin()
    {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
        WSACleanup();
    }

    // EuroScope ruft das einmal pro Sekunde auf: angeklicktes Flugzeug (ASEL) prüfen
    void OnTimer(int Counter) override
    {
        if (m_bindFailed.exchange(false)) {
            DisplayUserMessage("AudioScope", "", "FEHLER: Port ist belegt, Server nicht gestartet",
                true, true, true, false, false);
        }

        // Statusbericht: nur bei Aenderung von Radarschirmen oder App-Verbindungen
        int nScr = (int)gScreens.size();
        m_screenCount = nScr;   // der Server-Thread meldet Aenderungen an die App
        int nCli = m_clientCount.load();
        if (nScr != m_lastScr || nCli != m_lastCli) {
            m_lastScr = nScr;
            m_lastCli = nCli;
            std::string st = "Radarschirme=" + std::to_string(nScr) +
                ", App-Verbindungen=" + std::to_string(nCli);
            if (nScr == 0) st += " -> Anzeige vom Typ AudioScope oeffnen";
            DisplayUserMessage("AudioScope", "", st.c_str(), true, true, true, false, false);
        }

        CheckSelection(true);
    }

    CRadarScreen* OnRadarScreenCreated(const char* sDisplayName, bool NeedRadarContent,
        bool GeoReferenced, bool CanBeSaved, bool CanBeCreated) override
    {
        // Diagnose: echten Namen des Schirms anzeigen (nur im Verbose-Modus)
        Dbg(std::string("OnRadarScreenCreated: Name=\"") + (sDisplayName ? sDisplayName : "(null)") +
            "\" NeedRadarContent=" + (NeedRadarContent ? "1" : "0") +
            " GeoReferenced=" + (GeoReferenced ? "1" : "0") +
            " CanBeSaved=" + (CanBeSaved ? "1" : "0") +
            " CanBeCreated=" + (CanBeCreated ? "1" : "0"));
        return new CMyScreen();
    }

    // Testbefehl in der EuroScope-Kommandozeile:  .asgs TEXT
    // Setzt den Scratchpad des ausgewaehlten Flugzeugs auf TEXT und meldet, ob sich der Ground-Status aendert.
    // Damit lassen sich Kandidaten wie NSTS, ONFREQ, LINEUP ... ohne Neukompilieren ausprobieren.
    bool OnCompileCommand(const char* sCommandLine) override
    {
        std::string raw = S(sCommandLine);

        if (Lower(raw).compare(0, 5, ".asgs") != 0) return false;

        std::string text = raw.substr(5);
        size_t a = text.find_first_not_of(" \t");
        text = (a == std::string::npos) ? "" : text.substr(a);
        size_t b = text.find_last_not_of(" \t\r\n");
        if (b != std::string::npos) text = text.substr(0, b + 1);

        if (text.empty()) {
            LogMsg("Benutzung: Flugzeug anklicken, dann  .asgs TEXT  (zum Beispiel .asgs LINEUP)");
            return true;
        }
        CFlightPlan fp = FlightPlanSelectASEL();
        if (!fp.IsValid()) {
            LogMsg(".asgs: kein Flugzeug ausgewaehlt");
            return true;
        }

        std::string cs = S(fp.GetCallsign());
        std::string before = S(fp.GetGroundState());
        std::string spBefore = S(fp.GetControllerAssignedData().GetScratchPadString());
        bool ok = fp.GetControllerAssignedData().SetScratchPadString(text.c_str());
        CFlightPlan fp2 = FlightPlanSelect(cs.c_str());
        std::string after = S(fp2.GetGroundState());
        std::string spAfter = S(fp2.GetControllerAssignedData().GetScratchPadString());

        bool restored = false;
        if (ok && after == before && spAfter == text) {
            fp2.GetControllerAssignedData().SetScratchPadString(spBefore.c_str());
            restored = true;
        }
        LogMsg(".asgs " + cs + " \"" + text + "\": SetScratchPadString=" + (ok ? "true" : "false") +
            ", Ground-Status vorher=\"" + before + "\" nachher=\"" + after + "\", Scratchpad nachher=\"" + spAfter + "\"" +
            (restored ? " -> ohne Wirkung, Scratchpad zurueckgesetzt" : (after != before ? " -> WIRKT" : "")));
        return true;
    }

    // Aendert sich in EuroScope der Ground-Status oder das Clearance-Flag (egal ob per Klick oder per Plugin),
    // wird der neue Stand sofort an die App gepusht. So bleibt die App synchron, ohne dass sie nachfragen muss.
    void OnFlightPlanControllerAssignedDataUpdate(CFlightPlan FlightPlan, int DataType) override
    {
        if (DataType != CTR_DATA_TYPE_GROUND_STATE && DataType != CTR_DATA_TYPE_CLEARENCE_FLAG) return;
        if (!FlightPlan.IsValid()) return;
        std::string cs = S(FlightPlan.GetCallsign());
        if (!SafeToken(cs)) return;
        std::string j = BuildInfo(cs);   // loggt dabei (nur Verbose) auch den rohen Ground-Status-String
        std::lock_guard<std::mutex> lk(m_mx);
        m_outbox.push_back(j);
    }

    // Wird bei jeder Positionsaktualisierung irgendeines Flugzeugs aufgerufen (meist mehrmals pro Sekunde).
    // Nutzen wir als zusätzlichen, schnelleren Takt für die Auswahlprüfung.
    void OnRadarTargetPositionUpdate(CRadarTarget RadarTarget) override
    {
        CheckSelection(false);
    }

private:
    // Nur im EuroScope-Hauptthread aufrufen (OnTimer / OnRadarTargetPositionUpdate).
    // Ohne force höchstens alle 50 ms, damit viele gleichzeitige Updates nicht bremsen.
    ULONGLONG m_lastCheck = 0;
    int m_lastScr = -1;
    int m_lastCli = -1;
    std::atomic<int> m_clientCount{ 0 };
    std::atomic<int> m_screenCount{ 0 };           // Anzahl Radarschirme, vom Hauptthread gesetzt, vom Server-Thread gelesen
    std::map<std::string, std::string> m_lastGs;   // zuletzt geloggter Ground-Status je Callsign

    void CheckSelection(bool force)
    {
        ULONGLONG now = GetTickCount64();
        if (!force && now - m_lastCheck < 50) return;
        m_lastCheck = now;

        ProcessRequests();

        // Auswahl-Anfrage von der App (vom Server-Thread abgelegt) hier im Hauptthread ausführen
        std::string want;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            want.swap(m_pendingSelect);
        }
        if (!want.empty()) {
            CRadarTarget target = RadarTargetSelect(want.c_str());
            CFlightPlan fp = FlightPlanSelect(want.c_str());
            bool tv = target.IsValid();
            bool fv = fp.IsValid();
            // Radarziel UND Flugplan setzen: Listen (z. B. Departure List) hängen am Flugplan.
            // Der Flugplan zuletzt, damit er die endgültige Auswahl ist.
            if (tv) SetASELAircraft(target);
            if (fv) SetASELAircraft(fp);
            for (auto* scr : gScreens) scr->RefreshMapContent();   // Markierung sofort neu zeichnen

            // Ergebnis prüfen (zur Fehlersuche, nur Verbose)
#if AS_VERBOSE
            CRadarTarget sel = RadarTargetSelectASEL();
            std::string got = sel.IsValid() ? sel.GetCallsign() : "(keins)";
            CFlightPlan selFp = FlightPlanSelectASEL();
            std::string gotFp = selFp.IsValid() ? selFp.GetCallsign() : "(keiner)";
            Dbg("App-Auswahl " + want +
                ": Radarziel=" + (tv ? "ja" : "nein") +
                ", Flugplan=" + (fv ? "ja" : "nein") +
                ", jetzt ausgewählt=" + got + ", Flugplan-Auswahl=" + gotFp);
#else
            // Wichtig ist nur, wenn die App ein Flugzeug waehlen wollte, das es nicht gibt
            if (!tv && !fv) LogMsg("App-Auswahl " + want + ": Flugzeug nicht gefunden");
#endif
        }

        CRadarTarget rt = RadarTargetSelectASEL();
        std::string cs = rt.IsValid() ? rt.GetCallsign() : "";

        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            if (cs != m_callsign) {
                m_callsign = cs;
                m_seq++;
                changed = true;
            }
        }
        if (changed && !cs.empty()) Dbg("Ausgewählt: " + cs);
    }

    // Flugplan-Daten eines Flugzeugs als JSON (nur im Hauptthread aufrufen)
    std::string BuildInfo(const std::string& cs)
    {
        std::string j = "{\"type\":\"info\",\"callsign\":\"" + Esc(cs) + "\"";
        CFlightPlan fp = FlightPlanSelect(cs.c_str());
        if (!fp.IsValid()) return j + ",\"found\":false}";

        CFlightPlanData d = fp.GetFlightPlanData();
        CFlightPlanControllerAssignedData ca = fp.GetControllerAssignedData();
        CRadarTarget rt = fp.GetCorrelatedRadarTarget();

        // Wie in der CLMB-Spalte: die vom Lotsen gesetzte Höhe (CFL), NICHT fp.GetClearedAltitude(),
        // das fällt auf die Reiseflughöhe zurück, wenn nichts gesetzt ist.
        int cleared = ca.GetClearedAltitude();
        if (cleared < 100) cleared = 0;            // 0/1/2 sind Sonderwerte, keine Höhe

        int fin = fp.GetFinalAltitude();

        std::string gs = S(fp.GetGroundState());
        std::string gsKey;
        for (const auto& g : kGS)
            if (!gs.empty() && NormGS(gs) == NormGS(g.es)) gsKey = g.key;

        // Log bei jeder Aenderung des Ground-Status: zeigt den ROHEN String von EuroScope (nur Verbose)
        {
            auto it = m_lastGs.find(cs);
            if (it == m_lastGs.end() || it->second != gs) {
                m_lastGs[cs] = gs;
                Dbg("Ground-Status " + cs + ": EuroScope meldet \"" + gs + "\" -> App-Key \"" +
                    (gsKey.empty() ? std::string("(keiner, nicht in kGS)") : gsKey) + "\"");
            }
        }

        int spd = 0;
        bool airborne = false;
        if (rt.IsValid()) {
            spd = rt.GetPosition().GetReportedGS();
            airborne = spd >= 60;
        }

        j += ",\"found\":true";
        j += ",\"dep\":\"" + Esc(S(d.GetOrigin())) + "\"";
        j += ",\"arr\":\"" + Esc(S(d.GetDestination())) + "\"";
        j += ",\"sid\":\"" + Esc(S(d.GetSidName())) + "\"";
        j += ",\"route\":\"" + Esc(S(d.GetRoute())) + "\"";
        j += ",\"remarks\":\"" + Esc(S(d.GetRemarks())) + "\"";
        j += ",\"squawk\":\"" + Esc(S(ca.GetSquawk())) + "\"";
        j += ",\"clearedAlt\":" + std::to_string(cleared);
        j += ",\"finalAlt\":" + std::to_string(fin);
        j += std::string(",\"clearance\":") + (fp.GetClearenceFlag() ? "true" : "false");
        j += ",\"gs\":\"" + Esc(gs) + "\"";
        j += ",\"gsKey\":\"" + Esc(gsKey) + "\"";
        j += std::string(",\"airborne\":") + (airborne ? "true" : "false");
        j += ",\"speed\":" + std::to_string(spd);
        j += "}";
        return j;
    }

    void DoCommand(const Cmd& c)
    {
        Dbg("App-Befehl empfangen: cmd=" + c.cmd + " callsign=" + c.callsign + " arg=" + c.arg +
            " (Radarschirme=" + std::to_string(gScreens.size()) + ")");

        CFlightPlan fp = FlightPlanSelect(c.callsign.c_str());
        std::string msg;
        bool important = false;   // true = Meldung erscheint auch ohne AS_VERBOSE

        if (!fp.IsValid()) {
            msg = "App-Befehl " + c.cmd + " " + c.callsign + ": kein Flugplan gefunden";
            important = true;
        }
        else if (c.cmd == "clearance") {
            bool want = (c.arg != "off");
            bool before = fp.GetClearenceFlag();
            if (before == want) {
                msg = "App: Clearance-Flag bei " + c.callsign + " ist schon " + (want ? "gesetzt" : "entfernt");
            }
            else if (gScreens.empty()) {
                msg = "App: Clearance nicht gesetzt - kein Radarschirm. Anzeige vom Typ AudioScope oeffnen (Open -> New display)";
                important = true;
            }
            else {
                // Eingebaute EuroScope-Funktion "Clearance-Flag setzen" ausloesen, als haette man in der
                // Departure List auf die C-Spalte geklickt. Dazu das Flugzeug vorher auswaehlen (ASEL).
                SetASELAircraft(fp);
                POINT pt = { 0, 0 };
                RECT area = { 0, 0, 0, 0 };
                gScreens[0]->StartTagFunction(c.callsign.c_str(), nullptr, TAG_ITEM_TYPE_CLEARENCE, "",
                    nullptr, TAG_ITEM_FUNCTION_SET_CLEARED_FLAG, pt, area);
                bool after = FlightPlanSelect(c.callsign.c_str()).GetClearenceFlag();
                msg = "App: Clearance-Flag bei " + c.callsign + ": vorher=" + (before ? "ja" : "nein") +
                    ", nachher=" + (after ? "ja" : "nein");
                if (after != want) {   // hat nicht geklappt
                    msg += " -> NICHT geaendert";
                    important = true;
                }
            }
        }
        else if (c.cmd == "gs") {
            // TEST: eingebaute Tag-Funktion fuer den Ground-Status ausloesen. Sie oeffnet in EuroScope
            // vermutlich nur das Auswahlmenue; ob sie direkt setzt, zeigt die Meldung (vorher/nachher).
            bool known = false;
            for (const auto& g : kGS) if (c.arg == g.key) known = true;

            if (!known) {
                msg = "App: Ground-Status \"" + c.arg + "\" gibt es nicht (oder wurde entfernt), ignoriert";
                important = true;
            }
            else if (AS_GS_VIA_SCRATCHPAD && ScratchFor(c.arg)) {
                // Weg ohne Radarschirm: Scratchpad-Text setzen, EuroScope leitet daraus den Ground-Status ab
                const char* sp = ScratchFor(c.arg);
                std::string before = S(fp.GetGroundState());
                std::string spBefore = S(fp.GetControllerAssignedData().GetScratchPadString());
                bool ok = fp.GetControllerAssignedData().SetScratchPadString(sp);
                CFlightPlan fp2 = FlightPlanSelect(c.callsign.c_str());
                std::string after = S(fp2.GetGroundState());
                std::string spAfter = S(fp2.GetControllerAssignedData().GetScratchPadString());
                // Hat der Text nichts bewirkt, den alten Scratchpad-Inhalt wiederherstellen
                bool restored = false;
                if (ok && after == before && spAfter == sp) {
                    fp2.GetControllerAssignedData().SetScratchPadString(spBefore.c_str());
                    restored = true;
                }
                msg = "App: Ground-Status per Scratchpad \"" + std::string(sp) + "\" bei " + c.callsign +
                    (restored ? " (ohne Wirkung, Scratchpad zurueckgesetzt)" : "") +
                    ": SetScratchPadString=" + (ok ? "true" : "false") +
                    ", Scratchpad vorher=\"" + spBefore + "\" nachher=\"" + spAfter +
                    "\", Ground-Status vorher=\"" + before + "\" nachher=\"" + after + "\"";
                if (!ok || restored) important = true;   // Fehlschlag oder ohne Wirkung
            }
            else if (gScreens.empty()) {
                msg = "App: Ground-Status - kein Radarschirm und kein Scratchpad-Weg fuer \"" + c.arg +
                    "\" (Anzeige vom Typ AudioScope oeffnen)";
                important = true;
            }
            else {
                std::string before = S(fp.GetGroundState());
                SetASELAircraft(fp);
                POINT pt = { 300, 300 };
                RECT area = { 300, 300, 360, 320 };
                gScreens[0]->StartTagFunction(c.callsign.c_str(), nullptr, TAG_ITEM_TYPE_GROUND_STATUS,
                    before.c_str(), nullptr, AS_GS_FUNCTION, pt, area);
                std::string after = S(FlightPlanSelect(c.callsign.c_str()).GetGroundState());
                msg = "App: Ground-Status-Funktion " + std::to_string(AS_GS_FUNCTION) + " fuer " + c.callsign +
                    " (gewuenscht " + c.arg + "): vorher=\"" + before + "\", nachher=\"" + after + "\"";
            }
        }
        else {
            msg = "App-Befehl unbekannt: " + c.cmd;
            important = true;
        }

        if (important) LogMsg(msg);
        else Dbg(msg);

        // aktuellen Stand sofort an die App zurueckmelden
        std::string j = BuildInfo(c.callsign);
        std::lock_guard<std::mutex> lk(m_mx);
        m_outbox.push_back(j);
    }

    // Liste der aktiven Flughaefen (Runway-Aktivitaet in EuroScope) als JSON
    std::string BuildAirports()
    {
        std::string list;
        CSectorElement el = SectorFileElementSelectFirst(SECTOR_ELEMENT_AIRPORT);
        while (el.IsValid()) {
            if (el.IsElementActive(true) || el.IsElementActive(false)) {
                std::string n = S(el.GetName());
                if (!n.empty()) {
                    if (!list.empty()) list += ",";
                    list += n;
                }
            }
            el = SectorFileElementSelectNext(el, SECTOR_ELEMENT_AIRPORT);
        }
        return "{\"type\":\"airports\",\"list\":\"" + Esc(list) + "\"}";
    }

    void OnAirportRunwayActivityChanged(void) override { m_wantAirports = true; }

    // Anfragen/Befehle der App abarbeiten (läuft im EuroScope-Hauptthread)
    void ProcessRequests()
    {
        std::vector<std::string> infos;
        std::vector<Cmd> cmds;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            infos.swap(m_pendingInfo);
            cmds.swap(m_pendingCmd);
        }
        for (const auto& c : cmds) DoCommand(c);
        if (m_wantAirports.exchange(false)) {
            std::string j = BuildAirports();
            std::lock_guard<std::mutex> lk(m_mx);
            m_outbox.push_back(j);
        }
        for (const auto& cs : infos) {
            std::string j = BuildInfo(cs);
            std::lock_guard<std::mutex> lk(m_mx);
            m_outbox.push_back(j);
        }
    }

    struct Client {
        SOCKET s = INVALID_SOCKET;
        bool ws = false;      // Handshake abgeschlossen
        bool alive = true;
        std::string buf;
        ULONGLONG since = 0;
    };

    std::thread m_thread;
    std::atomic<bool> m_stop{ false };
    std::atomic<bool> m_bindFailed{ false };
    std::atomic<bool> m_wantAirports{ false };
    std::mutex m_mx;
    std::string m_callsign;
    std::string m_pendingSelect;   // Callsign-Wunsch der App, geschützt durch m_mx
    std::vector<std::string> m_pendingInfo;   // Info-Anfragen der App (Callsigns), geschützt durch m_mx
    std::vector<Cmd> m_pendingCmd;            // Befehle der App, geschützt durch m_mx
    std::vector<std::string> m_outbox;        // Antworten, die der Server-Thread an alle Clients sendet
    int m_seq = 0;

    void Snapshot(std::string& cs, int& seq)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        cs = m_callsign;
        seq = m_seq;
    }

    // WebSocket-Server (eigener Thread). Normale HTTP-Anfragen (z. B. Browser-Test)
    // bekommen weiterhin die JSON-Antwort.
    void Serve()
    {
        SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls == INVALID_SOCKET) { m_bindFailed = true; return; }

        int yes = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(SERVER_PORT);

        if (bind(ls, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
            listen(ls, 8) == SOCKET_ERROR) {
            closesocket(ls);
            m_bindFailed = true;
            return;
        }

        std::vector<Client> clients;
        int sentSeq = -1;
        int sentScr = -1;   // zuletzt an die Clients gemeldete Anzahl Radarschirme

        while (!m_stop) {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(ls, &rf);
            for (auto& c : clients) FD_SET(c.s, &rf);
            timeval tv = { 0, 30000 };   // 30 ms
            int r = select(0, &rf, nullptr, nullptr, &tv);

            if (r > 0) {
                if (FD_ISSET(ls, &rf)) {
                    SOCKET s = accept(ls, nullptr, nullptr);
                    if (s != INVALID_SOCKET) {
                        if (clients.size() >= 40) {
                            closesocket(s);
                        }
                        else {
                            DWORD to = 1000;
                            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
                            Client c;
                            c.s = s;
                            c.since = GetTickCount64();
                            clients.push_back(c);
                        }
                    }
                }
                for (auto& c : clients) {
                    if (c.alive && FD_ISSET(c.s, &rf)) OnReadable(c);
                }
            }

            // Verbindungen ohne Handshake nach 5 s verwerfen
            ULONGLONG now = GetTickCount64();
            for (auto& c : clients) {
                if (c.alive && !c.ws && now - c.since > 5000) c.alive = false;
            }

            // Anzahl der fertig verbundenen App-Clients fuer den Statusbericht
            {
                int n = 0;
                for (auto& c : clients) if (c.alive && c.ws) n++;
                m_clientCount = n;
            }

            // Auswahl hat sich geändert -> sofort an alle verbundenen Clients pushen
            std::string cs; int seq;
            Snapshot(cs, seq);
            if (seq != sentSeq) {
                sentSeq = seq;
                std::string j = Json(cs, seq);
                for (auto& c : clients) {
                    if (c.alive && c.ws && !SendFrame(c.s, 0x1, j)) c.alive = false;
                }
            }

            // Anzahl der Radarschirme hat sich geändert -> an alle verbundenen Clients melden
            int sc = m_screenCount.load();
            if (sc != sentScr) {
                sentScr = sc;
                std::string sj = StatusJson(sc);
                for (auto& c : clients) {
                    if (c.alive && c.ws && !SendFrame(c.s, 0x1, sj)) c.alive = false;
                }
            }

            // Antworten (Info/Befehlsergebnis) an alle verbundenen Clients senden
            std::vector<std::string> out;
            {
                std::lock_guard<std::mutex> lk(m_mx);
                out.swap(m_outbox);
            }
            for (const auto& oj : out) {
                for (auto& c : clients) {
                    if (c.alive && c.ws && !SendFrame(c.s, 0x1, oj)) c.alive = false;
                }
            }

            // Tote Verbindungen entfernen
            for (size_t i = 0; i < clients.size();) {
                if (!clients[i].alive) {
                    closesocket(clients[i].s);
                    clients.erase(clients.begin() + i);
                }
                else {
                    ++i;
                }
            }
        }

        for (auto& c : clients) closesocket(c.s);
        closesocket(ls);
    }

    void OnReadable(Client& c)
    {
        char tmp[2048];
        int n = recv(c.s, tmp, sizeof(tmp), 0);
        if (n <= 0) { c.alive = false; return; }
        c.buf.append(tmp, (size_t)n);
        if (c.buf.size() > 16384) { c.alive = false; return; }

        if (!c.ws) {
            size_t e = c.buf.find("\r\n\r\n");
            if (e == std::string::npos) return;   // Anfrage noch nicht komplett
            std::string req = c.buf.substr(0, e + 2);
            c.buf.erase(0, e + 4);

            std::string up, key;
            if (HeaderValue(req, "upgrade", up) && Lower(up).find("websocket") != std::string::npos &&
                HeaderValue(req, "sec-websocket-key", key)) {
                std::string acc = AcceptKey(key);
                if (acc.empty()) { c.alive = false; return; }
                std::string resp =
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " + acc + "\r\n\r\n";
                if (!SendAll(c.s, resp)) { c.alive = false; return; }
                c.ws = true;

                // aktuellen Stand gleich senden (die App nutzt ihn als Ausgangswert)
                std::string cs; int seq;
                Snapshot(cs, seq);
                if (!SendFrame(c.s, 0x1, Json(cs, seq))) { c.alive = false; return; }

                // Radarschirm-Status gleich mitsenden (0 = App zeigt nur Remarks)
                if (!SendFrame(c.s, 0x1, StatusJson(m_screenCount.load()))) { c.alive = false; return; }

                // Plugin-Version mitsenden (die App prueft damit, ob eine neuere Version verfuegbar ist)
                if (!SendFrame(c.s, 0x1, VersionJson())) { c.alive = false; return; }
            }
            else {
                // normale HTTP-Anfrage (Browser-Test): JSON ausliefern und schließen
                std::string cs; int seq;
                Snapshot(cs, seq);
                std::string body = Json(cs, seq);
                std::string resp =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Cache-Control: no-store\r\n"
                    "Connection: close\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
                SendAll(c.s, resp);
                c.alive = false;
                return;
            }
        }

        ProcessFrames(c);
    }

    // Frames vom Client: Text ({"select":...}), Ping (-> Pong) und Close beachten, Rest ignorieren
    void ProcessFrames(Client& c)
    {
        while (c.alive && c.buf.size() >= 2) {
            const unsigned char* b = (const unsigned char*)c.buf.data();
            int opcode = b[0] & 0x0F;
            bool masked = (b[1] & 0x80) != 0;
            size_t len = b[1] & 0x7F;
            size_t off = 2;
            if (len == 126) {
                if (c.buf.size() < 4) return;
                len = ((size_t)b[2] << 8) | b[3];
                off = 4;
            }
            else if (len == 127) {
                c.alive = false;   // viel zu groß für unseren Zweck
                return;
            }
            size_t maskOff = off;
            if (masked) off += 4;
            if (c.buf.size() < off + len) return;   // Frame noch nicht komplett

            std::string payload = c.buf.substr(off, len);
            if (masked) {
                for (size_t i = 0; i < len; i++) payload[i] ^= b[maskOff + (i & 3)];
            }
            c.buf.erase(0, off + len);

            if (opcode == 0x8) { c.alive = false; return; }                       // Close
            if (opcode == 0x1) {                                                  // Text-Nachricht der App
                std::string cs = ParseSelect(payload);
                if (!cs.empty()) {
                    std::lock_guard<std::mutex> lk(m_mx);
                    m_pendingSelect = cs;
                }
                std::string inf = JsonStr(payload, "info");
                if (SafeToken(inf)) {
                    std::lock_guard<std::mutex> lk(m_mx);
                    bool have = false;
                    for (const auto& x : m_pendingInfo) if (x == inf) have = true;
                    if (!have && m_pendingInfo.size() < 32) m_pendingInfo.push_back(inf);
                }
                if (!JsonStr(payload, "airports").empty()) m_wantAirports = true;
                std::string cmd = JsonStr(payload, "cmd");
                if (!cmd.empty()) {
                    Cmd cm;
                    cm.cmd = cmd;
                    cm.callsign = JsonStr(payload, "callsign");
                    cm.arg = JsonStr(payload, "arg");
                    if (SafeToken(cm.cmd) && SafeToken(cm.callsign) && SafeToken(cm.arg)) {
                        std::lock_guard<std::mutex> lk(m_mx);
                        if (m_pendingCmd.size() < 16) m_pendingCmd.push_back(cm);
                    }
                }
            }
            if (opcode == 0x9 && !SendFrame(c.s, 0xA, payload)) c.alive = false;  // Ping -> Pong
        }
    }
};

static CMyPlugin* gpPlugin = nullptr;

static void LogMsg(const std::string& m)
{
    if (gpPlugin) gpPlugin->DisplayUserMessage("AudioScope", "", m.c_str(), true, true, true, false, false);
}

static void Dbg(const std::string& m)
{
#if AS_VERBOSE
    LogMsg(m);
#else
    (void)m;
#endif
}

void __declspec(dllexport) EuroScopePlugInInit(CPlugIn** ppPlugInInstance)
{
    *ppPlugInInstance = gpPlugin = new CMyPlugin();
}

void __declspec(dllexport) EuroScopePlugInExit(void)
{
    delete gpPlugin;
}