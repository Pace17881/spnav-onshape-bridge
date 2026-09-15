# spnav-onshape-bridge — Postmortem (2026-09-14)

## Aktueller Stand (2026-09-15)

Die dauernde Reconnect-Schleife ist behoben. Der Nutzer hat die Funktion in
Chromium und Firefox bestätigt. Die anschließend ergänzte Empfindlichkeit
`--sensitivity 0.35` wurde in Firefox erneut getestet und als deutlich besser
bewertet; 0.35 ist jetzt der Standard. Build, Unit- und Integrationstests
bestehen. Details und verbleibende Einschränkungen stehen in
[BROWSER_TEST.md](BROWSER_TEST.md), insbesondere zur Firefox-Zertifikatsausnahme
und zum einzelnen späteren Chromium-Abbruch.

Alle temporären Testprozesse und Testdaten wurden auf Nutzerwunsch beendet
beziehungsweise entfernt. Kein systemd-Service und keine Zertifikate wurden
dauerhaft installiert. Der Quellcode wird mit diesem Stand erstmals committet.
Der historische Bericht ab „Ziel“ bleibt zur Nachvollziehbarkeit erhalten.

## Nachtrag: Live-Diagnose am 2026-09-15

Der untenstehende Text beschreibt den damaligen Stand. Im erneuten Test mit
der echten Onshape-Anwendung wurde der Abbruch reproduziert und ein konkreter
Auslöser gefunden: Nach dem Command-Tree sendet Onshape ein weiteres
WAMP-Update mit Befehlsicons im Feld `images`, im beobachteten Dokument rund
373.583 Bytes groß. Unmittelbar danach brach die Verbindung ab.

Die bisherigen Tests bis etwa 31 KB bildeten diesen Fall nicht ab.
Der Eingangspuffer fasste nur 64 KiB, der Reassembly-Puffer 256 KiB.
Ein einzelner großer Frame füllte den Eingangspuffer, bevor er vollständig
dekodiert werden konnte; der Daemon schloss daraufhin die Verbindung.
Auch bei kleineren Fragmenten war das gesamte Nachrichtenlimit zu niedrig.
Der Browser meldete dies als 1006.

Ein neuer Integrationstest mit synthetischen Icon-Daten reproduzierte vor
dem Fix einen TCP-Verbindungsreset. Nun gilt ein gemeinsames, begrenztes
Nachrichtenlimit von 1 MiB; der Eingangspuffer bietet zusätzlich Platz für
den Frame-Header. Tests für einzelne und fragmentierte Nachrichten von rund
374 KB bestehen. Nach Austausch ausschließlich des temporären Testdaemons
akzeptierte dieser die echte Icon-Nachricht; die zuvor fortlaufenden
Reconnects hörten im Beobachtungszeitraum auf. Der Nutzer bestätigte anschließend den erfolgreichen Test der
Maussteuerung und schloss Chromium. Der Testcontroller beendete den Daemon
und entfernte das temporäre Browserprofil samt Zertifikat.

Damit waren die früheren Aussagen „Protokoll korrekt“ und „Server
ausgeschlossen“ zu weitgehend. Ebenso beweist 1006 allein keine vom Server
unabhängige TLS-/Browserursache. Die Kommentare, die den Leaf-Zertifikatswechsel
als Lösung bezeichneten, wurden korrigiert.

## Ziel
Eigener, auditierbarer C-Daemon als sicherere Alternative zu [spacenav-ws](https://github.com/RmStorm/spacenav-ws),
um eine SpaceMouse (via spacenavd) in Onshape unter Linux (Chromium + Firefox) nutzbar zu machen.

**Status: nicht abgeschlossen.** Der Daemon implementiert das Protokoll korrekt (siehe unten),
aber die WebSocket-Verbindung zum Browser bricht nach ca. 1 Sekunde mit Code `1006` (abnormal
closure, kein Close-Frame) ab. Die Ursache wurde trotz umfangreicher Eingrenzung nicht gefunden.

Quellcode bleibt unter `~/dev/spnav-onshape-bridge` erhalten (nie committet, aber auf Wunsch
nicht gelöscht). Alle Systeminstallation (Service, Zertifikate, Browser-Trust-Einträge) wurde
rückgängig gemacht — siehe "Aufräumen" unten.

## Was zuverlässig funktioniert (verifiziert)

- **spacenavd + libspnav**: SpaceMouse Wireless wird korrekt erkannt, Events kommen sauber an.
- **TLS-Handshake**: vollständig funktionsfähig, mit `openssl s_client` und einem echten
  Python-WebSocket-Client verifiziert (Chain-Validierung, IP-SAN-Match, beides grün).
- **HTTP/CORS-Layer**: `/3dconnexion/nlproxy`-Endpunkt liefert korrekte Antwort inkl.
  `Access-Control-Allow-Origin` — im echten Browser bestätigt (grünes Schloss, valides JSON).
- **WebSocket-Framing inkl. Fragmentierung**: RFC6455-Handshake und Frame-Codec (inkl. Nachbau
  der Fragmentierungs-Reassembly für Onshapes ~20-30KB "commands"-Tree-Nachricht) wurden isoliert
  getestet und funktionieren korrekt.
- **WAMP-Protokoll-Logik**: Der komplette Handshake (create mouse/controller, Subscribe, Prefix-
  Resolution) sowie die Motion-Event-zu-`view.affine`-Umrechnung (Rotation/Translation/Pivot,
  Gram-Schmidt statt SVD) wurden gegen reale, mitgeschnittene Onshape-Nachrichten verifiziert —
  inklusive echter SpaceMouse-Bewegungsdaten, die korrekt zu sinnvollen Kameramatrizen führten.
- **Performance**: Ein simulierter Client mit einer echten ~31KB-Nachricht (Nachbau des
  "commands"-Trees) und zehn schnellen Anfragen hintereinander bekam Antworten in <1ms. Der
  Server lag bei 0% CPU-Auslastung während der gesamten Testphase.
- **Origin-Allowlist (Sicherheitsfix)**: Live gegen falschen und korrekten Origin getestet —
  funktioniert wie vorgesehen (403 bei fremder Origin).

Kurz: Alles, was wir mit einem eigenen, nicht-Browser-Client (Python `websockets`) testen
konnten, funktioniert einwandfrei — auch mit realistischen Nachrichtengrößen und -mustern.

## Das ungelöste Problem

Sobald ein **echter Browser** (Chromium 153 oder Firefox) die Verbindung öffnet:

1. TLS-Handshake und WAMP-Handshake laufen sauber durch.
2. Reale Daten fließen einige hundert Millisekunden bis ~1 Sekunde lang korrekt (teils mehrere
   volle Request/Response-Zyklen inklusive realer Mausbewegungsdaten).
3. Die zugrundeliegende TCP/TLS-Verbindung wird dann **abrupt und ohne WebSocket-Close-Frame**
   getrennt — bestätigt über direkte JS-Instrumentierung: `CloseEvent.code = 1006`,
   `reason = ""`, `wasClean = false`.
4. Onshapes eigene Reconnect-Logik (`window.ab.connect.maxRetries`) greift, baut die Verbindung
   neu auf, dieselbe kurze funktionierende Phase, dann wieder Abbruch — Dauerschleife.
5. Ergebnis für den Nutzer: ruckelige, verzögerte Reaktion, erhöhte CPU-Last (durch den
   ständigen Reconnect-Zyklus inkl. Neuübertragung der großen Command-Tree-Nachricht).

Code 1006 bedeutet: der Abbruch geschieht **unterhalb** der WebSocket-Protokollebene (TCP-Reset
oder TLS-Abbruch), nicht durch eine JS-seitige `ws.close()`-Entscheidung und nicht durch einen
regulären WAMP-Protokollfehler unsererseits (kein Close-Frame, keine Fehlermeldung serverseitig).

## Eingegrenzte und ausgeschlossene Ursachen

Jede der folgenden Hypothesen wurde konkret getestet (nicht nur vermutet) und **ausgeschlossen**:

| Hypothese | Test | Ergebnis |
|---|---|---|
| WebSocket-Frame-Fragmentierung nicht unterstützt | Fund: `first bytes: 01 ff 00 00` (FIN=0) in echten Logs; RFC6455-Reassembly implementiert und isoliert verifiziert | Bug real und behoben, aber **nicht** die Ursache des 1006-Problems |
| Fehlender `Access-Control-Allow-Origin` (CORS) auf `/3dconnexion/nlproxy` | Header ergänzt, mit `curl` verifiziert | Behoben (Onshape kommt jetzt bis zum WebSocket-Versuch), löst aber nicht das Kernproblem |
| Chrome/Firefox "Local Network Access" (LNA) / "Private Network Access" (PNA), allgemein | `chrome://flags/#local-network-access-check` → Disabled, Chromium neu gestartet | Keine Änderung |
| LNA/PNA speziell für WebSockets (`LocalNetworkAccessChecksWebSockets`) | `chromium --disable-features=LocalNetworkAccessChecksWebSockets` | Keine Änderung |
| Firefox-spezifisches LNA | `network.lna.skip-domains` = `cad.onshape.com` gesetzt | Keine Änderung |
| Fehlender `Access-Control-Allow-Private-Network`-Header | Header zu HTTP- und WS-Upgrade-Antwort hinzugefügt | Keine Änderung |
| Eigenes Server-Logging verlangsamt Event-Loop | Direkter Timing-Test mit simuliertem Client: <1ms Antwortzeit auch bei 31KB-Nachrichten | Ausgeschlossen — Server ist nicht der Flaschenhals |
| Zertifikats-Vertrauensmodell: lokale CA (mächtiger, verdächtiger) vs. direkt gepinntes Leaf-Zertifikat (wie spacenav-ws es nutzt) | Umbau von CA+Leaf-Hierarchie auf einzelnes selbstsigniertes Leaf-Zertifikat, NSS-Import mit `P,,` (Peer-Trust) statt `C,,` (CA-Trust) | Keine Änderung |
| Onshape-Dokument-Berechtigungen (403-Fehler auf `/api/v14/documents/.../permissionset`) | Mit neu erstelltem, garantiert eigenem Dokument getestet | Keine Änderung — Nutzer bestätigt: liegt nicht an Onshape/Permissions |
| Werbeblocker/Privacy-Extension | Pi-hole (netzwerkweiter DNS-Blocker auf separatem Host) deaktiviert getestet | Keine Änderung (ohnehin für Loopback-Traffic irrelevant) |
| Chrome-Sandboxing / Kernel-RC-Version (`7.3.0-rc2-1-mainline`) interagiert mit Netzwerk-Namespaces | `chromium --no-sandbox` | Keine Änderung |

## Nicht mehr durchgeführter, aber vielversprechendster nächster Schritt

**Die Referenzimplementierung (`uvx spacenav-ws@latest serve`, Python/FastAPI/Uvicorn) auf
genau dieser Maschine, in genau diesem Browser, gegen dasselbe Dokument laufen lassen.**

Dieser A/B-Test wurde begonnen (unser Dienst gestoppt, Port 8181 freigegeben), aber auf
Nutzerwunsch abgebrochen, um stattdessen aufzuräumen und zu dokumentieren. Er hätte die Suche
eindeutig in zwei Hälften geteilt:

- **Bricht spacenav-ws ebenfalls mit Code 1006 ab** → Ursache liegt an dieser Maschine/diesem
  Browser-Build/dieser Netzwerkumgebung, nicht am eigenen Server-Code. Kein C-Code-Problem.
- **spacenav-ws läuft stabil** → es gibt einen echten, noch unbekannten strukturellen
  Unterschied zwischen unserer minimalen C-Implementierung und einer reifen ASGI/Uvicorn-
  Websocket-Stack-Implementierung (z. B. HTTP-Header-Details, TCP-Socket-Optionen,
  Keep-Alive-Verhalten, TLS-Record-Timing), der gezielt eingrenzbar wäre.

Falls das Thema später wieder aufgenommen wird, ist das der klar sinnvollste erste Schritt.

## Aufräumen (durchgeführt am 2026-09-14)

- systemd `--user`-Service gestoppt, deaktiviert, Unit-Datei entfernt
- Binary aus `~/.local/bin/spnav-onshape-bridge` entfernt
- Zertifikate/Keys aus `~/.local/state/spnav-onshape-bridge` entfernt
- Zertifikats-Trust-Einträge aus `~/.pki/nssdb` (Chromium) und beiden Firefox-Profilen
  (`99cn02xt.default`, `dhbxwyki.default-release`) entfernt, verifiziert sauber

**Manuell durch dich noch zu erledigen** (nicht per Terminal automatisierbar):

- Chromium: `chrome://extensions` → "SpaceMouse for Onshape" deinstallieren
- Firefox: Tampermonkey → das Skript "Onshape 3D-Mouse on Linux" löschen
- Firefox: `about:config` → `network.lna.skip-domains` zurücksetzen/leeren
- Chromium: `chrome://flags/#local-network-access-check` zurück auf "Default" (falls noch
  auf "Disabled" stehend)
- Quellcode-Verzeichnis `~/dev/spnav-onshape-bridge` bleibt auf Wunsch erhalten; bei Bedarf
  später selbst löschen (`rm -rf ~/dev/spnav-onshape-bridge`) — nie committet, daher ohne
  Git-Historie unwiderruflich.

## Für Onshape/SpaceMouse in der Zwischenzeit

Bis (falls) das Problem gelöst wird, bleibt [spacenav-ws](https://github.com/RmStorm/spacenav-ws)
die einzige bekannte funktionierende Lösung für diesen Anwendungsfall unter Linux — mit den
eingangs genannten Sicherheitsvorbehalten (siehe erste Analyse: fehlender Origin-Check bei
WebSocket-Upgrades, geteilter TLS-Key im öffentlichen Repo, ungepinnte Laufzeit-Nachladung).
