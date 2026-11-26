# Block diagram draft

Hand-drawn, not generated. From the hardware to the product function; a line is a connection. Blue: ways of communicating. Green: drivers. Inside a device: its aspects, grouped by the bus they use.

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
    gm["GPIO 4, 25,<br/>32, 34"]
    gl["GPIO 12"]
    gr["GPIO 21"]
    wf["Wi-Fi"]
    u0["UART0"]
    subgraph conn["modem driver"]
      mqtt["server<br/>connection"]
      cast["caster<br/>connection"]
      clk["clock"]
      mob["mobile data"]
      mqtt ~~~ clk
      cast ~~~ mob
    end
    phd["phone driver"]
    con["console driver"]
    subgraph msg["router"]
      lock["control lock"]
      chk["command check"]
      q["offline queue"]
      ls["link state"]
      q ~~~ ls
      lock ~~~ chk
    end
    subgraph tel["telemetry"]
      meas["measurements"]
      state["gateway state"]
    end
    subgraph mgmt["gateway management"]
      rec["recovery"]
      up["server link up"]
    end
    subgraph esp["ESP32 driver"]
      rst["restart"]
      boot["boot counter"]
      live["task check"]
      load["processor load"]
      rst ~~~ live
      boot ~~~ load
    end
    bl["autopilot driver"]
    subgraph fs["storage driver"]
      set["settings"]
      prov["provisioning"]
      upd["firmware update"]
      p2[" "]
      set ~~~ prov
      upd ~~~ p2
    end
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
  con --- msg
  conn --- msg
  msg --- tel
  msg --- mgmt
  mgmt --- fs
  mgmt --- esp
  msg --- bl
  fs --- sp --- fparts
  bl --- u2 --- amsg
  bl --- gr --- areset
  classDef bus stroke:#58a6ff,stroke-width:2px
  class u1,gm,gl,gr,u2,wf,u0,sp bus
  classDef driver stroke:#3fb950,stroke-width:2px
  class con,phd,bl driver
  classDef owner fill:#8884,stroke:#aaa,stroke-width:2px
  class msg,tel,mgmt owner
  classDef ownerdriver fill:#8884,stroke:#3fb950,stroke-width:2px
  class conn,fs,esp ownerdriver
  classDef pad fill:none,stroke:none
  class p2 pad
  classDef device fill:none,stroke:#888
  class modem,mdata,mctl,led,lparts,ap,amsg,areset,ph,pparts,usb,uparts,fl,fparts device
```
