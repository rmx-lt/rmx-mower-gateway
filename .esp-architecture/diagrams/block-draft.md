# Block diagram draft

Hand-drawn, not generated. From the hardware to the product function; a line is a connection. Dashed blue: ways of communicating.

```mermaid
flowchart TD
  modem["<b>modem</b><hr/>data<br/>power control<br/>status"]
  led["LED"]
  ph["phone"]
  usb["USB bridge"]
  subgraph cpu["<b>ESP32</b>"]
    u1["UART1"]
    gp["GPIO"]
    wf["Wi-Fi"]
    u0["UART0"]
    conn["modem connection"]
    pwr["modem power"]
    ledd["LED driver"]
    wl["Wi-Fi link"]
    con["console"]
    ntrip["corrections"]
    mqtt["server connection"]
    drive["driving"]
    prov["provisioning"]
    cmd["commands"]
    upd["firmware update"]
    state["gateway state"]
    met["metrics"]
    mon["monitor"]
    ls["link state"]
    clk["clock"]
    rec["recovery"]
    boot["boot counter"]
    relay["relay"]
    q["offline queue"]
    bl["board link"]
    set["settings"]
    fs["flash storage"]
    u2["UART2"]
    sp["SPI0/1"]
  end
  ap["<b>autopilot</b><hr/>messages<br/>reset"]
  fl["<b>flash</b><hr/>settings<br/>identity<br/>app slots<br/>crash dump"]
  modem --- u1 --- conn
  modem --- gp
  led --- gp
  gp --- pwr --- conn
  gp --- ledd --- conn
  ph --- wf --- wl
  usb --- u0 --- con
  wl --- drive
  wl --- prov
  con --- prov
  conn --- mqtt
  conn --- ntrip
  conn --- clk
  conn --- ls
  mqtt --- ls
  mqtt --- cmd
  mqtt --- state
  mqtt --- met
  mqtt --- relay
  met --- mon
  cmd --- mon
  cmd --- upd
  prov --- mqtt
  prov --- set
  ls --- rec
  rec --- boot
  boot --- set
  relay --- q
  relay --- bl
  ntrip --- bl
  drive --- bl
  upd --- fs
  set --- fs
  fs --- sp --- fl
  bl --- u2 --- ap
  gp --- ap
  classDef bus stroke:#58a6ff,stroke-dasharray:3 3
  class u1,gp,u2,wf,u0,sp bus
  classDef device fill:none,stroke:#888
  class modem,led,ap,ph,usb,fl device
```
