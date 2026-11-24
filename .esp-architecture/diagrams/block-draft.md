# Block diagram draft

Hand-drawn, not generated. From the hardware to the product function; a line is a connection. Blue: ways of communicating. Green: drivers; dashed: provided by the system, not ours. Inside a device: its aspects, grouped by the bus they use.

```mermaid
---
config:
  flowchart:
    subGraphTitleMargin:
      top: 10
      bottom: 4
---
flowchart TD
  subgraph modem["<b>modem</b>"]
    mctl["operation<hr/>power<br/>radio<br/>sleep<br/>status"]
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
    subgraph conn["modem driver"]
      mqtt["server connection"]
      cast["caster connection"]
    end
    phd["phone driver"]
    con["console driver"]
    subgraph msg["message routing"]
      lock["control lock"]
      q["offline queue"]
    end
    prov["provisioning"]
    cmd["commands"]
    upd["firmware update"]
    state["gateway state"]
    met["metrics"]
    mon["monitor"]
    subgraph help["helpers"]
      clk["clock"]
      ls["link state"]
      rec["recovery"]
      boot["boot counter"]
    end
    bl["autopilot driver"]
    set["settings"]
    fs["flash driver"]
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
  mctl --- gm --- conn
  lparts --- gl --- conn
  pparts --- wf --- phd
  uparts --- u0 --- con
  phd --- msg
  msg --- prov
  con --- prov
  conn --- msg
  msg --- upd
  msg --- cmd
  msg --- state
  msg --- met
  msg --- bl
  met --- mon
  cmd --- mon
  cmd --- upd
  conn --- set
  prov --- set
  cmd --- bl
  state --- bl
  upd --- fs
  set --- fs
  fs --- sp --- fparts
  bl --- u2 --- amsg
  bl --- gr --- areset
  classDef bus stroke:#58a6ff,stroke-width:2px
  class u1,gm,gl,gr,u2,wf,u0,sp bus
  classDef driver stroke:#3fb950,stroke-width:2px
  class con,phd,bl driver
  classDef sysdriver stroke:#3fb950,stroke-width:2px,stroke-dasharray:9 6
  class fs sysdriver
  classDef helpers fill:none,stroke:#888,stroke-dasharray:4 3
  class help helpers
  classDef owner fill:#8884,stroke:#aaa,stroke-width:2px
  class msg owner
  classDef ownerdriver fill:#8884,stroke:#3fb950,stroke-width:2px
  class conn ownerdriver
  classDef device fill:none,stroke:#888
  class modem,mdata,mctl,led,lparts,ap,amsg,areset,ph,pparts,usb,uparts,fl,fparts device
```
