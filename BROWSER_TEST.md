# Browser-Test am 2026-09-15

## Ergebnis

Der aktuelle Daemon hielt im isolierten Chromium-Test die WebSocket-Verbindung
35 Sekunden offen: 15 Sekunden ohne Anfragen nach dem WAMP-Handshake, danach
20 Sekunden mit ungefähr zehn Nachrichten pro Sekunde. Kein WebSocket-Fehler
und kein unerwartetes Close-Event während dieses Zeitraums.

Am Messzeitpunkt: 200 große Anfragen gesendet, 199 Antworten empfangen,
WebSocket.readyState = OPEN. Die letzte Anfrage wurde unmittelbar vor der
Momentaufnahme gesendet; auf ihre Antwort wurde nicht mehr gewartet.

Der im POSTMORTEM.md beschriebene Abbruch nach etwa einer Sekunde wurde in
diesem Test nicht reproduziert. Das belegt noch keine Behebung des ursprünglichen
Onshape-Problems.

## Aufbau und Grenzen

- Chromium 153.0.8010.36 (Arch Linux), headless, frisches temporäres Profil.
- Aktueller echter Daemon, direkt gestartet, mit Verbindung zum vorhandenen
  spacenavd-Socket; kein systemd-Service.
- Echter TLS-/WebSocket-Stack des Browsers gegen 127.51.68.120:8181.
- Per DevTools lokal bereitgestellte Testseite unter der Origin
  https://cad.onshape.com. Die echte Onshape-Anwendung wurde nicht geladen.
- Synthetischer WAMP-Handshake: create mouse, create controller (Name Onshape),
  Prefix und Subscribe. Danach Update-Aufrufe mit jeweils 31.000 Zeichen
  Fülldaten im commands-Feld. Kein echter Onshape-Command-Tree.
- Keine aufgezeichneten physischen Mausbewegungen und keine visuelle Prüfung
  der Kamerasteuerung. Firefox und die Referenzimplementierung wurden nicht getestet.
- Frisches selbstsigniertes Testzertifikat. Nur der Testbrowser akzeptierte
  dessen öffentlichen Schlüssel über --ignore-certificate-errors-spki-list.
  Die normale Zertifikatsvalidierung über die Browser-Trust-Stores wurde somit
  nicht getestet.

## Beobachtung zur Browserberechtigung

Ohne passende Berechtigung scheiterte die Verbindung bereits vor dem
WAMP-Handshake mit:

    net::ERR_BLOCKED_BY_LOCAL_NETWORK_ACCESS_CHECKS

Der Browser meldete dabei Close-Code 1006 nach wenigen Millisekunden; der
Daemon erhielt keinen WebSocket-Upgrade. Das ist ein anderes Fehlerbild als
der im Postmortem beschriebene Abbruch nach zunächst erfolgreichem Datenverkehr.

Browser.setPermission mit dem Namen local-network-access reichte in diesem
Test nicht aus. Mit dem Namen loopback-network und setting=granted für die
Test-Origin funktionierte die Verbindung. Es wurden keine LNA-Prüfungen per
Browser-Flag abgeschaltet.

## Systemzustand

Keine Installation oder Aktivierung eines systemd-Service. Kein Import in
bestehende Chromium-/Firefox-Zertifikatsspeicher. Keine Änderung bestehender
Browserprofile. Testdaemon und Testbrowser nach dem Lauf beendet.
Temporäre Browserprofile, Zertifikate und Testskript anschließend entfernt.

Der erste, synthetische Test allein erlaubte noch keine Aussage zur echten
Onshape-Anwendung; die anschließend durchgeführten Nutzertests folgen unten.


## Nachtrag: echte Onshape-Anwendung

Im anschließend vom Nutzer bedienten, sichtbaren Testbrowser trat die
Reconnect-Schleife wieder auf, mit Abbrüchen etwa 150 ms nach dem Upgrade.
Die Diagnose zeigte eine zusätzliche `images`-Nachricht von rund 374 KB
nach dem kleineren Command-Tree. Das überstieg beide bisherigen Puffergrenzen.

Der neue Integrationstest reproduzierte den Verbindungsreset vor dem Fix.
Nach Anhebung des begrenzten Nachrichtenlimits auf 1 MiB und entsprechender
Dimensionierung des Frame-Eingangspuffers besteht der Test für einzelne und
fragmentierte Icon-Nachrichten. Im weiter geöffneten echten Onshape-Dokument
akzeptierte der ersetzte Testdaemon die Icon-Nachricht; die Reconnect-Schleife
hörte auf. Der Nutzer bestätigte anschließend den erfolgreichen Test der Kamerasteuerung
und schloss das Testfenster.

Auch der sichtbare Test ist beendet. Der Testcontroller hat den Daemon
beendet und das temporäre Browserprofil samt Zertifikat entfernt. Es wurde
kein Service installiert oder Zertifikat in bestehende Trust-Stores importiert.

Die Diagnose enthielt nach dem Fix noch einen einzelnen 1006-Abbruch nach
rund 160 Sekunden und anschließenden Reconnect; dessen Ursache wurde nicht
gesondert ermittelt. Die zuvor dauernde Reconnect-Schleife war beendet.
Der erfolgreiche Nutzertest ist daher kein Nachweis vollständiger
Langzeitstabilität.


## Firefox 155.0.1: erfolgreicher Nutzertest

Der Nutzer bestätigte auch in Firefox eine einwandfreie Funktion.
Im sichtbaren Test wurden eine einzige Bridge-Verbindung, 4.370 gesendete
und 4.367 empfangene Nachrichten gezählt, ohne Close-Event während der
Messung. Die größte gesendete Nachricht hatte 372.216 Zeichen und wurde
verarbeitet. Browser und Testdaemon sind beendet; temporäres Profil und
Testzertifikat wurden entfernt.

Der anfängliche NSS-Import mit `P,,` allein reichte in diesem Firefox-Profil
nicht aus: Firefox meldete MOZILLA_PKIX_ERROR_SELF_SIGNED_CERT.
Für den erfolgreichen Test wurde stattdessen eine Ausnahme für genau
127.51.68.120:8181 und den SHA-256-Fingerabdruck des Testzertifikats in
`cert_override.txt` des temporären Profils hinterlegt.
`acceptInsecureCerts` blieb deaktiviert; die HTTPS-Discovery wurde vor der
Onshape-Anmeldung erfolgreich geladen. Bestehende Firefox-Profile und
systemweite Trust-Stores wurden nicht geändert. Der bisher dokumentierte
NSS-Import allein ist damit kein verifiziert funktionierender Firefox-Setupweg.

Der Nutzer meldete browserübergreifend eine zu hohe Empfindlichkeit.
Anschließend wurde `--sensitivity` ergänzt und der Standardwert auf 0.35
gesetzt. Unit-Tests prüfen die Änderung für Rotation, Translation und Zoom.
Ein weiterer Firefox-Test mit `--sensitivity 0.35` wurde durchgeführt.
Der Nutzer bewertete die Steuerung als „schon viel besser“ und bestätigte
damit den neuen Standardwert als geeigneten Ausgangspunkt.

Bei diesem Test wurden eine Verbindung, 5.267 gesendete und 5.264 empfangene
Nachrichten aufgezeichnet, ohne Close-Event bis zur abschließenden Messung.
Auf Nutzerwunsch wurde anschließend alles beendet: Testbrowser, Daemon,
temporäres Profil, Zertifikat und Diagnoseskripte. Es besteht weiterhin keine
dauerhafte Installation. Weitere individuelle Abstimmung ist über
`--sensitivity` möglich.


## Nachtrag 2026-09-15: Ursache des Firefox-Zertifikatsproblems gefunden

Der oben dokumentierte Bedarf einer manuellen Host+Fingerabdruck-Ausnahme in
Firefox hatte eine konkrete, vermeidbare Ursache: Zwischenzeitlich war das
Projekt (während der Fehlersuche zum ursprünglichen Chromium-Verbindungsabbruch,
siehe POSTMORTEM.md) von einer lokalen CA, die ein Leaf-Zertifikat signiert,
auf ein einzelnes, direkt selbstsigniertes Leaf-Zertifikat umgestellt worden -
auf der (falschen) Annahme, das CA-Vertrauensmodell hätte den Chromium-Abbruch
verursacht. Tatsächlich lag jener Bug an einer zu kleinen Nachrichtengrößen-
Grenze, völlig unabhängig vom Zertifikatsmodell.

Empirisch verifiziert (jeweils mit `firefox --headless --screenshot` gegen
ein frisches, temporäres Profil):

- Selbstsigniertes Leaf-Zertifikat, importiert mit NSS-Peer-Trust (`certutil
  -t P,,`): Firefox zeigt die Zertifikatswarnung, keine automatische
  Akzeptanz.
- Von einer lokalen CA signiertes Leaf-Zertifikat, CA importiert mit
  regulärem CA-Trust (`certutil -t C,,`): Firefox lädt die Seite direkt,
  ohne jede Warnung oder manuelle Interaktion - ebenso in Chromium.

Firefox' Zertifikatsprüfung (mozilla::pkix) unterstützt NSS-Peer-Trust für
selbstsignierte Zertifikate offenbar nicht zuverlässig auf dieselbe Weise wie
das klassische NSS-Modell es nahelegt; echte CA-Ketten-Validierung ist der
zuverlässig unterstützte Weg. `src/tls.c` und `contrib/nss-trust-install.sh`
wurden entsprechend zurückgebaut (lokale CA + Leaf, Import mit `C,,`). Damit
entfällt der manuelle Ausnahme-Schritt für Firefox vollständig - bestätigt
mit demselben Screenshot-Verfahren gegen den echten, laufenden Daemon
(reales generiertes Zertifikat, realer `nss-trust-install.sh`-Importbefehl):
`https://127.51.68.120:8181/3dconnexion/nlproxy` lädt direkt, ohne
Interstitial.

Der reine Vertrauensmodell-Wechsel (CA vs. Peer) hatte also nie etwas mit dem
Chromium-1006-Bug zu tun, wohl aber sehr direkt mit der Firefox-Erfahrung -
zwei unabhängige Fragen, die während der Fehlersuche vermischt wurden.
