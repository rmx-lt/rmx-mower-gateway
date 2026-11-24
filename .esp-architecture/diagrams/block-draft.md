# Block diagram draft

Hand-drawn, not generated. From the hardware to the product function; a line is a connection. Blue: ways of communicating. Inside a device: its aspects, grouped by the bus they use.

```mermaid
flowchart TD
  subgraph modem["<b>modem</b>"]
    mctl["power management<hr/>power<br/>radio<br/>sleep<br/>status"]
    mdata["data link"]
  end
  subgraph led["<b>LED</b>"]
    lparts["light"]
  end
  subgraph ph["<b>phone</b>"]
    pparts["messages"]
  end
  subgraph usb["<b>USB bridge</b>"]
    uparts["console"]
  end
  subgraph cpu["<b>ESP32</b>"]
    u1["UART1"]
    gm["GPIO 4, 25, 32, 34"]
    gl["GPIO 12"]
    gr["GPIO 21"]
    wf["Wi-Fi"]
    u0["UART0"]
    conn["modem connection"]
    pwr["modem power"]
    ledd["LED driver"]
    wl["Wi-Fi link"]
    con["console"]
    ntrip["corrections"]
    mqtt["server connection"]
    phc["phone connection"]
    msg["message routing"]
    lock["control lock"]
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
  subgraph ap["<b>autopilot</b>"]
    areset["reset"]
    amsg["messages"]
  end
  subgraph fl["<b>flash</b>"]
    fparts["storage<hr/>settings<br/>identity<br/>app slots<br/>crash dump"]
  end
  mdata --- u1 --- conn
  mctl --- gm --- pwr --- conn
  lparts --- gl --- ledd --- conn
  pparts --- wf --- wl
  uparts --- u0 --- con
  wl --- phc
  phc --- msg
  phc --- prov
  con --- prov
  conn --- mqtt
  conn --- ntrip
  conn --- clk
  conn --- ls
  mqtt --- ls
  mqtt --- msg
  mqtt --- upd
  msg --- lock
  msg --- cmd
  msg --- state
  msg --- met
  msg --- relay
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
  upd --- fs
  set --- fs
  fs --- sp --- fparts
  bl --- u2 --- amsg
  bl --- gr --- areset
  classDef bus stroke:#58a6ff,stroke-dasharray:3 3
  class u1,gm,gl,gr,u2,wf,u0,sp bus
  classDef device fill:none,stroke:#888
  class modem,mdata,mctl,led,lparts,ap,amsg,areset,ph,pparts,usb,uparts,fl,fparts device
```
